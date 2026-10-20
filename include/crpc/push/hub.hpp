// CRPC -- server push hub: topics, subscriptions and publish routing.
//
// The hub is the single authority for one push shard. A "topic" maps to a list
// of subscribers, and a subscriber id is a logical identity that outlives any
// one connection: a reconnect with the same id resumes its subscriptions.
//
// Thread safety
// -------------
//   A `hub` is owned by one shard thread and is not internally synchronised;
//   every method must be called from that thread. Cross-shard publish never
//   touches another hub: a caller routes a topic to one shard with
//   `topic_shard()` / `owns_topic()` and calls `publish()` only on the owning
//   hub. That is what gives every topic a single writer and keeps ordering free
//   of cross-thread hazards. No method blocks on a subscriber.
//
// Allocation
// ----------
//   * `publish` allocates one immutable payload block per call (shared by all
//     subscribers) plus one queue node per delivery; the payload is copied once.
//   * `subscribe` / `attach` / `unsubscribe` allocate map and string storage.
//   * `try_take` / `poll_event` allocate nothing.
//   * A failed `publish` allocation is counted (`dropped_allocation`) and the
//     call returns; the publisher is never stalled or terminated.
//
// Bounds
// ------
//   * Per subscription: `max_messages` (default 1024) and `max_bytes`
//     (default 1 MiB). Overflow follows `on_overflow`: drop_oldest (default),
//     drop_newest, or disconnect_subscriber. Drops are counted and never block
//     the publisher (R18).
//   * Reliable replay buffer: `max_replay` (default 256 entries) per
//     subscription; retransmission is capped by `retransmit_budget`.
//   * Event ring: `hub_options::max_events` (default 256); the oldest event is
//     evicted first and `events_dropped` is incremented.
//
// Delivery model
// --------------
//   Messages are queued per subscription and pulled by the owning connection
//   with `try_take`. A reliable message is assigned `(publisher_id, epoch, seq)`
//   per subscription; it stays unacknowledged until `ack`. Reconnect replays
//   unacknowledged messages (at-least-once; the consumer dedups by `seq`). An
//   ordered subscription consumes the per-topic monotonic `topic_seq`; a gap or
//   a duplicate resets/ drops with an observable event (see `hub_event_kind`).
#ifndef CRPC_PUSH_HUB_HPP
#define CRPC_PUSH_HUB_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc::push {

using byte_span = span<const std::byte>;
using subscriber_id = u64;

// Best-effort or acknowledged delivery. Reliability is a property of the
// (subscription, message) pair: a message is reliable only when both the
// subscription and the publish are reliable.
enum class delivery_mode : u8 {
    reliable = 0,
    unreliable = 1
};

// Replay policy used when a connection token is (re)attached.
//   * `tail` -- replay unacknowledged reliable messages at the queue tail
//     (the SSE reconnect behaviour);
//   * `none` -- retarget queued messages only; the caller decides what, if
//     anything, to replay (long polling);
//   * `front` -- replay unacknowledged reliable messages at the queue front, in
//     sequence order (ordered long polling).
enum class attach_replay : u8 {
    none = 0,
    tail = 1,
    front = 2
};

// Bounded-queue overflow behaviour. `disconnect_subscriber` removes every
// subscription of the offending subscriber and emits an event.
enum class overflow_policy : u8 {
    drop_oldest = 0,
    drop_newest = 1,
    disconnect_subscriber = 2
};

// Observable hub events. Every drop or forced disconnect is reported here so a
// stalled or misbehaving subscriber is never silently starved (R18/P7).
enum class hub_event_kind : u8 {
    overflow = 0,           // a queued message was dropped for queue bounds
    slow_subscriber = 1,    // queue stayed above the watermark past the timeout
    disconnected = 2,       // subscriber removed (overflow/duplicate connection)
    ordered_gap = 3,        // ordered stream gap or stale sequence observed
    not_acknowledged = 4    // reliable retransmission budget exhausted
};

