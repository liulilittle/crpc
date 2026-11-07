// CRPC -- lease/session manager.
//
// A lease grants a node the right to act for an instance for a bounded time.
// Leases are driven by an injected monotonic clock and an explicit
// expire_now() sweep; there is no hidden thread, so tests are deterministic.
//
// Lease expiry is applied through the registry authority path (owner node_id
// plus a fencing token), never by mutating catalog memory directly:
//
//   * first expiry transitions the instance to critical;
//   * after critical_grace it transitions to deregistered (a tombstone) and the
//     lease is released.
//
// Fencing
// -------
// Every grant and renewal returns a fence. The fence is derived from the
// registry's monotonic index and bumped locally when necessary so that it is
// strictly increasing. It is never derived from a wall clock. Registry writes
// carry the fence; the registry rejects a fence lower than the last accepted
// fence for that instance. A lease that lost a race therefore cannot write.
//
// Concurrency contract
// --------------------
//   * lease_manager is thread-safe.
//   * Lock order: lease_manager::mutex_ -> registry::mutex_. The registry never
//     calls back into the lease manager, so the order is acyclic.
//   * The registry must outlive the lease_manager.
//   * The sweep calls the registry while holding the lease mutex. This is safe
//     because registry sinks (the watch hub) never call the lease manager.
#ifndef CRPC_CLUSTER_LEASE_HPP
#define CRPC_CLUSTER_LEASE_HPP

#include <chrono>
#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include <crpc/cluster/registry.hpp>

namespace crpc {
namespace cluster {

struct lease_config {
    std::chrono::milliseconds default_ttl{5000};
    std::chrono::milliseconds max_ttl{3600000};
    // Time spent in critical before the tombstone is written. Zero deregisters
    // in the same sweep that observes the expiry.
    std::chrono::milliseconds critical_grace{5000};
    std::size_t max_leases = 4096;
};

struct lease {
    u64 id = 0;
    std::string service;
    std::string node_id;
    // Registry incarnation captured at grant/renew. Expiry writes carry it, so
    // a lease cannot mutate a newer incarnation of the same instance.
    u64 incarnation = 0;
    monotonic_clock::time_point expiry;
    std::chrono::milliseconds ttl{0};
    u64 fence = 0;
};

struct lease_sweep_result {
    u32 expired = 0;
    u32 deregistered = 0;
    u32 rejected = 0;
};

#if defined(CRPC_COMPILER_MSVC)
// Private STL members are an implementation detail; the exported ABI is the
// method set. C4251 would otherwise reject every class with a std:: member in
// a shared build.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
class CRPC_API lease_manager {
public:
    lease_manager(registry& catalog, monotonic_clock clock,
                  lease_config config = lease_config{});

    lease_manager(const lease_manager&) = delete;
    lease_manager& operator=(const lease_manager&) = delete;
    lease_manager(lease_manager&&) = delete;
    lease_manager& operator=(lease_manager&&) = delete;

    // Grant (or replace) the lease for (service, node_id). A ttl of zero uses
    // default_ttl. The instance does not have to exist yet; expiry is a no-op
    // for an instance that is absent or already a tombstone.
    //
    // A grant publishes a fencing barrier for the key before returning, so a
    // replaced lease can no longer write with its old fence even before the
    // new holder writes (P13). A lease granted before the instance exists
    // binds to the first incarnation it observes on renew/sweep (P7).
    [[nodiscard]] result<lease> grant(const std::string& service,
                                      const std::string& node_id,
                                      std::chrono::milliseconds ttl =
                                          std::chrono::milliseconds{0});

    // Extend the lease. Renewing a critical lease restores the instance to
    // passing through the registry authority path. An active lease whose
    // expiry already passed is rejected with deadline_exceeded; it is never
    // resurrected (P4). A lease bound to a superseded incarnation is rejected.
    [[nodiscard]] result<lease> renew(u64 lease_id,
                                      std::chrono::milliseconds ttl =
                                          std::chrono::milliseconds{0});

    // Release the lease immediately. Idempotent; does not deregister the
    // instance.
    [[nodiscard]] status revoke(u64 lease_id);

    // True only while the lease is in the active phase and now < expiry. A
    // critical (expired) lease is not active even before it is swept.
    [[nodiscard]] bool is_active(u64 lease_id) const;
    [[nodiscard]] bool is_active(u64 lease_id, monotonic_clock::time_point now) const;

    [[nodiscard]] std::size_t size() const;

    // Next strictly increasing fencing token, derived from the registry index.
    // Saturates at UINT64_MAX.
    [[nodiscard]] u64 next_fence();

    // Explicit sweep. Applies critical transitions and tombstone writes.
    [[nodiscard]] lease_sweep_result expire_now();
    [[nodiscard]] lease_sweep_result expire_now(monotonic_clock::time_point now);

private:
    struct key {
        std::string service;
        std::string node_id;
    };

    struct key_less {
        [[nodiscard]] bool operator()(const key& a, const key& b) const {
            if (a.service != b.service) {
                return a.service < b.service;
            }
            return a.node_id < b.node_id;
        }
    };

    enum class phase : u8 {
        active = 0,
        critical = 1
    };

    struct entry {
        lease token;
        phase state = phase::active;
        monotonic_clock::time_point remove_at;
    };

    [[nodiscard]] u64 next_fence_locked();
    [[nodiscard]] u64 current_incarnation(const std::string& service,
                                          const std::string& node_id) const;
    [[nodiscard]] std::chrono::milliseconds resolve_ttl(
        std::chrono::milliseconds ttl) const;
    [[nodiscard]] result<write_result> apply_state(const entry& current,
                                                   instance_state state, u64 fence);

    registry& catalog_;
    monotonic_clock clock_;
    lease_config config_;
    mutable std::mutex mutex_;
    std::map<u64, entry> leases_;
    std::map<key, u64, key_less> by_instance_;
    u64 next_id_ = 1;
    u64 last_fence_ = 0;
};

#if defined(CRPC_COMPILER_MSVC)
#pragma warning(pop)
#endif
} // namespace cluster
} // namespace crpc

#endif // CRPC_CLUSTER_LEASE_HPP
