// CRPC -- HTTP/2 frame layer (RFC 7540 sections 4 and 6).
//
// HTTP/2 frames are *not* the CRPC binary protocol frames (crpc/frame.hpp):
//
//   * the fixed header is 9 bytes, not 16;
//   * every multi-byte field is big-endian (network order), whereas the CRPC
//     binary wire is little-endian;
//   * the length field is 24 bits, the type and flags are 8 bits each, and the
//     stream identifier is 31 bits with the top bit reserved.
//
// This header owns the frame constants, the 9-byte header codec and the
// per-type payload codecs. The connection state machine lives in
// connection.hpp; header compression lives in hpack.hpp.
//
// All payload codecs are bounds-checked and allocation-free except
// decode_settings, which appends into a caller-owned vector and is only used
// off the hot path.
//
// Thread safety: every type here is a plain value or a caller-buffer view.
// Nothing is shared; a connection's decoder is single-threaded (documented in
// connection.hpp).
#ifndef CRPC_HTTP2_FRAME_HPP
#define CRPC_HTTP2_FRAME_HPP

#include <array>
#include <cstddef>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc::http2 {

// --- Sizes and limits --------------------------------------------------------

inline constexpr std::size_t frame_header_size = 9;

// SETTINGS_MAX_FRAME_SIZE is bounded by the protocol: the length field is 24
// bits, so the largest representable value is 2^24 - 1.
inline constexpr u32 default_max_frame_size = 16384;
inline constexpr u32 min_max_frame_size = 16384;
inline constexpr u32 max_max_frame_size = 16777215u;

// stream identifier is 31 bits; the reserved high bit is always zero.
inline constexpr u32 max_stream_id = 0x7FFFFFFFu;
inline constexpr u32 connection_stream_id = 0;

// HPACK and flow-control defaults from RFC 7540 sections 6.5.2 and 6.9.2.
inline constexpr u32 default_initial_window_size = 65535;
inline constexpr u32 max_window_size = 0x7FFFFFFFu; // 2^31 - 1
inline constexpr u32 default_header_table_size = 4096;
inline constexpr u32 default_max_concurrent_streams = 100;
inline constexpr u32 default_max_header_list_size = 16384;

// --- Frame types -------------------------------------------------------------

enum class frame_type : u8 {
    data = 0x0,
    headers = 0x1,
    priority = 0x2,
    rst_stream = 0x3,
    settings = 0x4,
    push_promise = 0x5,
    ping = 0x6,
    goaway = 0x7,
    window_update = 0x8,
    continuation = 0x9
};

// True for the ten frame types defined by RFC 7540. Unknown types are a matter
// for the connection layer: RFC 7540 section 4.1 says they must be ignored,
// while the CRPC ruling (R9/R13) mandates fail-closed for the *CRPC binary*
// frame layer. The h2 connection defaults to the RFC behaviour and exposes an
// option to reject instead; see connection.hpp.
[[nodiscard]] constexpr bool is_known_frame_type(u8 value) noexcept {
    return value <= static_cast<u8>(frame_type::continuation);
}

[[nodiscard]] constexpr bool is_known_frame_type(frame_type value) noexcept {
    return is_known_frame_type(static_cast<u8>(value));
}

// --- Flags -------------------------------------------------------------------
//
// Flag bits are type-specific. DATA, HEADERS and PUSH_PROMISE reuse bit 0x1 and
// 0x8 with different names; the constants below are named by their bit and
// documented per type.

namespace flags {
inline constexpr u8 end_stream = 0x1;  // DATA, HEADERS
inline constexpr u8 ack = 0x1;         // SETTINGS, PING
inline constexpr u8 end_headers = 0x4; // HEADERS, PUSH_PROMISE, CONTINUATION
inline constexpr u8 padded = 0x8;      // DATA, HEADERS, PUSH_PROMISE
inline constexpr u8 priority = 0x20;   // HEADERS
} // namespace flags

// --- Error codes (RFC 7540 section 7) ----------------------------------------

