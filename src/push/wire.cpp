#include <crpc/push/wire.hpp>

#include <cstddef>
#include <limits>
#include <utility>

#include <crpc/wire.hpp>

namespace crpc::push {
namespace {

constexpr std::size_t subscribe_request_fixed_size = 12;
constexpr std::size_t push_subscription_fixed_size = 10;
constexpr std::size_t max_topic_length = 0xFFFFu;

[[nodiscard]] bool valid_mode(delivery_mode mode) noexcept {
    return mode == delivery_mode::reliable || mode == delivery_mode::unreliable;
}

} // namespace

std::size_t encoded_push_envelope_size(const push_envelope& value) noexcept {
    return push_envelope_size + value.body.size();
}

status encode_push_envelope(span<std::byte> out, const push_envelope& value,
                            std::size_t& written) noexcept {
    written = 0;
    if (value.version != push_wire_version || !valid_mode(value.mode) ||
        value.topic_id == 0 ||
        (value.mode == delivery_mode::reliable && value.seq == 0) ||
        (value.mode == delivery_mode::unreliable && value.seq != 0)) {
        return status{rpc_code::invalid_argument};
    }
    if (value.body.size() > static_cast<std::size_t>(std::numeric_limits<u32>::max())) {
        return status{rpc_code::invalid_argument};
    }
    const std::size_t size = encoded_push_envelope_size(value);
    if (out.size() < size) {
        return status{rpc_code::encode_error};
    }
    byte_writer writer(out.first(size));
    writer.put_u8(value.version);
    writer.put_u8(static_cast<u8>(value.mode));
    writer.put_u16(value.topic_id);
    writer.put_u64(value.publisher_id);
    writer.put_u64(value.epoch);
    writer.put_u64(value.seq);
    writer.put_u64(value.topic_seq);
    writer.put_u32(static_cast<u32>(value.body.size()));
    writer.put_bytes(value.body);
    if (!writer.ok()) {
        return status{writer.error()};
    }
    written = size;
    return status{};
}

status decode_push_envelope(span<const std::byte> in, push_envelope& value) noexcept {
    byte_reader reader(in);
    u8 version = 0;
    u8 mode = 0;
    u16 topic_id = 0;
    u64 publisher_id = 0;
    u64 epoch = 0;
    u64 seq = 0;
    u64 topic_seq = 0;
    u32 body_len = 0;
    if (!reader.get_u8(version) || !reader.get_u8(mode) || !reader.get_u16(topic_id) ||
        !reader.get_u64(publisher_id) || !reader.get_u64(epoch) || !reader.get_u64(seq) ||
        !reader.get_u64(topic_seq) || !reader.get_u32(body_len)) {
        return status{rpc_code::protocol_error};
    }
    if (version != push_wire_version || mode > static_cast<u8>(delivery_mode::unreliable) ||
        topic_id == 0 ||
        (mode == static_cast<u8>(delivery_mode::reliable) && seq == 0) ||
        (mode == static_cast<u8>(delivery_mode::unreliable) && seq != 0)) {
        return status{rpc_code::protocol_error};
    }
    if (static_cast<u64>(body_len) != static_cast<u64>(reader.remaining())) {
        return status{rpc_code::protocol_error};
    }
    push_envelope parsed;
    parsed.version = version;
    parsed.mode = static_cast<delivery_mode>(mode);
    parsed.topic_id = topic_id;
    parsed.publisher_id = publisher_id;
    parsed.epoch = epoch;
    parsed.seq = seq;
    parsed.topic_seq = topic_seq;
    parsed.body = reader.remaining_bytes();
    value = parsed;
    return status{};
}

std::size_t encoded_subscribe_request_size(const subscribe_request& value) noexcept {
    return subscribe_request_fixed_size + value.topic.size();
}

status encode_subscribe_request(span<std::byte> out, const subscribe_request& value,
                                std::size_t& written) noexcept {
    written = 0;
    if (value.subscriber_id == 0 || !valid_mode(value.mode) ||
        value.topic.empty() || value.topic.size() > max_topic_length) {
        return status{rpc_code::invalid_argument};
    }
    const std::size_t size = encoded_subscribe_request_size(value);
    if (out.size() < size) {
        return status{rpc_code::encode_error};
    }
    byte_writer writer(out.first(size));
    writer.put_u64(value.subscriber_id);
    writer.put_u8(static_cast<u8>(value.mode));
    writer.put_u8(value.ordered ? 1 : 0);
    writer.put_u16(static_cast<u16>(value.topic.size()));
    writer.put_bytes(span<const std::byte>(
        reinterpret_cast<const std::byte*>(value.topic.data()), value.topic.size()));
    if (!writer.ok()) {
        return status{writer.error()};
    }
    written = size;
    return status{};
}

status decode_subscribe_request(span<const std::byte> in, subscribe_request& value) noexcept {
    byte_reader reader(in);
    u64 subscriber_id = 0;
    u8 mode = 0;
    u8 ordered = 0;
    u16 topic_len = 0;
    if (!reader.get_u64(subscriber_id) || !reader.get_u8(mode) ||
        !reader.get_u8(ordered) || !reader.get_u16(topic_len)) {
        return status{rpc_code::protocol_error};
    }
    if (subscriber_id == 0 || mode > static_cast<u8>(delivery_mode::unreliable) ||
        ordered > 1 || topic_len == 0 ||
        static_cast<u64>(topic_len) != static_cast<u64>(reader.remaining())) {
        return status{rpc_code::protocol_error};
    }
    const span<const std::byte> bytes = reader.remaining_bytes();
    subscribe_request parsed;
    parsed.subscriber_id = subscriber_id;
    parsed.mode = static_cast<delivery_mode>(mode);
    parsed.ordered = ordered != 0;
    parsed.topic.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    value = std::move(parsed);
    return status{};
}

std::size_t encoded_push_subscription_size(const push_subscription& value) noexcept {
    return push_subscription_fixed_size + value.topic.size();
}

status encode_push_subscription(span<std::byte> out, const push_subscription& value,
                                std::size_t& written) noexcept {
    written = 0;
    if (value.stream_id == 0 || value.topic_id == 0 || !valid_mode(value.mode) ||
        value.topic.empty() || value.topic.size() > max_topic_length) {
        return status{rpc_code::invalid_argument};
    }
    const std::size_t size = encoded_push_subscription_size(value);
    if (out.size() < size) {
        return status{rpc_code::encode_error};
    }
    byte_writer writer(out.first(size));
    writer.put_u32(value.stream_id);
    writer.put_u16(value.topic_id);
    writer.put_u8(static_cast<u8>(value.mode));
    writer.put_u8(value.ordered ? 1 : 0);
    writer.put_u16(static_cast<u16>(value.topic.size()));
    writer.put_bytes(span<const std::byte>(
        reinterpret_cast<const std::byte*>(value.topic.data()), value.topic.size()));
    if (!writer.ok()) {
        return status{writer.error()};
    }
    written = size;
    return status{};
}

status decode_push_subscription(span<const std::byte> in,
                                push_subscription& value) noexcept {
    byte_reader reader(in);
    u32 stream_id = 0;
    u16 topic_id = 0;
    u8 mode = 0;
    u8 ordered = 0;
    u16 topic_len = 0;
    if (!reader.get_u32(stream_id) || !reader.get_u16(topic_id) ||
        !reader.get_u8(mode) || !reader.get_u8(ordered) || !reader.get_u16(topic_len)) {
        return status{rpc_code::protocol_error};
    }
    if (stream_id == 0 || topic_id == 0 ||
        mode > static_cast<u8>(delivery_mode::unreliable) || ordered > 1 ||
        topic_len == 0 ||
        static_cast<u64>(topic_len) != static_cast<u64>(reader.remaining())) {
        return status{rpc_code::protocol_error};
    }
    const span<const std::byte> bytes = reader.remaining_bytes();
    push_subscription parsed;
    parsed.stream_id = stream_id;
    parsed.topic_id = topic_id;
    parsed.mode = static_cast<delivery_mode>(mode);
    parsed.ordered = ordered != 0;
    parsed.topic.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    value = std::move(parsed);
    return status{};
}

} // namespace crpc::push
