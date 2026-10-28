// CRPC -- net transport integration tests on real localhost sockets.
//
// No mocks: every case drives a real loopback TCP connection between a
// `net::listener` server thread and a `net::connect` client. The server-side
// threads never touch the test framework counters (they would race); they
// record a plain string, and the main thread asserts it after `join`.
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/net/address.hpp>
#include <crpc/net/channel.hpp>
#include <crpc/net/connector.hpp>
#include <crpc/net/listener.hpp>
#include <crpc/span.hpp>
#include <crpc/config.hpp>

#if defined(CRPC_OS_WINDOWS)
#include <winsock2.h>
#else
#include <cerrno>
#endif

namespace {

using crpc::net::address;
using crpc::net::listener;
using crpc::net::tcp_stream;

[[nodiscard]] crpc::u16 free_loopback_port() {
    auto bound = listener::bind(address::tcp("127.0.0.1", 0));
    if (!bound) {
        return 0;
    }
    return bound.value()->local().port();
}

// Deterministic payload byte.
[[nodiscard]] std::byte payload_byte(std::size_t index) {
    return std::byte{static_cast<unsigned char>((index * 31u + 7u) & 0xffu)};
}

constexpr std::size_t kPayloadBytes = 4u * 1024u * 1024u;

} // namespace

CRPC_TEST(net_listener_binds_and_reports_ephemeral_port) {
    auto bound = listener::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(bound.ok());
    if (bound.ok()) {
        CRPC_CHECK(bound.value()->is_open());
        CRPC_CHECK(bound.value()->local().is_tcp());
        CRPC_CHECK(bound.value()->local().port() != 0);
        CRPC_CHECK_EQ(std::string("tcp"), bound.value()->local().to_string().substr(0, 3));
    }
}

CRPC_TEST(net_connector_success_echo_round_trip) {
    auto bound = listener::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(bound.ok());
    if (!bound.ok()) {
        return;
    }
    auto& server = bound.value();
    const address target = address::tcp(server->local().host(), server->local().port());

    std::atomic<bool> server_done{false};
    std::string server_error;
    std::thread server_thread([&] {
        auto accepted = server->accept(std::chrono::seconds(5));
        if (!accepted) {
            server_error = "server accept failed";
            server_done.store(true);
            return;
        }
        auto& stream = accepted.value();
        auto request = stream->read_exact(5);
        if (!request || request.value().size() != 5) {
            server_error = "server read failed";
        } else {
            (void)stream->write(crpc::span<const std::byte>(request.value().data(),
                                                            request.value().size()));
        }
        server_done.store(true);
    });

    auto client = crpc::net::connect(target);
    CRPC_CHECK(client.ok());
    if (!client.ok()) {
        server->close();
        server_thread.join();
        return;
    }
    CRPC_CHECK(client.value()->is_open());

    const std::byte payload[] = {std::byte{'h'}, std::byte{'e'}, std::byte{'l'},
                                 std::byte{'l'}, std::byte{'o'}};
    CRPC_CHECK(client.value()->write(crpc::span<const std::byte>(payload)).ok());
    auto reply = client.value()->read_exact(5);
    CRPC_CHECK(reply.ok());
    if (reply.ok()) {
        CRPC_CHECK_EQ(std::size_t{5}, reply.value().size());
        for (std::size_t i = 0; i < 5; ++i) {
            CRPC_CHECK(reply.value()[i] == payload[i]);
        }
    }
    server_thread.join();
    CRPC_CHECK(server_error.empty());
}

