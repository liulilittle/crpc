// CRPC -- ordered-lane scheduler unit tests (requirement 11).
//
// `rpc::lane_queue` is the storage and ordering half of ordered lanes; these
// cases pin its state machine directly: in-order execution, out-of-order
// buffering, the bounded gap wait and its skip, duplicate rejection, lane
// independence, teardown release, and the "an empty lane releases its entry"
// invariant. The E2E behaviour (what the server does with these verdicts) is
// covered in test_rpc_lanes.cpp.
#include "test_framework.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <crpc/rpc/lane_queue.hpp>

using crpc::rpc::lane_queue;
using namespace std::chrono_literals;

namespace {

// Manual clock + timer bridge. The scheduler arms a gap timer through
// `arm`; the test fires it by calling `skip_gap` exactly as the server's
// timer callback would, which keeps the lifetime contract (the timer never
// points into the scheduler) exercised without a real wheel.
struct timer_bridge {
    struct armed_timer {
        std::uint64_t lane_id = 0;
        std::uint64_t expected_seq = 0;
        std::uint64_t deadline_ns = 0;
    };

    std::vector<armed_timer> armed;
    std::vector<std::uint64_t> cancelled;
    std::uint64_t now = 1'000'000'000ull; // arbitrary non-zero monotonic ns

    static std::uint64_t now_hook(void* ctx) noexcept {
        return static_cast<timer_bridge*>(ctx)->now;
    }

    static bool arm_hook(void* ctx, std::uint64_t lane_id, std::uint64_t expected_seq,
                         std::uint64_t deadline_ns) noexcept {
        auto* self = static_cast<timer_bridge*>(ctx);
        self->armed.push_back(armed_timer{lane_id, expected_seq, deadline_ns});
        return true;
    }

    static void cancel_hook(void* ctx, std::uint64_t lane_id) noexcept {
        static_cast<timer_bridge*>(ctx)->cancelled.push_back(lane_id);
    }

    lane_queue::options options(std::chrono::nanoseconds gap_timeout) const {
        lane_queue::options opts;
        opts.gap_timeout = gap_timeout;
        opts.now = &now_hook;
        opts.now_ctx = const_cast<timer_bridge*>(this);
        opts.arm = &arm_hook;
        opts.cancel = &cancel_hook;
        opts.timer_ctx = const_cast<timer_bridge*>(this);
        return opts;
    }
};

// A queued payload whose `drop` records the release, mirroring what the
// server's `drop_lane_item` observes.
struct recorded_payload {
    std::uint64_t value = 0;
    std::vector<std::uint64_t>* dropped = nullptr;
};

void drop_payload(void* raw) noexcept {
    auto* payload = static_cast<recorded_payload*>(raw);
    if (payload->dropped != nullptr) {
        payload->dropped->push_back(payload->value);
    }
    delete payload;
}


lane_queue::item item_for(std::uint64_t seq, std::vector<std::uint64_t>* dropped) {
    lane_queue::item work;
    work.seq = seq;
    work.payload = new recorded_payload{seq, dropped};
    work.drop = &drop_payload;
    return work;
}

// Admits one work item and releases the payload the scheduler declined:
// `run_now` hands the item straight back to the caller and `duplicate`
// never transfers ownership, so the test deletes the payload in both
// cases; only a `buffered` verdict leaves the scheduler owning it (the
// item is then released by a pop or by `fail_all`).
lane_queue::admission admit_one(lane_queue& queue, std::uint64_t lane_id,
                                std::uint64_t seq,
                                std::vector<std::uint64_t>* dropped) {
    lane_queue::item work = item_for(seq, dropped);
    const lane_queue::admission verdict = queue.admit(lane_id, work);
    if (verdict != lane_queue::admission::buffered) {
        delete static_cast<recorded_payload*>(work.payload);
    }
    return verdict;
}


// Fires the newest armed timer for `lane_id` through the scheduler's own
// validation path and reports whether a gap was skipped.
bool fire_timer(lane_queue& queue, timer_bridge& timers, std::uint64_t lane_id) {
    for (auto it = timers.armed.rbegin(); it != timers.armed.rend(); ++it) {
        if (it->lane_id == lane_id) {
            return queue.skip_gap(lane_id, it->expected_seq);
        }
    }
    return false;
}

} // namespace

