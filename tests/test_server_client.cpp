// CRPC -- end-to-end tests for the server, client facade and shard runtime.
//
// The in-process backend is used for every behavioural test; the IOCP backend
// is exercised on Windows when it can bind a local port.
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#if defined(_MSC_VER)
#pragma comment(lib, "psapi.lib")
#endif
#elif defined(__linux__)
#include <unistd.h>
#include <fstream>
#endif

#include <crpc/client_facade.hpp>
#include <crpc/io.hpp>
#include <crpc/io/iocp_backend.hpp>
#include <crpc/resilience/circuit_breaker.hpp>
#include <crpc/resilience/retry.hpp>
#include <crpc/rpc/call.hpp>
#include <crpc/rpc/client.hpp>
#include <crpc/rpc/dispatcher.hpp>
#include <crpc/runtime.hpp>
#include <crpc/server.hpp>

using namespace std::chrono_literals;

namespace {

constexpr crpc::u64 kService = 100;
constexpr crpc::u64 kEcho = 1;
constexpr crpc::u64 kSlow = 2;
constexpr crpc::u64 kFlaky = 3;
constexpr crpc::u64 kFail = 4;

// Diagnostic override for the coroutine stack size. Unset (or <= 0) keeps the
// library default. It exists so the TSan matrix can test whether the
// fiber-switch SEGV is a stack-size limitation; it never changes a default.
std::size_t coro_stack_bytes_override() {
    long parsed = 0;
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, "CRPC_TEST_CORO_STACK_BYTES") == 0 &&
        value != nullptr) {
        parsed = std::strtol(value, nullptr, 10);
        std::free(value);
    }
#else
    const char* value = std::getenv("CRPC_TEST_CORO_STACK_BYTES");
    if (value != nullptr) {
        parsed = std::strtol(value, nullptr, 10);
    }
#endif
    return parsed > 0 ? static_cast<std::size_t>(parsed) : 0;
}

struct service_state {
    std::atomic<int> echo_calls{0};
    std::atomic<int> slow_calls{0};
    std::atomic<int> flaky_calls{0};
    std::atomic<int> fail_calls{0};
    std::atomic<int> canceled_observed{0};
    std::atomic<int> flaky_failures_remaining{1};

    int echo(const int& value) {
        echo_calls.fetch_add(1, std::memory_order_relaxed);
        return value + 1;
    }

    int slow(const int& value, crpc::rpc::call_context& ctx) {
        slow_calls.fetch_add(1, std::memory_order_relaxed);
        for (int i = 0; i < 40; ++i) {
            std::this_thread::sleep_for(5ms);
            if (ctx.has_deadline && ctx.expired(std::chrono::steady_clock::now())) {
                canceled_observed.store(1, std::memory_order_relaxed);
            }
        }
        return value + 100;
    }

    static crpc::status raw_flaky(void* self, const crpc::call_envelope&,
                                  crpc::span<const std::byte>, crpc::buffer& out,
                                  crpc::rpc::call_context&) {
        auto* state = static_cast<service_state*>(self);
        state->flaky_calls.fetch_add(1, std::memory_order_relaxed);
        if (state->flaky_failures_remaining.fetch_sub(1, std::memory_order_acq_rel) > 0) {
            return crpc::status{crpc::rpc_error(crpc::rpc_code::unavailable, "flaky")};
        }
        return crpc::rpc::call_codec<int>::encode(42, out);
    }

    static crpc::status raw_fail(void* self, const crpc::call_envelope&,
                                 crpc::span<const std::byte>, crpc::buffer&,
                                 crpc::rpc::call_context&) {
        auto* state = static_cast<service_state*>(self);
        state->fail_calls.fetch_add(1, std::memory_order_relaxed);
        return crpc::status{crpc::rpc_error(crpc::rpc_code::unavailable, "always")};
    }
};

using clock_type = std::chrono::steady_clock;

void register_services(crpc::server::builder& builder, service_state& service) {
    builder.add_service(kService, "svc");
    builder.template add_method<service_state, &service_state::echo>(
        kService, kEcho, "echo", service);
    builder.template add_method<service_state, &service_state::slow>(
        kService, kSlow, "slow", service);
    builder.add_raw_method(kService, kFlaky, "flaky", &service_state::raw_flaky,
                           &service);
    builder.add_raw_method(kService, kFail, "fail", &service_state::raw_fail,
                           &service);
}

struct fixture_config {
    std::size_t server_workers = 8;
    std::size_t per_connection = 1024;
    std::size_t per_service = 65536;
    std::chrono::milliseconds drain{2000};
    std::chrono::milliseconds overload_retry{10};
    std::chrono::nanoseconds default_deadline{1000ms};
    bool default_idempotent = false;
    std::size_t pool_size = 1;
    std::size_t max_inflight = 1024;
    std::chrono::milliseconds idle_eviction{0};
    bool configure_retry = false;
    crpc::resilience::retry_policy retry{};
    bool configure_breaker = false;
    crpc::resilience::breaker_config breaker{};
    bool configure_server_breaker = false;
    crpc::resilience::breaker_config server_breaker{};
};

struct fixture {
    crpc::inproc_backend backend;
    service_state service;
    std::unique_ptr<crpc::runtime> shard;
    std::unique_ptr<crpc::server> srv;
    std::unique_ptr<crpc::client> cli;
    std::string name = "crpc-test";

    ~fixture() {
        if (cli) {
            cli->close();
        }
        if (srv) {
            srv->stop();
        }
        if (shard) {
            shard->stop();
            shard->join();
        }
    }
};

bool start_inproc(fixture& f, const fixture_config& cfg) {
    const std::size_t stack_bytes = coro_stack_bytes_override();
    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc(f.name), f.backend);
    register_services(sb, f.service);
    sb.handler_workers(cfg.server_workers);
    sb.per_connection_inflight(cfg.per_connection);
    sb.per_service_inflight(cfg.per_service);
    sb.drain_timeout(cfg.drain);
    sb.overload_retry_after(cfg.overload_retry);
    if (cfg.configure_server_breaker) {
        sb.breaker(cfg.server_breaker);
    }
    if (stack_bytes != 0) {
        sb.handler_stack_bytes(stack_bytes);
    }
    f.srv = sb.build();
    if (!f.srv) {
        return false;
    }
    if (!f.srv->start().ok()) {
        return false;
    }

    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    if (stack_bytes != 0) {
        ro.handler_stack_bytes = stack_bytes;
    }
    f.shard = std::make_unique<crpc::runtime>(ro);
    f.shard->start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc(f.name), f.backend);
    cb.pool_size(cfg.pool_size);
    cb.max_inflight_per_connection(cfg.max_inflight);
    cb.default_deadline(cfg.default_deadline);
    cb.default_idempotent(cfg.default_idempotent);
    cb.on_shard(*f.shard);
    cb.idle_eviction(cfg.idle_eviction);
    if (cfg.configure_retry) {
        cb.retry(cfg.retry);
    }
    if (cfg.configure_breaker) {
        cb.breaker(cfg.breaker);
    }
    f.cli = cb.build();
    return f.cli != nullptr;
}

// A regression guard may not rely on a resident-set delta under AddressSanitizer
// (quarantined allocations keep the number growing even when nothing leaks) or
// ThreadSanitizer (its allocator keeps freed memory mapped).
constexpr bool memory_tracking_usable() noexcept {
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    return false;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    return false;
#else
    return true;
#endif
#else
    return true;
#endif
}

// Committed (Windows) or resident (Linux) bytes. Zero means the platform did
// not report a value, in which case the caller skips the delta assertion.
std::size_t process_committed_bytes() noexcept {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             static_cast<DWORD>(sizeof(counters))) == 0) {
        return 0;
    }
    return static_cast<std::size_t>(counters.PrivateUsage);
#elif defined(__linux__)
    std::ifstream statm("/proc/self/statm");
    unsigned long pages = 0;
    unsigned long resident = 0;
    if (!(statm >> pages >> resident)) {
        return 0;
    }
    (void)pages;
    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return 0;
    }
    return static_cast<std::size_t>(resident) *
           static_cast<std::size_t>(page_size);
#else
    return 0;
#endif
}

