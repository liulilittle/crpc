// CRPC -- adaptive flow control.
//
// The design here follows three rules that come from how a feedback loop
// actually behaves, not from how a textbook describes it:
//
//   1. You never observe the path, only a polluted scalar. The observation is
//      split by behaviour, not by physical location:
//          observed = propagation + queueing + noise
//      Propagation is the anchor and does not grow with load. Queueing is the
//      only congestion signal. Noise carries no congestion information and must
//      be structurally excluded rather than filtered after the fact.
//
//   2. Do not try to "know", try to approach. The baseline moves toward a new
//      observation in small, gated steps: downward instantly (a lower sample
//      cannot be caused by queueing), upward geometrically and bounded (an
//      upward sample may be queueing or a genuine path change, and you cannot
//      tell which).
//
//   3. Overshoot is how information is bought. A sender that never probes
//      cannot discover spare capacity, but it still suffers congestion caused
//      by others. Probing is therefore mandatory; its cost is bounded by a
//      budget, not eliminated.
//
// Every decision below is deliberately simple. A clever mechanism that
// "detects that the path is clear" was removed from an earlier design because
// it was, in practice, always detecting clearance: fast samples are normal, not
// exceptional. Simplicity here is not a shortcut; it is what keeps the loop from
// oscillating.
#ifndef CRPC_FLOW_ESTIMATOR_HPP
#define CRPC_FLOW_ESTIMATOR_HPP

#include <cstddef>

#include <crpc/config.hpp>

namespace crpc {
namespace flow {

// A monotonic nanosecond clock. The library never reads a wall clock for
// control decisions: a wall clock can jump, and a jump inside a control loop is
// indistinguishable from a real change in the path.
using now_fn = u64 (*)(void* context);

struct clock_source {
    now_fn now = nullptr;
    void* context = nullptr;

    bool valid() const noexcept { return now != nullptr; }
    u64 operator()() const noexcept { return now != nullptr ? now(context) : 0; }
};

// --- Propagation-delay estimator -------------------------------------------
//
// One state variable (the baseline) plus two confirmation counters. There is no
// covariance matrix and no process model: the directional gate is what makes
// the estimate robust, and adding an unobservable model on top of an
// unobservable state buys nothing.
class CRPC_API rtt_estimator {
public:
    struct config {
        // Starting point when the first sample is much larger than expected.
        // A conservative value avoids a burst on a slow path.
        u64 initial_baseline_ns = 1'000'000;

        // G2: upward growth per sample, in per-mille. 122 = 12.2%.
        // The growth is additionally capped at the observation itself, so a
        // single large sample can never drag the baseline up in one step.
        u32 growth_per_mille = 122;

        // G3: a persistent upward trend means the path itself changed (a route
        // switch), not that a queue formed. Two thresholds are used because a
        // slow drift needs more evidence than a sharp jump.
        u32 fast_threshold_per_mille = 1100;
        u32 slow_threshold_per_mille = 1050;
        u32 fast_confirmations = 3;
        u32 slow_confirmations = 4;

        // Samples above this are treated as a measurement artifact (a stalled
        // ACK, a suspended process) and do not move the baseline upward.
        u64 sample_ceiling_ns = 500'000'000;
    };

    rtt_estimator() noexcept;
    explicit rtt_estimator(const config& cfg) noexcept;

    // Feeds one RTT sample. Returns true when the baseline was updated.
    // Non-monotonic or absurd samples are ignored rather than trusted.
    bool on_sample(u64 rtt_ns) noexcept;

    // Current propagation-delay estimate. Never zero after the first sample.
    u64 baseline_ns() const noexcept { return baseline_ns_; }

    // Queueing delay implied by an observation, or zero when the observation is
    // at or below the baseline. This is the congestion signal: it is the only
    // quantity that should drive rate reduction.
    u64 queue_delay_ns(u64 observed_ns) const noexcept;

    // Mean absolute innovation (|observed - baseline|), an exponentially
    // weighted estimate. Used to distinguish a noisy path from a stable one;
    // it must never be used to move the baseline.
    u64 jitter_ns() const noexcept { return jitter_ns_; }

    // Number of samples observed since construction.
    u64 sample_count() const noexcept { return sample_count_; }

    // Forgets history. Used when a connection is re-established: the path may
    // be entirely different, and carrying the old baseline over would make the
    // new connection start with the wrong estimate.
    void reset() noexcept;

    const config& settings() const noexcept { return cfg_; }

private:
    void update_jitter(u64 innovation_ns) noexcept;

