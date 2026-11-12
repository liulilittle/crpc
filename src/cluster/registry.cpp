// CRPC -- service catalog implementation.
#include <crpc/cluster/registry.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <crpc/cluster/registry_log.hpp>

namespace crpc {
namespace cluster {
namespace {

constexpr const char* reason_authority = "authority";
constexpr const char* reason_stale_incarnation = "stale_incarnation";
constexpr const char* reason_stale_fence = "stale_fence";
constexpr const char* reason_fence_required = "fence_required";
constexpr const char* reason_fence_out_of_range = "fence_out_of_range";
constexpr const char* reason_not_issued = "not_issued";
constexpr const char* reason_stale_writer = "stale_writer";
constexpr const char* reason_epoch_required = "epoch_required";
constexpr const char* reason_tombstone = "tombstone";
constexpr const char* reason_conflict = "conflict";
constexpr const char* reason_not_live = "not_live";
constexpr const char* reason_limit = "limit";
constexpr const char* reason_overflow = "index_overflow";
constexpr const char* reason_invalid = "invalid";

// Deterministic FNV-1a over the node id. std::hash is not used because the
// resulting principal is part of an issuance record that must compare equal
// across builds and (from step3 on) across nodes; some standard libraries seed
// their string hash per process.
[[nodiscard]] u64 fnv1a(const std::string& value) noexcept {
    u64 hash = 1469598103934665603ull;
    for (const char c : value) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

[[nodiscard]] bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

[[nodiscard]] std::string trim(std::string value) {
    std::size_t begin = 0;
    while (begin < value.size() && is_space(value[begin])) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && is_space(value[end - 1])) {
        --end;
    }
    return value.substr(begin, end - begin);
}

[[nodiscard]] bool tag_matches(const metadata_list& meta, const std::string& wanted) {
    if (wanted.empty()) {
        return true;
    }
    if (const std::string* single = meta.find("tag")) {
        if (*single == wanted) {
            return true;
        }
    }
    if (const std::string* list = meta.find("tags")) {
        std::size_t start = 0;
        while (start <= list->size()) {
            const std::size_t comma = list->find(',', start);
            const std::size_t length =
                comma == std::string::npos ? list->size() - start : comma - start;
            if (trim(list->substr(start, length)) == wanted) {
                return true;
            }
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }
    }
    return false;
}

[[nodiscard]] bool instance_less(const service_instance& a, const service_instance& b) {
    if (a.service != b.service) {
        return a.service < b.service;
    }
    if (a.node_id != b.node_id) {
        return a.node_id < b.node_id;
    }
    if (a.address.host != b.address.host) {
        return a.address.host < b.address.host;
    }
    return a.address.port < b.address.port;
}

} // namespace

const char* to_string(instance_state state) noexcept {
    switch (state) {
    case instance_state::passing:      return "passing";
    case instance_state::warning:      return "warning";
    case instance_state::critical:     return "critical";
    case instance_state::maintenance:  return "maintenance";
    case instance_state::deregistered: return "deregistered";
    }
    return "unknown";
}

registry::registry(registry_config config)
    : config_(std::move(config)),
      snapshot_(std::make_shared<registry_snapshot>()) {}

bool registry::is_admin(const write_context& context) const noexcept {
    return !config_.admin_token.empty() && context.admin_token == config_.admin_token;
}

bool registry::check_authority(const write_context& context,
                               const std::string& owner) const noexcept {
    return is_admin(context) || (!owner.empty() && context.node_id == owner);
}

u64 registry::published_fence_locked() const noexcept {
    return last_global_fence_;
}

u64 registry::principal_hash(const std::string& node_id) noexcept {
    return fnv1a(node_id);
}

bool registry::issuance_matches_locked(const key& id, u64 fence, u64 principal,
                                       u64 lease_id) const noexcept {
    const auto found = last_fence_.find(id);
    if (found == last_fence_.end()) {
        return false;
    }
    const issued_fence& record = found->second;
    return record.outstanding && record.token == fence &&
           record.principal_hash == principal && record.lease_id == lease_id;
}

u64 registry::record_fence_locked(const key& id, u64 requested, u64 authority,
                                  u64 principal, u64 lease_id) {
    issued_fence& key_fence = last_fence_[id];
    if (requested > key_fence.token) {
        key_fence.token = requested;
        key_fence.principal_hash = principal;
        key_fence.lease_id = lease_id;
    }
    if (authority > key_fence.token) {
        key_fence.token = authority;
        key_fence.principal_hash = principal;
        key_fence.lease_id = lease_id;
    }
    if (requested != 0) {
        fenced_keys_[id] = true;
    }
    // The issuance is spent: the next mutation of this key needs a strictly
    // greater token from the authority, so a token cannot be reused by a second
    // writer that learnt it.
    key_fence.outstanding = false;
    // The effective per-key high-water, not the bare authority allocation: a
    // fence_barrier can hold the key above the global allocator.
    return key_fence.token;
}

bool registry::identity_equal(const service_instance& a,
                              const service_instance& b,
                              bool compare_state) noexcept {
    if (a.address != b.address || a.weight != b.weight) {
        return false;
    }
    if (compare_state && a.state != b.state) {
        return false;
    }
    if (a.meta.size() != b.meta.size()) {
        return false;
    }
    for (const auto& entry : a.meta) {
        const std::string* other = b.meta.find(entry.first);
        if (other == nullptr || *other != entry.second) {
            return false;
        }
    }
    return true;
}

registry::fence_verdict registry::check_writer_epoch(const key& id,
                                                     u64 writer_epoch) const noexcept {
    // UINT64_MAX is the "caller did not set an epoch" sentinel. Defaulting to
    // zero instead would silently make a forgetful writer an exempt external
    // one, i.e. the guard would fail open.
    if (writer_epoch == std::numeric_limits<u64>::max()) {
        return fence_verdict::required;
    }
    if (writer_epoch == 0) {
        return fence_verdict::ok;
    }
    const auto found = last_fence_.find(id);
    if (found == last_fence_.end()) {
        return fence_verdict::ok;
    }
    if (writer_epoch < found->second.writer_epoch) {
        return fence_verdict::stale_writer;
    }
    return fence_verdict::ok;
}

void registry::record_writer_epoch_locked(const key& id, u64 writer_epoch) {
    if (writer_epoch == 0 || writer_epoch == std::numeric_limits<u64>::max()) {
        return;
    }
    issued_fence& key_fence = last_fence_[id];
    if (writer_epoch > key_fence.writer_epoch) {
        key_fence.writer_epoch = writer_epoch;
    }
}

registry::fence_verdict registry::check_fence(const key& id, u64 fence, bool creating,
                                              bool exact_replay, u64 principal,
                                              u64 lease_id, bool issuing) const noexcept {
    if (fence == 0) {
        // Z2: under the strict authorization mode a zero fence never
        // authorises a mutation, not even the first registration of a key with
        // no history. The historical grant-before-register back door let an
        // unfenced create occupy a key that a fence_barrier had already raised,
        // and a squatter could take a key before its owner registered.
        if (config_.require_issued_fence) {
            return fence_verdict::required;
        }
        // Legacy (default) mode keeps the Phase 0 rule so in-process users that
        // never configure fencing are unaffected: an update of a key that
        // accepted an external epoch, or a create behind a surviving tombstone
        // watermark, requires a token; anything else may be unfenced.
        if (config_.strict_fencing) {
            if (creating) {
                if (tombstone_watermark_.find(id) != tombstone_watermark_.end()) {
                    return fence_verdict::required;
                }
            } else {
                const auto fenced = fenced_keys_.find(id);
                if (fenced != fenced_keys_.end() && fenced->second) {
                    return fence_verdict::required;
                }
            }
        }
        return fence_verdict::ok;
    }
    if (config_.max_fence_lead != 0) {
        u64 high_water = published_fence_locked();
        // A lease barrier advances the per-key high-water without moving the
        // global allocation, so the bound must consider it; otherwise repeated
        // grants on one key would drift past the lead and be rejected.
        const auto accepted = last_fence_.find(id);
        if (accepted != last_fence_.end() && accepted->second.token > high_water) {
            high_water = accepted->second.token;
        }
        const u64 lead = config_.max_fence_lead;
        const u64 limit = high_water > std::numeric_limits<u64>::max() - lead
                              ? std::numeric_limits<u64>::max()
                              : high_water + lead;
        if (fence > limit) {
            return fence_verdict::out_of_range;
        }
    }
    const auto accepted = last_fence_.find(id);
    const u64 high_water = accepted != last_fence_.end() ? accepted->second.token : 0;
    // L-a: below the key high-water is always stale.
    if (fence < high_water) {
        return fence_verdict::stale;
    }
    if (!config_.require_issued_fence) {
        // Legacy numeric contract: any in-range token above the high-water is
        // accepted. Kept for in-process users; a network deployment must set
        // require_issued_fence.
        return fence_verdict::ok;
    }
    // L-b: with issuance attribution on, the token must be one the authority
    // issued for this key, principal and lease.
    if (issuing) {
        // fence_barrier publishes a token. A strictly greater token always
        // passes; an equal one rebinds the outstanding issuance to the caller,
        // which is how a lease grant hands the same barrier value to the new
        // holder. The barrier itself is authority-checked, so this is the
        // authority moving the token, not a peer claiming it.
        if (fence >= high_water) {
            return fence_verdict::ok;
        }
        return fence_verdict::stale;
    }
    if (fence == high_water) {
        // Equal to the key high-water: only an exact replay of an accepted
        // write, or the first use of an outstanding issuance, may spend it.
        // A new incarnation that merely observed the high-water is rejected.
        if (exact_replay || issuance_matches_locked(id, fence, principal, lease_id)) {
            return fence_verdict::ok;
        }
        if (accepted != last_fence_.end() && accepted->second.outstanding) {
            // The token is live but it was issued to another principal or
            // another lease: it exists, it is simply not this writer's. Report
            // that precisely instead of calling it stale, which would suggest
            // the token has already been spent.
            return fence_verdict::not_issued;
        }
        return fence_verdict::stale;
    }
    // Strictly above the high-water: the authority must have issued it.
    if (issuance_matches_locked(id, fence, principal, lease_id)) {
        return fence_verdict::ok;
    }
    return fence_verdict::not_issued;
}

bool registry::allocate_fence(u64 requested, u64 next_index,
                              u64 key_high_water, u64& out) noexcept {
    if (last_global_fence_ == std::numeric_limits<u64>::max()) {
        return false;
    }
    u64 candidate = next_index;
    if (requested > candidate) {
        candidate = requested;
    }
    if (key_high_water > candidate) {
        candidate = key_high_water;
    }
    if (requested != 0) {
        // The caller presented a token the authority issued. The issuance was
        // itself the global allocation (next_fence() and fence_barrier() both
        // move last_global_fence_), so committing it must not spend a second
        // number: an issued token of N has to stay N. Only the global
        // high-water may move, and never downwards.
        if (candidate > last_global_fence_) {
            last_global_fence_ = candidate;
        }
        out = candidate;
        return true;
    }
    // An unfenced (legacy) write takes a fresh number from the global counter.
    if (candidate <= last_global_fence_) {
        candidate = last_global_fence_ + 1;
    }
    last_global_fence_ = candidate;
    out = candidate;
    return true;
}

result<u64> registry::fence_barrier(const std::string& service,
                                    const std::string& node_id, u64 fence,
                                    const write_context& context) {
    return fence_barrier(service, node_id, fence, context, /*lease_id=*/0);
}

result<u64> registry::fence_barrier(const std::string& service,
                                    const std::string& node_id, u64 fence,
                                    const write_context& context, u64 lease_id) {
    if (service.empty() || node_id.empty() || fence == 0) {
        return reject(rpc_code::invalid_argument,
                      "service, node_id and fence are required", reason_invalid);
    }
    if (!check_authority(context, node_id)) {
        return reject(rpc_code::failed_precondition,
                      "write authority does not belong to this node",
                      reason_authority);
    }

    const key id{service, node_id};
    std::lock_guard<std::mutex> lock(mutex_);
    const u64 principal = principal_hash(context.node_id);
    const fence_verdict verdict =
        check_fence(id, fence, /*creating=*/true, /*exact_replay=*/false, principal,
                    lease_id, /*issuing=*/true);
    if (verdict == fence_verdict::out_of_range) {
        return reject(rpc_code::failed_precondition,
                      "fencing token is out of range",
                      reason_fence_out_of_range);
    }
    if (verdict == fence_verdict::stale) {
        return reject_stale_fence_locked(id);
    }
    if (verdict == fence_verdict::required) {
        return reject(rpc_code::failed_precondition,
                      "a fencing token is required for this key",
                      reason_fence_required);
    }
    issued_fence& key_fence = last_fence_[id];
    if (fence >= key_fence.token) {
        key_fence.token = fence;
        // A barrier publishes the token: the holder that calls it may spend it
        // once even though it equals the key high-water.
        key_fence.principal_hash = principal;
        key_fence.lease_id = lease_id;
        key_fence.outstanding = true;
    }
    fenced_keys_[id] = true;
    return key_fence.token;
}


rpc_error registry::reject(rpc_code code, const char* message, const char* reason) {
    rpc_error error(code, message);
    error.metadata.set("registry.reason", reason);
    return error;
}

rpc_error registry::reject_stale_fence_locked(const key& id) const {
    rpc_error error = reject(rpc_code::failed_precondition, "stale fencing token",
                             reason_stale_fence);
    const auto accepted = last_fence_.find(id);
    if (accepted != last_fence_.end()) {
        error.metadata.set("registry.fence", std::to_string(accepted->second.token));
    }
    return error;
}

std::size_t registry::count_instances(const std::string& service) const {
    std::size_t count = 0;
    for (const auto& entry : instances_) {
        if (entry.first.service == service) {
            ++count;
        }
    }
    return count;
}

std::size_t registry::count_services() const {
    std::size_t count = 0;
    std::string previous;
    bool first = true;
    for (const auto& entry : instances_) {
        if (first || entry.first.service != previous) {
            ++count;
            previous = entry.first.service;
            first = false;
        }
    }
    return count;
}

bool registry::service_exists(const std::string& service) const {
    for (const auto& entry : instances_) {
        if (entry.first.service == service) {
            return true;
        }
    }
    return false;
}

std::size_t registry::metadata_size(const metadata_list& meta) {
    const std::size_t limit = std::numeric_limits<std::size_t>::max();
    std::size_t total = 0;
    for (const auto& entry : meta) {
        const std::size_t part = entry.first.size() + entry.second.size();
        if (total > limit - part) {
            return limit;
        }
        total += part;
    }
    return total;
}

std::shared_ptr<registry_snapshot>
registry::snapshot_with(const service_instance& instance, u64 index) const {
    auto next = std::make_shared<registry_snapshot>();
    next->index = index;
    const std::shared_ptr<const registry_snapshot> current =
        snapshot_.load(std::memory_order_acquire);
    next->instances.reserve(current->instances.size() + 1);
    bool replaced = false;
    for (const auto& entry : current->instances) {
        if (entry.service == instance.service &&
            entry.node_id == instance.node_id) {
            next->instances.push_back(instance);
            replaced = true;
        } else {
            next->instances.push_back(entry);
        }
    }
    if (!replaced) {
        next->instances.push_back(instance);
    }
    return next;
}

void registry::notify(const catalog_event& event) {
    // Sinks are called with the sink-list mutex held so that remove_sink() can
    // act as a barrier for a sink being destroyed. Sinks must be fast and must
    // not re-enter the registry.
    std::lock_guard<std::mutex> lock(sinks_mutex_);
    for (const auto& entry : sinks_) {
        if (entry.sink != nullptr) {
            entry.sink(entry.context, event);
        }
    }
}

result<write_result> registry::register_instance(service_instance instance,
                                                 const write_context& context) {
    return register_with(std::move(instance), context, /*lease_id=*/0);
}

result<write_result> registry::register_instance(service_instance instance,
                                                 const write_context& context,
                                                 u64 lease_id) {
    return register_with(std::move(instance), context, lease_id);
}

result<write_result> registry::register_with(service_instance instance,
                                             const write_context& context,
                                             u64 lease_id) {
    if (instance.service.empty() || instance.node_id.empty()) {
        return reject(rpc_code::invalid_argument, "service and node_id are required",
                      reason_invalid);
    }
    // Endpoint kind: an empty host cannot name a network endpoint and cannot
    // name an in-process endpoint either. Port 0 with a non-empty host is the
    // established inproc sentinel ("address by name"), so it is accepted; a
    // network endpoint carries a non-zero port.
    if (trim(instance.address.host).empty()) {
        return reject(rpc_code::invalid_argument,
                      "endpoint host is required", reason_invalid);
    }
    if (instance.state == instance_state::deregistered) {
        return reject(rpc_code::invalid_argument,
                      "register_instance cannot write a tombstone", reason_invalid);
    }
    if (config_.max_metadata_bytes != 0 &&
        metadata_size(instance.meta) > config_.max_metadata_bytes) {
        return reject(rpc_code::resource_exhausted, "metadata exceeds the configured limit",
                      reason_limit);
    }
    if (!check_authority(context, instance.node_id)) {
        return reject(rpc_code::failed_precondition,
                      "write authority does not belong to this node", reason_authority);
    }

    const key id{instance.service, instance.node_id};
    catalog_event event;
    write_result outcome;
    bool idempotent = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        const auto existing = instances_.find(id);
        if (existing == instances_.end()) {
            // A purged tombstone keeps its incarnation high-water: the visible
            // row is gone, but an old incarnation must not resurrect the key.
            const auto watermark = tombstone_watermark_.find(id);
            if (watermark != tombstone_watermark_.end() &&
                instance.incarnation <= watermark->second) {
                return reject(rpc_code::failed_precondition,
                              "tombstone blocks resurrection with an old incarnation",
                              reason_tombstone);
            }
            if (config_.max_total_instances != 0 &&
                instances_.size() >= config_.max_total_instances) {
                return reject(rpc_code::resource_exhausted,
                              "catalog instance limit reached", reason_limit);
            }
            if (config_.max_services != 0 && !service_exists(instance.service) &&
                count_services() >= config_.max_services) {
                return reject(rpc_code::resource_exhausted,
                              "catalog service limit reached", reason_limit);
            }
            if (config_.max_instances_per_service != 0 &&
                count_instances(instance.service) >= config_.max_instances_per_service) {
                return reject(rpc_code::resource_exhausted,
                              "per-service instance limit reached", reason_limit);
            }
        } else {
            service_instance& stored = existing->second;
            if (stored.state == instance_state::deregistered) {
                // A tombstone rejects every incarnation that is not strictly
                // newer. This is the "NULL cannot penetrate" rule.
                if (instance.incarnation <= stored.incarnation) {
                    return reject(rpc_code::failed_precondition,
                                  "tombstone blocks resurrection with an old incarnation",
                                  reason_tombstone);
                }
            } else if (instance.incarnation < stored.incarnation) {
                return reject(rpc_code::failed_precondition,
                              "stale incarnation", reason_stale_incarnation);
            } else if (instance.incarnation == stored.incarnation &&
                       config_.strict_incarnation) {
                // Exactly-once anchor: one (key, incarnation) names one write.
                // Opt-in because the historical in-process contract allowed a
                // same-incarnation endpoint update; network deployments enable
                // it together with a single registration writer.
                if (identity_equal(instance, stored,
                                   !config_.separate_state_writers)) {
                    idempotent = true;
                } else {
                    return reject(rpc_code::failed_precondition,
                                  "conflicting registration for the same incarnation",
                                  reason_conflict);
                }
            }
        }

        const u64 principal = principal_hash(context.node_id);
        // S7: epoch before fence. A stale epoch means the writer has been
        // replaced and must yield; reporting stale_fence instead would tell it
        // to re-anchor and keep writing.
        const fence_verdict epoch_verdict =
            check_writer_epoch(id, context.writer_epoch);
        if (epoch_verdict == fence_verdict::required) {
            return reject(rpc_code::failed_precondition,
                          "a writer epoch is required for this key",
                          reason_epoch_required);
        }
        if (epoch_verdict == fence_verdict::stale_writer) {
            return reject(rpc_code::failed_precondition,
                          "writer epoch is older than the recorded one",
                          reason_stale_writer);
        }
        const fence_verdict verdict =
            check_fence(id, context.fence, existing == instances_.end(), idempotent,
                        principal, lease_id, /*issuing=*/false);
        if (verdict == fence_verdict::stale) {
            return reject_stale_fence_locked(id);
        }
        if (verdict == fence_verdict::out_of_range) {
            return reject(rpc_code::failed_precondition,
                          "fencing token is out of range", reason_fence_out_of_range);
        }
        if (verdict == fence_verdict::required) {
            return reject(rpc_code::failed_precondition,
                          "a fencing token is required for this key",
                          reason_fence_required);
        }
        if (verdict == fence_verdict::not_issued) {
            return reject(rpc_code::failed_precondition,
                          "fencing token was not issued for this writer",
                          reason_not_issued);
        }

        if (idempotent) {
            // Replay of an accepted write: the index and the event stream do
            // not move, and the recorded result is returned unchanged.
            const auto recorded = last_fence_.find(id);
            outcome.index = existing->second.revision;
            outcome.fence = recorded != last_fence_.end()
                                ? recorded->second.token
                                : existing->second.revision;
            outcome.incarnation = existing->second.incarnation;
            return outcome;
        }

        if (existing != instances_.end() &&
            config_.separate_state_writers &&
            existing->second.state != instance_state::deregistered) {
            // The state is owned by the health/lease writer. A registration
            // updates identity fields only and keeps the current verdict.
            instance.state = existing->second.state;
        }

        if (index_ == std::numeric_limits<u64>::max()) {
            return reject(rpc_code::resource_exhausted,
                          "registry index exhausted", reason_overflow);
        }

        const u64 next_index =
            context.raft_index > 0 ? context.raft_index : index_ + 1;
        const auto accepted_fence = last_fence_.find(id);
        const u64 key_high_water =
            accepted_fence != last_fence_.end() ? accepted_fence->second.token : 0;
        u64 next_fence = 0;
        if (!allocate_fence(context.fence, next_index, key_high_water,
                            next_fence)) {
            return reject(rpc_code::resource_exhausted, "fence exhausted",
                          reason_overflow);
        }
        const u64 returned = record_fence_locked(id, context.fence, next_fence,
                                                 principal, lease_id);
        record_writer_epoch_locked(id, context.writer_epoch);
        instance.revision = next_index;

        auto next = snapshot_with(instance, next_index);
        if (existing == instances_.end()) {
            instances_.emplace(id, instance);
        } else {
            existing->second = instance;
        }
        index_ = next_index;
        std::shared_ptr<const registry_snapshot> published(std::move(next));
        snapshot_.store(published, std::memory_order_release);

        event.index = next_index;
        event.kind = catalog_event_kind::put;
        event.instance = instance;
        outcome.index = next_index;
        outcome.fence = returned;
        outcome.incarnation = instance.incarnation;
    }

