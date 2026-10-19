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
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/frame.hpp>
#include <crpc/io.hpp>
#include <crpc/push/hub.hpp>
#include <crpc/push/wire.hpp>
#include <crpc/resilience/circuit_breaker.hpp>
#include <crpc/rpc/dispatcher.hpp>
#include <crpc/runtime.hpp>
#include <crpc/span.hpp>

namespace crpc {

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
        bool send_goaway_on_stop = true;

        // Per-connection bound on PUSH bytes staged for the transport (the
        // in-flight push plus the send queue), independent of the response
        // in-flight bound. When it is reached the push pump stops calling
        // `try_take`; messages stay in the hub's bounded per-subscription queue
        // and its overflow policy decides. Zero disables the bound.
        std::size_t push_egress_max_bytes = 1u << 20; // 1 MiB
        // Bounds and overflow policy applied to every PUSH subscription.
        push::subscribe_options push_subscribe{};
    };

    struct statistics {
        std::size_t active_connections = 0;
        std::size_t in_flight_calls = 0;
        std::size_t accepted_connections = 0;
        std::size_t requests_handled = 0;
        std::size_t rejected_overload = 0;
        std::size_t handler_failures = 0;
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

    class builder {
    public:
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
        builder& per_connection_inflight(std::size_t count);
        builder& per_service_inflight(std::size_t count);
        builder& drain_timeout(std::chrono::milliseconds value);
        builder& overload_retry_after(std::chrono::milliseconds value);
        builder& breaker(resilience::breaker_config config);
        builder& push_egress_max_bytes(std::size_t bytes);
        builder& push_subscribe_options(push::subscribe_options options);

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

    // Publishes one message to `topic` and drains every connection on the
    // shard. Never blocks on a subscriber: a slow connection stops the pump at
    // `push_egress_max_bytes` and the hub queue bounds take over. Returns false
    // when this server is not in the supported single-shard configuration or
    // the shard is not running. The call copies the body; the caller's span may
    // be released when it returns.
    [[nodiscard]] bool publish(std::string_view topic, span<const std::byte> body,
                               push::delivery_mode mode =
                                   push::delivery_mode::reliable) noexcept;

    struct impl;

private:
    explicit server(options opt);

    std::unique_ptr<impl> impl_;
};

} // namespace crpc

#endif // CRPC_SERVER_HPP
