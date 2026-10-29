// CRPC -- net ICMP integration tests on real loopback pings and the
// TEST-NET-1 black hole. No mocks: every case below opens a real socket,
// sends real echo requests and measures real round trips, with short
// timeouts where a reply never comes so the suite stays fast.
//
// Privilege note: RAW-mode cases skip with a reason when the OS refuses the
// socket (non-admin Windows, non-root Linux without ping_group_range), so
// the suite runs everywhere and exercises what the environment allows.
#include "test_framework.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/net/icmp.hpp>
#include <crpc/net/ip.hpp>

namespace {

using crpc::net::icmp_mode;
using crpc::net::icmp_options;
using crpc::net::icmp_socket;
using crpc::net::ip_address;

constexpr std::chrono::nanoseconds k_loopback_timeout{1000000000};  // 1 s
constexpr std::chrono::nanoseconds k_blackhole_timeout{200000000};  // 200 ms
constexpr std::chrono::nanoseconds k_ping_interval{10000000};       // 10 ms

[[nodiscard]] icmp_options loopback_options() {
    icmp_options options;
    options.timeout = k_loopback_timeout;
    return options;
}

[[nodiscard]] icmp_options blackhole_options() {
    icmp_options options;
    options.timeout = k_blackhole_timeout;
    return options;
}

// Opens 127.0.0.1 with automatic mode or skips when this environment cannot
// ping at all (e.g. Windows without an administrator token, Linux without
// ping_group_range and without privileges).
[[nodiscard]] std::unique_ptr<icmp_socket> open_loopback_or_skip(
    const icmp_options& options) {
    auto opened = icmp_socket::open(ip_address::parse("127.0.0.1").value(), options);
    if (!opened) {
        const crpc::rpc_code code = opened.code();
        if (code == crpc::rpc_code::permission_denied) {
            crpc_test::skip("ICMP requires privileges in this environment");
        }
        CRPC_CHECK(false);  // any other failure is a real defect
        return nullptr;
    }
    return std::move(opened.value());
}

} // namespace

CRPC_TEST(net_icmp_open_reports_effective_mode) {
    auto socket = open_loopback_or_skip(loopback_options());
    CRPC_CHECK(socket->is_open());
    CRPC_CHECK(socket->identifier() != 0);
    CRPC_CHECK(socket->target() == ip_address::parse("127.0.0.1").value());
    CRPC_CHECK_EQ(std::size_t{32}, socket->options().payload_bytes);
#if defined(CRPC_OS_WINDOWS)
    // Windows has no DGRAM ICMP; automatic must land on raw.
    CRPC_CHECK_EQ(static_cast<int>(icmp_mode::raw),
                  static_cast<int>(socket->mode()));
#elif defined(__ANDROID__)
    // Android only allows the unprivileged DGRAM ping socket.
    CRPC_CHECK_EQ(static_cast<int>(icmp_mode::datagram),
                  static_cast<int>(socket->mode()));
#else
    // Linux: DGRAM when ping_group_range permits, otherwise the RAW
    // fallback (root/CAP_NET_RAW). Both are legitimate automatic outcomes.
    const icmp_mode effective = socket->mode();
    CRPC_CHECK(effective == icmp_mode::datagram || effective == icmp_mode::raw);
#endif
    (void)socket->close();
    CRPC_CHECK_FALSE(socket->is_open());
}

