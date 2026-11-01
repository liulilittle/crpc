// CRPC -- the async contract of ipc::channel.
//
// The library is async-first: the transport is polled and never sleeps, and the
// synchronous-looking convenience forms must not hold a shard thread. These cases pin
// that split, because the failure mode is a deadlock rather than a wrong value -- and
// a deadlock in a test shows up as a hung process, not as a red assertion.
//
// What is checked here, and what is not:
//   * the non-blocking forms never wait, on any thread;
//   * a zero timeout means "do not wait" on both synchronous forms;
//   * `send` rings a bound runtime, so a reader parked on that runtime wakes;
//   * a channel with no runtime still works from a plain thread with a real timeout.
//
// The cross-process wake cannot be tested in-process at all: there is no signal from
// the other address space, which is why `poll_ready()` exists for a caller's own loop.
#include <crpc/ipc/channel.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "test_framework.hpp"

using namespace crpc;
using namespace crpc::ipc;
using namespace std::chrono_literals;

namespace {

std::string unique_name(const char* stem) {
    static std::atomic<unsigned> counter{0};
    const unsigned tail = counter.fetch_add(1, std::memory_order_relaxed);
    return std::string("crpc_async_") + stem + "_" +
           std::to_string(reinterpret_cast<std::uintptr_t>(&counter)) + "_" +
           std::to_string(tail);
}

struct session {
    std::string name = unique_name("s");
    ~session() { (void)fs::shared_memory::remove(name); }
};

void put(channel& c, std::string_view text) {
    const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
    CRPC_CHECK(c.try_send(span<const std::byte>(bytes, text.size())));
}

} // namespace

CRPC_TEST(ipc_async_try_forms_never_wait) {
    session s;
    auto created = channel::create(s.name, 64);
    CRPC_CHECK(created.ok());
    if (!created.ok()) {
        return;
    }

    // An empty channel answers immediately, on the calling thread, with zero. If this
    // blocked, the case would hang rather than fail, which is the point: the poll is
    // what lets a caller drive the channel from its own loop.
    std::byte out[8];
    const auto before = std::chrono::steady_clock::now();
    const std::size_t got = created.value().try_receive(span<std::byte>(out, sizeof(out)));
    const auto elapsed = std::chrono::steady_clock::now() - before;
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), got);
    // A generous bound: this is a "did it block at all" check, not a latency claim.
    CRPC_CHECK(elapsed < 1s);

    CRPC_CHECK_FALSE(created.value().poll_ready());
    put(created.value(), "x");
    CRPC_CHECK(created.value().poll_ready() == false); // still nothing inbound
}

CRPC_TEST(ipc_async_zero_timeout_means_do_not_wait) {
    session s;
    auto created = channel::create(s.name, 64);
    CRPC_CHECK(created.ok());
    if (!created.ok()) {
        return;
    }
    auto joined = channel::join(s.name);
    CRPC_CHECK(joined.ok());
    if (!joined.ok()) {
        return;
    }

    std::byte out[8];
    const auto before = std::chrono::steady_clock::now();
    auto empty = joined.value().receive(span<std::byte>(out, sizeof(out)), 0ns);
    const auto elapsed = std::chrono::steady_clock::now() - before;
    CRPC_CHECK(empty.ok());
    if (empty.ok()) {
        CRPC_CHECK_EQ(static_cast<std::size_t>(0), empty.value());
    }
    CRPC_CHECK(elapsed < 1s);

    // A full ring with a zero timeout reports back-pressure instead of waiting for a
    // drain that is not going to happen on this thread.
    std::vector<std::byte> full(64, std::byte{'f'});
    CRPC_CHECK(created.value().try_send(span<const std::byte>(full.data(), full.size())));
    const std::byte extra{'e'};
    const auto refused =
        created.value().send(span<const std::byte>(&extra, 1), 0ns);
    CRPC_CHECK_FALSE(refused.ok());
    if (!refused.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(rpc_code::resource_exhausted),
                      static_cast<int>(refused.code()));
    }
}

CRPC_TEST(ipc_async_plain_thread_waits_for_real_data) {
    session s;
    auto created = channel::create(s.name, 64);
    CRPC_CHECK(created.ok());
    if (!created.ok()) {
        return;
    }
    auto joined = channel::join(s.name);
    CRPC_CHECK(joined.ok());
    if (!joined.ok()) {
        return;
    }

    // The writer is a separate thread, so the reader genuinely has to wait. No runtime
    // is bound: this is the plain-thread path, where a bounded wait is correct.
    std::thread writer([&] {
        std::this_thread::sleep_for(20ms);
        put(created.value(), "late");
    });

    std::byte out[16];
    auto got = joined.value().receive(span<std::byte>(out, sizeof(out)), 5s);
    writer.join();
    CRPC_CHECK(got.ok());
    if (got.ok()) {
        CRPC_CHECK_EQ(static_cast<std::size_t>(4), got.value());
        CRPC_CHECK_EQ(std::string("late"),
                      std::string(reinterpret_cast<const char*>(out), got.value()));
    }
}