    notify(event);
    return outcome;
}

result<write_result> registry::deregister(const std::string& service,
                                          const std::string& node_id,
                                          const write_context& context) {
    return deregister_with(service, node_id, context, /*lease_id=*/0);
}

result<write_result> registry::deregister(const std::string& service,
                                          const std::string& node_id,
                                          const write_context& context,
                                          u64 lease_id) {
    return deregister_with(service, node_id, context, lease_id);
}

result<write_result> registry::deregister_with(const std::string& service,
                                               const std::string& node_id,
                                               const write_context& context,
                                               u64 lease_id) {
    if (service.empty() || node_id.empty()) {
        return reject(rpc_code::invalid_argument, "service and node_id are required",
                      reason_invalid);
    }

    const key id{service, node_id};
    catalog_event event;
    write_result outcome;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        const auto existing = instances_.find(id);
        if (existing == instances_.end()) {
            return reject(rpc_code::not_found, "instance is not registered", reason_not_live);
        }

        const service_instance& stored = existing->second;
        if (!check_authority(context, stored.node_id)) {
            return reject(rpc_code::failed_precondition,
                          "write authority does not belong to this node", reason_authority);
        }
        const u64 principal = principal_hash(context.node_id);
        // S7: epoch before fence (see register_with).
        const fence_verdict epoch_verdict =
            check_writer_epoch(id, context.writer_epoch);
        if (epoch_verdict == fence_verdict::required) {
            return reject(rpc_code::failed_precondition,
                          "a writer epoch is required for this key",
                          reason_epoch_required);
        }
        if (epoch_verdict == fence_verdict::stale_writer) {
            return reject(rpc_code::failed_precondition,
                          "writer epoch is older than the recorded one",
                          reason_stale_writer);
        }
        const fence_verdict verdict =
            check_fence(id, context.fence, /*creating=*/false,
                        /*exact_replay=*/false, principal, lease_id,
                        /*issuing=*/false);
        if (verdict == fence_verdict::stale) {
            return reject_stale_fence_locked(id);
        }
        if (verdict == fence_verdict::out_of_range) {
            return reject(rpc_code::failed_precondition,
                          "fencing token is out of range", reason_fence_out_of_range);
        }
        if (verdict == fence_verdict::required) {
            return reject(rpc_code::failed_precondition,
                          "a fencing token is required for this key",
                          reason_fence_required);
        }
        if (verdict == fence_verdict::not_issued) {
            return reject(rpc_code::failed_precondition,
                          "fencing token was not issued for this writer",
                          reason_not_issued);
        }

        if (stored.state == instance_state::deregistered) {
            const auto fence_it = last_fence_.find(id);
            outcome.index = index_;
            outcome.fence =
                fence_it == last_fence_.end() ? index_ : fence_it->second.token;
            outcome.incarnation = stored.incarnation;
            return outcome;
        }
        if (context.incarnation < stored.incarnation) {
            return reject(rpc_code::failed_precondition, "stale incarnation",
                          reason_stale_incarnation);
        }
        if (index_ == std::numeric_limits<u64>::max()) {
            return reject(rpc_code::resource_exhausted,
                          "registry index exhausted", reason_overflow);
        }
        if (stored.incarnation == std::numeric_limits<u64>::max()) {
            return reject(rpc_code::resource_exhausted,
                          "incarnation exhausted", reason_overflow);
        }

        service_instance updated = stored;
        updated.state = instance_state::deregistered;
        updated.incarnation = stored.incarnation + 1;
        if (context.incarnation > updated.incarnation) {
            updated.incarnation = context.incarnation;
        }
        const u64 next_index =
            context.raft_index > 0 ? context.raft_index : index_ + 1;
        const auto accepted_fence = last_fence_.find(id);
        const u64 key_high_water =
            accepted_fence != last_fence_.end() ? accepted_fence->second.token : 0;
        u64 next_fence = 0;
        if (!allocate_fence(context.fence, next_index, key_high_water,
                            next_fence)) {
            return reject(rpc_code::resource_exhausted, "fence exhausted",
                          reason_overflow);
        }
        const u64 returned = record_fence_locked(id, context.fence, next_fence,
                                                 principal, lease_id);
        record_writer_epoch_locked(id, context.writer_epoch);
        updated.revision = next_index;

        // Keep the tombstone incarnation high-water even after a later purge
        // physically removes this row.
        u64& watermark = tombstone_watermark_[id];
        if (updated.incarnation > watermark) {
            watermark = updated.incarnation;
        }

        auto next = snapshot_with(updated, next_index);
        existing->second = updated;
        index_ = next_index;
        std::shared_ptr<const registry_snapshot> published(std::move(next));
        snapshot_.store(published, std::memory_order_release);

        event.index = next_index;
        event.kind = catalog_event_kind::put;
        event.instance = updated;
        outcome.index = next_index;
        outcome.fence = returned;
        outcome.incarnation = updated.incarnation;
    }

