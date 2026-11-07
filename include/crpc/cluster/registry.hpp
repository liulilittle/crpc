// CRPC -- service catalog (registry).
//
// The registry is the single authority for the service catalog. It implements
// the Consul-shaped data model from the design: a service
// instance is identified by (service, node_id), every accepted mutation bumps a
// global monotonic index, and readers observe immutable snapshots.
//
// Concurrency contract
// --------------------
//   * registry itself is thread-safe.
//   * Mutations are serialised by one writer mutex. There is exactly one
//     logical writer at a time (the authority shard in a real deployment).
//   * Reads never take the writer mutex. They atomically load a
//     std::shared_ptr<const registry_snapshot>; a mutation publishes a brand
//     new snapshot with atomic_store(release). The shared_ptr atomic operations
//     may use a small implementation spinlock, but never the writer mutex. This
//     trades O(n) copying per mutation for a read path with no tearing, no
//     reader starvation and no writer lock. v1 favours correctness and
//     simplicity; a persistent map can replace the copy later without changing
//     the public API.
//   * Catalog event sinks are invoked after the writer mutex is released, but
//     while the sink-list mutex is held. A sink must therefore be non-blocking
//     and must not mutate the registry or add/remove sinks (that would recurse
//     into the sink-list mutex). The watch hub satisfies this contract.
//
// Authority and fencing
// ---------------------
//   * A write is accepted only when write_context::node_id equals the stored
//     instance node_id, or when a non-empty configured admin token is supplied.
//   * A write carrying an incarnation lower than the stored incarnation is
//     rejected. A tombstone (deregistered instance) rejects every incarnation
//     that is not strictly greater, so an older registration can never
//     penetrate a deletion ("NULL cannot penetrate"). The tombstone incarnation
//     high-water survives purge_expired_tombstones(), so a purged key cannot be
//     resurrected by an old incarnation either.
//   * One (key, incarnation) names one write: an identical replay is idempotent
//     (same index, no event) and a different payload for the same incarnation
//     is a conflict. All strict defaults can be turned off per registry for
//     legacy compatibility; the flags document their risk.
//   * The authority allocates a strictly increasing fence from the accepted
//     mutation count and raises it to a caller-supplied token only within
//     registry_config::max_fence_lead (a larger token is rejected, so a single
//     peer can no longer pin the fence domain at UINT64_MAX). A supplied fence
//     lower than the last accepted fence for the key is rejected. With
//     strict_fencing, an update of a key that has accepted an external epoch
//     must carry a non-zero token.
//
// Bounded resources
// -----------------
//   * max_services, max_instances_per_service, max_total_instances and
//     max_metadata_bytes bound the catalog. Exceeding a bound returns
//     rpc_code::resource_exhausted. A zero limit means "unlimited"; the
//     defaults are finite. Tombstones count against the instance and service
//     bounds until purge_expired_tombstones() removes them.
#ifndef CRPC_CLUSTER_REGISTRY_HPP
#define CRPC_CLUSTER_REGISTRY_HPP

#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/detail/atomic_shared_ptr.hpp>

namespace crpc {
namespace cluster {

// --- Shared cluster clock ----------------------------------------------------
//
// All cluster timers (lease expiry, health probes, election timeouts) use an
// injected monotonic clock. The function-pointer + context pair keeps the
// timer sources allocation-free and lets tests drive time deterministically.
// A default steady-clock source is provided; it is stateless.

struct monotonic_clock {
    using time_point = std::chrono::steady_clock::time_point;
    using now_fn = time_point (*)(void* context) noexcept;

    now_fn now = nullptr;
    void* context = nullptr;

