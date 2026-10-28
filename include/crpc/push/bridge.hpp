// CRPC -- cross-instance push bridge over an injected message bus.
//
// `push::hub` is a single-process structure: a publish reaches only the
// subscribers of the hub instance that received it. Behind a load balancer,
// a client connected to instance 1 never sees a message published on
// instance 2. The bridge closes that gap without coupling the hub to any
// transport: it fans local publishes out to a bus and injects bus messages
// back into the local hub.
//
// The bus is dependency injection (`hub_bus`), not a hard-wired Redis client:
// tests supply a fake in-process bus, production supplies an adapter over
// Redis Pub/Sub, AMQP, or any other broadcast channel.
//
// Wire contract
// -------------
//   Every bus message carries the publishing instance's id and a
//   per-instance monotonic `message_id`. A bus that echoes the publisher's
//   own messages back (Redis Pub/Sub does) is handled by the instance-id
//   filter; a bus that redelivers is handled by the bounded
//   `(source, message_id)` deduplication window. Both filters are the
//   bridge's job, because only the bridge knows its own identity.
//
// Failure semantics
// -----------------
//   * Local delivery always happens first. A bus failure is counted
//     (`publish_failures`) and never blocks or undoes the local publish.
//   * A disconnected bus is reconnected by `pump()`, which then SUBSCRIBEs
//     again: the subscription state lives on the bus connection and is lost
//     with it. Resubscription is driven by the bus `generation()` counter,
//     so a bridge also resubscribes when another actor reconnected the bus
//     behind its back.
//
// Thread safety
// -------------
//   `publish()` and `pump()` must be called on the thread that owns the
//   target hub (the shard thread), exactly like `hub::publish` itself. The
//   only cross-thread surface is the inbound handoff: the bus invokes the
//   delivery handler on its own thread, which appends to a mutex-guarded
//   queue; the hub is touched exclusively from `pump()` on the shard thread.
#ifndef CRPC_PUSH_BRIDGE_HPP
#define CRPC_PUSH_BRIDGE_HPP

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/push/hub.hpp>
#include <crpc/span.hpp>

namespace crpc::push {

// One message on the bus. `source` and `message_id` together identify a
// publish across the deployment: `source` filters the instance's own echo,
// `(source, message_id)` suppresses bus redelivery. `mode` is forwarded so a
// reliable publish stays reliable for the receiving subscribers; the
// receiving hub assigns its own per-topic and per-subscription sequences,
// so the source hub's counters are deliberately not transported.
struct bridge_message {
    std::string source;
    std::string topic;
    std::vector<std::byte> payload;
    u64 message_id = 0; // per-source monotonic; 0 is invalid on the bus
    delivery_mode mode = delivery_mode::unreliable;
};

// The delivery handler a bus invokes for every message it receives. The
// handler is cheap and never touches a hub: it only enqueues.
using message_handler = std::function<void(const bridge_message&)>;

// The broadcast channel between hub instances. Implemented by the deployer
// (Redis Pub/Sub, AMQP, ...) and faked in tests. All methods return rather
// than block for long: a bridge must never stall its shard thread on I/O.
//
// Redis semantics the implementations should mirror:
//   * `publish` reaches every subscriber, including the publishing instance
//     itself (the echo is what the bridge's loop filter consumes);
//   * the SUBSCRIBE state lives on the connection -- `reconnect()` restores
//     the connection but the caller must `subscribe()` again, which the
//     bridge does as soon as `generation()` changes.
class hub_bus {
public:
    virtual ~hub_bus() = default;

    // True while the bus connection is usable. A false value makes the next
    // `pump()` attempt `reconnect()`.
    [[nodiscard]] virtual bool connected() const noexcept = 0;

    // Bumped every time a new bus connection is established. The bridge
    // resubscribes whenever this differs from the generation it last
    // subscribed at, which covers both a self-reconnect and a reconnect
    // performed by another actor sharing the bus.
    [[nodiscard]] virtual u64 generation() const noexcept = 0;