    notify(event);
    return outcome;
}

result<write_result> registry::set_state(const std::string& service,
                                         const std::string& node_id,
                                         instance_state state,
                                         const write_context& context) {
    return set_state_with(service, node_id, state, context, /*lease_id=*/0);
}

result<write_result> registry::set_state(const std::string& service,
                                         const std::string& node_id,
                                         instance_state state,
                                         const write_context& context,
                                         u64 lease_id) {
    return set_state_with(service, node_id, state, context, lease_id);
}

result<write_result> registry::set_state_with(const std::string& service,
                                              const std::string& node_id,
                                              instance_state state,
                                              const write_context& context,
                                              u64 lease_id) {
    if (service.empty() || node_id.empty()) {
        return reject(rpc_code::invalid_argument, "service and node_id are required",
                      reason_invalid);
    }
    if (state == instance_state::deregistered) {
        return reject(rpc_code::invalid_argument,
                      "set_state cannot write a tombstone; use deregister", reason_invalid);
    }

    const key id{service, node_id};
    catalog_event event;
    write_result outcome;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        const auto existing = instances_.find(id);
        if (existing == instances_.end()) {
            return reject(rpc_code::not_found, "instance is not registered", reason_not_live);
        }
        const service_instance& stored = existing->second;
        if (!check_authority(context, stored.node_id)) {
            return reject(rpc_code::failed_precondition,
                          "write authority does not belong to this node", reason_authority);
        }
        const u64 principal = principal_hash(context.node_id);
        // S7: epoch before fence. A stale epoch means the writer has been
        // replaced and must yield; reporting stale_fence instead would tell it
        // to re-anchor and keep writing.
        const fence_verdict epoch_verdict =
            check_writer_epoch(id, context.writer_epoch);
        if (epoch_verdict == fence_verdict::required) {
            return reject(rpc_code::failed_precondition,
                          "a writer epoch is required for this key",
                          reason_epoch_required);
        }
        if (epoch_verdict == fence_verdict::stale_writer) {
            return reject(rpc_code::failed_precondition,
                          "writer epoch is older than the recorded one",
                          reason_stale_writer);
        }
        const fence_verdict verdict =
            check_fence(id, context.fence, /*creating=*/false,
                        /*exact_replay=*/false, principal, lease_id,
                        /*issuing=*/false);
        if (verdict == fence_verdict::stale) {
            return reject_stale_fence_locked(id);
        }
        if (verdict == fence_verdict::out_of_range) {
            return reject(rpc_code::failed_precondition,
                          "fencing token is out of range", reason_fence_out_of_range);
        }
        if (verdict == fence_verdict::required) {
            return reject(rpc_code::failed_precondition,
                          "a fencing token is required for this key",
                          reason_fence_required);
        }
        if (verdict == fence_verdict::not_issued) {
            return reject(rpc_code::failed_precondition,
                          "fencing token was not issued for this writer",
                          reason_not_issued);
        }
        if (stored.state == instance_state::deregistered) {
            return reject(rpc_code::not_found,
                          "instance is deregistered", reason_not_live);
        }
        if (context.incarnation < stored.incarnation) {
            return reject(rpc_code::failed_precondition, "stale incarnation",
                          reason_stale_incarnation);
        }
        if (index_ == std::numeric_limits<u64>::max()) {
            return reject(rpc_code::resource_exhausted,
                          "registry index exhausted", reason_overflow);
        }

        service_instance updated = stored;
        updated.state = state;
        const u64 next_index =
            context.raft_index > 0 ? context.raft_index : index_ + 1;
        const auto accepted_fence = last_fence_.find(id);
        const u64 key_high_water =
            accepted_fence != last_fence_.end() ? accepted_fence->second.token : 0;
        u64 next_fence = 0;
        if (!allocate_fence(context.fence, next_index, key_high_water,
                            next_fence)) {
            return reject(rpc_code::resource_exhausted, "fence exhausted",
                          reason_overflow);
        }
        const u64 returned = record_fence_locked(id, context.fence, next_fence,
                                                 principal, lease_id);
        record_writer_epoch_locked(id, context.writer_epoch);
        updated.revision = next_index;

        auto next = snapshot_with(updated, next_index);
        existing->second = updated;
        index_ = next_index;
        std::shared_ptr<const registry_snapshot> published(std::move(next));
        snapshot_.store(published, std::memory_order_release);

        event.index = next_index;
        event.kind = catalog_event_kind::put;
        event.instance = updated;
        outcome.index = next_index;
        outcome.fence = returned;
        outcome.incarnation = updated.incarnation;
    }

    notify(event);
    return outcome;
}

