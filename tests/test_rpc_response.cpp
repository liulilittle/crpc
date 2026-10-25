// CRPC -- RPC response workflow: non-blocking accessors on client::call<Resp>.
//
// Task 1 of the unified response workflow. `client::call<Resp>` exposes
// `valid()`/`ready()`/`attempts()`/`degraded()`/`get()`/`try_get()`/`then()`
// already. This file pins the read-only, non-blocking diagnostic accessors
// `code()`/`origin()`/`error()` that the `response<Resp>` wrapper forwards:
// an invalid handle, a completed success, a completed failure, and a cancel.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <thread>
#include <utility>

#include <crpc/client_facade.hpp>
#include <crpc/runtime.hpp>
#include <crpc/server.hpp>

#include "test_framework.hpp"

using namespace std::chrono_literals;

// Stream adapters so CRPC_CHECK_EQ can print the enum-class values it compares.
namespace crpc {
inline std::ostream& operator<<(std::ostream& out, rpc_code code) {
    return out << to_string(code);
}
} // namespace crpc

namespace {

struct echo_service {
    int echo(const int& value) { return value + 1; }
};

crpc::status fail_body(void*, const crpc::call_envelope&, crpc::span<const std::byte>,
                       crpc::buffer&, crpc::rpc::call_context&) {
    return crpc::status{crpc::rpc_error(crpc::rpc_code::unavailable, "boom")};
}

// Fallback state for the degraded case. `refs` is intrusive: the caller owns one
// reference and the installed policy owns another; whoever drops the last frees.
struct cache_state {
    int value = 0;
    int calls = 0;
    std::atomic<int> refs{1};
};

crpc::status cached_fallback(void* self,
                             const crpc::resilience::fallback_context&,
                             crpc::buffer& out) noexcept {
    auto* cache = static_cast<cache_state*>(self);
    ++cache->calls;
    (void)crpc::rpc::call_codec<int>::encode(cache->value, out);
    return crpc::status{};
}

void* retain_cache(void* self) noexcept {
    ++static_cast<cache_state*>(self)->refs;
    return self;
}

void release_cache(void* self) noexcept {
    auto* cache = static_cast<cache_state*>(self);
    if (cache->refs.fetch_sub(1) == 1) {
        delete cache;
    }
}

// A backend whose connection accepts and never completes a write: the call
// stays in flight so it can be cancelled before get().
struct stalled_connection final : crpc::connection {
    crpc::io::source& source() noexcept override { return source_; }
    crpc::io::sink& sink() noexcept override { return sink_; }
    void bind_source(crpc::io::source*) noexcept override {}
    void resume_read() noexcept override {}
    void close() noexcept override { open.store(false, std::memory_order_release); }
    [[nodiscard]] bool is_open() const noexcept override {
        return open.load(std::memory_order_acquire);
    }
    [[nodiscard]] crpc::endpoint remote() const noexcept override {
        return crpc::endpoint::inproc("stalled");
    }

private:
    struct noop_source final : crpc::io::source {
        std::size_t on_data(crpc::span<const std::byte>) noexcept override { return 0; }
        void on_eof() noexcept override {}
        void on_error(std::error_code) noexcept override {}
    };
    struct never_sink final : crpc::io::sink {
        void push(crpc::span<const std::byte>, crpc::io::completion*) noexcept override {}
    };
    noop_source source_;
    never_sink sink_;
    std::atomic<bool> open{true};
};

struct stalled_backend final : crpc::backend_factory {
    std::unique_ptr<crpc::connection> connect(const crpc::endpoint&,
                                              const crpc::connect_options&) override {
        return std::make_unique<stalled_connection>();
    }
    std::unique_ptr<crpc::listener> listen(const crpc::endpoint&,
                                           const crpc::listen_options&) override {
        return nullptr;
    }
};

// A handler that blocks until the test releases it. It makes "the call is still
// in flight when `then` is armed" deterministic instead of racing a fast reply.
struct gate_state {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
};

crpc::status gate_body(void* self, const crpc::call_envelope&, crpc::span<const std::byte>,
                       crpc::buffer& out, crpc::rpc::call_context&) {
    auto* gate = static_cast<gate_state*>(self);
    {
        std::unique_lock<std::mutex> lock(gate->mutex);
        gate->entered = true;
        gate->cv.notify_all();
        gate->cv.wait(lock, [gate] { return gate->release; });
    }
    (void)crpc::rpc::call_codec<int>::encode(7, out);
    return crpc::status{};
}

} // namespace

// An invalid handle has no state: the accessors answer the sentinel
// `invalid_argument` pair instead of touching a null pointer.
CRPC_TEST(response_call_peek_accessors_invalid_handle) {
    crpc::client::call<int> handle;
    CRPC_CHECK_FALSE(handle.valid());
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument, handle.code());
    CRPC_CHECK(handle.origin() == crpc::client_detail::failure_origin::local_lifecycle);
    const crpc::rpc_error err = handle.error();
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument, err.code);
    CRPC_CHECK(err.message.find("invalid call handle") != std::string::npos);
}

// A call that completed against a live inproc server: the accessors read the
// terminal code, origin and error without blocking. Both the success and the
// server-produced failure are covered.
CRPC_TEST(response_call_peek_accessors_completed) {
    crpc::inproc_backend backend;
    echo_service service;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("peek-accessors"), backend);
    sb.add_service(1, "svc");
    sb.add_method<echo_service, &echo_service::echo>(1, 1, "echo", service);
    sb.add_raw_method(1, 2, "fail", &fail_body, nullptr);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("peek-accessors"), backend);
    cb.pool_size(1);
    cb.default_deadline(2s);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        srv->stop();
        shard.stop();
        shard.join();
        return;
    }

    // Success: the terminal code is ok, the origin is remote, and the error
    // carries ok (empty message).
    {
        auto stub = cli->service<int, int>(1, 1);
        auto started = stub.call(41);
        CRPC_CHECK(started.ok());
        if (started.ok()) {
            crpc::client::call<int> handle = std::move(started).value();
            const crpc::rpc::rpc_result<int> result = handle.get();
            CRPC_CHECK(result.ok());
            CRPC_CHECK_EQ(42, result.value());
            CRPC_CHECK_EQ(crpc::rpc_code::ok, handle.code());
            CRPC_CHECK(handle.origin() == crpc::client_detail::failure_origin::remote);
            CRPC_CHECK_EQ(crpc::rpc_code::ok, handle.error().code);
        }
    }

    // Failure: the server answered `unavailable`; the accessors expose the
    // terminal code, the origin and the error message.
    {
        auto stub = cli->service<int, int>(1, 2);
        auto started = stub.call(1);
        CRPC_CHECK(started.ok());
        if (started.ok()) {
            crpc::client::call<int> handle = std::move(started).value();
            const crpc::rpc::rpc_result<int> result = handle.get();
            CRPC_CHECK_FALSE(result.ok());
            CRPC_CHECK_EQ(crpc::rpc_code::unavailable, handle.code());
            CRPC_CHECK(handle.origin() == crpc::client_detail::failure_origin::remote);
            const crpc::rpc_error err = handle.error();
            CRPC_CHECK_EQ(crpc::rpc_code::unavailable, err.code);
            CRPC_CHECK(err.message.find("boom") != std::string::npos);
        }
    }

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}