    // Publishes one message to every subscriber of the bus. An error return
    // (bus down, send failed) is counted by the bridge; local delivery is
    // unaffected.
    [[nodiscard]] virtual status publish(const bridge_message& message) = 0;

    // Starts delivery of received messages into `handler`. Called once when
    // the bridge is created and again after every reconnect (the previous
    // subscription died with the old connection).
    [[nodiscard]] virtual status subscribe(message_handler handler) = 0;

    // Re-establishes the connection after `connected()` returned false.
    // Must bump `generation()` on success.
    [[nodiscard]] virtual status reconnect() = 0;
};

struct bridge_options {
    // Bounded window of recent `(source, message_id)` pairs used to suppress
    // bus redelivery. Zero is clamped to one. A duplicate older than the
    // window is accepted; the window bounds memory, not history.
    std::size_t dedup_window = 1024;

    // Payloads above this size are not sent to or accepted from the bus;
    // local delivery is unaffected. Zero disables the limit.
    std::size_t max_payload_bytes = 1u << 20; // 1 MiB

    // Inbound handoff queue bound. Messages beyond it are dropped and
    // counted (`inbound_dropped_full`) instead of growing without bound.
    // Zero is clamped to one.
    std::size_t max_pending = 4096;
};

struct bridge_stats {
    u64 outbound = 0;                    // messages accepted by the bus
    u64 outbound_rejected = 0;           // empty topic
    u64 outbound_dropped_oversize = 0;   // payload above max_payload_bytes
    u64 outbound_dropped_allocation = 0; // message construction failed
    u64 publish_failures = 0;            // the bus refused a message
    u64 inbound = 0;                     // messages accepted from the bus
    u64 inbound_rejected = 0;            // missing source/id/topic
    u64 inbound_dropped_oversize = 0;
    u64 inbound_dropped_allocation = 0;
    u64 inbound_dropped_full = 0;        // handoff queue full
    u64 loops_filtered = 0;              // own echoes dropped
    u64 duplicates_dropped = 0;          // repeated (source, message_id)
    u64 injected = 0;                    // messages handed to the local hub
    u64 reconnects = 0;                  // successful reconnects
    u64 subscribes = 0;                  // successful subscriptions
};

// Cross-instance bridge for one hub. Created through `make`; `publish`
// replaces the plain `hub::publish` call site for topics that must fan out
// across instances.
class CRPC_API hub_bridge {
public:
    // `instance_id` must be non-empty: it is the loop-suppression key, and
    // instances sharing an id cannot tell their own echoes from each other.
    // The bridge subscribes to the bus immediately when it is connected; a
    // bus that is down at creation time is handled by `pump()`.
    static result<std::unique_ptr<hub_bridge>> make(
        hub& target, hub_bus& bus, std::string_view instance_id,
        const bridge_options& options = {});

    ~hub_bridge();

    hub_bridge(const hub_bridge&) = delete;
    hub_bridge& operator=(const hub_bridge&) = delete;
    hub_bridge(hub_bridge&&) = delete;
    hub_bridge& operator=(hub_bridge&&) = delete;

    // Publishes to the local hub first, then fans the message out to the
    // bus. Local delivery is never blocked, rolled back or reported as
    // failed by a bus error: the failure is counted instead. Shard thread.
    void publish(std::string_view topic, byte_span payload,
                 delivery_mode mode) noexcept;

    // Drives the bus lifecycle and the inbound injection: reconnects when
    // the bus is down, resubscribes when the bus generation changed, then
    // drains the inbound queue into the hub. Shard thread; call it from the
    // shard timer or pump loop.
    void pump() noexcept;

    [[nodiscard]] const std::string& instance_id() const noexcept;

    // Bus connection state, mirrored from the bus for observability.
    [[nodiscard]] bool connected() const noexcept;

    // True once the bus accepted this bridge's subscription and the
    // subscription's generation is current.
    [[nodiscard]] bool subscribed() const noexcept;

    [[nodiscard]] bridge_stats stats() const noexcept;

private:
    struct state;

    hub_bridge() noexcept = default;

    std::shared_ptr<state> state_;
};

} // namespace crpc::push

#endif // CRPC_PUSH_BRIDGE_HPP
