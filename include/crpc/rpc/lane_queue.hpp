// CRPC -- ordered-lane execution scheduler (requirement 11).
//
// One atomic serial queue per linear target: calls that opt into ordered
// delivery by carrying a `lane_token` are executed strictly in their per-lane
// `seq` order, at most one at a time, while different lanes never wait on each
// other. The scheduler is the storage and ordering half; the caller (the
// server's request path) stays in control of execution:
//
//   * `admit` inspects one arriving call and either hands it back for
//     immediate execution (`run_now`), keeps it buffered until its turn
//     (`buffered`), or rejects it as a stale replay of an already-consumed
//     position (`duplicate`).
//   * after a call finishes, the caller pops the next runnable item with
//     `pop_runnable` and executes it; the loop ends when the lane is empty or
//     blocked behind a gap.
//   * a gap -- a buffered call whose `seq` is above the next expected value --
//     is reported to the caller through the injected timer hooks; when the
//     armed timer fires, the caller asks `skip_gap` to give up on the missing
//     sequences and then resumes draining.
//
// The scheduler never runs user code and never blocks: `admit` and
// `pop_runnable` return immediately, so the caller's thread (the server's
// shard) is never parked inside a waiting call. Buffered work is released,
// never executed, by `fail_all`.
//
// Semantics (normative for the server wiring in src/server/server.cpp)
// -------------------------------------------------------------------
//   * Scope. State is per scheduler instance. The server creates one scheduler
//     per connection and keys it by the client-assigned `lane_id`, so the
//     serial domain is one `(connection, lane_id)` pair. A fresh connection
//     starts with no lane state, and every fresh lane expects `seq == 0`
//     (the client facade allocates per-lane sequences from 0). A first frame
//     above 0 is a gap: it is buffered and the bounded wait below applies,
//     which is also what keeps a multiplexing client safe when its very first
//     two frames race -- the lower sequence is buffered, never mistaken for a
//     baseline or refused as a duplicate. A client that resumes a lane with a
//     higher counter on a new connection therefore pays one gap wait before
//     the lane proceeds.
//   * Order. Within a lane, execution is strictly ascending by `seq` and at
//     most one call runs at a time. Positions are claimed on arrival: a call
//     whose `seq` equals the expected value consumes that position immediately,
//     whether it executes at once or is buffered behind its predecessor.
//   * Duplicates. A call whose `seq` is below the expected value arrives too
//     late -- its position was already claimed, and executing it would repeat a
//     side effect inside a lane that promises exactly-once ordering. The
//     scheduler answers `duplicate`; the server rejects the call with
//     `failed_precondition` without running its handler. A duplicate never
//     moves the expected value. The consumed positions outlive the queue: when
//     an idle lane releases its entry, the scheduler keeps a one-word
//     watermark per lane id, and a call that arrives later below that
//     watermark is still a duplicate. Without the watermark, a replayed
//     sequence after a quiet moment would be mistaken for the first frame of
//     a fresh lane and executed twice. The watermark is the ordering memory of
//     the connection and lives until the scheduler is failed or destroyed, so
//     a client must not reset its per-lane counter on a live connection; the
//     watermark itself is eight bytes per distinct lane, while the queued
//     work -- the part that scales with load -- is fully released.
//   * Gaps. A call whose `seq` is above the expected value is buffered and the
//     missing positions below it are waited on for a bounded time
//     (`options::gap_timeout`, measured from the first observation of the
//     gap). If the missing call arrives in time, ordering resumes normally.
//     When the timer fires on a still-unfilled gap, every sequence below the
//     earliest buffered call is treated as skipped: the skipped calls never
//     arrived and are never executed, and execution continues from the earliest
//     buffered call. A sequence that arrives after its position was skipped is
//     answered as a duplicate. The timer hooks are optional: without them a gap
//     blocks the lane until the missing call arrives or the lane is failed.
    //   * Buffered-byte caps. A call that would push the summed `frame_size`
    //     of its lane's buffered calls over `options::lane_max_buffered_bytes`
    //     (default 16 KiB, the design budget for one lane), or the scheduler's
    //     buffered total over `options::max_buffered_bytes` (default 64 KiB,
    //     the connection-level bound), is answered `rejected` and the server
    //     refuses it with `rpc_code::overload`. The refusal consumes no
    //     position and evicts nothing: the queue, the sequence state and the
    //     byte accounting are exactly as they were, so a retry after a drain
    //     is possible. A call that runs immediately is never buffered and is
    //     never refused by these caps.
    //   * Failures. A call that fails -- handler error, deadline, decode error, a
    //     local admission refusal -- has still consumed its position; the lane
    //     continues with the next sequence. Only connection loss or scheduler
    //     teardown ends a lane outright, and then every buffered call is dropped
    //     with `fail_all` (the server answers them with an explicit error where
    //     the transport still allows one).
