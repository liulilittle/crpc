// CRPC -- runtime timer keep-alive regression.
//
// The timer wheel pops an entry under its lock and invokes the callback after
// releasing it. Without a keep-alive reference the owner can free the callback
// context in that window. The five-argument arm_timer overload holds a
// shared_ptr until the callback returns; this test drives that path without
// coroutines so it runs on nofiber sanitizer builds.

#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

#include <crpc/runtime.hpp>

namespace {

struct probe {
    std::atomic<bool>* fired = nullptr;
};

void probe_fn(void* ctx) noexcept {
    auto* p = static_cast<probe*>(ctx);
    p->fired->store(true, std::memory_order_release);
}

} // namespace

CRPC_TEST(runtime_arm_timer_keeps_context_alive) {
    crpc::runtime_options ro;
    ro.start_handler_pool = false;
    crpc::runtime shard(ro);
    shard.start_thread();

    std::atomic<bool> fired{false};
    auto keep = std::make_shared<probe>(probe{&fired});
    std::weak_ptr<probe> weak = keep;
    const std::uint64_t deadline =
        shard.now_ns() + 20ull * 1000ull * 1000ull;
    CRPC_CHECK(shard.arm_timer(keep.get(), deadline, &probe_fn, keep.get(),
                               keep));
    // Drop the only external reference: the wheel entry is now the sole owner.
    keep.reset();

    // The callback runs (fired) and the keep-alive is released only after it
    // returns (weak expires).
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return fired.load(std::memory_order_acquire); }, 5000));
    CRPC_CHECK(crpc_test::wait_for([&] { return weak.expired(); }, 5000));

    shard.stop();
    shard.join();
}

CRPC_TEST_MAIN()
