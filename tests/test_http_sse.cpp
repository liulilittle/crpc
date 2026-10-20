// CRPC -- tests for the HTTP/1.1 Server-Sent Events subset.
//
// Coverage:
//   * SSE framing: id/data encoding (text, multi-line, binary/base64),
//     heartbeat, and strict Last-Event-ID parsing;
//   * end-to-end: handshake headers (chunked, no Content-Length), publish ->
//     id/data delivery, heartbeat while otherwise idle, idle-timeout exemption;
//   * reconnect: cursor clamped to the server's delivered high water, ack then
//     attach (no re-subscribe), only unacknowledged events replayed, epoch
//     change not acknowledged;
//   * backpressure: the pump stops at the per-session bound, a taken message is
//     parked rather than dropped, publish never blocks, hub accounting is
//     conserved, and reading again resumes delivery;
//   * lifecycle: peer close and heartbeat write failure detach the hub token;
//   * HTTP/2 requests to an SSE path fail closed with one complete response.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../src/http/longpoll_sweep.hpp"

#include <crpc/http/server.hpp>
#include <crpc/http/sse.hpp>
#include <crpc/http2/connection.hpp>
#include <crpc/http2/frame.hpp>
#include <crpc/http2/hpack.hpp>
#include <crpc/io.hpp>
#include <crpc/mvc/router.hpp>

#include "test_framework.hpp"

using namespace crpc;

namespace {

namespace h2 = crpc::http2;

constexpr std::string_view h2_preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

// --- Client capture ----------------------------------------------------------

struct capture_source final : io::source {
    std::mutex mu;
    std::string data;
    bool eof = false;

    std::size_t on_data(span<const std::byte> bytes) noexcept override {
        std::lock_guard<std::mutex> lk(mu);
        data.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return bytes.size();
    }
    void on_eof() noexcept override {
        std::lock_guard<std::mutex> lk(mu);
        eof = true;
    }
    void on_error(std::error_code) noexcept override {
        std::lock_guard<std::mutex> lk(mu);
        eof = true;
    }
    [[nodiscard]] std::string snapshot() {
        std::lock_guard<std::mutex> lk(mu);
        return data;
    }
    [[nodiscard]] bool closed() {
        std::lock_guard<std::mutex> lk(mu);
        return eof;
    }
};

// A source that models a peer which stops reading. While closed every offered
// chunk is refused; the inproc backend retains it until `resume_read`.
struct gated_source final : io::source {
    std::mutex mu;
    std::string data;
    std::atomic<bool> open{true};
    std::atomic<bool> error_called{false};

    std::size_t on_data(span<const std::byte> bytes) noexcept override {
        std::lock_guard<std::mutex> lk(mu);
        if (!open.load(std::memory_order_acquire)) {
            return 0;
        }
        data.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return bytes.size();
    }
    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {
        error_called.store(true, std::memory_order_release);
    }
    [[nodiscard]] bool on_error_called() const {
        return error_called.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::string snapshot() {
        std::lock_guard<std::mutex> lk(mu);
        return data;
    }
};

struct noop_completion final : io::completion {
    void complete(io::io_result) noexcept override { delete this; }
};

void send_raw(connection& conn, std::string_view bytes) {
    auto* op = new noop_completion();
    conn.sink().push(span<const std::byte>(
                         reinterpret_cast<const std::byte*>(bytes.data()),
                         bytes.size()),
                     op);
}

std::string http1(std::string_view method, std::string_view path,
                  std::string_view extra = {}) {
    std::string out;
    out += method;
    out += " ";
    out += path;
    out += " HTTP/1.1\r\nHost: test\r\n";
    out += extra;
    out += "\r\n";
    return out;
}

std::size_t count_substring(std::string_view haystack, std::string_view needle) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

// --- SSE chunked response parser --------------------------------------------

struct sse_parser {
    std::string buffer;
    std::string head;
    std::string body;
    bool head_done = false;
    bool complete = false;
    std::size_t raw_consumed = 0;

    void pump(capture_source& source) {
        const std::string snapshot = source.snapshot();
        if (snapshot.size() <= raw_consumed) {
            return;
        }
        feed(std::string_view(snapshot).substr(raw_consumed));
        raw_consumed = snapshot.size();
    }

    void feed(std::string_view delta) {
        buffer.append(delta.data(), delta.size());
        if (!head_done) {
            const std::size_t end = buffer.find("\r\n\r\n");
            if (end == std::string::npos) {
                return;
            }
            head.assign(buffer.data(), end + 2);
            buffer.erase(0, end + 4);
            head_done = true;
        }
        for (;;) {
            const std::size_t line_end = buffer.find("\r\n");
            if (line_end == std::string::npos) {
                return;
            }
            std::size_t size = 0;
            bool valid = true;
            for (std::size_t i = 0; i < line_end; ++i) {
                const char c = buffer[i];
                std::size_t digit = 0;
                if (c >= '0' && c <= '9') {
                    digit = static_cast<std::size_t>(c - '0');
                } else if (c >= 'a' && c <= 'f') {
                    digit = static_cast<std::size_t>(c - 'a' + 10);
                } else if (c >= 'A' && c <= 'F') {
                    digit = static_cast<std::size_t>(c - 'A' + 10);
                } else {
                    valid = false;
                    break;
                }
                size = size * 16 + digit;
            }
            if (!valid) {
                buffer.erase(0, line_end + 2);
                continue;
            }
            if (size == 0) {
                complete = true;
                buffer.erase(0, line_end + 2);
                return;
            }
            if (buffer.size() < line_end + 2 + size + 2) {
                return;
            }
            body.append(buffer.data() + line_end + 2, size);
            buffer.erase(0, line_end + 2 + size + 2);
        }
    }

    [[nodiscard]] std::size_t events() const {
        return count_substring(body, "id: ");
    }
};

// --- Fixture -----------------------------------------------------------------

struct sse_fixture {
    inproc_backend backend;
    mvc::router router;
    std::unique_ptr<http::http_server> server;
    std::string endpoint_name;

    explicit sse_fixture(http::http_server_options options,
                         std::string path = "/events",
                         std::string topic = "news",
                         std::string name = "http-sse")
        : endpoint_name(std::move(name)) {
        const status frozen = router.freeze();
        CRPC_CHECK(frozen.ok());
        http::http_server::builder builder;
        builder.add_listener(endpoint::inproc(endpoint_name), backend);
        builder.router(router);
        builder.options(options);
        http::sse_route route;
        route.path = std::move(path);
        route.topic = std::move(topic);
        builder.add_sse_route(std::move(route));
        server = builder.build();
        CRPC_CHECK(server != nullptr);
        CRPC_CHECK(server->start().ok());
    }

    ~sse_fixture() {
        if (server) {
            server->stop();
        }
    }

