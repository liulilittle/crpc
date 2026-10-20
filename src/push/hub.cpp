#include <crpc/push/hub.hpp>

#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

namespace crpc::push {
namespace {

constexpr u64 k_fnv_offset = 14695981039346656037ull;
constexpr u64 k_fnv_prime = 1099511628211ull;

u64 fnv1a(std::string_view text) noexcept {
    u64 hash = k_fnv_offset;
    for (char ch : text) {
        hash ^= static_cast<u64>(static_cast<unsigned char>(ch));
        hash *= k_fnv_prime;
    }
    return hash;
}

u64 mix_key(u64 hash, u64 key) noexcept {
    hash ^= key + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    return hash;
}

struct queued_message {
    hub::connection_id target = hub::invalid_connection;
    u64 publisher_id = 0;
    u64 epoch = 0;
    u64 seq = 0;
    u64 topic_seq = 0;
    delivery_mode mode = delivery_mode::unreliable;
    push_payload_ptr payload;
    std::size_t bytes = 0;
};

struct replay_entry {
    u64 seq = 0;
    u64 topic_seq = 0;
    u64 publisher_id = 0;
    u64 epoch = 1;
    push_payload_ptr payload;
    std::size_t bytes = 0;
    u32 attempts = 0;
    bool in_queue = false;
    // True once the message was handed to a connection at least once. Only an
    // actual retransmission of a transmitted message counts against the
    // retransmission budget; re-queuing a message that never left the queue is
    // not a retransmission.
    bool transmitted = false;
};

struct subscription_state {
    subscriber_id id = 0;
    std::string topic;
    delivery_mode mode = delivery_mode::unreliable;
    bool ordered = false;
    subscribe_options options{};
    std::deque<queued_message> queue;
    std::size_t queued_bytes = 0;
    std::deque<replay_entry> replay;
    u64 next_seq = 1;
    u64 acked_upto = 0;
    bool expected_valid = false;
    u64 expected_topic_seq = 0;
    bool high_water = false;
    u64 high_water_since_ns = 0;
};

struct subscriber_state {
    subscriber_id id = 0;
    hub::connection_id active = hub::invalid_connection;
    std::map<std::string, subscription_state, std::less<>> subscriptions;
};

struct topic_state {
    u64 shard_key = 0;
    bool declared = false;
    u64 next_topic_seq = 1;
    std::vector<subscriber_id> subscribers;
};

} // namespace

u32 topic_shard(std::string_view topic, u64 shard_key, u32 shard_count) noexcept {
    if (shard_count <= 1) {
        return 0;
    }
    u64 hash = fnv1a(topic);
    if (shard_key != 0) {
        hash = mix_key(hash, shard_key);
    }
    return static_cast<u32>(hash % static_cast<u64>(shard_count));
}

struct hub::impl {
    hub_options options;
    u64 now_ns = 0;
    hub::connection_id next_connection = 1;
    hub_stats counters;
    std::map<std::string, topic_state, std::less<>> topics;
    std::unordered_map<subscriber_id, subscriber_state> subscribers;
    std::unordered_map<hub::connection_id, subscriber_id> connections;
    std::deque<hub_event> events;

    [[nodiscard]] topic_state* find_topic(std::string_view topic) noexcept {
        auto it = topics.find(topic);
        return it == topics.end() ? nullptr : &it->second;
    }

    [[nodiscard]] const topic_state* find_topic(std::string_view topic) const noexcept {
        auto it = topics.find(topic);
        return it == topics.end() ? nullptr : &it->second;
    }

    void remove_topic_subscriber(std::string_view topic, subscriber_id id) noexcept {
        topic_state* ts = find_topic(topic);
        if (ts == nullptr) {
            return;
        }
        auto& list = ts->subscribers;
        for (auto it = list.begin(); it != list.end(); ++it) {
            if (*it == id) {
                list.erase(it);
                return;
            }
        }
    }

    void push_event(hub_event_kind kind, subscriber_id id, std::string_view topic,
                    rpc_code code, u64 seq, u64 detail) noexcept {
        try {
            hub_event event;
            event.kind = kind;
            event.subscriber = id;
            event.topic.assign(topic.data(), topic.size());
            event.code = code;
            event.seq = seq;
            event.detail = detail;
            events.push_back(std::move(event));
            const std::size_t cap = options.max_events == 0 ? 1 : options.max_events;
            while (events.size() > cap) {
                events.pop_front();
                ++counters.events_dropped;
            }
        } catch (...) {
            ++counters.events_dropped;
        }
    }

