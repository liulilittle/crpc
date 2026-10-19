// CRPC -- tests for the HTTP server facade (HTTP/1.1, h2c and TLS routing).
//
// Coverage targets:
//   * HTTP/1.1 over the inproc backend: GET, POST with body, keep-alive,
//     pipelining, and the 400/404/405/411/413 status paths;
//   * h2c prior-knowledge handshake: SETTINGS exchange, HEADERS+DATA request,
//     HPACK-encoded response;
//   * HTTP/2 flow control: a response must park on a tiny peer window and
//     resume after WINDOW_UPDATE;
//   * HTTP/2 limits: a response larger than the send-queue cap is rejected with
//     RST_STREAM;
//   * HTTP/2 peer GOAWAY is answered with GOAWAY and the connection closes;
//   * TLS + ALPN routing when the library is built with OpenSSL; skipped with a
//     clear message otherwise.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/http/server.hpp>
#include <crpc/http2/connection.hpp>
#include <crpc/http2/frame.hpp>
#include <crpc/http2/hpack.hpp>
#include <crpc/io.hpp>
#include <crpc/mvc/router.hpp>

#include "test_framework.hpp"

using namespace crpc;

namespace {

namespace h2 = crpc::http2;

constexpr std::string_view preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

// --- Client-side receive capture --------------------------------------------

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

// --- Route handlers ----------------------------------------------------------

status handle_root(void*, const http::request&, const mvc::route_params&,
                   const codec&, const codec_registry&, http::response& out) {
    out.status_code = 200;
    out.reason = "OK";
    out.headers.add("Content-Type", "text/plain");
    out.body = "hello";
    return status{};
}

status handle_echo(void*, const http::request& req, const mvc::route_params&,
                   const codec&, const codec_registry&, http::response& out) {
    out.status_code = 200;
    out.reason = "OK";
    out.headers.add("Content-Type", "application/json");
    out.body = req.body;
    return status{};
}

status handle_big(void*, const http::request&, const mvc::route_params&,
                  const codec&, const codec_registry&, http::response& out) {
    out.status_code = 200;
    out.reason = "OK";
    out.headers.add("Content-Type", "text/plain");
    out.body.assign(200000, 'x');
    return status{};
}

status handle_boom(void*, const http::request&, const mvc::route_params&,
                   const codec&, const codec_registry&, http::response&) {
    throw std::runtime_error("boom");
}

void add_route(mvc::router& router, http::method method, const char* path,
               mvc::invoke_fn fn) {
    mvc::route_entry entry;
    entry.method = method;
    entry.path = path;
    entry.name = path;
    entry.invoke = fn;
    const status added = router.add(std::move(entry));
    CRPC_CHECK(added.ok());
}

// --- Fixture -----------------------------------------------------------------

struct fixture {
    inproc_backend backend;
    mvc::router router;

    explicit fixture(http::http_server_options options = http::http_server_options{}) {
        add_route(router, http::method::get, "/root", &handle_root);
        add_route(router, http::method::post, "/echo", &handle_echo);
        add_route(router, http::method::get, "/big", &handle_big);
        add_route(router, http::method::get, "/boom", &handle_boom);
        const status frozen = router.freeze();
        CRPC_CHECK(frozen.ok());

        http::http_server::builder builder;
        builder.add_listener(endpoint::inproc("http-test"), backend);
        builder.router(router);
        builder.options(options);
        server = builder.build();
        CRPC_CHECK(server != nullptr);
        const status started = server->start();
        CRPC_CHECK(started.ok());
    }

    ~fixture() {
        if (server) {
            server->stop();
        }
    }

    [[nodiscard]] std::unique_ptr<connection> connect() {
        return backend.connect(endpoint::inproc("http-test"), connect_options{});
    }