    [[nodiscard]] std::unique_ptr<connection> connect() {
        return backend.connect(endpoint::inproc(endpoint_name),
                               connect_options{});
    }
};

bool publish_text(http::http_server& server, std::string_view topic,
                  std::string_view text) {
    return server.publish(
        topic,
        span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                              text.size()));
}

// --- Framing / Last-Event-ID -------------------------------------------------

CRPC_TEST(sse_framing_and_last_event_id) {
    http::sse::event_id id;
    CRPC_CHECK(http::sse::parse_last_event_id("7:42", id).ok());
    CRPC_CHECK_EQ(u64{7}, id.epoch);
    CRPC_CHECK_EQ(u64{42}, id.seq);

    const char* const bad[] = {
        "",        "7",          ":",          "7:",       ":1",
        "a:b",     "7:4x",       "7:-1",       "-1:2",     "+1:2",
        " 1:2",    "1 :2",       "1: 2",       "1:2:3",    "1:2\n",
        "18446744073709551616:1", "1:18446744073709551616",
        "22:99:",   "1:2:3:4",    "100000000000000000000000:1"};
    for (const char* value : bad) {
        http::sse::event_id ignored;
        CRPC_CHECK_FALSE(http::sse::parse_last_event_id(value, ignored).ok());
    }

    std::string frame;
    CRPC_CHECK(http::sse::encode_event({1, 3}, "hello", frame).ok());
    CRPC_CHECK_EQ(std::string("id: 1:3\ndata: hello\n\n"), frame);

    frame.clear();
    CRPC_CHECK(http::sse::encode_event({1, 4}, "a\nb\r\nc", frame).ok());
    CRPC_CHECK_EQ(std::string("id: 1:4\ndata: a\ndata: b\ndata: c\n\n"), frame);

    frame.clear();
    const std::byte binary[] = {std::byte{0}, std::byte{1}, std::byte{0xFF}};
    CRPC_CHECK(http::sse::encode_event({2, 9}, span<const std::byte>(binary, 3),
                                       frame)
                   .ok());
    CRPC_CHECK_EQ(std::string("id: 2:9\ndata: b64:AAH/\n\n"), frame);
    CRPC_CHECK_FALSE(http::sse::is_text(span<const std::byte>(binary, 3)));

    CRPC_CHECK_EQ(std::string(":hb\n\n"), http::sse::heartbeat());
    CRPC_CHECK_EQ(std::string("retry: 1500\n\n"),
                  http::sse::retry_hint(std::chrono::milliseconds(1500)));
}

// --- End to end --------------------------------------------------------------

CRPC_TEST(sse_http1_publish_and_heartbeat_with_idle_exempt) {
    http::http_server_options options;
    options.idle_timeout = std::chrono::milliseconds(60);
    options.request_timeout = std::chrono::milliseconds(0);
    options.sse.heartbeat = std::chrono::milliseconds(25);
    options.sse.slow_timeout = std::chrono::milliseconds(0);
    sse_fixture fx(options);

    capture_source capture;
    auto client = fx.connect();
    CRPC_CHECK(client != nullptr);
    client->bind_source(&capture);

    send_raw(*client, http1("GET", "/events?topic=news&subscriber=7"));

    sse_parser parser;
    const bool head = crpc_test::wait_for(
        [&] {
            parser.pump(capture);
            return parser.head_done;
        },
        5000);
    CRPC_CHECK(head);
    CRPC_CHECK(parser.head.find("200 OK") != std::string::npos);
    CRPC_CHECK(parser.head.find("Content-Type: text/event-stream") !=
               std::string::npos);
    CRPC_CHECK(parser.head.find("Transfer-Encoding: chunked") != std::string::npos);
    CRPC_CHECK(parser.head.find("Content-Length") == std::string::npos);

    // No inbound traffic: the idle timeout must be exempt and the heartbeat
    // must keep flowing on its own.
    const bool heartbeat = crpc_test::wait_for(
        [&] {
            parser.pump(capture);
            return parser.body.find(":hb") != std::string::npos;
        },
        5000);
    CRPC_CHECK(heartbeat);

    CRPC_CHECK(publish_text(*fx.server, "news", "hello"));
    const bool event = crpc_test::wait_for(
        [&] {
            parser.pump(capture);
            return parser.body.find("id: 1:1") != std::string::npos &&
                   parser.body.find("data: hello") != std::string::npos;
        },
        5000);
    CRPC_CHECK(event);
    CRPC_CHECK_FALSE(capture.closed());

    // Still alive well past the idle timeout.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    parser.pump(capture);
    CRPC_CHECK_FALSE(capture.closed());
    CRPC_CHECK_EQ(std::size_t{1},
                  fx.server->stats().sse_connections);
    CRPC_CHECK_EQ(std::size_t{1},
                  count_substring(parser.body, "data: hello"));
}

CRPC_TEST(sse_reconnect_replays_unacked_and_clamps_cursor) {
    http::http_server_options options;
    options.idle_timeout = std::chrono::milliseconds(0);
    options.request_timeout = std::chrono::milliseconds(0);
    options.sse.heartbeat = std::chrono::milliseconds(0);
    options.sse.slow_timeout = std::chrono::milliseconds(0);
    sse_fixture fx(options);

    capture_source first;
    auto client = fx.connect();
    client->bind_source(&first);
    send_raw(*client, http1("GET", "/events?topic=news&subscriber=9"));
    sse_parser first_parser;
    CRPC_CHECK(crpc_test::wait_for(
        [&] {
            first_parser.pump(first);
            return first_parser.head_done;
        },
        5000));
    CRPC_CHECK(publish_text(*fx.server, "news", "one"));
    CRPC_CHECK(publish_text(*fx.server, "news", "two"));
    CRPC_CHECK(crpc_test::wait_for(
        [&] {
            first_parser.pump(first);
            return first_parser.events() >= 2;
        },
        5000));
    CRPC_CHECK(first_parser.body.find("id: 1:2") != std::string::npos);

    client->close();
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return fx.server->stats().sse_connections == 0; }, 5000));

    // Published while detached: it is replayed after attach, not lost.
    CRPC_CHECK(publish_text(*fx.server, "news", "three"));

    // An over-large cursor (99 > delivered=2) must be clamped to 2: acking 99
    // would acknowledge events the server never delivered and silently drop
    // seq 3.
    capture_source second;
    auto again = fx.connect();
    again->bind_source(&second);
    send_raw(*again, http1("GET", "/events?topic=news&subscriber=9",
                           "Last-Event-ID: 1:99\r\n"));
    sse_parser second_parser;
    CRPC_CHECK(crpc_test::wait_for(
        [&] {
            second_parser.pump(second);
            return second_parser.head_done && second_parser.events() >= 1;
        },
        5000));
    CRPC_CHECK(second_parser.body.find("id: 1:3") != std::string::npos);
    CRPC_CHECK(second_parser.body.find("data: three") != std::string::npos);
    CRPC_CHECK(second_parser.body.find("id: 1:1") == std::string::npos);
    CRPC_CHECK(second_parser.body.find("id: 1:2") == std::string::npos);

    // Ack advance: reconnect with cursor 3, publish 4, replay only 4.
    again->close();
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return fx.server->stats().sse_connections == 0; }, 5000));
    CRPC_CHECK(publish_text(*fx.server, "news", "four"));
    capture_source third;
    auto last = fx.connect();
    last->bind_source(&third);
    send_raw(*last, http1("GET", "/events?topic=news&subscriber=9",
                          "Last-Event-ID: 1:3\r\n"));
    sse_parser third_parser;
    CRPC_CHECK(crpc_test::wait_for(
        [&] {
            third_parser.pump(third);
            return third_parser.head_done && third_parser.events() >= 1;
        },
        5000));
    CRPC_CHECK(third_parser.body.find("id: 1:4") != std::string::npos);
    CRPC_CHECK(third_parser.body.find("id: 1:1") == std::string::npos);
    CRPC_CHECK(third_parser.body.find("id: 1:2") == std::string::npos);
    CRPC_CHECK(third_parser.body.find("id: 1:3") == std::string::npos);
}

CRPC_TEST(sse_epoch_change_is_not_acknowledged) {
    inproc_backend backend;
    mvc::router router;
    CRPC_CHECK(router.freeze().ok());

    const auto build = [&](u64 epoch) -> std::unique_ptr<http::http_server> {
        http::http_server_options options;
        options.idle_timeout = std::chrono::milliseconds(0);
        options.request_timeout = std::chrono::milliseconds(0);
        options.sse.heartbeat = std::chrono::milliseconds(0);
        options.sse.slow_timeout = std::chrono::milliseconds(0);
        options.sse.epoch = epoch;
        http::http_server::builder builder;
        builder.add_listener(endpoint::inproc("http-sse-epoch"), backend);
        builder.router(router);
        builder.options(options);
        http::sse_route route;
        route.path = "/events";
        route.topic = "news";
        builder.add_sse_route(std::move(route));
        auto server = builder.build();
        CRPC_CHECK(server != nullptr);
        CRPC_CHECK(server->start().ok());
        return server;
    };

    {
        auto server = build(1);
        capture_source capture;
        auto client =
            backend.connect(endpoint::inproc("http-sse-epoch"), connect_options{});
        client->bind_source(&capture);
        send_raw(*client, http1("GET", "/events?topic=news&subscriber=42"));
        sse_parser parser;
        CRPC_CHECK(crpc_test::wait_for(
            [&] {
                parser.pump(capture);
                return parser.head_done;
            },
            5000));
        CRPC_CHECK(publish_text(*server, "news", "old"));
        CRPC_CHECK(crpc_test::wait_for(
            [&] {
                parser.pump(capture);
                return parser.body.find("id: 1:1") != std::string::npos;
            },
            5000));
        client->close();
        CRPC_CHECK(crpc_test::wait_for(
            [&] { return server->stats().sse_connections == 0; }, 5000));
        server->stop();
    }

    auto server = build(2);
    capture_source capture;
    auto client =
        backend.connect(endpoint::inproc("http-sse-epoch"), connect_options{});
    client->bind_source(&capture);
    // Cursor from epoch 1. It must not acknowledge anything in the new hub.
    send_raw(*client, http1("GET", "/events?topic=news&subscriber=42",
                            "Last-Event-ID: 1:1\r\n"));
    sse_parser parser;
    CRPC_CHECK(crpc_test::wait_for(
        [&] {
            parser.pump(capture);
            return parser.head_done;
        },
        5000));
    CRPC_CHECK(publish_text(*server, "news", "new"));
    CRPC_CHECK(crpc_test::wait_for(
        [&] {
            parser.pump(capture);
            return parser.body.find("data: new") != std::string::npos;
        },
        5000));
    CRPC_CHECK(parser.body.find("id: 2:1") != std::string::npos);
    CRPC_CHECK(parser.body.find("id: 1:1") == std::string::npos);
    server->stop();
}

// --- Backpressure ------------------------------------------------------------

CRPC_TEST(sse_backpressure_parks_and_resumes) {
    http::http_server_options options;
    options.egress_cap_bytes = 4096;
    options.idle_timeout = std::chrono::milliseconds(0);
    options.request_timeout = std::chrono::milliseconds(0);
    options.sse.heartbeat = std::chrono::milliseconds(0);
    options.sse.slow_timeout = std::chrono::milliseconds(10000);
    // Keep the replay window larger than the publish burst so the hub's
    // dropped_overflow counter contains queue drops only; the conservation
    // assertion below is then exact. Tail replay does not need
    // max_messages >= max_replay (that is a front-replay requirement only).
    options.sse.subscribe.max_replay = 4096;
    sse_fixture fx(options);

    gated_source gate;
    auto client = fx.connect();
    client->bind_source(&gate);
    send_raw(*client, http1("GET", "/events?topic=news&subscriber=11"));
    CRPC_CHECK(crpc_test::wait_for(
        [&] {
            const std::string data = gate.snapshot();
            return data.find("text/event-stream") != std::string::npos &&
                   data.find("\r\n\r\n") != std::string::npos;
        },
        5000));

    // The peer stops reading. Publishing must not block and must not grow the
    // server's write queue past the SSE bound; the hub owns every drop.
    gate.open.store(false, std::memory_order_release);
    const std::string payload(1024, 'x');
    for (int i = 0; i < 2000; ++i) {
        CRPC_CHECK(publish_text(*fx.server, "news", payload));
    }

    std::size_t last = 0;
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const std::size_t now = fx.server->stats().sse_events_delivered;
        if (now == last) {
            break;
        }
        last = now;
    }

    http::http_statistics stats = fx.server->stats();
    CRPC_CHECK_EQ(std::size_t{2000}, stats.sse_hub_published);
    CRPC_CHECK(stats.sse_events_delivered < 2000);
    CRPC_CHECK(stats.sse_hub_dropped > 0);
    CRPC_CHECK_EQ(stats.sse_hub_published,
                  stats.sse_hub_delivered + stats.sse_hub_queued +
                      stats.sse_hub_dropped);
    CRPC_CHECK_FALSE(gate.on_error_called());

    // Opening the gate resumes the pump with no permanent stall.
    const std::size_t expected = stats.sse_hub_published - stats.sse_hub_dropped;
    gate.open.store(true, std::memory_order_release);
    client->resume_read();

    sse_parser parser;
    std::size_t consumed = 0;
    const bool resumed = crpc_test::wait_for(
        [&] {
            const std::string data = gate.snapshot();
            if (data.size() > consumed) {
                parser.feed(std::string_view(data).substr(consumed));
                consumed = data.size();
            }
            return parser.events() >= expected;
        },
        20000);
    CRPC_CHECK(resumed);
    CRPC_CHECK_EQ(expected, parser.events());
}

