// CRPC -- end-to-end WebSocket tests over real loopback transports.
//
// The in-process backend is a real delivery path (both roles run their complete
// wire layers), and a TCP loopback test on 127.0.0.1 exercises the same code
// over actual sockets. Together they cover the full RFC 6455 conversation the
// acceptance criteria demand: handshake, text and binary messages, ping/pong,
// the close handshake, and -- for a large fragmented message -- reassembly on
// the receiving side.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include <crpc/io.hpp>
#include <crpc/ws/client.hpp>
#include <crpc/ws/server.hpp>

#include "test_tcp_backend.hpp"

#if defined(_WIN32)
// GetCurrentProcessId() below is a Win32 call. Relying on a transitive include
// from iocp_backend.hpp happened to work in Debug and failed in Release, where
// the include order differs -- the declaration must be asked for explicitly.
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "test_framework.hpp"
#include <ostream>
#include <type_traits>

// check_equal() streams both sides of a failed comparison; this library's
// enum-classes have no operator<<, so the integer form is provided for ADL.
namespace crpc {
template <typename E,
          typename std::enable_if<std::is_enum<E>::value, int>::type = 0>
std::ostream& operator<<(std::ostream& os, E value) {
    return os << static_cast<long long>(value);
}
} // namespace crpc

using namespace crpc;

namespace {

[[nodiscard]] span<const std::byte> as_bytes(std::string_view text) noexcept {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                 text.size());
}

crpc::u16 next_port() {
#if defined(_WIN32)
    static const unsigned base =
        26000u + (::GetCurrentProcessId() % 20000u);
#else
    static const unsigned base =
        26000u + (static_cast<unsigned>(::getpid()) % 20000u);
#endif
    static std::atomic<unsigned> counter{0};
    const unsigned value = base + counter.fetch_add(7, std::memory_order_relaxed);
    return static_cast<crpc::u16>(value % 60000u + 1024u);
}

// Answers every message with the same opcode until the close handshake ends.
void run_echo_server(crpc::ws::server& srv, std::atomic<int>& accepted) {
    result<std::unique_ptr<ws::stream>> session = srv.accept();
    if (!session) {
        accepted.store(-1);
        return;
    }
    accepted.store(1);
    std::unique_ptr<ws::stream> s = std::move(session.value());
    for (;;) {
        result<ws::event> ev = s->recv();
        if (!ev) {
            break;
        }
        if (ev.value().type == ws::event::kind::closed) {
            // The stream auto-replied with its own Close frame when it decoded
            // the client's; destroying the stream aborts the link, and an
            // abortive close (io.hpp) discards bytes still queued in the
            // backend's send ring. Hold briefly so the reply reaches the
            // kernel before the teardown, like a real server's graceful close.
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            break;
        }
        if (ev.value().type != ws::event::kind::message) {
            continue;
        }
        if (ev.value().op == ws::opcode::text) {
            if (!s->send_text(ev.value().data).ok()) {
                break;
            }
        } else {
            if (!s->send_binary(as_bytes(ev.value().data)).ok()) {
                break;
            }
        }
    }
}

// The full conversation the acceptance criteria name, driven from one role.
void run_full_exchange(std::unique_ptr<ws::client> client) {
    CRPC_CHECK(client->is_open());

    CRPC_CHECK(client->send_text("Hello WebSocket").ok());
    result<ws::event> echo = client->recv();
    CRPC_CHECK(echo.ok());
    CRPC_CHECK_EQ(ws::event::kind::message, echo.value().type);
    CRPC_CHECK_EQ(ws::opcode::text, echo.value().op);
    CRPC_CHECK_EQ("Hello WebSocket", echo.value().data);

    const std::string binary("\x00\x01\x7f\xff payload", 11);
    CRPC_CHECK(client->send_binary(as_bytes(binary)).ok());
    result<ws::event> echo_bin = client->recv();
    CRPC_CHECK(echo_bin.ok());
    CRPC_CHECK_EQ(ws::opcode::binary, echo_bin.value().op);
    CRPC_CHECK_EQ(binary, echo_bin.value().data);

    CRPC_CHECK(client->send_ping(as_bytes("probe")).ok());
    result<ws::event> pong = client->recv();
    CRPC_CHECK(pong.ok());
    CRPC_CHECK_EQ(ws::event::kind::pong, pong.value().type);
    CRPC_CHECK_EQ("probe", pong.value().data);

    CRPC_CHECK(client->send_close().ok());
    result<ws::event> closed = client->recv();
    CRPC_CHECK(closed.ok());
    CRPC_CHECK_EQ(ws::event::kind::closed, closed.value().type);
    CRPC_CHECK_EQ(1000u, closed.value().code);
    // The close handshake completed; both recv() and data sends are finished.
    CRPC_CHECK_FALSE(client->send_text("late").ok());
}

