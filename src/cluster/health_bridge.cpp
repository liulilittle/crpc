// CRPC -- health-to-registry bridge implementation.
#include <crpc/cluster/health_bridge.hpp>

#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace crpc {
namespace cluster {
namespace {

[[nodiscard]] health_state health_from(instance_state state) noexcept {
    switch (state) {
    case instance_state::passing:
        return health_state::passing;
    case instance_state::warning:
    case instance_state::maintenance:
        return health_state::warning;
    case instance_state::critical:
    case instance_state::deregistered:
        return health_state::critical;
    }
    return health_state::critical;
}

// Stable 64-bit identity for the load-balancer view. Hash-based rather than
// index-based so that adding or removing another node does not renumber the
// surviving ones (consistent_hash keys would otherwise move for no reason).
[[nodiscard]] u64 stable_id(const std::string& service,
                            const std::string& node) noexcept {
    u64 hash = 1469598103934665603ull;
    for (const char ch : service) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 1099511628211ull;
    }
    hash ^= 0xffu;
    hash *= 1099511628211ull;
    for (const char ch : node) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 1099511628211ull;
    }
    return hash == 0 ? 1 : hash;
}

[[nodiscard]] u32 saturating_increment(u32 value) noexcept {
    return value == 0xFFFFFFFFu ? value : value + 1;
}

// True when the registry refusal carries exactly this `registry.reason`.
[[nodiscard]] bool reason_is(const rpc_error& error, const char* reason) {
    const std::string* found = error.metadata.find("registry.reason");
    return found != nullptr && *found == reason;
}

} // namespace

struct health_bridge::impl {
    struct record {
        health_state committed = health_state::passing;
        health_state desired = health_state::passing;
        monotonic_clock::time_point desired_since{};
        u32 successes = 0;
        u32 failures = 0;
        // Revision of the catalog instance this bridge last observed or wrote.
        u64 observed_revision = 0;
        bool seeded = false;
        // An external writer downgraded the catalog state; rewrite `committed`.
        bool reassert = false;
        // A newer writer epoch owns this instance; stop writing until a fresh
        // epoch proves that owner is gone.
        bool yielded = false;
    };

    registry* catalog = nullptr;
    std::chrono::milliseconds debounce{0};
    u32 failure_threshold = 1;
    u32 success_threshold = 2;
    std::size_t max_nodes = 4096;
    monotonic_clock clock = system_monotonic_clock();
    // Writer epoch granted to this bridge. A newer bridge takes over by holding
    // a strictly greater epoch; this one stops writing until it can obtain a
    // fresh, greater epoch from the authority (which is possible once the
    // taker-over dies, so the yield is recoverable rather than permanent).
    u64 writer_epoch = 0;

    mutable std::mutex mutex;
    std::map<std::pair<std::string, std::string>, record> records;
    // Committed health transitions (diagnostic).
    u64 version = 0;
    // Single-domain, strictly monotonic view version. It advances once per
    // observed catalog index change and once per committed health transition,
    // so it never aliases two different states the way an index+version sum
    // can.
    u64 view_epoch = 0;
    u64 observed_index = 0;
    bool index_seen = false;

    void bump_health() noexcept {
        ++version;
        ++view_epoch;
    }

    // Caller must hold `mutex`.
    [[nodiscard]] u64 view_version_locked() noexcept {
        const u64 current = catalog->index();
        if (!index_seen || current != observed_index) {
            observed_index = current;
            index_seen = true;
            ++view_epoch;
        }
        return view_epoch;
    }
};

struct health_instance_set::impl {
    struct entry {
        std::string node_id;
        endpoint address;
        u64 id = 0;
        u32 weight = 1;
        bool healthy = true;
    };

    // Shared with the bridge: the view may outlive the bridge and keeps the
    // state it observes alive. The catalog must outlive both.
    std::shared_ptr<health_bridge::impl> source;
    std::string service;

    mutable std::mutex mutex;
    mutable std::vector<entry> entries;
    mutable u64 stamp = 0;
    mutable bool loaded = false;
};

