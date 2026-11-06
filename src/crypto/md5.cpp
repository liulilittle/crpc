// CRPC -- MD5 (RFC 1321).
//
// Straight transcription of the specification: four-word state, 64-byte blocks,
// little-endian message words, and the three-round padding rule from §3.2. The
// only literal tables are the digest's constants; the shifts are the ones named
// in §3.4.
#include <crpc/crypto/md5.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace crpc {
namespace crypto {
namespace {

struct md5_state {
    std::uint32_t a = 0x67452301u;
    std::uint32_t b = 0xefcdab89u;
    std::uint32_t c = 0x98badcfeu;
    std::uint32_t d = 0x10325476u;
    std::uint64_t total = 0;
    std::array<std::uint8_t, 64> block{};
    std::size_t buffered = 0;
};

// T[i] = floor(2^32 * abs(sin(i + 1))), RFC 1321 §3.4.
constexpr std::array<std::uint32_t, 64> k_table = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au,
    0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u,
    0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
    0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
    0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u,
    0xffeff47du, 0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u};

// Per-round left-rotation counts, RFC 1321 §3.4.
constexpr std::array<std::uint8_t, 64> s_table = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

[[nodiscard]] constexpr std::uint32_t rotl32(std::uint32_t value,
                                             std::uint32_t count) noexcept {
    return static_cast<std::uint32_t>((value << count) | (value >> (32u - count)));
}

void md5_block(md5_state& state, const std::uint8_t* block) noexcept {
    std::uint32_t words[16];
    for (std::size_t i = 0; i < 16; ++i) {
        words[i] = static_cast<std::uint32_t>(block[i * 4]) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 8) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 16) |
                   (static_cast<std::uint32_t>(block[i * 4 + 3]) << 24);
    }

    std::uint32_t a = state.a;
    std::uint32_t b = state.b;
    std::uint32_t c = state.c;
    std::uint32_t d = state.d;

    for (std::size_t i = 0; i < 64; ++i) {
        std::uint32_t f = 0;
        std::size_t g = 0;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        f = f + a + k_table[i] + words[g];
        a = d;
        d = c;
        c = b;
        b = b + rotl32(f, s_table[i]);
    }

    state.a += a;
    state.b += b;
    state.c += c;
    state.d += d;
}

void absorb(md5_state& state, const std::uint8_t* data, std::size_t size) noexcept {
    state.total += size;
    while (size > 0) {
        const std::size_t room = state.block.size() - state.buffered;
        const std::size_t take = size < room ? size : room;
        for (std::size_t i = 0; i < take; ++i) {
            state.block[state.buffered + i] = data[i];
        }
        state.buffered += take;
        data += take;
        size -= take;
        if (state.buffered == state.block.size()) {
            md5_block(state, state.block.data());
            state.buffered = 0;
        }
    }
}

[[nodiscard]] md5_digest finish(md5_state& state) noexcept {
    const std::uint64_t bit_length = state.total * 8u;

    // Padding: one 0x80 byte, then zeros until 8 bytes short of the block
    // boundary, then the 64-bit little-endian bit length (§3.2).
    const std::uint8_t pad_lead = 0x80u;
    absorb(state, &pad_lead, 1);
    state.total -= 1; // padding is not message length
    const std::uint8_t zero = 0;
    while (state.buffered != 56) {
        absorb(state, &zero, 1);
        state.total -= 1;
    }
    std::uint8_t length_bytes[8];
    for (std::size_t i = 0; i < 8; ++i) {
        length_bytes[i] = static_cast<std::uint8_t>((bit_length >> (8 * i)) & 0xffu);
    }
    absorb(state, length_bytes, 8);
    state.total -= 8;

    md5_digest digest{};
    const std::uint32_t words[4] = {state.a, state.b, state.c, state.d};
    for (std::size_t w = 0; w < 4; ++w) {
        for (std::size_t i = 0; i < 4; ++i) {
            digest[w * 4 + i] = static_cast<std::uint8_t>((words[w] >> (8 * i)) & 0xffu);
        }
    }
    return digest;
}

} // namespace

md5_digest md5(span<const std::byte> data) noexcept {
    md5_state state;
    absorb(state, reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
    return finish(state);
}

md5_digest md5(span<const std::byte> first, span<const std::byte> second) noexcept {
    md5_state state;
    absorb(state, reinterpret_cast<const std::uint8_t*>(first.data()), first.size());
    absorb(state, reinterpret_cast<const std::uint8_t*>(second.data()), second.size());
    return finish(state);
}

std::string to_hex(md5_digest digest) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(digest.size() * 2);
    for (const std::uint8_t value : digest) {
        out.push_back(digits[value >> 4]);
        out.push_back(digits[value & 0x0fu]);
    }
    return out;
}

} // namespace crypto
} // namespace crpc