struct hub_event {
    hub_event_kind kind = hub_event_kind::overflow;
    subscriber_id subscriber = 0;
    std::string topic;
    rpc_code code = rpc_code::ok;
    u64 seq = 0;
    u64 detail = 0;
};

// Immutable, shared by every subscriber of one publish.
struct push_payload {
    std::vector<std::byte> bytes;
};
using push_payload_ptr = std::shared_ptr<const push_payload>;

// A message handed to a subscriber. It shares the payload block with the other
// subscribers of the same publish, so no per-subscriber copy is made.
struct push_message {
    std::string topic;
    u64 publisher_id = 0;
    u64 epoch = 0;
    u64 seq = 0;         // per-subscriber reliable sequence; 0 when unreliable
    u64 topic_seq = 0;   // per-topic monotonic sequence
    delivery_mode mode = delivery_mode::unreliable;
    push_payload_ptr payload;

    [[nodiscard]] byte_span data() const noexcept {
        if (!payload) {
            return byte_span{};
        }
        return byte_span(payload->bytes.data(), payload->bytes.size());
    }
};

// Per-publish overrides. Zero ids fall back to the hub's configured
// `publisher_id` / `epoch`. `has_topic_seq` exists so tests and replicated
// publishers can inject an explicit sequence for gap detection.
struct publish_options {
    u64 publisher_id = 0;
    u64 epoch = 0;
    bool has_topic_seq = false;
    u64 topic_seq = 0;
};

// Per-subscription bounds and policy. Values are exact; the defaults are the
// documented v1 defaults.
struct subscribe_options {
    std::size_t max_messages = 1024;
    std::size_t max_bytes = 1u << 20; // 1 MiB
    overflow_policy on_overflow = overflow_policy::drop_oldest;
    std::size_t retransmit_budget = 3;
    std::size_t max_replay = 256;
};

struct hub_options {
    u32 shard_count = 1;
    u32 shard_index = 0;
    u64 publisher_id = 0;
    u64 epoch = 1;
    // A subscription whose queued bytes stay above this watermark for longer
    // than `slow_timeout` is disconnected. Zero disables the check.
    std::size_t slow_watermark_bytes = 512u << 10; // 512 KiB
    std::chrono::nanoseconds slow_timeout{std::chrono::seconds(5)};
    std::size_t max_events = 256;
};

struct hub_stats {
    u64 published = 0;
    u64 delivered = 0;
    u64 dropped_overflow = 0;
    u64 dropped_unreliable = 0;
    u64 dropped_disconnected = 0;
    u64 dropped_allocation = 0;
    u64 duplicate_acks = 0;
    u64 retransmits = 0;
    u64 not_acknowledged = 0;
    u64 slow_disconnects = 0;
    u64 ordered_gaps = 0;
    u64 ordered_duplicates = 0;
    u64 events_dropped = 0;
    u64 active_subscriptions = 0;
    u64 active_queues = 0;
    u64 queued_messages = 0;
    u64 queued_bytes = 0;
    u64 active_connections = 0;
};

// Deterministic topic -> shard mapping. With a declared shard key the key is
// mixed into the topic hash so the caller controls co-location. `shard_count`
// is clamped to at least 1.
[[nodiscard]] CRPC_API u32 topic_shard(std::string_view topic, u64 shard_key,
                                       u32 shard_count) noexcept;

class hub {
public:
    using connection_id = u64;
    static constexpr connection_id invalid_connection = 0;

    explicit hub(hub_options options = {});
    ~hub();

    hub(const hub&) = delete;
    hub& operator=(const hub&) = delete;
    hub(hub&&) = delete;
    hub& operator=(hub&&) = delete;

    // True when this shard owns `topic`. A topic declared with `declare_topic`
    // is routed by its shard key; otherwise the topic name is hashed.
    [[nodiscard]] bool owns_topic(std::string_view topic) const noexcept;

