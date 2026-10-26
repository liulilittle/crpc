// CRPC -- regression: poll() must never finish a slot that a pending handle
// already recycled under it.
//
// The deadline poller collects expired slots and then claims them terminal. The
// claim and the collection must be serialized by `completion_mutex_` taken
// BEFORE `map_mutex_`; with the opposite shape a handle destructor can detach
// (completion), cancel and release the collected slot (map) in the gap, a new
// call can acquire and publish that same slot, and the poll then marks the
// fresh, never-sent call `timed_out`.
//
// This case parks the poller inside its clock callback, destroys the collected
// handle while the poller is parked, and lets a spinner re-acquire the freed
// slot. The injected clock is called from inside the poller's lock scope, so
// the test is deterministic rather than timing-dependent.
#include <crpc/frame.hpp>
#include <crpc/io.hpp>
#include <crpc/rpc/client.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <utility>

#include "test_framework.hpp"

namespace {

using namespace crpc;
using namespace crpc::rpc;
using namespace std::chrono_literals;

struct null_source final : io::source {
    std::size_t on_data(span<const std::byte> data) noexcept override { return data.size(); }
    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {}
};

struct null_connection final : connection {
    struct sink_impl final : io::sink {
        void push(span<const std::byte> data, io::completion* c) noexcept override {
            c->complete(io::io_result{{}, data.size()});
        }
    };

    io::source& source() noexcept override { return source_; }
    io::sink& sink() noexcept override { return sink_; }
    void bind_source(io::source*) noexcept override {}
    void resume_read() noexcept override {}
    void close() noexcept override { open_ = false; }
    [[nodiscard]] bool is_open() const noexcept override { return open_; }
    [[nodiscard]] endpoint remote() const override { return endpoint::inproc("poll-recycle"); }

