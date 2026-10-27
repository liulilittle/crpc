// CRPC -- IP address and CIDR tests.
//
// Address parsing is a security boundary: a lenient parser lets a forged hop
// through. The cases therefore pin what must be REJECTED as carefully as what
// must be accepted, and round-tripping is checked against the RFC 5952 canonical
// form rather than against the input spelling.
#include "test_framework.hpp"

#include <cstdint>
#include <string>
#include <string_view>

#include <crpc/error.hpp>
#include <crpc/net/ip.hpp>

namespace {

using crpc::net::ip_address;
using crpc::net::ip_network;

[[nodiscard]] bool accepts(std::string_view text) {
    return ip_address::parse(text).ok();
}

} // namespace

CRPC_TEST(ip_v4_accepts_canonical_forms) {
    struct sample {
        const char* text;
        const char* canonical;
    };
    const sample samples[] = {
        {"0.0.0.0", "0.0.0.0"},
        {"127.0.0.1", "127.0.0.1"},
        {"10.0.0.255", "10.0.0.255"},
        {"255.255.255.255", "255.255.255.255"},
        {"192.0.2.1", "192.0.2.1"},
        {"1.2.3.4", "1.2.3.4"},
    };
    for (const sample& item : samples) {
        const auto parsed = ip_address::parse(item.text);
        CRPC_CHECK(parsed.ok());
        if (!parsed.ok()) {
            continue;
        }
        CRPC_CHECK(parsed.value().is_v4());
        CRPC_CHECK(!parsed.value().is_v6());
        CRPC_CHECK_EQ(std::string(item.canonical), parsed.value().to_string());
    }
}

CRPC_TEST(ip_v4_rejects_malformed) {
    const char* bad[] = {
        "",          "1",         "1.2",       "1.2.3",     "1.2.3.4.5",
        "1.2.3.",    ".1.2.3",    "256.1.1.1", "1.2.3.256", "-1.2.3.4",
        "1.2.3.-4",  "1.2.3.4 ",  " 1.2.3.4",  "1.2.3.4a",  "0x7f.0.0.1",
        "010.0.0.1", "00.0.0.0",  "1..2.3",    "1.2.3.4/8",
    };
    for (const char* text : bad) {
        CRPC_CHECK_FALSE(accepts(text));
    }
}

CRPC_TEST(ip_v6_accepts_and_canonicalises) {
    struct sample {
        const char* text;
        const char* canonical;
    };
    const sample samples[] = {
        {"::", "::"},
        {"::1", "::1"},
        {"0:0:0:0:0:0:0:0", "::"},
        {"0:0:0:0:0:0:0:1", "::1"},
        {"2001:db8::1", "2001:db8::1"},
        {"2001:0db8:0000:0000:0000:0000:0000:0001", "2001:db8::1"},
        {"FE80::1", "fe80::1"},
        {"fe80::1%eth0", ""}, // zone id is not part of the literal
        {"2001:db8:0:0:1:0:0:1", "2001:db8::1:0:0:1"},
        {"::ffff:192.0.2.1", "::ffff:c000:201"},
        {"1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7:8"},
    };
    for (const sample& item : samples) {
        if (std::string(item.text) == "fe80::1%eth0") {
            // A zone id is transport metadata, not an address; it must be refused
            // here rather than silently ignored.
            CRPC_CHECK_FALSE(accepts(item.text));
            continue;
        }
        const auto parsed = ip_address::parse(item.text);
        CRPC_CHECK(parsed.ok());
        if (!parsed.ok()) {
            continue;
        }
        CRPC_CHECK(parsed.value().is_v6());
        CRPC_CHECK_EQ(std::string(item.canonical), parsed.value().to_string());
    }
}

CRPC_TEST(ip_v6_rejects_malformed) {
    const char* bad[] = {
        ":",          ":::",        "1::2::3",    "12345::1",   "::1::",
        "1:2:3:4:5:6:7",            "1:2:3:4:5:6:7:8:9",
        "1:2:3:4:5:6:7:",           "::ffff:192.0.2",
        "::ffff:192.0.2.256",       "g::1",       "1:2:3:4:5:6:7:8::",
        "2001:db8::1 ",
    };
    for (const char* text : bad) {
        CRPC_CHECK_FALSE(accepts(text));
    }
}

CRPC_TEST(ip_v4_and_v6_are_not_interchangeable) {
    const auto v4 = ip_address::parse("127.0.0.1");
    const auto v6 = ip_address::parse("::1");
    CRPC_CHECK(v4.ok());
    CRPC_CHECK(v6.ok());
    if (v4.ok() && v6.ok()) {
        CRPC_CHECK(v4.value() != v6.value());
        // The v4-mapped form is a distinct address value, which is why a v4
        // network cannot contain it.
        const auto mapped = ip_address::parse("::ffff:127.0.0.1");
        CRPC_CHECK(mapped.ok());
        if (mapped.ok()) {
            CRPC_CHECK(mapped.value() != v4.value());
            CRPC_CHECK(mapped.value().is_v6());
        }
    }
}