// A coroutine body plus a completion flag. The owning test waits for `done`
// before reading values written by the coroutine.
struct coroutine_job {
    std::function<void()> body;
    std::atomic<bool> done{false};
};

void coroutine_entry(void* ctx) {
    auto* job = static_cast<coroutine_job*>(ctx);
    job->body();
    job->done.store(true, std::memory_order_release);
}

bool run_on_shard(crpc::runtime& shard, std::function<void()> body,
                  int timeout_ms = 5000) {
    auto* job = new coroutine_job{std::move(body), {}};
    if (!shard.spawn(&coroutine_entry, job)) {
        delete job;
        return false;
    }
    const bool finished = crpc_test::wait_for(
        [job] { return job->done.load(std::memory_order_acquire); }, timeout_ms);
    delete job;
    return finished;
}

// --- runtime primitives ------------------------------------------------------

void increment_task(void* ctx) noexcept {
    static_cast<std::atomic<int>*>(ctx)->fetch_add(1, std::memory_order_relaxed);
}

void set_timer_flag(void* ctx) noexcept {
    static_cast<std::atomic<bool>*>(ctx)->store(true, std::memory_order_release);
}

// These cases assert coroutine-loop semantics (loop_mode(), park()). In a
// build without CRPC_HAVE_COROCC the runtime is the thread-per-shard fallback:
// loop_mode() is always false and park() always returns failed_precondition, so
// the assertions cannot hold. Skip explicitly rather than weakening them.
// The check reads the build capability, not loop_mode() itself, so a corocc
// build still runs (and asserts) the full original body.
bool coroutine_loop_available() {
#if defined(CRPC_HAVE_COROCC)
    return true;
#else
    return false;
#endif
}

constexpr const char* kNoCoroutineLoop =
    "requires CRPC_HAVE_COROCC (nofiber runtime has no loop_mode()/park())";

CRPC_TEST(runtime_post_submit_timer_and_stop) {
    if (!coroutine_loop_available()) {
        crpc_test::skip(kNoCoroutineLoop);
    }
    crpc::runtime_options options;
    options.start_handler_pool = true;
    options.handler_workers = 2;
    options.handler_stack_bytes = 64 * 1024;
    if (coro_stack_bytes_override() != 0) {
        options.handler_stack_bytes = coro_stack_bytes_override();
    }
    crpc::runtime shard(options);
    shard.start_thread();
    CRPC_CHECK(shard.loop_mode());

    std::atomic<int> inbox{0};
    std::atomic<int> handler{0};
    std::atomic<bool> fired{false};
    CRPC_CHECK(shard.post(&increment_task, &inbox));
    CRPC_CHECK(shard.submit(&increment_task, &handler));
    CRPC_CHECK(shard.arm_timer(&fired, shard.now_ns() + 10'000'000ull,
                               &set_timer_flag, &fired));

    CRPC_CHECK(crpc_test::wait_for(
        [&] {
            return inbox.load() == 1 && handler.load() == 1 && fired.load();
        },
        3000));
    CRPC_CHECK_EQ(1, inbox.load());
    CRPC_CHECK_EQ(1, handler.load());
    CRPC_CHECK(fired.load());

    shard.stop();
    shard.stop();
    shard.join();
    CRPC_CHECK(!shard.running());
    CRPC_CHECK(!shard.cancel_timer(&fired));
}

// Every queued task is disposed exactly once: either it runs, or the shard
// stops first and the drop hook releases its context. Zero or two would mean a
// leak or a double free, both of which happened in this area (the HTTP server
// leaked a holder per wakeup queued during stop()).
CRPC_TEST(runtime_drops_queued_tasks_exactly_once) {
    crpc::runtime_options options;
    options.start_handler_pool = true;
    options.handler_workers = 2;
    options.handler_stack_bytes = 64 * 1024;
    if (coro_stack_bytes_override() != 0) {
        options.handler_stack_bytes = coro_stack_bytes_override();
    }
    crpc::runtime shard(options);
    shard.start_thread();

    constexpr int kTasks = 2000;
    struct tally {
        std::atomic<int> ran{0};
        std::atomic<int> dropped{0};
    } counts;

    // The handler sleeps, so the shard is busy and a good share of these
    // entries is still queued when stop() lands. Both outcomes are legal; the
    // invariant is that each task takes exactly one of them.
    auto run_slow = [](void* ctx) noexcept {
        auto* t = static_cast<tally*>(ctx);
        t->ran.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    };
    auto drop_count = [](void* ctx) noexcept {
        static_cast<tally*>(ctx)->dropped.fetch_add(1, std::memory_order_relaxed);
    };

    int accepted = 0;
    for (int i = 0; i < kTasks; ++i) {
        if (shard.submit(run_slow, &counts, drop_count)) {
            ++accepted;
        }
    }
    CRPC_CHECK(accepted > 0);

    shard.stop();
    shard.join();

    // A submit rejected because the queue was full or the shard was already
    // stopping is not a queued task; only accepted ones must be disposed.
    const int disposed = counts.ran.load() + counts.dropped.load();
    CRPC_CHECK_EQ(accepted, disposed);
    CRPC_CHECK(counts.dropped.load() > 0);
}

// --- tests -------------------------------------------------------------------

CRPC_TEST(inproc_echo) {
    if (!coroutine_loop_available()) {
        crpc_test::skip(kNoCoroutineLoop);
    }
    fixture f;
    fixture_config cfg;
    CRPC_CHECK(start_inproc(f, cfg));
    CRPC_CHECK(f.shard->loop_mode());

    int value = -1;
    int code = -1;
    const bool ran = run_on_shard(*f.shard, [&] {
        auto stub = f.cli->service<int, int>(kService, kEcho);
        auto started = stub.call(41);
        if (!started.ok()) {
            code = static_cast<int>(started.code());
            return;
        }
        auto handle = std::move(started).value();
        auto result = handle.get();
        if (result.ok()) {
            value = result.value();
            code = 0;
        } else {
            code = static_cast<int>(result.code());
        }
    });
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(0, code);
    CRPC_CHECK_EQ(42, value);
}

CRPC_TEST(sequential_1000_calls) {
    fixture f;
    fixture_config cfg;
    cfg.default_deadline = 5s;
    CRPC_CHECK(start_inproc(f, cfg));

    int completed = 0;
    int failed = 0;
    const bool ran = run_on_shard(
        *f.shard,
        [&] {
            auto stub = f.cli->service<int, int>(kService, kEcho);
            for (int i = 0; i < 1000; ++i) {
                auto started = stub.call(i);
                if (!started.ok()) {
                    ++failed;
                    continue;
                }
                auto handle = std::move(started).value();
                auto result = handle.get();
                if (result.ok() && result.value() == i + 1) {
                    ++completed;
                } else {
                    ++failed;
                }
            }
        },
        20000);
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(1000, completed);
    CRPC_CHECK_EQ(0, failed);
    CRPC_CHECK_EQ(1000, f.service.echo_calls.load());
}

CRPC_TEST(concurrent_100_calls) {
    fixture f;
    fixture_config cfg;
    cfg.server_workers = 64;
    cfg.default_deadline = 5s;
    CRPC_CHECK(start_inproc(f, cfg));

    int started_count = 0;
    int completed = 0;
    int failed = 0;
    const bool ran = run_on_shard(
        *f.shard,
        [&] {
            auto stub = f.cli->service<int, int>(kService, kEcho);
            std::vector<crpc::client::call<int>> calls;
            calls.reserve(100);
            for (int i = 0; i < 100; ++i) {
                auto started = stub.call(i);
                if (!started.ok()) {
                    ++failed;
                    continue;
                }
                calls.push_back(std::move(started).value());
                ++started_count;
            }
            int index = 0;
            for (auto& handle : calls) {
                auto result = handle.get();
                if (result.ok() && result.value() == index + 1) {
                    ++completed;
                } else {
                    ++failed;
                }
                ++index;
            }
        },
        20000);
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(100, started_count);
    CRPC_CHECK_EQ(100, completed);
    CRPC_CHECK_EQ(0, failed);
}

