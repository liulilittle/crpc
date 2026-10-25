// CRPC -- response<Resp> co_await support (C++20 builds only).
//
// Task 8 of the unified response workflow. Under `__cplusplus >= 202002L` and
// `<coroutine>` available, `response<Resp>` provides `operator co_await()`. The
// awaiter's `await_ready()` returns `response::ready()`, so an already-terminal
// response (a composition value, or a call that completed before the await) does
// not suspend; an in-flight call suspends the coroutine and is resumed by the
// call's `then` callback on the thread that terminates it. `await_resume()`
// returns `response::get()`.
//
// This file is compiled in both build modes. In a C++17 build `co_await` is not
// a language feature, so the real cases are guarded out and the translation unit
// reduces to a trivial pass-through main; the operator itself is likewise
// absent from the header in that mode.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/client_facade.hpp>
#include <crpc/runtime.hpp>
#include <crpc/server.hpp>

#include "test_framework.hpp"

#if __cplusplus >= 202002L && __has_include(<coroutine>)

#include <coroutine>
#include <exception>

using namespace std::chrono_literals;

namespace {

// Minimal coroutine handle type. `initial_suspend`/`final_suspend` are
// `suspend_always`, so the test drives the body explicitly with `resume()` and
// observes completion through `done()` instead of a callback.
struct task {
    struct promise_type {
        task get_return_object() noexcept {
            return task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };

    std::coroutine_handle<promise_type> handle;

    explicit task(std::coroutine_handle<promise_type> h) noexcept : handle(h) {}
    task(task&& other) noexcept : handle(other.handle) { other.handle = nullptr; }
    task(const task&) = delete;
    task& operator=(const task&) = delete;
    ~task() {
        if (handle) {
            handle.destroy();
        }
    }

    void resume() const { handle.resume(); }
    [[nodiscard]] bool done() const noexcept { return handle.done(); }
};

struct gate_state {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    bool fail = false;
};

// A handler that blocks until the test releases it. It makes "the coroutine is
// suspended in co_await" deterministic instead of racing a fast reply: the
// server handler holds the response until the test has observed that the
// coroutine is parked, then it answers.
crpc::status gate_body(void* self, const crpc::call_envelope&, crpc::span<const std::byte>,
                       crpc::buffer& out, crpc::rpc::call_context&) {
    auto* gate = static_cast<gate_state*>(self);
    {
        std::unique_lock<std::mutex> lock(gate->mutex);
        gate->entered = true;
        gate->cv.notify_all();
        gate->cv.wait(lock, [gate] { return gate->release; });
    }
    if (gate->fail) {
        return crpc::status{crpc::rpc_error(crpc::rpc_code::unavailable, "boom")};
    }
    (void)crpc::rpc::call_codec<int>::encode(7, out);
    return crpc::status{};
}

// Coroutine body: start a call through `client::request` and co_await the
// `response<int>`. The phase/value/code atomics let the test prove that the
// coroutine was suspended (value still -1 while the server holds the gate) and
// that it resumed with the decoded result.
struct coro_result {
    std::atomic<int> value{-1};
    std::atomic<int> code{-1};
};

task coro_await_request(crpc::client* cli, crpc::method_id method, coro_result* out) {
    auto r = cli->request<int>(crpc::service_id(1), method, 41);
    const crpc::rpc::rpc_result<int> got = co_await r;
    out->value.store(got.ok() ? got.value() : -1, std::memory_order_release);
    out->code.store(got.ok() ? 0 : static_cast<int>(got.code()),
                    std::memory_order_release);
}

// An already-terminal (computed) response: `when_all` over an empty set returns
// a composition value that is ready by construction, so the awaiter must not
// suspend and must hand the decoded result straight to `await_resume()`.
task coro_await_ready(coro_result* out) {
    std::vector<crpc::response<int>> empty;
    auto r = crpc::when_all<int>(empty);
    const crpc::rpc::rpc_result<std::vector<crpc::rpc::rpc_result<int>>> got =
        co_await r;
    out->value.store(got.ok() ? static_cast<int>(got.value().size()) : -1,
                     std::memory_order_release);
    out->code.store(got.ok() ? 0 : static_cast<int>(got.code()),
                    std::memory_order_release);
}

// A start-failure response: there is no call to wait for, so the awaiter must
// report ready and `await_resume()` must return the wrapped error. With a
// `ready()`-only `await_ready()` the coroutine would suspend and never resume.
task coro_await_start_failure(coro_result* out) {
    auto r = crpc::response<int>::error(
        crpc::rpc_error(crpc::rpc_code::unavailable, "no endpoint"));
    const crpc::rpc::rpc_result<int> got = co_await r;
    out->value.store(got.ok() ? got.value() : -1, std::memory_order_release);
    out->code.store(got.ok() ? 0 : static_cast<int>(got.code()),
                    std::memory_order_release);
}

} // namespace

// co_await suspends an in-flight call and resumes the coroutine with the
// decoded value. The server handler holds a gate, so the coroutine must be
// suspended while the gate is held and must resume only after it is released.
CRPC_TEST(response_cpp20_co_await_suspends_then_resumes) {
    crpc::inproc_backend backend;
    gate_state gate;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("cpp20-co-await-gate"), backend);
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
    cb.add_endpoint(crpc::endpoint::inproc("cpp20-co-await-gate"), backend);
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

