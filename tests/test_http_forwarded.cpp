// CRPC -- reverse-proxy header resolution tests.
//
// The trust model is what makes these headers safe or unsafe: a chain from an
// untrusted peer is a client-supplied lie, and only the walk from the trusted
// peer produces a usable address. Each case pins one rule of that model, and the
// negative cases matter more than the positive ones.
#include "test_framework.hpp"

#include <cstdint>
#include <string>
#include <string_view>

#include <crpc/error.hpp>
#include <crpc/http/forwarded.hpp>
#include <crpc/http/message.hpp>
#include <crpc/net/ip.hpp>

namespace {

using crpc::http::forwarded_proto;
using crpc::http::request;
using crpc::http::trusted_proxies;
using crpc::http::resolve_forwarded;

[[nodiscard]] crpc::net::ip_address ip(const char* text) {
    return crpc::net::ip_address::parse(text).value();
}

[[nodiscard]] trusted_proxies trust_one_range(const char* range) {
    trusted_proxies trusted;
    trusted.networks.push_back(crpc::net::ip_network::parse(range).value());
    return trusted;
}

[[nodiscard]] request with_headers(std::initializer_list<const char*> headers) {
    request req;
    for (const char* header : headers) {
        const std::string_view text(header);
        const std::size_t colon = text.find(':');
        req.headers.add(text.substr(0, colon), text.substr(colon + 2));
    }
    return req;
}

} // namespace

CRPC_TEST(forwarded_untrusted_peer_is_the_client) {
    const request req = with_headers({"X-Forwarded-For: 192.0.2.1"});
    const trusted_proxies trusted = trust_one_range("10.0.0.0/8");
    const auto client = resolve_forwarded(req, ip("203.0.113.9"), trusted);
    CRPC_CHECK(client.ok());
    if (!client.ok()) {
        return;
    }
    // Nothing in the header is believed: the peer is the client.
    CRPC_CHECK(client.value().has_address());
    CRPC_CHECK_EQ(std::string("203.0.113.9"), client.value().address.to_string());
    CRPC_CHECK_FALSE(client.value().used_forwarded_header);
    CRPC_CHECK_EQ(std::size_t{0}, client.value().trusted_hops);
}

CRPC_TEST(forwarded_xff_single_trusted_proxy) {
    const request req = with_headers({"X-Forwarded-For: 192.0.2.1"});
    const trusted_proxies trusted = trust_one_range("10.0.0.0/8");
    const auto client = resolve_forwarded(req, ip("10.0.0.1"), trusted);
    CRPC_CHECK(client.ok());
    if (!client.ok()) {
        return;
    }
    CRPC_CHECK(client.value().has_address());
    CRPC_CHECK_EQ(std::string("192.0.2.1"), client.value().address.to_string());
    CRPC_CHECK_FALSE(client.value().used_forwarded_header);
    CRPC_CHECK_EQ(std::size_t{0}, client.value().trusted_hops);
}

CRPC_TEST(forwarded_xff_chain_walks_to_first_untrusted) {
    // proxy1 (trusted) -> proxy2 (trusted) -> client (untrusted).
    const request req = with_headers({"X-Forwarded-For: 198.51.100.7, 10.0.4.1"});
    const trusted_proxies trusted = trust_one_range("10.0.0.0/8");
    const auto client = resolve_forwarded(req, ip("10.0.8.1"), trusted);
    CRPC_CHECK(client.ok());
    if (!client.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::string("198.51.100.7"), client.value().address.to_string());
    CRPC_CHECK_EQ(std::size_t{1}, client.value().trusted_hops);
}

CRPC_TEST(forwarded_xff_all_hops_trusted) {
    const request req = with_headers({"X-Forwarded-For: 10.1.2.3"});
    const trusted_proxies trusted = trust_one_range("10.0.0.0/8");
    const auto client = resolve_forwarded(req, ip("10.0.8.1"), trusted);
    CRPC_CHECK(client.ok());
    if (!client.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::string("10.1.2.3"), client.value().address.to_string());
    CRPC_CHECK_EQ(std::size_t{1}, client.value().trusted_hops);
}

CRPC_TEST(forwarded_rfc7239_wins_over_xff) {
    const request req = with_headers({"X-Forwarded-For: 203.0.113.1",
                                      "Forwarded: for=198.51.100.7;proto=https"});
    const trusted_proxies trusted = trust_one_range("10.0.0.0/8");
    const auto client = resolve_forwarded(req, ip("10.0.0.1"), trusted);
    CRPC_CHECK(client.ok());
    if (!client.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::string("198.51.100.7"), client.value().address.to_string());
    CRPC_CHECK(client.value().used_forwarded_header);
    CRPC_CHECK(client.value().is_https());
}

