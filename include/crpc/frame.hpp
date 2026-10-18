// CRPC -- frame codec.
//
// Wire layout (every multi-byte field is little-endian):
//
//   offset  size  field
//   0       4     payload_len   u32  bytes after the fixed header and extensions
//   4       1     type          u8   frame_type
//   5       1     flags         u8   frame_flag_*
//   6       2     reserved      u16  must be zero
//   8       4     stream_id     u32  bit 31 must be zero
//   12      4     ext           u32  CRC32C when frame_flag_crc32c, else zero
//
// When frame_flag_ordered is set a 16-byte ordered extension follows the fixed
// header: lane_id u64, lane_seq u64. payload_len does not include it.
//
// Call envelope (REQUEST and RESPONSE payloads):
//
//   service_id  u64
//   method_id   u64
//   call_flags  u8    bit0 has_extensions; bits 1-7 must be zero
//   codec       u16   codec_id
//   [extension block when call_flags bit0 is set]
//   body        remaining bytes
//
// Extension block:
//
//   ext_flags   u8    bit0 has_trace_id, bit1 has_span_id,
//                     bit2 has_parent_span_id, bit3 has_deadline,
//                     bit4 has_idempotency_key; bits 5-7 must be zero
//   trace_id          16 bytes  if bit0
//   span_id            8 bytes  if bit1
//   parent_span_id     8 bytes  if bit2
//   deadline_ns   u64           if bit3
//   key_len       u8            if bit4, then key_len bytes (<= 128)
//
// The frame flags have no spare bit (bits 0-4 are defined and bits 5-7 carry
// the compression algorithm), so the optional trace/idempotency block is an
// extension of the call envelope rather than of the fixed frame header. This
// keeps the 16-byte header exactly as specified in section 7.1 of the design.
//
// Fail-closed rules: an unknown frame type, a non-zero reserved field, a
// stream id with bit 31 set, a stream scope violation, an inconsistent
// compression flag, an unknown compression algorithm, a non-zero ext without
// CRC32C, a CRC mismatch, a length mismatch, or a payload longer than the
// caller-supplied maximum all produce rpc_code::protocol_error. No function
// allocates for a frame whose header has not passed validation.
#ifndef CRPC_FRAME_HPP
#define CRPC_FRAME_HPP

#include <array>
#include <cstddef>

#include <crpc/buffer.hpp>
#include <crpc/codec_id.hpp>
#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {

// --- Sizes and limits --------------------------------------------------------

inline constexpr std::size_t frame_header_size = 16;
inline constexpr std::size_t ordered_extension_size = 16;

// Default and absolute maximum payload length. A decoder never allocates for a
// payload before checking it against the maximum it was constructed with.
inline constexpr u32 max_frame_default = 16u * 1024u * 1024u;
inline constexpr u32 max_frame_hard_cap = 64u * 1024u * 1024u;

// stream_id bit 31 is reserved and must be zero.
inline constexpr u32 max_stream_id = 0x7FFFFFFFu;

// Version of the binary protocol. The frame header itself carries no version;
// version negotiation happens in the handshake (section 7.5), which compares
// this value and fails closed on a mismatch.
inline constexpr u16 protocol_version = 1;

// Longest idempotency key the call envelope can carry (u8 length prefix).
inline constexpr std::size_t max_idempotency_key = 128;

// --- Frame types -------------------------------------------------------------

// Numeric values are fixed by the specification; changing one is a wire break.
// The brief's CANCEL is RST_STREAM (0x04); its ACK is SETTINGS_ACK (0x06) or
// PUSH_ACK (0x0C) depending on what is being acknowledged.
enum class frame_type : u8 {
    data = 0x00,
    request = 0x01,
    response = 0x02,
    one_way = 0x03,
    rst_stream = 0x04,
    settings = 0x05,
    settings_ack = 0x06,
    ping = 0x07,
    pong = 0x08,
    goaway = 0x09,
    window_update = 0x0A,
    push = 0x0B,
    push_ack = 0x0C
};

