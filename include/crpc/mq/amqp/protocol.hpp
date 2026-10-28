// CRPC -- AMQP 0-9-1 wire codec.
//
// This header owns bytes, not I/O:
//
//   * the eight-byte protocol header and the frame layout (type, channel, size,
//     payload, 0xCE);
//   * the field-table encoding shared by method arguments, headers and the
//     server's properties;
//   * the Basic content properties and the header/body split that respects
//     `frame_max`.
//
// The class and method numbers are the ones from the 0-9-1 specification. They
// are verified against RabbitMQ 3.12.1 by tests/test_mq_amqp_it.cpp: a wrong
// number is not rejected locally, so only a real broker proves it.
//
// Field numbers and bit positions that are easy to get wrong, and that the
// integration test therefore covers end to end:
//
//   * the method frame payload starts with class-id (u16), then method-id (u16),
//     both big-endian;
//   * consecutive boolean arguments are packed LSB-first into one octet;
//   * the Basic content header carries class-id, weight, body size (u64), then
//     the property flags (u16) and only the properties whose flag is set;
//   * every frame ends with the single octet 0xCE. A decoder that does not check
//     it will happily accept a stream that is off by one.
#ifndef CRPC_MQ_AMQP_PROTOCOL_HPP
#define CRPC_MQ_AMQP_PROTOCOL_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/mq/amqp/types.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace mq {
namespace amqp {

// The protocol header is written once, before any frame. Version 0-9-1 uses
// the fourth byte 0x00 and the last four bytes 0x00 0x09 0x01.
inline constexpr std::array<std::byte, 8> protocol_header = {
    std::byte{0x41}, std::byte{0x4D}, std::byte{0x51}, std::byte{0x50},
    std::byte{0x00}, std::byte{0x00}, std::byte{0x09}, std::byte{0x01}};

inline constexpr std::byte frame_end_octet{0xCE};

// Eight bytes per frame are not payload: type (1), channel (2), size (4) and
// the frame-end octet (1).
inline constexpr std::size_t frame_overhead = 8;

inline constexpr std::uint32_t default_frame_max = 131072;

// Hard ceiling on a frame this decoder will accept. The negotiated frame_max is
// normally 131072; the ceiling only stops a hostile peer from announcing a
// gigabyte payload.
inline constexpr std::uint32_t max_frame_payload = 16u * 1024u * 1024u;

enum class frame_type : std::uint8_t {
    method = 1,
    header = 2,
    body = 3,
    heartbeat = 8
};

namespace class_id {
inline constexpr std::uint16_t connection = 10;
inline constexpr std::uint16_t channel = 20;
inline constexpr std::uint16_t exchange = 40;
inline constexpr std::uint16_t queue = 50;
inline constexpr std::uint16_t basic = 60;
inline constexpr std::uint16_t confirm = 85;
} // namespace class_id

namespace method_id {
// connection
inline constexpr std::uint16_t start = 10;
inline constexpr std::uint16_t start_ok = 11;
inline constexpr std::uint16_t secure = 20;
inline constexpr std::uint16_t secure_ok = 21;
inline constexpr std::uint16_t tune = 30;
inline constexpr std::uint16_t tune_ok = 31;
inline constexpr std::uint16_t open = 40;
inline constexpr std::uint16_t open_ok = 41;
inline constexpr std::uint16_t close = 50;
inline constexpr std::uint16_t close_ok = 51;
// channel
inline constexpr std::uint16_t channel_open = 10;
inline constexpr std::uint16_t channel_open_ok = 11;
inline constexpr std::uint16_t channel_close = 40;
inline constexpr std::uint16_t channel_close_ok = 41;
// exchange
inline constexpr std::uint16_t exchange_declare = 10;
inline constexpr std::uint16_t exchange_declare_ok = 11;
// queue
inline constexpr std::uint16_t queue_declare = 10;
inline constexpr std::uint16_t queue_declare_ok = 11;
inline constexpr std::uint16_t queue_bind = 20;
inline constexpr std::uint16_t queue_bind_ok = 21;
inline constexpr std::uint16_t queue_delete = 40;
inline constexpr std::uint16_t queue_delete_ok = 41;
// basic
inline constexpr std::uint16_t basic_qos = 10;
inline constexpr std::uint16_t basic_qos_ok = 11;
inline constexpr std::uint16_t basic_consume = 20;
inline constexpr std::uint16_t basic_consume_ok = 21;
inline constexpr std::uint16_t basic_cancel = 30;
inline constexpr std::uint16_t basic_cancel_ok = 31;
inline constexpr std::uint16_t basic_publish = 40;
inline constexpr std::uint16_t basic_return = 50;
inline constexpr std::uint16_t basic_deliver = 60;
inline constexpr std::uint16_t basic_get = 70;
inline constexpr std::uint16_t basic_get_ok = 71;
inline constexpr std::uint16_t basic_get_empty = 72;
inline constexpr std::uint16_t basic_ack = 80;
inline constexpr std::uint16_t basic_reject = 90;
inline constexpr std::uint16_t basic_nack = 120;
// confirm
inline constexpr std::uint16_t confirm_select = 10;
inline constexpr std::uint16_t confirm_select_ok = 11;
} // namespace method_id

struct frame_header {
    frame_type type = frame_type::method;
    std::uint16_t channel = 0;
    std::uint32_t size = 0;
};

struct frame {
    frame_type type = frame_type::method;
    std::uint16_t channel = 0;
    std::vector<std::byte> payload;
};

// --- Frame encoding/decoding -------------------------------------------------

// Appends one complete frame. The payload must fit a u32; a caller that builds
// one larger than that is already outside the protocol.
CRPC_API void append_frame(std::vector<std::byte>& out, frame_type type,
                           std::uint16_t channel, span<const std::byte> payload);

[[nodiscard]] CRPC_API std::vector<std::byte> encode_frame(
    frame_type type, std::uint16_t channel, span<const std::byte> payload);

// Appends a method frame whose payload is class-id, method-id and `arguments`.
CRPC_API void append_method_frame(std::vector<std::byte>& out, std::uint16_t channel,
                                  std::uint16_t method_class, std::uint16_t method,
                                  span<const std::byte> arguments);

// Decodes a seven-byte header. A type that is not one of method/header/body/
// heartbeat, or a size above `max_frame_payload`, is `protocol_error`.
[[nodiscard]] CRPC_API status decode_frame_header(span<const std::byte> in,
                                                  frame_header& out) noexcept;

// Decodes one whole frame, including the frame-end octet. `consumed` is set to
// the number of bytes the frame occupied. A wrong frame-end octet, a truncated
// frame or an unknown type is `protocol_error`; the end octet is the check that
// keeps a stream that lost a byte from being accepted.
[[nodiscard]] CRPC_API status decode_frame(span<const std::byte> in, frame& out,
                                           std::size_t& consumed) noexcept;

// --- Argument writer ---------------------------------------------------------

// Builds method arguments. A call after an error is a no-op, so the caller can
// chain the whole argument list and check `ok()` once. Oversized short strings
// (over 255 bytes) and long strings/tables over 4 GiB are `encode_error`.
class CRPC_API writer {
public:
    writer() = default;

    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    // Writes one octet holding `count` bits from `value`, LSB first. AMQP packs
    // consecutive booleans into one octet in declaration order.
    void bits(std::uint8_t value, std::uint8_t count);
    void short_string(std::string_view value);
    void long_string(std::string_view value);
    void long_string(span<const std::byte> value);
    void table(const field_table& value);
    void properties(const message_properties& value);

