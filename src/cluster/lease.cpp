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

u64 lease_manager::next_fence_locked(const std::string& service,
                                     const std::string& node_id) {
    const u64 ceiling = std::numeric_limits<u64>::max();
    // A8: the allocator must clear the per-key high-water in ONE step. A lease
    // fence_barrier can push one key far above the global counter, and walking
    // towards it one grant at a time made a barrier of 2^32 cost 2^32 rejected
    // grants (probe_barrier_livelock / probe_grant_walk). Reading the floor is
    // the only way to see it without deliberately failing first.
    const result<u64> floor = catalog_.fence_floor(service, node_id);
    const u64 key_floor = floor.ok() ? floor.value() : 0;

    u64 base = catalog_.index();
    const u64 registry_fence = catalog_.fence();
    if (registry_fence > base) {
        base = registry_fence;
    }
    if (last_fence_ > base) {
        base = last_fence_;
    }
    if (base == ceiling) {
        return ceiling;
    }
    u64 candidate = base + 1;
    // The new token must also exceed the key floor, otherwise the barrier would
    // republish the very value the previous holder still holds and the two
    // would be indistinguishable.
    if (key_floor != ceiling && candidate <= key_floor) {
        candidate = key_floor + 1;
    }
    last_fence_ = candidate;
    return candidate;
}

u64 lease_manager::next_fence() {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_fence_locked(std::string{}, std::string{});
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
    if (existing == by_instance_.end() && config_.max_leases != 0 &&
        leases_.size() >= config_.max_leases) {
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
    next.token.fence = next_fence_locked(service, node_id);
    next.state = phase::active;
    next.remove_at = monotonic_clock::time_point{};

    // Fence barrier (P13): publish the new fence for the key before the new
    // holder starts writing, so a replaced lease's old fence is rejected even
    // if the new holder has not written yet. Lock order lease -> registry is
    // the documented order. The barrier binds the token to this lease id so a
    // different lease cannot spend it (L-b), and it publishes the token to the
    // registry's issuance record, which is what makes the first write legal
    // under require_issued_fence.
    write_context barrier_context;
    barrier_context.node_id = node_id;
    barrier_context.incarnation = next.token.incarnation;
    barrier_context.fence = next.token.fence;
    barrier_context.writer_epoch = 0;  // lease write: not an epoch-tracked writer
    const result<u64> barrier = catalog_.fence_barrier(
        service, node_id, next.token.fence, barrier_context, id);
    if (!barrier.ok()) {
        return barrier.error();
    }
    // The barrier may raise the effective token above the allocator result.
    next.token.fence = barrier.value();

    if (existing != by_instance_.end()) {
        leases_.erase(existing->second);
        by_instance_.erase(existing);
    }

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
    // P4: an active lease whose expiry has already passed is expired even
    // before the sweep observes it. Renewal must not resurrect it; is_active()
    // and renew() use the same boundary.
    if (current.state == phase::active && now >= current.token.expiry) {
        return rpc_error(rpc_code::deadline_exceeded,
                         "lease has already expired");
    }

    const u64 live_incarnation =
        current_incarnation(current.token.service, current.token.node_id);
    if (current.token.incarnation == 0) {
        // P7: a lease granted before the instance existed binds to the first
        // incarnation it observes.
        if (live_incarnation != 0) {
            current.token.incarnation = live_incarnation;
        }
    } else if (live_incarnation != 0 &&
               live_incarnation != current.token.incarnation) {
        // The instance moved to a newer incarnation (restart). This lease no
        // longer represents it and must not renew against the new one.
        return rpc_error(rpc_code::failed_precondition,
                         "lease is bound to a superseded incarnation");
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
    current.token.fence = next_fence_locked(current.token.service,
                                            current.token.node_id);
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
    // Publish the fresh issuance as a barrier for this lease id first. The
    // registry accepts only an outstanding issuance, and the critical mark /
    // tombstone below then spend it; without the barrier the write would be
    // refused as not_issued. This mirrors grant(), which publishes its token
    // before the holder can spend it.
    write_context barrier_context;
    barrier_context.node_id = current.token.node_id;
    barrier_context.incarnation = current.token.incarnation;
    barrier_context.fence = fence;
    barrier_context.writer_epoch = 0;  // lease write: not an epoch-tracked writer
    const result<u64> barrier = catalog_.fence_barrier(
        current.token.service, current.token.node_id, fence, barrier_context,
        current.token.id);
    if (!barrier.ok()) {
        return barrier.error();
    }
    write_context context;
    context.node_id = current.token.node_id;
    // P7: the entry may have been bound to the incarnation it observed after the
    // grant, so the write must present that incarnation rather than the value
    // captured at grant time (which can be 0).
    context.incarnation = current.token.incarnation;
    context.fence = barrier.value();
    context.writer_epoch = 0;  // lease write: not an epoch-tracked writer
    // The barrier bound this token to this lease id, so the write must present
    // the same one (L-b).
    return catalog_.set_state(current.token.service, current.token.node_id, state,
                              context, current.token.id);
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
            // P7: bind an unbound lease to the incarnation that exists now, so
            // grant-before-register expiry is applied instead of being lost as
            // a stale write. A lease bound to an older incarnation no longer
            // represents the instance and is dropped without writing.
            const u64 live_incarnation = current_incarnation(
                current.token.service, current.token.node_id);
            if (current.token.incarnation == 0) {
                if (live_incarnation != 0) {
                    current.token.incarnation = live_incarnation;
                }
            } else if (live_incarnation != 0 &&
                       live_incarnation != current.token.incarnation) {
                ++sweep.rejected;
                by_instance_.erase(
                    key{current.token.service, current.token.node_id});
                it = leases_.erase(it);
                continue;
            }
            // Re-issue before the write. The token this entry holds was spent by
            // the holder's registration (or by an earlier expiry write), and the
            // authority accepts only an outstanding issuance: presenting the
            // consumed value returns stale_fence. next_fence_locked() advances
            // both the per-key floor and this manager's own allocator, so the
            // next grant cannot collide with the token just issued.
            current.token.fence = next_fence_locked(current.token.service,
                                                    current.token.node_id);
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
            // The critical write spent the entry's token, so the tombstone needs
            // its own issuance (same rule as the critical mark above).
            const u64 removal_fence = next_fence_locked(current.token.service,
                                                        current.token.node_id);
            write_context barrier_context;
            barrier_context.node_id = current.token.node_id;
            barrier_context.incarnation = current.token.incarnation;
            barrier_context.fence = removal_fence;
            barrier_context.writer_epoch = 0;  // lease write: not epoch-tracked
            const result<u64> barrier = catalog_.fence_barrier(
                current.token.service, current.token.node_id, removal_fence,
                barrier_context, current.token.id);
            if (!barrier.ok()) {
                if (barrier.code() != rpc_code::not_found) {
                    ++sweep.rejected;
                }
                by_instance_.erase(key{current.token.service, current.token.node_id});
                it = leases_.erase(it);
                continue;
            }
            write_context context;
            context.node_id = current.token.node_id;
            context.incarnation = current.token.incarnation;
            context.fence = barrier.value();
            context.writer_epoch = 0;  // lease write: not an epoch-tracked writer
            const result<write_result> removed = catalog_.deregister(
                current.token.service, current.token.node_id, context,
                current.token.id);
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