// --- Lifecycle ---------------------------------------------------------------

CRPC_TEST(sse_peer_close_detaches_and_heartbeat_failure_closes) {
    http::http_server_options options;
    options.idle_timeout = std::chrono::milliseconds(0);
    options.request_timeout = std::chrono::milliseconds(0);
    options.sse.heartbeat = std::chrono::milliseconds(20);
    options.sse.slow_timeout = std::chrono::milliseconds(0);
    sse_fixture fx(options);

    capture_source capture;
    auto client = fx.connect();
    client->bind_source(&capture);
    send_raw(*client, http1("GET", "/events?topic=news&subscriber=13"));
    sse_parser parser;
    CRPC_CHECK(crpc_test::wait_for(
        [&] {
            parser.pump(capture);
            return parser.head_done &&
                   parser.body.find(":hb") != std::string::npos;
        },
        5000));
    CRPC_CHECK_EQ(std::size_t{1}, fx.server->stats().sse_connections);

    // Closing the peer makes the next heartbeat write fail (or delivers EOF).
    // Either path must detach the hub token before the session goes away.
    client->close();
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return fx.server->stats().sse_connections == 0; }, 5000));
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return fx.server->stats().active_connections == 0; }, 5000));
}

// --- HTTP/2 fail-closed ------------------------------------------------------

std::string h2_frame(h2::frame_type type, u8 flags, u32 stream_id,
                     std::string_view payload) {
    h2::frame_header header;
    header.type = type;
    header.flags = flags;
    header.stream_id = stream_id;
    header.length = static_cast<u32>(payload.size());
    std::string out(h2::frame_header_size + payload.size(), '\0');
    std::size_t written = 0;
    (void)h2::encode_frame(
        span<std::byte>(reinterpret_cast<std::byte*>(out.data()), out.size()),
        header,
        span<const std::byte>(
            reinterpret_cast<const std::byte*>(payload.data()), payload.size()),
        written);
    return out;
}

std::string h2_request(u32 stream_id, std::string_view method,
                       std::string_view path, h2::hpack_encoder& encoder) {
    std::vector<h2::header_field> fields;
    fields.push_back({":method", std::string(method)});
    fields.push_back({":scheme", "http"});
    fields.push_back({":path", std::string(path)});
    fields.push_back({":authority", "test"});
    std::string block;
    (void)encoder.encode(
        span<const h2::header_field>(fields.data(), fields.size()), block);
    return h2_frame(h2::frame_type::headers,
                    static_cast<u8>(h2::flags::end_headers |
                                    h2::flags::end_stream),
                    stream_id, block);
}

CRPC_TEST(h2_sse_route_fails_closed) {
    http::http_server_options options;
    sse_fixture fx(options);

    capture_source capture;
    auto client = fx.connect();
    client->bind_source(&capture);

    h2::hpack_encoder encoder;
    std::string out(h2_preface);
    out += h2_request(1, "GET", "/events?topic=news", encoder);
    send_raw(*client, out);

    h2::hpack_decoder decoder;
    std::string buf;
    std::string block;
    std::string body;
    bool in_headers = false;
    int status = 0;
    bool end_stream = false;
    std::size_t consumed = 0;

    const bool complete = crpc_test::wait_for(
        [&] {
            const std::string data = capture.snapshot();
            if (data.size() > consumed) {
                buf.append(data.data() + consumed, data.size() - consumed);
                consumed = data.size();
            }
            for (;;) {
                if (buf.size() < h2::frame_header_size) {
                    return end_stream && status != 0;
                }
                h2::frame_header frame;
                const span<const std::byte> head(
                    reinterpret_cast<const std::byte*>(buf.data()),
                    h2::frame_header_size);
                if (!h2::decode_frame_header(head, frame,
                                             h2::max_max_frame_size)
                         .ok()) {
                    return false;
                }
                const std::size_t total = h2::frame_header_size + frame.length;
                if (buf.size() < total) {
                    return end_stream && status != 0;
                }
                const span<const std::byte> payload(
                    reinterpret_cast<const std::byte*>(buf.data()) +
                        h2::frame_header_size,
                    frame.length);
                if (frame.type == h2::frame_type::headers) {
                    h2::headers_payload decoded;
                    if (!h2::decode_headers(payload, frame.flags, decoded).ok()) {
                        return false;
                    }
                    block.assign(
                        reinterpret_cast<const char*>(decoded.header_block.data()),
                        decoded.header_block.size());
                    in_headers = true;
                    end_stream = end_stream || decoded.end_stream;
                } else if (frame.type == h2::frame_type::continuation) {
                    block.append(reinterpret_cast<const char*>(payload.data()),
                                 payload.size());
                } else if (frame.type == h2::frame_type::data) {
                    span<const std::byte> decoded;
                    if (h2::decode_data(payload, frame.flags, decoded).ok()) {
                        body.append(reinterpret_cast<const char*>(decoded.data()),
                                    decoded.size());
                    }
                    end_stream = end_stream ||
                                 (frame.flags & h2::flags::end_stream) != 0;
                }
                if (in_headers &&
                    (frame.type == h2::frame_type::headers
                         ? (frame.flags & h2::flags::end_headers) != 0
                         : (frame.flags & h2::flags::end_headers) != 0)) {
                    http::header_list fields;
                    if (decoder
                            .decode(span<const std::byte>(
                                        reinterpret_cast<const std::byte*>(
                                            block.data()),
                                        block.size()),
                                    fields)
                            .ok()) {
                        for (const http::header& field : fields) {
                            if (field.name == ":status") {
                                status = std::atoi(field.value.c_str());
                            }
                        }
                    }
                    in_headers = false;
                }
                buf.erase(0, total);
            }
        },
        5000);

    CRPC_CHECK(complete);
    CRPC_CHECK_EQ(501, status);
    CRPC_CHECK(body.find("not supported") != std::string::npos);
    CRPC_CHECK(end_stream);
    CRPC_CHECK_EQ(std::size_t{0}, fx.server->stats().sse_connections);
}

// --- HTTP/1.1 long polling ---------------------------------------------------

