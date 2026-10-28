#include <crpc/push/bridge.hpp>

#include <atomic>
#include <cstddef>
#include <deque>
#include <mutex>
#include <new>
#include <set>
#include <utility>

namespace crpc::push {
namespace {

// Atomic mirror of `bridge_stats`: the outbound counters are written on the
// shard thread, the inbound counters on the bus thread, and `stats()` may be
// read from anywhere. Plain hub counters would be a data race.
struct bridge_counters {
    std::atomic<u64> outbound{0};
    std::atomic<u64> outbound_rejected{0};
    std::atomic<u64> outbound_dropped_oversize{0};
    std::atomic<u64> outbound_dropped_allocation{0};
    std::atomic<u64> publish_failures{0};
    std::atomic<u64> inbound{0};
    std::atomic<u64> inbound_rejected{0};
    std::atomic<u64> inbound_dropped_oversize{0};
    std::atomic<u64> inbound_dropped_allocation{0};
    std::atomic<u64> inbound_dropped_full{0};
    std::atomic<u64> loops_filtered{0};
    std::atomic<u64> duplicates_dropped{0};
    std::atomic<u64> injected{0};
    std::atomic<u64> reconnects{0};
    std::atomic<u64> subscribes{0};
};

} // namespace

struct hub_bridge::state {
    state(hub& target_ref, hub_bus& bus_ref) noexcept
        : target(target_ref), bus(bus_ref) {}

    hub& target;
    hub_bus& bus;
    std::string instance_id;
    bridge_options options;

    // The handler handed to the bus. It holds a weak reference so a delivery
    // racing the bridge destructor becomes a no-op instead of a use-after-free.
    message_handler handler;

    // Shard thread only.
    u64 next_message_id = 0;
    u64 subscribed_generation = 0;

    // Written by the shard thread, read by observability from any thread.
    std::atomic<bool> subscribed{false};

    bridge_counters counters;

    // Inbound handoff: the bus thread appends, the shard thread drains. The
    // deduplication window shares the mutex because the check-and-record must
    // be atomic with respect to concurrent deliveries.
    std::mutex mu;
    std::deque<bridge_message> pending;
    std::deque<std::pair<std::string, u64>> dedup_order;
    std::set<std::pair<std::string, u64>> dedup_keys;

    // Bus thread. Filters what never needs the hub (own echo, identity-less,
    // oversize, duplicate) and queues the rest for the shard pump.
    void on_bus_message(const bridge_message& message) noexcept {
        if (message.source == instance_id) {
            // Redis Pub/Sub echoes the publisher's own message back; without
            // this filter every local subscriber would receive it twice.
            ++counters.loops_filtered;
            return;
        }
        if (message.topic.empty() || message.source.empty() ||
            message.message_id == 0) {
            // A message without identity cannot be loop-checked or
            // deduplicated later; refuse it instead of guessing.
            ++counters.inbound_rejected;
            return;
        }
        if (options.max_payload_bytes != 0 &&
            message.payload.size() > options.max_payload_bytes) {
            ++counters.inbound_dropped_oversize;
            return;
        }
        try {
            std::lock_guard<std::mutex> guard(mu);
            const std::pair<std::string, u64> key{message.source,
                                                  message.message_id};
            if (dedup_keys.find(key) != dedup_keys.end()) {
                ++counters.duplicates_dropped;
                return;
            }
            if (pending.size() >= options.max_pending) {
                // Dropped before the dedup record exists, so a later
                // redelivery of the same message is still accepted.
                ++counters.inbound_dropped_full;
                return;
            }
            pending.push_back(message);
            dedup_order.push_back(key);
            dedup_keys.insert(key);
            while (dedup_order.size() > options.dedup_window) {
                dedup_keys.erase(dedup_order.front());
                dedup_order.pop_front();
            }
            ++counters.inbound;
        } catch (...) {
            ++counters.inbound_dropped_allocation;
        }
    }

