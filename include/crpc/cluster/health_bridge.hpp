// CRPC -- health-to-registry bridge.
//
// `health.hpp` owns the probe state machines; it deliberately does not know the
// catalog. `health_bridge` is the missing link: it folds probe results per
// (service, node_id) and writes the resulting state into `cluster::registry`
// through exactly one writer, `flush()`.
//
// Contract
// --------
//   * `report()` is thread-safe and allocation-bounded. It only records a
//     candidate state; it never touches the registry. Concurrent probes are
//     therefore harmless, and a slow registry write can never run on a probe
//     thread.
//   * `flush()` is the single writer. It applies every candidate whose debounce
//     window has elapsed. A failed registry write (unknown instance, stale
//     incarnation, an allocation failure inside the registry) leaves the
//     candidate pending, so the next flush retries it; a node that is not in
//     the catalog at all is pruned from the bridge instead of leaking.
//   * Debounce: a candidate must stay unchanged for `debounce` before it is
//     written, which suppresses a single flapping probe. A candidate that
//     returns to the committed state before the window elapses is dropped.
//   * Failure threshold: `failure_threshold` consecutive failures are needed
//     before the catalog goes critical; a sub-threshold failure is a warning.
//   * Half-open recovery: a critical node leaves critical only after
//     `success_threshold` consecutive passes (default 2, matching
//     `health_check`). `half_open()` is true while such a node is making
//     progress through that window.
//   * External writers: the bridge is the only writer of health, so an
//     external catalog write is never adopted wholesale. The first catalog
//     observation seeds the committed baseline; afterwards an external state
//     is adopted only when it is *more severe* than the bridge's committed
//     state (a higher-priority liveness writer such as lease expiry, adopted
//     fail-safe). A less severe external write (for example a
//     register_instance that clobbers health back to passing) is not adopted:
//     the bridge keeps its committed state and repairs the catalog with its
//     own write. Recovery therefore only happens through a debounced,
//     half-open, fenced write of this bridge.
//   * Fencing: every write carries a token the registry authority issued for
//     that exact key (`registry::next_fence(service, node_id)`). The bridge
//     manufactures no fence of its own, so a token it presents is one the
//     authority granted (L-b); the authority's allocation is strictly above the
//     key's high-water and above every lease barrier, so a replaced or stale
//     writer cannot be re-anchored by a local arithmetic guess. A refused token
//     leaves the candidate pending: the next flush asks for a fresh issuance.
//   * Writer epoch: a bridge owns one `writer_epoch` taken from
//     `registry::next_writer_epoch()` at construction. Every write carries it,
//     and a write whose epoch is below the key's recorded epoch is rejected with
//     `registry.reason=stale_writer`; the bridge then yields: it keeps observing
//     but stops writing for the rest of its lifetime.
//
//     The yield is permanent on purpose. `next_writer_epoch()` is a monotonic
//     counter, not a capability: it is granted to any caller with no identity or
//     liveness check, so "I obtained a larger epoch" does not prove the previous
//     owner is gone. A bridge that re-acquired an epoch after being refused would
//     therefore win the key back on its next probe and the guard would be a
//     no-op. The supported recovery path is to construct a new health_bridge,
//     which is granted a strictly newer epoch and may write immediately.
//
//     LIFETIME: the epoch is process-local and is NOT persisted. A registry
//     restored from a log rewinds its allocator to the highest fence the log
//     records, which can be below an epoch a surviving bridge holds. Step 1
//     keeps bridges and their registry in one process, so a restart rebuilds
//     both; a registry replayed under live bridges is a step-2 scenario and
//     requires the cross-process epoch persistence listed in plan.md.
//   * The node set is bounded by `max_nodes`. `report()` returns false at the
//     bound or when the record cannot be allocated.
//
// Load balancing
// --------------
// `lb_view(service)` returns a read-only `resilience::instance_set` over the
// registered instances of one service. `at()` reports the catalog state merged
// with the bridge's committed health; `version()` is a single monotonic change
// counter that advances on every observed membership change and every committed
// health transition, which is exactly the signal the load-balancer strategies
// rebuild their caches from. It is not an arithmetic combination of the catalog
// index and the health version, so two different states can never share a
// version.
//
// Lifetime: a view shares ownership of the bridge's state. A view may outlive
// the bridge; it keeps using that shared state and keeps observing the
// catalog (only the health records stop changing). The registry passed to the
// bridge must outlive both the bridge and every view.
//
// Allocation: `report()` allocates once per new node (the key strings). A
// steady stream of probes for known nodes does not allocate. `flush()` queries
// the registry and may allocate there; `lb_view()` allocates once per view.
#ifndef CRPC_CLUSTER_HEALTH_BRIDGE_HPP
#define CRPC_CLUSTER_HEALTH_BRIDGE_HPP

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

