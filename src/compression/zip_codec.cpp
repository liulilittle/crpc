// CRPC -- ZIP local file entry codec (PKZIP APPNOTE 4.4 / 6.3.10.1).
//
// One local file header (PK\x03\x04), one raw DEFLATE stream, and the CRC32 and
// sizes stored in that header (general-purpose flag bit 3 clear, so no data
// descriptor follows). The DEFLATE core is shared with gzip and deflate.
//
// The decoder checks the signature, the compression method, the declared
// compressed size and the CRC32, and rejects trailing bytes: the frame
// container carries exactly one entry.
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

constexpr u32 zip_local_signature = 0x04034B50u;
constexpr u16 zip_method_deflate = 8;
constexpr u16 zip_version_needed = 20;

constexpr std::size_t zip_local_header_size = 30;

[[nodiscard]] std::size_t zip_member_bound(std::size_t plain_size) noexcept {
    return zip_local_header_size + deflate_stream_bound(plain_size);
}

} // namespace

std::size_t zip_stream_bound(std::size_t plain_size) noexcept {
    return zip_member_bound(plain_size);
}

status zip_compress(span<const std::byte> in, span<std::byte> out,
                    std::size_t& written) noexcept {
    written = 0;
    if (in.empty()) {
        return rpc_code::invalid_argument;
    }
    if (out.size() < zip_local_header_size) {
        return rpc_code::encode_error;
    }

    endian::store_le32(out.data(), zip_local_signature);
    endian::store_le16(out.data() + 4, zip_version_needed);
    endian::store_le16(out.data() + 6, 0); // flags: no data descriptor
    endian::store_le16(out.data() + 8, zip_method_deflate);
    endian::store_le16(out.data() + 10, 0); // modification time
    endian::store_le16(out.data() + 12, 0); // modification date
    endian::store_le16(out.data() + 26, 0); // file name length
    endian::store_le16(out.data() + 28, 0); // extra field length

    std::size_t deflate_size = 0;
    const status deflate_status =
        deflate_compress(in, out.subspan(zip_local_header_size), deflate_size);
    if (!deflate_status.ok()) {
        return deflate_status;
    }

    const std::size_t data_offset = zip_local_header_size + deflate_size;
    if (data_offset > out.size()) {
        return rpc_code::encode_error;
    }
    // The CRC32 and the sizes are written after the payload is known; the
    // reserved header is patched in place.
    endian::store_le32(out.data() + 14, crc32_of(in));
    endian::store_le32(out.data() + 18, static_cast<u32>(deflate_size));
    endian::store_le32(out.data() + 22, static_cast<u32>(in.size() & 0xFFFFFFFFu));

    written = data_offset;
    return status();
}

status zip_decompress(span<const std::byte> in, std::size_t expected_size,
                      span<std::byte> out, std::size_t& written) noexcept {
    written = 0;
    if (expected_size == 0 || expected_size > out.size()) {
        return rpc_code::protocol_error;
    }
    if (in.size() < zip_local_header_size) {
        return rpc_code::protocol_error;
    }

    const auto* bytes = reinterpret_cast<const u8*>(in.data());
    if (endian::load_le32(bytes) != zip_local_signature) {
        return rpc_code::protocol_error;
    }
    if (endian::load_le16(bytes + 8) != zip_method_deflate) {
        return rpc_code::protocol_error;
    }
    const u16 flags = endian::load_le16(bytes + 6);
    const u16 name_length = endian::load_le16(bytes + 26);
    const u16 extra_length = endian::load_le16(bytes + 28);
    const u32 declared_crc = endian::load_le32(bytes + 14);
    const u32 declared_compressed = endian::load_le32(bytes + 18);
    const u32 declared_uncompressed = endian::load_le32(bytes + 22);

    const std::size_t data_offset = zip_local_header_size +
                                    static_cast<std::size_t>(name_length) +
                                    static_cast<std::size_t>(extra_length);
    if (data_offset > in.size() || data_offset + declared_compressed != in.size()) {
        return rpc_code::protocol_error;
    }
    if ((flags & 0x08u) != 0) {
        // A data descriptor would put the CRC and sizes after the payload; this
        // container writes them in the header and does not accept that variant.
        return rpc_code::protocol_error;
    }
    if (declared_uncompressed != static_cast<u32>(expected_size & 0xFFFFFFFFu)) {
        return rpc_code::protocol_error;
    }

    std::size_t consumed = 0;
    const status deflate_status = deflate_decompress(
        in.subspan(data_offset, declared_compressed), expected_size, out, written, &consumed);
    if (!deflate_status.ok()) {
        return deflate_status;
    }
    if (consumed != declared_compressed) {
        return rpc_code::protocol_error;
    }
    if (declared_crc != crc32_of(out.first(expected_size))) {
        return rpc_code::protocol_error;
    }
    return status();
}

} // namespace detail
} // namespace crpc
