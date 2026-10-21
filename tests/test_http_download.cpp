// CRPC -- HTTP file download and Range wiring.
//
// The module-level parser/stream tests live in test_file_transfer.cpp; this
// file targets the session path: routing, 200/206/416, exact byte selection,
// HEAD, path traversal, backpressure and the HTTP/2 fail-closed answer.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <algorithm>
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

// `test_root()` names its directory after the process id. On Windows that is
// GetCurrentProcessId(); everywhere else getpid().
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

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

// A peer that accepts at most `budget` bytes per `on_data` call and then
// returns short (the backend pauses reading until `resume_read`). The test
// resumes it on a timer, producing steady but slow progress.
struct slow_source final : io::source {
    std::mutex mu;
    std::string data;
    std::size_t budget = 1024;

    std::size_t on_data(span<const std::byte> bytes) noexcept override {
        std::lock_guard<std::mutex> lk(mu);
        const std::size_t take = std::min(budget, bytes.size());
        data.append(reinterpret_cast<const char*>(bytes.data()), take);
        return take;
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
        content_length = static_cast<std::size_t>(
            std::strtoull(out.header("Content-Length").c_str(), nullptr, 10));
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
    // A per-process directory: two copies of this executable (ctest -j, or a
    // parallel build tree) otherwise delete each other's fixtures while the
    // other run is mid-download, which shows up as a spurious stall or a 404.
    const auto pid = static_cast<unsigned long long>(
#if defined(_WIN32)
        ::GetCurrentProcessId()
#else
        ::getpid()
#endif
    );
    const std::filesystem::path root = std::filesystem::temp_directory_path(ec) /
                                       ("crpc_dl_tests_" + std::to_string(pid));
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

// The traversal fixture must sit outside the served root. Naming it after the
// process id keeps two concurrent runs (ctest -j, parallel build trees) from
// deleting each other's file.
std::filesystem::path secret_path() {
    const auto pid = static_cast<unsigned long long>(
#if defined(_WIN32)
        ::GetCurrentProcessId()
#else
        ::getpid()
#endif
    );
    return std::filesystem::temp_directory_path() /
           ("crpc_dl_secret_" + std::to_string(pid) + ".txt");
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

    explicit fixture(
        http::http_server_options options = http::http_server_options{},
        std::string content_type = "application/octet-stream") {
        root = test_root();
        write_file(root / "small.txt", "0123456789");
        write_file(root / "big.bin", big_payload());
        // Larger than the inproc peer queue (64 KiB) so a non-draining peer
        // actually defers the server's in-flight write.
        write_file(root / "slow.bin", big_payload().substr(0, 80000));
        write_file(secret_path(), "SECRET");
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
        route.content_type = std::move(content_type);
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
        std::filesystem::remove(secret_path(), ec);
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
    bool vary_accept_encoding = false;
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
                        } else if (field.name == "vary" &&
                                   field.value.find("Accept-Encoding") !=
                                       std::string::npos) {
                            vary_accept_encoding = true;
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

int h2_status(fixture& fx, std::string_view path,
              std::initializer_list<std::pair<std::string, std::string>> extra,
              bool* vary_accept_encoding = nullptr) {
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
    if (vary_accept_encoding != nullptr) {
        *vary_accept_encoding = state.vary_accept_encoding;
    }
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
    CRPC_CHECK_EQ(std::string("application/octet-stream"),
                  response.header("Content-Type"));
}

CRPC_TEST(download_empty_content_type_falls_back) {
    fixture fx(http::http_server_options{}, "");
    parsed_response response;
    CRPC_CHECK(round_trip(fx, http1("GET", "/files/small.txt"), response));
    CRPC_CHECK_EQ(200, response.status);
    CRPC_CHECK_EQ(std::string("application/octet-stream"),
                  response.header("Content-Type"));
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
    CRPC_CHECK_EQ(std::string("application/octet-stream"),
                  response.header("Content-Type"));
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
    // The secret really exists one level above the root, so a 404 proves the
    // traversal was rejected rather than that the target was missing.
    const std::string secret = secret_path().filename().string();
    const std::string paths[] = {"/files/../" + secret,
                                 "/files/%2e%2e/" + secret,
                                 "/files/%2E%2E/" + secret,
                                 "/files/./small.txt"};
    for (const std::string& path : paths) {
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

CRPC_TEST(download_head_404_has_no_body) {
    fixture fx;
    capture_source capture;
    auto client = fx.connect();
    CRPC_CHECK(client != nullptr);
    client->bind_source(&capture);
    send_raw(*client, http1("HEAD", "/files/nope.txt"));
    CRPC_CHECK(crpc_test::wait_for(
        [&] {
            return capture.snapshot().find("\r\n\r\n") != std::string::npos;
        },
        5000));
    // Give a buggy body a chance to arrive, then require the response to end
    // exactly at the header terminator.
    (void)crpc_test::wait_for(
        [&] {
            const std::string data = capture.snapshot();
            const std::size_t end = data.find("\r\n\r\n");
            return end != std::string::npos && data.size() > end + 4;
        },
        200);
    const std::string data = capture.snapshot();
    const std::size_t end = data.find("\r\n\r\n");
    CRPC_CHECK_EQ(end + 4, data.size());
}

// A slow peer that still drains bytes must survive longer than the stall
// timeout: every write completion refreshes the deadline.
CRPC_TEST(download_slow_progress_survives_stall_timeout) {
    http::http_server_options options;
    options.download_chunk_bytes = 8192;
    options.egress_cap_bytes = 32u * 1024u;
    // 500 ms stall budget against a 50 ms resume cadence: 450 ms of margin
    // absorbs scheduler delay on a loaded host. The no-progress case below
    // keeps 100 ms because there the deadline itself is what is asserted.
    options.download_stall_timeout = std::chrono::milliseconds(500);
    fixture fx(options);

    slow_source capture;
    capture.budget = 8192;
    auto client = fx.connect();
    CRPC_CHECK(client != nullptr);
    client->bind_source(&capture);
    send_raw(*client, http1("GET", "/files/slow.bin"));

    auto last_resume = std::chrono::steady_clock::now();
    parsed_response response;
    bool complete = false;
    const bool ok = crpc_test::wait_for(
        [&] {
            if (parse_response(capture.snapshot(), response)) {
                complete = true;
                return true;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - last_resume >= std::chrono::milliseconds(50)) {
                last_resume = now;
                client->resume_read();
            }
            return false;
        },
        15000);
    CRPC_CHECK(ok);
    CRPC_CHECK(complete);
    CRPC_CHECK_EQ(200, response.status);
    CRPC_CHECK_EQ(std::string("80000"), response.header("Content-Length"));
    CRPC_CHECK_EQ(big_payload().substr(0, 80000), response.body);
}

// A peer that drains nothing at all must be closed at the stall deadline; the
// download cannot make progress and the connection is reaped, not held open.
CRPC_TEST(download_no_progress_is_killed_by_stall) {
    http::http_server_options options;
    options.download_chunk_bytes = 8192;
    options.egress_cap_bytes = 32u * 1024u;
    // Deliberately left at 100 ms: this case asserts that zero progress is
    // killed at the deadline, so the tight deadline is the subject of the test,
    // not a scheduling margin.
    options.download_stall_timeout = std::chrono::milliseconds(100);
    fixture fx(options);

    slow_source capture;
    capture.budget = 0;
    auto client = fx.connect();
    CRPC_CHECK(client != nullptr);
    client->bind_source(&capture);
    send_raw(*client, http1("GET", "/files/big.bin"));

    const bool killed = crpc_test::wait_for(
        [&] { return fx.server->stats().timed_out_connections >= 1; }, 5000);
    CRPC_CHECK(killed);
    CRPC_CHECK(capture.snapshot().empty());
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
    // A normal h2 client offers gzip: serve identity 200 (no 501) and advertise
    // the variance.
    bool vary = false;
    CRPC_CHECK_EQ(200, h2_status(fx, "/plain", {{"accept-encoding", "gzip"}},
                                 &vary));
    CRPC_CHECK(vary);
    // Identity refused and no supported coding acceptable -> 406 + Vary.
    bool denied_vary = false;
    CRPC_CHECK_EQ(406, h2_status(fx, "/plain",
                                 {{"accept-encoding", "identity;q=0"}},
                                 &denied_vary));
    CRPC_CHECK(denied_vary);
}

CRPC_TEST_MAIN()