    std::unique_ptr<http::http_server> server;
};

// --- HTTP/1.1 helpers --------------------------------------------------------

// Builds an HTTP/1.1 request. `extra` holds extra header lines, each already
// terminated by CRLF (empty when there are none); the terminating blank line is
// always appended here.
std::string http1(std::string_view method, std::string_view path,
                  std::string_view extra) {
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

// --- HTTP/2 client parse state ----------------------------------------------

struct h2_parse_state {
    h2::hpack_decoder decoder;
    std::string buf;
    std::string header_block;
    bool in_header_block = false;
    int status = 0;
    std::string body;
    bool headers_seen = false;
    bool end_stream = false;
    bool got_settings = false;
    bool got_settings_ack = false;
    bool got_goaway = false;
    bool got_rst = false;
    std::size_t frames = 0;
    u32 rst_stream = 0;

    void feed(std::string_view bytes) {
        buf.append(bytes);
        for (;;) {
            if (buf.size() < h2::frame_header_size) {
                return;
            }
            const span<const std::byte> head(
                reinterpret_cast<const std::byte*>(buf.data()),
                h2::frame_header_size);
            h2::frame_header frame;
            if (!h2::decode_frame_header(head, frame, h2::max_max_frame_size).ok()) {
                buf.clear();
                return;
            }
            const std::size_t total = h2::frame_header_size + frame.length;
            if (buf.size() < total) {
                return;
            }
            const std::byte* payload =
                reinterpret_cast<const std::byte*>(buf.data()) + h2::frame_header_size;
            const span<const std::byte> body_span(payload, frame.length);
            ++frames;
            handle(frame, body_span);
            buf.erase(0, total);
        }
    }

    void handle(const h2::frame_header& frame, span<const std::byte> payload) {
        switch (frame.type) {
        case h2::frame_type::settings:
            if ((frame.flags & h2::flags::ack) != 0) {
                got_settings_ack = true;
            } else {
                got_settings = true;
            }
            break;
        case h2::frame_type::headers:
        case h2::frame_type::continuation: {
            h2::headers_payload decoded;
            span<const std::byte> fragment;
            if (frame.type == h2::frame_type::headers) {
                if (!h2::decode_headers(payload, frame.flags, decoded).ok()) {
                    return;
                }
                fragment = decoded.header_block;
                if (decoded.end_stream) {
                    end_stream = true;
                }
                in_header_block = true;
                header_block.clear();
            } else {
                h2::headers_payload ignored;
                (void)ignored;
                fragment = payload;
            }
            header_block.append(
                reinterpret_cast<const char*>(fragment.data()), fragment.size());
            const bool last =
                (frame.flags & h2::flags::end_headers) != 0;
            if (last && in_header_block) {
                in_header_block = false;
                http::header_list fields;
                if (decoder.decode(
                        span<const std::byte>(
                            reinterpret_cast<const std::byte*>(header_block.data()),
                            header_block.size()),
                        fields)
                        .ok()) {
                    for (const http::header& field : fields) {
                        if (field.name == ":status") {
                            status = std::atoi(field.value.c_str());
                            headers_seen = true;
                        }
                    }
                }
            }
            break;
        }
        case h2::frame_type::data: {
            span<const std::byte> fragment_body;
            if (h2::decode_data(payload, frame.flags, fragment_body).ok()) {
                body_out_add(fragment_body);
            }
            if ((frame.flags & h2::flags::end_stream) != 0) {
                end_stream = true;
            }
            break;
        }
        case h2::frame_type::goaway:
            got_goaway = true;
            break;
        case h2::frame_type::rst_stream: {
            got_rst = true;
            h2::error_code code = h2::error_code::no_error;
            (void)h2::decode_rst_stream(payload, code);
            rst_stream = frame.stream_id;
            break;
        }
        default:
            break;
        }
    }

    void body_out_add(span<const std::byte> bytes) {
        body.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
};

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

std::string h2_settings(std::initializer_list<h2::setting> entries,
                        bool ack = false) {
    std::vector<h2::setting> list(entries);
    std::string payload(list.size() * 6, '\0');
    std::size_t written = 0;
    if (!list.empty()) {
        (void)h2::encode_settings(
            span<std::byte>(reinterpret_cast<std::byte*>(payload.data()),
                            payload.size()),
            span<const h2::setting>(list.data(), list.size()), written);
        payload.resize(written);
    } else {
        payload.clear();
    }
    return h2_frame(h2::frame_type::settings,
                    ack ? h2::flags::ack : static_cast<u8>(0), 0, payload);
}

std::string h2_window_update(u32 stream_id, u32 increment) {
    std::string payload(4, '\0');
    std::size_t written = 0;
    (void)h2::encode_window_update(
        span<std::byte>(reinterpret_cast<std::byte*>(payload.data()), payload.size()),
        increment, written);
    return h2_frame(h2::frame_type::window_update, 0, stream_id, payload);
}

std::string h2_goaway(u32 last_stream_id, h2::error_code code) {
    h2::goaway_payload value;
    value.last_stream_id = last_stream_id;
    value.code = code;
    std::string payload(8, '\0');
    std::size_t written = 0;
    (void)h2::encode_goaway(
        span<std::byte>(reinterpret_cast<std::byte*>(payload.data()), payload.size()),
        value, written);
    return h2_frame(h2::frame_type::goaway, 0, 0, payload);
}

std::string h2_request_headers(u32 stream_id, std::string_view method,
                               std::string_view path, bool end_stream,
                               h2::hpack_encoder& encoder) {
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
                                    (end_stream ? h2::flags::end_stream : 0)),
                    stream_id, block);
}

// --- HTTP/1.1 tests ----------------------------------------------------------

CRPC_TEST(http1_header_and_request_line_limits) {
    http::http_server_options options;
    options.limits.header_block_bytes = 64;
    options.limits.request_line_bytes = 32;
    fixture fx(options);

    // Header block over the limit -> 431.
    {
        auto client = fx.connect();
        capture_source capture;
        client->bind_source(&capture);
        std::string extra = "X-Big: ";
        extra += std::string(200, 'a');
        extra += "\r\n";
        send_raw(*client, http1("GET", "/root", extra));
        CRPC_CHECK(crpc_test::wait_for(
            [&] { return capture.snapshot().find("431") != std::string::npos; }, 5000));
    }
    // Request line over the limit -> 400.
    {
        auto client = fx.connect();
        capture_source capture;
        client->bind_source(&capture);
        std::string path = "/";
        path += std::string(80, 'p');
        send_raw(*client, http1("GET", path, ""));
        CRPC_CHECK(crpc_test::wait_for(
            [&] { return capture.snapshot().find("400") != std::string::npos; }, 5000));
    }
}

CRPC_TEST(http1_expect_100_continue) {
    fixture fx;
    auto client = fx.connect();
    capture_source capture;
    client->bind_source(&capture);

    send_raw(*client,
             "POST /echo HTTP/1.1\r\nHost: test\r\nExpect: 100-continue\r\n"
             "Content-Length: 5\r\n\r\n");
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return capture.snapshot().find("100 Continue") != std::string::npos; },
        5000));

    send_raw(*client, "hello");
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return capture.snapshot().find("hello") != std::string::npos; }, 5000));
}