enum class error_code : u32 {
    no_error = 0x0,
    protocol_error = 0x1,
    internal_error = 0x2,
    flow_control_error = 0x3,
    settings_timeout = 0x4,
    stream_closed = 0x5,
    frame_size_error = 0x6,
    refused_stream = 0x7,
    cancel = 0x8,
    compression_error = 0x9,
    connect_error = 0xa,
    enhance_your_calm = 0xb,
    inadequate_security = 0xc,
    http_1_1_required = 0xd
};

// Stable name for diagnostics; never null.
[[nodiscard]] CRPC_API const char* to_string(error_code code) noexcept;

// Maps an h2 error code onto the CRPC error model (R6). Not every h2 code has
// an exact CRPC counterpart; the mapping is total and documented in the .cpp.
[[nodiscard]] CRPC_API rpc_code to_rpc_code(error_code code) noexcept;

// --- Settings identifiers ----------------------------------------------------

enum class setting_id : u16 {
    header_table_size = 0x1,
    enable_push = 0x2,
    max_concurrent_streams = 0x3,
    initial_window_size = 0x4,
    max_frame_size = 0x5,
    max_header_list_size = 0x6
};

// --- Frame header ------------------------------------------------------------

struct frame_header {
    // 24-bit payload length, excluding the 9-byte header.
    u32 length = 0;
    frame_type type = frame_type::data;
    u8 flags = 0;
    // 31-bit stream identifier (bit 31 is always zero on the wire).
    u32 stream_id = 0;

    [[nodiscard]] constexpr bool has_flag(u8 bit) const noexcept {
        return (flags & bit) != 0;
    }
    [[nodiscard]] constexpr bool is(frame_type value) const noexcept {
        return type == value;
    }
};

// --- Big-endian reader / writer ---------------------------------------------
//
// These mirror crpc::byte_reader/byte_writer but use network byte order and are
// the only sanctioned way to move integers into an h2 frame buffer. Reads are
// bounded; a short read sets rpc_code::protocol_error, a short write sets
// rpc_code::encode_error.

class frame_writer {
public:
    explicit frame_writer(span<std::byte> out) noexcept : out_(out) {}

    [[nodiscard]] std::size_t written() const noexcept { return pos_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return out_.size() - pos_; }
    [[nodiscard]] bool ok() const noexcept { return error_ == rpc_code::ok; }
    [[nodiscard]] rpc_code error() const noexcept { return error_; }

    bool put_u8(u8 value) noexcept {
        if (!ensure(1)) {
            return false;
        }
        out_[pos_++] = static_cast<std::byte>(value);
        return true;
    }

    bool put_u16(u16 value) noexcept {
        if (!ensure(2)) {
            return false;
        }
        out_[pos_++] = static_cast<std::byte>((value >> 8) & 0xFFu);
        out_[pos_++] = static_cast<std::byte>(value & 0xFFu);
        return true;
    }

    // Writes the low 24 bits of `value`, most significant byte first.
    bool put_u24(u32 value) noexcept {
        if (!ensure(3)) {
            return false;
        }
        out_[pos_++] = static_cast<std::byte>((value >> 16) & 0xFFu);
        out_[pos_++] = static_cast<std::byte>((value >> 8) & 0xFFu);
        out_[pos_++] = static_cast<std::byte>(value & 0xFFu);
        return true;
    }

    bool put_u32(u32 value) noexcept {
        if (!ensure(4)) {
            return false;
        }
        out_[pos_++] = static_cast<std::byte>((value >> 24) & 0xFFu);
        out_[pos_++] = static_cast<std::byte>((value >> 16) & 0xFFu);
        out_[pos_++] = static_cast<std::byte>((value >> 8) & 0xFFu);
        out_[pos_++] = static_cast<std::byte>(value & 0xFFu);
        return true;
    }

    bool put_bytes(span<const std::byte> bytes) noexcept {
        if (!ensure(bytes.size())) {
            return false;
        }
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            out_[pos_ + i] = bytes[i];
        }
        pos_ += bytes.size();
        return true;
    }