// An in-order stream runs immediately when the lane is idle and otherwise
// parks; the lane entry is released once everything has run.
CRPC_TEST(lane_queue_in_order_runs_and_releases) {
    timer_bridge timers;
    auto queue = std::make_shared<lane_queue>();
    queue->set_options(timers.options(50ms));
    std::vector<std::uint64_t> dropped;

    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::run_now),
                  static_cast<int>(admit_one(*queue, 7, 0, &dropped)));
    // The lane is running: the next two park behind their predecessor.
    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::buffered),
                  static_cast<int>(admit_one(*queue, 7, 1, &dropped)));
    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::buffered),
                  static_cast<int>(admit_one(*queue, 7, 2, &dropped)));
    CRPC_CHECK_EQ(1u, queue->active_lanes());
    CRPC_CHECK_EQ(2u, queue->buffered_calls());
    CRPC_CHECK(timers.armed.empty()); // in-order arrivals never arm a gap timer

    for (int expected = 1; expected <= 2; ++expected) {
        std::optional<lane_queue::item> next = queue->pop_runnable(7);
        CRPC_CHECK(next.has_value());
        if (next.has_value()) {
            CRPC_CHECK_EQ(static_cast<std::uint64_t>(expected), next->seq);
            delete static_cast<recorded_payload*>(next->payload);
        }
    }
    CRPC_CHECK_FALSE(queue->pop_runnable(7).has_value());
    // The drained lane released its entry and nothing was dropped.
    CRPC_CHECK_EQ(0u, queue->active_lanes());
    CRPC_CHECK_EQ(0u, queue->buffered_calls());
    CRPC_CHECK(dropped.empty());
}

// seq=2 arriving before seq=1 is buffered; when the missing call arrives the
// lane resumes in order without any timer involvement. `pop_runnable` both
// completes the previously running call and hands out the next one, so the
// first pop below is seq 0's completion.
CRPC_TEST(lane_queue_out_of_order_buffers_until_filled) {
    timer_bridge timers;
    auto queue = std::make_shared<lane_queue>();
    queue->set_options(timers.options(50ms));
    std::vector<std::uint64_t> dropped;

    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::run_now),
                  static_cast<int>(admit_one(*queue, 3, 0, &dropped)));
    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::buffered),
                  static_cast<int>(admit_one(*queue, 3, 2, &dropped)));
    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::buffered),
                  static_cast<int>(admit_one(*queue, 3, 1, &dropped)));
    CRPC_CHECK_EQ(1u, queue->active_lanes());
    CRPC_CHECK_EQ(2u, queue->buffered_calls());
    CRPC_CHECK(timers.armed.empty()); // the gap never blocked: seq 1 arrived

    // seq 0's caller finished; the handout is seq 1, which was claimed on
    // arrival (it equalled the expected value) and therefore precedes the
    // gapped seq 2 even though seq 2 arrived first.
    std::optional<lane_queue::item> next = queue->pop_runnable(3);
    CRPC_CHECK(next.has_value());
    if (next.has_value()) {
        CRPC_CHECK_EQ(1u, next->seq);
        delete static_cast<recorded_payload*>(next->payload);
    }
    next = queue->pop_runnable(3);
    CRPC_CHECK(next.has_value());
    if (next.has_value()) {
        CRPC_CHECK_EQ(2u, next->seq);
        delete static_cast<recorded_payload*>(next->payload);
    }
    // seq 2's caller finished: the drain's final pop finds the lane empty and
    // releases its entry.
    CRPC_CHECK_FALSE(queue->pop_runnable(3).has_value());
    CRPC_CHECK_EQ(0u, queue->active_lanes());
    CRPC_CHECK(dropped.empty());
}

// A missing sequence blocks the lane for the bounded wait; when the timer
// fires the scheduler skips the gap and execution continues from the earliest
// buffered call.
CRPC_TEST(lane_queue_gap_timeout_skips_missing) {
    timer_bridge timers;
    auto queue = std::make_shared<lane_queue>();
    queue->set_options(timers.options(250ms));
    std::vector<std::uint64_t> dropped;

    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::run_now),
                  static_cast<int>(admit_one(*queue, 4, 0, &dropped)));
    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::buffered),
                  static_cast<int>(admit_one(*queue, 4, 2, &dropped)));
    // seq 1 never arrives. The drain observes the gap and arms the timer.
    CRPC_CHECK_FALSE(queue->pop_runnable(4).has_value());
    CRPC_CHECK_EQ(1u, timers.armed.size());
    CRPC_CHECK_EQ(4u, timers.armed.front().lane_id);
    CRPC_CHECK_EQ(1u, timers.armed.front().expected_seq);
    CRPC_CHECK_EQ(timers.now + 250'000'000ull, timers.armed.front().deadline_ns);

    // The timer path: the same gap, so the skip happens.
    CRPC_CHECK(fire_timer(*queue, timers, 4));
    std::optional<lane_queue::item> next = queue->pop_runnable(4);
    CRPC_CHECK(next.has_value());
    if (next.has_value()) {
        CRPC_CHECK_EQ(2u, next->seq);
        delete static_cast<recorded_payload*>(next->payload);
    }
    CRPC_CHECK_FALSE(queue->pop_runnable(4).has_value());
    CRPC_CHECK_EQ(0u, queue->active_lanes());
}