// A fallback that answered the call is a degraded success: `get()` returns the
// fallback value, so `code()` must report `ok` and agree with `get().code()`,
// not the underlying breaker refusal that triggered the fallback.
CRPC_TEST(response_call_peek_accessors_degraded) {
    crpc::inproc_backend backend;
    echo_service service;

    // The server breaker opens after one dependency failure and refuses later
    // calls before the handler runs, marking the refusal `crpc-degradable`.
    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("peek-degraded"), backend);
    sb.add_service(1, "svc");
    sb.add_method<echo_service, &echo_service::echo>(1, 1, "echo", service);
    sb.add_raw_method(1, 2, "fail", &fail_body, nullptr);
    crpc::resilience::breaker_config server_breaker;
    server_breaker.min_requests = 1;
    server_breaker.failure_ratio_threshold = 0.0;
    server_breaker.slow_call_threshold = std::chrono::seconds(10);
    server_breaker.open_duration = std::chrono::seconds(5);
    sb.breaker(server_breaker);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("peek-degraded"), backend);
    cb.pool_size(1);
    cb.default_deadline(std::chrono::seconds(2));
    cb.plain_thread_timeout(std::chrono::seconds(2));
    // A remote refusal (sent == true) may only be degraded for an idempotent
    // call.
    cb.default_idempotent(true);
    crpc::resilience::retry_policy retry;
    retry.max_attempts = 1;
    cb.retry(retry);
    auto cli = cb.build();
    if (!cli) {
        srv->stop();
        return;
    }

    auto* cache = new cache_state{41, 0};
    CRPC_CHECK(cli->set_fallback(&cached_fallback, cache,
                                 std::chrono::nanoseconds{0}, &retain_cache,
                                 &release_cache));

    // Open the server breaker with a plain handler failure.
    {
        auto fail = cli->service<int, int>(1, 2);
        auto started = fail.call(1);
        if (started.ok()) {
            auto handle = std::move(started).value();
            (void)handle.get();
        }
    }

    // An idempotent echo now meets the open breaker and is answered by the
    // fallback value.
    {
        auto echo = cli->service<int, int>(1, 1);
        auto started = echo.call(2);
        CRPC_CHECK(started.ok());
        if (started.ok()) {
            crpc::client::call<int> handle = std::move(started).value();
            const crpc::rpc::rpc_result<int> result = handle.get();
            CRPC_CHECK(result.ok());
            if (result.ok()) {
                CRPC_CHECK_EQ(41, result.value());
            }
            CRPC_CHECK(handle.degraded());
            CRPC_CHECK_EQ(crpc::rpc_code::ok, handle.code());
            CRPC_CHECK_EQ(crpc::rpc_code::ok, result.code());
        }
    }

    release_cache(cache);
    cli->close();
    srv->stop();
}

// A cancelled call reports `canceled` from the accessors without a wait.
CRPC_TEST(response_call_peek_accessors_canceled) {
    stalled_backend backend;
    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("stalled"), backend);
    cb.pool_size(1);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        shard.stop();
        shard.join();
        return;
    }

    auto stub = cli->service<int, int>(1, 1);
    auto started = stub.call(1);
    CRPC_CHECK(started.ok());
    if (started.ok()) {
        crpc::client::call<int> handle = std::move(started).value();
        handle.cancel();
        const crpc::rpc::rpc_result<int> result = handle.get();
        CRPC_CHECK_FALSE(result.ok());
        CRPC_CHECK_EQ(crpc::rpc_code::canceled, handle.code());
        CRPC_CHECK(handle.origin() == crpc::client_detail::failure_origin::cancel);
        CRPC_CHECK_EQ(crpc::rpc_code::canceled, handle.error().code);
    }

    cli->close();
    shard.stop();
    shard.join();
}

// ready-without-get: the low-level call completed (signal fired the callback)
// but no consumer processed the attempt, so terminal_ready() is false and
// code() is still the default ok (not a failure). ready() is true (done).
// get() then records the terminal outcome.
CRPC_TEST(response_call_peek_accessors_ready_without_get) {
    crpc::inproc_backend backend;
    echo_service service;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("peek-ready"), backend);
    sb.add_service(1, "svc");
    sb.add_method<echo_service, &echo_service::echo>(1, 1, "echo", service);
    sb.add_raw_method(1, 2, "fail", &fail_body, nullptr);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("peek-ready"), backend);
    cb.pool_size(1);
    cb.default_deadline(2s);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        srv->stop();
        shard.stop();
        shard.join();
        return;
    }

    auto stub = cli->service<int, int>(1, 2);
    auto started = stub.call(1);
    CRPC_CHECK(started.ok());
    if (started.ok()) {
        crpc::client::call<int> handle = std::move(started).value();
        std::atomic<bool> fired{false};
        handle.then([&fired](crpc::client::call<int>&) {
            fired.store(true, std::memory_order_release);
        });
        // The low-level call completed and the callback ran, but no consumer
        // called get() yet. `ready()` is true (done), but the terminal
        // snapshot is published only when a consumer processes the attempt,
        // so the accessors still hold defaults.
        CRPC_CHECK(crpc_test::wait_for(
            [&fired] { return fired.load(std::memory_order_acquire); }, 3000));
        CRPC_CHECK(handle.ready());
        CRPC_CHECK_FALSE(handle.terminal_ready());
        CRPC_CHECK_EQ(crpc::rpc_code::ok, handle.code());
        // get() publishes the terminal outcome.
        const crpc::rpc::rpc_result<int> result = handle.get();
        CRPC_CHECK_FALSE(result.ok());
        CRPC_CHECK(handle.ready());
        CRPC_CHECK(handle.terminal_ready());
        CRPC_CHECK_EQ(crpc::rpc_code::unavailable, handle.code());
        CRPC_CHECK(handle.origin() == crpc::client_detail::failure_origin::remote);
        const crpc::rpc_error peek_err = handle.error();
        CRPC_CHECK_EQ(crpc::rpc_code::unavailable, peek_err.code);
        CRPC_CHECK(peek_err.message.find("boom") != std::string::npos);
    }

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}

