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
//   CRPC_AMQP_SECOND_PORT (default 5673; second broker for failover cases)
//   CRPC_AMQP_FAILOVER_IT=1 (also stops/restarts the primary broker's systemd
//                            unit through WSL, so the test restores it)
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
#include <cstdio>
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

// Options built purely from the environment; used by the failover cases that
// open through `connection::open` instead of the manual `it_session` path.
[[nodiscard]] connection_options options_from_env() {
    connection_options options;
    options.host = env_or("CRPC_AMQP_HOST", "localhost");
    options.port = static_cast<std::uint16_t>(std::stoi(env_or("CRPC_AMQP_PORT", "5672")));
    options.user = env_or("CRPC_AMQP_USER", "guest");
    options.password = env_or("CRPC_AMQP_PASSWORD", "guest");
    options.virtual_host = "/";
    options.operation_timeout = std::chrono::seconds(5);
    return options;
}

[[nodiscard]] std::uint16_t second_port() {
    return static_cast<std::uint16_t>(std::stoi(env_or("CRPC_AMQP_SECOND_PORT", "5673")));
}

// Runs a WSL command and captures its output. Returns false when the command
// itself failed to launch or returned non-zero.
[[nodiscard]] bool run_wsl(const char* arguments, std::string& output);

[[nodiscard]] const char* require_failover_control() {
    (void)require_server();
    const char* value = std::getenv("CRPC_AMQP_FAILOVER_IT");
    if (value == nullptr || std::strcmp(value, "1") != 0) {
        crpc_test::skip("CRPC_AMQP_FAILOVER_IT is not 1");
    }
    // Belt and suspenders: even if the primary broker is stopped and the test
    // dies abnormally (a crash skips destructors, and normal failure paths are
    // covered by the guard in the case), the service is restarted when the
    // process exits. `systemctl start` is a no-op when the unit already runs.
    static bool registered = false;
    if (!registered) {
        registered = true;
        std::atexit([] {
            std::string ignore;
            (void)run_wsl("sudo systemctl start rabbitmq-server", ignore);
        });
    }
    return "";
}

// Runs a WSL command and captures its output. Returns false when the command
// itself failed to launch or returned non-zero.
[[nodiscard]] bool run_wsl(const char* arguments, std::string& output) {
    std::string command = "wsl -e bash -lc \"";
    command += arguments;
    command += "\"";
    FILE* pipe = _popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return false;
    }
    char buffer[512] = {0};
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    const int rc = _pclose(pipe);
    return rc == 0;
}

// The stop/restart of the primary broker is real state on the test machine.
// The guard restores the service even when an assertion in the middle fails,
// so the suite never leaves the broker down.
struct primary_broker_guard {
    std::string stop_output;
    bool stopped = false;

    ~primary_broker_guard() {
        if (stopped) {
            std::string recovered;
            const bool ok = run_wsl("sudo systemctl start rabbitmq-server", recovered);
            std::printf("  failover: node1 restart %s (%s)\n",
                        ok ? "succeeded" : "FAILED", recovered.c_str());
        }
    }
};

// Finds a port on `host` that currently refuses connections, used as the "dead
// node" of a node list.
[[nodiscard]] std::uint16_t find_closed_port(crpc::backend_factory& backend,
                                             const std::string& host) {
    for (std::uint16_t port = 65535; port > 50000; --port) {
        connection_options probe = options_from_env();
        probe.host = host;
        probe.port = port;
        probe.operation_timeout = std::chrono::milliseconds(400);
        const auto attempt = crpc::mq::amqp::connection::open(probe, backend);
        if (!attempt.ok()) {
            return port;
        }
    }
    return 65535;
}

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

