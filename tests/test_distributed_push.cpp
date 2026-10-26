// CRPC -- distributed push tests.
//
// The push hub is deliberately single-shard: one hub owns a topic and there is
// no hidden cross-node replication in the library. A multi-node deployment
// relays a push between nodes at the application boundary (a gateway/bridge
// subscription that re-publishes on the next node). These tests exercise that
// shape with three servers:
//
//   server A --relay 1--> server B --relay 2--> server C --> subscriber
//
// Each hop is a real PUSH subscription over the in-process backend and each hop
// acknowledges with PUSH_ACK after the sink returns. The tests assert:
//   * the body crosses every hop in order;
//   * PUSH_ACK advances the hub on every node (the bridge is a subscriber too);
//   * a reconnect after a fully acknowledged stream replays nothing;
//   * a middle-hop partition queues at the upstream hub and is replayed after
//     the bridge reconnects, without duplicating the messages already acked;
//   * an unreliable hop delivers without generating PUSH_ACKs.
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/endian.hpp>
#include <crpc/frame.hpp>
#include <crpc/io.hpp>
#include <crpc/push/wire.hpp>
#include <crpc/rpc/client.hpp>
#include <crpc/server.hpp>

using namespace std::chrono_literals;

namespace {

std::atomic<unsigned> fixture_counter{0};

constexpr crpc::push::delivery_mode k_reliable =
    crpc::push::delivery_mode::reliable;
constexpr crpc::push::delivery_mode k_unreliable =
    crpc::push::delivery_mode::unreliable;

std::string unique_name(const char* prefix) {
    return std::string(prefix) + "-" +
           std::to_string(fixture_counter.fetch_add(1, std::memory_order_relaxed));
}

struct push_node {
    crpc::inproc_backend* backend = nullptr;
    std::unique_ptr<crpc::server> srv;
    std::string name;

    push_node() = default;
    push_node(const push_node&) = delete;
    push_node& operator=(const push_node&) = delete;
    ~push_node() { stop(); }

    bool start() {
        crpc::server::builder builder;
        builder.add_listener(crpc::endpoint::inproc(name), *backend);
        builder.drain_timeout(200ms);
        srv = builder.build();
        if (!srv) {
            return false;
        }
        return srv->start().ok();
    }

    void stop() noexcept {
        if (srv) {
            srv->stop();
        }
    }

    [[nodiscard]] crpc::endpoint ep() const {
        return crpc::endpoint::inproc(name);
    }
};

std::unique_ptr<crpc::rpc::client> connect_client(crpc::inproc_backend& backend,
                                                  const crpc::endpoint& ep) {
    auto connection = backend.connect(ep, crpc::connect_options{});
    if (!connection) {
        return nullptr;
    }
    crpc::rpc::client_options options;
    options.slot_capacity = 32;
    options.default_timeout = 2s;
    return std::make_unique<crpc::rpc::client>(std::move(connection), options);
}

// --- subscriber capture ------------------------------------------------------

struct sink_state {
    std::mutex mutex;
    std::vector<std::string> bodies;
    std::vector<crpc::u64> sequences;
    std::vector<crpc::push::delivery_mode> modes;

    [[nodiscard]] std::size_t size() {
        std::lock_guard<std::mutex> lk(mutex);
        return bodies.size();
    }

    [[nodiscard]] std::string body(std::size_t index) {
        std::lock_guard<std::mutex> lk(mutex);
        return index < bodies.size() ? bodies[index] : std::string();
    }

    [[nodiscard]] crpc::u64 sequence(std::size_t index) {
        std::lock_guard<std::mutex> lk(mutex);
        return index < sequences.size() ? sequences[index] : 0;
    }

    [[nodiscard]] crpc::push::delivery_mode mode(std::size_t index) {
        std::lock_guard<std::mutex> lk(mutex);
        return index < modes.size() ? modes[index] : k_unreliable;
    }
};

void record_push(void* context,
                 const crpc::push::push_message_view& message) noexcept {
    auto* state = static_cast<sink_state*>(context);
    std::lock_guard<std::mutex> lk(state->mutex);
    state->bodies.emplace_back(
        reinterpret_cast<const char*>(message.body.data()), message.body.size());
    state->sequences.push_back(message.seq);
    state->modes.push_back(message.mode);
}

// --- relay -------------------------------------------------------------------

// A relay subscribes on the upstream server and re-publishes every message on
// the downstream server. The sink only copies into a queue (it runs on the
// reader thread); a worker thread performs the downstream publish so a slow
// downstream node cannot hold the upstream reader.
class relay {
public:
    struct queued {
        std::string body;
        crpc::push::delivery_mode mode = k_reliable;
    };

