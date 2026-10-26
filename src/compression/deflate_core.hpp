// CRPC -- DEFLATE core (RFC 1951).
//
// This is the shared codec behind three of the frame algorithms: `deflate`
// writes exactly the raw DEFLATE stream; `gzip` wraps it in the RFC 1952
// member header/trailer; `zip` wraps it in a ZIP local file header and a data
// descriptor. All three therefore agree on block layout and share the LZ77
// match finder, the bit I/O and the Huffman tables.
//
// The encoder emits *dynamic* Huffman blocks and falls back to *stored* blocks
// when that would be larger, so its worst case is the stored-block size and
// `deflate_stream_bound` can be a true upper bound. The decoder understands all
// three block types (stored, fixed, dynamic), which is what a peer-built
// gzip/zip writer will actually send.
#ifndef CRPC_COMPRESSION_DEFLATE_CORE_HPP
#define CRPC_COMPRESSION_DEFLATE_CORE_HPP

#include <cstddef>

#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace detail {

// Worst-case size in bytes of the raw DEFLATE stream produced from
// `plain_size` input bytes (the 4-byte frame prefix is not included).
[[nodiscard]] std::size_t deflate_stream_bound(std::size_t plain_size) noexcept;

// Compresses `in` as a raw DEFLATE stream into `out`. `out` must hold at least
// deflate_stream_bound(in.size()) bytes; a smaller buffer is
// rpc_code::encode_error. Empty input is rpc_code::protocol_error (the frame
// layer refuses it before the codec, but the codec keeps the same rule).
[[nodiscard]] status deflate_compress(span<const std::byte> in, span<std::byte> out,
                                       std::size_t& written) noexcept;

// Decompresses a raw DEFLATE stream. `expected_size` is the declared logical
// length and is the only write bound; `out` must be at least that large. A
// stream that produces a different number of bytes, overruns the input, or is
// structurally invalid is rpc_code::protocol_error.
//
// `consumed`, when not null, receives the number of input bytes the DEFLATE
// stream occupies (rounded up to a byte). gzip and zip need it to locate their
// trailer; the frame path leaves it null.
[[nodiscard]] status deflate_decompress(span<const std::byte> in, std::size_t expected_size,
                                         span<std::byte> out, std::size_t& written,
                                         std::size_t* consumed = nullptr) noexcept;

} // namespace detail
} // namespace crpc

#endif // CRPC_COMPRESSION_DEFLATE_CORE_HPP
