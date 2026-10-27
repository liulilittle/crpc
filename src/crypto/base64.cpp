// CRPC -- base64 implementation.
#include <crpc/crypto/base64.hpp>

#include <cstdint>

namespace crpc {
namespace crypto {
namespace {

constexpr char alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] int decode_char(char c) noexcept {
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

} // namespace

std::string base64_encode(span<const std::byte> data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
    std::size_t i = 0;
    while (i + 3 <= data.size()) {
        const std::uint32_t block = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                                    (static_cast<std::uint32_t>(bytes[i + 1]) << 8) |
                                    static_cast<std::uint32_t>(bytes[i + 2]);
        out.push_back(alphabet[(block >> 18) & 0x3Fu]);
        out.push_back(alphabet[(block >> 12) & 0x3Fu]);
        out.push_back(alphabet[(block >> 6) & 0x3Fu]);
        out.push_back(alphabet[block & 0x3Fu]);
        i += 3;
    }
    const std::size_t remaining = data.size() - i;
    if (remaining == 1) {
        const std::uint32_t block = static_cast<std::uint32_t>(bytes[i]) << 16;
        out.push_back(alphabet[(block >> 18) & 0x3Fu]);
        out.push_back(alphabet[(block >> 12) & 0x3Fu]);
        out.append("==", 2);
    } else if (remaining == 2) {
        const std::uint32_t block = (static_cast<std::uint32_t>(bytes[i]) << 16) |
                                    (static_cast<std::uint32_t>(bytes[i + 1]) << 8);
        out.push_back(alphabet[(block >> 18) & 0x3Fu]);
        out.push_back(alphabet[(block >> 12) & 0x3Fu]);
        out.push_back(alphabet[(block >> 6) & 0x3Fu]);
        out.push_back('=');
    }
    return out;
}

std::string base64_encode(std::string_view text) {
    return base64_encode(span<const std::byte>(
        reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

result<std::string> base64_decode(std::string_view text) {
    std::string out;
    out.reserve((text.size() / 4) * 3);
    std::uint32_t accumulator = 0;
    int bits = 0;
    std::size_t padding = 0;
    for (const char c : text) {
        // Whitespace is skipped so PEM blocks and line-wrapped SASL payloads decode.
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') {
            continue;
        }
        if (c == '=') {
            ++padding;
            continue;
        }
        // Padding may only come last; data after it means the text is malformed.
        if (padding != 0) {
            return rpc_error{rpc_code::decode_error, "base64 has data after its padding"};
        }
        const int value = decode_char(c);
        if (value < 0) {
            return rpc_error{rpc_code::decode_error, "base64 has a character outside its alphabet"};
        }
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((accumulator >> bits) & 0xFFu));
        }
    }
    if (padding > 2) {
        return rpc_error{rpc_code::decode_error, "base64 has more than two padding characters"};
    }
    // A leftover of six bits or more means the input was not a whole number of bytes,
    // which no valid encoding produces.
    if (bits >= 6) {
        return rpc_error{rpc_code::decode_error, "base64 length is not valid"};
    }
    return out;
}

} // namespace crypto
} // namespace crpc