    // Pins `topic` to a shard key. Must be called before subscribing.
    void declare_topic(std::string_view topic, u64 shard_key) noexcept;

    // Subscribes `id` to `topic`. Re-subscribing the same (id, topic) fails
    // with `failed_precondition`. A topic not owned by this shard fails the
    // same way.
    [[nodiscard]] status subscribe(std::string_view topic, subscriber_id id,
                                   delivery_mode mode, bool ordered,
                                   subscribe_options options = {});
    [[nodiscard]] status unsubscribe(std::string_view topic,
                                     subscriber_id id) noexcept;
    [[nodiscard]] status unsubscribe_all(subscriber_id id) noexcept;

    // (Re)attaches a live connection for `id` and returns its token. The
    // previous token (if any) becomes stale immediately; queued messages are
    // retargeted to the new token so nothing is delivered twice. If no
    // connection was attached, unacknowledged reliable messages are replayed
    // according to `mode` (the default `tail` is the SSE behaviour).
    [[nodiscard]] connection_id attach(subscriber_id id) noexcept;
    [[nodiscard]] connection_id attach(subscriber_id id,
                                       attach_replay mode) noexcept;

    // Closes a connection token. Queued messages targeted at it are released
    // at once; the subscriptions remain so a later `attach` resumes them.
    [[nodiscard]] status detach(connection_id connection) noexcept;

    // Closes a connection token without touching its queues: queued messages
    // stay `in_queue`, no replay entry is requeued and no retransmission is
    // counted. A later `attach` retargets the queue to the new token. This is
    // the session-end path for long polling; `detach` keeps the historical
    // clear-queue semantics.
    [[nodiscard]] status release(connection_id connection) noexcept;

    // Publishes to every subscriber of `topic`. Never blocks: on a full queue
    // the configured overflow policy applies and the drop is counted.
    void publish(std::string_view topic, byte_span payload,
                 delivery_mode mode) noexcept;
    void publish(std::string_view topic, byte_span payload, delivery_mode mode,
                 const publish_options& options) noexcept;

    // Pops the next queued message for `connection` (optionally for one topic).
    // Returns false when the connection is stale or its queues are empty.
    [[nodiscard]] bool try_take(connection_id connection,
                                push_message& out) noexcept;
    [[nodiscard]] bool try_take(connection_id connection,
                                std::string_view topic,
                                push_message& out) noexcept;

    // Acknowledges a reliable message (cumulative: everything up to `seq`).
    // Idempotent; a duplicate ack is counted.
    [[nodiscard]] status ack(subscriber_id id, std::string_view topic,
                             u64 seq) noexcept;

    // Re-queues unacknowledged reliable messages. Exceeding the retransmission
    // budget disconnects the subscription and reports `not_acknowledged`.
    [[nodiscard]] status retransmit(subscriber_id id) noexcept;
    [[nodiscard]] status retransmit(subscriber_id id,
                                    std::string_view topic) noexcept;

    // Re-queues the unacknowledged reliable messages of `topic` at the FRONT of
    // the queue, in sequence order. Only messages already transmitted are
    // counted against `retransmit_budget`; messages that never left the queue
    // are not a retransmission. A budget overrun disconnects the subscriber
    // (`not_acknowledged`) and returns that code. A replay window that no
    // longer contains the first unacknowledged sequence returns
    // `resource_exhausted`: the caller must restart the subscription instead of
    // silently skipping a message.
    [[nodiscard]] status retransmit_front(subscriber_id id,
                                          std::string_view topic) noexcept;

    // Advances the hub clock for slow-subscriber detection. The shard timer
    // calls this with a monotonic timestamp; time is never read internally.
    void tick(u64 now_ns) noexcept;

    // Pops one observable event.
    [[nodiscard]] bool poll_event(hub_event& out) noexcept;

    // Running counters plus the live size of every queue.
    [[nodiscard]] hub_stats stats() const noexcept;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace crpc::push

#endif // CRPC_PUSH_HUB_HPP
