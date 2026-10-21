// CRPC -- frame-level compression (G29a: lz4 block).
//
// This header is the whole public surface of CRPC frame compression. The frame
// header (include/crpc/frame.hpp) stays compression-agnostic on purpose: it
// declares the flags and the algorithm ids, but it never includes this header.
// The implementation dependency is one-way:
//
//   src/frame/frame.cpp (implementation only) -> frame_compression.hpp
//   src/compression/*.cpp                     -> frame_compression.hpp
//
// Wire format of a compressed frame (see docs/protocol.md section 24):
//
//   fixed header (16) [ordered extension (16)] payload
//   payload := uncompressed_len u32 LE | codec_stream
//   payload_len = 4 + len(codec_stream)          <= max_frame
//   uncompressed_len in [1, max_frame]           logical payload size
//   CRC32C, when flagged, covers the on-the-wire payload (prefix + stream)
//
// A decoder verifies CRC32C before spending CPU on decompression, decompresses
// into a caller-supplied buffer, and normalises the header it returns:
// COMPRESSED and ALGO are cleared and payload_len becomes the logical length,
// so a returned frame always describes the payload it carries.
//
// Algorithms:
//   none  -- identity, never flagged COMPRESSED
//   lz4   -- LZ4 *block* format (no frame magic, no frame checksums)
//   zstd  -- reserved, always fail-closed (protocol_error)
//   gzip  -- reserved for G29b, fail-closed in this slice
//
// Dynamic configuration: `compression_limits` is a plain value type passed by
// the caller to every entry point. It carries the logical payload ceiling, the
// compression-ratio guard and the minimum payload worth compressing, so the
// policy is a runtime decision rather than a compile-time constant. The
// incremental decoder (frame_decoder) cannot carry limits without an ABI
// break, so its logical ceiling is the size of the caller-supplied payload
// buffer and its ratio guard uses the default limit; that is documented on
// frame_decoder::next.
#ifndef CRPC_FRAME_COMPRESSION_HPP
#define CRPC_FRAME_COMPRESSION_HPP

#include <cstddef>
#include <cstdint>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/frame.hpp>
#include <crpc/span.hpp>

