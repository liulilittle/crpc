// CRPC -- AMQP 0-9-1 integration against a real RabbitMQ.
//
// Skipped unless CRPC_AMQP_IT=1. Everything here is a property a scripted server
// cannot prove: the field numbers in every method are checked by RabbitMQ's own
// decoder, the SASL PLAIN exchange is accepted by the broker, and Basic.Return
// only exists because a real exchange refused a mandatory publication.
//
// Connection details come from the environment:
//   CRPC_AMQP_IT=1
//   CRPC_AMQP_HOST (default localhost)
//   CRPC_AMQP_PORT (default 5672)
//   CRPC_AMQP_USER (default guest)
//   CRPC_AMQP_PASSWORD (default guest)
//
// The default host is `localhost`, not `127.0.0.1`, because that is what this
// machine's WSL2 relay actually forwards: RabbitMQ listens on `*:5672` inside
// WSL, but the Windows relay binds only `[::1]:5672`, so the IPv4 loopback does
// not reach it. A deployment passes its own host through the environment.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <crpc/db/channel.hpp>
#include <crpc/error.hpp>
#include <crpc/io/iocp_backend.hpp>
#include <crpc/mq/amqp/connection.hpp>
#include <crpc/mq/amqp/types.hpp>
#include <crpc/net/connection.hpp>

namespace {

using crpc::mq::amqp::connection_options;
using crpc::mq::amqp::field_value;
using crpc::mq::amqp::message_properties;

[[nodiscard]] std::string env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::string(value) : std::string(fallback);
}

[[nodiscard]] bool it_enabled() {
    const char* value = std::getenv("CRPC_AMQP_IT");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

[[nodiscard]] const char* require_server() {
    if (!it_enabled()) {
        crpc_test::skip("CRPC_AMQP_IT is not 1");
    }
    return "";
}

void report(const char* what, const crpc::rpc_error& error) {
    std::printf("  %s failed: %s (%d)\n", what, error.message.c_str(),
                static_cast<int>(error.code));
}

// A name unique to this process and run. RabbitMQ keeps queues and exchanges
// between runs, and reusing a name would make a durable-flag mistake look like a
// routing failure.
[[nodiscard]] std::string unique_name(std::string_view prefix) {
    static unsigned int counter = 0;
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::string(prefix) + "." + std::to_string(static_cast<long long>(now)) + "." +
           std::to_string(counter++);
}

[[nodiscard]] crpc::span<const std::byte> bytes_of(std::string_view text) {
    return crpc::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                       text.size());
}

struct it_session {
    crpc::iocp_backend backend;
    std::unique_ptr<crpc::mq::amqp::connection> session;
    std::unique_ptr<crpc::mq::amqp::channel> channel;

    // `heartbeat` of zero accepts the server's proposal.
    explicit it_session(std::uint16_t heartbeat = 0) {
        connection_options options;
        options.host = env_or("CRPC_AMQP_HOST", "localhost");
        options.port = static_cast<std::uint16_t>(std::stoi(env_or("CRPC_AMQP_PORT", "5672")));
        options.user = env_or("CRPC_AMQP_USER", "guest");
        options.password = env_or("CRPC_AMQP_PASSWORD", "guest");
        options.virtual_host = "/";
        options.heartbeat = heartbeat;
        options.operation_timeout = std::chrono::seconds(5);

        crpc::net::connection_config config;
        config.ep = crpc::endpoint::tcp(options.host, options.port);
        config.operation_timeout = std::chrono::seconds(5);
        // An AMQP link carries a handshake and session state; a transparent
        // reconnect behind the caller's back would send a frame without the
        // protocol header. A close is terminal instead.
        config.reconnect_on_eof = false;
        auto link = std::unique_ptr<crpc::db::channel>(
            new crpc::db::socket_channel(std::move(config), backend));
        session.reset(new crpc::mq::amqp::connection(options, std::move(link)));
        if (session->is_open()) {
            auto opened = session->open_channel();
            if (opened) {
                channel = std::move(opened.value());
            } else {
                report("channel.open", opened.error());
            }
        }
    }

    [[nodiscard]] bool ok() const {
        return session != nullptr && session->is_open() && channel != nullptr &&
               channel->is_open();
    }