CRPC_TEST(net_icmp_ping_loopback_statistics) {
    auto socket = open_loopback_or_skip(loopback_options());
    auto stats = socket->ping(3, k_ping_interval);
    CRPC_CHECK(stats.ok());
    if (!stats.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::uint32_t{3}, stats.value().sent);
    CRPC_CHECK_EQ(std::uint32_t{3}, stats.value().received);
    CRPC_CHECK_EQ(std::uint32_t{0}, stats.value().lost);
    CRPC_CHECK(stats.value().min_rtt > std::chrono::nanoseconds{0});
    CRPC_CHECK(stats.value().max_rtt > std::chrono::nanoseconds{0});
    CRPC_CHECK(stats.value().avg_rtt > std::chrono::nanoseconds{0});
    CRPC_CHECK(stats.value().min_rtt <= stats.value().avg_rtt);
    CRPC_CHECK(stats.value().avg_rtt <= stats.value().max_rtt);
    CRPC_CHECK(stats.value().min_rtt <= stats.value().max_rtt);
    CRPC_CHECK(stats.value().jitter >= std::chrono::nanoseconds{0});
}

CRPC_TEST(net_icmp_echo_reply_fields) {
    auto socket = open_loopback_or_skip(loopback_options());
    auto reply = socket->echo(77, k_loopback_timeout);
    CRPC_CHECK(reply.ok());
    if (!reply.ok()) {
        return;
    }
    // The reply must come from the probed address, carry the request's
    // sequence and payload size, and a positive round trip.
    CRPC_CHECK(reply.value().source == ip_address::parse("127.0.0.1").value());
    CRPC_CHECK_EQ(static_cast<int>(77), static_cast<int>(reply.value().sequence));
    CRPC_CHECK_EQ(static_cast<int>(socket->identifier()),
                  static_cast<int>(reply.value().identifier));
    CRPC_CHECK_EQ(std::size_t{32}, reply.value().payload_bytes);
    CRPC_CHECK(reply.value().rtt > std::chrono::nanoseconds{0});
    // TTL travels from the IPv4 header (RAW, macOS DGRAM) or the IP_RECVTTL
    // ancillary message (Linux/Android DGRAM); zero means "not available".
    // On every platform this suite targets the value is available.
    CRPC_CHECK(reply.value().ttl > 0);
}

CRPC_TEST(net_icmp_sequence_matching_no_crosstalk) {
    auto socket = open_loopback_or_skip(loopback_options());
    // Sequential probes with distinct sequence numbers: each reply must
    // match its own request (a late reply of probe N must never satisfy
    // probe N+1, which is what the sequence filter inside echo() guards).
    for (int sequence : {100, 101, 205}) {
        auto reply = socket->echo(static_cast<std::uint16_t>(sequence),
                                  k_loopback_timeout);
        CRPC_CHECK(reply.ok());
        if (!reply.ok()) {
            return;
        }
        CRPC_CHECK_EQ(sequence, static_cast<int>(reply.value().sequence));
        CRPC_CHECK_EQ(static_cast<int>(socket->identifier()),
                      static_cast<int>(reply.value().identifier));
    }
}

CRPC_TEST(net_icmp_timeout_to_test_net_1) {
    // TEST-NET-1 (RFC 5737) must not be routed anywhere, so no echo reply
    // can come back: the probe has to end in deadline_exceeded and the
    // statistics have to count every attempt as lost. Short timeouts keep
    // this case fast. ICMP "destination unreachable" errors are not echo
    // replies and (without IP_RECVERR) never surface from recv on an
    // unconnected socket, so the timeout path is the expected outcome on
    // every platform this suite runs on.
    auto opened =
        icmp_socket::open(ip_address::parse("192.0.2.1").value(),
                          blackhole_options());
    if (!opened && opened.code() == crpc::rpc_code::permission_denied) {
        crpc_test::skip("ICMP requires privileges in this environment");
    }
    CRPC_CHECK(opened.ok());
    if (!opened.ok()) {
        return;
    }
    auto socket = std::move(opened.value());
    auto reply = socket->echo(9, k_blackhole_timeout);
    CRPC_CHECK_FALSE(reply.ok());
    if (!reply.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::deadline_exceeded),
                      static_cast<int>(reply.code()));
    }

    auto stats = socket->ping(3, k_ping_interval);
    CRPC_CHECK(stats.ok());
    if (!stats.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::uint32_t{3}, stats.value().sent);
    CRPC_CHECK_EQ(std::uint32_t{0}, stats.value().received);
    CRPC_CHECK_EQ(std::uint32_t{3}, stats.value().lost);
    CRPC_CHECK(stats.value().min_rtt == std::chrono::nanoseconds{0});
    CRPC_CHECK(stats.value().avg_rtt == std::chrono::nanoseconds{0});
    CRPC_CHECK(stats.value().jitter == std::chrono::nanoseconds{0});

    auto attempts = socket->ping_sequence(2, k_ping_interval);
    CRPC_CHECK(attempts.ok());
    if (!attempts.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::size_t{2}, attempts.value().size());
    for (std::size_t i = 0; i < attempts.value().size(); ++i) {
        CRPC_CHECK_FALSE(attempts.value()[i].replied);
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::deadline_exceeded),
                      static_cast<int>(attempts.value()[i].error));
    }
    // The socket assigns sequences monotonically across the whole run
    // (the ping() above consumed 1..3, so this run continues at 4).
    CRPC_CHECK_EQ(static_cast<int>(4),
                  static_cast<int>(attempts.value()[0].sequence));
    CRPC_CHECK_EQ(static_cast<int>(5),
                  static_cast<int>(attempts.value()[1].sequence));
}

