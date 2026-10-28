// CRPC -- net address layer tests: parsing, port validation, name resolution
// with its failure classes, and the Unix-domain-socket path rules.
#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/net/address.hpp>
#include <crpc/net/ip.hpp>
#include <crpc/net/listener.hpp>

namespace {

using crpc::net::address;
using crpc::net::ip_address;
using crpc::net::resolve_host;

[[nodiscard]] bool parses_tcp(std::string_view text) {
    return address::parse_tcp(text).ok();
}

} // namespace

CRPC_TEST(net_port_validation) {
    CRPC_CHECK_EQ(std::uint16_t{1}, crpc::net::parse_port("1").value_or(0));
    CRPC_CHECK_EQ(std::uint16_t{65535}, crpc::net::parse_port("65535").value_or(0));
    CRPC_CHECK_EQ(std::uint16_t{6379}, crpc::net::parse_port("6379").value_or(0));
    CRPC_CHECK_FALSE(crpc::net::parse_port("").ok());
    CRPC_CHECK_FALSE(crpc::net::parse_port("0").ok());
    CRPC_CHECK_FALSE(crpc::net::parse_port("65536").ok());
    CRPC_CHECK_FALSE(crpc::net::parse_port("70000").ok());
    CRPC_CHECK_FALSE(crpc::net::parse_port("-1").ok());
    CRPC_CHECK_FALSE(crpc::net::parse_port("abc").ok());
    CRPC_CHECK_FALSE(crpc::net::parse_port("12a").ok());
    CRPC_CHECK_FALSE(crpc::net::parse_port(" 80").ok());
}

CRPC_TEST(net_tcp_address_parsing) {
    auto plain = address::parse_tcp("localhost:6379");
    CRPC_CHECK(plain.ok());
    if (plain.ok()) {
        CRPC_CHECK_EQ(std::string("localhost"), plain.value().host());
        CRPC_CHECK_EQ(std::uint16_t{6379}, plain.value().port());
        CRPC_CHECK(plain.value().is_tcp());
        CRPC_CHECK_EQ(std::string("tcp:localhost:6379"), plain.value().to_string());
    }

    auto ipv4 = address::parse_tcp("127.0.0.1:80");
    CRPC_CHECK(ipv4.ok());
    if (ipv4.ok()) {
        CRPC_CHECK_EQ(std::string("127.0.0.1"), ipv4.value().host());
        CRPC_CHECK_EQ(std::uint16_t{80}, ipv4.value().port());
    }

    // A bracketed IPv6 literal is the only form allowed to contain ':'. It
    // round-trips with brackets so the port stays unambiguous.
    auto ipv6 = address::parse_tcp("[::1]:443");
    CRPC_CHECK(ipv6.ok());
    if (ipv6.ok()) {
        CRPC_CHECK_EQ(std::string("::1"), ipv6.value().host());
        CRPC_CHECK_EQ(std::uint16_t{443}, ipv6.value().port());
        CRPC_CHECK_EQ(std::string("tcp:[::1]:443"), ipv6.value().to_string());
    }

    CRPC_CHECK_FALSE(parses_tcp(""));
    CRPC_CHECK_FALSE(parses_tcp("127.0.0.1"));
    CRPC_CHECK_FALSE(parses_tcp("127.0.0.1:0"));
    CRPC_CHECK_FALSE(parses_tcp("127.0.0.1:70000"));
    CRPC_CHECK_FALSE(parses_tcp("[::1]"));
    CRPC_CHECK_FALSE(parses_tcp("[::1]80"));
    CRPC_CHECK_FALSE(parses_tcp(":80"));
    CRPC_CHECK_FALSE(parses_tcp("host:"));
    CRPC_CHECK_FALSE(parses_tcp("::1:80"));      // must be bracketed
    CRPC_CHECK_FALSE(parses_tcp("host\tname:80"));
}

CRPC_TEST(net_resolve_num_literals) {
    {
        auto resolved = resolve_host("127.0.0.1");
        CRPC_CHECK(resolved.ok());
        if (resolved.ok()) {
            CRPC_CHECK_EQ(std::size_t{1}, resolved.value().size());
            if (!resolved.value().empty()) {
                CRPC_CHECK(resolved.value()[0].is_v4());
                CRPC_CHECK_EQ(std::string("127.0.0.1"), resolved.value()[0].to_string());
            }
        }
    }
    {
        auto resolved = resolve_host("::1");
        CRPC_CHECK(resolved.ok());
        if (resolved.ok()) {
            CRPC_CHECK_EQ(std::size_t{1}, resolved.value().size());
            if (!resolved.value().empty()) {
                CRPC_CHECK(resolved.value()[0].is_v6());
                CRPC_CHECK_EQ(std::string("::1"), resolved.value()[0].to_string());
            }
        }
    }
    {
        // Family filtering works for literals too.
        crpc::net::resolve_options options;
        options.family = crpc::net::ip_family::ipv4;
        auto v4 = resolve_host("::1", options);
        CRPC_CHECK_FALSE(v4.ok());
        options.family = crpc::net::ip_family::ipv6;
        auto v6 = resolve_host("127.0.0.1", options);
        CRPC_CHECK_FALSE(v6.ok());
    }
}