namespace {

struct lp_response {
    int status = 0;
    std::string head;
    std::string body;
};

bool header_name_is(std::string_view text, std::string_view name) {
    if (text.size() != name.size()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        char a = text[i];
        char b = name[i];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<char>(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

// Parses a sequence of fixed-length (or bodyless) HTTP/1.1 responses from a
// capture source. Long polling uses Content-Length or 204, never chunked.
struct lp_parser {
    std::string buffer;
    std::vector<lp_response> responses;
    std::size_t raw_consumed = 0;

    void pump(capture_source& source) {
        const std::string snapshot = source.snapshot();
        if (snapshot.size() <= raw_consumed) {
            return;
        }
        buffer.append(snapshot.data() + raw_consumed,
                      snapshot.size() - raw_consumed);
        raw_consumed = snapshot.size();
        parse();
    }

    void parse() {
        for (;;) {
            const std::size_t end = buffer.find("\r\n\r\n");
            if (end == std::string::npos) {
                return;
            }
            const std::size_t first_space = buffer.find(' ');
            int status = 0;
            if (first_space != std::string::npos) {
                status = std::atoi(buffer.c_str() + first_space + 1);
            }
            const std::string head = buffer.substr(0, end + 2);
            std::size_t content_length = 0;
            bool has_length = false;
            std::size_t pos = buffer.find("\r\n");
            while (pos != std::string::npos && pos + 2 <= end) {
                const std::size_t line_end = buffer.find("\r\n", pos + 2);
                if (line_end == std::string::npos || line_end > end) {
                    break;
                }
                const std::string_view line(buffer.data() + pos + 2,
                                            line_end - (pos + 2));
                const std::size_t colon = line.find(':');
                if (colon != std::string_view::npos &&
                    header_name_is(line.substr(0, colon), "content-length")) {
                    std::string_view value = line.substr(colon + 1);
                    while (!value.empty() && (value.front() == ' ' ||
                                              value.front() == '\t')) {
                        value.remove_prefix(1);
                    }
                    content_length = static_cast<std::size_t>(
                        std::strtoull(std::string(value).c_str(), nullptr, 10));
                    has_length = true;
                }
                pos = line_end;
            }
            const std::size_t body_start = end + 4;
            if (status == 204) {
                lp_response response;
                response.status = status;
                response.head = head;
                responses.push_back(std::move(response));
                buffer.erase(0, body_start);
                continue;
            }
            if (!has_length || buffer.size() < body_start + content_length) {
                return;
            }
            lp_response response;
            response.status = status;
            response.head = head;
            response.body.assign(buffer.data() + body_start, content_length);
            responses.push_back(std::move(response));
            buffer.erase(0, body_start + content_length);
        }
    }
};

struct longpoll_fixture {
    inproc_backend backend;
    mvc::router router;
    std::unique_ptr<http::http_server> server;
    std::string endpoint_name;

    explicit longpoll_fixture(http::http_server_options options,
                              std::string name = "http-lp",
                              bool with_sse = false)
        : endpoint_name(std::move(name)) {
        const status frozen = router.freeze();
        CRPC_CHECK(frozen.ok());
        http::http_server::builder builder;
        builder.add_listener(endpoint::inproc(endpoint_name), backend);
        builder.router(router);
        builder.options(options);
        http::longpoll_route route;
        route.path = "/poll";
        route.topic = "news";
        builder.add_longpoll_route(std::move(route));
        if (with_sse) {
            http::sse_route sse;
            sse.path = "/events";
            sse.topic = "news";
            builder.add_sse_route(std::move(sse));
        }
        server = builder.build();
        CRPC_CHECK(server != nullptr);
        CRPC_CHECK(server->start().ok());
    }

    ~longpoll_fixture() {
        if (server) {
            server->stop();
        }
    }

    [[nodiscard]] std::unique_ptr<connection> connect() {
        return backend.connect(endpoint::inproc(endpoint_name),
                               connect_options{});
    }
};

bool wait_polls(lp_parser& parser, capture_source& capture,
                std::size_t count, int timeout_ms = 5000) {
    return crpc_test::wait_for(
        [&] {
            parser.pump(capture);
            return parser.responses.size() >= count;
        },
        timeout_ms);
}

std::string poll_target(u64 subscriber, std::string_view extra = {}) {
    std::string target = "/poll?topic=news&subscriber=";
    target += std::to_string(subscriber);
    if (!extra.empty()) {
        target += "&";
        target += extra;
    }
    return target;
}

} // namespace

CRPC_TEST(longpoll_immediate_200_timeout_204_keepalive) {
    http::http_server_options options;
    options.idle_timeout = std::chrono::milliseconds(0);
    options.request_timeout = std::chrono::milliseconds(0);
    options.longpoll.timeout = std::chrono::milliseconds(100);
    options.longpoll.max_timeout = std::chrono::milliseconds(500);
    options.longpoll.slow_timeout = std::chrono::milliseconds(0);
    longpoll_fixture fx(options);
    // Defaults (256/1024) are accepted and equalized to the queue bound.
    CRPC_CHECK_EQ(std::size_t{1024},
                  fx.server->stats().longpoll_replay_window);

    const u64 subscriber = (1ull << 32) + 7;
    capture_source capture;
    auto client = fx.connect();
    CRPC_CHECK(client != nullptr);
    client->bind_source(&capture);
    lp_parser parser;

    // Subscribe with a zero timeout: no message, so a bodyless 204 that still
    // carries the server-issued subscriber id.
    send_raw(*client, http1("GET",
                            poll_target(subscriber, "timeout=0"),
                            "Accept-Encoding: gzip\r\n"));
    CRPC_CHECK(wait_polls(parser, capture, 1));
    CRPC_CHECK_EQ(204, parser.responses[0].status);
    CRPC_CHECK(parser.responses[0].body.empty());
    CRPC_CHECK(parser.responses[0].head.find("Content-Length") ==
               std::string::npos);
    CRPC_CHECK(parser.responses[0].head.find(
                   "X-CRPC-Subscriber: " + std::to_string(subscriber)) !=
               std::string::npos);

    CRPC_CHECK(publish_text(*fx.server, "news", "one"));
    send_raw(*client, http1("GET",
                            poll_target(subscriber, "timeout=0"),
                            "Accept-Encoding: gzip\r\n"));
    CRPC_CHECK(wait_polls(parser, capture, 2));
    const lp_response& event = parser.responses[1];
    CRPC_CHECK_EQ(200, event.status);
    CRPC_CHECK_EQ(std::string("one"), event.body);
    CRPC_CHECK(event.head.find("Content-Length: 3") != std::string::npos);
    CRPC_CHECK(event.head.find("X-CRPC-Event-ID: 1:1") != std::string::npos);
    CRPC_CHECK(event.head.find("X-CRPC-Subscriber: " +
                               std::to_string(subscriber)) !=
               std::string::npos);
    CRPC_CHECK(event.head.find("Cache-Control:") != std::string::npos);
    CRPC_CHECK(event.head.find("Content-Encoding") == std::string::npos);
    CRPC_CHECK(event.head.find("Vary") == std::string::npos);

    // No new message: a 100 ms wait ends in 204, on the same keep-alive
    // connection.
    const auto begin = std::chrono::steady_clock::now();
    send_raw(*client, http1("GET",
                            poll_target(subscriber),
                            "Last-Event-ID: 1:1\r\nX-Lp: wait\r\n"));
    CRPC_CHECK(wait_polls(parser, capture, 3));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - begin)
                             .count();
    CRPC_CHECK_EQ(204, parser.responses[2].status);
    CRPC_CHECK(parser.responses[2].body.empty());
    CRPC_CHECK(elapsed >= 50);
    CRPC_CHECK(elapsed < 2000);
    CRPC_CHECK_EQ(std::size_t{1}, fx.server->stats().active_connections);

    CRPC_CHECK(publish_text(*fx.server, "news", "two"));
    send_raw(*client, http1("GET",
                            poll_target(subscriber, "timeout=0"),
                            "Last-Event-ID: 1:1\r\n"));
    CRPC_CHECK(wait_polls(parser, capture, 4));
    CRPC_CHECK_EQ(200, parser.responses[3].status);
    CRPC_CHECK_EQ(std::string("two"), parser.responses[3].body);
    CRPC_CHECK(parser.responses[3].head.find("X-CRPC-Event-ID: 1:2") !=
               std::string::npos);
    client->close();
}

CRPC_TEST(longpoll_waiting_publish_wakes_immediately) {
    http::http_server_options options;
    options.idle_timeout = std::chrono::milliseconds(0);
    options.request_timeout = std::chrono::milliseconds(0);
    options.longpoll.timeout = std::chrono::milliseconds(5000);
    options.longpoll.max_timeout = std::chrono::milliseconds(5000);
    options.longpoll.slow_timeout = std::chrono::milliseconds(0);
    longpoll_fixture fx(options);

    const u64 subscriber = (1ull << 32) + 11;
    capture_source capture;
    auto client = fx.connect();
    client->bind_source(&capture);
    lp_parser parser;
    send_raw(*client, http1("GET", poll_target(subscriber, "timeout=0")));
    CRPC_CHECK(wait_polls(parser, capture, 1));
    CRPC_CHECK_EQ(204, parser.responses[0].status);

    send_raw(*client, http1("GET", poll_target(subscriber)));
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return fx.server->stats().longpoll_waiters == 1; }, 2000));
    const auto begin = std::chrono::steady_clock::now();
    CRPC_CHECK(publish_text(*fx.server, "news", "wake"));
    CRPC_CHECK(wait_polls(parser, capture, 2, 2000));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - begin)
                             .count();
    CRPC_CHECK(elapsed < 1000);
    CRPC_CHECK_EQ(200, parser.responses[1].status);
    CRPC_CHECK_EQ(std::string("wake"), parser.responses[1].body);
    CRPC_CHECK(parser.responses[1].head.find("X-CRPC-Event-ID: 1:1") !=
               std::string::npos);
    CRPC_CHECK_EQ(std::size_t{1}, fx.server->stats().longpoll_events);
    client->close();
}

CRPC_TEST(longpoll_burst_keeps_queue_and_order) {
    http::http_server_options options;
    options.idle_timeout = std::chrono::milliseconds(0);
    options.request_timeout = std::chrono::milliseconds(0);
    options.longpoll.timeout = std::chrono::milliseconds(0);
    options.longpoll.max_timeout = std::chrono::milliseconds(0);
    options.longpoll.slow_timeout = std::chrono::milliseconds(0);
    options.longpoll.subscribe.max_replay = 16;
    longpoll_fixture fx(options);

    const u64 subscriber = (1ull << 32) + 13;
    capture_source capture;
    auto client = fx.connect();
    client->bind_source(&capture);
    lp_parser parser;
    send_raw(*client, http1("GET", poll_target(subscriber, "timeout=0")));
    CRPC_CHECK(wait_polls(parser, capture, 1));
    CRPC_CHECK_EQ(204, parser.responses[0].status);

    CRPC_CHECK(publish_text(*fx.server, "news", "m1"));
    CRPC_CHECK(publish_text(*fx.server, "news", "m2"));
    CRPC_CHECK(publish_text(*fx.server, "news", "m3"));

    for (int i = 1; i <= 3; ++i) {
        std::string extra = "timeout=0";
        if (i > 1) {
            extra += "&cursor=1:";
            extra += std::to_string(i - 1);
        }
        send_raw(*client, http1("GET", poll_target(subscriber, extra)));
        CRPC_CHECK(wait_polls(parser, capture,
                              static_cast<std::size_t>(i) + 1));
        const lp_response& response =
            parser.responses[static_cast<std::size_t>(i)];
        CRPC_CHECK_EQ(200, response.status);
        CRPC_CHECK_EQ("m" + std::to_string(i), response.body);
        CRPC_CHECK(response.head.find("X-CRPC-Event-ID: 1:" +
                                      std::to_string(i)) != std::string::npos);
    }
    send_raw(*client, http1("GET",
                            poll_target(subscriber,
                                        "timeout=0&cursor=1:3")));
    CRPC_CHECK(wait_polls(parser, capture, 5));
    CRPC_CHECK_EQ(204, parser.responses[4].status);
    CRPC_CHECK_EQ(std::size_t{3}, fx.server->stats().longpoll_events);
    client->close();
}

