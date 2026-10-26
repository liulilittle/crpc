// CRPC -- gzip member codec (RFC 1952).
//
// A gzip member is a fixed 10-byte header, one raw DEFLATE stream, and an
// 8-byte trailer holding the CRC32 and the input size. The DEFLATE core is
// shared with the deflate and zip algorithms; this file only adds the container
// and its integrity checks.
//
// The decoder accepts the optional header fields (FEXTRA, FNAME, FCOMMENT and
// the header CRC16) so a member written by a third-party tool round-trips, and
// it verifies both the DEFLATE stream and the CRC32/ISIZE trailer. A member
// that does not end exactly where the trailer ends is rejected: the frame
// container carries exactly one member.
#include <cstring>

#include <crpc/endian.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

#include "crc32.hpp"
#include "deflate_core.hpp"

namespace crpc {
namespace detail {
namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

constexpr u8 gzip_magic_0 = 0x1F;
constexpr u8 gzip_magic_1 = 0x8B;
constexpr u8 gzip_cm_deflate = 8;

constexpr u8 gzip_flag_fextra = 0x04;
constexpr u8 gzip_flag_fname = 0x08;
constexpr u8 gzip_flag_fcomment = 0x10;
constexpr u8 gzip_flag_fhcrc = 0x02;

constexpr std::size_t gzip_header_size = 10;
constexpr std::size_t gzip_trailer_size = 8;

// Worst-case size of a gzip member for `plain_size` input bytes, excluding the
// 4-byte frame prefix: header + DEFLATE worst case + trailer.
[[nodiscard]] std::size_t gzip_member_bound(std::size_t plain_size) noexcept {
    return gzip_header_size + deflate_stream_bound(plain_size) + gzip_trailer_size;
}

} // namespace

std::size_t gzip_stream_bound(std::size_t plain_size) noexcept {
    return gzip_member_bound(plain_size);
}

status gzip_compress(span<const std::byte> in, span<std::byte> out,
                    std::size_t& written) noexcept {
    written = 0;
    if (in.empty()) {
        return rpc_code::invalid_argument;
    }
    if (out.size() < gzip_header_size) {
        return rpc_code::encode_error;
    }

    auto* bytes = reinterpret_cast<u8*>(out.data());
    bytes[0] = gzip_magic_0;
    bytes[1] = gzip_magic_1;
    bytes[2] = gzip_cm_deflate;
    bytes[3] = 0; // FLG: no optional fields
    // MTIME = 0 (no timestamp); a decoder must not depend on it.
    bytes[4] = 0;
    bytes[5] = 0;
    bytes[6] = 0;
    bytes[7] = 0;
    bytes[8] = 0;   // XFL
    bytes[9] = 255; // OS: unknown

    std::size_t deflate_size = 0;
    const status deflate_status =
        deflate_compress(in, out.subspan(gzip_header_size), deflate_size);
    if (!deflate_status.ok()) {
        return deflate_status;
    }

    const std::size_t trailer_offset = gzip_header_size + deflate_size;
    if (trailer_offset + gzip_trailer_size > out.size()) {
        return rpc_code::encode_error;
    }
    endian::store_le32(out.data() + trailer_offset, crc32_of(in));
    endian::store_le32(out.data() + trailer_offset + 4,
                       static_cast<u32>(in.size() & 0xFFFFFFFFu));

    written = trailer_offset + gzip_trailer_size;
    return status();
}

status gzip_decompress(span<const std::byte> in, std::size_t expected_size,
                      span<std::byte> out, std::size_t& written) noexcept {
    written = 0;
    if (expected_size == 0 || expected_size > out.size()) {
        return rpc_code::protocol_error;
    }
    if (in.size() < gzip_header_size + gzip_trailer_size) {
        return rpc_code::protocol_error;
    }

    const auto* bytes = reinterpret_cast<const u8*>(in.data());
    if (bytes[0] != gzip_magic_0 || bytes[1] != gzip_magic_1 ||
        bytes[2] != gzip_cm_deflate) {
        return rpc_code::protocol_error;
    }
    const u8 flags = bytes[3];

    std::size_t position = gzip_header_size;
    if ((flags & gzip_flag_fextra) != 0) {
        if (position + 2 > in.size()) {
            return rpc_code::protocol_error;
        }
        const u16 extra_length = static_cast<u16>(bytes[position]) |
                                 (static_cast<u16>(bytes[position + 1]) << 8);
        position += 2 + static_cast<std::size_t>(extra_length);
        if (position > in.size()) {
            return rpc_code::protocol_error;
        }
    }
    const auto skip_zero_terminated = [&](std::size_t& cursor) -> bool {
        while (cursor < in.size() && bytes[cursor] != 0) {
            ++cursor;
        }
        if (cursor >= in.size()) {
            return false;
        }
        ++cursor;
        return true;
    };
    if ((flags & gzip_flag_fname) != 0 && !skip_zero_terminated(position)) {
        return rpc_code::protocol_error;
    }
    if ((flags & gzip_flag_fcomment) != 0 && !skip_zero_terminated(position)) {
        return rpc_code::protocol_error;
    }
    if ((flags & gzip_flag_fhcrc) != 0) {
        if (position + 2 > in.size()) {
            return rpc_code::protocol_error;
        }
        const u16 stored = static_cast<u16>(bytes[position]) |
                           (static_cast<u16>(bytes[position + 1]) << 8);
        const u16 computed =
            static_cast<u16>(crc32_of(span<const std::byte>(in.data(), position)) & 0xFFFFu);
        if (stored != computed) {
            return rpc_code::protocol_error;
        }
        position += 2;
    }

    std::size_t consumed = 0;
    const status deflate_status =
        deflate_decompress(in.subspan(position), expected_size, out, written, &consumed);
    if (!deflate_status.ok()) {
        return deflate_status;
    }
    position += consumed;

    if (position + gzip_trailer_size != in.size()) {
        return rpc_code::protocol_error;
    }
    const u32 stored_crc = endian::load_le32(in.data() + position);
    const u32 stored_size = endian::load_le32(in.data() + position + 4);
    if (stored_crc != crc32_of(out.first(expected_size))) {
        return rpc_code::protocol_error;
    }
    if (stored_size != static_cast<u32>(expected_size & 0xFFFFFFFFu)) {
        return rpc_code::protocol_error;
    }
    return status();
}

} // namespace detail
} // namespace crpc
