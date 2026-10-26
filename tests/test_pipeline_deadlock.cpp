// CRPC -- regression test for the reliable pipelined deadlock.
//
// A pipelined batch issues `batch` calls before waiting for any of them. With
// concurrency > 1 and batch > 1 the in-process backend can deadlock in a Debug
// build: the emit path holds the connection slot lock while the synchronous
// completion path takes the client's completion lock, and a server handler
// coroutine can hold the completion lock while it waits on a worker. The cycle
// is timing-dependent, so the case runs a bounded workload and a watchdog: a
// run that makes no progress for `kWatchdogMs` is killed with a distinct exit
// code and a message, which the test runner (and `ctest`) reports as a
// failure rather than a silent hang.
//
// The parameters mirror the published pipeline benchmark (server 4 shards /
// 8 handler workers, client pool = concurrency, batch > 1) at a smaller
// workload so the case runs in well under a second once it is fixed.
#include <crpc/client_facade.hpp>
#include <crpc/rpc/dispatcher.hpp>
#include <crpc/server.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "test_framework.hpp"

namespace {

using namespace std::chrono_literals;

constexpr crpc::u64 k_service = 710;
constexpr crpc::u64 k_echo = 1;

// No-progress watchdog. A deadlocked process cannot be unblocked from inside:
// the workers hold library locks, so the process is exited. The exit code and
// the message make the cause visible without a debugger. The watchdog is only
// armed on a separate thread; on the fixed path the workers finish first and
// the main thread joins them normally.
constexpr int kWatchdogMs = 12000;
constexpr int kDeadlockExitCode = 42;

// Overridable so a debugger can be attached to a live deadlock.
int watchdog_ms() {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, "CRPC_PIPELINE_WATCHDOG_MS") == 0 && value != nullptr) {
        const int parsed = static_cast<int>(std::strtol(value, nullptr, 10));
        std::free(value);
        if (parsed > 0) {
            return parsed;
        }
    }
#else
    const char* value = std::getenv("CRPC_PIPELINE_WATCHDOG_MS");
    if (value != nullptr) {
        const int parsed = static_cast<int>(std::strtol(value, nullptr, 10));
        if (parsed > 0) {
            return parsed;
        }
    }
#endif
    return kWatchdogMs;
}

struct echo_service {
    crpc::u64 echo(const crpc::u64& value) { return value + 1; }
};

struct pipeline_fixture {
    crpc::inproc_backend backend;
    echo_service service;
    std::unique_ptr<crpc::server> srv;
    std::unique_ptr<crpc::client> cli;
    std::string name = "crpc-pipeline-deadlock";

    ~pipeline_fixture() {
        if (cli) {
            cli->close();
        }
        if (srv) {
            srv->stop();
        }
    }
};

bool start_pipeline(pipeline_fixture& f, std::size_t concurrency) {
    crpc::server::builder sb;
    sb.add_listener(crpc::endpoint::inproc(f.name), f.backend);
    sb.add_service(k_service, "bench.pipeline");
    sb.template add_method<echo_service, &echo_service::echo>(
        k_service, k_echo, "echo", f.service);
    sb.shards(4);
    sb.handler_workers(8);
    sb.max_inflight_per_connection(65536);
    sb.per_service_inflight(65536);
    sb.drain_timeout(200ms);
    f.srv = sb.build();
    if (!f.srv || !f.srv->start().ok()) {
        return false;
    }

    crpc::client::builder cb;
    cb.add_endpoint(crpc::endpoint::inproc(f.name), f.backend);
    cb.pool_size(concurrency);
    cb.slot_capacity(16384);
    cb.max_inflight_per_connection(16384);
    cb.default_deadline(5s);
    cb.plain_thread_timeout(5s);
    cb.default_idempotent(true);
    crpc::resilience::retry_policy retry;
    retry.max_attempts = 1;
    cb.retry(retry);
    crpc::resilience::breaker_config breaker;
    breaker.min_requests = 1000000000u;
    breaker.failure_ratio_threshold = 1.0;
    breaker.slow_call_threshold = 1h;
    breaker.slow_call_ratio_threshold = 1.0;
    breaker.open_duration = 1s;
    cb.breaker(breaker);
    cb.resilience_enabled(false);
    // No `on_shard`: the pipelined path under test is the plain-thread client.
    f.cli = cb.build();
    return f.cli != nullptr;
}

struct pipeline_result {
    std::size_t completed = 0;
    std::size_t failed = 0;
};