// A sequence that arrives after its position was skipped is a duplicate: the
// lane already gave up on that position and executing it would double-apply
// the side effect.
CRPC_TEST(lane_queue_late_seq_after_skip_is_duplicate) {
    timer_bridge timers;
    auto queue = std::make_shared<lane_queue>();
    queue->set_options(timers.options(50ms));
    std::vector<std::uint64_t> dropped;

    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::run_now),
                  static_cast<int>(admit_one(*queue, 5, 0, &dropped)));
    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::buffered),
                  static_cast<int>(admit_one(*queue, 5, 2, &dropped)));
    CRPC_CHECK_FALSE(queue->pop_runnable(5).has_value());
    CRPC_CHECK(fire_timer(*queue, timers, 5));

    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::duplicate),
                  static_cast<int>(admit_one(*queue, 5, 1, &dropped)));
    // The duplicate did not move the expected value and nothing was dropped.
    CRPC_CHECK(dropped.empty());
    std::optional<lane_queue::item> next = queue->pop_runnable(5);
    CRPC_CHECK(next.has_value());
    if (next.has_value()) {
        CRPC_CHECK_EQ(2u, next->seq);
        delete static_cast<recorded_payload*>(next->payload);
    }
}

// A replay of a consumed position -- and of a still-parked one -- is rejected
// without disturbing the queue. The first frame runs only when it carries the
// expected seq 0.
CRPC_TEST(lane_queue_duplicate_rejected) {
    timer_bridge timers;
    auto queue = std::make_shared<lane_queue>();
    queue->set_options(timers.options(50ms));
    std::vector<std::uint64_t> dropped;

    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::run_now),
                  static_cast<int>(admit_one(*queue, 6, 0, &dropped)));
    // Consumed position: refused.
    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::duplicate),
                  static_cast<int>(admit_one(*queue, 6, 0, &dropped)));
    // Next position arrives while the lane is busy: parked (claimed).
    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::buffered),
                  static_cast<int>(admit_one(*queue, 6, 1, &dropped)));
    // A re-send of the parked position: refused, scheduler keeps one copy.
    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::duplicate),
                  static_cast<int>(admit_one(*queue, 6, 1, &dropped)));
    CRPC_CHECK_EQ(1u, queue->active_lanes());
    CRPC_CHECK_EQ(1u, queue->buffered_calls());
    CRPC_CHECK(queue->is_parked(6, 1));
    std::optional<lane_queue::item> next = queue->pop_runnable(6);
    CRPC_CHECK(next.has_value());
    if (next.has_value()) {
        CRPC_CHECK_EQ(1u, next->seq);
        delete static_cast<recorded_payload*>(next->payload);
    }
    CRPC_CHECK_FALSE(queue->pop_runnable(6).has_value());
    // The queue entry is released, but the consumed positions are remembered:
    // a replay of a run position after the quiet moment is still a duplicate,
    // and the next fresh call continues above the watermark.
    CRPC_CHECK_EQ(0u, queue->active_lanes());
    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::duplicate),
                  static_cast<int>(admit_one(*queue, 6, 0, &dropped)));
    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::run_now),
                  static_cast<int>(admit_one(*queue, 6, 2, &dropped)));
    CRPC_CHECK_FALSE(queue->pop_runnable(6).has_value());
    CRPC_CHECK_EQ(0u, queue->active_lanes());
    CRPC_CHECK(dropped.empty());
}

