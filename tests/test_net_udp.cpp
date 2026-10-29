// CRPC -- net UDP integration tests on real localhost sockets.
//
// One UDP socket binds and talks to another over loopback; the source address
// is read back from `recv_from`, and the truncation case is exercised with a
// deliberately small buffer. No mocks.
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
#include <crpc/net/ip.hpp>
#include <crpc/net/udp.hpp>
#include <crpc/span.hpp>

namespace {

using crpc::net::address;
using crpc::net::udp_socket;

} // namespace

CRPC_TEST(net_udp_round_trip_with_source_address) {
    auto receiver = udp_socket::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(receiver.ok());
    if (!receiver.ok()) {
        return;
    }
    auto& rx = receiver.value();
    const crpc::u16 rx_port = rx->local().port();
    CRPC_CHECK(rx_port != 0);

    auto sender = udp_socket::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(sender.ok());
    if (!sender.ok()) {
        return;
    }
    auto& tx = sender.value();

    const std::byte payload[] = {std::byte{'a'}, std::byte{'b'}, std::byte{'c'},
                                 std::byte{'1'}, std::byte{'2'}, std::byte{'3'}};
    CRPC_CHECK(tx->send_to(crpc::span<const std::byte>(payload),
                           address::tcp("127.0.0.1", rx_port),
                           std::chrono::milliseconds(500)).ok());

    std::byte buffer[16];
    auto datagram = rx->recv_from(crpc::span<std::byte>(buffer),
                                  std::chrono::seconds(2));
    CRPC_CHECK(datagram.ok());
    if (!datagram.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::size_t{6}, datagram.value().bytes);
    for (std::size_t i = 0; i < 6; ++i) {
        CRPC_CHECK(buffer[i] == payload[i]);
    }
    // The sender's address must be reported next to the bytes.
    CRPC_CHECK(datagram.value().source.is_tcp());
    CRPC_CHECK_EQ(std::string("127.0.0.1"), datagram.value().source.host());
    CRPC_CHECK_EQ(tx->local().port(), datagram.value().source.port());
}

CRPC_TEST(net_udp_recv_timeout) {
    auto bound = udp_socket::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(bound.ok());
    if (!bound.ok()) {
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    std::byte buffer[4];
    auto datagram = bound.value()->recv_from(crpc::span<std::byte>(buffer),
                                             std::chrono::milliseconds(120));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CRPC_CHECK_FALSE(datagram.ok());
    if (!datagram.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::deadline_exceeded),
                      static_cast<int>(datagram.code()));
    }
    CRPC_CHECK(elapsed >= std::chrono::milliseconds(60));
}

CRPC_TEST(net_udp_zero_length_datagram) {
    auto receiver = udp_socket::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(receiver.ok());
    if (!receiver.ok()) {
        return;
    }
    auto& rx = receiver.value();
    auto sender = udp_socket::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(sender.ok());
    if (!sender.ok()) {
        return;
    }
    auto& tx = sender.value();

    // A zero-length datagram is a legal, distinct UDP message.
    CRPC_CHECK(tx->send_to(crpc::span<const std::byte>(),
                           address::tcp("127.0.0.1", rx->local().port()),
                           std::chrono::milliseconds(500)).ok());
    std::byte buffer[8];
    auto datagram = rx->recv_from(crpc::span<std::byte>(buffer),
                                  std::chrono::seconds(2));
    CRPC_CHECK(datagram.ok());
    if (datagram.ok()) {
        CRPC_CHECK_EQ(std::size_t{0}, datagram.value().bytes);
        CRPC_CHECK(datagram.value().source.is_tcp());
    }
}

