// CRPC -- single-producer single-consumer value queue.
//
// The queue moves values, not bytes, and the properties worth pinning are the ones a
// mutex-based queue would also satisfy while being much slower -- so they are checked
// against the failure modes a ring actually has:
//
//   * a full queue refuses the push and keeps what is already queued;
//   * a drained queue accepts exactly `capacity()` values again (no lost slot);
//   * the queue is FIFO across the wrap;
//   * the two ends really are two ends: a consumer view cannot push, so a queue
//     cannot acquire a second producer by accident.
//
// The layout check matters for the shared-memory use: a queue of `int` mapped as a
// queue of `double` would read plausible garbage, so the element size is part of the
// control block and a mismatch is refused.
#include <crpc/ipc/spsc_queue.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "test_framework.hpp"

using namespace crpc;
using namespace crpc::ipc;

namespace {

struct record {
    std::uint32_t id = 0;
    std::uint32_t payload = 0;
};

} // namespace

CRPC_TEST(spsc_queue_capacity_is_exact_with_no_sentinel_slot) {
    spsc_queue<int> q(4);
    CRPC_CHECK_EQ(static_cast<std::size_t>(4), q.capacity());
    CRPC_CHECK(q.empty());
    CRPC_CHECK_FALSE(q.full());

    // Exactly capacity values must fit. A queue that spends a slot on the full/empty
    // distinction would refuse the fourth.
    for (int i = 0; i < 4; ++i) {
        CRPC_CHECK(q.push(i));
    }
    CRPC_CHECK_EQ(static_cast<std::size_t>(4), q.size());
    CRPC_CHECK(q.full());

    // Full: the next push is refused and must not overwrite the oldest.
    CRPC_CHECK_FALSE(q.push(99));

    int value = -1;
    for (int i = 0; i < 4; ++i) {
        CRPC_CHECK(q.pop(value));
        CRPC_CHECK_EQ(i, value);
    }
    CRPC_CHECK(q.empty());
    // Popping an empty queue leaves the caller's value alone, so "no value" is not
    // confused with "a zero".
    value = 1234;
    CRPC_CHECK_FALSE(q.pop(value));
    CRPC_CHECK_EQ(1234, value);
}

CRPC_TEST(spsc_queue_is_fifo_across_the_wrap) {
    // 3 is deliberately not a power of two so the wrap lands on different offsets.
    spsc_queue<int> q(3);
    // Model the queue independently: `pushed` is what has been accepted, `popped` is
    // what has been taken. The queue must agree with that model value for value; a
    // wrap bug shows up as a mismatch rather than as a lost count.
    int pushed = 0;
    int popped = 0;
    int value = 0;
    for (int round = 0; round < 40; ++round) {
        const int burst = 1 + (round % 3);
        for (int i = 0; i < burst; ++i) {
            if (q.push(pushed)) {
                ++pushed;
            }
        }
        const int drain = 1 + ((round * 2) % 3);
        for (int i = 0; i < drain; ++i) {
            if (q.pop(value)) {
                CRPC_CHECK_EQ(popped, value);
                ++popped;
            }
        }
        CRPC_CHECK_EQ(static_cast<std::size_t>(pushed - popped), q.size());
    }
    while (q.pop(value)) {
        CRPC_CHECK_EQ(popped, value);
        ++popped;
    }
    CRPC_CHECK_EQ(pushed, popped);
}

CRPC_TEST(spsc_queue_recovers_capacity_after_drain) {
    spsc_queue<record> q(8);
    std::vector<record> written;
    for (std::uint32_t i = 0; i < 8; ++i) {
        record r;
        r.id = i;
        r.payload = i * 7;
        written.push_back(r);
        CRPC_CHECK(q.push(r));
    }
    CRPC_CHECK(q.full());

    record out;
    for (std::uint32_t i = 0; i < 8; ++i) {
        CRPC_CHECK(q.pop(out));
        CRPC_CHECK_EQ(written[i].id, out.id);
        CRPC_CHECK_EQ(written[i].payload, out.payload);
    }
    CRPC_CHECK(q.empty());
    CRPC_CHECK_EQ(static_cast<std::size_t>(8), q.free_space());

    // Refill to capacity: a queue that leaked a slot would stop one short here.
    for (std::uint32_t i = 0; i < 8; ++i) {
        record r;
        r.id = 100 + i;
        CRPC_CHECK(q.push(r));
    }
    CRPC_CHECK(q.full());
}

CRPC_TEST(spsc_queue_consumer_view_cannot_push) {
    spsc_queue<int> q(4);
    // The two ends are distinct views; the consumer end must refuse to produce, which
    // is what keeps the "one producer" invariant true by construction.
    spsc_view<int> consumer = q.consumer();
    CRPC_CHECK_FALSE(consumer.try_push(1));

    spsc_view<int> producer = q.producer();
    CRPC_CHECK(producer.try_push(7));
    int value = 0;
    CRPC_CHECK(consumer.try_pop(value));
    CRPC_CHECK_EQ(7, value);
}

CRPC_TEST(spsc_queue_control_block_rejects_a_different_element_type) {
    spsc_control control{};
    spsc_initialise(&control, 16, sizeof(int));
    CRPC_CHECK(spsc_matches(&control, sizeof(int)));
    // Same capacity, different element size: reading this as a queue of `double` would
    // be plausible garbage, so it must be refused.
    CRPC_CHECK_FALSE(spsc_matches(&control, sizeof(double)));
    // An all-zero region is not a queue.
    spsc_control blank{};
    CRPC_CHECK_FALSE(spsc_matches(&blank, sizeof(int)));
    // A zero-capacity queue is not usable either.
    spsc_control empty{};
    spsc_initialise(&empty, 0, sizeof(int));
    CRPC_CHECK_FALSE(spsc_matches(&empty, sizeof(int)));
}

CRPC_TEST(spsc_queue_carries_values_between_threads_in_order) {
    constexpr int kTotal = 5000;
    spsc_queue<std::uint64_t> q(64);
    std::vector<std::uint64_t> received;
    received.reserve(kTotal);

    std::thread consumer([&] {
        std::uint64_t value = 0;
        while (static_cast<int>(received.size()) < kTotal) {
            if (q.pop(value)) {
                received.push_back(value);
            } else {
                std::this_thread::yield();
            }
        }
    });

    for (int i = 0; i < kTotal; ++i) {
        const std::uint64_t value = static_cast<std::uint64_t>(i);
        // Genuine back-pressure: the queue is smaller than the total, so the producer
        // waits for the consumer. The wait is bounded by the consumer making progress,
        // which it must.
        while (!q.push(value)) {
            std::this_thread::yield();
        }
    }
    consumer.join();

    CRPC_CHECK_EQ(static_cast<std::size_t>(kTotal), received.size());
    bool in_order = true;
    for (int i = 0; i < kTotal; ++i) {
        if (received[static_cast<std::size_t>(i)] != static_cast<std::uint64_t>(i)) {
            in_order = false;
            break;
        }
    }
    CRPC_CHECK(in_order);
}

CRPC_TEST_MAIN()