#include <crpc/cluster/health.hpp>
#include <crpc/cluster/registry.hpp>
#include <crpc/config.hpp>
#include <crpc/resilience/load_balancer.hpp>

namespace crpc {
namespace cluster {

class health_bridge;

// Read-only instance_set over one service. Instances are taken from the
// registry snapshot and health from the bridge's committed state. The view is
// safe to read while probes and flushes run on other threads.
class CRPC_API health_instance_set final : public resilience::instance_set {
public:
    ~health_instance_set() override;

    health_instance_set(const health_instance_set&) = delete;
    health_instance_set& operator=(const health_instance_set&) = delete;

    [[nodiscard]] std::size_t size() const noexcept override;
    [[nodiscard]] resilience::instance at(std::size_t index) const noexcept override;
    [[nodiscard]] u64 version() const noexcept override;

    // Diagnostics: the node identity behind an index of the current snapshot.
    [[nodiscard]] std::string node_id_at(std::size_t index) const;
    [[nodiscard]] endpoint address_at(std::size_t index) const;

private:
    friend class health_bridge;
    health_instance_set(const health_bridge& bridge, std::string service);

    // Refreshes the cached snapshot when the catalog or the health version
    // moved. Observation only; never mutates the bridge.
    void refresh() const noexcept;

    struct impl;
    std::unique_ptr<impl> impl_;
};

class CRPC_API health_bridge {
public:
    // `max_nodes == 0` means unlimited. All thresholds of zero are raised to
    // one, matching `health_check`.
    explicit health_bridge(registry& catalog,
                           std::chrono::milliseconds debounce = std::chrono::milliseconds{0},
                           u32 failure_threshold = 1, u32 success_threshold = 2,
                           std::size_t max_nodes = 4096,
                           monotonic_clock clock = system_monotonic_clock());
    ~health_bridge();

    health_bridge(const health_bridge&) = delete;
    health_bridge& operator=(const health_bridge&) = delete;
    health_bridge(health_bridge&&) = delete;
    health_bridge& operator=(health_bridge&&) = delete;

    // Records one probe result. Returns false when the node is new and the
    // bridge is at its bound, or when the record could not be allocated. Never
    // touches the registry.
    [[nodiscard]] bool report(const std::string& service, const std::string& node_id,
                              bool pass) noexcept;
    [[nodiscard]] bool report(const std::string& service, const std::string& node_id,
                              bool pass, monotonic_clock::time_point now) noexcept;

    // The single writer. Applies every transition whose debounce window has
    // elapsed and repairs external downgrades. Returns the number of registry
    // writes that were accepted. A write the registry refuses with stale_fence,
    // not_issued or a stale incarnation leaves the candidate pending, and the
    // next flush obtains a fresh authority token and retries. A write refused
    // with stale_writer does not: that bridge has been deposed and stops writing
    // for the rest of its lifetime (see the writer-epoch note above).
    [[nodiscard]] std::size_t flush();
    [[nodiscard]] std::size_t flush(monotonic_clock::time_point now);

    // Observation of the committed (catalog-visible) state.
    [[nodiscard]] health_state state(const std::string& service,
                                     const std::string& node_id) const noexcept;
    [[nodiscard]] bool half_open(const std::string& service,
                                 const std::string& node_id) const noexcept;
    [[nodiscard]] u32 consecutive_failures(const std::string& service,
                                           const std::string& node_id) const noexcept;
    [[nodiscard]] u32 consecutive_successes(const std::string& service,
                                            const std::string& node_id) const noexcept;
    [[nodiscard]] std::size_t node_count() const noexcept;

    // Increases on every committed health transition.
    [[nodiscard]] u64 version() const noexcept;

    [[nodiscard]] registry& catalog() noexcept;
    [[nodiscard]] const registry& catalog() const noexcept;

    [[nodiscard]] std::shared_ptr<health_instance_set> lb_view(const std::string& service) const;

private:
    friend class health_instance_set;

    [[nodiscard]] u64 instance_version() const noexcept;

    struct impl;
    std::shared_ptr<impl> impl_;
};

} // namespace cluster
} // namespace crpc

#endif // CRPC_CLUSTER_HEALTH_BRIDGE_HPP