CRPC_TEST(net_icmp_datagram_identifier_is_kernel_assigned) {
#if defined(CRPC_OS_WINDOWS)
    crpc_test::skip("Windows provides ICMP only through raw sockets");
#else
    icmp_options options;
    options.mode = icmp_mode::datagram;
    options.timeout = k_loopback_timeout;
    auto first = icmp_socket::open(ip_address::parse("127.0.0.1").value(), options);
    if (!first && first.code() == crpc::rpc_code::permission_denied) {
        crpc_test::skip("ping_group_range does not include this user");
    }
    CRPC_CHECK(first.ok());
    if (!first.ok()) {
        return;
    }
    // The DGRAM identifier is the socket's local port as assigned by the
    // kernel (getsockname); it is never 0 and never chosen by the caller.
    const std::uint16_t first_id = first.value()->identifier();
    CRPC_CHECK(first_id != 0);

    // Two sockets cannot share one kernel-assigned identifier.
    auto second = icmp_socket::open(ip_address::parse("127.0.0.1").value(), options);
    CRPC_CHECK(second.ok());
    if (second.ok()) {
        const std::uint16_t second_id = second.value()->identifier();
        CRPC_CHECK(second_id != 0);
        CRPC_CHECK(first_id != second_id);
    }

    // The kernel echoes the getsockname identifier back on the wire: the
    // reply's identifier field must equal what the socket reports.
    auto reply = first.value()->echo(31, k_loopback_timeout);
    CRPC_CHECK(reply.ok());
    if (reply.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(first_id),
                      static_cast<int>(reply.value().identifier));
    }
#endif
}

CRPC_TEST(net_icmp_raw_identifier_is_the_configured_one) {
    icmp_options options;
    options.mode = icmp_mode::raw;
    options.identifier = 0x1234;
    options.timeout = k_loopback_timeout;
    auto opened = icmp_socket::open(ip_address::parse("127.0.0.1").value(), options);
    if (!opened && opened.code() == crpc::rpc_code::permission_denied) {
        crpc_test::skip("raw ICMP sockets require privileges here");
    }
    CRPC_CHECK(opened.ok());
    if (!opened.ok()) {
        return;
    }
    // RAW: the caller's identifier is used verbatim and the reply carries it.
    CRPC_CHECK_EQ(static_cast<int>(0x1234),
                  static_cast<int>(opened.value()->identifier()));
    auto reply = opened.value()->echo(5, k_loopback_timeout);
    CRPC_CHECK(reply.ok());
    if (reply.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(0x1234),
                      static_cast<int>(reply.value().identifier));
        CRPC_CHECK_EQ(static_cast<int>(5),
                      static_cast<int>(reply.value().sequence));
    }
}