CRPC_TEST(deadline_exceeded_and_handler_observes) {
    fixture f;
    fixture_config cfg;
    cfg.default_deadline = 1s;
    CRPC_CHECK(start_inproc(f, cfg));

    int code = -1;
    const bool ran = run_on_shard(*f.shard, [&] {
        auto stub = f.cli->service<int, int>(kService, kSlow);
        crpc::rpc::call_context ctx;
        ctx.has_deadline = true;
        ctx.deadline = clock_type::now() + 40ms;
        auto started = stub.call(1, ctx);
        if (!started.ok()) {
            code = static_cast<int>(started.code());
            return;
        }
        auto handle = std::move(started).value();
        auto result = handle.get();
        code = result.ok() ? 0 : static_cast<int>(result.code());
    });
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::deadline_exceeded), code);
    CRPC_CHECK(crpc_test::wait_for(
        [&f] { return f.service.canceled_observed.load() > 0; }, 2000));
}

CRPC_TEST(per_connection_overload) {
    fixture f;
    fixture_config cfg;
    cfg.per_connection = 1;
    cfg.default_deadline = 2s;
    CRPC_CHECK(start_inproc(f, cfg));

    int first_code = -1;
    int second_code = -1;
    int retry_after = 0;
    const bool ran = run_on_shard(
        *f.shard,
        [&] {
            auto stub = f.cli->service<int, int>(kService, kSlow);
            auto first = stub.call(7);
            auto second = stub.call(8);
            if (!second.ok()) {
                second_code = static_cast<int>(second.code());
            } else {
                auto handle = std::move(second).value();
                auto result = handle.get();
                second_code = result.ok() ? 0 : static_cast<int>(result.code());
                if (!result.ok()) {
                    retry_after = static_cast<int>(result.error().retry_after.count());
                }
            }
            if (!first.ok()) {
                first_code = static_cast<int>(first.code());
            } else {
                auto handle = std::move(first).value();
                auto result = handle.get();
                first_code = result.ok() ? 0 : static_cast<int>(result.code());
            }
        },
        10000);
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::overload), second_code);
    CRPC_CHECK(retry_after > 0);
    CRPC_CHECK_EQ(0, first_code);
    CRPC_CHECK(f.srv->stats().rejected_overload >= 1);
}

CRPC_TEST(graceful_shutdown_drains_and_rejects) {
    fixture f;
    fixture_config cfg;
    cfg.drain = 2s;
    cfg.default_deadline = 2s;
    CRPC_CHECK(start_inproc(f, cfg));

    std::atomic<bool> started{false};
    int in_flight_code = -1;
    auto* thread = new std::thread([&] {
        const bool ran = run_on_shard(
            *f.shard,
            [&] {
                auto stub = f.cli->service<int, int>(kService, kSlow);
                auto result_handle = stub.call(5);
                started.store(true, std::memory_order_release);
                if (!result_handle.ok()) {
                    in_flight_code = static_cast<int>(result_handle.code());
                    return;
                }
                auto handle = std::move(result_handle).value();
                auto result = handle.get();
                in_flight_code = result.ok() ? 0 : static_cast<int>(result.code());
            },
            10000);
        (void)ran;
    });

    CRPC_CHECK(crpc_test::wait_for(
        [&started] { return started.load(std::memory_order_acquire); }, 2000));
    // Let the shard hand the request to a handler before shutdown starts.
    std::this_thread::sleep_for(50ms);
    const auto begin = clock_type::now();
    f.srv->stop();
    const auto elapsed = clock_type::now() - begin;
    thread->join();
    delete thread;

    CRPC_CHECK(in_flight_code == 0);
    CRPC_CHECK(elapsed < 2s);
    CRPC_CHECK(!f.srv->running());

    int after_code = -1;
    const bool ran = run_on_shard(*f.shard, [&] {
        auto stub = f.cli->service<int, int>(kService, kEcho);
        auto started_call = stub.call(1);
        after_code = started_call.ok()
                         ? 0
                         : static_cast<int>(started_call.code());
    });
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::shutting_down), after_code);
}

CRPC_TEST(server_drops_queued_jobs_and_zeroes_in_flight) {
    // Shutdown must dispose of queued request jobs: the ones that never reach a
    // handler are dropped (their drop hook returns the in-flight accounting),
    // and the ones already running finish. Either way the server's in-flight
    // counter must end at zero, or drain bookkeeping stays stuck forever.
    fixture f;
    fixture_config cfg;
    cfg.server_workers = 1;      // one handler, so the rest queue up
    cfg.per_connection = 32;
    cfg.drain = 1ms;             // force the drop path instead of a clean drain
    cfg.default_deadline = 5s;
    CRPC_CHECK(start_inproc(f, cfg));

    // Three calls: the first occupies the only handler (200 ms), the other two
    // are still in the ready queue when stop() gives up waiting.
    std::vector<std::unique_ptr<crpc::client::call<int>>> held;
    const bool ran = run_on_shard(*f.shard, [&] {
        for (int i = 0; i < 3; ++i) {
            auto stub = f.cli->service<int, int>(kService, kSlow);
            auto started = stub.call(i);
            if (!started.ok()) {
                continue;
            }
            held.push_back(
                std::make_unique<crpc::client::call<int>>(std::move(started).value()));
        }
    });
    CRPC_CHECK(ran);
    CRPC_CHECK(!held.empty());

    // Let the requests reach the server and fill the handler slot.
    std::this_thread::sleep_for(50ms);
    const std::size_t before = f.srv->stats().in_flight_calls;
    CRPC_CHECK(before > 0);

    f.srv->stop();

    // The drop hook returns the accounting for jobs that never ran; the running
    // handler finishes before the shard joins. Nothing may be left behind.
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), f.srv->stats().in_flight_calls);
    held.clear();
}

CRPC_TEST(retry_idempotent_only) {
    fixture f;
    fixture_config cfg;
    cfg.default_deadline = 2s;
    cfg.configure_retry = true;
    cfg.retry.max_attempts = 2;
    cfg.retry.initial_backoff = 1ms;
    cfg.retry.max_backoff = 4ms;
    cfg.retry.retry_budget = 10.0;
    CRPC_CHECK(start_inproc(f, cfg));

    int idempotent_value = -1;
    int idempotent_attempts = 0;
    f.service.flaky_failures_remaining.store(1);
    const bool first_ran = run_on_shard(*f.shard, [&] {
        auto stub = f.cli->service<int, int>(kService, kFlaky);
        crpc::rpc::call_context ctx;
        ctx.idempotent = true;
        auto started = stub.call(0, ctx);
        if (!started.ok()) {
            return;
        }
        auto handle = std::move(started).value();
        idempotent_attempts = static_cast<int>(handle.attempts());
        auto result = handle.get();
        if (result.ok()) {
            idempotent_value = result.value();
        }
        idempotent_attempts = static_cast<int>(handle.attempts());
    });
    CRPC_CHECK(first_ran);
    CRPC_CHECK_EQ(42, idempotent_value);
    CRPC_CHECK_EQ(2, idempotent_attempts);
    CRPC_CHECK_EQ(2, f.service.flaky_calls.load());

    f.service.flaky_calls.store(0);
    f.service.flaky_failures_remaining.store(1);
    int non_idempotent_code = -1;
    int non_idempotent_attempts = 0;
    const bool second_ran = run_on_shard(*f.shard, [&] {
        auto stub = f.cli->service<int, int>(kService, kFlaky);
        crpc::rpc::call_context ctx;
        ctx.idempotent = false;
        auto started = stub.call(0, ctx);
        if (!started.ok()) {
            non_idempotent_code = static_cast<int>(started.code());
            return;
        }
        auto handle = std::move(started).value();
        auto result = handle.get();
        non_idempotent_code =
            result.ok() ? 0 : static_cast<int>(result.code());
        non_idempotent_attempts = static_cast<int>(handle.attempts());
    });
    CRPC_CHECK(second_ran);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable), non_idempotent_code);
    CRPC_CHECK_EQ(1, non_idempotent_attempts);
    CRPC_CHECK_EQ(1, f.service.flaky_calls.load());
}