CRPC_TEST(net_resolve_hostname) {
    crpc::net::resolve_options options;
    options.timeout = std::chrono::seconds(5);
    auto resolved = resolve_host("localhost", options);
    CRPC_CHECK(resolved.ok());
    if (resolved.ok() && !resolved.value().empty()) {
        // The loopback name resolves to either family, but the literals must be
        // real loopback values, never a stretched 0.0.0.0.
        for (const ip_address& ip : resolved.value()) {
            if (ip.is_v6()) {
                CRPC_CHECK_EQ(std::string("::1"), ip.to_string());
            } else {
                CRPC_CHECK_EQ(std::string("127.0.0.1"), ip.to_string());
            }
        }
    }
}

CRPC_TEST(net_resolve_rejects_malformed) {
    const auto empty = resolve_host("");
    CRPC_CHECK_FALSE(empty.ok());
    if (!empty.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(empty.code()));
    }
    const std::string control("bad\x01name");
    const auto with_control = resolve_host(control);
    CRPC_CHECK_FALSE(with_control.ok());
    if (!with_control.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(with_control.code()));
    }
}

CRPC_TEST(net_resolve_failure_classification) {
    // ".invalid" is reserved (RFC 6761) to never resolve. A working resolver
    // answers "no such host" quickly; an offline resolver surfaces a deadline
    // instead. Both are resolution failures and the connector-facing class is
    // `unavailable` or `deadline_exceeded`.
    crpc::net::resolve_options options;
    options.timeout = std::chrono::seconds(4);
    const auto bogus = resolve_host("no-such-host.invalid", options);
    CRPC_CHECK_FALSE(bogus.ok());
    if (!bogus.ok()) {
        const bool expected = bogus.code() == crpc::rpc_code::unavailable ||
                              bogus.code() == crpc::rpc_code::deadline_exceeded;
        CRPC_CHECK(expected);
    }
}

CRPC_TEST(net_unix_path_validation) {
    CRPC_CHECK(crpc::net::validate_unix_path("C:/tmp/echo.sock").ok());
    CRPC_CHECK(crpc::net::validate_unix_path("/tmp/echo.sock").ok());
    CRPC_CHECK_FALSE(crpc::net::validate_unix_path("").ok());
    const std::string with_nul("\x00", 1);
    CRPC_CHECK_FALSE(crpc::net::validate_unix_path(with_nul).ok());
    const std::string too_long(200, 'a');
    CRPC_CHECK_FALSE(crpc::net::validate_unix_path(too_long).ok());

    auto parsed = address::parse_unix("/tmp/crpc.sock");
    CRPC_CHECK(parsed.ok());
    if (parsed.ok()) {
        CRPC_CHECK(parsed.value().is_unix());
        CRPC_CHECK_EQ(std::string("/tmp/crpc.sock"), parsed.value().path());
        CRPC_CHECK_EQ(std::string("unix:/tmp/crpc.sock"), parsed.value().to_string());
    }
    CRPC_CHECK_FALSE(address::parse_unix("").ok());
}

CRPC_TEST(net_unix_support_probe_matches_a_real_attempt) {
    const bool probe = crpc::net::unix_socket_supported();
    // A real attempt, not a tautology: bind an AF_UNIX listener to a fresh
    // temporary path and see whether the host truly supports the family. The
    // probe must agree with that attempt -- if it ever disagrees, the
    // probe is testing a different thing than the socket layer does.
    const std::string path =
        (std::filesystem::temp_directory_path() /
         ("crpc-net-unix-probe-" + std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) + ".sock"))
            .string();
    auto bound = crpc::net::listener::bind(crpc::net::address::unix_path(path));
    const bool really_works = bound.ok();
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
    CRPC_CHECK_EQ(really_works, probe);
    if (really_works != probe) {
        // The discrepancy would mean the cached probe disagrees with the
        // actual socket layer; that is a probe contract violation.
        CRPC_CHECK(probe);
    }
}

CRPC_TEST_MAIN()