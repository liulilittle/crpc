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
//     peer can no longer pin the fence domain at UINT64_MAX).
//   * Fence comparison is three-way (L-a): a token below the key high-water is
//     stale, a token equal to it is accepted only as an exact replay of an
//     accepted write or as the first use of a live authority issuance, and a
//     token above it is accepted within max_fence_lead.
//   * Every accepted token is an authority issuance (L-b): next_fence() and
//     fence_barrier() are the only issuers, and a write must present the token
//     that was issued for its key, principal and lease. A token the authority
//     never issued is rejected with registry.reason=not_issued, so knowing the
//     key high-water is not the same as being allowed to use it. A successful
//     mutation consumes its issuance: the next mutation of the key needs a
//     strictly greater token obtained from the authority.
//   * With strict_fencing a zero fence is rejected on every path, including a
//     create of a key that has never existed (Z2). There is no
//     grant-before-register back door; a caller that holds a lease token from
//     fence_barrier registers with that token.
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
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/detail/atomic_shared_ptr.hpp>

// registry_log is only forward-declared here: the header is included by
// registry_log.hpp's consumers, and the two must not include each other.
// The definition of restore_from_log takes the log by reference and calls
// replay() on it, so the complete type is required at the call site (the
// caller includes <crpc/cluster/registry_log.hpp>).
namespace crpc {
namespace cluster {
class registry_log;

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
    // Fencing token. It must be the token the authority issued for this key
    // (registry::next_fence or registry::fence_barrier) with matching principal
    // and lease id. Zero is rejected under strict_fencing (fence_required).
    u64 fence = 0;
    // Writer epoch. The bridge/service stamps the epoch it was issued when it
    // became a writer. Zero means "external writer, not epoch-tracked" and is
    // exempt. UINT64_MAX means "caller did not set an epoch" and is rejected
    // with epoch_required, so a forgotten assignment can never fail open.
    //
    // Parenthesised on purpose: a default member initialiser is expanded while
    // this header is parsed, so a `max` function-like macro (windows.h defines
    // one unless NOMINMAX is set) would break every consumer that includes
    // windows.h first. Wrapping the name stops the macro from being applied.
    u64 writer_epoch = (std::numeric_limits<u64>::max)();
    // Raft commit index at which this mutation was committed (D-A-9 A1: single
    // submission index). When > 0 the catalog uses it instead of its own
    // per-key index_, so the anchor and query ModifyIndex agree with
    // anchor()/commit_index(). Zero means "not raft-replicated" (the catalog
    // keeps its own index).
    u64 raft_index = 0;
};

struct write_result {
    u64 index = 0;
    // Globally strictly increasing across accepted mutations. The authority
    // allocates a monotonically increasing value from the accepted count,
    // raises it to a caller-supplied fencing token within
    // registry_config::max_fence_lead (a larger token is rejected), and never
    // lowers it below the key's effective high-water (including a lease
    // fence_barrier).
    //
    // This is the key's high-water AFTER the write, not a reusable ticket: the
    // next mutation of the same key must present a strictly greater token
    // obtained from the authority (registry::next_fence or
    // registry::fence_barrier). Feeding this value back to a second write is an
    // equal-fence write and is rejected as stale unless it is an exact replay.
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
    // registry.reason=fence_required. Set this to false only for legacy
    // compatibility: unfenced peers can then pierce an established epoch.
    bool strict_fencing = true;
    // Strict incarnation replay (default ON). A register with the same
    // (service, node_id, incarnation) but a different identity payload is
    // rejected as a conflict (failed_precondition/conflict) instead of
    // overwriting; an identical replay is idempotent and never emits a second
    // event. Set this to false only for legacy compatibility: a same-incarnation
    // endpoint change silently overwrites and loses the exactly-once anchor.
    bool strict_incarnation = true;
    // Issuance attribution (L-b, default ON like strict_fencing,
    // strict_incarnation and separate_state_writers).
    // When ON, check_fence() uses the three-way comparison (a token equal to the
    // key high-water is accepted only as an exact replay or as the first use of
    // an outstanding authority issuance) and rejects a token the authority never
    // issued for this key, principal and lease with
    // registry.reason=not_issued. When OFF, the historical numeric comparison
    // applies and any in-range token above the high-water is accepted.
    //
    // The default is ON because the historical default of OFF is not a working
    // configuration: it means L-b and Z2 do not apply at all. A peer that learnt
    // the high-water from any rejection can then write with high_water+1, and a
    // zero fence still creates a key. Any deployment with more than one writer
    // needs this ON; a caller that turns it OFF opts into both holes.
    //
    // In-process callers that construct a registry and immediately write without
    // calling next_fence()/fence_barrier() are the only reason the flag exists:
    // set it to false explicitly for that legacy shape and accept that L-b and
    // Z2 are then inert. New code must keep the default and obtain a token from
    // the authority.
    //
    // This switch also owns Z2: with it ON a zero fence is rejected on every
    // path, including the first registration of a key that has never existed.
    // There is deliberately no separate "require a fence" flag: splitting the
    // two would allow "issuance checked but zero fence accepted", which is the
    // half-on state this contract exists to forbid.
    bool require_issued_fence = true;
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
    // Lease-attributed overload (MINOR: new symbol, no layout change). The
    // token must have been issued by next_fence(service, node_id, lease_id) for
    // this lease; a mismatch is rejected with registry.reason=not_issued.
    [[nodiscard]] result<write_result> register_instance(service_instance instance,
                                                         const write_context& context,
                                                         u64 lease_id);