CRPC_TEST(breaker_opens_after_five_failures) {
    fixture f;
    fixture_config cfg;
    cfg.default_deadline = 2s;
    cfg.configure_breaker = true;
    cfg.breaker.min_requests = 5;
    cfg.breaker.failure_ratio_threshold = 0.5;
    cfg.breaker.open_duration = 5s;
    cfg.retry.max_attempts = 1;
    CRPC_CHECK(start_inproc(f, cfg));

    const bool ran = run_on_shard(*f.shard, [&] {
        auto stub = f.cli->service<int, int>(kService, kFail);
        for (int i = 0; i < 5; ++i) {
            auto started = stub.call(i);
            if (!started.ok()) {
                continue;
            }
            auto handle = std::move(started).value();
            (void)handle.get();
        }
        auto started = stub.call(99);
        if (started.ok()) {
            auto handle = std::move(started).value();
            (void)handle.get();
        }
    });
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(5, f.service.fail_calls.load());
    CRPC_CHECK(f.cli->stats().breaker_rejections >= 1);
}

CRPC_TEST(unknown_service_ids_do_not_create_server_guards) {
    fixture f;
    fixture_config cfg;
    cfg.default_deadline = 2s;
    cfg.configure_server_breaker = true;
    cfg.server_breaker.min_requests = 1;
    cfg.server_breaker.failure_ratio_threshold = 0.0;
    cfg.server_breaker.open_duration = 5s;
    CRPC_CHECK(start_inproc(f, cfg));

    // A raw core client bypasses the facade breaker so every request reaches
    // the server, exactly like the audit probe.
    auto conn = f.backend.connect(crpc::endpoint::inproc(f.name), crpc::connect_options{});
    CRPC_CHECK(conn != nullptr);
    if (!conn) {
        return;
    }
    crpc::rpc::client_options co;
    co.slot_capacity = 8;
    co.default_timeout = std::chrono::nanoseconds{0};
    crpc::rpc::client raw(std::move(conn), co);

    crpc::buffer body;
    CRPC_CHECK(crpc::rpc::call_codec<int>::encode(0, body).ok());
    const auto call_unknown = [&raw, &body](crpc::u64 service) {
        auto started = raw.start_call<int>(service, kEcho, body.readable());
        if (!started.ok()) {
            return static_cast<int>(started.error().code);
        }
        auto handle = std::move(started).value();
        return static_cast<int>(handle.get().code());
    };

    // No per-service guard may exist for an id the frozen registry does not
    // know. A lazily created breaker would open on the first not_found (the
    // server breaker below is configured to do so) and reject the second call.
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found), call_unknown(900001));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found), call_unknown(900001));

    if (memory_tracking_usable()) {
        const crpc::u64 unknown_base = 1000000;
        const int warmup = 200;
        for (int i = 0; i < warmup; ++i) {
            CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                          call_unknown(unknown_base + static_cast<crpc::u64>(i)));
        }
        const std::size_t before = process_committed_bytes();
        const int count = 8000;
        int bad = 0;
        for (int i = 0; i < count; ++i) {
            if (call_unknown(unknown_base +
                             static_cast<crpc::u64>(warmup + i)) !=
                static_cast<int>(crpc::rpc_code::not_found)) {
                ++bad;
            }
        }
        const std::size_t after = process_committed_bytes();
        CRPC_CHECK_EQ(0, bad);
        if (before != 0 && after >= before) {
            // Without the registry pre-check this grows by roughly 2.3 KiB per
            // unknown id (about 18 MiB for the calls above).
            CRPC_CHECK(after - before < 8u * 1024u * 1024u);
        }
    }
}

CRPC_TEST(breaker_records_logical_call_not_attempts) {
    fixture f;
    fixture_config cfg;
    cfg.default_deadline = 2s;
    cfg.configure_breaker = true;
    cfg.breaker.min_requests = 2;
    cfg.breaker.failure_ratio_threshold = 0.5;
    cfg.breaker.open_duration = 5s;
    cfg.configure_retry = true;
    cfg.retry.max_attempts = 2;
    cfg.retry.initial_backoff = 1ms;
    cfg.retry.max_backoff = 4ms;
    cfg.retry.retry_budget = 10.0;
    CRPC_CHECK(start_inproc(f, cfg));

    std::vector<int> codes;
    std::vector<crpc::u32> attempts;
    const bool ran = run_on_shard(
        *f.shard,
        [&] {
            auto stub = f.cli->service<int, int>(kService, kFlaky);
            for (int i = 0; i < 3; ++i) {
                // Each logical call fails once and succeeds on retry.
                f.service.flaky_failures_remaining.store(1);
                crpc::rpc::call_context ctx;
                ctx.idempotent = true;
                auto started = stub.call(i, ctx);
                if (!started.ok()) {
                    codes.push_back(static_cast<int>(started.code()));
                    attempts.push_back(0);
                    continue;
                }
                auto handle = std::move(started).value();
                auto result = handle.get();
                codes.push_back(result.ok() ? 0 : static_cast<int>(result.code()));
                attempts.push_back(handle.attempts());
            }
        },
        10000);
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(static_cast<std::size_t>(3), codes.size());
    CRPC_CHECK_EQ(static_cast<std::size_t>(3), attempts.size());
    if (codes.size() == 3 && attempts.size() == 3) {
        for (std::size_t i = 0; i < codes.size(); ++i) {
            CRPC_CHECK_EQ(0, codes[i]);
            CRPC_CHECK_EQ(static_cast<crpc::u32>(2), attempts[i]);
        }
    }
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), f.cli->stats().breaker_rejections);
}

CRPC_TEST(breaker_ignores_application_errors) {
    fixture f;
    fixture_config cfg;
    cfg.default_deadline = 2s;
    cfg.configure_breaker = true;
    cfg.breaker.min_requests = 20;
    cfg.breaker.failure_ratio_threshold = 0.5;
    cfg.breaker.open_duration = 5s;
    CRPC_CHECK(start_inproc(f, cfg));

    int not_found = 0;
    int echo_code = -1;
    const bool ran = run_on_shard(
        *f.shard,
        [&] {
            auto missing = f.cli->service<int, int>(kService + 1000, kEcho);
            for (int i = 0; i < 20; ++i) {
                auto started = missing.call(i);
                if (!started.ok()) {
                    continue;
                }
                auto handle = std::move(started).value();
                auto result = handle.get();
                if (result.code() == crpc::rpc_code::not_found) {
                    ++not_found;
                }
            }
            auto echo = f.cli->service<int, int>(kService, kEcho);
            auto started = echo.call(7);
            if (!started.ok()) {
                echo_code = static_cast<int>(started.code());
                return;
            }
            auto handle = std::move(started).value();
            auto result = handle.get();
            echo_code = result.ok() ? 0 : static_cast<int>(result.code());
        },
        10000);
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(20, not_found);
    CRPC_CHECK_EQ(0, echo_code);
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), f.cli->stats().breaker_rejections);
}

CRPC_TEST(breaker_counts_async_try_get_results) {
    // No shard: the asynchronous path is driven entirely by try_get(), which
    // must feed the breaker just like get() does. This also runs on nofiber
    // builds where park() is unavailable.
    crpc::inproc_backend backend;
    service_state service;
    const std::string name = "crpc-breaker-async";

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc(name), backend);
    register_services(sb, service);
    sb.handler_workers(8);
    sb.drain_timeout(2000ms);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc(name), backend);
    cb.pool_size(1);
    cb.default_deadline(2s);
    cb.plain_thread_timeout(2s);
    crpc::resilience::breaker_config breaker;
    breaker.min_requests = 5;
    breaker.failure_ratio_threshold = 0.5;
    breaker.open_duration = 5s;
    cb.breaker(breaker);
    auto cli = cb.build();
    CRPC_CHECK(cli != nullptr);
    if (!cli) {
        srv->stop();
        return;
    }

    auto stub = cli->service<int, int>(kService, kFail);
    std::vector<crpc::client::call<int>> calls;
    calls.reserve(5);
    for (int i = 0; i < 5; ++i) {
        auto started = stub.call(i);
        CRPC_CHECK(started.ok());
        if (!started.ok()) {
            cli->close();
            srv->stop();
            return;
        }
        calls.push_back(std::move(started).value());
    }
    int async_failures = 0;
    for (crpc::client::call<int>& handle : calls) {
        CRPC_CHECK(crpc_test::wait_for([&handle] { return handle.ready(); }, 2000));
        crpc::rpc::rpc_result<int> out(crpc::rpc_code::internal);
        if (handle.try_get(out) && out.code() == crpc::rpc_code::unavailable) {
            ++async_failures;
        }
    }
    int sixth_code = -1;
    auto started = stub.call(99);
    if (!started.ok()) {
        sixth_code = static_cast<int>(started.code());
    } else {
        auto handle = std::move(started).value();
        auto result = handle.get();
        sixth_code = result.ok() ? 0 : static_cast<int>(result.code());
    }

    CRPC_CHECK_EQ(5, async_failures);
    // The five asynchronous terminal results fed the breaker, so it is open.
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable), sixth_code);
    CRPC_CHECK_EQ(5, service.fail_calls.load());
    CRPC_CHECK(cli->stats().breaker_rejections >= 1);

    cli->close();
    srv->stop();
}