CRPC_TEST(amqp_it_publisher_confirms_ack_path) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }
    const std::string exchange = unique_name("crpc.it.confirm.ack.ex");
    const std::string queue = it.declare_queue(unique_name("crpc.it.confirm.ack"));
    if (queue.empty()) {
        CRPC_CHECK_FALSE(queue.empty());
        return;
    }
    CRPC_CHECK(it.channel->exchange_declare(exchange, "direct").ok());
    CRPC_CHECK(it.channel->queue_bind(queue, exchange, "routed").ok());
    CRPC_CHECK_FALSE(it.channel->publisher_confirms_enabled());
    CRPC_CHECK(it.channel->confirm_select().ok());
    CRPC_CHECK(it.channel->publisher_confirms_enabled());

    for (int i = 0; i < 3; ++i) {
        const std::string body = "confirmed-" + std::to_string(i);
        auto confirmed = it.channel->basic_publish_confirm(
            exchange, "routed", message_properties(), bytes_of(body),
            /*mandatory=*/false, /*immediate=*/false, std::chrono::seconds(10));
        if (!confirmed) {
            report("basic.publish_confirm ack", confirmed.error());
        }
        CRPC_CHECK(confirmed.ok());
        if (confirmed) {
            CRPC_CHECK(confirmed.value().acknowledged);
            CRPC_CHECK(confirmed.value().delivery_tag != 0);
            std::printf("  confirm ack tag=%llu\n",
                        static_cast<unsigned long long>(confirmed.value().delivery_tag));
        }
    }

    // All three messages were accepted by the broker, so Basic.Get finds them.
    for (int i = 0; i < 3; ++i) {
        auto got = it.channel->basic_get(queue, /*no_ack=*/true);
        CRPC_CHECK(got.ok());
        if (got && !got.value().empty) {
            const std::string body = "confirmed-" + std::to_string(i);
            CRPC_CHECK_EQ(body, std::string(reinterpret_cast<const char*>(
                                                got.value().body.data()),
                                            got.value().body.size()));
        } else {
            CRPC_CHECK(false);
        }
    }

    // A mandatory publication that cannot be routed is returned to the
    // publisher and RabbitMQ 3.12.1 still *acks* it (a return is a handled
    // message, not a failure of the broker to process it). Record the actual
    // answer: the ack arrives, then the Basic.Return with NO_ROUTE.
    auto unroutable = it.channel->basic_publish_confirm(
        exchange, "missing", message_properties(),
        bytes_of(std::string_view("unroutable-but-acked")), /*mandatory=*/true,
        /*immediate=*/false, std::chrono::seconds(10));
    if (!unroutable) {
        report("basic.publish_confirm unroutable", unroutable.error());
    }
    CRPC_CHECK(unroutable.ok());
    if (unroutable.ok()) {
        std::printf("  unroutable mandatory publish answered: acknowledged=%d tag=%llu\n",
                    unroutable.value().acknowledged ? 1 : 0,
                    static_cast<unsigned long long>(unroutable.value().delivery_tag));
        auto returned = it.channel->next_return(std::chrono::seconds(5));
        CRPC_CHECK(returned.ok());
        if (returned.ok()) {
            CRPC_CHECK(returned.value().present);
            CRPC_CHECK_EQ(312, static_cast<int>(returned.value().reply_code));
            std::printf("  returned reply_code=%u reply_text=%s\n",
                        static_cast<unsigned int>(returned.value().reply_code),
                        returned.value().reply_text.c_str());
        }
    }
}