    coro_result out;
    task t = coro_await_request(cli.get(), crpc::method_id(1), &out);
    t.resume();

    // The coroutine reached co_await and suspended: the server handler has
    // entered the gate, so the call is in flight.
    CRPC_CHECK(crpc_test::wait_for(
        [&] {
            std::lock_guard<std::mutex> lock(gate.mutex);
            return gate.entered;
        },
        3000));
    // While the coroutine is suspended the result has not been observed.
    CRPC_CHECK_EQ(-1, out.value.load(std::memory_order_acquire));

    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.release = true;
    }
    gate.cv.notify_all();

    CRPC_CHECK(crpc_test::wait_for([&] { return t.done(); }, 3000));
    CRPC_CHECK_EQ(0, out.code.load(std::memory_order_acquire));
    CRPC_CHECK_EQ(7, out.value.load(std::memory_order_acquire));

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}

// A server-produced failure resumes the coroutine with the terminal error
// instead of a value.
CRPC_TEST(response_cpp20_co_await_failure) {
    crpc::inproc_backend backend;
    gate_state gate;
    gate.fail = true;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("cpp20-co-await-fail"), backend);
    sb.add_service(1, "svc");
    sb.add_raw_method(1, 1, "gated-fail", &gate_body, &gate);
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
    cb.add_endpoint(crpc::endpoint::inproc("cpp20-co-await-fail"), backend);
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

    coro_result out;
    task t = coro_await_request(cli.get(), crpc::method_id(1), &out);
    t.resume();
    CRPC_CHECK(crpc_test::wait_for(
        [&] {
            std::lock_guard<std::mutex> lock(gate.mutex);
            return gate.entered;
        },
        3000));

    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.release = true;
    }
    gate.cv.notify_all();

    CRPC_CHECK(crpc_test::wait_for([&] { return t.done(); }, 3000));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable),
                  out.code.load(std::memory_order_acquire));
    CRPC_CHECK_EQ(-1, out.value.load(std::memory_order_acquire));

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}

// An already-terminal composition response does not suspend: `await_ready()`
// reports ready and `await_resume()` returns the decoded value directly.
CRPC_TEST(response_cpp20_co_await_ready_no_suspend) {
    coro_result out;
    task t = coro_await_ready(&out);
    // The coroutine must run to completion on the first resume, with no
    // external release needed.
    t.resume();
    CRPC_CHECK(t.done());
    CRPC_CHECK_EQ(0, out.code.load(std::memory_order_acquire));
    CRPC_CHECK_EQ(0, out.value.load(std::memory_order_acquire));
}

// A start-failure response has no call to wait for: `await_ready()` must report
// ready (its `valid()` is false) so the coroutine resumes immediately and
// `await_resume()` returns the wrapped error. A `ready()`-only `await_ready()`
// would suspend forever.
CRPC_TEST(response_cpp20_co_await_start_failure_no_suspend) {
    coro_result out;
    task t = coro_await_start_failure(&out);
    t.resume();
    CRPC_CHECK(t.done());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable),
                  out.code.load(std::memory_order_acquire));
    CRPC_CHECK_EQ(-1, out.value.load(std::memory_order_acquire));
}

CRPC_TEST_MAIN()

#else

// C++17 build: `co_await` is not a language feature, so the operator and these
// cases are not compiled. The test binary still links and passes trivially.
int main() { return 0; }

#endif
