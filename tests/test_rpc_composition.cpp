// CRPC -- RPC composition: `when_all` / `when_any`.
//
// Task 5 of the unified response workflow. `when_all` waits for every response
// and returns the collected results, or the first failure. `when_any` returns
// the first response that is already terminal, or `failed_precondition` when
// none is. Both build a `response` that already holds a terminal value rather
// than a call handle, which is the "computed" mode of `response<Resp>`.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/client_facade.hpp>
#include <crpc/runtime.hpp>
#include <crpc/server.hpp>

#include "test_framework.hpp"

using namespace std::chrono_literals;

namespace crpc {
inline std::ostream& operator<<(std::ostream& out, rpc_code code) {
    return out << to_string(code);
}
} // namespace crpc

namespace {

struct echo_service {
    std::atomic<int> calls{0};
    int echo(const int& value) {
        ++calls;
        return value + 1;
    }
};

crpc::status fail_body(void*, const crpc::call_envelope&, crpc::span<const std::byte>,
                       crpc::buffer&, crpc::rpc::call_context&) {
    return crpc::status{crpc::rpc_error(crpc::rpc_code::unavailable, "boom")};
}

// A backend whose connection accepts and never completes a write: the call
// stays in flight so `when_any` sees no terminal response.
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
        return crpc::endpoint::inproc("composition-stalled");
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

// when_all over two successful calls returns both decoded values in order.
CRPC_TEST(response_when_all_all_success) {
    crpc::inproc_backend backend;
    echo_service service;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("composition-all-ok"), backend);
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
    cb.add_endpoint(crpc::endpoint::inproc("composition-all-ok"), backend);
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

    std::vector<crpc::response<int>> responses;
    {
        auto stub = cli->service<int, int>(1, 1);
        auto started = stub.call(1);
        CRPC_CHECK(started.ok());
        if (started.ok()) {
            responses.push_back(crpc::response<int>(std::move(started)));
        }
    }
    {
        auto stub = cli->service<int, int>(1, 1);
        auto started = stub.call(2);
        CRPC_CHECK(started.ok());
        if (started.ok()) {
            responses.push_back(crpc::response<int>(std::move(started)));
        }
    }

    auto all = crpc::when_all(responses);
    const crpc::rpc::rpc_result<std::vector<crpc::rpc::rpc_result<int>>> result = all.get();
    CRPC_CHECK(result.ok());
    if (result.ok()) {
        const std::vector<crpc::rpc::rpc_result<int>>& values = result.value();
        CRPC_CHECK_EQ(2u, values.size());
        if (values.size() == 2) {
            CRPC_CHECK(values[0].ok());
            CRPC_CHECK_EQ(2, values[0].value());
            CRPC_CHECK(values[1].ok());
            CRPC_CHECK_EQ(3, values[1].value());
        }
    }

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}

// when_all keeps the first failure: the whole result is an error carrying that
// failure, even though every response was drained.
CRPC_TEST(response_when_all_one_failure) {
    crpc::inproc_backend backend;
    echo_service service;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("composition-all-fail"), backend);
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
    cb.add_endpoint(crpc::endpoint::inproc("composition-all-fail"), backend);
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

    std::vector<crpc::response<int>> responses;
    {
        auto stub = cli->service<int, int>(1, 1);
        auto started = stub.call(1);
        CRPC_CHECK(started.ok());
        if (started.ok()) {
            responses.push_back(crpc::response<int>(std::move(started)));
        }
    }
    {
        auto stub = cli->service<int, int>(1, 2);
        auto started = stub.call(1);
        CRPC_CHECK(started.ok());
        if (started.ok()) {
            responses.push_back(crpc::response<int>(std::move(started)));
        }
    }

    auto all = crpc::when_all(responses);
    const crpc::rpc::rpc_result<std::vector<crpc::rpc::rpc_result<int>>> result = all.get();
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(crpc::rpc_code::unavailable, result.error().code);
    CRPC_CHECK(result.error().message.find("boom") != std::string::npos);
    // The computed response exposes the same terminal code through `code()`,
    // and a composition failure is a terminal (computed) result, so `ready()`
    // is true just like the all-success case.
    CRPC_CHECK(all.ready());
    CRPC_CHECK_EQ(crpc::rpc_code::unavailable, all.code());

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}

