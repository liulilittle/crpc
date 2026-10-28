// CRPC -- response<Resp>::get() inside a corocc stack coroutine.
//
// Task 7 of the unified response workflow. `response<Resp>::get()` forwards to
// `client::call<Resp>::get()`, which parks the calling coroutine when it runs on
// a shard driven in corocc LOOP mode (`runtime::wait_ready` -> `runtime::park`).
// These tests pin that behaviour: the call suspends (the shard keeps running
// other work while the coroutine is parked) and resumes with the decoded value
// once the server answers. The inproc server/client setup mirrors
// tests/test_rpc_response.cpp.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <thread>
#include <utility>

#include <crpc/client_facade.hpp>
#include <crpc/runtime.hpp>
#include <crpc/server.hpp>

#include "test_framework.hpp"

using namespace std::chrono_literals;

namespace {

struct echo_service {
    int echo(const int& value) { return value + 1; }
};

crpc::status fail_body(void*, const crpc::call_envelope&, crpc::span<const std::byte>,
                       crpc::buffer&, crpc::rpc::call_context&) {
    return crpc::status{crpc::rpc_error(crpc::rpc_code::unavailable, "boom")};
}

// A handler that blocks until the test releases it. It makes "the call is still
// in flight when get() is called" deterministic instead of racing a fast reply.
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

// True when the build has the corocc-backed runtime. Without it the shard is
// the thread-per-shard fallback: there is no coroutine to park, so the
// coroutine-loop assertions cannot hold.
bool coroutine_loop_available() {
#if defined(CRPC_HAVE_COROCC)
    return true;
#else
    return false;
#endif
}

constexpr const char* kNoCoroutineLoop =
    "requires CRPC_HAVE_COROCC (nofiber runtime cannot park a coroutine)";

// Phases recorded by the coroutine so the test can observe that get() suspended
// before the server answered and resumed after.
enum class phase : int { idle, before_get, after_get };

struct coro_job {
    crpc::client* cli = nullptr;
    std::atomic<int>* phase = nullptr;
    std::atomic<int>* value = nullptr;
    std::atomic<int>* code = nullptr;
    std::atomic<bool> done{false};
};

// Coroutine body: start a call, then call `response.get()`. The phase stores let
// the test prove the call parked between `before_get` and `after_get`.
void coro_entry(void* ctx) {
    auto* job = static_cast<coro_job*>(ctx);
    auto stub = job->cli->service<int, int>(1, 1);
    auto started = stub.call(41);
    if (!started.ok()) {
        *job->code = static_cast<int>(started.code());
        job->phase->store(static_cast<int>(phase::after_get), std::memory_order_release);
        job->done.store(true, std::memory_order_release);
        return;
    }
    crpc::rpc::rpc_result<crpc::client::call<int>> wrapped(std::move(started).value());
    crpc::response<int> r(std::move(wrapped));
    job->phase->store(static_cast<int>(phase::before_get), std::memory_order_release);
    const crpc::rpc::rpc_result<int> got = r.get();
    *job->value = got.ok() ? got.value() : -1;
    *job->code = got.ok() ? 0 : static_cast<int>(got.code());
    job->phase->store(static_cast<int>(phase::after_get), std::memory_order_release);
    job->done.store(true, std::memory_order_release);
}

void set_true(void* ctx) noexcept {
    static_cast<std::atomic<bool>*>(ctx)->store(true, std::memory_order_release);
}

} // namespace