CRPC_TEST(breaker_counts_local_timeouts) {
    crpc::inproc_backend backend;
    service_state service;
    const std::string name = "crpc-breaker-timeout";

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc(name), backend);
    register_services(sb, service);
    sb.handler_workers(8);
    sb.drain_timeout(2000ms);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc(name), backend);
    cb.pool_size(1);
    cb.default_deadline(5s);
    cb.plain_thread_timeout(5s);
    crpc::resilience::breaker_config breaker;
    breaker.min_requests = 2;
    breaker.failure_ratio_threshold = 0.5;
    breaker.open_duration = 5s;
    cb.breaker(breaker);
    auto cli = cb.build();
    CRPC_CHECK(cli != nullptr);
    if (!cli) {
        srv->stop();
        return;
    }

    auto stub = cli->service<int, int>(kService, kSlow);
    const auto timed_out_call = [&]() {
        crpc::rpc::call_context ctx;
        ctx.has_deadline = true;
        ctx.deadline = clock_type::now() + 40ms;
        auto started = stub.call(1, ctx);
        if (!started.ok()) {
            return static_cast<int>(started.code());
        }
        auto handle = std::move(started).value();
        auto result = handle.get();
        return result.ok() ? 0 : static_cast<int>(result.code());
    };

    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::deadline_exceeded),
                  timed_out_call());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::deadline_exceeded),
                  timed_out_call());

    // The two locally observed timeouts are endpoint failures, so the next
    // call must be rejected instead of waiting for another timeout.
    const int third = timed_out_call();
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable), third);
    CRPC_CHECK(cli->stats().breaker_rejections >= 1);

    cli->close();
    srv->stop();
}

CRPC_TEST(breaker_ignores_caller_cancellation) {
    fixture f;
    fixture_config cfg;
    cfg.default_deadline = 5s;
    cfg.configure_breaker = true;
    cfg.breaker.min_requests = 5;
    cfg.breaker.failure_ratio_threshold = 0.5;
    cfg.breaker.open_duration = 5s;
    CRPC_CHECK(start_inproc(f, cfg));

    int echo_code = -1;
    const bool ran = run_on_shard(
        *f.shard,
        [&] {
            auto slow = f.cli->service<int, int>(kService, kSlow);
            for (int i = 0; i < 5; ++i) {
                auto started = slow.call(i);
                if (!started.ok()) {
                    return;
                }
                auto handle = std::move(started).value();
                handle.cancel();
            }
            auto echo = f.cli->service<int, int>(kService, kEcho);
            auto started = echo.call(3);
            if (!started.ok()) {
                echo_code = static_cast<int>(started.code());
                return;
            }
            auto handle = std::move(started).value();
            auto result = handle.get();
            echo_code = result.ok() ? 0 : static_cast<int>(result.code());
        },
        10000);
    CRPC_CHECK(ran);
    // Cancellation is not evidence of endpoint failure: five cancelled calls
    // plus a successful echo leave the breaker closed.
    CRPC_CHECK_EQ(0, echo_code);
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), f.cli->stats().breaker_rejections);
}

CRPC_TEST(pool_exhaustion_does_not_stall) {
    fixture f;
    fixture_config cfg;
    cfg.pool_size = 1;
    cfg.max_inflight = 1;
    cfg.default_deadline = 2s;
    cfg.retry.max_attempts = 1;
    CRPC_CHECK(start_inproc(f, cfg));

    int first_code = -1;
    int second_code = -1;
    const auto begin = clock_type::now();
    const bool ran = run_on_shard(
        *f.shard,
        [&] {
            auto stub = f.cli->service<int, int>(kService, kSlow);
            auto first = stub.call(1);
            auto second = stub.call(2);
            second_code = second.ok() ? 0 : static_cast<int>(second.code());
            if (first.ok()) {
                auto handle = std::move(first).value();
                auto result = handle.get();
                first_code = result.ok() ? 0 : static_cast<int>(result.code());
            } else {
                first_code = static_cast<int>(first.code());
            }
        },
        10000);
    const auto elapsed = clock_type::now() - begin;
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::resource_exhausted), second_code);
    CRPC_CHECK_EQ(0, first_code);
    CRPC_CHECK(elapsed < 2s);
}

CRPC_TEST(no_leak_after_close) {
    fixture f;
    fixture_config cfg;
    cfg.pool_size = 2;
    CRPC_CHECK(start_inproc(f, cfg));

    int value = -1;
    const bool ran = run_on_shard(*f.shard, [&] {
        auto stub = f.cli->service<int, int>(kService, kEcho);
        auto started = stub.call(5);
        if (!started.ok()) {
            return;
        }
        auto handle = std::move(started).value();
        auto result = handle.get();
        if (result.ok()) {
            value = result.value();
        }
    });
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(6, value);

    f.cli->close();
    f.srv->stop();
    const crpc::client::statistics stats = f.cli->stats();
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), stats.active_connections);
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), stats.in_flight_calls);
    CRPC_CHECK(f.srv->stats().active_connections == 0);
    CRPC_CHECK(f.srv->stats().in_flight_calls == 0);
}

// --- A-1/A-2: attempting via try_get / destructor must release the slot and
// cancel the deadline timer, not leave it armed on a destroyed client. ---

CRPC_TEST(facade_try_get_releases_connection_slot) {
    if (!coroutine_loop_available()) {
        crpc_test::skip(kNoCoroutineLoop);
    }
    fixture f;
    fixture_config cfg;
    cfg.pool_size = 1;
    cfg.max_inflight = 1;
    cfg.default_deadline = 2s;
    CRPC_CHECK(start_inproc(f, cfg));

    int value = -1;
    int code = -1;
    bool got = false;
    const bool ran = run_on_shard(*f.shard, [&] {
        auto stub = f.cli->service<int, int>(kService, kEcho);
        auto started = stub.call(41);
        if (!started.ok()) {
            code = static_cast<int>(started.code());
            return;
        }
        auto handle = std::move(started).value();
        while (!handle.ready()) {
            const crpc::status p = f.shard->park();
            if (!p.ok() && p.code() != crpc::rpc_code::canceled) {
                code = static_cast<int>(p.code());
                return;
            }
        }
        crpc::rpc::rpc_result<int> out(crpc::rpc_code::internal);
        got = handle.try_get(out);
        if (got && out.ok()) {
            value = out.value();
            code = 0;
        } else if (got) {
            code = static_cast<int>(out.code());
        }
    });
    CRPC_CHECK(ran);
    CRPC_CHECK(got);
    CRPC_CHECK_EQ(0, code);
    CRPC_CHECK_EQ(42, value);
    // try_get finalized the attempt: the slot accounting must be back to zero.
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), f.cli->stats().in_flight_calls);

    int second_code = -1;
    const bool ran2 = run_on_shard(*f.shard, [&] {
        auto stub = f.cli->service<int, int>(kService, kEcho);
        auto started = stub.call(1);
        if (!started.ok()) {
            second_code = static_cast<int>(started.code());
            return;
        }
        auto handle = std::move(started).value();
        auto result = handle.get();
        second_code = result.ok() ? 0 : static_cast<int>(result.code());
    });
    CRPC_CHECK(ran2);
    CRPC_CHECK_EQ(0, second_code);
}

