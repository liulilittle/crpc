// CRPC -- resilience module tests.
//
// All time-dependent tests use `manual_clock`; no test reads the wall clock.
// The concurrency cases are also intended to be run under TSan as a separate
// build (`-DCRPC_SANITIZER=thread` on GCC/Clang), which is not part of the
// normal CTest run.
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
// The shared harness captures minidumps in debug builds; the library is not
// linked by the top-level test target.
#pragma comment(lib, "dbghelp.lib")
#endif
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <thread>
#include <vector>

#include <crpc/resilience/bulkhead.hpp>
#include <crpc/resilience/circuit_breaker.hpp>
#include <crpc/resilience/load_balancer.hpp>
#include <crpc/resilience/rate_limiter.hpp>
#include <crpc/resilience/retry.hpp>

using namespace crpc;
using namespace crpc::resilience;
using namespace std::chrono_literals;

namespace {

int state_id(breaker_state state) {
    return static_cast<int>(state);
}

int code_id(rpc_code code) {
    return static_cast<int>(code);
}

struct transition_log {
    std::atomic<int> to_open{0};
    std::atomic<int> to_half_open{0};
    std::atomic<int> to_closed{0};
};

void record_transition(void* context, breaker_state old_state,
                       breaker_state new_state) noexcept {
    auto* log = static_cast<transition_log*>(context);
    if (new_state == breaker_state::open) {
        log->to_open.fetch_add(1, std::memory_order_relaxed);
    } else if (new_state == breaker_state::half_open) {
        log->to_half_open.fetch_add(1, std::memory_order_relaxed);
    } else if (new_state == breaker_state::closed) {
        log->to_closed.fetch_add(1, std::memory_order_relaxed);
    }
    (void)old_state;
}

bool always_retry(rpc_code, bool, void*) noexcept {
    return true;
}

class vector_set final : public instance_set {
public:
    std::vector<instance> items;
    u64 version_value = 1;

    [[nodiscard]] std::size_t size() const noexcept override { return items.size(); }
    [[nodiscard]] instance at(std::size_t index) const noexcept override {
        return items[index];
    }
    [[nodiscard]] u64 version() const noexcept override { return version_value; }
};

breaker_config fast_breaker_config() {
    breaker_config config;
    config.window = 10s;
    config.buckets = 10;
    config.min_requests = 1;
    config.failure_ratio_threshold = 0.5;
    config.open_duration = 10s;
    config.half_open_max_probes = 3;
    config.half_open_successes_to_close = 5;
    return config;
}

} // namespace

// --- Circuit breaker ---------------------------------------------------------

CRPC_TEST(breaker_closed_to_open_on_failure_ratio) {
    manual_clock clock;
    breaker_config config;
    config.window = 10s;
    config.buckets = 10;
    config.min_requests = 5;
    config.failure_ratio_threshold = 0.5;
    config.open_duration = 10s;

    circuit_breaker breaker(config, clock.source());
    CRPC_CHECK_EQ(state_id(breaker.state()), state_id(breaker_state::closed));

    for (int i = 0; i < 4; ++i) {
        auto permit = breaker.allow();
        CRPC_CHECK(permit.allowed());
        permit.failure();
    }
    CRPC_CHECK_EQ(state_id(breaker.state()), state_id(breaker_state::closed));

    auto permit = breaker.allow();
    CRPC_CHECK(permit.allowed());
    permit.failure();
    CRPC_CHECK_EQ(state_id(breaker.state()), state_id(breaker_state::open));
    CRPC_CHECK_EQ(breaker.stats().total(), static_cast<u64>(5));
}

CRPC_TEST(breaker_open_rejects_and_retry_after) {
    manual_clock clock;
    breaker_config config = fast_breaker_config();
    config.open_duration = 10s;

    circuit_breaker breaker(config, clock.source());
    {
        auto permit = breaker.allow();
        permit.failure();
    }
    CRPC_CHECK_EQ(state_id(breaker.state()), state_id(breaker_state::open));

    auto rejected = breaker.allow();
    CRPC_CHECK_FALSE(rejected.allowed());
    CRPC_CHECK_EQ(code_id(rejected.code()), code_id(rpc_code::unavailable));
    CRPC_CHECK(rejected.retry_after().count() > 0);

    clock.advance(5s);
    auto still_rejected = breaker.allow();
    CRPC_CHECK_FALSE(still_rejected.allowed());

    clock.advance(5s);
    auto probe = breaker.allow();
    CRPC_CHECK(probe.allowed());
    CRPC_CHECK_EQ(state_id(breaker.state()), state_id(breaker_state::half_open));
}

