#include <crpc/resilience/rate_limiter.hpp>

namespace crpc::resilience {

namespace {

constexpr double k_nanoseconds_per_second = 1'000'000'000.0;
constexpr double k_max_retry_after_ns = 3'600.0 * k_nanoseconds_per_second;

} // namespace

rate_limiter::rate_limiter(const rate_limit_config& config,
                           clock_source clock) noexcept
    : config_(config), clock_(clock) {
    if (!(config_.rate_per_second >= 0.0)) {
        config_.rate_per_second = 0.0;
    }
    if (!(config_.burst >= 1.0)) {
        config_.burst = 1.0;
    }

    if (!(config_.initial_tokens >= 0.0)) {
        tokens_ = config_.burst;
    } else {
        tokens_ = config_.initial_tokens;
    }
    if (tokens_ > config_.burst) {
        tokens_ = config_.burst;
    }
    if (tokens_ < 0.0) {
        tokens_ = 0.0;
    }
    last_ns_ = clock_.now_ns();
}

rate_limit_result rate_limiter::try_acquire(u64 tokens) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return attempt_locked(tokens, clock_.now_ns(), false, 0);
}

rate_limit_result rate_limiter::acquire_for(u64 tokens,
                                            std::chrono::nanoseconds deadline_ns) noexcept {
    const auto count = deadline_ns.count();
    const u64 deadline = count > 0 ? static_cast<u64>(count) : 0;
    std::lock_guard<std::mutex> lock(mutex_);
    return attempt_locked(tokens, clock_.now_ns(), true, deadline);
}

double rate_limiter::tokens() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    const u64 now = clock_.now_ns();
    const double elapsed =
        now > last_ns_ ? static_cast<double>(now - last_ns_) : 0.0;
    last_ns_ = now;
    if (elapsed > 0.0 && config_.rate_per_second > 0.0) {
        tokens_ += elapsed * config_.rate_per_second / k_nanoseconds_per_second;
        if (tokens_ > config_.burst) {
            tokens_ = config_.burst;
        }
    }
    return tokens_;
}

rate_limit_result rate_limiter::attempt_locked(u64 tokens, u64 now,
                                               bool has_deadline,
                                               u64 deadline_ns) noexcept {
    const double elapsed =
        now > last_ns_ ? static_cast<double>(now - last_ns_) : 0.0;
    last_ns_ = now;

    if (elapsed > 0.0 && config_.rate_per_second > 0.0) {
        tokens_ += elapsed * config_.rate_per_second / k_nanoseconds_per_second;
        if (tokens_ > config_.burst) {
            tokens_ = config_.burst;
        }
    }

    if (tokens == 0) {
        return rate_limit_result{};
    }

    const double requested = static_cast<double>(tokens);
    if (tokens_ >= requested) {
        tokens_ -= requested;
        return rate_limit_result{};
    }

    const double deficit = requested - tokens_;
    double wait_ns = config_.rate_per_second > 0.0
                         ? deficit / config_.rate_per_second * k_nanoseconds_per_second
                         : k_max_retry_after_ns;
    if (!(wait_ns > 0.0)) {
        wait_ns = 1.0;
    }
    if (wait_ns > k_max_retry_after_ns) {
        wait_ns = k_max_retry_after_ns;
    }

    u64 wait = static_cast<u64>(wait_ns);
    if (wait == 0) {
        wait = 1;
    }
    u64 milliseconds = (wait + 999'999) / 1'000'000;
    if (milliseconds == 0) {
        milliseconds = 1;
    }

    rate_limit_result result;
    result.code = rpc_code::resource_exhausted;
    result.retry_after = std::chrono::milliseconds(milliseconds);
    if (has_deadline) {
        const u64 remaining = deadline_ns > now ? deadline_ns - now : 0;
        result.deadline_met = wait <= remaining;
    }
    return result;
}

} // namespace crpc::resilience
