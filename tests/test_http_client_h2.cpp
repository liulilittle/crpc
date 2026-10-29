// CRPC -- HTTP/2 client tests: negotiation, h2c, multiplexing and wire checks.
//
// Coverage targets:
//   * h2c prior knowledge against the library's own `http_server` (the response
//     version must be HTTP/2, not a downgraded HTTP/1.1 answer);
//   * the cleartext default stays HTTP/1.1 unless `client_options::h2c` is set;
//   * multiplexing: three requests issued on one connection before any response
//     is read, responses collected out of order;
//   * wire-level assertions through a scripted in-process HTTP/2 endpoint
//     (raw frames + an independent HPACK decoder): pseudo-headers first,
//     forbidden connection-specific fields filtered, lower-case names,
//     content-length mismatch reported, RST_STREAM/GOAWAY handling,
//     SETTINGS_MAX_CONCURRENT_STREAMS=0, flow-controlled large bodies,
//     trailers;
//   * the connection layer accepts SETTINGS_ENABLE_PUSH=0 from a server (the
//     library's own server advertises it) and records the value;
//   * TLS + ALPN negotiation and the HTTP/1.1 fallback when the library is
//     built with OpenSSL; a clear refusal without it.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/http/client.hpp>
#include <crpc/http/h2_client.hpp>
#include <crpc/http/message.hpp>
#include <crpc/http/server.hpp>
#include <crpc/http2/connection.hpp>
#include <crpc/http2/frame.hpp>
#include <crpc/http2/hpack.hpp>
#include <crpc/io.hpp>
#include <crpc/mvc/router.hpp>

#include "test_tcp_backend.hpp"
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "test_framework.hpp"

#if defined(CRPC_HAVE_OPENSSL)
#include <crpc/io/tls.hpp>
#include "tls_test_certificate.hpp"
#endif

using namespace crpc;

namespace h2 = crpc::http2;

namespace {

[[nodiscard]] span<const std::byte> as_bytes(std::string_view text) noexcept {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                 text.size());
}

[[nodiscard]] std::string as_string(span<const std::byte> bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

[[nodiscard]] span<const std::byte> span_of(const std::vector<std::byte>& bytes) {
    return span<const std::byte>(bytes.data(), bytes.size());
}

constexpr std::string_view h2_preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

// --- Server-side write plumbing ----------------------------------------------

// A heap completion the test can wait on. An inproc push completes on the
// caller's stack, but the wait keeps the pattern honest for any backend.
struct server_write {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;
};

class write_completion final : public io::completion {
public:
    explicit write_completion(std::shared_ptr<server_write> state)
        : state_(std::move(state)) {}

    void complete(io::io_result result) noexcept override {
        {
            std::lock_guard<std::mutex> lk(state_->mu);
            state_->done = true;
            state_->ok = !result.ec;
        }
        state_->cv.notify_all();
        delete this;
    }

private:
    std::shared_ptr<server_write> state_;
};

bool wait_write(const std::shared_ptr<server_write>& write, int timeout_ms) {
    std::unique_lock<std::mutex> lk(write->mu);
    if (!write->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                            [&] { return write->done; })) {
        return false;
    }
    return write->ok;
}

[[nodiscard]] std::string frame_bytes(h2::frame_header header,
                                      span<const std::byte> payload) {
    std::string out(h2::frame_header_size + payload.size(), '\0');
    std::size_t written = 0;
    const status encoded = h2::encode_frame(
        span<std::byte>(reinterpret_cast<std::byte*>(out.data()), out.size()),
        header, payload, written);
    CRPC_CHECK(encoded.ok());
    out.resize(written);
    return out;
}

// --- A scripted in-process HTTP/2 endpoint ------------------------------------
//
// The fixture listens on an inproc endpoint and speaks raw HTTP/2 frames to
// the client under test. inproc deliveries run on the pushing thread (the
// client's), so the source callbacks only append to a bounded buffer and wake
// the test; all parsing and replies happen on the test thread.

struct captured_frame {
    h2::frame_header header{};
    std::vector<std::byte> payload;
};

struct captured_block {
    u32 stream_id = 0;
    http::header_list fields;
    bool end_stream = false;
};

class scripted_h2_server final : public io::source {
public:
    scripted_h2_server(crpc::backend_factory& backend, std::string name)
        : backend_(backend), name_(std::move(name)) {
        listener_ = backend_.listen(endpoint::inproc(name_), listen_options{});
        CRPC_CHECK(listener_ != nullptr);
        if (listener_ != nullptr) {
            listener_->start(&accepts_);
        }
    }

    ~scripted_h2_server() override { close(); }

    scripted_h2_server(const scripted_h2_server&) = delete;
    scripted_h2_server& operator=(const scripted_h2_server&) = delete;

    // Waits for the first accepted connection and installs the source.
    [[nodiscard]] bool adopt(int timeout_ms = 5000) {
        conn_ = accepts_.wait_for(std::chrono::milliseconds(timeout_ms));
        if (!conn_) {
            return false;
        }
        conn_->bind_source(this);
        return true;
    }

    // --- source (runs on the client's thread) --------------------------------

