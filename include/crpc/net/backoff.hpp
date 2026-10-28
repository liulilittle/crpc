// CRPC -- reusable connect backoff and retry.
//
// The stream `connection` layer already reconnects, but its strategy is
// "retry on the next operation", and it applies only to one logical link. A
// connector-facing caller needs an *eager* retry loop with a delay it can
// observe and a bound it can budget for. `net::backoff` is that component:
// pure delay computation with exponential growth, a ceiling, and optional
// uniform jitter, with no hidden sleeps -- so it is directly unit-testable.
//
// `connect_with_retry` is the eager loop built on it.
#ifndef CRPC_NET_BACKOFF_HPP
#define CRPC_NET_BACKOFF_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/net/address.hpp>
#include <crpc/net/connector.hpp>

namespace crpc {
namespace net {

// Tunables for one backoff sequence.
struct backoff_config {
    // First delay; each subsequent delay doubles up to `ceiling`.
    std::chrono::nanoseconds base{std::chrono::milliseconds(50)};
    std::chrono::nanoseconds ceiling{std::chrono::seconds(2)};

    // Doubling factor, at least 1 (a cap of 1 disables growth).
    std::uint32_t multiplier = 2;

    // Uniform jitter as a fraction of the computed delay, in [0, 1). 0 = the
    // exact exponential sequence (used by tests asserting the doubling).
    double jitter = 0.0;

    // Number of delays delivered before `exhausted()` turns true. 0 = unlimited.
    std::size_t max_attempts = 0;
};

// Recommended default for client retry loops.
[[nodiscard]] inline backoff_config default_backoff_config() noexcept {
    backoff_config c;
    c.base = std::chrono::milliseconds(50);
    c.ceiling = std::chrono::seconds(2);
    c.multiplier = 2;
    c.jitter = 0.2;
    c.max_attempts = 0;
    return c;
}

// Undisturbed exponential delay sequence. Deterministic.
class CRPC_API backoff {
public:
    explicit backoff(const backoff_config& config) : backoff(config, 0x9e3779b9u) {}
    backoff(const backoff_config& config, std::uint32_t seed) noexcept;

    // The delay before the next attempt. First call returns `base`; the
    // sequence doubles (or clamps at `ceiling`) until `exhausted()`.
    [[nodiscard]] std::chrono::nanoseconds next_attempt_delay() noexcept;

    void reset() noexcept;
    [[nodiscard]] std::size_t attempts() const noexcept;
    [[nodiscard]] bool exhausted() const noexcept;
    [[nodiscard]] const backoff_config& config() const noexcept { return config_; }

private:
    std::uint32_t next_random() noexcept;

    backoff_config config_;
    std::size_t attempts_ = 0;
    std::uint32_t state_;
};

// What an eager retry loop actually did, for tests and dashboards.
struct retry_report {
    // Connect attempts made, including the successful one.
    std::size_t attempts = 0;
    // Delays the loop slept between failed attempts (length == failed attempts).
    std::vector<std::chrono::nanoseconds> delays;
    // The last error observed (the terminal error when the report ends in
    // failure).
    rpc_code last_error = rpc_code::ok;
};

// Connects with exponential backoff + jitter between attempts. `max_attempts`
// in `cfg` (1 or more) bounds the attempts; unlimited runs until `connect()`
// succeeds. The delay each failure slept is recorded in `report` when given.
[[nodiscard]] CRPC_API result<std::unique_ptr<tcp_stream>> connect_with_retry(
    const address& target, const connect_options& options,
    const backoff_config& cfg, retry_report* report = nullptr);

} // namespace net
} // namespace crpc

#endif // CRPC_NET_BACKOFF_HPP