CRPC_TEST(net_connector_refused_classification) {
    const crpc::u16 port = free_loopback_port();
    CRPC_CHECK(port != 0);
    if (port == 0) {
        return;
    }
    crpc::net::connect_options options;
    options.timeout = std::chrono::seconds(2);
    auto client = crpc::net::connect(address::tcp("127.0.0.1", port), options);
    CRPC_CHECK_FALSE(client.ok());
    if (!client.ok()) {
        // A closed endpoint is either refused (RST: connection_refused) or has
        // its SYN silently dropped (deadline_exceeded) -- a per-host property
        // of the TCP stack. The OS-code-to-rpc_code mapping itself is asserted
        // deterministically in net_os_error_mapping_is_total; this case proves
        // a real connect to a closed port fails with one of the two correct
        // classes, never a success and never an unrelated category.
        const bool expected = client.code() == crpc::rpc_code::connection_refused ||
                              client.code() == crpc::rpc_code::deadline_exceeded;
        CRPC_CHECK(expected);
    }
}

CRPC_TEST(net_os_error_mapping_is_total) {
    using crpc::rpc_code;
#if defined(CRPC_OS_WINDOWS)
    CRPC_CHECK_EQ(static_cast<int>(rpc_code::connection_refused),
                  static_cast<int>(crpc::net::classify_connect_error(WSAECONNREFUSED)));
    // Windows reports some loopback refusals as a reset at connect time.
    CRPC_CHECK_EQ(static_cast<int>(rpc_code::connection_refused),
                  static_cast<int>(crpc::net::classify_connect_error(WSAECONNRESET)));
    CRPC_CHECK_EQ(static_cast<int>(rpc_code::deadline_exceeded),
                  static_cast<int>(crpc::net::classify_connect_error(WSAETIMEDOUT)));
    CRPC_CHECK_EQ(static_cast<int>(rpc_code::unavailable),
                  static_cast<int>(crpc::net::classify_connect_error(WSAENETUNREACH)));
    CRPC_CHECK_EQ(static_cast<int>(rpc_code::invalid_argument),
                  static_cast<int>(crpc::net::classify_connect_error(WSAEADDRINUSE)));
    CRPC_CHECK_EQ(static_cast<int>(rpc_code::resource_exhausted),
                  static_cast<int>(crpc::net::classify_connect_error(WSAENOBUFS)));
    CRPC_CHECK_EQ(static_cast<int>(rpc_code::internal),
                  static_cast<int>(crpc::net::classify_connect_error(1234567)));
    CRPC_CHECK_EQ(static_cast<int>(rpc_code::connection_reset),
                  static_cast<int>(crpc::net::classify_stream_error(WSAECONNRESET)));
    CRPC_CHECK_EQ(static_cast<int>(rpc_code::deadline_exceeded),
                  static_cast<int>(crpc::net::classify_stream_error(WSAETIMEDOUT)));
#else
    CRPC_CHECK_EQ(static_cast<int>(rpc_code::connection_refused),
                  static_cast<int>(crpc::net::classify_connect_error(ECONNREFUSED)));
    CRPC_CHECK_EQ(static_cast<int>(rpc_code::deadline_exceeded),
                  static_cast<int>(crpc::net::classify_connect_error(ETIMEDOUT)));
    CRPC_CHECK_EQ(static_cast<int>(rpc_code::connection_reset),
                  static_cast<int>(crpc::net::classify_stream_error(ECONNRESET)));
    CRPC_CHECK_EQ(static_cast<int>(rpc_code::broken_pipe),
                  static_cast<int>(crpc::net::classify_stream_error(EPIPE)));
#endif
}

CRPC_TEST(net_connector_dns_failure_classification) {
    crpc::net::connect_options options;
    options.timeout = std::chrono::seconds(3);
    const auto client = crpc::net::connect(
        address::tcp("no-such-host.invalid", 12345), options);
    CRPC_CHECK_FALSE(client.ok());
    if (!client.ok()) {
        // Resolution failure is unavailable (or a resolver deadline offline);
        // it must never look like a refused port.
        const bool expected = client.code() == crpc::rpc_code::unavailable ||
                              client.code() == crpc::rpc_code::deadline_exceeded;
        CRPC_CHECK(expected);
    }
}