    std::size_t on_data(span<const std::byte> data) noexcept override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (wire_.size() < 1024u * 1024u) {
                wire_.append(reinterpret_cast<const char*>(data.data()), data.size());
            }
        }
        wake_.notify_all();
        return data.size();
    }

    void on_eof() noexcept override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            eof_ = true;
        }
        wake_.notify_all();
    }

    void on_error(std::error_code) noexcept override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            eof_ = true;
        }
        wake_.notify_all();
    }

    // --- test-side observation -----------------------------------------------

    // Waits until the connection preface and one SETTINGS frame arrived and
    // pops the SETTINGS frame.
    [[nodiscard]] bool wait_client_settings(captured_frame& out, int timeout_ms = 5000) {
        const bool ready = crpc_test::wait_for(
            [&] {
                std::lock_guard<std::mutex> lk(mu_);
                pump_locked();
                for (auto it = frames_.begin(); it != frames_.end(); ++it) {
                    if (it->header.type == h2::frame_type::settings &&
                        !it->header.has_flag(h2::flags::ack)) {
                        out = *it;
                        frames_.erase(it);
                        return true;
                    }
                }
                return false;
            },
            timeout_ms);
        return ready && saw_preface();
    }

    [[nodiscard]] bool saw_preface() const {
        std::lock_guard<std::mutex> lk(mu_);
        return saw_preface_;
    }

    // Waits for one complete request header block on `stream` (HPACK-decoded
    // with this connection's decoder) and pops it.
    [[nodiscard]] bool wait_headers(u32 stream, captured_block& out, int timeout_ms = 5000) {
        return crpc_test::wait_for(
            [&] {
                return pop_block(stream, out);
            },
            timeout_ms);
    }

    // Waits for one frame of `type` (optionally on `stream`; 0 matches any)
    // and pops it. SETTINGS ACKs and client WINDOW_UPDATEs are ignorable
    // noise, so `type` matching skips them by construction.
    [[nodiscard]] bool wait_frame(h2::frame_type type, u32 stream, captured_frame& out,
                                  int timeout_ms = 5000) {
        return crpc_test::wait_for(
            [&] {
                std::lock_guard<std::mutex> lk(mu_);
                pump_locked();
                for (auto it = frames_.begin(); it != frames_.end(); ++it) {
                    if (it->header.type != type) {
                        continue;
                    }
                    if (stream != 0 && it->header.stream_id != stream) {
                        continue;
                    }
                    out = *it;
                    frames_.erase(it);
                    return true;
                }
                return false;
            },
            timeout_ms);
    }

    [[nodiscard]] bool wait_data(u32 stream, std::string& out, int timeout_ms = 5000) {
        captured_frame frame;
        if (!wait_frame(h2::frame_type::data, stream, frame, timeout_ms)) {
            return false;
        }
        span<const std::byte> data;
        CRPC_CHECK(h2::decode_data(span_of(frame.payload), frame.header.flags, data).ok());
        out = as_string(data);
        return true;
    }

    // Every DATA byte received on `stream`, assembled across frames.
    [[nodiscard]] std::string data_of(u32 stream) {
        std::lock_guard<std::mutex> lk(mu_);
        pump_locked();
        const auto it = data_.find(stream);
        return it == data_.end() ? std::string() : it->second;
    }

    [[nodiscard]] bool has_data(u32 stream) {
        std::lock_guard<std::mutex> lk(mu_);
        pump_locked();
        if (data_.find(stream) != data_.end()) {
            return true;
        }
        for (const captured_frame& frame : frames_) {
            if (frame.header.type == h2::frame_type::data &&
                frame.header.stream_id == stream) {
                return true;
            }
        }
        return false;
    }

    // --- server -> client sends ----------------------------------------------

    [[nodiscard]] bool send_raw(std::string_view bytes) {
        connection* conn = nullptr;
        {
            std::lock_guard<std::mutex> lk(mu_);
            conn = conn_.get();
        }
        if (conn == nullptr) {
            return false;
        }
        auto state = std::make_shared<server_write>();
        auto* completion = new write_completion(state);
        conn->sink().push(as_bytes(bytes), completion);
        return wait_write(state, 5000);
    }

    [[nodiscard]] bool send_settings(std::vector<h2::setting> entries) {
        std::string payload(entries.size() * 6, '\0');
        std::size_t written = 0;
        const status encoded = h2::encode_settings(
            span<std::byte>(reinterpret_cast<std::byte*>(payload.data()), payload.size()),
            span<const h2::setting>(entries.data(), entries.size()), written);
        CRPC_CHECK(encoded.ok());
        payload.resize(written);
        h2::frame_header header;
        header.type = h2::frame_type::settings;
        header.flags = 0;
        header.stream_id = 0;
        header.length = static_cast<u32>(payload.size());
        return send_raw(frame_bytes(header, as_bytes(payload)));
    }

    [[nodiscard]] bool send_window_update(u32 stream, u32 increment) {
        std::string payload(4, '\0');
        std::size_t written = 0;
        const status encoded = h2::encode_window_update(
            span<std::byte>(reinterpret_cast<std::byte*>(payload.data()), payload.size()),
            increment, written);
        CRPC_CHECK(encoded.ok());
        payload.resize(written);
        h2::frame_header header;
        header.type = h2::frame_type::window_update;
        header.flags = 0;
        header.stream_id = stream;
        header.length = static_cast<u32>(payload.size());
        return send_raw(frame_bytes(header, as_bytes(payload)));
    }

    [[nodiscard]] bool send_headers(u32 stream, std::vector<h2::header_field> fields,
                                    bool end_stream) {
        std::string block;
        const status encoded = response_encoder_.encode(
            span<const h2::header_field>(fields.data(), fields.size()), block);
        CRPC_CHECK(encoded.ok());
        h2::frame_header header;
        header.type = h2::frame_type::headers;
        header.flags = h2::flags::end_headers |
                       (end_stream ? h2::flags::end_stream : static_cast<u8>(0));
        header.stream_id = stream;
        header.length = static_cast<u32>(block.size());
        return send_raw(frame_bytes(header, as_bytes(block)));
    }

    [[nodiscard]] bool send_trailers(u32 stream, std::vector<h2::header_field> fields) {
        return send_headers(stream, std::move(fields), true);
    }

    [[nodiscard]] bool send_data(u32 stream, std::string_view bytes, bool end_stream) {
        h2::frame_header header;
        header.type = h2::frame_type::data;
        header.flags = end_stream ? h2::flags::end_stream : static_cast<u8>(0);
        header.stream_id = stream;
        header.length = static_cast<u32>(bytes.size());
        return send_raw(frame_bytes(header, as_bytes(bytes)));
    }

    [[nodiscard]] bool send_rst(u32 stream, h2::error_code code) {
        std::string payload(4, '\0');
        std::size_t written = 0;
        const status encoded = h2::encode_rst_stream(
            span<std::byte>(reinterpret_cast<std::byte*>(payload.data()), payload.size()),
            code, written);
        CRPC_CHECK(encoded.ok());
        payload.resize(written);
        h2::frame_header header;
        header.type = h2::frame_type::rst_stream;
        header.flags = 0;
        header.stream_id = stream;
        header.length = static_cast<u32>(payload.size());
        return send_raw(frame_bytes(header, as_bytes(payload)));
    }

    [[nodiscard]] bool send_goaway(u32 last_stream_id, h2::error_code code) {
        h2::goaway_payload payload;
        payload.last_stream_id = last_stream_id;
        payload.code = code;
        std::string bytes(8, '\0');
        std::size_t written = 0;
        const status encoded = h2::encode_goaway(
            span<std::byte>(reinterpret_cast<std::byte*>(bytes.data()), bytes.size()),
            payload, written);
        CRPC_CHECK(encoded.ok());
        bytes.resize(written);
        h2::frame_header header;
        header.type = h2::frame_type::goaway;
        header.flags = 0;
        header.stream_id = 0;
        header.length = static_cast<u32>(bytes.size());
        return send_raw(frame_bytes(header, as_bytes(bytes)));
    }

    void close() {
        std::unique_ptr<connection> conn;
        {
            std::lock_guard<std::mutex> lk(mu_);
            conn = std::move(conn_);
        }
        if (conn) {
            // Unbind outside the lock: bind_source waits for in-flight source
            // callbacks, which take the same mutex.
            conn->bind_source(nullptr);
            conn->close();
        }
    }