CRPC_TEST(breaker_half_open_probe_limit) {
    manual_clock clock;
    breaker_config config = fast_breaker_config();
    config.half_open_max_probes = 3;
    config.half_open_successes_to_close = 5;

    circuit_breaker breaker(config, clock.source());
    {
        auto permit = breaker.allow();
        permit.failure();
    }
    clock.advance(10s);

    auto first = breaker.allow();
    auto second = breaker.allow();
    auto third = breaker.allow();
    CRPC_CHECK(first.allowed());
    CRPC_CHECK(second.allowed());
    CRPC_CHECK(third.allowed());

    auto fourth = breaker.allow();
    CRPC_CHECK_FALSE(fourth.allowed());
    CRPC_CHECK_EQ(code_id(fourth.code()), code_id(rpc_code::unavailable));

    // Completing a probe returns its token, so the limit is on concurrent
    // probes rather than on the total number of probes.
    first.success();
    auto fifth = breaker.allow();
    CRPC_CHECK(fifth.allowed());
}

CRPC_TEST(breaker_probe_successes_close) {
    manual_clock clock;
    breaker_config config = fast_breaker_config();
    config.min_requests = 2;
    config.half_open_max_probes = 3;
    config.half_open_successes_to_close = 3;

    circuit_breaker breaker(config, clock.source());
    for (int i = 0; i < 2; ++i) {
        auto permit = breaker.allow();
        permit.failure();
    }
    clock.advance(10s);

    auto first = breaker.allow();
    auto second = breaker.allow();
    auto third = breaker.allow();
    CRPC_CHECK(first.allowed());
    CRPC_CHECK(second.allowed());
    CRPC_CHECK(third.allowed());

    first.success();
    second.success();
    CRPC_CHECK_EQ(state_id(breaker.state()), state_id(breaker_state::half_open));
    third.success();
    CRPC_CHECK_EQ(state_id(breaker.state()), state_id(breaker_state::closed));

    // Closing clears the old window, so a new failure does not immediately
    // reopen the breaker.
    auto after = breaker.allow();
    CRPC_CHECK(after.allowed());
    after.failure();
    CRPC_CHECK_EQ(state_id(breaker.state()), state_id(breaker_state::closed));
}

CRPC_TEST(breaker_probe_failure_reopens) {
    manual_clock clock;
    breaker_config config = fast_breaker_config();

    circuit_breaker breaker(config, clock.source());
    {
        auto permit = breaker.allow();
        permit.failure();
    }
    clock.advance(10s);

    auto probe = breaker.allow();
    CRPC_CHECK(probe.allowed());
    probe.failure();
    CRPC_CHECK_EQ(state_id(breaker.state()), state_id(breaker_state::open));

    auto rejected = breaker.allow();
    CRPC_CHECK_FALSE(rejected.allowed());
    CRPC_CHECK(rejected.retry_after().count() > 0);
}

CRPC_TEST(breaker_ignored_permits_do_not_count) {
    manual_clock clock;
    breaker_config config = fast_breaker_config();
    config.min_requests = 2;
    config.failure_ratio_threshold = 0.5;

    circuit_breaker breaker(config, clock.source());
    {
        auto permit = breaker.allow();
        CRPC_CHECK(permit.allowed());
    }
    CRPC_CHECK_EQ(breaker.stats().total(), static_cast<u64>(0));
    CRPC_CHECK_EQ(state_id(breaker.state()), state_id(breaker_state::closed));

    auto permit = breaker.allow();
    permit.failure();
    CRPC_CHECK_EQ(breaker.stats().total(), static_cast<u64>(1));
    CRPC_CHECK_EQ(state_id(breaker.state()), state_id(breaker_state::closed));
}