    relay(crpc::inproc_backend& backend, crpc::endpoint upstream,
          crpc::server& downstream, std::string topic,
          crpc::u64 subscriber_id, crpc::push::delivery_mode mode)
        : backend_(&backend),
          upstream_(std::move(upstream)),
          downstream_(&downstream),
          topic_(std::move(topic)),
          subscriber_id_(subscriber_id),
          mode_(mode) {}

    relay(const relay&) = delete;
    relay& operator=(const relay&) = delete;
    ~relay() { stop(); }

    bool start() {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            stop_ = false;
            queue_.clear();
        }
        client_ = connect_client(*backend_, upstream_);
        if (!client_) {
            return false;
        }
        client_->set_push_sink(&relay::on_push, this);
        auto subscription =
            client_->subscribe_push(topic_, subscriber_id_, mode_, false);
        if (!subscription.ok()) {
            client_.reset();
            return false;
        }
        worker_ = std::thread([this] { run(); });
        return true;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        if (client_) {
            client_->close();
            client_.reset();
        }
    }

    [[nodiscard]] std::size_t relayed() const noexcept {
        return relayed_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t publish_errors() const noexcept {
        return publish_errors_.load(std::memory_order_acquire);
    }

private:
    static void on_push(void* context,
                        const crpc::push::push_message_view& message) noexcept {
        auto* self = static_cast<relay*>(context);
        queued item;
        item.body.assign(reinterpret_cast<const char*>(message.body.data()),
                         message.body.size());
        item.mode = message.mode;
        {
            std::lock_guard<std::mutex> lk(self->mutex_);
            self->queue_.push_back(std::move(item));
        }
        self->cv_.notify_one();
    }

    void run() {
        for (;;) {
            queued item;
            {
                std::unique_lock<std::mutex> lk(mutex_);
                cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
                if (queue_.empty()) {
                    if (stop_) {
                        return;
                    }
                    continue;
                }
                item = std::move(queue_.front());
                queue_.pop_front();
            }

            const crpc::span<const std::byte> body(
                reinterpret_cast<const std::byte*>(item.body.data()),
                item.body.size());
            if (downstream_->publish(topic_, body, item.mode).ok()) {
                relayed_.fetch_add(1, std::memory_order_release);
            } else {
                publish_errors_.fetch_add(1, std::memory_order_release);
            }
        }
    }

    crpc::inproc_backend* backend_;
    crpc::endpoint upstream_;
    crpc::server* downstream_;
    std::string topic_;
    crpc::u64 subscriber_id_ = 0;
    crpc::push::delivery_mode mode_ = k_reliable;

    std::unique_ptr<crpc::rpc::client> client_;
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<queued> queue_;
    bool stop_ = false;
    std::atomic<std::size_t> relayed_{0};
    std::atomic<std::size_t> publish_errors_{0};
};

std::string message_body(const char* prefix, int index) {
    return std::string(prefix) + std::to_string(index);
}

} // namespace

// --- reliable three-hop delivery and PUSH_ACK --------------------------------

