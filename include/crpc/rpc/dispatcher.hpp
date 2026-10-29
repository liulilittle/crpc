// CRPC -- service registry, method slots and server-side dispatch.
//
// The dispatcher is the server half of the RPC core. A `service_registry` maps
// a numeric service id to a set of methods, each method being a type-erased
// function pointer plus the user object it runs on. The registry is mutable
// during startup and frozen before traffic: after `freeze()` no lookup takes a
// lock and no registration is accepted.
//
// `dispatcher::dispatch` takes one decoded REQUEST frame and produces the
// RESPONSE payload:
//
//   * unknown service            -> not_found
//   * unknown method             -> no_such_method
//   * malformed call envelope    -> decode_error
//   * an already-expired deadline -> deadline_exceeded, handler not invoked
//   * a throwing handler         -> internal (no exception escapes dispatch)
//
// The response payload layout (shared with client.hpp, implemented in
// dispatcher.cpp) is:
//
//   success:  0x00 || body
//   error:    0x01 || code:varint || retryable:u8 || retry_after_ms:varint
//                     || message_len:u16 || message
//                     || meta_count:u16 || (key_len:u16 key value_len:u16 value)*
//
// Thread safety and allocation
// ----------------------------
//   * `service_registry` registration is not synchronized; perform it from one
//     thread before `freeze()`. Lookups are safe from any number of threads
//     once frozen (they only read immutable maps).
//   * `method_slot::enter/leave` and the service in-flight counter are atomic.
//   * `dispatch` touches no shared mutable state except those counters; the
//     handler is responsible for its own thread safety.
//   * `dispatch` allocates only the response buffer, which the caller owns.
#ifndef CRPC_RPC_DISPATCHER_HPP
#define CRPC_RPC_DISPATCHER_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/frame.hpp>
#include <crpc/rpc/call.hpp>
#include <crpc/rpc/dedup.hpp>
#include <crpc/span.hpp>

namespace crpc::rpc {

// --- Response payload codec --------------------------------------------------

// Encodes a success (code == ok) or error response into `out`. Clears `out`
// first. `body` is the application response body on the success path. Returns
// rpc_code::encode_error when the buffer cannot grow.
[[nodiscard]] CRPC_API status encode_response(rpc_code code, const rpc_error& error,
                                              span<const std::byte> body, buffer& out) noexcept;

// Decodes a response payload. On success `body` borrows `in` and is only valid
// for as long as `in` is. On failure `code`, `error` and `body` are unchanged.
[[nodiscard]] CRPC_API status decode_response(span<const std::byte> in, rpc_code& code,
                                              rpc_error& error, span<const std::byte>& body) noexcept;

// --- Method registration -----------------------------------------------------

// Optional per-method knobs.
struct method_options {
    // 0 means unlimited. A non-zero value is a bulkhead: dispatch rejects the
    // call with rpc_code::overload when the method already has this many calls
    // in flight.
    u32 max_concurrency = 0;
};

// The type-erased handler signature. It is deliberately *not* noexcept: a user
// handler may throw and dispatch will map the exception to `internal`.
using method_handler = status (*)(void* self, const call_envelope& envelope,
                                  span<const std::byte> body, buffer& response,
                                  call_context& ctx);

class CRPC_API method_slot {
public:
    method_slot() noexcept = default;
    method_slot(u64 service_id, u64 method_id, std::string name, method_handler handler,
                void* self, method_options options) noexcept;

    [[nodiscard]] u64 service_id() const noexcept { return service_id_; }
    [[nodiscard]] u64 method_id() const noexcept { return method_id_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] u32 max_concurrency() const noexcept { return max_concurrency_; }
    [[nodiscard]] u32 in_flight() const noexcept {
        return in_flight_.load(std::memory_order_acquire);
    }

    // Bulkhead admission. Always increments the counter on success; the caller
    // must pair it with `leave()`. With no limit it still tracks in-flight.
    // Const so a frozen-registry lookup (which returns a const pointer) can
    // still admit a call; the counter is mutable.
    [[nodiscard]] bool enter() const noexcept;

    // Releases one admission.
    void leave() const noexcept;

    // Calls the handler. Does not catch; `dispatcher::dispatch` does.
    [[nodiscard]] status invoke(const call_envelope& envelope, span<const std::byte> body,
                                buffer& response, call_context& ctx) const;

private:
    u64 service_id_ = 0;
    u64 method_id_ = 0;
    std::string name_;
    method_handler handler_ = nullptr;
    void* self_ = nullptr;
    u32 max_concurrency_ = 0;
    mutable std::atomic<u32> in_flight_{0};
};

class CRPC_API service_slot {
public:
    service_slot(u64 id, std::string name);

    service_slot(const service_slot&) = delete;
    service_slot& operator=(const service_slot&) = delete;

    [[nodiscard]] u64 id() const noexcept { return id_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] u32 in_flight() const noexcept {
        return in_flight_.load(std::memory_order_acquire);
    }

