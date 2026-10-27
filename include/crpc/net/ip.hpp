// CRPC -- IP addresses and CIDR networks.
//
// Needed by the proxy-header resolver, because "is this hop a proxy I trust?" is
// a question about address ranges and nothing else. Kept separate from the HTTP
// layer so an ACL, a registry or a rate limiter can reuse it.
//
// The parser is strict on purpose. A lenient parser is a security bug here: the
// proxy chain is attacker-controlled, and accepting "10.0.0.1 " or "010.0.0.1"
// where a peer expects a canonical literal is how a fake hop is smuggled in.
#ifndef CRPC_NET_IP_HPP
#define CRPC_NET_IP_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace net {

// A 32-bit or 128-bit address. The default value is not "0.0.0.0": `valid()`
// distinguishes "parsed to zero" from "not parsed", so a caller cannot mistake
// an absent address for a wildcard.
class CRPC_API ip_address {
public:
    ip_address() noexcept = default;

    // Strict parse of a bare literal: dotted quad for IPv4, RFC 4291 text for
    // IPv6 (including `::` compression and an embedded IPv4 tail). Brackets and
    // ports are NOT accepted here; the proxy parser strips those first.
    [[nodiscard]] static result<ip_address> parse(std::string_view text);

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] bool is_v4() const noexcept { return valid_ && !v6_; }
    [[nodiscard]] bool is_v6() const noexcept { return valid_ && v6_; }

    // RFC 5952 text form: lowercase, the longest run of two or more zero groups
    // compressed, and no leading zeros. A v4 address prints as a dotted quad.
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] friend bool operator==(const ip_address& a, const ip_address& b) noexcept {
        return a.valid_ == b.valid_ && a.v6_ == b.v6_ && a.bytes_ == b.bytes_;
    }
    [[nodiscard]] friend bool operator!=(const ip_address& a, const ip_address& b) noexcept {
        return !(a == b);
    }

    // The raw bytes: 4 significant for v4, 16 for v6.
    [[nodiscard]] const std::array<std::uint8_t, 16>& bytes() const noexcept { return bytes_; }

private:
    std::array<std::uint8_t, 16> bytes_{};
    bool v6_ = false;
    bool valid_ = false;

    friend class ip_network;
};

// `address/prefix`. A bare address is accepted and means a full-length prefix
// (/32 or /128). The address is masked to the prefix on construction, so two
// spellings of the same network compare equal and a containment test cannot be
// fooled by host bits that were left set.
class CRPC_API ip_network {
public:
    ip_network() noexcept = default;

    [[nodiscard]] static result<ip_network> parse(std::string_view text);
    [[nodiscard]] static ip_network from(ip_address address, std::uint8_t prefix);

    [[nodiscard]] bool valid() const noexcept { return address_.valid(); }
    [[nodiscard]] bool is_v4() const noexcept { return address_.is_v4(); }
    [[nodiscard]] bool contains(const ip_address& candidate) const noexcept;
    [[nodiscard]] const ip_address& address() const noexcept { return address_; }
    [[nodiscard]] std::uint8_t prefix() const noexcept { return prefix_; }
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] friend bool operator==(const ip_network& a, const ip_network& b) noexcept {
        return a.address_ == b.address_ && a.prefix_ == b.prefix_;
    }

private:
    ip_address address_;
    std::uint8_t prefix_ = 0;
};

} // namespace net
} // namespace crpc

#endif // CRPC_NET_IP_HPP