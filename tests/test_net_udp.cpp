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

// `join_multicast` and `leave_multicast` are advertised in the header next to
// `set_broadcast`, which is tested; the two multicast halves were never called by
// anything, so their Windows and POSIX branches had never run once.
//
// Two layers, because they fail for different reasons and only one of them depends
// on the host:
//
//   * the state transition is unconditional. Joining puts the socket into the group,
//     so leaving then succeeds; leaving a group the socket is not in fails with
//     EADDRNOTAVAIL on both platforms. That asymmetry is observable without any
//     multicast routing, and it is what proves the two calls actually change socket
//     state rather than returning ok unconditionally.
//
//   * delivery needs a host that routes group traffic. A container without a
//     multicast route, or a firewall that drops it, is not a library defect, so that
//     half skips -- the same rule the broadcast case follows.
CRPC_TEST(net_udp_multicast_join_receives_and_leave_stops) {
    // A group in the administratively scoped range (239.0.0.0/8), which needs no
    // coordination and is the conventional choice for local tests.
    auto parsed_group = crpc::net::ip_address::parse("239.255.42.99");
    CRPC_CHECK(parsed_group.ok());
    if (!parsed_group.ok()) {
        return;
    }
    const crpc::net::ip_address group = parsed_group.value();
    CRPC_CHECK(group.valid());

    auto receiver = udp_socket::bind(address::tcp("0.0.0.0", 0));
    CRPC_CHECK(receiver.ok());
    if (!receiver.ok()) {
        return;
    }
    auto& rx = receiver.value();
    const crpc::u16 rx_port = rx->local().port();
    CRPC_CHECK(rx_port != 0);

    const crpc::status joined = rx->join_multicast(group, 0);
    if (!joined.ok()) {
        crpc_test::skip("this host cannot join an IPv4 multicast group");
        return;
    }

    // Leaving a group the socket just joined must succeed.
    CRPC_CHECK(rx->leave_multicast(group, 0).ok());
    // Leaving it a second time must not: the socket is no longer a member. This is
    // what shows the first leave was not a no-op.
    CRPC_CHECK_FALSE(rx->leave_multicast(group, 0).ok());

    // Join again for the delivery half, so the socket is in the group if the host
    // can deliver.
    if (!rx->join_multicast(group, 0).ok()) {
        crpc_test::skip("re-joining the multicast group failed on this host");
        return;
    }

    auto sender = udp_socket::bind(address::tcp("127.0.0.1", 0));
    CRPC_CHECK(sender.ok());
    if (!sender.ok()) {
        (void)rx->leave_multicast(group, 0);
        return;
    }
    auto& tx = sender.value();

    const std::byte payload[] = {std::byte{'m'}, std::byte{'c'}, std::byte{'a'},
                                 std::byte{'s'}, std::byte{'t'}};
    const crpc::status sent = tx->send_to(
        crpc::span<const std::byte>(payload),
        address::tcp(group.to_string(), rx_port), std::chrono::milliseconds(400));
    if (!sent.ok()) {
        (void)rx->leave_multicast(group, 0);
        crpc_test::skip("local multicast send is not permitted on this host");
        return;
    }

    std::byte buffer[8];
    auto datagram = rx->recv_from(crpc::span<std::byte>(buffer), std::chrono::seconds(2));
    if (!datagram.ok()) {
        (void)rx->leave_multicast(group, 0);
        crpc_test::skip("multicast datagram was not delivered on this host");
        return;
    }
    CRPC_CHECK_EQ(std::size_t{5}, datagram.value().bytes);
    for (std::size_t i = 0; i < 5; ++i) {
        CRPC_CHECK(buffer[i] == payload[i]);
    }

    // After leaving, group traffic must no longer be delivered here.
    CRPC_CHECK(rx->leave_multicast(group, 0).ok());
    const crpc::status sent_after = tx->send_to(
        crpc::span<const std::byte>(payload),
        address::tcp(group.to_string(), rx_port), std::chrono::milliseconds(400));
    if (sent_after.ok()) {
        auto late = rx->recv_from(crpc::span<std::byte>(buffer),
                                  std::chrono::milliseconds(500));
        CRPC_CHECK_FALSE(late.ok());
    }
}

CRPC_TEST_MAIN()