    // Per-service advisory in-flight counter (never blocks).
    void enter() const noexcept { in_flight_.fetch_add(1, std::memory_order_acq_rel); }
    void leave() const noexcept {
        u32 current = in_flight_.load(std::memory_order_relaxed);
        while (current != 0 &&
               !in_flight_.compare_exchange_weak(current, current - 1, std::memory_order_acq_rel,
                                                 std::memory_order_relaxed)) {
        }
    }

    [[nodiscard]] const method_slot* find_method(u64 method_id) const noexcept;
    [[nodiscard]] std::size_t method_count() const noexcept { return methods_.size(); }

    // Read-only traversal used by startup code to pre-build a frozen guard
    // table. Call it after `freeze()`; it does not allocate or lock.
    template <typename Fn>
    void for_each_method(Fn&& fn) const {
        for (const auto& entry : methods_) {
            fn(*entry.second);
        }
    }

private:
    friend class service_registry;

    u64 id_ = 0;
    std::string name_;
    mutable std::atomic<u32> in_flight_{0};
    std::unordered_map<u64, std::unique_ptr<method_slot>> methods_;
};

// --- Service registry --------------------------------------------------------

class CRPC_API service_registry {
public:
    service_registry();
    ~service_registry();

    service_registry(const service_registry&) = delete;
    service_registry& operator=(const service_registry&) = delete;
    service_registry(service_registry&&) = delete;
    service_registry& operator=(service_registry&&) = delete;

    // Registers a service id. Fails with failed_precondition on a duplicate or
    // after freeze, and with invalid_argument on id 0.
    [[nodiscard]] status register_service(u64 service_id, std::string name);

    // Registers a method under an existing service. Fails with not_found when
    // the service is unknown, failed_precondition on a duplicate or after
    // freeze, and invalid_argument on method id 0 or a null handler.
    [[nodiscard]] status register_method(u64 service_id, u64 method_id, std::string name,
                                         method_handler handler, void* self,
                                         method_options options = {});

    // Strongly typed overloads. They forward to the `u64` versions and make a
    // swapped service/method argument list a compile error, which the raw `u64`
    // signature cannot catch.
    [[nodiscard]] status register_service(service_id service, std::string name) {
        return register_service(service.value, std::move(name));
    }
    [[nodiscard]] status register_method(service_id service, method_id method,
                                         std::string name, method_handler handler,
                                         void* self, method_options options = {}) {
        return register_method(service.value, method.value, std::move(name), handler,
                               self, options);
    }

    // Freezes the registry. Idempotent; after this call registration fails and
    // lookups are lock-free.
    [[nodiscard]] status freeze() noexcept;
    [[nodiscard]] bool frozen() const noexcept {
        return frozen_.load(std::memory_order_acquire);
    }

    [[nodiscard]] const service_slot* find_service(u64 service_id) const noexcept;
    [[nodiscard]] const method_slot* find_method(u64 service_id, u64 method_id) const noexcept;

    [[nodiscard]] std::size_t service_count() const noexcept { return services_.size(); }
    [[nodiscard]] std::size_t method_count() const noexcept;

    // Read-only traversal used by startup code to pre-build a frozen guard
    // table. Call it after `freeze()`; it does not allocate or lock.
    template <typename Fn>
    void for_each_service(Fn&& fn) const {
        for (const auto& entry : services_) {
            fn(*entry.second);
        }
    }

private:
    std::unordered_map<u64, std::unique_ptr<service_slot>> services_;
    std::atomic<bool> frozen_{false};
};

// --- Dispatcher --------------------------------------------------------------

class CRPC_API dispatcher {
public:
    explicit dispatcher(service_registry& registry) noexcept : registry_(&registry) {}

    // A dispatcher with a configured idempotency dedup table. The options are
    // consumed by this INLINE constructor, so their layout never crosses the
    // shared-library boundary (docs/abi.md section 4). Note that adding a data
    // member to `dispatcher` grows the type: a consumer and the library must be
    // rebuilt from the same headers, which this cycle already requires.
    dispatcher(service_registry& registry, const dedup_options& dedup) noexcept
        : registry_(&registry), dedup_(dedup) {}

    // Dispatches one REQUEST (or ONE_WAY) frame.
    //
    // `response_out` receives the RESPONSE payload when `has_response` is true.
    // `now` is the monotonic clock used for the deadline check; a request whose
    // deadline is already in the past fails with deadline_exceeded and the
    // handler is not invoked.
    //
    // Returns a non-ok status only when the dispatcher itself failed (for
    // example the response buffer could not grow); application failures are
    // carried in the response payload.
    [[nodiscard]] status dispatch(const frame& request, buffer& response_out, bool& has_response,
                                  std::chrono::steady_clock::time_point now) noexcept;