result<catalog_result> registry::query(const query_options& options) const {
    const auto snap = snapshot();
    if (snap == nullptr) {
        return reject(rpc_code::internal, "registry snapshot is unavailable",
                      "internal");
    }
    catalog_result out;
    out.index = snap->index;
    out.instances.reserve(snap->instances.size());

    for (const auto& instance : snap->instances) {
        if (!options.service.empty() && instance.service != options.service) {
            continue;
        }
        if (!options.node_id.empty() && instance.node_id != options.node_id) {
            continue;
        }
        if (!options.include_deregistered &&
            instance.state == instance_state::deregistered) {
            continue;
        }
        if (options.passing_only && instance.state != instance_state::passing) {
            continue;
        }
        if (!options.tag.empty() && !tag_matches(instance.meta, options.tag)) {
            continue;
        }
        out.instances.push_back(instance);
    }

    std::sort(out.instances.begin(), out.instances.end(), instance_less);
    return out;
}

std::shared_ptr<const registry_snapshot> registry::snapshot() const noexcept {
    return snapshot_.load(std::memory_order_acquire);
}

u64 registry::index() const noexcept {
    const auto snap = snapshot();
    return snap != nullptr ? snap->index : 0;
}

u64 registry::fence() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return published_fence_locked();
}

result<u64> registry::issue_fence(const std::string& service,
                                  const std::string& node_id, u64 lease_id) {
    if (service.empty() || node_id.empty()) {
        return reject(rpc_code::invalid_argument, "service and node_id are required",
                      reason_invalid);
    }
    const key id{service, node_id};
    std::lock_guard<std::mutex> lock(mutex_);
    if (last_global_fence_ == std::numeric_limits<u64>::max()) {
        return reject(rpc_code::resource_exhausted, "fence exhausted",
                      reason_overflow);
    }
    // max(global, per-key high-water) + 1: the allocator must jump over a
    // barrier rather than walk towards it one grant at a time, and the new
    // token must be strictly above the token it replaces so the previous
    // holder is fenced out immediately.
    u64 floor = published_fence_locked();
    const auto accepted = last_fence_.find(id);
    if (accepted != last_fence_.end() && accepted->second.token > floor) {
        floor = accepted->second.token;
    }
    if (floor == std::numeric_limits<u64>::max()) {
        return reject(rpc_code::resource_exhausted, "fence exhausted",
                      reason_overflow);
    }
    const u64 token = floor + 1;
    issued_fence& record = last_fence_[id];
    record.token = token;
    // The principal is the issuer's own identity: next_fence() is called by the
    // authority on behalf of the key owner, and the write must come from the
    // same node id.
    record.principal_hash = principal_hash(node_id);
    record.lease_id = lease_id;
    record.outstanding = true;
    if (token > last_global_fence_) {
        last_global_fence_ = token;
    }
    fenced_keys_[id] = true;
    return token;
}