    // Declares a queue that disappears with this connection. Every case gets its
    // own queue so cases cannot observe each other's leftovers.
    [[nodiscard]] std::string declare_queue(std::string_view queue) {
        auto declared =
            channel->queue_declare(queue, /*passive=*/false, /*durable=*/false,
                                   /*exclusive=*/true, /*auto_delete=*/true);
        if (!declared) {
            report("queue.declare", declared.error());
            return {};
        }
        return declared.value().queue;
    }
};

[[nodiscard]] message_properties sample_properties() {
    message_properties properties;
    properties.content_type = "text/plain";
    properties.content_encoding = "utf-8";
    properties.delivery_mode = std::uint8_t{2};
    properties.priority = std::uint8_t{3};
    properties.correlation_id = "corr-42";
    properties.reply_to = "reply.queue";
    properties.expiration = "60000";
    properties.message_id = "msg-42";
    properties.timestamp = std::uint64_t{1700000000};
    properties.type = "crpc.it";
    properties.user_id = "guest";
    properties.app_id = "crpc-it";
    properties.cluster_id = "cluster-it";

    crpc::mq::amqp::field_table nested;
    nested.set("deep", field_value::make_int32(7));
    properties.headers.set("trace", field_value::make_string("abc"));
    properties.headers.set("flag", field_value::make_bool(true));
    properties.headers.set("count", field_value::make_int32(3));
    properties.headers.set("nested", field_value::make_table(nested));
    properties.headers.set("list", field_value::make_array(
                                        {field_value::make_int32(1),
                                         field_value::make_string("two")}));
    return properties;
}

} // namespace

CRPC_TEST(amqp_it_handshake_channel_and_clean_close) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        report("handshake", it.session->connect_error());
        CRPC_CHECK(it.ok());
        return;
    }
    CRPC_CHECK(it.session->is_open());
    // RabbitMQ 3.12 proposes 60s, 2047 channels and a 131072-byte frame.
    CRPC_CHECK(it.session->heartbeat_interval() > 0);
    CRPC_CHECK(it.session->frame_max() > 0);
    CRPC_CHECK(it.session->channel_max() > 0);
    // The exact version is the broker's business; what this case proves is that
    // Connection.Start carried a decodable properties table. Pinning the patch
    // version would fail on a different RabbitMQ for no protocol reason.
    CRPC_CHECK(it.session->server_version().find("RabbitMQ") != std::string_view::npos);
    CRPC_CHECK(it.session->server_version().size() > std::string_view("RabbitMQ").size());
    // Record what the broker reported: the validation note for this environment
    // needs the observed string, and a differing broker must be visible in the
    // log rather than inferred from a pass.
    std::printf("  server_version=%s heartbeat=%u frame_max=%u channel_max=%u\n",
                std::string(it.session->server_version()).c_str(),
                static_cast<unsigned int>(it.session->heartbeat_interval()),
                static_cast<unsigned int>(it.session->frame_max()),
                static_cast<unsigned int>(it.session->channel_max()));
    // Server properties arrive as a field table and include a product string.
    const field_value* product = it.session->server_properties().find("product");
    CRPC_CHECK(product != nullptr);
    if (product != nullptr) {
        CRPC_CHECK_EQ(std::string("RabbitMQ"), product->text);
    }
    CRPC_CHECK(it.channel->is_open());

    const crpc::status channel_closed = it.channel->close();
    if (!channel_closed) {
        report("channel.close", channel_closed.error());
    }
    CRPC_CHECK(channel_closed.ok());
    CRPC_CHECK_FALSE(it.channel->is_open());

    const crpc::status closed = it.session->close();
    if (!closed) {
        report("connection.close", closed.error());
    }
    CRPC_CHECK(closed.ok());
    CRPC_CHECK_FALSE(it.session->is_open());
}