// A cancel after a successful get() must not change the terminal code.
CRPC_TEST(response_call_peek_accessors_cancel_after_get) {
    crpc::inproc_backend backend;
    echo_service service;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("peek-cancel-after"), backend);
    sb.add_service(1, "svc");
    sb.add_method<echo_service, &echo_service::echo>(1, 1, "echo", service);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("peek-cancel-after"), backend);
    cb.pool_size(1);
    cb.default_deadline(2s);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        srv->stop();
        shard.stop();
        shard.join();
        return;
    }

    auto stub = cli->service<int, int>(1, 1);
    auto started = stub.call(41);
    CRPC_CHECK(started.ok());
    if (started.ok()) {
        crpc::client::call<int> handle = std::move(started).value();
        const crpc::rpc::rpc_result<int> result = handle.get();
        CRPC_CHECK(result.ok());
        CRPC_CHECK_EQ(42, result.value());
        CRPC_CHECK_EQ(crpc::rpc_code::ok, handle.code());
        handle.cancel();
        CRPC_CHECK_EQ(crpc::rpc_code::ok, handle.code());
        CRPC_CHECK(handle.origin() == crpc::client_detail::failure_origin::remote);
    }

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}

// --- response<Resp> skeleton ------------------------------------------------
//
// `response<Resp>` wraps the `rpc::rpc_result<client::call<Resp>>` a start
// returns and forwards the diagnostic accessors. Task 2 builds only the
// skeleton (no get/wait/then), so these tests pin the forwarding rules:
// an error result answers the error, a valid handle answers the call, and
// `ready()` follows the terminal snapshot (`terminal_ready()`), not the
// low-level completion (`call::ready()`).

// A start failure built with the static error() factory is invalid but still
// exposes the error it was created with. A start failure is terminal, so
// `ready()` is true, `try_get()` writes the error and returns true, and
// `get()` returns the error; `cancel()` is a no-op.
CRPC_TEST(response_wrapper_start_error) {
    crpc::response<int> r = crpc::response<int>::error(
        crpc::rpc_error(crpc::rpc_code::unavailable, "no endpoint"));

    CRPC_CHECK_FALSE(r.valid());
    CRPC_CHECK(r.ready());
    CRPC_CHECK_EQ(0u, r.attempts());
    CRPC_CHECK_FALSE(r.degraded());
    CRPC_CHECK_EQ(crpc::rpc_code::unavailable, r.code());
    CRPC_CHECK(r.origin() == crpc::client_detail::failure_origin::remote);
    CRPC_CHECK_EQ(crpc::rpc_code::unavailable, r.error().code);
    CRPC_CHECK(r.error().message == "no endpoint");

    crpc::rpc::rpc_result<int> out(crpc::rpc_code::internal);
    CRPC_CHECK(r.try_get(out));
    CRPC_CHECK_FALSE(out.ok());
    CRPC_CHECK_EQ(crpc::rpc_code::unavailable, out.error().code);

    r.cancel();
    CRPC_CHECK_EQ(crpc::rpc_code::unavailable, r.code());
}

// An ok result that holds an invalid call handle: the wrapper forwards the
// handle's sentinel answers instead of claiming a usable response.
CRPC_TEST(response_wrapper_invalid_handle) {
    crpc::client::call<int> handle;
    crpc::rpc::rpc_result<crpc::client::call<int>> result(std::move(handle));
    crpc::response<int> r(std::move(result));

    CRPC_CHECK_FALSE(r.valid());
    CRPC_CHECK_FALSE(r.ready());
    CRPC_CHECK_EQ(0u, r.attempts());
    CRPC_CHECK_FALSE(r.degraded());
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument, r.code());
    CRPC_CHECK(r.origin() == crpc::client_detail::failure_origin::local_lifecycle);
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument, r.error().code);
    CRPC_CHECK(r.error().message.find("invalid call handle") != std::string::npos);
}

// A completed success forwards valid/ready/attempts/code/origin/error through
// both the concrete type and the `response_base` interface, and a cancel after
// the terminal outcome leaves the code untouched.
CRPC_TEST(response_wrapper_completed_success) {
    crpc::inproc_backend backend;
    echo_service service;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("response-ok"), backend);
    sb.add_service(1, "svc");
    sb.add_method<echo_service, &echo_service::echo>(1, 1, "echo", service);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("response-ok"), backend);
    cb.pool_size(1);
    cb.default_deadline(2s);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        srv->stop();
        shard.stop();
        shard.join();
        return;
    }

    auto stub = cli->service<int, int>(1, 1);
    auto started = stub.call(41);
    CRPC_CHECK(started.ok());
    if (started.ok()) {
        crpc::client::call<int> handle = std::move(started).value();
        const crpc::rpc::rpc_result<int> result = handle.get();
        CRPC_CHECK(result.ok());
        CRPC_CHECK_EQ(42, result.value());

        crpc::rpc::rpc_result<crpc::client::call<int>> wrapped(std::move(handle));
        crpc::response<int> r(std::move(wrapped));

        CRPC_CHECK(r.valid());
        CRPC_CHECK(r.ready());
        CRPC_CHECK_EQ(1u, r.attempts());
        CRPC_CHECK_FALSE(r.degraded());
        CRPC_CHECK_EQ(crpc::rpc_code::ok, r.code());
        CRPC_CHECK(r.origin() == crpc::client_detail::failure_origin::remote);
        CRPC_CHECK_EQ(crpc::rpc_code::ok, r.error().code);

        crpc::response_base& base = r;
        CRPC_CHECK(base.valid());
        CRPC_CHECK(base.ready());
        CRPC_CHECK_EQ(1u, base.attempts());
        CRPC_CHECK_FALSE(base.degraded());
        CRPC_CHECK_EQ(crpc::rpc_code::ok, base.code());
        CRPC_CHECK(base.origin() == crpc::client_detail::failure_origin::remote);
        CRPC_CHECK_EQ(crpc::rpc_code::ok, base.error().code);

        base.cancel();
        CRPC_CHECK_EQ(crpc::rpc_code::ok, r.code());
        CRPC_CHECK(r.origin() == crpc::client_detail::failure_origin::remote);
    }

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}

