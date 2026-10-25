// CRPC -- server facade: listeners, shards, dispatcher, handler pool, shutdown.
//
// Architecture
// ------------
// A `server` owns one `runtime` per shard and one listener per configured
// endpoint. Each accepted connection is bound to a shard and gets a decode
// source. The source only frames and copies complete requests into the shard's
// bounded handler queue; user code never runs on the reader. A fixed pool of
// handler coroutines pulls requests, invokes the dispatcher and writes one
// RESPONSE frame back.
//
// Bounds
// ------
//   * Per-connection in-flight calls: default 1024. Over the cap the server
//     answers immediately with `overload` and a `retry_after` hint.
//   * Per-service in-flight calls: default 65536, same fast-failure answer.
//   * Handler queue: `runtime_options::ready_capacity` (default 65536).
//   * Listener backlog and socket buffers come from `listen_options`.
//
// Shutdown order
// --------------
//   1. listener.stop()       -- no new accepts.
//   2. GOAWAY(shutting_down) -- no new calls are admitted.
//   3. drain in-flight up to `drain_timeout`.
//   4. close every connection.
//   5. stop and join every shard.
// `stop()` is idempotent and bounded by `drain_timeout` plus the runtime's
// internal quiesce budget.
//
// Thread safety
// -------------
// `start()` and `stop()` are meant to be called from a control thread, not
// from a handler or a shard coroutine. `stats()` is safe from any thread.
// Registration is startup-only; the registry is frozen in `start()`.
#ifndef CRPC_SERVER_HPP
#define CRPC_SERVER_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/frame.hpp>
#include <crpc/io.hpp>
#include <crpc/push/hub.hpp>
#include <crpc/push/wire.hpp>
#include <crpc/resilience/bulkhead.hpp>
#include <crpc/resilience/circuit_breaker.hpp>
#include <crpc/resilience/rate_limiter.hpp>
#include <crpc/rpc/dispatcher.hpp>
#include <crpc/runtime.hpp>
#include <crpc/span.hpp>

namespace crpc {

// Distinguishable result of `server::publish`.
//
// The historical return type was a bare `bool`: a caller could not tell an
// empty topic from a stopped server or an unsupported multi-shard
// configuration. This type names each case through `code` (an error_code-style
// value) and converts implicitly to `bool` (`true` only for `code::ok`), so an
// existing `if (server.publish(...))` or `CRPC_CHECK(server.publish(...))`
// keeps compiling and keeps meaning "ok". The codes are append-only.
class CRPC_API publish_result {
public:
    enum class code : u8 {
        ok = 0,
        empty_topic,
        not_running,
        unsupported_configuration,
        no_push_hub,
        internal_error
    };

    constexpr publish_result(code value = code::ok) noexcept : value_(value) {}

    [[nodiscard]] constexpr code value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool ok() const noexcept { return value_ == code::ok; }
    // Implicit for backward compatibility with the historical `bool` return.
    [[nodiscard]] constexpr operator bool() const noexcept { return ok(); }

    [[nodiscard]] constexpr bool operator==(code other) const noexcept {
        return value_ == other;
    }
    [[nodiscard]] constexpr bool operator!=(code other) const noexcept {
        return value_ != other;
    }
    [[nodiscard]] constexpr bool operator==(publish_result other) const noexcept {
        return value_ == other.value_;
    }
    [[nodiscard]] constexpr bool operator!=(publish_result other) const noexcept {
        return value_ != other.value_;
    }
    [[nodiscard]] constexpr bool operator==(bool other) const noexcept {
        return ok() == other;
    }
    [[nodiscard]] constexpr bool operator!=(bool other) const noexcept {
        return ok() != other;
    }

private:
    code value_;
};

// A stable, human-readable name. Never returns nullptr.
[[nodiscard]] CRPC_API const char* to_string(publish_result result) noexcept;

// True only for `publish_result::ok`. Equivalent to the implicit `bool`
// conversion, spelled out for a caller that prefers a predicate.
[[nodiscard]] constexpr bool is_ok(publish_result result) noexcept {
    return result.ok();
}

// Optional request/response observer. `on_request` runs on the shard thread
// before dispatch; `on_response` runs after the response frame is built.
// Implementations must be noexcept and must not block.
class CRPC_API server_interceptor {
public:
    virtual ~server_interceptor() = default;
    virtual void on_request(const frame& request) noexcept { (void)request; }
    virtual void on_response(rpc_code code) noexcept { (void)code; }
};

class CRPC_API server {
public:
    struct listener_config {
        endpoint ep;
        backend_factory* backend = nullptr;
    };

