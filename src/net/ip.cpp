// CRPC -- IP address and CIDR parsing.
#include <crpc/net/ip.hpp>

#include <cstdio>

namespace crpc {
namespace net {
namespace {

[[nodiscard]] int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Dotted quad. Leading zeros are rejected: "010" is octal to some resolvers and
// decimal to others, and a peer that accepts it disagrees with one of them.
[[nodiscard]] bool parse_v4(std::string_view text, std::uint8_t* out) noexcept {
    std::size_t part = 0;
    std::size_t index = 0;
    while (part < 4) {
        std::size_t digits = 0;
        unsigned value = 0;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            if (digits == 3) {
                return false;
            }
            value = value * 10 + static_cast<unsigned>(text[index] - '0');
            ++digits;
            ++index;
        }
        if (digits == 0 || value > 255) {
            return false;
        }
        if (digits > 1 && text[index - digits] == '0') {
            return false;
        }
        out[part] = static_cast<std::uint8_t>(value);
        ++part;
        if (part == 4) {
            break;
        }
        if (index >= text.size() || text[index] != '.') {
            return false;
        }
        ++index;
    }
    return index == text.size();
}

// IPv6 per RFC 4291, with at most one `::`, and an optional dotted-quad tail.
[[nodiscard]] bool parse_v6(std::string_view text, std::uint8_t* out) noexcept {
    std::uint16_t groups[8] = {};
    std::size_t count = 0;
    std::ptrdiff_t compressed = -1;

    std::size_t index = 0;
    if (text.size() >= 2 && text[0] == ':' && text[1] == ':') {
        compressed = 0;
        index = 2;
        if (index == text.size()) {
            // "::" is all zeros.
            for (std::size_t i = 0; i < 16; ++i) {
                out[i] = 0;
            }
            return true;
        }
    } else if (text.empty() || text[0] == ':') {
        return false;
    }

    while (index < text.size()) {
        // An embedded IPv4 tail can only be the last element.
        const std::size_t tail = text.find(':', index);
        const std::string_view element =
            tail == std::string_view::npos ? text.substr(index) : text.substr(index, tail - index);
        if (element.empty()) {
            return false;
        }
        if (element.find('.') != std::string_view::npos) {
            if (count > 6) {
                return false;
            }
            std::uint8_t v4[4] = {};
            if (!parse_v4(element, v4)) {
                return false;
            }
            groups[count++] = static_cast<std::uint16_t>((v4[0] << 8) | v4[1]);
            groups[count++] = static_cast<std::uint16_t>((v4[2] << 8) | v4[3]);
            index = text.size();
            break;
        }
        if (element.size() > 4) {
            return false;
        }
        unsigned value = 0;
        for (const char c : element) {
            const int digit = hex_value(c);
            if (digit < 0) {
                return false;
            }
            value = value * 16 + static_cast<unsigned>(digit);
        }
        if (count == 8) {
            return false;
        }
        groups[count++] = static_cast<std::uint16_t>(value);

        if (tail == std::string_view::npos) {
            index = text.size();
            break;
        }
        index = tail + 1;
        if (index < text.size() && text[index] == ':') {
            // A second "::".
            if (compressed >= 0) {
                return false;
            }
            compressed = static_cast<std::ptrdiff_t>(count);
            ++index;
            if (index == text.size()) {
                break;
            }
        } else if (index == text.size()) {
            // A trailing single ':' is malformed.
            return false;
        }
    }

    if (compressed < 0) {
        if (count != 8) {
            return false;
        }
    } else if (count > 7) {
        // "::" must stand for at least one group.
        return false;
    }

    std::uint16_t final_groups[8] = {};
    if (compressed < 0) {
        for (std::size_t i = 0; i < 8; ++i) {
            final_groups[i] = groups[i];
        }
    } else {
        const std::size_t tail_count = count - static_cast<std::size_t>(compressed);
        for (std::size_t i = 0; i < static_cast<std::size_t>(compressed); ++i) {
            final_groups[i] = groups[i];
        }
        const std::size_t zeros = 8 - count;
        for (std::size_t i = 0; i < tail_count; ++i) {
            final_groups[compressed + zeros + i] =
                groups[static_cast<std::size_t>(compressed) + i];
        }
    }

    for (std::size_t i = 0; i < 8; ++i) {
        out[i * 2] = static_cast<std::uint8_t>(final_groups[i] >> 8);
        out[i * 2 + 1] = static_cast<std::uint8_t>(final_groups[i] & 0xffu);
    }
    return true;
}

void mask_bytes(std::uint8_t* bytes, std::size_t size, std::uint8_t prefix) noexcept {
    for (std::size_t i = 0; i < size; ++i) {
        const std::size_t bit = i * 8;
        if (bit >= prefix) {
            bytes[i] = 0;
        } else if (prefix - bit < 8) {
            const std::uint8_t keep =
                static_cast<std::uint8_t>(0xffu << (8 - (prefix - bit)));
            bytes[i] = static_cast<std::uint8_t>(bytes[i] & keep);
        }
    }
}

[[nodiscard]] bool prefix_matches(const std::uint8_t* lhs, const std::uint8_t* rhs,
                                  std::size_t size, std::uint8_t prefix) noexcept {
    std::size_t remaining = prefix;
    for (std::size_t i = 0; i < size && remaining > 0; ++i) {
        const std::uint8_t take = remaining >= 8 ? 8 : static_cast<std::uint8_t>(remaining);
        const std::uint8_t mask = static_cast<std::uint8_t>(0xffu << (8 - take));
        if ((lhs[i] & mask) != (rhs[i] & mask)) {
            return false;
        }
        remaining = remaining > take ? static_cast<std::uint8_t>(remaining - take) : 0;
    }
    return true;
}

} // namespace

// --- ip_address --------------------------------------------------------------

result<ip_address> ip_address::parse(std::string_view text) {
    ip_address address;
    if (text.empty()) {
        return rpc_error{rpc_code::invalid_argument, "empty IP literal"};
    }
    // A ':' anywhere means IPv6, which also covers the embedded-v4 form.
    if (text.find(':') != std::string_view::npos) {
        if (!parse_v6(text, address.bytes_.data())) {
            return rpc_error{rpc_code::invalid_argument, "malformed IPv6 literal"};
        }
        address.v6_ = true;
    } else {
        if (!parse_v4(text, address.bytes_.data())) {
            return rpc_error{rpc_code::invalid_argument, "malformed IPv4 literal"};
        }
        address.v6_ = false;
    }
    address.valid_ = true;
    return address;
}

std::string ip_address::to_string() const {
    if (!valid_) {
        return {};
    }
    if (!v6_) {
        char text[16];
        std::snprintf(text, sizeof(text), "%u.%u.%u.%u", static_cast<unsigned>(bytes_[0]),
                      static_cast<unsigned>(bytes_[1]), static_cast<unsigned>(bytes_[2]),
                      static_cast<unsigned>(bytes_[3]));
        return text;
    }

    std::uint16_t groups[8] = {};
    for (std::size_t i = 0; i < 8; ++i) {
        groups[i] = static_cast<std::uint16_t>((bytes_[i * 2] << 8) | bytes_[i * 2 + 1]);
    }

    // RFC 5952: compress the longest run of two or more zero groups, the first
    // such run when there is a tie.
    std::size_t best_start = 8;
    std::size_t best_length = 0;
    for (std::size_t i = 0; i < 8;) {
        if (groups[i] != 0) {
            ++i;
            continue;
        }
        std::size_t length = 0;
        while (i + length < 8 && groups[i + length] == 0) {
            ++length;
        }
        if (length > best_length) {
            best_length = length;
            best_start = i;
        }
        i += length;
    }

    std::string out;
    char buffer[8];
    for (std::size_t i = 0; i < 8; ++i) {
        if (i == best_start && best_length >= 2) {
            out += "::";
            i += best_length - 1;
            continue;
        }
        if (!out.empty() && out.back() != ':') {
            out += ':';
        }
        std::snprintf(buffer, sizeof(buffer), "%x", static_cast<unsigned>(groups[i]));
        out += buffer;
    }
    if (out.empty()) {
        return "::";
    }
    return out;
}

// --- ip_network --------------------------------------------------------------

result<ip_network> ip_network::parse(std::string_view text) {
    const std::size_t slash = text.find('/');
    const std::string_view address_text =
        slash == std::string_view::npos ? text : text.substr(0, slash);

    result<ip_address> address = ip_address::parse(address_text);
    if (!address) {
        return address.error();
    }

    std::uint8_t prefix = address.value().is_v4() ? 32 : 128;
    if (slash != std::string_view::npos) {
        const std::string_view prefix_text = text.substr(slash + 1);
        if (prefix_text.empty() || prefix_text.size() > 3) {
            return rpc_error{rpc_code::invalid_argument, "malformed CIDR prefix"};
        }
        unsigned value = 0;
        for (const char c : prefix_text) {
            if (c < '0' || c > '9') {
                return rpc_error{rpc_code::invalid_argument, "malformed CIDR prefix"};
            }
            value = value * 10 + static_cast<unsigned>(c - '0');
        }
        const unsigned limit = address.value().is_v4() ? 32u : 128u;
        if (value > limit) {
            return rpc_error{rpc_code::invalid_argument, "CIDR prefix out of range"};
        }
        prefix = static_cast<std::uint8_t>(value);
    }

    return from(std::move(address.value()), prefix);
}

ip_network ip_network::from(ip_address address, std::uint8_t prefix) {
    ip_network network;
    if (!address.valid()) {
        return network;
    }
    const std::uint8_t limit = address.is_v4() ? 32 : 128;
    network.address_ = std::move(address);
    network.prefix_ = prefix > limit ? limit : prefix;
    // Masking here means `contains` never has to, and two spellings of one
    // network compare equal.
    mask_bytes(network.address_.bytes_.data(), network.address_.is_v4() ? 4 : 16,
               network.prefix_);
    return network;
}

bool ip_network::contains(const ip_address& candidate) const noexcept {
    if (!address_.valid() || !candidate.valid()) {
        return false;
    }
    // A v4 network never contains a v6 address, even a v4-mapped one: treating
    // them as equal is how an attacker reaches a range that was meant to be
    // v4-only.
    if (candidate.is_v4() != address_.is_v4()) {
        return false;
    }
    return prefix_matches(address_.bytes_.data(), candidate.bytes().data(),
                          address_.is_v4() ? 4 : 16, prefix_);
}

std::string ip_network::to_string() const {
    if (!address_.valid()) {
        return {};
    }
    return address_.to_string() + "/" + std::to_string(prefix_);
}

} // namespace net
} // namespace crpc