// Teardown releases every buffered call through `drop` and clears the state.
CRPC_TEST(lane_queue_fail_all_drops_buffered) {
    timer_bridge timers;
    auto queue = std::make_shared<lane_queue>();
    queue->set_options(timers.options(50ms));
    std::vector<std::uint64_t> dropped;

    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::run_now),
                  static_cast<int>(admit_one(*queue, 1, 0, &dropped)));
    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::buffered),
                  static_cast<int>(admit_one(*queue, 1, 1, &dropped)));
    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::buffered),
                  static_cast<int>(admit_one(*queue, 1, 2, &dropped)));
    CRPC_CHECK_EQ(1u, queue->active_lanes());
    CRPC_CHECK_EQ(2u, queue->buffered_calls());

    CRPC_CHECK_EQ(2u, queue->fail_all());
    CRPC_CHECK_EQ(0u, queue->active_lanes());
    CRPC_CHECK_EQ(0u, queue->buffered_calls());
    CRPC_CHECK_EQ(2u, dropped.size());
    if (dropped.size() == 2) {
        CRPC_CHECK_EQ(1u, dropped[0]);
        CRPC_CHECK_EQ(2u, dropped[1]);
    }
    // A drained scheduler accepts new lanes again.
    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::run_now),
                  static_cast<int>(admit_one(*queue, 1, 0, &dropped)));
}

// Different lanes never wait on each other: a blocked lane A does not stop
// lane B from running, and both entries release independently. Each pop both
// completes the previously running call and hands out the next; the final pop
// of a lane finds it empty and releases its entry.
CRPC_TEST(lane_queue_lanes_are_independent) {
    timer_bridge timers;
    auto queue = std::make_shared<lane_queue>();
    queue->set_options(timers.options(50ms));
    std::vector<std::uint64_t> dropped;

    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::run_now),
                  static_cast<int>(admit_one(*queue, 10, 0, &dropped)));
    // Lane 11 is a different serial domain: it runs even though lane 10 has a
    // call in flight.
    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::run_now),
                  static_cast<int>(admit_one(*queue, 11, 0, &dropped)));
    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::buffered),
                  static_cast<int>(admit_one(*queue, 10, 1, &dropped)));
    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::buffered),
                  static_cast<int>(admit_one(*queue, 11, 1, &dropped)));
    CRPC_CHECK_EQ(2u, queue->active_lanes());
    CRPC_CHECK_EQ(2u, queue->buffered_calls());

    // B's work is runnable even though A never finished.
    std::optional<lane_queue::item> next = queue->pop_runnable(11);
    CRPC_CHECK(next.has_value());
    if (next.has_value()) {
        CRPC_CHECK_EQ(1u, next->seq);
        delete static_cast<recorded_payload*>(next->payload);
    }
    // B's seq 1 finished: the drain's final pop releases lane 11's entry.
    CRPC_CHECK_FALSE(queue->pop_runnable(11).has_value());
    CRPC_CHECK_EQ(1u, queue->active_lanes()); // lane 11 released, lane 10 alive

    next = queue->pop_runnable(10);
    CRPC_CHECK(next.has_value());
    if (next.has_value()) {
        CRPC_CHECK_EQ(1u, next->seq);
        delete static_cast<recorded_payload*>(next->payload);
    }
    CRPC_CHECK_FALSE(queue->pop_runnable(10).has_value());
    CRPC_CHECK_EQ(0u, queue->active_lanes());
    CRPC_CHECK(dropped.empty());
}

// When the missing call arrives in time it runs immediately (the lane is idle
// and the arrival precedes everything buffered), the armed gap timer becomes
// stale, and the next drain cancels it; a fire on the filled gap is a no-op.
CRPC_TEST(lane_queue_gap_timer_cancelled_when_gap_fills) {
    timer_bridge timers;
    auto queue = std::make_shared<lane_queue>();
    queue->set_options(timers.options(5s));
    std::vector<std::uint64_t> dropped;

    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::run_now),
                  static_cast<int>(admit_one(*queue, 8, 0, &dropped)));
    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::buffered),
                  static_cast<int>(admit_one(*queue, 8, 2, &dropped)));
    CRPC_CHECK_FALSE(queue->pop_runnable(8).has_value());
    CRPC_CHECK_EQ(1u, timers.armed.size());

    // The missing seq arrives while the lane is idle: it is next, everything
    // buffered sits above it, so it runs now.
    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::run_now),
                  static_cast<int>(admit_one(*queue, 8, 1, &dropped)));
    // A timer fired here would re-validate and do nothing (the expected value
    // moved when the arrival claimed its position).
    CRPC_CHECK_FALSE(fire_timer(*queue, timers, 8));

    // The next drain runs the buffered call and cancels the stale timer.
    std::optional<lane_queue::item> next = queue->pop_runnable(8);
    CRPC_CHECK(next.has_value());
    if (next.has_value()) {
        CRPC_CHECK_EQ(2u, next->seq);
        delete static_cast<recorded_payload*>(next->payload);
    }
    CRPC_CHECK_EQ(1u, timers.cancelled.size());
    CRPC_CHECK_EQ(8u, timers.cancelled.front());
    CRPC_CHECK_FALSE(queue->pop_runnable(8).has_value());
    CRPC_CHECK_EQ(0u, queue->active_lanes());
    CRPC_CHECK(dropped.empty());
}

