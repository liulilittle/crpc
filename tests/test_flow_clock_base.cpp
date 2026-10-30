// CRPC -- zero-based clock and pacer rate-change tests.
//
// Three defects shared one root cause: "0 means uninitialised" used in a domain
// where 0 is a legal value (a timestamp). A monotonic clock may legitimately
// start at zero, so:
//
//   * `rtt_tracker::on_send(0)` produced a token that `on_ack` rejected, which
//     made the first probe of such a clock unusable;
//   * `pacer::refill(0)` re-anchored to zero on every call, so the bucket never
//     gained credit;
//   * `pacer::set_rate` only changed the rate, leaving the old (smaller) burst
//     credit in place, so a rate increase took two refill periods to be felt.
//
// The sentinel is now all-ones, which is not a timestamp, and `set_rate` rescales
// the stored credit.
#include <cstddef>

#include <crpc/flow/estimator.hpp>

#include "test_framework.hpp"

namespace {

using namespace crpc;
using namespace crpc::flow;

} // namespace

// A clock that starts at zero must still produce a usable first sample.
CRPC_TEST(flow_tracker_zero_clock_first_sample) {
    rtt_tracker tracker;
    const u64 token = tracker.on_send(0);
    CRPC_CHECK_EQ(0ull, token);
    u64 sample = 0;
    // One millisecond later: the reply must be accepted and the sample exact.
    CRPC_CHECK(tracker.on_ack(1'000'000, token, sample));
    CRPC_CHECK_EQ(1'000'000ull, sample);
    // The outstanding token was consumed; the tracker is idle again.
    u64 second = 0;
    CRPC_CHECK(tracker.on_ack(2'000'000, token, second));
}

// A pacer anchored at zero must gain credit as time advances. With zero as the
// "not anchored" sentinel, `refill(0)` re-anchored on every call and the bucket
// stayed empty forever.
CRPC_TEST(pacer_zero_clock_base_refill) {
    pacer bucket;
    bucket.refill(0);
    const u64 initial = bucket.available();
    CRPC_CHECK(initial > 0);
    // Drain it, then let one second pass: credit must come back. The bucket is
    // capped at one 2 ms burst, so it refills to exactly that, not beyond.
    (void)bucket.take(initial);
    CRPC_CHECK_EQ(0ull, bucket.available());
    bucket.refill(1'000'000'000);
    const u64 burst = bucket.rate_bytes_per_second() * 2'000'000ull / 1'000'000'000ull;
    CRPC_CHECK_EQ(burst, bucket.available());
    CRPC_CHECK(bucket.available() > 0);
}

// Raising the rate must make the new burst available on the next send, not two
// refill periods later. With the old credit carried over unchanged, a 64 KiB
// frame would still be throttled by the previous 1 MiB/s burst (2097 bytes).
CRPC_TEST(pacer_rate_increase_applies_without_waiting_two_bursts) {
    pacer bucket;
    bucket.refill(1'000);
    // Drain the bucket at the default rate.
    (void)bucket.take(bucket.available());
    CRPC_CHECK_EQ(0ull, bucket.available());

    // Refill to a full burst at the default rate, then raise the rate.
    bucket.refill(1'000 + 2'000'000);
    const u64 default_burst = 1u * 1024u * 1024u * 2'000'000ull / 1'000'000'000ull;
    CRPC_CHECK_EQ(default_burst, bucket.available());

    const u64 fast_rate = 64u * 1024u * 1024u; // 64 MiB/s
    bucket.set_rate(fast_rate);
    const u64 fast_burst = fast_rate * 2'000'000ull / 1'000'000'000ull;
    CRPC_CHECK_EQ(fast_burst, bucket.available());
    CRPC_CHECK(bucket.available() >= 65536);
    // The 64 KiB frame is sendable immediately: no further refill needed.
    CRPC_CHECK_EQ(0ull, bucket.next_send_ns(3'000'000, 65536) - 3'000'000);
}

// Lowering the rate must clamp the carried credit to the smaller burst, so a
// reduced rate cannot be bypassed by a burst of old credit.
CRPC_TEST(pacer_rate_decrease_clamps_carried_credit) {
    pacer bucket;
    bucket.set_rate(64u * 1024u * 1024u);
    bucket.refill(1'000);
    bucket.refill(1'000 + 2'000'000);
    const u64 fast_burst = 64u * 1024u * 1024u * 2'000'000ull / 1'000'000'000ull;
    CRPC_CHECK_EQ(fast_burst, bucket.available());

    const u64 slow_rate = 1u * 1024u * 1024u;
    bucket.set_rate(slow_rate);
    const u64 slow_burst = slow_rate * 2'000'000ull / 1'000'000'000ull;
    CRPC_CHECK_EQ(slow_burst, bucket.available());
    CRPC_CHECK(bucket.available() < fast_burst);
}

// The clamps still apply after a rate change: the window-derived rate can be
// enormous or tiny and the bucket must stay inside its configured bounds.
CRPC_TEST(pacer_rate_change_respects_bounds) {
    pacer::config cfg;
    cfg.min_rate_bytes_per_second = 1000;
    cfg.max_rate_bytes_per_second = 2000;
    pacer bucket(cfg);
    bucket.refill(1'000);
    bucket.set_rate(1);
    CRPC_CHECK_EQ(1000ull, bucket.rate_bytes_per_second());
    bucket.set_rate(1'000'000'000);
    CRPC_CHECK_EQ(2000ull, bucket.rate_bytes_per_second());
    // The credit is bounded by the clamped burst, not by the requested rate.
    CRPC_CHECK(bucket.available() <= 2000ull * 2'000'000ull / 1'000'000'000ull);
}

CRPC_TEST_MAIN()