    [[nodiscard]] time_point read() const noexcept {
        return now != nullptr ? now(context) : time_point{};
    }
};

namespace detail {
inline monotonic_clock::time_point steady_now(void*) noexcept {
    return std::chrono::steady_clock::now();
}
} // namespace detail

[[nodiscard]] inline monotonic_clock system_monotonic_clock() noexcept {
    return monotonic_clock{&detail::steady_now, nullptr};
}

// --- Catalog types -----------------------------------------------------------

struct endpoint {
    std::string host;
    u16 port = 0;
};

[[nodiscard]] inline bool operator==(const endpoint& a, const endpoint& b) {
    return a.port == b.port && a.host == b.host;
}

[[nodiscard]] inline bool operator!=(const endpoint& a, const endpoint& b) {
    return !(a == b);
}

enum class instance_state : u8 {
    passing = 0,
    warning = 1,
    critical = 2,
    maintenance = 3,
    deregistered = 4
};

[[nodiscard]] CRPC_API const char* to_string(instance_state state) noexcept;

struct service_instance {
    std::string service;
    std::string node_id;
    endpoint address;
    u32 weight = 1;
    metadata_list meta;
    u64 revision = 0;   // index at which this instance last changed
    u64 incarnation = 0;
    instance_state state = instance_state::passing;
};

struct query_options {
    std::string service;
    std::string tag;
    std::string node_id;
    bool passing_only = false;
    // Tombstones are hidden by default; set this to inspect them.
    bool include_deregistered = false;
};

struct catalog_result {
    u64 index = 0;
    std::vector<service_instance> instances;
};

struct write_context {
    // Caller identity. Must equal the instance node_id unless an admin token is
    // supplied.
    std::string node_id;
    // Optional static admin token (see registry_config::admin_token).
    std::string admin_token;
    // Expected incarnation for state changes / deregistration. A value lower
    // than the stored one is stale and is rejected.
    u64 incarnation = 0;
    // Fencing token. Zero means "unfenced"; a non-zero value lower than the
    // last accepted fence for the key is rejected.
    u64 fence = 0;
};

struct write_result {
    u64 index = 0;
    // Globally strictly increasing across accepted mutations. The authority
    // allocates a monotonically increasing value from the accepted count,
    // raises it to a caller-supplied fencing token within
    // registry_config::max_fence_lead (a larger token is rejected), and never
    // lowers it below the key's effective high-water (including a lease
    // fence_barrier). A successful write therefore returns a value that a
    // subsequent write of the same key can present as context.fence without
    // being rejected as stale.
    u64 fence = 0;
    u64 incarnation = 0;
};

struct registry_config {
    std::size_t max_services = 1024;
    std::size_t max_instances_per_service = 1024;
    std::size_t max_total_instances = 65536;
    std::size_t max_metadata_bytes = 4096;
    std::size_t max_sinks = 64;
    // A tombstone may be physically removed by purge_expired_tombstones() only
    // after this many accepted mutations have elapsed. Zero means "eligible
    // immediately".
    u64 tombstone_grace = 4096;
    // Empty disables the admin path.
    std::string admin_token;
    // Field-writer separation (default ON, the correct Phase 0 semantics).
    // register_instance() never writes the health/lease-owned state: an update
    // keeps the stored state, so a registration cannot clobber a health or
    // lease verdict. Set this to false only for legacy compatibility: the
    // historical whole-instance overwrite is then restored and a registration
    // can silently reset a critical verdict to passing. New code must keep the
    // default and write state through set_state().
    bool separate_state_writers = true;
    // Strict fencing (default ON). An update (register/set_state/deregister) of
    // a key that has accepted an external fencing epoch must carry a non-zero
    // fence; a zero fence on such a key is rejected with
    // registry.reason=fence_required. A create of a key that has never existed
    // is allowed unfenced so a pre-granted lease can be followed by its first
    // registration. A key with a surviving tombstone watermark is NOT fresh:
    // purge_expired_tombstones() keeps the fence epoch, so a zero-fence create
    // after a purge is rejected with registry.reason=fence_required as well.
    // Set this to false only for legacy compatibility: unfenced peers can then
    // pierce an established epoch.
    bool strict_fencing = true;
    // Strict incarnation replay (default ON). A register with the same
    // (service, node_id, incarnation) but a different identity payload is
    // rejected as a conflict (failed_precondition/conflict) instead of
    // overwriting; an identical replay is idempotent and never emits a second
    // event. Set this to false only for legacy compatibility: a same-incarnation
    // endpoint change silently overwrites and loses the exactly-once anchor.
    bool strict_incarnation = true;
    // A caller-supplied fencing token may lead the authority high-water by at
    // most this much; a larger token is rejected with
    // registry.reason=fence_out_of_range. This bounds the damage one peer can
    // do to the shared fence domain (a single write can no longer pin the
    // global fence at UINT64_MAX). Zero restores the unbounded historical
    // behaviour and is not recommended.
    u64 max_fence_lead = 1ull << 32;
};

struct registry_snapshot {
    u64 index = 0;
    std::vector<service_instance> instances;
};

enum class catalog_event_kind : u8 {
    put = 0,
    remove = 1
};

struct catalog_event {
    u64 index = 0;
    catalog_event_kind kind = catalog_event_kind::put;
    service_instance instance;
};

using catalog_sink = void (*)(void* context, const catalog_event& event) noexcept;

// --- Registry ----------------------------------------------------------------

#if defined(CRPC_COMPILER_MSVC)
// Private STL members are an implementation detail; the exported ABI is the
// method set. C4251 would otherwise reject every class with a std:: member in
// a shared build.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
class CRPC_API registry {
public:
    explicit registry(registry_config config = registry_config{});