// Runs one round of the pipelined shape. `handles` is destroyed as soon as it
// is read (the CF-1 fix makes this safe), which is the exact shape the
// deadlock was found in. `progress` is bumped per completed call so the
// watchdog can distinguish a stall from a slow round.
pipeline_result run_pipelined(pipeline_fixture& f, std::size_t concurrency,
                              std::size_t batch, std::size_t total,
                              std::atomic<std::size_t>& progress) {
    pipeline_result out;
    std::atomic<std::size_t> completed{0};
    std::atomic<std::size_t> errors{0};
    const std::size_t base = total / concurrency;
    const std::size_t remainder = total % concurrency;

    std::vector<std::thread> workers;
    workers.reserve(concurrency);
    for (std::size_t worker = 0; worker < concurrency; ++worker) {
        workers.emplace_back([&, worker] {
            const std::size_t count = base + (worker < remainder ? 1 : 0);
            std::vector<crpc::client::call<crpc::u64>> handles;
            handles.reserve(batch);
            std::size_t issued = 0;
            std::size_t done = 0;
            std::size_t failed = 0;
            while (issued < count) {
                const std::size_t n = std::min(batch, count - issued);
                handles.clear();
                for (std::size_t i = 0; i < n; ++i) {
                    auto stub = f.cli->service<crpc::u64, crpc::u64>(k_service, k_echo);
                    auto started = stub.call(issued + i);
                    if (!started.ok()) {
                        ++failed;
                        continue;
                    }
                    handles.push_back(std::move(started).value());
                }
                for (crpc::client::call<crpc::u64>& handle : handles) {
                    auto result = handle.get();
                    if (result.ok()) {
                        ++done;
                    } else {
                        ++failed;
                    }
                    progress.fetch_add(1, std::memory_order_release);
                }
                issued += n;
            }
            completed.fetch_add(done, std::memory_order_relaxed);
            errors.fetch_add(failed, std::memory_order_relaxed);
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    out.completed = completed.load(std::memory_order_relaxed);
    out.failed = errors.load(std::memory_order_relaxed);
    return out;
}

// Arms the no-progress watchdog and returns the stop flag. The watchdog exits
// the process with `kDeadlockExitCode` when the last observed progress is
// older than `kWatchdogMs`.
std::atomic<bool>& arm_watchdog(std::atomic<std::size_t>& progress,
                                std::thread& watchdog) {
    static std::atomic<bool> stop{false};
    stop.store(false, std::memory_order_release);
    progress.store(0, std::memory_order_release);
    watchdog = std::thread([&] {
        std::size_t last = progress.load(std::memory_order_acquire);
        auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(watchdog_ms());
        while (!stop.load(std::memory_order_acquire)) {
            const std::size_t now = progress.load(std::memory_order_acquire);
            if (now != last) {
                last = now;
                deadline = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(watchdog_ms());
            } else if (std::chrono::steady_clock::now() >= deadline) {
                std::fprintf(stderr,
                             "CRPC pipeline-deadlock: no progress in %d ms; "
                             "reliable pipelined path deadlocked\n",
                             static_cast<int>(watchdog_ms()));
                std::fflush(stderr);
                std::_Exit(kDeadlockExitCode);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    return stop;
}

void stop_watchdog(std::atomic<bool>& stop, std::thread& watchdog) {
    stop.store(true, std::memory_order_release);
    if (watchdog.joinable()) {
        watchdog.join();
    }
}

} // namespace

CRPC_TEST(pipeline_reliable_high_batch_no_deadlock) {
    pipeline_fixture f;
    const std::size_t concurrency = 4;
    const std::size_t batch = 16;
    const std::size_t total = 4000;
    CRPC_CHECK(start_pipeline(f, concurrency));

    // Warm-up on the main thread: brings the connection, codec and handler
    // pools to steady state before the concurrent phase.
    for (int i = 0; i < 500; ++i) {
        auto stub = f.cli->service<crpc::u64, crpc::u64>(k_service, k_echo);
        auto started = stub.call(static_cast<crpc::u64>(i));
        if (started.ok()) {
            (void)started.value().get();
        }
    }

    std::atomic<std::size_t> progress{0};
    std::thread watchdog;
    std::atomic<bool>& stop = arm_watchdog(progress, watchdog);
    pipeline_result result = run_pipelined(f, concurrency, batch, total, progress);
    stop_watchdog(stop, watchdog);

    CRPC_CHECK_EQ(total, result.completed + result.failed);
    CRPC_CHECK_EQ(total, result.completed);
}

CRPC_TEST(pipeline_reliable_batch_64_no_deadlock) {
    pipeline_fixture f;
    const std::size_t concurrency = 4;
    const std::size_t batch = 64;
    const std::size_t total = 4096;
    CRPC_CHECK(start_pipeline(f, concurrency));

    for (int i = 0; i < 500; ++i) {
        auto stub = f.cli->service<crpc::u64, crpc::u64>(k_service, k_echo);
        auto started = stub.call(static_cast<crpc::u64>(i));
        if (started.ok()) {
            (void)started.value().get();
        }
    }

    std::atomic<std::size_t> progress{0};
    std::thread watchdog;
    std::atomic<bool>& stop = arm_watchdog(progress, watchdog);
    pipeline_result result = run_pipelined(f, concurrency, batch, total, progress);
    stop_watchdog(stop, watchdog);

    CRPC_CHECK_EQ(total, result.completed + result.failed);
    CRPC_CHECK_EQ(total, result.completed);
}

CRPC_TEST_MAIN();