    // Same as above, but reports whether the handler was invoked. `handler_ran`
    // is set to false first and to true right before the handler runs, so a
    // caller can tell a pre-execution refusal (unknown service/method,
    // exhausted admission, expired wire deadline) apart from a result produced
    // while running the handler.
    [[nodiscard]] status dispatch(const frame& request, buffer& response_out, bool& has_response,
                                  std::chrono::steady_clock::time_point now,
                                  bool& handler_ran) noexcept;

    // Same as above with idempotency dedup enabled for this call.
    //
    // When the request carries a non-empty `call_id`, the dispatcher consults
    // the table before running the handler: a fresh key executes, a key that is
    // still in flight suspends the caller until the leader publishes and then
    // resends its exact response, and a completed key resends the cached
    // response without running the handler. `dedup_wait_key` is the opaque
    // connection identity handed to `dedup_table::abandon` when that connection
    // closes; null means the wait is only bounded by time, never abandoned.
    // See `rpc/dedup.hpp` for the full semantics.
    [[nodiscard]] status dispatch(const frame& request, buffer& response_out, bool& has_response,
                                  std::chrono::steady_clock::time_point now, bool& handler_ran,
                                  const void* dedup_wait_key) noexcept;

    [[nodiscard]] const service_registry& registry() const noexcept { return *registry_; }

    // The server-side idempotency dedup table. The server calls `abandon` on
    // connection teardown; tests read `stats()`.
    [[nodiscard]] dedup_table& dedup() noexcept { return dedup_; }

private:
    service_registry* registry_;
    // Declared after `registry_` and constructed by the inline constructors, so
    // a consumer and the library always agree on its size (same headers).
    dedup_table dedup_;
};

// --- Typed method trampoline -------------------------------------------------

// Derives the request and response types of a handler from its member-pointer
// type. A signature not listed here has no trait and therefore cannot be
// registered with the typed helper.
template <typename>
struct method_traits;

template <typename Ctl, typename Resp, typename Req>
struct method_traits<Resp (Ctl::*)(const Req&)> {
    using controller_type = Ctl;
    using request_type = Req;
    using response_type = Resp;
    static constexpr bool has_context = false;
};

template <typename Ctl, typename Resp, typename Req>
struct method_traits<Resp (Ctl::*)(const Req&, call_context&)> {
    using controller_type = Ctl;
    using request_type = Req;
    using response_type = Resp;
    static constexpr bool has_context = true;
};

template <typename Ctl, typename Resp>
struct method_traits<Resp (Ctl::*)()> {
    using controller_type = Ctl;
    using request_type = void;
    using response_type = Resp;
    static constexpr bool has_context = false;
};

template <typename Ctl, typename Resp>
struct method_traits<Resp (Ctl::*)(call_context&)> {
    using controller_type = Ctl;
    using request_type = void;
    using response_type = Resp;
    static constexpr bool has_context = true;
};

// Adapts a typed member function into a `method_handler`. The request is
// decoded with `call_codec<Req>` and the response is encoded with
// `call_codec<Resp>`, so no codec registry or RTTI is required.
template <typename Ctl, auto Method>
status method_trampoline(void* self, const call_envelope&, span<const std::byte> body,
                         buffer& out, call_context& ctx) {
    using traits = method_traits<decltype(Method)>;
    using request_type = typename traits::request_type;
    using response_type = typename traits::response_type;
    Ctl* controller = static_cast<Ctl*>(self);

    if constexpr (std::is_void_v<request_type>) {
        if constexpr (traits::has_context) {
            if constexpr (std::is_void_v<response_type>) {
                (controller->*Method)(ctx);
                return status{};
            } else {
                const response_type value = (controller->*Method)(ctx);
                return call_codec<response_type>::encode(value, out);
            }
        } else {
            if constexpr (std::is_void_v<response_type>) {
                (controller->*Method)();
                return status{};
            } else {
                const response_type value = (controller->*Method)();
                return call_codec<response_type>::encode(value, out);
            }
        }
    } else {
        request_type request{};
        const status decoded = call_codec<request_type>::decode(body, request);
        if (!decoded.ok()) {
            return decoded;
        }
        if constexpr (traits::has_context) {
            if constexpr (std::is_void_v<response_type>) {
                (controller->*Method)(request, ctx);
                return status{};
            } else {
                const response_type value = (controller->*Method)(request, ctx);
                return call_codec<response_type>::encode(value, out);
            }
        } else {
            if constexpr (std::is_void_v<response_type>) {
                (controller->*Method)(request);
                return status{};
            } else {
                const response_type value = (controller->*Method)(request);
                return call_codec<response_type>::encode(value, out);
            }
        }
    }
}

// Convenience: registers a typed member function.
template <typename Ctl, auto Method>
status register_typed_method(service_registry& registry, u64 service_id, u64 method_id,
                             std::string name, Ctl& instance, method_options options = {}) {
    return registry.register_method(service_id, method_id, std::move(name),
                                    &method_trampoline<Ctl, Method>, &instance, options);
}

} // namespace crpc::rpc

#endif // CRPC_RPC_DISPATCHER_HPP
