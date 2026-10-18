// CRPC -- index-based watch/subscription.
//
// A watch starts with a full snapshot at the registry's current index and then
// receives deltas. The monotonic registry index is the only ordering anchor:
//
//   * a delta with index <= the snapshot index is dropped;
//   * the next expected delta has index == last_index + 1;
//   * a gap marks the subscription for resync (the ErrCompacted equivalent);
//   * deltas are delivered with strictly increasing index.
//
// The watch hub receives catalog events from a registry sink. The sink only
// appends to bounded per-subscriber queues; it never invokes a user callback,
// so a slow subscriber cannot block the registry writer. When a queue is full
// the subscriber is marked dropped and receives one observable
// watch_event_kind::dropped event on the next dispatch, then it is removed.
//
// Concurrency contract
// --------------------
//   * watch_hub is thread-safe for watch(), cancel(), resync() and the
//     registry sink. dispatch() is serialised internally; at most one dispatch
//     runs at a time.
//   * User callbacks run on the dispatch() thread (or the watch() thread for
//     the initial snapshot) and outside all registry locks. A callback must not
//     block; it may call cancel().
//   * watch_stream is thread-safe for on_event()/dispatch()/apply_resync();
//     callbacks are serialised by an internal callback mutex. A callback must
//     not re-enter deliver_initial(), apply_resync() or dispatch() on the same
//     stream.
//   * The registry must outlive the watch_hub. The watch_hub destructor must not
//     run concurrently with dispatch().
#ifndef CRPC_CLUSTER_WATCH_HPP
#define CRPC_CLUSTER_WATCH_HPP

#include <atomic>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <crpc/cluster/registry.hpp>

namespace crpc {
namespace cluster {

enum class watch_event_kind : u8 {
    snapshot = 0,
    delta = 1,
    resync = 2,
    dropped = 3
};

struct watch_event {
    u64 index = 0;
    watch_event_kind kind = watch_event_kind::snapshot;
    std::string service;
    // Set for delta events; valid only for the duration of the callback.
    const service_instance* instance = nullptr;
    // Set for snapshot/resync events; valid only for the duration of the
    // callback.
    const std::vector<service_instance>* snapshot = nullptr;
};

using watch_callback = void (*)(void* context, const watch_event& event) noexcept;

#if defined(CRPC_COMPILER_MSVC)
// Private STL members are an implementation detail; the exported ABI is the
// method set. C4251 would otherwise reject every class with a std:: member in
// a shared build.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
class CRPC_API watch_stream {
public:
    watch_stream(std::string service, u64 snapshot_index,
                 std::vector<service_instance> snapshot, watch_callback callback,
                 void* context, std::size_t max_queue = 64,
                 watch_event_kind initial_kind = watch_event_kind::snapshot);

    watch_stream(const watch_stream&) = delete;
    watch_stream& operator=(const watch_stream&) = delete;

    // Delivers the initial snapshot exactly once. Returns 1 when this call
    // delivered it, 0 otherwise.
    [[nodiscard]] std::size_t deliver_initial();

    // Feeds one catalog event. Events for other services still advance the
    // global index, so unrelated traffic does not create false gaps.
    void on_event(u64 index, const std::string& service,
                  const service_instance& instance) noexcept;

    [[nodiscard]] bool needs_resync() const;
    [[nodiscard]] bool dropped() const;
    [[nodiscard]] bool closed() const;
    [[nodiscard]] u64 last_index() const;
    [[nodiscard]] u64 snapshot_index() const;
    [[nodiscard]] std::size_t queued() const;
    [[nodiscard]] const std::string& service() const noexcept { return service_; }

    // Applies a fresh snapshot. Clears the resync flag and resets the index
    // anchor. Returns 1 when the resync callback was delivered.
    [[nodiscard]] std::size_t apply_resync(u64 index,
                                           std::vector<service_instance> snapshot);

    // Delivers queued deltas (or the dropped event). Returns the number of
    // callbacks delivered.
    [[nodiscard]] std::size_t dispatch();

private:
    struct queued_event {
        u64 index = 0;
        service_instance instance;
    };

    std::string service_;
    watch_callback callback_ = nullptr;
    void* context_ = nullptr;
    std::size_t max_queue_ = 64;
    watch_event_kind initial_kind_ = watch_event_kind::snapshot;

    mutable std::mutex mutex_;
    std::mutex callback_mutex_;
    std::shared_ptr<const std::vector<service_instance>> snapshot_;
    u64 snapshot_index_ = 0;
    u64 last_seen_ = 0;
    std::deque<queued_event> queue_;
    bool needs_resync_ = false;
    bool dropped_ = false;
    bool closed_ = false;
    bool initial_delivered_ = false;
};

class CRPC_API watch_hub {
public:
    explicit watch_hub(registry& catalog, std::size_t max_subscribers = 1024);
    ~watch_hub();

    watch_hub(const watch_hub&) = delete;
    watch_hub& operator=(const watch_hub&) = delete;
    watch_hub(watch_hub&&) = delete;
    watch_hub& operator=(watch_hub&&) = delete;

    // from_index == 0 starts a fresh watch. A smaller non-zero from_index means
    // the caller missed history, so the initial snapshot is tagged resync. A
    // future index is rejected with invalid_argument.
    [[nodiscard]] result<u64> watch(const std::string& service, u64 from_index,
                                    watch_callback callback, void* context,
                                    std::size_t max_queue = 64);

    [[nodiscard]] status cancel(u64 subscription_id);

    // Drives resync and delta delivery. Returns the number of callbacks.
    [[nodiscard]] std::size_t dispatch();

    // Forces a snapshot resync for one subscription.
    [[nodiscard]] status resync(u64 subscription_id);

    [[nodiscard]] std::size_t subscriber_count() const;

private:
    struct subscriber {
        u64 id = 0;
        std::string service;
        std::shared_ptr<watch_stream> stream;
    };

    static void sink_bridge(void* context, const catalog_event& event) noexcept;
    void on_catalog_event(const catalog_event& event) noexcept;

    registry& catalog_;
    std::size_t max_subscribers_ = 1024;
    mutable std::mutex mutex_;
    std::map<u64, subscriber> subscribers_;
    u64 next_id_ = 1;
    u64 sink_id_ = 0;
    bool sink_registered_ = false;
    std::atomic<bool> dispatching_{false};
};

#if defined(CRPC_COMPILER_MSVC)
#pragma warning(pop)
#endif
} // namespace cluster
} // namespace crpc

#endif // CRPC_CLUSTER_WATCH_HPP