    struct options {
        // Shards == runtimes == OS threads. Default 1.
        std::size_t shards = 1;
        runtime_options runtime{};
        // Per-connection in-flight cap. Zero disables the cap.
        std::size_t per_connection_inflight = 1024;
        // Per-service in-flight cap. Zero disables the cap.
        std::size_t per_service_inflight = 65536;
        // Drain budget for `stop()`.
        std::chrono::milliseconds drain_timeout{5000};
        // Hint attached to an `overload` response.
        std::chrono::milliseconds overload_retry_after{10};
        // Server-side per-service breaker. It rejects fast when the service
        // keeps failing; disabled when `min_requests` is zero and no window
        // activity ever opens it.
        resilience::breaker_config breaker{};
        // Connection-level flood protection applied in `submit`, before the
        // per-connection in-flight cap and before any job is allocated. The
        // bucket belongs to the connection and is not part of the per-service
        // six-stage pipeline. Null disables it (the default).
        std::optional<resilience::rate_limit_config> per_connection_rate;
        // Optional per-service admission stages. Null means disabled. The
        // bulkhead capacity defaults to the configured capacity; a zero
        // capacity falls back to `per_service_inflight`. A zero
        // `per_service_inflight` means unlimited and builds no bulkhead.
        std::optional<resilience::rate_limit_config> per_service_rate;
        std::optional<resilience::bulkhead_config> per_service_bulkhead;
        // When false, the per-connection/per-service rate and bulkhead stages
        // are skipped entirely. The per-service circuit breaker stays active,
        // so this matches the pre-pipeline behaviour.
        bool resilience_enabled = true;
        bool send_goaway_on_stop = true;

        // Per-connection bound on PUSH bytes staged for the transport (the
        // in-flight push plus the send queue), independent of the response
        // in-flight bound. When it is reached the push pump stops calling
        // `try_take`; messages stay in the hub's bounded per-subscription queue
        // and its overflow policy decides. Zero disables the bound.
        std::size_t push_egress_max_bytes = 1u << 20; // 1 MiB
        // Bounds and overflow policy applied to every PUSH subscription.
        push::subscribe_options push_subscribe{};

        // The monotonic clock every server-side time read uses. It is appended
        // last on purpose: every offset above keeps its value, so only
        // `sizeof(options)` grows (304 -> 320 on x64, 256 -> 264 on x86), which
        // is still a MAJOR change under docs/abi.md section 1.
        //
        // The same clock must reach all four consumers -- the per-connection
        // rate bucket, the per-service rate bucket, the per-service circuit
        // breaker and the dispatcher's deadline check. Injecting it into only
        // some of them makes one request read two different "now" values, which
        // is a defect the test suite can detect, not a style question.
        //
        // An invalid clock is replaced by `resilience::monotonic_clock()` in the
        // library, so a default-constructed `options` keeps the previous
        // behaviour without making this struct's default construction call into
        // the library.
        resilience::clock_source clock_source{};
    };

    struct statistics {
        std::size_t active_connections = 0;
        std::size_t in_flight_calls = 0;
        std::size_t accepted_connections = 0;
        std::size_t requests_handled = 0;
        // Per-connection cap, per-service cap and per-method concurrency
        // rejections. Rate, bulkhead and breaker rejections are reported in
        // their own counters.
        std::size_t rejected_overload = 0;
        std::size_t handler_failures = 0;
        // Resilience admissions. Monotonic, never reset, snapshot from 64-bit
        // atomics.
        std::uint64_t rate_limited_connection = 0;
        std::uint64_t rate_limited_service = 0;
        std::uint64_t bulkhead_rejected = 0;
        std::uint64_t breaker_open = 0;
        std::uint64_t deadline_rejected = 0;
        std::uint64_t probe_admitted = 0;
        // Push counters. They are snapshots published under the push lock, so a
        // reader never touches the single-threaded hub.
        std::size_t push_active_connections = 0;
        std::size_t push_queued_messages = 0;
        std::size_t push_queued_bytes = 0;
        std::size_t push_egress_bytes = 0;
        std::size_t push_egress_high_water = 0;
        std::size_t push_dropped_overflow = 0;
        std::size_t push_acks = 0;
    };

    // Snapshot of the send-path record pool (see `send_payload`). `ops_allocated`
    // is the number of records ever created: it stops growing once the pool
    // reaches its high-water mark, which is what "no per-frame allocation" means.
    // `buffers_allocated` counts byte-buffer growths (a buffer keeps its capacity
    // between uses, so this also stops growing), and `ops_live` is the number of
    // records currently in flight.
    struct send_pool_counters {
        std::size_t ops_allocated = 0;
        std::size_t buffers_allocated = 0;
        std::size_t ops_live = 0;
    };