private:
    class accept_queue final : public accept_sink {
    public:
        void on_accept(std::unique_ptr<connection> accepted) noexcept override {
            if (!accepted) {
                return;
            }
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (!slot_) {
                    slot_ = std::move(accepted);
                }
            }
            cv_.notify_all();
        }

        [[nodiscard]] std::unique_ptr<connection> wait_for(std::chrono::milliseconds timeout) {
            std::unique_lock<std::mutex> lk(mu_);
            if (!cv_.wait_for(lk, timeout, [&] { return slot_ != nullptr; })) {
                return nullptr;
            }
            return std::move(slot_);
        }

    private:
        std::mutex mu_;
        std::condition_variable cv_;
        std::unique_ptr<connection> slot_;
    };

    // Parses every complete frame out of the raw byte buffer. The 24-byte
    // client preface is consumed once; HEADERS/CONTINUATION payloads are
    // accumulated into a block and HPACK-decoded when END_HEADERS arrives.
    void pump_locked() {
        for (;;) {
            if (!saw_preface_) {
                if (wire_.size() < h2_preface.size()) {
                    return;
                }
                if (std::memcmp(wire_.data(), h2_preface.data(), h2_preface.size()) != 0) {
                    // Not HTTP/2: keep the bytes for diagnosis.
                    preface_bad_ = true;
                    return;
                }
                wire_.erase(0, h2_preface.size());
                saw_preface_ = true;
            }
            if (wire_.size() < h2::frame_header_size) {
                return;
            }
            h2::frame_header header;
            const status decoded = h2::decode_frame_header(
                span<const std::byte>(reinterpret_cast<const std::byte*>(wire_.data()),
                                      h2::frame_header_size),
                header, 1u << 20u);
            if (!decoded.ok()) {
                parse_error_ = true;
                return;
            }
            const std::size_t total = h2::frame_header_size + header.length;
            if (wire_.size() < total) {
                return;
            }
            captured_frame frame;
            frame.header = header;
            frame.payload.assign(
                reinterpret_cast<const std::byte*>(wire_.data()) + h2::frame_header_size,
                reinterpret_cast<const std::byte*>(wire_.data()) + total);
            wire_.erase(0, static_cast<std::size_t>(total));
            ingest_frame(frame);
            frames_.push_back(std::move(frame));
        }
    }

    void ingest_frame(const captured_frame& frame) {
        if (frame.header.type == h2::frame_type::data) {
            span<const std::byte> data;
            const status decoded =
                h2::decode_data(span_of(frame.payload), frame.header.flags, data);
            CRPC_CHECK(decoded.ok());
            data_[frame.header.stream_id].append(as_string(data));
        } else if (frame.header.type == h2::frame_type::headers) {
            h2::headers_payload payload;
            const status decoded =
                h2::decode_headers(span_of(frame.payload), frame.header.flags, payload);
            CRPC_CHECK(decoded.ok());
            pending_end_stream_ = payload.end_stream;
            pending_block_.insert(pending_block_.end(), payload.header_block.begin(),
                                  payload.header_block.end());
            if (payload.end_headers) {
                finish_block(frame.header.stream_id);
            }
        } else if (frame.header.type == h2::frame_type::continuation) {
            pending_block_.insert(pending_block_.end(), frame.payload.begin(),
                                  frame.payload.end());
            if (frame.header.has_flag(h2::flags::end_headers)) {
                finish_block(frame.header.stream_id);
            }
        }
    }

    void finish_block(u32 stream_id) {
        captured_block block;
        block.stream_id = stream_id;
        block.end_stream = pending_end_stream_;
        pending_end_stream_ = false;
        const status decoded = request_decoder_.decode(
            span<const std::byte>(pending_block_.data(), pending_block_.size()),
            block.fields);
        CRPC_CHECK(decoded.ok());
        pending_block_.clear();
        blocks_.push_back(std::move(block));
    }

    [[nodiscard]] bool pop_block(u32 stream, captured_block& out) {
        std::lock_guard<std::mutex> lk(mu_);
        pump_locked();
        for (auto it = blocks_.begin(); it != blocks_.end(); ++it) {
            if (it->stream_id == stream) {
                out = *it;
                blocks_.erase(it);
                return true;
            }
        }
        return false;
    }

    crpc::backend_factory& backend_;
    std::string name_;
    std::unique_ptr<listener> listener_;
    accept_queue accepts_;
    std::unique_ptr<connection> conn_;

    mutable std::mutex mu_;
    std::condition_variable wake_;
    std::string wire_;
    bool saw_preface_ = false;
    bool preface_bad_ = false;
    bool parse_error_ = false;
    bool eof_ = false;
    std::deque<captured_frame> frames_;
    std::deque<captured_block> blocks_;
    std::map<u32, std::string> data_;
    std::vector<std::byte> pending_block_;
    bool pending_end_stream_ = false;
    h2::hpack_decoder request_decoder_{};
    h2::hpack_encoder response_encoder_{};
};

// --- Library HTTP server fixture ---------------------------------------------

status handle_root(void*, const http::request&, const mvc::route_params&,
                   const codec&, const codec_registry&, http::response& out) {
    out.status_code = 200;
    out.reason = "OK";
    out.headers.add("Content-Type", "text/plain");
    out.body = "hello";
    return status{};
}