CRPC_TEST(distributed_push_three_hops_ack_and_order) {
    crpc::inproc_backend backend;
    push_node a;
    push_node b;
    push_node c;
    a.backend = &backend;
    b.backend = &backend;
    c.backend = &backend;
    a.name = unique_name("push-a");
    b.name = unique_name("push-b");
    c.name = unique_name("push-c");
    CRPC_CHECK(a.start());
    CRPC_CHECK(b.start());
    CRPC_CHECK(c.start());
    if (!a.srv || !b.srv || !c.srv) {
        return;
    }

    const std::string topic = "orders";
    sink_state sink;
    auto subscriber = connect_client(backend, c.ep());
    CRPC_CHECK(subscriber != nullptr);
    if (!subscriber) {
        return;
    }
    subscriber->set_push_sink(&record_push, &sink);
    auto subscription = subscriber->subscribe_push(topic, 501, k_reliable, false);
    CRPC_CHECK(subscription.ok());

    relay hop1(backend, a.ep(), *b.srv, topic, 601, k_reliable);
    relay hop2(backend, b.ep(), *c.srv, topic, 602, k_reliable);
    CRPC_CHECK(hop1.start());
    CRPC_CHECK(hop2.start());

    for (int i = 0; i < 3; ++i) {
        const std::string body = message_body("m", i);
        CRPC_CHECK(a.srv->publish(
            topic,
            crpc::span<const std::byte>(
                reinterpret_cast<const std::byte*>(body.data()), body.size()),
            k_reliable));
    }

    CRPC_CHECK(crpc_test::wait_for([&sink] { return sink.size() == 3; }, 5000));
    CRPC_CHECK_EQ(std::size_t{3}, sink.size());
    for (int i = 0; i < 3; ++i) {
        CRPC_CHECK_EQ(message_body("m", i), sink.body(static_cast<std::size_t>(i)));
        CRPC_CHECK_EQ(static_cast<crpc::u64>(i + 1),
                      sink.sequence(static_cast<std::size_t>(i)));
        CRPC_CHECK(sink.mode(static_cast<std::size_t>(i)) == k_reliable);
    }

    // Each hop is a real subscriber and acknowledges after its sink returns.
    CRPC_CHECK(crpc_test::wait_for(
        [&a] { return a.srv->stats().push_acks >= 3; }, 5000));
    CRPC_CHECK(crpc_test::wait_for(
        [&b] { return b.srv->stats().push_acks >= 3; }, 5000));
    CRPC_CHECK(crpc_test::wait_for(
        [&c] { return c.srv->stats().push_acks >= 3; }, 5000));

    // The subscriber had acknowledged everything before it reconnects, so the
    // hub replays nothing. A new publish continues at sequence 4.
    subscriber->close();
    subscriber.reset();
    sink_state resumed;
    auto reconnected = connect_client(backend, c.ep());
    CRPC_CHECK(reconnected != nullptr);
    if (!reconnected) {
        return;
    }
    reconnected->set_push_sink(&record_push, &resumed);
    auto resumed_subscription =
        reconnected->subscribe_push(topic, 501, k_reliable, false);
    CRPC_CHECK(resumed_subscription.ok());
    CRPC_CHECK_EQ(std::size_t{0}, resumed.size());

    const std::string next = message_body("m", 3);
    CRPC_CHECK(a.srv->publish(
        topic,
        crpc::span<const std::byte>(
            reinterpret_cast<const std::byte*>(next.data()), next.size()),
        k_reliable));
    CRPC_CHECK(
        crpc_test::wait_for([&resumed] { return resumed.size() == 1; }, 5000));
    CRPC_CHECK_EQ(std::string("m3"), resumed.body(0));
    CRPC_CHECK_EQ(static_cast<crpc::u64>(4), resumed.sequence(0));

    hop2.stop();
    hop1.stop();
    reconnected->close();
}

// --- middle-hop partition and replay -----------------------------------------

