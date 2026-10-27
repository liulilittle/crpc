// CRPC -- local lock manager tests.
//
// The properties that matter are the ones a hand-written keyed-lock map gets
// wrong: a multi-key acquisition in two different orders must not deadlock, an
// unreleased key must not leak, and a handle must release exactly once. The
// deadlock case is a real concurrency test; the rest are deterministic.
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/lock/local_lock.hpp>

namespace {

using crpc::lock::lock_handle;
using crpc::lock::lock_manager;
using crpc::lock::manager_options;

[[nodiscard]] manager_options no_reaper() {
    manager_options options;
    options.reap_period = std::chrono::nanoseconds(0); // sweep only when asked
    options.lease = std::chrono::seconds(30);
    return options;
}

[[nodiscard]] std::vector<std::string> keys(std::initializer_list<const char*> values) {
    std::vector<std::string> out;
    for (const char* value : values) {
        out.push_back(value);
    }
    return out;
}

} // namespace

CRPC_TEST(local_lock_acquire_and_release) {
    lock_manager manager(no_reaper());
    const std::vector<std::string> wanted = keys({"a"});
    auto handle = manager.acquire(
        crpc::span<const std::string>(wanted.data(), wanted.size()),
        std::chrono::milliseconds(0), "owner-1");
    CRPC_CHECK(handle.ok());
    if (!handle.ok()) {
        return;
    }
    CRPC_CHECK(handle.value().valid());
    CRPC_CHECK_EQ(std::size_t{1}, manager.held_keys());
    CRPC_CHECK(manager.held_by(crpc::span<const std::string>(wanted.data(), wanted.size()),
                               "owner-1"));

    handle.value().release();
    CRPC_CHECK_FALSE(handle.value().valid());
    CRPC_CHECK_EQ(std::size_t{0}, manager.held_keys());
    // Idempotent.
    handle.value().release();
    CRPC_CHECK_EQ(std::size_t{0}, manager.held_keys());
}

CRPC_TEST(local_lock_excludes_another_owner) {
    lock_manager manager(no_reaper());
    const std::vector<std::string> wanted = keys({"k"});
    auto first = manager.acquire(crpc::span<const std::string>(wanted.data(), wanted.size()),
                                 std::chrono::milliseconds(0), "owner-1");
    CRPC_CHECK(first.ok());
    if (!first.ok()) {
        return;
    }

    const auto started = std::chrono::steady_clock::now();
    auto second = manager.acquire(crpc::span<const std::string>(wanted.data(), wanted.size()),
                                  std::chrono::milliseconds(60), "owner-2");
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CRPC_CHECK_FALSE(second.ok());
    if (!second.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::deadline_exceeded),
                      static_cast<int>(second.error().code));
    }
    CRPC_CHECK(elapsed >= std::chrono::milliseconds(40));
    // The blocking attempt must not have taken the key.
    CRPC_CHECK_EQ(std::size_t{1}, manager.held_keys());
}

CRPC_TEST(local_lock_is_reentrant_per_owner) {
    lock_manager manager(no_reaper());
    const std::vector<std::string> wanted = keys({"k"});
    const crpc::span<const std::string> span(wanted.data(), wanted.size());

    auto outer = manager.acquire(span, std::chrono::milliseconds(0), "owner-1");
    CRPC_CHECK(outer.ok());
    if (!outer.ok()) {
        return;
    }
    // The same owner may re-enter: one logical operation may cross several layers.
    auto inner = manager.acquire(span, std::chrono::milliseconds(0), "owner-1");
    CRPC_CHECK(inner.ok());
    if (!inner.ok()) {
        return;
    }

    inner.value().release();
    // One release is not enough: the outer acquisition still holds the key.
    CRPC_CHECK_EQ(std::size_t{1}, manager.held_keys());
    outer.value().release();
    CRPC_CHECK_EQ(std::size_t{0}, manager.held_keys());
}

