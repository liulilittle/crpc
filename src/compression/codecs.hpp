// CRPC -- internal declarations of the frame payload codecs.
//
// The public surface is <crpc/frame_compression.hpp>; these functions are the
// bare codec streams (no 4-byte logical-length prefix, no frame header) that the
// frame container drives. Each pair takes caller-provided output spans and
// never allocates internally beyond its own scratch.
#ifndef CRPC_COMPRESSION_CODECS_HPP
#define CRPC_COMPRESSION_CODECS_HPP

#include <cstddef>

#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace detail {

// LZ4 block (lz4_block.cpp).
[[nodiscard]] status lz4_block_compress(span<const std::byte> in, span<std::byte> out,
                                        std::size_t& written) noexcept;
[[nodiscard]] status lz4_block_decompress(span<const std::byte> in, std::size_t expected_size,
                                          span<std::byte> out, std::size_t& written) noexcept;

// Raw DEFLATE (deflate_core.cpp); also the payload of gzip and zip.
[[nodiscard]] std::size_t deflate_stream_bound(std::size_t plain_size) noexcept;
[[nodiscard]] status deflate_compress(span<const std::byte> in, span<std::byte> out,
                                      std::size_t& written) noexcept;
[[nodiscard]] status deflate_decompress(span<const std::byte> in, std::size_t expected_size,
                                        span<std::byte> out, std::size_t& written,
                                        std::size_t* consumed = nullptr) noexcept;

// gzip member (RFC 1952).
[[nodiscard]] std::size_t gzip_stream_bound(std::size_t plain_size) noexcept;
[[nodiscard]] status gzip_compress(span<const std::byte> in, span<std::byte> out,
                                   std::size_t& written) noexcept;
[[nodiscard]] status gzip_decompress(span<const std::byte> in, std::size_t expected_size,
                                     span<std::byte> out, std::size_t& written) noexcept;

// ZIP local file entry.
[[nodiscard]] std::size_t zip_stream_bound(std::size_t plain_size) noexcept;
[[nodiscard]] status zip_compress(span<const std::byte> in, span<std::byte> out,
                                  std::size_t& written) noexcept;
[[nodiscard]] status zip_decompress(span<const std::byte> in, std::size_t expected_size,
                                    span<std::byte> out, std::size_t& written) noexcept;

// LZ77 block (CRPC-specific container).
[[nodiscard]] std::size_t lz77_stream_bound(std::size_t plain_size) noexcept;
[[nodiscard]] status lz77_compress(span<const std::byte> in, span<std::byte> out,
                                   std::size_t& written) noexcept;
[[nodiscard]] status lz77_decompress(span<const std::byte> in, std::size_t expected_size,
                                     span<std::byte> out, std::size_t& written) noexcept;

} // namespace detail
} // namespace crpc

#endif // CRPC_COMPRESSION_CODECS_HPP