CRPC_TEST(net_connector_local_bind) {
    auto bound = listener::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(bound.ok());
    if (!bound.ok()) {
        return;
    }
    auto& server = bound.value();
    const address target = address::tcp(server->local().host(), server->local().port());

    std::atomic<bool> server_done{false};
    std::string server_error;
    std::thread server_thread([&] {
        auto accepted = server->accept(std::chrono::seconds(5));
        if (!accepted) {
            server_error = "server accept failed";
        }
        server_done.store(true);
    });

    crpc::net::connect_options options;
    options.local_bind = address::tcp("127.0.0.1", 0);
    auto client = crpc::net::connect(target, options);
    CRPC_CHECK(client.ok());
    if (client.ok()) {
        // The connection must have been made from the requested interface.
        CRPC_CHECK_EQ(std::string("127.0.0.1"), client.value()->local().host());
        // A completed connect() does not mean accept() has returned: the kernel
        // finishes the handshake for the client first. Closing the listener in
        // that window cancels the pending accept and the server thread records
        // an error -- which is correct behaviour for a closed listener, but not
        // what this case is about. Wait for the accept to land first.
        (void)crpc_test::wait_for([&] { return server_done.load(); }, 5000);
    } else {
        server->close();
    }
    server->close();
    server_thread.join();
    CRPC_CHECK(server_error.empty());
}

CRPC_TEST(net_connector_local_bind_port_in_use_is_invalid) {
    // Hold one port, ask the connector to bind another connection to it.
    auto holder = listener::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(holder.ok());
    if (!holder.ok()) {
        return;
    }
    const address busy = address::tcp("127.0.0.1", holder.value()->local().port());
    const crpc::u16 free_port = free_loopback_port();
    CRPC_CHECK(free_port != 0);
    if (free_port == 0) {
        return;
    }
    crpc::net::connect_options options;
    options.timeout = std::chrono::seconds(2);
    options.local_bind = busy;
    auto client = crpc::net::connect(address::tcp("127.0.0.1", free_port), options);
    CRPC_CHECK_FALSE(client.ok());
    if (!client.ok()) {
        // The local bind may be rejected (invalid_argument) or the endpoint
        // may stay unreachable (deadline_exceeded); either way the connector
        // must not silently ignore the busy local address.
        const bool expected = client.code() == crpc::rpc_code::invalid_argument ||
                              client.code() == crpc::rpc_code::deadline_exceeded;
        CRPC_CHECK(expected);
    }
}

CRPC_TEST(net_zero_length_read_and_write) {
    auto bound = listener::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(bound.ok());
    if (!bound.ok()) {
        return;
    }
    auto& server = bound.value();
    const address target = address::tcp(server->local().host(), server->local().port());

    std::atomic<bool> server_done{false};
    std::string server_error;
    std::thread server_thread([&] {
        auto accepted = server->accept(std::chrono::seconds(5));
        if (!accepted) {
            server_error = "server accept failed";
            server_done.store(true);
            return;
        }
        auto& stream = accepted.value();
        auto empty = stream->read_exact(0);
        if (!empty || !empty.value().empty()) {
            server_error = "zero-byte read_exact must return an empty vector";
        }
        crpc::span<std::byte> zero;
        auto none = stream->read_some(zero);
        if (!none || none.value() != 0) {
            server_error = "zero-byte read_some must return 0";
        }
        if (!stream->write(crpc::span<const std::byte>()).ok()) {
            server_error = "empty write must be ok";
        }
        server_done.store(true);
    });

    auto client = crpc::net::connect(target);
    CRPC_CHECK(client.ok());
    if (!client.ok()) {
        server->close();
        server_thread.join();
        return;
    }
    auto empty = client.value()->read_exact(0);
    CRPC_CHECK(empty.ok());
    if (empty.ok()) {
        CRPC_CHECK(empty.value().empty());
    }
    crpc::span<std::byte> zero;
    auto none = client.value()->read_some(zero);
    CRPC_CHECK(none.ok());
    if (none.ok()) {
        CRPC_CHECK_EQ(std::size_t{0}, none.value());
    }
    CRPC_CHECK(client.value()->write(crpc::span<const std::byte>()).ok());
    server_thread.join();
    CRPC_CHECK(server_error.empty());
}