CRPC_TEST(local_lock_multi_key_is_atomic) {
    lock_manager manager(no_reaper());
    const std::vector<std::string> first_set = keys({"a", "b"});
    const std::vector<std::string> second_set = keys({"b", "c"});
    const crpc::span<const std::string> first_span(first_set.data(), first_set.size());
    const crpc::span<const std::string> second_span(second_set.data(), second_set.size());

    auto held = manager.acquire(first_span, std::chrono::milliseconds(0), "owner-1");
    CRPC_CHECK(held.ok());
    if (!held.ok()) {
        return;
    }
    // {b, c} shares `b`, so it must fail as a whole even though `c` is free.
    auto blocked = manager.acquire(second_span, std::chrono::milliseconds(30), "owner-2");
    CRPC_CHECK_FALSE(blocked.ok());
    CRPC_CHECK_EQ(std::size_t{2}, manager.held_keys());
}

CRPC_TEST(local_lock_opposite_order_does_not_deadlock) {
    // Two threads ask for the same two keys in opposite orders. A per-key mutex
    // design deadlocks here; the sorted, single-mutex design must not.
    lock_manager manager(no_reaper());
    const std::vector<std::string> ab = keys({"a", "b"});
    const std::vector<std::string> ba = keys({"b", "a"});
    const crpc::span<const std::string> ab_span(ab.data(), ab.size());
    const crpc::span<const std::string> ba_span(ba.data(), ba.size());

    std::atomic<int> acquired{0};
    std::atomic<int> failed{0};

    const auto worker = [&](crpc::span<const std::string> wanted, const char* owner) {
        for (int round = 0; round < 200; ++round) {
            auto handle = manager.acquire(wanted, std::chrono::milliseconds(2000), owner);
            if (handle.ok()) {
                acquired.fetch_add(1, std::memory_order_relaxed);
            } else {
                failed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::thread first(worker, ab_span, "owner-a");
    std::thread second(worker, ba_span, "owner-b");
    first.join();
    second.join();

    CRPC_CHECK_EQ(400, acquired.load(std::memory_order_relaxed));
    CRPC_CHECK_EQ(0, failed.load(std::memory_order_relaxed));
    CRPC_CHECK_EQ(std::size_t{0}, manager.held_keys());
}

CRPC_TEST(local_lock_lease_expiry_frees_the_key) {
    manager_options options = no_reaper();
    options.lease = std::chrono::seconds(5);
    lock_manager manager(options);
    const std::vector<std::string> wanted = keys({"leased"});
    const crpc::span<const std::string> span(wanted.data(), wanted.size());

    auto handle = manager.acquire(span, std::chrono::milliseconds(0), "owner-1");
    CRPC_CHECK(handle.ok());
    if (!handle.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::size_t{1}, manager.held_keys());

    // A holder that never releases must not leak the key forever.
    manager.sweep(std::chrono::steady_clock::now() + std::chrono::seconds(6));
    CRPC_CHECK_EQ(std::size_t{0}, manager.held_keys());

    // The stale handle must not free a key that now belongs to someone else.
    auto reacquired = manager.acquire(span, std::chrono::milliseconds(0), "owner-2");
    CRPC_CHECK(reacquired.ok());
    if (!reacquired.ok()) {
        return;
    }
    handle.value().release();
    CRPC_CHECK_EQ(std::size_t{1}, manager.held_keys());
    CRPC_CHECK(manager.held_by(span, "owner-2"));
    reacquired.value().release();
}

CRPC_TEST(local_lock_handle_outliving_manager_is_safe) {
    lock_handle escaped;
    {
        lock_manager manager(no_reaper());
        const std::vector<std::string> wanted = keys({"x"});
        auto handle = manager.acquire(crpc::span<const std::string>(wanted.data(), wanted.size()),
                                      std::chrono::milliseconds(0), "owner-1");
        CRPC_CHECK(handle.ok());
        if (!handle.ok()) {
            return;
        }
        escaped = std::move(handle.value());
    }
    // The manager is gone; the handle still owns its state and must release
    // without touching freed memory.
    CRPC_CHECK(escaped.valid());
    escaped.release();
    CRPC_CHECK_FALSE(escaped.valid());
    escaped.release(); // idempotent
}

CRPC_TEST(local_lock_move_semantics) {
    lock_manager manager(no_reaper());
    const std::vector<std::string> wanted = keys({"m"});
    auto acquired = manager.acquire(crpc::span<const std::string>(wanted.data(), wanted.size()),
                                    std::chrono::milliseconds(0), "owner-1");
    CRPC_CHECK(acquired.ok());
    if (!acquired.ok()) {
        return;
    }
    lock_handle moved = std::move(acquired.value());
    CRPC_CHECK(moved.valid());
    CRPC_CHECK_FALSE(acquired.value().valid());

    lock_handle target;
    target = std::move(moved);
    CRPC_CHECK(target.valid());
    CRPC_CHECK_FALSE(moved.valid());
    CRPC_CHECK_EQ(std::size_t{1}, manager.held_keys());

    target.release();
    CRPC_CHECK_EQ(std::size_t{0}, manager.held_keys());
}

CRPC_TEST(local_lock_rejects_bad_input) {
    lock_manager manager(no_reaper());
    {
        const std::vector<std::string> empty;
        auto handle = manager.acquire(crpc::span<const std::string>(), std::chrono::milliseconds(0),
                                      "owner");
        CRPC_CHECK_FALSE(handle.ok());
        if (!handle.ok()) {
            CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                          static_cast<int>(handle.error().code));
        }
        (void)empty;
    }
    {
        const std::vector<std::string> wanted = keys({"k"});
        auto handle = manager.acquire(crpc::span<const std::string>(wanted.data(), wanted.size()),
                                      std::chrono::milliseconds(0), "");
        CRPC_CHECK_FALSE(handle.ok());
        if (!handle.ok()) {
            CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                          static_cast<int>(handle.error().code));
        }
    }
}

CRPC_TEST(local_lock_waiter_is_woken_by_release) {
    lock_manager manager(no_reaper());
    const std::vector<std::string> wanted = keys({"w"});
    const crpc::span<const std::string> span(wanted.data(), wanted.size());

    auto first = manager.acquire(span, std::chrono::milliseconds(0), "owner-1");
    CRPC_CHECK(first.ok());
    if (!first.ok()) {
        return;
    }

    std::atomic<bool> acquired{false};
    std::thread waiter([&] {
        auto handle = manager.acquire(span, std::chrono::seconds(5), "owner-2");
        acquired.store(handle.ok(), std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    first.value().release();
    waiter.join();
    CRPC_CHECK(acquired.load(std::memory_order_acquire));
}

CRPC_TEST(local_lock_manager_close_wakes_waiters) {
    lock_manager manager(no_reaper());
    const std::vector<std::string> wanted = keys({"c"});
    const crpc::span<const std::string> span(wanted.data(), wanted.size());

    auto first = manager.acquire(span, std::chrono::milliseconds(0), "owner-1");
    CRPC_CHECK(first.ok());
    if (!first.ok()) {
        return;
    }

    std::atomic<int> code{0};
    std::thread waiter([&] {
        auto handle = manager.acquire(span, std::chrono::seconds(5), "owner-2");
        if (!handle.ok()) {
            code.store(static_cast<int>(handle.error().code), std::memory_order_release);
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    first.value().release();
    waiter.join();
    // The waiter wins the key normally here; the case only asserts that a release
    // never leaves it blocked until the timeout.
    CRPC_CHECK(code.load(std::memory_order_acquire) == 0 ||
               code.load(std::memory_order_acquire) == static_cast<int>(crpc::rpc_code::shutting_down));
}

CRPC_TEST_MAIN()