    void mark_replay_in_queue(subscription_state& sub, u64 seq, bool in_queue) noexcept {
        for (auto& entry : sub.replay) {
            if (entry.seq == seq) {
                entry.in_queue = in_queue;
                return;
            }
        }
    }

    void mark_replay_transmitted(subscription_state& sub, u64 seq) noexcept {
        for (auto& entry : sub.replay) {
            if (entry.seq == seq) {
                entry.in_queue = false;
                entry.transmitted = true;
                return;
            }
        }
    }

    void drop_queue_front(subscription_state& sub) noexcept {
        if (sub.queue.empty()) {
            return;
        }
        queued_message& front = sub.queue.front();
        if (front.mode == delivery_mode::reliable) {
            mark_replay_in_queue(sub, front.seq, false);
        }
        sub.queued_bytes -= front.bytes;
        sub.queue.pop_front();
    }

    void clear_queue(subscription_state& sub) noexcept {
        for (queued_message& message : sub.queue) {
            if (message.mode == delivery_mode::reliable) {
                mark_replay_in_queue(sub, message.seq, false);
            }
        }
        sub.queue.clear();
        sub.queued_bytes = 0;
        sub.high_water = false;
        sub.high_water_since_ns = 0;
    }

    void note_high_water(subscription_state& sub) noexcept {
        if (options.slow_watermark_bytes != 0 &&
            sub.queued_bytes > options.slow_watermark_bytes && !sub.high_water) {
            sub.high_water = true;
            sub.high_water_since_ns = now_ns;
        }
    }

    void disconnect_subscriber(subscriber_id id, hub_event_kind kind, rpc_code code,
                               std::string_view topic) noexcept {
        auto it = subscribers.find(id);
        if (it == subscribers.end()) {
            return;
        }
        for (auto& kv : it->second.subscriptions) {
            remove_topic_subscriber(kv.first, id);
            counters.dropped_disconnected += static_cast<u64>(kv.second.queue.size());
        }
        if (it->second.active != hub::invalid_connection) {
            connections.erase(it->second.active);
        }
        subscribers.erase(it);
        if (kind == hub_event_kind::slow_subscriber) {
            ++counters.slow_disconnects;
        }
        if (code == rpc_code::not_acknowledged) {
            ++counters.not_acknowledged;
        }
        push_event(kind, id, topic, code, 0, 0);
    }

    [[nodiscard]] bool requeue(subscriber_state& state, subscription_state& sub,
                               replay_entry& entry) noexcept {
        const std::size_t max_messages =
            sub.options.max_messages == 0 ? 1 : sub.options.max_messages;
        const std::size_t max_bytes =
            sub.options.max_bytes == 0 ? 1 : sub.options.max_bytes;
        if (sub.queue.size() >= max_messages ||
            sub.queued_bytes + entry.bytes > max_bytes) {
            if (sub.options.on_overflow == overflow_policy::drop_oldest) {
                while (!sub.queue.empty() &&
                       (sub.queue.size() >= max_messages ||
                        sub.queued_bytes + entry.bytes > max_bytes)) {
                    drop_queue_front(sub);
                    ++counters.dropped_overflow;
                }
            }
            if (sub.queue.size() >= max_messages ||
                sub.queued_bytes + entry.bytes > max_bytes) {
                return false;
            }
        }

        queued_message message;
        message.target = state.active;
        message.publisher_id = entry.publisher_id;
        message.epoch = entry.epoch;
        message.seq = entry.seq;
        message.topic_seq = entry.topic_seq;
        message.mode = delivery_mode::reliable;
        message.payload = entry.payload;
        message.bytes = entry.bytes;
        sub.queue.push_back(std::move(message));
        sub.queued_bytes += entry.bytes;
        entry.in_queue = true;
        if (entry.transmitted) {
            ++entry.attempts;
            ++counters.retransmits;
        }
        note_high_water(sub);
        return true;
    }

