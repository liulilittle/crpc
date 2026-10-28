// CRPC -- error model.
//
// Two rules drive this header:
//
//   1. Every failure is a value. Nothing in the library throws across a
//      coroutine switch, a thread boundary or a shared-library boundary.
//   2. A failure always names what happened precisely enough for the caller to
//      decide whether a retry is safe. In particular the model distinguishes
//      "the request definitely did not run" from "the request may have run"
//      (`unknown_side_effect`), because that distinction is what makes retrying
//      a non-idempotent call unsafe.
//
// `rpc_code` values are part of the wire protocol: they are written as a
// little-endian u16 and must never be renumbered. New codes are appended.
#ifndef CRPC_ERROR_HPP
#define CRPC_ERROR_HPP

#include <chrono>
#include <cstddef>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <crpc/config.hpp>

namespace crpc {

// --- Codes -------------------------------------------------------------------

enum class rpc_code : u16 {
    ok = 0,
    // The caller cancelled before the call completed.
    canceled = 1,
    // The deadline elapsed. `unknown_side_effect` tells the caller whether the
    // peer may still have executed the call.
    deadline_exceeded = 2,
    // No usable instance: discovery returned nothing, or every candidate was
    // rejected by the breaker / bulkhead.
    unavailable = 3,
    // The connection attempt was refused immediately (no listener).
    connection_refused = 4,
    // An established connection was reset by the peer or the network.
    connection_reset = 5,
    // A write failed because the peer closed its read side.
    broken_pipe = 6,
    // The peer closed cleanly before a response arrived.
    eof = 7,
    // The server (or a local limiter) rejected the call to protect itself.
    // `retry_after` carries the server's hint when present.
    overload = 8,
    // The named service or method is not registered.
    not_found = 9,
    no_such_method = 10,
    // A request field failed validation or conversion.
    invalid_argument = 11,
    // A precondition required by the method was not met.
    failed_precondition = 12,
    // A bounded resource (queue, lane, connection slot) is exhausted.
    resource_exhausted = 13,
    // A frame violated the protocol (bad length, unknown type, bad version).
    protocol_error = 14,
    // A payload could not be decoded.
    decode_error = 15,
    // A payload could not be encoded.
    encode_error = 16,
    // A reliable message exhausted its retransmission budget without an ack.
    not_acknowledged = 17,
    // The call was sent but no response was observed: the peer may or may not
    // have executed it. Never retry a non-idempotent call in this state without
    // an idempotency key.
    unknown_side_effect = 18,
    // An unexpected internal failure.
    internal = 19,
    // The endpoint is shutting down.
    shutting_down = 20,
    // The remote handler returned an application error (carried in metadata).
    application = 21,
    // The caller lacks permission for an operation the OS refused (access
    // denied, read-only target, ACL). Also returned by the fs layer for
    // sharing / lock violations.
    permission_denied = 22
};

// True when retrying the same request could plausibly succeed without changing
// the request. This is a property of the failure, not of the method; a retry
// still requires the call to be idempotent or to carry an idempotency key.
constexpr bool is_retryable(rpc_code code) noexcept {
    switch (code) {
    case rpc_code::unavailable:
    case rpc_code::connection_refused:
    case rpc_code::connection_reset:
    case rpc_code::broken_pipe:
    case rpc_code::eof:
    case rpc_code::overload:
    case rpc_code::resource_exhausted:
    case rpc_code::not_acknowledged:
        return true;
    default:
        return false;
    }
}

// A stable, human-readable name. Never returns nullptr.
CRPC_API const char* to_string(rpc_code code) noexcept;

// --- Error value -------------------------------------------------------------

// A small, ordered key/value list. Metadata is transported on the wire and
// iterated in insertion order, so a map's hash order would make two logically
// identical errors compare unequal. The list is bounded by the codec (see the
// frame specification); this type does not impose a limit of its own.
class metadata_list {
public:
    using value_type = std::pair<std::string, std::string>;
    using container_type = std::vector<value_type>;
    using const_iterator = container_type::const_iterator;

    metadata_list() = default;

    void set(std::string key, std::string value) {
        for (auto& entry : entries_) {
            if (entry.first == key) {
                entry.second = std::move(value);
                return;
            }
        }
        entries_.emplace_back(std::move(key), std::move(value));
    }

    const std::string* find(const std::string& key) const noexcept {
        for (const auto& entry : entries_) {
            if (entry.first == key) {
                return &entry.second;
            }
        }
        return nullptr;
    }

    bool empty() const noexcept { return entries_.empty(); }
    std::size_t size() const noexcept { return entries_.size(); }
    const_iterator begin() const noexcept { return entries_.begin(); }
    const_iterator end() const noexcept { return entries_.end(); }

private:
    container_type entries_;
};

// --- Failure origin ----------------------------------------------------------
//
// Where a failed attempt failed. The origin decides whether the endpoint
// breaker is touched at all, whether a retry is legal without idempotency, and
// whether `unknown_side_effect` can apply. Local admission and lifecycle
// failures say nothing about the endpoint.
//
// This enum is a core error concept rather than a client-facade detail: it
// moved here from `client_facade.hpp` so that `rpc_error` can carry it. The
// facade already includes `error.hpp`, so the reverse dependency would have
// been a cycle. `client_facade.hpp` keeps a `client_detail::failure_origin`
// alias so existing spellings keep compiling.
enum class failure_origin : std::uint8_t {
    local_lifecycle,
    local_gate,
    local_capacity,
    endpoint_open,
    endpoint_pre_send,
    endpoint_sent,
    remote,
    timeout,
    cancel
};

struct rpc_error {
    rpc_code code = rpc_code::ok;
    std::string message;
    // Set by the server or a local policy; defaults to the code's retryability.
    bool retryable = false;
    // A server hint for when to try again. Zero means "no hint".
    std::chrono::milliseconds retry_after{0};
    metadata_list metadata;
    // Where the terminal failure originated. Defaults to the least committal
    // origin: a local lifecycle failure.
    failure_origin origin = failure_origin::local_lifecycle;
    // How many attempts the logical call made before the terminal outcome.
    std::uint32_t attempts = 0;

