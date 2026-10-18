#include <crpc/resilience/retry.hpp>

#include <cmath>

namespace crpc::resilience {

namespace {

constexpr double k_uniform_scale = 1.0 / 9007199254740992.0; // 2^53
constexpr double k_budget_epsilon = 1e-9;

double clamp_unit(double value) noexcept {
    if (!(value >= 0.0)) {
        return 0.0;
    }
    return value > 1.0 ? 1.0 : value;
}

} // namespace

bool default_retryable(rpc_code code, bool idempotent) noexcept {
    if (code == rpc_code::unknown_side_effect) {
        return idempotent;
    }
    return is_retryable(code);
}

// --- Retry budget ------------------------------------------------------------

retry_budget::retry_budget(const retry_budget_config& config,
                           clock_source clock) noexcept
    : config_(config), clock_(clock) {
    if (!(config_.ratio >= 0.0)) {
        config_.ratio = 0.0;
    }
    if (!(config_.capacity >= 1.0)) {
        config_.capacity = 1.0;
    }
    if (config_.decay_interval.count() < 0) {
        config_.decay_interval = std::chrono::seconds(0);
    }
    last_ns_ = clock_.now_ns();
}

void retry_budget::apply_decay_locked(u64 now_ns) noexcept {
    if (now_ns <= last_ns_) {
        last_ns_ = now_ns;
        return;
    }

    const u64 elapsed_ns = now_ns - last_ns_;
    last_ns_ = now_ns;

    const auto interval_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(config_.decay_interval)
            .count();
    if (interval_ns <= 0 || tokens_ <= 0.0) {
        return;
    }

    const double half_lives =
        static_cast<double>(elapsed_ns) / static_cast<double>(interval_ns);
    tokens_ *= std::pow(0.5, half_lives);
    if (tokens_ < 0.0) {
        tokens_ = 0.0;
    }
}

void retry_budget::on_request() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    apply_decay_locked(clock_.now_ns());
    tokens_ += config_.ratio;
    if (tokens_ > config_.capacity) {
        tokens_ = config_.capacity;
    }
}

bool retry_budget::try_acquire() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    apply_decay_locked(clock_.now_ns());
    if (tokens_ + k_budget_epsilon < 1.0) {
        return false;
    }
    tokens_ -= 1.0;
    if (tokens_ < 0.0) {
        tokens_ = 0.0;
    }
    return true;
}

double retry_budget::tokens() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    apply_decay_locked(clock_.now_ns());
    return tokens_;
}

// --- Retry sequence ----------------------------------------------------------

retry_sequence::retry_sequence(const retry_policy& policy, retry_budget* budget,
                               clock_source clock, random_fn random,
                               void* random_context) noexcept
    : policy_(policy),
      budget_(budget),
      random_(random),
      random_context_(random_context),
      internal_rng_(clock.now_ns() ^ 0x9E3779B97F4A7C15ull) {
    if (policy_.max_attempts == 0) {
        policy_.max_attempts = 1;
    }
    if (policy_.initial_backoff.count() < 0) {
        policy_.initial_backoff = std::chrono::nanoseconds(0);
    }
    if (policy_.max_backoff.count() < 0) {
        policy_.max_backoff = std::chrono::nanoseconds(0);
    }
    if (!(policy_.multiplier >= 1.0)) {
        policy_.multiplier = 1.0;
    }
    policy_.jitter_ratio = clamp_unit(policy_.jitter_ratio);

    if (internal_rng_.state() == 0) {
        // splitmix64 never produces a zero-state problem, but keeping the seed
        // non-zero makes the generated stream easy to reason about.
        internal_rng_ = deterministic_rng(0xA5A5A5A5A5A5A5A5ull);
    }

    if (budget_ != nullptr) {
        budget_->on_request();
    }
}

bool retry_sequence::is_retryable(rpc_code code, bool idempotent) const noexcept {
    if (policy_.retryable != nullptr) {
        return policy_.retryable(code, idempotent, policy_.retryable_context);
    }
    return default_retryable(code, idempotent);
}

bool retry_sequence::next(rpc_code code, bool idempotent,
                          std::chrono::nanoseconds& delay) noexcept {
    if (attempts_ >= policy_.max_attempts) {
        return false;
    }
    if (!is_retryable(code, idempotent)) {
        predicate_denied_ = true;
        return false;
    }
    if (budget_ != nullptr && !budget_->try_acquire()) {
        budget_denied_ = true;
        return false;
    }

    delay = compute_backoff();
    ++attempts_;
    return true;
}

double retry_sequence::uniform_unit() noexcept {
    const u64 value = random_ != nullptr ? random_(random_context_) : internal_rng_.next();
    return static_cast<double>(value >> 11) * k_uniform_scale;
}

std::chrono::nanoseconds retry_sequence::compute_backoff() noexcept {
    const double initial =
        static_cast<double>(policy_.initial_backoff.count());
    const double maximum = static_cast<double>(policy_.max_backoff.count());

    const u32 exponent = attempts_ > 0 ? attempts_ - 1 : 0;
    const double factor =
        exponent == 0 ? 1.0
                      : std::pow(policy_.multiplier, static_cast<double>(exponent));

    double backoff = initial * factor;
    if (!(backoff <= maximum)) {
        // Also catches overflow to infinity.
        backoff = maximum;
    }
    if (backoff < 0.0) {
        backoff = 0.0;
    }

    if (policy_.jitter_ratio > 0.0 && backoff > 0.0) {
        const double lower = backoff * (1.0 - policy_.jitter_ratio);
        backoff = lower + uniform_unit() * (backoff - lower);
        if (backoff > maximum) {
            backoff = maximum;
        }
    }

    return std::chrono::nanoseconds(static_cast<i64>(backoff));
}

} // namespace crpc::resilience