//   * `lane_id == 0`. The scheduler itself keys lanes opaquely; the server
//     never admits `lane_id == 0` (and a frame without the ORDERED extension
//     carries no token at all). Both keep the pre-lane behaviour: no ordering,
//     no queueing, no state.
//
// Thread safety: every method takes the internal mutex, so one scheduler may
// be shared by every thread that touches the connection's request path. The
// intended shape is narrower -- admissions and drains run on the owning
// connection's shard thread, while `fail_all` may be called from the reader or
// control thread during teardown -- but the mutex makes any shape correct.
//
// This class is deliberately header-only and carries no `CRPC_API`: it is an
// internal building block of the server request path (the same standing as
// `rpc::stream_map` and `rpc::deadline_map`), so its layout is not part of the
// binary interface documented in docs/abi.md.
#ifndef CRPC_RPC_LANE_QUEUE_HPP
#define CRPC_RPC_LANE_QUEUE_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

#include <crpc/config.hpp>

namespace crpc::rpc {

class lane_queue {
public:
    // Monotonic nanosecond clock. Injected so a test can drive time; when
    // null the scheduler reads time as 0, which only degrades the gap
    // deadline (it fires immediately and the lane skips forward).
    using clock_fn = std::uint64_t (*)(void* ctx) noexcept;

    // Gap-timer hooks. `arm` schedules one callback for the gap in front of
    // `expected_seq` on `lane_id`, to fire at the absolute `deadline_ns`; the
    // implementation owns the callback payload. `cancel` removes a pending
    // timer for `lane_id`. At most one gap timer per lane is outstanding at
    // any time. Both hooks receive `timer_ctx`. Either hook may be null,
    // which disables gap skipping for lanes admitted through this scheduler.
    using arm_fn = bool (*)(void* ctx, std::uint64_t lane_id,
                            std::uint64_t expected_seq,
                            std::uint64_t deadline_ns) noexcept;
    using cancel_fn = void (*)(void* ctx, std::uint64_t lane_id) noexcept;

    struct options {
        // How long a gap blocks the lane before the missing sequences are
        // skipped. Zero skips on the first observation of the gap.
        std::chrono::nanoseconds gap_timeout{std::chrono::seconds(5)};
        // Byte caps on buffered work. A call that would push its lane over
        // `lane_max_buffered_bytes`, or the scheduler's buffered total over
        // `max_buffered_bytes`, is refused with `admission::rejected`
        // (answered with `rpc_code::overload` by the server); nothing already
        // buffered is ever evicted. The per-lane default is the design budget
        // for one lane (docs/design.md §17.1); the 64 KiB total bounds how
        // the per-lane cap multiplies across lanes (four full lanes) and
        // follows the design's fast-failure policy. Zero admits only
        // zero-byte frames.
        std::uint64_t lane_max_buffered_bytes = 16 * 1024;
        std::uint64_t max_buffered_bytes = 64 * 1024;
        clock_fn now = nullptr;
        void* now_ctx = nullptr;
        arm_fn arm = nullptr;
        cancel_fn cancel = nullptr;
        void* timer_ctx = nullptr;
    };

    // One queued unit of work. The scheduler never inspects `payload`; `drop`
    // releases it without executing (lane teardown, connection close,
    // shutdown). Ownership of `payload` transfers to the scheduler on
    // `buffered` and back to the caller on `pop_runnable`. `frame_size` is
    // the exact encoded size of the arriving frame
    // (`rpc::client::predicted_frame_size` at the call site); it feeds the
    // buffered-byte admission caps and is charged only while the call is
    // buffered.
    struct item {
        std::uint64_t seq = 0;
        std::uint64_t frame_size = 0;
        void* payload = nullptr;
        void (*drop)(void* payload) noexcept = nullptr;
    };

