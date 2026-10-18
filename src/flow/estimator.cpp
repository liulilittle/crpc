// CRPC -- adaptive flow control implementation.
#include <crpc/flow/estimator.hpp>

namespace crpc {
namespace flow {
namespace {

// Returns `value * per_mille / 1000` without overflowing for the ranges used
// here. The operands are bounded by max_bytes (2^28) and per_mille (1000), so
// the product fits comfortably in 64 bits; the helper exists to make the
// rounding rule explicit (truncation toward zero, never a silent wrap).
u64 scale_per_mille(u64 value, u32 per_mille) noexcept {
    return (value / 1000u) * per_mille + ((value % 1000u) * per_mille) / 1000u;
}

u64 saturating_add(u64 a, u64 b) noexcept {
    const u64 max = static_cast<u64>(-1);
    return (a > max - b) ? max : a + b;
}

} // namespace

// --- rtt_estimator -----------------------------------------------------------

rtt_estimator::rtt_estimator(const config& cfg) noexcept : cfg_(cfg) {
    baseline_ns_ = cfg_.initial_baseline_ns;
}

rtt_estimator::rtt_estimator() noexcept : rtt_estimator(config()) {}

bool rtt_estimator::on_sample(u64 rtt_ns) noexcept {
    if (rtt_ns == 0) {
        return false;
    }
    // A sample above the ceiling is a measurement artifact, not a path change.
    // Accepting it would inflate the baseline and, because upward movement is
    // slow, the inflation would persist for many round trips.
    if (rtt_ns > cfg_.sample_ceiling_ns) {
        ++sample_count_;
        return false;
    }

    // The first sample is the only one that carries no history: there is
    // nothing to compare it against, so it becomes the baseline. Every later
    // sample is gated.
    if (sample_count_ == 0) {
        ++sample_count_;
        baseline_ns_ = rtt_ns;
        jitter_ns_ = 0;
        fast_confirmations_ = 0;
        slow_confirmations_ = 0;
        return true;
    }

    ++sample_count_;

    // G1: a lower sample cannot be caused by queueing. Accept it immediately.
    // This is what gives the estimator its fast downward convergence; a
    // running-minimum would need several samples to reach the same point, and a
    // single corrupt low sample would then be permanent.
    if (rtt_ns < baseline_ns_) {
        const u64 innovation = baseline_ns_ - rtt_ns;
        baseline_ns_ = rtt_ns;
        fast_confirmations_ = 0;
        slow_confirmations_ = 0;
        update_jitter(innovation);
        return true;
    }

    const u64 innovation = rtt_ns - baseline_ns_;
    update_jitter(innovation);

    // G3: a sustained upward offset means the path itself changed (a route
    // switch). Two thresholds: a sharp jump needs fewer confirmations than a
    // slow drift. Both counters reset as soon as the sample returns to the
    // baseline, so an isolated burst cannot accumulate evidence.
    const u64 fast_threshold =
        scale_per_mille(baseline_ns_, cfg_.fast_threshold_per_mille);
    const u64 slow_threshold =
        scale_per_mille(baseline_ns_, cfg_.slow_threshold_per_mille);

    if (rtt_ns >= fast_threshold) {
        ++fast_confirmations_;
        ++slow_confirmations_;
    } else if (rtt_ns >= slow_threshold) {
        fast_confirmations_ = 0;
        ++slow_confirmations_;
    } else {
        fast_confirmations_ = 0;
        slow_confirmations_ = 0;
    }

    const bool path_changed =
        fast_confirmations_ >= cfg_.fast_confirmations ||
        slow_confirmations_ >= cfg_.slow_confirmations;
    if (path_changed) {
        baseline_ns_ = rtt_ns;
        fast_confirmations_ = 0;
        slow_confirmations_ = 0;
        return true;
    }

    // G2: bounded upward growth. The new baseline never exceeds the observation
    // itself, so a single sample can move it by at most one observation.
    const u64 grown = saturating_add(
        baseline_ns_, scale_per_mille(baseline_ns_, cfg_.growth_per_mille));
    baseline_ns_ = (grown < rtt_ns) ? grown : rtt_ns;
    return true;
}

u64 rtt_estimator::queue_delay_ns(u64 observed_ns) const noexcept {
    if (observed_ns <= baseline_ns_) {
        return 0;
    }
    return observed_ns - baseline_ns_;
}

void rtt_estimator::update_jitter(u64 innovation_ns) noexcept {
    // Exponential moving average with a 1/8 weight. The exact weight is not
    // important; what matters is that the value is never used to move the
    // baseline. Noise is measured, not followed.
    if (sample_count_ <= 1) {
        jitter_ns_ = innovation_ns;
        return;
    }
    jitter_ns_ = jitter_ns_ - (jitter_ns_ / 8u) + (innovation_ns / 8u);
}

void rtt_estimator::reset() noexcept {
    baseline_ns_ = cfg_.initial_baseline_ns;
    jitter_ns_ = 0;
    sample_count_ = 0;
    fast_confirmations_ = 0;
    slow_confirmations_ = 0;
}

// --- pacer -------------------------------------------------------------------

pacer::pacer(const config& cfg) noexcept : cfg_(cfg) {
    rate_ = cfg_.initial_rate_bytes_per_second;
    if (rate_ < cfg_.min_rate_bytes_per_second) {
        rate_ = cfg_.min_rate_bytes_per_second;
    }
    if (rate_ > cfg_.max_rate_bytes_per_second) {
        rate_ = cfg_.max_rate_bytes_per_second;
    }
    credit_ = rate_ * cfg_.burst_ns / 1'000'000'000ull;
}

pacer::pacer() noexcept : pacer(config()) {}

void pacer::set_rate(u64 bytes_per_second) noexcept {
    if (bytes_per_second < cfg_.min_rate_bytes_per_second) {
        bytes_per_second = cfg_.min_rate_bytes_per_second;
    }
    if (bytes_per_second > cfg_.max_rate_bytes_per_second) {
        bytes_per_second = cfg_.max_rate_bytes_per_second;
    }
    rate_ = bytes_per_second;
}

void pacer::refill(u64 now_ns) noexcept {
    if (last_refill_ns_ == 0) {
        last_refill_ns_ = now_ns;
        return;
    }
    if (now_ns <= last_refill_ns_) {
        // A non-monotonic timestamp means the clock was replaced or the caller
        // made an error. Do not fabricate credit; just re-anchor.
        last_refill_ns_ = now_ns;
        return;
    }
    const u64 elapsed = now_ns - last_refill_ns_;
    last_refill_ns_ = now_ns;

    // bytes = rate * elapsed / 1e9, computed in two steps to avoid overflow on
    // fast rates (up to 8 GiB/s) multiplied by long elapsed intervals.
    const u64 whole_seconds = elapsed / 1'000'000'000ull;
    const u64 remainder_ns = elapsed % 1'000'000'000ull;
    u64 added = saturating_add(rate_ * whole_seconds,
                               rate_ * remainder_ns / 1'000'000'000ull);

    const u64 burst_credit = rate_ * cfg_.burst_ns / 1'000'000'000ull;
    credit_ = saturating_add(credit_, added);
    if (credit_ > burst_credit) {
        credit_ = burst_credit;
    }
    (void)added;
}

u64 pacer::take(u64 bytes) noexcept {
    if (bytes <= credit_) {
        credit_ -= bytes;
        return bytes;
    }
    const u64 granted = credit_;
    credit_ = 0;
    return granted;
}

u64 pacer::next_send_ns(u64 now_ns, u64 bytes) const noexcept {
    if (bytes <= credit_) {
        return now_ns;
    }
    const u64 missing = bytes - credit_;
    if (rate_ == 0) {
        return now_ns;
    }
    // missing / rate seconds, expressed in nanoseconds.
    const u64 delay_ns = (missing / rate_) * 1'000'000'000ull +
                         ((missing % rate_) * 1'000'000'000ull) / rate_;
    return saturating_add(now_ns, delay_ns);
}

void pacer::reset(u64 now_ns) noexcept {
    last_refill_ns_ = now_ns;
    credit_ = rate_ * cfg_.burst_ns / 1'000'000'000ull;
}

// --- congestion_window -------------------------------------------------------

congestion_window::congestion_window(const config& cfg) noexcept : cfg_(cfg) {
    window_bytes_ = cfg_.initial_bytes;
    clamp();
    high_water_bytes_ = window_bytes_;
}

congestion_window::congestion_window() noexcept : congestion_window(config()) {}

void congestion_window::clamp() noexcept {
    if (window_bytes_ < cfg_.min_bytes) {
        window_bytes_ = cfg_.min_bytes;
    }
    if (window_bytes_ > cfg_.max_bytes) {
        window_bytes_ = cfg_.max_bytes;
    }
}

void congestion_window::on_ack(u64 acked_bytes) noexcept {
    if (acked_bytes == 0) {
        return;
    }
    u64 increment = 0;
    if (slow_start_) {
        // Exponential growth: each acknowledged byte adds roughly one byte, so
        // the window doubles per round trip.
        increment = acked_bytes;
    } else {
        // Linear growth: one maximum-segment per round trip. The step is
        // deliberately small because the only thing that ends congestion
        // avoidance is another congestion signal, and a large step would make
        // that signal arrive too late.
        const u64 segment = 1460;
        increment = (acked_bytes * segment) / window_bytes_;
        if (increment == 0) {
            increment = 1;
        }
    }
    window_bytes_ = saturating_add(window_bytes_, increment);
    clamp();
    if (window_bytes_ > high_water_bytes_) {
        high_water_bytes_ = window_bytes_;
    }
}

void congestion_window::on_congestion() noexcept {
    if (window_bytes_ > high_water_bytes_) {
        high_water_bytes_ = window_bytes_;
    }
    slow_start_ = false;
    window_bytes_ = scale_per_mille(window_bytes_, cfg_.decrease_per_mille);
    clamp();
}

void congestion_window::restore_high_water() noexcept {
    // Restoring the high-water mark rather than ramping from the current value
    // is the "take the high ground" rule: when a path is re-established and the
    // traffic profile is unchanged, the previous peak is the best available
    // estimate. Scaling it down proportionally would make recovery slower for
    // no evidence-based reason.
    window_bytes_ = high_water_bytes_;
    clamp();
    slow_start_ = false;
}

void congestion_window::reset() noexcept {
    window_bytes_ = cfg_.initial_bytes;
    clamp();
    high_water_bytes_ = window_bytes_;
    slow_start_ = true;
}

// --- rtt_tracker -------------------------------------------------------------

u64 rtt_tracker::on_send(u64 now_ns) noexcept {
    last_send_ns_ = now_ns;
    if (oldest_outstanding_ns_ == 0) {
        oldest_outstanding_ns_ = now_ns;
    }
    return now_ns;
}

bool rtt_tracker::on_ack(u64 now_ns, u64 token_ns, u64& sample_ns) noexcept {
    if (token_ns == 0 || now_ns < token_ns) {
        return false;
    }
    const u64 sample = now_ns - token_ns;
    if (sample > max_sample_age_ns) {
        return false;
    }
    sample_ns = sample;
    if (token_ns == oldest_outstanding_ns_) {
        oldest_outstanding_ns_ = 0;
    }
    return true;
}

void rtt_tracker::reset() noexcept {
    last_send_ns_ = 0;
    oldest_outstanding_ns_ = 0;
}

} // namespace flow
} // namespace crpc