    void replay_unacked(subscriber_state& state, std::string_view only_topic,
                        bool has_topic) noexcept {
        for (auto& kv : state.subscriptions) {
            if (has_topic && kv.first != only_topic) {
                continue;
            }
            subscription_state& sub = kv.second;
            if (sub.mode != delivery_mode::reliable) {
                continue;
            }
            for (std::size_t index = 0; index < sub.replay.size(); ++index) {
                replay_entry& entry = sub.replay[index];
                if (entry.seq <= sub.acked_upto || entry.in_queue) {
                    continue;
                }
                if (entry.transmitted &&
                    entry.attempts >= sub.options.retransmit_budget) {
                    disconnect_subscriber(state.id, hub_event_kind::not_acknowledged,
                                          rpc_code::not_acknowledged, kv.first);
                    return;
                }
                if (!requeue(state, sub, entry)) {
                    break;
                }
            }
        }
    }

    [[nodiscard]] bool deliver(subscriber_state& state, std::string_view topic,
                               const push_payload_ptr& block, std::size_t bytes,
                               delivery_mode mode, u64 topic_seq,
                               const publish_options& publish_opts) noexcept {
        auto it = state.subscriptions.find(topic);
        if (it == state.subscriptions.end()) {
            return false;
        }
        subscription_state& sub = it->second;

        if (sub.ordered) {
            if (!sub.expected_valid) {
                sub.expected_valid = true;
                sub.expected_topic_seq = topic_seq + 1;
            } else if (topic_seq == sub.expected_topic_seq) {
                ++sub.expected_topic_seq;
            } else if (topic_seq > sub.expected_topic_seq) {
                ++counters.ordered_gaps;
                push_event(hub_event_kind::ordered_gap, state.id, topic,
                           rpc_code::failed_precondition, topic_seq,
                           sub.expected_topic_seq);
                clear_queue(sub);
                sub.expected_topic_seq = topic_seq + 1;
            } else {
                ++counters.ordered_duplicates;
                push_event(hub_event_kind::ordered_gap, state.id, topic,
                           rpc_code::failed_precondition, topic_seq,
                           sub.expected_topic_seq);
                return false;
            }
        }

        const bool reliable = sub.mode == delivery_mode::reliable &&
                              mode == delivery_mode::reliable;
        const std::size_t max_messages =
            sub.options.max_messages == 0 ? 1 : sub.options.max_messages;
        const std::size_t max_bytes =
            sub.options.max_bytes == 0 ? 1 : sub.options.max_bytes;

        if (sub.queue.size() >= max_messages ||
            sub.queued_bytes + bytes > max_bytes) {
            if (sub.options.on_overflow == overflow_policy::disconnect_subscriber) {
                disconnect_subscriber(state.id, hub_event_kind::disconnected,
                                      rpc_code::resource_exhausted, topic);
                return true;
            }
            if (sub.options.on_overflow == overflow_policy::drop_newest) {
                ++counters.dropped_overflow;
                if (!reliable) {
                    ++counters.dropped_unreliable;
                }
                push_event(hub_event_kind::overflow, state.id, topic,
                           rpc_code::resource_exhausted, topic_seq, bytes);
                return false;
            }
            while (!sub.queue.empty() &&
                   (sub.queue.size() >= max_messages ||
                    sub.queued_bytes + bytes > max_bytes)) {
                const delivery_mode dropped_mode = sub.queue.front().mode;
                drop_queue_front(sub);
                ++counters.dropped_overflow;
                if (dropped_mode != delivery_mode::reliable) {
                    ++counters.dropped_unreliable;
                }
            }
            push_event(hub_event_kind::overflow, state.id, topic,
                       rpc_code::resource_exhausted, topic_seq, bytes);
            if (sub.queue.size() >= max_messages ||
                sub.queued_bytes + bytes > max_bytes) {
                ++counters.dropped_overflow;
                if (!reliable) {
                    ++counters.dropped_unreliable;
                }
                return false;
            }
        }

        queued_message message;
        message.target = state.active;
        message.publisher_id =
            publish_opts.publisher_id != 0 ? publish_opts.publisher_id : options.publisher_id;
        message.epoch = publish_opts.epoch != 0 ? publish_opts.epoch : options.epoch;
        message.topic_seq = topic_seq;
        message.payload = block;
        message.bytes = bytes;
        message.mode = reliable ? delivery_mode::reliable : delivery_mode::unreliable;

        if (reliable) {
            message.seq = sub.next_seq;
            ++sub.next_seq;
        }
        const u64 assigned_seq = message.seq;

        if (sub.mode == delivery_mode::reliable && reliable) {
            const std::size_t max_replay =
                sub.options.max_replay == 0 ? 1 : sub.options.max_replay;
            if (sub.replay.size() >= max_replay) {
                sub.replay.pop_front();
                ++counters.dropped_overflow;
            }
            replay_entry entry;
            entry.seq = assigned_seq;
            entry.topic_seq = topic_seq;
            entry.publisher_id = message.publisher_id;
            entry.epoch = message.epoch;
            entry.payload = block;
            entry.bytes = bytes;
            entry.in_queue = true;
            sub.replay.push_back(std::move(entry));
        }

        sub.queue.push_back(std::move(message));
        sub.queued_bytes += bytes;
        note_high_water(sub);
        return false;
    }

