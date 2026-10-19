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
#include <utility>
#include <vector>

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
    // assertion below is then exact.
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

} // namespace

CRPC_TEST_MAIN()
