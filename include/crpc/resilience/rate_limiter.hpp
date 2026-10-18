// CRPC -- token-bucket rate limiter.
//
// The bucket is guarded by a small mutex. A single 64-bit word cannot hold both
// a full-width token count and an absolute monotonic nanosecond timestamp: the
// truncated timestamp makes refill arithmetic wrong after a long idle period.
// The critical section is a handful of arithmetic operations and the public API
// never blocks, so the lock is not on a blocking path.
//
// Thread safety:
//   * `rate_limiter` is safe for concurrent use from any number of threads.
//   * `try_acquire()` and `acquire_for()` are non-blocking. They never sleep;
//     `acquire_for()` reports whether the required delay fits the deadline.
//   * `retry_after` is always positive on rejection and is capped at one hour.
#ifndef CRPC_RESILIENCE_RATE_LIMITER_HPP
#define CRPC_RESILIENCE_RATE_LIMITER_HPP

#include <chrono>
#include <mutex>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/resilience/circuit_breaker.hpp>

namespace crpc::resilience {

// Thread safety: a plain value type; copy it before sharing between threads.
struct rate_limit_config {
    // Sustained refill rate in tokens per second. Zero disables refill.
    double rate_per_second{1000.0};
    // Maximum bucket depth (the burst size). Clamped to at least 1.
    double burst{1000.0};
    // Starting tokens. A negative value means "start full".
    double initial_tokens{-1.0};
};

// Thread safety: a plain value type.
struct rate_limit_result {
    rpc_code code = rpc_code::ok;
    // Minimum delay before the request could succeed. Positive on rejection.
    std::chrono::milliseconds retry_after{0};
    // False only when an absolute deadline was supplied and `retry_after`
    // would cross it. The code remains `resource_exhausted` in that case.
    bool deadline_met = true;

    [[nodiscard]] bool ok() const noexcept { return code == rpc_code::ok; }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
};

class rate_limiter {
public:
    explicit rate_limiter(const rate_limit_config& config,
                          clock_source clock = monotonic_clock()) noexcept;

    rate_limiter(const rate_limiter&) = delete;
    rate_limiter& operator=(const rate_limiter&) = delete;
    rate_limiter(rate_limiter&&) = delete;
    rate_limiter& operator=(rate_limiter&&) = delete;

    // Non-blocking. Returns `rpc_code::ok` when `tokens` were taken, otherwise
    // `rpc_code::resource_exhausted` with a positive `retry_after`. A request
    // larger than `burst` can never succeed because the bucket never holds more
    // than `burst` tokens.
    [[nodiscard]] rate_limit_result try_acquire(u64 tokens = 1) noexcept;

    // Non-blocking deadline-aware variant. `deadline_ns` is an absolute
    // monotonic timestamp. The request is admitted only if tokens are available
    // now; on rejection `deadline_met` reports whether waiting `retry_after`
    // would still meet the deadline.
    [[nodiscard]] rate_limit_result acquire_for(
        u64 tokens, std::chrono::nanoseconds deadline_ns) noexcept;

    // Current token count after applying refill. Intended for observability.
    [[nodiscard]] double tokens() noexcept;

    [[nodiscard]] const rate_limit_config& config() const noexcept { return config_; }

private:
    rate_limit_result attempt_locked(u64 tokens, u64 now, bool has_deadline,
                                     u64 deadline_ns) noexcept;

    std::mutex mutex_;
    rate_limit_config config_;
    clock_source clock_;
    double tokens_ = 0.0;
    u64 last_ns_ = 0;
};

} // namespace crpc::resilience

#endif // CRPC_RESILIENCE_RATE_LIMITER_HPP