CRPC_TEST(longpoll_lost_response_retransmits_front) {
    http::http_server_options options;
    options.idle_timeout = std::chrono::milliseconds(0);
    options.request_timeout = std::chrono::milliseconds(0);
    options.longpoll.timeout = std::chrono::milliseconds(0);
    options.longpoll.max_timeout = std::chrono::milliseconds(0);
    options.longpoll.slow_timeout = std::chrono::milliseconds(0);
    options.longpoll.subscribe.max_replay = 16;
    longpoll_fixture fx(options);

    const u64 subscriber = (1ull << 32) + 17;
    capture_source capture;
    auto client = fx.connect();
    client->bind_source(&capture);
    lp_parser parser;
    send_raw(*client, http1("GET", poll_target(subscriber, "timeout=0")));
    CRPC_CHECK(wait_polls(parser, capture, 1));
    CRPC_CHECK(publish_text(*fx.server, "news", "a"));
    CRPC_CHECK(publish_text(*fx.server, "news", "b"));
    CRPC_CHECK(publish_text(*fx.server, "news", "c"));

    // seq1 is delivered but the response is lost (no ack). The next cursor is
    // 1:0, so the server must put seq1 back at the FRONT.
    send_raw(*client, http1("GET", poll_target(subscriber, "timeout=0")));
    CRPC_CHECK(wait_polls(parser, capture, 2));
    CRPC_CHECK_EQ(std::string("a"), parser.responses[1].body);
    send_raw(*client, http1("GET",
                            poll_target(subscriber, "timeout=0&cursor=1:0")));
    CRPC_CHECK(wait_polls(parser, capture, 3));
    CRPC_CHECK_EQ(std::string("a"), parser.responses[2].body);
    CRPC_CHECK(parser.responses[2].head.find("X-CRPC-Event-ID: 1:1") !=
               std::string::npos);

    send_raw(*client, http1("GET",
                            poll_target(subscriber, "timeout=0&cursor=1:1")));
    CRPC_CHECK(wait_polls(parser, capture, 4));
    CRPC_CHECK_EQ(std::string("b"), parser.responses[3].body);
    CRPC_CHECK(parser.responses[3].head.find("X-CRPC-Event-ID: 1:2") !=
               std::string::npos);

    send_raw(*client, http1("GET",
                            poll_target(subscriber, "timeout=0&cursor=1:2")));
    CRPC_CHECK(wait_polls(parser, capture, 5));
    CRPC_CHECK_EQ(std::string("c"), parser.responses[4].body);
    CRPC_CHECK(parser.responses[4].head.find("X-CRPC-Event-ID: 1:3") !=
               std::string::npos);
    client->close();
}

CRPC_TEST(longpoll_cursor_clamp_and_epoch) {
    http::http_server_options options;
    options.idle_timeout = std::chrono::milliseconds(0);
    options.request_timeout = std::chrono::milliseconds(0);
    options.longpoll.timeout = std::chrono::milliseconds(0);
    options.longpoll.max_timeout = std::chrono::milliseconds(0);
    options.longpoll.slow_timeout = std::chrono::milliseconds(0);
    options.longpoll.subscribe.max_replay = 16;
    longpoll_fixture fx(options);

    const u64 subscriber = (1ull << 32) + 19;
    capture_source capture;
    auto client = fx.connect();
    client->bind_source(&capture);
    lp_parser parser;
    send_raw(*client, http1("GET", poll_target(subscriber, "timeout=0")));
    CRPC_CHECK(wait_polls(parser, capture, 1));
    CRPC_CHECK(publish_text(*fx.server, "news", "a"));
    send_raw(*client, http1("GET", poll_target(subscriber, "timeout=0")));
    CRPC_CHECK(wait_polls(parser, capture, 2));
    CRPC_CHECK_EQ(std::string("a"), parser.responses[1].body);

    CRPC_CHECK(publish_text(*fx.server, "news", "b"));
    // A cursor beyond the delivered high water is clamped to what was actually
    // sent (1), so seq2 is not silently acknowledged.
    send_raw(*client, http1("GET",
                            poll_target(subscriber, "timeout=0&cursor=1:99")));
    CRPC_CHECK(wait_polls(parser, capture, 3));
    CRPC_CHECK_EQ(std::string("b"), parser.responses[2].body);
    CRPC_CHECK_EQ(std::size_t{1}, fx.server->stats().longpoll_ack_clamped);

    CRPC_CHECK(publish_text(*fx.server, "news", "c"));
    // Wrong epoch: no ack. The next message is still delivered.
    send_raw(*client, http1("GET",
                            poll_target(subscriber, "timeout=0&cursor=2:2")));
    CRPC_CHECK(wait_polls(parser, capture, 4));
    CRPC_CHECK_EQ(std::string("c"), parser.responses[3].body);
    CRPC_CHECK(parser.responses[3].head.find("X-CRPC-Event-ID: 1:3") !=
               std::string::npos);

    // Correct epoch, cursor 1:1. seq2 was never acknowledged (the epoch-2
    // cursor did not ack), so the front replay must deliver seq2 again.
    send_raw(*client, http1("GET",
                            poll_target(subscriber, "timeout=0&cursor=1:1")));
    CRPC_CHECK(wait_polls(parser, capture, 5));
    CRPC_CHECK_EQ(std::string("b"), parser.responses[4].body);
    CRPC_CHECK(parser.responses[4].head.find("X-CRPC-Event-ID: 1:2") !=
               std::string::npos);
    client->close();
}

CRPC_TEST(longpoll_same_subscriber_conflict_409) {
    http::http_server_options options;
    options.idle_timeout = std::chrono::milliseconds(0);
    options.request_timeout = std::chrono::milliseconds(0);
    options.longpoll.timeout = std::chrono::milliseconds(5000);
    options.longpoll.max_timeout = std::chrono::milliseconds(5000);
    options.longpoll.slow_timeout = std::chrono::milliseconds(0);
    longpoll_fixture fx(options);

    const u64 subscriber = (1ull << 32) + 23;
    capture_source first_capture;
    auto first = fx.connect();
    first->bind_source(&first_capture);
    lp_parser first_parser;
    send_raw(*first, http1("GET", poll_target(subscriber, "timeout=0")));
    CRPC_CHECK(wait_polls(first_parser, first_capture, 1));
    CRPC_CHECK_EQ(204, first_parser.responses[0].status);
    send_raw(*first, http1("GET", poll_target(subscriber)));
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return fx.server->stats().longpoll_waiters == 1; }, 2000));

    capture_source second_capture;
    auto second = fx.connect();
    second->bind_source(&second_capture);
    lp_parser second_parser;
    send_raw(*second, http1("GET", poll_target(subscriber, "timeout=0")));
    CRPC_CHECK(wait_polls(second_parser, second_capture, 1));
    CRPC_CHECK_EQ(409, second_parser.responses[0].status);
    CRPC_CHECK_EQ(std::size_t{1}, fx.server->stats().longpoll_conflicts);

    // The first poll is untouched and still receives its event.
    CRPC_CHECK(publish_text(*fx.server, "news", "go"));
    CRPC_CHECK(wait_polls(first_parser, first_capture, 2));
    CRPC_CHECK_EQ(200, first_parser.responses[1].status);
    CRPC_CHECK_EQ(std::string("go"), first_parser.responses[1].body);
    first->close();
    second->close();
}

CRPC_TEST(longpoll_max_waiters_503_then_recovers) {
    http::http_server_options options;
    options.idle_timeout = std::chrono::milliseconds(0);
    options.request_timeout = std::chrono::milliseconds(0);
    options.longpoll.timeout = std::chrono::milliseconds(5000);
    options.longpoll.max_timeout = std::chrono::milliseconds(5000);
    options.longpoll.max_waiters = 1;
    options.longpoll.slow_timeout = std::chrono::milliseconds(0);
    longpoll_fixture fx(options);

    const u64 first_id = (1ull << 32) + 29;
    const u64 second_id = (1ull << 32) + 31;
    capture_source first_capture;
    auto first = fx.connect();
    first->bind_source(&first_capture);
    lp_parser first_parser;
    send_raw(*first, http1("GET", poll_target(first_id, "timeout=0")));
    CRPC_CHECK(wait_polls(first_parser, first_capture, 1));
    send_raw(*first, http1("GET", poll_target(first_id)));
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return fx.server->stats().longpoll_waiters == 1; }, 2000));

    capture_source second_capture;
    auto second = fx.connect();
    second->bind_source(&second_capture);
    lp_parser second_parser;
    send_raw(*second, http1("GET",
                            poll_target(second_id, "timeout=1000")));
    CRPC_CHECK(wait_polls(second_parser, second_capture, 1));
    CRPC_CHECK_EQ(503, second_parser.responses[0].status);
    CRPC_CHECK(second_parser.responses[0].head.find("Retry-After: 1") !=
               std::string::npos);
    CRPC_CHECK_EQ(std::size_t{1}, fx.server->stats().longpoll_overloaded);

    first->close();
    second->close();
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return fx.server->stats().longpoll_waiters == 0; }, 5000));

    capture_source third_capture;
    auto third = fx.connect();
    third->bind_source(&third_capture);
    lp_parser third_parser;
    send_raw(*third,
             http1("GET", poll_target((1ull << 32) + 37, "timeout=0")));
    CRPC_CHECK(wait_polls(third_parser, third_capture, 1));
    CRPC_CHECK_EQ(204, third_parser.responses[0].status);
    third->close();
}

