// CRPC -- HTTPS client tests.
//
// The cases run against real loopback sockets and real OpenSSL handshakes:
//   * a full request/response against the library's own TLS-enabled
//     `http::http_server`;
//   * a minimal TLS probe server that records the received SNI and the ALPN
//     protocol, which proves the client sends both;
//   * an untrusted self-signed certificate with verification enabled (must
//     fail with a clear error) and with verification explicitly disabled (must
//     connect);
//   * a plaintext HTTP server while the client asks for HTTPS (must fail, never
//     silently downgrade);
//   * the build without `CRPC_HAVE_OPENSSL`, which must refuse HTTPS with a
//     `failed_precondition` that names the build option.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/db/stream_channel.hpp>
#include <crpc/http/client.hpp>
#include <crpc/http/server.hpp>
#include <crpc/io.hpp>
#include <crpc/mvc/router.hpp>

#if defined(_WIN32)
#include <crpc/io/iocp_backend.hpp>
#include <windows.h>
#else
#include <crpc/io/epoll_backend.hpp>
#include <unistd.h>
#endif

#include "test_framework.hpp"
#include "tls_test_certificate.hpp"

using namespace crpc;

namespace {

#if defined(_WIN32)
using tcp_backend = crpc::iocp_backend;
#else
using tcp_backend = crpc::io::epoll_backend;
#endif

[[nodiscard]] span<const std::byte> as_bytes(std::string_view text) noexcept {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                 text.size());
}

crpc::u16 next_port() {
#if defined(_WIN32)
    static const unsigned base = 26000u + (::GetCurrentProcessId() % 20000u);
#else
    static const unsigned base =
        26000u + (static_cast<unsigned>(::getpid()) % 20000u);
#endif
    static std::atomic<unsigned> counter{0};
    const unsigned value = base + counter.fetch_add(7, std::memory_order_relaxed);
    return static_cast<crpc::u16>(value % 60000u + 1024u);
}

// --- Certificate fixture -----------------------------------------------------

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
        certificate = (dir / ("crpc_http_tls_certificate_" + std::to_string(pid) + ".pem"))
                          .string();
        private_key = (dir / ("crpc_http_tls_private_key_" + std::to_string(pid) + ".pem"))
                          .string();
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

// --- Server-side plumbing ----------------------------------------------------