    null_source source_;
    sink_impl sink_;
    bool open_ = true;
};

struct parking_clock {
    std::atomic<long long> ns{0};
    std::atomic<bool> park_poll{false};
    std::atomic<bool> poll_parked{false};
    std::atomic<bool> release_poll{false};
    std::thread::id poll_thread{};
};

std::chrono::steady_clock::time_point parking_now(void* ctx) noexcept {
    auto* clock = static_cast<parking_clock*>(ctx);
    if (clock->park_poll.load(std::memory_order_acquire) &&
        std::this_thread::get_id() == clock->poll_thread) {
        clock->poll_parked.store(true, std::memory_order_release);
        while (!clock->release_poll.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        clock->park_poll.store(false, std::memory_order_release);
    }
    return std::chrono::steady_clock::time_point{} +
           std::chrono::nanoseconds{clock->ns.load(std::memory_order_acquire)};
}

} // namespace

CRPC_TEST(poll_does_not_finish_a_recycled_slot) {
    parking_clock clock;
    client_options options;
    options.slot_capacity = 1;
    options.clock = &parking_now;
    options.clock_ctx = &clock;
    client c(std::make_unique<null_connection>(), options);

    const span<const std::byte> empty{};

    for (int iteration = 0; iteration < 200; ++iteration) {
        clock.ns.store(0, std::memory_order_release);
        clock.park_poll.store(false, std::memory_order_release);
        clock.poll_parked.store(false, std::memory_order_release);
        clock.release_poll.store(false, std::memory_order_release);

        call_context expired_ctx;
        expired_ctx.has_deadline = true;
        expired_ctx.deadline = std::chrono::steady_clock::time_point{} + 1000ms;
        auto started = c.start_call<int>(1, 1, empty, expired_ctx);
        CRPC_CHECK(started.ok());
        auto expired = std::make_unique<call<int>>(std::move(started).value());

        clock.ns.store(2'000'000'000LL, std::memory_order_release);

        std::atomic<bool> poll_done{false};
        std::atomic<bool> corrupted{false};
        std::atomic<bool> stop_spinner{false};

        // With slot_capacity == 1 every attempt fails while the expired call
        // holds the only slot; the first success after the release wins.
        std::thread spinner([&] {
            while (!stop_spinner.load(std::memory_order_acquire)) {
                auto fresh = c.start_call<int>(1, 1, empty, call_context{});
                if (!fresh.ok()) {
                    if (fresh.error().code == rpc_code::resource_exhausted) {
                        continue;
                    }
                    break;
                }
                auto handle = std::make_unique<call<int>>(std::move(fresh).value());
                while (!poll_done.load(std::memory_order_acquire) &&
                       !stop_spinner.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                if (handle->ready() && handle->state() == call_state::timed_out) {
                    corrupted.store(true, std::memory_order_release);
                }
                handle.reset();
                break;
            }
        });

        clock.park_poll.store(true, std::memory_order_release);
        std::thread poller([&] {
            clock.poll_thread = std::this_thread::get_id();
            c.poll();
            poll_done.store(true, std::memory_order_release);
        });

        while (!clock.poll_parked.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        std::thread destroyer([&] { expired.reset(); });
        std::this_thread::sleep_for(1ms);
        clock.release_poll.store(true, std::memory_order_release);

        destroyer.join();
        poller.join();
        stop_spinner.store(true, std::memory_order_release);
        spinner.join();

        CRPC_CHECK(!corrupted.load(std::memory_order_acquire));
        CRPC_CHECK_EQ(0u, c.in_flight());
        CRPC_CHECK_EQ(0u, c.slots_in_use());
    }
}

// A 	hen callback runs under the completion lock and may legally destroy a
// *sibling* handle (the contract only forbids destroying the handle the callback
// belongs to). Destroying a sibling detaches, cancels and returns its slot to
// the pool, so any code that snapshots raw call_slot* and finishes them later
// can write a terminal state into an already-recycled slot. This case pins that
// shape: two expired calls, the first one's callback destroys the second.
CRPC_TEST(poll_survives_a_callback_destroying_a_sibling) {
    parking_clock clock;
    client_options options;
    options.slot_capacity = 4;
    options.clock = &parking_now;
    options.clock_ctx = &clock;
    client c(std::make_unique<null_connection>(), options);

    const span<const std::byte> empty{};

    for (int iteration = 0; iteration < 200; ++iteration) {
        clock.ns.store(0, std::memory_order_release);
        clock.park_poll.store(false, std::memory_order_release);
        clock.poll_parked.store(false, std::memory_order_release);
        clock.release_poll.store(false, std::memory_order_release);

        call_context expired_ctx;
        expired_ctx.has_deadline = true;
        expired_ctx.deadline = std::chrono::steady_clock::time_point{} + 1000ms;

        // A expires first and B second, so the poll finishes A (running its
        // callback, which destroys B) BEFORE it reaches B's deadline entry.
        auto first = c.start_call<int>(1, 1, empty, expired_ctx);
        CRPC_CHECK(first.ok());
        call_context later_ctx = expired_ctx;
        later_ctx.deadline = std::chrono::steady_clock::time_point{} + 2000ms;
        auto second = c.start_call<int>(1, 1, empty, later_ctx);
        CRPC_CHECK(second.ok());

        auto first_handle = std::make_unique<call<int>>(std::move(first).value());
        auto second_handle = std::make_unique<call<int>>(std::move(second).value());

        // The first callback DESTROYS the sibling handle. The contract only
        // forbids a callback destroying the handle it belongs to, so this is
        // legal: detach + cancel + release return the sibling's slot to the
        // pool while `poll` still holds its completion lock.
        call_slot* const sibling_raw = second_handle->slot();
        const u32 sibling_generation = second_handle->generation();
        auto* holder = &second_handle;
        first_handle->then([holder](call<int>&) { holder->reset(); });

        clock.ns.store(3'000'000'000LL, std::memory_order_release);
        c.poll();

        CRPC_CHECK(first_handle->state() == call_state::timed_out);
        // The sibling was destroyed by the callback, so its slot is back in the
        // pool. `poll` must not have written a terminal result into it after the
        // recycle: the slot is owned by the pool now, and a later call that
        // acquires it would inherit a bogus `timed_out`.
        CRPC_CHECK_EQ(sibling_generation + 1, sibling_raw->generation);
        CRPC_CHECK_FALSE(result_ready(*sibling_raw));
        CRPC_CHECK_EQ(0u, c.in_flight());
        first_handle.reset();
        CRPC_CHECK_EQ(0u, c.slots_in_use());
    }
}

CRPC_TEST_MAIN();
