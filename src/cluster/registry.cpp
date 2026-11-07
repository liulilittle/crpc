// CRPC -- service catalog implementation.
#include <crpc/cluster/registry.hpp>

#include <algorithm>
#include <limits>
#include <set>
#include <string>
#include <utility>

namespace crpc {
namespace cluster {
namespace {

constexpr const char* reason_authority = "authority";
constexpr const char* reason_stale_incarnation = "stale_incarnation";
constexpr const char* reason_stale_fence = "stale_fence";
constexpr const char* reason_fence_required = "fence_required";
constexpr const char* reason_fence_out_of_range = "fence_out_of_range";
constexpr const char* reason_tombstone = "tombstone";
constexpr const char* reason_conflict = "conflict";
constexpr const char* reason_not_live = "not_live";
constexpr const char* reason_limit = "limit";
constexpr const char* reason_overflow = "index_overflow";
constexpr const char* reason_invalid = "invalid";

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

u64 registry::record_fence_locked(const key& id, u64 requested,
                                  u64 authority) {
    u64& key_fence = last_fence_[id];
    if (requested > key_fence) {
        key_fence = requested;
    }
    if (authority > key_fence) {
        key_fence = authority;
    }
    if (requested != 0) {
        fenced_keys_[id] = true;
    }
    // The effective per-key high-water, not the bare authority allocation: a
    // fence_barrier can hold the key above the global allocator, and a caller
    // must be able to feed the returned token back without being told it is
    // stale (F3).
    return key_fence;
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

registry::fence_verdict registry::check_fence(const key& id, u64 fence,
                                              bool creating) const noexcept {
    if (fence == 0) {
        if (config_.strict_fencing) {
            if (creating) {
                // A surviving tombstone watermark means the key already had an
                // incarnation epoch; purge_expired_tombstones() removes the
                // visible row but keeps the fence history. A zero fence would
                // reset the epoch, so a purged key is not fresh (F4).
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
        if (accepted != last_fence_.end() && accepted->second > high_water) {
            high_water = accepted->second;
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
    if (accepted != last_fence_.end() && fence < accepted->second) {
        return fence_verdict::stale;
    }
    return fence_verdict::ok;
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
    const fence_verdict verdict = check_fence(id, fence, true);
    if (verdict == fence_verdict::out_of_range) {
        return reject(rpc_code::failed_precondition,
                      "fencing token is out of range",
                      reason_fence_out_of_range);
    }
    if (verdict == fence_verdict::stale) {
        return reject_stale_fence_locked(id);
    }
    u64& key_fence = last_fence_[id];
    if (fence > key_fence) {
        key_fence = fence;
    }
    fenced_keys_[id] = true;
    return key_fence;
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
        error.metadata.set("registry.fence", std::to_string(accepted->second));
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
registry::snapshot_with(const service_instance& instance) const {
    auto next = std::make_shared<registry_snapshot>();
    next->index = index_ + 1;
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

        const fence_verdict verdict =
            check_fence(id, context.fence, existing == instances_.end());
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

        if (idempotent) {
            // Replay of an accepted write: the index and the event stream do
            // not move, and the recorded result is returned unchanged.
            const auto recorded = last_fence_.find(id);
            outcome.index = existing->second.revision;
            outcome.fence = recorded != last_fence_.end()
                                ? recorded->second
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

        const u64 next_index = index_ + 1;
        const auto accepted_fence = last_fence_.find(id);
        const u64 key_high_water =
            accepted_fence != last_fence_.end() ? accepted_fence->second : 0;
        u64 next_fence = 0;
        if (!allocate_fence(context.fence, next_index, key_high_water,
                            next_fence)) {
            return reject(rpc_code::resource_exhausted, "fence exhausted",
                          reason_overflow);
        }
        const u64 returned =
            record_fence_locked(id, context.fence, next_fence);
        instance.revision = next_index;

        auto next = snapshot_with(instance);
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
        const fence_verdict verdict = check_fence(id, context.fence, false);
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

        if (stored.state == instance_state::deregistered) {
            const auto fence_it = last_fence_.find(id);
            outcome.index = index_;
            outcome.fence = fence_it == last_fence_.end() ? index_ : fence_it->second;
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
        const u64 next_index = index_ + 1;
        const auto accepted_fence = last_fence_.find(id);
        const u64 key_high_water =
            accepted_fence != last_fence_.end() ? accepted_fence->second : 0;
        u64 next_fence = 0;
        if (!allocate_fence(context.fence, next_index, key_high_water,
                            next_fence)) {
            return reject(rpc_code::resource_exhausted, "fence exhausted",
                          reason_overflow);
        }
        const u64 returned =
            record_fence_locked(id, context.fence, next_fence);
        updated.revision = next_index;

        // Keep the tombstone incarnation high-water even after a later purge
        // physically removes this row.
        u64& watermark = tombstone_watermark_[id];
        if (updated.incarnation > watermark) {
            watermark = updated.incarnation;
        }

        auto next = snapshot_with(updated);
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
        const fence_verdict verdict = check_fence(id, context.fence, false);
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
        const u64 next_index = index_ + 1;
        const auto accepted_fence = last_fence_.find(id);
        const u64 key_high_water =
            accepted_fence != last_fence_.end() ? accepted_fence->second : 0;
        u64 next_fence = 0;
        if (!allocate_fence(context.fence, next_index, key_high_water,
                            next_fence)) {
            return reject(rpc_code::resource_exhausted, "fence exhausted",
                          reason_overflow);
        }
        const u64 returned =
            record_fence_locked(id, context.fence, next_fence);
        updated.revision = next_index;

        auto next = snapshot_with(updated);
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

} // namespace cluster
} // namespace crpc
