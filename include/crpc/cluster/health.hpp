// CRPC -- health checkers.
//
// Three checker kinds share one state machine:
//
//   * ttl_health_check       -- the client reports pass/fail heartbeats;
//   * tcp_health_check       -- an injected TCP connect probe runs on an
//                               interval (the I/O backend owns the actual
//                               socket; a null probe is fail-closed);
//   * callback_health_check  -- an injected callback runs on an interval.
//
// Aggregation is worst-of: passing < warning < critical. A checker recovers
// from critical only after success_threshold consecutive passes (default 2),
// which suppresses flapping. A failure is immediate (failure_threshold default
// 1). All time comes from the injected monotonic clock.
//
// Concurrency contract
// --------------------
//   * health_check and health_monitor are not thread-safe; one owner thread
//     drives them. A checker must not be shared between monitors.
//   * Probe callbacks are invoked synchronously from poll() and must be fast
//     and non-blocking. A network probe belongs in the I/O backend; this layer
//     only records its result.
#ifndef CRPC_CLUSTER_HEALTH_HPP
#define CRPC_CLUSTER_HEALTH_HPP

#include <chrono>
#include <cstddef>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include <crpc/cluster/registry.hpp>

namespace crpc {
namespace cluster {

enum class health_state : u8 {
    passing = 0,
    warning = 1,
    critical = 2
};

[[nodiscard]] constexpr u8 health_rank(health_state state) noexcept {
    return static_cast<u8>(state);
}

[[nodiscard]] constexpr health_state worst_of(health_state a, health_state b) noexcept {
    return health_rank(a) >= health_rank(b) ? a : b;
}

[[nodiscard]] constexpr instance_state to_instance_state(health_state state) noexcept {
    switch (state) {
    case health_state::passing:  return instance_state::passing;
    case health_state::warning:  return instance_state::warning;
    case health_state::critical: return instance_state::critical;
    }
    return instance_state::critical;
}

using tcp_connect_probe = bool (*)(void* context, const endpoint& address) noexcept;
using callback_probe = bool (*)(void* context) noexcept;

#if defined(CRPC_COMPILER_MSVC)
// Private STL members are an implementation detail; the exported ABI is the
// method set. C4251 would otherwise reject every class with a std:: member in
// a shared build.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
class CRPC_API health_check {
public:
    health_check(std::string id, u32 success_threshold = 2,
                 u32 failure_threshold = 1);
    virtual ~health_check() = default;

    health_check(const health_check&) = delete;
    health_check& operator=(const health_check&) = delete;

    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    [[nodiscard]] health_state state() const noexcept { return state_; }
    [[nodiscard]] u32 consecutive_successes() const noexcept {
        return consecutive_successes_;
    }
    [[nodiscard]] u32 consecutive_failures() const noexcept {
        return consecutive_failures_;
    }

    // Manual pass/fail report (TTL heartbeats use this).
    void report(bool pass, monotonic_clock::time_point now) noexcept;

    // Active checks override this to run their probe. The default returns the
    // current state without side effects.
    [[nodiscard]] virtual health_state poll(monotonic_clock::time_point now);

protected:
    void record(bool pass, monotonic_clock::time_point now) noexcept;
    void force_state(health_state state, monotonic_clock::time_point now) noexcept;

    std::string id_;
    health_state state_ = health_state::passing;
    u32 success_threshold_ = 1;
    u32 failure_threshold_ = 1;
    u32 consecutive_successes_ = 0;
    u32 consecutive_failures_ = 0;
    monotonic_clock::time_point last_change_;
};

class CRPC_API ttl_health_check final : public health_check {
public:
    ttl_health_check(std::string id, std::chrono::milliseconds ttl,
                     monotonic_clock::time_point start,
                     u32 success_threshold = 2);

    void heartbeat(bool pass, monotonic_clock::time_point now) noexcept;
    [[nodiscard]] health_state poll(monotonic_clock::time_point now) override;
    [[nodiscard]] std::chrono::milliseconds ttl() const noexcept { return ttl_; }

private:
    std::chrono::milliseconds ttl_;
    monotonic_clock::time_point start_;
    monotonic_clock::time_point last_report_;
    bool has_report_ = false;
};

class CRPC_API tcp_health_check final : public health_check {
public:
    tcp_health_check(std::string id, endpoint address,
                     std::chrono::milliseconds interval,
                     tcp_connect_probe probe, void* context,
                     monotonic_clock::time_point start,
                     u32 success_threshold = 2);

    [[nodiscard]] health_state poll(monotonic_clock::time_point now) override;
    [[nodiscard]] const endpoint& address() const noexcept { return address_; }

private:
    endpoint address_;
    std::chrono::milliseconds interval_;
    tcp_connect_probe probe_ = nullptr;
    void* context_ = nullptr;
    monotonic_clock::time_point last_probe_;
    bool has_probed_ = false;
};

class CRPC_API callback_health_check final : public health_check {
public:
    callback_health_check(std::string id, callback_probe probe, void* context,
                          std::chrono::milliseconds interval,
                          monotonic_clock::time_point start,
                          u32 success_threshold = 2);

    [[nodiscard]] health_state poll(monotonic_clock::time_point now) override;

private:
    callback_probe probe_ = nullptr;
    void* context_ = nullptr;
    std::chrono::milliseconds interval_;
    monotonic_clock::time_point last_probe_;
    bool has_probed_ = false;
};

struct health_config {
    std::size_t max_checks = 64;
};

// Owns a bounded set of checks and aggregates them worst-of. add_* returns
// nullptr when the bound is reached or allocation fails.
class CRPC_API health_monitor {
public:
    explicit health_monitor(monotonic_clock clock,
                            health_config config = health_config{});

    health_monitor(const health_monitor&) = delete;
    health_monitor& operator=(const health_monitor&) = delete;

    [[nodiscard]] ttl_health_check*
    add_ttl(const std::string& id, std::chrono::milliseconds ttl,
            u32 success_threshold = 2);

    [[nodiscard]] tcp_health_check*
    add_tcp(const std::string& id, endpoint address,
            std::chrono::milliseconds interval, tcp_connect_probe probe,
            void* context, u32 success_threshold = 2);

    [[nodiscard]] callback_health_check*
    add_callback(const std::string& id, callback_probe probe, void* context,
                 std::chrono::milliseconds interval, u32 success_threshold = 2);

    [[nodiscard]] health_state poll();
    [[nodiscard]] health_state poll(monotonic_clock::time_point now);
    [[nodiscard]] health_state state() const noexcept { return aggregate_; }
    [[nodiscard]] std::size_t size() const noexcept { return checks_.size(); }
    [[nodiscard]] health_check* find(const std::string& id) noexcept;

private:
    [[nodiscard]] bool can_add() const noexcept;

    monotonic_clock clock_;
    health_config config_;
    std::vector<std::unique_ptr<health_check>> checks_;
    health_state aggregate_ = health_state::passing;
};

#if defined(CRPC_COMPILER_MSVC)
#pragma warning(pop)
#endif
} // namespace cluster
} // namespace crpc

#endif // CRPC_CLUSTER_HEALTH_HPP
