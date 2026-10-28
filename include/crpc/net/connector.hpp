// CRPC -- the unified outbound connector.
//
// `net::connect` is the one place a protocol client turns a `net::address` into
// an established `net::tcp_stream`. It replaces the per-adapter copies of
// "create socket, set options, connect, classify the failure" with a single
// implementation whose failure vocabulary is the library's own `rpc_code`.
//
// Failure classification
// ----------------------
//   * `invalid_argument`   -- the target or local address is unusable before any
//                             packet leaves (empty host, port 0, a local bind
//                             that cannot be satisfied). Not retryable.
//   * `connection_refused` -- the peer answered immediately with a reset: no
//                             listener on the port (or the OS translated an
//                             unreachable loopback case into a reset).
//   * `unavailable`        -- DNS failure (name lookup failed) or the network
//                             said "no route / host down". Retryable.
//   * `deadline_exceeded`  -- no answer within `connect_options::timeout`.
//                             Retryable.
//   * `resource_exhausted` -- a local resource (memory, descriptor) ran out.
//
// This is deliberately distinct from a *read/write* reset: `connection_reset`
// on the stream still means "an established stream was torn down by the peer".
#ifndef CRPC_NET_CONNECTOR_HPP
#define CRPC_NET_CONNECTOR_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/net/address.hpp>
#include <crpc/net/channel.hpp>

namespace crpc {
namespace net {

// TCP keepalive settings. When `enabled` is false nothing is configured and the
// kernel default applies (no keepalive probes on most platforms).
struct tcp_keepalive {
    bool enabled = false;
    std::chrono::seconds idle{60};      // first probe after this much idle
    std::chrono::seconds interval{10};  // probes after that
    std::uint32_t probes = 3;           // failures before the link dies
};

// Tunables for one outbound connection.
struct connect_options {
    // Bound on the whole connect (resolution + every candidate family).
    std::chrono::nanoseconds timeout{std::chrono::seconds(3)};

    // Optional local interface to bind before connecting (a TCP-form address
    // whose host is an interface literal). Invalid/empty = the kernel picks.
    address local_bind{};

    bool tcp_no_delay = true;
    tcp_keepalive keepalive{};

    // Socket buffers and the stream's per-operation read/write deadline.
    stream_options stream{};
};

// Connects `target` (TCP host/port or a Unix-domain socket path) and returns an
// established stream. See the file comment for the failure classification.
[[nodiscard]] CRPC_API result<std::unique_ptr<tcp_stream>> connect(
    const address& target, const connect_options& options = connect_options{});

// Maps a native OS socket error to the library's classification for the two
// situations that want different answers:
//
//   * `classify_connect_error` -- a connect attempt failed (`WSAECONNREFUSED` /
//     `ECONNREFUSED` become `connection_refused`, unreachable becomes
//     `unavailable`, a deadline becomes `deadline_exceeded`).
//   * `classify_stream_error` -- an established stream op failed (`ECONNRESET`
//     becomes `connection_reset`, `EPIPE` becomes `broken_pipe`).
//
// The mapping is total: an unrecognised value is `internal`, never a wrong
// category. Upper layers that run their own sockets use these to speak the
// same failure vocabulary as the connector.
[[nodiscard]] CRPC_API rpc_code classify_connect_error(int os_error) noexcept;
[[nodiscard]] CRPC_API rpc_code classify_stream_error(int os_error) noexcept;

} // namespace net
} // namespace crpc

#endif // CRPC_NET_CONNECTOR_HPP