// A completed remote failure forwards the terminal code, origin and message.
CRPC_TEST(response_wrapper_completed_failure) {
    crpc::inproc_backend backend;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("response-fail"), backend);
    sb.add_service(1, "svc");
    sb.add_raw_method(1, 2, "fail", &fail_body, nullptr);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("response-fail"), backend);
    cb.pool_size(1);
    cb.default_deadline(2s);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        srv->stop();
        shard.stop();
        shard.join();
        return;
    }

    auto stub = cli->service<int, int>(1, 2);
    auto started = stub.call(1);
    CRPC_CHECK(started.ok());
    if (started.ok()) {
        crpc::client::call<int> handle = std::move(started).value();
        const crpc::rpc::rpc_result<int> result = handle.get();
        CRPC_CHECK_FALSE(result.ok());

        crpc::rpc::rpc_result<crpc::client::call<int>> wrapped(std::move(handle));
        crpc::response<int> r(std::move(wrapped));

        CRPC_CHECK(r.valid());
        CRPC_CHECK(r.ready());
        CRPC_CHECK_EQ(1u, r.attempts());
        CRPC_CHECK_FALSE(r.degraded());
        CRPC_CHECK_EQ(crpc::rpc_code::unavailable, r.code());
        CRPC_CHECK(r.origin() == crpc::client_detail::failure_origin::remote);
        CRPC_CHECK_EQ(crpc::rpc_code::unavailable, r.error().code);
        CRPC_CHECK(r.error().message.find("boom") != std::string::npos);
    }

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}

// A cancelled call forwards `canceled`/`cancel`; cancel() stays idempotent.
CRPC_TEST(response_wrapper_canceled) {
    stalled_backend backend;
    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("response-stalled"), backend);
    cb.pool_size(1);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        shard.stop();
        shard.join();
        return;
    }

    auto stub = cli->service<int, int>(1, 1);
    auto started = stub.call(1);
    CRPC_CHECK(started.ok());
    if (started.ok()) {
        crpc::client::call<int> handle = std::move(started).value();
        handle.cancel();
        const crpc::rpc::rpc_result<int> result = handle.get();
        CRPC_CHECK_FALSE(result.ok());

        crpc::rpc::rpc_result<crpc::client::call<int>> wrapped(std::move(handle));
        crpc::response<int> r(std::move(wrapped));

        CRPC_CHECK(r.valid());
        CRPC_CHECK(r.ready());
        CRPC_CHECK_EQ(1u, r.attempts());
        CRPC_CHECK_FALSE(r.degraded());
        CRPC_CHECK_EQ(crpc::rpc_code::canceled, r.code());
        CRPC_CHECK(r.origin() == crpc::client_detail::failure_origin::cancel);
        CRPC_CHECK_EQ(crpc::rpc_code::canceled, r.error().code);

        r.cancel();
        CRPC_CHECK_EQ(crpc::rpc_code::canceled, r.code());
    }

    cli->close();
    shard.stop();
    shard.join();
}

// A fallback-answered call is a degraded success: `code()` reports ok and
// `degraded()` distinguishes it.
CRPC_TEST(response_wrapper_degraded) {
    crpc::inproc_backend backend;
    echo_service service;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("response-degraded"), backend);
    sb.add_service(1, "svc");
    sb.add_method<echo_service, &echo_service::echo>(1, 1, "echo", service);
    sb.add_raw_method(1, 2, "fail", &fail_body, nullptr);
    crpc::resilience::breaker_config server_breaker;
    server_breaker.min_requests = 1;
    server_breaker.failure_ratio_threshold = 0.0;
    server_breaker.slow_call_threshold = std::chrono::seconds(10);
    server_breaker.open_duration = std::chrono::seconds(5);
    sb.breaker(server_breaker);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("response-degraded"), backend);
    cb.pool_size(1);
    cb.default_deadline(std::chrono::seconds(2));
    cb.plain_thread_timeout(std::chrono::seconds(2));
    cb.default_idempotent(true);
    crpc::resilience::retry_policy retry;
    retry.max_attempts = 1;
    cb.retry(retry);
    auto cli = cb.build();
    if (!cli) {
        srv->stop();
        return;
    }

    auto* cache = new cache_state{41, 0};
    CRPC_CHECK(cli->set_fallback(&cached_fallback, cache,
                                 std::chrono::nanoseconds{0}, &retain_cache,
                                 &release_cache));

    {
        auto fail = cli->service<int, int>(1, 2);
        auto started = fail.call(1);
        if (started.ok()) {
            auto handle = std::move(started).value();
            (void)handle.get();
        }
    }

    {
        auto echo = cli->service<int, int>(1, 1);
        auto started = echo.call(2);
        CRPC_CHECK(started.ok());
        if (started.ok()) {
            crpc::client::call<int> handle = std::move(started).value();
            const crpc::rpc::rpc_result<int> result = handle.get();
            CRPC_CHECK(result.ok());
            if (result.ok()) {
                CRPC_CHECK_EQ(41, result.value());
            }

            crpc::rpc::rpc_result<crpc::client::call<int>> wrapped(std::move(handle));
            crpc::response<int> r(std::move(wrapped));

            CRPC_CHECK(r.valid());
            CRPC_CHECK(r.ready());
            CRPC_CHECK_EQ(1u, r.attempts());
            CRPC_CHECK(r.degraded());
            // A fallback-answered call is a success: both `code()` and
            // `error()` must agree on `ok`, not expose the refusal that
            // triggered the fallback.
            CRPC_CHECK_EQ(crpc::rpc_code::ok, r.code());
            CRPC_CHECK_EQ(crpc::rpc_code::ok, r.error().code);
        }
    }

    release_cache(cache);
    cli->close();
    srv->stop();
}

// ready() follows the terminal snapshot, not the low-level completion: a call
// whose callback fired but whose outcome no consumer recorded yet is valid but
// not ready, and the accessors still hold defaults.
CRPC_TEST(response_wrapper_ready_follows_terminal) {
    crpc::inproc_backend backend;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("response-ready"), backend);
    sb.add_service(1, "svc");
    sb.add_raw_method(1, 2, "fail", &fail_body, nullptr);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("response-ready"), backend);
    cb.pool_size(1);
    cb.default_deadline(2s);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        srv->stop();
        shard.stop();
        shard.join();
        return;
    }

    auto stub = cli->service<int, int>(1, 2);
    auto started = stub.call(1);
    CRPC_CHECK(started.ok());
    if (started.ok()) {
        crpc::client::call<int> handle = std::move(started).value();
        std::atomic<bool> fired{false};
        handle.then([&fired](crpc::client::call<int>&) {
            fired.store(true, std::memory_order_release);
        });
        CRPC_CHECK(crpc_test::wait_for(
            [&fired] { return fired.load(std::memory_order_acquire); }, 3000));
        CRPC_CHECK(handle.ready());
        CRPC_CHECK_FALSE(handle.terminal_ready());

        crpc::rpc::rpc_result<crpc::client::call<int>> wrapped(std::move(handle));
        crpc::response<int> r(std::move(wrapped));

        CRPC_CHECK(r.valid());
        CRPC_CHECK_FALSE(r.ready());
        CRPC_CHECK_EQ(1u, r.attempts());
        CRPC_CHECK_FALSE(r.degraded());
        CRPC_CHECK_EQ(crpc::rpc_code::ok, r.code());
        CRPC_CHECK(r.origin() == crpc::client_detail::failure_origin::local_lifecycle);
        CRPC_CHECK_EQ(crpc::rpc_code::ok, r.error().code);
    }

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}