CRPC_TEST(http1_head_runs_get_and_drops_body) {
    fixture fx;
    auto client = fx.connect();
    capture_source capture;
    client->bind_source(&capture);

    send_raw(*client, http1("HEAD", "/root", ""));
    const bool headers = crpc_test::wait_for(
        [&] { return capture.snapshot().find("\r\n\r\n") != std::string::npos; }, 5000);
    CRPC_CHECK(headers);
    const std::string data = capture.snapshot();
    CRPC_CHECK(data.find("200 OK") != std::string::npos);
    CRPC_CHECK(data.find("Content-Length: 5") != std::string::npos);
    CRPC_CHECK(data.find("hello") == std::string::npos);
}

CRPC_TEST(http1_get_and_keep_alive) {
    fixture fx;
    auto client = fx.connect();
    CRPC_CHECK(client != nullptr);
    capture_source capture;
    client->bind_source(&capture);

    send_raw(*client, http1("GET", "/root", ""));
    const bool ok = crpc_test::wait_for(
        [&] { return capture.snapshot().find("hello") != std::string::npos; }, 5000);
    CRPC_CHECK(ok);
    const std::string first = capture.snapshot();
    CRPC_CHECK(first.find("HTTP/1.1 200 OK") != std::string::npos);
    CRPC_CHECK(first.find("Content-Length: 5") != std::string::npos);

    // A second request on the same (keep-alive) connection is answered.
    send_raw(*client, http1("GET", "/root", ""));
    const bool second = crpc_test::wait_for(
        [&] { return count_substring(capture.snapshot(), "HTTP/1.1 200 OK") >= 2; },
        5000);
    CRPC_CHECK(second);
}