CRPC_TEST(amqp_it_publisher_confirms_nack_path_with_reject_publish) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }
    // A queue that rejects a publication when it is full makes the broker send
    // a negative publisher confirm -- the only real, deterministic nack a
    // classic broker issues.
    crpc::mq::amqp::field_table limits;
    limits.set("x-max-length", crpc::mq::amqp::field_value::make_int32(1));
    limits.set("x-overflow", crpc::mq::amqp::field_value::make_string("reject-publish"));
    const std::string queue = unique_name("crpc.it.confirm.nack");
    auto declared = it.channel->queue_declare(queue, /*passive=*/false,
                                              /*durable=*/false, /*exclusive=*/true,
                                              /*auto_delete=*/true, limits);
    if (!declared) {
        report("queue.declare reject-publish", declared.error());
        CRPC_CHECK(declared.ok());
        return;
    }
    CRPC_CHECK(it.channel->confirm_select().ok());

    // First publication fills the queue: ack.
    auto filled = it.channel->basic_publish_confirm(
        "", queue, message_properties(), bytes_of(std::string_view("fill")),
        /*mandatory=*/false, /*immediate=*/false, std::chrono::seconds(10));
    if (!filled) {
        report("basic.publish_confirm fill", filled.error());
    }
    CRPC_CHECK(filled.ok());
    if (filled.ok()) {
        CRPC_CHECK(filled.value().acknowledged);
        std::printf("  confirm ack on accepted publication tag=%llu\n",
                    static_cast<unsigned long long>(filled.value().delivery_tag));
    }

    // Second publication hits the full queue and is rejected: nack.
    auto refused = it.channel->basic_publish_confirm(
        "", queue, message_properties(), bytes_of(std::string_view("rejected")),
        /*mandatory=*/false, /*immediate=*/false, std::chrono::seconds(10));
    if (!refused) {
        report("basic.publish_confirm nack", refused.error());
    }
    CRPC_CHECK(refused.ok());
    if (refused.ok()) {
        std::printf("  confirm nack on rejected publication tag=%llu\n",
                    static_cast<unsigned long long>(refused.value().delivery_tag));
        CRPC_CHECK_FALSE(refused.value().acknowledged);
    }

    // Only the first message made it into the queue.
    auto got = it.channel->basic_get(queue, /*no_ack=*/true);
    CRPC_CHECK(got.ok());
    if (got) {
        CRPC_CHECK_FALSE(got.value().empty);
        CRPC_CHECK_EQ(std::string("fill"),
                      std::string(reinterpret_cast<const char*>(got.value().body.data()),
                                  got.value().body.size()));
    }
}

CRPC_TEST(amqp_it_consumer_cancel_notification) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }
    const std::string queue = unique_name("crpc.it.cancel.notify");
    auto declared = it.channel->queue_declare(queue, /*passive=*/false,
                                              /*durable=*/false, /*exclusive=*/false,
                                              /*auto_delete=*/true);
    if (!declared) {
        report("queue.declare cancel notify", declared.error());
        CRPC_CHECK(declared.ok());
        return;
    }

    auto consumed = it.channel->basic_consume(queue, "crpc-it-cancel-consumer",
                                              /*no_ack=*/false);
    if (!consumed) {
        report("basic.consume cancel notify", consumed.error());
        CRPC_CHECK(consumed.ok());
        return;
    }
    const std::string tag = consumed.value();
    CRPC_CHECK_EQ(std::string("crpc-it-cancel-consumer"), tag);

    // Delete the queue through a second channel: the broker must cancel the
    // consumer on the first channel with an asynchronous Basic.Cancel.
    auto second = it.session->open_channel();
    if (!second) {
        report("channel.open cancel notify", second.error());
        CRPC_CHECK(second.ok());
        return;
    }
    const crpc::status deleted = second.value()->queue_delete(queue);
    if (!deleted) {
        report("queue.delete cancel notify", deleted.error());
    }
    CRPC_CHECK(deleted.ok());

    auto cancelled = it.channel->next_cancel(std::chrono::seconds(5));
    if (!cancelled) {
        report("next_cancel", cancelled.error());
    }
    CRPC_CHECK(cancelled.ok());
    if (cancelled) {
        CRPC_CHECK(cancelled.value().present);
        CRPC_CHECK_EQ(tag, cancelled.value().consumer_tag);
        std::printf("  consumer cancelled by broker: %s\n",
                    cancelled.value().consumer_tag.c_str());
    }
}