// --- response<Resp> waiting/consuming ---------------------------------------
//
// Task 3 adds the blocking and value accessors: `get()`/`wait()`/`park()`
// (all equivalent), the non-blocking `try_get(out)`, `value()` (throws on
// failure) and `value_or(fallback)`. A start failure answers from the wrapped
// error; a started call answers from the handle.

// get()/wait()/park() are equivalent: each returns the same decoded value for a
// completed success.
CRPC_TEST(response_wrapper_get_wait_park_success) {
    crpc::inproc_backend backend;
    echo_service service;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("response-wait-park"), backend);
    sb.add_service(1, "svc");
    sb.add_method<echo_service, &echo_service::echo>(1, 1, "echo", service);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("response-wait-park"), backend);
    cb.pool_size(1);
    cb.default_deadline(2s);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        srv->stop();
        shard.stop();
        shard.join();
        return;
    }

    {
        auto stub = cli->service<int, int>(1, 1);
        auto started = stub.call(41);
        CRPC_CHECK(started.ok());
        if (started.ok()) {
            crpc::rpc::rpc_result<crpc::client::call<int>> wrapped(
                std::move(started).value());
            crpc::response<int> r(std::move(wrapped));
            const crpc::rpc::rpc_result<int> got = r.get();
            CRPC_CHECK(got.ok());
            if (got.ok()) {
                CRPC_CHECK_EQ(42, got.value());
            }
        }
    }
    {
        auto stub = cli->service<int, int>(1, 1);
        auto started = stub.call(41);
        CRPC_CHECK(started.ok());
        if (started.ok()) {
            crpc::rpc::rpc_result<crpc::client::call<int>> wrapped(
                std::move(started).value());
            crpc::response<int> r(std::move(wrapped));
            const crpc::rpc::rpc_result<int> got = r.wait();
            CRPC_CHECK(got.ok());
            if (got.ok()) {
                CRPC_CHECK_EQ(42, got.value());
            }
        }
    }
    {
        auto stub = cli->service<int, int>(1, 1);
        auto started = stub.call(41);
        CRPC_CHECK(started.ok());
        if (started.ok()) {
            crpc::rpc::rpc_result<crpc::client::call<int>> wrapped(
                std::move(started).value());
            crpc::response<int> r(std::move(wrapped));
            const crpc::rpc::rpc_result<int> got = r.park();
            CRPC_CHECK(got.ok());
            if (got.ok()) {
                CRPC_CHECK_EQ(42, got.value());
            }
        }
    }

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}

// get()/wait()/park() on a start failure answer the wrapped error.
CRPC_TEST(response_wrapper_get_wait_park_start_error) {
    crpc::response<int> r = crpc::response<int>::error(
        crpc::rpc_error(crpc::rpc_code::unavailable, "no endpoint"));

    const crpc::rpc::rpc_result<int> got = r.get();
    CRPC_CHECK_FALSE(got.ok());
    CRPC_CHECK_EQ(crpc::rpc_code::unavailable, got.error().code);
    CRPC_CHECK(got.error().message == "no endpoint");

    const crpc::rpc::rpc_result<int> waited = r.wait();
    CRPC_CHECK_FALSE(waited.ok());
    CRPC_CHECK_EQ(crpc::rpc_code::unavailable, waited.error().code);

    const crpc::rpc::rpc_result<int> parked = r.park();
    CRPC_CHECK_FALSE(parked.ok());
    CRPC_CHECK_EQ(crpc::rpc_code::unavailable, parked.error().code);
}

// try_get(out) is non-blocking: an in-flight call that has not completed
// answers false and leaves `out` untouched.
CRPC_TEST(response_wrapper_try_get_pending) {
    stalled_backend backend;
    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("response-try-pending"), backend);
    cb.pool_size(1);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        shard.stop();
        shard.join();
        return;
    }

    auto stub = cli->service<int, int>(1, 1);
    auto started = stub.call(1);
    CRPC_CHECK(started.ok());
    if (started.ok()) {
        crpc::rpc::rpc_result<crpc::client::call<int>> wrapped(
            std::move(started).value());
        crpc::response<int> r(std::move(wrapped));

        crpc::rpc::rpc_result<int> out(crpc::rpc_code::internal);
        CRPC_CHECK_FALSE(r.try_get(out));
        CRPC_CHECK_EQ(crpc::rpc_code::internal, out.error().code);

        r.cancel();
        const crpc::rpc::rpc_result<int> got = r.get();
        CRPC_CHECK_FALSE(got.ok());
        CRPC_CHECK_EQ(crpc::rpc_code::canceled, got.error().code);
    }

    cli->close();
    shard.stop();
    shard.join();
}

// try_get(out) on a start failure writes the wrapped error and returns true.
CRPC_TEST(response_wrapper_try_get_error) {
    crpc::response<int> r = crpc::response<int>::error(
        crpc::rpc_error(crpc::rpc_code::unavailable, "no endpoint"));

    crpc::rpc::rpc_result<int> out(crpc::rpc_code::internal);
    CRPC_CHECK(r.try_get(out));
    CRPC_CHECK_FALSE(out.ok());
    CRPC_CHECK_EQ(crpc::rpc_code::unavailable, out.error().code);
    CRPC_CHECK(out.error().message == "no endpoint");
}

// value() returns the decoded value on success.
CRPC_TEST(response_wrapper_value_success) {
    crpc::inproc_backend backend;
    echo_service service;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("response-value"), backend);
    sb.add_service(1, "svc");
    sb.add_method<echo_service, &echo_service::echo>(1, 1, "echo", service);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("response-value"), backend);
    cb.pool_size(1);
    cb.default_deadline(2s);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        srv->stop();
        shard.stop();
        shard.join();
        return;
    }

    auto stub = cli->service<int, int>(1, 1);
    auto started = stub.call(41);
    CRPC_CHECK(started.ok());
    if (started.ok()) {
        crpc::rpc::rpc_result<crpc::client::call<int>> wrapped(
            std::move(started).value());
        crpc::response<int> r(std::move(wrapped));
        CRPC_CHECK_EQ(42, r.value());
    }

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}