// Every fresh lane expects seq 0. A first frame above 0 is a gap (buffered
// behind the bounded wait, never mistaken for a baseline and never refused),
// and a re-send of a still-gapped position is a duplicate: the scheduler keeps
// the first copy and takes no ownership of the second.
CRPC_TEST(lane_queue_first_seq_above_zero_is_a_gap) {
    timer_bridge timers;
    auto queue = std::make_shared<lane_queue>();
    queue->set_options(timers.options(50ms));
    std::vector<std::uint64_t> dropped;

    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::buffered),
                  static_cast<int>(admit_one(*queue, 9, 41, &dropped)));
    // The scheduler arms the gap timer for expected seq 0.
    CRPC_CHECK_EQ(1u, timers.armed.size());
    CRPC_CHECK_EQ(0u, timers.armed.front().expected_seq);
    // A re-send of the same still-gapped position is refused, and the
    // scheduler keeps only the first payload.
    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::duplicate),
                  static_cast<int>(admit_one(*queue, 9, 41, &dropped)));
    CRPC_CHECK_EQ(1u, queue->buffered_calls());
    // The skip fires on the gap: the buffered call runs, the missing prefix
    // (0..40) is never executed.
    CRPC_CHECK(fire_timer(*queue, timers, 9));
    std::optional<lane_queue::item> next = queue->pop_runnable(9);
    CRPC_CHECK(next.has_value());
    if (next.has_value()) {
        CRPC_CHECK_EQ(41u, next->seq);
        delete static_cast<recorded_payload*>(next->payload);
    }
    CRPC_CHECK_FALSE(queue->pop_runnable(9).has_value());
    CRPC_CHECK_EQ(0u, queue->active_lanes());
    CRPC_CHECK(dropped.empty());
}

// Without timer hooks a gap blocks the lane until the missing call arrives or
// the queue is failed -- never an unrequested skip.
CRPC_TEST(lane_queue_gap_without_timer_hooks_blocks) {
    lane_queue::options opts;
    opts.gap_timeout = 50ms; // hooks stay null
    auto queue = std::make_shared<lane_queue>();
    queue->set_options(opts);
    std::vector<std::uint64_t> dropped;

    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::run_now),
                  static_cast<int>(admit_one(*queue, 1, 0, &dropped)));
    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::buffered),
                  static_cast<int>(admit_one(*queue, 1, 2, &dropped)));
    CRPC_CHECK_FALSE(queue->pop_runnable(1).has_value());
    CRPC_CHECK_EQ(1u, queue->active_lanes());
    CRPC_CHECK_EQ(1u, queue->buffered_calls());

    // Teardown is the only way out for the gapped call.
    CRPC_CHECK_EQ(1u, queue->fail_all());
    CRPC_CHECK_EQ(1u, dropped.size());
    CRPC_CHECK_EQ(0u, queue->active_lanes());
}

// After a skip, the next gap in the same lane is waited on independently: the
// timer re-arms for the new expected value and the second skip fires too.
CRPC_TEST(lane_queue_rearm_after_partial_progress) {
    timer_bridge timers;
    auto queue = std::make_shared<lane_queue>();
    queue->set_options(timers.options(50ms));
    std::vector<std::uint64_t> dropped;

    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::run_now),
                  static_cast<int>(admit_one(*queue, 2, 0, &dropped)));
    CRPC_CHECK_EQ(static_cast<int>(lane_queue::admission::buffered),
                  static_cast<int>(admit_one(*queue, 2, 3, &dropped)));
    CRPC_CHECK_FALSE(queue->pop_runnable(2).has_value());
    CRPC_CHECK_EQ(1u, timers.armed.size());
    CRPC_CHECK_EQ(1u, timers.armed.front().expected_seq);

    CRPC_CHECK(fire_timer(*queue, timers, 2)); // skips 1 and 2, resumes at 3
    std::optional<lane_queue::item> next = queue->pop_runnable(2);
    CRPC_CHECK(next.has_value());
    if (next.has_value()) {
        CRPC_CHECK_EQ(3u, next->seq);
        delete static_cast<recorded_payload*>(next->payload);
    }
    CRPC_CHECK_FALSE(queue->pop_runnable(2).has_value());
    CRPC_CHECK_EQ(0u, queue->active_lanes());
    CRPC_CHECK(dropped.empty());
}

CRPC_TEST_MAIN()
