// CRPC -- health checkers implementation.
#include <crpc/cluster/health.hpp>

#include <limits>
#include <utility>

namespace crpc {
namespace cluster {

health_check::health_check(std::string id, u32 success_threshold,
                           u32 failure_threshold)
    : id_(std::move(id)),
      success_threshold_(success_threshold == 0 ? 1 : success_threshold),
      failure_threshold_(failure_threshold == 0 ? 1 : failure_threshold) {}

void health_check::report(bool pass, monotonic_clock::time_point now) noexcept {
    record(pass, now);
}

health_state health_check::poll(monotonic_clock::time_point) {
    return state_;
}

void health_check::record(bool pass, monotonic_clock::time_point now) noexcept {
    if (pass) {
        if (consecutive_successes_ != std::numeric_limits<u32>::max()) {
            ++consecutive_successes_;
        }
        consecutive_failures_ = 0;
        if (state_ != health_state::passing &&
            consecutive_successes_ >= success_threshold_) {
            state_ = health_state::passing;
            last_change_ = now;
        }
        return;
    }

    if (consecutive_failures_ != std::numeric_limits<u32>::max()) {
        ++consecutive_failures_;
    }
    consecutive_successes_ = 0;
    if (consecutive_failures_ >= failure_threshold_) {
        state_ = health_state::critical;
        last_change_ = now;
    }
}

void health_check::force_state(health_state state,
                               monotonic_clock::time_point now) noexcept {
    if (state_ != state) {
        state_ = state;
        last_change_ = now;
    }
}

ttl_health_check::ttl_health_check(std::string id, std::chrono::milliseconds ttl,
                                   monotonic_clock::time_point start,
                                   u32 success_threshold)
    : health_check(std::move(id), success_threshold),
      ttl_(ttl.count() > 0 ? ttl : std::chrono::milliseconds{1}),
      start_(start) {}

void ttl_health_check::heartbeat(bool pass,
                                 monotonic_clock::time_point now) noexcept {
    last_report_ = now;
    has_report_ = true;
    record(pass, now);
}

health_state ttl_health_check::poll(monotonic_clock::time_point now) {
    const monotonic_clock::time_point reference = has_report_ ? last_report_ : start_;
    const std::chrono::milliseconds age =
        now > reference ? std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - reference)
                        : std::chrono::milliseconds{0};

    if (age > ttl_) {
        record(false, now);
    } else if (age > ttl_ / 2 && state_ != health_state::critical) {
        // Approaching expiry is a warning, not a failure. A critical state from
        // an explicit fail report is never softened by the passage of time.
        force_state(health_state::warning, now);
    }
    return state_;
}

tcp_health_check::tcp_health_check(std::string id, endpoint address,
                                   std::chrono::milliseconds interval,
                                   tcp_connect_probe probe, void* context,
                                   monotonic_clock::time_point start,
                                   u32 success_threshold)
    : health_check(std::move(id), success_threshold),
      address_(std::move(address)),
      interval_(interval),
      probe_(probe),
      context_(context),
      last_probe_(start) {}

health_state tcp_health_check::poll(monotonic_clock::time_point now) {
    if (has_probed_ && interval_.count() > 0 && now < last_probe_ + interval_) {
        return state_;
    }
    last_probe_ = now;
    has_probed_ = true;
    const bool pass = probe_ != nullptr && probe_(context_, address_);
    record(pass, now);
    return state_;
}

callback_health_check::callback_health_check(std::string id, callback_probe probe,
                                             void* context,
                                             std::chrono::milliseconds interval,
                                             monotonic_clock::time_point start,
                                             u32 success_threshold)
    : health_check(std::move(id), success_threshold),
      probe_(probe),
      context_(context),
      interval_(interval),
      last_probe_(start) {}

health_state callback_health_check::poll(monotonic_clock::time_point now) {
    if (has_probed_ && interval_.count() > 0 && now < last_probe_ + interval_) {
        return state_;
    }
    last_probe_ = now;
    has_probed_ = true;
    const bool pass = probe_ != nullptr && probe_(context_);
    record(pass, now);
    return state_;
}

health_monitor::health_monitor(monotonic_clock clock, health_config config)
    : clock_(clock), config_(std::move(config)) {
    if (clock_.now == nullptr) {
        clock_ = system_monotonic_clock();
    }
    if (config_.max_checks != 0) {
        checks_.reserve(config_.max_checks);
    }
}

bool health_monitor::can_add() const noexcept {
    return config_.max_checks == 0 || checks_.size() < config_.max_checks;
}

ttl_health_check* health_monitor::add_ttl(const std::string& id,
                                          std::chrono::milliseconds ttl,
                                          u32 success_threshold) {
    if (!can_add()) {
        return nullptr;
    }
    std::unique_ptr<ttl_health_check> check(new (std::nothrow)
                                               ttl_health_check(id, ttl,
                                                                clock_.read(),
                                                                success_threshold));
    if (!check) {
        return nullptr;
    }
    ttl_health_check* raw = check.get();
    checks_.push_back(std::move(check));
    return raw;
}

tcp_health_check* health_monitor::add_tcp(const std::string& id, endpoint address,
                                          std::chrono::milliseconds interval,
                                          tcp_connect_probe probe, void* context,
                                          u32 success_threshold) {
    if (!can_add()) {
        return nullptr;
    }
    std::unique_ptr<tcp_health_check> check(new (std::nothrow)
                                               tcp_health_check(id, std::move(address),
                                                                interval, probe, context,
                                                                clock_.read(),
                                                                success_threshold));
    if (!check) {
        return nullptr;
    }
    tcp_health_check* raw = check.get();
    checks_.push_back(std::move(check));
    return raw;
}

callback_health_check* health_monitor::add_callback(const std::string& id,
                                                    callback_probe probe,
                                                    void* context,
                                                    std::chrono::milliseconds interval,
                                                    u32 success_threshold) {
    if (!can_add()) {
        return nullptr;
    }
    std::unique_ptr<callback_health_check> check(
        new (std::nothrow) callback_health_check(id, probe, context, interval,
                                                 clock_.read(), success_threshold));
    if (!check) {
        return nullptr;
    }
    callback_health_check* raw = check.get();
    checks_.push_back(std::move(check));
    return raw;
}

health_state health_monitor::poll() {
    return poll(clock_.read());
}

health_state health_monitor::poll(monotonic_clock::time_point now) {
    health_state aggregate = health_state::passing;
    for (const auto& check : checks_) {
        aggregate = worst_of(aggregate, check->poll(now));
    }
    aggregate_ = aggregate;
    return aggregate_;
}

health_check* health_monitor::find(const std::string& id) noexcept {
    for (const auto& check : checks_) {
        if (check->id() == id) {
            return check.get();
        }
    }
    return nullptr;
}

} // namespace cluster
} // namespace crpc
