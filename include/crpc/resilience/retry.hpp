// CRPC -- retry policy, exponential backoff and retry budget.
//
// Two pieces cooperate:
//   * `retry_sequence` is a per-logical-call decision object. It owns the
//     attempt counter and computes the next delay with exponential backoff and
//     jitter. It is intentionally not thread-safe: one sequence belongs to one
//     call.
//   * `retry_budget` is a shared, thread-safe token bucket. Every logical call
//     deposits `ratio` tokens and every retry withdraws one token, so retries
//     can never exceed a fraction of total traffic. Tokens decay exponentially
//     (half-life `decay_interval`) so a burst of traffic cannot accumulate a
//     permanent retry allowance.
//
// Thread safety:
//   * `retry_budget` is safe for concurrent use from any number of threads.
//   * `retry_sequence` must be used by a single logical call at a time.
//   * The injected RNG function must be thread-safe for its own context.
#ifndef CRPC_RESILIENCE_RETRY_HPP
#define CRPC_RESILIENCE_RETRY_HPP

#include <chrono>
#include <mutex>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/resilience/circuit_breaker.hpp>

namespace crpc::resilience {

// A uniform 64-bit random source. The context is owned by the caller.
using random_fn = u64 (*)(void* context) noexcept;

// Deterministic splitmix64 generator. Useful in tests and as the default jitter
// source when no external RNG is supplied.
//
// Thread safety: not thread-safe; use one generator per retry sequence.
class deterministic_rng {
public:
    explicit deterministic_rng(u64 seed) noexcept : state_(seed) {}

    [[nodiscard]] u64 next() noexcept {
        u64 z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    [[nodiscard]] u64 state() const noexcept { return state_; }

    static u64 next_fn(void* context) noexcept {
        return static_cast<deterministic_rng*>(context)->next();
    }

private:
    u64 state_;
};

// A retry predicate decides whether a failure is worth retrying. It must be
// noexcept and must not block. `idempotent` is the caller's explicit statement
// about the operation; it is required to retry `unknown_side_effect`.
using retry_predicate = bool (*)(rpc_code code, bool idempotent,
                                 void* context) noexcept;

// Default policy: transient transport codes are retryable; caller errors and
// `canceled` are not; `unknown_side_effect` is retryable only when idempotent.
[[nodiscard]] CRPC_API bool default_retryable(rpc_code code, bool idempotent) noexcept;

// Thread safety: a plain value type; copy it before sharing between threads.
struct retry_policy {
    // Total attempts including the first one. Clamped to at least 1.
    u32 max_attempts{3};
    std::chrono::nanoseconds initial_backoff{std::chrono::milliseconds(2)};
    std::chrono::nanoseconds max_backoff{std::chrono::milliseconds(200)};
    double multiplier{2.0};
    // Fraction of the computed backoff that may be removed. 0 disables jitter;
    // 1 yields full jitter in [0, backoff]. The delay is always in
    // [backoff * (1 - jitter_ratio), backoff].
    double jitter_ratio{0.2};
    // Fraction of calls that may be retried. Use `budget_config_from()` to
    // build the shared `retry_budget` that `retry_sequence` consumes.
    double retry_budget{0.1};
    retry_predicate retryable{nullptr};
    void* retryable_context{nullptr};
};

// Compatibility alias used by the design document.
using retry_config = retry_policy;

// Thread safety: a plain value type; copy it before sharing between threads.
struct retry_budget_config {
    // Tokens deposited per logical call. 0 disables retries entirely when the
    // budget is enabled.
    double ratio{0.1};
    // Maximum accumulated tokens.
    double capacity{100.0};
    // Token half-life. Zero disables decay.
    std::chrono::seconds decay_interval{std::chrono::seconds(10)};
};

// Builds a shared-budget configuration from the policy's `retry_budget` ratio.
[[nodiscard]] inline retry_budget_config budget_config_from(
    const retry_policy& policy) noexcept {
    retry_budget_config config;
    config.ratio = policy.retry_budget;
    return config;
}

// Shared retry budget. Starts empty; call `on_request()` once per logical call.
class CRPC_API retry_budget {
public:
    explicit retry_budget(const retry_budget_config& config,
                          clock_source clock = monotonic_clock()) noexcept;

    retry_budget(const retry_budget&) = delete;
    retry_budget& operator=(const retry_budget&) = delete;
    retry_budget(retry_budget&&) = delete;
    retry_budget& operator=(retry_budget&&) = delete;

    // Deposits one call's share of tokens.
    void on_request() noexcept;

    // Withdraws one retry token. Returns false when the budget is exhausted.
    [[nodiscard]] bool try_acquire() noexcept;

    [[nodiscard]] double tokens() noexcept;

    [[nodiscard]] const retry_budget_config& config() const noexcept { return config_; }

private:
    void apply_decay_locked(u64 now_ns) noexcept;

    std::mutex mutex_;
    retry_budget_config config_;
    clock_source clock_;
    double tokens_ = 0.0;
    u64 last_ns_ = 0;
};

// Per-call retry state machine.
class CRPC_API [[nodiscard]] retry_sequence {
public:
    // `budget` may be null, in which case the storm guard is disabled for this
    // call. Production callers should pass a shared budget.
    explicit retry_sequence(const retry_policy& policy, retry_budget* budget = nullptr,
                            clock_source clock = monotonic_clock(),
                            random_fn random = nullptr,
                            void* random_context = nullptr) noexcept;

    retry_sequence(const retry_sequence&) = delete;
    retry_sequence& operator=(const retry_sequence&) = delete;

    // Call after an attempt failed. Returns true and writes the delay when
    // another attempt is allowed. Returns false when the attempt limit, the
    // retry predicate or the retry budget refuses the retry.
    [[nodiscard]] bool next(rpc_code code, bool idempotent,
                            std::chrono::nanoseconds& delay) noexcept;

    // Deadline- and hint-aware variant. `min_delay` is a lower bound supplied
    // by the failure (for example a server `retry_after`). `deadline` is an
    // absolute monotonic timestamp used when `has_deadline` is true. The
    // deadline check runs before the budget is charged, so a retry that cannot
    // fit does not consume a token.
    [[nodiscard]] bool next(rpc_code code, bool idempotent,
                            std::chrono::nanoseconds min_delay,
                            std::chrono::nanoseconds deadline, bool has_deadline,
                            std::chrono::nanoseconds& delay) noexcept;

    [[nodiscard]] u32 attempts() const noexcept { return attempts_; }
    [[nodiscard]] bool budget_denied() const noexcept { return budget_denied_; }
    [[nodiscard]] bool predicate_denied() const noexcept { return predicate_denied_; }

private:
    [[nodiscard]] bool is_retryable(rpc_code code, bool idempotent) const noexcept;
    [[nodiscard]] std::chrono::nanoseconds compute_backoff() noexcept;
    [[nodiscard]] double uniform_unit() noexcept;

    retry_policy policy_;
    retry_budget* budget_ = nullptr;
    clock_source clock_;
    random_fn random_ = nullptr;
    void* random_context_ = nullptr;
    deterministic_rng internal_rng_;
    u32 attempts_ = 1;
    bool budget_denied_ = false;
    bool predicate_denied_ = false;
};

} // namespace crpc::resilience

#endif // CRPC_RESILIENCE_RETRY_HPP