    // Leave a tombstone. Idempotent when the instance is already a tombstone.
    [[nodiscard]] result<write_result> deregister(const std::string& service,
                                                  const std::string& node_id,
                                                  const write_context& context);
    [[nodiscard]] result<write_result> deregister(const std::string& service,
                                                  const std::string& node_id,
                                                  const write_context& context,
                                                  u64 lease_id);

    // Advance the fencing high-water for (service, node_id) without changing
    // the catalog content, the global index or the sink events. Used by
    // lease_manager::grant (and election after a campaign) to publish the new
    // fence before the new holder starts writing, so a replaced holder's old
    // fence is rejected. Returns the effective per-key fence.
    [[nodiscard]] result<u64> fence_barrier(const std::string& service,
                                            const std::string& node_id,
                                            u64 fence,
                                            const write_context& context);
    // Lease-attributed barrier: publishes `fence` as issued to `lease_id`, so
    // the holder can spend it with set_state(..., lease_id).
    [[nodiscard]] result<u64> fence_barrier(const std::string& service,
                                            const std::string& node_id,
                                            u64 fence,
                                            const write_context& context,
                                            u64 lease_id);

    // Update the state of a live instance (health reports, lease expiry).
    // Passing, warning, critical and maintenance are accepted; deregistered is
    // rejected because it must carry the tombstone semantics of deregister().
    [[nodiscard]] result<write_result> set_state(const std::string& service,
                                                 const std::string& node_id,
                                                 instance_state state,
                                                 const write_context& context);
    [[nodiscard]] result<write_result> set_state(const std::string& service,
                                                 const std::string& node_id,
                                                 instance_state state,
                                                 const write_context& context,
                                                 u64 lease_id);

    // Deterministically sorted by (service, node_id, host, port).
    [[nodiscard]] result<catalog_result> query(const query_options& options) const;

    // Immutable snapshot. The returned pointer keeps the data alive.
    [[nodiscard]] std::shared_ptr<const registry_snapshot> snapshot() const noexcept;

    [[nodiscard]] u64 index() const noexcept;
    // Last globally accepted fencing token. Used by the lease manager so a new
    // lease is fenced above every previously accepted write.
    [[nodiscard]] u64 fence() const;

    // Authority allocation for one key (MINOR addition). Returns
    // max(global allocation, per-key high-water) + 1 and records the issuance
    // as belonging to `node_id` with no lease. This is the only way for a
    // caller to obtain a token a strict registry (require_issued_fence) will
    // accept on the next mutation of the key; a caller-side +1 is not an
    // issuance and is rejected with registry.reason=not_issued. It does not
    // touch the catalog, the index or the sink events.
    [[nodiscard]] result<u64> next_fence(const std::string& service,
                                        const std::string& node_id);
    // Same allocation bound to a lease id. A write that presents the token with
    // a different lease id is rejected with registry.reason=not_issued (L-b
    // identity binding, same principal).
    [[nodiscard]] result<u64> next_fence(const std::string& service,
                                        const std::string& node_id,
                                        u64 lease_id);

    // Returns a strictly increasing writer epoch drawn from the same counter as
    // the global fence high-water, so a restored catalog never hands out an
    // epoch at or below one that a pre-restart writer already used. Fails with
    // the same overflow reason as next_fence when the allocator is exhausted;
    // the sentinel UINT64_MAX is reserved for "caller did not set an epoch" and
    // must never be returned as a real epoch.
    [[nodiscard]] result<u64> next_writer_epoch();

