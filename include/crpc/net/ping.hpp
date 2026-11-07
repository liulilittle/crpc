// CRPC -- ping: ICMP host reachability and RPC PING/PONG round trips.
//
// Two probe families share one result type:
//
//   * ICMP: `ping_host_*` sends one IPv4 echo request and reports the reply's
//     round trip and TTL. The host may be an IPv4 literal or a name; a name is
//     resolved first (IPv4 only, because <crpc/net/icmp.hpp> deliberately does
//     not implement ICMPv6).
//
//   * RPC: `rpc_ping_*` measures the round trip of the connection's own PING
//     frame. It uses the wire protocol that already exists; no new frame type
//     and no payload format are invented here:
//       - `frame_type::ping` is 0x07 and `frame_type::pong` is 0x08
//         (include/crpc/frame.hpp:105-106).
//       - A PING request carries an 8-byte opaque payload. The receiver
//         answers with a PONG on stream 0 that echoes those 8 bytes verbatim:
//         the server does so in src/server/server.cpp:2128-2140, and the
//         client does so in src/rpc/client.cpp:455-485.
//       - The requester distinguishes a reply by the frame type (PONG) and
//         matches it by the echoed payload; the client's existing PONG path is
//         src/rpc/client.cpp:487-505.
//     A public probe uses the payload's most significant bit as a marker so
//     it can never be mistaken for a flow-control probe, whose token is a raw
//     send timestamp (include/crpc/rpc/flow_control.hpp:180-210).
//
// Three shapes per family:
//
//   sync  -- blocks the calling thread until the reply or the timeout.
//   async -- runs the blocking probe on a caller-owned `async::executor` and
//            reports through `async::future<ping_reply>`. This is the exact
//            purpose of that executor: a blocking OS call cannot park a shard
//            coroutine, so it runs off the shard (include/crpc/async/executor.hpp:1-21).
//   co    -- for a corocc-driven shard. C++17 has no language coroutines; the
//            framework's coroutine abstraction is `crpc::runtime::park()`
//            (include/crpc/runtime.hpp:163-174), the same primitive the RPC
//            facade uses to suspend a call (`crpc::client::wait_ready`,
//            src/client/client_facade.cpp:1712-1735). The blocking probe runs
//            on the executor and the calling coroutine parks, so the shard
//            thread stays free. The runtime must be the one running the
//            current coroutine; calling a `_co` function from a plain thread
//            (or from a coroutine of another runtime) falls back to the
//            blocking wait, which is what `runtime::park()` itself reports
//            (`failed_precondition`). The caller supplies both the runtime and
//            the executor because the low-level `rpc::client` does not know a
//            runtime (the facade's shard binding is private to the facade).
//
// Error model: every function returns a value, never throws. `replied == true`
// means `code == rpc_code::ok`, `error` is empty, and `rtt` is the measured
// non-negative round trip. `replied == false` means `code` classifies the
// failure (`deadline_exceeded`, `permission_denied`, `unavailable`, ...) and
// `error` is a human-readable message. A zero timeout is an immediate
// `deadline_exceeded` and never sends a frame; a negative timeout is
// `invalid_argument`.
//
// Lifetime: an async probe captures the client by pointer and the host by
// value; the caller must keep the client alive until the future completes.
// `rpc_ping_sync`/`rpc_ping_async`/`rpc_ping_co` must not be called from a
// callback running inside the client (a call completion), for the same reason
// `rpc::client::start_call` documents: that scope already holds a client lock.
#ifndef CRPC_NET_PING_HPP
#define CRPC_NET_PING_HPP

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include <crpc/async/executor.hpp>
#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc {

class runtime;

namespace rpc {
class client;
} // namespace rpc

namespace net {

// One probe outcome, shared by the ICMP and RPC families.
//
//   replied -- true only when the matching reply arrived.
//   rtt     -- measured round trip; zero when `replied` is false.
//   ttl     -- ICMP only, the reply's IPv4 time-to-live. It is zero when the
//              platform does not hand it over (see <crpc/net/icmp.hpp>) and
//              always zero for the RPC family, which has no IP header.
//   code    -- `ok` for a reply, the classified failure otherwise.
//   error   -- empty for a reply, a readable description otherwise.
struct ping_reply {
    bool replied = false;
    std::chrono::nanoseconds rtt{0};
    std::uint8_t ttl = 0;
    rpc_code code = rpc_code::ok;
    std::string error;
};

// --- ICMP -------------------------------------------------------------------

// Resolves `host` when it is not an IPv4 literal, opens an ICMP echo socket to
// it and sends one request. `timeout` bounds resolution plus the probe.
[[nodiscard]] CRPC_API ping_reply ping_host_sync(
    std::string_view host, std::chrono::nanoseconds timeout);

// Runs `ping_host_sync` on `workers`. The queue may refuse the task, in which
// case the returned future is invalid (`future::valid() == false`).
[[nodiscard]] CRPC_API async::future<ping_reply> ping_host_async(
    async::executor& workers, std::string_view host,
    std::chrono::nanoseconds timeout);

// Coroutine form: parks the current coroutine of `shard` while `workers` runs
// the blocking probe. Falls back to a blocking wait when not called from a
// coroutine of `shard`.
[[nodiscard]] CRPC_API ping_reply ping_host_co(
    runtime& shard, async::executor& workers, std::string_view host,
    std::chrono::nanoseconds timeout);

// --- RPC --------------------------------------------------------------------

// Sends one PING on `connection` and waits for the matching PONG echo.
[[nodiscard]] CRPC_API ping_reply rpc_ping_sync(
    rpc::client& connection, std::chrono::nanoseconds timeout);

// Runs `rpc_ping_sync` on `workers`; the connection must outlive the future.
[[nodiscard]] CRPC_API async::future<ping_reply> rpc_ping_async(
    async::executor& workers, rpc::client& connection,
    std::chrono::nanoseconds timeout);

// Coroutine form: parks the current coroutine of `shard` while `workers` waits
// for the PONG. Falls back to a blocking wait outside a coroutine of `shard`.
[[nodiscard]] CRPC_API ping_reply rpc_ping_co(
    runtime& shard, async::executor& workers, rpc::client& connection,
    std::chrono::nanoseconds timeout);

} // namespace net
} // namespace crpc

#endif // CRPC_NET_PING_HPP