CRPC_TEST(facade_destroyed_handle_releases_connection_slot) {
    fixture f;
    fixture_config cfg;
    cfg.pool_size = 1;
    cfg.max_inflight = 1;
    cfg.default_deadline = 2s;
    CRPC_CHECK(start_inproc(f, cfg));

    const bool ran = run_on_shard(*f.shard, [&] {
        auto stub = f.cli->service<int, int>(kService, kEcho);
        auto started = stub.call(5);
        if (!started.ok()) {
            return;
        }
        auto handle = std::move(started).value();
        while (!handle.ready()) {
            const crpc::status p = f.shard->park();
            if (!p.ok() && p.code() != crpc::rpc_code::canceled) {
                return;
            }
        }
        // Handle destructs without get()/try_get(): ~call must abandon it.
    });
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), f.cli->stats().in_flight_calls);
}

CRPC_TEST(facade_destroy_client_with_armed_timer_no_crash) {
    if (!coroutine_loop_available()) {
        crpc_test::skip(kNoCoroutineLoop);
    }
    fixture f;
    fixture_config cfg;
    cfg.pool_size = 1;
    cfg.max_inflight = 64;
    cfg.default_deadline = 150ms;
    CRPC_CHECK(start_inproc(f, cfg));

    int value = -1;
    bool got = false;
    const bool ran = run_on_shard(*f.shard, [&] {
        auto stub = f.cli->service<int, int>(kService, kEcho);
        auto started = stub.call(7);
        if (!started.ok()) {
            return;
        }
        auto handle = std::move(started).value();
        while (!handle.ready()) {
            const crpc::status p = f.shard->park();
            if (!p.ok() && p.code() != crpc::rpc_code::canceled) {
                return;
            }
        }
        crpc::rpc::rpc_result<int> out(crpc::rpc_code::internal);
        got = handle.try_get(out);
        if (got && out.ok()) {
            value = out.value();
        }
    });
    CRPC_CHECK(ran);
    CRPC_CHECK(got);
    CRPC_CHECK_EQ(8, value);

    // Destroy the client while the deadline timer could still be armed on the
    // shard. Without the fix the timer fires against the freed client.
    //
    // NOTE: `try_get` above already finalized the attempt and cancelled the
    // timer, so this test alone would still pass if `finalize_active` were
    // removed entirely. The uncovered cases are the two tests below, which
    // never consume the call and let the handle outlive the client.
    f.cli.reset();
    std::this_thread::sleep_for(500ms);
    CRPC_CHECK(f.srv->running());
}

// --- CF-1: destroying the handle while the backend is delivering a response
// (or is about to) must not free call state a completion callback still reads.
// The response arrives on the server shard thread while the handle is
// destroyed on this shard thread, so the window is genuinely concurrent. ---

CRPC_TEST(facade_handle_destroy_races_completion) {
    fixture f;
    fixture_config cfg;
    cfg.server_workers = 16;
    cfg.default_deadline = 2s;
    CRPC_CHECK(start_inproc(f, cfg));

    int started_count = 0;
    int failed = 0;
    const bool ran = run_on_shard(
        *f.shard,
        [&] {
            auto stub = f.cli->service<int, int>(kService, kEcho);
            for (int i = 0; i < 2000; ++i) {
                auto started = stub.call(i);
                if (!started.ok()) {
                    ++failed;
                    continue;
                }
                // Drop the handle immediately: the completion may be racing on
                // the server shard thread.
                auto handle = std::move(started).value();
                ++started_count;
                (void)handle;
            }
        },
        30000);
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(2000, started_count);
    CRPC_CHECK_EQ(0, failed);
    // Give any in-flight callbacks time to finish before the client dies.
    std::this_thread::sleep_for(100ms);
}

// --- NEW-1 / R1 / R2: a handle that outlives its client; a start racing a
// concurrent close; and a blocked get() woken by close(). All three share a
// fake backend whose writes never complete, so the attempt stays in flight
// without a server and without touching the real in-process backend. ---

namespace {

struct stalled_source final : crpc::io::source {
    std::size_t on_data(crpc::span<const std::byte>) noexcept override { return 0; }
    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {}
};

struct stalled_sink final : crpc::io::sink {
    // Accept and never complete: the low-level call stays "sent", keeping the
    // deadline timer armed until it fires or is cancelled.
    void push(crpc::span<const std::byte>, crpc::io::completion*) noexcept override {}
};

struct stalled_connection final : crpc::connection {
    stalled_source source_;
    stalled_sink sink_;
    std::atomic<bool> open{true};

    crpc::io::source& source() noexcept override { return source_; }
    crpc::io::sink& sink() noexcept override { return sink_; }
    void bind_source(crpc::io::source*) noexcept override {}
    void resume_read() noexcept override {}
    void close() noexcept override { open.store(false, std::memory_order_release); }
    [[nodiscard]] bool is_open() const noexcept override {
        return open.load(std::memory_order_acquire);
    }
    [[nodiscard]] crpc::endpoint remote() const override {
        return crpc::endpoint::inproc("stalled");
    }
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

struct racing_start_job {
    crpc::client* cli = nullptr;
    std::vector<std::unique_ptr<crpc::client::call<int>>>* out = nullptr;
    std::atomic<bool> go{false};
    std::atomic<bool> done{false};
    bool started_ok = false;
};

void racing_start_entry(void* ctx) {
    auto* job = static_cast<racing_start_job*>(ctx);
    auto stub = job->cli->service<int, int>(1, 1);
    job->go.store(true, std::memory_order_release);
    auto started = stub.call(1);
    if (started.ok()) {
        job->out->emplace_back(
            std::make_unique<crpc::client::call<int>>(std::move(started).value()));
        job->started_ok = true;
    }
    job->done.store(true, std::memory_order_release);
}

} // namespace

CRPC_TEST(facade_destroy_client_handle_outlives_timer) {
    stalled_backend backend;
    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("stalled"), backend);
    cb.pool_size(1);
    // Long enough that the attempt is genuinely in flight even under a slow
    // sanitizer; the defect is independent of the exact deadline.
    cb.default_deadline(200ms);
    cb.on_shard(shard);

    auto cli = cb.build();
    CRPC_CHECK(cli != nullptr);
    if (!cli) {
        shard.stop();
        shard.join();
        return;
    }

    // The handle is moved to the test scope so it outlives the client. The
    // call is never consumed: the attempt is still active when the client dies.
    std::unique_ptr<crpc::client::call<int>> held;
    int code = -1;
    const bool ran = run_on_shard(shard, [&] {
        auto stub = cli->service<int, int>(1, 1);
        auto started = stub.call(1);
        if (!started.ok()) {
            code = static_cast<int>(started.code());
            return;
        }
        held = std::make_unique<crpc::client::call<int>>(std::move(started).value());
    });
    CRPC_CHECK(ran);
    CRPC_CHECK(held != nullptr);
    if (held == nullptr) {
        shard.stop();
        shard.join();
        return;
    }

    // Destroy the client while the deadline timer is armed. Without the
    // finalize_active fix the freed connection pool dangles under `held`.
    cli.reset();
    // Run the shard past the deadline: a surviving timer would call
    // deadline_thunk() on the freed client.
    std::this_thread::sleep_for(300ms);
    CRPC_CHECK(shard.running());

    // Now destroy the handle whose low-level call slot is still backed by the
    // retained connection (NEW-1). This must not touch a freed pool.
    held.reset();

    shard.stop();
    shard.join();
}