CRPC_TEST(distributed_push_recovers_after_middle_hop_partition) {
    crpc::inproc_backend backend;
    push_node a;
    push_node b;
    push_node c;
    a.backend = &backend;
    b.backend = &backend;
    c.backend = &backend;
    a.name = unique_name("push-pa");
    b.name = unique_name("push-pb");
    c.name = unique_name("push-pc");
    CRPC_CHECK(a.start());
    CRPC_CHECK(b.start());
    CRPC_CHECK(c.start());
    if (!a.srv || !b.srv || !c.srv) {
        return;
    }

    const std::string topic = "events";
    sink_state sink;
    auto subscriber = connect_client(backend, c.ep());
    CRPC_CHECK(subscriber != nullptr);
    if (!subscriber) {
        return;
    }
    subscriber->set_push_sink(&record_push, &sink);
    CRPC_CHECK(subscriber->subscribe_push(topic, 701, k_reliable, false).ok());

    relay hop1(backend, a.ep(), *b.srv, topic, 801, k_reliable);
    relay hop2(backend, b.ep(), *c.srv, topic, 802, k_reliable);
    CRPC_CHECK(hop1.start());
    CRPC_CHECK(hop2.start());

    const std::string first = "before-partition";
    CRPC_CHECK(a.srv->publish(
        topic,
        crpc::span<const std::byte>(
            reinterpret_cast<const std::byte*>(first.data()), first.size()),
        k_reliable));
    CRPC_CHECK(crpc_test::wait_for([&sink] { return sink.size() == 1; }, 5000));
    CRPC_CHECK(crpc_test::wait_for([&hop2] { return hop2.relayed() == 1; }, 5000));

    // The middle hop is down. The upstream hub keeps the subscription and
    // queues the message for the disconnected bridge; it is not acked.
    hop2.stop();
    const std::string second = "during-partition";
    CRPC_CHECK(a.srv->publish(
        topic,
        crpc::span<const std::byte>(
            reinterpret_cast<const std::byte*>(second.data()), second.size()),
        k_reliable));
    CRPC_CHECK(crpc_test::wait_for([&hop1] { return hop1.relayed() == 2; }, 5000));
    CRPC_CHECK(crpc_test::wait_for(
        [&b] { return b.srv->stats().push_queued_messages >= 1; }, 5000));

    // Reconnecting the same bridge subscriber id replays the unacknowledged
    // message; the downstream subscriber sees it exactly once.
    CRPC_CHECK(hop2.start());
    CRPC_CHECK(crpc_test::wait_for([&sink] { return sink.size() == 2; }, 5000));
    CRPC_CHECK_EQ(std::size_t{2}, sink.size());
    CRPC_CHECK_EQ(std::string("before-partition"), sink.body(0));
    CRPC_CHECK_EQ(std::string("during-partition"), sink.body(1));
    CRPC_CHECK_EQ(static_cast<crpc::u64>(1), sink.sequence(0));
    CRPC_CHECK_EQ(static_cast<crpc::u64>(2), sink.sequence(1));
    CRPC_CHECK(crpc_test::wait_for([&hop2] { return hop2.relayed() == 2; }, 5000));

    hop2.stop();
    hop1.stop();
    subscriber->close();
}

// --- unreliable hop ----------------------------------------------------------

CRPC_TEST(distributed_push_unreliable_hop_has_no_ack) {
    crpc::inproc_backend backend;
    push_node a;
    push_node b;
    a.backend = &backend;
    b.backend = &backend;
    a.name = unique_name("push-ua");
    b.name = unique_name("push-ub");
    CRPC_CHECK(a.start());
    CRPC_CHECK(b.start());
    if (!a.srv || !b.srv) {
        return;
    }

    const std::string topic = "telemetry";
    sink_state sink;
    auto subscriber = connect_client(backend, b.ep());
    CRPC_CHECK(subscriber != nullptr);
    if (!subscriber) {
        return;
    }
    subscriber->set_push_sink(&record_push, &sink);
    CRPC_CHECK(subscriber->subscribe_push(topic, 901, k_unreliable, false).ok());

    relay hop(backend, a.ep(), *b.srv, topic, 902, k_unreliable);
    CRPC_CHECK(hop.start());

    const std::string body = "unreliable-1";
    CRPC_CHECK(a.srv->publish(
        topic,
        crpc::span<const std::byte>(
            reinterpret_cast<const std::byte*>(body.data()), body.size()),
        k_unreliable));

    CRPC_CHECK(crpc_test::wait_for([&sink] { return sink.size() == 1; }, 5000));
    CRPC_CHECK_EQ(std::string("unreliable-1"), sink.body(0));
    CRPC_CHECK_EQ(static_cast<crpc::u64>(0), sink.sequence(0));
    CRPC_CHECK(sink.mode(0) == k_unreliable);

    // Neither hop acknowledges an unreliable message.
    CRPC_CHECK_EQ(std::size_t{0}, a.srv->stats().push_acks);
    CRPC_CHECK_EQ(std::size_t{0}, b.srv->stats().push_acks);

    hop.stop();
    subscriber->close();
}

CRPC_TEST_MAIN()