    // Per-key fence high-water (0 when the key has no record). This is the key
    // floor a next token must exceed; it is deliberately NOT the global fence()
    // allocation, which can be far below a key that a fence_barrier raised.
    //
    // The zero is the documented "no record" value, not an error: a key that has
    // never been fenced or written reports 0, and the first token issued for it
    // is next_fence() == 1. A caller that anchors on the floor must therefore
    // treat 0 as "start from the beginning", never as "the key is at 0".
    [[nodiscard]] result<u64> fence_floor(const std::string& service,
                                         const std::string& node_id) const;

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
        required = 3,
        not_issued = 4,
        stale_writer = 5
    };
    // One key's authority issuance record. It replaces the bare u64 value of
    // last_fence_ so that "knows the number" and "is allowed to use it" stop
    // being the same thing. sizeof(std::map) does not depend on the value type,
    // so folding the record into the existing member keeps sizeof(registry)
    // unchanged.
    struct issued_fence {
        // Key high-water the authority has issued or committed.
        u64 token = 0;
        // Deterministic FNV-1a hash of write_context::node_id. Not std::hash:
        // the value is persisted and compared across builds, so it must not
        // depend on an implementation-specific seed.
        u64 principal_hash = 0;
        // Lease the token was issued to; zero means "not a lease write".
        u64 lease_id = 0;
        // True from the moment next_fence()/fence_barrier() publishes the token
        // until a write commits it. Only an outstanding issuance (or an exact
        // replay) may spend a token that equals the key high-water.
        bool outstanding = false;
        // Highest writer epoch this key has committed. A writer whose epoch is
        // below this floor is stale_writer. Zero means "never recorded", which
        // is only possible for a key written by external (epoch-0) writers.
        u64 writer_epoch = 0;
    };
    // Three-way comparison (L-a) plus issuance attribution (L-b). `issuing` is
    // true only for fence_barrier(), which publishes a token instead of
    // spending one. `exact_replay` is true only for an identical registration
    // replay; set_state() and deregister() always pass false. `creating` only
    // shapes the legacy zero-fence rule (require_issued_fence off): under the
    // strict authorization mode a zero fence is rejected on every path (Z2).
    [[nodiscard]] fence_verdict check_fence(const key& id, u64 fence, bool creating,
                                            bool exact_replay, u64 principal,
                                            u64 lease_id, bool issuing) const noexcept;
    // G19 step1: writer-epoch admission. Returns stale_writer when the caller's
    // epoch is below the epoch this key last committed, required when the caller
    // did not set one. Zero is exempt: it marks an external, non-epoch writer.
    [[nodiscard]] fence_verdict check_writer_epoch(const key& id,
                                                   u64 writer_epoch) const noexcept;
    // Persists an accepted epoch as the key's new recorded floor. Only epoch
    // values above zero are recorded; a zero-epoch write leaves the floor alone
    // because it is not an epoch-tracked writer.
    void record_writer_epoch_locked(const key& id, u64 writer_epoch);
    // True when the stored record is an outstanding issuance of exactly this
    // token for this principal and lease. Must be called with mutex_ held.
    [[nodiscard]] bool issuance_matches_locked(const key& id, u64 fence,
                                               u64 principal,
                                               u64 lease_id) const noexcept;
    [[nodiscard]] static u64 principal_hash(const std::string& node_id) noexcept;
    // Authority allocation. It is bounded by registry_config::max_fence_lead:
    // check_fence() rejects a caller token farther ahead of the published
    // high-water, so a single peer cannot pin the fence domain at UINT64_MAX.
    // key_high_water is the per-key effective floor (a lease fence_barrier can
    // push it above the global allocation); the result is always >= it.
    [[nodiscard]] bool allocate_fence(u64 requested, u64 next_index,
                                      u64 key_high_water, u64& out) noexcept;
    // Must be called with mutex_ held. Records the accepted caller token, the
    // per-key high-water and the issuance attribution, and consumes the
    // issuance; the returned value is the effective per-key fence after the
    // write (>= authority, >= requested, >= the previous high-water).
    [[nodiscard]] u64 record_fence_locked(const key& id, u64 requested, u64 authority,
                                          u64 principal, u64 lease_id);
    // Shared entry point of both write paths: the context-based overload used
    // by every caller and the lease-attributed one used by lease_manager. The
    // lease id is a separate argument because write_context must not grow a
    // field (its layout is part of the documented ABI). NEXT MAJOR: write_context
    // gains a trailing u64 writer_epoch (G19 step1), 96->104 (Debug) /
    // 80->88 (Release); this is the last trailing field it may add.
    [[nodiscard]] result<write_result> register_with(service_instance instance,
                                                     const write_context& context,
                                                     u64 lease_id);
    [[nodiscard]] result<write_result> deregister_with(const std::string& service,
                                                      const std::string& node_id,
                                                      const write_context& context,
                                                      u64 lease_id);
    [[nodiscard]] result<write_result> set_state_with(const std::string& service,
                                                     const std::string& node_id,
                                                     instance_state state,
                                                     const write_context& context,
                                                     u64 lease_id);
    [[nodiscard]] result<u64> issue_fence(const std::string& service,
                                          const std::string& node_id, u64 lease_id);
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
    // appended. `index` is the global index this mutation commits at (D-A-9 A1:
    // snapshot.index must equal raft commit index, not a stale catalog index_).
    // Must be called with mutex_ held.
    [[nodiscard]] std::shared_ptr<registry_snapshot>
    snapshot_with(const service_instance& instance, u64 index) const;