    // What `admit` decided for one arriving call.
    enum class admission : std::uint8_t {
        // The lane is idle and this call is next: execute it now. The lane is
        // marked running until the caller pops again.
        run_now,
        // The scheduler owns the call until its turn comes; retrieve it with
        // `pop_runnable`. A gapped lane may also report `buffered` for a call
        // that is strictly ahead of a missing sequence.
        buffered,
        // The sequence was already consumed (executed, buffered or skipped).
        // The scheduler took no ownership; the caller rejects the call.
        duplicate,
        // The call would push the buffered frame bytes of its lane -- or of
        // the whole scheduler -- over the configured cap
        // (`options::lane_max_buffered_bytes` / `options::max_buffered_bytes`).
        // Nothing was consumed: the position is untouched and the scheduler
        // took no ownership, so a retry with a drain in between is possible.
        // The caller must not treat this as a duplicate; the server answers
        // `rpc_code::overload` and the client owns the retry.
        rejected
    };

    lane_queue() noexcept = default;
    ~lane_queue() { (void)fail_all(); }

    lane_queue(const lane_queue&) = delete;
    lane_queue& operator=(const lane_queue&) = delete;

    // Installs the options. Must be called before the first `admit`; the
    // server sets it once per connection, before traffic. Options are copied.
    void set_options(const options& value) noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        opt_ = value;
    }

    // Admits one arriving call for `lane_id`. See `admission` for the outcomes
    // and the class comment for the semantics behind each one.
    admission admit(std::uint64_t lane_id, const item& work) {
        std::lock_guard<std::mutex> lk(mutex_);
        lane_state& ls = lane_for(lane_id);
        if (work.seq < ls.next_run) {
            return admission::duplicate;
        }
        const bool claimed = work.seq == ls.next_run;
        const bool idle = !ls.running;
        const bool ahead_of_everything =
            ls.waiting.empty() || ls.waiting.begin()->first > work.seq;
        if (idle && claimed && ahead_of_everything) {
            // The call runs immediately and is never buffered: the byte caps
            // cannot refuse it.
            ls.next_run = work.seq + 1;
            ls.running = true;
            return admission::run_now;
        }
        // The call is about to be parked. Enforce the buffered-byte caps
        // before anything is consumed: a refusal leaves the position, the
        // queue and the byte accounting exactly as they were, so a retry
        // after a drain is possible and nothing already queued is evicted.
        if (ls.buffered_bytes + work.frame_size > opt_.lane_max_buffered_bytes ||
            buffered_total_ + work.frame_size > opt_.max_buffered_bytes) {
            return admission::rejected;
        }
        if (claimed) {
            ls.next_run = work.seq + 1;
        }
        if (ls.waiting.emplace(work.seq, work).second) {
            ls.buffered_bytes += work.frame_size;
            buffered_total_ += work.frame_size;
            const std::uint64_t head = ls.waiting.begin()->first;
            if (head <= ls.next_run) {
                // No blocking gap in front of the queue. An armed gap timer
                // whose gap just filled is stale: cancel it now rather than
                // letting it fire into a re-validated no-op later.
                if (ls.gap_armed) {
                    cancel_gap_locked(ls);
                }
            } else if (idle) {
                maybe_arm_gap(ls);
            }
            return admission::buffered;
        }
        // The position is already parked (a re-send of a still-gapped
        // sequence, for example). The scheduler keeps the first copy and took
        // no ownership of this one; the caller refuses it exactly like a
        // consumed position.
        return admission::duplicate;
    }

    // Marks the lane's running call finished and returns the next runnable
    // item, or nullopt when the lane has nothing runnable: it is empty (the
    // state entry is released), or it is blocked behind a gap (the gap timer
    // is armed and draining resumes from the timer callback). The returned
    // item is removed from the lane; ownership of `payload` is the caller's.
    std::optional<item> pop_runnable(std::uint64_t lane_id) {
        std::lock_guard<std::mutex> lk(mutex_);
        const auto it = lanes_.find(lane_id);
        if (it == lanes_.end()) {
            return std::nullopt;
        }
        lane_state& ls = *it->second;
        ls.running = false;
        if (ls.waiting.empty()) {
            release_locked(it);
            return std::nullopt;
        }
        const std::uint64_t head = ls.waiting.begin()->first;
        if (head <= ls.next_run) {
            // `head < next_run` is a claimed position whose predecessors have
            // all run; `head == next_run` closes a gap that filled in time.
            item work = ls.waiting.begin()->second;
            ls.waiting.erase(ls.waiting.begin());
            // The call leaves the queue: refund its bytes to the lane and to
            // the scheduler total.
            ls.buffered_bytes -= work.frame_size;
            buffered_total_ -= work.frame_size;
            if (head == ls.next_run) {
                ls.next_run = head + 1;
            }
            ls.running = true;
            cancel_gap_locked(ls);
            return item{work};
        }
        maybe_arm_gap(ls);
        return std::nullopt;
    }

