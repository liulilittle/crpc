// CRPC -- HTTP server content-coding (request decode / response compress).
//
// The response path is exercised over the inproc backend. The key contract is
// double-sided: an accepted coding must actually be applied, and an encoding
// that would not shrink the body must never be emitted (no silent inflation).
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <crpc/http/content_coding.hpp>
#include <crpc/http/server.hpp>
#include <crpc/io.hpp>
#include <crpc/mvc/router.hpp>

#include "test_framework.hpp"

using namespace crpc;

namespace {

span<const std::byte> as_span(const std::string& text) {
    return span<const std::byte>(
        reinterpret_cast<const std::byte*>(text.data()), text.size());
}

// Decodes a response body without the module's 100:1 bomb ceiling (which would
// reject a legitimately repetitive test payload).
result<std::string> decode_body(http::content_coding coding,
                                const std::string& body) {
    http::coding_limits limits;
    limits.max_compression_ratio = 0;
    limits.max_decompressed_bytes = 64u * 1024u * 1024u;
    return http::decompress_all(coding, as_span(body), limits);
}

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

std::string repetitive_payload() {
    std::string text;
    for (int i = 0; i < 4000; ++i) {
        text += "The quick brown fox jumps over the lazy dog. ";
    }
    return text;
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

struct parsed_response {
    int status = 0;
    std::string headers;
    std::string body;
};

// Splits a complete HTTP/1.1 response using its Content-Length. Returns false
// until the full body is present.
bool parse_response(const std::string& data, parsed_response& out) {
    const std::size_t head_end = data.find("\r\n\r\n");
    if (head_end == std::string::npos) {
        return false;
    }
    out.headers.assign(data.data(), head_end + 2);
    const std::size_t space = data.find(' ');
    out.status = std::atoi(data.c_str() + space + 1);
    std::size_t content_length = 0;
    const std::size_t cl = data.find("Content-Length: ");
    if (cl != std::string::npos && cl < head_end) {
        content_length = static_cast<std::size_t>(
            std::strtoull(data.c_str() + cl + 16, nullptr, 10));
    }
    const std::size_t body_start = head_end + 4;
    if (data.size() < body_start + content_length) {
        return false;
    }
    out.body.assign(data.data() + body_start, content_length);
    return true;
}

bool has_header(const parsed_response& response, std::string_view name,
                std::string_view value) {
    return response.headers.find(std::string(name) + ": " + std::string(value)) !=
           std::string::npos;
}

bool has_header_name(const parsed_response& response, std::string_view name) {
    return response.headers.find(std::string(name) + ":") != std::string::npos;
}

struct route_state {
    std::string echo_body;
};

status handle_echo(void* ctx, const http::request& req, const mvc::route_params&,
                   const codec&, const codec_registry&, http::response& out) {
    static_cast<route_state*>(ctx)->echo_body = req.body;
    out.status_code = 200;
    out.reason = "OK";
    out.body = req.body;
    return status{};
}

status handle_big(void*, const http::request&, const mvc::route_params&,
                  const codec&, const codec_registry&, http::response& out) {
    out.status_code = 200;
    out.reason = "OK";
    out.headers.add("Content-Type", "text/plain; charset=utf-8");
    out.body = repetitive_payload();
    return status{};
}

status handle_small(void*, const http::request&, const mvc::route_params&,
                    const codec&, const codec_registry&, http::response& out) {
    out.status_code = 200;
    out.reason = "OK";
    out.headers.add("Content-Type", "text/plain");
    out.body = "small body";
    return status{};
}

status handle_image(void*, const http::request&, const mvc::route_params&,
                    const codec&, const codec_registry&, http::response& out) {
    out.status_code = 200;
    out.reason = "OK";
    out.headers.add("Content-Type", "image/png");
    out.body = repetitive_payload();
    return status{};
}

void add_route(mvc::router& router, http::method method, const char* path,
               mvc::invoke_fn fn, void* ctx) {
    mvc::route_entry entry;
    entry.method = method;
    entry.path = path;
    entry.name = path;
    entry.invoke = fn;
    entry.controller = ctx;
    const status added = router.add(std::move(entry));
    CRPC_CHECK(added.ok());
}

struct fixture {
    inproc_backend backend;
    mvc::router router;
    route_state state;
    std::unique_ptr<http::http_server> server;

    explicit fixture(http::http_server_options options = http::http_server_options{}) {
        add_route(router, http::method::get, "/big", &handle_big, nullptr);
        add_route(router, http::method::get, "/small", &handle_small, nullptr);
        add_route(router, http::method::get, "/image", &handle_image, nullptr);
        add_route(router, http::method::post, "/echo", &handle_echo, &state);
        CRPC_CHECK(router.freeze().ok());

        http::http_server::builder builder;
        builder.add_listener(endpoint::inproc("coding-test"), backend);
        builder.router(router);
        builder.options(options);
        server = builder.build();
        CRPC_CHECK(server != nullptr);
        CRPC_CHECK(server->start().ok());
    }

    ~fixture() {
        if (server) {
            server->stop();
        }
    }

    std::unique_ptr<connection> connect() {
        return backend.connect(endpoint::inproc("coding-test"),
                               connect_options{});
    }
};

// Parses only the status line and headers (used for HEAD, which carries no
// body).
bool parse_head_response(const std::string& data, parsed_response& out) {
    const std::size_t head_end = data.find("\r\n\r\n");
    if (head_end == std::string::npos) {
        return false;
    }
    out.headers.assign(data.data(), head_end + 2);
    const std::size_t space = data.find(' ');
    out.status = std::atoi(data.c_str() + space + 1);
    out.body.clear();
    return true;
}

// Sends one request and returns the parsed response, or false on timeout.
bool round_trip(fixture& fx, std::string_view request, parsed_response& out,
                bool head_only = false) {
    capture_source capture;
    auto client = fx.connect();
    if (client == nullptr) {
        return false;
    }
    client->bind_source(&capture);
    send_raw(*client, request);
    const bool ok = crpc_test::wait_for(
        [&] {
            if (head_only) {
                return parse_head_response(capture.snapshot(), out);
            }
            return parse_response(capture.snapshot(), out);
        },
        5000);
    return ok;
}

} // namespace

CRPC_TEST(http1_response_is_gzip_when_accepted) {
    fixture fx;
    parsed_response response;
    CRPC_CHECK(round_trip(fx, http1("GET", "/big", "Accept-Encoding: gzip\r\n"),
                          response));
    CRPC_CHECK_EQ(200, response.status);
    CRPC_CHECK(has_header(response, "Content-Encoding", "gzip"));
    CRPC_CHECK(has_header(response, "Vary", "Accept-Encoding"));
    auto decoded = decode_body(http::content_coding::gzip, response.body);
    CRPC_CHECK(decoded.ok());
    CRPC_CHECK_EQ(repetitive_payload(), decoded.value());
    CRPC_CHECK(response.body.size() < decoded.value().size());
}

CRPC_TEST(http1_response_is_deflate_when_accepted) {
    fixture fx;
    parsed_response response;
    CRPC_CHECK(round_trip(
        fx, http1("GET", "/big", "Accept-Encoding: deflate\r\n"), response));
    CRPC_CHECK_EQ(200, response.status);
    CRPC_CHECK(has_header(response, "Content-Encoding", "deflate"));
    auto decoded = decode_body(http::content_coding::deflate, response.body);
    CRPC_CHECK(decoded.ok());
    CRPC_CHECK_EQ(repetitive_payload(), decoded.value());
}

CRPC_TEST(http1_response_identity_without_accept_encoding) {
    fixture fx;
    parsed_response response;
    CRPC_CHECK(round_trip(fx, http1("GET", "/big", ""), response));
    CRPC_CHECK_EQ(200, response.status);
    CRPC_CHECK(!has_header_name(response, "Content-Encoding"));
    // The response still varies on Accept-Encoding.
    CRPC_CHECK(has_header(response, "Vary", "Accept-Encoding"));
    CRPC_CHECK_EQ(repetitive_payload(), response.body);
}

CRPC_TEST(http1_response_406_when_identity_refused) {
    fixture fx;
    parsed_response response;
    CRPC_CHECK(round_trip(fx, http1("GET", "/big", "Accept-Encoding: identity;q=0\r\n"),
                          response));
    CRPC_CHECK_EQ(406, response.status);
    CRPC_CHECK(!has_header_name(response, "Content-Encoding"));
}

CRPC_TEST(http1_response_threshold_skips_small_body) {
    fixture fx;
    parsed_response response;
    CRPC_CHECK(round_trip(fx, http1("GET", "/small", "Accept-Encoding: gzip\r\n"),
                          response));
    CRPC_CHECK_EQ(200, response.status);
    CRPC_CHECK(!has_header_name(response, "Content-Encoding"));
    CRPC_CHECK(has_header(response, "Vary", "Accept-Encoding"));
    CRPC_CHECK_EQ(std::string("small body"), response.body);
}

CRPC_TEST(http1_response_skips_incompressible_type) {
    fixture fx;
    parsed_response response;
    CRPC_CHECK(round_trip(fx, http1("GET", "/image", "Accept-Encoding: gzip\r\n"),
                          response));
    CRPC_CHECK_EQ(200, response.status);
    CRPC_CHECK(!has_header_name(response, "Content-Encoding"));
    CRPC_CHECK(has_header(response, "Vary", "Accept-Encoding"));
}

CRPC_TEST(http1_head_carries_encoded_length_without_body) {
    fixture fx;
    parsed_response response;
    CRPC_CHECK(round_trip(fx, http1("HEAD", "/big", "Accept-Encoding: gzip\r\n"),
                          response, true));
    CRPC_CHECK_EQ(200, response.status);
    CRPC_CHECK(has_header(response, "Content-Encoding", "gzip"));
    CRPC_CHECK(response.body.empty());
    // The advertised length is the encoded length, not the identity length.
    parsed_response get;
    CRPC_CHECK(round_trip(fx, http1("GET", "/big", "Accept-Encoding: gzip\r\n"),
                          get));
    const std::size_t cl = response.headers.find("Content-Length: ");
    const std::size_t get_cl = get.headers.find("Content-Length: ");
    CRPC_CHECK(cl != std::string::npos && get_cl != std::string::npos);
    CRPC_CHECK_EQ(std::strtoull(response.headers.c_str() + cl + 16, nullptr, 10),
                  std::strtoull(get.headers.c_str() + get_cl + 16, nullptr, 10));
}

CRPC_TEST(http1_request_body_is_decoded) {
    fixture fx;
    const std::string plain = "decode me: hello hello hello hello";
    std::string encoded;
    CRPC_CHECK(http::compress_all(http::content_coding::gzip, as_span(plain),
                                  encoded)
                   .ok());
    std::string request =
        "POST /echo HTTP/1.1\r\nHost: test\r\nContent-Encoding: gzip\r\n"
        "Content-Length: ";
    request += std::to_string(encoded.size());
    request += "\r\n\r\n";
    request += encoded;

    parsed_response response;
    CRPC_CHECK(round_trip(fx, request, response));
    CRPC_CHECK_EQ(200, response.status);
    CRPC_CHECK_EQ(plain, fx.state.echo_body);
    CRPC_CHECK_EQ(plain, response.body);
}

CRPC_TEST(http1_request_body_unsupported_encoding_415) {
    fixture fx;
    parsed_response response;
    CRPC_CHECK(round_trip(fx,
                          "POST /echo HTTP/1.1\r\nHost: test\r\n"
                          "Content-Encoding: br\r\nContent-Length: 4\r\n\r\n"
                          "abcd",
                          response));
    CRPC_CHECK_EQ(415, response.status);
}

CRPC_TEST(http1_request_body_bad_data_400) {
    fixture fx;
    parsed_response response;
    CRPC_CHECK(round_trip(fx,
                          "POST /echo HTTP/1.1\r\nHost: test\r\n"
                          "Content-Encoding: gzip\r\nContent-Length: 6\r\n\r\n"
                          "xxxxxx",
                          response));
    CRPC_CHECK_EQ(400, response.status);
}

CRPC_TEST(http1_request_body_over_limit_413) {
    http::http_server_options options;
    options.max_decoded_body = 64;
    fixture fx(options);
    std::string plain(4096, 'a');
    std::string encoded;
    CRPC_CHECK(http::compress_all(http::content_coding::deflate, as_span(plain),
                                  encoded)
                   .ok());
    std::string request =
        "POST /echo HTTP/1.1\r\nHost: test\r\nContent-Encoding: deflate\r\n"
        "Content-Length: ";
    request += std::to_string(encoded.size());
    request += "\r\n\r\n";
    request += encoded;
    parsed_response response;
    CRPC_CHECK(round_trip(fx, request, response));
    CRPC_CHECK_EQ(413, response.status);
}

CRPC_TEST_MAIN()
