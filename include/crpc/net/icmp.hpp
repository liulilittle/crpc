// CRPC -- ICMP echo (ping) with round-trip statistics.
//
// `icmp_socket` is a single-destination echo client in the same `result`/
// `status` discipline as `net::udp_socket`. It speaks IPv4 echo
// request/reply only and picks its socket type per platform, because the
// operating systems disagree about how an unprivileged process may ping:
//
//   | platform        | unprivileged? | socket type              | recv buffer starts with |
//   |-----------------|---------------|--------------------------|-------------------------|
//   | Android         | yes           | SOCK_DGRAM, IPPROTO_ICMP | the ICMP header         |
//   | Linux           | kernel policy | SOCK_DGRAM, IPPROTO_ICMP | the ICMP header         |
//   | Linux (root)    | yes           | SOCK_RAW, IPPROTO_ICMP   | the IPv4 header         |
//   | macOS           | yes (DGRAM)   | SOCK_DGRAM / SOCK_RAW    | the IPv4 header         |
//   | Windows         | admin only    | SOCK_RAW, IPPROTO_ICMP   | the IPv4 header         |
//
// The DGRAM form is the kernel's "ping socket" (Linux `ping_group_range`,
// Android has it open to every UID). On that socket the kernel owns the
// identifier: it allocates one, reports it as the socket's local port
// (`getsockname()`), overwrites whatever the caller wrote, drops echo
// replies carrying a different identifier, and recomputes the checksum.
// The framework therefore never trusts a caller-chosen identifier in DGRAM
// mode; it reads the kernel-assigned one back and matches replies by
// sequence number. In RAW mode the caller owns the identifier and must
// filter replies by identifier and sequence itself -- a raw socket receives
// every ICMP packet the host sees, including other people's traffic.
//
// ICMP errors (destination unreachable, time exceeded, ...) are not echo
// replies: without `IP_RECVERR`/a connected socket they never surface from
// `recvfrom`, so an unreachable target fails with `deadline_exceeded`
// rather than an unreachable error. The error queue is deliberately not
// wired up; see the file-end note on what is intentionally not supported.
//
// IPv6 is not supported (see `icmp_options`): echo over ICMPv6 needs
// node-local multicast group membership on some stacks and a different
// socket protocol (`IPPROTO_ICMPV6`), which is out of scope here.
#ifndef CRPC_NET_ICMP_HPP
#define CRPC_NET_ICMP_HPP

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/net/ip.hpp>

