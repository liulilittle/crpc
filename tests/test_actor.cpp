// CRPC -- message-driven actor tests.
//
// The actor is the "message" half of the concurrency batch. It pairs a bounded
// SPSC mailbox with the existing `async::executor`, and the tests pin the four
// properties a caller depends on:
//
//   1. a posted message reaches the handler exactly once;
//   2. a full mailbox refuses with `resource_exhausted` instead of growing or
//      blocking (the M3 mutation target: a mailbox that does not refuse);
//   3. `stop()` is idempotent and drains what was already accepted;
//   4. a handler that returns an error or throws does not take the actor (or
//      the process) down.
//
// The cache-line case asserts the mailbox's producer and consumer cursors sit
// on different lines, which is what keeps post/pop from sharing a line.
//
// There is no `sleep_for` used as a synchronisation device: the tests wait on
// atomics through `crpc_test::wait_for` (bounded poll), so they are
// deterministic on a busy machine.
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/actor/actor.hpp>
#include <crpc/actor/mailbox.hpp>
#include <crpc/async/executor.hpp>
#include <crpc/cache_line.hpp>
#include <crpc/error.hpp>

namespace {

using namespace std::chrono_literals;

using crpc::actor::actor;
using crpc::actor::mailbox;
using crpc::actor::mpsc_mailbox;

[[nodiscard]] crpc::async::executor_options single_worker() {
    crpc::async::executor_options options;
    options.threads = 1;
    options.queue_capacity = 256;
    return options;
}

using int_actor = actor<int>;

} // namespace

// --- mailbox -----------------------------------------------------------------

CRPC_TEST(mailbox_posts_and_pops_in_order) {
    mailbox<int> box(4);
    CRPC_CHECK_EQ(std::size_t(4), box.capacity());
    CRPC_CHECK_EQ(std::size_t(0), box.size());

    CRPC_REQUIRE(box.try_post(1).ok());
    CRPC_REQUIRE(box.try_post(2).ok());
    CRPC_CHECK_EQ(std::size_t(2), box.size());

    int out = 0;
    CRPC_CHECK(box.try_pop(out));
    CRPC_CHECK_EQ(1, out);
    CRPC_CHECK(box.try_pop(out));
    CRPC_CHECK_EQ(2, out);
    CRPC_CHECK_FALSE(box.try_pop(out));
    CRPC_CHECK_EQ(std::size_t(0), box.size());
}

// M3 target: a full mailbox must refuse. The capacity is 2, so the third post
// is the one that has to fail with resource_exhausted.
CRPC_TEST(mailbox_refuses_when_full) {
    mailbox<int> box(2);
    CRPC_REQUIRE(box.try_post(10).ok());
    CRPC_REQUIRE(box.try_post(11).ok());

    const crpc::status overflow = box.try_post(12);
    CRPC_CHECK_FALSE(overflow.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::resource_exhausted),
                  static_cast<int>(overflow.code()));
    CRPC_CHECK_EQ(std::size_t(2), box.size());

    // The two accepted messages are still intact and in order.
    int out = 0;
    CRPC_REQUIRE(box.try_pop(out));
    CRPC_CHECK_EQ(10, out);
    CRPC_REQUIRE(box.try_pop(out));
    CRPC_CHECK_EQ(11, out);

    // Room is available again after popping.
    CRPC_CHECK(box.try_post(13).ok());
}