CRPC_TEST(amqp_it_declare_publish_get_and_ack) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }
    const std::string queue = it.declare_queue(unique_name("crpc.it.get"));
    if (queue.empty()) {
        CRPC_CHECK_FALSE(queue.empty());
        return;
    }

    // passive declare of the same queue must return the same name, and must not
    // create anything.
    auto passive = it.channel->queue_declare(queue, /*passive=*/true);
    if (!passive) {
        report("queue.declare passive", passive.error());
    }
    CRPC_CHECK(passive.ok());
    if (passive) {
        CRPC_CHECK_EQ(queue, passive.value().queue);
    }

    const message_properties properties = sample_properties();
    const std::string body = "hello amqp 0-9-1 \xE2\x9C\x93";
    const crpc::status published =
        it.channel->basic_publish("", queue, properties, bytes_of(body));
    if (!published) {
        report("basic.publish", published.error());
    }
    CRPC_CHECK(published.ok());

    auto got = it.channel->basic_get(queue, /*no_ack=*/false);
    if (!got) {
        report("basic.get", got.error());
    }
    CRPC_CHECK(got.ok());
    if (!got) {
        return;
    }
    CRPC_CHECK_FALSE(got.value().empty);
    CRPC_CHECK_EQ(body.size(), got.value().body.size());
    if (got.value().body.size() == body.size()) {
        const std::string actual(reinterpret_cast<const char*>(got.value().body.data()),
                                 got.value().body.size());
        CRPC_CHECK_EQ(body, actual);
    }
    CRPC_CHECK_EQ(std::string(""), got.value().exchange);
    CRPC_CHECK_EQ(queue, got.value().routing_key);
    CRPC_CHECK(got.value().delivery_tag != 0);
    CRPC_CHECK_EQ(0, static_cast<int>(got.value().message_count));
    CRPC_CHECK(properties == got.value().properties);

    const crpc::status acked = it.channel->basic_ack(got.value().delivery_tag);
    if (!acked) {
        report("basic.ack", acked.error());
    }
    CRPC_CHECK(acked.ok());

    // Acked means gone; a second get is a normal empty result, not an error.
    auto second = it.channel->basic_get(queue, /*no_ack=*/true);
    CRPC_CHECK(second.ok());
    if (second) {
        CRPC_CHECK(second.value().empty);
    }
}

CRPC_TEST(amqp_it_consume_receives_and_acks) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }
    const std::string queue = it.declare_queue(unique_name("crpc.it.consume"));
    if (queue.empty()) {
        CRPC_CHECK_FALSE(queue.empty());
        return;
    }

    CRPC_CHECK(it.channel->basic_qos(1).ok());
    CRPC_CHECK(it.channel->basic_publish("", queue, message_properties(),
                                         bytes_of(std::string_view("first")))
                   .ok());
    CRPC_CHECK(it.channel->basic_publish("", queue, message_properties(),
                                         bytes_of(std::string_view("second")))
                   .ok());

    auto consumed = it.channel->basic_consume(queue, "crpc-it-consumer", /*no_ack=*/false);
    if (!consumed) {
        report("basic.consume", consumed.error());
    }
    CRPC_CHECK(consumed.ok());
    if (!consumed) {
        return;
    }
    CRPC_CHECK_EQ(std::string("crpc-it-consumer"), consumed.value());

    std::vector<std::string> bodies;
    std::vector<std::uint64_t> tags;
    for (int i = 0; i < 2; ++i) {
        auto delivery = it.channel->next_delivery(std::chrono::seconds(5));
        if (!delivery) {
            report("basic.deliver", delivery.error());
        }
        CRPC_CHECK(delivery.ok());
        if (!delivery) {
            return;
        }
        CRPC_CHECK(delivery.value().present);
        bodies.emplace_back(reinterpret_cast<const char*>(delivery.value().body.data()),
                            delivery.value().body.size());
        tags.push_back(delivery.value().delivery_tag);
        CRPC_CHECK_EQ(std::string("crpc-it-consumer"), delivery.value().consumer_tag);
        CRPC_CHECK_EQ(queue, delivery.value().routing_key);
        CRPC_CHECK(delivery.value().delivery_tag != 0);
        CRPC_CHECK(it.channel->basic_ack(delivery.value().delivery_tag).ok());
    }
    CRPC_CHECK_EQ(std::size_t{2}, bodies.size());
    if (bodies.size() == 2) {
        CRPC_CHECK_EQ(std::string("first"), bodies[0]);
        CRPC_CHECK_EQ(std::string("second"), bodies[1]);
    }
    if (tags.size() == 2) {
        CRPC_CHECK(tags[1] > tags[0]);
    }

    const crpc::status cancelled = it.channel->basic_cancel("crpc-it-consumer");
    if (!cancelled) {
        report("basic.cancel", cancelled.error());
    }
    CRPC_CHECK(cancelled.ok());
}