CRPC_TEST(longpoll_ttl_sweeps_idle_subscriber) {
    http::http_server_options options;
    options.idle_timeout = std::chrono::milliseconds(0);
    options.request_timeout = std::chrono::milliseconds(0);
    options.longpoll.timeout = std::chrono::milliseconds(0);
    options.longpoll.max_timeout = std::chrono::milliseconds(0);
    options.longpoll.subscriber_ttl = std::chrono::milliseconds(50);
    options.longpoll.slow_timeout = std::chrono::milliseconds(0);
    longpoll_fixture fx(options);

    const u64 subscriber = (1ull << 32) + 41;
    {
        capture_source capture;
        auto client = fx.connect();
        client->bind_source(&capture);
        lp_parser parser;
        send_raw(*client, http1("GET", poll_target(subscriber, "timeout=0")));
        CRPC_CHECK(wait_polls(parser, capture, 1));
        CRPC_CHECK_EQ(204, parser.responses[0].status);
        CRPC_CHECK_EQ(std::size_t{1}, fx.server->stats().longpoll_subscribers);
        client->close();
    }
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return fx.server->stats().active_connections == 0; }, 5000));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // This publish lands in the stale subscriber's queue; the sweep on the next
    // poll must drop the subscription and that queue.
    CRPC_CHECK(publish_text(*fx.server, "news", "late"));

    capture_source capture;
    auto client = fx.connect();
    client->bind_source(&capture);
    lp_parser parser;
    send_raw(*client, http1("GET", "/poll?topic=news&timeout=0"));
    CRPC_CHECK(wait_polls(parser, capture, 1));
    CRPC_CHECK_EQ(204, parser.responses[0].status);
    CRPC_CHECK_EQ(std::size_t{1}, fx.server->stats().longpoll_subscribers);
    const http::http_statistics stats = fx.server->stats();
    CRPC_CHECK_EQ(std::size_t{0}, stats.sse_hub_queued);

    // Reusing the expired id must yield a fresh server id: the old sequence
    // space is gone, so the client must reset its de-duplication state.
    send_raw(*client, http1("GET", poll_target(subscriber, "timeout=0")));
    CRPC_CHECK(wait_polls(parser, capture, 2));
    CRPC_CHECK_EQ(204, parser.responses[1].status);
    const std::string prefix = "X-CRPC-Subscriber: ";
    const std::size_t pos = parser.responses[1].head.find(prefix);
    CRPC_CHECK(pos != std::string::npos);
    u64 fresh = 0;
    if (pos != std::string::npos) {
        fresh = std::strtoull(parser.responses[1].head.c_str() + pos +
                                  prefix.size(),
                              nullptr, 10);
    }
    CRPC_CHECK(fresh != subscriber);
    CRPC_CHECK(fresh >= (1ull << 32) && fresh < (1ull << 33));
    client->close();
}

CRPC_TEST(longpoll_sweep_union_covers_owner_residues) {
    std::unordered_map<u64, int> owners;
    std::unordered_map<u64, u64> last_seen;
    std::unordered_set<u64> busy;
    constexpr u64 now = 1000;
    constexpr u64 ttl = 100;

    last_seen[1] = 800;        // ownerless, expired
    last_seen[2] = 950;        // ownerless, fresh
    owners[3] = 0;             // owner + expired last_seen
    last_seen[3] = 800;
    owners[4] = 0;             // owner without last_seen (defensive residue)
    owners[5] = 0;             // owner + fresh last_seen
    last_seen[5] = 950;
    last_seen[6] = 800;        // ownerless but busy
    busy.insert(6);
    owners[7] = 0;             // owner + busy
    last_seen[7] = 800;
    busy.insert(7);

    std::vector<u64> expired;
    crpc::http::detail::collect_expired_longpoll_ids(
        owners, last_seen, busy, now, ttl, expired);
    std::sort(expired.begin(), expired.end());
    const std::vector<u64> expected{1, 3, 4};
    CRPC_CHECK(expired == expected);
}

CRPC_TEST(longpoll_ttl_housekeeping_without_poll) {
    http::http_server_options options;
    options.idle_timeout = std::chrono::milliseconds(0);
    options.request_timeout = std::chrono::milliseconds(0);
    options.longpoll.timeout = std::chrono::milliseconds(0);
    options.longpoll.max_timeout = std::chrono::milliseconds(0);
    options.longpoll.subscriber_ttl = std::chrono::milliseconds(300);
    options.longpoll.slow_timeout = std::chrono::milliseconds(0);
    longpoll_fixture fx(options);

    const u64 subscriber = (1ull << 32) + 79;
    {
        capture_source capture;
        auto client = fx.connect();
        client->bind_source(&capture);
        lp_parser parser;
        send_raw(*client, http1("GET", poll_target(subscriber, "timeout=0")));
        CRPC_CHECK(wait_polls(parser, capture, 1));
        CRPC_CHECK_EQ(204, parser.responses[0].status);
        client->close();
    }
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return fx.server->stats().active_connections == 0; }, 5000));

    // The subscription is kept by the keep-queue release, so a publish lands
    // in its queue...
    CRPC_CHECK(publish_text(*fx.server, "news", "late"));
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return fx.server->stats().sse_hub_queued == 1; }, 2000));
    // ...and the shard housekeeping timer removes it after the TTL even though
    // no later poll arrives.
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return fx.server->stats().sse_hub_queued == 0; }, 5000));
    CRPC_CHECK_EQ(std::size_t{0}, fx.server->stats().longpoll_subscribers);
}

CRPC_TEST(longpoll_queue_bound_covers_replay_window) {
    http::http_server_options options;
    options.idle_timeout = std::chrono::milliseconds(0);
    options.request_timeout = std::chrono::milliseconds(0);
    options.longpoll.timeout = std::chrono::milliseconds(0);
    options.longpoll.max_timeout = std::chrono::milliseconds(0);
    options.longpoll.slow_timeout = std::chrono::milliseconds(0);
    // A smaller replay bound is raised to the queue bound at start(), so the
    // queue always holds the whole replay window. (A larger replay bound is
    // rejected by longpoll_replay_over_queue_rejected_at_start.)
    options.longpoll.subscribe.max_messages = 4;
    options.longpoll.subscribe.max_replay = 2;
    longpoll_fixture fx(options);
    CRPC_CHECK_EQ(std::size_t{4},
                  fx.server->stats().longpoll_replay_window);

    const u64 subscriber = (1ull << 32) + 83;
    capture_source capture;
    auto client = fx.connect();
    client->bind_source(&capture);
    lp_parser parser;
    send_raw(*client, http1("GET", poll_target(subscriber, "timeout=0")));
    CRPC_CHECK(wait_polls(parser, capture, 1));
    CRPC_CHECK_EQ(204, parser.responses[0].status);
    CRPC_CHECK(publish_text(*fx.server, "news", "m1"));
    CRPC_CHECK(publish_text(*fx.server, "news", "m2"));
    CRPC_CHECK(publish_text(*fx.server, "news", "m3"));
    CRPC_CHECK(publish_text(*fx.server, "news", "m4"));

    // The normalized queue holds all four messages, so the first poll is m1.
    send_raw(*client, http1("GET", poll_target(subscriber, "timeout=0")));
    CRPC_CHECK(wait_polls(parser, capture, 2));
    CRPC_CHECK_EQ(200, parser.responses[1].status);
    CRPC_CHECK_EQ(std::string("m1"), parser.responses[1].body);

    // The lost response is replayed from the front; the queue has room for the
    // whole window, so this must not be a 409 restart.
    send_raw(*client, http1("GET",
                            poll_target(subscriber, "timeout=0&cursor=1:0")));
    CRPC_CHECK(wait_polls(parser, capture, 3));
    CRPC_CHECK_EQ(200, parser.responses[2].status);
    CRPC_CHECK_EQ(std::string("m1"), parser.responses[2].body);
    CRPC_CHECK_EQ(std::size_t{0}, fx.server->stats().longpoll_restarts);

    for (int i = 2; i <= 4; ++i) {
        send_raw(*client, http1("GET",
                                poll_target(subscriber,
                                            "timeout=0&cursor=1:" +
                                                std::to_string(i - 1))));
        CRPC_CHECK(wait_polls(parser, capture,
                              static_cast<std::size_t>(i) + 2));
        const lp_response& response =
            parser.responses[static_cast<std::size_t>(i) + 1];
        CRPC_CHECK_EQ(200, response.status);
        CRPC_CHECK_EQ("m" + std::to_string(i), response.body);
        CRPC_CHECK(response.head.find("X-CRPC-Event-ID: 1:" +
                                      std::to_string(i)) != std::string::npos);
    }
    // The queue is empty now: the next poll is a bodyless 204, so the
    // 200/204 ordering is explicit.
    send_raw(*client, http1("GET",
                            poll_target(subscriber,
                                        "timeout=0&cursor=1:4")));
    CRPC_CHECK(wait_polls(parser, capture, 7));
    CRPC_CHECK_EQ(204, parser.responses[6].status);
    client->close();
}

CRPC_TEST(longpoll_replay_over_queue_rejected_at_start) {
    http::http_server_options options;
    options.idle_timeout = std::chrono::milliseconds(0);
    options.request_timeout = std::chrono::milliseconds(0);
    options.longpoll.timeout = std::chrono::milliseconds(0);
    options.longpoll.max_timeout = std::chrono::milliseconds(0);
    options.longpoll.subscribe.max_messages = 2;
    options.longpoll.subscribe.max_replay = 4;

    inproc_backend backend;
    mvc::router router;
    CRPC_CHECK(router.freeze().ok());
    http::http_server::builder builder;
    builder.add_listener(endpoint::inproc("http-lp-reject"), backend);
    builder.router(router);
    builder.options(options);
    http::longpoll_route route;
    route.path = "/poll";
    route.topic = "news";
    builder.add_longpoll_route(std::move(route));
    auto server = builder.build();
    CRPC_CHECK(server != nullptr);
    const status started = server->start();
    CRPC_CHECK_FALSE(started.ok());
    CRPC_CHECK_EQ(static_cast<int>(rpc_code::invalid_argument),
                  static_cast<int>(started.code()));
}