// A cancelled call leaves the client's registry (abandon_attempt), so close()
// never nulls its `owner`. A handle that outlives the client therefore still
// holds a pointer into freed memory. The liveness token makes that path observe
// a dead owner instead of calling into it.
//
// This is coverage of the contract, not a red-to-green regression test: in the
// cancel-then-destroy order the call is already complete, so the old code
// happened to return before dereferencing the client. The dangling call was
// real but unobservable, and the token closes it by construction, not by timing.
CRPC_TEST(facade_release_after_client_destroyed_is_safe) {
    stalled_backend backend;
    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("stalled"), backend);
    cb.pool_size(1);
    cb.default_deadline(5000ms);
    cb.on_shard(shard);
    auto cli = cb.build();
    CRPC_CHECK(cli != nullptr);
    if (!cli) {
        shard.stop();
        shard.join();
        return;
    }

    // Issue a call that never completes, then cancel it: cancel() abandons the
    // attempt, which removes it from the client's active set.
    std::unique_ptr<crpc::client::call<int>> held;
    const bool ran = run_on_shard(shard, [&] {
        auto stub = cli->service<int, int>(1, 1);
        auto started = stub.call(1);
        if (!started.ok()) {
            return;
        }
        held = std::make_unique<crpc::client::call<int>>(std::move(started).value());
        held->cancel();
    });
    CRPC_CHECK(ran);
    CRPC_CHECK(held != nullptr);
    if (held == nullptr) {
        cli.reset();
        shard.stop();
        shard.join();
        return;
    }

    // The client dies first; the handle is released afterwards.
    cli.reset();
    held.reset();

    shard.stop();
    shard.join();
}

CRPC_TEST(facade_racing_start_and_close_leaves_no_timer) {
    stalled_backend backend;
    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    // A wide least-inflight scan makes the admission window (closed-check ->
    // registration) large enough that the concurrent close below regularly
    // lands inside it; under ASan this reproduced the freed-client timer.
    for (int e = 0; e < 2048; ++e) {
        cb.add_endpoint(crpc::endpoint::inproc("stalled"), backend);
    }
    cb.load_balancer(crpc::resilience::lb_strategy::least_inflight);
    cb.pool_size(1);
    cb.default_deadline(20ms);
    cb.on_shard(shard);

    constexpr int kIterations = 400;
    std::vector<std::unique_ptr<crpc::client::call<int>>> held;
    held.reserve(kIterations);
    int started = 0;
    int refused = 0;
    for (int i = 0; i < kIterations; ++i) {
        auto cli = cb.build();
        if (!cli) {
            break;
        }
        racing_start_job job;
        job.cli = cli.get();
        job.out = &held;
        if (!shard.spawn(&racing_start_entry, &job)) {
            break;
        }
        while (!job.go.load(std::memory_order_acquire)) {
            // Busy-spin: a yield() deschedules the closer for a full quantum,
            // past the window.
        }
        // Race the close against the attempt's registration + timer arm.
        cli->close();
        const bool done = crpc_test::wait_for(
            [&job] { return job.done.load(std::memory_order_acquire); }, 2000);
        CRPC_CHECK(done);
        if (!done) {
            break;
        }
        if (job.started_ok) {
            ++started;
        } else {
            ++refused;
        }
        cli.reset();
    }
    // Any timer armed after close() on a now-freed client fires in this window.
    std::this_thread::sleep_for(100ms);
    held.clear();
    shard.stop();
    shard.join();
    CRPC_CHECK_EQ(kIterations, started + refused);
}

CRPC_TEST(facade_close_wakes_blocked_get) {
    stalled_backend backend;
    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc("stalled"), backend);
    cb.pool_size(1);
    cb.default_deadline(10s);
    cb.plain_thread_timeout(5s);
    cb.on_shard(shard);

    auto cli = cb.build();
    CRPC_CHECK(cli != nullptr);
    if (!cli) {
        shard.stop();
        shard.join();
        return;
    }

    std::atomic<bool> in_get{false};
    int code = -1;
    std::thread closer([&] {
        (void)crpc_test::wait_for([&] { return in_get.load(std::memory_order_acquire); },
                                  2000);
        // Give the coroutine time to park inside get().
        std::this_thread::sleep_for(20ms);
        cli->close();
    });

    const auto begin = clock_type::now();
    const bool ran = run_on_shard(
        shard,
        [&] {
            auto stub = cli->service<int, int>(1, 1);
            auto started = stub.call(1);
            if (!started.ok()) {
                code = static_cast<int>(started.code());
                return;
            }
            auto handle = std::move(started).value();
            in_get.store(true, std::memory_order_release);
            auto result = handle.get();
            code = result.ok() ? 0 : static_cast<int>(result.code());
        },
        2000);
    closer.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             clock_type::now() - begin)
                             .count();
    // R2: close() must finalize with `shutting_down` and notify, not let the
    // blocking wait time out.
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::shutting_down), code);
    CRPC_CHECK(elapsed < 1000);

    shard.stop();
    shard.join();
}

// --- A-3: a client without a shard must still enforce the call deadline. ---

CRPC_TEST(facade_plain_thread_deadline_enforced) {
    crpc::inproc_backend backend;
    service_state service;
    const std::string name = "crpc-deadline-plain";

    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc(name), backend);
    register_services(sb, service);
    sb.handler_workers(8);
    sb.drain_timeout(2000ms);
    auto srv = sb.build();
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }
    CRPC_CHECK(srv->start().ok());

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc(name), backend);
    cb.pool_size(1);
    cb.default_deadline(5s);
    cb.plain_thread_timeout(5s);
    // No shard: plain-thread path.
    auto cli = cb.build();
    CRPC_CHECK(cli != nullptr);
    if (!cli) {
        srv->stop();
        return;
    }

    auto stub = cli->service<int, int>(kService, kSlow);
    crpc::rpc::call_context ctx;
    ctx.has_deadline = true;
    ctx.deadline = clock_type::now() + 50ms;

    const auto begin = clock_type::now();
    int code = -1;
    auto started = stub.call(1, ctx);
    if (!started.ok()) {
        code = static_cast<int>(started.code());
    } else {
        auto handle = std::move(started).value();
        auto result = handle.get();
        code = result.ok() ? 0 : static_cast<int>(result.code());
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             clock_type::now() - begin)
                             .count();
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::deadline_exceeded), code);
    CRPC_CHECK(elapsed < 180);

    cli->close();
    srv->stop();
}

#if defined(_WIN32)
CRPC_TEST(iocp_echo_when_available) {
    crpc::iocp_backend backend;
    service_state service;

    std::unique_ptr<crpc::server> srv;
    crpc::endpoint bound;
    for (crpc::u16 port = 43210; port < 43220; ++port) {
        crpc::server::builder sb;
        sb.add_listener(crpc::endpoint::tcp("127.0.0.1", port), backend);
        register_services(sb, service);
        sb.handler_workers(8);
        sb.drain_timeout(1000ms);
        auto candidate = sb.build();
        if (!candidate) {
            continue;
        }
        if (!candidate->start().ok()) {
            continue;
        }
        srv = std::move(candidate);
        bound = crpc::endpoint::tcp("127.0.0.1", port);
        break;
    }
    CRPC_CHECK(srv != nullptr);
    if (!srv) {
        return;
    }

    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(bound, backend);
    cb.default_deadline(3s);
    cb.plain_thread_timeout(5s);
    cb.on_shard(shard);
    auto cli = cb.build();
    CRPC_CHECK(cli != nullptr);
    if (!cli) {
        srv->stop();
        return;
    }

    int value = -1;
    int code = -1;
    const bool ran = run_on_shard(shard, [&] {
        auto stub = cli->service<int, int>(kService, kEcho);
        auto started = stub.call(9);
        if (!started.ok()) {
            code = static_cast<int>(started.code());
            return;
        }
        auto handle = std::move(started).value();
        auto result = handle.get();
        if (result.ok()) {
            value = result.value();
            code = 0;
        } else {
            code = static_cast<int>(result.code());
        }
    }, 10000);
    CRPC_CHECK(ran);
    CRPC_CHECK_EQ(0, code);
    CRPC_CHECK_EQ(10, value);

    cli->close();
    srv->stop();
    shard.stop();
    shard.join();
}
#endif

// --- A transport that enforces the one-in-flight-push contract ---------------
//
// epoll and inproc both refuse a second push while one is still in flight
// (operation_not_supported). The server must therefore serialise its own
// pushes instead of assuming the transport queues them. This wrapper makes the
// rule observable on every platform: it holds the first push until the test
// releases it and refuses anything that arrives meanwhile, exactly like the
// real backends do.

class holding_sink final : public crpc::io::sink {
public:
    explicit holding_sink(crpc::io::sink& inner) noexcept : inner_(inner) {}