CRPC_TEST(amqp_it_quorum_queue_declares_and_routes) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }
    const std::string queue = unique_name("crpc.it.quorum");

    crpc::mq::amqp::field_table arguments;
    arguments.set("x-queue-type", crpc::mq::amqp::field_value::make_string("quorum"));
    auto declared = it.channel->queue_declare(queue, /*passive=*/false,
                                              /*durable=*/true, /*exclusive=*/false,
                                              /*auto_delete=*/false, arguments);
    if (!declared) {
        report("queue.declare quorum", declared.error());
        CRPC_CHECK(declared.ok());
        return;
    }
    CRPC_CHECK_EQ(queue, declared.value().queue);

    // A quorum queue is replicated; RabbitMQ refuses non-durable and exclusive
    // declarations for it. A classic broker would accept this declare, so the
    // 406 reply is the proof the x-queue-type took effect on the wire. The
    // refusal closes the channel it was issued on, so it must live on its own
    // channel and not disturb the working one.
    const std::string fragile = unique_name("crpc.it.quorum.invalid");
    auto probe = it.session->open_channel();
    if (!probe) {
        report("channel.open quorum probe", probe.error());
        CRPC_CHECK(probe.ok());
        return;
    }
    auto refused = probe.value()->queue_declare(fragile, /*passive=*/false,
                                                /*durable=*/false, /*exclusive=*/false,
                                                /*auto_delete=*/false, arguments);
    CRPC_CHECK_FALSE(refused.ok());
    if (!refused) {
        std::printf("  quorum non-durable rejected with reply_code=%d\n",
                    static_cast<int>(refused.error().code));
        report("queue.declare quorum non-durable", refused.error());
    }
    // A channel closed by a broker error is permanently dead and must report
    // that, not silently accept another method.
    CRPC_CHECK_FALSE(probe.value()->is_open());
    auto dead_call = probe.value()->queue_declare(fragile, /*passive=*/true);
    CRPC_CHECK_FALSE(dead_call.ok());

    CRPC_CHECK(it.channel->basic_publish("", queue, message_properties(),
                                         bytes_of(std::string_view("quorum-payload")))
                   .ok());
    // A brand-new quorum queue's Raft group may take a moment to index the
    // first publication, so poll for the message instead of assuming a strict
    // get-after-publish order on this one case.
    bool seen = false;
    for (int attempt = 0; attempt < 30 && !seen; ++attempt) {
        auto got = it.channel->basic_get(queue, /*no_ack=*/true);
        if (!got) {
            report("basic.get quorum", got.error());
            CRPC_CHECK(got.ok());
            return;
        }
        if (got.value().empty) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        seen = true;
        CRPC_CHECK_EQ(std::string("quorum-payload"),
                      std::string(reinterpret_cast<const char*>(got.value().body.data()),
                                  got.value().body.size()));
    }
    CRPC_CHECK(seen);

    CRPC_CHECK(it.channel->queue_delete(queue).ok());
}

CRPC_TEST(amqp_it_multi_node_connect_skips_a_dead_node) {
    (void)require_server();
    crpc::iocp_backend backend;
    connection_options options = options_from_env();
    const std::uint16_t dead =
        find_closed_port(backend, options.host);

    options.nodes = {{options.host, dead}, {options.host, options.port}};
    options.operation_timeout = std::chrono::seconds(2);

    auto session = crpc::mq::amqp::connection::open(options, backend);
    if (!session) {
        report("connection.open multi-node", session.error());
    }
    CRPC_CHECK(session.ok());
    if (!session) {
        return;
    }
    // The client had to try the dead node first, then the live one, and must
    // report the node it actually connected to.
    CRPC_CHECK_EQ(options.port, session.value()->active_node().port);
    std::printf("  connected to %s:%u through a dead first node\n",
                session.value()->active_node().host.c_str(),
                static_cast<unsigned int>(session.value()->active_node().port));
    CRPC_CHECK(session.value()->is_open());

    auto opened = session.value()->open_channel();
    CRPC_CHECK(opened.ok());
    if (opened.ok()) {
        auto declared = opened.value()->queue_declare(unique_name("crpc.it.multi"),
                                                      /*passive=*/false,
                                                      /*durable=*/false,
                                                      /*exclusive=*/true,
                                                      /*auto_delete=*/true);
        CRPC_CHECK(declared.ok());
    }

    // A node list with nothing alive must fail as a whole.
    options.nodes = {{options.host, dead}, {options.host, dead}};
    auto gone = crpc::mq::amqp::connection::open(options, backend);
    CRPC_CHECK_FALSE(gone.ok());
}