status handle_echo(void*, const http::request& request, const mvc::route_params&,
                   const codec&, const codec_registry&, http::response& out) {
    out.status_code = 200;
    out.reason = "OK";
    out.headers.add("Content-Type", "application/json");
    out.body = request.body;
    return status{};
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

struct http_fixture {
    inproc_backend backend;
    mvc::router router;
    std::unique_ptr<http::http_server> server;
    std::string name;

    explicit http_fixture(std::string endpoint_name) : name(std::move(endpoint_name)) {
        add_route(router, http::method::get, "/root", &handle_root);
        add_route(router, http::method::post, "/echo", &handle_echo);
        const status frozen = router.freeze();
        CRPC_CHECK(frozen.ok());

        http::http_server::builder builder;
        builder.add_listener(endpoint::inproc(name), backend);
        builder.router(router);
        server = builder.build();
        CRPC_CHECK(server != nullptr);
        const status started = server->start();
        CRPC_CHECK(started.ok());
    }

    ~http_fixture() {
        if (server) {
            server->stop();
        }
    }

    http_fixture(const http_fixture&) = delete;
    http_fixture& operator=(const http_fixture&) = delete;
};

[[nodiscard]] std::string unique_name(const char* base) {
    static std::atomic<unsigned> counter{0};
    return std::string(base) + "-" +
           std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

// --- Connection layer: SETTINGS_ENABLE_PUSH direction ------------------------
//
// RFC 7540 6.5.2 bounds the value to 0/1 and leaves the direction free: a
// server advertising ENABLE_PUSH=0 (the library's own server does) must not
// fail a client-role connection, and the client-to-server direction stays
// meaningful for a server-role connection.

CRPC_TEST(client_role_accepts_settings_enable_push_zero) {
    h2::connection_options options;
    options.role = h2::connection_role::client;
    h2::connection conn(options);
    const std::vector<h2::setting> entries{h2::setting{
        static_cast<u16>(h2::setting_id::enable_push), 0}};
    std::vector<std::byte> payload(entries.size() * 6);
    std::size_t written = 0;
    CRPC_CHECK(h2::encode_settings(span<std::byte>(payload.data(), payload.size()),
                                    span<const h2::setting>(entries.data(), entries.size()),
                                    written)
                   .ok());
    payload.resize(written);
    h2::frame_header header;
    header.type = h2::frame_type::settings;
    header.flags = 0;
    header.stream_id = 0;
    header.length = static_cast<u32>(payload.size());
    const h2::h2_error error =
        conn.receive(header, span<const std::byte>(payload.data(), payload.size()));
    CRPC_CHECK(error.ok());
    CRPC_CHECK(error.scope != h2::error_scope::connection);
    CRPC_CHECK_FALSE(conn.remote_settings().enable_push);
    CRPC_CHECK(conn.pending_settings_ack());
}

CRPC_TEST(server_role_records_settings_enable_push_one) {
    h2::connection_options options;
    options.role = h2::connection_role::server;
    h2::connection conn(options);
    // The client-to-server direction is the meaningful one: a client that
    // allows push sends 1, and the server must record it, not refuse it.
    const std::vector<h2::setting> entries{h2::setting{
        static_cast<u16>(h2::setting_id::enable_push), 1}};
    std::vector<std::byte> payload(entries.size() * 6);
    std::size_t written = 0;
    CRPC_CHECK(h2::encode_settings(span<std::byte>(payload.data(), payload.size()),
                                    span<const h2::setting>(entries.data(), entries.size()),
                                    written)
                   .ok());
    payload.resize(written);
    h2::frame_header header;
    header.type = h2::frame_type::settings;
    header.flags = 0;
    header.stream_id = 0;
    header.length = static_cast<u32>(payload.size());
    const h2::h2_error error =
        conn.receive(header, span<const std::byte>(payload.data(), payload.size()));
    CRPC_CHECK(error.ok());
    CRPC_CHECK(conn.remote_settings().enable_push);
}

CRPC_TEST(client_options_defaults_offer_h2_then_http_1_1) {
    const http::client_options options;
    CRPC_CHECK_EQ(std::size_t{2}, options.tls_alpn.size());
    if (options.tls_alpn.size() == 2) {
        CRPC_CHECK_EQ(std::string("h2"), options.tls_alpn[0]);
        CRPC_CHECK_EQ(std::string("http/1.1"), options.tls_alpn[1]);
    }
    CRPC_CHECK_FALSE(options.h2c);
}

// --- Scripted-endpoint helpers ------------------------------------------------

[[nodiscard]] std::string field(const http::header_list& fields, const char* name) {
    const std::string* value = fields.find(name);
    return value == nullptr ? std::string() : *value;
}

[[nodiscard]] bool has_field(const http::header_list& fields, const char* name) {
    return fields.find(name) != nullptr;
}

// Waits for the client's HEADERS on `stream` and asserts the pseudo-header
// contract: all four pseudo fields first, no forbidden connection-specific
// field, lower-case names.
void check_request_head(scripted_h2_server& server, u32 stream, std::string_view method,
                        std::string_view path, const http::header_list& expect_regular,
                        std::size_t body_size) {
    captured_block request;
    CRPC_CHECK(server.wait_headers(stream, request));
    if (request.fields.empty()) {
        // wait_headers already failed via its own timeout; avoid noise.
        return;
    }
    CRPC_CHECK(request.fields.size() >= 4);
    if (request.fields.size() < 4) {
        return;
    }
    // Pseudo-headers first: the first four fields, in order.
    CRPC_CHECK_EQ(std::string(":method"), request.fields.begin()->name);
    CRPC_CHECK_EQ(std::string(method), request.fields.begin()->value);
    CRPC_CHECK_EQ(std::string(":scheme"), request.fields.begin()[1].name);
    CRPC_CHECK_EQ(std::string(":path"), request.fields.begin()[2].name);
    CRPC_CHECK_EQ(std::string(path), request.fields.begin()[2].value);
    CRPC_CHECK_EQ(std::string(":authority"), request.fields.begin()[3].name);
    // No pseudo-header may appear after a regular one (RFC 9113 8.3).
    bool pseudo_done = false;
    for (const http::header& item : request.fields) {
        if (!item.name.empty() && item.name.front() == ':') {
            CRPC_CHECK_FALSE(pseudo_done);
            const bool known = item.name == ":method" || item.name == ":scheme" ||
                               item.name == ":path" || item.name == ":authority";
            CRPC_CHECK(known);
        } else {
            pseudo_done = true;
        }
    }
    // Connection-specific fields are forbidden in HTTP/2 (RFC 9113 8.2.2).
    CRPC_CHECK_FALSE(has_field(request.fields, "connection"));
    CRPC_CHECK_FALSE(has_field(request.fields, "keep-alive"));
    CRPC_CHECK_FALSE(has_field(request.fields, "proxy-connection"));
    CRPC_CHECK_FALSE(has_field(request.fields, "transfer-encoding"));
    CRPC_CHECK_FALSE(has_field(request.fields, "upgrade"));
    CRPC_CHECK_FALSE(has_field(request.fields, "host"));
    CRPC_CHECK_FALSE(has_field(request.fields, "te"));
    // Names are lower-case on the h2 wire.
    for (const http::header& item : request.fields) {
        std::string lower(item.name);
        for (char& c : lower) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        CRPC_CHECK_EQ(lower, item.name);
    }
    if (body_size != 0) {
        CRPC_CHECK_EQ(std::to_string(body_size), field(request.fields, "content-length"));
    }
    for (const http::header& item : expect_regular) {
        CRPC_CHECK_EQ(item.value, field(request.fields, item.name.c_str()));
    }
}

// --- Scripted-endpoint tests ---------------------------------------------------

CRPC_TEST(h2_client_round_trip_answers_settings_and_ping) {
    inproc_backend backend;
    const std::string name = unique_name("h2-roundtrip");
    scripted_h2_server server(backend, name);

    http::h2_client_options options;
    options.host = "scripted.test";
    auto made = http::make_h2_client(options, backend, endpoint::inproc(name));
    CRPC_CHECK(made.ok());
    if (!made) {
        return;
    }
    CRPC_CHECK(server.adopt());
    std::unique_ptr<http::h2_client>& client = made.value();

    // The client speaks h2 without any fallback attempt: the preface opens the
    // connection. begin() returns once the request is on the wire.
    const auto opened = client->begin(http::method::get, "/root", {});
    CRPC_CHECK(opened.ok());
    if (!opened) {
        return;
    }
    CRPC_CHECK_EQ(1u, opened.value() & 1u); // client streams are odd

    captured_frame client_settings;
    CRPC_CHECK(server.wait_client_settings(client_settings));

    // Server SETTINGS must be answered by an ACK once the client pumps.
    CRPC_CHECK(server.send_settings({}));
    h2::ping_opaque opaque{};
    for (std::size_t i = 0; i < opaque.size(); ++i) {
        opaque[i] = static_cast<std::byte>(0x5A);
    }
    std::string ping_payload(8, '\0');
    std::size_t ping_written = 0;
    CRPC_CHECK(h2::encode_ping(
                   span<std::byte>(reinterpret_cast<std::byte*>(ping_payload.data()), 8),
                   opaque, ping_written)
                   .ok());
    ping_payload.resize(ping_written);
    h2::frame_header ping_header;
    ping_header.type = h2::frame_type::ping;
    ping_header.flags = 0;
    ping_header.stream_id = 0;
    ping_header.length = static_cast<u32>(ping_payload.size());
    CRPC_CHECK(server.send_raw(frame_bytes(ping_header, as_bytes(ping_payload))));

    const http::header_list no_regular;
    check_request_head(server, opened.value(), "GET", "/root", no_regular, 0);

    CRPC_CHECK(server.send_headers(opened.value(),
                                   {{":status", "200"}, {"content-length", "5"}}, false));
    CRPC_CHECK(server.send_data(opened.value(), "hello", true));

    const auto reply = client->finish(opened.value());
    CRPC_CHECK(reply.ok());
    if (reply) {
        CRPC_CHECK_EQ(200, reply.value().status_code);
        CRPC_CHECK_EQ(std::string("hello"), reply.value().body);
        CRPC_CHECK(static_cast<int>(reply.value().version) ==
                   static_cast<int>(http::http_version::http_2));
    }

    // The pump answered the server while reading the response.
    captured_frame settings_ack;
    CRPC_CHECK(server.wait_frame(h2::frame_type::settings, 0, settings_ack));
    CRPC_CHECK(settings_ack.header.has_flag(h2::flags::ack));
    captured_frame ping_ack;
    CRPC_CHECK(server.wait_frame(h2::frame_type::ping, 0, ping_ack));
    CRPC_CHECK(ping_ack.header.has_flag(h2::flags::ack));

    client->close();
    server.close();
}

CRPC_TEST(pseudo_headers_precede_regular_and_forbidden_fields_are_filtered) {
    inproc_backend backend;
    const std::string name = unique_name("h2-pseudo");
    scripted_h2_server server(backend, name);

    http::h2_client_options options;
    options.host = "scripted.test";
    auto made = http::make_h2_client(options, backend, endpoint::inproc(name));
    CRPC_CHECK(made.ok());
    if (!made) {
        return;
    }
    CRPC_CHECK(server.adopt());
    std::unique_ptr<http::h2_client>& client = made.value();

    std::vector<http::header> extra;
    // Connection-specific fields are protocol errors in HTTP/2 and must be
    // dropped, not sent. `Host` is superseded by :authority. `TE` may only
    // carry "trailers". The mixed-case name must reach the wire lower-cased.
    extra.push_back({"X-MiXeD-Case", "val"});
    extra.push_back({"Connection", "close"});
    extra.push_back({"Keep-Alive", "timeout=5"});
    extra.push_back({"Proxy-Connection", "keep-alive"});
    extra.push_back({"Transfer-Encoding", "chunked"});
    extra.push_back({"Upgrade", "websocket"});
    extra.push_back({"Host", "somewhere.else"});
    extra.push_back({"TE", "bogus"});

    const auto opened = client->begin(http::method::post, "/echo", as_bytes("hello"),
                                      span<const http::header>(extra.data(), extra.size()));
    CRPC_CHECK(opened.ok());
    if (!opened) {
        return;
    }

    http::header_list expect;
    expect.add("x-mixed-case", "val");
    check_request_head(server, opened.value(), "POST", "/echo", expect, 5);

    // The body follows as DATA with END_STREAM.
    std::string body;
    CRPC_CHECK(server.wait_data(opened.value(), body));
    CRPC_CHECK_EQ(std::string("hello"), body);

    CRPC_CHECK(server.send_headers(opened.value(),
                                   {{":status", "200"}, {"content-length", "5"}}, false));
    CRPC_CHECK(server.send_data(opened.value(), "hello", true));
    const auto reply = client->finish(opened.value());
    CRPC_CHECK(reply.ok());

    client->close();
    server.close();
}

CRPC_TEST(three_requests_in_flight_then_responses_out_of_order) {
    inproc_backend backend;
    const std::string name = unique_name("h2-mux-scripted");
    scripted_h2_server server(backend, name);

    http::h2_client_options options;
    options.host = "scripted.test";
    auto made = http::make_h2_client(options, backend, endpoint::inproc(name));
    CRPC_CHECK(made.ok());
    if (!made) {
        return;
    }
    CRPC_CHECK(server.adopt());
    std::unique_ptr<http::h2_client>& client = made.value();

    // Issue all three requests before reading any response: three streams are
    // open on one connection at the same time.
    const auto first = client->begin(http::method::get, "/one", {});
    const auto second = client->begin(http::method::get, "/two", {});
    const auto third = client->begin(http::method::get, "/three", {});
    CRPC_CHECK(first.ok());
    CRPC_CHECK(second.ok());
    CRPC_CHECK(third.ok());
    if (!first || !second || !third) {
        return;
    }
    CRPC_CHECK_EQ(1u, first.value());
    CRPC_CHECK_EQ(3u, second.value());
    CRPC_CHECK_EQ(5u, third.value());

    http::header_list no_regular;
    check_request_head(server, first.value(), "GET", "/one", no_regular, 0);
    check_request_head(server, second.value(), "GET", "/two", no_regular, 0);
    check_request_head(server, third.value(), "GET", "/three", no_regular, 0);

    // Answer in the opposite order: the client must demux by stream id.
    const auto answer = [&](u32 stream, std::string_view body) {
        std::string length = std::to_string(body.size());
        CRPC_CHECK(server.send_headers(stream, {{":status", "200"},
                                                {"content-length", length}},
                                       false));
        CRPC_CHECK(server.send_data(stream, body, true));
    };
    answer(third.value(), "third");
    answer(second.value(), "second");
    answer(first.value(), "first");

    const auto reply_first = client->finish(first.value());
    CRPC_CHECK(reply_first.ok());
    if (reply_first) {
        CRPC_CHECK_EQ(std::string("first"), reply_first.value().body);
    }
    const auto reply_second = client->finish(second.value());
    CRPC_CHECK(reply_second.ok());
    if (reply_second) {
        CRPC_CHECK_EQ(std::string("second"), reply_second.value().body);
    }
    const auto reply_third = client->finish(third.value());
    CRPC_CHECK(reply_third.ok());
    if (reply_third) {
        CRPC_CHECK_EQ(std::string("third"), reply_third.value().body);
    }

    client->close();
    server.close();
}

CRPC_TEST(max_concurrent_streams_zero_is_refused) {
    inproc_backend backend;
    const std::string name = unique_name("h2-conc0");
    scripted_h2_server server(backend, name);

    http::h2_client_options options;
    options.host = "scripted.test";
    auto made = http::make_h2_client(options, backend, endpoint::inproc(name));
    CRPC_CHECK(made.ok());
    if (!made) {
        return;
    }
    CRPC_CHECK(server.adopt());
    std::unique_ptr<http::h2_client>& client = made.value();

    // The first request is opened optimistically (the peer's SETTINGS may not
    // have arrived yet); the zero limit is then enforced by the server's
    // RST_STREAM(REFUSED_STREAM) and by a local check afterwards.
    const auto opened = client->begin(http::method::get, "/root", {});
    CRPC_CHECK(opened.ok());
    if (!opened) {
        return;
    }
    captured_frame client_settings;
    CRPC_CHECK(server.wait_client_settings(client_settings));
    std::vector<h2::setting> limits;
    limits.push_back({static_cast<u16>(h2::setting_id::max_concurrent_streams), 0});
    CRPC_CHECK(server.send_settings(limits));

    captured_block request;
    CRPC_CHECK(server.wait_headers(opened.value(), request));
    CRPC_CHECK(server.send_rst(opened.value(), h2::error_code::refused_stream));

    const auto refused = client->finish(opened.value());
    CRPC_CHECK_FALSE(refused.ok());
    if (!refused) {
        CRPC_CHECK_EQ(rpc_code::unavailable, refused.error().code);
    }

    // Once the peer's SETTINGS are applied, a new stream is refused locally
    // instead of being written onto the wire.
    const auto local = client->begin(http::method::get, "/root", {});
    CRPC_CHECK_FALSE(local.ok());
    if (!local) {
        CRPC_CHECK_EQ(rpc_code::failed_precondition, local.error().code);
    }

    client->close();
    server.close();
}

CRPC_TEST(empty_body_get_sends_end_stream_on_headers_alone) {
    inproc_backend backend;
    const std::string name = unique_name("h2-empty");
    scripted_h2_server server(backend, name);

    http::h2_client_options options;
    options.host = "scripted.test";
    auto made = http::make_h2_client(options, backend, endpoint::inproc(name));
    CRPC_CHECK(made.ok());
    if (!made) {
        return;
    }
    CRPC_CHECK(server.adopt());
    std::unique_ptr<http::h2_client>& client = made.value();

    const auto opened = client->begin(http::method::get, "/root", {});
    CRPC_CHECK(opened.ok());
    if (!opened) {
        return;
    }
    captured_block request;
    CRPC_CHECK(server.wait_headers(opened.value(), request));
    CRPC_CHECK(request.end_stream);
    CRPC_CHECK_FALSE(server.has_data(opened.value()));

    CRPC_CHECK(server.send_headers(opened.value(),
                                   {{":status", "200"}, {"content-length", "5"}}, false));
    CRPC_CHECK(server.send_data(opened.value(), "hello", true));
    const auto reply = client->finish(opened.value());
    CRPC_CHECK(reply.ok());
    if (reply) {
        CRPC_CHECK_EQ(std::string("hello"), reply.value().body);
    }

    client->close();
    server.close();
}

CRPC_TEST(zero_content_length_response_completes) {
    inproc_backend backend;
    const std::string name = unique_name("h2-cl0");
    scripted_h2_server server(backend, name);

    http::h2_client_options options;
    options.host = "scripted.test";
    auto made = http::make_h2_client(options, backend, endpoint::inproc(name));
    CRPC_CHECK(made.ok());
    if (!made) {
        return;
    }
    CRPC_CHECK(server.adopt());
    std::unique_ptr<http::h2_client>& client = made.value();

    const auto opened = client->begin(http::method::get, "/root", {});
    CRPC_CHECK(opened.ok());
    if (!opened) {
        return;
    }
    captured_block request;
    CRPC_CHECK(server.wait_headers(opened.value(), request));
    CRPC_CHECK(server.send_headers(
        opened.value(), {{":status", "200"}, {"content-length", "0"}}, true));
    const auto reply = client->finish(opened.value());
    CRPC_CHECK(reply.ok());
    if (reply) {
        CRPC_CHECK(reply.value().body.empty());
        CRPC_CHECK_EQ(200, reply.value().status_code);
    }

    client->close();
    server.close();
}

CRPC_TEST(content_length_mismatch_fails_explicitly) {
    inproc_backend backend;
    const std::string name = unique_name("h2-clmismatch");
    scripted_h2_server server(backend, name);

    http::h2_client_options options;
    options.host = "scripted.test";
    auto made = http::make_h2_client(options, backend, endpoint::inproc(name));
    CRPC_CHECK(made.ok());
    if (!made) {
        return;
    }
    CRPC_CHECK(server.adopt());
    std::unique_ptr<http::h2_client>& client = made.value();

    const auto opened = client->begin(http::method::get, "/root", {});
    CRPC_CHECK(opened.ok());
    if (!opened) {
        return;
    }
    captured_block request;
    CRPC_CHECK(server.wait_headers(opened.value(), request));
    // Ten bytes declared, five delivered: the client must report the
    // discrepancy instead of silently truncating.
    CRPC_CHECK(server.send_headers(opened.value(),
                                   {{":status", "200"}, {"content-length", "10"}}, false));
    CRPC_CHECK(server.send_data(opened.value(), "12345", true));
    const auto reply = client->finish(opened.value());
    CRPC_CHECK_FALSE(reply.ok());
    if (!reply) {
        CRPC_CHECK_EQ(rpc_code::protocol_error, reply.error().code);
        CRPC_CHECK(reply.error().message.find("content-length") != std::string::npos ||
                   reply.error().message.find("Content-Length") != std::string::npos);
    }

    client->close();
    server.close();
}

CRPC_TEST(trailers_are_collected) {
    inproc_backend backend;
    const std::string name = unique_name("h2-trailers");
    scripted_h2_server server(backend, name);

    http::h2_client_options options;
    options.host = "scripted.test";
    auto made = http::make_h2_client(options, backend, endpoint::inproc(name));
    CRPC_CHECK(made.ok());
    if (!made) {
        return;
    }
    CRPC_CHECK(server.adopt());
    std::unique_ptr<http::h2_client>& client = made.value();

    const auto opened = client->begin(http::method::get, "/root", {});
    CRPC_CHECK(opened.ok());
    if (!opened) {
        return;
    }
    captured_block request;
    CRPC_CHECK(server.wait_headers(opened.value(), request));
    CRPC_CHECK(server.send_headers(opened.value(),
                                   {{":status", "200"}, {"content-length", "3"}}, false));
    CRPC_CHECK(server.send_data(opened.value(), "abc", false));
    CRPC_CHECK(server.send_trailers(opened.value(), {{"x-checksum", "abc123"}}));
    const auto reply = client->finish(opened.value());
    CRPC_CHECK(reply.ok());
    if (reply) {
        CRPC_CHECK_EQ(std::string("abc"), reply.value().body);
        CRPC_CHECK_EQ(std::string("abc123"), field(reply.value().trailers, "x-checksum"));
    }

    client->close();
    server.close();
}

CRPC_TEST(rst_stream_fails_the_pending_request) {
    inproc_backend backend;
    const std::string name = unique_name("h2-rst");
    scripted_h2_server server(backend, name);

    http::h2_client_options options;
    options.host = "scripted.test";
    auto made = http::make_h2_client(options, backend, endpoint::inproc(name));
    CRPC_CHECK(made.ok());
    if (!made) {
        return;
    }
    CRPC_CHECK(server.adopt());
    std::unique_ptr<http::h2_client>& client = made.value();

    const auto opened = client->begin(http::method::get, "/root", {});
    CRPC_CHECK(opened.ok());
    if (!opened) {
        return;
    }
    captured_block request;
    CRPC_CHECK(server.wait_headers(opened.value(), request));
    CRPC_CHECK(server.send_headers(opened.value(), {{":status", "200"}}, false));
    CRPC_CHECK(server.send_rst(opened.value(), h2::error_code::cancel));
    const auto reply = client->finish(opened.value());
    CRPC_CHECK_FALSE(reply.ok());
    if (!reply) {
        CRPC_CHECK_EQ(rpc_code::canceled, reply.error().code);
    }

    client->close();
    server.close();
}

CRPC_TEST(goaway_blocks_new_streams) {
    inproc_backend backend;
    const std::string name = unique_name("h2-goaway-new");
    scripted_h2_server server(backend, name);

    http::h2_client_options options;
    options.host = "scripted.test";
    auto made = http::make_h2_client(options, backend, endpoint::inproc(name));
    CRPC_CHECK(made.ok());
    if (!made) {
        return;
    }
    CRPC_CHECK(server.adopt());
    std::unique_ptr<http::h2_client>& client = made.value();

    const auto opened = client->begin(http::method::get, "/root", {});
    CRPC_CHECK(opened.ok());
    if (!opened) {
        return;
    }
    captured_block request;
    CRPC_CHECK(server.wait_headers(opened.value(), request));
    CRPC_CHECK(server.send_headers(opened.value(),
                                   {{":status", "200"}, {"content-length", "2"}}, false));
    CRPC_CHECK(server.send_data(opened.value(), "ok", true));
    const auto reply = client->finish(opened.value());
    CRPC_CHECK(reply.ok());

    // The server is going away. A client that has not read the GOAWAY yet may
    // still open one stream optimistically (RFC 7540 6.8 leaves the race to
    // the stream id); that stream is refused, and once the GOAWAY has been
    // pumped, every further begin() fails locally.
    CRPC_CHECK(server.send_goaway(opened.value(), h2::error_code::no_error));
    const auto optimistic = client->begin(http::method::get, "/root", {});
    CRPC_CHECK(optimistic.ok());
    if (optimistic) {
        CRPC_CHECK(server.send_rst(optimistic.value(), h2::error_code::refused_stream));
        const auto refused = client->finish(optimistic.value());
        CRPC_CHECK_FALSE(refused.ok());
        if (!refused) {
            CRPC_CHECK_EQ(rpc_code::unavailable, refused.error().code);
        }
    }
    const auto next = client->begin(http::method::get, "/root", {});
    CRPC_CHECK_FALSE(next.ok());
    if (!next) {
        CRPC_CHECK_EQ(rpc_code::unavailable, next.error().code);
    }

    client->close();
    server.close();
}

CRPC_TEST(goaway_fails_an_in_flight_stream) {
    inproc_backend backend;
    const std::string name = unique_name("h2-goaway-inflight");
    scripted_h2_server server(backend, name);

    http::h2_client_options options;
    options.host = "scripted.test";
    auto made = http::make_h2_client(options, backend, endpoint::inproc(name));
    CRPC_CHECK(made.ok());
    if (!made) {
        return;
    }
    CRPC_CHECK(server.adopt());
    std::unique_ptr<http::h2_client>& client = made.value();

    const auto first = client->begin(http::method::get, "/one", {});
    const auto second = client->begin(http::method::get, "/two", {});
    CRPC_CHECK(first.ok());
    CRPC_CHECK(second.ok());
    if (!first || !second) {
        return;
    }

    // Answer stream 1, then go away before stream 3 is answered.
    captured_block request;
    CRPC_CHECK(server.wait_headers(first.value(), request));
    CRPC_CHECK(server.send_headers(first.value(),
                                   {{":status", "200"}, {"content-length", "2"}}, false));
    CRPC_CHECK(server.send_data(first.value(), "ok", true));
    CRPC_CHECK(server.send_goaway(first.value(), h2::error_code::no_error));

    const auto reply_first = client->finish(first.value());
    CRPC_CHECK(reply_first.ok());
    const auto reply_second = client->finish(second.value());
    CRPC_CHECK_FALSE(reply_second.ok());
    if (!reply_second) {
        // The request may have run: the shutdown is announced, not an error.
        CRPC_CHECK_EQ(rpc_code::unavailable, reply_second.error().code);
    }

    client->close();
    server.close();
}

CRPC_TEST(response_header_list_over_limit_fails) {
    inproc_backend backend;
    const std::string name = unique_name("h2-hdrlimit");
    scripted_h2_server server(backend, name);

    http::h2_client_options options;
    options.host = "scripted.test";
    options.max_header_bytes = 100;
    auto made = http::make_h2_client(options, backend, endpoint::inproc(name));
    CRPC_CHECK(made.ok());
    if (!made) {
        return;
    }
    CRPC_CHECK(server.adopt());
    std::unique_ptr<http::h2_client>& client = made.value();

    const auto opened = client->begin(http::method::get, "/root", {});
    CRPC_CHECK(opened.ok());
    if (!opened) {
        return;
    }
    captured_block request;
    CRPC_CHECK(server.wait_headers(opened.value(), request));
    // The decoded list (~status + content-type + a large field) exceeds the
    // 100-byte limit this client advertised.
    CRPC_CHECK(server.send_headers(opened.value(),
                                   {{":status", "200"},
                                    {"content-type", "text/plain"},
                                    {"x-big", std::string(300, 'x')}},
                                   true));
    const auto reply = client->finish(opened.value());
    CRPC_CHECK_FALSE(reply.ok());
    if (!reply) {
        CRPC_CHECK_EQ(rpc_code::resource_exhausted, reply.error().code);
    }

    client->close();
    server.close();
}

CRPC_TEST(large_body_respects_flow_control) {
    inproc_backend backend;
    const std::string name = unique_name("h2-flow");
    scripted_h2_server server(backend, name);

    http::h2_client_options options;
    options.host = "scripted.test";
    auto made = http::make_h2_client(options, backend, endpoint::inproc(name));
    CRPC_CHECK(made.ok());
    if (!made) {
        return;
    }
    CRPC_CHECK(server.adopt());
    std::unique_ptr<http::h2_client>& client = made.value();

    // 200 KiB against the default 64 KiB initial windows: the client must
    // park the remainder and resume as WINDOW_UPDATEs arrive, and the server
    // must receive every byte exactly once.
    const std::string body(200u * 1024u, 'b');

    std::thread server_side([&] {
        const std::size_t total = body.size();
        // Dribble credits (connection and stream level) until every byte has
        // arrived; the client may only resume what the windows allow.
        std::size_t seen = 0;
        while (seen < total) {
            const u32 credit = 32u * 1024u;
            if (!server.send_window_update(0, credit) ||
                !server.send_window_update(1, credit)) {
                break;
            }
            if (!crpc_test::wait_for([&] { return server.data_of(1).size() > seen; },
                                     5000)) {
                break;
            }
            seen = server.data_of(1).size();
        }
        // Every byte must have arrived, exactly once, before the answer.
        CRPC_CHECK_EQ(total, server.data_of(1).size());
        CRPC_CHECK_EQ(body, server.data_of(1));
        CRPC_CHECK(server.send_headers(1, {{":status", "200"}, {"content-length", "2"}},
                                       false));
        CRPC_CHECK(server.send_data(1, "ok", true));
    });

    const auto opened = client->begin(http::method::post, "/upload", as_bytes(body));
    CRPC_CHECK(opened.ok());
    if (opened) {
        CRPC_CHECK_EQ(1u, opened.value());
    }
    server_side.join();

    if (opened) {
        const auto reply = client->finish(opened.value());
        CRPC_CHECK(reply.ok());
        if (reply) {
            CRPC_CHECK_EQ(std::string("ok"), reply.value().body);
        }
    }

    client->close();
    server.close();
}

// --- End-to-end against the library's own server ------------------------------

CRPC_TEST(h2c_prior_knowledge_completes_with_library_server) {
    const std::string name = unique_name("h2c-e2e");
    http_fixture fx(name);

    http::client_options options;
    options.host = "test.local";
    options.h2c = true;
    std::unique_ptr<http::client> client =
        http::make_client(options, fx.backend, endpoint::inproc(name));
    CRPC_CHECK(client != nullptr);
    if (!client) {
        return;
    }

    const auto reply = client->send(http::method::get, "/root", {});
    CRPC_CHECK(reply.ok());
    if (reply) {
        // The negotiation must be visible: this is HTTP/2, not a downgraded
        // HTTP/1.1 answer.
        CRPC_CHECK(static_cast<int>(reply.value().version) ==
                   static_cast<int>(http::http_version::http_2));
        CRPC_CHECK_EQ(200, reply.value().status_code);
        CRPC_CHECK_EQ(std::string("hello"), reply.value().body);
    }
    CRPC_CHECK_EQ(std::size_t{1}, fx.server->stats().h2_connections);
    CRPC_CHECK_EQ(std::size_t{1}, fx.server->stats().requests_handled);

    client->close();
}

CRPC_TEST(cleartext_without_h2c_stays_http_1_1) {
    const std::string name = unique_name("h1-default");
    http_fixture fx(name);

    http::client_options options;
    options.host = "test.local";
    std::unique_ptr<http::client> client =
        http::make_client(options, fx.backend, endpoint::inproc(name));
    CRPC_CHECK(client != nullptr);
    if (!client) {
        return;
    }

    const auto reply = client->send(http::method::get, "/root", {});
    CRPC_CHECK(reply.ok());
    if (reply) {
        CRPC_CHECK(static_cast<int>(reply.value().version) ==
                   static_cast<int>(http::http_version::http_1_1));
        CRPC_CHECK_EQ(200, reply.value().status_code);
        CRPC_CHECK_EQ(std::string("hello"), reply.value().body);
    }

    client->close();
}

CRPC_TEST(h2_client_multiplexes_against_library_server) {
    const std::string name = unique_name("h2-mux-e2e");
    http_fixture fx(name);

    http::h2_client_options options;
    options.host = "test.local";
    auto made = http::make_h2_client(options, fx.backend, endpoint::inproc(name));
    CRPC_CHECK(made.ok());
    if (!made) {
        return;
    }
    std::unique_ptr<http::h2_client>& client = made.value();

    // Three requests on one connection, issued before any response is read.
    const auto one = client->begin(http::method::get, "/root", {});
    const auto two = client->begin(http::method::get, "/root", {});
    const auto three = client->begin(http::method::post, "/echo", as_bytes("three"));
    CRPC_CHECK(one.ok());
    CRPC_CHECK(two.ok());
    CRPC_CHECK(three.ok());
    if (!one || !two || !three) {
        return;
    }

    const auto reply_one = client->finish(one.value());
    const auto reply_two = client->finish(two.value());
    const auto reply_three = client->finish(three.value());
    CRPC_CHECK(reply_one.ok());
    CRPC_CHECK(reply_two.ok());
    if (!reply_three.ok()) {
        std::fprintf(stderr, "  third failed: %s\n", reply_three.error().message.c_str());
    }
    CRPC_CHECK(reply_three.ok());
    if (reply_one && reply_two && reply_three) {
        CRPC_CHECK_EQ(std::string("hello"), reply_one.value().body);
        CRPC_CHECK_EQ(std::string("hello"), reply_two.value().body);
        CRPC_CHECK_EQ(std::string("three"), reply_three.value().body);
        CRPC_CHECK(static_cast<int>(reply_three.value().version) ==
                   static_cast<int>(http::http_version::http_2));
    }

    // One connection carried all three requests.
    CRPC_CHECK_EQ(std::size_t{1}, fx.server->stats().h2_connections);
    CRPC_CHECK_EQ(std::size_t{3}, fx.server->stats().requests_handled);

    client->close();
}

CRPC_TEST(h2_client_send_and_send_text_round_trip) {
    const std::string name = unique_name("h2-send-e2e");
    http_fixture fx(name);

    http::h2_client_options options;
    options.host = "test.local";
    auto made = http::make_h2_client(options, fx.backend, endpoint::inproc(name));
    CRPC_CHECK(made.ok());
    if (!made) {
        return;
    }
    std::unique_ptr<http::h2_client>& client = made.value();

    const auto reply = client->send_text(http::method::post, "/echo", "payload",
                                         "text/plain");
    CRPC_CHECK(reply.ok());
    if (reply) {
        CRPC_CHECK_EQ(200, reply.value().status_code);
        CRPC_CHECK_EQ(std::string("payload"), reply.value().body);
    }

    client->close();
}

// --- TLS + ALPN ---------------------------------------------------------------

// Only the OpenSSL-gated fixtures below call this.
#if defined(CRPC_HAVE_OPENSSL)
[[nodiscard]] crpc::u16 next_port() {
#if defined(_WIN32)
    static const unsigned base = 26200u + (::GetCurrentProcessId() % 20000u);
#else
    static const unsigned base = 26200u + (static_cast<unsigned>(::getpid()) % 20000u);
#endif
    static std::atomic<unsigned> counter{0};
    const unsigned value = base + counter.fetch_add(7, std::memory_order_relaxed);
    return static_cast<crpc::u16>(1024u + value % (65536u - 1024u));
}
#endif

#if defined(CRPC_HAVE_OPENSSL)

struct cert_files {
    std::string certificate;
    std::string private_key;

    cert_files() {
        const std::filesystem::path dir = std::filesystem::temp_directory_path();
#if defined(_WIN32)
        const unsigned pid = ::GetCurrentProcessId();
#else
        const unsigned pid = static_cast<unsigned>(::getpid());
#endif
        certificate =
            (dir / ("crpc_http_h2_certificate_" + std::to_string(pid) + ".pem")).string();
        private_key =
            (dir / ("crpc_http_h2_private_key_" + std::to_string(pid) + ".pem")).string();
        write(certificate, crpc_test_tls::k_certificate_pem);
        write(private_key, crpc_test_tls::k_private_key_pem);
    }

private:
    static void write(const std::string& path, const char* data) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(data, static_cast<std::streamsize>(std::strlen(data)));
    }
};

const cert_files& test_certificates() {
    static const cert_files files;
    return files;
}

// A TLS h2/h1.1 server with the shared routes.
struct tls_fixture {
    tcp_backend server_backend;
    tcp_backend client_backend;
    mvc::router router;
    std::unique_ptr<http::http_server> server;
    crpc::u16 port = 0;

    tls_fixture() {
        add_route(router, http::method::get, "/secure", &handle_root);
        add_route(router, http::method::post, "/echo", &handle_echo);
        const status frozen = router.freeze();
        CRPC_CHECK(frozen.ok());

        port = next_port();
        http::http_server::builder builder;
        builder.router(router);
        server = builder.build();
        CRPC_CHECK(server != nullptr);
        if (!server) {
            return;
        }
        io::tls_config config;
        config.certificate_chain = test_certificates().certificate;
        config.private_key = test_certificates().private_key;
        const status registered =
            server->listen_tls(endpoint::tcp("127.0.0.1", port), server_backend, config);
        CRPC_CHECK(registered.ok());
        if (registered.ok()) {
            const status started = server->start();
            CRPC_CHECK(started.ok());
        }
    }

    ~tls_fixture() {
        if (server) {
            server->stop();
        }
    }

    tls_fixture(const tls_fixture&) = delete;
    tls_fixture& operator=(const tls_fixture&) = delete;
};

CRPC_TEST(tls_alpn_negotiates_h2_with_library_server) {
    tls_fixture fx;

    // The default ALPN list offers h2 first; the library's server prefers it
    // too, so the conversation must be HTTP/2 end to end.
    http::client_options options = http::client_options::https("localhost");
    options.port = fx.port;
    options.tls_ca_bundle = test_certificates().certificate;
    std::unique_ptr<http::client> client = http::make_client(
        options, fx.client_backend, endpoint::tcp("127.0.0.1", fx.port));
    CRPC_CHECK(client != nullptr);
    if (!client) {
        return;
    }
    const auto reply = client->send(http::method::get, "/secure", {});
    CRPC_CHECK(reply.ok());
    if (reply) {
        CRPC_CHECK(static_cast<int>(reply.value().version) ==
                   static_cast<int>(http::http_version::http_2));
        CRPC_CHECK_EQ(200, reply.value().status_code);
        CRPC_CHECK_EQ(std::string("hello"), reply.value().body);
    }
    CRPC_CHECK_EQ(std::size_t{1}, fx.server->stats().h2_connections);
    CRPC_CHECK_EQ(std::size_t{1}, fx.server->stats().tls_connections);
    client->close();
}

CRPC_TEST(tls_alpn_falls_back_to_http_1_1_when_client_offers_h1_only) {
    tls_fixture fx;

    // A client that offers only http/1.1 must complete over HTTP/1.1: the
    // negotiation picked the only protocol both sides speak.
    http::client_options options = http::client_options::https("localhost");
    options.port = fx.port;
    options.tls_ca_bundle = test_certificates().certificate;
    options.tls_alpn = {"http/1.1"};
    std::unique_ptr<http::client> client = http::make_client(
        options, fx.client_backend, endpoint::tcp("127.0.0.1", fx.port));
    CRPC_CHECK(client != nullptr);
    if (!client) {
        return;
    }
    const auto reply = client->send(http::method::get, "/secure", {});
    CRPC_CHECK(reply.ok());
    if (reply) {
        CRPC_CHECK(static_cast<int>(reply.value().version) ==
                   static_cast<int>(http::http_version::http_1_1));
        CRPC_CHECK_EQ(200, reply.value().status_code);
        CRPC_CHECK_EQ(std::string("hello"), reply.value().body);
    }
    client->close();
}

CRPC_TEST(make_h2_client_over_tls_negotiates_h2) {
    tls_fixture fx;

    http::h2_client_options options;
    options.host = "localhost";
    options.port = fx.port;
    options.tls = true;
    options.tls_ca_bundle = test_certificates().certificate;
    auto made =
        http::make_h2_client(options, fx.client_backend, endpoint::tcp("127.0.0.1", fx.port));
    CRPC_CHECK(made.ok());
    if (!made) {
        return;
    }
    std::unique_ptr<http::h2_client>& client = made.value();
    const auto reply = client->send(http::method::get, "/secure", {});
    CRPC_CHECK(reply.ok());
    if (reply) {
        CRPC_CHECK(static_cast<int>(reply.value().version) ==
                   static_cast<int>(http::http_version::http_2));
        CRPC_CHECK_EQ(std::string("hello"), reply.value().body);
    }
    CRPC_CHECK_EQ(std::size_t{1}, fx.server->stats().h2_connections);
    client->close();
}

CRPC_TEST(make_h2_client_falls_back_when_h1_negotiated) {
    tls_fixture fx;

    // The multiplexer is h2-only: when the caller pins the ALPN list to
    // http/1.1 the h2 factory must not pretend to speak h2. It reports the
    // outcome instead of silently downgrading, and the plain client still
    // completes the same request over HTTP/1.1.
    http::h2_client_options h2_options;
    h2_options.host = "localhost";
    h2_options.port = fx.port;
    h2_options.tls = true;
    h2_options.tls_ca_bundle = test_certificates().certificate;
    h2_options.tls_alpn = {"http/1.1"};
    auto made =
        http::make_h2_client(h2_options, fx.client_backend, endpoint::tcp("127.0.0.1", fx.port));
    CRPC_CHECK_FALSE(made.ok());
    if (!made) {
        CRPC_CHECK_EQ(rpc_code::failed_precondition, made.error().code);
    }

    http::client_options h1_options = http::client_options::https("localhost");
    h1_options.port = fx.port;
    h1_options.tls_ca_bundle = test_certificates().certificate;
    h1_options.tls_alpn = {"http/1.1"};
    std::unique_ptr<http::client> client = http::make_client(
        h1_options, fx.client_backend, endpoint::tcp("127.0.0.1", fx.port));
    CRPC_CHECK(client != nullptr);
    if (client) {
        const auto reply = client->send(http::method::get, "/secure", {});
        CRPC_CHECK(reply.ok());
        if (reply) {
            CRPC_CHECK(static_cast<int>(reply.value().version) ==
                       static_cast<int>(http::http_version::http_1_1));
        }
        client->close();
    }
}

#else // !CRPC_HAVE_OPENSSL

CRPC_TEST(make_h2_client_over_tls_is_refused_without_openssl) {
    tcp_backend backend;
    http::h2_client_options options;
    options.host = "localhost";
    options.tls = true;
    auto made = http::make_h2_client(options, backend, endpoint::tcp("127.0.0.1", 1));
    CRPC_CHECK_FALSE(made.ok());
    if (!made) {
        CRPC_CHECK_EQ(rpc_code::failed_precondition, made.error().code);
        CRPC_CHECK(made.error().message.find("CRPC_WITH_OPENSSL") != std::string::npos);
    }
}

#endif // CRPC_HAVE_OPENSSL

} // namespace

CRPC_TEST_MAIN()