    registry(const registry&) = delete;
    registry& operator=(const registry&) = delete;
    registry(registry&&) = delete;
    registry& operator=(registry&&) = delete;

    // Register or update an instance. The instance identity is
    // (service, node_id). A registered instance must not carry the
    // deregistered state; use deregister() for that.
    [[nodiscard]] result<write_result> register_instance(service_instance instance,
                                                         const write_context& context);

    // Leave a tombstone. Idempotent when the instance is already a tombstone.
    [[nodiscard]] result<write_result> deregister(const std::string& service,
                                                  const std::string& node_id,
                                                  const write_context& context);

    // Advance the fencing high-water for (service, node_id) without changing
    // the catalog content, the global index or the sink events. Used by
    // lease_manager::grant (and election after a campaign) to publish the new
    // fence before the new holder starts writing, so a replaced holder's old
    // fence is rejected. Returns the effective per-key fence.
    [[nodiscard]] result<u64> fence_barrier(const std::string& service,
                                            const std::string& node_id,
                                            u64 fence,
                                            const write_context& context);

    // Update the state of a live instance (health reports, lease expiry).
    // Passing, warning, critical and maintenance are accepted; deregistered is
    // rejected because it must carry the tombstone semantics of deregister().
    [[nodiscard]] result<write_result> set_state(const std::string& service,
                                                 const std::string& node_id,
                                                 instance_state state,
                                                 const write_context& context);

    // Deterministically sorted by (service, node_id, host, port).
    [[nodiscard]] result<catalog_result> query(const query_options& options) const;

    // Immutable snapshot. The returned pointer keeps the data alive.
    [[nodiscard]] std::shared_ptr<const registry_snapshot> snapshot() const noexcept;

    [[nodiscard]] u64 index() const noexcept;
    // Last globally accepted fencing token. Used by the lease manager so a new
    // lease is fenced above every previously accepted write.
    [[nodiscard]] u64 fence() const;

    // Event sinks. Sinks are invoked synchronously on the writer thread after
    // the catalog mutex is released; they must be fast and non-blocking.
    [[nodiscard]] result<u64> add_sink(catalog_sink sink, void* context);
    [[nodiscard]] status remove_sink(u64 sink_id);

    // Remove tombstones at least registry_config::tombstone_grace accepted
    // mutations old (zero means "eligible immediately"). Returns the number
    // removed. Every removal advances the global index and emits one
    // catalog_event_kind::remove event, because the include_deregistered view
    // changed; the tombstone incarnation high-water is kept so an old
    // incarnation cannot resurrect the key afterwards.
    [[nodiscard]] std::size_t purge_expired_tombstones();

    [[nodiscard]] const registry_config& config() const noexcept { return config_; }

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

    struct sink_entry {
        u64 id = 0;
        catalog_sink sink = nullptr;
        void* context = nullptr;
    };

