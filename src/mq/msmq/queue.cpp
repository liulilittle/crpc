// CRPC -- MSMQ queue implementation.
//
// All MSMQ API calls live here, behind a CRPC_OS_WINDOWS guard. A non-Windows
// build compiles the same public declarations and answers every operation with
// `unavailable`, so a caller never has to know which platform it was built for.
//
// Format-name resolution:
//   * a path that already starts with "FORMATNAME:" or "DIRECT=" is used as-is;
//   * any other path goes through MQPathNameToFormatName, which is what makes
//     `.\private$\name`, public queue paths and remote/DNS-named destinations
//     resolve correctly. The format name is never assembled by string surgery
//     here.
//
// Receive is two-pass. The first call has a zero-sized body buffer plus every
// other property; a non-empty body is answered with MQ_ERROR_BUFFER_OVERFLOW
// and the message stays in the queue, with the needed size in
// PROPID_M_BODY_SIZE. Every property except the body is already filled by the
// first call, so the second call only adds the correctly sized body buffer.
// An empty body is consumed by the first call directly.
#include <crpc/mq/msmq/queue.hpp>
#include "transaction_internal.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(CRPC_OS_WINDOWS)
// mq.h forward-declares ITransaction with the COM `interface` keyword, which
// windows.h (stripped by WIN32_LEAN_AND_MEAN) does not provide. ole2.h is what
// brings the COM headers that define `interface`; include it before mq.h.
#include <windows.h>
#include <ole2.h>
#include <mq.h>
#pragma comment(lib, "mqrt.lib")
#endif

#include "msmq_internal.hpp"

namespace crpc {
namespace mq {
namespace msmq {

#if defined(CRPC_OS_WINDOWS)

namespace detail {

std::wstring to_wide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    (void)MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                              &out[0], length);
    return out;
}

std::string to_utf8(const wchar_t* text, std::size_t length) {
    if (text == nullptr || length == 0) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text,
                                          static_cast<int>(length), nullptr, 0,
                                          nullptr, nullptr);
    if (bytes <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(bytes), '\0');
    (void)WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(length), &out[0],
                              bytes, nullptr, nullptr);
    return out;
}