CRPC_TEST(net_clean_close_is_eof_not_error) {
    auto bound = listener::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(bound.ok());
    if (!bound.ok()) {
        return;
    }
    auto& server = bound.value();
    const address target = address::tcp(server->local().host(), server->local().port());

    std::atomic<bool> server_closed{false};
    std::string server_error;
    std::thread server_thread([&] {
        auto accepted = server->accept(std::chrono::seconds(5));
        if (!accepted) {
            server_error = "server accept failed";
            server_closed.store(true);
            return;
        }
        auto request = accepted.value()->read_exact(4);
        if (!request) {
            server_error = "server read failed";
        } else {
            const std::byte ok[] = {std::byte{'o'}, std::byte{'k'}};
            (void)accepted.value()->write(crpc::span<const std::byte>(ok));
        }
        accepted.value()->close();  // clean FIN, after the reply was accepted
        server_closed.store(true);
    });

    auto client = crpc::net::connect(target);
    CRPC_CHECK(client.ok());
    if (!client.ok()) {
        server->close();
        server_thread.join();
        return;
    }
    const std::byte payload[] = {std::byte{'d'}, std::byte{'a'}, std::byte{'t'},
                                 std::byte{'a'}};
    CRPC_CHECK(client.value()->write(crpc::span<const std::byte>(payload)).ok());
    // Every byte the server sent before closing must arrive before the EOF.
    auto reply = client.value()->read_exact(2);
    CRPC_CHECK(reply.ok());
    if (reply.ok()) {
        CRPC_CHECK(reply.value()[0] == std::byte{'o'});
        CRPC_CHECK(reply.value()[1] == std::byte{'k'});
    }
    server_thread.join();
    CRPC_CHECK(server_error.empty());
    CRPC_CHECK(server_closed.load());

    // The peer closed cleanly, having delivered all its bytes: a further read
    // must surface `eof`, never a reset and never a silent hang.
    std::byte sink[4];
    auto after = client.value()->read_some(crpc::span<std::byte>(sink));
    CRPC_CHECK_FALSE(after.ok());
    if (!after.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::eof),
                      static_cast<int>(after.code()));
    }
}

CRPC_TEST(net_abortive_close_is_reset_not_eof) {
    auto bound = listener::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(bound.ok());
    if (!bound.ok()) {
        return;
    }
    auto& server = bound.value();
    const address target = address::tcp(server->local().host(), server->local().port());

    std::atomic<bool> client_ready{false};
    std::atomic<bool> server_done{false};
    std::string server_error;
    std::thread server_thread([&] {
        auto accepted = server->accept(std::chrono::seconds(5));
        if (!accepted) {
            server_error = "server accept failed";
            server_done.store(true);
            return;
        }
        // Wait until the client has an established stream, then RST it.
        while (!client_ready.load()) {
            std::this_thread::yield();
        }
        accepted.value()->abort();
        server_done.store(true);
    });

    auto client = crpc::net::connect(target);
    CRPC_CHECK(client.ok());
    if (!client.ok()) {
        server->close();
        server_thread.join();
        return;
    }
    client_ready.store(true);
    std::byte sink[4];
    auto read = client.value()->read_some(crpc::span<std::byte>(sink));
    CRPC_CHECK_FALSE(read.ok());
    if (!read.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::connection_reset),
                      static_cast<int>(read.code()));
    }
    server_thread.join();
    CRPC_CHECK(server_error.empty());
}