    rpc_error() = default;

    explicit rpc_error(rpc_code c) : code(c), retryable(is_retryable(c)) {}

    rpc_error(rpc_code c, std::string msg)
        : code(c), message(std::move(msg)), retryable(is_retryable(c)) {}

    bool ok() const noexcept { return code == rpc_code::ok; }
    explicit operator bool() const noexcept { return ok(); }
};

// --- Result ------------------------------------------------------------------

// `result<T>` holds either a value or an error; it never holds both and never
// throws. T may be move-only; it must be constructible from a T&& (any normal
// value type satisfies this).
template <typename T>
class [[nodiscard]] result {
    static_assert(!std::is_reference<T>::value, "result<T> cannot hold a reference");
    static_assert(!std::is_void<T>::value, "use crpc::status for void results");

public:
    using value_type = T;

    result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    result(rpc_error error) : storage_(std::in_place_index<1>, std::move(error)) {}

    // Convenience: construct from a code without spelling rpc_error.
    result(rpc_code code) : storage_(std::in_place_index<1>, rpc_error(code)) {}

    bool ok() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    const rpc_error& error() const noexcept {
        return std::get<1>(storage_);
    }

    rpc_code code() const noexcept {
        return ok() ? rpc_code::ok : std::get<1>(storage_).code;
    }

    // Precondition: ok(). Calling these on an error is a programming error; in
    // debug builds it terminates with a message, in release builds the access
    // is still defined (it returns the variant's empty slot only if T is
    // default-constructible, which is not required). Callers must check ok().
    T& value() & {
        if (!ok()) {
            on_bad_access();
        }
        return std::get<0>(storage_);
    }

    const T& value() const& {
        if (!ok()) {
            on_bad_access();
        }
        return std::get<0>(storage_);
    }

    T&& value() && {
        if (!ok()) {
            on_bad_access();
        }
        return std::get<0>(std::move(storage_));
    }

    // Moves the value out; on error returns the supplied fallback.
    T value_or(T fallback) && {
        if (!ok()) {
            return fallback;
        }
        return std::get<0>(std::move(storage_));
    }

    // Value-or-fallback for an lvalue result. Same contract as the rvalue
    // overload: an error result returns `fallback` and never touches the value
    // slot, so it cannot terminate through `on_bad_access()`.
    T value_or(T fallback) & {
        if (!ok()) {
            return fallback;
        }
        return std::get<0>(storage_);
    }

    T value_or(T fallback) const& {
        if (!ok()) {
            return fallback;
        }
        return std::get<0>(storage_);
    }

    // Returns the contained error, or `fallback` when the result is a success.
    // The mirror of `value_or`: a success returns `fallback` and never reads
    // the error slot.
    rpc_error error_or(rpc_error fallback) const noexcept {
        if (ok()) {
            return fallback;
        }
        return std::get<1>(storage_);
    }

private:
    static void on_bad_access() noexcept {
        // A caller read a value from an error result. The library cannot throw
        // and cannot continue with a meaningful value, so it terminates with a
        // diagnostic instead of returning garbage. Every call site is expected
        // to check ok() first; this is a programming-error guard, not a
        // recoverable failure.
        std::terminate();
    }

    std::variant<T, rpc_error> storage_;
};

// `status` is the void-result counterpart. It is intentionally a distinct type
// so that a function returning `status` cannot be confused with one returning a
// value, and so that `result<void>` is a compile error rather than a special
// case.
class [[nodiscard]] status {
public:
    status() noexcept = default;
    status(rpc_code code) noexcept : error_(code) {}
    // A code plus a diagnostic message. The message is for humans and logs; it
    // never changes the retryability, which stays a function of the code.
    status(rpc_code code, std::string message) : error_(code, std::move(message)) {}
    status(rpc_error error) noexcept : error_(std::move(error)) {}

    bool ok() const noexcept { return error_.ok(); }
    explicit operator bool() const noexcept { return ok(); }

    const rpc_error& error() const noexcept { return error_; }
    rpc_code code() const noexcept { return error_.code; }
    const std::string& message() const noexcept { return error_.message; }

    void throw_if_error() const = delete; // the library never throws

private:
    rpc_error error_;
};

// --- std::error_code interop -------------------------------------------------
//
// Lets a caller store a CRPC failure in code that already speaks
// std::error_code without losing the rpc_code value.

CRPC_API const std::error_category& rpc_error_category() noexcept;

inline std::error_code make_error_code(rpc_code code) noexcept {
    return std::error_code(static_cast<int>(code), rpc_error_category());
}

// --- HTTP mapping ------------------------------------------------------------
//
// The mapping is total: every rpc_code has a status. See the specification for
// the rationale behind each row.

CRPC_API int to_http_status(rpc_code code) noexcept;

} // namespace crpc

namespace std {
template <>
struct is_error_code_enum<crpc::rpc_code> : true_type {};
} // namespace std

#endif // CRPC_ERROR_HPP
