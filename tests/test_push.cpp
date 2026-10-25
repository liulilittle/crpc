// CRPC -- push hub and streaming tests.
//
// Everything here is deterministic: the hub never reads the wall clock, and the
// slow-subscriber test drives `tick(now_ns)` by hand. The stream tests use a
// fake in-memory transport that models a connection demultiplexer (bounded send
// credit, borrowed payloads, reset/eof).
#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#pragma comment(lib, "dbghelp.lib")
#endif
#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/push/hub.hpp>
#include <crpc/push/stream.hpp>

using namespace crpc;
using namespace crpc::push;

namespace {

byte_span as_bytes(std::string_view text) {
    return byte_span(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

bool payload_is(const push_message& message, std::string_view text) {
    const byte_span data = message.data();
    if (data.size() != text.size()) {
        return false;
    }
    return data.size() == 0 ||
           std::memcmp(data.data(), text.data(), data.size()) == 0;
}

int code_id(rpc_code code) { return static_cast<int>(code); }

// A demultiplexer stand-in: inbound payload frames, a bounded send ring with
// credit, and cancellation. The borrowed `pull` span stays valid until the next
// `pull` on the same transport.
class fake_transport final : public stream_transport {
public:
    std::deque<std::vector<std::byte>> inbound;
    std::vector<std::byte> sent;
    std::vector<std::byte> current;
    std::size_t credit = 1u << 20;
    std::size_t max_send = static_cast<std::size_t>(-1);
    std::size_t credits_released = 0;
    std::size_t resets = 0;
    std::size_t end_sends = 0;
    bool at_eof = false;
    bool canceled_flag = false;
    bool end_full = false;
    rpc_code cancel_code = rpc_code::canceled;

    pull_result pull(u32 stream_id, span<const std::byte>& payload,
                     rpc_code& error) noexcept override {
        (void)stream_id;
        if (canceled_flag) {
            error = cancel_code;
            return pull_result::canceled;
        }
        if (!inbound.empty()) {
            current = std::move(inbound.front());
            inbound.pop_front();
            payload = span<const std::byte>(current.data(), current.size());
            return pull_result::data;
        }
        if (at_eof) {
            error = rpc_code::eof;
            return pull_result::eof;
        }
        error = rpc_code::unavailable;
        return pull_result::would_block;
    }

    std::size_t send_credit(u32 stream_id) const noexcept override {
        (void)stream_id;
        return credit;
    }

    std::size_t send(u32 stream_id, span<const std::byte> data) noexcept override {
        (void)stream_id;
        std::size_t count = data.size();
        if (count > credit) {
            count = credit;
        }
        if (count > max_send) {
            count = max_send;
        }
        sent.insert(sent.end(), data.begin(), data.begin() + count);
        credit -= count;
        return count;
    }

    bool send_end(u32 stream_id) noexcept override {
        (void)stream_id;
        if (end_full) {
            return false;
        }
        ++end_sends;
        return true;
    }

    void release_read_credit(u32 stream_id, std::size_t bytes) noexcept override {
        (void)stream_id;
        credits_released += bytes;
    }

    void send_reset(u32 stream_id, rpc_code code) noexcept override {
        (void)stream_id;
        ++resets;
        canceled_flag = true;
        cancel_code = code;
    }

    void push_frame(std::string_view text) {
        std::vector<std::byte> frame(text.size());
        if (!text.empty()) {
            std::memcpy(frame.data(), text.data(), text.size());
        }
        inbound.push_back(std::move(frame));
    }
};

std::string sent_text(const fake_transport& transport) {
    return std::string(reinterpret_cast<const char*>(transport.sent.data()),
                       transport.sent.size());
}

} // namespace

// --- Hub: basic lifecycle ----------------------------------------------------

CRPC_TEST(hub_subscribe_publish_unsubscribe) {
    hub h;
    hub_options options;
    (void)options;

    // Publishing before subscribing is a no-op.
    h.publish("topic", as_bytes("x"), delivery_mode::unreliable);
    CRPC_CHECK_EQ(h.stats().delivered, static_cast<u64>(0));

    CRPC_CHECK(h.subscribe("topic", 1, delivery_mode::unreliable, false).ok());
    auto connection = h.attach(1);
    CRPC_CHECK(connection != hub::invalid_connection);

    h.publish("topic", as_bytes("hello"), delivery_mode::unreliable);
    push_message message;
    CRPC_CHECK(h.try_take(connection, std::string_view("topic"), message));
    CRPC_CHECK(payload_is(message, "hello"));
    CRPC_CHECK_EQ(std::string(message.topic), std::string("topic"));

    CRPC_CHECK(h.unsubscribe("topic", 1).ok());
    CRPC_CHECK_EQ(h.stats().active_subscriptions, static_cast<u64>(0));
    CRPC_CHECK_EQ(h.stats().queued_messages, static_cast<u64>(0));
    CRPC_CHECK_EQ(code_id(h.unsubscribe("topic", 1).code()),
                  code_id(rpc_code::not_found));

    h.publish("topic", as_bytes("late"), delivery_mode::unreliable);
    CRPC_CHECK_FALSE(h.try_take(connection, std::string_view("topic"), message));
}

CRPC_TEST(hub_fanout_to_n_subscribers) {
    hub h;
    constexpr int k_count = 8;
    std::vector<hub::connection_id> connections;
    for (int i = 0; i < k_count; ++i) {
        CRPC_CHECK(h.subscribe("broadcast", static_cast<u64>(i),
                               delivery_mode::unreliable, false)
                       .ok());
        connections.push_back(h.attach(static_cast<u64>(i)));
    }

    h.publish("broadcast", as_bytes("payload"), delivery_mode::unreliable);
    CRPC_CHECK_EQ(h.stats().published, static_cast<u64>(1));

    int received = 0;
    for (int i = 0; i < k_count; ++i) {
        push_message message;
        if (h.try_take(connections[static_cast<std::size_t>(i)],
                       std::string_view("broadcast"), message) &&
            payload_is(message, "payload")) {
            ++received;
        }
    }
    CRPC_CHECK_EQ(received, k_count);
    CRPC_CHECK_EQ(h.stats().delivered, static_cast<u64>(k_count));
}

CRPC_TEST(hub_reconnect_resumes_subscriptions) {
    hub h;
    CRPC_CHECK(h.subscribe("a", 5, delivery_mode::reliable, false).ok());
    CRPC_CHECK(h.subscribe("b", 5, delivery_mode::reliable, false).ok());

    auto first = h.attach(5);
    h.publish("a", as_bytes("m1"), delivery_mode::reliable);
    push_message message;
    CRPC_CHECK(h.try_take(first, std::string_view("a"), message));
    CRPC_CHECK_EQ(message.seq, static_cast<u64>(1));
    CRPC_CHECK(h.ack(5, "a", message.seq).ok());

    // Reconnect while the first connection is still alive: the queued messages
    // retarget to the new connection and the stale one can no longer take.
    auto second = h.attach(5);
    h.publish("b", as_bytes("m2"), delivery_mode::reliable);
    CRPC_CHECK_FALSE(h.try_take(first, std::string_view("b"), message));
    CRPC_CHECK(h.try_take(second, std::string_view("b"), message));
    CRPC_CHECK(payload_is(message, "m2"));

    // Detach, publish while disconnected, then resume.
    CRPC_CHECK(h.detach(second).ok());
    h.publish("a", as_bytes("m3"), delivery_mode::reliable);
    auto third = h.attach(5);
    CRPC_CHECK(h.try_take(third, std::string_view("a"), message));
    CRPC_CHECK(payload_is(message, "m3"));
    CRPC_CHECK_EQ(message.seq, static_cast<u64>(2));

    CRPC_CHECK(h.ack(5, "a", message.seq).ok());
}

// --- Hub: overflow policies --------------------------------------------------

namespace {

subscribe_options small_queue_options(overflow_policy policy) {
    subscribe_options options;
    options.max_messages = 2;
    options.max_bytes = 1u << 20;
    options.on_overflow = policy;
    return options;
}

} // namespace

CRPC_TEST(hub_overflow_drop_oldest) {
    hub h;
    CRPC_CHECK(h.subscribe("t", 1, delivery_mode::unreliable, false,
                           small_queue_options(overflow_policy::drop_oldest))
                   .ok());
    auto connection = h.attach(1);

    h.publish("t", as_bytes("1"), delivery_mode::unreliable);
    h.publish("t", as_bytes("2"), delivery_mode::unreliable);
    h.publish("t", as_bytes("3"), delivery_mode::unreliable);
    CRPC_CHECK_EQ(h.stats().dropped_overflow, static_cast<u64>(1));

    push_message message;
    CRPC_CHECK(h.try_take(connection, std::string_view("t"), message));
    CRPC_CHECK(payload_is(message, "2"));
    CRPC_CHECK(h.try_take(connection, std::string_view("t"), message));
    CRPC_CHECK(payload_is(message, "3"));
    CRPC_CHECK_FALSE(h.try_take(connection, std::string_view("t"), message));
}

CRPC_TEST(hub_overflow_drop_newest) {
    hub h;
    CRPC_CHECK(h.subscribe("t", 1, delivery_mode::unreliable, false,
                           small_queue_options(overflow_policy::drop_newest))
                   .ok());
    auto connection = h.attach(1);

    h.publish("t", as_bytes("1"), delivery_mode::unreliable);
    h.publish("t", as_bytes("2"), delivery_mode::unreliable);
    h.publish("t", as_bytes("3"), delivery_mode::unreliable);
    CRPC_CHECK_EQ(h.stats().dropped_overflow, static_cast<u64>(1));
    CRPC_CHECK_EQ(h.stats().dropped_unreliable, static_cast<u64>(1));

    push_message message;
    CRPC_CHECK(h.try_take(connection, std::string_view("t"), message));
    CRPC_CHECK(payload_is(message, "1"));
    CRPC_CHECK(h.try_take(connection, std::string_view("t"), message));
    CRPC_CHECK(payload_is(message, "2"));
    CRPC_CHECK_FALSE(h.try_take(connection, std::string_view("t"), message));
}

CRPC_TEST(hub_overflow_disconnect_subscriber) {
    hub h;
    CRPC_CHECK(h.subscribe("t", 1, delivery_mode::unreliable, false,
                           small_queue_options(
                               overflow_policy::disconnect_subscriber))
                   .ok());
    auto connection = h.attach(1);

    h.publish("t", as_bytes("1"), delivery_mode::unreliable);
    h.publish("t", as_bytes("2"), delivery_mode::unreliable);
    h.publish("t", as_bytes("3"), delivery_mode::unreliable);

    CRPC_CHECK_EQ(h.stats().active_subscriptions, static_cast<u64>(0));
    push_message message;
    CRPC_CHECK_FALSE(h.try_take(connection, std::string_view("t"), message));

    hub_event event;
    bool seen = false;
    while (h.poll_event(event)) {
        if (event.kind == hub_event_kind::disconnected) {
            seen = true;
        }
    }
    CRPC_CHECK(seen);
}

CRPC_TEST(hub_publisher_never_blocks) {
    hub h;
    subscribe_options options;
    options.max_messages = 1;
    options.on_overflow = overflow_policy::drop_oldest;
    CRPC_CHECK(h.subscribe("t", 1, delivery_mode::unreliable, false, options).ok());
    auto connection = h.attach(1);
    (void)connection; // The subscriber never drains: a stalled consumer.

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100000; ++i) {
        h.publish("t", as_bytes("payload"), delivery_mode::unreliable);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    CRPC_CHECK(h.stats().dropped_overflow > 0);
    CRPC_CHECK_EQ(h.stats().queued_messages, static_cast<u64>(1));
    // Accounting invariant for one subscriber that never drains: every
    // published message is either still queued or was dropped on overflow.
    // This replaces the old wall-clock bound, which asserted a performance
    // property a loaded CI host could not guarantee.
    const hub_stats stats = h.stats();
    CRPC_CHECK_EQ(stats.published,
                  stats.queued_messages + stats.dropped_overflow);
    // A 20 s watchdog only guards against a hang; it is not a throughput claim.
    CRPC_CHECK(elapsed < std::chrono::seconds(20));
}

// --- Hub: reliable mode ------------------------------------------------------

CRPC_TEST(hub_reliable_retransmit_and_dedup) {
    hub h;
    subscribe_options options;
    options.retransmit_budget = 4;
    CRPC_CHECK(h.subscribe("t", 7, delivery_mode::reliable, false, options).ok());
    auto connection = h.attach(7);

    h.publish("t", as_bytes("m1"), delivery_mode::reliable);
    push_message message;
    CRPC_CHECK(h.try_take(connection, std::string_view("t"), message));
    CRPC_CHECK_EQ(message.seq, static_cast<u64>(1));

    CRPC_CHECK(h.retransmit(7).ok());
    CRPC_CHECK_EQ(h.stats().retransmits, static_cast<u64>(1));
    CRPC_CHECK(h.try_take(connection, std::string_view("t"), message));
    CRPC_CHECK_EQ(message.seq, static_cast<u64>(1)); // same seq, no renumbering

    CRPC_CHECK(h.ack(7, "t", 1).ok());
    CRPC_CHECK_EQ(h.stats().queued_messages, static_cast<u64>(0));
    CRPC_CHECK(h.ack(7, "t", 1).ok()); // duplicate ack is idempotent
    CRPC_CHECK(h.stats().duplicate_acks >= static_cast<u64>(1));

    // A retransmit after the ack finds nothing to replay.
    const u64 before = h.stats().retransmits;
    CRPC_CHECK(h.retransmit(7).ok());
    CRPC_CHECK_EQ(h.stats().retransmits, before);
}

CRPC_TEST(hub_reliable_budget_exhausted) {
    hub h;
    subscribe_options options;
    options.retransmit_budget = 1;
    CRPC_CHECK(h.subscribe("t", 9, delivery_mode::reliable, false, options).ok());
    auto connection = h.attach(9);

    h.publish("t", as_bytes("m"), delivery_mode::reliable);
    push_message message;
    CRPC_CHECK(h.try_take(connection, std::string_view("t"), message));

    CRPC_CHECK(h.retransmit(9).ok());
    CRPC_CHECK(h.try_take(connection, std::string_view("t"), message));
    CRPC_CHECK(h.retransmit(9).ok()); // budget exceeded: reports not_acknowledged

    CRPC_CHECK_EQ(h.stats().not_acknowledged, static_cast<u64>(1));
    CRPC_CHECK_FALSE(h.try_take(connection, std::string_view("t"), message));

    hub_event event;
    bool seen = false;
    while (h.poll_event(event)) {
        if (event.kind == hub_event_kind::not_acknowledged &&
            code_id(event.code) == code_id(rpc_code::not_acknowledged)) {
            seen = true;
        }
    }
    CRPC_CHECK(seen);
}

// --- Hub: ordered mode -------------------------------------------------------

CRPC_TEST(hub_ordered_in_order) {
    hub h;
    CRPC_CHECK(h.subscribe("t", 1, delivery_mode::unreliable, true).ok());
    auto connection = h.attach(1);

    for (int i = 0; i < 5; ++i) {
        h.publish("t", as_bytes("m"), delivery_mode::unreliable);
    }
    push_message message;
    u64 expected = 1;
    while (h.try_take(connection, std::string_view("t"), message)) {
        CRPC_CHECK_EQ(message.topic_seq, expected);
        ++expected;
    }
    CRPC_CHECK_EQ(expected, static_cast<u64>(6));
    CRPC_CHECK_EQ(h.stats().ordered_gaps, static_cast<u64>(0));
}

CRPC_TEST(hub_ordered_gap_detected) {
    hub h;
    CRPC_CHECK(h.subscribe("t", 1, delivery_mode::unreliable, true).ok());
    auto connection = h.attach(1);

    h.publish("t", as_bytes("a"), delivery_mode::unreliable); // topic_seq 1
    push_message message;
    CRPC_CHECK(h.try_take(connection, std::string_view("t"), message));
    CRPC_CHECK_EQ(message.topic_seq, static_cast<u64>(1));

    publish_options jump;
    jump.has_topic_seq = true;
    jump.topic_seq = 3;
    h.publish("t", as_bytes("c"), delivery_mode::unreliable, jump);
    CRPC_CHECK_EQ(h.stats().ordered_gaps, static_cast<u64>(1));
    CRPC_CHECK(h.try_take(connection, std::string_view("t"), message));
    CRPC_CHECK_EQ(message.topic_seq, static_cast<u64>(3));

    publish_options stale;
    stale.has_topic_seq = true;
    stale.topic_seq = 2;
    h.publish("t", as_bytes("b"), delivery_mode::unreliable, stale);
    CRPC_CHECK_EQ(h.stats().ordered_duplicates, static_cast<u64>(1));
    CRPC_CHECK_FALSE(h.try_take(connection, std::string_view("t"), message));

    publish_options next;
    next.has_topic_seq = true;
    next.topic_seq = 4;
    h.publish("t", as_bytes("d"), delivery_mode::unreliable, next);
    CRPC_CHECK(h.try_take(connection, std::string_view("t"), message));
    CRPC_CHECK_EQ(message.topic_seq, static_cast<u64>(4));

    hub_event event;
    bool seen = false;
    while (h.poll_event(event)) {
        if (event.kind == hub_event_kind::ordered_gap) {
            seen = true;
        }
    }
    CRPC_CHECK(seen);
}

// --- Hub: slow subscriber and leaks -----------------------------------------

CRPC_TEST(hub_slow_subscriber_disconnected) {
    hub_options hub_config;
    hub_config.slow_watermark_bytes = 64;
    hub_config.slow_timeout = std::chrono::milliseconds(1);
    hub h(hub_config);

    subscribe_options options;
    options.max_messages = 1000;
    options.max_bytes = 1u << 20;
    CRPC_CHECK(h.subscribe("t", 3, delivery_mode::unreliable, false, options).ok());
    auto connection = h.attach(3);

    h.tick(0);
    for (int i = 0; i < 3; ++i) {
        h.publish("t", as_bytes(std::string(100, 'x')), delivery_mode::unreliable);
    }
    CRPC_CHECK_EQ(h.stats().slow_disconnects, static_cast<u64>(0));

    h.tick(2'000'000); // 2 ms > slow_timeout
    CRPC_CHECK_EQ(h.stats().slow_disconnects, static_cast<u64>(1));
    CRPC_CHECK_EQ(h.stats().active_subscriptions, static_cast<u64>(0));

    push_message message;
    CRPC_CHECK_FALSE(h.try_take(connection, std::string_view("t"), message));

    hub_event event;
    bool seen = false;
    while (h.poll_event(event)) {
        if (event.kind == hub_event_kind::slow_subscriber) {
            seen = true;
        }
    }
    CRPC_CHECK(seen);
}

CRPC_TEST(hub_no_leak_after_close) {
    hub h;
    CRPC_CHECK(h.subscribe("a", 1, delivery_mode::reliable, false).ok());
    CRPC_CHECK(h.subscribe("b", 1, delivery_mode::reliable, false).ok());
    CRPC_CHECK(h.subscribe("a", 2, delivery_mode::unreliable, false).ok());
    auto c1 = h.attach(1);
    auto c2 = h.attach(2);

    h.publish("a", as_bytes("x"), delivery_mode::reliable);
    h.publish("b", as_bytes("y"), delivery_mode::reliable);
    h.publish("a", as_bytes("z"), delivery_mode::unreliable);

    CRPC_CHECK(h.detach(c1).ok());
    CRPC_CHECK(h.detach(c2).ok());
    CRPC_CHECK(h.unsubscribe_all(1).ok());
    CRPC_CHECK(h.unsubscribe_all(2).ok());

    const hub_stats stats = h.stats();
    CRPC_CHECK_EQ(stats.active_subscriptions, static_cast<u64>(0));
    CRPC_CHECK_EQ(stats.active_queues, static_cast<u64>(0));
    CRPC_CHECK_EQ(stats.queued_messages, static_cast<u64>(0));
    CRPC_CHECK_EQ(stats.queued_bytes, static_cast<u64>(0));
    CRPC_CHECK_EQ(stats.active_connections, static_cast<u64>(0));
}

CRPC_TEST(hub_shard_routing) {
    hub_options first;
    first.shard_count = 2;
    first.shard_index = 0;
    hub_options second;
    second.shard_count = 2;
    second.shard_index = 1;
    hub h0(first);
    hub h1(second);

    int owned0 = 0;
    for (const char* name : {"alpha", "beta", "gamma", "delta", "epsilon"}) {
        const bool a = h0.owns_topic(name);
        const bool b = h1.owns_topic(name);
        CRPC_CHECK(a != b);
        if (a) {
            ++owned0;
        }
    }
    CRPC_CHECK(owned0 > 0);
    CRPC_CHECK(owned0 < 5);

    CRPC_CHECK_EQ(topic_shard("topic", 42u, 2u), topic_shard("topic", 42u, 2u));
    CRPC_CHECK(topic_shard("topic", 0u, 1u) == 0u);
}

// --- Hub: keep-queue release and front replay --------------------------------

CRPC_TEST(hub_release_keeps_queue_and_attach_none) {
    hub h;
    subscribe_options options;
    options.max_messages = 8;
    options.max_replay = 8;
    options.retransmit_budget = 3;
    CRPC_CHECK(
        h.subscribe("t", 1, delivery_mode::reliable, true, options).ok());
    auto first = h.attach(1);
    for (int i = 1; i <= 6; ++i) {
        const std::string body = "m" + std::to_string(i);
        h.publish("t", as_bytes(body), delivery_mode::reliable);
    }

    push_message message;
    CRPC_CHECK(h.try_take(first, std::string_view("t"), message));
    CRPC_CHECK_EQ(message.seq, static_cast<u64>(1));

    // Session end: keep-queue release. The queue is untouched and nothing is
    // counted as a retransmission.
    CRPC_CHECK(h.release(first).ok());
    CRPC_CHECK_FALSE(h.try_take(first, std::string_view("t"), message));
    CRPC_CHECK_EQ(h.stats().retransmits, static_cast<u64>(0));
    CRPC_CHECK_EQ(h.stats().not_acknowledged, static_cast<u64>(0));
    CRPC_CHECK_EQ(h.stats().queued_messages, static_cast<u64>(5));

    // A new connection only retargets the queue; attach(none) does not replay
    // the already-taken sequence.
    const auto second = h.attach(1, attach_replay::none);
    CRPC_CHECK(second != hub::invalid_connection);
    for (int i = 2; i <= 6; ++i) {
        CRPC_CHECK(h.try_take(second, std::string_view("t"), message));
        CRPC_CHECK_EQ(message.seq, static_cast<u64>(i));
        CRPC_CHECK(h.ack(1, "t", message.seq).ok());
    }
    CRPC_CHECK_FALSE(h.try_take(second, std::string_view("t"), message));
    CRPC_CHECK_EQ(h.stats().retransmits, static_cast<u64>(0));
    CRPC_CHECK_EQ(h.stats().not_acknowledged, static_cast<u64>(0));
    CRPC_CHECK_EQ(h.stats().active_subscriptions, static_cast<u64>(1));

    CRPC_CHECK_EQ(code_id(h.release(first).code()), code_id(rpc_code::not_found));
}

CRPC_TEST(hub_retransmit_front_preserves_order) {
    hub h;
    subscribe_options options;
    options.max_messages = 8;
    options.max_replay = 8;
    options.retransmit_budget = 3;
    CRPC_CHECK(
        h.subscribe("t", 1, delivery_mode::reliable, true, options).ok());
    const auto connection = h.attach(1);
    for (int i = 1; i <= 6; ++i) {
        const std::string body = "m" + std::to_string(i);
        h.publish("t", as_bytes(body), delivery_mode::reliable);
    }

    // seq1 is taken but not acknowledged (the response was lost). Front replay
    // must put it back at the head, not at the tail.
    push_message message;
    CRPC_CHECK(h.try_take(connection, std::string_view("t"), message));
    CRPC_CHECK_EQ(message.seq, static_cast<u64>(1));
    CRPC_CHECK(h.retransmit_front(1, "t").ok());
    CRPC_CHECK_EQ(h.stats().retransmits, static_cast<u64>(1));

    for (int i = 1; i <= 6; ++i) {
        CRPC_CHECK(h.try_take(connection, std::string_view("t"), message));
        CRPC_CHECK_EQ(message.seq, static_cast<u64>(i));
        CRPC_CHECK(payload_is(message, "m" + std::to_string(i)));
        CRPC_CHECK(h.ack(1, "t", message.seq).ok());
    }
    CRPC_CHECK_FALSE(h.try_take(connection, std::string_view("t"), message));
    CRPC_CHECK_EQ(h.stats().not_acknowledged, static_cast<u64>(0));
}

CRPC_TEST(hub_front_replay_capacity_insufficient) {
    hub h;
    subscribe_options options;
    options.max_messages = 2;
    options.max_replay = 4;
    // hub::subscribe accepts this combination (tail replay is legal); it is
    // only the long-poll start() that rejects it for front replay.
    CRPC_CHECK(
        h.subscribe("t", 1, delivery_mode::reliable, true, options).ok());
    const auto connection = h.attach(1);
    for (int i = 1; i <= 4; ++i) {
        h.publish("t", as_bytes("x"), delivery_mode::reliable);
    }
    push_message message;
    CRPC_CHECK(h.try_take(connection, std::string_view("t"), message));
    CRPC_CHECK_EQ(message.seq, static_cast<u64>(3));

    // The front replay would need four queue slots but only two exist. This is
    // a safe explicit restart, not a silent skip or a disconnect.
    const status front = h.retransmit_front(1, "t");
    CRPC_CHECK_FALSE(front.ok());
    CRPC_CHECK_EQ(code_id(front.code()), code_id(rpc_code::resource_exhausted));
    CRPC_CHECK_EQ(h.stats().active_subscriptions, static_cast<u64>(1));
    CRPC_CHECK(h.try_take(connection, std::string_view("t"), message));
    CRPC_CHECK_EQ(message.seq, static_cast<u64>(4));
}

// The count bound and the byte bound are independent capacities: satisfying
// `max_messages >= max_replay` does not make the front replay fit. This pins
// the `resource_exhausted` answer for a byte-budget overrun, so the protocol
// documentation's "409 Restart, never a silent drop" promise is tested.
CRPC_TEST(hub_front_replay_byte_budget_insufficient) {
    hub h;
    subscribe_options options;
    options.max_messages = 8;
    options.max_replay = 8;
    options.max_bytes = 2; // exactly two one-byte messages
    CRPC_CHECK(
        h.subscribe("t", 1, delivery_mode::reliable, true, options).ok());
    const auto connection = h.attach(1);
    for (int i = 1; i <= 3; ++i) {
        h.publish("t", as_bytes("x"), delivery_mode::reliable);
    }
    // The byte bound kept only seq2/seq3 in the queue (seq1 was dropped from
    // the front by the overflow policy); the replay window still holds 1..3.
    push_message message;
    CRPC_CHECK(h.try_take(connection, std::string_view("t"), message));
    CRPC_CHECK_EQ(message.seq, static_cast<u64>(2));
    CRPC_CHECK(h.try_take(connection, std::string_view("t"), message));
    CRPC_CHECK_EQ(message.seq, static_cast<u64>(3));

    // Requeueing all three needs three bytes but only two are allowed, and the
    // queued reliable messages cannot be dropped to make room. Explicit
    // restart, never a silent skip.
    const status front = h.retransmit_front(1, "t");
    CRPC_CHECK_FALSE(front.ok());
    CRPC_CHECK_EQ(code_id(front.code()), code_id(rpc_code::resource_exhausted));
    CRPC_CHECK_EQ(h.stats().active_subscriptions, static_cast<u64>(1));
    CRPC_CHECK_FALSE(h.try_take(connection, std::string_view("t"), message));
}

CRPC_TEST(hub_attach_front_replays_at_head) {
    hub h;
    subscribe_options options;
    options.max_messages = 8;
    options.max_replay = 8;
    CRPC_CHECK(
        h.subscribe("t", 1, delivery_mode::reliable, true, options).ok());
    auto first = h.attach(1);
    for (int i = 1; i <= 3; ++i) {
        h.publish("t", as_bytes("x"), delivery_mode::reliable);
    }
    push_message message;
    CRPC_CHECK(h.try_take(first, std::string_view("t"), message));
    CRPC_CHECK_EQ(message.seq, static_cast<u64>(1));
    CRPC_CHECK(h.release(first).ok());

    // The explicit front mode replays the taken sequence before the still
    // queued ones, in sequence order.
    const auto second = h.attach(1, attach_replay::front);
    CRPC_CHECK(second != hub::invalid_connection);
    for (int i = 1; i <= 3; ++i) {
        CRPC_CHECK(h.try_take(second, std::string_view("t"), message));
        CRPC_CHECK_EQ(message.seq, static_cast<u64>(i));
    }
    CRPC_CHECK_FALSE(h.try_take(second, std::string_view("t"), message));
}

CRPC_TEST(hub_budget_counts_only_transmitted) {
    hub h;
    subscribe_options options;
    options.max_messages = 8;
    options.max_replay = 8;
    options.retransmit_budget = 1;
    CRPC_CHECK(
        h.subscribe("t", 1, delivery_mode::reliable, true, options).ok());
    auto first = h.attach(1);
    for (int i = 1; i <= 4; ++i) {
        h.publish("t", as_bytes("x"), delivery_mode::reliable);
    }
    // A keep-queue release/attach of never-transmitted messages is not a
    // retransmission and must not consume the budget.
    CRPC_CHECK(h.release(first).ok());
    const auto second = h.attach(1, attach_replay::none);
    CRPC_CHECK_EQ(h.stats().retransmits, static_cast<u64>(0));

    push_message message;
    CRPC_CHECK(h.try_take(second, std::string_view("t"), message));
    CRPC_CHECK_EQ(message.seq, static_cast<u64>(1));
    CRPC_CHECK(h.retransmit_front(1, "t").ok()); // attempts = 1, budget = 1
    CRPC_CHECK(h.try_take(second, std::string_view("t"), message));
    CRPC_CHECK_EQ(message.seq, static_cast<u64>(1));
    // The second retransmission exceeds the budget: the subscriber is
    // disconnected and the caller is told to restart, not to wait.
    const status over = h.retransmit_front(1, "t");
    CRPC_CHECK_FALSE(over.ok());
    CRPC_CHECK_EQ(code_id(over.code()), code_id(rpc_code::not_acknowledged));
    CRPC_CHECK_EQ(h.stats().not_acknowledged, static_cast<u64>(1));
    CRPC_CHECK_EQ(h.stats().active_subscriptions, static_cast<u64>(0));
}

CRPC_TEST(hub_trimmed_replay_reports_gap) {
    hub h;
    subscribe_options options;
    options.max_messages = 2;
    options.max_replay = 2;
    options.retransmit_budget = 3;
    CRPC_CHECK(
        h.subscribe("t", 1, delivery_mode::reliable, true, options).ok());
    const auto connection = h.attach(1);
    for (int i = 1; i <= 4; ++i) {
        h.publish("t", as_bytes("x"), delivery_mode::reliable);
    }
    // The replay window held only seq3/seq4; seq1/seq2 can no longer be
    // retransmitted. This must be an explicit restart, not a silent skip.
    push_message message;
    CRPC_CHECK(h.try_take(connection, std::string_view("t"), message));
    CRPC_CHECK(h.try_take(connection, std::string_view("t"), message));
    const status gap = h.retransmit_front(1, "t");
    CRPC_CHECK_FALSE(gap.ok());
    CRPC_CHECK_EQ(code_id(gap.code()), code_id(rpc_code::resource_exhausted));
    CRPC_CHECK_EQ(h.stats().active_subscriptions, static_cast<u64>(1));
}

// --- Stream: reader ----------------------------------------------------------

CRPC_TEST(stream_read_some_and_exact) {
    fake_transport transport;
    transport.push_frame("12");
    transport.push_frame("345");
    stream_state state(1, stream_shape::bidirectional);
    stream_reader reader(transport, state);

    std::byte buffer[4] = {};
    auto first = reader.read_some(span<std::byte>(buffer, 2));
    CRPC_CHECK(first.ok());
    CRPC_CHECK_EQ(first.value(), static_cast<std::size_t>(2));
    CRPC_CHECK_EQ(std::memcmp(buffer, "12", 2), 0);

    // The pending payload is drained before the next pull; a partial read must
    // not lose the tail.
    auto second = reader.read_some(span<std::byte>(buffer, 4));
    CRPC_CHECK(second.ok());
    CRPC_CHECK_EQ(second.value(), static_cast<std::size_t>(3));
    CRPC_CHECK_EQ(std::memcmp(buffer, "345", 3), 0);

    auto empty = reader.read_some(span<std::byte>());
    CRPC_CHECK(empty.ok());
    CRPC_CHECK_EQ(empty.value(), static_cast<std::size_t>(0));

    auto none = reader.read_some(span<std::byte>(buffer, 4));
    CRPC_CHECK_FALSE(none.ok());
    CRPC_CHECK_EQ(code_id(none.error().code), code_id(rpc_code::unavailable));

    CRPC_CHECK_EQ(transport.credits_released, static_cast<std::size_t>(5));

    // read_exact spans frame boundaries.
    fake_transport exact_transport;
    exact_transport.push_frame("hel");
    exact_transport.push_frame("lo");
    stream_state exact_state(2, stream_shape::bidirectional);
    stream_reader exact_reader(exact_transport, exact_state);
    char text[6] = {};
    CRPC_CHECK(exact_reader.read_exact(
                   span<std::byte>(reinterpret_cast<std::byte*>(text), 5))
                   .ok());
    CRPC_CHECK_EQ(std::string(text, 5), std::string("hello"));
}

CRPC_TEST(stream_eof_vs_canceled) {
    fake_transport eof_transport;
    stream_state eof_state(1, stream_shape::server_streaming);
    stream_reader eof_reader(eof_transport, eof_state);
    eof_transport.at_eof = true;
    std::byte buffer[4] = {};
    auto eof = eof_reader.read_some(span<std::byte>(buffer, 4));
    CRPC_CHECK_FALSE(eof.ok());
    CRPC_CHECK_EQ(code_id(eof.error().code), code_id(rpc_code::eof));
    CRPC_CHECK(eof_reader.eof());

    fake_transport canceled_transport;
    stream_state canceled_state(2, stream_shape::server_streaming);
    stream_reader canceled_reader(canceled_transport, canceled_state);
    canceled_transport.canceled_flag = true;
    canceled_transport.cancel_code = rpc_code::deadline_exceeded;
    auto canceled = canceled_reader.read_some(span<std::byte>(buffer, 4));
    CRPC_CHECK_FALSE(canceled.ok());
    CRPC_CHECK_EQ(code_id(canceled.error().code),
                  code_id(rpc_code::deadline_exceeded));
    CRPC_CHECK(canceled_reader.canceled());
}

// --- Stream: writer ----------------------------------------------------------

CRPC_TEST(stream_writer_backpressure) {
    fake_transport transport;
    transport.credit = 3;
    stream_state state(1, stream_shape::bidirectional);
    stream_writer writer(transport, state);

    auto accepted = writer.write(as_bytes("0123456789"));
    CRPC_CHECK(accepted.ok());
    CRPC_CHECK_EQ(accepted.value(), static_cast<std::size_t>(3));

    transport.credit = 0;
    auto stalled = writer.write(as_bytes("abcdefg"));
    CRPC_CHECK(stalled.ok());
    CRPC_CHECK_EQ(stalled.value(), static_cast<std::size_t>(0));

    transport.credit = 100;
    auto resumed = writer.write(as_bytes("abcdefg"));
    CRPC_CHECK(resumed.ok());
    CRPC_CHECK_EQ(resumed.value(), static_cast<std::size_t>(7));
    CRPC_CHECK_EQ(sent_text(transport), std::string("012abcdefg"));

    // A short transport write is reported verbatim so the caller retries the
    // tail.
    fake_transport partial;
    partial.credit = 100;
    partial.max_send = 4;
    stream_state partial_state(2, stream_shape::bidirectional);
    stream_writer partial_writer(partial, partial_state);
    auto short_write = partial_writer.write(as_bytes("0123456789"));
    CRPC_CHECK(short_write.ok());
    CRPC_CHECK_EQ(short_write.value(), static_cast<std::size_t>(4));
}

CRPC_TEST(stream_reset_idempotent) {
    fake_transport transport;
    stream_state state(1, stream_shape::bidirectional);
    stream_writer writer(transport, state);
    stream_reader reader(transport, state);

    CRPC_CHECK(writer.reset(rpc_code::canceled).ok());
    CRPC_CHECK_EQ(transport.resets, static_cast<std::size_t>(1));
    CRPC_CHECK(writer.reset(rpc_code::canceled).ok());
    CRPC_CHECK_EQ(transport.resets, static_cast<std::size_t>(1));

    std::byte buffer[4] = {};
    auto read = reader.read_some(span<std::byte>(buffer, 4));
    CRPC_CHECK_FALSE(read.ok());
    CRPC_CHECK_EQ(code_id(read.error().code), code_id(rpc_code::canceled));

    auto write = writer.write(as_bytes("x"));
    CRPC_CHECK_FALSE(write.ok());
    CRPC_CHECK_EQ(code_id(write.error().code), code_id(rpc_code::canceled));

    CRPC_CHECK(writer.close().ok());
    CRPC_CHECK_EQ(transport.end_sends, static_cast<std::size_t>(0));
}

CRPC_TEST(stream_illegal_transitions) {
    fake_transport transport;
    stream_state state(1, stream_shape::client_streaming);
    stream_reader reader(transport, state);
    stream_writer writer(transport, state);

    // Client streaming: the response cannot be read before the request stream
    // is closed.
    std::byte buffer[4] = {};
    auto early = reader.read_some(span<std::byte>(buffer, 4));
    CRPC_CHECK_FALSE(early.ok());
    CRPC_CHECK_EQ(code_id(early.error().code),
                  code_id(rpc_code::failed_precondition));

    CRPC_CHECK(writer.close().ok());
    CRPC_CHECK_EQ(transport.end_sends, static_cast<std::size_t>(1));
    CRPC_CHECK(writer.close().ok()); // idempotent
    CRPC_CHECK_EQ(transport.end_sends, static_cast<std::size_t>(1));

    auto write_after_close = writer.write(as_bytes("x"));
    CRPC_CHECK_FALSE(write_after_close.ok());
    CRPC_CHECK_EQ(code_id(write_after_close.error().code),
                  code_id(rpc_code::failed_precondition));

    // After close the read side is legal and currently has no frame.
    auto after_close = reader.read_some(span<std::byte>(buffer, 4));
    CRPC_CHECK_FALSE(after_close.ok());
    CRPC_CHECK_EQ(code_id(after_close.error().code),
                  code_id(rpc_code::unavailable));

    // A close that the transport cannot enqueue reports backpressure and stays
    // open.
    fake_transport full_transport;
    full_transport.end_full = true;
    stream_state full_state(2, stream_shape::bidirectional);
    stream_writer full_writer(full_transport, full_state);
    auto blocked_close = full_writer.close();
    CRPC_CHECK_FALSE(blocked_close.ok());
    CRPC_CHECK_EQ(code_id(blocked_close.code()),
                  code_id(rpc_code::resource_exhausted));
    CRPC_CHECK(full_writer.write(as_bytes("ok")).ok()); // still open
}

CRPC_TEST(stream_shapes_state_machine) {
    stream_state client_streaming(1, stream_shape::client_streaming);
    CRPC_CHECK_FALSE(client_streaming.can_read());
    CRPC_CHECK(client_streaming.can_write());
    client_streaming.mark_local_close();
    CRPC_CHECK(client_streaming.can_read());
    CRPC_CHECK_FALSE(client_streaming.can_write());

    stream_state server_streaming(2, stream_shape::server_streaming);
    CRPC_CHECK(server_streaming.can_read());
    CRPC_CHECK(server_streaming.can_write());
    server_streaming.mark_remote_eof();
    CRPC_CHECK(server_streaming.remote_closed());

    stream_state bidirectional(3, stream_shape::bidirectional);
    CRPC_CHECK(bidirectional.can_read());
    CRPC_CHECK(bidirectional.can_write());
    bidirectional.mark_reset(rpc_code::canceled);
    CRPC_CHECK(bidirectional.canceled());
    CRPC_CHECK_FALSE(bidirectional.can_read());
    CRPC_CHECK_FALSE(bidirectional.can_write());
    CRPC_CHECK_EQ(code_id(bidirectional.terminal_code()),
                  code_id(rpc_code::canceled));
}

// `stats()` is O(1): the four derived fields are maintained incrementally at
// every mutation point. This case drives each path and compares the aggregates
// against the value that a full scan of the subscription table would produce.
// A missed mutation point (an unaccounted push/pop/erase/clear/subscribe/
// unsubscribe) shows up here as a wrong count.
CRPC_TEST(hub_stats_aggregates_track_every_mutation) {
    hub h;
    subscribe_options opts;
    opts.max_messages = 2;
    opts.on_overflow = overflow_policy::drop_oldest;

    // --- A: push_back, overflow pop, ack erase, take pop -------------------
    CRPC_CHECK(h.subscribe("a", 1, delivery_mode::reliable, false, opts).ok());
    const hub::connection_id c1 = h.attach(1);
    CRPC_CHECK_EQ(h.stats().active_subscriptions, static_cast<u64>(1));
    CRPC_CHECK_EQ(h.stats().active_queues, static_cast<u64>(0));
    CRPC_CHECK_EQ(h.stats().queued_messages, static_cast<u64>(0));
    CRPC_CHECK_EQ(h.stats().queued_bytes, static_cast<u64>(0));

    h.publish("a", as_bytes("1234"), delivery_mode::reliable);
    CRPC_CHECK_EQ(h.stats().active_queues, static_cast<u64>(1));
    CRPC_CHECK_EQ(h.stats().queued_messages, static_cast<u64>(1));
    CRPC_CHECK_EQ(h.stats().queued_bytes, static_cast<u64>(4));

    h.publish("a", as_bytes("5678"), delivery_mode::reliable);
    CRPC_CHECK_EQ(h.stats().queued_messages, static_cast<u64>(2));
    CRPC_CHECK_EQ(h.stats().queued_bytes, static_cast<u64>(8));

    // Overflow drop_oldest pops the front: the count stays bounded.
    h.publish("a", as_bytes("9012"), delivery_mode::reliable);
    CRPC_CHECK_EQ(h.stats().queued_messages, static_cast<u64>(2));
    CRPC_CHECK_EQ(h.stats().queued_bytes, static_cast<u64>(8));
    CRPC_CHECK_EQ(h.stats().active_queues, static_cast<u64>(1));

    // Cumulative ack erases the queued reliable run.
    CRPC_CHECK(h.ack(1, "a", 3).ok());
    CRPC_CHECK_EQ(h.stats().queued_messages, static_cast<u64>(0));
    CRPC_CHECK_EQ(h.stats().queued_bytes, static_cast<u64>(0));
    CRPC_CHECK_EQ(h.stats().active_queues, static_cast<u64>(0));

    // take pops the front.
    h.publish("a", as_bytes("pq"), delivery_mode::reliable);
    CRPC_CHECK_EQ(h.stats().queued_messages, static_cast<u64>(1));
    CRPC_CHECK_EQ(h.stats().queued_bytes, static_cast<u64>(2));
    push_message taken;
    CRPC_CHECK(h.try_take(c1, std::string_view("a"), taken));
    CRPC_CHECK_EQ(h.stats().queued_messages, static_cast<u64>(0));
    CRPC_CHECK_EQ(h.stats().queued_bytes, static_cast<u64>(0));

    // --- B: front replay push_front ----------------------------------------
    // Two unacknowledged entries survive a detach in the replay window and are
    // re-queued at the front by attach(front).
    h.publish("a", as_bytes("pq"), delivery_mode::reliable); // seq 5, unacked
    CRPC_CHECK_EQ(h.stats().queued_messages, static_cast<u64>(1));
    CRPC_CHECK_EQ(h.stats().queued_bytes, static_cast<u64>(2));
    CRPC_CHECK(h.detach(c1).ok());
    CRPC_CHECK_EQ(h.stats().queued_messages, static_cast<u64>(0));
    CRPC_CHECK_EQ(h.stats().active_queues, static_cast<u64>(0));
    const hub::connection_id c1b = h.attach(1, attach_replay::front);
    CRPC_CHECK(c1b != hub::invalid_connection);
    CRPC_CHECK_EQ(h.stats().queued_messages, static_cast<u64>(2));
    CRPC_CHECK_EQ(h.stats().queued_bytes, static_cast<u64>(4));
    CRPC_CHECK_EQ(h.stats().active_queues, static_cast<u64>(1));

    // --- C: detach clears, release keeps -----------------------------------
    CRPC_CHECK(h.subscribe("b", 2, delivery_mode::reliable, false, opts).ok());
    const hub::connection_id c2 = h.attach(2);
    h.publish("b", as_bytes("xyz"), delivery_mode::reliable);
    CRPC_CHECK_EQ(h.stats().active_subscriptions, static_cast<u64>(2));
    CRPC_CHECK_EQ(h.stats().queued_messages, static_cast<u64>(3));
    CRPC_CHECK_EQ(h.stats().queued_bytes, static_cast<u64>(7));
    CRPC_CHECK(h.detach(c2).ok());
    CRPC_CHECK_EQ(h.stats().queued_messages, static_cast<u64>(2));
    CRPC_CHECK_EQ(h.stats().queued_bytes, static_cast<u64>(4));
    const hub::connection_id c2b = h.attach(2, attach_replay::none);
    h.publish("b", as_bytes("abc"), delivery_mode::reliable);
    CRPC_CHECK_EQ(h.stats().queued_messages, static_cast<u64>(3));
    CRPC_CHECK_EQ(h.stats().queued_bytes, static_cast<u64>(7));
    CRPC_CHECK(h.release(c2b).ok());
    CRPC_CHECK_EQ(h.stats().queued_messages, static_cast<u64>(3));
    CRPC_CHECK_EQ(h.stats().queued_bytes, static_cast<u64>(7));

    // --- D: unsubscribe ------------------------------------------------------
    CRPC_CHECK(h.unsubscribe("b", 2).ok());
    CRPC_CHECK_EQ(h.stats().active_subscriptions, static_cast<u64>(1));
    CRPC_CHECK_EQ(h.stats().queued_messages, static_cast<u64>(2));
    CRPC_CHECK_EQ(h.stats().queued_bytes, static_cast<u64>(4));
    CRPC_CHECK(h.unsubscribe("a", 1).ok());
    CRPC_CHECK_EQ(h.stats().active_subscriptions, static_cast<u64>(0));
    CRPC_CHECK_EQ(h.stats().queued_messages, static_cast<u64>(0));
    CRPC_CHECK_EQ(h.stats().queued_bytes, static_cast<u64>(0));

    // --- E: ordered gap clear_queue, unsubscribe_all ------------------------
    CRPC_CHECK(h.subscribe("g", 7, delivery_mode::reliable, true, opts).ok());
    CRPC_CHECK(h.subscribe("h", 9, delivery_mode::reliable, false, opts).ok());
    publish_options first;
    first.has_topic_seq = true;
    first.topic_seq = 1;
    h.publish("g", as_bytes("one"), delivery_mode::reliable, first);
    CRPC_CHECK_EQ(h.stats().active_subscriptions, static_cast<u64>(2));
    CRPC_CHECK_EQ(h.stats().queued_messages, static_cast<u64>(1));
    publish_options gap;
    gap.has_topic_seq = true;
    gap.topic_seq = 3;
    h.publish("g", as_bytes("two"), delivery_mode::reliable, gap);
    // The gap cleared "g"'s queue, then the new message was queued.
    CRPC_CHECK_EQ(h.stats().queued_messages, static_cast<u64>(1));
    CRPC_CHECK_EQ(h.stats().active_queues, static_cast<u64>(1));
    CRPC_CHECK(h.unsubscribe_all(7).ok());
    CRPC_CHECK_EQ(h.stats().active_subscriptions, static_cast<u64>(1));
    CRPC_CHECK_EQ(h.stats().queued_messages, static_cast<u64>(0));

    // --- F: disconnect_subscriber ------------------------------------------
    hub d;
    subscribe_options drop;
    drop.max_messages = 1;
    drop.on_overflow = overflow_policy::disconnect_subscriber;
    CRPC_CHECK(d.subscribe("d", 8, delivery_mode::reliable, false, drop).ok());
    const hub::connection_id c8 = d.attach(8);
    CRPC_CHECK(c8 != hub::invalid_connection);
    d.publish("d", as_bytes("1"), delivery_mode::reliable);
    CRPC_CHECK_EQ(d.stats().active_subscriptions, static_cast<u64>(1));
    CRPC_CHECK_EQ(d.stats().queued_messages, static_cast<u64>(1));
    d.publish("d", as_bytes("2"), delivery_mode::reliable);
    CRPC_CHECK_EQ(d.stats().active_subscriptions, static_cast<u64>(0));
    CRPC_CHECK_EQ(d.stats().active_queues, static_cast<u64>(0));
    CRPC_CHECK_EQ(d.stats().queued_messages, static_cast<u64>(0));
    CRPC_CHECK_EQ(d.stats().queued_bytes, static_cast<u64>(0));
}

CRPC_TEST_MAIN()