// True when `type` is one of the values fixed by the specification.
[[nodiscard]] constexpr bool is_known_frame_type(frame_type type) noexcept {
    return static_cast<u8>(type) <= static_cast<u8>(frame_type::push_ack);
}

// --- Flags -------------------------------------------------------------------

inline constexpr u8 frame_flag_end_stream = 0x01;
inline constexpr u8 frame_flag_compressed = 0x02;
inline constexpr u8 frame_flag_crc32c = 0x04;
inline constexpr u8 frame_flag_ordered = 0x08;
inline constexpr u8 frame_flag_unreliable = 0x10;
inline constexpr u8 frame_flag_algo_mask = 0xE0;
inline constexpr u8 frame_flag_algo_shift = 5;

// Compression algorithms carried in flags bits 5-7. Values 4-7 are reserved.
enum class compression_algorithm : u8 {
    none = 0,
    lz4 = 1,
    zstd = 2,
    gzip = 3
};

// --- Header and frame --------------------------------------------------------

// The fixed 16-byte header. `reserved` is not represented: it is always written
// as zero and a non-zero value on the wire is rejected by decode_frame_header.
struct frame_header {
    u32 payload_len = 0;
    frame_type type = frame_type::data;
    u8 flags = 0;
    u32 stream_id = 0;
    // CRC32C of the payload when frame_flag_crc32c is set; otherwise zero.
    u32 ext = 0;
};

// Present immediately after the fixed header exactly when frame_flag_ordered is
// set. `lane_id` identifies the ordered lane and `lane_seq` is its monotonic
// sequence number.
struct ordered_extension {
    u64 lane_id = 0;
    u64 lane_seq = 0;
};

// A decoded frame. `payload` never owns its bytes: it points either into the
// input buffer (decode_frame), into the decoder's caller-supplied payload
// buffer, or into the decoder's internal scratch buffer. See the lifetime note
// on frame_decoder::next.
struct frame {
    frame_header header;
    ordered_extension ordered;
    span<const std::byte> payload;
};

// Returns the compression algorithm encoded in the header flags.
[[nodiscard]] constexpr compression_algorithm frame_compression(const frame_header& header) noexcept {
    return static_cast<compression_algorithm>((header.flags & frame_flag_algo_mask) >> frame_flag_algo_shift);
}

// --- Header and frame codec --------------------------------------------------

// Writes exactly frame_header_size bytes to `out`. `out` must hold at least
// frame_header_size bytes. The reserved field is written as zero. This function
// does not validate the semantic rules (stream scope, flag consistency, ...);
// it is the raw header writer. Failure (buffer too small) is
// rpc_code::encode_error and leaves `out` partially written.
[[nodiscard]] CRPC_API status encode_frame_header(span<std::byte> out,
                                                  const frame_header& header) noexcept;

// Reads and validates a header from the first frame_header_size bytes of `in`.
// `in` must hold at least frame_header_size bytes. A payload_len greater than
// `max_frame` is rejected before any allocation. Every semantic violation is
// rpc_code::protocol_error and leaves `header` unchanged. CRC32C is not checked
// here because the payload is not available; decode_frame and frame_decoder do
// that.
[[nodiscard]] CRPC_API status decode_frame_header(span<const std::byte> in,
                                                  frame_header& header,
                                                  u32 max_frame = max_frame_default) noexcept;

// Total encoded size of `value`: header, optional ordered extension, payload.
[[nodiscard]] CRPC_API std::size_t encoded_frame_size(const frame& value) noexcept;

// Encodes a complete frame into `out` and reports the number of bytes written.
// The header's payload_len is replaced by value.payload.size(); when
// frame_flag_crc32c is set, ext is replaced by crc32c(value.payload). All other
// header fields must already be valid. `out` must hold at least
// encoded_frame_size(value) bytes. Failure modes:
//   * rpc_code::invalid_argument -- invalid type, flags, stream scope, ext, or
//     payload longer than `max_frame`;
//   * rpc_code::encode_error -- `out` is too small.
// On failure `written` is zero.
[[nodiscard]] CRPC_API status encode_frame(span<std::byte> out,
                                           const frame& value,
                                           std::size_t& written,
                                           u32 max_frame = max_frame_default) noexcept;