// when_any skips a response that is still in flight and returns the first one
// that is already terminal. The stalled call (index 0) stays pending while the
// echo call (index 1) completes, so the returned value is the echo result.
CRPC_TEST(response_when_any_first_completion) {
    crpc::inproc_backend backend;
    echo_service service;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("composition-any"), backend);
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
    cb.add_endpoint(crpc::endpoint::inproc("composition-any"), backend);
    cb.pool_size(1);
    cb.default_deadline(5s);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        srv->stop();
        shard.stop();
        shard.join();
        return;
    }

    // A second client on a stalled backend: its call never completes, so it
    // stays pending while the inproc call finishes.
    stalled_backend stalled_backend_var;
    crpc::client::builder stalled_cb;
    stalled_cb.add_endpoint(crpc::endpoint::inproc("composition-stalled"), stalled_backend_var);
    stalled_cb.pool_size(1);
    stalled_cb.on_shard(shard);
    auto stalled_cli = stalled_cb.build();
    if (!stalled_cli) {
        cli->close();
        srv->stop();
        shard.stop();
        shard.join();
        return;
    }

    std::vector<crpc::response<int>> responses;
    {
        auto stub = stalled_cli->service<int, int>(1, 1);
        auto started = stub.call(1);
        CRPC_CHECK(started.ok());
        if (started.ok()) {
            responses.push_back(crpc::response<int>(std::move(started)));
        }
    }
    // Arm a completion callback on the echo call so the test can wait for the
    // low-level completion without consuming the terminal snapshot (`ready()`
    // stays false until a consumer calls `try_get`/`get`).
    std::atomic<bool> echo_done{false};
    {
        auto stub = cli->service<int, int>(1, 1);
        auto started = stub.call(1);
        CRPC_CHECK(started.ok());
        if (started.ok()) {
            crpc::response<int> echo_response(std::move(started));
            [[maybe_unused]] crpc::response<int>& chained = echo_response.then(
                [&echo_done](crpc::rpc::rpc_result<int>) {
                    echo_done.store(true, std::memory_order_release);
                });
            CRPC_CHECK(&chained == &echo_response);
            responses.push_back(std::move(echo_response));
        }
    }

    CRPC_CHECK(crpc_test::wait_for(
        [&echo_done] { return echo_done.load(std::memory_order_acquire); }, 3000));
    // The stalled response has not completed, so it is not terminal.
    CRPC_CHECK_FALSE(responses[0].ready());

    auto any = crpc::when_any(responses);
    const crpc::rpc::rpc_result<crpc::rpc::rpc_result<int>> result = any.get();
    CRPC_CHECK(result.ok());
    if (result.ok()) {
        const crpc::rpc::rpc_result<int>& inner = result.value();
        CRPC_CHECK(inner.ok());
        if (inner.ok()) {
            CRPC_CHECK_EQ(2, inner.value());
        }
    }

    cli->close();
    stalled_cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}

// when_any returns the first terminal response even when it is a failure: the
// start-failure response (index 0) is terminal, so it wins over the still
// pending stalled call (index 1).
CRPC_TEST(response_when_any_first_failure) {
    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    stalled_backend stalled_backend_var;
    crpc::client::builder stalled_cb;
    stalled_cb.add_endpoint(crpc::endpoint::inproc("composition-stalled"), stalled_backend_var);
    stalled_cb.pool_size(1);
    stalled_cb.on_shard(shard);
    auto stalled_cli = stalled_cb.build();
    if (!stalled_cli) {
        shard.stop();
        shard.join();
        return;
    }

    std::vector<crpc::response<int>> responses;
    responses.push_back(crpc::response<int>::error(
        crpc::rpc_error(crpc::rpc_code::unavailable, "no endpoint")));
    {
        auto stub = stalled_cli->service<int, int>(1, 1);
        auto started = stub.call(1);
        CRPC_CHECK(started.ok());
        if (started.ok()) {
            responses.push_back(crpc::response<int>(std::move(started)));
        }
    }

    auto any = crpc::when_any(responses);
    const crpc::rpc::rpc_result<crpc::rpc::rpc_result<int>> result = any.get();
    CRPC_CHECK(result.ok());
    if (result.ok()) {
        const crpc::rpc::rpc_result<int>& inner = result.value();
        CRPC_CHECK_FALSE(inner.ok());
        CRPC_CHECK_EQ(crpc::rpc_code::unavailable, inner.error().code);
        CRPC_CHECK(inner.error().message == "no endpoint");
    }

    stalled_cli->close();
    shard.stop();
    shard.join();
}

// when_any with no terminal response answers failed_precondition.
CRPC_TEST(response_when_any_none_ready) {
    stalled_backend backend;
    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("composition-stalled"), backend);
    cb.pool_size(1);
    cb.on_shard(shard);
    auto cli = cb.build();
    if (!cli) {
        shard.stop();
        shard.join();
        return;
    }

    std::vector<crpc::response<int>> responses;
    {
        auto stub = cli->service<int, int>(1, 1);
        auto started = stub.call(1);
        CRPC_CHECK(started.ok());
        if (started.ok()) {
            responses.push_back(crpc::response<int>(std::move(started)));
        }
    }
    {
        auto stub = cli->service<int, int>(1, 1);
        auto started = stub.call(2);
        CRPC_CHECK(started.ok());
        if (started.ok()) {
            responses.push_back(crpc::response<int>(std::move(started)));
        }
    }

    auto any = crpc::when_any(responses);
    const crpc::rpc::rpc_result<crpc::rpc::rpc_result<int>> result = any.get();
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(crpc::rpc_code::failed_precondition, result.error().code);
    // The "none ready" result is terminal by contract, so `ready()` is true and
    // its code is observable without a wait.
    CRPC_CHECK(any.ready());
    CRPC_CHECK_EQ(crpc::rpc_code::failed_precondition, any.code());

    cli->close();
    shard.stop();
    shard.join();
}

CRPC_TEST_MAIN()