    void notify(const catalog_event& event);

    registry_config config_;
    mutable std::mutex mutex_;
    std::map<key, service_instance, key_less> instances_;
    // Per-key accepted fence high-water and its authority issuance. Survives a
    // tombstone purge: an old holder (or an old incarnation) must never write
    // again just because the visible row was physically removed. The value is
    // an issuance record rather than a bare token, so the same map also proves
    // who may use a token (L-b).
    std::map<key, issued_fence, key_less> last_fence_;
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

    // restore_from_log is a friend free function rather than a member: the
    // restore needs the private write/fence state (instances_, last_fence_,
    // tombstone_watermark_, index_, last_global_fence_) to rebuild the catalog
    // atomically, and a member would not change the ABI while a member
    // *function* that took the log by reference keeps `registry` free of any
    // log handle. A data member (a `registry_log*` or a `unique_ptr`) would
    // grow the class by 8 bytes, which is a second MAJOR layout change.
    friend CRPC_API result<std::size_t> restore_from_log(registry& target,
                                                         const registry_log& log);
};

#if defined(CRPC_COMPILER_MSVC)
#pragma warning(pop)
#endif

// --- Log restore -------------------------------------------------------------
//
// Rebuilds a registry from a replayed log. Fail-closed: every check below runs
// before the catalog is touched, so a rejected stream leaves `target` exactly
// as it was (R9: the index is never zeroed on failure).
//
//   R1  a torn tail is the prefix replay() returns; that prefix is accepted.
//   G1  the log's index space must be dense across ALL record kinds: 1, 2, 3,
//       ... with no hole. The consensus layer interleaves its own record kinds
//       in the same log (registry_log.hpp:64-73), so a gap in the
//       registry-event subsequence is the expected shape; a gap in the full
//       sequence means records were lost, which is corruption
//       (invalid_argument, message names the gap).
//   G3  the log's maximum index (of any kind) must exceed target.index():
//       restoring an older log over a newer catalog is a rollback, not a
//       recovery (failed_precondition).
//   R4  one (service, node_id, incarnation) names one write; a second,
//       different payload for the same incarnation is a conflict.
//   R5  the restored fence water-mark must not go below the target's
//       last_global_fence_: a lower water-mark would let an old holder write
//       again (failed_precondition).
//   R8  a corrupt frame surfaces replay()'s own error verbatim.
//
// Folding: replay() returns vector<log_record> and `registry_log` is a
// replaceable interface, so a backend may hand back duplicate indexes. The
// records are folded last-wins by index and applied in ascending index order;
// the surviving set is dense by construction after the G1 check.
//
// The payload codec is documented in tests/test_registry_restore.cpp: it is a
// versioned, length-prefixed, little-endian encoding of service_instance. The
// decoder is fail-closed (truncated input, unknown version or an over-long
// length is an error), so a partially parsed instance can never be applied.
//
// `registry` gains no data member and `sizeof(registry)` is unchanged.
[[nodiscard]] CRPC_API result<std::size_t> restore_from_log(
    registry& target, const registry_log& log);

} // namespace cluster
} // namespace crpc

#endif // CRPC_CLUSTER_REGISTRY_HPP