    [[nodiscard]] bool ok() const noexcept { return error_ == rpc_code::ok; }
    [[nodiscard]] rpc_code error() const noexcept { return error_; }
    [[nodiscard]] const std::vector<std::byte>& data() const noexcept { return data_; }
    [[nodiscard]] std::vector<std::byte> take() { return std::move(data_); }

private:
    void fail(rpc_code code) noexcept;

    std::vector<std::byte> data_;
    rpc_code error_ = rpc_code::ok;
};

// --- Argument reader ---------------------------------------------------------

// Reads method arguments and field tables. Every accessor is bounds checked; a
// failure sets `error()` and the accessor returns a zero/empty value. The caller
// checks `ok()` (or the bool return of `read_table`/`read_properties`) once the
// arguments are consumed.
class CRPC_API reader {
public:
    explicit reader(span<const std::byte> data) noexcept;

    [[nodiscard]] bool ok() const noexcept { return error_ == rpc_code::ok; }
    [[nodiscard]] rpc_code error() const noexcept { return error_; }

    std::uint8_t u8() noexcept;
    std::uint16_t u16() noexcept;
    std::uint32_t u32() noexcept;
    std::uint64_t u64() noexcept;
    // Reads one octet and keeps `count` bits; returns false on a short buffer.
    bool bits(std::uint8_t count, std::uint8_t& value) noexcept;
    // The returned views point into the buffer the reader was built from.
    std::string_view short_string() noexcept;
    std::string_view long_string() noexcept;

    bool read_table(field_table& out) noexcept;
    // One type octet and its payload. Used for table entries and array items;
    // exposed because a caller that walks a table by hand needs the same
    // decoding rules.
    bool read_value(field_value& out) noexcept;
    bool read_properties(message_properties& out) noexcept;

    [[nodiscard]] std::size_t remaining() const noexcept {
        return data_.size() - offset_;
    }
    bool skip(std::size_t count) noexcept;

private:
    bool read_array(std::vector<field_value>& out) noexcept;
    bool need(std::size_t count) noexcept;
    void fail(rpc_code code) noexcept;

    span<const std::byte> data_;
    std::size_t offset_ = 0;
    rpc_code error_ = rpc_code::ok;
};

// --- Content -----------------------------------------------------------------

// Appends the Basic content header frame and as many body frames as the body
// needs. The largest body frame is `frame_max - 8` bytes; a body of exactly one
// chunk is still one body frame, and an empty body has none. `frame_max` must be
// larger than `frame_overhead`, otherwise the split would make no progress and
// this reports `encode_error`.
[[nodiscard]] CRPC_API status append_content_frames(
    std::vector<std::byte>& out, std::uint16_t channel, std::uint16_t content_class,
    const message_properties& properties, span<const std::byte> body,
    std::uint32_t frame_max);

// Header frame plus body frames. Zero when `frame_max` is too small.
[[nodiscard]] CRPC_API std::size_t content_frame_count(std::size_t body_size,
                                                       std::uint32_t frame_max) noexcept;

// Maps an AMQP reply code (the code in Connection.Close/Channel.Close) to the
// library's error model. The server's own code and text stay in the caller's
// message, so this only classifies.
[[nodiscard]] CRPC_API rpc_code map_reply_code(std::uint16_t reply_code) noexcept;

} // namespace amqp
} // namespace mq
} // namespace crpc

#endif // CRPC_MQ_AMQP_PROTOCOL_HPP