    // Gap-timer path: called when the timer armed for `expected_seq` fires.
    // Skips the gap only if it is still the same one -- the missing sequences
    // below the earliest buffered call are given up and the expected value
    // moves to that call. Returns true when something was skipped and the
    // caller must resume draining with `pop_runnable`.
    bool skip_gap(std::uint64_t lane_id, std::uint64_t expected_seq) {
        std::lock_guard<std::mutex> lk(mutex_);
        const auto it = lanes_.find(lane_id);
        if (it == lanes_.end()) {
            return false;
        }
        lane_state& ls = *it->second;
        if (ls.next_run != expected_seq || ls.waiting.empty()) {
            return false;
        }
        const std::uint64_t head = ls.waiting.begin()->first;
        if (head <= ls.next_run) {
            return false;
        }
        ls.next_run = head;
        cancel_gap_locked(ls);
        return true;
    }

    // True when `seq` of `lane_id` is still parked (admitted, not yet run).
    // The server routes a keyed replay with it: a replay of a call that has
    // already finished can ride the dispatcher's dedup table to its recorded
    // outcome, but a replay of a call that is still queued must not execute
    // ahead of it, so it is refused like an unkeyed duplicate.
    [[nodiscard]] bool is_parked(std::uint64_t lane_id, std::uint64_t seq) const {
        std::lock_guard<std::mutex> lk(mutex_);
        const auto it = lanes_.find(lane_id);
        return it != lanes_.end() && it->second->waiting.count(seq) != 0;
    }

    // Releases every buffered call of every lane through its `drop` callback
    // and clears all state. Running calls are not touched: their ownership
    // never entered the scheduler. Returns the number of dropped calls.
    std::size_t fail_all() noexcept {
        std::lock_guard<std::mutex> lk(mutex_);
        std::size_t dropped = 0;
        for (auto& entry : lanes_) {
            lane_state& ls = *entry.second;
            ls.detached = true;
            cancel_gap_locked(ls);
            for (auto& pending : ls.waiting) {
                const item& work = pending.second;
                if (work.drop != nullptr) {
                    work.drop(work.payload);
                }
                ++dropped;
            }
            ls.waiting.clear();
        }
        lanes_.clear();
        consumed_.clear();
        // Every buffered call was released above; no lane state survives.
        buffered_total_ = 0;
        return dropped;
    }

    // Live lane entries. A lane is released as soon as it has no buffered
    // call and nothing running, so this is the observable form of the
    // "an empty lane releases its resources" invariant.
    [[nodiscard]] std::size_t active_lanes() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return lanes_.size();
    }

    // Buffered calls across all lanes (running calls are not counted).
    [[nodiscard]] std::size_t buffered_calls() const {
        std::lock_guard<std::mutex> lk(mutex_);
        std::size_t total = 0;
        for (const auto& entry : lanes_) {
            total += entry.second->waiting.size();
        }
        return total;
    }

    // Buffered frame bytes across all lanes (running calls are not counted).
    // This is the quantity `options::max_buffered_bytes` caps.
    [[nodiscard]] std::uint64_t buffered_bytes() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return buffered_total_;
    }

    // Buffered frame bytes of one lane, or zero when the lane has no entry.
    // This is the quantity `options::lane_max_buffered_bytes` caps.
    [[nodiscard]] std::uint64_t lane_buffered_bytes(std::uint64_t lane_id) const {
        std::lock_guard<std::mutex> lk(mutex_);
        const auto it = lanes_.find(lane_id);
        return it != lanes_.end() ? it->second->buffered_bytes : 0;
    }