CRPC_TEST(net_udp_oversized_datagram_is_reported) {
    auto receiver = udp_socket::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(receiver.ok());
    if (!receiver.ok()) {
        return;
    }
    auto& rx = receiver.value();
    auto sender = udp_socket::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(sender.ok());
    if (!sender.ok()) {
        return;
    }
    auto& tx = sender.value();

    std::vector<std::byte> big(1024 * 4, std::byte{'M'});
    CRPC_CHECK(tx->send_to(crpc::span<const std::byte>(big.data(), big.size()),
                           address::tcp("127.0.0.1", rx->local().port()),
                           std::chrono::milliseconds(500)).ok());

    // A buffer too small for the datagram must be reported, not silently
    // truncated-and-pretended-to-fit.
    std::byte small[16];
    auto datagram = rx->recv_from(crpc::span<std::byte>(small),
                                  std::chrono::seconds(2));
    CRPC_CHECK_FALSE(datagram.ok());
    if (!datagram.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(datagram.code()));
    }
}

CRPC_TEST(net_udp_multiple_datagrams_keep_order) {
    auto receiver = udp_socket::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(receiver.ok());
    if (!receiver.ok()) {
        return;
    }
    auto& rx = receiver.value();
    auto sender = udp_socket::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(sender.ok());
    if (!sender.ok()) {
        return;
    }
    auto& tx = sender.value();

    const address target = address::tcp("127.0.0.1", rx->local().port());
    for (int i = 0; i < 8; ++i) {
        const std::byte one = std::byte{static_cast<unsigned char>('0' + i)};
        CRPC_CHECK(tx->send_to(crpc::span<const std::byte>(&one, 1), target,
                               std::chrono::milliseconds(500)).ok());
    }
    for (int i = 0; i < 8; ++i) {
        std::byte buffer[2];
        auto datagram = rx->recv_from(crpc::span<std::byte>(buffer),
                                      std::chrono::seconds(2));
        CRPC_CHECK(datagram.ok());
        if (datagram.ok()) {
            CRPC_CHECK_EQ(std::size_t{1}, datagram.value().bytes);
            CRPC_CHECK(buffer[0] == std::byte{static_cast<unsigned char>('0' + i)});
        }
    }
}

CRPC_TEST(net_udp_broadcast_round_trip) {
    auto receiver = udp_socket::bind(address::tcp("0.0.0.0", 0));
    CRPC_CHECK(receiver.ok());
    if (!receiver.ok()) {
        return;
    }
    auto& rx = receiver.value();

    auto sender = udp_socket::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(sender.ok());
    if (!sender.ok()) {
        return;
    }
    auto& tx = sender.value();
    CRPC_CHECK(tx->set_broadcast(true).ok());

    // Windows delivers a loopback broadcast to sockets bound on the wildcard;
    // hosts that reject local broadcast delivery just skip the case.
    const std::byte payload[] = {std::byte{'b'}, std::byte{'c'}, std::byte{'s'}, std::byte{'t'}};
    const crpc::status sent = tx->send_to(crpc::span<const std::byte>(payload),
                                    address::tcp("127.255.255.255", rx->local().port()),
                                    std::chrono::milliseconds(400));
    if (!sent.ok()) {
        crpc_test::skip("local broadcast send is not permitted on this host");
        return;
    }
    std::byte buffer[8];
    auto datagram = rx->recv_from(crpc::span<std::byte>(buffer), std::chrono::seconds(2));
    CRPC_CHECK(datagram.ok());
    if (!datagram.ok()) {
        crpc_test::skip("broadcast datagram was not delivered on this host");
        return;
    }
    CRPC_CHECK_EQ(std::size_t{4}, datagram.value().bytes);
    for (std::size_t i = 0; i < 4; ++i) {
        CRPC_CHECK(buffer[i] == payload[i]);
    }
}

CRPC_TEST(net_udp_local_reports_bound_address) {
    auto bound = udp_socket::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(bound.ok());
    if (bound.ok()) {
        CRPC_CHECK_EQ(std::string("127.0.0.1"), bound.value()->local().host());
        CRPC_CHECK(bound.value()->local().port() != 0);
        CRPC_CHECK(bound.value()->is_open());
        bound.value()->close();
        CRPC_CHECK_FALSE(bound.value()->is_open());
    }
}

CRPC_TEST_MAIN()