CRPC_TEST(amqp_it_failover_reconnect_redeclares_and_consumes) {
    (void)require_failover_control();
    crpc::iocp_backend backend;
    connection_options options = options_from_env();
    const std::uint16_t second = second_port();
    options.nodes = {{options.host, options.port}, {options.host, second}};
    options.operation_timeout = std::chrono::seconds(3);
    options.reconnect_delay_base = std::chrono::milliseconds(100);
    options.reconnect_delay_cap = std::chrono::milliseconds(1000);
    options.reconnect_attempts = 20;

    // The second broker must actually be reachable, otherwise this case would
    // validate nothing about failover.
    {
        connection_options probe = options_from_env();
        probe.host = options.host;
        probe.port = second;
        probe.operation_timeout = std::chrono::milliseconds(800);
        const auto reachable = crpc::mq::amqp::connection::open(probe, backend);
        if (!reachable) {
            crpc_test::skip("the second AMQP node (port 5673) is not reachable");
        }
    }

    auto session = crpc::mq::amqp::connection::open(options, backend);
    if (!session) {
        report("connection.open failover", session.error());
        CRPC_CHECK(session.ok());
        return;
    }
    CRPC_CHECK_EQ(options.port, session.value()->active_node().port);

    auto channel = session.value()->open_channel();
    if (!channel) {
        report("channel.open failover", channel.error());
        CRPC_CHECK(channel.ok());
        return;
    }
    const std::string queue = unique_name("crpc.it.failover");
    auto declared = channel.value()->queue_declare(queue, /*passive=*/false,
                                                   /*durable=*/true,
                                                   /*exclusive=*/false,
                                                   /*auto_delete=*/false);
    if (!declared) {
        report("queue.declare failover", declared.error());
        CRPC_CHECK(declared.ok());
        return;
    }
    CRPC_CHECK(channel.value()->confirm_select().ok());
    CRPC_CHECK(channel.value()->basic_qos(1).ok());
    const std::string consumer_tag = "crpc-it-failover-consumer";
    auto consumed = channel.value()->basic_consume(queue, consumer_tag, /*no_ack=*/false);
    if (!consumed) {
        report("basic.consume failover", consumed.error());
        CRPC_CHECK(consumed.ok());
        return;
    }
    CRPC_CHECK_EQ(consumer_tag, consumed.value());

    // Prove the consumer works on the primary before the failover.
    auto before = channel.value()->basic_publish_confirm(
        "", queue, message_properties(), bytes_of(std::string_view("before-failover")),
        /*mandatory=*/false, /*immediate=*/false, std::chrono::seconds(10));
    CRPC_CHECK(before.ok());
    CRPC_CHECK(before.ok() && before.value().acknowledged);
    if (before.ok() && before.value().acknowledged) {
        auto delivered = channel.value()->next_delivery(std::chrono::seconds(5));
        CRPC_CHECK(delivered.ok());
        if (delivered.ok() && delivered.value().present) {
            CRPC_CHECK_EQ(consumer_tag, delivered.value().consumer_tag);
            CRPC_CHECK(channel.value()->basic_ack(delivered.value().delivery_tag).ok());
        }
    }

    // Pull the rug: stop the primary broker. Everything after this point must
    // self-heal.
    primary_broker_guard guard;
    guard.stopped = run_wsl("sudo systemctl stop rabbitmq-server", guard.stop_output);
    std::printf("  failover: node1 stop issued (%s)\n",
                guard.stopped ? "ok" : "COMMAND FAILED");
    if (!guard.stopped) {
        CRPC_CHECK(guard.stopped);
        return;
    }

    // Wait until the client observes the dead link on a bounded call.
    const crpc::rpc_error* drop = nullptr;
    crpc::rpc_error seen;
    bool dropped = false;
    for (int attempt = 0; attempt < 40 && !dropped; ++attempt) {
        auto delivery = channel.value()->next_delivery(std::chrono::milliseconds(300));
        if (!delivery) {
            seen = delivery.error();
            drop = &seen;
            dropped = true;
        }
    }
    const std::string drop_message = drop != nullptr ? drop->message : "<none>";
    CRPC_CHECK(dropped);
    if (!dropped) {
        CRPC_CHECK(dropped);
        return;
    }
    std::printf("  failover: link dropped: %s (%d)\n", drop_message.c_str(),
                static_cast<int>(drop->code));
    CRPC_CHECK_FALSE(session.value()->is_open());

    // The next public call must reconnect (node1 is down, so node2 is tried)
    // and re-declare the queue tier: exchange/queue/binding/consumer/QoS and
    // confirm mode were all recorded.
    auto after = channel.value()->basic_get(queue, /*no_ack=*/true);
    if (!after) {
        // The reconnect happens inside this call and is bounded; a failure here
        // is a real failover failure, not a test artefact.
        report("basic.get after failover", after.error());
    }
    CRPC_CHECK(after.ok());
    if (!after) {
        return;
    }
    CRPC_CHECK(after.value().empty);
    CRPC_CHECK(session.value()->is_open());
    CRPC_CHECK_EQ(second, session.value()->active_node().port);
    std::printf("  failover: reconnected on %s:%u\n",
                session.value()->active_node().host.c_str(),
                static_cast<unsigned int>(session.value()->active_node().port));

    // The ORIGINAL consumer resumed on the standby: publish through the same
    // channel and read it with the same consumer tag. If the re-consume had not
    // happened, no delivery would ever arrive.
    auto after_pub = channel.value()->basic_publish_confirm(
        "", queue, message_properties(), bytes_of(std::string_view("after-failover")),
        /*mandatory=*/false, /*immediate=*/false, std::chrono::seconds(10));
    CRPC_CHECK(after_pub.ok());
    CRPC_CHECK(after_pub.ok() && after_pub.value().acknowledged);
    if (after_pub.ok() && after_pub.value().acknowledged) {
        auto delivered = channel.value()->next_delivery(std::chrono::seconds(5));
        if (!delivered) {
            report("basic.deliver after failover", delivered.error());
        }
        CRPC_CHECK(delivered.ok());
        if (delivered.ok()) {
            CRPC_CHECK(delivered.value().present);
            CRPC_CHECK_EQ(consumer_tag, delivered.value().consumer_tag);
            CRPC_CHECK_EQ(std::string("after-failover"),
                          std::string(reinterpret_cast<const char*>(
                                          delivered.value().body.data()),
                                      delivered.value().body.size()));
            CRPC_CHECK(channel.value()->basic_ack(delivered.value().delivery_tag).ok());
        }
    }

    // Leave no durable queue on the standby.
    CRPC_CHECK(channel.value()->basic_cancel(consumer_tag).ok());
    CRPC_CHECK(channel.value()->queue_delete(queue).ok());

    // Self-heal: restore the primary and wait until it accepts connections.
    // The destructor also restores it if an assertion fails above, but the
    // wait below proves the machine is left in its original state.
    if (guard.stopped) {
        std::string restart;
        const bool restarted = run_wsl("sudo systemctl start rabbitmq-server", restart);
        std::printf("  failover: node1 restart %s\n",
                    restarted ? "succeeded" : "FAILED");
        std::uint32_t node1_cleanup = 0;
        if (restarted) {
            bool back = false;
            for (int attempt = 0; attempt < 60 && !back; ++attempt) {
                connection_options probe = options_from_env();
                probe.host = options.host;
                probe.port = options.port;
                probe.operation_timeout = std::chrono::milliseconds(600);
                const auto revived = crpc::mq::amqp::connection::open(probe, backend);
                if (revived.ok()) {
                    back = true;
                    // The primary still holds its copy of the durable queue.
                    auto cleanup = revived.value()->open_channel();
                    if (cleanup.ok()) {
                        const crpc::status removed = cleanup.value()->queue_delete(queue);
                        if (!removed) {
                            report("queue.delete primary copy", removed.error());
                        }
                        node1_cleanup = removed.ok() ? 1u : 0u;
                    }
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
            }
            CRPC_CHECK(back);
        } else {
            CRPC_CHECK(restarted);
        }
        std::printf("  failover: node1 queue cleanup %s\n",
                    node1_cleanup != 0 ? "ok" : "skipped-or-failed");
        guard.stopped = false;  // already restored here; the guard must not repeat it
    }
}

CRPC_TEST_MAIN()