bool starts_with_ci(std::string_view text, std::string_view prefix) noexcept {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        char a = text[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<char>(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

std::string hex_of(std::uint32_t value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08lX",
                  static_cast<unsigned long>(value));
    return std::string(buffer);
}

rpc_error mq_error(const char* what, HRESULT hr) {
    const std::uint32_t value = static_cast<std::uint32_t>(hr);
    rpc_error out{map_mq_error(value),
                  std::string(what) + " (MSMQ " + hex_of(value) + ")"};
    out.metadata.set("hr", hex_of(value));
    return out;
}

} // namespace detail

namespace {

// Builds an MQMSGPROPS list. Property order is irrelevant to MSMQ; the builder
// keeps the ids/vars/status arrays in lockstep whatever the order.
struct prop_builder {
    PROPID ids[14]{};
    MQPROPVARIANT vars[14]{};
    HRESULT status[14]{};
    int count = 0;

    int ui1(PROPID id, UCHAR value) {
        const int at = count++;
        ids[at] = id;
        vars[at].vt = VT_UI1;
        vars[at].bVal = value;
        return at;
    }
    int ui4(PROPID id, ULONG value) {
        const int at = count++;
        ids[at] = id;
        vars[at].vt = VT_UI4;
        vars[at].ulVal = value;
        return at;
    }
    int ui8(PROPID id, ULONGLONG value) {
        const int at = count++;
        ids[at] = id;
        vars[at].vt = VT_UI8;
        vars[at].uhVal.QuadPart = value;
        return at;
    }
    int lpwstr(PROPID id, wchar_t* value) {
        const int at = count++;
        ids[at] = id;
        vars[at].vt = VT_LPWSTR;
        vars[at].pwszVal = value;
        return at;
    }
    // Binary payload (VT_VECTOR | VT_UI1). `value` may be nullptr with a zero
    // size, which is how a receive probes the required body size.
    int bytes(PROPID id, BYTE* value, ULONG size) {
        const int at = count++;
        ids[at] = id;
        vars[at].vt = VT_VECTOR | VT_UI1;
        vars[at].caub.pElems = value;
        vars[at].caub.cElems = size;
        return at;
    }
    void make(MQMSGPROPS& out) const {
        out.cProp = static_cast<DWORD>(count);
        out.aPropID = const_cast<PROPID*>(ids);
        out.aPropVar = const_cast<MQPROPVARIANT*>(vars);
        out.aStatus = const_cast<HRESULT*>(status);
    }
};

constexpr int kPropBody = 0;
constexpr int kPropBodySize = 1;
constexpr int kPropLabelLen = 2;
constexpr int kPropLabel = 3;
constexpr int kPropPriority = 4;
constexpr int kPropDelivery = 5;
constexpr int kPropAppSpecific = 6;
constexpr int kPropCorrelation = 7;
constexpr int kPropBodyType = 8;
constexpr int kPropLookupId = 9;
constexpr int kPropArrivedTime = 10;
constexpr int kPropCount = 11;

DWORD to_timeout_ms(std::chrono::milliseconds timeout) noexcept {
    if (timeout.count() < 0) {
        return 0;
    }
    if (timeout.count() >= static_cast<long long>(INFINITE)) {
        return static_cast<DWORD>(INFINITE);
    }
    return static_cast<DWORD>(timeout.count());
}

// Reads the non-body receive properties out of a completed property list.
// `label`/`correlation` are the buffers the properties wrote into.
void unpack_common(const prop_builder& props, wchar_t* label,
                   const BYTE* correlation, received_message& out) {
    if (props.status[kPropLabelLen] == MQ_OK) {
        ULONG length = props.vars[kPropLabelLen].ulVal;
        if (length > MQ_MAX_MSG_LABEL_LEN) {
            length = MQ_MAX_MSG_LABEL_LEN;
        }
        // PROPID_M_LABEL_LEN counts the terminating L'\0' (measured: a
        // 7-character label reports 8); strip it before converting to UTF-8.
        while (length > 0 && label[length - 1] == L'\0') {
            --length;
        }
        out.msg.label = detail::to_utf8(label, length);
    }
    if (props.status[kPropPriority] == MQ_OK) {
        out.msg.priority = props.vars[kPropPriority].bVal;
    }
    if (props.status[kPropDelivery] == MQ_OK) {
        out.msg.recoverable =
            props.vars[kPropDelivery].bVal == MQMSG_DELIVERY_RECOVERABLE;
    }
    if (props.status[kPropAppSpecific] == MQ_OK) {
        out.msg.app_specific = props.vars[kPropAppSpecific].ulVal;
    }
    if (props.status[kPropCorrelation] == MQ_OK &&
        props.vars[kPropCorrelation].caub.cElems == PROPID_M_CORRELATIONID_SIZE) {
        std::memcpy(out.msg.correlation_id.data(), correlation,
                    PROPID_M_CORRELATIONID_SIZE);
    }
    if (props.status[kPropBodyType] == MQ_OK) {
        out.msg.body_type =
            static_cast<std::uint16_t>(props.vars[kPropBodyType].ulVal);
    }
    if (props.status[kPropLookupId] == MQ_OK) {
        out.lookup_id = props.vars[kPropLookupId].uhVal.QuadPart;
    }
    if (props.status[kPropArrivedTime] == MQ_OK) {
        out.arrived_at = std::chrono::system_clock::time_point(
            std::chrono::seconds(props.vars[kPropArrivedTime].ulVal));
    }
}

// One receive with a timeout, optionally under `transaction`. Fills `out`; a
// timeout sets `out.present = false` and is a normal outcome.
[[nodiscard]] status receive_once(HANDLE handle,
                                  std::chrono::milliseconds timeout,
                                  ITransaction* transaction,
                                  received_message& out) {
    wchar_t label[MQ_MAX_MSG_LABEL_LEN + 1] = {};
    BYTE correlation[PROPID_M_CORRELATIONID_SIZE] = {};

    // Pass 1: probe the size with a zero-length body buffer, reading every
    // other property. A non-empty body answers MQ_ERROR_BUFFER_OVERFLOW and the
    // message stays in the queue.
    prop_builder probe;
    probe.bytes(PROPID_M_BODY, nullptr, 0);
    probe.ui4(PROPID_M_BODY_SIZE, 0);
    // PROPID_M_LABEL_LEN is an input/output property: on input it must carry
    // the capacity of the label buffer, on output the actual label length.
    // Passing 0 makes the service answer MQ_ERROR_LABEL_BUFFER_TOO_SMALL (the
    // message stays in the queue); verified against a real service.
    probe.ui4(PROPID_M_LABEL_LEN, MQ_MAX_MSG_LABEL_LEN);
    probe.lpwstr(PROPID_M_LABEL, label);
    probe.ui1(PROPID_M_PRIORITY, 0);
    probe.ui1(PROPID_M_DELIVERY, 0);
    probe.ui4(PROPID_M_APPSPECIFIC, 0);
    probe.bytes(PROPID_M_CORRELATIONID, correlation, sizeof(correlation));
    probe.ui4(PROPID_M_BODY_TYPE, 0);
    probe.ui8(PROPID_M_LOOKUPID, 0);
    probe.ui4(PROPID_M_ARRIVEDTIME, 0);

    DWORD timeout_ms = to_timeout_ms(timeout);
    MQMSGPROPS first;
    probe.make(first);
    HRESULT hr = MQReceiveMessage(handle, timeout_ms, 0, &first, nullptr, nullptr,
                                  nullptr, transaction);
    if (hr == MQ_ERROR_IO_TIMEOUT) {
        out.present = false;
        return status{};
    }
    if (FAILED(hr) && hr != MQ_ERROR_BUFFER_OVERFLOW) {
        return status(detail::mq_error("MQReceiveMessage failed", hr));
    }

    const ULONG needed = probe.vars[kPropBodySize].ulVal;
    if (hr == MQ_ERROR_BUFFER_OVERFLOW) {
        if (needed == 0) {
            return status{rpc_code::internal,
                          "MQReceiveMessage reported a buffer overflow with a "
                          "zero body size"};
        }
        std::vector<std::byte> body(needed);

        // Pass 2: the real receive, adding only the correctly sized body.
        prop_builder second;
        second.bytes(PROPID_M_BODY,
                     reinterpret_cast<BYTE*>(body.data()),
                     static_cast<ULONG>(body.size()));
        second.ui4(PROPID_M_BODY_SIZE, needed);
        // LABEL_LEN input = label buffer capacity, like the probe pass.
        second.ui4(PROPID_M_LABEL_LEN, MQ_MAX_MSG_LABEL_LEN);
        second.lpwstr(PROPID_M_LABEL, label);
        second.ui1(PROPID_M_PRIORITY, 0);
        second.ui1(PROPID_M_DELIVERY, 0);
        second.ui4(PROPID_M_APPSPECIFIC, 0);
        second.bytes(PROPID_M_CORRELATIONID, correlation, sizeof(correlation));
        second.ui4(PROPID_M_BODY_TYPE, 0);
        second.ui8(PROPID_M_LOOKUPID, 0);
        second.ui4(PROPID_M_ARRIVEDTIME, 0);

        MQMSGPROPS props;
        second.make(props);
        hr = MQReceiveMessage(handle, timeout_ms, 0, &props, nullptr, nullptr,
                              nullptr, transaction);
        if (FAILED(hr)) {
            return status(detail::mq_error("MQReceiveMessage (body) failed", hr));
        }
        const ULONG actual = second.vars[kPropBodySize].ulVal;
        if (actual > body.size()) {
            return status{
                rpc_code::internal,
                "MQReceiveMessage returned a body larger than the probing size"};
        }
        body.resize(actual);
        out.present = true;
        out.msg.body = std::move(body);
        unpack_common(second, label, correlation, out);
        return status{};
    }

    // Empty body: the probe pass consumed the message together with all other
    // properties.
    out.present = true;
    out.msg.body.clear();
    unpack_common(probe, label, correlation, out);
    return status{};
}

} // namespace

struct queue::impl {
    HANDLE handle = nullptr;
    std::string format_name;

    ~impl() {
        if (handle != nullptr) {
            (void)MQCloseQueue(handle);
            handle = nullptr;
        }
    }

    [[nodiscard]] status send_message(const message& msg,
                                      ITransaction* transaction) {
        if (msg.priority > max_priority) {
            return status{rpc_code::invalid_argument,
                          "message priority must be within 0..7"};
        }
        std::wstring label = detail::to_wide(msg.label);
        if (label.size() > max_label_length) {
            return status{rpc_code::encode_error,
                          "message label exceeds the MSMQ limit of 249 "
                          "UTF-16 code units (250 is rejected with "
                          "MQ_ERROR_LABEL_TOO_LONG)"};
        }
        if (msg.body.size() >
            static_cast<std::size_t>(std::numeric_limits<ULONG>::max())) {
            return status{rpc_code::encode_error,
                          "message body is too large for MSMQ"};
        }

        prop_builder builder;
        const BYTE* body_data =
            msg.body.empty()
                ? nullptr
                : reinterpret_cast<const BYTE*>(msg.body.data());
        builder.bytes(PROPID_M_BODY, const_cast<BYTE*>(body_data),
                      static_cast<ULONG>(msg.body.size()));
        builder.ui4(PROPID_M_BODY_SIZE, static_cast<ULONG>(msg.body.size()));
        builder.ui4(PROPID_M_BODY_TYPE, static_cast<ULONG>(msg.body_type));
        if (!label.empty()) {
            builder.ui4(PROPID_M_LABEL_LEN, static_cast<ULONG>(label.size()));
            builder.lpwstr(PROPID_M_LABEL, &label[0]);
        }
        builder.ui1(PROPID_M_PRIORITY, msg.priority);
        builder.ui1(PROPID_M_DELIVERY,
                    msg.recoverable ? MQMSG_DELIVERY_RECOVERABLE
                                    : MQMSG_DELIVERY_EXPRESS);
        builder.ui4(PROPID_M_APPSPECIFIC, msg.app_specific);
        builder.bytes(PROPID_M_CORRELATIONID,
                      const_cast<BYTE*>(reinterpret_cast<const BYTE*>(
                          msg.correlation_id.data())),
                      PROPID_M_CORRELATIONID_SIZE);
        std::wstring admin_queue;
        if (msg.ack != acknowledgement::none) {
            builder.ui1(PROPID_M_ACKNOWLEDGE,
                        static_cast<UCHAR>(
                            static_cast<std::uint32_t>(msg.ack)));
            if (!msg.admin_queue.empty()) {
                admin_queue = detail::to_wide(msg.admin_queue);
                if (admin_queue.empty()) {
                    return status{rpc_code::invalid_argument,
                                  "message admin queue could not be encoded"};
                }
                builder.ui4(PROPID_M_ADMIN_QUEUE_LEN,
                            static_cast<ULONG>(admin_queue.size()));
                builder.lpwstr(PROPID_M_ADMIN_QUEUE, &admin_queue[0]);
            }
        }

        MQMSGPROPS props;
        builder.make(props);
        const HRESULT hr = MQSendMessage(handle, &props, transaction);
        if (FAILED(hr)) {
            return status(detail::mq_error("MQSendMessage failed", hr));
        }
        return status{};
    }

    [[nodiscard]] result<received_message> receive_message(
        std::chrono::milliseconds timeout, ITransaction* transaction) {
        received_message out;
        const crpc::status outcome =
            receive_once(handle, timeout, transaction, out);
        if (!outcome) {
            return outcome.error();
        }
        return out;
    }
};

#endif // CRPC_OS_WINDOWS

#if !defined(CRPC_OS_WINDOWS)
// MSMQ only exists on Windows: nothing on this platform ever creates an impl.
// The out-of-line constructor and destructor below still need the type to be
// complete so the member unique_ptr can be destroyed, hence this empty
// definition.
struct queue::impl {};
#endif // !CRPC_OS_WINDOWS

queue::queue(std::unique_ptr<impl> pimpl) noexcept : impl_(std::move(pimpl)) {}

queue::~queue() = default;

result<std::unique_ptr<queue>> queue::open(const queue_options& options) {
#if defined(CRPC_OS_WINDOWS)
    if (options.path.empty()) {
        return rpc_error{rpc_code::invalid_argument,
                         "queue.open: a queue path is required"};
    }
    // A combined access value is not implementable: this host's Message Queuing
    // service rejects it with MQ_ERROR_UNSUPPORTED_ACCESS_MODE, and an
    // individual handle grants only the single direction it was opened for.
    // Refuse at the API boundary with the reason rather than surfacing a
    // service HRESULT that would make the caller guess.
    if (options.access != queue_access::receive &&
        options.access != queue_access::send &&
        options.access != queue_access::peek) {
        return rpc_error{rpc_code::invalid_argument,
                         "queue.open: access must be exactly one of "
                         "queue_access::{receive, send, peek}; a single MSMQ "
                         "handle cannot combine directions"};
    }

    std::string format_name;
    if (detail::starts_with_ci(options.path, "FORMATNAME:") ||
        detail::starts_with_ci(options.path, "DIRECT=")) {
        format_name = options.path;
    } else {
        const std::wstring wide_path = detail::to_wide(options.path);
        if (wide_path.empty()) {
            return rpc_error{rpc_code::invalid_argument,
                             "queue.open: the queue path could not be encoded"};
        }
        // Resolve through MQPathNameToFormatName, never by hand: the runtime
        // knows the right form for local, remote and DNS-named destinations.
        std::uint32_t capacity = std::max<std::uint32_t>(
            256u, static_cast<std::uint32_t>(wide_path.size()) * 2u + 8u);
        bool resolved = false;
        for (int attempt = 0; attempt < 4; ++attempt) {
            std::vector<wchar_t> buffer(capacity, L'\0');
            DWORD count = capacity;
            const HRESULT hr = MQPathNameToFormatName(wide_path.c_str(),
                                                      buffer.data(), &count);
            if (hr == MQ_INFORMATION_FORMATNAME_BUFFER_TOO_SMALL) {
                if (count <= capacity) {
                    break; // no progress: refuse to loop forever
                }
                capacity = count;
                continue;
            }
            if (FAILED(hr)) {
                return detail::mq_error(
                    "queue.open: MQPathNameToFormatName failed", hr);
            }
            format_name = detail::to_utf8(buffer.data(), count);
            resolved = true;
            break;
        }
        if (!resolved) {
            return rpc_error{
                rpc_code::internal,
                "queue.open: MQPathNameToFormatName did not converge"};
        }
    }

    const std::wstring wide_format = detail::to_wide(format_name);
    if (wide_format.empty()) {
        return rpc_error{rpc_code::invalid_argument,
                         "queue.open: the resolved format name could not be "
                         "encoded"};
    }

    HANDLE opened = nullptr;
    const HRESULT hr = MQOpenQueue(wide_format.c_str(), options.access,
                                   MQ_DENY_NONE, &opened);
    if (FAILED(hr) || opened == nullptr) {
        return detail::mq_error("queue.open: MQOpenQueue failed", hr);
    }

    std::unique_ptr<impl> state(new impl());
    state->handle = opened;
    state->format_name = std::move(format_name);
    return std::unique_ptr<queue>(new queue(std::move(state)));
#else
    (void)options;
    return rpc_error{rpc_code::unavailable,
                     "queue.open: MSMQ is only available on Windows"};
#endif
}

status queue::send(const message& msg) {
#if defined(CRPC_OS_WINDOWS)
    if (impl_ == nullptr || impl_->handle == nullptr) {
        return status{rpc_code::shutting_down, "queue.send: the queue is closed"};
    }
    return impl_->send_message(msg, nullptr);
#else
    (void)msg;
    return status{rpc_code::unavailable,
                  "queue.send: MSMQ is only available on Windows"};
#endif
}

status queue::send(const message& msg, transaction& tx) {
#if defined(CRPC_OS_WINDOWS)
    if (impl_ == nullptr || impl_->handle == nullptr) {
        return status{rpc_code::shutting_down, "queue.send: the queue is closed"};
    }
    return impl_->send_message(
        msg, static_cast<ITransaction*>(transaction_handle(tx)));
#else
    (void)msg;
    (void)tx;
    return status{rpc_code::unavailable,
                  "queue.send: MSMQ is only available on Windows"};
#endif
}

result<received_message> queue::receive(std::chrono::milliseconds timeout) {
#if defined(CRPC_OS_WINDOWS)
    if (impl_ == nullptr || impl_->handle == nullptr) {
        return rpc_error{rpc_code::shutting_down,
                         "queue.receive: the queue is closed"};
    }
    return impl_->receive_message(timeout, nullptr);
#else
    (void)timeout;
    return rpc_error{rpc_code::unavailable,
                     "queue.receive: MSMQ is only available on Windows"};
#endif
}

result<received_message> queue::receive(std::chrono::milliseconds timeout,
                                        transaction& tx) {
#if defined(CRPC_OS_WINDOWS)
    if (impl_ == nullptr || impl_->handle == nullptr) {
        return rpc_error{rpc_code::shutting_down,
                         "queue.receive: the queue is closed"};
    }
    return impl_->receive_message(
        timeout, static_cast<ITransaction*>(transaction_handle(tx)));
#else
    (void)timeout;
    (void)tx;
    return rpc_error{rpc_code::unavailable,
                     "queue.receive: MSMQ is only available on Windows"};
#endif
}

status queue::close() noexcept {
#if defined(CRPC_OS_WINDOWS)
    if (impl_ != nullptr && impl_->handle != nullptr) {
        (void)MQCloseQueue(impl_->handle);
        impl_->handle = nullptr;
    }
    return status{};
#else
    return status{rpc_code::unavailable,
                  "queue.close: MSMQ is only available on Windows"};
#endif
}

bool queue::is_open() const noexcept {
#if defined(CRPC_OS_WINDOWS)
    return impl_ != nullptr && impl_->handle != nullptr;
#else
    return false;
#endif
}

const std::string& queue::format_name() const noexcept {
    static const std::string empty_name;
#if defined(CRPC_OS_WINDOWS)
    return impl_ != nullptr ? impl_->format_name : empty_name;
#else
    return empty_name;
#endif
}

} // namespace msmq
} // namespace mq
} // namespace crpc