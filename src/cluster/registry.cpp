// CRPC -- service catalog implementation.
#include <crpc/cluster/registry.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace crpc {
namespace cluster {
namespace {

constexpr const char* reason_authority = "authority";
constexpr const char* reason_stale_incarnation = "stale_incarnation";
constexpr const char* reason_stale_fence = "stale_fence";
constexpr const char* reason_tombstone = "tombstone";
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

bool registry::allocate_fence(u64 requested, u64 next_index, u64& out) noexcept {
    if (last_global_fence_ == std::numeric_limits<u64>::max()) {
        return false;
    }
    u64 candidate = next_index;
    if (requested > candidate) {
        candidate = requested;
    }
    if (candidate <= last_global_fence_) {
        candidate = last_global_fence_ + 1;
    }
    last_global_fence_ = candidate;
    out = candidate;
    return true;
}

bool registry::check_fence(const key& id, u64 fence) const noexcept {
    if (fence == 0) {
        return true;
    }
    const auto it = last_fence_.find(id);
    if (it == last_fence_.end()) {
        return true;
    }
    return fence >= it->second;
}

rpc_error registry::reject(rpc_code code, const char* message, const char* reason) {
    rpc_error error(code, message);
    error.metadata.set("registry.reason", reason);
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
    next->instances.reserve(snapshot_->instances.size() + 1);
    bool replaced = false;
    for (const auto& current : snapshot_->instances) {
        if (current.service == instance.service && current.node_id == instance.node_id) {
            next->instances.push_back(instance);
            replaced = true;
        } else {
            next->instances.push_back(current);
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

    {
        std::lock_guard<std::mutex> lock(mutex_);

        const auto existing = instances_.find(id);
        if (existing == instances_.end()) {
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
            const service_instance& stored = existing->second;
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
            }
        }

        if (!check_fence(id, context.fence)) {
            return reject(rpc_code::failed_precondition, "stale fencing token",
                          reason_stale_fence);
        }
        if (index_ == std::numeric_limits<u64>::max()) {
            return reject(rpc_code::resource_exhausted,
                          "registry index exhausted", reason_overflow);
        }

        const u64 next_index = index_ + 1;
        u64 next_fence = 0;
        if (!allocate_fence(context.fence, next_index, next_fence)) {
            return reject(rpc_code::resource_exhausted, "fence exhausted",
                          reason_overflow);
        }
        instance.revision = next_index;

        auto next = snapshot_with(instance);
        if (existing == instances_.end()) {
            instances_.emplace(id, instance);
        } else {
            existing->second = instance;
        }
        last_fence_[id] = next_fence;
        index_ = next_index;
        std::shared_ptr<const registry_snapshot> published(std::move(next));
        std::atomic_store_explicit(&snapshot_, published, std::memory_order_release);

        event.index = next_index;
        event.kind = catalog_event_kind::put;
        event.instance = instance;
        outcome.index = next_index;
        outcome.fence = next_fence;
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
        if (!check_fence(id, context.fence)) {
            return reject(rpc_code::failed_precondition, "stale fencing token",
                          reason_stale_fence);
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
        u64 next_fence = 0;
        if (!allocate_fence(context.fence, next_index, next_fence)) {
            return reject(rpc_code::resource_exhausted, "fence exhausted",
                          reason_overflow);
        }
        updated.revision = next_index;

        auto next = snapshot_with(updated);
        existing->second = updated;
        last_fence_[id] = next_fence;
        index_ = next_index;
        std::shared_ptr<const registry_snapshot> published(std::move(next));
        std::atomic_store_explicit(&snapshot_, published, std::memory_order_release);

        event.index = next_index;
        event.kind = catalog_event_kind::put;
        event.instance = updated;
        outcome.index = next_index;
        outcome.fence = next_fence;
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
        if (!check_fence(id, context.fence)) {
            return reject(rpc_code::failed_precondition, "stale fencing token",
                          reason_stale_fence);
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
        u64 next_fence = 0;
        if (!allocate_fence(context.fence, next_index, next_fence)) {
            return reject(rpc_code::resource_exhausted, "fence exhausted",
                          reason_overflow);
        }
        updated.revision = next_index;

        auto next = snapshot_with(updated);
        existing->second = updated;
        last_fence_[id] = next_fence;
        index_ = next_index;
        std::shared_ptr<const registry_snapshot> published(std::move(next));
        std::atomic_store_explicit(&snapshot_, published, std::memory_order_release);

        event.index = next_index;
        event.kind = catalog_event_kind::put;
        event.instance = updated;
        outcome.index = next_index;
        outcome.fence = next_fence;
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
    return std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
}

u64 registry::index() const noexcept {
    const auto snap = snapshot();
    return snap != nullptr ? snap->index : 0;
}

u64 registry::fence() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_global_fence_;
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
    std::shared_ptr<registry_snapshot> next;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : instances_) {
            const service_instance& instance = entry.second;
            if (instance.state != instance_state::deregistered) {
                continue;
            }
            const u64 age = index_ >= instance.revision ? index_ - instance.revision : 0;
            if (age > config_.tombstone_grace) {
                expired.push_back(entry.first);
            }
        }
        if (expired.empty()) {
            return 0;
        }

        next = std::make_shared<registry_snapshot>();
        next->index = index_;
        next->instances.reserve(instances_.size() - expired.size());
        for (const auto& entry : instances_) {
            bool removed = false;
            for (const auto& id : expired) {
                if (id.service == entry.first.service && id.node_id == entry.first.node_id) {
                    removed = true;
                    break;
                }
            }
            if (!removed) {
                next->instances.push_back(entry.second);
            }
        }
        for (const auto& id : expired) {
            instances_.erase(id);
            last_fence_.erase(id);
        }
        std::shared_ptr<const registry_snapshot> published(std::move(next));
        std::atomic_store_explicit(&snapshot_, published, std::memory_order_release);
    }
    return expired.size();
}

} // namespace cluster
} // namespace crpc
