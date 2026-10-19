// CRPC -- bound_guard lifetime tests.
//
// The guard is the shared primitive that lets a backend borrow an io::source
// and still guarantee that bind_source() (including bind_source(nullptr)) does
// not return while a callback is running. These tests pin the primitive's
// contract directly; the epoll test in test_io_epoll.cpp exercises it through a
// real backend.

#include <crpc/io.hpp>

#include "../src/io/bound_guard.hpp"

#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <thread>

namespace {

struct dummy_source final : crpc::io::source {
    std::size_t on_data(crpc::span<const std::byte> data) noexcept override {
        return data.size();
    }
    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {}
};

bool wait_flag(const std::atomic<bool>& flag, int timeout_ms = 3000) {
    return crpc_test::wait_for(
        [&flag] { return flag.load(std::memory_order_acquire); }, timeout_ms);
}

} // namespace

CRPC_TEST(bound_guard_enter_rebind_and_retire) {
    crpc::io::detail::bound_guard guard;
    dummy_source first;
    dummy_source second;

    CRPC_CHECK(guard.enter() == nullptr);
    CRPC_CHECK_FALSE(guard.active());

    guard.rebind(&first);
    CRPC_CHECK(guard.active());
    crpc::io::source* s = guard.enter();
    CRPC_CHECK(s == &first);
    guard.leave();

    // Rebinding to a non-null source clears the retired state.
    guard.retire();
    CRPC_CHECK_FALSE(guard.active());
    CRPC_CHECK(guard.enter() == nullptr);
    guard.rebind(&second);
    CRPC_CHECK(guard.active());
    s = guard.enter();
    CRPC_CHECK(s == &second);
    guard.leave();
}

CRPC_TEST(bound_guard_rebind_waits_for_in_flight_call) {
    crpc::io::detail::bound_guard guard;
    dummy_source first;
    dummy_source second;
    guard.rebind(&first);

    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<bool> left{false};

    std::thread worker([&] {
        crpc::io::source* s = guard.enter();
        if (s != &first) {
            return;
        }
        entered.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        guard.leave();
        left.store(true, std::memory_order_release);
    });
    CRPC_CHECK(wait_flag(entered));

    std::atomic<bool> rebind_done{false};
    std::thread rebinder([&] {
        guard.rebind(&second);
        rebind_done.store(true, std::memory_order_release);
    });

    // The call is still in flight, so rebind must block.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CRPC_CHECK_FALSE(rebind_done.load(std::memory_order_acquire));

    release.store(true, std::memory_order_release);
    worker.join();
    rebinder.join();
    CRPC_CHECK(left.load(std::memory_order_acquire));
    CRPC_CHECK(rebind_done.load(std::memory_order_acquire));

    crpc::io::source* s = guard.enter();
    CRPC_CHECK(s == &second);
    guard.leave();
}

CRPC_TEST(bound_guard_retire_waits_for_in_flight_call) {
    crpc::io::detail::bound_guard guard;
    dummy_source source;
    guard.rebind(&source);

    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::thread worker([&] {
        if (guard.enter() == nullptr) {
            return;
        }
        entered.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        guard.leave();
    });
    CRPC_CHECK(wait_flag(entered));

    std::atomic<bool> retired{false};
    std::thread retirer([&] {
        guard.retire();
        retired.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CRPC_CHECK_FALSE(retired.load(std::memory_order_acquire));
    CRPC_CHECK_FALSE(guard.active());

    release.store(true, std::memory_order_release);
    worker.join();
    retirer.join();
    CRPC_CHECK(retired.load(std::memory_order_acquire));
    CRPC_CHECK(guard.enter() == nullptr);
}

// A callback that unbinds its own connection must not wait for itself.
CRPC_TEST(bound_guard_self_rebind_is_exempt) {
    crpc::io::detail::bound_guard guard;
    dummy_source source;
    guard.rebind(&source);

    crpc::io::source* s = guard.enter();
    CRPC_CHECK(s == &source);
    guard.retire(); // same thread: returns immediately
    CRPC_CHECK_FALSE(guard.active());
    guard.leave();
    CRPC_CHECK(guard.enter() == nullptr);
}

CRPC_TEST_MAIN()