CRPC_TEST(amqp_it_nack_requeue_makes_the_message_available_again) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }
    const std::string queue = it.declare_queue(unique_name("crpc.it.requeue"));
    if (queue.empty()) {
        CRPC_CHECK_FALSE(queue.empty());
        return;
    }
    CRPC_CHECK(it.channel->basic_publish("", queue, message_properties(),
                                         bytes_of(std::string_view("requeue-me")))
                   .ok());

    auto first = it.channel->basic_get(queue, /*no_ack=*/false);
    if (!first) {
        report("basic.get first", first.error());
    }
    CRPC_CHECK(first.ok());
    if (!first || first.value().empty) {
        CRPC_CHECK(false);
        return;
    }
    CRPC_CHECK_FALSE(first.value().redelivered);
    const std::uint64_t tag = first.value().delivery_tag;

    const crpc::status nacked = it.channel->basic_nack(tag, /*multiple=*/false,
                                                       /*requeue=*/true);
    if (!nacked) {
        report("basic.nack", nacked.error());
    }
    CRPC_CHECK(nacked.ok());

    auto second = it.channel->basic_get(queue, /*no_ack=*/false);
    if (!second) {
        report("basic.get after nack", second.error());
    }
    CRPC_CHECK(second.ok());
    if (!second) {
        return;
    }
    CRPC_CHECK_FALSE(second.value().empty);
    CRPC_CHECK(second.value().redelivered);
    CRPC_CHECK_EQ(std::string("requeue-me"),
                  std::string(reinterpret_cast<const char*>(second.value().body.data()),
                              second.value().body.size()));
    CRPC_CHECK(it.channel->basic_ack(second.value().delivery_tag).ok());

    // Reject with requeue=false must discard the message instead.
    CRPC_CHECK(it.channel->basic_publish("", queue, message_properties(),
                                         bytes_of(std::string_view("discard-me")))
                   .ok());
    auto third = it.channel->basic_get(queue, /*no_ack=*/false);
    CRPC_CHECK(third.ok());
    if (third && !third.value().empty) {
        CRPC_CHECK(it.channel->basic_reject(third.value().delivery_tag,
                                            /*requeue=*/false)
                       .ok());
    }
    auto fourth = it.channel->basic_get(queue, /*no_ack=*/true);
    CRPC_CHECK(fourth.ok());
    if (fourth) {
        CRPC_CHECK(fourth.value().empty);
    }
}

CRPC_TEST(amqp_it_direct_exchange_binding_and_routing) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }
    const std::string exchange = unique_name("crpc.it.direct");
    const std::string alpha = it.declare_queue(unique_name("crpc.it.alpha"));
    const std::string beta = it.declare_queue(unique_name("crpc.it.beta"));
    if (alpha.empty() || beta.empty()) {
        CRPC_CHECK_FALSE(alpha.empty() && beta.empty());
        return;
    }

    const crpc::status declared =
        it.channel->exchange_declare(exchange, "direct", /*passive=*/false,
                                     /*durable=*/false, /*auto_delete=*/true);
    if (!declared) {
        report("exchange.declare", declared.error());
    }
    CRPC_CHECK(declared.ok());
    CRPC_CHECK(it.channel->queue_bind(alpha, exchange, "alpha").ok());
    CRPC_CHECK(it.channel->queue_bind(beta, exchange, "beta").ok());

    CRPC_CHECK(it.channel->basic_publish(exchange, "alpha", message_properties(),
                                         bytes_of(std::string_view("routed")))
                   .ok());
    auto routed = it.channel->basic_get(alpha, /*no_ack=*/true);
    CRPC_CHECK(routed.ok());
    if (routed) {
        CRPC_CHECK_FALSE(routed.value().empty);
        CRPC_CHECK_EQ(std::string("routed"),
                      std::string(reinterpret_cast<const char*>(routed.value().body.data()),
                                  routed.value().body.size()));
        CRPC_CHECK_EQ(exchange, routed.value().exchange);
        CRPC_CHECK_EQ(std::string("alpha"), routed.value().routing_key);
    }
    auto other = it.channel->basic_get(beta, /*no_ack=*/true);
    CRPC_CHECK(other.ok());
    if (other) {
        CRPC_CHECK(other.value().empty);
    }

    // mandatory with no binding must come back as Basic.Return with 312
    // (NO_ROUTE), not as a silent drop.
    CRPC_CHECK(it.channel->basic_publish(exchange, "missing", message_properties(),
                                         bytes_of(std::string_view("unrouted")),
                                         /*mandatory=*/true)
                   .ok());
    auto returned = it.channel->next_return(std::chrono::seconds(5));
    if (!returned) {
        report("basic.return", returned.error());
    }
    CRPC_CHECK(returned.ok());
    if (returned) {
        CRPC_CHECK(returned.value().present);
        CRPC_CHECK_EQ(312, static_cast<int>(returned.value().reply_code));
        CRPC_CHECK_EQ(exchange, returned.value().exchange);
        CRPC_CHECK_EQ(std::string("missing"), returned.value().routing_key);
        CRPC_CHECK_EQ(std::string("unrouted"),
                      std::string(reinterpret_cast<const char*>(
                                      returned.value().body.data()),
                                  returned.value().body.size()));
    }
}