// value() throws the terminal rpc_error on failure.
CRPC_TEST(response_wrapper_value_throws) {
    crpc::response<int> r = crpc::response<int>::error(
        crpc::rpc_error(crpc::rpc_code::unavailable, "no endpoint"));

    bool threw = false;
    try {
        (void)r.value();
    } catch (const crpc::rpc_error& e) {
        threw = true;
        CRPC_CHECK_EQ(crpc::rpc_code::unavailable, e.code);
        CRPC_CHECK(e.message == "no endpoint");
    }
    CRPC_CHECK(threw);
}

// value_or(fallback) returns the value on success and the fallback on failure.
CRPC_TEST(response_wrapper_value_or) {
    crpc::response<int> failed = crpc::response<int>::error(
        crpc::rpc_error(crpc::rpc_code::unavailable, "no endpoint"));
    CRPC_CHECK_EQ(7, failed.value_or(7));

    crpc::inproc_backend backend;
    echo_service service;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("response-value-or"), backend);
    sb.add_service(1, "svc");
    sb.add_method<echo_service, &echo_service::echo>(1, 1, "echo", service);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("response-value-or"), backend);
    cb.pool_size(1);
    cb.default_deadline(2s);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        srv->stop();
        shard.stop();
        shard.join();
        return;
    }

    auto stub = cli->service<int, int>(1, 1);
    auto started = stub.call(41);
    CRPC_CHECK(started.ok());
    if (started.ok()) {
        crpc::rpc::rpc_result<crpc::client::call<int>> wrapped(
            std::move(started).value());
        crpc::response<int> r(std::move(wrapped));
        CRPC_CHECK_EQ(42, r.value_or(7));
    }

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}

// --- response<Resp> then ----------------------------------------------------
//
// Task 4 adds `then(F)`: it forwards to the wrapped `client::call<Resp>::then`,
// so the callback receives the underlying call handle, runs on the thread that
// terminates the call, exactly once, and runs immediately on this thread when
// the call already terminated. `then` returns the response for chaining. A start
// failure has no call to arm, so it registers nothing.

// A completed call arms the callback after the fact: it runs synchronously on
// the calling thread, exactly once, with the underlying handle already ready.
CRPC_TEST(response_wrapper_then_completed_invokes_immediately) {
    crpc::inproc_backend backend;
    echo_service service;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("response-then-done"), backend);
    sb.add_service(1, "svc");
    sb.add_method<echo_service, &echo_service::echo>(1, 1, "echo", service);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("response-then-done"), backend);
    cb.pool_size(1);
    cb.default_deadline(2s);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        srv->stop();
        shard.stop();
        shard.join();
        return;
    }

    auto stub = cli->service<int, int>(1, 1);
    auto started = stub.call(41);
    CRPC_CHECK(started.ok());
    if (started.ok()) {
        crpc::client::call<int> handle = std::move(started).value();
        const crpc::rpc::rpc_result<int> got = handle.get();
        CRPC_CHECK(got.ok());
        if (got.ok()) {
            CRPC_CHECK_EQ(42, got.value());
        }

        crpc::rpc::rpc_result<crpc::client::call<int>> wrapped(std::move(handle));
        crpc::response<int> r(std::move(wrapped));

        int calls = 0;
        bool ok_in_callback = false;
        int value_in_callback = -1;
        const std::thread::id caller = std::this_thread::get_id();
        std::thread::id callback_thread;
        crpc::response<int>& chained = r.then(
            [&calls, &ok_in_callback, &value_in_callback, &callback_thread](
                crpc::rpc::rpc_result<int> result) {
                ++calls;
                callback_thread = std::this_thread::get_id();
                ok_in_callback = result.ok();
                if (result.ok()) {
                    value_in_callback = result.value();
                }
            });

        CRPC_CHECK(&chained == &r);
        CRPC_CHECK_EQ(1, calls);
        CRPC_CHECK(callback_thread == caller);
        CRPC_CHECK(ok_in_callback);
        CRPC_CHECK_EQ(42, value_in_callback);
    }

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}

// A call that is still in flight when `then` is armed runs the callback on the
// completion thread, exactly once, after the handle became ready.
CRPC_TEST(response_wrapper_then_runs_on_completion_thread_once) {
    crpc::inproc_backend backend;
    gate_state gate;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("response-then-async"), backend);
    sb.add_service(1, "svc");
    sb.add_raw_method(1, 1, "gated", &gate_body, &gate);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("response-then-async"), backend);
    cb.pool_size(1);
    cb.default_deadline(10s);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        srv->stop();
        shard.stop();
        shard.join();
        return;
    }

    auto stub = cli->service<int, int>(1, 1);
    auto started = stub.call(1);
    CRPC_CHECK(started.ok());
    if (started.ok()) {
        crpc::rpc::rpc_result<crpc::client::call<int>> wrapped(std::move(started).value());
        crpc::response<int> r(std::move(wrapped));

        bool entered = false;
        {
            std::unique_lock<std::mutex> lock(gate.mutex);
            entered = gate.cv.wait_for(lock, 3s, [&gate] { return gate.entered; });
        }
        CRPC_CHECK(entered);

        std::atomic<int> calls{0};
        const std::thread::id caller = std::this_thread::get_id();
        std::thread::id callback_thread;
        bool ok_in_callback = false;
        int value_in_callback = -1;
        crpc::response<int>& chained = r.then(
            [&calls, &ok_in_callback, &value_in_callback, &callback_thread](
                crpc::rpc::rpc_result<int> result) {
                ++calls;
                callback_thread = std::this_thread::get_id();
                ok_in_callback = result.ok();
                if (result.ok()) {
                    value_in_callback = result.value();
                }
            });
        CRPC_CHECK(&chained == &r);
        CRPC_CHECK_EQ(0, calls.load());

        {
            std::lock_guard<std::mutex> lock(gate.mutex);
            gate.release = true;
        }
        gate.cv.notify_all();

        CRPC_CHECK(crpc_test::wait_for(
            [&calls] { return calls.load(std::memory_order_acquire) > 0; }, 3000));
        const crpc::rpc::rpc_result<int> got = r.get();
        CRPC_CHECK(got.ok());
        if (got.ok()) {
            CRPC_CHECK_EQ(7, got.value());
        }
        // A second, late invocation would land in this window.
        std::this_thread::sleep_for(50ms);
        CRPC_CHECK_EQ(1, calls.load());
        CRPC_CHECK(callback_thread != caller);
        CRPC_CHECK(ok_in_callback);
        CRPC_CHECK_EQ(7, value_in_callback);
    }

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}