namespace crpc {
namespace net {

// How the socket is created. The choice is not free: the DGRAM ping socket
// is the only form an unprivileged Android application may use, and the
// RAW form is the only one Windows offers at all.
enum class icmp_mode : std::uint8_t {
    // Try the unprivileged DGRAM ping socket first; fall back to RAW where
    // the platform allows it (Linux without `ping_group_range`, or a
    // privileged caller). Both failing produces an error that names the
    // knob to turn or the privilege to obtain.
    automatic,
    // Force SOCK_DGRAM + IPPROTO_ICMP. The only mode Android supports;
    // Windows has no DGRAM ICMP and rejects this mode up front.
    datagram,
    // Force SOCK_RAW + IPPROTO_ICMP. Needs root/CAP_NET_RAW on Linux and
    // an administrator token on Windows; Android refuses it outright.
    raw
};

// One echo reply. `ttl` is taken from the IPv4 header when the platform
// hands it over (RAW everywhere, DGRAM on macOS via the header, DGRAM on
// Linux/Android via the `IP_RECVTTL` ancillary message); a platform that
// provides neither reports 0. `payload_bytes` counts the reply's user
// bytes, excluding the 8-byte ICMP header.
struct icmp_echo_reply {
    ip_address source;
    std::uint16_t sequence = 0;
    std::uint16_t identifier = 0;
    std::uint8_t ttl = 0;
    std::chrono::nanoseconds rtt{0};
    std::size_t payload_bytes = 0;
};

// Tunables for one ICMP socket.
struct icmp_options {
    // Socket selection policy; `mode()` on the opened socket reports what
    // actually took effect.
    icmp_mode mode = icmp_mode::automatic;
    // User bytes in the echo payload, excluding the 8-byte ICMP header.
    std::size_t payload_bytes = 32;
    // RAW mode: 0 = generate one from the process id. DGRAM mode: ignored,
    // because the kernel assigns (and overwrites) the identifier itself.
    std::uint16_t identifier = 0;
    // IPv4 time-to-live for outgoing requests.
    std::uint8_t ttl = 64;
    // Default reply wait; `echo()` also takes an explicit timeout.
    std::chrono::nanoseconds timeout{1000};
    // IPv4 only. ICMPv6 echo (`IPPROTO_ICMPV6`) is not implemented; a v6
    // target fails `invalid_argument` rather than silently degrading.
    // (Not implemented: IPv6, traceroute and the `IP_RECVERR` error queue.)
};

// The outcome of one ping attempt inside `ping_sequence()`. `replied` is
// false for a timeout (`deadline_exceeded`) or a hard socket failure, in
// which case `error`/`error_message` say which.
struct icmp_ping_result {
    std::uint16_t sequence = 0;
    bool replied = false;
    std::chrono::nanoseconds rtt{0};
    rpc_code error = rpc_code::ok;
    std::string error_message;
};

// Summary of a `ping()` run. `jitter` is the mean absolute difference of
// consecutive successfully-measured RTTs (a simple average, not the RFC
// 3550 smoothed interarrival estimate): it is cheaper, needs no hidden
// state between runs, and for equal-interval probes it answers the same
// "how steady is this path" question. Fewer than two replies yield 0.
struct icmp_ping_statistics {
    std::uint32_t sent = 0;
    std::uint32_t received = 0;
    std::uint32_t lost = 0;
    std::chrono::nanoseconds min_rtt{0};
    std::chrono::nanoseconds max_rtt{0};
    std::chrono::nanoseconds avg_rtt{0};
    std::chrono::nanoseconds jitter{0};
};

// The socket the factory hands out.
class CRPC_API icmp_socket {
public:
    // Prepares an echo client for `target` (an IPv4 literal; nothing here
    // resolves host names -- resolve first and pass the address). The
    // socket is created per `options.mode` and the platform table in the
    // file comment; `mode()` reports the effective choice.
    [[nodiscard]] static result<std::unique_ptr<icmp_socket>> open(
        const ip_address& target, const icmp_options& options = icmp_options{});

    ~icmp_socket();

    icmp_socket(const icmp_socket&) = delete;
    icmp_socket& operator=(const icmp_socket&) = delete;
    icmp_socket(icmp_socket&&) = delete;
    icmp_socket& operator=(icmp_socket&&) = delete;

    // Sends one echo request and waits for the matching reply. Replies
    // whose sequence does not match (late arrivals from an earlier probe
    // on this socket) are discarded and the wait continues. In RAW mode a
    // reply must also carry the socket's identifier, because a raw socket
    // sees every ICMP packet the host receives. Fails `deadline_exceeded`
    // when no matching reply arrives in time.
    [[nodiscard]] result<icmp_echo_reply> echo(std::uint16_t sequence,
                                               std::chrono::nanoseconds timeout);

    // Runs `count` echo requests, waiting `interval` between consecutive
    // sends, and reports each attempt. Sequences are assigned by the
    // socket, monotonically increasing.
    [[nodiscard]] result<std::vector<icmp_ping_result>> ping_sequence(
        std::uint16_t count, std::chrono::nanoseconds interval);

    // Runs `ping_sequence()` and folds the attempts into summary numbers.
    [[nodiscard]] result<icmp_ping_statistics> ping(
        std::uint16_t count, std::chrono::nanoseconds interval);

    [[nodiscard]] status close();
    [[nodiscard]] bool is_open() const noexcept;

    // The socket type that actually took effect (an `automatic` open may
    // have fallen back from DGRAM to RAW).
    [[nodiscard]] icmp_mode mode() const noexcept;
    // DGRAM mode: the kernel-assigned identifier, equal to the socket's
    // local port from `getsockname()`. RAW mode: the caller-chosen or
    // generated identifier.
    [[nodiscard]] std::uint16_t identifier() const noexcept;
    [[nodiscard]] const ip_address& target() const noexcept;
    [[nodiscard]] const icmp_options& options() const noexcept;

private:
    icmp_socket();

    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace net
} // namespace crpc

#endif // CRPC_NET_ICMP_HPP