CRPC_TEST(longpoll_lost_subscription_gets_fresh_id) {
    http::http_server_options options;
    options.idle_timeout = std::chrono::milliseconds(0);
    options.request_timeout = std::chrono::milliseconds(0);
    options.longpoll.timeout = std::chrono::milliseconds(0);
    options.longpoll.max_timeout = std::chrono::milliseconds(0);
    options.longpoll.slow_timeout = std::chrono::milliseconds(0);
    options.longpoll.subscribe.max_messages = 1;
    options.longpoll.subscribe.max_bytes = 1024;
    options.longpoll.subscribe.max_replay = 1;
    options.longpoll.subscribe.on_overflow =
        push::overflow_policy::disconnect_subscriber;
    longpoll_fixture fx(options);

    const u64 subscriber = (1ull << 32) + 49;
    capture_source capture;
    auto client = fx.connect();
    client->bind_source(&capture);
    lp_parser parser;
    send_raw(*client, http1("GET", poll_target(subscriber, "timeout=0")));
    CRPC_CHECK(wait_polls(parser, capture, 1));
    CRPC_CHECK_EQ(204, parser.responses[0].status);

    // The second publish overflows a one-message queue with the disconnect
    // policy, so the whole subscription disappears.
    CRPC_CHECK(publish_text(*fx.server, "news", "first"));
    CRPC_CHECK(publish_text(*fx.server, "news", "second"));

    // Reusing the old id must produce a NEW server id (the old sequence space
    // is gone); the old cursor is not acknowledged.
    send_raw(*client, http1("GET",
                            poll_target(subscriber,
                                        "timeout=0&cursor=1:1")));
    CRPC_CHECK(wait_polls(parser, capture, 2));
    CRPC_CHECK_EQ(204, parser.responses[1].status);
    const std::string fresh_header = "X-CRPC-Subscriber: ";
    const std::size_t header_pos =
        parser.responses[1].head.find(fresh_header);
    CRPC_CHECK(header_pos != std::string::npos);
    u64 fresh = 0;
    if (header_pos != std::string::npos) {
        fresh = std::strtoull(
            parser.responses[1].head.c_str() + header_pos + fresh_header.size(),
            nullptr, 10);
    }
    CRPC_CHECK(fresh != subscriber);
    CRPC_CHECK(fresh >= (1ull << 32) && fresh < (1ull << 33));

    // The fresh id is stable for a sequential poll.
    send_raw(*client, http1("GET", poll_target(fresh, "timeout=0")));
    CRPC_CHECK(wait_polls(parser, capture, 3));
    CRPC_CHECK_EQ(204, parser.responses[2].status);
    CRPC_CHECK(parser.responses[2].head.find(
                   fresh_header + std::to_string(fresh)) != std::string::npos);
    client->close();
}

CRPC_TEST(longpoll_zero_length_event_and_unreliable_ids) {
    http::http_server_options options;
    options.idle_timeout = std::chrono::milliseconds(0);
    options.request_timeout = std::chrono::milliseconds(0);
    options.longpoll.timeout = std::chrono::milliseconds(0);
    options.longpoll.max_timeout = std::chrono::milliseconds(0);
    options.longpoll.slow_timeout = std::chrono::milliseconds(0);
    longpoll_fixture fx(options);

    const u64 subscriber = (1ull << 32) + 43;
    capture_source capture;
    auto client = fx.connect();
    client->bind_source(&capture);
    lp_parser parser;
    send_raw(*client, http1("GET", poll_target(subscriber, "timeout=0")));
    CRPC_CHECK(wait_polls(parser, capture, 1));
    CRPC_CHECK_EQ(204, parser.responses[0].status);

    CRPC_CHECK(fx.server->publish("news", span<const std::byte>{},
                                  push::delivery_mode::reliable));
    send_raw(*client, http1("GET", poll_target(subscriber, "timeout=0")));
    CRPC_CHECK(wait_polls(parser, capture, 2));
    CRPC_CHECK_EQ(200, parser.responses[1].status);
    CRPC_CHECK(parser.responses[1].body.empty());
    CRPC_CHECK(parser.responses[1].head.find("Content-Length: 0") !=
               std::string::npos);
    CRPC_CHECK(parser.responses[1].head.find("X-CRPC-Event-ID: 1:1") !=
               std::string::npos);

    // Unreliable mode stamps topic_seq instead of the reliable seq and never
    // updates the cursor high water.
    const u64 unreliable_subscriber = (1ull << 32) + 47;
    send_raw(*client, http1("GET",
                            poll_target(unreliable_subscriber,
                                        "timeout=0&mode=unreliable")));
    CRPC_CHECK(wait_polls(parser, capture, 3));
    CRPC_CHECK_EQ(204, parser.responses[2].status);
    CRPC_CHECK(fx.server->publish("news", span<const std::byte>(
                                                reinterpret_cast<const std::byte*>(
                                                    "u"),
                                                1),
                                  push::delivery_mode::unreliable));
    send_raw(*client, http1("GET",
                            poll_target(unreliable_subscriber,
                                        "timeout=0&mode=unreliable")));
    CRPC_CHECK(wait_polls(parser, capture, 4));
    CRPC_CHECK_EQ(200, parser.responses[3].status);
    CRPC_CHECK_EQ(std::string("u"), parser.responses[3].body);
    CRPC_CHECK(parser.responses[3].head.find("X-CRPC-Event-ID: 1:") !=
               std::string::npos);
    client->close();
}

CRPC_TEST(longpoll_invalid_arguments_and_methods) {
    http::http_server_options options;
    options.idle_timeout = std::chrono::milliseconds(0);
    options.request_timeout = std::chrono::milliseconds(0);
    options.longpoll.timeout = std::chrono::milliseconds(0);
    options.longpoll.max_timeout = std::chrono::milliseconds(0);
    options.longpoll.slow_timeout = std::chrono::milliseconds(0);
    longpoll_fixture fx(options);

    const u64 subscriber = (1ull << 32) + 53;
    const auto expect = [&](std::string_view raw, int status) {
        capture_source capture;
        auto client = fx.connect();
        client->bind_source(&capture);
        send_raw(*client, raw);
        lp_parser parser;
        CRPC_CHECK(wait_polls(parser, capture, 1));
        CRPC_CHECK_EQ(status, parser.responses[0].status);
        client->close();
    };

    // HEAD gets the 405 head with no body (the response must stay framed for
    // keep-alive), so it is checked without the body-reading parser.
    {
        capture_source capture;
        auto client = fx.connect();
        client->bind_source(&capture);
        send_raw(*client, http1("HEAD", poll_target(subscriber, "timeout=0")));
        CRPC_CHECK(crpc_test::wait_for(
            [&] {
                return capture.snapshot().find("\r\n\r\n") != std::string::npos;
            },
            5000));
        CRPC_CHECK(capture.snapshot().find("HTTP/1.1 405") != std::string::npos);
        client->close();
    }
    expect(http1("POST", poll_target(subscriber, "timeout=0")), 405);
    expect(http1("PUT", poll_target(subscriber, "timeout=0")), 405);
    expect(http1("GET", "/poll?topic=news&subscriber=7&timeout=0"), 400);
    expect(http1("GET", poll_target(subscriber, "timeout=abc")), 400);
    expect(http1("GET", poll_target(subscriber, "timeout=0"), "Last-Event-ID: nope\r\n"),
           400);
    expect(std::string("GET ") + poll_target(subscriber, "timeout=0") +
               " HTTP/1.0\r\nHost: test\r\n\r\n",
           400);
}

CRPC_TEST(longpoll_stall_closes_and_front_replays) {
    http::http_server_options options;
    options.idle_timeout = std::chrono::milliseconds(0);
    options.request_timeout = std::chrono::milliseconds(0);
    options.longpoll.timeout = std::chrono::milliseconds(0);
    options.longpoll.max_timeout = std::chrono::milliseconds(0);
    options.longpoll.slow_timeout = std::chrono::milliseconds(80);
    options.longpoll.subscribe.max_replay = 16;
    longpoll_fixture fx(options);

    const u64 subscriber = (1ull << 32) + 59;
    capture_source capture;
    gated_source gate;
    auto client = fx.connect();
    client->bind_source(&capture);
    lp_parser parser;
    send_raw(*client, http1("GET", poll_target(subscriber, "timeout=0")));
    CRPC_CHECK(wait_polls(parser, capture, 1));
    CRPC_CHECK_EQ(204, parser.responses[0].status);

    // From now on the peer stops draining. A 64 KiB body cannot complete its
    // write, so the long-poll stall deadline must close the session.
    client->bind_source(&gate);
    gate.open.store(false, std::memory_order_release);
    const std::string payload(64 * 1024, 'z');
    CRPC_CHECK(publish_text(*fx.server, "news", payload));
    send_raw(*client, http1("GET", poll_target(subscriber, "timeout=0")));
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return fx.server->stats().active_connections == 0; }, 5000));
    CRPC_CHECK(fx.server->stats().timed_out_connections >= 1);
    client->close();

    // The taken message was never acknowledged. A new session with cursor 1:0
    // replays it from the front.
    capture_source replay_capture;
    auto replay = fx.connect();
    replay->bind_source(&replay_capture);
    lp_parser replay_parser;
    send_raw(*replay, http1("GET",
                            poll_target(subscriber,
                                        "timeout=0&cursor=1:0")));
    CRPC_CHECK(wait_polls(replay_parser, replay_capture, 1, 5000));
    CRPC_CHECK_EQ(200, replay_parser.responses[0].status);
    CRPC_CHECK_EQ(payload, replay_parser.responses[0].body);
    CRPC_CHECK(replay_parser.responses[0].head.find("X-CRPC-Event-ID: 1:1") !=
               std::string::npos);
    replay->close();
}