CRPC_TEST(ws_e2e_inproc_full_exchange) {
    inproc_backend backend;
    ws::server_options sopt;
    ws::server srv(backend, endpoint::inproc("ws-e2e-inproc"), sopt);
    CRPC_CHECK(srv.running());

    std::atomic<int> accepted{0};
    std::thread server_thread([&] { run_echo_server(srv, accepted); });

    ws::client_options copt;
    copt.host = "test";
    copt.path = "/echo";
    result<std::unique_ptr<ws::client>> client =
        ws::connect(backend, endpoint::inproc("ws-e2e-inproc"), copt);
    CRPC_CHECK(client.ok());
    run_full_exchange(std::move(client.value()));
    server_thread.join();
    CRPC_CHECK_EQ(1, accepted.load());
}

struct tcp_fixture {
    tcp_backend backend;
    std::unique_ptr<ws::server> srv;
    endpoint ep;

    bool start() {
        for (int attempt = 0; attempt < 40; ++attempt) {
            const endpoint candidate = endpoint::tcp("127.0.0.1", next_port());
            auto server =
                std::make_unique<ws::server>(backend, candidate, ws::server_options{});
            if (!server->running()) {
                continue;
            }
            srv = std::move(server);
            ep = candidate;
            return true;
        }
        return false;
    }
};

CRPC_TEST(ws_e2e_tcp_loopback_full_exchange) {
    tcp_fixture fixture;
    CRPC_CHECK(fixture.start());

    std::atomic<int> accepted{0};
    std::thread server_thread([&] { run_echo_server(*fixture.srv, accepted); });

    ws::client_options copt;
    copt.host = "127.0.0.1";
    copt.port = fixture.ep.port();
    copt.path = "/chat";
    result<std::unique_ptr<ws::client>> client =
        ws::connect(fixture.backend, fixture.ep, copt);
    CRPC_CHECK(client.ok());
    run_full_exchange(std::move(client.value()));
    server_thread.join();
    CRPC_CHECK_EQ(1, accepted.load());
}

CRPC_TEST(ws_e2e_fragmented_message_over_loopback) {
    inproc_backend backend;
    ws::server_options sopt;
    ws::server srv(backend, endpoint::inproc("ws-e2e-frag"), sopt);
    CRPC_CHECK(srv.running());

    std::atomic<int> accepted{0};
    std::thread server_thread([&] { run_echo_server(srv, accepted); });

    ws::client_options copt;
    copt.host = "test";
    copt.stream.max_fragment_bytes = 97;
    result<std::unique_ptr<ws::client>> client =
        ws::connect(backend, endpoint::inproc("ws-e2e-frag"), copt);
    CRPC_CHECK(client.ok());

    std::string big(1000, 'm');
    CRPC_CHECK(client.value()->send_binary(as_bytes(big)).ok());
    result<ws::event> echoed = client.value()->recv();
    CRPC_CHECK(echoed.ok());
    CRPC_CHECK_EQ(ws::event::kind::message, echoed.value().type);
    CRPC_CHECK_EQ(1000u, echoed.value().data.size());
    CRPC_CHECK_EQ(big, echoed.value().data);

    CRPC_CHECK(client.value()->send_close().ok());
    result<ws::event> closed = client.value()->recv();
    CRPC_CHECK(closed.ok());
    CRPC_CHECK_EQ(ws::event::kind::closed, closed.value().type);

    server_thread.join();
    CRPC_CHECK_EQ(1, accepted.load());
}

CRPC_TEST(ws_e2e_wss_requires_tls_backend) {
    // A wss:// client must never silently fall back to cleartext. Without the
    // TLS backend it fails with a clear precondition; with it, connecting to a
    // *cleartext* listener still fails the TLS handshake.
    inproc_backend backend;
    ws::server_options sopt;
    ws::server srv(backend, endpoint::inproc("ws-e2e-wss"), sopt);
    CRPC_CHECK(srv.running());

    ws::client_options copt;
    copt.host = "localhost";
    copt.tls = true;
    result<std::unique_ptr<ws::client>> client =
        ws::connect(backend, endpoint::inproc("ws-e2e-wss"), copt);
    CRPC_CHECK_FALSE(client.ok());
#if !defined(CRPC_HAVE_OPENSSL)
    CRPC_CHECK_EQ(rpc_code::failed_precondition, client.error().code);
#endif
}

} // namespace

CRPC_TEST_MAIN()