    // Front-inserts the unacknowledged, reliable replay entries of one
    // subscription in sequence order. The replay window is checked first: a
    // missing sequence means the window was trimmed and the caller must restart
    // the subscription (resource_exhausted) rather than skip data.
    [[nodiscard]] status retransmit_front_sub(const subscriber_state& state,
                                              subscription_state& sub) noexcept {
        if (sub.mode != delivery_mode::reliable) {
            return status{};
        }
        const std::size_t count_capacity =
            sub.options.max_messages == 0 ? 1 : sub.options.max_messages;
        const std::size_t byte_capacity =
            sub.options.max_bytes == 0 ? 1 : sub.options.max_bytes;
        const std::size_t budget = sub.options.retransmit_budget;

        // A complete replay window contains every assigned sequence above the
        // acknowledged one. Entries are appended in sequence order and are only
        // popped after an ack (from the front) or by max_replay trimming (also
        // from the front), so the first unacknowledged sequence must be the
        // first surviving entry.
        u64 expected = sub.acked_upto + 1;
        bool gap = false;
        std::size_t needed = 0;
        std::size_t needed_bytes = 0;
        for (const replay_entry& entry : sub.replay) {
            if (entry.seq <= sub.acked_upto) {
                continue;
            }
            if (entry.seq != expected) {
                gap = true;
                break;
            }
            ++expected;
            if (!entry.in_queue) {
                if (entry.transmitted && entry.attempts >= budget) {
                    return status(rpc_error(rpc_code::not_acknowledged,
                                            "retransmission budget exhausted"));
                }
                ++needed;
                needed_bytes += entry.bytes;
            }
        }
        if (gap) {
            return status(rpc_error(rpc_code::resource_exhausted,
                                    "replay window gap"));
        }
        // Every assigned sequence is present iff the expected successor equals
        // next_seq. A gap here means the window was trimmed at the front.
        if (sub.next_seq > sub.acked_upto + 1 &&
            expected != sub.next_seq) {
            return status(rpc_error(rpc_code::resource_exhausted,
                                    "replay window gap"));
        }
        if (needed == 0) {
            return status{};
        }

        // Make room by dropping queued UNRELIABLE messages only. A reliable
        // queued message is part of the contiguous unacknowledged run; dropping
        // it here would let a later sequence reach the consumer first. With
        // `max_replay >= max_messages` (a long-poll option invariant) the
        // reliable run always fits, so a reliable overflow is a restart.
        if (sub.queue.size() + needed > count_capacity ||
            sub.queued_bytes + needed_bytes > byte_capacity) {
            for (auto it = sub.queue.begin();
                 it != sub.queue.end() &&
                 (sub.queue.size() + needed > count_capacity ||
                  sub.queued_bytes + needed_bytes > byte_capacity);) {
                if (it->mode == delivery_mode::reliable) {
                    ++it;
                    continue;
                }
                sub.queued_bytes -= it->bytes;
                it = sub.queue.erase(it);
                ++counters.dropped_overflow;
            }
        }
        if (sub.queue.size() + needed > count_capacity ||
            sub.queued_bytes + needed_bytes > byte_capacity) {
            return status(rpc_error(rpc_code::resource_exhausted,
                                    "queue cannot hold the replay window"));
        }

        for (std::size_t index = sub.replay.size(); index > 0; --index) {
            replay_entry& entry = sub.replay[index - 1];
            if (entry.seq <= sub.acked_upto || entry.in_queue) {
                continue;
            }
            queued_message message;
            message.target = state.active;
            message.publisher_id = entry.publisher_id;
            message.epoch = entry.epoch;
            message.seq = entry.seq;
            message.topic_seq = entry.topic_seq;
            message.mode = delivery_mode::reliable;
            message.payload = entry.payload;
            message.bytes = entry.bytes;
            sub.queue.push_front(std::move(message));
            sub.queued_bytes += entry.bytes;
            entry.in_queue = true;
            if (entry.transmitted) {
                ++entry.attempts;
                ++counters.retransmits;
            }
        }
        note_high_water(sub);
        return status{};
    }