CRPC_TEST(breaker_window_slides) {
    manual_clock clock;
    breaker_config config;
    config.window = 1s;
    config.buckets = 4;
    config.min_requests = 100;
    config.failure_ratio_threshold = 0.5;

    circuit_breaker breaker(config, clock.source());
    for (int i = 0; i < 5; ++i) {
        auto permit = breaker.allow();
        permit.failure();
    }
    CRPC_CHECK_EQ(breaker.stats().total(), static_cast<u64>(5));

    clock.advance(2s);
    CRPC_CHECK_EQ(breaker.stats().total(), static_cast<u64>(0));

    auto permit = breaker.allow();
    permit.success();
    CRPC_CHECK_EQ(breaker.stats().total(), static_cast<u64>(1));
}

CRPC_TEST(breaker_aged_failures_do_not_open) {
    manual_clock clock;
    breaker_config config;
    config.window = 1s;
    config.buckets = 4;
    config.min_requests = 5;
    config.failure_ratio_threshold = 0.5;

    circuit_breaker breaker(config, clock.source());
    for (int i = 0; i < 3; ++i) {
        auto permit = breaker.allow();
        permit.failure();
    }
    CRPC_CHECK_EQ(state_id(breaker.state()), state_id(breaker_state::closed));

    clock.advance(2s);
    for (int i = 0; i < 2; ++i) {
        auto permit = breaker.allow();
        permit.failure();
    }
    CRPC_CHECK_EQ(state_id(breaker.state()), state_id(breaker_state::closed));

    for (int i = 0; i < 3; ++i) {
        auto permit = breaker.allow();
        permit.failure();
    }
    CRPC_CHECK_EQ(state_id(breaker.state()), state_id(breaker_state::open));
}

CRPC_TEST(breaker_slow_ratio_opens) {
    manual_clock clock;
    breaker_config config = fast_breaker_config();
    config.min_requests = 2;
    config.failure_ratio_threshold = 1.0;
    config.slow_call_threshold = 1ms;
    config.slow_call_ratio_threshold = 0.5;

    circuit_breaker breaker(config, clock.source());
    {
        auto permit = breaker.allow();
        permit.success();
    }
    {
        auto permit = breaker.allow();
        permit.slow();
    }
    CRPC_CHECK_EQ(state_id(breaker.state()), state_id(breaker_state::open));
}

CRPC_TEST(breaker_slow_success_auto_classified) {
    manual_clock clock;
    breaker_config config = fast_breaker_config();
    config.min_requests = 100;
    config.slow_call_threshold = 5ms;

    circuit_breaker breaker(config, clock.source());
    {
        auto permit = breaker.allow();
        clock.advance(6ms);
        permit.success();
    }
    {
        auto permit = breaker.allow();
        permit.success();
    }
    CRPC_CHECK_EQ(breaker.stats().slow, static_cast<u64>(1));
    CRPC_CHECK_EQ(breaker.stats().success, static_cast<u64>(1));
}

CRPC_TEST(breaker_derived_slow_threshold) {
    manual_clock clock;
    breaker_config config = fast_breaker_config();
    config.min_requests = 100;
    config.p99_target = 10ms;
    config.slow_call_threshold = std::chrono::nanoseconds(0);

    circuit_breaker breaker(config, clock.source());
    CRPC_CHECK_EQ(breaker.config().slow_call_threshold.count(), 50'000'000);

    {
        auto permit = breaker.allow();
        clock.advance(20ms);
        permit.success();
    }
    {
        auto permit = breaker.allow();
        clock.advance(60ms);
        permit.success();
    }
    CRPC_CHECK_EQ(breaker.stats().success, static_cast<u64>(1));
    CRPC_CHECK_EQ(breaker.stats().slow, static_cast<u64>(1));
}

CRPC_TEST(breaker_state_change_callback) {
    manual_clock clock;
    transition_log log;
    breaker_config config = fast_breaker_config();
    config.half_open_max_probes = 1;
    config.half_open_successes_to_close = 1;

    circuit_breaker breaker(config, clock.source(), &record_transition, &log);
    {
        auto permit = breaker.allow();
        permit.failure();
    }
    CRPC_CHECK_EQ(log.to_open.load(), 1);

    clock.advance(10s);
    auto probe = breaker.allow();
    CRPC_CHECK(probe.allowed());
    CRPC_CHECK_EQ(log.to_half_open.load(), 1);

    probe.success();
    CRPC_CHECK_EQ(log.to_closed.load(), 1);
}