CRPC_TEST(forwarded_rfc7239_host_and_proto) {
    const request req = with_headers(
        {"Forwarded: for=198.51.100.7:6321;proto=https;host=example.com:8443;by=10.0.0.1"});
    const trusted_proxies trusted = trust_one_range("10.0.0.0/8");
    const auto client = resolve_forwarded(req, ip("10.0.0.1"), trusted);
    CRPC_CHECK(client.ok());
    if (!client.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::string("198.51.100.7"), client.value().address.to_string());
    CRPC_CHECK_EQ(std::uint16_t{6321}, client.value().port);
    CRPC_CHECK(client.value().is_https());
    CRPC_CHECK_EQ(std::string("example.com"), client.value().host);
    CRPC_CHECK_EQ(std::uint16_t{8443}, client.value().host_port);
}

CRPC_TEST(forwarded_bracketed_v6_and_quoted_value) {
    {
        const request req = with_headers({"Forwarded: for=[2001:db8::1]:4711"});
        const trusted_proxies trusted = trust_one_range("10.0.0.0/8");
        const auto client = resolve_forwarded(req, ip("10.0.0.1"), trusted);
        CRPC_CHECK(client.ok());
        if (!client.ok()) {
            return;
        }
        CRPC_CHECK(client.value().has_address());
        CRPC_CHECK_EQ(std::string("2001:db8::1"), client.value().address.to_string());
        CRPC_CHECK_EQ(std::uint16_t{4711}, client.value().port);
    }
    {
        // RFC 7239 allows the value to be quoted and escaped.
        const request req = with_headers({"Forwarded: for=\"[2001:db8::2]:4712\";proto=\"https\""});
        const trusted_proxies trusted = trust_one_range("10.0.0.0/8");
        const auto client = resolve_forwarded(req, ip("10.0.0.1"), trusted);
        CRPC_CHECK(client.ok());
        if (!client.ok()) {
            return;
        }
        CRPC_CHECK(client.value().has_address());
        CRPC_CHECK_EQ(std::string("2001:db8::2"), client.value().address.to_string());
        CRPC_CHECK_EQ(std::uint16_t{4712}, client.value().port);
        CRPC_CHECK(client.value().is_https());
    }
}

CRPC_TEST(forwarded_obfuscated_hop_stops_the_walk) {
    // The client side of the chain is obfuscated: the resolver cannot name it,
    // which must be visible as a missing address rather than as a default value.
    const request req = with_headers({"Forwarded: for=_hidden"});
    const trusted_proxies trusted = trust_one_range("10.0.0.0/8");
    const auto client = resolve_forwarded(req, ip("10.0.0.1"), trusted);
    CRPC_CHECK(client.ok());
    if (!client.ok()) {
        return;
    }
    CRPC_CHECK_FALSE(client.value().has_address());
    CRPC_CHECK(client.value().used_forwarded_header);
}

CRPC_TEST(forwarded_obfuscated_hop_in_the_middle) {
    // An obfuscated hop in the middle stops the walk: nothing past it can be
    // verified, even if the names after it would be trusted.
    const request req = with_headers({"X-Forwarded-For: 198.51.100.7, _hidden, 10.0.9.9"});
    const trusted_proxies trusted = trust_one_range("10.0.0.0/8");
    const auto client = resolve_forwarded(req, ip("10.0.8.1"), trusted);
    CRPC_CHECK(client.ok());
    if (!client.ok()) {
        return;
    }
    CRPC_CHECK_FALSE(client.value().has_address());
    // One trusted hop was skipped before the break.
    CRPC_CHECK_EQ(std::size_t{1}, client.value().trusted_hops);
}

CRPC_TEST(forwarded_malformed_header_is_an_error) {
    {
        const request req = with_headers({"Forwarded: for=10.0.0.1 junk"});
        const trusted_proxies trusted = trust_one_range("10.0.0.0/8");
        CRPC_CHECK_FALSE(resolve_forwarded(req, ip("10.0.0.1"), trusted).ok());
    }
    {
        const request req = with_headers({"Forwarded: for"});
        CRPC_CHECK_FALSE(resolve_forwarded(req, ip("10.0.0.1"), trust_one_range("10.0.0.0/8")).ok());
    }
    {
        const request req = with_headers({"Forwarded: "});
        CRPC_CHECK_FALSE(resolve_forwarded(req, ip("10.0.0.1"), trust_one_range("10.0.0.0/8")).ok());
    }
}