// The SPSC queue under real concurrency: one producer, one consumer, a queue
// smaller than the total, so the producer observes refusals and retries.  The
// first refusal is produced deterministically (fill to capacity before the
// consumer exists); requiring every refusal to come from the race instead made
// this check scheduler-dependent and flaky on multicore hosts.
CRPC_TEST(mailbox_spsc_producer_consumer_smoke) {
    constexpr int kTotal = 2000;
    mailbox<int> box(64);
    const int capacity = static_cast<int>(box.capacity());
    std::atomic<bool> producer_done{false};
    std::atomic<int> received{0};
    std::atomic<int> refusals{0};

    for (int i = 0; i < capacity; ++i) {
        CRPC_REQUIRE(box.try_post(i).ok());
    }
    CRPC_REQUIRE(!box.try_post(capacity).ok());
    refusals.fetch_add(1, std::memory_order_relaxed);

    std::thread producer([&] {
        int next = capacity;
        while (next < kTotal) {
            if (box.try_post(next).ok()) {
                ++next;
            } else {
                refusals.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        int expected = 0;
        while (expected < kTotal) {
            int out = 0;
            if (box.try_pop(out)) {
                if (out == expected) {
                    ++expected;
                } else {
                    expected = -1; // order violation; the check below fails
                }
                received.store(expected, std::memory_order_release);
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    CRPC_CHECK(producer_done.load(std::memory_order_acquire));
    CRPC_CHECK_EQ(kTotal, received.load());
    CRPC_CHECK(refusals.load() > 0);
}

CRPC_TEST(mailbox_cursors_live_on_separate_cache_lines) {
    mailbox<int> box(8);
    const auto head = reinterpret_cast<std::uintptr_t>(box.head_cursor_address());
    const auto tail = reinterpret_cast<std::uintptr_t>(box.tail_cursor_address());

    // Each cursor is aligned to a line and the two do not share one.
    CRPC_CHECK_EQ(std::uintptr_t(0), head % crpc::cache_line_size);
    CRPC_CHECK_EQ(std::uintptr_t(0), tail % crpc::cache_line_size);
    CRPC_CHECK(head != tail);
    CRPC_CHECK((head > tail ? head - tail : tail - head) >=
               crpc::cache_line_size);

    // The multi-producer fallback must keep the same cursor separation.
    mpsc_mailbox<int> fallback(8);
    const auto fh =
        reinterpret_cast<std::uintptr_t>(fallback.head_cursor_address());
    const auto ft =
        reinterpret_cast<std::uintptr_t>(fallback.tail_cursor_address());
    CRPC_CHECK_EQ(std::uintptr_t(0), fh % crpc::cache_line_size);
    CRPC_CHECK_EQ(std::uintptr_t(0), ft % crpc::cache_line_size);
    CRPC_CHECK(fh != ft);
}

CRPC_TEST(mpsc_mailbox_accepts_concurrent_producers) {
    crpc::async::executor pool(single_worker());
    mpsc_mailbox<int> box(8);
    std::atomic<int> posted{0};
    std::atomic<int> refused{0};

    std::vector<std::thread> producers;
    for (int p = 0; p < 4; ++p) {
        producers.emplace_back([&] {
            for (int i = 0; i < 100; ++i) {
                if (box.try_post(i).ok()) {
                    posted.fetch_add(1, std::memory_order_relaxed);
                } else {
                    refused.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::thread& t : producers) {
        t.join();
    }

    int drained = 0;
    int out = 0;
    while (box.try_pop(out)) {
        ++drained;
    }
    // Every accepted message is exactly the set that can be drained; a
    // refused one is never in the queue.
    CRPC_CHECK_EQ(posted.load(), drained);
    CRPC_CHECK(refused.load() > 0);
    CRPC_CHECK_EQ(0, static_cast<int>(box.size()));
}

// --- actor -------------------------------------------------------------------

CRPC_TEST(actor_posts_and_handles_each_message_once) {
    crpc::async::executor pool(single_worker());
    std::atomic<int> handled{0};
    int_actor a(pool, 16, [&](int&& value) {
        handled.fetch_add(value, std::memory_order_release);
        return crpc::status{};
    });

    CRPC_CHECK(a.running());
    CRPC_REQUIRE(a.post(1).ok());
    CRPC_REQUIRE(a.post(2).ok());
    CRPC_REQUIRE(a.post(3).ok());

    CRPC_CHECK(crpc_test::wait_for(
        [&] { return handled.load(std::memory_order_acquire) == 6; }, 5000));
    CRPC_CHECK_EQ(6, handled.load());
    CRPC_CHECK_EQ(std::uint64_t(0), a.handler_failures());
    a.stop();
    CRPC_CHECK_EQ(6, handled.load());
}

CRPC_TEST(actor_full_mailbox_refuses_with_resource_exhausted) {
    crpc::async::executor pool(single_worker());
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<int> handled{0};

    int_actor a(pool, 2, [&](int&&) {
        entered.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
        handled.fetch_add(1, std::memory_order_relaxed);
        return crpc::status{};
    });

    // The first message occupies the handler, so the mailbox fills behind it.
    CRPC_REQUIRE(a.post(1).ok());
    CRPC_REQUIRE(crpc_test::wait_for(
        [&] { return entered.load(std::memory_order_acquire); }, 5000));
    CRPC_REQUIRE(a.post(2).ok());
    CRPC_REQUIRE(a.post(3).ok());

    const crpc::status overflow = a.post(4);
    CRPC_CHECK_FALSE(overflow.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::resource_exhausted),
                  static_cast<int>(overflow.code()));
    CRPC_CHECK_EQ(0, handled.load(std::memory_order_relaxed));

    release.store(true, std::memory_order_release);
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return handled.load(std::memory_order_relaxed) == 3; }, 5000));
    a.stop();
    CRPC_CHECK_EQ(3, handled.load());
    // The refused message was never handled.
    CRPC_CHECK_EQ(std::uint64_t(0), a.handler_failures());
}

CRPC_TEST(actor_stop_is_idempotent_and_drains_accepted_messages) {
    crpc::async::executor pool(single_worker());
    std::atomic<int> handled{0};
    int_actor a(pool, 64, [&](int&&) {
        handled.fetch_add(1, std::memory_order_release);
        return crpc::status{};
    });

    for (int i = 0; i < 10; ++i) {
        CRPC_REQUIRE(a.post(i).ok());
    }

    a.stop();
    CRPC_CHECK_FALSE(a.running());
    // stop() drains: every accepted message has been handled when it returns.
    CRPC_CHECK_EQ(10, handled.load());

    a.stop();
    a.stop();
    CRPC_CHECK_EQ(10, handled.load());
    CRPC_CHECK_FALSE(a.running());

    const crpc::status after = a.post(11);
    CRPC_CHECK_FALSE(after.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::shutting_down),
                  static_cast<int>(after.code()));
    CRPC_CHECK_EQ(10, handled.load());
}

CRPC_TEST(actor_handler_failure_does_not_stop_the_actor) {
    crpc::async::executor pool(single_worker());
    std::atomic<int> handled{0};
    int_actor a(pool, 16, [&](int&& value) -> crpc::status {
        handled.fetch_add(1, std::memory_order_release);
        if (value == 1) {
            return crpc::rpc_error{crpc::rpc_code::internal,
                                   "the handler rejected this message"};
        }
        if (value == 2) {
            throw 7;
        }
        return {};
    });

    CRPC_REQUIRE(a.post(1).ok());
    CRPC_REQUIRE(a.post(2).ok());
    CRPC_REQUIRE(a.post(3).ok());

    CRPC_CHECK(crpc_test::wait_for(
        [&] { return handled.load(std::memory_order_acquire) == 3; }, 5000));
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return a.handler_failures() == 2; }, 5000));
    CRPC_CHECK(a.running());
    CRPC_CHECK_EQ(3, handled.load());
    CRPC_CHECK_EQ(std::uint64_t(2), a.handler_failures());

    // The actor is still able to accept and process more work.
    CRPC_REQUIRE(a.post(4).ok());
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return handled.load(std::memory_order_acquire) == 4; }, 5000));
    a.stop();
    CRPC_CHECK_EQ(4, handled.load());
}