// --- Rate limiter ------------------------------------------------------------

CRPC_TEST(limiter_burst_then_throttled) {
    manual_clock clock;
    rate_limit_config config;
    config.rate_per_second = 1.0;
    config.burst = 5.0;

    rate_limiter limiter(config, clock.source());
    for (int i = 0; i < 5; ++i) {
        auto result = limiter.try_acquire();
        CRPC_CHECK(result.ok());
    }

    auto rejected = limiter.try_acquire();
    CRPC_CHECK_FALSE(rejected.ok());
    CRPC_CHECK_EQ(code_id(rejected.code), code_id(rpc_code::resource_exhausted));
    CRPC_CHECK(rejected.retry_after.count() >= 900);
    CRPC_CHECK(rejected.retry_after.count() <= 1100);
}

CRPC_TEST(limiter_refills_over_time) {
    manual_clock clock;
    rate_limit_config config;
    config.rate_per_second = 1.0;
    config.burst = 5.0;

    rate_limiter limiter(config, clock.source());
    for (int i = 0; i < 5; ++i) {
        CRPC_CHECK(limiter.try_acquire().ok());
    }

    clock.advance(500ms);
    CRPC_CHECK_FALSE(limiter.try_acquire().ok());

    clock.advance(500ms);
    CRPC_CHECK(limiter.try_acquire().ok());
    CRPC_CHECK_FALSE(limiter.try_acquire().ok());
}

CRPC_TEST(limiter_retry_after_is_bounded) {
    manual_clock clock;
    rate_limit_config config;
    config.rate_per_second = 100.0;
    config.burst = 10.0;

    rate_limiter limiter(config, clock.source());
    for (int i = 0; i < 10; ++i) {
        CRPC_CHECK(limiter.try_acquire().ok());
    }

    auto rejected = limiter.try_acquire();
    CRPC_CHECK_FALSE(rejected.ok());
    CRPC_CHECK(rejected.retry_after.count() > 0);
    CRPC_CHECK(rejected.retry_after.count() <= 10);
}

CRPC_TEST(limiter_acquire_for_deadline) {
    manual_clock clock;
    rate_limit_config config;
    config.rate_per_second = 100.0;
    config.burst = 1.0;

    rate_limiter limiter(config, clock.source());
    CRPC_CHECK(limiter.try_acquire().ok());

    const u64 now = clock.now_ns();
    auto short_deadline = limiter.acquire_for(1, std::chrono::nanoseconds(
                                                     static_cast<i64>(now) + 5'000'000));
    CRPC_CHECK_FALSE(short_deadline.ok());
    CRPC_CHECK_FALSE(short_deadline.deadline_met);
    CRPC_CHECK(short_deadline.retry_after.count() > 0);

    auto long_deadline = limiter.acquire_for(1, std::chrono::nanoseconds(
                                                    static_cast<i64>(now) + 50'000'000));
    CRPC_CHECK_FALSE(long_deadline.ok());
    CRPC_CHECK(long_deadline.deadline_met);
}

// --- Bulkhead ----------------------------------------------------------------

CRPC_TEST(bulkhead_capacity_and_release) {
    bulkhead_config config;
    config.capacity = 2;
    config.retry_after = 25ms;

    bulkhead bulkhead_instance(config);
    auto first = bulkhead_instance.try_enter();
    auto second = bulkhead_instance.try_enter();
    CRPC_CHECK(first.allowed());
    CRPC_CHECK(second.allowed());
    CRPC_CHECK_EQ(bulkhead_instance.in_use(), static_cast<u32>(2));

    auto third = bulkhead_instance.try_enter();
    CRPC_CHECK_FALSE(third.allowed());
    CRPC_CHECK_EQ(code_id(third.code()), code_id(rpc_code::resource_exhausted));
    CRPC_CHECK_EQ(third.retry_after().count(), 25);

    first.leave();
    CRPC_CHECK_EQ(bulkhead_instance.in_use(), static_cast<u32>(1));
    auto fourth = bulkhead_instance.try_enter();
    CRPC_CHECK(fourth.allowed());
}

