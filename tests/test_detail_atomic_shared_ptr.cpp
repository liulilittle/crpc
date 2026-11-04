// CRPC -- `detail::atomic_shared_ptr`, the snapshot-swap primitive.
//
// `cluster::registry` and `resilience::load_balancer` publish immutable snapshots
// by swapping a `shared_ptr`; readers must be able to keep the value they loaded
// while a writer publishes a new one. That contract is what this checks, and it
// is checked through the wrapper rather than the raw free functions because the
// wrapper is what the library uses in both C++17 and C++20 builds (see the
// header for why the spelling differs).
//
// The interesting case is not the round trip -- it is that a loaded reference
// keeps the *old* value alive and unchanged after the writer stores a new one.
// A plain `shared_ptr` member read without synchronisation would fail that under
// concurrency; the wrapper must not.
#include "test_framework.hpp"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include <crpc/detail/atomic_shared_ptr.hpp>

namespace {

struct payload {
    explicit payload(int v) noexcept : value(v) {}
    int value;
};

using payload_ptr = std::shared_ptr<const payload>;

} // namespace

CRPC_TEST(atomic_shared_ptr_round_trips) {
    crpc::detail::atomic_shared_ptr<const payload> slot;
    CRPC_CHECK(slot.load() == nullptr);

    slot.store(std::make_shared<const payload>(7));
    const payload_ptr first = slot.load();
    CRPC_CHECK(first != nullptr);
    if (first != nullptr) {
        CRPC_CHECK_EQ(7, first->value);
    }
}

CRPC_TEST(atomic_shared_ptr_reader_keeps_its_snapshot) {
    // The whole point: after a writer publishes a replacement, a reader that
    // already loaded keeps a valid, unchanged view of the old value.
    crpc::detail::atomic_shared_ptr<const payload> slot(
        std::make_shared<const payload>(1));
    const payload_ptr reader = slot.load();
    CRPC_CHECK_EQ(1, reader->value);

    slot.store(std::make_shared<const payload>(2));
    // The old value is still alive through the reader's reference...
    CRPC_CHECK_EQ(1, reader->value);
    // ...and the slot now reports the new one.
    CRPC_CHECK_EQ(2, slot.load()->value);
}

CRPC_TEST(atomic_shared_ptr_concurrent_swap_never_tears) {
    // Writers alternate between two payloads whose `value` encodes their
    // identity; readers must never observe a payload whose identity disagrees
    // with its own content. A torn or unsynchronised read shows up as a mismatch.
    crpc::detail::atomic_shared_ptr<const payload> slot(
        std::make_shared<const payload>(0));

    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};
    std::atomic<int> mismatches{0};

    std::thread writer([&] {
        for (int i = 0; i < 20000; ++i) {
            slot.store(std::make_shared<const payload>(i % 2));
        }
        stop.store(true, std::memory_order_release);
    });

    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_acquire)) {
                const payload_ptr view = slot.load();
                if (view == nullptr) {
                    mismatches.fetch_add(1);
                } else if (view->value != 0 && view->value != 1) {
                    mismatches.fetch_add(1);
                }
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    writer.join();
    for (std::thread& th : readers) {
        th.join();
    }

    std::printf("  reads=%d mismatches=%d\n", reads.load(), mismatches.load());
    // The readers must have actually run; a vacuous pass is the failure mode
    // this assertion exists to prevent.
    CRPC_CHECK(reads.load() > 1000);
    CRPC_CHECK_EQ(0, mismatches.load());
}

CRPC_TEST(atomic_shared_ptr_keeps_its_owner_layout) {
    // `cluster::registry` and `resilience::load_balancer` are exported and
    // consumer-constructible, so the size and alignment of this wrapper are
    // part of their ABI. The wrapper must therefore be exactly as big and as
    // aligned as a plain `std::shared_ptr`.
    //
    // This is not hypothetical. `std::atomic<std::shared_ptr<T>>` -- the C++20
    // replacement for the free-function API -- is 16-byte aligned on MSVC x64
    // while `std::shared_ptr<T>` is 8-byte aligned, so using it here changed
    // `load_balancer` from 160 to 176 bytes and `registry` alignment from 8 to
    // 16 *in C++20 builds only*. A C++20 consumer of a C++17-built DLL would
    // then disagree with the library about an object it allocates itself.
    CRPC_CHECK_EQ(sizeof(std::shared_ptr<const payload>),
                  sizeof(crpc::detail::atomic_shared_ptr<const payload>));
    CRPC_CHECK_EQ(alignof(std::shared_ptr<const payload>),
                  alignof(crpc::detail::atomic_shared_ptr<const payload>));

    // Report the numbers so a future diff shows what actually changed.
    std::printf("  wrapper sizeof=%zu align=%zu; shared_ptr sizeof=%zu align=%zu\n",
                sizeof(crpc::detail::atomic_shared_ptr<const payload>),
                alignof(crpc::detail::atomic_shared_ptr<const payload>),
                sizeof(std::shared_ptr<const payload>),
                alignof(std::shared_ptr<const payload>));
}

CRPC_TEST_MAIN()