result<u64> registry::next_fence(const std::string& service,
                                 const std::string& node_id) {
    return issue_fence(service, node_id, /*lease_id=*/0);
}

result<u64> registry::next_fence(const std::string& service,
                                 const std::string& node_id, u64 lease_id) {
    return issue_fence(service, node_id, lease_id);
}

result<u64> registry::next_writer_epoch() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (last_global_fence_ == std::numeric_limits<u64>::max()) {
        return reject(rpc_code::resource_exhausted, "fence exhausted",
                      reason_overflow);
    }
    // The epoch shares the fence allocation counter: a bridge epoch is a
    // reservation in the same number space, so a restored catalog can never hand
    // out an epoch at or below one a surviving writer already used. The
    // reservation is deliberately not a fence issuance (it touches no key), so
    // it cannot spend or rebind any token.
    return ++last_global_fence_;
}

result<u64> registry::fence_floor(const std::string& service,
                                  const std::string& node_id) const {
    if (service.empty() || node_id.empty()) {
        return reject(rpc_code::invalid_argument, "service and node_id are required",
                      reason_invalid);
    }
    const key id{service, node_id};
    std::lock_guard<std::mutex> lock(mutex_);
    const auto accepted = last_fence_.find(id);
    // The per-key high-water, deliberately not the global allocation: a lease
    // barrier can hold one key far above the global counter, and the whole
    // point of the floor is to let an allocator see that.
    return accepted != last_fence_.end() ? accepted->second.token : 0;
}