void health_instance_set::refresh() const noexcept {
    std::lock_guard<std::mutex> outer(impl_->mutex);
    try {
        health_bridge::impl& source = *impl_->source;
        std::lock_guard<std::mutex> inner(source.mutex);
        const std::shared_ptr<const registry_snapshot> snapshot =
            source.catalog->snapshot();
        if (!snapshot) {
            return;
        }
        const u64 current = source.view_version_locked();
        if (impl_->loaded && impl_->stamp == current) {
            return;
        }

        std::vector<impl::entry> fresh;
        for (const service_instance& instance : snapshot->instances) {
            if (instance.service != impl_->service) {
                continue;
            }
            if (instance.state == instance_state::deregistered) {
                continue;
            }
            health_state health = health_from(instance.state);
            const auto found = source.records.find(
                std::make_pair(impl_->service, instance.node_id));
            if (found != source.records.end()) {
                health = found->second.committed;
            }
            impl::entry value;
            value.node_id = instance.node_id;
            value.address = instance.address;
            value.id = stable_id(impl_->service, instance.node_id);
            value.weight = instance.weight;
            value.healthy = instance.state == instance_state::passing &&
                            health == health_state::passing;
            fresh.push_back(std::move(value));
        }

        impl_->entries = std::move(fresh);
        impl_->stamp = current;
        impl_->loaded = true;
    } catch (...) {
        // Observation must never throw. A failed refresh leaves the previous
        // snapshot in place; the next call retries.
    }
}

// --- health_instance_set -----------------------------------------------------

health_instance_set::health_instance_set(const health_bridge& bridge,
                                         std::string service)
    : impl_(new impl()) {
    impl_->source = bridge.impl_;
    impl_->service = std::move(service);
}

health_instance_set::~health_instance_set() = default;

std::size_t health_instance_set::size() const noexcept {
    refresh();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->entries.size();
}

resilience::instance health_instance_set::at(std::size_t index) const noexcept {
    refresh();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (index >= impl_->entries.size()) {
        return resilience::instance{};
    }
    const impl::entry& value = impl_->entries[index];
    resilience::instance result;
    result.id = value.id;
    result.weight = value.weight;
    result.healthy = value.healthy;
    result.inflight = 0;
    return result;
}

u64 health_instance_set::version() const noexcept {
    refresh();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->stamp;
}

std::string health_instance_set::node_id_at(std::size_t index) const {
    refresh();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (index >= impl_->entries.size()) {
        return std::string{};
    }
    return impl_->entries[index].node_id;
}

endpoint health_instance_set::address_at(std::size_t index) const {
    refresh();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (index >= impl_->entries.size()) {
        return endpoint{};
    }
    return impl_->entries[index].address;
}

// --- health_bridge -----------------------------------------------------------

namespace {

// The bridge owns no fencing arithmetic. Every token it presents is obtained
// from the registry authority (catalog->next_fence) for the exact key it is
// about to write, so a token carries the authority's own high-water and the
// bridge cannot manufacture a fence the authority never granted (L-b). The old
// `next_fence_epoch`/`impl::fence_base` pair is gone: a self-made
// high_water+1 was exactly the hole L-b closes.

} // namespace

health_bridge::health_bridge(registry& catalog, std::chrono::milliseconds debounce,
                             u32 failure_threshold, u32 success_threshold,
                             std::size_t max_nodes, monotonic_clock clock)
    : impl_(new impl()) {
    impl_->catalog = &catalog;
    impl_->debounce = debounce.count() < 0 ? std::chrono::milliseconds{0} : debounce;
    impl_->failure_threshold = failure_threshold == 0 ? 1 : failure_threshold;
    impl_->success_threshold = success_threshold == 0 ? 1 : success_threshold;
    impl_->max_nodes = max_nodes;
    impl_->clock = clock.now != nullptr ? clock : system_monotonic_clock();
    const result<u64> epoch = impl_->catalog->next_writer_epoch();
    impl_->writer_epoch = epoch.ok() ? epoch.value() : 0;
}

health_bridge::~health_bridge() = default;

bool health_bridge::report(const std::string& service, const std::string& node_id,
                           bool pass) noexcept {
    return report(service, node_id, pass, impl_->clock.read());
}

