// CRPC -- service registry and server-side dispatch implementation.
#include <crpc/rpc/dispatcher.hpp>

#include <string>

#include <crpc/wire.hpp>

namespace crpc::rpc {
namespace {

void append_u16(buffer& out, u16 value) {
    std::byte tmp[2];
    endian::store_le16(tmp, value);
    (void)out.append(tmp, sizeof(tmp));
}

void append_varint(buffer& out, u64 value) {
    while (value >= 0x80u) {
        (void)out.push_back(static_cast<std::byte>((value & 0x7Fu) | 0x80u));
        value >>= 7;
    }
    (void)out.push_back(static_cast<std::byte>(value));
}

u16 clamp_u16(std::size_t value) {
    return value > 0xFFFFu ? static_cast<u16>(0xFFFFu) : static_cast<u16>(value);
}

// Produces an error response unless the request was a fire-and-forget ONE_WAY.
status respond_error(buffer& out, bool& has_response, bool wants_response, rpc_code code,
                     std::string message) {
    if (!wants_response) {
        return status{};
    }
    has_response = true;
    const rpc_error error(code, std::move(message));
    return encode_response(code, error, span<const std::byte>(), out);
}

// Rebuilds the per-call context a handler sees from the decoded envelope.
// `now` is the dispatcher's reference instant; the wire carries the time left,
// so the local deadline is `now` plus that duration.
call_context build_context(const call_envelope& envelope,
                           std::chrono::steady_clock::time_point now) {
    call_context ctx;
    const trace_extension& ext = envelope.extensions;
    ctx.has_deadline = ext.has(trace_extension::has_deadline);
    if (ext.has(trace_extension::has_deadline)) {
        ctx.deadline = now + std::chrono::nanoseconds(
                                 static_cast<std::chrono::nanoseconds::rep>(ext.deadline_ns));
    }
    if (ext.has(trace_extension::has_trace_id)) {
        ctx.trace.trace.bytes = ext.trace_id;
    }
    if (ext.has(trace_extension::has_span_id)) {
        ctx.trace.span.bytes = ext.span_id;
    }
    if (ext.has(trace_extension::has_parent_span_id)) {
        ctx.trace.parent.bytes = ext.parent_span_id;
    }
    if (ext.has(trace_extension::has_idempotency_key)) {
        ctx.has_idempotency_key = true;
        ctx.idempotency = call_id::decode(ext.idempotency_key);
    }
    return ctx;
}

} // namespace

// --- Response payload codec --------------------------------------------------

status encode_response(rpc_code code, const rpc_error& error, span<const std::byte> body,
                       buffer& out) noexcept {
    out.clear();
    if (code == rpc_code::ok) {
        if (!out.push_back(std::byte{0})) {
            return status{rpc_code::encode_error};
        }
        if (!body.empty() && !out.append(body)) {
            return status{rpc_code::encode_error};
        }
        return status{};
    }

    if (!out.push_back(std::byte{1})) {
        return status{rpc_code::encode_error};
    }
    append_varint(out, static_cast<u64>(code));
    if (!out.push_back(error.retryable ? std::byte{1} : std::byte{0})) {
        return status{rpc_code::encode_error};
    }
    append_varint(out, static_cast<u64>(error.retry_after.count()));

    const u16 message_length = clamp_u16(error.message.size());
    append_u16(out, message_length);
    if (message_length != 0 && !out.append(error.message.data(), message_length)) {
        return status{rpc_code::encode_error};
    }

    const u16 meta_count = clamp_u16(error.metadata.size());
    append_u16(out, meta_count);
    std::size_t written = 0;
    for (const auto& entry : error.metadata) {
        if (written >= meta_count) {
            break;
        }
        ++written;
        const u16 key_length = clamp_u16(entry.first.size());
        append_u16(out, key_length);
        if (key_length != 0 && !out.append(entry.first.data(), key_length)) {
            return status{rpc_code::encode_error};
        }
        const u16 value_length = clamp_u16(entry.second.size());
        append_u16(out, value_length);
        if (value_length != 0 && !out.append(entry.second.data(), value_length)) {
            return status{rpc_code::encode_error};
        }
    }
    return status{};
}

status decode_response(span<const std::byte> in, rpc_code& code, rpc_error& error,
                       span<const std::byte>& body) noexcept {
    byte_reader reader(in);
    u8 status_byte = 0;
    if (!reader.get_u8(status_byte)) {
        return status{rpc_code::decode_error};
    }
    if (status_byte == 0) {
        code = rpc_code::ok;
        error = rpc_error{};
        body = reader.remaining_bytes();
        return status{};
    }
    if (status_byte != 1) {
        return status{rpc_code::protocol_error};
    }

    u64 raw_code = 0;
    if (!reader.get_varint(raw_code)) {
        return status{rpc_code::decode_error};
    }
    rpc_error decoded;
    if (raw_code > static_cast<u64>(rpc_code::application)) {
        decoded.metadata.set("crpc.raw_code", std::to_string(raw_code));
        decoded.code = rpc_code::internal;
    } else {
        decoded.code = static_cast<rpc_code>(raw_code);
    }

    u8 retryable = 0;
    u64 retry_after = 0;
    u16 message_length = 0;
    if (!reader.get_u8(retryable) || !reader.get_varint(retry_after) ||
        !reader.get_u16(message_length)) {
        return status{rpc_code::decode_error};
    }
    span<const std::byte> message;
    if (!reader.get_bytes(message_length, message)) {
        return status{rpc_code::decode_error};
    }
    decoded.message.assign(reinterpret_cast<const char*>(message.data()), message.size());

    u16 meta_count = 0;
    if (!reader.get_u16(meta_count)) {
        return status{rpc_code::decode_error};
    }
    for (u16 i = 0; i < meta_count; ++i) {
        u16 key_length = 0;
        u16 value_length = 0;
        span<const std::byte> key;
        span<const std::byte> value;
        if (!reader.get_u16(key_length) || !reader.get_bytes(key_length, key) ||
            !reader.get_u16(value_length) || !reader.get_bytes(value_length, value)) {
            return status{rpc_code::decode_error};
        }
        decoded.metadata.set(std::string(reinterpret_cast<const char*>(key.data()), key.size()),
                             std::string(reinterpret_cast<const char*>(value.data()), value.size()));
    }
    decoded.retryable = retryable != 0;
    decoded.retry_after = std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(retry_after));