    config cfg_;
    u64 baseline_ns_ = 0;
    u64 jitter_ns_ = 0;
    u64 sample_count_ = 0;
    u32 fast_confirmations_ = 0;
    u32 slow_confirmations_ = 0;
};

// --- Pacing -----------------------------------------------------------------
//
// A token bucket in bytes. Pacing exists because a window alone allows a burst
// that fills a shallow queue in one instant; spreading the same bytes over time
// keeps the queue empty without reducing throughput. This is the practical form
// of "send at the rate of the bottleneck, not as fast as the window allows".
class CRPC_API pacer {
public:
    struct config {
        u64 initial_rate_bytes_per_second = 1u * 1024u * 1024u;
        u64 min_rate_bytes_per_second = 16u * 1024u;
        u64 max_rate_bytes_per_second = 8ull * 1024ull * 1024ull * 1024ull;
        u64 burst_ns = 2'000'000; // 2 ms of credit
    };

    pacer() noexcept;
    explicit pacer(const config& cfg) noexcept;

    void set_rate(u64 bytes_per_second) noexcept;
    u64 rate_bytes_per_second() const noexcept { return rate_; }

    // Adds credit for elapsed time, capped at one burst. Safe to call often.
    void refill(u64 now_ns) noexcept;

    // Bytes that may be sent right now without violating the rate.
    u64 available() const noexcept { return credit_; }

    // Consumes credit. Returns the number of bytes actually granted, which may
    // be less than requested; a caller must send only the granted amount.
    u64 take(u64 bytes) noexcept;

    // When the next byte may be sent, given an empty credit. Used to arm a
    // timer instead of spinning.
    u64 next_send_ns(u64 now_ns, u64 bytes) const noexcept;

    void reset(u64 now_ns) noexcept;

private:
    config cfg_;
    u64 rate_ = 0;
    u64 credit_ = 0;
    u64 last_refill_ns_ = 0;
};

// --- Congestion window ------------------------------------------------------
//
// Window growth is self-clocked: it advances on acknowledgements, never on a
// timer. A timer-driven increase would send more data precisely when the path
// is silent, which is the opposite of what a feedback loop should do.
//
// The window is expressed in bytes and bounded below by one maximum segment so
// that a shrinking path can always make progress.
class CRPC_API congestion_window {
public:
    struct config {
        u64 initial_bytes = 64u * 1024u;
        u64 min_bytes = 4u * 1024u;
        u64 max_bytes = 256u * 1024u * 1024u;
        // Growth per acknowledged round trip, in per-mille of the current
        // window. 1000 = double per RTT during slow start.
        u32 growth_per_mille = 1000;
        // Multiplicative decrease applied when the queue signal crosses the
        // configured threshold.
        u32 decrease_per_mille = 700; // keep 70%
    };

    congestion_window() noexcept;
    explicit congestion_window(const config& cfg) noexcept;

    u64 bytes() const noexcept { return window_bytes_; }

    // Slow start: exponential until the first congestion signal, then linear.
    // The transition is one-way until reset, which prevents the sawtooth from
    // restarting after every minor queue event.
    bool in_slow_start() const noexcept { return slow_start_; }

    // Called once per acknowledged segment with the number of newly
    // acknowledged bytes.
    void on_ack(u64 acked_bytes) noexcept;

    // Called when the queue delay exceeds the caller's threshold.
    void on_congestion() noexcept;

    // Restores the window to the high-water mark recorded before the last
    // decrease. This is used when a connection is re-established on a path that
    // is known to have carried the same traffic before: starting from the old
    // high-water mark beats ramping up again from a cold value.
    void restore_high_water() noexcept;

    u64 high_water_bytes() const noexcept { return high_water_bytes_; }

    void reset() noexcept;

private:
    void clamp() noexcept;

    config cfg_;
    u64 window_bytes_ = 0;
    u64 high_water_bytes_ = 0;
    bool slow_start_ = true;
};

// --- Round-trip tracker -----------------------------------------------------
//
// Maps a send timestamp to a round-trip sample when the acknowledgement
// arrives. This is where the observation delay becomes explicit: the sample
// describes a moment in the past, and every consumer of the sample must know
// that. The tracker does not try to compensate for it; it records it.
class CRPC_API rtt_tracker {
public:
    // Records a send and returns a token to be handed back on acknowledgement.
    u64 on_send(u64 now_ns) noexcept;

    // Computes the sample for a token. Returns false when the token is stale
    // (older than the window) or out of order; the caller must then ignore the
    // sample rather than feed a fabricated one to the estimator.
    bool on_ack(u64 now_ns, u64 token_ns, u64& sample_ns) noexcept;

    void reset() noexcept;

    // Samples older than this are discarded: they describe a path state so old
    // that acting on it would be acting on a different path.
    static constexpr u64 max_sample_age_ns = 10'000'000'000ull;

private:
    u64 last_send_ns_ = 0;
    u64 oldest_outstanding_ns_ = 0;
};

} // namespace flow
} // namespace crpc

#endif // CRPC_FLOW_ESTIMATOR_HPP