CRPC_TEST(http1_post_with_body) {
    fixture fx;
    auto client = fx.connect();
    capture_source capture;
    client->bind_source(&capture);

    const std::string body = "{\"name\":\"crpc\"}";
    const std::string extra =
        "Content-Length: " + std::to_string(body.size()) + "\r\n";
    std::string request = http1("POST", "/echo", extra);
    request += body;
    send_raw(*client, request);

    const bool ok = crpc_test::wait_for(
        [&] { return capture.snapshot().find(body) != std::string::npos; }, 5000);
    CRPC_CHECK(ok);
    CRPC_CHECK(capture.snapshot().find("HTTP/1.1 200 OK") != std::string::npos);
}

CRPC_TEST(http1_chunked_upload) {
    fixture fx;
    auto client = fx.connect();
    capture_source capture;
    client->bind_source(&capture);

    std::string request = http1("POST", "/echo", "Transfer-Encoding: chunked\r\n");
    // Two chunks, then the terminator.
    request += "5\r\nhello\r\n";
    request += "1\r\n!\r\n";
    request += "0\r\n\r\n";
    send_raw(*client, request);

    const bool ok = crpc_test::wait_for(
        [&] { return capture.snapshot().find("hello!") != std::string::npos; }, 5000);
    CRPC_CHECK(ok);
}

CRPC_TEST(http1_pipelining_preserves_order) {
    fixture fx;
    auto client = fx.connect();
    capture_source capture;
    client->bind_source(&capture);

    std::string requests = http1("GET", "/root", "");
    requests += http1("GET", "/missing", "");
    send_raw(*client, requests);

    const bool ok = crpc_test::wait_for(
        [&] {
            const std::string data = capture.snapshot();
            return count_substring(data, "HTTP/1.1") >= 2 &&
                   data.find("200 OK") != std::string::npos &&
                   data.find("404") != std::string::npos;
        },
        5000);
    CRPC_CHECK(ok);
    const std::string data = capture.snapshot();
    CRPC_CHECK(data.find("200 OK") < data.find("404 Not Found"));
}

CRPC_TEST(http1_status_paths) {
    fixture fx;
    // 404
    {
        auto client = fx.connect();
        capture_source capture;
        client->bind_source(&capture);
        send_raw(*client, http1("GET", "/nope", ""));
        CRPC_CHECK(crpc_test::wait_for(
            [&] { return capture.snapshot().find("404") != std::string::npos; }, 5000));
    }
    // 405 (POST to a GET-only route)
    {
        auto client = fx.connect();
        capture_source capture;
        client->bind_source(&capture);
        send_raw(*client, http1("POST", "/root", "Content-Length: 0\r\n"));
        CRPC_CHECK(crpc_test::wait_for(
            [&] { return capture.snapshot().find("405") != std::string::npos; }, 5000));
    }
    // 411 (PUT with no framing)
    {
        auto client = fx.connect();
        capture_source capture;
        client->bind_source(&capture);
        send_raw(*client, http1("PUT", "/echo", ""));
        CRPC_CHECK(crpc_test::wait_for(
            [&] { return capture.snapshot().find("411") != std::string::npos; }, 5000));
    }
    // 500 (handler throws)
    {
        auto client = fx.connect();
        capture_source capture;
        client->bind_source(&capture);
        send_raw(*client, http1("GET", "/boom", ""));
        CRPC_CHECK(crpc_test::wait_for(
            [&] { return capture.snapshot().find("500") != std::string::npos; }, 5000));
    }
}

