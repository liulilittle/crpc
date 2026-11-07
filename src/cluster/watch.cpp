// CRPC -- index-based watch/subscription implementation.
#include <crpc/cluster/watch.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace crpc {
namespace cluster {
namespace {

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

[[nodiscard]] std::vector<service_instance>
filter_snapshot(const registry_snapshot& snap, const std::string& service) {
    std::vector<service_instance> out;
    out.reserve(snap.instances.size());
    for (const auto& instance : snap.instances) {
        if (!service.empty() && instance.service != service) {
            continue;
        }
        if (instance.state == instance_state::deregistered) {
            continue;
        }
        out.push_back(instance);
    }
    std::sort(out.begin(), out.end(), instance_less);
    return out;
}

struct dispatch_guard {
    std::atomic<bool>& flag;
    ~dispatch_guard() { flag.store(false, std::memory_order_release); }
};

} // namespace

watch_stream::watch_stream(std::string service, u64 snapshot_index,
                           std::vector<service_instance> snapshot,
                           watch_callback callback, void* context,
                           std::size_t max_queue, watch_event_kind initial_kind)
    : service_(std::move(service)),
      callback_(callback),
      context_(context),
      max_queue_(max_queue),
      initial_kind_(initial_kind),
      snapshot_(std::make_shared<const std::vector<service_instance>>(
          std::move(snapshot))),
      snapshot_index_(snapshot_index),
      last_seen_(snapshot_index) {}

bool watch_stream::invoke_callback(const watch_event& event) {
    if (callback_ == nullptr) {
        return false;
    }
    // Cancellation barrier. callback_mutex_ serialises callbacks; taking it
    // before the closed_ check closes the race between "cancel returned" and
    // "callback starts". cancel() takes the same mutex, except when it is
    // called from the callback itself (then the running callback is the last
    // one).
    std::lock_guard<std::mutex> callback_lock(callback_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return false;
        }
        in_callback_ = true;
        callback_thread_ = std::this_thread::get_id();
    }
    callback_(context_, event);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        in_callback_ = false;
        callback_thread_ = std::thread::id{};
    }
    return true;
}

std::size_t watch_stream::deliver_initial() {
    std::shared_ptr<const std::vector<service_instance>> snapshot;
    u64 index = 0;
    watch_event_kind kind = watch_event_kind::snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (initial_delivered_ || closed_) {
            return 0;
        }
        initial_delivered_ = true;
        snapshot = snapshot_;
        index = snapshot_index_;
        kind = initial_kind_;
    }
    if (callback_ == nullptr) {
        return 0;
    }

    watch_event event;
    event.index = index;
    event.kind = kind;
    event.service = service_;
    event.snapshot = snapshot.get();
    return invoke_callback(event) ? 1 : 0;
}

void watch_stream::park() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    initial_delivered_ = true;
}

void watch_stream::cancel() noexcept {
    bool self = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        queue_.clear();
        self = in_callback_ &&
               callback_thread_ == std::this_thread::get_id();
    }
    if (!self) {
        // Wait for an in-flight callback (if any) to finish. After this point
        // the closed_ flag stops every later callback.
        std::lock_guard<std::mutex> callback_lock(callback_mutex_);
    }
}

void watch_stream::on_event(u64 index, const std::string& service,
                            const service_instance& instance) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_ || dropped_) {
        return;
    }
    if (index <= last_seen_) {
        // Duplicate or old index: the snapshot already covers it.
        return;
    }
    if (needs_resync_) {
        // Keep the anchor moving while waiting for the snapshot.
        last_seen_ = index;
        return;
    }
    if (last_seen_ != std::numeric_limits<u64>::max() && index == last_seen_ + 1) {
        last_seen_ = index;
        if (service != service_) {
            return;
        }
        if (queue_.size() >= max_queue_) {
            dropped_ = true;
            queue_.clear();
            return;
        }
        queued_event queued;
        queued.index = index;
        queued.instance = instance;
        queue_.push_back(std::move(queued));
        return;
    }

    // A gap means this subscription missed at least one index.
    needs_resync_ = true;
    queue_.clear();
    last_seen_ = index;
}

bool watch_stream::needs_resync() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return needs_resync_;
}

bool watch_stream::dropped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

bool watch_stream::closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

u64 watch_stream::last_index() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_seen_;
}

u64 watch_stream::snapshot_index() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_index_;
}

std::size_t watch_stream::queued() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