    [[nodiscard]] bool take_from(hub::connection_id connection,
                                 subscription_state& sub,
                                 push_message& out) noexcept {
        if (sub.queue.empty()) {
            return false;
        }
        queued_message& front = sub.queue.front();
        if (front.target != connection) {
            return false;
        }
        try {
            out.topic = sub.topic;
        } catch (...) {
            return false;
        }
        out.publisher_id = front.publisher_id;
        out.epoch = front.epoch;
        out.seq = front.seq;
        out.topic_seq = front.topic_seq;
        out.mode = front.mode;
        out.payload = front.payload;

        const std::size_t bytes = front.bytes;
        const bool was_reliable = front.mode == delivery_mode::reliable;
        const u64 seq = front.seq;
        sub.queued_bytes -= bytes;
        sub.queue.pop_front();
        ++counters.delivered;
        if (was_reliable) {
            mark_replay_transmitted(sub, seq);
        }
        if (sub.queue.empty() || sub.queued_bytes <= options.slow_watermark_bytes) {
            sub.high_water = false;
            sub.high_water_since_ns = 0;
        }
        return true;
    }

    [[nodiscard]] bool take_any(hub::connection_id connection,
                                push_message& out) noexcept {
        auto cit = connections.find(connection);
        if (cit == connections.end()) {
            return false;
        }
        auto sit = subscribers.find(cit->second);
        if (sit == subscribers.end()) {
            return false;
        }
        for (auto& kv : sit->second.subscriptions) {
            if (!kv.second.queue.empty() &&
                kv.second.queue.front().target == connection) {
                return take_from(connection, kv.second, out);
            }
        }
        return false;
    }

    [[nodiscard]] bool take_topic(hub::connection_id connection,
                                  std::string_view topic,
                                  push_message& out) noexcept {
        auto cit = connections.find(connection);
        if (cit == connections.end()) {
            return false;
        }
        auto sit = subscribers.find(cit->second);
        if (sit == subscribers.end()) {
            return false;
        }
        auto sub_it = sit->second.subscriptions.find(topic);
        if (sub_it == sit->second.subscriptions.end()) {
            return false;
        }
        return take_from(connection, sub_it->second, out);
    }

    status subscribe(std::string_view topic, subscriber_id id, delivery_mode mode,
                     bool ordered, const subscribe_options& opts) {
        try {
            subscriber_state& state = subscribers[id];
            state.id = id;
            if (state.subscriptions.find(topic) != state.subscriptions.end()) {
                return status(rpc_error(rpc_code::failed_precondition,
                                        "already subscribed"));
            }
            subscription_state sub;
            sub.id = id;
            sub.topic.assign(topic.data(), topic.size());
            sub.mode = mode;
            sub.ordered = ordered;
            sub.options = opts;
            if (sub.options.max_messages == 0) {
                sub.options.max_messages = 1;
            }
            if (sub.options.max_bytes == 0) {
                sub.options.max_bytes = 1;
            }
            if (sub.options.max_replay == 0) {
                sub.options.max_replay = 1;
            }
            auto inserted = state.subscriptions.emplace(sub.topic, std::move(sub));
            if (!inserted.second) {
                return status(rpc_error(rpc_code::internal, "subscribe insert failed"));
            }
            topic_state& ts = topics[inserted.first->first];
            ts.subscribers.push_back(id);
            return status{};
        } catch (...) {
            return status(rpc_code::resource_exhausted);
        }
    }

