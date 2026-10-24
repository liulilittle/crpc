// CRPC -- RPC response workflow: non-blocking accessors on client::call<Resp>.
//
// Task 1 of the unified response workflow. `client::call<Resp>` exposes
// `valid()`/`ready()`/`attempts()`/`degraded()`/`get()`/`try_get()`/`then()`
// already. This file pins the read-only, non-blocking diagnostic accessors
// `code()`/`origin()`/`error()` that the `response<Resp>` wrapper forwards:
// an invalid handle, a completed success, a completed failure, and a cancel.
#include <atomic>
#include <chrono>
#include <memory>
#include <ostream>
#include <string>
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

// A start failure (the result holds an error, not a handle) is invalid but
// still exposes the error it was created with; cancel() is a no-op.
CRPC_TEST(response_wrapper_start_error) {
    crpc::rpc::rpc_result<crpc::client::call<int>> failed(
        crpc::rpc_error(crpc::rpc_code::unavailable, "no endpoint"));
    crpc::response<int> r(std::move(failed));

    CRPC_CHECK_FALSE(r.valid());
    CRPC_CHECK_FALSE(r.ready());
    CRPC_CHECK_EQ(0u, r.attempts());
    CRPC_CHECK_FALSE(r.degraded());
    CRPC_CHECK_EQ(crpc::rpc_code::unavailable, r.code());
    CRPC_CHECK(r.origin() == crpc::client_detail::failure_origin::remote);
    CRPC_CHECK_EQ(crpc::rpc_code::unavailable, r.error().code);
    CRPC_CHECK(r.error().message == "no endpoint");

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
            // `code()` reports the caller-visible success; `error()` forwards
            // the call's terminal snapshot unchanged, which still carries the
            // refusal that triggered the fallback.
            CRPC_CHECK_EQ(crpc::rpc_code::ok, r.code());
            CRPC_CHECK_EQ(crpc::rpc_code::unavailable, r.error().code);
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

CRPC_TEST_MAIN()