private:
    struct lane_state {
        std::uint64_t lane_id = 0;
        // One past the last claimed position. Every fresh lane expects seq 0:
        // the client facade allocates per-lane sequences from 0, so a first
        // frame above 0 is a gap the bounded wait resolves (a reconnecting
        // client that resumes mid-lane pays one gap wait; a client that
        // really lost its first frames gets them skipped, never wedged).
        // Claimed positions have arrived; a buffered `seq < next_run` was
        // claimed on arrival, a buffered `seq > next_run` sits behind a gap.
        std::uint64_t next_run = 0;
        bool running = false;
        // The outstanding gap timer targets `gap_armed_for`; false when none.
        bool gap_armed = false;
        std::uint64_t gap_armed_for = 0;
        // Set under the queue mutex when the entry is released; a timer
        // callback that still holds the lane id validates through `skip_gap`,
        // which finds nothing.
        bool detached = false;
        // Summed `frame_size` of this lane's buffered calls. Charged on
        // admission, refunded on pop; checked against
        // `options::lane_max_buffered_bytes` before a call is parked. The
        // entry is released only when `waiting` is empty, so it is zero
        // there; the release still refunds defensively.
        std::uint64_t buffered_bytes = 0;
        std::map<std::uint64_t, item> waiting;
    };

    using lane_table = std::unordered_map<std::uint64_t, std::unique_ptr<lane_state>>;

    [[nodiscard]] lane_state& lane_for(std::uint64_t lane_id) {
        const auto it = lanes_.find(lane_id);
        if (it != lanes_.end()) {
            return *it->second;
        }
        std::unique_ptr<lane_state> fresh(new lane_state());
        fresh->lane_id = lane_id;
        // A released lane left a watermark of the positions it consumed: the
        // fresh state resumes above it, so a replay below the watermark is
        // still a duplicate instead of a fresh first frame.
        const auto consumed = consumed_.find(lane_id);
        if (consumed != consumed_.end()) {
            fresh->next_run = consumed->second;
        }
        lane_state& ref = *fresh;
        lanes_.emplace(lane_id, std::move(fresh));
        return ref;
    }

    [[nodiscard]] std::uint64_t now_ns() const noexcept {
        return opt_.now != nullptr ? opt_.now(opt_.now_ctx) : 0;
    }

    // Arms (or re-arms) the gap timer when the lane is blocked behind a gap.
    // A no-op without timer hooks, when the same gap is already armed, or
    // when the arm callback fails (the next drain retries).
    void maybe_arm_gap(lane_state& ls) noexcept {
        if (opt_.arm == nullptr) {
            return;
        }
        if (ls.waiting.empty()) {
            return;
        }
        if (ls.waiting.begin()->first <= ls.next_run) {
            return;
        }
        if (ls.gap_armed && ls.gap_armed_for == ls.next_run) {
            return;
        }
        if (ls.gap_armed) {
            cancel_gap_locked(ls);
        }
        const std::uint64_t deadline = now_ns() + gap_timeout_ns();
        if (opt_.arm(opt_.timer_ctx, ls.lane_id, ls.next_run, deadline)) {
            ls.gap_armed = true;
            ls.gap_armed_for = ls.next_run;
        }
    }

    // Removes the lane's pending gap timer, if any. The timer implementation
    // may already have fired the callback; `skip_gap` re-validates every
    // condition under the same mutex, so a late callback is a no-op.
    void cancel_gap_locked(lane_state& ls) noexcept {
        if (!ls.gap_armed) {
            return;
        }
        if (opt_.cancel != nullptr) {
            opt_.cancel(opt_.timer_ctx, ls.lane_id);
        }
        ls.gap_armed = false;
    }

    void release_locked(lane_table::iterator it) noexcept {
        lane_state& ls = *it->second;
        ls.detached = true;
        cancel_gap_locked(ls);
        // Keep the ordering memory: everything up to `next_run` was claimed
        // (executed, buffered-then-run, or skipped). A later replay below the
        // watermark must stay a duplicate even though the queue entry -- the
        // part that scales with load -- is released here.
        consumed_[ls.lane_id] = ls.next_run;
        // A released lane has no buffered call left, so this refunds zero in
        // the nominal case; the subtraction keeps the invariant explicit.
        buffered_total_ -= ls.buffered_bytes;
        ls.buffered_bytes = 0;
        lanes_.erase(it);
    }

    [[nodiscard]] std::uint64_t gap_timeout_ns() const noexcept {
        const std::chrono::nanoseconds timeout = opt_.gap_timeout;
        return timeout.count() > 0 ? static_cast<std::uint64_t>(timeout.count()) : 0;
    }

    options opt_{};
    mutable std::mutex mutex_;
    lane_table lanes_;
    // Summed `frame_size` across every lane's buffered calls; checked against
    // `options::max_buffered_bytes` before a call is parked, refunded on
    // pop/release and cleared by `fail_all` (teardown releases everything).
    std::uint64_t buffered_total_ = 0;
    // Per-lane consumed watermark, kept after the queue entry is released.
    // One word per distinct lane id; cleared by `fail_all` (teardown has no
    // ordering left to protect).
    std::unordered_map<std::uint64_t, std::uint64_t> consumed_;
};

} // namespace crpc::rpc

#endif // CRPC_RPC_LANE_QUEUE_HPP