std::size_t watch_stream::apply_resync(u64 index,
                                       std::vector<service_instance> snapshot) {
    if (callback_ == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || dropped_ || index < last_seen_) {
            return 0;
        }
        snapshot_ = std::make_shared<const std::vector<service_instance>>(
            std::move(snapshot));
        snapshot_index_ = index;
        last_seen_ = index;
        needs_resync_ = false;
        initial_delivered_ = true;
        queue_.clear();
        return 0;
    }

    auto next = std::make_shared<const std::vector<service_instance>>(
        std::move(snapshot));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // W4: a snapshot older than the applied anchor must never roll the
        // stream back or discard already queued newer events.
        if (closed_ || dropped_ || index < last_seen_) {
            return 0;
        }
        snapshot_ = next;
        snapshot_index_ = index;
        last_seen_ = index;
        needs_resync_ = false;
        initial_delivered_ = true;
        queue_.clear();
    }

    watch_event event;
    event.index = index;
    event.kind = watch_event_kind::resync;
    event.service = service_;
    event.snapshot = next.get();
    return invoke_callback(event) ? 1 : 0;
}

std::size_t watch_stream::dispatch() {
    std::size_t delivered = deliver_initial();

    std::deque<queued_event> local;
    bool emit_dropped = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return delivered;
        }
        if (dropped_) {
            emit_dropped = true;
            queue_.clear();
        } else {
            local.swap(queue_);
        }
    }

    if (emit_dropped) {
        watch_event event;
        event.index = last_index();
        event.kind = watch_event_kind::dropped;
        event.service = service_;
        // The dropped event is the last callback, so closed_ is set after it
        // (setting it before would suppress it through the cancel barrier).
        if (invoke_callback(event)) {
            ++delivered;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        return delivered;
    }

    for (const auto& queued : local) {
        watch_event event;
        event.index = queued.index;
        event.kind = watch_event_kind::delta;
        event.service = service_;
        event.instance = &queued.instance;
        // W1: cancel() from this callback (or another thread) stops the batch
        // before the next callback starts.
        if (!invoke_callback(event)) {
            break;
        }
        ++delivered;
    }
    return delivered;
}

watch_hub::watch_hub(registry& catalog, std::size_t max_subscribers)
    : watch_hub(catalog, max_subscribers, 256) {}

watch_hub::watch_hub(registry& catalog, std::size_t max_subscribers,
                     std::size_t max_replay)
    : catalog_(catalog),
      max_subscribers_(max_subscribers),
      max_replay_(max_replay) {
    const result<u64> added = catalog_.add_sink(&watch_hub::sink_bridge, this);
    if (added.ok()) {
        sink_id_ = added.value();
        sink_registered_ = true;
    }
}

watch_hub::~watch_hub() {
    if (sink_registered_) {
        const status removed = catalog_.remove_sink(sink_id_);
        (void)removed;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    subscribers_.clear();
}

void watch_hub::sink_bridge(void* context, const catalog_event& event) noexcept {
    static_cast<watch_hub*>(context)->on_catalog_event(event);
}

void watch_hub::on_catalog_event(const catalog_event& event) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : subscribers_) {
        entry.second.stream->on_event(event.index, event.instance.service,
                                      event.instance);
    }
    // Bounded replay ring: a reconnect at from_index < current replays the
    // missed deltas when this still covers them; otherwise it falls back to a
    // resync snapshot.
    if (max_replay_ != 0) {
        replay_.push_back(event);
        while (replay_.size() > max_replay_) {
            replay_.pop_front();
        }
    }
}