CRPC_TEST(net_icmp_raw_identifier_auto_generated) {
    icmp_options options;
    options.mode = icmp_mode::raw;
    options.identifier = 0;  // auto-generate
    options.timeout = k_loopback_timeout;
    auto opened = icmp_socket::open(ip_address::parse("127.0.0.1").value(), options);
    if (!opened && opened.code() == crpc::rpc_code::permission_denied) {
        crpc_test::skip("raw ICMP sockets require privileges here");
    }
    CRPC_CHECK(opened.ok());
    if (!opened.ok()) {
        return;
    }
    CRPC_CHECK(opened.value()->identifier() != 0);
    // A second socket must not collide with the first.
    auto other =
        icmp_socket::open(ip_address::parse("127.0.0.1").value(), options);
    if (other.ok()) {
        CRPC_CHECK(opened.value()->identifier() != other.value()->identifier());
    }
}

CRPC_TEST(net_icmp_forced_raw_rejected_on_android) {
#if defined(__ANDROID__)
    icmp_options options;
    options.mode = icmp_mode::raw;
    auto opened = icmp_socket::open(ip_address::parse("127.0.0.1").value(), options);
    CRPC_CHECK_FALSE(opened.ok());
    if (!opened.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                      static_cast<int>(opened.code()));
        CRPC_CHECK(opened.error().message.find("Android") != std::string::npos);
    }
#else
    crpc_test::skip("android-only case");
#endif
}

CRPC_TEST(net_icmp_forced_datagram_rejected_on_windows) {
#if defined(CRPC_OS_WINDOWS)
    icmp_options options;
    options.mode = icmp_mode::datagram;
    auto opened = icmp_socket::open(ip_address::parse("127.0.0.1").value(), options);
    CRPC_CHECK_FALSE(opened.ok());
    if (!opened.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                      static_cast<int>(opened.code()));
    }
#else
    crpc_test::skip("windows-only case");
#endif
}

CRPC_TEST(net_icmp_bad_targets_rejected) {
    icmp_options options;
    options.timeout = std::chrono::milliseconds{50};

    // Default-constructed (invalid) address.
    auto invalid = icmp_socket::open(ip_address{}, options);
    CRPC_CHECK_FALSE(invalid.ok());
    if (!invalid.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(invalid.code()));
    }

    // IPv6: explicitly not implemented, must fail loudly.
    auto v6 = icmp_socket::open(ip_address::parse("::1").value(), options);
    CRPC_CHECK_FALSE(v6.ok());
    if (!v6.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(v6.code()));
        CRPC_CHECK(v6.error().message.find("IPv4") != std::string::npos);
    }
}

CRPC_TEST(net_icmp_ping_zero_count) {
    auto socket = open_loopback_or_skip(loopback_options());
    auto stats = socket->ping(0, k_ping_interval);
    CRPC_CHECK(stats.ok());
    if (stats.ok()) {
        CRPC_CHECK_EQ(std::uint32_t{0}, stats.value().sent);
        CRPC_CHECK_EQ(std::uint32_t{0}, stats.value().received);
        CRPC_CHECK_EQ(std::uint32_t{0}, stats.value().lost);
    }
}

CRPC_TEST(net_icmp_closed_socket_fails_cleanly) {
    auto socket = open_loopback_or_skip(loopback_options());
    CRPC_CHECK(socket->close().ok());
    CRPC_CHECK_FALSE(socket->is_open());
    auto reply = socket->echo(1, k_blackhole_timeout);
    CRPC_CHECK_FALSE(reply.ok());
    if (!reply.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::shutting_down),
                      static_cast<int>(reply.code()));
    }
    auto stats = socket->ping(1, k_ping_interval);
    CRPC_CHECK_FALSE(stats.ok());
    // Idempotent close.
    CRPC_CHECK(socket->close().ok());
}

CRPC_TEST_MAIN()
