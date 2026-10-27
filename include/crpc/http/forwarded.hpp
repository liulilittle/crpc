// CRPC -- reverse-proxy client resolution.
//
// A request that arrives through nginx (or any proxy) carries the client's
// address in a header, and that header is attacker-controlled. The only safe
// question is "given the peer I am actually connected to, which hop can I still
// trust?", so this resolver takes the peer address and a set of trusted proxy
// networks and walks the chain from the right.
//
// Rules:
//
//   * if the immediate peer is not a trusted proxy, the forwarding headers are
//     ignored entirely and the peer is the client. Accepting them would let
//     anyone set their own address.
//   * otherwise the chain is walked right to left, skipping trusted hops; the
//     first untrusted hop is the client.
//   * RFC 7239 `Forwarded` wins over `X-Forwarded-*` when both are present,
//     because it is the standardised form and is what a modern proxy emits.
//   * a syntactically invalid header is an error, not something to skip: the
//     caller decides whether to answer 400 or to fall back to the peer address.
//
// Scheme, host and port are only believed when the peer itself is trusted, for
// the same reason: they are used for redirects and cookie `Secure` decisions.
#ifndef CRPC_HTTP_FORWARDED_HPP
#define CRPC_HTTP_FORWARDED_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/http/message.hpp>
#include <crpc/net/ip.hpp>

namespace crpc {
namespace http {

enum class forwarded_proto : std::uint8_t { unknown, http, https };

// The set of peers whose forwarding headers are believed.
struct CRPC_API trusted_proxies {
    std::vector<net::ip_network> networks;

    // Trusts any peer. Exists for a deployment whose proxy address is not known
    // in advance; it means any client can forge its own address, so it must be
    // an explicit decision, never a default.
    bool trust_any = false;

    [[nodiscard]] bool trusts(const net::ip_address& address) const noexcept;
};

// The resolved view of a request that may have crossed proxies.
struct CRPC_API forwarded_client {
    // The client address. `address.valid()` is false when the chain named a hop
    // that is not an IP literal (an obfuscated `for=_hidden`, for example), which
    // must be checked before using it.
    net::ip_address address;

    // The client's TCP port, when the header carried one. Zero means unknown.
    std::uint16_t port = 0;

    // The scheme the client used, when it can be trusted.
    forwarded_proto proto = forwarded_proto::unknown;

    // The `Host` the client addressed, when it can be trusted. A proxy that
    // terminates TLS usually reports it in `Forwarded: host=` or
    // `X-Forwarded-Host`, because the proxied request itself says the backend's
    // host.
    std::string host;
    std::uint16_t host_port = 0;

    // True when RFC 7239 `Forwarded` was used, false when the resolver fell back
    // to `X-Forwarded-*` or found neither.
    bool used_forwarded_header = false;

    // Number of trusted hops that were skipped, i.e. how many proxies the request
    // demonstrably crossed.
    std::size_t trusted_hops = 0;

    [[nodiscard]] bool has_address() const noexcept { return address.valid(); }
    [[nodiscard]] bool is_https() const noexcept { return proto == forwarded_proto::https; }
};

// Resolves the client for `request` arriving from `peer`.
//
// `peer` must be the address of the socket the request arrived on, not a value
// taken from a header. Passing a header-derived address here defeats the whole
// trust model.
[[nodiscard]] CRPC_API result<forwarded_client> resolve_forwarded(
    const request& request, const net::ip_address& peer, const trusted_proxies& trusted);

} // namespace http
} // namespace crpc

#endif // CRPC_HTTP_FORWARDED_HPP