result<u64> registry::add_sink(catalog_sink sink, void* context) {
    if (sink == nullptr) {
        return reject(rpc_code::invalid_argument, "sink is null", reason_invalid);
    }
    std::lock_guard<std::mutex> lock(sinks_mutex_);
    if (config_.max_sinks != 0 && sinks_.size() >= config_.max_sinks) {
        return reject(rpc_code::resource_exhausted, "sink limit reached", reason_limit);
    }
    u64 id = next_sink_id_++;
    if (id == 0) {
        id = next_sink_id_++;
    }
    sinks_.push_back(sink_entry{id, sink, context});
    return id;
}

status registry::remove_sink(u64 sink_id) {
    std::lock_guard<std::mutex> lock(sinks_mutex_);
    for (auto it = sinks_.begin(); it != sinks_.end(); ++it) {
        if (it->id == sink_id) {
            sinks_.erase(it);
            return status{};
        }
    }
    return status{};
}

std::size_t registry::purge_expired_tombstones() {
    std::vector<key> expired;
    std::vector<catalog_event> events;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : instances_) {
            const service_instance& instance = entry.second;
            if (instance.state != instance_state::deregistered) {
                continue;
            }
            const u64 age = index_ >= instance.revision ? index_ - instance.revision : 0;
            // tombstone_grace == 0 means "eligible immediately".
            if (age >= config_.tombstone_grace) {
                expired.push_back(entry.first);
            }
        }
        if (expired.empty()) {
            return 0;
        }
        // Each physical removal changes the include_deregistered view, so each
        // one is a logical mutation with its own index and remove event. The
        // index can never denote two different contents.
        if (expired.size() > std::numeric_limits<u64>::max() - index_) {
            return 0;
        }

        std::set<key, key_less> removed_keys(expired.begin(), expired.end());
        auto next = std::make_shared<registry_snapshot>();
        next->instances.reserve(instances_.size() - expired.size());
        for (const auto& entry : instances_) {
            if (removed_keys.find(entry.first) == removed_keys.end()) {
                next->instances.push_back(entry.second);
            }
        }

        const u64 base = index_ + 1;
        u64 offset = 0;
        for (const auto& id : expired) {
            const auto found = instances_.find(id);
            if (found == instances_.end()) {
                continue;
            }
            const service_instance removed = found->second;
            // Keep the tombstone incarnation high-water; the per-key fence
            // high-water in last_fence_ is kept as well.
            u64& watermark = tombstone_watermark_[id];
            if (removed.incarnation > watermark) {
                watermark = removed.incarnation;
            }
            instances_.erase(found);
            events.push_back(catalog_event{base + offset,
                                           catalog_event_kind::remove, removed});
            ++offset;
        }
        index_ = base + expired.size() - 1;
        next->index = index_;
        std::shared_ptr<const registry_snapshot> published(std::move(next));
        snapshot_.store(published, std::memory_order_release);
    }
    for (const auto& event : events) {
        notify(event);
    }
    return expired.size();
}