CRPC_TEST(bulkhead_concurrent_enters) {
    bulkhead_config config;
    config.capacity = 4;

    bulkhead bulkhead_instance(config);
    std::atomic<int> attempted{0};
    std::atomic<int> admitted{0};
    std::atomic<bool> release{false};

    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&]() {
            auto permit = bulkhead_instance.try_enter();
            if (permit.allowed()) {
                admitted.fetch_add(1, std::memory_order_relaxed);
            }
            attempted.fetch_add(1, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });
    }

    while (attempted.load(std::memory_order_acquire) < 8) {
        std::this_thread::yield();
    }
    CRPC_CHECK_EQ(admitted.load(), 4);

    release.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }
    CRPC_CHECK_EQ(bulkhead_instance.in_use(), static_cast<u32>(0));
}

// --- Retry -------------------------------------------------------------------

CRPC_TEST(retry_attempt_count) {
    retry_policy policy;
    policy.max_attempts = 3;
    policy.initial_backoff = 1ms;
    policy.jitter_ratio = 0.0;

    manual_clock clock;
    deterministic_rng rng(7);
    retry_sequence sequence(policy, nullptr, clock.source(),
                            &deterministic_rng::next_fn, &rng);

    std::chrono::nanoseconds delay{0};
    CRPC_CHECK(sequence.next(rpc_code::connection_reset, true, delay));
    CRPC_CHECK(sequence.next(rpc_code::connection_reset, true, delay));
    CRPC_CHECK_FALSE(sequence.next(rpc_code::connection_reset, true, delay));
    CRPC_CHECK_EQ(sequence.attempts(), static_cast<u32>(3));
}

CRPC_TEST(retry_backoff_monotonic_and_capped) {
    retry_policy policy;
    policy.max_attempts = 6;
    policy.initial_backoff = 10ms;
    policy.max_backoff = 50ms;
    policy.multiplier = 2.0;
    policy.jitter_ratio = 0.0;

    manual_clock clock;
    deterministic_rng rng(11);
    retry_sequence sequence(policy, nullptr, clock.source(),
                            &deterministic_rng::next_fn, &rng);

    std::vector<std::chrono::nanoseconds> delays;
    std::chrono::nanoseconds delay{0};
    while (sequence.next(rpc_code::connection_reset, true, delay)) {
        delays.push_back(delay);
    }

    CRPC_CHECK_EQ(delays.size(), static_cast<std::size_t>(5));
    CRPC_CHECK_EQ(delays[0].count(), 10'000'000);
    CRPC_CHECK_EQ(delays[1].count(), 20'000'000);
    CRPC_CHECK_EQ(delays[2].count(), 40'000'000);
    CRPC_CHECK_EQ(delays[3].count(), 50'000'000);
    CRPC_CHECK_EQ(delays[4].count(), 50'000'000);
    for (std::size_t i = 1; i < delays.size(); ++i) {
        CRPC_CHECK(delays[i].count() >= delays[i - 1].count());
    }
}

CRPC_TEST(retry_jitter_within_bounds) {
    retry_policy policy;
    policy.max_attempts = 100;
    policy.initial_backoff = 100ms;
    policy.max_backoff = 100ms;
    policy.multiplier = 1.0;
    policy.jitter_ratio = 0.5;

    manual_clock clock;
    deterministic_rng rng(12345);
    retry_sequence sequence(policy, nullptr, clock.source(),
                            &deterministic_rng::next_fn, &rng);

    std::chrono::nanoseconds delay{0};
    int samples = 0;
    while (sequence.next(rpc_code::connection_reset, true, delay)) {
        CRPC_CHECK(delay.count() >= 50'000'000);
        CRPC_CHECK(delay.count() <= 100'000'000);
        ++samples;
    }
    CRPC_CHECK_EQ(samples, 99);
}