private:
    bool ensure(std::size_t count) noexcept {
        if (error_ != rpc_code::ok) {
            return false;
        }
        if (count > out_.size() - pos_) {
            error_ = rpc_code::encode_error;
            return false;
        }
        return true;
    }

    span<std::byte> out_;
    std::size_t pos_ = 0;
    rpc_code error_ = rpc_code::ok;
};

class frame_reader {
public:
    explicit frame_reader(span<const std::byte> in) noexcept : in_(in) {}

    [[nodiscard]] std::size_t read() const noexcept { return pos_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return in_.size() - pos_; }
    [[nodiscard]] bool ok() const noexcept { return error_ == rpc_code::ok; }
    [[nodiscard]] rpc_code error() const noexcept { return error_; }

    [[nodiscard]] span<const std::byte> remaining_bytes() const noexcept {
        return span<const std::byte>(in_.data() + pos_, in_.size() - pos_);
    }

    bool get_u8(u8& value) noexcept {
        if (!ensure(1)) {
            return false;
        }
        value = static_cast<u8>(in_[pos_++]);
        return true;
    }

    bool get_u16(u16& value) noexcept {
        if (!ensure(2)) {
            return false;
        }
        value = static_cast<u16>((static_cast<u16>(in_[pos_]) << 8) |
                                 static_cast<u16>(in_[pos_ + 1]));
        pos_ += 2;
        return true;
    }

    bool get_u24(u32& value) noexcept {
        if (!ensure(3)) {
            return false;
        }
        value = (static_cast<u32>(in_[pos_]) << 16) |
                (static_cast<u32>(in_[pos_ + 1]) << 8) |
                static_cast<u32>(in_[pos_ + 2]);
        pos_ += 3;
        return true;
    }

    bool get_u32(u32& value) noexcept {
        if (!ensure(4)) {
            return false;
        }
        value = (static_cast<u32>(in_[pos_]) << 24) |
                (static_cast<u32>(in_[pos_ + 1]) << 16) |
                (static_cast<u32>(in_[pos_ + 2]) << 8) |
                static_cast<u32>(in_[pos_ + 3]);
        pos_ += 4;
        return true;
    }

    bool get_bytes(std::size_t count, span<const std::byte>& out) noexcept {
        if (!ensure(count)) {
            return false;
        }
        out = span<const std::byte>(in_.data() + pos_, count);
        pos_ += count;
        return true;
    }

    bool skip(std::size_t count) noexcept {
        if (!ensure(count)) {
            return false;
        }
        pos_ += count;
        return true;
    }

private:
    bool ensure(std::size_t count) noexcept {
        if (error_ != rpc_code::ok) {
            return false;
        }
        if (count > in_.size() - pos_) {
            error_ = rpc_code::protocol_error;
            return false;
        }
        return true;
    }

    span<const std::byte> in_;
    std::size_t pos_ = 0;
    rpc_code error_ = rpc_code::ok;
};

// --- Header codec ------------------------------------------------------------

// Writes exactly frame_header_size bytes. `length` is truncated to 24 bits and
// `stream_id` to 31 bits; callers must have validated them (the connection
// does). Failure is rpc_code::encode_error.
[[nodiscard]] CRPC_API status encode_frame_header(span<std::byte> out,
                                                  const frame_header& header) noexcept;

// Reads and validates a 9-byte header from `in` (which must hold at least
// frame_header_size bytes). The reserved stream-id bit is masked off. A declared
// length greater than `max_frame_size` is rejected with
// rpc_code::protocol_error *before* the payload is considered. On failure
// `header` is unchanged.
[[nodiscard]] CRPC_API status decode_frame_header(span<const std::byte> in,
                                                  frame_header& header,
                                                  u32 max_frame_size = default_max_frame_size) noexcept;

// Encodes a full frame: 9-byte header followed by `payload`. The header's
// `length` field is overwritten with payload.size() and must not exceed
// max_max_frame_size. On failure `written` is zero.
[[nodiscard]] CRPC_API status encode_frame(span<std::byte> out,
                                           frame_header header,
                                           span<const std::byte> payload,
                                           std::size_t& written) noexcept;

// --- Payload views -----------------------------------------------------------