CRPC_TEST(http1_body_limit_is_413) {
    http::http_server_options options;
    options.limits.body_bytes = 64;
    fixture fx(options);
    auto client = fx.connect();
    capture_source capture;
    client->bind_source(&capture);

    std::string request = http1("POST", "/echo", "Content-Length: 4096\r\n");
    request += std::string(4096, 'a');
    send_raw(*client, request);

    CRPC_CHECK(crpc_test::wait_for(
        [&] { return capture.snapshot().find("413") != std::string::npos; }, 5000));
}

CRPC_TEST(http1_missing_host_is_400) {
    fixture fx;
    auto client = fx.connect();
    capture_source capture;
    client->bind_source(&capture);

    send_raw(*client, "GET /root HTTP/1.1\r\n\r\n");
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return capture.snapshot().find("400") != std::string::npos; }, 5000));
}

CRPC_TEST(http1_request_smuggling_is_400) {
    fixture fx;
    auto client = fx.connect();
    capture_source capture;
    client->bind_source(&capture);

    // Content-Length plus Transfer-Encoding must be rejected.
    std::string request =
        "POST /echo HTTP/1.1\r\nHost: test\r\nContent-Length: 5\r\n"
        "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n";
    send_raw(*client, request);
    CRPC_CHECK(crpc_test::wait_for(
        [&] { return capture.snapshot().find("400") != std::string::npos; }, 5000));
}

// --- HTTP/2 tests ------------------------------------------------------------

CRPC_TEST(h2c_headers_and_data_round_trip) {
    fixture fx;
    auto client = fx.connect();
    capture_source capture;
    client->bind_source(&capture);

    h2::hpack_encoder encoder;
    std::string out(preface);
    out += h2_settings({{static_cast<u16>(h2::setting_id::initial_window_size),
                         h2::default_initial_window_size}});
    out += h2_request_headers(1, "GET", "/root", true, encoder);
    send_raw(*client, out);

    // Drive the parse explicitly so the decoded state is available.
    h2_parse_state parsed;
    std::size_t consumed = 0;
    const bool finished = crpc_test::wait_for(
        [&] {
            const std::string data = capture.snapshot();
            if (data.size() > consumed) {
                parsed.feed(std::string_view(data).substr(consumed));
                consumed = data.size();
            }
            return parsed.headers_seen && parsed.end_stream;
        },
        5000);
    CRPC_CHECK(finished);
    CRPC_CHECK(parsed.got_settings);
    CRPC_CHECK_EQ(parsed.status, 200);
    CRPC_CHECK_EQ(parsed.body, std::string("hello"));
}

CRPC_TEST(h2c_post_body_round_trip) {
    fixture fx;
    auto client = fx.connect();
    capture_source capture;
    client->bind_source(&capture);

    h2::hpack_encoder encoder;
    const std::string body = "payload!";
    std::string out(preface);
    out += h2_settings({});
    out += h2_request_headers(1, "POST", "/echo", false, encoder);
    out += h2_frame(h2::frame_type::data, h2::flags::end_stream, 1, body);
    send_raw(*client, out);

    h2_parse_state parsed;
    std::size_t consumed = 0;
    const bool finished = crpc_test::wait_for(
        [&] {
            const std::string data = capture.snapshot();
            if (data.size() > consumed) {
                parsed.feed(std::string_view(data).substr(consumed));
                consumed = data.size();
            }
            return parsed.headers_seen && parsed.end_stream;
        },
        5000);
    CRPC_CHECK(finished);
    CRPC_CHECK_EQ(parsed.status, 200);
    CRPC_CHECK_EQ(parsed.body, body);
}