result<u64> watch_hub::watch(const std::string& service, u64 from_index,
                             watch_callback callback, void* context,
                             std::size_t max_queue) {
    if (callback == nullptr) {
        return rpc_error(rpc_code::invalid_argument, "watch callback is null");
    }

    u64 id = 0;
    std::shared_ptr<watch_stream> created;
    bool park = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!sink_registered_) {
            return rpc_error(rpc_code::resource_exhausted,
                             "watch sink is not registered");
        }
        if (max_subscribers_ != 0 && subscribers_.size() >= max_subscribers_) {
            return rpc_error(rpc_code::resource_exhausted,
                             "watch subscriber limit reached");
        }

        const auto current = catalog_.snapshot();
        if (current == nullptr) {
            return rpc_error(rpc_code::internal, "registry snapshot is unavailable");
        }
        if (from_index > current->index) {
            return rpc_error(rpc_code::invalid_argument,
                             "from_index is ahead of the registry index");
        }

        std::vector<service_instance> initial;
        std::vector<catalog_event> replay;
        watch_event_kind initial_kind = watch_event_kind::snapshot;
        u64 anchor = current->index;

        if (from_index != 0 && from_index < current->index) {
            // Collect the ring entries that cover (from_index, current]. The
            // ring may be unordered under concurrent writers, so sort a copy
            // and require one contiguous event per index.
            for (const auto& event : replay_) {
                if (event.index > from_index && event.index <= current->index) {
                    replay.push_back(event);
                }
            }
            std::sort(replay.begin(), replay.end(),
                      [](const catalog_event& a, const catalog_event& b) {
                          return a.index < b.index;
                      });
            bool contiguous = !replay.empty();
            u64 expected = from_index + 1;
            for (const auto& event : replay) {
                if (event.index != expected) {
                    contiguous = false;
                    break;
                }
                if (expected != std::numeric_limits<u64>::max()) {
                    ++expected;
                }
            }
            if (contiguous) {
                contiguous = replay.back().index == current->index;
            }
            if (contiguous) {
                // Incremental replay: the queued deltas are the initial
                // delivery, so no snapshot is sent.
                initial_kind = watch_event_kind::snapshot;
                anchor = from_index;
                park = true;
            } else {
                replay.clear();
                initial = filter_snapshot(*current, service);
                initial_kind = watch_event_kind::resync;
            }
        } else if (from_index == 0) {
            initial = filter_snapshot(*current, service);
        } else {
            // from_index == current: park until the next change. No immediate
            // snapshot is delivered.
            anchor = from_index;
            park = true;
        }

        id = next_id_++;
        if (id == 0) {
            id = next_id_++;
        }
        created = std::make_shared<watch_stream>(
            service, anchor, std::move(initial), callback, context, max_queue,
            initial_kind);
        if (park) {
            created->park();
            for (const auto& event : replay) {
                created->on_event(event.index, event.instance.service,
                                  event.instance);
            }
        }
        subscriber entry;
        entry.id = id;
        entry.service = service;
        entry.stream = created;
        subscribers_.emplace(id, std::move(entry));
    }

    (void)created->deliver_initial();
    return id;
}

status watch_hub::cancel(u64 subscription_id) {
    std::shared_ptr<watch_stream> stream;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = subscribers_.find(subscription_id);
        if (found == subscribers_.end()) {
            return status{};
        }
        stream = found->second.stream;
        subscribers_.erase(found);
    }
    // Delivery barrier: after cancel returns no callback starts on the stream.
    stream->cancel();
    return status{};
}

std::size_t watch_hub::dispatch() {
    bool expected = false;
    if (!dispatching_.compare_exchange_strong(expected, true,
                                              std::memory_order_acq_rel)) {
        return 0;
    }
    dispatch_guard guard{dispatching_};

    std::vector<subscriber> work;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        work.reserve(subscribers_.size());
        for (const auto& entry : subscribers_) {
            work.push_back(entry.second);
        }
    }

    std::size_t delivered = 0;
    std::vector<u64> remove;
    for (const auto& entry : work) {
        if (entry.stream->needs_resync()) {
            for (int attempt = 0; attempt < 8; ++attempt) {
                const auto current = catalog_.snapshot();
                if (current == nullptr) {
                    break;
                }
                const u64 applied = current->index;
                const std::size_t count = entry.stream->apply_resync(
                    applied, filter_snapshot(*current, entry.service));
                delivered += count;
                if (count == 0 || catalog_.index() <= applied) {
                    break;
                }
            }
        }
        delivered += entry.stream->deliver_initial();
        delivered += entry.stream->dispatch();
        if (entry.stream->closed()) {
            remove.push_back(entry.id);
        }
    }

    if (!remove.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const u64 id : remove) {
            subscribers_.erase(id);
        }
    }
    return delivered;
}

status watch_hub::resync(u64 subscription_id) {
    std::shared_ptr<watch_stream> stream;
    std::string service;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = subscribers_.find(subscription_id);
        if (found == subscribers_.end()) {
            return status{rpc_code::not_found};
        }
        stream = found->second.stream;
        service = found->second.service;
    }

    std::size_t delivered = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const auto current = catalog_.snapshot();
        if (current == nullptr) {
            return status{rpc_code::internal};
        }
        const u64 applied = current->index;
        const std::size_t count = stream->apply_resync(
            applied, filter_snapshot(*current, service));
        delivered += count;
        if (count == 0 || catalog_.index() <= applied) {
            break;
        }
    }
    return delivered > 0 ? status{} : status{rpc_code::failed_precondition};
}

std::size_t watch_hub::subscriber_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return subscribers_.size();
}

} // namespace cluster
} // namespace crpc