    [[nodiscard]] hub::connection_id attach(subscriber_id id,
                                            attach_replay mode) noexcept {
        try {
            const hub::connection_id cid = next_connection++;
            subscriber_state& state = subscribers[id];
            state.id = id;
            const hub::connection_id previous = state.active;
            if (previous != hub::invalid_connection) {
                connections.erase(previous);
            }
            state.active = cid;
            connections[cid] = id;
            for (auto& kv : state.subscriptions) {
                for (queued_message& message : kv.second.queue) {
                    message.target = cid;
                }
            }
            if (previous == hub::invalid_connection) {
                if (mode == attach_replay::tail) {
                    replay_unacked(state, std::string_view{}, false);
                } else if (mode == attach_replay::front) {
                    for (auto& kv : state.subscriptions) {
                        const status replayed =
                            retransmit_front_sub(state, kv.second);
                        if (replayed.code() == rpc_code::not_acknowledged) {
                            // A budget overrun disconnects the subscriber; the
                            // token handed out above is stale. Copy the topic:
                            // the disconnect erases the subscription state.
                            const std::string topic_copy = kv.first;
                            disconnect_subscriber(
                                id, hub_event_kind::not_acknowledged,
                                rpc_code::not_acknowledged, topic_copy);
                            return hub::invalid_connection;
                        }
                    }
                }
            }
            return cid;
        } catch (...) {
            return hub::invalid_connection;
        }
    }

    status detach(hub::connection_id connection) noexcept {
        auto cit = connections.find(connection);
        if (cit == connections.end()) {
            return status(rpc_code::not_found);
        }
        const subscriber_id id = cit->second;
        connections.erase(cit);
        auto sit = subscribers.find(id);
        if (sit != subscribers.end()) {
            if (sit->second.active == connection) {
                sit->second.active = hub::invalid_connection;
            }
            for (auto& kv : sit->second.subscriptions) {
                clear_queue(kv.second);
                for (replay_entry& entry : kv.second.replay) {
                    entry.in_queue = false;
                }
            }
        }
        return status{};
    }

    status release(hub::connection_id connection) noexcept {
        auto cit = connections.find(connection);
        if (cit == connections.end()) {
            return status(rpc_code::not_found);
        }
        const subscriber_id id = cit->second;
        connections.erase(cit);
        auto sit = subscribers.find(id);
        if (sit != subscribers.end() && sit->second.active == connection) {
            sit->second.active = hub::invalid_connection;
        }
        // Deliberately no clear_queue(): queued messages stay in_queue and the
        // next attach only retargets them. Already-taken messages remain in the
        // replay window for a cursor-gap retransmit.
        return status{};
    }

    status retransmit_front(subscriber_id id,
                            std::string_view topic) noexcept {
        auto sit = subscribers.find(id);
        if (sit == subscribers.end()) {
            return status(rpc_code::not_found);
        }
        auto sub_it = sit->second.subscriptions.find(topic);
        if (sub_it == sit->second.subscriptions.end()) {
            return status(rpc_code::not_found);
        }
        const status front = retransmit_front_sub(sit->second, sub_it->second);
        if (front.code() == rpc_code::not_acknowledged) {
            disconnect_subscriber(id, hub_event_kind::not_acknowledged,
                                  rpc_code::not_acknowledged, topic);
        }
        return front;
    }

    void publish(std::string_view topic, byte_span payload, delivery_mode mode,
                 const publish_options& publish_opts) noexcept {
        topic_state* ts = find_topic(topic);
        if (ts == nullptr) {
            return;
        }
        if (topic_shard(topic, ts->shard_key, options.shard_count) !=
            options.shard_index) {
            return;
        }

        push_payload_ptr block;
        try {
            auto created = std::make_shared<push_payload>();
            created->bytes.assign(payload.begin(), payload.end());
            block = std::move(created);
        } catch (...) {
            ++counters.dropped_allocation;
            return;
        }

        u64 topic_seq = ts->next_topic_seq;
        if (publish_opts.has_topic_seq) {
            topic_seq = publish_opts.topic_seq;
        }
        if (topic_seq >= ts->next_topic_seq) {
            ts->next_topic_seq = topic_seq + 1;
        }
        ++counters.published;

        const std::size_t bytes = payload.size();
        auto& list = ts->subscribers;
        std::size_t index = 0;
        while (index < list.size()) {
            const subscriber_id id = list[index];
            auto sit = subscribers.find(id);
            if (sit == subscribers.end()) {
                list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
                continue;
            }
            const bool removed =
                deliver(sit->second, topic, block, bytes, mode, topic_seq, publish_opts);
            if (!removed) {
                ++index;
            }
        }
    }