CRPC_TEST(retry_budget_exhaustion_stops_retries) {
    manual_clock clock;
    retry_budget_config budget_config;
    budget_config.ratio = 0.5;
    budget_config.capacity = 10.0;
    budget_config.decay_interval = 3600s;

    retry_budget budget(budget_config, clock.source());
    budget.on_request();
    budget.on_request();
    CRPC_CHECK(budget.tokens() >= 1.0);

    retry_policy policy;
    policy.max_attempts = 5;
    policy.initial_backoff = 1ms;
    policy.jitter_ratio = 0.0;

    deterministic_rng rng(3);
    retry_sequence sequence(policy, &budget, clock.source(),
                            &deterministic_rng::next_fn, &rng);

    std::chrono::nanoseconds delay{0};
    CRPC_CHECK(sequence.next(rpc_code::connection_reset, true, delay));
    CRPC_CHECK_FALSE(sequence.next(rpc_code::connection_reset, true, delay));
    CRPC_CHECK_EQ(sequence.attempts(), static_cast<u32>(2));
    CRPC_CHECK(sequence.budget_denied());
}

CRPC_TEST(retry_budget_accumulates_fractional_deposits) {
    manual_clock clock;
    retry_budget_config config;
    config.ratio = 0.1;
    config.capacity = 10.0;
    config.decay_interval = 3600s;

    retry_budget budget(config, clock.source());
    for (int i = 0; i < 10; ++i) {
        budget.on_request();
    }
    CRPC_CHECK(budget.try_acquire());
    CRPC_CHECK_FALSE(budget.try_acquire());
}

CRPC_TEST(retry_budget_decays) {
    manual_clock clock;
    retry_budget_config config;
    config.ratio = 1.0;
    config.capacity = 10.0;
    config.decay_interval = 1s;

    retry_budget budget(config, clock.source());
    for (int i = 0; i < 5; ++i) {
        budget.on_request();
    }
    CRPC_CHECK(std::fabs(budget.tokens() - 5.0) < 0.001);

    clock.advance(1s);
    CRPC_CHECK(std::fabs(budget.tokens() - 2.5) < 0.01);
}

CRPC_TEST(retry_non_retryable_codes_stop_immediately) {
    retry_policy policy;
    policy.max_attempts = 10;

    manual_clock clock;
    deterministic_rng rng(5);
    retry_sequence sequence(policy, nullptr, clock.source(),
                            &deterministic_rng::next_fn, &rng);

    std::chrono::nanoseconds delay{0};
    CRPC_CHECK_FALSE(sequence.next(rpc_code::canceled, true, delay));
    CRPC_CHECK_FALSE(sequence.next(rpc_code::invalid_argument, true, delay));
    CRPC_CHECK_FALSE(sequence.next(rpc_code::failed_precondition, true, delay));
    CRPC_CHECK_FALSE(sequence.next(rpc_code::not_found, true, delay));
    CRPC_CHECK_FALSE(sequence.next(rpc_code::no_such_method, true, delay));
    CRPC_CHECK_FALSE(sequence.next(rpc_code::deadline_exceeded, true, delay));
    CRPC_CHECK_EQ(sequence.attempts(), static_cast<u32>(1));
    CRPC_CHECK(sequence.predicate_denied());
}

CRPC_TEST(retry_unknown_side_effect_requires_idempotency) {
    retry_policy policy;
    policy.max_attempts = 5;
    policy.jitter_ratio = 0.0;

    manual_clock clock;
    deterministic_rng rng(9);
    retry_sequence sequence(policy, nullptr, clock.source(),
                            &deterministic_rng::next_fn, &rng);

    std::chrono::nanoseconds delay{0};
    CRPC_CHECK_FALSE(sequence.next(rpc_code::unknown_side_effect, false, delay));
    CRPC_CHECK(sequence.next(rpc_code::unknown_side_effect, true, delay));
    CRPC_CHECK_EQ(sequence.attempts(), static_cast<u32>(2));
}