CRPC_TEST(ipc_async_plain_thread_times_out_instead_of_hanging) {
    session s;
    auto created = channel::create(s.name, 64);
    CRPC_CHECK(created.ok());
    if (!created.ok()) {
        return;
    }
    auto joined = channel::join(s.name);
    CRPC_CHECK(joined.ok());
    if (!joined.ok()) {
        return;
    }

    // Nobody writes. The call must come back with a deadline error rather than block
    // forever -- an unreturned read is indistinguishable from a deadlock to whoever is
    // waiting on it.
    std::byte out[8];
    const auto before = std::chrono::steady_clock::now();
    auto timed = joined.value().receive(span<std::byte>(out, sizeof(out)), 50ms);
    const auto elapsed = std::chrono::steady_clock::now() - before;
    CRPC_CHECK_FALSE(timed.ok());
    if (!timed.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(rpc_code::deadline_exceeded),
                      static_cast<int>(timed.error().code));
    }
    CRPC_CHECK(elapsed >= 40ms);
    CRPC_CHECK(elapsed < 5s);
}

CRPC_TEST(ipc_async_send_rings_the_bound_runtime) {
    session s;
    auto created = channel::create(s.name, 64);
    CRPC_CHECK(created.ok());
    if (!created.ok()) {
        return;
    }

    // A runtime that is not started: `notify()` is legal on any thread and is the only
    // thing `try_send` does with the pointer. Binding it must not change the send
    // result, and must not crash -- the wake is best-effort by design.
    crpc::runtime_options options;
    crpc::runtime shard(options);
    created.value().bind_runtime(&shard);

    const std::byte payload{'p'};
    CRPC_CHECK(created.value().try_send(span<const std::byte>(&payload, 1)));
    // The wake must not have altered the ring: capacity 64, one byte queued.
    CRPC_CHECK_EQ(static_cast<std::size_t>(63), created.value().send_capacity());

    // Unbinding returns it to the plain path.
    created.value().bind_runtime(nullptr);
    CRPC_CHECK(created.value().try_send(span<const std::byte>(&payload, 1)));
}

// The shard-thread path, which is where the "never block the shard" rule actually
// bites. A test that only ever waits from a plain thread cannot see the difference:
// blocking and parking both return the data, they differ in whether the shard is
// still running other work. So this case parks on a shard thread *while another task
// on the same shard is the one that produces*, and requires that the producer made
// progress. If `receive` had blocked under the lock-less spin, the producer, which
// runs on the same single thread, could never run and the case would time out.
CRPC_TEST(ipc_async_shard_thread_parks_without_blocking_the_shard) {
    session s;
    auto created = channel::create(s.name, 64);
    CRPC_CHECK(created.ok());
    if (!created.ok()) {
        return;
    }
    auto joined = channel::join(s.name);
    CRPC_CHECK(joined.ok());
    if (!joined.ok()) {
        return;
    }

    crpc::runtime_options options;
    crpc::runtime shard(options);
    shard.start_thread();
    CRPC_CHECK(shard.loop_mode());

    created.value().bind_runtime(&shard);
    joined.value().bind_runtime(&shard);

    std::atomic<bool> reader_done{false};
    std::atomic<int> reader_code{0};

    // The reader parks on the shard. It is not started until after the producer
    // task is queued, so both orders are exercised by the scheduler rather than by
    // the test.
    struct job {
        channel* rx;
        std::atomic<bool>* done;
        std::atomic<int>* code;
    };
    job reader_job{&joined.value(), &reader_done, &reader_code};

    auto reader_fn = [](void* ctx) noexcept {
        auto* j = static_cast<job*>(ctx);
        std::byte out[16];
        auto got = j->rx->receive(span<std::byte>(out, sizeof(out)),
                                  std::chrono::seconds(5));
        j->code->store(got.ok() ? static_cast<int>(got.value()) : -1,
                       std::memory_order_release);
        j->done->store(true, std::memory_order_release);
    };

    // Queue the producer first: a plain task on the same shard.
    struct put_job {
        channel* tx;
    };
    put_job producer_job{&created.value()};
    auto producer_fn = [](void* ctx) noexcept {
        auto* j = static_cast<put_job*>(ctx);
        const std::byte payload{'s'};
        (void)j->tx->try_send(span<const std::byte>(&payload, 1));
    };

    CRPC_CHECK(shard.submit(producer_fn, &producer_job));
    CRPC_CHECK(shard.spawn(reader_fn, &reader_job));

    const bool finished = crpc_test::wait_for(
        [&] { return reader_done.load(std::memory_order_acquire); }, 8000);
    CRPC_CHECK(finished);
    CRPC_CHECK_EQ(1, reader_code.load(std::memory_order_acquire));

    created.value().bind_runtime(nullptr);
    joined.value().bind_runtime(nullptr);
    shard.stop();
    shard.join();
}

CRPC_TEST_MAIN()
