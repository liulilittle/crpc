// CRPC -- LZ77 block codec (CRPC-specific container).
//
// A self-contained LZ77 stream: a fixed 20-byte header carrying the magic, the
// uncompressed size, the compressed size and a CRC32, followed by a token
// stream of literal runs and (length, distance) matches. The format is defined
// by this file and is not interoperable with any external tool; the checksum
// and the declared sizes are what make it safe to decode.
//
// Token encoding:
//   0x00            end of stream
//   0x01 .. 0x7F    literal run of that many bytes (1..127), bytes follow
//   0x80 .. 0xFF    match: length = (tag & 0x7F) + 3 (3..130), then a u16 LE
//                   distance (1..32768)
//
// A match longer than 130 bytes is split into several tokens, so the length
// field never overflows. The window is 32768 bytes.
#include <algorithm>
#include <cstring>
#include <vector>

#include <crpc/endian.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

#include "crc32.hpp"

namespace crpc {
namespace detail {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

constexpr u8 lz77_magic[4] = {'L', 'Z', '7', '7'};
constexpr u8 lz77_version = 1;
constexpr std::size_t lz77_header_size = 20;
constexpr std::size_t lz77_window = 32768;
constexpr std::size_t lz77_max_match = 130;
constexpr std::size_t lz77_min_match = 3;
constexpr std::size_t lz77_max_chain = 128;
constexpr std::size_t lz77_hash_bits = 15;
constexpr std::size_t lz77_hash_size = 1 << lz77_hash_bits;
constexpr u32 lz77_hash_multiplier = 2654435761u;

[[nodiscard]] std::size_t lz77_stream_bound_impl(std::size_t plain_size) noexcept {
    // Worst case is an all-literal stream: 127 literals cost one tag byte plus
    // 127 literal bytes, so ~1.008x, plus the header.
    return lz77_header_size + plain_size + plain_size / 64 + 64;
}

[[nodiscard]] u32 hash4(span<const std::byte> in, std::size_t position) noexcept {
    const u32 value = static_cast<u32>(static_cast<u8>(in[position])) |
                      (static_cast<u32>(static_cast<u8>(in[position + 1])) << 8) |
                      (static_cast<u32>(static_cast<u8>(in[position + 2])) << 16) |
                      (static_cast<u32>(static_cast<u8>(in[position + 3])) << 24);
    return (value * lz77_hash_multiplier) >> (32 - lz77_hash_bits);
}

[[nodiscard]] bool emit_literals(span<std::byte> out, std::size_t& position,
                                 span<const std::byte> in, std::size_t from,
                                 std::size_t to) noexcept {
    while (from < to) {
        const std::size_t run = std::min<std::size_t>(to - from, 127);
        if (position >= out.size()) {
            return false;
        }
        out[position++] = static_cast<std::byte>(static_cast<u8>(run));
        for (std::size_t k = 0; k < run; ++k) {
            if (position >= out.size()) {
                return false;
            }
            out[position++] = in[from + k];
        }
        from += run;
    }
    return true;
}

[[nodiscard]] bool emit_match(span<std::byte> out, std::size_t& position, std::size_t length,
                              std::size_t distance) noexcept {
    if (length < lz77_min_match || length > lz77_max_match || distance == 0 ||
        distance > lz77_window) {
        return false;
    }
    if (position + 3 > out.size()) {
        return false;
    }
    out[position++] = static_cast<std::byte>(static_cast<u8>(0x80u | ((length - 3) & 0x7Fu)));
    out[position++] = static_cast<std::byte>(static_cast<u8>(distance & 0xFFu));
    out[position++] = static_cast<std::byte>(static_cast<u8>((distance >> 8) & 0xFFu));
    return true;
}

} // namespace

std::size_t lz77_stream_bound(std::size_t plain_size) noexcept {
    return lz77_stream_bound_impl(plain_size);
}

status lz77_compress(span<const std::byte> in, span<std::byte> out,
                     std::size_t& written) noexcept {
    written = 0;
    if (in.empty()) {
        return rpc_code::invalid_argument;
    }
    if (out.size() < lz77_header_size) {
        return rpc_code::encode_error;
    }

    auto* bytes = reinterpret_cast<u8*>(out.data());
    std::memcpy(bytes, lz77_magic, 4);
    bytes[4] = lz77_version;
    bytes[5] = 0;
    endian::store_le16(bytes + 6, 0);
    endian::store_le32(bytes + 8, static_cast<u32>(in.size() & 0xFFFFFFFFu));
    endian::store_le32(bytes + 12, 0);
    endian::store_le32(bytes + 16, crc32_of(in));

    const std::size_t size = in.size();
    std::vector<int> head(lz77_hash_size, -1);
    std::vector<int> previous(lz77_window, -1);

    std::size_t position = lz77_header_size;
    std::size_t literal_start = 0;
    std::size_t index = 0;
    while (index < size) {
        std::size_t best_length = 0;
        std::size_t best_distance = 0;
        if (index + lz77_min_match <= size) {
            const u32 hash = hash4(in, index);
            int candidate = head[hash];
            previous[index & (lz77_window - 1)] = candidate;
            head[hash] = static_cast<int>(index);
            const std::size_t limit =
                std::min<std::size_t>(lz77_max_match, size - index);
            int chain = 0;
            while (candidate >= 0 && chain < static_cast<int>(lz77_max_chain)) {
                const std::size_t distance = index - static_cast<std::size_t>(candidate);
                if (distance > lz77_window) {
                    break;
                }
                const std::size_t candidate_index = static_cast<std::size_t>(candidate);
                std::size_t length = 0;
                while (length < limit && in[candidate_index + length] == in[index + length]) {
                    ++length;
                }
                if (length > best_length) {
                    best_length = length;
                    best_distance = distance;
                    if (length >= limit) {
                        break;
                    }
                }
                candidate = previous[candidate_index & (lz77_window - 1)];
                ++chain;
            }
        }

        if (best_length >= lz77_min_match) {
            if (!emit_literals(out, position, in, literal_start, index)) {
                return rpc_code::encode_error;
            }
            std::size_t remaining = best_length;
            while (remaining > 0) {
                const std::size_t chunk = std::min<std::size_t>(remaining, lz77_max_match);
                if (!emit_match(out, position, chunk, best_distance)) {
                    return rpc_code::encode_error;
                }
                remaining -= chunk;
            }
            const std::size_t end = index + best_length;
            ++index;
            while (index < end) {
                if (index + lz77_min_match <= size) {
                    const u32 hash = hash4(in, index);
                    previous[index & (lz77_window - 1)] = head[hash];
                    head[hash] = static_cast<int>(index);
                }
                ++index;
            }
            literal_start = index;
        } else {
            ++index;
        }
    }

    if (!emit_literals(out, position, in, literal_start, size)) {
        return rpc_code::encode_error;
    }
    if (position >= out.size()) {
        return rpc_code::encode_error;
    }
    out[position++] = std::byte{0x00};

    endian::store_le32(bytes + 12, static_cast<u32>(position - lz77_header_size));
    written = position;
    return status();
}

status lz77_decompress(span<const std::byte> in, std::size_t expected_size,
                       span<std::byte> out, std::size_t& written) noexcept {
    written = 0;
    if (expected_size == 0 || expected_size > out.size()) {
        return rpc_code::protocol_error;
    }
    if (in.size() < lz77_header_size) {
        return rpc_code::protocol_error;
    }
    const auto* bytes = reinterpret_cast<const u8*>(in.data());
    if (std::memcmp(bytes, lz77_magic, 4) != 0 || bytes[4] != lz77_version) {
        return rpc_code::protocol_error;
    }
    const u32 uncompressed = endian::load_le32(bytes + 8);
    const u32 compressed = endian::load_le32(bytes + 12);
    const u32 stored_crc = endian::load_le32(bytes + 16);
    if (uncompressed != static_cast<u32>(expected_size & 0xFFFFFFFFu)) {
        return rpc_code::protocol_error;
    }
    if (lz77_header_size + static_cast<std::size_t>(compressed) != in.size()) {
        return rpc_code::protocol_error;
    }

    const std::size_t end = in.size();
    std::size_t position = lz77_header_size;
    std::size_t produced = 0;
    for (;;) {
        if (position >= end) {
            return rpc_code::protocol_error;
        }
        const u8 tag = bytes[position++];
        if (tag == 0) {
            break;
        }
        if (tag < 0x80) {
            const std::size_t run = tag;
            if (position + run > end || produced + run > expected_size) {
                return rpc_code::protocol_error;
            }
            std::memcpy(out.data() + produced, bytes + position, run);
            position += run;
            produced += run;
        } else {
            const std::size_t length = static_cast<std::size_t>(tag & 0x7Fu) + 3;
            if (position + 2 > end) {
                return rpc_code::protocol_error;
            }
            const std::size_t distance = static_cast<std::size_t>(bytes[position]) |
                                        (static_cast<std::size_t>(bytes[position + 1]) << 8);
            position += 2;
            if (distance == 0 || distance > produced || produced + length > expected_size) {
                return rpc_code::protocol_error;
            }
            const std::byte* source = out.data() + (produced - distance);
            for (std::size_t k = 0; k < length; ++k) {
                out[produced + k] = source[k];
            }
            produced += length;
        }
    }

    if (position != end || produced != expected_size) {
        return rpc_code::protocol_error;
    }
    if (stored_crc != crc32_of(out.first(expected_size))) {
        return rpc_code::protocol_error;
    }
    written = produced;
    return status();
}

} // namespace detail
} // namespace crpc
