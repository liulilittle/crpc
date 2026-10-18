// CRPC -- lease/session manager implementation.
#include <crpc/cluster/lease.hpp>

#include <limits>
#include <utility>

namespace crpc {
namespace cluster {

lease_manager::lease_manager(registry& catalog, monotonic_clock clock,
                             lease_config config)
    : catalog_(catalog), clock_(clock), config_(std::move(config)) {
    if (clock_.now == nullptr) {
        clock_ = system_monotonic_clock();
    }
    if (config_.default_ttl.count() <= 0) {
        config_.default_ttl = std::chrono::milliseconds{1};
    }
    if (config_.max_ttl.count() > 0 && config_.default_ttl > config_.max_ttl) {
        config_.default_ttl = config_.max_ttl;
    }
    if (config_.critical_grace.count() < 0) {
        config_.critical_grace = std::chrono::milliseconds{0};
    }
}

std::chrono::milliseconds
lease_manager::resolve_ttl(std::chrono::milliseconds ttl) const {
    return ttl.count() > 0 ? ttl : config_.default_ttl;
}

u64 lease_manager::next_fence_locked() {
    u64 candidate = catalog_.index();
    const u64 registry_fence = catalog_.fence();
    if (registry_fence > candidate) {
        candidate = registry_fence;
    }
    if (candidate <= last_fence_) {
        if (last_fence_ == std::numeric_limits<u64>::max()) {
            return last_fence_;
        }
        candidate = last_fence_ + 1;
    }
    last_fence_ = candidate;
    return candidate;
}

u64 lease_manager::next_fence() {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_fence_locked();
}

u64 lease_manager::current_incarnation(const std::string& service,
                                       const std::string& node_id) const {
    const auto snapshot = catalog_.snapshot();
    if (snapshot == nullptr) {
        return 0;
    }
    for (const auto& instance : snapshot->instances) {
        if (instance.service == service && instance.node_id == node_id) {
            return instance.incarnation;
        }
    }
    return 0;
}

result<lease> lease_manager::grant(const std::string& service,
                                   const std::string& node_id,
                                   std::chrono::milliseconds ttl) {
    if (service.empty() || node_id.empty()) {
        return rpc_error(rpc_code::invalid_argument,
                         "service and node_id are required");
    }
    const std::chrono::milliseconds resolved = resolve_ttl(ttl);
    if (resolved.count() <= 0) {
        return rpc_error(rpc_code::invalid_argument, "lease ttl must be positive");
    }
    if (config_.max_ttl.count() > 0 && resolved > config_.max_ttl) {
        return rpc_error(rpc_code::invalid_argument, "lease ttl exceeds max_ttl");
    }

    std::lock_guard<std::mutex> lock(mutex_);

    const key instance_key{service, node_id};
    const auto existing = by_instance_.find(instance_key);
    if (existing != by_instance_.end()) {
        leases_.erase(existing->second);
        by_instance_.erase(existing);
    }
    if (config_.max_leases != 0 && leases_.size() >= config_.max_leases) {
        return rpc_error(rpc_code::resource_exhausted, "lease limit reached");
    }

    u64 id = next_id_++;
    if (id == 0) {
        id = next_id_++;
    }
    entry next;
    next.token.id = id;
    next.token.service = service;
    next.token.node_id = node_id;
    next.token.ttl = resolved;
    next.token.incarnation = current_incarnation(service, node_id);
    next.token.expiry = clock_.read() + resolved;
    next.token.fence = next_fence_locked();
    next.state = phase::active;
    next.remove_at = monotonic_clock::time_point{};

    const lease token = next.token;
    leases_.emplace(id, std::move(next));
    by_instance_.emplace(instance_key, id);
    return token;
}

result<lease> lease_manager::renew(u64 lease_id, std::chrono::milliseconds ttl) {
    const std::chrono::milliseconds resolved = resolve_ttl(ttl);
    if (resolved.count() <= 0) {
        return rpc_error(rpc_code::invalid_argument, "lease ttl must be positive");
    }
    if (config_.max_ttl.count() > 0 && resolved > config_.max_ttl) {
        return rpc_error(rpc_code::invalid_argument, "lease ttl exceeds max_ttl");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = leases_.find(lease_id);
    if (found == leases_.end()) {
        return rpc_error(rpc_code::not_found, "lease not found");
    }

    entry& current = found->second;
    const monotonic_clock::time_point now = clock_.read();
    if (current.state == phase::critical && now >= current.remove_at) {
        return rpc_error(rpc_code::deadline_exceeded,
                         "lease expired past its critical grace period");
    }

    const u64 live_incarnation =
        current_incarnation(current.token.service, current.token.node_id);
    if (live_incarnation != 0) {
        current.token.incarnation = live_incarnation;
    }

    if (current.state == phase::critical) {
        const result<write_result> restored =
            apply_state(current, instance_state::passing, current.token.fence);
        if (!restored.ok()) {
            const key instance_key{current.token.service, current.token.node_id};
            by_instance_.erase(instance_key);
            leases_.erase(found);
            return restored.error();
        }
        current.state = phase::active;
        current.remove_at = monotonic_clock::time_point{};
        current.token.fence = restored.value().fence;
    }

    current.token.ttl = resolved;
    current.token.expiry = now + resolved;
    current.token.fence = next_fence_locked();
    return current.token;
}

status lease_manager::revoke(u64 lease_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = leases_.find(lease_id);
    if (found == leases_.end()) {
        return status{};
    }
    const key instance_key{found->second.token.service, found->second.token.node_id};
    by_instance_.erase(instance_key);
    leases_.erase(found);
    return status{};
}

bool lease_manager::is_active(u64 lease_id) const {
    return is_active(lease_id, clock_.read());
}

bool lease_manager::is_active(u64 lease_id, monotonic_clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = leases_.find(lease_id);
    if (found == leases_.end()) {
        return false;
    }
    return found->second.state == phase::active && now < found->second.token.expiry;
}

std::size_t lease_manager::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return leases_.size();
}

result<write_result> lease_manager::apply_state(const entry& current,
                                                instance_state state, u64 fence) {
    write_context context;
    context.node_id = current.token.node_id;
    context.incarnation = current.token.incarnation;
    context.fence = fence;
    return catalog_.set_state(current.token.service, current.token.node_id, state,
                              context);
}

lease_sweep_result lease_manager::expire_now() {
    return expire_now(clock_.read());
}

lease_sweep_result lease_manager::expire_now(monotonic_clock::time_point now) {
    lease_sweep_result sweep;
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto it = leases_.begin(); it != leases_.end();) {
        entry& current = it->second;

        if (current.state == phase::active) {
            if (now < current.token.expiry) {
                ++it;
                continue;
            }
            const result<write_result> marked =
                apply_state(current, instance_state::critical, current.token.fence);
            if (!marked.ok()) {
                if (marked.code() != rpc_code::not_found) {
                    ++sweep.rejected;
                }
                by_instance_.erase(key{current.token.service, current.token.node_id});
                it = leases_.erase(it);
                continue;
            }
            current.state = phase::critical;
            current.token.fence = marked.value().fence;
            current.remove_at = now + config_.critical_grace;
            ++sweep.expired;
            if (config_.critical_grace.count() > 0) {
                ++it;
                continue;
            }
        }

        if (current.state == phase::critical && now >= current.remove_at) {
            write_context context;
            context.node_id = current.token.node_id;
            context.incarnation = current.token.incarnation;
            context.fence = current.token.fence;
            const result<write_result> removed = catalog_.deregister(
                current.token.service, current.token.node_id, context);
            if (removed.ok()) {
                ++sweep.deregistered;
            } else if (removed.code() != rpc_code::not_found) {
                ++sweep.rejected;
            }
            by_instance_.erase(key{current.token.service, current.token.node_id});
            it = leases_.erase(it);
            continue;
        }

        ++it;
    }

    return sweep;
}

} // namespace cluster
} // namespace crpc
