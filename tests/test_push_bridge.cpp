// CRPC -- push hub bridge tests (cross-instance publish via an injected bus).
//
// The hub is a single-process structure: a publish reaches only the
// subscribers of the hub instance that received it. The bridge closes that gap
// with a dependency-injected `hub_bus`: outbound publishes fan out to the bus,
// inbound bus messages are handed to the local hub on the shard thread.
//
// Everything here runs against a fake bus, so the tests are deterministic and
// single-threaded except where a test deliberately delivers from another
// thread. The fake models the two properties that make real buses dangerous:
// it echoes every published message back to its sender (Redis Pub/Sub does),
// and a reconnect loses the subscription state that lived on the old socket.
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#pragma comment(lib, "dbghelp.lib")
#endif
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/push/bridge.hpp>
#include <crpc/push/hub.hpp>

using namespace crpc;
using namespace crpc::push;

namespace {

byte_span as_bytes(std::string_view text) {
    return byte_span(reinterpret_cast<const std::byte*>(text.data()),
                     text.size());
}

bool payload_is(const push_message& message, std::string_view text) {
    const byte_span data = message.data();
    if (data.size() != text.size()) {
        return false;
    }
    return data.size() == 0 ||
           std::memcmp(data.data(), text.data(), data.size()) == 0;
}

std::string payload_text(const push_message& message) {
    const byte_span data = message.data();
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

// A bus that behaves like Redis Pub/Sub over one socket: publish reaches every
// subscriber including the sender, subscribe registers the handler, and
// reconnect() re-establishes the connection but loses the SUBSCRIBE state.
// The generation counter stands in for "a new connection exists"; a bridge
// that does not resubscribe after it changes never receives again, which is
// exactly the failure mode the reconnect test must catch.
class fake_bus final : public hub_bus {
public:
    bool up = true;
    bool fail_publish = false;
    bool fail_reconnect = false;
    rpc_code error_code = rpc_code::unavailable;
    std::vector<message_handler> handlers;
    std::vector<bridge_message> sent;
    u64 generation_value = 0;

    [[nodiscard]] bool connected() const noexcept override { return up; }

    [[nodiscard]] u64 generation() const noexcept override {
        return generation_value;
    }

    [[nodiscard]] status publish(const bridge_message& message) override {
        sent.push_back(message);
        if (!up || fail_publish) {
            return status(error_code);
        }
        deliver(message);
        return status{};
    }

    [[nodiscard]] status subscribe(message_handler handler) override {
        if (!up) {
            return status(rpc_code::failed_precondition);
        }
        handlers.push_back(std::move(handler));
        return status{};
    }

    [[nodiscard]] status reconnect() override {
        if (fail_reconnect) {
            return status(rpc_code::connection_refused);
        }
        up = true;
        handlers.clear();
        ++generation_value;
        return status{};
    }

    void disconnect() noexcept {
        up = false;
        handlers.clear();
    }

    void deliver(const bridge_message& message) {
        for (const message_handler& handler : handlers) {
            handler(message);
        }
    }
};

std::vector<std::string> drain(hub& target, hub::connection_id connection) {
    std::vector<std::string> out;
    push_message message;
    while (target.try_take(connection, message)) {
        out.push_back(payload_text(message));
    }
    return out;
}

bridge_message foreign_message(std::string_view source, std::string_view topic,
                               std::string_view text, u64 message_id) {
    bridge_message message;
    message.source.assign(source.data(), source.size());
    message.topic.assign(topic.data(), topic.size());
    const std::string body(text);
    message.payload.assign(reinterpret_cast<const std::byte*>(body.data()),
                           reinterpret_cast<const std::byte*>(body.data()) +
                               body.size());
    message.message_id = message_id;
    message.mode = delivery_mode::unreliable;
    return message;
}

} // namespace

CRPC_TEST(bridge_make_rejects_empty_instance_id) {
    fake_bus bus;
    hub target;
    const auto empty = hub_bridge::make(target, bus, "");
    CRPC_CHECK_FALSE(static_cast<bool>(empty));
    CRPC_CHECK(empty.error().code == rpc_code::invalid_argument);

    const auto named = hub_bridge::make(target, bus, "instance-a");
    CRPC_CHECK(static_cast<bool>(named));
    CRPC_CHECK_EQ(std::string("instance-a"), named.value()->instance_id());
}

CRPC_TEST(bridge_local_delivery_survives_bus_failure) {
    fake_bus bus;
    bus.fail_publish = true;
    hub target;
    const auto bridge = hub_bridge::make(target, bus, "instance-a");
    CRPC_CHECK(static_cast<bool>(bridge));

    CRPC_CHECK(target.subscribe("room1", 1, delivery_mode::unreliable, false).ok());
    const hub::connection_id connection = target.attach(1);
    CRPC_CHECK(connection != hub::invalid_connection);

    bridge.value()->publish("room1", as_bytes("hello"),
                            delivery_mode::unreliable);

    // The bus rejected the message, but the local subscriber still received it.
    const std::vector<std::string> got = drain(target, connection);
    CRPC_CHECK_EQ(std::size_t(1), got.size());
    CRPC_CHECK_EQ(std::string("hello"), got.empty() ? std::string() : got[0]);

    CRPC_CHECK_EQ(u64(0), bridge.value()->stats().outbound);
    CRPC_CHECK_EQ(u64(1), bridge.value()->stats().publish_failures);
    CRPC_CHECK_EQ(u64(1), bus.sent.size());
}

CRPC_TEST(bridge_empty_topic_is_rejected) {
    fake_bus bus;
    hub target;
    const auto bridge = hub_bridge::make(target, bus, "instance-a");
    CRPC_CHECK(static_cast<bool>(bridge));

    bridge.value()->publish("", as_bytes("x"), delivery_mode::unreliable);

    CRPC_CHECK_EQ(u64(0), bus.sent.size());
    CRPC_CHECK_EQ(u64(0), bridge.value()->stats().outbound);
    CRPC_CHECK_EQ(u64(1), bridge.value()->stats().outbound_rejected);
    CRPC_CHECK_EQ(u64(0), target.stats().published);
}

CRPC_TEST(bridge_oversize_payload_is_delivered_locally_only) {
    fake_bus bus;
    hub target;
    bridge_options options;
    options.max_payload_bytes = 8;
    const auto bridge = hub_bridge::make(target, bus, "instance-a", options);
    CRPC_CHECK(static_cast<bool>(bridge));

    CRPC_CHECK(target.subscribe("room1", 1, delivery_mode::unreliable, false).ok());
    const hub::connection_id connection = target.attach(1);

    bridge.value()->publish("room1", as_bytes("0123456789"),
                            delivery_mode::unreliable);

    // Local subscribers still receive the oversized publish; only the bus copy
    // is skipped.
    const std::vector<std::string> got = drain(target, connection);
    CRPC_CHECK_EQ(std::size_t(1), got.size());
    CRPC_CHECK_EQ(std::string("0123456789"), got.empty() ? std::string() : got[0]);

    CRPC_CHECK_EQ(u64(0), bus.sent.size());
    CRPC_CHECK_EQ(u64(1), bridge.value()->stats().outbound_dropped_oversize);

    // A payload at the limit goes to the bus.
    bridge.value()->publish("room1", as_bytes("01234567"),
                            delivery_mode::unreliable);
    CRPC_CHECK_EQ(u64(1), bus.sent.size());
    CRPC_CHECK_EQ(u64(1), bridge.value()->stats().outbound);
}

CRPC_TEST(bridge_oversize_inbound_is_dropped) {
    fake_bus bus;
    hub target;
    bridge_options options;
    options.max_payload_bytes = 8;
    const auto bridge = hub_bridge::make(target, bus, "instance-a", options);
    CRPC_CHECK(static_cast<bool>(bridge));

    CRPC_CHECK(target.subscribe("room1", 1, delivery_mode::unreliable, false).ok());
    const hub::connection_id connection = target.attach(1);

    const bridge_message big =
        foreign_message("instance-b", "room1", "0123456789", 1);
    bus.deliver(big);
    bridge.value()->pump();

    CRPC_CHECK(drain(target, connection).empty());
    CRPC_CHECK_EQ(u64(0), bridge.value()->stats().injected);
    CRPC_CHECK_EQ(u64(1), bridge.value()->stats().inbound_dropped_oversize);
    CRPC_CHECK_EQ(u64(0), bridge.value()->stats().inbound);
}

CRPC_TEST(bridge_zero_payload_limit_is_unbounded) {
    fake_bus bus;
    hub target;
    bridge_options options;
    options.max_payload_bytes = 0;
    const auto bridge = hub_bridge::make(target, bus, "instance-a", options);
    CRPC_CHECK(static_cast<bool>(bridge));

    std::vector<std::byte> big(256 * 1024);
    bridge.value()->publish("room1", byte_span(big.data(), big.size()),
                            delivery_mode::unreliable);

    CRPC_CHECK_EQ(u64(1), bus.sent.size());
    CRPC_CHECK_EQ(u64(1), bridge.value()->stats().outbound);
    CRPC_CHECK_EQ(u64(0), bridge.value()->stats().outbound_dropped_oversize);
}

CRPC_TEST(bridge_filters_own_bus_echo) {
    fake_bus bus;
    hub target;
    const auto bridge = hub_bridge::make(target, bus, "instance-a");
    CRPC_CHECK(static_cast<bool>(bridge));

    CRPC_CHECK(target.subscribe("room1", 1, delivery_mode::unreliable, false).ok());
    const hub::connection_id connection = target.attach(1);

    // The fake bus echoes every publish back to the sender, like Redis.
    bridge.value()->publish("room1", as_bytes("hello"),
                            delivery_mode::unreliable);

    const std::vector<std::string> got = drain(target, connection);
    CRPC_CHECK_EQ(std::size_t(1), got.size());
    CRPC_CHECK_EQ(std::string("hello"), got.empty() ? std::string() : got[0]);

    CRPC_CHECK_EQ(u64(1), bridge.value()->stats().loops_filtered);
    CRPC_CHECK_EQ(u64(0), bridge.value()->stats().inbound);
    CRPC_CHECK_EQ(u64(0), bridge.value()->stats().injected);
}

CRPC_TEST(bridge_rejects_inbound_without_identity) {
    fake_bus bus;
    hub target;
    const auto bridge = hub_bridge::make(target, bus, "instance-a");
    CRPC_CHECK(static_cast<bool>(bridge));

    // A bus message without a source or without an id cannot be loop-checked
    // or deduplicated, so the bridge refuses it instead of guessing.
    bus.deliver(foreign_message("", "room1", "x", 1));
    bus.deliver(foreign_message("instance-b", "room1", "x", 0));
    bridge.value()->pump();

    CRPC_CHECK_EQ(u64(0), bridge.value()->stats().injected);
    CRPC_CHECK_EQ(u64(2), bridge.value()->stats().inbound_rejected);
    CRPC_CHECK_EQ(u64(0), bridge.value()->stats().inbound);
}

CRPC_TEST(bridge_suppresses_duplicate_bus_messages) {
    fake_bus bus;
    hub target;
    const auto bridge = hub_bridge::make(target, bus, "instance-a");
    CRPC_CHECK(static_cast<bool>(bridge));

    CRPC_CHECK(target.subscribe("room1", 1, delivery_mode::unreliable, false).ok());
    const hub::connection_id connection = target.attach(1);

    const bridge_message once =
        foreign_message("instance-b", "room1", "hello", 7);
    bus.deliver(once);
    bus.deliver(once); // the bus redelivered the same (source, id) pair
    bridge.value()->pump();

    const std::vector<std::string> got = drain(target, connection);
    CRPC_CHECK_EQ(std::size_t(1), got.size());
    CRPC_CHECK_EQ(std::string("hello"), got.empty() ? std::string() : got[0]);

    CRPC_CHECK_EQ(u64(1), bridge.value()->stats().injected);
    CRPC_CHECK_EQ(u64(1), bridge.value()->stats().duplicates_dropped);
}

CRPC_TEST(bridge_dedup_window_is_bounded) {
    fake_bus bus;
    hub target;
    bridge_options options;
    options.dedup_window = 2;
    const auto bridge = hub_bridge::make(target, bus, "instance-a", options);
    CRPC_CHECK(static_cast<bool>(bridge));

    CRPC_CHECK(target.subscribe("room1", 1, delivery_mode::unreliable, false).ok());
    const hub::connection_id connection = target.attach(1);

    bus.deliver(foreign_message("instance-b", "room1", "m1", 1));
    bus.deliver(foreign_message("instance-b", "room1", "m2", 2));
    bus.deliver(foreign_message("instance-b", "room1", "m3", 3));
    // m1 fell out of the two-entry window, so this redelivery is accepted:
    // the window bounds memory, it does not promise dedup forever.
    bus.deliver(foreign_message("instance-b", "room1", "m1", 1));
    bridge.value()->pump();

    const std::vector<std::string> got = drain(target, connection);
    CRPC_CHECK_EQ(std::size_t(4), got.size());
    CRPC_CHECK_EQ(u64(4), bridge.value()->stats().injected);
    CRPC_CHECK_EQ(u64(0), bridge.value()->stats().duplicates_dropped);
}

CRPC_TEST(bridge_pending_queue_bound_drops_overflow) {
    fake_bus bus;
    hub target;
    bridge_options options;
    options.max_pending = 1;
    const auto bridge = hub_bridge::make(target, bus, "instance-a", options);
    CRPC_CHECK(static_cast<bool>(bridge));

    CRPC_CHECK(target.subscribe("room1", 1, delivery_mode::unreliable, false).ok());
    const hub::connection_id connection = target.attach(1);

    bus.deliver(foreign_message("instance-b", "room1", "m1", 1));
    bus.deliver(foreign_message("instance-b", "room1", "m2", 2));
    bridge.value()->pump();

    const std::vector<std::string> got = drain(target, connection);
    CRPC_CHECK_EQ(std::size_t(1), got.size());
    CRPC_CHECK_EQ(std::string("m1"), got.empty() ? std::string() : got[0]);

    CRPC_CHECK_EQ(u64(1), bridge.value()->stats().injected);
    CRPC_CHECK_EQ(u64(1), bridge.value()->stats().inbound_dropped_full);
}

CRPC_TEST(bridge_inbound_waits_for_shard_pump) {
    fake_bus bus;
    hub target;
    const auto bridge = hub_bridge::make(target, bus, "instance-a");
    CRPC_CHECK(static_cast<bool>(bridge));

    CRPC_CHECK(target.subscribe("room1", 1, delivery_mode::unreliable, false).ok());
    const hub::connection_id connection = target.attach(1);

    hub other;
    const auto sender = hub_bridge::make(other, bus, "instance-b");
    CRPC_CHECK(static_cast<bool>(sender));
    sender.value()->publish("room1", as_bytes("cross"),
                            delivery_mode::unreliable);

    // The bus handler only queues: the hub must not be touched from the bus
    // thread, so nothing is visible before the shard pump runs.
    CRPC_CHECK_EQ(u64(0), target.stats().delivered);
    CRPC_CHECK(drain(target, connection).empty());
    CRPC_CHECK_EQ(u64(0), bridge.value()->stats().injected);
    CRPC_CHECK_EQ(u64(1), bridge.value()->stats().inbound);

    bridge.value()->pump();
    const std::vector<std::string> got = drain(target, connection);
    CRPC_CHECK_EQ(std::size_t(1), got.size());
    CRPC_CHECK_EQ(std::string("cross"), got.empty() ? std::string() : got[0]);
    CRPC_CHECK_EQ(u64(1), bridge.value()->stats().injected);
}

CRPC_TEST(two_instances_end_to_end) {
    fake_bus bus;
    hub hub_a;
    hub hub_b;
    const auto a = hub_bridge::make(hub_a, bus, "instance-a");
    const auto b = hub_bridge::make(hub_b, bus, "instance-b");
    CRPC_CHECK(static_cast<bool>(a));
    CRPC_CHECK(static_cast<bool>(b));
    if (!a || !b) {
        return;
    }

    CRPC_CHECK(hub_a.subscribe("room1", 1, delivery_mode::unreliable, false).ok());
    CRPC_CHECK(hub_b.subscribe("room1", 2, delivery_mode::unreliable, false).ok());
    const hub::connection_id ca = hub_a.attach(1);
    const hub::connection_id cb = hub_b.attach(2);
    CRPC_CHECK(ca != hub::invalid_connection);
    CRPC_CHECK(cb != hub::invalid_connection);

    // A publishes: its own subscriber receives the message exactly once and
    // the bus echo back to A is filtered.
    a.value()->publish("room1", as_bytes("hello"), delivery_mode::unreliable);
    std::vector<std::string> got_a = drain(hub_a, ca);
    CRPC_CHECK_EQ(std::size_t(1), got_a.size());
    CRPC_CHECK_EQ(std::string("hello"), got_a.empty() ? std::string() : got_a[0]);
    CRPC_CHECK_EQ(u64(1), a.value()->stats().loops_filtered);

    // B's subscriber receives the same message exactly once, after B's shard
    // pump injects it.
    b.value()->pump();
    const std::vector<std::string> got_b = drain(hub_b, cb);
    CRPC_CHECK_EQ(std::size_t(1), got_b.size());
    CRPC_CHECK_EQ(std::string("hello"), got_b.empty() ? std::string() : got_b[0]);
    CRPC_CHECK_EQ(u64(1), b.value()->stats().injected);
    CRPC_CHECK_EQ(u64(0), b.value()->stats().duplicates_dropped);
    CRPC_CHECK_EQ(u64(0), b.value()->stats().loops_filtered);

    // The reverse direction works the same way.
    b.value()->publish("room1", as_bytes("hi-a"), delivery_mode::unreliable);
    std::vector<std::string> got_b2 = drain(hub_b, cb);
    CRPC_CHECK_EQ(std::size_t(1), got_b2.size());
    CRPC_CHECK_EQ(std::string("hi-a"), got_b2.empty() ? std::string() : got_b2[0]);
    a.value()->pump();
    const std::vector<std::string> got_a2 = drain(hub_a, ca);
    CRPC_CHECK_EQ(std::size_t(1), got_a2.size());
    CRPC_CHECK_EQ(std::string("hi-a"), got_a2.empty() ? std::string() : got_a2[0]);
    CRPC_CHECK_EQ(u64(1), a.value()->stats().injected);
    CRPC_CHECK_EQ(u64(1), a.value()->stats().loops_filtered);
}

CRPC_TEST(bridge_forwards_delivery_mode) {
    fake_bus bus;
    hub hub_a;
    hub hub_b;
    const auto a = hub_bridge::make(hub_a, bus, "instance-a");
    const auto b = hub_bridge::make(hub_b, bus, "instance-b");
    CRPC_CHECK(static_cast<bool>(a));
    CRPC_CHECK(static_cast<bool>(b));
    if (!a || !b) {
        return;
    }

    CRPC_CHECK(hub_b.subscribe("room1", 2, delivery_mode::reliable, false).ok());
    const hub::connection_id cb = hub_b.attach(2);

    a.value()->publish("room1", as_bytes("r"), delivery_mode::reliable);
    b.value()->pump();

    push_message message;
    CRPC_CHECK(hub_b.try_take(cb, message));
    CRPC_CHECK(message.mode == delivery_mode::reliable);
    CRPC_CHECK(payload_is(message, "r"));
    // The receiving hub assigns its own per-subscription sequence; the bridge
    // does not forward the source hub's counters.
    CRPC_CHECK_EQ(u64(1), message.seq);
}

CRPC_TEST(bridge_reconnects_and_resubscribes_after_bus_disconnect) {
    fake_bus bus;
    hub hub_a;
    hub hub_b;
    const auto a = hub_bridge::make(hub_a, bus, "instance-a");
    const auto b = hub_bridge::make(hub_b, bus, "instance-b");
    CRPC_CHECK(static_cast<bool>(a));
    CRPC_CHECK(static_cast<bool>(b));
    if (!a || !b) {
        return;
    }

    CRPC_CHECK(hub_a.subscribe("room1", 1, delivery_mode::unreliable, false).ok());
    CRPC_CHECK(hub_b.subscribe("room1", 2, delivery_mode::unreliable, false).ok());
    const hub::connection_id ca = hub_a.attach(1);
    const hub::connection_id cb = hub_b.attach(2);

    a.value()->publish("room1", as_bytes("m1"), delivery_mode::unreliable);
    b.value()->pump();
    CRPC_CHECK_EQ(std::size_t(1), drain(hub_b, cb).size());
    CRPC_CHECK_EQ(std::size_t(1), drain(hub_a, ca).size());
    CRPC_CHECK_EQ(u64(0), bus.generation());

    // The bus connection drops: the SUBSCRIBE state lived on that socket and
    // is gone with it.
    bus.disconnect();
    CRPC_CHECK_FALSE(bus.connected());
    CRPC_CHECK_FALSE(a.value()->connected());

    // A publish while the bus is down still delivers locally and is counted.
    a.value()->publish("room1", as_bytes("m2"), delivery_mode::unreliable);
    const std::vector<std::string> local = drain(hub_a, ca);
    CRPC_CHECK_EQ(std::size_t(1), local.size());
    CRPC_CHECK_EQ(std::string("m2"), local.empty() ? std::string() : local[0]);
    CRPC_CHECK_EQ(u64(1), a.value()->stats().publish_failures);

    // B pumps first and reconnects the shared bus; the generation advances.
    b.value()->pump();
    CRPC_CHECK(bus.connected());
    CRPC_CHECK_EQ(u64(1), bus.generation());
    CRPC_CHECK_EQ(u64(1), b.value()->stats().reconnects);
    CRPC_CHECK_EQ(u64(2), b.value()->stats().subscribes);

    // A's own subscribe flag is still set, but the bus lost A's handler with
    // the dead socket. The generation mismatch is what makes A resubscribe
    // even though it never attempted a reconnect itself.
    a.value()->pump();
    CRPC_CHECK_EQ(u64(0), a.value()->stats().reconnects);
    CRPC_CHECK_EQ(u64(2), a.value()->stats().subscribes);
    CRPC_CHECK_EQ(std::size_t(2), bus.handlers.size());
    CRPC_CHECK(a.value()->subscribed());

    // After both bridges resubscribed, a new publish crosses to B. m2 never
    // crossed: it was published while the bus was down.
    a.value()->publish("room1", as_bytes("m3"), delivery_mode::unreliable);
    b.value()->pump();
    const std::vector<std::string> got = drain(hub_b, cb);
    CRPC_CHECK_EQ(std::size_t(1), got.size());
    CRPC_CHECK_EQ(std::string("m3"), got.empty() ? std::string() : got[0]);

    CRPC_CHECK_EQ(u64(2), a.value()->stats().outbound);
    CRPC_CHECK_EQ(u64(2), b.value()->stats().injected);
    CRPC_CHECK_EQ(u64(2), a.value()->stats().loops_filtered);
}

CRPC_TEST(bridge_reconnect_failure_retries_on_next_pump) {
    fake_bus bus;
    hub target;
    const auto bridge = hub_bridge::make(target, bus, "instance-a");
    CRPC_CHECK(static_cast<bool>(bridge));

    bus.disconnect();
    bus.fail_reconnect = true;
    bridge.value()->pump();

    CRPC_CHECK_EQ(u64(0), bridge.value()->stats().reconnects);
    CRPC_CHECK_EQ(u64(1), bridge.value()->stats().subscribes);
    CRPC_CHECK_FALSE(bridge.value()->subscribed());

    bus.fail_reconnect = false;
    bridge.value()->pump();
    CRPC_CHECK_EQ(u64(1), bridge.value()->stats().reconnects);
    CRPC_CHECK_EQ(u64(2), bridge.value()->stats().subscribes);
    CRPC_CHECK(bridge.value()->subscribed());
    CRPC_CHECK(bridge.value()->connected());
}

CRPC_TEST(bridge_delivers_messages_from_another_thread) {
    fake_bus bus;
    hub target;
    const auto bridge = hub_bridge::make(target, bus, "instance-a");
    CRPC_CHECK(static_cast<bool>(bridge));

    CRPC_CHECK(target.subscribe("room1", 1, delivery_mode::unreliable, false).ok());
    const hub::connection_id connection = target.attach(1);

    // A real bus invokes the handler on its reader thread. The handoff queue
    // must accept it; the shard thread drains it afterwards.
    const bridge_message remote =
        foreign_message("instance-b", "room1", "threaded", 1);
    std::thread sender([&bus, &remote] { bus.deliver(remote); });
    sender.join();

    bridge.value()->pump();
    const std::vector<std::string> got = drain(target, connection);
    CRPC_CHECK_EQ(std::size_t(1), got.size());
    CRPC_CHECK_EQ(std::string("threaded"), got.empty() ? std::string() : got[0]);
    CRPC_CHECK_EQ(u64(1), bridge.value()->stats().injected);
}

CRPC_TEST(bridge_stats_report_counters_and_connection) {
    fake_bus bus;
    hub target;
    const auto bridge = hub_bridge::make(target, bus, "instance-a");
    CRPC_CHECK(static_cast<bool>(bridge));

    CRPC_CHECK(bridge.value()->connected());
    CRPC_CHECK(bridge.value()->subscribed());
    CRPC_CHECK_EQ(u64(1), bridge.value()->stats().subscribes);
    CRPC_CHECK_EQ(u64(0), bridge.value()->stats().outbound);
    CRPC_CHECK_EQ(u64(0), bridge.value()->stats().inbound);

    CRPC_CHECK(target.subscribe("room1", 1, delivery_mode::unreliable, false).ok());
    bridge.value()->publish("room1", as_bytes("x"), delivery_mode::unreliable);
    CRPC_CHECK_EQ(u64(1), bridge.value()->stats().outbound);

    bus.disconnect();
    CRPC_CHECK_FALSE(bridge.value()->connected());
    bridge.value()->pump();
    CRPC_CHECK(bridge.value()->connected());
    CRPC_CHECK_EQ(u64(1), bridge.value()->stats().reconnects);
}

CRPC_TEST_MAIN()