    // Shard thread. Pops one message and hands it to the hub; the hub applies
    // its own shard-ownership and subscriber routing as with a local publish.
    bool inject_one() noexcept {
        bridge_message message;
        {
            std::lock_guard<std::mutex> guard(mu);
            if (pending.empty()) {
                return false;
            }
            message = std::move(pending.front());
            pending.pop_front();
        }
        target.publish(message.topic,
                       byte_span(message.payload.data(), message.payload.size()),
                       message.mode);
        ++counters.injected;
        return true;
    }
};

result<std::unique_ptr<hub_bridge>> hub_bridge::make(
    hub& target, hub_bus& bus, std::string_view instance_id,
    const bridge_options& options) {
    if (instance_id.empty()) {
        return result<std::unique_ptr<hub_bridge>>(
            rpc_error(rpc_code::invalid_argument,
                      "instance id must not be empty"));
    }

    std::unique_ptr<hub_bridge> bridge;
    try {
        bridge.reset(new hub_bridge());
        bridge->state_ = std::make_shared<state>(target, bus);
        bridge->state_->instance_id.assign(instance_id.data(),
                                           instance_id.size());
        bridge->state_->options = options;
        if (bridge->state_->options.dedup_window == 0) {
            bridge->state_->options.dedup_window = 1;
        }
        if (bridge->state_->options.max_pending == 0) {
            bridge->state_->options.max_pending = 1;
        }
        std::weak_ptr<state> weak = bridge->state_;
        bridge->state_->handler =
            message_handler([weak](const bridge_message& message) noexcept {
                if (const std::shared_ptr<state> alive = weak.lock()) {
                    alive->on_bus_message(message);
                }
            });
    } catch (...) {
        return result<std::unique_ptr<hub_bridge>>(rpc_code::resource_exhausted);
    }

    // Subscribe while the bus is up so delivery starts without a pump. A bus
    // that is down right now is handled by the first `pump()`.
    try {
        const status subscribed = bus.subscribe(bridge->state_->handler);
        if (subscribed.ok()) {
            bridge->state_->subscribed.store(true, std::memory_order_release);
            bridge->state_->subscribed_generation = bus.generation();
            ++bridge->state_->counters.subscribes;
        }
    } catch (...) {
        // Leave the bridge unsubscribed; the next pump retries.
    }
    return result<std::unique_ptr<hub_bridge>>(std::move(bridge));
}

hub_bridge::~hub_bridge() = default;

const std::string& hub_bridge::instance_id() const noexcept {
    return state_->instance_id;
}

bool hub_bridge::connected() const noexcept {
    return state_->bus.connected();
}

bool hub_bridge::subscribed() const noexcept {
    return state_->subscribed.load(std::memory_order_acquire);
}

void hub_bridge::publish(std::string_view topic, byte_span payload,
                         delivery_mode mode) noexcept {
    state* s = state_.get();
    if (s == nullptr) {
        return;
    }
    if (topic.empty()) {
        ++s->counters.outbound_rejected;
        return;
    }

    // Local delivery first: a bus outage or a bus rejection must never block,
    // undo or delay the service this instance owes its own subscribers.
    s->target.publish(topic, payload, mode);

    if (s->options.max_payload_bytes != 0 &&
        payload.size() > s->options.max_payload_bytes) {
        ++s->counters.outbound_dropped_oversize;
        return;
    }

    bridge_message message;
    try {
        message.source = s->instance_id;
        message.topic.assign(topic.data(), topic.size());
        message.payload.assign(payload.begin(), payload.end());
    } catch (...) {
        ++s->counters.outbound_dropped_allocation;
        return;
    }
    message.message_id = ++s->next_message_id;
    message.mode = mode;

    bool sent = false;
    try {
        sent = s->bus.publish(message).ok();
    } catch (...) {
        sent = false;
    }
    if (sent) {
        ++s->counters.outbound;
    } else {
        ++s->counters.publish_failures;
    }
}

void hub_bridge::pump() noexcept {
    state* s = state_.get();
    if (s == nullptr) {
        return;
    }

    // A disconnected bus is reconnected here, never inside `publish`: the
    // publish path must stay non-blocking and must not couple local delivery
    // to transport health.
    if (!s->bus.connected()) {
        s->subscribed.store(false, std::memory_order_release);
        bool reconnected = false;
        try {
            reconnected = s->bus.reconnect().ok();
        } catch (...) {
            reconnected = false;
        }
        if (!reconnected) {
            return; // retry on the next pump
        }
        ++s->counters.reconnects;
    }

    // The SUBSCRIBE state lives on the bus connection: it is lost with the
    // dead socket, whether this bridge reconnected or another actor sharing
    // the bus did. The generation counter is what turns "reconnected" into
    // "must subscribe again" for every bridge, not just the one that called
    // reconnect(). Forgetting this step leaves an instance permanently deaf
    // to cross-instance traffic -- the classic distributed-bridge failure.
    if (!s->subscribed.load(std::memory_order_acquire) ||
        s->bus.generation() != s->subscribed_generation) {
        bool subscribed_now = false;
        try {
            subscribed_now = s->bus.subscribe(s->handler).ok();
        } catch (...) {
            subscribed_now = false;
        }
        if (!subscribed_now) {
            s->subscribed.store(false, std::memory_order_release);
            return; // retry on the next pump
        }
        s->subscribed.store(true, std::memory_order_release);
        s->subscribed_generation = s->bus.generation();
        ++s->counters.subscribes;
    }

    while (s->inject_one()) {
    }
}

bridge_stats hub_bridge::stats() const noexcept {
    const state* s = state_.get();
    bridge_stats out;
    if (s == nullptr) {
        return out;
    }
    out.outbound = s->counters.outbound.load(std::memory_order_relaxed);
    out.outbound_rejected =
        s->counters.outbound_rejected.load(std::memory_order_relaxed);
    out.outbound_dropped_oversize =
        s->counters.outbound_dropped_oversize.load(std::memory_order_relaxed);
    out.outbound_dropped_allocation =
        s->counters.outbound_dropped_allocation.load(std::memory_order_relaxed);
    out.publish_failures =
        s->counters.publish_failures.load(std::memory_order_relaxed);
    out.inbound = s->counters.inbound.load(std::memory_order_relaxed);
    out.inbound_rejected =
        s->counters.inbound_rejected.load(std::memory_order_relaxed);
    out.inbound_dropped_oversize =
        s->counters.inbound_dropped_oversize.load(std::memory_order_relaxed);
    out.inbound_dropped_allocation =
        s->counters.inbound_dropped_allocation.load(std::memory_order_relaxed);
    out.inbound_dropped_full =
        s->counters.inbound_dropped_full.load(std::memory_order_relaxed);
    out.loops_filtered =
        s->counters.loops_filtered.load(std::memory_order_relaxed);
    out.duplicates_dropped =
        s->counters.duplicates_dropped.load(std::memory_order_relaxed);
    out.injected = s->counters.injected.load(std::memory_order_relaxed);
    out.reconnects = s->counters.reconnects.load(std::memory_order_relaxed);
    out.subscribes = s->counters.subscribes.load(std::memory_order_relaxed);
    return out;
}

} // namespace crpc::push
