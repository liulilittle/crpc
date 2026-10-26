// CRPC -- CRC32 (IEEE 802.3, polynomial 0xEDB88320 reflected).
//
// This is the checksum the gzip container (RFC 1952) and the ZIP local header
// require. It is deliberately *not* crc32c: crc32c (the frame checksum,
// src/frame/frame.cpp) uses a different polynomial and must not be reused here,
// otherwise a gzip member would validate with a CRC the peer computed differently.
//
// The table is generated in a constexpr loop so the codec depends on no global
// initialiser and pays the first-call cost at compile time.
#ifndef CRPC_COMPRESSION_CRC32_HPP
#define CRPC_COMPRESSION_CRC32_HPP

#include <array>
#include <cstddef>
#include <cstdint>

#include <crpc/config.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace detail {

inline constexpr std::uint32_t crc32_reflected_polynomial = 0xEDB88320u;

struct crc32_table {
    std::array<std::uint32_t, 256> entries{};
};

[[nodiscard]] constexpr crc32_table make_crc32_table() noexcept {
    crc32_table table{};
    for (std::size_t i = 0; i < 256; ++i) {
        std::uint32_t value = static_cast<std::uint32_t>(i);
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1u) != 0 ? (crc32_reflected_polynomial ^ (value >> 1))
                                      : (value >> 1);
        }
        table.entries[i] = value;
    }
    return table;
}

inline constexpr crc32_table crc32_lookup = make_crc32_table();

// Updates a running CRC32 with `len` bytes. `crc` is the value returned by a
// previous call, or 0 for the first block.
[[nodiscard]] constexpr std::uint32_t crc32_update(std::uint32_t crc,
                                                    const std::uint8_t* data,
                                                    std::size_t len) noexcept {
    for (std::size_t i = 0; i < len; ++i) {
        const std::uint32_t index = (crc ^ data[i]) & 0xFFu;
        crc = crc32_lookup.entries[index] ^ (crc >> 8);
    }
    return crc;
}

// CRC32 of a whole buffer, the value written into a gzip trailer or a ZIP
// local header.
[[nodiscard]] constexpr std::uint32_t crc32_of(const std::uint8_t* data,
                                               std::size_t len) noexcept {
    return crc32_update(0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}

[[nodiscard]] constexpr std::uint32_t crc32_of(span<const std::byte> data) noexcept {
    return crc32_of(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

// crpc::span has no implicit qualification conversion, so a writable view is
// narrowed explicitly where a read-only span is needed.
[[nodiscard]] constexpr std::uint32_t crc32_of(span<std::byte> data) noexcept {
    return crc32_of(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

} // namespace detail
} // namespace crpc

#endif // CRPC_COMPRESSION_CRC32_HPP