// Hands the first accepted connection to the server thread.
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

    [[nodiscard]] std::unique_ptr<connection> wait_for(
        std::chrono::milliseconds timeout) {
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

// A one-shot completion whose state outlives the caller: the completion
// deletes itself after reporting, so a timeout never leaves a dangling object
// for a late callback.
struct write_state {
    std::mutex mu;
    std::condition_variable cv;
    io::io_result result{};
    bool done = false;
};

class self_deleting_completion final : public io::completion {
public:
    explicit self_deleting_completion(std::shared_ptr<write_state> state) noexcept
        : state_(std::move(state)) {}

    void complete(io::io_result result) noexcept override {
        {
            std::lock_guard<std::mutex> lk(state_->mu);
            state_->result = result;
            state_->done = true;
        }
        state_->cv.notify_all();
        delete this;
    }

private:
    std::shared_ptr<write_state> state_;
};

[[nodiscard]] bool wait_for_write(const std::shared_ptr<write_state>& state,
                                  std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(state->mu);
    return state->cv.wait_for(lk, timeout, [&] { return state->done; });
}

// A TLS server built from the same blocking channel the HTTPS client uses.
// It answers one HTTP/1.1 request and records what TLS actually negotiated.
class tls_probe_server {
public:
    explicit tls_probe_server(crpc::u16 port) {
        const cert_files& files = test_certificates();
        io::tls_config config;
        config.role = io::tls_role::server;
        config.verify = io::tls_verify_mode::none;
        config.certificate_chain = files.certificate;
        config.private_key = files.private_key;
        config.alpn_protocols = {"http/1.1"};
        config.require_alpn = false;
        std::error_code ec;
        context_ = io::tls_context::create(config, ec);
        CRPC_CHECK(!ec);
        CRPC_CHECK(context_ != nullptr);
        if (!context_) {
            return;
        }
        listener_ = backend_.listen(endpoint::tcp("127.0.0.1", port),
                                    listen_options{});
        CRPC_CHECK(listener_ != nullptr);
        if (!listener_) {
            return;
        }
        listener_->start(&accepts_);
        worker_ = std::thread([this] { serve(); });
    }

    ~tls_probe_server() {
        if (listener_) {
            listener_->stop();
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    tls_probe_server(const tls_probe_server&) = delete;
    tls_probe_server& operator=(const tls_probe_server&) = delete;

    [[nodiscard]] bool finished() const noexcept {
        return finished_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::string server_name() {
        std::lock_guard<std::mutex> lk(mu_);
        return server_name_;
    }
    [[nodiscard]] std::string alpn() {
        std::lock_guard<std::mutex> lk(mu_);
        return alpn_;
    }
    [[nodiscard]] std::string failure() {
        std::lock_guard<std::mutex> lk(mu_);
        return failure_;
    }

private:
    void set_failure(std::string text) {
        std::lock_guard<std::mutex> lk(mu_);
        if (failure_.empty()) {
            failure_ = std::move(text);
        }
    }

    void serve() {
        std::unique_ptr<connection> accepted =
            accepts_.wait_for(std::chrono::milliseconds(5000));
        if (!accepted) {
            set_failure("no connection was accepted");
            finished_.store(true, std::memory_order_release);
            return;
        }
        try {
            db::stream_channel channel(std::move(accepted), *context_, std::string(),
                                       std::chrono::milliseconds(5000));
            std::string request;
            while (request.find("\r\n\r\n") == std::string::npos &&
                   request.size() < 64u * 1024u) {
                std::byte buffer[4096];
                result<std::size_t> got =
                    channel.read_some(span<std::byte>(buffer, sizeof buffer));
                if (!got) {
                    set_failure(got.error().message);
                    break;
                }
                if (got.value() == 0) {
                    continue;
                }
                request.append(reinterpret_cast<const char*>(buffer), got.value());
            }
            {
                std::lock_guard<std::mutex> lk(mu_);
                server_name_ = channel.tls_server_name();
                alpn_ = channel.tls_alpn_protocol();
            }
            if (request.find("\r\n\r\n") != std::string::npos) {
                const std::string response =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: 9\r\n"
                    "Connection: close\r\n\r\n"
                    "https-ok!";
                const status written = channel.write(as_bytes(response));
                if (!written) {
                    set_failure(written.message());
                }
                // Keep the connection open until the client has consumed the
                // response and closed its side. Closing first would make the
                // test race the transport's send queue.
                std::byte discard[512];
                for (;;) {
                    result<std::size_t> got = channel.read_some(
                        span<std::byte>(discard, sizeof discard));
                    if (!got || got.value() == 0) {
                        break;
                    }
                }
            } else {
                set_failure("no complete request was received");
            }
        } catch (...) {
            set_failure("the TLS probe server caught an exception");
        }
        finished_.store(true, std::memory_order_release);
    }

    tcp_backend backend_;
    std::unique_ptr<listener> listener_;
    std::unique_ptr<io::tls_context> context_;
    accept_queue accepts_;
    std::thread worker_;
    std::atomic<bool> finished_{false};
    std::mutex mu_;
    std::string server_name_;
    std::string alpn_;
    std::string failure_;
};

// A cleartext HTTP server that answers immediately, so a TLS client sees a
// non-TLS byte stream and fails the handshake instead of waiting for a timeout.
class plaintext_probe_server {
public:
    explicit plaintext_probe_server(crpc::u16 port) {
        listener_ = backend_.listen(endpoint::tcp("127.0.0.1", port),
                                    listen_options{});
        CRPC_CHECK(listener_ != nullptr);
        if (listener_) {
            listener_->start(&accepts_);
            worker_ = std::thread([this] { serve(); });
        }
    }

    ~plaintext_probe_server() {
        if (listener_) {
            listener_->stop();
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    plaintext_probe_server(const plaintext_probe_server&) = delete;
    plaintext_probe_server& operator=(const plaintext_probe_server&) = delete;

private:
    void serve() {
        std::unique_ptr<connection> accepted =
            accepts_.wait_for(std::chrono::milliseconds(5000));
        if (!accepted) {
            return;
        }
        auto state = std::make_shared<write_state>();
        auto* completion = new self_deleting_completion(state);
        const std::string response =
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nhi";
        accepted->sink().push(as_bytes(response), completion);
        (void)wait_for_write(state, std::chrono::milliseconds(5000));
        accepted->close();
    }

    tcp_backend backend_;
    std::unique_ptr<listener> listener_;
    accept_queue accepts_;
    std::thread worker_;
};

// --- Library HTTP server route ----------------------------------------------

status handle_secure(void*, const http::request&, const mvc::route_params&,
                     const codec&, const codec_registry&, http::response& out) {
    out.status_code = 200;
    out.reason = "OK";
    out.headers.add("Content-Type", "text/plain");
    out.body = "secure-ok";
    return status{};
}

void add_secure_route(mvc::router& router) {
    mvc::route_entry entry;
    entry.method = http::method::get;
    entry.path = "/secure";
    entry.name = "/secure";
    entry.invoke = &handle_secure;
    CRPC_CHECK(router.add(std::move(entry)).ok());
    CRPC_CHECK(router.freeze().ok());
}

// --- Tests -------------------------------------------------------------------

CRPC_TEST(https_convenience_options_are_defaults) {
    const http::client_options options = http::client_options::https("example.com");
    CRPC_CHECK(options.tls);
    CRPC_CHECK_EQ(static_cast<unsigned>(443), static_cast<unsigned>(options.port));
    CRPC_CHECK(options.tls_verify_peer); // verification is on unless explicitly off
    // The default offer negotiates h2 when the server supports it and keeps
    // http/1.1 as the fallback.
    CRPC_CHECK_EQ(std::size_t{2}, options.tls_alpn.size());
    if (options.tls_alpn.size() == 2) {
        CRPC_CHECK_EQ(std::string("h2"), options.tls_alpn[0]);
        CRPC_CHECK_EQ(std::string("http/1.1"), options.tls_alpn[1]);
    }
    CRPC_CHECK_EQ(std::string("example.com"), options.host);
    const http::client_options explicit_port =
        http::client_options::https("example.com", 8443);
    CRPC_CHECK_EQ(static_cast<unsigned>(8443), static_cast<unsigned>(explicit_port.port));
}

#if !defined(CRPC_HAVE_OPENSSL)

CRPC_TEST(https_without_openssl_is_refused) {
    tcp_backend backend;
    http::client_options options = http::client_options::https("localhost");
    std::unique_ptr<http::client> client = http::make_client(
        options, backend, endpoint::tcp("127.0.0.1", 1));
    CRPC_CHECK(client != nullptr);
    if (!client) {
        return;
    }
    CRPC_CHECK_FALSE(client->is_open());
    const result<http::response> reply =
        client->send(http::method::get, "/", {});
    CRPC_CHECK_FALSE(reply.ok());
    if (!reply) {
        std::fprintf(stderr, "  refusal: %s\n", reply.error().message.c_str());
        CRPC_CHECK_EQ(rpc_code::failed_precondition, reply.error().code);
        CRPC_CHECK(reply.error().message.find("CRPC_WITH_OPENSSL") != std::string::npos);
    }
}

#else

CRPC_TEST(https_round_trip_with_library_server) {
    tcp_backend server_backend;
    tcp_backend client_backend;
    mvc::router router;
    add_secure_route(router);

    const crpc::u16 port = next_port();
    http::http_server::builder builder;
    builder.router(router);
    std::unique_ptr<http::http_server> server = builder.build();
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
    if (!registered) {
        return;
    }
    const status started = server->start();
    CRPC_CHECK(started.ok());
    if (!started) {
        return;
    }

    http::client_options options = http::client_options::https("localhost");
    options.port = port;
    options.tls_ca_bundle = test_certificates().certificate;
    std::unique_ptr<http::client> client = http::make_client(
        options, client_backend, endpoint::tcp("127.0.0.1", port));
    CRPC_CHECK(client != nullptr);
    if (client) {
        const result<http::response> reply =
            client->send(http::method::get, "/secure", {});
        if (!reply) {
            std::fprintf(stderr, "  https request failed: %s (%d)\n",
                         reply.error().message.c_str(),
                         static_cast<int>(reply.error().code));
        }
        CRPC_CHECK(reply.ok());
        if (reply) {
            CRPC_CHECK_EQ(200, reply.value().status_code);
            CRPC_CHECK_EQ(std::string("secure-ok"), reply.value().body);
        }
    }
    CRPC_CHECK(server->stats().tls_connections >= 1);
    server->stop();
}

CRPC_TEST(https_sends_sni_and_alpn) {
    const crpc::u16 port = next_port();
    tls_probe_server server(port);

    http::client_options options = http::client_options::https("localhost");
    options.port = port;
    options.tls_ca_bundle = test_certificates().certificate;
    tcp_backend backend;
    std::unique_ptr<http::client> client =
        http::make_client(options, backend, endpoint::tcp("127.0.0.1", port));
    CRPC_CHECK(client != nullptr);
    if (!client) {
        return;
    }
    const result<http::response> reply = client->send(http::method::get, "/probe", {});
    if (!reply) {
        std::fprintf(stderr, "  probe request failed: %s\n",
                     reply.error().message.c_str());
    }
    CRPC_CHECK(reply.ok());
    if (reply) {
        CRPC_CHECK_EQ(200, reply.value().status_code);
        CRPC_CHECK_EQ(std::string("https-ok!"), reply.value().body);
    }
    client->close();
    CRPC_CHECK(crpc_test::wait_for([&] { return server.finished(); }, 5000));
    std::fprintf(stderr, "  server saw SNI=[%s] ALPN=[%s] failure=[%s]\n",
                 server.server_name().c_str(), server.alpn().c_str(),
                 server.failure().c_str());
    CRPC_CHECK_EQ(std::string("localhost"), server.server_name());
    CRPC_CHECK_EQ(std::string("http/1.1"), server.alpn());
}

CRPC_TEST(https_rejects_untrusted_certificate) {
    const crpc::u16 port = next_port();
    tls_probe_server server(port);

    // Verification is on by default and no CA bundle is configured, so the
    // self-signed server certificate must be rejected.
    http::client_options options = http::client_options::https("localhost");
    options.port = port;
    options.timeout_seconds = 10;
    tcp_backend backend;
    std::unique_ptr<http::client> client =
        http::make_client(options, backend, endpoint::tcp("127.0.0.1", port));
    CRPC_CHECK(client != nullptr);
    if (!client) {
        return;
    }
    const result<http::response> reply = client->send(http::method::get, "/", {});
    CRPC_CHECK_FALSE(reply.ok());
    if (!reply) {
        std::fprintf(stderr, "  verification failure: %s (%d)\n",
                     reply.error().message.c_str(),
                     static_cast<int>(reply.error().code));
        CRPC_CHECK_EQ(rpc_code::failed_precondition, reply.error().code);
        CRPC_CHECK(reply.error().message.find("verification") != std::string::npos);
    }
}

CRPC_TEST(https_connects_when_verification_is_explicitly_disabled) {
    const crpc::u16 port = next_port();
    tls_probe_server server(port);

    http::client_options options = http::client_options::https("localhost");
    options.port = port;
    options.tls_verify_peer = false; // the caller opted out explicitly
    tcp_backend backend;
    std::unique_ptr<http::client> client =
        http::make_client(options, backend, endpoint::tcp("127.0.0.1", port));
    CRPC_CHECK(client != nullptr);
    if (!client) {
        return;
    }
    const result<http::response> reply = client->send(http::method::get, "/", {});
    if (!reply) {
        std::fprintf(stderr, "  unverified request failed: %s (%d)\n",
                     reply.error().message.c_str(),
                     static_cast<int>(reply.error().code));
    }
    CRPC_CHECK(reply.ok());
    if (reply) {
        CRPC_CHECK_EQ(200, reply.value().status_code);
        CRPC_CHECK_EQ(std::string("https-ok!"), reply.value().body);
    }
    client->close();
    CRPC_CHECK(crpc_test::wait_for([&] { return server.finished(); }, 5000));
    std::fprintf(stderr, "  unverified server failure=[%s]\n", server.failure().c_str());
    CRPC_CHECK_EQ(std::string("localhost"), server.server_name());
}

CRPC_TEST(https_against_a_plaintext_server_fails) {
    const crpc::u16 port = next_port();
    plaintext_probe_server server(port);

    http::client_options options = http::client_options::https("localhost");
    options.port = port;
    options.tls_verify_peer = false;
    options.timeout_seconds = 10;
    tcp_backend backend;
    std::unique_ptr<http::client> client =
        http::make_client(options, backend, endpoint::tcp("127.0.0.1", port));
    CRPC_CHECK(client != nullptr);
    if (!client) {
        return;
    }
    const result<http::response> reply = client->send(http::method::get, "/", {});
    CRPC_CHECK_FALSE(reply.ok());
    if (!reply) {
        std::fprintf(stderr, "  plaintext failure: %s (%d)\n",
                     reply.error().message.c_str(),
                     static_cast<int>(reply.error().code));
        CRPC_CHECK(reply.error().message.find("TLS") != std::string::npos);
        CRPC_CHECK(reply.error().code == rpc_code::unavailable ||
                   reply.error().code == rpc_code::protocol_error);
    }
}

#endif // CRPC_HAVE_OPENSSL

} // namespace

CRPC_TEST_MAIN()