// get() parks the calling coroutine and resumes it with the decoded value. The
// server handler holds a gate, so the call cannot complete until the test
// releases it: the coroutine must be suspended in get() at that point, and the
// shard thread must still be free to run a task posted from this thread.
CRPC_TEST(response_coro_get_parks_then_resumes) {
    if (!coroutine_loop_available()) {
        crpc_test::skip(kNoCoroutineLoop);
    }
    crpc::inproc_backend backend;
    gate_state gate;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("response-coro-gate"), backend);
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
    CRPC_CHECK(shard.loop_mode());

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("response-coro-gate"), backend);
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

    std::atomic<int> observed{static_cast<int>(phase::idle)};
    std::atomic<int> value{-1};
    std::atomic<int> code{-1};
    auto* job = new coro_job{cli.get(), &observed, &value, &code, {}};
    CRPC_CHECK(shard.spawn(&coro_entry, job));

    // The coroutine reached get() and is parked: the server handler holds the
    // gate, so the call cannot complete yet.
    CRPC_CHECK(crpc_test::wait_for(
        [&] {
            return observed.load(std::memory_order_acquire) ==
                   static_cast<int>(phase::before_get);
        },
        3000));
    CRPC_CHECK_EQ(static_cast<int>(phase::before_get),
                  observed.load(std::memory_order_acquire));

    // While the coroutine is parked the shard thread is free: a task posted
    // from this thread runs to completion. A shard blocked inside get() would
    // leave this false.
    std::atomic<bool> shard_ran{false};
    CRPC_CHECK(shard.post(&set_true, &shard_ran));
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return shard_ran.load(std::memory_order_acquire); }, 3000));
    // The parked coroutine still has not observed a result.
    CRPC_CHECK_EQ(static_cast<int>(phase::before_get),
                  observed.load(std::memory_order_acquire));

    // Release the server handler; the coroutine resumes with the value.
    {
        std::lock_guard<std::mutex> lock(gate.mutex);
        gate.release = true;
    }
    gate.cv.notify_all();

    CRPC_CHECK(crpc_test::wait_for(
        [&] { return job->done.load(std::memory_order_acquire); }, 3000));
    CRPC_CHECK_EQ(static_cast<int>(phase::after_get),
                  observed.load(std::memory_order_acquire));
    CRPC_CHECK_EQ(0, code.load());
    CRPC_CHECK_EQ(7, value.load());

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
    // join() above guarantees no coroutine of this shard runs past this
    // point, so the job is released only after the coroutine that owns it is
    // gone (the waits above may have timed out with the coroutine parked).
    delete job;
}

// A plain echo call inside a coroutine: get() parks until the server answers
// and returns the decoded value.
CRPC_TEST(response_coro_get_success) {
    if (!coroutine_loop_available()) {
        crpc_test::skip(kNoCoroutineLoop);
    }
    crpc::inproc_backend backend;
    echo_service service;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("response-coro-ok"), backend);
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
    CRPC_CHECK(shard.loop_mode());

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("response-coro-ok"), backend);
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

    std::atomic<int> observed{static_cast<int>(phase::idle)};
    std::atomic<int> value{-1};
    std::atomic<int> code{-1};
    auto* job = new coro_job{cli.get(), &observed, &value, &code, {}};
    CRPC_CHECK(shard.spawn(&coro_entry, job));
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return job->done.load(std::memory_order_acquire); }, 3000));
    CRPC_CHECK_EQ(static_cast<int>(phase::after_get),
                  observed.load(std::memory_order_acquire));
    CRPC_CHECK_EQ(0, code.load());
    CRPC_CHECK_EQ(42, value.load());

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
    // join() above guarantees no coroutine of this shard runs past this
    // point, so the job is released only after the coroutine that owns it is
    // gone (the waits above may have timed out with the coroutine parked).
    delete job;
}

// A server-produced failure inside a coroutine: get() parks, then returns the
// terminal error instead of a value.
CRPC_TEST(response_coro_get_failure) {
    if (!coroutine_loop_available()) {
        crpc_test::skip(kNoCoroutineLoop);
    }
    crpc::inproc_backend backend;

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc("response-coro-fail"), backend);
    sb.add_service(1, "svc");
    sb.add_raw_method(1, 1, "fail", &fail_body, nullptr);
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
    CRPC_CHECK(shard.loop_mode());

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("response-coro-fail"), backend);
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

    std::atomic<int> observed{static_cast<int>(phase::idle)};
    std::atomic<int> value{-1};
    std::atomic<int> code{-1};
    auto* job = new coro_job{cli.get(), &observed, &value, &code, {}};
    CRPC_CHECK(shard.spawn(&coro_entry, job));
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return job->done.load(std::memory_order_acquire); }, 3000));
    CRPC_CHECK_EQ(static_cast<int>(phase::after_get),
                  observed.load(std::memory_order_acquire));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable), code.load());
    CRPC_CHECK_EQ(-1, value.load());

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
    // join() above guarantees no coroutine of this shard runs past this
    // point, so the job is released only after the coroutine that owns it is
    // gone (the waits above may have timed out with the coroutine parked).
    delete job;
}

CRPC_TEST_MAIN()