// --- restore_from_log --------------------------------------------------------
//
// A friend free function (see registry.hpp): it needs the private catalog and
// fence state to rebuild them atomically, and keeping it out of the class
// avoids adding a log handle member, which would grow `registry` by 8 bytes
// (a second MAJOR layout change).

namespace {

// The payload codec: version(u8) | state(u8) | service | node_id | host
// (each length-prefixed u32) | port(u16) | weight(u32) | incarnation(u64) |
// meta_count(u32) then per entry key/value (length-prefixed u32). The first
// 16 bytes carry the record's fence and incarnation; the rest is the instance
// body. Every read is bounds-checked, so a truncated payload is an error
// rather than a partially parsed instance.
constexpr std::uint8_t kRestoreVersion = 1;
constexpr std::size_t kRestoreHeader = 16;

class payload_reader {
public:
    payload_reader(const std::vector<std::byte>& bytes, std::size_t offset)
        : bytes_(bytes), pos_(offset) {}

    bool u8(std::uint8_t& out) {
        if (pos_ + 1 > bytes_.size()) {
            return false;
        }
        out = std::to_integer<std::uint8_t>(bytes_[pos_]);
        pos_ += 1;
        return true;
    }
    bool u16(std::uint16_t& out) {
        if (pos_ + 2 > bytes_.size()) {
            return false;
        }
        out = static_cast<std::uint16_t>(
            std::to_integer<unsigned>(bytes_[pos_]) |
            (static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes_[pos_ + 1]))
             << 8));
        pos_ += 2;
        return true;
    }
    bool u32(std::uint32_t& out) {
        if (pos_ + 4 > bytes_.size()) {
            return false;
        }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(
                     std::to_integer<unsigned>(bytes_[pos_ + i]))
                 << (8 * i);
        }
        out = v;
        pos_ += 4;
        return true;
    }
    bool u64(std::uint64_t& out) {
        if (pos_ + 8 > bytes_.size()) {
            return false;
        }
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<std::uint64_t>(
                     std::to_integer<unsigned>(bytes_[pos_ + i]))
                 << (8 * i);
        }
        out = v;
        pos_ += 8;
        return true;
    }
    bool str(std::string& out) {
        std::uint32_t len = 0;
        if (!u32(len) || len > bytes_.size() - pos_) {
            return false;
        }
        out.assign(reinterpret_cast<const char*>(bytes_.data()) + pos_, len);
        pos_ += len;
        return true;
    }
    [[nodiscard]] bool at_end() const { return pos_ == bytes_.size(); }

private:
    const std::vector<std::byte>& bytes_;
    std::size_t pos_;
};

bool decode_restore_payload(const std::vector<std::byte>& payload,
                            u64& fence, u64& incarnation,
                            service_instance& out) {
    if (payload.size() < kRestoreHeader) {
        return false;
    }
    fence = 0;
    incarnation = 0;
    for (int i = 0; i < 8; ++i) {
        fence |= static_cast<u64>(
                     std::to_integer<unsigned>(payload[static_cast<std::size_t>(i)]))
                 << (8 * i);
        incarnation |= static_cast<u64>(
                           std::to_integer<unsigned>(
                               payload[8 + static_cast<std::size_t>(i)]))
                       << (8 * i);
    }
    payload_reader r(payload, kRestoreHeader);
    std::uint8_t version = 0;
    std::uint8_t state = 0;
    if (!r.u8(version) || version != kRestoreVersion) {
        return false;
    }
    if (!r.u8(state) ||
        state > static_cast<std::uint8_t>(instance_state::deregistered)) {
        return false;
    }
    out.state = static_cast<instance_state>(state);
    if (!r.str(out.service) || !r.str(out.node_id) || !r.str(out.address.host)) {
        return false;
    }
    if (!r.u16(out.address.port) || !r.u32(out.weight) || !r.u64(out.incarnation)) {
        return false;
    }
    std::uint32_t meta_count = 0;
    if (!r.u32(meta_count) || meta_count > 100000u) {
        return false;
    }
    for (std::uint32_t i = 0; i < meta_count; ++i) {
        std::string key;
        std::string value;
        if (!r.str(key) || !r.str(value)) {
            return false;
        }
        out.meta.set(std::move(key), std::move(value));
    }
    return r.at_end();
}

// One folded, validated entry. The catalog key is private to `registry`, so
// the fold uses a plain pair; the apply step constructs the private key.
struct folded_record {
    u64 index = 0;
    u64 fence = 0;
    u64 incarnation = 0;
    service_instance instance;
};

} // namespace