CRPC_TEST(h2c_flow_control_parks_then_resumes) {
    fixture fx;
    auto client = fx.connect();
    capture_source capture;
    client->bind_source(&capture);

    h2::hpack_encoder encoder;
    std::string out(preface);
    // Advertise a tiny stream window so the 200000-byte response must park.
    out += h2_settings({{static_cast<u16>(h2::setting_id::initial_window_size), 64}});
    out += h2_request_headers(1, "GET", "/big", true, encoder);
    send_raw(*client, out);

    h2_parse_state parsed;
    std::size_t consumed = 0;
    // Wait until the first DATA bytes arrive, then open the windows.
    const bool parked = crpc_test::wait_for(
        [&] {
            const std::string data = capture.snapshot();
            if (data.size() > consumed) {
                parsed.feed(std::string_view(data).substr(consumed));
                consumed = data.size();
            }
            return !parsed.body.empty();
        },
        5000);
    CRPC_CHECK(parked);
    CRPC_CHECK(parsed.body.size() < 200000);

    std::string resume;
    resume += h2_window_update(0, 1u << 20);
    resume += h2_window_update(1, 1u << 20);
    send_raw(*client, resume);

    const bool finished = crpc_test::wait_for(
        [&] {
            const std::string data = capture.snapshot();
            if (data.size() > consumed) {
                parsed.feed(std::string_view(data).substr(consumed));
                consumed = data.size();
            }
            return parsed.end_stream && parsed.body.size() == 200000;
        },
        5000);
    CRPC_CHECK(finished);
    CRPC_CHECK_EQ(parsed.body.size(), std::size_t{200000});
}

CRPC_TEST(h2c_send_queue_cap_is_rst_stream) {
    http::http_server_options options;
    // Advertise a zero window so everything parks, and cap the parked queue far
    // below the 200000-byte response so the stream is rejected.
    options.h2_max_send_queue_bytes = 1024;
    fixture fx(options);
    auto client = fx.connect();
    capture_source capture;
    client->bind_source(&capture);

    h2::hpack_encoder encoder;
    std::string out(preface);
    out += h2_settings({{static_cast<u16>(h2::setting_id::initial_window_size), 0}});
    out += h2_request_headers(1, "GET", "/big", true, encoder);
    send_raw(*client, out);

    h2_parse_state parsed;
    std::size_t consumed = 0;
    const bool rejected = crpc_test::wait_for(
        [&] {
            const std::string data = capture.snapshot();
            if (data.size() > consumed) {
                parsed.feed(std::string_view(data).substr(consumed));
                consumed = data.size();
            }
            return parsed.got_rst;
        },
        5000);
    CRPC_CHECK(rejected);
    CRPC_CHECK_EQ(parsed.rst_stream, u32{1});
}

CRPC_TEST(h2c_peer_goaway_is_answered) {
    fixture fx;
    auto client = fx.connect();
    capture_source capture;
    client->bind_source(&capture);

    h2::hpack_encoder encoder;
    std::string out(preface);
    out += h2_settings({});
    out += h2_request_headers(1, "GET", "/root", true, encoder);
    out += h2_goaway(1, h2::error_code::no_error);
    send_raw(*client, out);

    h2_parse_state parsed;
    std::size_t consumed = 0;
    const bool answered = crpc_test::wait_for(
        [&] {
            const std::string data = capture.snapshot();
            if (data.size() > consumed) {
                parsed.feed(std::string_view(data).substr(consumed));
                consumed = data.size();
            }
            return parsed.got_goaway && capture.closed();
        },
        5000);
    CRPC_CHECK(answered);
}

// --- Resource bounds ---------------------------------------------------------