// A start failure is terminal even though no call handle exists: `then` invokes
// the callback immediately with the wrapped error and still returns the response
// for chaining.
CRPC_TEST(response_wrapper_then_start_error_invokes_immediately) {
    crpc::response<int> r = crpc::response<int>::error(
        crpc::rpc_error(crpc::rpc_code::unavailable, "no endpoint"));

    int calls = 0;
    bool ok_in_callback = true;
    crpc::rpc_code code_in_callback = crpc::rpc_code::ok;
    crpc::response<int>& chained = r.then(
        [&calls, &ok_in_callback, &code_in_callback](crpc::rpc::rpc_result<int> result) {
            ++calls;
            ok_in_callback = result.ok();
            code_in_callback = result.code();
        });

    CRPC_CHECK(&chained == &r);
    CRPC_CHECK_EQ(1, calls);
    CRPC_CHECK_FALSE(ok_in_callback);
    CRPC_CHECK_EQ(crpc::rpc_code::unavailable, code_in_callback);
    CRPC_CHECK_EQ(crpc::rpc_code::unavailable, r.code());
}

// A request type whose codec always fails to encode. The `client.request`
// entry must report that local encode error instead of starting a call.
struct unencodable_request {
    int value = 0;
};

namespace crpc::rpc {
template <>
struct call_codec<unencodable_request> {
    static status encode(const unencodable_request&, buffer&) {
        return status{rpc_error{rpc_code::encode_error, "bad request"}};
    }
    static status decode(span<const std::byte>, unencodable_request&) {
        return status{rpc_code::decode_error};
    }
};
} // namespace crpc::rpc

// --- client.request<Resp>(service, method, request) entry point --------------
//
// Task 6 adds the convenience entry that encodes the request and wraps the
// started call in a `response<Resp>`. These tests pin the success path, the
// server-produced failure path, and the local serialization failure path.

CRPC_TEST(response_request_entry_success) {
    crpc::inproc_backend backend;
    echo_service service;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("request-entry-ok"), backend);
    sb.add_service(1, "svc");
    sb.add_method<echo_service, &echo_service::echo>(1, 1, "echo", service);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("request-entry-ok"), backend);
    cb.pool_size(1);
    cb.default_deadline(2s);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        srv->stop();
        shard.stop();
        shard.join();
        return;
    }

    // Three arguments: the default `ctx` must be applied.
    crpc::response<int> r =
        cli->request<int, int>(crpc::service_id{1}, crpc::method_id{1}, 41);
    const crpc::rpc::rpc_result<int> got = r.get();
    CRPC_CHECK(got.ok());
    if (got.ok()) {
        CRPC_CHECK_EQ(42, got.value());
    }
    CRPC_CHECK(r.valid());
    CRPC_CHECK(r.ready());
    CRPC_CHECK_EQ(1u, r.attempts());
    CRPC_CHECK_FALSE(r.degraded());
    CRPC_CHECK_EQ(crpc::rpc_code::ok, r.code());
    CRPC_CHECK(r.origin() == crpc::client_detail::failure_origin::remote);

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}

// A server-produced failure is forwarded through the entry: `get()` fails and
// the accessors expose the terminal code, origin and message.
CRPC_TEST(response_request_entry_failure) {
    crpc::inproc_backend backend;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("request-entry-fail"), backend);
    sb.add_service(1, "svc");
    sb.add_raw_method(1, 2, "fail", &fail_body, nullptr);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("request-entry-fail"), backend);
    cb.pool_size(1);
    cb.default_deadline(2s);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        srv->stop();
        shard.stop();
        shard.join();
        return;
    }

    crpc::response<int> r =
        cli->request<int, int>(crpc::service_id{1}, crpc::method_id{2}, 1);
    const crpc::rpc::rpc_result<int> got = r.get();
    CRPC_CHECK_FALSE(got.ok());
    CRPC_CHECK(r.valid());
    CRPC_CHECK(r.ready());
    CRPC_CHECK_EQ(1u, r.attempts());
    CRPC_CHECK_FALSE(r.degraded());
    CRPC_CHECK_EQ(crpc::rpc_code::unavailable, r.code());
    CRPC_CHECK(r.origin() == crpc::client_detail::failure_origin::remote);
    const crpc::rpc_error err = r.error();
    CRPC_CHECK_EQ(crpc::rpc_code::unavailable, err.code);
    CRPC_CHECK(err.message.find("boom") != std::string::npos);

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}

// A local serialization failure is reported as an invalid response carrying the
// encode error, before any call is started.
CRPC_TEST(response_request_entry_encode_error) {
    stalled_backend backend;
    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("request-entry-encode"), backend);
    cb.pool_size(1);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        shard.stop();
        shard.join();
        return;
    }

    const unencodable_request req{5};
    crpc::response<int> r =
        cli->request<int, unencodable_request>(crpc::service_id{1},
                                               crpc::method_id{1}, req);
    const crpc::rpc::rpc_result<int> got = r.get();
    CRPC_CHECK_FALSE(got.ok());
    CRPC_CHECK_EQ(crpc::rpc_code::encode_error, got.error().code);
    CRPC_CHECK_FALSE(r.valid());
    CRPC_CHECK(r.ready());
    CRPC_CHECK_EQ(0u, r.attempts());
    CRPC_CHECK_FALSE(r.degraded());
    CRPC_CHECK_EQ(crpc::rpc_code::encode_error, r.code());
    const crpc::rpc_error err = r.error();
    CRPC_CHECK_EQ(crpc::rpc_code::encode_error, err.code);
    CRPC_CHECK(err.message == "bad request");

    cli->close();
    shard.stop();
    shard.join();
}

// --- response<Resp> idempotency / type erasure / lifecycle / conversion ------
//
// These pin the final-review fixes: `get()`/`try_get()` are idempotent, the
// `response_base&` view can consume a response without naming `Resp`, a response
// outlives its client, and `rpc_result<client::call<Resp>>` converts implicitly
// to `response<Resp>`.

// State handed to a type-erased `then` callback.
struct erased_cb_state {
    std::atomic<bool> fired{false};
    std::atomic<bool> ok{false};
};