CRPC_TEST(net_half_close_surfaces_eof_to_peer) {
    auto bound = listener::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(bound.ok());
    if (!bound.ok()) {
        return;
    }
    auto& server = bound.value();
    const address target = address::tcp(server->local().host(), server->local().port());

    std::atomic<bool> server_done{false};
    std::string server_error;
    std::thread server_thread([&] {
        auto accepted = server->accept(std::chrono::seconds(5));
        if (!accepted) {
            server_error = "server accept failed";
            server_done.store(true);
            return;
        }
        auto& stream = accepted.value();
        auto request = stream->read_exact(5);
        if (!request) {
            server_error = "server read failed";
            server_done.store(true);
            return;
        }
        // After the client's shutdown_send, the server observes EOF.
        std::byte sink[4];
        auto after = stream->read_some(crpc::span<std::byte>(sink));
        if (after.ok() || after.code() != crpc::rpc_code::eof) {
            server_error = "server did not observe eof after client half-close";
        } else {
            const std::byte ok[] = {std::byte{'o'}, std::byte{'k'}};
            (void)stream->write(crpc::span<const std::byte>(ok));
        }
        server_done.store(true);
    });

    auto client = crpc::net::connect(target);
    CRPC_CHECK(client.ok());
    if (!client.ok()) {
        server->close();
        server_thread.join();
        return;
    }
    const std::byte payload[] = {std::byte{'h'}, std::byte{'e'}, std::byte{'l'},
                                 std::byte{'l'}, std::byte{'o'}};
    CRPC_CHECK(client.value()->write(crpc::span<const std::byte>(payload)).ok());
    CRPC_CHECK(client.value()->shutdown_send().ok());
    auto reply = client.value()->read_exact(2);
    CRPC_CHECK(reply.ok());
    if (reply.ok()) {
        CRPC_CHECK(reply.value()[0] == std::byte{'o'});
        CRPC_CHECK(reply.value()[1] == std::byte{'k'});
    }
    server_thread.join();
    CRPC_CHECK(server_error.empty());
}

CRPC_TEST(net_read_timeout_is_deadline_exceeded) {
    auto bound = listener::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(bound.ok());
    if (!bound.ok()) {
        return;
    }
    auto& server = bound.value();
    const address target = address::tcp(server->local().host(), server->local().port());

    std::atomic<bool> server_done{false};
    std::thread server_thread([&] {
        auto accepted = server->accept(std::chrono::seconds(5));
        if (accepted) {
            // Consume nothing: the client's read must hit its own deadline.
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
        }
        server_done.store(true);
    });

    crpc::net::connect_options options;
    options.stream.rw_timeout = std::chrono::milliseconds(120);
    auto client = crpc::net::connect(target, options);
    CRPC_CHECK(client.ok());
    if (!client.ok()) {
        server->close();
        server_thread.join();
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    std::byte sink[4];
    auto read = client.value()->read_some(crpc::span<std::byte>(sink));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CRPC_CHECK_FALSE(read.ok());
    if (!read.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::deadline_exceeded),
                      static_cast<int>(read.code()));
    }
    CRPC_CHECK(elapsed >= std::chrono::milliseconds(60));
    server_thread.join();
}

CRPC_TEST(net_connector_timeout_on_unreachable_net) {
    // TEST-NET-1 (RFC 5737) is non-routable; a SYN to it stays unanswered, so
    // the connector's select deadline fires. On hosts that reject it instantly
    // with "network unreachable" the classification is `unavailable` instead.
    crpc::net::connect_options options;
    options.timeout = std::chrono::milliseconds(300);
    const auto client = crpc::net::connect(address::tcp("192.0.2.1", 80), options);
    CRPC_CHECK_FALSE(client.ok());
    if (!client.ok()) {
        const bool expected = client.code() == crpc::rpc_code::deadline_exceeded ||
                              client.code() == crpc::rpc_code::unavailable;
        CRPC_CHECK(expected);
    }
}