    class CRPC_API builder {
    public:
        // The backend must outlive the server, including every stop() ->
        // start() cycle: the listener configuration is retained so a later
        // start() can bind it again, and it holds a borrowed pointer.
        builder& add_listener(endpoint ep, backend_factory& backend);
        builder& add_service(u64 service_id, std::string name);
        builder& add_raw_method(u64 service_id, u64 method_id, std::string name,
                                rpc::method_handler handler, void* self,
                                rpc::method_options options = {});

        // Typed registration. The controller instance must outlive `build()`.
        template <typename Ctl, auto Method>
        builder& add_method(u64 service_id, u64 method_id, std::string name,
                            Ctl& instance, rpc::method_options options = {}) {
            registrations_.push_back(
                [service_id, method_id, name, &instance, options](
                    rpc::service_registry& registry) -> status {
                    return rpc::register_typed_method<Ctl, Method>(
                        registry, service_id, method_id, name, instance, options);
                });
            return *this;
        }

        builder& add_interceptor(server_interceptor& interceptor);
        builder& shards(std::size_t count);
        builder& handler_workers(std::size_t count);
        builder& handler_stack_bytes(std::size_t bytes);

        // Strongly typed registration overloads. They forward to the `u64`
        // versions and make a swapped service/method argument list a compile
        // error.
        builder& add_service(service_id service, std::string name) {
            return add_service(service.value, std::move(name));
        }
        builder& add_raw_method(service_id service, method_id method, std::string name,
                                rpc::method_handler handler, void* self,
                                rpc::method_options options = {}) {
            return add_raw_method(service.value, method.value, std::move(name), handler,
                                  self, options);
        }
        template <typename Ctl, auto Method>
        builder& add_method(service_id service, method_id method, std::string name,
                            Ctl& instance, rpc::method_options options = {}) {
            return add_method<Ctl, Method>(service.value, method.value,
                                           std::move(name), instance, options);
        }
        [[deprecated("use builder::max_inflight_per_connection")]]
        builder& per_connection_inflight(std::size_t count);
        builder& per_service_inflight(std::size_t count);
        // `max_inflight_per_connection` is the canonical, client/server-shared
        // spelling of the per-connection in-flight cap; `per_connection_inflight`
        // above is the deprecated server-side alias and forwards to it.
        builder& max_inflight_per_connection(std::size_t count);
        builder& drain_timeout(std::chrono::milliseconds value);
        builder& overload_retry_after(std::chrono::milliseconds value);
        builder& breaker(resilience::breaker_config config);
        builder& per_connection_rate(resilience::rate_limit_config config);
        builder& per_service_rate(resilience::rate_limit_config config);
        builder& per_service_bulkhead(resilience::bulkhead_config config);
        builder& resilience_enabled(bool enabled);
        builder& push_egress_max_bytes(std::size_t bytes);
        builder& push_subscribe_options(push::subscribe_options options);
        // Injects the clock every server-side time read uses. The source must
        // outlive the server (a `manual_clock` owns its context). An invalid
        // source keeps the default `resilience::monotonic_clock()`.
        builder& clock(resilience::clock_source source);

        [[nodiscard]] std::unique_ptr<server> build();

    private:
        options options_{};
        std::vector<listener_config> listeners_;
        std::vector<std::function<status(rpc::service_registry&)>> registrations_;
        std::vector<server_interceptor*> interceptors_;
    };

    ~server();
    server(const server&) = delete;
    server& operator=(const server&) = delete;
    server(server&&) = delete;
    server& operator=(server&&) = delete;

    // Binds all listeners, starts the shards and returns once listening.
    [[nodiscard]] status start();

    // Graceful shutdown. Idempotent. Bounded by `drain_timeout`.
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] statistics stats() const noexcept;

    // Snapshot of the send-path record pool. Cheap (one mutex lock, two atomic
    // reads); safe from any thread. The counters are monotonic until the pool is
    // destroyed, so a caller can assert they stop growing after warm-up.
    [[nodiscard]] send_pool_counters send_pool_stats() const noexcept;

    // Publishes one message to `topic` and drains every connection on the
    // shard. Never blocks on a subscriber: a slow connection stops the pump at
    // `push_egress_max_bytes` and the hub queue bounds take over.
    //
    // Returns `publish_result::ok` on success. The bare `bool` this used to
    // return could not distinguish an empty topic, a stopped server, an
    // unsupported multi-shard configuration and an internal failure; the enum
    // names each one. The conversion to `bool` is implicit, so an existing
    // `if (server.publish(...))` or `CRPC_CHECK(server.publish(...))` keeps
    // compiling and means "ok". The call copies the body; the caller's span may
    // be released when it returns.
    [[nodiscard]] publish_result publish(
        std::string_view topic, span<const std::byte> body,
        push::delivery_mode mode = push::delivery_mode::reliable) noexcept;

    struct impl;

private:
    explicit server(options opt);

    std::unique_ptr<impl> impl_;
};

} // namespace crpc

#endif // CRPC_SERVER_HPP
