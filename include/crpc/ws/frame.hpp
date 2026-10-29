// CRPC -- WebSocket wire codec (RFC 6455 sections 5 and 7).
//
// This is the bytes layer only: opcodes, the three payload-length encodings,
// masking, the control-frame constraints and the close codes. It knows nothing
// about the HTTP Upgrade exchange (crpc/ws/handshake.hpp) and nothing about
// fragmentation ordering or the ping/close state machine (crpc/ws/stream.hpp),
// so each layer can be tested in isolation against the RFC's own vectors.
//
// The decoder is incremental: `feed` may be called with any split of the byte
// stream, including one byte at a time. It is deliberately strict in the
// places where RFC 6455 demands strictness (reserved bits, reserved length
// encodings, control-frame shape, mask direction) and refuses to allocate a
// payload whose declared length exceeds the configured ceiling, so a hostile
// peer that announces a 2^63-byte frame cannot exhaust memory.
#ifndef CRPC_WS_FRAME_HPP
#define CRPC_WS_FRAME_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace ws {

// RFC 6455 section 1.3: the GUID concatenated with the client key before
// SHA-1. Both endpoints of every WebSocket derive the accept value from it, so
// it is pinned here rather than spelled out in the handshake code.
inline constexpr std::string_view websocket_guid =
    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// Opcodes (RFC 6455 section 5.2). Note the gap between 2 and 8: 0x3-0x7 and
// 0xB-0xF are reserved for future extensions and are rejected by the decoder.
enum class opcode : std::uint8_t {
    continuation = 0x0,
    text = 0x1,
    binary = 0x2,
    close = 0x8,
    ping = 0x9,
    pong = 0xA
};

[[nodiscard]] CRPC_API bool is_control(opcode op) noexcept;
[[nodiscard]] CRPC_API bool is_data(opcode op) noexcept;

// Close status codes (RFC 6455 section 7.4.1). The three values marked "never
// on the wire" are defined only for the local side: 1005 and 1006 are what a
// stack reports when no status or an abnormal close was observed, and 1015 is
// the TLS-handshake failure report. 1004 is reserved but undefined.
enum class close_code : std::uint16_t {
    normal = 1000,
    going_away = 1001,
    protocol_error = 1002,
    unsupported_data = 1003,
    no_status_received = 1005, // never on the wire
    abnormal_closure = 1006,   // never on the wire
    invalid_payload = 1007,
    policy_violation = 1008,
    message_too_big = 1009,
    missing_extension = 1010,  // sent by a client only
    internal_error = 1011,
    tls_handshake = 1015       // never on the wire
};

// True when `code` may legally appear in a Close frame on the wire. The
// defined codes (1000-1003, 1007-1011), the later IANA-registered codes
// 1012-1014, and the registered/private ranges 3000-4999 are acceptable. The
// reserved values (1004-1006, 1015) and the protocol-reserved range
// 1016-2999 are not; receiving one of them is a protocol error (section
// 7.4.2). Sending 1006/1005/1015 is also forbidden, which the send-side uses
// the same function to enforce.
[[nodiscard]] CRPC_API bool is_valid_close_code(std::uint16_t code) noexcept;

// True when `text` is valid UTF-8 (RFC 3629, as required by RFC 6455 sections
// 5.6 and 8.1). Rejects overlong encodings, surrogates, code points above
// U+10FFFF and a multibyte sequence truncated at the end of the input. The
// stream layer uses this on every text message (after fragmentation is
// reassembled) and on Close reasons.
[[nodiscard]] CRPC_API bool is_valid_utf8(span<const std::byte> text) noexcept;
[[nodiscard]] CRPC_API bool is_valid_utf8(std::string_view text) noexcept;

// Describes one frame for encoding. `masked` selects the client-side masking
// (`encode_frame` generates a fresh random mask per call, as RFC 6455 section
// 5.3 requires); the payload is then masked within the returned buffer.
struct frame_header {
    bool fin = true;
    std::uint8_t rsv = 0; // extensions are not negotiated, so this must be 0
    opcode op = opcode::binary;
    bool masked = false;
    std::uint64_t length = 0; // payload length; encode derives it from the payload
};

// A complete decoded frame. The payload is already unmasked.
struct decoded_frame {
    bool fin = true;
    std::uint8_t rsv = 0;
    opcode op = opcode::binary;
    std::string payload;
};

// Serializes one frame. A control frame (close/ping/pong) with a payload
// larger than 125 bytes or with FIN=0 is rejected, as is a non-zero `rsv`, an
// unknown opcode, or a payload larger than the 63-bit length field can carry.
// On failure `out` is left unchanged.
[[nodiscard]] CRPC_API status encode_frame(const frame_header& header,
                                           span<const std::byte> payload,
                                           std::string& out);

enum class frame_status : std::uint8_t { need_more, frame, error };

// Incremental WebSocket frame decoder.
//
// `feed` appends `data` and attempts to complete one frame. It returns
// `frame` when a full frame is available (`out` is filled and the bytes are
// removed from the internal buffer), `need_more` while the frame is
// incomplete, and `error` on a protocol violation (after which every further
// call returns `error`, and `error()` names the violation).
//
// The mask policy is fixed per direction (RFC 6455 section 5.1): a client
// must mask every frame it sends and a server must not, so a server-side
// decoder expects masked frames (`expect_masked = true`) and a client-side
// decoder expects unmasked frames. A frame with the wrong mask bit is a
// protocol error.
class CRPC_API frame_decoder {
public:
    struct options {
        // Server side requires masked input; client side requires unmasked.
        bool expect_masked = false;
        // Ceiling on one frame's declared payload. A header that claims more
        // is rejected with `resource_exhausted` before any payload bytes are
        // buffered, which is what turns a hostile length field into an error
        // instead of an allocation.
        std::size_t max_frame_payload = 64u * 1024u * 1024u;
    };

    explicit frame_decoder(options opt) noexcept;
    frame_decoder() noexcept : frame_decoder(options{}) {}

    [[nodiscard]] frame_status feed(span<const std::byte> data,
                                    decoded_frame& out);
    [[nodiscard]] const rpc_error& error() const noexcept;
    [[nodiscard]] std::size_t buffered() const noexcept;
    void reset() noexcept;

private:
    bool fail(rpc_code code, std::string message);
    [[nodiscard]] bool parse_header(std::uint64_t& header_size,
                                    std::uint64_t& payload_length,
                                    bool& masked,
                                    std::array<std::byte, 4>& mask);

    options opt_;
    std::string buf_;
    rpc_error error_;
    bool failed_ = false;
};

} // namespace ws
} // namespace crpc

#endif // CRPC_WS_FRAME_HPP