    status ack(subscriber_id id, std::string_view topic, u64 seq) noexcept {
        auto sit = subscribers.find(id);
        if (sit == subscribers.end()) {
            return status(rpc_code::not_found);
        }
        auto sub_it = sit->second.subscriptions.find(topic);
        if (sub_it == sit->second.subscriptions.end()) {
            return status(rpc_code::not_found);
        }
        subscription_state& sub = sub_it->second;
        if (sub.mode != delivery_mode::reliable) {
            return status(rpc_code::failed_precondition);
        }
        if (seq == 0) {
            return status(rpc_code::invalid_argument);
        }
        if (seq <= sub.acked_upto) {
            ++counters.duplicate_acks;
            return status{};
        }
        if (seq >= sub.next_seq) {
            seq = sub.next_seq - 1;
        }
        if (seq <= sub.acked_upto) {
            ++counters.duplicate_acks;
            return status{};
        }
        sub.acked_upto = seq;
        while (!sub.replay.empty() && sub.replay.front().seq <= seq) {
            sub.replay.pop_front();
        }
        for (auto it = sub.queue.begin(); it != sub.queue.end();) {
            if (it->mode == delivery_mode::reliable && it->seq <= seq) {
                sub.queued_bytes -= it->bytes;
                it = sub.queue.erase(it);
            } else {
                ++it;
            }
        }
        return status{};
    }

    status retransmit(subscriber_id id) noexcept {
        auto sit = subscribers.find(id);
        if (sit == subscribers.end()) {
            return status(rpc_code::not_found);
        }
        replay_unacked(sit->second, std::string_view{}, false);
        return status{};
    }

    status retransmit(subscriber_id id, std::string_view topic) noexcept {
        auto sit = subscribers.find(id);
        if (sit == subscribers.end()) {
            return status(rpc_code::not_found);
        }
        if (sit->second.subscriptions.find(topic) ==
            sit->second.subscriptions.end()) {
            return status(rpc_code::not_found);
        }
        replay_unacked(sit->second, topic, true);
        return status{};
    }

    void tick(u64 now_value) noexcept {
        now_ns = now_value;
        if (options.slow_watermark_bytes == 0 || options.slow_timeout.count() <= 0) {
            return;
        }
        const u64 timeout = static_cast<u64>(options.slow_timeout.count());
        std::vector<subscriber_id> victims;
        try {
            for (const auto& kv : subscribers) {
                for (const auto& skv : kv.second.subscriptions) {
                    const subscription_state& sub = skv.second;
                    if (sub.high_water && now_ns - sub.high_water_since_ns >= timeout) {
                        victims.push_back(kv.first);
                        break;
                    }
                }
            }
        } catch (...) {
            return;
        }
        for (subscriber_id id : victims) {
            disconnect_subscriber(id, hub_event_kind::slow_subscriber,
                                  rpc_code::resource_exhausted, std::string_view{});
        }
    }

