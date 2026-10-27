// CRPC -- worker executor tests.
//
// The executor exists to run a blocking call off the caller's thread. The tests
// pin the three ways that goes wrong: an unbounded queue, a worker waiting on its
// own pool (a deadlock by construction), and shutdown dropping queued work so a
// handed-out future never completes.
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/async/executor.hpp>
#include <crpc/error.hpp>

namespace {

using crpc::async::executor;
using crpc::async::executor_options;

[[nodiscard]] executor_options one_thread() {
    executor_options options;
    options.threads = 1;
    options.queue_capacity = 64;
    return options;
}

} // namespace

CRPC_TEST(executor_runs_a_task_and_returns_its_value) {
    executor pool(one_thread());
    auto future = pool.submit([] { return 42; });
    CRPC_CHECK(future.valid());
    auto value = future.get();
    CRPC_CHECK(value.ok());
    if (value.ok()) {
        CRPC_CHECK_EQ(42, value.value());
    }
}

CRPC_TEST(executor_carries_a_result_failure_as_data) {
    executor pool(one_thread());
    auto future = pool.submit([]() -> crpc::result<int> {
        return crpc::rpc_error{crpc::rpc_code::not_found, "no such row"};
    });
    auto value = future.get();
    CRPC_CHECK_FALSE(value.ok());
    if (!value.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                      static_cast<int>(value.error().code));
        CRPC_CHECK_EQ(std::string("no such row"), value.error().message);
    }
}

CRPC_TEST(executor_try_get_reports_not_ready) {
    executor pool(one_thread());
    // Block the only worker until the test releases it, so the future is observably
    // pending.
    std::atomic<bool> release{false};
    auto future = pool.submit([&release] {
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return 7;
    });

    auto early = future.try_get();
    CRPC_CHECK_FALSE(early.ok());
    if (!early.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                      static_cast<int>(early.error().code));
    }
    CRPC_CHECK_FALSE(future.ready());

    release.store(true, std::memory_order_release);
    auto value = future.get();
    CRPC_CHECK(value.ok());
    if (value.ok()) {
        CRPC_CHECK_EQ(7, value.value());
    }
}

CRPC_TEST(executor_wait_for_times_out) {
    executor pool(one_thread());
    std::atomic<bool> release{false};
    auto future = pool.submit([&release] {
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return 1;
    });
    auto timed = future.wait_for(std::chrono::milliseconds(20));
    CRPC_CHECK_FALSE(timed.ok());
    if (!timed.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::deadline_exceeded),
                      static_cast<int>(timed.error().code));
    }
    release.store(true, std::memory_order_release);
    CRPC_CHECK(future.get().ok());
}

CRPC_TEST(executor_refuses_a_worker_blocking_on_its_own_pool) {
    executor pool(one_thread());
    // The task waits on a future of the same one-thread pool. Without the guard this
    // is a permanent hang: the only worker is the one waiting.
    std::atomic<int> code{0};
    auto outer = pool.submit([&pool, &code]() -> int {
        auto inner = pool.submit([] { return 5; });
        auto value = inner.get();
        if (!value.ok()) {
            code.store(static_cast<int>(value.error().code), std::memory_order_release);
        }
        return value.ok() ? value.value() : -1;
    });
    auto value = outer.get();
    CRPC_CHECK(value.ok());
    if (value.ok()) {
        CRPC_CHECK_EQ(-1, value.value());
    }
    // The nested wait was refused instead of deadlocking.
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  code.load(std::memory_order_acquire));
}

CRPC_TEST(executor_rejects_when_the_queue_is_full) {
    executor_options options = one_thread();
    options.queue_capacity = 1;
    executor pool(options);

    std::atomic<bool> release{false};
    std::atomic<bool> started{false};
    auto blocker = pool.submit([&release, &started] {
        started.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return 0;
    });
    // Wait until the worker is actually running the blocker. Without this the queue
    // slot is still free and the next submit takes it — a race in the test, not in
    // the executor.
    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // The worker is busy, so this one occupies the single queue slot.
    auto queued = pool.submit([] { return 1; });
    CRPC_CHECK(queued.valid());
    // The queue is full: the next submit is refused and returns an invalid future,
    // so a caller cannot wait forever on work that was never accepted.
    auto rejected = pool.submit([] { return 2; });
    CRPC_CHECK_FALSE(rejected.valid());

    release.store(true, std::memory_order_release);
    CRPC_CHECK(blocker.get().ok());
    CRPC_CHECK(queued.get().ok());
}

CRPC_TEST(executor_shutdown_drains_queued_tasks) {
    std::atomic<int> completed{0};
    {
        executor pool(one_thread());
        std::atomic<bool> hold{false};
        auto first = pool.submit([&hold, &completed] {
            while (!hold.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            completed.fetch_add(1, std::memory_order_relaxed);
            return 0;
        });
        auto second = pool.submit([&completed] {
            completed.fetch_add(1, std::memory_order_relaxed);
            return 1;
        });
        auto third = pool.submit([&completed] {
            completed.fetch_add(1, std::memory_order_relaxed);
            return 2;
        });
        hold.store(true, std::memory_order_release);
        // The destructor must run both queued tasks before joining, so every future
        // handed out completes.
        CRPC_CHECK(first.get().ok());
        CRPC_CHECK(second.get().ok());
        CRPC_CHECK(third.get().ok());
    }
    CRPC_CHECK_EQ(3, completed.load(std::memory_order_relaxed));
}

CRPC_TEST(executor_shutdown_is_idempotent) {
    executor pool(one_thread());
    auto future = pool.submit([] { return 3; });
    pool.shutdown();
    pool.shutdown();
    CRPC_CHECK(pool.is_closed());
    // A task submitted after shutdown is refused rather than queued forever.
    auto late = pool.submit([] { return 4; });
    CRPC_CHECK_FALSE(late.valid());
    // The task accepted before shutdown still completed.
    auto value = future.get();
    CRPC_CHECK(value.ok());
    if (value.ok()) {
        CRPC_CHECK_EQ(3, value.value());
    }
}

CRPC_TEST(executor_runs_tasks_in_parallel_with_enough_threads) {
    executor_options options;
    options.threads = 4;
    options.queue_capacity = 32;
    executor pool(options);

    std::atomic<int> in_flight{0};
    std::atomic<int> peak{0};
    std::vector<crpc::async::future<int>> futures;
    for (int i = 0; i < 4; ++i) {
        futures.push_back(pool.submit([&in_flight, &peak] {
            const int now = in_flight.fetch_add(1, std::memory_order_acq_rel) + 1;
            int seen = peak.load(std::memory_order_acquire);
            while (now > seen && !peak.compare_exchange_weak(seen, now)) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            in_flight.fetch_sub(1, std::memory_order_acq_rel);
            return 1;
        }));
    }
    for (auto& future : futures) {
        CRPC_CHECK(future.get().ok());
    }
    // With four threads and four blocking tasks, more than one must have overlapped.
    CRPC_CHECK(peak.load(std::memory_order_acquire) > 1);
}

CRPC_TEST_MAIN()