    void push(crpc::span<const std::byte> data,
              crpc::io::completion* c) noexcept override {
        std::vector<std::byte> held;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (holding_ && !held_) {
                held_ = true;
                held_c_ = c;
                held_bytes_.assign(data.begin(), data.end());
                return;
            }
            if (held_) {
                // The real backends' behaviour: refuse, do not queue.
                rejected_.fetch_add(1, std::memory_order_relaxed);
                c->complete(crpc::io::io_result{
                    std::make_error_code(std::errc::operation_not_supported), 0});
                return;
            }
        }
        forward(data, c);
    }

    bool wait_held(int timeout_ms) {
        return crpc_test::wait_for(
            [this] {
                std::lock_guard<std::mutex> lk(m_);
                return held_;
            },
            timeout_ms);
    }

    void release() {
        crpc::io::completion* c = nullptr;
        std::vector<std::byte> bytes;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (!held_) {
                return;
            }
            c = held_c_;
            held_c_ = nullptr;
            bytes = std::move(held_bytes_);
            held_bytes_.clear();
            held_ = false;
            holding_ = false;
        }
        forward(crpc::span<const std::byte>(bytes.data(), bytes.size()), c);
    }

    int rejected() const { return rejected_.load(std::memory_order_relaxed); }

private:
    // The transport contract keeps the span valid until `complete`, so the
    // bytes travel with a completion that owns them.
    struct owned_completion final : crpc::io::completion {
        std::vector<std::byte> bytes;
        crpc::io::completion* inner = nullptr;
        void complete(crpc::io::io_result result) noexcept override {
            inner->complete(result);
            delete this;
        }
    };

    void forward(crpc::span<const std::byte> data,
                 crpc::io::completion* c) noexcept {
        owned_completion* oc = new (std::nothrow) owned_completion();
        if (oc == nullptr) {
            c->complete(crpc::io::io_result{
                std::make_error_code(std::errc::not_enough_memory), 0});
            return;
        }
        oc->bytes.assign(data.begin(), data.end());
        oc->inner = c;
        inner_.push(crpc::span<const std::byte>(oc->bytes.data(), oc->bytes.size()),
                    oc);
    }

    crpc::io::sink& inner_;
    std::mutex m_;
    bool holding_ = true;
    bool held_ = false;
    crpc::io::completion* held_c_ = nullptr;
    std::vector<std::byte> held_bytes_;
    std::atomic<int> rejected_{0};
};

class holding_connection final : public crpc::connection {
public:
    explicit holding_connection(std::unique_ptr<crpc::connection> inner)
        : inner_(std::move(inner)), sink_(inner_->sink()) {}

    crpc::io::source& source() noexcept override { return inner_->source(); }
    crpc::io::sink& sink() noexcept override { return sink_; }
    void bind_source(crpc::io::source* s) noexcept override {
        inner_->bind_source(s);
    }
    void resume_read() noexcept override { inner_->resume_read(); }
    void close() noexcept override { inner_->close(); }
    bool is_open() const noexcept override { return inner_->is_open(); }
    crpc::endpoint remote() const override { return inner_->remote(); }

    holding_sink& holding() noexcept { return sink_; }

private:
    std::unique_ptr<crpc::connection> inner_;
    holding_sink sink_;
};

class holding_backend final : public crpc::backend_factory {
public:
    explicit holding_backend(crpc::backend_factory& inner) : inner_(inner) {}

    std::unique_ptr<crpc::connection> connect(
        const crpc::endpoint& ep, const crpc::connect_options& opt) override {
        return inner_.connect(ep, opt);
    }

    std::unique_ptr<crpc::listener> listen(
        const crpc::endpoint& ep, const crpc::listen_options& opt) override;

    void note(holding_connection* c) noexcept {
        std::lock_guard<std::mutex> lk(m_);
        last_ = c;
    }

    holding_connection* last() noexcept {
        std::lock_guard<std::mutex> lk(m_);
        return last_;
    }

private:
    struct acceptor final : crpc::accept_sink {
        crpc::accept_sink* target = nullptr;
        holding_backend* owner = nullptr;
        void on_accept(std::unique_ptr<crpc::connection> c) noexcept override {
            auto wrapped =
                std::unique_ptr<holding_connection>(
                    new (std::nothrow) holding_connection(std::move(c)));
            if (!wrapped) {
                return;
            }
            owner->note(wrapped.get());
            target->on_accept(std::move(wrapped));
        }
    };

    struct listener_impl final : crpc::listener {
        std::unique_ptr<crpc::listener> inner;
        acceptor sink;
        void start(crpc::accept_sink* target) override {
            sink.target = target;
            inner->start(&sink);
        }
        void stop() noexcept override { inner->stop(); }
    };

    crpc::backend_factory& inner_;
    std::mutex m_;
    holding_connection* last_ = nullptr;
};

std::unique_ptr<crpc::listener> holding_backend::listen(
    const crpc::endpoint& ep, const crpc::listen_options& opt) {
    auto inner = inner_.listen(ep, opt);
    if (!inner) {
        return nullptr;
    }
    auto wrapper = std::unique_ptr<listener_impl>(new (std::nothrow) listener_impl());
    if (!wrapper) {
        return nullptr;
    }
    wrapper->inner = std::move(inner);
    wrapper->sink.owner = this;
    return wrapper;
}

CRPC_TEST(server_queues_a_response_while_one_push_is_in_flight) {
    // Two pipelined requests on one connection: the first response cannot be
    // written yet (the peer is slow), so the second must be queued by the
    // server. Pushing it straight into the transport violates the one-in-flight
    // rule, and the transport reports operation_not_supported -- which used to
    // make the server close the connection and lose both responses.
    fixture f;
    fixture_config cfg;
    cfg.per_connection = 8;
    cfg.max_inflight = 8;
    holding_backend holding(f.backend);

    {
        crpc::server::builder sb;
        sb.add_listener(crpc::endpoint::inproc(f.name), holding);
        register_services(sb, f.service);
        sb.handler_workers(cfg.server_workers);
        sb.per_connection_inflight(cfg.per_connection);
        sb.per_service_inflight(cfg.per_service);
        sb.drain_timeout(cfg.drain);
        sb.overload_retry_after(cfg.overload_retry);
        f.srv = sb.build();
        CRPC_CHECK(f.srv != nullptr);
        if (!f.srv) {
            return;
        }
        CRPC_CHECK(f.srv->start().ok());
    }

    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    f.shard = std::make_unique<crpc::runtime>(ro);
    f.shard->start_thread();

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc(f.name), f.backend);
    cb.pool_size(1);
    cb.max_inflight_per_connection(cfg.max_inflight);
    cb.default_deadline(cfg.default_deadline);
    cb.on_shard(*f.shard);
    f.cli = cb.build();
    CRPC_CHECK(f.cli != nullptr);
    if (!f.cli) {
        return;
    }

    auto first = f.cli->service<int, int>(kService, kEcho).call(1);
    CRPC_CHECK(first.ok());
    if (!first.ok()) {
        return;
    }
    auto first_call = std::move(first).value();

    CRPC_CHECK(crpc_test::wait_for(
        [&] { return holding.last() != nullptr; }, 5000));
    holding_connection* hc = holding.last();
    CRPC_CHECK(hc != nullptr);
    if (hc == nullptr) {
        return;
    }
    // The first response is now held: the server has an outstanding push.
    CRPC_CHECK(hc->holding().wait_held(5000));

    auto second = f.cli->service<int, int>(kService, kEcho).call(2);
    CRPC_CHECK(second.ok());
    if (!second.ok()) {
        return;
    }
    auto second_call = std::move(second).value();

    // Let the server try to write the second response while the first is still
    // in flight.
    std::this_thread::sleep_for(150ms);

    hc->holding().release();

    auto r1 = first_call.get();
    auto r2 = second_call.get();
    CRPC_CHECK(r1.ok());
    CRPC_CHECK(r2.ok());
    if (r1.ok()) {
        CRPC_CHECK_EQ(2, r1.value());
    }
    if (r2.ok()) {
        CRPC_CHECK_EQ(3, r2.value());
    }
    CRPC_CHECK_EQ(0, hc->holding().rejected());
}

} // namespace

CRPC_TEST_MAIN()