// Decodes exactly one complete frame from `in`. `in.size()` must equal the
// encoded frame size; trailing bytes are a length mismatch and are rejected.
// CRC32C is verified when the flag is set. On success `value.payload` points
// into `in` and is valid for as long as `in` is valid. Failure modes: any
// protocol violation is rpc_code::protocol_error; `value` is unchanged.
[[nodiscard]] CRPC_API status decode_frame(span<const std::byte> in,
                                           frame& value,
                                           u32 max_frame = max_frame_default) noexcept;

// CRC32C (Castagnoli, reflected polynomial 0x82F63B78). The implementation is
// table-driven with a constexpr table and does not allocate.
[[nodiscard]] CRPC_API u32 crc32c(span<const std::byte> data) noexcept;

// --- Call envelope -----------------------------------------------------------

// The codec id type and its numeric values are defined in <crpc/codec_id.hpp>,
// which both this header and the codec registry include. See that file for the
// rules (0 reserved, append-only, fail closed on unknown).

inline constexpr u8 call_flag_has_extensions = 0x01;
inline constexpr u8 call_flag_known_mask = call_flag_has_extensions;

// Optional trace, deadline, and idempotency fields of a call envelope. `flags`
// contains the presence bits below; a field is present exactly when its bit is
// set. The byte layout is documented at the top of this file.
struct trace_extension {
    static constexpr u8 has_trace_id = 0x01;
    static constexpr u8 has_span_id = 0x02;
    static constexpr u8 has_parent_span_id = 0x04;
    static constexpr u8 has_deadline = 0x08;
    static constexpr u8 has_idempotency_key = 0x10;
    static constexpr u8 known_mask = 0x1F;

    u8 flags = 0;
    std::array<std::byte, 16> trace_id{};
    std::array<std::byte, 8> span_id{};
    std::array<std::byte, 8> parent_span_id{};
    u64 deadline_ns = 0;
    // Borrowed, not owned. On decode it points into the envelope input; on
    // encode the caller owns the bytes for the duration of the call.
    span<const std::byte> idempotency_key;

    [[nodiscard]] constexpr bool has(u8 bit) const noexcept { return (flags & bit) != 0; }
    [[nodiscard]] constexpr bool empty() const noexcept { return flags == 0; }
};

// Decoded call envelope. `body` borrows the remaining bytes of the input.
struct call_envelope {
    u64 service_id = 0;
    u64 method_id = 0;
    u8 call_flags = 0;
    codec_id codec = codec_id::crpc_binary;
    trace_extension extensions;
    span<const std::byte> body;
};

// Returns the exact encoded size of `value`. No validation is performed.
[[nodiscard]] CRPC_API std::size_t encoded_call_envelope_size(const call_envelope& value) noexcept;

// Encodes a call envelope. Failure modes:
//   * rpc_code::invalid_argument -- unknown call flag, unknown codec, unknown
//     extension flag, span/parent without their prerequisite, idempotency key
//     longer than max_idempotency_key, or an extension field set while
//     call_flag_has_extensions is clear;
//   * rpc_code::encode_error -- `out` is too small.
// On failure `written` is zero.
[[nodiscard]] CRPC_API status encode_call_envelope(span<std::byte> out,
                                                   const call_envelope& value,
                                                   std::size_t& written) noexcept;

// Decodes a call envelope from a payload. `body` is everything after the
// envelope fields. Failure modes: truncation, unknown call flag, unknown codec,
// unknown extension flag, invalid span/parent combination, idempotency key
// longer than max_idempotency_key -- all rpc_code::protocol_error. `value` is
// unchanged on failure.
[[nodiscard]] CRPC_API status decode_call_envelope(span<const std::byte> in,
                                                   call_envelope& value) noexcept;