namespace crpc {

// Size of the logical-length prefix at the start of a compressed payload.
inline constexpr std::size_t compressed_payload_prefix_size = 4;

// Smallest legal on-the-wire length of a compressed payload: the prefix plus at
// least one codec byte. A shorter COMPRESSED payload is protocol_error.
inline constexpr std::size_t min_compressed_payload_size = compressed_payload_prefix_size + 1;

// Default ceiling on the decompressed/compressed byte ratio. The LZ4 block
// format tops out around 250:1, so 300 admits every legal LZ4 stream while
// still bounding the CPU an attacker can buy with a tiny frame. 0 disables the
// check.
//
// This guard is not a memory bound and must not be read as one. The memory
// bound is the declared logical length: `uncompressed_len <= payload_out.size()`
// is checked before decompression, and the codec is handed
// `payload_out.first(uncompressed_len)`, so it can never write past the declared
// length no matter how small the codec stream is. For LZ4 the ratio guard can
// only reject CPU amplification, and even that is bounded by the declared
// length: a 4 MiB declaration with a 4 KiB stream is refused at 300:1, but a
// 100-byte declaration carrying a 4 MiB stream is refused earlier, by the length
// check inside the codec.
inline constexpr std::size_t default_max_compression_ratio = 300;

// Default threshold below which a payload is not worth compressing (LZ4
// inflates small inputs).
inline constexpr std::size_t default_min_compression_payload = 512;

// True for the frame types whose payload is application data and may therefore
// be compressed. Control frames carry fixed-size fields where compression can
// only inflate, and PUSH_ACK in particular is size-checked by the server, so a
// COMPRESSED control frame is a protocol error. This rule is deliberately not
// part of validate_header: decode_frame_header must keep accepting such a
// header so the whole-frame decoders can decide.
constexpr bool frame_type_carries_compression(frame_type type) noexcept {
    switch (type) {
    case frame_type::request:
    case frame_type::response:
    case frame_type::data:
    case frame_type::one_way:
    case frame_type::push:
        return true;
    // Every control frame is listed rather than defaulted: a future frame type
    // must be classified here, and -Werror=switch reports it if it is not.
    case frame_type::rst_stream:
    case frame_type::settings:
    case frame_type::settings_ack:
    case frame_type::ping:
    case frame_type::pong:
    case frame_type::goaway:
    case frame_type::window_update:
    case frame_type::push_ack:
        return false;
    }
    return false;
}

// Policy for one encode or decode call. All fields are runtime values.
struct compression_limits {
    // Logical payload ceiling. Must not exceed the max_frame used for the same
    // call; the effective ceiling is the smaller of the two.
    std::size_t max_payload = max_frame_default;
    // Decompressed/compressed byte ratio ceiling; 0 disables the check.
    std::size_t max_compression_ratio = default_max_compression_ratio;
    // Payloads smaller than this are sent uncompressed.
    std::size_t min_payload = default_min_compression_payload;
};

// Counters a connection layer can accumulate. The frame-level entry points
// update the fields they can observe; `decompress_payload` only reports
// decode_failures (it sees no frame).
struct compression_statistics {
    std::uint64_t frames_compressed = 0;
    std::uint64_t frames_decompressed = 0;
    std::uint64_t bytes_plain = 0;
    std::uint64_t bytes_wire = 0;
    std::uint64_t fallbacks_not_smaller = 0;
    std::uint64_t skipped_below_min = 0;
    std::uint64_t rejected_size = 0;
    std::uint64_t rejected_ratio = 0;
    std::uint64_t rejected_algorithm = 0;
    std::uint64_t decode_failures = 0;
    // ONE_WAY frames whose compressed payload was malformed and explicitly
    // dropped. Other frame types never increment this: for them the same
    // failure is fatal.
    std::uint64_t frames_dropped_unreliable = 0;
};

// Per-frame report filled by frame_decoder::next(frame&, frame_compression_info*).
//
// It exists because frame_decoder cannot grow a member without an ABI break
// (docs/abi.md), yet a connection layer needs to know what came off the wire:
// which algorithm a compressed frame used, how many bytes it occupied there and
// how large its logical payload is. The struct is declared in <crpc/frame.hpp>
// and defined here, so the frame header stays compression-agnostic.
//
// `dropped` is true only for a ONE_WAY frame whose compressed payload was
// malformed (or whose CRC32C did not match): such a frame is explicitly
// discarded, the decoder keeps reading, and `drop_reason` names the failure. In
// that case the decoder's error() stays rpc_code::ok on purpose - the error
// channel is latched, so recording a drop there would make every later frame
// unreadable. On every other frame type the same failure is fatal.
struct frame_compression_info {
    // Algorithm carried on the wire (none for an uncompressed frame).
    compression_algorithm algorithm = compression_algorithm::none;
    // payload_len as received (on-the-wire bytes).
    std::uint32_t wire_size = 0;
    // Logical payload length; equal to wire_size for an uncompressed frame.
    std::uint32_t logical_size = 0;
    bool dropped = false;
    // rpc_code::ok unless dropped; then rpc_code::decode_error.
    rpc_code drop_reason = rpc_code::ok;
};

// none and lz4 are usable in this slice. zstd and gzip are recognised ids that
// fail closed (gzip is opened in G29b when the DEFLATE core is shared).
[[nodiscard]] CRPC_API bool is_supported_compression(compression_algorithm algorithm) noexcept;

// Upper bound on the encoded payload for `plain_size` input bytes, including
// the 4-byte prefix. It is the size to reserve before calling
// encode_frame_compressed. For algorithms that cannot be used, or for the
// identity fallback, the bound is plain_size (an uncompressed payload has no
// prefix).
[[nodiscard]] CRPC_API std::size_t compressed_payload_bound(
    compression_algorithm algorithm, std::size_t plain_size) noexcept;

// Compresses a bare codec stream (no prefix, no frame header) into `out`.
// `out` must hold at least compressed_payload_bound(algorithm, in.size())
// bytes. Failure modes:
//   * invalid_argument -- algorithm unsupported, empty input, or input above
//     limits.max_payload;
//   * encode_error -- `out` is too small.
// `written` is zero on failure.
[[nodiscard]] CRPC_API status compress_payload(
    compression_algorithm algorithm, span<const std::byte> in, span<std::byte> out,
    std::size_t& written, const compression_limits& limits = {}) noexcept;

// Decompresses a bare codec stream into `out`.
//
// `expected_size` is the declared logical length and is required by the LZ4
// block format, which carries no length of its own. It is the *only* write
// bound: `out` is normally `payload_out.first(uncompressed_len)`, so
// `out.size() == expected_size`, and the codec stops at `expected_size` even if
// the caller passes a larger buffer. Every literal and every match is checked
// against the remaining declared length before it is written, and the stream
// must produce exactly `expected_size` bytes - one byte more or less is a
// protocol error. A caller that passes the full 16 MiB output buffer with a
// 100-byte declaration still cannot be made to write 4 MiB.
//
// Failure modes:
//   * protocol_error -- algorithm unsupported or the stream is malformed,
//     truncated, uses a zero/out-of-range offset, overruns `expected_size`, or
//     decodes to a different length;
//   * protocol_error -- expected_size is zero or above limits.max_payload;
//   * resource_exhausted -- expected_size exceeds `out.size()`, or the
//     compressed/plain ratio exceeds limits.max_compression_ratio (a CPU guard,
//     not the memory bound; see default_max_compression_ratio).
// A decode failure increments statistics::decode_failures when `stats` is set.
[[nodiscard]] CRPC_API status decompress_payload(
    compression_algorithm algorithm, span<const std::byte> in, std::size_t expected_size,
    span<std::byte> out, std::size_t& written, const compression_limits& limits = {},
    compression_statistics* stats = nullptr) noexcept;

// Worst-case encoded frame size for `value` under `preferred`: header, ordered
// extension and compressed_payload_bound(payload.size()). Reserve this before
// calling encode_frame_compressed; it covers the identity fallback too.
[[nodiscard]] CRPC_API std::size_t encoded_frame_size_compressed(
    const frame& value, compression_algorithm preferred,
    const compression_limits& limits = {}) noexcept;

// Encodes a frame, compressing the payload when that is a net win.
//
// value.header.flags must not carry COMPRESSED or ALGO (invalid_argument); a
// COMPRESSED frame is produced only here. `preferred` is the runtime algorithm
// choice: `none` means "send this frame uncompressed", `lz4` asks for LZ4, and
// any unsupported value (zstd, gzip in this build, reserved) is
// invalid_argument - never a silent fallback to a different codec, because a
// caller that configured gzip and got lz4 would be back to "the flag does not
// describe the payload".
//
// With a usable algorithm, compression is used only when the payload is at
// least limits.min_payload bytes, the type carries application data, and the
// encoded payload (including the 4-byte prefix) is strictly smaller. Otherwise
// the call is exactly encode_frame and `chosen` reports none: a sender never
// inflates a frame.
//
// Header validation is the same as encode_frame's, on both branches: known type,
// stream scope (stream frames need a non-zero stream_id, connection frames a
// zero one, WINDOW_UPDATE either), stream_id bit 31 clear, ONE_WAY requires
// UNRELIABLE, a non-zero ext requires CRC32C, and payload_len <= max_frame. A
// violation is invalid_argument with nothing written - this entry point must
// never emit a frame its own decode_frame_header would refuse. On the
// compressed branch the caller's `ext` is validated before it is replaced by
// the payload checksum; it is not silently normalised to zero.
//
// `chosen` (optional) receives the algorithm actually used. `stats` (optional)
// receives the counters. On failure `written` is zero. `max_frame` bounds the
// logical payload exactly as it does for encode_frame.
[[nodiscard]] CRPC_API status encode_frame_compressed(
    span<std::byte> out, const frame& value, compression_algorithm preferred,
    const compression_limits& limits, std::size_t& written,
    compression_algorithm* chosen = nullptr, compression_statistics* stats = nullptr,
    u32 max_frame = max_frame_default) noexcept;

// Decodes exactly one frame, decompressing it into `payload_out`.
//
// Uncompressed frames behave like decode_frame (payload borrows `in`). For a
// compressed frame the decoded payload lives in `payload_out`, the returned
// header is normalised (COMPRESSED/ALGO cleared, payload_len and CRC32C
// rewritten for the logical payload), and `wire_algorithm` receives the
// algorithm that was on the wire.
//
// Failure modes: protocol_error for an unsupported algorithm, a control frame
// type, a payload shorter than 5 bytes, an invalid logical length, malformed
// codec data, or a CRC32C mismatch; resource_exhausted when the logical length
// exceeds min(max_frame, limits.max_payload, payload_out.size()) or the ratio
// guard trips. On a ONE_WAY frame a frame-local failure (malformed stream,
// length mismatch, CRC32C) is an explicit drop: the call returns decode_error,
// `value` is unchanged, and statistics::frames_dropped_unreliable increments.
// Algorithm and limit failures stay fatal on every frame type.
[[nodiscard]] CRPC_API status decode_frame_compressed(
    span<const std::byte> in, frame& value, span<std::byte> payload_out, u32 max_frame,
    const compression_limits& limits = {}, compression_algorithm* wire_algorithm = nullptr,
    compression_statistics* stats = nullptr) noexcept;

} // namespace crpc

#endif // CRPC_FRAME_COMPRESSION_HPP