CRPC_TEST(net_large_payload_survives_fragmentation) {
    auto bound = listener::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(bound.ok());
    if (!bound.ok()) {
        return;
    }
    auto& server = bound.value();
    const address target = address::tcp(server->local().host(), server->local().port());

    std::atomic<bool> server_done{false};
    std::string server_error;
    std::thread server_thread([&] {
        auto accepted = server->accept(std::chrono::seconds(5));
        if (!accepted) {
            server_error = "server accept failed";
            server_done.store(true);
            return;
        }
        crpc::net::stream_options so = server->options().stream;
        so.rw_timeout = std::chrono::seconds(15);
        auto body = accepted.value()->read_exact(kPayloadBytes);
        if (!body || body.value().size() != kPayloadBytes) {
            server_error = "server read_exact did not reassemble the payload";
            server_done.store(true);
            return;
        }
        for (std::size_t i = 0; i < kPayloadBytes; ++i) {
            if (body.value()[i] != payload_byte(i)) {
                server_error = "payload corrupted at " + std::to_string(i);
                break;
            }
        }
        const std::byte ack[] = {std::byte{'a'}, std::byte{'c'}, std::byte{'k'}};
        (void)accepted.value()->write(crpc::span<const std::byte>(ack));
        server_done.store(true);
    });

    crpc::net::connect_options options;
    options.stream.rw_timeout = std::chrono::seconds(15);
    auto client = crpc::net::connect(target, options);
    CRPC_CHECK(client.ok());
    if (!client.ok()) {
        server->close();
        server_thread.join();
        return;
    }
    std::vector<std::byte> chunk(64 * 1024);
    for (std::size_t i = 0; i < kPayloadBytes; i += chunk.size()) {
        for (std::size_t j = 0; j < chunk.size(); ++j) {
            chunk[j] = payload_byte(i + j);
        }
        CRPC_CHECK(client.value()->write(
                       crpc::span<const std::byte>(chunk.data(), chunk.size()))
                       .ok());
        if (!client.value()->is_open()) {
            break;
        }
    }
    auto ack = client.value()->read_exact(3);
    CRPC_CHECK(ack.ok());
    if (ack.ok()) {
        CRPC_CHECK(ack.value()[0] == std::byte{'a'});
        CRPC_CHECK(ack.value()[1] == std::byte{'c'});
        CRPC_CHECK(ack.value()[2] == std::byte{'k'});
    }
    server_thread.join();
    CRPC_CHECK(server_error.empty());
}

CRPC_TEST(net_write_timeout_closes_stream_not_corrupts) {
    auto bound = listener::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(bound.ok());
    if (!bound.ok()) {
        return;
    }
    auto& server = bound.value();
    const address target = address::tcp(server->local().host(), server->local().port());

    std::atomic<bool> server_done{false};
    std::thread server_thread([&] {
        auto accepted = server->accept(std::chrono::seconds(5));
        if (accepted) {
            // Never read: the client's send buffer must fill and time out.
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        server_done.store(true);
    });

    crpc::net::connect_options options;
    options.stream.rw_timeout = std::chrono::milliseconds(150);
    auto client = crpc::net::connect(target, options);
    CRPC_CHECK(client.ok());
    if (!client.ok()) {
        server->close();
        server_thread.join();
        return;
    }
    // 16 MiB with a peer that never reads greatly exceeds any send buffer.
    std::vector<std::byte> chunk(64 * 1024, std::byte{'x'});
    crpc::status last_status;
    bool any_write_ok = true;
    for (std::size_t sent = 0; sent < 16u * 1024u * 1024u; sent += chunk.size()) {
        last_status = client.value()->write(
            crpc::span<const std::byte>(chunk.data(), chunk.size()));
        if (!last_status.ok()) {
            any_write_ok = false;
            break;
        }
    }
    CRPC_CHECK_FALSE(any_write_ok);
    if (!any_write_ok) {
        // A timed-out write is terminal, and the classification is a deadline.
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::deadline_exceeded),
                      static_cast<int>(last_status.code()));
        CRPC_CHECK_FALSE(client.value()->is_open());
    }
    server_thread.join();
}

CRPC_TEST_MAIN()