bool health_bridge::report(const std::string& service, const std::string& node_id,
                           bool pass, monotonic_clock::time_point now) noexcept {
    try {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto key = std::make_pair(service, node_id);
        auto found = impl_->records.find(key);
        if (found == impl_->records.end()) {
            if (impl_->max_nodes != 0 && impl_->records.size() >= impl_->max_nodes) {
                return false;
            }
            impl::record fresh;
            fresh.desired_since = now;
            found = impl_->records.emplace(key, fresh).first;
        }

        impl::record& record = found->second;
        health_state target = record.committed;
        if (pass) {
            record.successes = saturating_increment(record.successes);
            record.failures = 0;
            if (record.committed == health_state::critical &&
                record.successes < impl_->success_threshold) {
                // Half-open: still critical until enough consecutive passes.
                target = health_state::critical;
            } else {
                target = health_state::passing;
            }
        } else {
            record.failures = saturating_increment(record.failures);
            record.successes = 0;
            if (record.committed == health_state::critical) {
                target = health_state::critical;
            } else if (record.failures >= impl_->failure_threshold) {
                target = health_state::critical;
            } else {
                target = health_state::warning;
            }
        }

        if (target == record.committed) {
            // Returning to the committed state cancels a pending transition.
            record.desired = record.committed;
            record.desired_since = now;
        } else if (target != record.desired) {
            record.desired = target;
            record.desired_since = now;
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::size_t health_bridge::flush() {
    return flush(impl_->clock.read());
}

std::size_t health_bridge::flush(monotonic_clock::time_point now) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::size_t applied = 0;
    for (auto it = impl_->records.begin(); it != impl_->records.end();) {
        const std::string& service = it->first.first;
        const std::string& node_id = it->first.second;
        impl::record& record = it->second;

        query_options query;
        query.service = service;
        query.node_id = node_id;
        // Ask for tombstones explicitly: a deletion is a first-class answer,
        // not "the instance disappeared". The two cases prune the record, but
        // the tombstone branch must be reachable (H5).
        query.include_deregistered = true;
        std::optional<result<catalog_result>> queried;
        try {
            queried.emplace(impl_->catalog->query(query));
        } catch (...) {
            ++it;
            continue;
        }
        if (!queried->ok()) {
            ++it;
            continue;
        }
        if (queried->value().instances.empty()) {
            // The catalog does not know this node; do not grow the bridge for
            // an instance that can never be written.
            it = impl_->records.erase(it);
            continue;
        }

        const service_instance& instance = queried->value().instances.front();
        if (instance.state == instance_state::deregistered) {
            it = impl_->records.erase(it);
            continue;
        }
        if (instance.state == instance_state::maintenance) {
            // Maintenance is an operator decision; health probes must not
            // override it.
            ++it;
            continue;
        }

        const health_state stored = health_from(instance.state);
        if (!record.seeded) {
            // First observation: the catalog is the baseline. There is no
            // prior committed state of ours to protect.
            record.seeded = true;
            record.committed = stored;
            record.observed_revision = instance.revision;
            if (health_rank(record.desired) < health_rank(record.committed)) {
                // Do not let a pass that predates the baseline bypass the
                // half-open window.
                record.desired = record.committed;
                record.desired_since = now;
            }
        } else if (instance.revision > record.observed_revision) {
            // Somebody else wrote this instance after our last observation.
            record.observed_revision = instance.revision;
            if (health_rank(stored) > health_rank(record.committed)) {
                // A higher-priority liveness writer (lease expiry) or a worse
                // verdict: adopt it fail-safe and cancel a weaker recovery.
                record.committed = stored;
                impl_->bump_health();
                if (health_rank(record.desired) < health_rank(record.committed)) {
                    record.desired = record.committed;
                    record.desired_since = now;
                    record.reassert = false;
                }
            } else if (health_rank(stored) < health_rank(record.committed)) {
                // A lower-priority writer tried to downgrade health. Never
                // adopt that as committed: recover only through our own
                // debounced, fenced write.
                if (record.desired == record.committed) {
                    record.reassert = true;
                }
            }
            // Equal severity: an identity-only rewrite. Re-anchor above.
        }

        if (record.desired != record.committed) {
            // A pending, probe-driven transition supersedes a repair write.
            record.reassert = false;
        }
        health_state target = record.desired;
        bool forced = false;
        if (record.reassert) {
            target = record.committed;
            forced = true;
        }
        if (target == record.committed && !forced) {
            ++it;
            continue;
        }
        if (!forced && now < record.desired_since + impl_->debounce) {
            ++it;
            continue;
        }

        if (record.yielded) {
            // A newer writer epoch owns this instance. This bridge yielded for
            // good: only a freshly constructed bridge gets a new epoch, because
            // next_writer_epoch() cannot prove the taker-over died (it hands out
            // a strictly greater value to any caller, including this one, so
            // re-acquiring would let a stale writer win the key back).
            ++it;
            continue;
        }

        // Every write carries a token the registry issued for this exact key.
        // The bridge never adds one itself: a caller-side high_water+1 is not an
        // issuance and is rejected with not_issued under the default config.
        const result<u64> token = impl_->catalog->next_fence(service, node_id);
        if (!token.ok()) {
            ++it;
            continue;
        }
        write_context context;
        context.node_id = node_id;
        context.incarnation = instance.incarnation;
        context.fence = token.value();
        context.writer_epoch = impl_->writer_epoch;
        std::optional<result<write_result>> written;
        try {
            written.emplace(impl_->catalog->set_state(
                service, node_id, to_instance_state(target), context));
        } catch (...) {
            ++it;
            continue;
        }
        if (written->ok()) {
            record.committed = target;
            record.desired = target;
            record.desired_since = now;
            record.observed_revision = written->value().index;
            record.reassert = false;
            impl_->bump_health();
            ++applied;
        } else if (written->error().code == rpc_code::failed_precondition &&
                   reason_is(written->error(), "stale_writer")) {
            // A newer writer epoch owns this instance. Stop writing until a
            // fresh epoch proves that owner is gone (see the yield block above).
            record.yielded = true;
            ++it;
            continue;
        } else if (written->error().code == rpc_code::not_found) {
            // The catalog no longer knows this key: prune the record. erase()
            // returns the next iterator, so the loop must not advance again.
            it = impl_->records.erase(it);
            continue;
        }
        // Any other refusal (stale_fence, not_issued, stale incarnation) leaves
        // the candidate pending: the next flush reads the key again and asks the
        // authority for a fresh token, which is the only legal way to re-anchor.
        // There is no local high-water bookkeeping to update: the authority's
        // next_fence() already jumps over every floor.
        ++it;
    }
    return applied;
}

health_state health_bridge::state(const std::string& service,
                                  const std::string& node_id) const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->records.find(std::make_pair(service, node_id));
    return found == impl_->records.end() ? health_state::passing
                                         : found->second.committed;
}

bool health_bridge::half_open(const std::string& service,
                              const std::string& node_id) const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->records.find(std::make_pair(service, node_id));
    if (found == impl_->records.end()) {
        return false;
    }
    const impl::record& record = found->second;
    return record.committed == health_state::critical && record.successes > 0 &&
           record.successes < impl_->success_threshold;
}

u32 health_bridge::consecutive_failures(const std::string& service,
                                        const std::string& node_id) const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->records.find(std::make_pair(service, node_id));
    return found == impl_->records.end() ? 0 : found->second.failures;
}

u32 health_bridge::consecutive_successes(const std::string& service,
                                         const std::string& node_id) const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->records.find(std::make_pair(service, node_id));
    return found == impl_->records.end() ? 0 : found->second.successes;
}

std::size_t health_bridge::node_count() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->records.size();
}

u64 health_bridge::version() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->version;
}

registry& health_bridge::catalog() noexcept {
    return *impl_->catalog;
}

const registry& health_bridge::catalog() const noexcept {
    return *impl_->catalog;
}

u64 health_bridge::instance_version() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->view_version_locked();
}

std::shared_ptr<health_instance_set> health_bridge::lb_view(
    const std::string& service) const {
    return std::shared_ptr<health_instance_set>(
        new health_instance_set(*this, service));
}

} // namespace cluster
} // namespace crpc
