// CRPC -- tests for the adaptive flow-control primitives.
//
// The tests below are written against the behaviour the control loop promises,
// not against the implementation: a lower sample is absorbed immediately, an
// upward sample is bounded, noise never moves the baseline, a sustained upward
// shift is recognised as a path change, and the window is self-clocked.
#include <crpc/flow/estimator.hpp>

#include "test_framework.hpp"

using namespace crpc;
using namespace crpc::flow;

CRPC_TEST(estimator_first_sample_sets_baseline) {
    rtt_estimator est;
    CRPC_CHECK(est.on_sample(5'000'000));
    CRPC_CHECK_EQ(est.baseline_ns(), 5'000'000ull);
}

CRPC_TEST(estimator_zero_sample_ignored) {
    rtt_estimator est;
    const u64 before = est.baseline_ns();
    CRPC_CHECK_FALSE(est.on_sample(0));
    CRPC_CHECK_EQ(est.baseline_ns(), before);
}

CRPC_TEST(estimator_downward_is_immediate) {
    rtt_estimator est;
    est.on_sample(10'000'000);
    // One lower sample is enough: a lower sample cannot be queueing.
    CRPC_CHECK(est.on_sample(4'000'000));
    CRPC_CHECK_EQ(est.baseline_ns(), 4'000'000ull);
}

CRPC_TEST(estimator_upward_is_bounded_and_capped_at_observation) {
    rtt_estimator est;
    est.on_sample(1'000'000);
    // A single huge upward sample must not move the baseline to it.
    est.on_sample(100'000'000);
    CRPC_CHECK(est.baseline_ns() < 100'000'000ull);
    // And it must never exceed the observation.
    CRPC_CHECK(est.baseline_ns() >= 1'000'000ull);
}

CRPC_TEST(estimator_ignores_samples_above_ceiling) {
    rtt_estimator::config cfg;
    cfg.sample_ceiling_ns = 100'000'000;
    rtt_estimator est(cfg);
    est.on_sample(1'000'000);
    CRPC_CHECK_FALSE(est.on_sample(500'000'000));
    CRPC_CHECK_EQ(est.baseline_ns(), 1'000'000ull);
}

