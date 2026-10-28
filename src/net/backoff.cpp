// CRPC -- reusable connect backoff and the eager retry loop.
#include <crpc/net/backoff.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

namespace crpc {
namespace net {

backoff::backoff(const backoff_config& config, std::uint32_t seed) noexcept
    : config_(config), state_(seed == 0 ? 0x9e3779b9u : seed) {
    if (config_.multiplier < 1) {
        config_.multiplier = 1;
    }
    if (config_.ceiling < config_.base) {
        config_.ceiling = config_.base;
    }
    if (config_.jitter < 0.0) {
        config_.jitter = 0.0;
    }
    if (config_.jitter > 1.0) {
        config_.jitter = 1.0;
    }
}

std::uint32_t backoff::next_random() noexcept {
    // xorshift32: tiny, fast, and deterministic for a given seed.
    std::uint32_t value = state_;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    state_ = value;
    return value;
}

std::chrono::nanoseconds backoff::next_attempt_delay() noexcept {
    std::chrono::nanoseconds delay = config_.base;
    if (config_.multiplier > 1) {
        for (std::size_t i = 0; i < attempts_; ++i) {
            if (delay > config_.ceiling / config_.multiplier) {
                delay = config_.ceiling;
                break;
            }
            delay *= config_.multiplier;
        }
    }
    ++attempts_;
    if (config_.jitter > 0.0) {
        const double unit = static_cast<double>(next_random()) / 4294967296.0;
        const double factor = 1.0 + config_.jitter * (2.0 * unit - 1.0);
        delay = std::chrono::nanoseconds(
            static_cast<std::chrono::nanoseconds::rep>(
                static_cast<double>(delay.count()) * factor));
    }
    return delay;
}

void backoff::reset() noexcept { attempts_ = 0; }
std::size_t backoff::attempts() const noexcept { return attempts_; }

bool backoff::exhausted() const noexcept {
    return config_.max_attempts != 0 && attempts_ >= config_.max_attempts;
}

result<std::unique_ptr<tcp_stream>> connect_with_retry(
    const address& target, const connect_options& options,
    const backoff_config& cfg, retry_report* report) {
    retry_report local;
    retry_report& rep = report != nullptr ? *report : local;
    rep.attempts = 0;
    rep.delays.clear();
    rep.last_error = rpc_code::ok;

    backoff schedule(cfg);
    for (;;) {
        result<std::unique_ptr<tcp_stream>> attempt = connect(target, options);
        ++rep.attempts;
        if (attempt) {
            rep.last_error = rpc_code::ok;
            return attempt;
        }
        rep.last_error = attempt.code();
        if (cfg.max_attempts != 0 && rep.attempts >= cfg.max_attempts) {
            return attempt;
        }
        const std::chrono::nanoseconds delay = schedule.next_attempt_delay();
        rep.delays.push_back(delay);
        std::this_thread::sleep_for(delay);
    }
}

} // namespace net
} // namespace crpc