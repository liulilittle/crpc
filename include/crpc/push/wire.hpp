// CRPC -- PUSH frame payload codec and the push control contract.
//
// A PUSH frame (type 0x0B) carries a fixed push envelope followed by the
// application body. The frame header stays exactly as specified in
// docs/protocol.md section 3:
//
//   PUSH (0x0B), stream_id = the subscription's non-zero push stream id
//     u8  version = 1
//     u8  mode          0 reliable, 1 unreliable
//     u16 topic_id      server-assigned, non-zero
//     u64 publisher_id
//     u64 epoch
//     u64 seq           per-subscription reliable sequence; 0 when unreliable
//     u64 topic_seq     per-topic monotonic sequence
//     u32 body_len      must equal the remaining payload length
//     body
//
// PUSH_ACK keeps its frozen protocol.md shape: frame type 0x0C, stream_id of
// the subscription, payload = `push_seq` u64 (cumulative acknowledgement).
// Because the acknowledgement carries no topic, the server maps each
// subscription to its own non-zero stream id. The subscription reply below is
// that mapping: it tells the client which stream id and topic id belong to the
// topic it subscribed to.
//
// Push control is carried over the ordinary REQUEST path using the reserved
// `push_control_service_id` / method ids below; it is not a new frame type.
// Subscriber authentication/authorization is deliberately deferred: the
// minimal slice trusts `subscriber_id` from the application.
//
// Fail-closed rules: an unknown version, an unknown mode, topic id 0, a
// reliable envelope with seq 0, a body length that is not exactly the remaining
// payload, a truncated fixed field, or a truncated/trailing topic string is
// rejected (protocol_error on decode, invalid_argument on encode).
#ifndef CRPC_PUSH_WIRE_HPP
#define CRPC_PUSH_WIRE_HPP

#include <cstddef>
#include <string>
#include <string_view>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/push/hub.hpp>
#include <crpc/span.hpp>

namespace crpc::push {

inline constexpr u8 push_wire_version = 1;
inline constexpr std::size_t push_envelope_size = 40;
inline constexpr std::size_t push_ack_size = 8;

// Reserved service/method pair for the subscription control request. The
// service id is the top of the u64 range so it cannot collide with a
// realistically configured application service.
inline constexpr u64 push_control_service_id = ~u64{0};
inline constexpr u64 push_control_method_subscribe = 1;

// Push envelope. `body` borrows the input on decode and the caller's bytes on
// encode.
struct push_envelope {
    u8 version = push_wire_version;
    delivery_mode mode = delivery_mode::unreliable;
    u16 topic_id = 0;
    u64 publisher_id = 0;
    u64 epoch = 0;
    u64 seq = 0;
    u64 topic_seq = 0;
    span<const std::byte> body;
};

// One PUSH as delivered to the client sink. `body` is valid only for the
// duration of the callback.
struct push_message_view {
    u32 stream_id = 0;
    u16 topic_id = 0;
    delivery_mode mode = delivery_mode::reliable;
    u64 publisher_id = 0;
    u64 epoch = 0;
    u64 seq = 0;
    u64 topic_seq = 0;
    span<const std::byte> body;
};

using push_sink_fn = void (*)(void* ctx, const push_message_view& message) noexcept;

// Subscribe request body (REQUEST payload body to push_control_service_id).
struct subscribe_request {
    u64 subscriber_id = 0;
    delivery_mode mode = delivery_mode::reliable;
    bool ordered = false;
    std::string topic;
};

// Subscribe reply body. It is the stream_id/topic mapping the client keeps for
// PUSH_ACK and for interpreting the `topic_id` of incoming PUSH frames.
struct push_subscription {
    u32 stream_id = 0;
    u16 topic_id = 0;
    delivery_mode mode = delivery_mode::reliable;
    bool ordered = false;
    std::string topic;
};

[[nodiscard]] CRPC_API std::size_t encoded_push_envelope_size(
    const push_envelope& value) noexcept;

// Encodes a push envelope. Failure: invalid_argument for a field that violates
// the rules above, encode_error when `out` is too small. `written` is zero on
// failure.
[[nodiscard]] CRPC_API status encode_push_envelope(span<std::byte> out,
                                                   const push_envelope& value,
                                                   std::size_t& written) noexcept;

// Decodes a push envelope. Every malformed input is protocol_error; `value` is
// unchanged on failure.
[[nodiscard]] CRPC_API status decode_push_envelope(span<const std::byte> in,
                                                   push_envelope& value) noexcept;

[[nodiscard]] CRPC_API std::size_t encoded_subscribe_request_size(
    const subscribe_request& value) noexcept;
[[nodiscard]] CRPC_API status encode_subscribe_request(span<std::byte> out,
                                                       const subscribe_request& value,
                                                       std::size_t& written) noexcept;
[[nodiscard]] CRPC_API status decode_subscribe_request(span<const std::byte> in,
                                                       subscribe_request& value) noexcept;

[[nodiscard]] CRPC_API std::size_t encoded_push_subscription_size(
    const push_subscription& value) noexcept;
[[nodiscard]] CRPC_API status encode_push_subscription(span<std::byte> out,
                                                       const push_subscription& value,
                                                       std::size_t& written) noexcept;
[[nodiscard]] CRPC_API status decode_push_subscription(span<const std::byte> in,
                                                       push_subscription& value) noexcept;

} // namespace crpc::push

#endif // CRPC_PUSH_WIRE_HPP