CRPC_TEST(ip_network_parses_and_masks) {
    {
        const auto network = ip_network::parse("10.0.0.0/8");
        CRPC_CHECK(network.ok());
        if (network.ok()) {
            CRPC_CHECK(network.value().is_v4());
            CRPC_CHECK_EQ(std::uint8_t{8}, network.value().prefix());
            CRPC_CHECK_EQ(std::string("10.0.0.0/8"), network.value().to_string());
        }
    }
    {
        // Host bits are masked off, so two spellings of one network are equal.
        const auto with_host_bits = ip_network::parse("10.1.2.3/8");
        CRPC_CHECK(with_host_bits.ok());
        if (with_host_bits.ok()) {
            CRPC_CHECK_EQ(std::string("10.0.0.0/8"), with_host_bits.value().to_string());
        }
    }
    {
        // A bare address is a full-length prefix.
        const auto single = ip_network::parse("192.0.2.1");
        CRPC_CHECK(single.ok());
        if (single.ok()) {
            CRPC_CHECK_EQ(std::uint8_t{32}, single.value().prefix());
        }
        const auto single_v6 = ip_network::parse("2001:db8::1");
        CRPC_CHECK(single_v6.ok());
        if (single_v6.ok()) {
            CRPC_CHECK_EQ(std::uint8_t{128}, single_v6.value().prefix());
        }
    }
    {
        const auto v6 = ip_network::parse("2001:db8::/32");
        CRPC_CHECK(v6.ok());
        if (v6.ok()) {
            CRPC_CHECK_FALSE(v6.value().is_v4());
            CRPC_CHECK_EQ(std::string("2001:db8::/32"), v6.value().to_string());
        }
    }
}

CRPC_TEST(ip_network_rejects_bad_prefix) {
    const char* bad[] = {"10.0.0.0/", "10.0.0.0/33", "10.0.0.0/-1", "10.0.0.0/8/8",
                         "10.0.0.0/abc", "::/129", "::/", "10.0.0.0/ 8"};
    for (const char* text : bad) {
        CRPC_CHECK_FALSE(ip_network::parse(text).ok());
    }
}

CRPC_TEST(ip_network_contains) {
    const auto lan = ip_network::parse("10.0.0.0/8");
    const auto host = ip_network::parse("10.0.0.0/32");
    CRPC_CHECK(lan.ok());
    CRPC_CHECK(host.ok());
    if (!lan.ok() || !host.ok()) {
        return;
    }

    const auto inside = ip_address::parse("10.255.1.2");
    const auto outside = ip_address::parse("11.0.0.1");
    const auto almost = ip_address::parse("9.255.255.255");
    CRPC_CHECK(inside.ok());
    CRPC_CHECK(outside.ok());
    CRPC_CHECK(almost.ok());
    if (!inside.ok() || !outside.ok() || !almost.ok()) {
        return;
    }
    CRPC_CHECK(lan.value().contains(inside.value()));
    CRPC_CHECK_FALSE(lan.value().contains(outside.value()));
    CRPC_CHECK_FALSE(lan.value().contains(almost.value()));
    CRPC_CHECK(host.value().contains(inside.value()) == false);
    const auto exact = ip_address::parse("10.0.0.0");
    CRPC_CHECK(exact.ok());
    if (exact.ok()) {
        CRPC_CHECK(host.value().contains(exact.value()));
    }

    // A v4 network never contains a v6 address, not even a v4-mapped one.
    const auto mapped = ip_address::parse("::ffff:10.0.0.1");
    CRPC_CHECK(mapped.ok());
    if (mapped.ok()) {
        CRPC_CHECK_FALSE(lan.value().contains(mapped.value()));
    }

    // Prefix boundaries: /7 covers 10.0.0.0 and 11.0.0.0, /9 splits at 10.128.
    const auto seven = ip_network::parse("10.0.0.0/7");
    CRPC_CHECK(seven.ok());
    if (seven.ok()) {
        CRPC_CHECK(seven.value().contains(outside.value()));
        CRPC_CHECK_FALSE(seven.value().contains(almost.value()));
    }
    const auto nine = ip_network::parse("10.0.0.0/9");
    const auto low = ip_address::parse("10.127.255.255");
    const auto high = ip_address::parse("10.128.0.0");
    CRPC_CHECK(nine.ok());
    CRPC_CHECK(low.ok());
    CRPC_CHECK(high.ok());
    if (nine.ok() && low.ok() && high.ok()) {
        CRPC_CHECK(nine.value().contains(low.value()));
        CRPC_CHECK_FALSE(nine.value().contains(high.value()));
    }
}

CRPC_TEST(ip_network_v6_contains) {
    const auto network = ip_network::parse("2001:db8::/32");
    CRPC_CHECK(network.ok());
    if (!network.ok()) {
        return;
    }
    const auto inside = ip_address::parse("2001:db8:ffff::1");
    const auto outside = ip_address::parse("2001:db9::1");
    CRPC_CHECK(inside.ok());
    CRPC_CHECK(outside.ok());
    if (inside.ok() && outside.ok()) {
        CRPC_CHECK(network.value().contains(inside.value()));
        CRPC_CHECK_FALSE(network.value().contains(outside.value()));
    }

    const auto loopback = ip_network::parse("::1/128");
    const auto one = ip_address::parse("::1");
    const auto two = ip_address::parse("::2");
    CRPC_CHECK(loopback.ok());
    CRPC_CHECK(one.ok());
    CRPC_CHECK(two.ok());
    if (loopback.ok() && one.ok() && two.ok()) {
        CRPC_CHECK(loopback.value().contains(one.value()));
        CRPC_CHECK_FALSE(loopback.value().contains(two.value()));
    }
}

CRPC_TEST_MAIN()