// --- Incremental decoder -----------------------------------------------------

// Streaming frame decoder. Bytes are copied into an internal buffer; a complete
// frame is only produced by next(). The decoder performs no allocation for a
// frame payload when a caller payload buffer was supplied to the constructor,
// and it never allocates for a payload before the header has been validated
// against max_frame.
//
// Usage:
//
//   frame_decoder decoder(span<std::byte>(payload_storage));
//   decoder.feed(chunk);
//   for (;;) {
//       frame value;
//       switch (decoder.next(value)) {
//       case frame_decoder::result::frame_ready: handle(value); break;
//       case frame_decoder::result::need_more: goto next_chunk;
//       case frame_decoder::result::error: fail(decoder.error()); break;
//       }
//   }
//
// feed() may be called with arbitrarily split chunks, including one byte at a
// time, and with several frames in one chunk. next() may be called before a
// frame is complete; it then returns need_more and leaves `out` unchanged.
class CRPC_API frame_decoder {
public:
    enum class result : u8 {
        need_more,
        frame_ready,
        error
    };

    // Borrowing mode: decoded payloads point into the decoder's internal
    // scratch buffer and are valid until the next call to feed() or next().
    // `max_frame` is clamped to max_frame_hard_cap.
    explicit frame_decoder(u32 max_frame = max_frame_default) noexcept;

    // Copying mode: decoded payloads are copied into `payload_buffer` and are
    // valid until the next call to next(). A frame whose payload is larger than
    // `payload_buffer` is rejected with rpc_code::resource_exhausted. Pass an
    // empty span to select borrowing mode. `max_frame` is clamped to
    // max_frame_hard_cap.
    frame_decoder(span<std::byte> payload_buffer, u32 max_frame = max_frame_default) noexcept;

    frame_decoder(const frame_decoder&) = delete;
    frame_decoder& operator=(const frame_decoder&) = delete;
    frame_decoder(frame_decoder&&) noexcept = default;
    frame_decoder& operator=(frame_decoder&&) noexcept = default;

    // Copies `data` into the internal buffer. Input need not outlive the call.
    // Precondition: `data` must not alias this decoder's internal buffer (for
    // example, a payload span previously returned in borrowing mode); feeding
    // such a span back would be a self-append. Calling feed() after an error is
    // a no-op. If the internal buffer would exceed its cap, the decoder enters
    // the error state with rpc_code::resource_exhausted.
    void feed(span<const std::byte> data) noexcept;

    // Attempts to decode one frame from the buffered bytes.
    //   * need_more   -- the header or payload is incomplete; `out` unchanged;
    //   * frame_ready -- `out` is filled and the consumed bytes are released;
    //   * error       -- a protocol violation; error() names it and the decoder
    //                    stays in the error state until reset().
    // In borrowing mode out.payload is invalidated by the next feed() or
    // next(). In copying mode it is invalidated by the next next().
    [[nodiscard]] result next(frame& out) noexcept;

    // Error code of the last failure. rpc_code::ok when no error occurred.
    [[nodiscard]] rpc_code error() const noexcept;

    // True when the decoder is in the error state.
    [[nodiscard]] bool failed() const noexcept;

    // Number of bytes buffered but not yet consumed.
    [[nodiscard]] std::size_t buffered() const noexcept;

    // Effective maximum payload length (the constructor argument clamped to
    // max_frame_hard_cap).
    [[nodiscard]] u32 max_frame() const noexcept;

    // Clears buffered bytes and the error state. Retains allocated capacity.
    void reset() noexcept;

private:
    void compact() noexcept;

    buffer buffer_;
    span<std::byte> payload_buffer_;
    u32 max_frame_ = max_frame_default;
    std::size_t read_offset_ = 0;
    rpc_code error_ = rpc_code::ok;
};

} // namespace crpc

#endif // CRPC_FRAME_HPP