CRPC_TEST(actor_stop_from_inside_a_handler_completes_the_queue) {
    crpc::async::executor pool(single_worker());
    std::atomic<int> handled{0};
    std::atomic<bool> stopped_inside{false};
    actor<int> a(pool, 16, [&](int&&) {
        const int now = handled.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (now == 1) {
            stopped_inside.store(true, std::memory_order_release);
            a.stop();
        }
        return crpc::status{};
    });

    // The handler is alive before the actor's first drain runs, so posting and
    // then letting the drain stop itself covers the re-entrant path.
    CRPC_REQUIRE(a.post(1).ok());
    CRPC_REQUIRE(a.post(2).ok());
    CRPC_REQUIRE(a.post(3).ok());

    CRPC_CHECK(crpc_test::wait_for(
        [&] { return stopped_inside.load(std::memory_order_acquire); }, 5000));
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return handled.load(std::memory_order_acquire) == 3; }, 5000));
    a.stop();
    CRPC_CHECK_EQ(3, handled.load());
    CRPC_CHECK_FALSE(a.running());
}

CRPC_TEST(actor_stop_before_any_drain_still_handles_everything) {
    crpc::async::executor pool(single_worker());
    std::atomic<int> handled{0};
    int_actor a(pool, 32, [&](int&&) {
        handled.fetch_add(1, std::memory_order_release);
        return crpc::status{};
    });

    for (int i = 0; i < 25; ++i) {
        CRPC_REQUIRE(a.post(i).ok());
    }
    // Immediately stopping races the executor task: whichever wins, stop()
    // cannot return with a message still in the mailbox.
    a.stop();
    CRPC_CHECK_EQ(25, handled.load());
    CRPC_CHECK_FALSE(a.running());
}

CRPC_TEST_MAIN()