    code = decoded.code;
    error = std::move(decoded);
    body = span<const std::byte>();
    return status{};
}

// --- method_slot -------------------------------------------------------------

method_slot::method_slot(u64 service_id, u64 method_id, std::string name,
                         method_handler handler, void* self, method_options options) noexcept
    : service_id_(service_id),
      method_id_(method_id),
      name_(std::move(name)),
      handler_(handler),
      self_(self),
      max_concurrency_(options.max_concurrency) {}

bool method_slot::enter() const noexcept {
    const u32 limit = max_concurrency_;
    u32 current = in_flight_.load(std::memory_order_relaxed);
    for (;;) {
        if (limit != 0 && current >= limit) {
            return false;
        }
        if (in_flight_.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
            return true;
        }
    }
}

void method_slot::leave() const noexcept {
    u32 current = in_flight_.load(std::memory_order_relaxed);
    while (current != 0 &&
           !in_flight_.compare_exchange_weak(current, current - 1,
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
    }
}

status method_slot::invoke(const call_envelope& envelope, span<const std::byte> body,
                           buffer& response, call_context& ctx) const {
    if (handler_ == nullptr) {
        return status{rpc_code::internal};
    }
    return handler_(self_, envelope, body, response, ctx);
}

// --- service_slot ------------------------------------------------------------

service_slot::service_slot(u64 id, std::string name) : id_(id), name_(std::move(name)) {}

const method_slot* service_slot::find_method(u64 method_id) const noexcept {
    const auto it = methods_.find(method_id);
    return it == methods_.end() ? nullptr : it->second.get();
}

// --- service_registry --------------------------------------------------------

service_registry::service_registry() = default;
service_registry::~service_registry() = default;

status service_registry::register_service(u64 service_id, std::string name) {
    if (frozen()) {
        return status{rpc_code::failed_precondition};
    }
    if (service_id == 0) {
        return status{rpc_code::invalid_argument};
    }
    if (services_.find(service_id) != services_.end()) {
        return status{rpc_code::failed_precondition};
    }
    auto service = std::make_unique<service_slot>(service_id, std::move(name));
    services_.emplace(service_id, std::move(service));
    return status{};
}

status service_registry::register_method(u64 service_id, u64 method_id, std::string name,
                                         method_handler handler, void* self,
                                         method_options options) {
    if (frozen()) {
        return status{rpc_code::failed_precondition};
    }
    if (method_id == 0 || handler == nullptr) {
        return status{rpc_code::invalid_argument};
    }
    const auto it = services_.find(service_id);
    if (it == services_.end()) {
        return status{rpc_code::not_found};
    }
    if (it->second->methods_.find(method_id) != it->second->methods_.end()) {
        return status{rpc_code::failed_precondition};
    }
    it->second->methods_.emplace(
        method_id, std::make_unique<method_slot>(service_id, method_id, std::move(name), handler,
                                                 self, options));
    return status{};
}

status service_registry::freeze() noexcept {
    frozen_.store(true, std::memory_order_release);
    return status{};
}

const service_slot* service_registry::find_service(u64 service_id) const noexcept {
    const auto it = services_.find(service_id);
    return it == services_.end() ? nullptr : it->second.get();
}

const method_slot* service_registry::find_method(u64 service_id, u64 method_id) const noexcept {
    const service_slot* service = find_service(service_id);
    return service == nullptr ? nullptr : service->find_method(method_id);
}

std::size_t service_registry::method_count() const noexcept {
    std::size_t total = 0;
    for (const auto& entry : services_) {
        total += entry.second->method_count();
    }
    return total;
}

// --- dispatcher --------------------------------------------------------------

status dispatcher::dispatch(const frame& request, buffer& response_out, bool& has_response,
                            std::chrono::steady_clock::time_point now) noexcept {
    has_response = false;
    response_out.clear();

    const bool one_way = request.header.type == frame_type::one_way;
    if (request.header.type != frame_type::request && !one_way) {
        return status{rpc_code::protocol_error};
    }
    const bool wants_response = !one_way;

    call_envelope envelope;
    const status decoded = decode_call_envelope(request.payload, envelope);
    if (!decoded.ok()) {
        const std::string message =
            decoded.message().empty() ? "request decode failed" : decoded.message();
        return respond_error(response_out, has_response, wants_response, rpc_code::decode_error,
                             message);
    }

    const service_slot* service = registry_->find_service(envelope.service_id);
    if (service == nullptr) {
        return respond_error(response_out, has_response, wants_response, rpc_code::not_found,
                             "unknown service");
    }
    const method_slot* method = service->find_method(envelope.method_id);
    if (method == nullptr) {
        return respond_error(response_out, has_response, wants_response, rpc_code::no_such_method,
                             "unknown method");
    }

    if (envelope.extensions.has(trace_extension::has_deadline)) {
        // Relative deadline (design section 8.5): refuse to start work that
        // cannot finish. 1 ms is the documented minimum remaining time.
        constexpr u64 min_remaining_ns = 1'000'000;
        if (envelope.extensions.deadline_ns < min_remaining_ns) {
            return respond_error(response_out, has_response, wants_response,
                                 rpc_code::deadline_exceeded, "deadline already passed");
        }
    }

    if (!method->enter()) {
        return respond_error(response_out, has_response, wants_response, rpc_code::overload,
                             "method concurrency limit reached");
    }
    service->enter();

    call_context ctx = build_context(envelope, now);
    buffer body_out;
    status handler_status;
    try {
        handler_status = method->invoke(envelope, envelope.body, body_out, ctx);
    } catch (const std::exception& ex) {
        handler_status = status{rpc_error(rpc_code::internal, std::string("handler threw: ") + ex.what())};
    } catch (...) {
        handler_status = status{rpc_error(rpc_code::internal, "handler threw an exception")};
    }

    service->leave();
    method->leave();

    if (!handler_status.ok()) {
        return respond_error(response_out, has_response, wants_response, handler_status.code(),
                             handler_status.message());
    }
    if (!wants_response) {
        return status{};
    }
    has_response = true;
    return encode_response(rpc_code::ok, rpc_error{}, body_out.readable(), response_out);
}

} // namespace crpc::rpc