CRPC_TEST(retry_transient_codes_are_retried) {
    const rpc_code codes[] = {
        rpc_code::unavailable,       rpc_code::connection_refused,
        rpc_code::connection_reset,  rpc_code::broken_pipe,
        rpc_code::eof,               rpc_code::overload,
        rpc_code::resource_exhausted, rpc_code::not_acknowledged,
    };

    manual_clock clock;
    deterministic_rng rng(13);
    for (rpc_code code : codes) {
        retry_policy policy;
        policy.max_attempts = 2;
        policy.jitter_ratio = 0.0;
        retry_sequence sequence(policy, nullptr, clock.source(),
                                &deterministic_rng::next_fn, &rng);
        std::chrono::nanoseconds delay{0};
        CRPC_CHECK(sequence.next(code, true, delay));
    }
}

CRPC_TEST(retry_custom_predicate) {
    retry_policy policy;
    policy.max_attempts = 2;
    policy.jitter_ratio = 0.0;
    policy.retryable = &always_retry;

    manual_clock clock;
    deterministic_rng rng(17);
    retry_sequence sequence(policy, nullptr, clock.source(),
                            &deterministic_rng::next_fn, &rng);

    std::chrono::nanoseconds delay{0};
    CRPC_CHECK(sequence.next(rpc_code::invalid_argument, false, delay));
}

// --- Load balancer -----------------------------------------------------------

CRPC_TEST(lb_round_robin_distributes_evenly) {
    vector_set set;
    set.items = {instance{1, 1, true, 0}, instance{2, 1, true, 0},
                 instance{3, 1, true, 0}};

    load_balancer balancer(lb_strategy::round_robin);
    int counts[4] = {0, 0, 0, 0};
    for (int i = 0; i < 9; ++i) {
        auto selected = balancer.select(set);
        CRPC_CHECK(selected.ok());
        ++counts[static_cast<std::size_t>(selected.value().id)];
    }
    CRPC_CHECK_EQ(counts[1], 3);
    CRPC_CHECK_EQ(counts[2], 3);
    CRPC_CHECK_EQ(counts[3], 3);
}

CRPC_TEST(lb_random_distributes) {
    vector_set set;
    set.items = {instance{1, 1, true, 0}, instance{2, 1, true, 0}};

    load_balancer balancer(lb_strategy::random, 160, 0x123456789ABCDEFull);
    int counts[3] = {0, 0, 0};
    for (int i = 0; i < 2000; ++i) {
        auto selected = balancer.select(set);
        CRPC_CHECK(selected.ok());
        ++counts[static_cast<std::size_t>(selected.value().id)];
    }
    CRPC_CHECK(counts[1] > 800);
    CRPC_CHECK(counts[1] < 1200);
    CRPC_CHECK(counts[2] > 800);
    CRPC_CHECK(counts[2] < 1200);
}

CRPC_TEST(lb_least_inflight) {
    vector_set set;
    set.items = {instance{1, 1, true, 7}, instance{2, 1, true, 2},
                 instance{3, 1, true, 5}};

    load_balancer balancer(lb_strategy::least_inflight);
    for (int i = 0; i < 10; ++i) {
        auto selected = balancer.select(set);
        CRPC_CHECK(selected.ok());
        CRPC_CHECK_EQ(selected.value().id, static_cast<u64>(2));
    }
}

CRPC_TEST(lb_weighted_round_robin) {
    vector_set set;
    set.items = {instance{1, 1, true, 0}, instance{2, 3, true, 0}};

    load_balancer balancer(lb_strategy::weighted_round_robin);
    int counts[3] = {0, 0, 0};
    for (int i = 0; i < 40; ++i) {
        auto selected = balancer.select(set);
        CRPC_CHECK(selected.ok());
        ++counts[static_cast<std::size_t>(selected.value().id)];
    }
    CRPC_CHECK_EQ(counts[1], 10);
    CRPC_CHECK_EQ(counts[2], 30);
}

CRPC_TEST(lb_unhealthy_skipped) {
    vector_set set;
    set.items = {instance{1, 1, true, 0}, instance{2, 1, false, 0},
                 instance{3, 1, true, 0}};

    load_balancer balancer(lb_strategy::round_robin);
    for (int i = 0; i < 20; ++i) {
        auto selected = balancer.select(set);
        CRPC_CHECK(selected.ok());
        CRPC_CHECK(selected.value().id != 2);
    }
}

