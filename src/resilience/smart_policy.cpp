// CRPC -- smart resilience defaults, derivation.
//
// The rules are documented on the header. This translation unit holds only the
// arithmetic so the header stays a set of type definitions and the derivation
// can be changed without recompiling every consumer.
#include <crpc/resilience/smart_policy.hpp>

namespace crpc::resilience {

smart_policy make_smart_policy(const call_traits& traits) noexcept {
    smart_policy policy;

    // timeout: an explicit deadline wins, otherwise the 1s default.
    policy.timeout = traits.deadline > std::chrono::nanoseconds::zero()
                         ? traits.deadline
                         : default_smart_timeout;

    // retry: an idempotent call may be attempted three times, a non-idempotent
    // one only once. The backoff window is a fraction of the timeout so a retry
    // can still fit inside the caller's budget.
    policy.retry.max_attempts = traits.idempotent ? 3u : 1u;
    policy.retry.initial_backoff = policy.timeout / 10;
    policy.retry.max_backoff = policy.timeout / 2;

    // breaker: the slow-call threshold is the timeout itself, so a call that
    // would miss the deadline counts as slow; the p99 target is twice the
    // timeout and the window is a fixed 10s.
    policy.breaker.window = std::chrono::seconds(10);
    policy.breaker.p99_target = policy.timeout * 2;
    policy.breaker.slow_call_threshold = policy.timeout;
    policy.breaker.open_duration = std::chrono::seconds(5);

    // rate: the endpoint capacity per second, with a tenth of it as burst.
    policy.rate.rate_per_second = default_smart_endpoint_rate_per_second;
    policy.rate.burst = default_smart_endpoint_rate_per_second / 10.0;

    // bulkhead: the endpoint concurrency limit.
    policy.bulkhead.capacity = default_smart_endpoint_concurrency;

    return policy;
}

} // namespace crpc::resilience