CRPC_TEST(forwarded_xff_with_garbage_stops_the_walk) {
    const request req = with_headers({"X-Forwarded-For: 198.51.100.7, not-an-ip"});
    const trusted_proxies trusted = trust_one_range("10.0.0.0/8");
    const auto client = resolve_forwarded(req, ip("10.0.8.1"), trusted);
    CRPC_CHECK(client.ok());
    if (!client.ok()) {
        return;
    }
    CRPC_CHECK_FALSE(client.value().has_address());
    // The hop nearest the peer is the garbage one, so nothing was verified: the
    // count is 0, not 1. Reporting 1 would claim a trusted hop was confirmed.
    CRPC_CHECK_EQ(std::size_t{0}, client.value().trusted_hops);
}

CRPC_TEST(forwarded_xff_fields_resolve_host_and_proto) {
    const request req = with_headers({"X-Forwarded-For: 198.51.100.7",
                                      "X-Forwarded-Proto: https",
                                      "X-Forwarded-Host: example.com",
                                      "X-Forwarded-Port: 8443"});
    const trusted_proxies trusted = trust_one_range("10.0.0.0/8");
    const auto client = resolve_forwarded(req, ip("10.0.0.1"), trusted);
    CRPC_CHECK(client.ok());
    if (!client.ok()) {
        return;
    }
    CRPC_CHECK(client.value().is_https());
    CRPC_CHECK_EQ(std::string("example.com"), client.value().host);
    CRPC_CHECK_EQ(std::uint16_t{8443}, client.value().host_port);
    CRPC_CHECK_FALSE(client.value().used_forwarded_header);
}

CRPC_TEST(forwarded_repeated_xff_fields_are_concatenated) {
    // HTTP allows a list field to be split; the chain is only correct when the
    // fields join in wire order.
    const request req = with_headers({"X-Forwarded-For: 198.51.100.7, 10.0.4.1",
                                      "X-Forwarded-For: 10.0.5.1"});
    const trusted_proxies trusted = trust_one_range("10.0.0.0/8");
    const auto client = resolve_forwarded(req, ip("10.0.8.1"), trusted);
    CRPC_CHECK(client.ok());
    if (!client.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::string("198.51.100.7"), client.value().address.to_string());
    CRPC_CHECK_EQ(std::size_t{2}, client.value().trusted_hops);
}

CRPC_TEST(forwarded_unknown_proto_is_unknown) {
    const request req = with_headers({"X-Forwarded-Proto: ftp"});
    const trusted_proxies trusted = trust_one_range("10.0.0.0/8");
    const auto client = resolve_forwarded(req, ip("10.0.0.1"), trusted);
    CRPC_CHECK(client.ok());
    if (!client.ok()) {
        return;
    }
    CRPC_CHECK(client.value().proto == forwarded_proto::unknown);
    CRPC_CHECK_FALSE(client.value().is_https());
}

CRPC_TEST(forwarded_trust_any_accepts_any_chain) {
    const request req = with_headers({"X-Forwarded-For: 192.0.2.1"});
    trusted_proxies trusted;
    trusted.trust_any = true;
    const auto client = resolve_forwarded(req, ip("203.0.113.9"), trusted);
    CRPC_CHECK(client.ok());
    if (!client.ok()) {
        return;
    }
    // Documented and unsafe: any client can now name itself.
    CRPC_CHECK_EQ(std::string("192.0.2.1"), client.value().address.to_string());
}

CRPC_TEST(forwarded_needs_a_valid_peer) {
    const request req;
    const trusted_proxies trusted = trust_one_range("10.0.0.0/8");
    CRPC_CHECK_FALSE(
        resolve_forwarded(req, crpc::net::ip_address{}, trusted).ok());
}

CRPC_TEST(forwarded_no_headers_and_trusted_peer) {
    const request req;
    const trusted_proxies trusted = trust_one_range("10.0.0.0/8");
    const auto client = resolve_forwarded(req, ip("10.0.0.1"), trusted);
    CRPC_CHECK(client.ok());
    if (!client.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::string("10.0.0.1"), client.value().address.to_string());
    CRPC_CHECK_EQ(std::size_t{0}, client.value().trusted_hops);
    CRPC_CHECK_FALSE(client.value().is_https());
}

CRPC_TEST_MAIN()