    [[nodiscard]] bool is_admin(const write_context& context) const noexcept;
    [[nodiscard]] bool check_authority(const write_context& context,
                                       const std::string& owner) const noexcept;
    enum class fence_verdict : u8 {
        ok = 0,
        stale = 1,
        out_of_range = 2,
        required = 3
    };
    [[nodiscard]] fence_verdict check_fence(const key& id, u64 fence,
                                            bool creating) const noexcept;
    // Authority allocation. It is bounded by registry_config::max_fence_lead:
    // check_fence() rejects a caller token farther ahead of the published
    // high-water, so a single peer cannot pin the fence domain at UINT64_MAX.
    // key_high_water is the per-key effective floor (a lease fence_barrier can
    // push it above the global allocation); the result is always >= it, so a
    // successful write reports a token that is directly usable on the key.
    [[nodiscard]] bool allocate_fence(u64 requested, u64 next_index,
                                      u64 key_high_water, u64& out) noexcept;
    // Must be called with mutex_ held. Records the accepted caller token and
    // the per-key high-water; the returned value is the effective per-key
    // fence after the write (>= authority, >= requested, >= the previous
    // high-water), never the bare authority allocation.
    [[nodiscard]] u64 record_fence_locked(const key& id, u64 requested,
                                          u64 authority);
    [[nodiscard]] u64 published_fence_locked() const noexcept;
    [[nodiscard]] static rpc_error reject(rpc_code code, const char* message,
                                          const char* reason);
    // Builds the stale_fence rejection with the key's current effective
    // high-water in `registry.fence` metadata, so a client (notably the health
    // bridge) can re-anchor above it instead of retrying a permanently stale
    // token (F3). Must be called with mutex_ held.
    [[nodiscard]] rpc_error reject_stale_fence_locked(const key& id) const;
    [[nodiscard]] std::size_t count_instances(const std::string& service) const;
    [[nodiscard]] std::size_t count_services() const;
    [[nodiscard]] bool service_exists(const std::string& service) const;
    [[nodiscard]] static std::size_t metadata_size(const metadata_list& meta);
    [[nodiscard]] static bool identity_equal(const service_instance& a,
                                             const service_instance& b,
                                             bool compare_state) noexcept;

    // Builds a snapshot from the current one with one instance replaced or
    // appended. Must be called with mutex_ held.
    [[nodiscard]] std::shared_ptr<registry_snapshot>
    snapshot_with(const service_instance& instance) const;

    void notify(const catalog_event& event);

    registry_config config_;
    mutable std::mutex mutex_;
    std::map<key, service_instance, key_less> instances_;
    // Per-key accepted fence high-water. Survives a tombstone purge: an old
    // holder (or an old incarnation) must never write again just because the
    // visible row was physically removed.
    std::map<key, u64, key_less> last_fence_;
    // Per-key tombstone incarnation high-water. purge_expired_tombstones()
    // removes the visible row but keeps this watermark, so an old incarnation
    // cannot resurrect a purged key ("NULL cannot penetrate"). One u64 per key
    // ever seen; a zero watermark is never stored.
    std::map<key, u64, key_less> tombstone_watermark_;
    // Keys that have accepted an external (caller-supplied) fencing epoch or a
    // lease barrier. With strict_fencing, an update of such a key must carry a
    // non-zero fence.
    std::map<key, bool, key_less> fenced_keys_;
    // Swapped atomically on every publish; readers keep the snapshot they
    // loaded. Holds a plain shared_ptr and uses the free-function atomic API;
    // see detail/atomic_shared_ptr.hpp for why (layout must not depend on the
    // language standard, and the C++20 atomic<shared_ptr> is 16-aligned).
    ::crpc::detail::atomic_shared_ptr<const registry_snapshot> snapshot_;
    u64 index_ = 0;
    // Authority allocation high-water (accepted count plus the bounded caller
    // lead and any lease barrier).
    u64 last_global_fence_ = 0;

    mutable std::mutex sinks_mutex_;
    std::vector<sink_entry> sinks_;
    u64 next_sink_id_ = 1;
};

#if defined(CRPC_COMPILER_MSVC)
#pragma warning(pop)
#endif
} // namespace cluster
} // namespace crpc

#endif // CRPC_CLUSTER_REGISTRY_HPP