CRPC_TEST(lb_all_unhealthy_unavailable) {
    vector_set set;
    set.items = {instance{1, 1, false, 0}, instance{2, 1, false, 0}};

    load_balancer balancer(lb_strategy::round_robin);
    auto selected = balancer.select(set);
    CRPC_CHECK_FALSE(selected.ok());
    CRPC_CHECK_EQ(code_id(selected.code()), code_id(rpc_code::unavailable));
}

CRPC_TEST(lb_consistent_hash_stability) {
    vector_set set;
    set.items = {instance{1, 1, true, 0}, instance{2, 1, true, 0},
                 instance{3, 1, true, 0}};

    load_balancer balancer(lb_strategy::consistent_hash, 160);
    std::vector<u64> before;
    before.reserve(1000);
    for (u64 key = 0; key < 1000; ++key) {
        auto selected = balancer.select(set, key);
        CRPC_CHECK(selected.ok());
        before.push_back(selected.value().id);
    }

    // Same key, same instance.
    for (u64 key = 0; key < 1000; key += 37) {
        auto selected = balancer.select(set, key);
        CRPC_CHECK_EQ(selected.value().id, before[static_cast<std::size_t>(key)]);
    }

    set.items.push_back(instance{4, 1, true, 0});
    ++set.version_value;

    int moved = 0;
    for (u64 key = 0; key < 1000; ++key) {
        auto selected = balancer.select(set, key);
        CRPC_CHECK(selected.ok());
        if (selected.value().id != before[static_cast<std::size_t>(key)]) {
            ++moved;
        }
    }
    CRPC_CHECK(moved > 100);
    CRPC_CHECK(moved < 400);
}

CRPC_TEST(lb_consistent_hash_distributes) {
    vector_set set;
    set.items = {instance{1, 1, true, 0}, instance{2, 1, true, 0},
                 instance{3, 1, true, 0}};

    load_balancer balancer(lb_strategy::consistent_hash, 160);
    int counts[4] = {0, 0, 0, 0};
    for (u64 key = 0; key < 1000; ++key) {
        auto selected = balancer.select(set, key);
        CRPC_CHECK(selected.ok());
        ++counts[static_cast<std::size_t>(selected.value().id)];
    }
    CRPC_CHECK(counts[1] > 150);
    CRPC_CHECK(counts[2] > 150);
    CRPC_CHECK(counts[3] > 150);
}

CRPC_TEST(lb_consistent_hash_skips_unhealthy) {
    vector_set set;
    set.items = {instance{1, 1, true, 0}, instance{2, 1, true, 0},
                 instance{3, 1, true, 0}};

    load_balancer balancer(lb_strategy::consistent_hash, 160);
    set.items[1].healthy = false;
    ++set.version_value;

    for (u64 key = 0; key < 500; ++key) {
        auto selected = balancer.select(set, key);
        CRPC_CHECK(selected.ok());
        CRPC_CHECK(selected.value().id != 2);
    }
}

// --- Concurrency -------------------------------------------------------------

CRPC_TEST(concurrent_breaker_allow) {
    manual_clock clock;
    breaker_config config;
    config.window = 30s;
    config.buckets = 10;
    config.min_requests = 1'000'000;
    config.failure_ratio_threshold = 1.0;

    circuit_breaker breaker(config, clock.source());
    std::atomic<int> failures{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&breaker, &failures]() {
            for (int i = 0; i < 2000; ++i) {
                auto permit = breaker.allow();
                if (!permit.allowed()) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if ((i & 1) == 0) {
                    permit.success();
                } else {
                    permit.failure();
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    CRPC_CHECK_EQ(failures.load(), 0);
    CRPC_CHECK_EQ(breaker.stats().total(), static_cast<u64>(16000));
}

CRPC_TEST(concurrent_limiter_try_acquire) {
    manual_clock clock;
    rate_limit_config config;
    config.rate_per_second = 1'000'000'000.0;
    config.burst = 1'000'000.0;

    rate_limiter limiter(config, clock.source());
    std::atomic<int> rejected{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&limiter, &rejected]() {
            for (int i = 0; i < 2000; ++i) {
                if (!limiter.try_acquire().ok()) {
                    rejected.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    CRPC_CHECK_EQ(rejected.load(), 0);
}

CRPC_TEST_MAIN()