CRPC_TEST(longpoll_cross_protocol_same_subscriber_409) {
    http::http_server_options options;
    options.idle_timeout = std::chrono::milliseconds(0);
    options.request_timeout = std::chrono::milliseconds(0);
    options.longpoll.timeout = std::chrono::milliseconds(5000);
    options.longpoll.max_timeout = std::chrono::milliseconds(5000);
    options.longpoll.slow_timeout = std::chrono::milliseconds(0);
    options.sse.heartbeat = std::chrono::milliseconds(0);
    options.sse.slow_timeout = std::chrono::milliseconds(0);
    longpoll_fixture fx(options, "http-lp-cross", true);

    // SSE holds the id: a long poll with the same id is 409.
    const u64 sse_id = (1ull << 32) + 61;
    capture_source sse_capture;
    auto sse_client = fx.connect();
    sse_client->bind_source(&sse_capture);
    send_raw(*sse_client,
             http1("GET", "/events?topic=news&subscriber=" +
                              std::to_string(sse_id)));
    sse_parser sse_parser_state;
    CRPC_CHECK(crpc_test::wait_for(
        [&] {
            sse_parser_state.pump(sse_capture);
            return sse_parser_state.head_done;
        },
        5000));
    CRPC_CHECK_EQ(std::size_t{1}, fx.server->stats().sse_connections);

    capture_source denied_capture;
    auto denied = fx.connect();
    denied->bind_source(&denied_capture);
    lp_parser denied_parser;
    send_raw(*denied, http1("GET", poll_target(sse_id, "timeout=0")));
    CRPC_CHECK(wait_polls(denied_parser, denied_capture, 1));
    CRPC_CHECK_EQ(409, denied_parser.responses[0].status);
    sse_client->close();
    denied->close();
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return fx.server->stats().sse_connections == 0; }, 5000));

    // A waiting long poll holds the id: an SSE request with the same id is 409.
    const u64 lp_id = (1ull << 32) + 67;
    capture_source lp_capture;
    auto lp_client = fx.connect();
    lp_client->bind_source(&lp_capture);
    lp_parser lp_parser_state;
    send_raw(*lp_client, http1("GET", poll_target(lp_id, "timeout=0")));
    CRPC_CHECK(wait_polls(lp_parser_state, lp_capture, 1));
    CRPC_CHECK_EQ(204, lp_parser_state.responses[0].status);
    send_raw(*lp_client, http1("GET", poll_target(lp_id)));
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return fx.server->stats().longpoll_waiters == 1; }, 2000));

    capture_source sse_denied_capture;
    auto sse_denied = fx.connect();
    sse_denied->bind_source(&sse_denied_capture);
    lp_parser sse_denied_parser;
    send_raw(*sse_denied,
             http1("GET", "/events?topic=news&subscriber=" +
                              std::to_string(lp_id)));
    CRPC_CHECK(wait_polls(sse_denied_parser, sse_denied_capture, 1));
    CRPC_CHECK_EQ(409, sse_denied_parser.responses[0].status);
    sse_denied->close();

    // The waiting poll is unaffected.
    CRPC_CHECK(publish_text(*fx.server, "news", "still-here"));
    CRPC_CHECK(wait_polls(lp_parser_state, lp_capture, 2));
    CRPC_CHECK_EQ(200, lp_parser_state.responses[1].status);
    CRPC_CHECK_EQ(std::string("still-here"), lp_parser_state.responses[1].body);
    lp_client->close();
}

CRPC_TEST(longpoll_timer_stop_repeated_is_safe) {
    for (int i = 0; i < 5; ++i) {
        http::http_server_options options;
        options.idle_timeout = std::chrono::milliseconds(0);
        options.request_timeout = std::chrono::milliseconds(0);
        options.longpoll.timeout = std::chrono::milliseconds(0);
        options.longpoll.max_timeout = std::chrono::milliseconds(0);
        options.longpoll.subscriber_ttl = std::chrono::milliseconds(20);
        options.longpoll.slow_timeout = std::chrono::milliseconds(0);
        longpoll_fixture fx(options,
                            "http-lp-timer-" + std::to_string(i));
        // Let the periodic housekeeping timer fire while the server is up,
        // then cancel it from stop().
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        fx.server->stop();
        CRPC_CHECK_FALSE(fx.server->running());
        CRPC_CHECK_EQ(std::size_t{0}, fx.server->stats().longpoll_waiters);
    }
}

CRPC_TEST(longpoll_stop_wakes_waiters) {
    http::http_server_options options;
    options.drain_timeout = std::chrono::milliseconds(100);
    options.idle_timeout = std::chrono::milliseconds(0);
    options.request_timeout = std::chrono::milliseconds(0);
    options.longpoll.timeout = std::chrono::milliseconds(5000);
    options.longpoll.max_timeout = std::chrono::milliseconds(5000);
    options.longpoll.slow_timeout = std::chrono::milliseconds(0);
    longpoll_fixture fx(options);

    capture_source capture;
    auto client = fx.connect();
    client->bind_source(&capture);
    lp_parser parser;
    send_raw(*client, http1("GET", poll_target((1ull << 32) + 71, "timeout=0")));
    CRPC_CHECK(wait_polls(parser, capture, 1));
    send_raw(*client, http1("GET", poll_target((1ull << 32) + 71)));
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return fx.server->stats().longpoll_waiters == 1; }, 2000));

    const auto begin = std::chrono::steady_clock::now();
    fx.server->stop();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - begin)
                             .count();
    CRPC_CHECK(elapsed < 2000);
    CRPC_CHECK_EQ(std::size_t{0}, fx.server->stats().longpoll_waiters);
    CRPC_CHECK_EQ(std::size_t{0}, fx.server->stats().active_connections);
    client->close();
}

CRPC_TEST(longpoll_h2_fails_closed) {
    http::http_server_options options;
    options.longpoll.timeout = std::chrono::milliseconds(0);
    longpoll_fixture fx(options);

    capture_source capture;
    auto client = fx.connect();
    client->bind_source(&capture);
    h2::hpack_encoder encoder;
    std::string out(h2_preface);
    out += h2_request(1, "GET", "/poll?topic=news", encoder);
    send_raw(*client, out);

    h2::hpack_decoder decoder;
    std::string buf;
    std::string block;
    std::string body;
    int status = 0;
    bool end_stream = false;
    bool headers_done = false;
    std::size_t consumed = 0;
    const bool complete = crpc_test::wait_for(
        [&] {
            const std::string data = capture.snapshot();
            if (data.size() > consumed) {
                buf.append(data.data() + consumed, data.size() - consumed);
                consumed = data.size();
            }
            for (;;) {
                if (buf.size() < h2::frame_header_size) {
                    return end_stream && status != 0;
                }
                h2::frame_header frame;
                const span<const std::byte> head(
                    reinterpret_cast<const std::byte*>(buf.data()),
                    h2::frame_header_size);
                if (!h2::decode_frame_header(head, frame,
                                             h2::max_max_frame_size)
                         .ok()) {
                    return false;
                }
                const std::size_t total = h2::frame_header_size + frame.length;
                if (buf.size() < total) {
                    return end_stream && status != 0;
                }
                const span<const std::byte> payload(
                    reinterpret_cast<const std::byte*>(buf.data()) +
                        h2::frame_header_size,
                    frame.length);
                if (frame.type == h2::frame_type::headers) {
                    h2::headers_payload decoded;
                    if (!h2::decode_headers(payload, frame.flags, decoded)
                             .ok()) {
                        return false;
                    }
                    block.assign(
                        reinterpret_cast<const char*>(
                            decoded.header_block.data()),
                        decoded.header_block.size());
                    end_stream = end_stream || decoded.end_stream;
                    if ((frame.flags & h2::flags::end_headers) != 0 &&
                        !headers_done) {
                        http::header_list fields;
                        if (decoder
                                .decode(span<const std::byte>(
                                            reinterpret_cast<const std::byte*>(
                                                block.data()),
                                            block.size()),
                                        fields)
                                .ok()) {
                            for (const http::header& field : fields) {
                                if (field.name == ":status") {
                                    status = std::atoi(field.value.c_str());
                                }
                            }
                        }
                        headers_done = true;
                    }
                } else if (frame.type == h2::frame_type::continuation) {
                    block.append(reinterpret_cast<const char*>(payload.data()),
                                 payload.size());
                } else if (frame.type == h2::frame_type::data) {
                    span<const std::byte> decoded;
                    if (h2::decode_data(payload, frame.flags, decoded).ok()) {
                        body.append(
                            reinterpret_cast<const char*>(decoded.data()),
                            decoded.size());
                    }
                    end_stream = end_stream ||
                                 (frame.flags & h2::flags::end_stream) != 0;
                }
                buf.erase(0, total);
            }
        },
        5000);
    CRPC_CHECK(complete);
    CRPC_CHECK_EQ(501, status);
    CRPC_CHECK(body.find("not supported") != std::string::npos);
    CRPC_CHECK(end_stream);
    CRPC_CHECK_EQ(std::size_t{0}, fx.server->stats().longpoll_subscribers);
    client->close();
}

CRPC_TEST(longpoll_multishard_unavailable_503) {
    http::http_server_options options;
    options.shards = 2;
    options.idle_timeout = std::chrono::milliseconds(0);
    options.request_timeout = std::chrono::milliseconds(0);
    options.longpoll.timeout = std::chrono::milliseconds(0);
    options.longpoll.max_timeout = std::chrono::milliseconds(0);
    longpoll_fixture fx(options, "http-lp-multi");

    capture_source capture;
    auto client = fx.connect();
    client->bind_source(&capture);
    lp_parser parser;
    send_raw(*client,
             http1("GET", poll_target((1ull << 32) + 73, "timeout=0")));
    CRPC_CHECK(wait_polls(parser, capture, 1));
    CRPC_CHECK_EQ(503, parser.responses[0].status);
    CRPC_CHECK(parser.responses[0].head.find("Retry-After: 1") !=
               std::string::npos);
    client->close();
}

} // namespace

CRPC_TEST_MAIN()