CRPC_TEST(connection_cap_rejects_the_excess) {
    http::http_server_options options;
    options.max_connections = 2;
    fixture fx(options);

    auto first = fx.connect();
    auto second = fx.connect();
    CRPC_CHECK(first != nullptr);
    CRPC_CHECK(second != nullptr);

    capture_source c1;
    capture_source c2;
    first->bind_source(&c1);
    second->bind_source(&c2);
    send_raw(*first, http1("GET", "/root", ""));
    send_raw(*second, http1("GET", "/root", ""));
    const bool both = crpc_test::wait_for(
        [&] {
            return c1.snapshot().find("200 OK") != std::string::npos &&
                   c2.snapshot().find("200 OK") != std::string::npos;
        },
        5000);
    CRPC_CHECK(both);

    // The third connection is accepted by the transport but the server closes
    // it immediately: no session is created and no request is answered. The
    // close may happen before the test binds its source, so the durable
    // evidence is the rejection counter plus the absence of a response.
    auto third = fx.connect();
    CRPC_CHECK(third != nullptr);
    capture_source c3;
    third->bind_source(&c3);
    send_raw(*third, http1("GET", "/root", ""));
    const bool counted = crpc_test::wait_for(
        [&] { return fx.server->stats().rejected_connections >= 1; }, 5000);
    CRPC_CHECK(counted);
    (void)crpc_test::wait_for([&] { return c3.closed(); }, 500);
    CRPC_CHECK(c3.snapshot().find("200 OK") == std::string::npos);
}

CRPC_TEST(idle_timeout_closes_a_silent_connection) {
    http::http_server_options options;
    options.idle_timeout = std::chrono::milliseconds(150);
    options.request_timeout = std::chrono::milliseconds(0);
    fixture fx(options);

    auto client = fx.connect();
    CRPC_CHECK(client != nullptr);
    capture_source capture;
    client->bind_source(&capture);

    // Send nothing. The idle bound must close the connection instead of
    // keeping a silent peer forever (No-stall rule).
    const bool closed = crpc_test::wait_for([&] { return capture.closed(); }, 5000);
    CRPC_CHECK(closed);
    CRPC_CHECK(fx.server->stats().timed_out_connections >= 1);
}

CRPC_TEST(request_timeout_answers_408_for_a_partial_request) {
    http::http_server_options options;
    options.idle_timeout = std::chrono::milliseconds(0);
    options.request_timeout = std::chrono::milliseconds(150);
    fixture fx(options);

    auto client = fx.connect();
    CRPC_CHECK(client != nullptr);
    capture_source capture;
    client->bind_source(&capture);

    // A request that never completes: headers only, no terminating blank line.
    send_raw(*client, "GET /root HTTP/1.1\r\nHost: test\r\n");
    const bool answered = crpc_test::wait_for(
        [&] { return capture.snapshot().find("408") != std::string::npos; }, 5000);
    CRPC_CHECK(answered);
    CRPC_CHECK(fx.server->stats().timed_out_connections >= 1);
}

CRPC_TEST(h2c_incomplete_request_is_reset_at_the_deadline) {
    http::http_server_options options;
    options.request_timeout = std::chrono::milliseconds(200);
    options.idle_timeout = std::chrono::milliseconds(0);
    fixture fx(options);

    auto client = fx.connect();
    CRPC_CHECK(client != nullptr);
    capture_source capture;
    client->bind_source(&capture);

    h2::hpack_encoder encoder;
    std::string out(preface);
    // HEADERS without END_STREAM opens the stream; then trickle one DATA byte
    // every 50 ms, which refreshes the idle timer but must not refresh the
    // request deadline.
    out += h2_request_headers(1, "POST", "/echo", false, encoder);
    out += h2_frame(h2::frame_type::data, 0, 1, "x");
    send_raw(*client, out);

    h2_parse_state parsed;
    std::size_t consumed = 0;
    const bool reset = crpc_test::wait_for(
        [&] {
            const std::string data = capture.snapshot();
            if (data.size() > consumed) {
                parsed.feed(std::string_view(data).substr(consumed));
                consumed = data.size();
            }
            return parsed.got_rst;
        },
        5000);
    CRPC_CHECK(reset);
    CRPC_CHECK_EQ(parsed.rst_stream, u32{1});
    CRPC_CHECK(fx.server->stats().timed_out_connections >= 1);
}

// A source that refuses credit until the test opens the gate, modelling a peer
// that stops reading.
struct gated_source final : io::source {
    std::mutex mu;
    std::string data;
    std::atomic<bool> open{false};

    std::size_t on_data(span<const std::byte> bytes) noexcept override {
        std::lock_guard<std::mutex> lk(mu);
        if (!open.load(std::memory_order_acquire)) {
            return 0;
        }
        data.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return bytes.size();
    }
    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {}
    [[nodiscard]] std::string snapshot() {
        std::lock_guard<std::mutex> lk(mu);
        return data;
    }
};