CRPC_TEST(estimator_noise_does_not_move_baseline_upward) {
    rtt_estimator est;
    est.on_sample(1'000'000);
    // Oscillating noise around the baseline must leave it in place: each
    // upward sample is gated, each downward sample is real but tiny.
    for (int i = 0; i < 100; ++i) {
        est.on_sample(1'200'000);
        est.on_sample(900'000);
    }
    CRPC_CHECK(est.baseline_ns() <= 1'000'000ull);
}

CRPC_TEST(estimator_sustained_shift_is_detected_as_path_change) {
    rtt_estimator est;
    est.on_sample(1'000'000);
    // A persistent 20% offset for enough samples means the path changed.
    for (int i = 0; i < 10; ++i) {
        est.on_sample(1'200'000);
    }
    CRPC_CHECK(est.baseline_ns() >= 1'100'000ull);
}

CRPC_TEST(estimator_isolated_burst_is_not_a_path_change) {
    rtt_estimator est;
    est.on_sample(1'000'000);
    est.on_sample(1'500'000);
    est.on_sample(1'000'000);
    // The burst must not have accumulated confirmations.
    CRPC_CHECK(est.baseline_ns() < 1'100'000ull);
}

CRPC_TEST(estimator_queue_delay_is_the_congestion_signal) {
    rtt_estimator est;
    est.on_sample(1'000'000);
    CRPC_CHECK_EQ(est.queue_delay_ns(1'000'000), 0ull);
    CRPC_CHECK_EQ(est.queue_delay_ns(900'000), 0ull);
    CRPC_CHECK_EQ(est.queue_delay_ns(1'300'000), 300'000ull);
}

CRPC_TEST(estimator_jitter_tracks_innovation) {
    rtt_estimator est;
    est.on_sample(1'000'000);
    for (int i = 0; i < 20; ++i) {
        est.on_sample(1'100'000);
        est.on_sample(1'000'000);
    }
    CRPC_CHECK(est.jitter_ns() > 0);
    CRPC_CHECK(est.jitter_ns() <= 100'000ull);
}

CRPC_TEST(estimator_reset_forgets_history) {
    rtt_estimator est;
    est.on_sample(5'000'000);
    est.reset();
    CRPC_CHECK_EQ(est.baseline_ns(), est.settings().initial_baseline_ns);
    CRPC_CHECK_EQ(est.sample_count(), 0ull);
}

CRPC_TEST(pacer_default_config_is_self_consistent) {
    // Guards against a 32-bit literal overflow silently turning the ceiling
    // into zero, which would clamp every rate to zero and stop all sending.
    pacer::config cfg;
    CRPC_CHECK(cfg.max_rate_bytes_per_second > cfg.initial_rate_bytes_per_second);
    CRPC_CHECK(cfg.initial_rate_bytes_per_second >= cfg.min_rate_bytes_per_second);
    CRPC_CHECK(cfg.burst_ns > 0);
}

CRPC_TEST(pacer_starts_with_one_burst_of_credit) {
    pacer p;
    p.refill(1'000);
    CRPC_CHECK(p.available() > 0);
    // Credit is capped at the burst, so a long idle period cannot accumulate an
    // unbounded allowance.
    p.refill(10'000'000'000ull);
    CRPC_CHECK(p.available() <= p.rate_bytes_per_second() * 2 / 1000 + 1);
}

CRPC_TEST(pacer_take_is_bounded_by_credit) {
    pacer p;
    p.refill(1'000);
    const u64 available = p.available();
    const u64 granted = p.take(available + 1000);
    CRPC_CHECK_EQ(granted, available);
    CRPC_CHECK_EQ(p.available(), 0ull);
}

CRPC_TEST(pacer_next_send_is_in_the_future_when_dry) {
    pacer p;
    p.set_rate(1'000'000); // 1 MB/s
    p.refill(1'000);
    p.take(p.available());
    const u64 next = p.next_send_ns(1'000, 1000);
    CRPC_CHECK(next > 1'000);
}

CRPC_TEST(pacer_rate_is_clamped) {
    pacer::config cfg;
    cfg.min_rate_bytes_per_second = 1000;
    cfg.max_rate_bytes_per_second = 2000;
    pacer p(cfg);
    p.set_rate(1);
    CRPC_CHECK_EQ(p.rate_bytes_per_second(), 1000ull);
    p.set_rate(1'000'000);
    CRPC_CHECK_EQ(p.rate_bytes_per_second(), 2000ull);
}

CRPC_TEST(window_slow_start_grows_per_ack) {
    congestion_window w;
    const u64 initial = w.bytes();
    CRPC_CHECK(w.in_slow_start());
    w.on_ack(initial);
    CRPC_CHECK(w.bytes() > initial);
}

CRPC_TEST(window_congestion_enters_linear_and_shrinks) {
    congestion_window w;
    const u64 initial = w.bytes();
    w.on_congestion();
    CRPC_CHECK_FALSE(w.in_slow_start());
    CRPC_CHECK(w.bytes() < initial);
    CRPC_CHECK(w.bytes() >= w.high_water_bytes() * 7 / 10);
}

CRPC_TEST(window_never_below_minimum) {
    congestion_window w;
    for (int i = 0; i < 100; ++i) {
        w.on_congestion();
    }
    CRPC_CHECK(w.bytes() >= 4u * 1024u);
}

CRPC_TEST(window_never_above_maximum) {
    congestion_window w;
    for (int i = 0; i < 10000; ++i) {
        w.on_ack(1024u * 1024u);
    }
    CRPC_CHECK(w.bytes() <= 256u * 1024u * 1024u);
}

CRPC_TEST(window_high_water_survives_a_decrease) {
    congestion_window w;
    for (int i = 0; i < 10; ++i) {
        w.on_ack(64u * 1024u);
    }
    const u64 peak = w.bytes();
    w.on_congestion();
    CRPC_CHECK(w.bytes() < peak);
    CRPC_CHECK_EQ(w.high_water_bytes(), peak);
    w.restore_high_water();
    CRPC_CHECK_EQ(w.bytes(), peak);
}

CRPC_TEST(tracker_computes_rtt_from_token) {
    rtt_tracker tracker;
    const u64 token = tracker.on_send(1'000);
    u64 sample = 0;
    CRPC_CHECK(tracker.on_ack(4'000, token, sample));
    CRPC_CHECK_EQ(sample, 3'000ull);
}

CRPC_TEST(tracker_rejects_stale_token) {
    rtt_tracker tracker;
    const u64 token = tracker.on_send(1'000);
    u64 sample = 0;
    // Ten seconds plus one nanosecond: the sample describes a path state too
    // old to act on.
    CRPC_CHECK_FALSE(
        tracker.on_ack(token + rtt_tracker::max_sample_age_ns + 1, token, sample));
}

CRPC_TEST(tracker_rejects_backwards_time) {
    rtt_tracker tracker;
    const u64 token = tracker.on_send(5'000);
    u64 sample = 0;
    CRPC_CHECK_FALSE(tracker.on_ack(4'000, token, sample));
}

CRPC_TEST_MAIN()
