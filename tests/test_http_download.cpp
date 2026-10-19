// CRPC -- HTTP file download and Range wiring.
//
// The module-level parser/stream tests live in test_file_transfer.cpp; this
// file targets the session path: routing, 200/206/416, exact byte selection,
// HEAD, path traversal, backpressure and the HTTP/2 fail-closed answer.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
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

// --- Client capture ----------------------------------------------------------

struct capture_source final : io::source {
    std::mutex mu;
    std::string data;

    std::size_t on_data(span<const std::byte> bytes) noexcept override {
        std::lock_guard<std::mutex> lk(mu);
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

struct parsed_response {
    int status = 0;
    std::string headers;
    std::string body;

    [[nodiscard]] bool has(std::string_view name, std::string_view value) const {
        return headers.find(std::string(name) + ": " + std::string(value)) !=
               std::string::npos;
    }
    [[nodiscard]] bool has_name(std::string_view name) const {
        return headers.find(std::string(name) + ":") != std::string::npos;
    }
    [[nodiscard]] std::string header(std::string_view name) const {
        const std::string needle = std::string(name) + ": ";
        const std::size_t pos = headers.find(needle);
        if (pos == std::string::npos) {
            return {};
        }
        const std::size_t end = headers.find("\r\n", pos);
        return headers.substr(pos + needle.size(),
                              end == std::string::npos ? std::string::npos
                                                       : end - pos - needle.size());
    }
};

bool parse_response(const std::string& data, parsed_response& out,
                    bool head_only = false) {
    const std::size_t head_end = data.find("\r\n\r\n");
    if (head_end == std::string::npos) {
        return false;
    }
    out.headers.assign(data.data(), head_end + 2);
    const std::size_t space = data.find(' ');
    out.status = std::atoi(data.c_str() + space + 1);
    if (head_only) {
        out.body.clear();
        return true;
    }
    std::size_t content_length = 0;
    const std::size_t cl = out.header("Content-Length").size();
    if (cl != 0) {
        content_length = std::strtoull(out.header("Content-Length").c_str(),
                                       nullptr, 10);
    }
    const std::size_t body_start = head_end + 4;
    if (data.size() < body_start + content_length) {
        return false;
    }
    out.body.assign(data.data() + body_start, content_length);
    return true;
}

std::string big_payload() {
    std::string data;
    data.reserve(300000);
    for (std::size_t i = 0; i < 300000; ++i) {
        data.push_back(static_cast<char>('a' + (i % 26)));
    }
    return data;
}

std::filesystem::path test_root() {
    std::error_code ec;
    const std::filesystem::path root =
        std::filesystem::temp_directory_path(ec) / "crpc_dl_tests";
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

void write_file(const std::filesystem::path& path, const std::string& data) {
    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    CRPC_CHECK(file != nullptr);
    CRPC_CHECK_EQ(data.size(), std::fwrite(data.data(), 1, data.size(), file));
    std::fclose(file);
}

status handle_plain(void*, const http::request&, const mvc::route_params&,
                    const codec&, const codec_registry&, http::response& out) {
    out.status_code = 200;
    out.reason = "OK";
    out.body = "plain";
    return status{};
}

struct fixture {
    inproc_backend backend;
    mvc::router router;
    std::filesystem::path root;
    std::unique_ptr<http::http_server> server;

    explicit fixture(http::http_server_options options = http::http_server_options{}) {
        root = test_root();
        write_file(root / "small.txt", "0123456789");
        write_file(root / "big.bin", big_payload());
        write_file(root.parent_path() / "crpc_dl_secret.txt", "SECRET");
        mvc::route_entry entry;
        entry.method = http::method::get;
        entry.path = "/plain";
        entry.name = "/plain";
        entry.invoke = &handle_plain;
        CRPC_CHECK(router.add(std::move(entry)).ok());
        CRPC_CHECK(router.freeze().ok());

        http::download_route route;
        route.path = "/files";
        route.root = root;
        http::http_server::builder builder;
        builder.add_listener(endpoint::inproc("download-test"), backend);
        builder.router(router);
        builder.options(options);
        builder.add_download_route(std::move(route));
        server = builder.build();
        CRPC_CHECK(server != nullptr);
        CRPC_CHECK(server->start().ok());
    }

    ~fixture() {
        if (server) {
            server->stop();
        }
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::remove(root.parent_path() / "crpc_dl_secret.txt", ec);
    }

    std::unique_ptr<connection> connect() {
        return backend.connect(endpoint::inproc("download-test"),
                               connect_options{});
    }
};

bool round_trip(fixture& fx, std::string_view request, parsed_response& out,
                bool head_only = false) {
    capture_source capture;
    auto client = fx.connect();
    if (client == nullptr) {
        return false;
    }
    client->bind_source(&capture);
    send_raw(*client, request);
    return crpc_test::wait_for(
        [&] { return parse_response(capture.snapshot(), out, head_only); }, 5000);
}

// --- HTTP/2 helpers ----------------------------------------------------------

struct h2_parse_state {
    h2::hpack_decoder decoder;
    std::string buf;
    std::string header_block;
    bool in_header_block = false;
    int status = 0;
    bool headers_seen = false;
    std::string body;

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
                reinterpret_cast<const std::byte*>(buf.data()) +
                h2::frame_header_size;
            handle(frame, span<const std::byte>(payload, frame.length));
            buf.erase(0, total);
        }
    }

    void handle(const h2::frame_header& frame, span<const std::byte> payload) {
        switch (frame.type) {
        case h2::frame_type::headers:
        case h2::frame_type::continuation: {
            span<const std::byte> fragment;
            if (frame.type == h2::frame_type::headers) {
                h2::headers_payload decoded;
                if (!h2::decode_headers(payload, frame.flags, decoded).ok()) {
                    return;
                }
                fragment = decoded.header_block;
                in_header_block = true;
                header_block.clear();
            } else {
                fragment = payload;
            }
            header_block.append(reinterpret_cast<const char*>(fragment.data()),
                                fragment.size());
            if ((frame.flags & h2::flags::end_headers) != 0 && in_header_block) {
                in_header_block = false;
                http::header_list fields;
                if (decoder
                        .decode(span<const std::byte>(
                                    reinterpret_cast<const std::byte*>(
                                        header_block.data()),
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
            span<const std::byte> fragment;
            if (h2::decode_data(payload, frame.flags, fragment).ok()) {
                body.append(reinterpret_cast<const char*>(fragment.data()),
                            fragment.size());
            }
            break;
        }
        default:
            break;
        }
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

std::string h2_settings(std::initializer_list<h2::setting> entries) {
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
    return h2_frame(h2::frame_type::settings, 0, 0, payload);
}

std::string h2_request(u32 stream_id,
                       std::initializer_list<std::pair<std::string, std::string>>
                           fields,
                       h2::hpack_encoder& encoder) {
    std::vector<h2::header_field> all;
    for (const auto& item : fields) {
        all.push_back({item.first, item.second});
    }
    std::string block;
    (void)encoder.encode(
        span<const h2::header_field>(all.data(), all.size()), block);
    return h2_frame(h2::frame_type::headers,
                    static_cast<u8>(h2::flags::end_headers | h2::flags::end_stream),
                    stream_id, block);
}

int h2_status(fixture& fx, std::string_view path,
              std::initializer_list<std::pair<std::string, std::string>> extra) {
    capture_source capture;
    auto client = fx.connect();
    if (client == nullptr) {
        return 0;
    }
    client->bind_source(&capture);
    h2::hpack_encoder encoder;
    std::string request;
    request += "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    request += h2_settings({});
    std::vector<std::pair<std::string, std::string>> fields;
    fields.push_back({":method", "GET"});
    fields.push_back({":scheme", "http"});
    fields.push_back({":path", std::string(path)});
    fields.push_back({":authority", "test"});
    for (const auto& item : extra) {
        fields.push_back(item);
    }
    // Build the HEADERS block with the dynamic field list.
    std::vector<h2::header_field> hf;
    for (const auto& item : fields) {
        hf.push_back({item.first, item.second});
    }
    std::string block;
    (void)encoder.encode(span<const h2::header_field>(hf.data(), hf.size()),
                         block);
    request += h2_frame(h2::frame_type::headers,
                        static_cast<u8>(h2::flags::end_headers |
                                        h2::flags::end_stream),
                        1, block);
    send_raw(*client, request);

    h2_parse_state state;
    CRPC_CHECK(crpc_test::wait_for(
        [&] {
            state.feed(capture.snapshot());
            return state.headers_seen;
        },
        5000));
    return state.status;
}

} // namespace

CRPC_TEST(download_full_file_200) {
    fixture fx;
    parsed_response response;
    CRPC_CHECK(round_trip(fx, http1("GET", "/files/small.txt"), response));
    CRPC_CHECK_EQ(200, response.status);
    CRPC_CHECK_EQ(std::string("0123456789"), response.body);
    CRPC_CHECK_EQ(std::string("10"), response.header("Content-Length"));
    CRPC_CHECK(response.has("Accept-Ranges", "bytes"));
    CRPC_CHECK(response.has("Vary", "Accept-Encoding"));
}

CRPC_TEST(download_range_returns_206_exact_bytes) {
    fixture fx;
    parsed_response response;
    CRPC_CHECK(round_trip(
        fx, http1("GET", "/files/small.txt", "Range: bytes=2-5\r\n"), response));
    CRPC_CHECK_EQ(206, response.status);
    CRPC_CHECK_EQ(std::string("2345"), response.body);
    CRPC_CHECK_EQ(std::string("bytes 2-5/10"), response.header("Content-Range"));
    CRPC_CHECK_EQ(std::string("4"), response.header("Content-Length"));
    CRPC_CHECK(response.has("Accept-Ranges", "bytes"));
    // Range responses are never content-coded and do not vary on encoding.
    CRPC_CHECK(!response.has_name("Content-Encoding"));
    CRPC_CHECK(!response.has_name("Vary"));
}

CRPC_TEST(download_range_boundaries) {
    fixture fx;
    {
        parsed_response response;
        CRPC_CHECK(round_trip(
            fx, http1("GET", "/files/small.txt", "Range: bytes=0-0\r\n"),
            response));
        CRPC_CHECK_EQ(206, response.status);
        CRPC_CHECK_EQ(std::string("0"), response.body);
    }
    {
        parsed_response response;
        CRPC_CHECK(round_trip(
            fx, http1("GET", "/files/small.txt", "Range: bytes=9-9\r\n"),
            response));
        CRPC_CHECK_EQ(206, response.status);
        CRPC_CHECK_EQ(std::string("9"), response.body);
    }
    {
        parsed_response response;
        CRPC_CHECK(round_trip(
            fx, http1("GET", "/files/small.txt", "Range: bytes=-3\r\n"),
            response));
        CRPC_CHECK_EQ(206, response.status);
        CRPC_CHECK_EQ(std::string("789"), response.body);
        CRPC_CHECK_EQ(std::string("bytes 7-9/10"),
                      response.header("Content-Range"));
    }
    {
        parsed_response response;
        CRPC_CHECK(round_trip(
            fx, http1("GET", "/files/small.txt", "Range: bytes=4-\r\n"),
            response));
        CRPC_CHECK_EQ(206, response.status);
        CRPC_CHECK_EQ(std::string("456789"), response.body);
    }
    {
        parsed_response response;
        CRPC_CHECK(round_trip(
            fx, http1("GET", "/files/small.txt", "Range: bytes=0-999\r\n"),
            response));
        CRPC_CHECK_EQ(206, response.status);
        CRPC_CHECK_EQ(std::string("0123456789"), response.body);
        CRPC_CHECK_EQ(std::string("bytes 0-9/10"),
                      response.header("Content-Range"));
    }
}

CRPC_TEST(download_unsatisfiable_416) {
    fixture fx;
    {
        parsed_response response;
        CRPC_CHECK(round_trip(
            fx, http1("GET", "/files/small.txt", "Range: bytes=20-30\r\n"),
            response));
        CRPC_CHECK_EQ(416, response.status);
        CRPC_CHECK_EQ(std::string("bytes */10"),
                      response.header("Content-Range"));
        CRPC_CHECK_EQ(std::string("0"), response.header("Content-Length"));
        CRPC_CHECK(response.body.empty());
        CRPC_CHECK(response.has("Accept-Ranges", "bytes"));
    }
    {
        parsed_response response;
        CRPC_CHECK(round_trip(
            fx, http1("GET", "/files/small.txt", "Range: bytes=-0\r\n"),
            response));
        CRPC_CHECK_EQ(416, response.status);
    }
}

CRPC_TEST(download_invalid_and_multiple_ranges_are_ignored) {
    fixture fx;
    for (const char* value : {"bytes=abc", "bytes=5-3", "bytes=0-1,3-4",
                              "items=0-1", "bytes"}) {
        parsed_response response;
        std::string extra = "Range: ";
        extra += value;
        extra += "\r\n";
        CRPC_CHECK(round_trip(fx, http1("GET", "/files/small.txt", extra),
                              response));
        CRPC_CHECK_EQ(200, response.status);
        CRPC_CHECK_EQ(std::string("0123456789"), response.body);
    }
}

CRPC_TEST(download_head_has_headers_without_body) {
    fixture fx;
    {
        parsed_response response;
        CRPC_CHECK(round_trip(fx, http1("HEAD", "/files/small.txt"), response,
                              true));
        CRPC_CHECK_EQ(200, response.status);
        CRPC_CHECK_EQ(std::string("10"), response.header("Content-Length"));
        CRPC_CHECK(response.has("Accept-Ranges", "bytes"));
        CRPC_CHECK(response.body.empty());
    }
    {
        parsed_response response;
        CRPC_CHECK(round_trip(
            fx, http1("HEAD", "/files/small.txt", "Range: bytes=0-3\r\n"),
            response, true));
        CRPC_CHECK_EQ(206, response.status);
        CRPC_CHECK_EQ(std::string("bytes 0-3/10"),
                      response.header("Content-Range"));
        CRPC_CHECK_EQ(std::string("4"), response.header("Content-Length"));
        CRPC_CHECK(response.body.empty());
    }
}

CRPC_TEST(download_if_range_without_validator_ignores_range) {
    fixture fx;
    parsed_response response;
    CRPC_CHECK(round_trip(
        fx,
        http1("GET", "/files/small.txt",
              "Range: bytes=2-5\r\nIf-Range: \"etag\"\r\n"),
        response));
    CRPC_CHECK_EQ(200, response.status);
    CRPC_CHECK_EQ(std::string("0123456789"), response.body);
}

CRPC_TEST(download_path_traversal_is_404) {
    fixture fx;
    for (const char* path : {"/files/../crpc_dl_secret.txt",
                             "/files/%2e%2e/crpc_dl_secret.txt",
                             "/files/%2E%2E/crpc_dl_secret.txt",
                             "/files/./small.txt"}) {
        parsed_response response;
        CRPC_CHECK(round_trip(fx, http1("GET", path), response));
        CRPC_CHECK_EQ(404, response.status);
        CRPC_CHECK(response.body.find("SECRET") == std::string::npos);
    }
}

CRPC_TEST(download_missing_file_is_404) {
    fixture fx;
    parsed_response response;
    CRPC_CHECK(round_trip(fx, http1("GET", "/files/nope.txt"), response));
    CRPC_CHECK_EQ(404, response.status);
}

CRPC_TEST(download_non_get_is_405) {
    fixture fx;
    parsed_response response;
    CRPC_CHECK(round_trip(
        fx, http1("POST", "/files/small.txt", "Content-Length: 0\r\n"),
        response));
    CRPC_CHECK_EQ(405, response.status);
    CRPC_CHECK(response.has("Allow", "GET, HEAD"));
}

CRPC_TEST(download_large_file_streams_with_backpressure) {
    http::http_server_options options;
    options.egress_cap_bytes = 4096;
    options.download_chunk_bytes = 8192;
    fixture fx(options);
    parsed_response response;
    CRPC_CHECK(round_trip(fx, http1("GET", "/files/big.bin"), response));
    CRPC_CHECK_EQ(200, response.status);
    CRPC_CHECK_EQ(std::string("300000"), response.header("Content-Length"));
    CRPC_CHECK_EQ(big_payload(), response.body);
}

CRPC_TEST(download_206_never_content_coded) {
    fixture fx;
    parsed_response response;
    CRPC_CHECK(round_trip(
        fx, http1("GET", "/files/big.bin",
                  "Range: bytes=0-1999\r\nAccept-Encoding: gzip\r\n"),
        response));
    CRPC_CHECK_EQ(206, response.status);
    CRPC_CHECK(!response.has_name("Content-Encoding"));
    CRPC_CHECK(!response.has_name("Vary"));
}

CRPC_TEST(h2_download_route_returns_501) {
    fixture fx;
    CRPC_CHECK_EQ(501, h2_status(fx, "/files/small.txt", {}));
    CRPC_CHECK_EQ(501, h2_status(fx, "/files/small.txt",
                                 {{"range", "bytes=0-3"}}));
}

CRPC_TEST(h2_request_content_coding_returns_501) {
    fixture fx;
    CRPC_CHECK_EQ(501, h2_status(fx, "/files/small.txt",
                                 {{"content-encoding", "gzip"}}));
}

CRPC_TEST(h2_accept_encoding_negotiation) {
    fixture fx;
    // Identity is acceptable on an ordinary route: served normally.
    CRPC_CHECK_EQ(200,
                  h2_status(fx, "/plain", {{"accept-encoding", "identity"}}));
    // Selecting gzip would require h2 response compression, which this slice
    // does not implement: fail closed.
    CRPC_CHECK_EQ(501, h2_status(fx, "/plain", {{"accept-encoding", "gzip"}}));
    // Identity refused and no supported coding acceptable -> 406.
    CRPC_CHECK_EQ(406, h2_status(fx, "/plain",
                                 {{"accept-encoding", "identity;q=0"}}));
}

CRPC_TEST_MAIN()