// DATA and HEADERS carry optional padding. When flags::padded is set the first
// payload byte is the pad length P and the last P bytes are padding. `out` is
// the meaningful region (excluding the pad byte and the padding). Failure is
// rpc_code::protocol_error when the padding does not fit.
[[nodiscard]] CRPC_API status strip_padding(span<const std::byte> payload,
                                            bool padded,
                                            span<const std::byte>& out) noexcept;

[[nodiscard]] CRPC_API status decode_data(span<const std::byte> payload,
                                          u8 frame_flags,
                                          span<const std::byte>& out) noexcept;

struct priority_payload {
    // 31-bit stream this stream depends on.
    u32 stream_dependency = 0;
    bool exclusive = false;
    // Raw wire byte; the RFC weight is this value plus one (range 1..256).
    u8 weight = 0;
};

struct headers_payload {
    bool end_stream = false;
    bool end_headers = false;
    bool has_priority = false;
    priority_payload priority{};
    // Borrows from the payload passed to decode_headers.
    span<const std::byte> header_block;
};

[[nodiscard]] CRPC_API status decode_headers(span<const std::byte> payload,
                                             u8 frame_flags,
                                             headers_payload& out) noexcept;

struct push_promise_payload {
    // 31-bit stream identifier of the promised stream.
    u32 promised_stream_id = 0;
    // Borrows from the payload passed to decode_push_promise.
    span<const std::byte> header_block;
};

[[nodiscard]] CRPC_API status decode_push_promise(span<const std::byte> payload,
                                                  u8 frame_flags,
                                                  push_promise_payload& out) noexcept;

[[nodiscard]] CRPC_API status decode_priority(span<const std::byte> payload,
                                              priority_payload& out) noexcept;

[[nodiscard]] CRPC_API status encode_priority(span<std::byte> out,
                                              const priority_payload& value,
                                              std::size_t& written) noexcept;

[[nodiscard]] CRPC_API status decode_rst_stream(span<const std::byte> payload,
                                                error_code& code) noexcept;

[[nodiscard]] CRPC_API status encode_rst_stream(span<std::byte> out,
                                                error_code code,
                                                std::size_t& written) noexcept;

struct goaway_payload {
    u32 last_stream_id = 0;
    error_code code = error_code::no_error;
    // Borrows from the payload passed to decode_goaway; may be empty.
    span<const std::byte> debug;
};

[[nodiscard]] CRPC_API status decode_goaway(span<const std::byte> payload,
                                            goaway_payload& out) noexcept;

[[nodiscard]] CRPC_API status encode_goaway(span<std::byte> out,
                                            const goaway_payload& value,
                                            std::size_t& written) noexcept;

[[nodiscard]] CRPC_API status decode_window_update(span<const std::byte> payload,
                                                   u32& increment) noexcept;

[[nodiscard]] CRPC_API status encode_window_update(span<std::byte> out,
                                                   u32 increment,
                                                   std::size_t& written) noexcept;

using ping_opaque = std::array<std::byte, 8>;

[[nodiscard]] CRPC_API status decode_ping(span<const std::byte> payload,
                                          ping_opaque& opaque) noexcept;

[[nodiscard]] CRPC_API status encode_ping(span<std::byte> out,
                                          const ping_opaque& opaque,
                                          std::size_t& written) noexcept;

struct setting {
    u16 id = 0;
    u32 value = 0;
};

// Appends one {id,value} per 6-byte chunk of `payload`. `out` is caller-owned
// and reused; this is the one codec that grows a container and is therefore not
// used on the per-frame hot path. Failure (payload not a multiple of 6) is
// rpc_code::protocol_error and leaves `out` unchanged.
[[nodiscard]] CRPC_API status decode_settings(span<const std::byte> payload,
                                              std::vector<setting>& out) noexcept;

// Writes 6 bytes per setting. `ack` frames must have no settings; this function
// accepts an empty entry list and emits nothing.
[[nodiscard]] CRPC_API status encode_settings(span<std::byte> out,
                                              span<const setting> entries,
                                              std::size_t& written) noexcept;

} // namespace crpc::http2

#endif // CRPC_HTTP2_FRAME_HPP