CRPC_TEST(amqp_it_get_on_empty_queue_is_not_an_error) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }
    const std::string queue = it.declare_queue(unique_name("crpc.it.empty"));
    if (queue.empty()) {
        CRPC_CHECK_FALSE(queue.empty());
        return;
    }
    auto got = it.channel->basic_get(queue, /*no_ack=*/true);
    if (!got) {
        report("basic.get empty", got.error());
    }
    CRPC_CHECK(got.ok());
    if (got) {
        CRPC_CHECK(got.value().empty);
        CRPC_CHECK_EQ(std::uint64_t{0}, got.value().delivery_tag);
    }
}

CRPC_TEST(amqp_it_queue_delete_removes_the_queue) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }
    // A durable, non-exclusive queue this case owns: it must survive the
    // connection and disappear only through Queue.Delete.
    const std::string queue = unique_name("crpc.it.delete");
    auto declared = it.channel->queue_declare(queue, /*passive=*/false,
                                              /*durable=*/true, /*exclusive=*/false,
                                              /*auto_delete=*/false);
    if (!declared) {
        report("queue.declare durable", declared.error());
    }
    CRPC_CHECK(declared.ok());
    if (!declared) {
        return;
    }
    CRPC_CHECK_EQ(queue, declared.value().queue);

    CRPC_CHECK(it.channel->basic_publish("", queue, message_properties(),
                                         bytes_of(std::string_view("left-behind")))
                   .ok());
    // `if_empty=false` therefore drops the message with the queue, which is what
    // proves the delete really reached the broker instead of only being written.
    const crpc::status deleted = it.channel->queue_delete(queue, /*if_unused=*/false,
                                                          /*if_empty=*/false);
    if (!deleted) {
        report("queue.delete", deleted.error());
    }
    CRPC_CHECK(deleted.ok());

    // A passive declare of a queue that no longer exists must fail with 404
    // NOT_FOUND; anything else would mean the delete never happened.
    auto gone = it.channel->queue_declare(queue, /*passive=*/true);
    CRPC_CHECK_FALSE(gone.ok());
    if (!gone) {
        report("queue.declare after delete", gone.error());
    }
}

CRPC_TEST(amqp_it_heartbeat_keeps_an_idle_link_alive) {
    (void)require_server();
    // Accepting RabbitMQ's 60s proposal would make this case take two minutes.
    // The value is negotiated down in TuneOk, and the broker then closes a
    // silent peer after twice the negotiated interval.
    it_session it(/*heartbeat=*/2);
    if (!it.ok()) {
        report("handshake heartbeat", it.session->connect_error());
        CRPC_CHECK(it.ok());
        return;
    }
    CRPC_CHECK_EQ(2, static_cast<int>(it.session->heartbeat_interval()));

    // Six seconds is longer than RabbitMQ's 2 * 2s timeout. Without a heartbeat
    // frame from the client the connection is closed and the next round trip
    // fails; with one it stays open.
    std::this_thread::sleep_for(std::chrono::seconds(6));
    CRPC_CHECK(it.session->is_open());

    const std::string queue = it.declare_queue(unique_name("crpc.it.heartbeat"));
    if (queue.empty()) {
        CRPC_CHECK_FALSE(queue.empty());
        return;
    }
    auto got = it.channel->basic_get(queue, /*no_ack=*/true);
    if (!got) {
        report("basic.get after idle", got.error());
    }
    CRPC_CHECK(got.ok());
    if (got) {
        CRPC_CHECK(got.value().empty);
    }
}

CRPC_TEST_MAIN()