result<std::size_t> restore_from_log(registry& target, const registry_log& log) {
    const auto reject = [](rpc_code code, const std::string& message,
                           const char* reason) {
        rpc_error error(code, message);
        error.metadata.set("registry.reason", reason);
        return error;
    };

    // R8: replay()'s own error is propagated verbatim; nothing is applied.
    const auto replayed = log.replay(0);
    if (!replayed.ok()) {
        return replayed.error();
    }
    const std::vector<log_record>& stream = replayed.value();

    // G1: the log's index space must be dense across ALL record kinds. The
    // consensus layer interleaves its own record kinds in the same log
    // (registry_log.hpp:64-73), so a gap in the *registry-event* subsequence is
    // the expected shape; a gap in the *full* sequence means records were lost,
    // which is corruption. Checking the full sequence is what keeps the step1
    // guarantee (a torn or truncated tail is rejected) while letting raft
    // records occupy the indices between registry events.
    //
    // The stream is folded by index first, last-wins, because replay() returns
    // the raw sequence and `registry_log` is a replaceable interface: a backend
    // may hand back a duplicate index (7.1.6b), and 1,2,2 is dense after folding.
    std::map<u64, log_record_kind> index_kinds;
    for (const auto& record : stream) {
        index_kinds[record.index] = record.kind;
    }
    u64 expected = 1;
    for (const auto& entry : index_kinds) {
        if (entry.first != expected) {
            return reject(rpc_code::invalid_argument,
                          "restore: index gap (expected " +
                              std::to_string(expected) + ", got " +
                              std::to_string(entry.first) + ")",
                          reason_invalid);
        }
        ++expected;
    }

    // Fold last-wins by index. A std::map keeps the output ordered by index,
    // which is the apply order.
    std::map<u64, folded_record> folded;
    for (const auto& record : stream) {
        // Only registry_event carries a catalog mutation. The other kinds
        // (raft_term/raft_vote/raft_snapshot/raft_entry) are consumed by the
        // consensus layer, not by the catalog restore, and their payloads are
        // not in the registry event codec. Skipping them by kind -- not by
        // "failed to decode" -- is what keeps a corrupted event visible: a
        // registry_event whose payload is undecodable still falls through to
        // the decode below and is rejected.
        if (record.kind != log_record_kind::registry_event) {
            continue;
        }
        folded_record decoded;
        if (!decode_restore_payload(record.payload, decoded.fence,
                                    decoded.incarnation, decoded.instance)) {
            return reject(rpc_code::invalid_argument,
                          "restore: record payload is not decodable",
                          reason_invalid);
        }
        decoded.index = record.index;
        // Last-wins: a later record at the same index replaces the earlier one.
        folded[record.index] = std::move(decoded);
    }

    // The log's high-water index is the last index of ANY kind, taken from the
    // full fold above: a trailing raft record occupies index space the catalog
    // does not apply but must still account for, or the next append would land
    // on top of it and truncate it.
    const u64 stream_max_index = index_kinds.empty() ? 0 : index_kinds.rbegin()->first;

    // G5: an empty stream is a successful no-op. A stream with no registry
    // event is too: there is no catalog mutation to apply, and the index still
    // advances to the log high-water so a later append cannot overwrite the
    // consensus records.
    if (folded.empty()) {
        if (stream_max_index == 0) {
            return std::size_t{0};
        }
        std::lock_guard<std::mutex> lock(target.mutex_);
        if (stream_max_index <= target.index_) {
            return reject(rpc_code::failed_precondition,
                          "restore: log is not ahead of the registry",
                          "stale_log");
        }
        target.index_ = stream_max_index;
        return std::size_t{0};
    }

    const u64 stream_max_fence = [&]() {
        u64 max_fence = 0;
        for (const auto& entry : folded) {
            if (entry.second.fence > max_fence) {
                max_fence = entry.second.fence;
            }
        }
        return max_fence;
    }();
    // R5 is per key, not global: a log that only ever mentions key A must not
    // be rejected because key B (which the log never mentions) has a higher
    // water-mark on the target. Fold the stream a second time by key, keeping
    // the maximum fence for each key (the log is append-only and the folded
    // order is by ascending index, so the last record of a key is its newest).
    //
    // `stream_max_fence` is deliberately folded over registry events only, while
    // `stream_max_index` (above) is the high-water of every kind: a raft record
    // occupies an index but carries no fence, so it can raise the index without
    // touching the fence water-mark.
    std::map<registry::key, u64, registry::key_less> fence_by_key;
    for (const auto& entry : folded) {
        const registry::key id{entry.second.instance.service,
                               entry.second.instance.node_id};
        u64& high = fence_by_key[id];
        if (entry.second.fence > high) {
            high = entry.second.fence;
        }
    }

    std::lock_guard<std::mutex> lock(target.mutex_);

    // G3: never roll a newer catalog back to an older log.
    if (stream_max_index <= target.index_) {
        return reject(rpc_code::failed_precondition,
                      "restore: log is not ahead of the registry",
                      "stale_log");
    }
    // R5: a fence regression on any key the log touches would let an old
    // holder of that key write again. A key the log does not mention has no
    // regression to check, so the global water-mark is deliberately not
    // compared here.
    for (const auto& entry : fence_by_key) {
        const auto it = target.last_fence_.find(entry.first);
        if (it != target.last_fence_.end() && entry.second < it->second.token) {
            return reject(rpc_code::failed_precondition,
                          "restore: log fence is below the registry water-mark",
                          "fence_regression");
        }
    }

    // R4: one (service, node_id, incarnation) names one write. The stream is
    // validated before anything is applied, so a conflict leaves the registry
    // untouched.
    struct identity {
        std::string service;
        std::string node_id;
        u64 incarnation = 0;

        bool operator<(const identity& other) const {
            if (service != other.service) {
                return service < other.service;
            }
            if (node_id != other.node_id) {
                return node_id < other.node_id;
            }
            return incarnation < other.incarnation;
        }
    };
    std::map<identity, const folded_record*> seen;
    for (const auto& entry : folded) {
        const folded_record& record = entry.second;
        const identity id{record.instance.service, record.instance.node_id,
                          record.incarnation};
        const auto existing = seen.find(id);
        if (existing != seen.end()) {
            const folded_record& other = *existing->second;
            const bool same = other.instance.address == record.instance.address &&
                              other.instance.weight == record.instance.weight &&
                              other.instance.state == record.instance.state &&
                              other.instance.meta.size() ==
                                  record.instance.meta.size();
            const bool meta_same = [&]() {
                if (other.instance.meta.size() != record.instance.meta.size()) {
                    return false;
                }
                for (const auto& item : other.instance.meta) {
                    const std::string* value = record.instance.meta.find(item.first);
                    if (value == nullptr || *value != item.second) {
                        return false;
                    }
                }
                return true;
            }();
            if (!same || !meta_same) {
                return reject(rpc_code::failed_precondition,
                              "restore: conflicting replay for one incarnation",
                              reason_conflict);
            }
            continue;
        }
        seen.emplace(id, &record);
    }

    // Apply. The target's index moves to the log's high-water of every kind
    // (`stream_max_index`), and the fence water-mark moves to the registry
    // events' maximum. The tombstones and incarnation high-water are rebuilt
    // from the same records so a later purge/registration cannot resurrect an
    // old row.
    auto next = std::make_shared<registry_snapshot>();
    next->instances.reserve(folded.size());
    for (const auto& entry : folded) {
        const folded_record& record = entry.second;
        service_instance instance = record.instance;
        // The payload header already carries the incarnation; the body's copy is
        // an independent encoding of the same field, so the header wins.
        instance.revision = record.index;
        instance.incarnation = record.incarnation;
        next->instances.push_back(instance);
        const registry::key id{instance.service, instance.node_id};
        target.instances_[id] = instance;
        target.last_fence_[id] = registry::issued_fence{
            record.fence, registry::principal_hash(instance.node_id), 0, false,
            // The epoch and the fence share one allocation counter
            // (next_writer_epoch() is ++last_global_fence_), so the highest fence
            // this key accepted in the log is also the lower bound of any epoch
            // that may write it. Zero would let a pre-restart writer overwrite
            // the restored data; UINT64_MAX would starve the key forever.
            /*writer_epoch=*/record.fence};
        if (instance.state == instance_state::deregistered) {
            u64& watermark = target.tombstone_watermark_[id];
            if (instance.incarnation > watermark) {
                watermark = instance.incarnation;
            }
        }
        if (record.fence != 0) {
            target.fenced_keys_[id] = true;
        }
    }
    next->index = stream_max_index;
    target.index_ = stream_max_index;
    if (stream_max_fence > target.last_global_fence_) {
        target.last_global_fence_ = stream_max_fence;
    }
    std::shared_ptr<const registry_snapshot> published(std::move(next));
    target.snapshot_.store(published, std::memory_order_release);

    return folded.size();
}

} // namespace cluster
} // namespace crpc