    void declare_topic(std::string_view topic, u64 shard_key) noexcept {
        auto it = topics.find(topic);
        if (it != topics.end()) {
            it->second.shard_key = shard_key;
            it->second.declared = true;
            return;
        }
        try {
            topics.emplace(std::string(topic), topic_state{});
        } catch (...) {
            return;
        }
        auto created = topics.find(topic);
        if (created != topics.end()) {
            created->second.shard_key = shard_key;
            created->second.declared = true;
        }
    }
};

hub::hub(hub_options options) : impl_(new impl()) {
    impl_->options = options;
    if (impl_->options.shard_count == 0) {
        impl_->options.shard_count = 1;
    }
    impl_->options.shard_index %= impl_->options.shard_count;
}

hub::~hub() = default;

bool hub::owns_topic(std::string_view topic) const noexcept {
    const topic_state* ts = impl_->find_topic(topic);
    const u64 key = ts != nullptr ? ts->shard_key : 0;
    return topic_shard(topic, key, impl_->options.shard_count) ==
           impl_->options.shard_index;
}

void hub::declare_topic(std::string_view topic, u64 shard_key) noexcept {
    impl_->declare_topic(topic, shard_key);
}

status hub::subscribe(std::string_view topic, subscriber_id id,
                      delivery_mode mode, bool ordered,
                      subscribe_options options) {
    if (topic.empty()) {
        return status(rpc_error(rpc_code::invalid_argument, "empty topic"));
    }
    if (!owns_topic(topic)) {
        return status(rpc_error(rpc_code::failed_precondition,
                                "topic not owned by this shard"));
    }
    return impl_->subscribe(topic, id, mode, ordered, options);
}

status hub::unsubscribe(std::string_view topic, subscriber_id id) noexcept {
    auto sit = impl_->subscribers.find(id);
    if (sit == impl_->subscribers.end()) {
        return status(rpc_code::not_found);
    }
    auto sub_it = sit->second.subscriptions.find(topic);
    if (sub_it == sit->second.subscriptions.end()) {
        return status(rpc_code::not_found);
    }
    impl_->remove_topic_subscriber(topic, id);
    sit->second.subscriptions.erase(sub_it);
    if (sit->second.subscriptions.empty() &&
        sit->second.active == invalid_connection) {
        impl_->subscribers.erase(sit);
    }
    return status{};
}

status hub::unsubscribe_all(subscriber_id id) noexcept {
    auto sit = impl_->subscribers.find(id);
    if (sit == impl_->subscribers.end()) {
        return status(rpc_code::not_found);
    }
    for (auto& kv : sit->second.subscriptions) {
        impl_->remove_topic_subscriber(kv.first, id);
    }
    if (sit->second.active != invalid_connection) {
        impl_->connections.erase(sit->second.active);
    }
    impl_->subscribers.erase(sit);
    return status{};
}

hub::connection_id hub::attach(subscriber_id id) noexcept {
    return impl_->attach(id, attach_replay::tail);
}

hub::connection_id hub::attach(subscriber_id id, attach_replay mode) noexcept {
    return impl_->attach(id, mode);
}

status hub::detach(connection_id connection) noexcept {
    return impl_->detach(connection);
}

status hub::release(connection_id connection) noexcept {
    return impl_->release(connection);
}

void hub::publish(std::string_view topic, byte_span payload,
                  delivery_mode mode) noexcept {
    impl_->publish(topic, payload, mode, publish_options{});
}

void hub::publish(std::string_view topic, byte_span payload, delivery_mode mode,
                  const publish_options& options) noexcept {
    impl_->publish(topic, payload, mode, options);
}

bool hub::try_take(connection_id connection, push_message& out) noexcept {
    return impl_->take_any(connection, out);
}

bool hub::try_take(connection_id connection, std::string_view topic,
                   push_message& out) noexcept {
    return impl_->take_topic(connection, topic, out);
}

status hub::ack(subscriber_id id, std::string_view topic, u64 seq) noexcept {
    return impl_->ack(id, topic, seq);
}

status hub::retransmit(subscriber_id id) noexcept {
    return impl_->retransmit(id);
}

status hub::retransmit(subscriber_id id, std::string_view topic) noexcept {
    return impl_->retransmit(id, topic);
}

status hub::retransmit_front(subscriber_id id,
                             std::string_view topic) noexcept {
    return impl_->retransmit_front(id, topic);
}

void hub::tick(u64 now_ns) noexcept {
    impl_->tick(now_ns);
}

bool hub::poll_event(hub_event& out) noexcept {
    if (impl_->events.empty()) {
        return false;
    }
    out = std::move(impl_->events.front());
    impl_->events.pop_front();
    return true;
}

hub_stats hub::stats() const noexcept {
    hub_stats result = impl_->counters;
    result.active_connections = static_cast<u64>(impl_->connections.size());
    result.active_subscriptions = 0;
    result.active_queues = 0;
    result.queued_messages = 0;
    result.queued_bytes = 0;
    for (const auto& kv : impl_->subscribers) {
        for (const auto& skv : kv.second.subscriptions) {
            ++result.active_subscriptions;
            const subscription_state& sub = skv.second;
            if (!sub.queue.empty()) {
                ++result.active_queues;
            }
            result.queued_messages += static_cast<u64>(sub.queue.size());
            result.queued_bytes += static_cast<u64>(sub.queued_bytes);
        }
    }
    return result;
}

} // namespace crpc::push