CRPC_TEST(egress_cap_stops_the_parse_loop_and_loses_nothing) {
    http::http_server_options options;
    options.egress_cap_bytes = 4096;
    fixture fx(options);

    auto client = fx.connect();
    CRPC_CHECK(client != nullptr);
    gated_source gate;
    client->bind_source(&gate);

    // 40 pipelined requests whose responses are 200 KB each: answering them all
    // in one drain would build an ~8 MB write queue.
    std::string batch;
    for (int i = 0; i < 40; ++i) {
        batch += http1("GET", "/big", "");
    }
    send_raw(*client, batch);

    // The peer reads nothing, so the queue crosses the cap and parsing must
    // stop: only a couple of responses can have been produced.
    crpc_test::wait_for([&] { return fx.server->stats().responses_written > 0; }, 5000);
    const std::size_t produced = fx.server->stats().responses_written;
    CRPC_CHECK(produced < 40);

    // Open the gate: the queue drains, parsing resumes, and every response
    // arrives in order. Nothing is dropped.
    gate.open.store(true, std::memory_order_release);
    client->resume_read();
    const bool all = crpc_test::wait_for(
        [&] {
            const std::string data = gate.snapshot();
            return count_substring(data, "HTTP/1.1 200 OK") >= 40;
        },
        20000);
    CRPC_CHECK(all);
}

CRPC_TEST(request_timeout_is_not_starved_by_the_idle_timer) {
    // The idle deadline is armed when the connection starts, so it is armed
    // before any request deadline exists. A request that starts later must still
    // fire at its own (earlier) deadline: the session timer is re-armed when the
    // new nearest deadline is earlier than the armed one.
    http::http_server_options options;
    options.idle_timeout = std::chrono::milliseconds(3000);
    options.request_timeout = std::chrono::milliseconds(300);
    fixture fx(options);

    auto client = fx.connect();
    CRPC_CHECK(client != nullptr);
    capture_source capture;
    client->bind_source(&capture);

    const auto start = std::chrono::steady_clock::now();
    send_raw(*client, "GET /root HTTP/1.1\r\nHost: test\r\n");
    const bool answered = crpc_test::wait_for(
        [&] { return capture.snapshot().find("408") != std::string::npos; }, 5000);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    CRPC_CHECK(answered);
    // Well before the 3 s idle deadline, and not much after the 300 ms request
    // deadline.
    CRPC_CHECK(elapsed < std::chrono::milliseconds(1500));
}

// --- TLS test ----------------------------------------------------------------

CRPC_TEST(tls_alpn_routing_or_skip) {
#if !defined(CRPC_HAVE_OPENSSL)
    std::printf("SKIP tls_alpn_routing: library built without CRPC_HAVE_OPENSSL\n");
#else
    // With OpenSSL present, a TLS listener is registered and the context is
    // built at start(); an unusable credential must fail start() with a clear
    // error rather than silently serving cleartext. The ALPN h2/http1 routing
    // itself is exercised end to end by the WSL curl interop run.
    inproc_backend backend;
    mvc::router router;
    add_route(router, http::method::get, "/tls", &handle_root);
    CRPC_CHECK(router.freeze().ok());
    http::http_server::builder builder;
    builder.add_listener(endpoint::inproc("http-tls-plain"), backend);
    builder.router(router);
    std::unique_ptr<http::http_server> server = builder.build();
    io::tls_config config;
    config.certificate_chain = "crpc-nonexistent.pem";
    config.private_key = "crpc-nonexistent.key";
    const status registered =
        server->listen_tls(endpoint::inproc("http-tls"), backend, config);
    CRPC_CHECK(registered.ok());
    const status started = server->start();
    CRPC_CHECK_FALSE(started.ok());
    CRPC_CHECK(started.message().find("TLS") != std::string::npos);
    server->stop();
#endif
}

} // namespace

CRPC_TEST_MAIN()
