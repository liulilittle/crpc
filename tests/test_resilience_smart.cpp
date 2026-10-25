// CRPC -- smart resilience defaults.
//
// `make_smart_policy` must derive a complete, self-consistent policy from a
// small set of call traits so that a caller which configures nothing still gets
// a sane timeout, retry, breaker, rate limit and bulkhead. These tests pin the
// derivation rules from the design (Ruling 18) and the brief:
//
//   * timeout      -- explicit `deadline` wins, otherwise the 1s default;
//   * retry        -- idempotent -> 3 attempts, otherwise 1; backoff is
//                     timeout/10 .. timeout/2;
//   * breaker      -- window 10s, p99_target timeout*2, slow_call_threshold
//                     timeout, open_duration 5s;
//   * rate         -- endpoint capacity per second, burst = rate/10;
//   * bulkhead     -- endpoint concurrency limit.
//
// The tests compare `.count()` values rather than streaming `std::chrono`
// durations: C++17 has no `operator<<` for a duration, so the harness's
// `check_equal` could not print one.
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "test_framework.hpp"

#include <chrono>
#include <cstdint>

#include <crpc/resilience/smart_policy.hpp>

using namespace crpc;
using namespace crpc::resilience;
using namespace std::chrono_literals;

namespace {

long long ns(std::chrono::nanoseconds value) {
    return value.count();
}

} // namespace

CRPC_TEST(smart_default_timeout_is_one_second) {
    call_traits traits;
    const smart_policy policy = make_smart_policy(traits);
    CRPC_CHECK_EQ(1000000000LL, ns(policy.timeout));
}

CRPC_TEST(smart_explicit_deadline_wins_over_default) {
    call_traits traits;
    traits.deadline = 500ms;
    const smart_policy policy = make_smart_policy(traits);
    CRPC_CHECK_EQ(500000000LL, ns(policy.timeout));
}

CRPC_TEST(smart_zero_deadline_falls_back_to_default) {
    call_traits traits;
    traits.deadline = std::chrono::nanoseconds::zero();
    const smart_policy policy = make_smart_policy(traits);
    CRPC_CHECK_EQ(1000000000LL, ns(policy.timeout));
}

CRPC_TEST(smart_idempotent_retries_three_times) {
    call_traits traits;
    traits.idempotent = true;
    const smart_policy policy = make_smart_policy(traits);
    CRPC_CHECK_EQ(3u, policy.retry.max_attempts);
    CRPC_CHECK_EQ(100000000LL, ns(policy.retry.initial_backoff));
    CRPC_CHECK_EQ(500000000LL, ns(policy.retry.max_backoff));
}

CRPC_TEST(smart_non_idempotent_retries_once) {
    call_traits traits;
    const smart_policy policy = make_smart_policy(traits);
    CRPC_CHECK_EQ(1u, policy.retry.max_attempts);
    CRPC_CHECK_EQ(100000000LL, ns(policy.retry.initial_backoff));
    CRPC_CHECK_EQ(500000000LL, ns(policy.retry.max_backoff));
}

CRPC_TEST(smart_retry_backoff_scales_with_explicit_deadline) {
    call_traits traits;
    traits.idempotent = true;
    traits.deadline = 2s;
    const smart_policy policy = make_smart_policy(traits);
    CRPC_CHECK_EQ(3u, policy.retry.max_attempts);
    CRPC_CHECK_EQ(200000000LL, ns(policy.retry.initial_backoff));
    CRPC_CHECK_EQ(1000000000LL, ns(policy.retry.max_backoff));
}

CRPC_TEST(smart_breaker_derived_from_timeout) {
    call_traits traits;
    traits.deadline = 250ms;
    const smart_policy policy = make_smart_policy(traits);
    CRPC_CHECK_EQ(10000000000LL, ns(policy.breaker.window));
    CRPC_CHECK_EQ(500000000LL, ns(policy.breaker.p99_target));
    CRPC_CHECK_EQ(250000000LL, ns(policy.breaker.slow_call_threshold));
    CRPC_CHECK_EQ(5000000000LL, ns(policy.breaker.open_duration));
}

CRPC_TEST(smart_breaker_defaults_when_no_deadline) {
    call_traits traits;
    const smart_policy policy = make_smart_policy(traits);
    CRPC_CHECK_EQ(2000000000LL, ns(policy.breaker.p99_target));
    CRPC_CHECK_EQ(1000000000LL, ns(policy.breaker.slow_call_threshold));
}

CRPC_TEST(smart_rate_uses_endpoint_capacity_and_tenth_burst) {
    call_traits traits;
    const smart_policy policy = make_smart_policy(traits);
    CRPC_CHECK_EQ(default_smart_endpoint_rate_per_second,
                   policy.rate.rate_per_second);
    CRPC_CHECK_EQ(default_smart_endpoint_rate_per_second / 10.0,
                   policy.rate.burst);
}

CRPC_TEST(smart_bulkhead_uses_endpoint_concurrency) {
    call_traits traits;
    const smart_policy policy = make_smart_policy(traits);
    CRPC_CHECK_EQ(default_smart_endpoint_concurrency, policy.bulkhead.capacity);
}

CRPC_TEST(smart_policy_is_a_plain_copyable_value) {
    call_traits traits;
    traits.idempotent = true;
    traits.deadline = 300ms;
    const smart_policy policy = make_smart_policy(traits);
    const smart_policy copy = policy;
    CRPC_CHECK_EQ(ns(policy.timeout), ns(copy.timeout));
    CRPC_CHECK_EQ(policy.retry.max_attempts, copy.retry.max_attempts);
    CRPC_CHECK_EQ(ns(policy.breaker.p99_target), ns(copy.breaker.p99_target));
    CRPC_CHECK_EQ(policy.rate.burst, copy.rate.burst);
    CRPC_CHECK_EQ(policy.bulkhead.capacity, copy.bulkhead.capacity);
}

CRPC_TEST_MAIN()