void erased_then_cb(void* ctx, crpc::rpc_error error) noexcept {
    auto* state = static_cast<erased_cb_state*>(ctx);
    state->ok.store(error.ok(), std::memory_order_release);
    state->fired.store(true, std::memory_order_release);
}

// get()/value() are idempotent: a second call returns the same result and does
// not re-enter issue(), so `attempts()` does not grow and the code is not
// `shutting_down`. try_get() after get() also returns the cached result.
CRPC_TEST(response_wrapper_get_idempotent) {
    crpc::inproc_backend backend;
    echo_service service;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("response-idempotent"), backend);
    sb.add_service(1, "svc");
    sb.add_method<echo_service, &echo_service::echo>(1, 1, "echo", service);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("response-idempotent"), backend);
    cb.pool_size(1);
    cb.default_deadline(2s);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        srv->stop();
        shard.stop();
        shard.join();
        return;
    }

    crpc::response<int> r =
        cli->request<int, int>(crpc::service_id{1}, crpc::method_id{1}, 41);

    const crpc::rpc::rpc_result<int> first = r.get();
    CRPC_CHECK(first.ok());
    CRPC_CHECK_EQ(42, first.value());
    const std::uint32_t attempts = r.attempts();

    const crpc::rpc::rpc_result<int> second = r.get();
    CRPC_CHECK(second.ok());
    CRPC_CHECK_EQ(42, second.value());
    CRPC_CHECK_EQ(attempts, r.attempts());
    CRPC_CHECK_EQ(42, r.value());

    crpc::rpc::rpc_result<int> out(crpc::rpc_code::internal);
    CRPC_CHECK(r.try_get(out));
    CRPC_CHECK(out.ok());
    CRPC_CHECK_EQ(42, out.value());

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}

// A `response_base&` can consume a response without naming `Resp`: the erased
// `get()`/`try_get()` return the terminal error (empty on success) and the
// erased `then()` fires once with it.
CRPC_TEST(response_base_type_erased_consumption) {
    crpc::inproc_backend backend;
    echo_service service;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("response-base-erased"), backend);
    sb.add_service(1, "svc");
    sb.add_method<echo_service, &echo_service::echo>(1, 1, "echo", service);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("response-base-erased"), backend);
    cb.pool_size(1);
    cb.default_deadline(2s);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        srv->stop();
        shard.stop();
        shard.join();
        return;
    }

    crpc::response<int> r =
        cli->request<int, int>(crpc::service_id{1}, crpc::method_id{1}, 41);
    crpc::response_base& base = r;

    const crpc::rpc_error waited = base.get();
    CRPC_CHECK(waited.ok());

    crpc::rpc_error out(crpc::rpc_code::internal);
    CRPC_CHECK(base.try_get(out));
    CRPC_CHECK(out.ok());

    erased_cb_state cb_state;
    base.then(&erased_then_cb, &cb_state);
    CRPC_CHECK(crpc_test::wait_for(
        [&cb_state] { return cb_state.fired.load(std::memory_order_acquire); }, 3000));
    CRPC_CHECK(cb_state.ok.load(std::memory_order_acquire));

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}

// The type-erased view of a start-failure response: `ready()` is true and the
// erased `get()`/`then()` deliver the wrapped error.
CRPC_TEST(response_base_type_erased_start_error) {
    crpc::response<int> r = crpc::response<int>::error(
        crpc::rpc_error(crpc::rpc_code::unavailable, "no endpoint"));
    crpc::response_base& base = r;

    CRPC_CHECK(base.ready());
    const crpc::rpc_error waited = base.get();
    CRPC_CHECK_FALSE(waited.ok());
    CRPC_CHECK_EQ(crpc::rpc_code::unavailable, waited.code);

    crpc::rpc_error out(crpc::rpc_code::internal);
    CRPC_CHECK(base.try_get(out));
    CRPC_CHECK_FALSE(out.ok());
    CRPC_CHECK_EQ(crpc::rpc_code::unavailable, out.code);

    erased_cb_state cb_state;
    base.then(&erased_then_cb, &cb_state);
    CRPC_CHECK(cb_state.fired.load(std::memory_order_acquire));
    CRPC_CHECK_FALSE(cb_state.ok.load(std::memory_order_acquire));
}

// A response outlives the client that started it: destroying the client does
// not invalidate the response's shared state, and consuming it answers a
// defined terminal error instead of crashing.
CRPC_TEST(response_wrapper_lifecycle_client_destroyed) {
    crpc::inproc_backend backend;
    echo_service service;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("response-lifecycle"), backend);
    sb.add_service(1, "svc");
    sb.add_method<echo_service, &echo_service::echo>(1, 1, "echo", service);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("response-lifecycle"), backend);
    cb.pool_size(1);
    cb.default_deadline(2s);
    cb.on_shard(shard);

    std::optional<crpc::response<int>> r;
    {
        auto cli = cb.build();
        CRPC_CHECK(cli != nullptr);
        if (!cli) {
            srv->stop();
            shard.stop();
            shard.join();
            return;
        }
        r.emplace(cli->request<int, int>(crpc::service_id{1}, crpc::method_id{1}, 41));
        cli->close();
        cli.reset();
    }

    const crpc::rpc::rpc_result<int> result = r->get();
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(crpc::rpc_code::failed_precondition, result.error().code);

    srv->stop();
    shard.stop();
    shard.join();
}

// `stub::call` still returns `rpc_result<client::call<Resp>>`; it converts
// implicitly to `response<Resp>` (spec:96).
CRPC_TEST(response_wrapper_implicit_conversion_from_call_result) {
    crpc::inproc_backend backend;
    echo_service service;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("response-conversion"), backend);
    sb.add_service(1, "svc");
    sb.add_method<echo_service, &echo_service::echo>(1, 1, "echo", service);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("response-conversion"), backend);
    cb.pool_size(1);
    cb.default_deadline(2s);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        srv->stop();
        shard.stop();
        shard.join();
        return;
    }

    auto stub = cli->service<int, int>(1, 1);
    crpc::response<int> r = stub.call(41);
    const crpc::rpc::rpc_result<int> got = r.get();
    CRPC_CHECK(got.ok());
    CRPC_CHECK_EQ(42, got.value());

    // `start_encoded` has the same conversion path.
    crpc::buffer encoded;
    CRPC_CHECK(crpc::rpc::call_codec<int>::encode(41, encoded).ok());
    crpc::response<int> r2 = cli->start_encoded<int>(1, 1, encoded.readable());
    const crpc::rpc::rpc_result<int> got2 = r2.get();
    CRPC_CHECK(got2.ok());
    CRPC_CHECK_EQ(42, got2.value());

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}

CRPC_TEST_MAIN()
