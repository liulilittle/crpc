// CRPC -- AMQP 0-9-1 wire codec implementation.
#include <crpc/mq/amqp/protocol.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace crpc {
namespace mq {
namespace amqp {
namespace {

void put_u8(std::vector<std::byte>& out, std::uint8_t value) {
    out.push_back(static_cast<std::byte>(value));
}

void put_u16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::byte>(value & 0xFFu));
}

void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
    out.push_back(static_cast<std::byte>((value >> 24) & 0xFFu));
    out.push_back(static_cast<std::byte>((value >> 16) & 0xFFu));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xFFu));
    out.push_back(static_cast<std::byte>(value & 0xFFu));
}

void put_u64(std::vector<std::byte>& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
    }
}

void put_short_string(std::vector<std::byte>& out, std::string_view value,
                      rpc_code& error) {
    if (value.size() > 255) {
        error = rpc_code::encode_error;
        return;
    }
put_u8(out, static_cast<std::uint8_t>(value.size()));
    if (!value.empty()) {
        out.insert(out.end(), reinterpret_cast<const std::byte*>(value.data()),
                   reinterpret_cast<const std::byte*>(value.data()) + value.size());
    }
}

void put_long_string(std::vector<std::byte>& out, std::string_view value,
                     rpc_code& error) {
    if (value.size() > static_cast<std::size_t>(0xFFFFFFFFu)) {
        error = rpc_code::encode_error;
        return;
    }
put_u32(out, static_cast<std::uint32_t>(value.size()));
    if (!value.empty()) {
        out.insert(out.end(), reinterpret_cast<const std::byte*>(value.data()),
                   reinterpret_cast<const std::byte*>(value.data()) + value.size());
    }
}

void put_long_bytes(std::vector<std::byte>& out, span<const std::byte> value,
                    rpc_code& error) {
    if (value.size() > static_cast<std::size_t>(0xFFFFFFFFu)) {
        error = rpc_code::encode_error;
        return;
    }
put_u32(out, static_cast<std::uint32_t>(value.size()));
    if (!value.empty()) {
        out.insert(out.end(), value.data(), value.data() + value.size());
    }
}

void append_field_value(std::vector<std::byte>& out, const field_value& value,
                        rpc_code& error);

void append_field_table(std::vector<std::byte>& out, const field_table& table,
                        rpc_code& error) {
    std::vector<std::byte> body;
    for (const field_table::entry_type& entry : table.entries()) {
        put_short_string(body, entry.first, error);
        if (error != rpc_code::ok) {
            return;
        }
        append_field_value(body, entry.second, error);
        if (error != rpc_code::ok) {
            return;
        }
    }
    if (body.size() > static_cast<std::size_t>(0xFFFFFFFFu)) {
        error = rpc_code::encode_error;
        return;
    }
    put_u32(out, static_cast<std::uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
}

void append_field_value(std::vector<std::byte>& out, const field_value& value,
                        rpc_code& error) {
    put_u8(out, static_cast<std::uint8_t>(value.type));
    switch (value.type) {
    case field_type::boolean:
        put_u8(out, static_cast<std::uint8_t>(value.boolean ? 1u : 0u));
        break;
    case field_type::int8:
        put_u8(out, static_cast<std::uint8_t>(static_cast<std::int8_t>(
                       value.signed_integer)));
        break;
    case field_type::uint8:
        put_u8(out, static_cast<std::uint8_t>(value.unsigned_integer));
        break;
    case field_type::int16:
        put_u16(out, static_cast<std::uint16_t>(static_cast<std::int16_t>(
                        value.signed_integer)));
        break;
    case field_type::uint16:
        put_u16(out, static_cast<std::uint16_t>(value.unsigned_integer));
        break;
    case field_type::int32:
        put_u32(out, static_cast<std::uint32_t>(static_cast<std::int32_t>(
                        value.signed_integer)));
        break;
    case field_type::uint32:
        put_u32(out, static_cast<std::uint32_t>(value.unsigned_integer));
        break;
    case field_type::int64:
        put_u64(out, static_cast<std::uint64_t>(value.signed_integer));
        break;
    case field_type::uint64:
        put_u64(out, value.unsigned_integer);
        break;
    case field_type::float32: {
        const float narrowed = static_cast<float>(value.real);
        std::uint32_t bits = 0;
        std::memcpy(&bits, &narrowed, sizeof(bits));
        put_u32(out, bits);
        break;
    }
    case field_type::float64: {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value.real, sizeof(bits));
        put_u64(out, bits);
        break;
    }
    case field_type::decimal:
        put_u8(out, value.decimal_scale);
        put_u32(out, value.decimal_value);
        break;
    case field_type::short_string:
        put_short_string(out, value.text, error);
        break;
    case field_type::long_string:
        put_long_string(out, value.text, error);
        break;
    case field_type::array: {
        std::vector<std::byte> body;
        for (const field_value& item : value.array) {
            append_field_value(body, item, error);
            if (error != rpc_code::ok) {
                return;
            }
        }
        if (body.size() > static_cast<std::size_t>(0xFFFFFFFFu)) {
            error = rpc_code::encode_error;
            return;
        }
        put_u32(out, static_cast<std::uint32_t>(body.size()));
        out.insert(out.end(), body.begin(), body.end());
        break;
    }
    case field_type::timestamp:
        put_u64(out, value.timestamp);
        break;
    case field_type::table:
        if (value.table != nullptr) {
            append_field_table(out, *value.table, error);
        } else {
            put_u32(out, 0);
        }
        break;
    case field_type::null_value:
        break;
    case field_type::bytes:
        put_long_bytes(out, span<const std::byte>(value.bytes.data(), value.bytes.size()),
                       error);
        break;
    }
}

constexpr std::uint16_t property_content_type = 1u << 15;
constexpr std::uint16_t property_content_encoding = 1u << 14;
constexpr std::uint16_t property_headers = 1u << 13;
constexpr std::uint16_t property_delivery_mode = 1u << 12;
constexpr std::uint16_t property_priority = 1u << 11;
constexpr std::uint16_t property_correlation_id = 1u << 10;
constexpr std::uint16_t property_reply_to = 1u << 9;
constexpr std::uint16_t property_expiration = 1u << 8;
constexpr std::uint16_t property_message_id = 1u << 7;
constexpr std::uint16_t property_timestamp = 1u << 6;
constexpr std::uint16_t property_type = 1u << 5;
constexpr std::uint16_t property_user_id = 1u << 4;
constexpr std::uint16_t property_app_id = 1u << 3;
constexpr std::uint16_t property_cluster_id = 1u << 2;

} // namespace

// --- Frames ------------------------------------------------------------------

void append_frame(std::vector<std::byte>& out, frame_type type, std::uint16_t channel,
                  span<const std::byte> payload) {
    put_u8(out, static_cast<std::uint8_t>(type));
    put_u16(out, channel);
put_u32(out, static_cast<std::uint32_t>(payload.size()));
    if (!payload.empty()) {
        out.insert(out.end(), payload.data(), payload.data() + payload.size());
    }
    out.push_back(frame_end_octet);
}

std::vector<std::byte> encode_frame(frame_type type, std::uint16_t channel,
                                    span<const std::byte> payload) {
    std::vector<std::byte> out;
    out.reserve(frame_overhead + payload.size());
    append_frame(out, type, channel, payload);
    return out;
}

void append_method_frame(std::vector<std::byte>& out, std::uint16_t channel,
                         std::uint16_t method_class, std::uint16_t method,
                         span<const std::byte> arguments) {
    std::vector<std::byte> payload;
    payload.reserve(4 + arguments.size());
    put_u16(payload, method_class);
    put_u16(payload, method);
    if (!arguments.empty()) {
        payload.insert(payload.end(), arguments.data(), arguments.data() + arguments.size());
    }
    append_frame(out, frame_type::method, channel,
                 span<const std::byte>(payload.data(), payload.size()));
}

status decode_frame_header(span<const std::byte> in, frame_header& out) noexcept {
    if (in.size() < 7) {
        return status{rpc_code::protocol_error, "an AMQP frame header is seven bytes"};
    }
    const std::uint8_t type_octet = static_cast<std::uint8_t>(in[0]);
    if (type_octet != static_cast<std::uint8_t>(frame_type::method) &&
        type_octet != static_cast<std::uint8_t>(frame_type::header) &&
        type_octet != static_cast<std::uint8_t>(frame_type::body) &&
        type_octet != static_cast<std::uint8_t>(frame_type::heartbeat)) {
        return status{rpc_code::protocol_error, "unknown AMQP frame type"};
    }
    out.type = static_cast<frame_type>(type_octet);
    out.channel = static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[1]) << 8) |
                                             static_cast<std::uint16_t>(in[2]));
    out.size = (static_cast<std::uint32_t>(in[3]) << 24) |
               (static_cast<std::uint32_t>(in[4]) << 16) |
               (static_cast<std::uint32_t>(in[5]) << 8) |
               static_cast<std::uint32_t>(in[6]);
    if (out.size > max_frame_payload) {
        return status{rpc_code::protocol_error, "AMQP frame payload is too large"};
    }
    // A heartbeat frame has no channel and no payload. Enforcing both catches a
    // peer that confused a heartbeat with a method frame early.
    if (out.type == frame_type::heartbeat && (out.channel != 0 || out.size != 0)) {
        return status{rpc_code::protocol_error, "malformed AMQP heartbeat frame"};
    }
    return status{};
}

status decode_frame(span<const std::byte> in, frame& out, std::size_t& consumed) noexcept {
    consumed = 0;
    if (in.size() < frame_overhead) {
        return status{rpc_code::protocol_error, "truncated AMQP frame"};
    }
    frame_header header;
    const status head = decode_frame_header(in.first(7), header);
    if (!head) {
        return head;
    }
    const std::size_t total = frame_overhead + static_cast<std::size_t>(header.size);
    if (in.size() < total) {
        return status{rpc_code::protocol_error, "truncated AMQP frame payload"};
    }
    if (in[total - 1] != frame_end_octet) {
        // The end octet is the only check that detects a stream that lost or
        // gained a byte; accepting the frame silently would desynchronise every
        // following frame.
        return status{rpc_code::protocol_error, "AMQP frame-end octet is not 0xCE"};
    }
    out.type = header.type;
    out.channel = header.channel;
    out.payload.assign(in.data() + 7, in.data() + 7 + header.size);
    consumed = total;
    return status{};
}

// --- Writer ------------------------------------------------------------------

void writer::fail(rpc_code code) noexcept {
    if (error_ == rpc_code::ok) {
        error_ = code;
    }
}

void writer::u8(std::uint8_t value) {
    if (!ok()) {
        return;
    }
    put_u8(data_, value);
}

void writer::u16(std::uint16_t value) {
    if (!ok()) {
        return;
    }
    put_u16(data_, value);
}

void writer::u32(std::uint32_t value) {
    if (!ok()) {
        return;
    }
    put_u32(data_, value);
}

void writer::u64(std::uint64_t value) {
    if (!ok()) {
        return;
    }
    put_u64(data_, value);
}

void writer::bits(std::uint8_t value, std::uint8_t count) {
    if (!ok()) {
        return;
    }
    if (count > 8) {
        fail(rpc_code::encode_error);
        return;
    }
    const std::uint8_t mask =
        count == 8 ? 0xFFu : static_cast<std::uint8_t>((1u << count) - 1u);
    put_u8(data_, static_cast<std::uint8_t>(value & mask));
}

void writer::short_string(std::string_view value) {
    if (!ok()) {
        return;
    }
    put_short_string(data_, value, error_);
}

void writer::long_string(std::string_view value) {
    if (!ok()) {
        return;
    }
    put_long_string(data_, value, error_);
}

void writer::long_string(span<const std::byte> value) {
    if (!ok()) {
        return;
    }
    put_long_bytes(data_, value, error_);
}

void writer::table(const field_table& value) {
    if (!ok()) {
        return;
    }
    append_field_table(data_, value, error_);
}

void writer::properties(const message_properties& value) {
    if (!ok()) {
        return;
    }
    std::uint16_t flags = 0;
    if (!value.content_type.empty()) {
        flags = static_cast<std::uint16_t>(flags | property_content_type);
    }
    if (!value.content_encoding.empty()) {
        flags = static_cast<std::uint16_t>(flags | property_content_encoding);
    }
    if (!value.headers.empty()) {
        flags = static_cast<std::uint16_t>(flags | property_headers);
    }
    if (value.delivery_mode.has_value()) {
        flags = static_cast<std::uint16_t>(flags | property_delivery_mode);
    }
    if (value.priority.has_value()) {
        flags = static_cast<std::uint16_t>(flags | property_priority);
    }
    if (!value.correlation_id.empty()) {
        flags = static_cast<std::uint16_t>(flags | property_correlation_id);
    }
    if (!value.reply_to.empty()) {
        flags = static_cast<std::uint16_t>(flags | property_reply_to);
    }
    if (!value.expiration.empty()) {
        flags = static_cast<std::uint16_t>(flags | property_expiration);
    }
    if (!value.message_id.empty()) {
        flags = static_cast<std::uint16_t>(flags | property_message_id);
    }
    if (value.timestamp.has_value()) {
        flags = static_cast<std::uint16_t>(flags | property_timestamp);
    }
    if (!value.type.empty()) {
        flags = static_cast<std::uint16_t>(flags | property_type);
    }
    if (!value.user_id.empty()) {
        flags = static_cast<std::uint16_t>(flags | property_user_id);
    }
    if (!value.app_id.empty()) {
        flags = static_cast<std::uint16_t>(flags | property_app_id);
    }
    if (!value.cluster_id.empty()) {
        flags = static_cast<std::uint16_t>(flags | property_cluster_id);
    }

    u16(flags);
    if ((flags & property_content_type) != 0) {
        short_string(value.content_type);
    }
    if ((flags & property_content_encoding) != 0) {
        short_string(value.content_encoding);
    }
    if ((flags & property_headers) != 0) {
        table(value.headers);
    }
    if ((flags & property_delivery_mode) != 0) {
        u8(value.delivery_mode.value_or(0));
    }
    if ((flags & property_priority) != 0) {
        u8(value.priority.value_or(0));
    }
    if ((flags & property_correlation_id) != 0) {
        short_string(value.correlation_id);
    }
    if ((flags & property_reply_to) != 0) {
        short_string(value.reply_to);
    }
    if ((flags & property_expiration) != 0) {
        short_string(value.expiration);
    }
    if ((flags & property_message_id) != 0) {
        short_string(value.message_id);
    }
    if ((flags & property_timestamp) != 0) {
        u64(value.timestamp.value_or(0));
    }
    if ((flags & property_type) != 0) {
        short_string(value.type);
    }
    if ((flags & property_user_id) != 0) {
        short_string(value.user_id);
    }
    if ((flags & property_app_id) != 0) {
        short_string(value.app_id);
    }
    if ((flags & property_cluster_id) != 0) {
        short_string(value.cluster_id);
    }
}

// --- Reader ------------------------------------------------------------------

reader::reader(span<const std::byte> data) noexcept : data_(data) {}

void reader::fail(rpc_code code) noexcept {
    if (error_ == rpc_code::ok) {
        error_ = code;
    }
}

bool reader::need(std::size_t count) noexcept {
    if (!ok()) {
        return false;
    }
    if (count > data_.size() - offset_) {
        fail(rpc_code::protocol_error);
        return false;
    }
    return true;
}

std::uint8_t reader::u8() noexcept {
    if (!need(1)) {
        return 0;
    }
    const std::uint8_t value = static_cast<std::uint8_t>(data_[offset_]);
    ++offset_;
    return value;
}

std::uint16_t reader::u16() noexcept {
    if (!need(2)) {
        return 0;
    }
    const std::uint16_t value =
        static_cast<std::uint16_t>((static_cast<std::uint16_t>(data_[offset_]) << 8) |
                                   static_cast<std::uint16_t>(data_[offset_ + 1]));
    offset_ += 2;
    return value;
}

std::uint32_t reader::u32() noexcept {
    if (!need(4)) {
        return 0;
    }
    const std::uint32_t value =
        (static_cast<std::uint32_t>(data_[offset_]) << 24) |
        (static_cast<std::uint32_t>(data_[offset_ + 1]) << 16) |
        (static_cast<std::uint32_t>(data_[offset_ + 2]) << 8) |
        static_cast<std::uint32_t>(data_[offset_ + 3]);
    offset_ += 4;
    return value;
}

std::uint64_t reader::u64() noexcept {
    if (!need(8)) {
        return 0;
    }
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<std::uint64_t>(data_[offset_ + static_cast<std::size_t>(i)]);
    }
    offset_ += 8;
    return value;
}

bool reader::bits(std::uint8_t count, std::uint8_t& value) noexcept {
    if (count == 0 || count > 8) {
        fail(rpc_code::protocol_error);
        return false;
    }
    const std::uint8_t octet = u8();
    if (!ok()) {
        return false;
    }
    const std::uint8_t mask =
        count == 8 ? 0xFFu : static_cast<std::uint8_t>((1u << count) - 1u);
    value = static_cast<std::uint8_t>(octet & mask);
    return true;
}

std::string_view reader::short_string() noexcept {
    const std::uint8_t length = u8();
    if (!ok()) {
        return {};
    }
    if (!need(static_cast<std::size_t>(length))) {
        return {};
    }
    const char* text = data_.data() == nullptr
                           ? ""
                           : reinterpret_cast<const char*>(data_.data() + offset_);
    const std::string_view value(text, static_cast<std::size_t>(length));
    offset_ += static_cast<std::size_t>(length);
    return value;
}

std::string_view reader::long_string() noexcept {
    const std::uint32_t length = u32();
    if (!ok()) {
        return {};
    }
    if (!need(static_cast<std::size_t>(length))) {
        return {};
    }
    const char* text = data_.data() == nullptr
                           ? ""
                           : reinterpret_cast<const char*>(data_.data() + offset_);
    const std::string_view value(text, static_cast<std::size_t>(length));
    offset_ += static_cast<std::size_t>(length);
    return value;
}

bool reader::skip(std::size_t count) noexcept {
    if (!need(count)) {
        return false;
    }
    offset_ += count;
    return true;
}

bool reader::read_array(std::vector<field_value>& out) noexcept {
    const std::uint32_t length = u32();
    if (!ok()) {
        return false;
    }
    if (static_cast<std::size_t>(length) > remaining()) {
        fail(rpc_code::protocol_error);
        return false;
    }
    if (data_.data() == nullptr && length != 0) {
        fail(rpc_code::protocol_error);
        return false;
    }
    const span<const std::byte> content = data_.subspan(offset_, length);
    offset_ += static_cast<std::size_t>(length);
    reader inner(content);
    while (inner.remaining() > 0) {
        field_value item;
        if (!inner.read_value(item)) {
            fail(inner.error());
            return false;
        }
        out.push_back(std::move(item));
    }
    return true;
}

bool reader::read_value(field_value& out) noexcept {
    const std::uint8_t type_octet = u8();
    if (!ok()) {
        return false;
    }
    out.type = static_cast<field_type>(type_octet);
    switch (out.type) {
    case field_type::boolean:
        out.boolean = u8() != 0;
        break;
    case field_type::int8:
        out.signed_integer = static_cast<std::int8_t>(u8());
        break;
    case field_type::uint8:
        out.unsigned_integer = u8();
        break;
    case field_type::int16:
        out.signed_integer = static_cast<std::int16_t>(u16());
        break;
    case field_type::uint16:
        out.unsigned_integer = u16();
        break;
    case field_type::int32:
        out.signed_integer = static_cast<std::int32_t>(u32());
        break;
    case field_type::uint32:
        out.unsigned_integer = u32();
        break;
    case field_type::int64:
        out.signed_integer = static_cast<std::int64_t>(u64());
        break;
    case field_type::uint64:
        out.unsigned_integer = u64();
        break;
    case field_type::float32: {
        const std::uint32_t bits = u32();
        float value = 0.0F;
        std::memcpy(&value, &bits, sizeof(value));
        out.real = static_cast<double>(value);
        break;
    }
    case field_type::float64: {
        const std::uint64_t bits = u64();
        std::memcpy(&out.real, &bits, sizeof(out.real));
        break;
    }
    case field_type::decimal:
        out.decimal_scale = u8();
        out.decimal_value = u32();
        break;
    case field_type::short_string:
        out.text = std::string(short_string());
        break;
    case field_type::long_string:
        out.text = std::string(long_string());
        break;
    case field_type::array:
        if (!read_array(out.array)) {
            return false;
        }
        break;
    case field_type::timestamp:
        out.timestamp = u64();
        break;
    case field_type::table:
        out.table = std::unique_ptr<field_table>(new field_table());
        if (!read_table(*out.table)) {
            return false;
        }
        break;
    case field_type::null_value:
        break;
    case field_type::bytes: {
        const std::uint32_t length = u32();
        if (!ok()) {
            return false;
        }
        if (static_cast<std::size_t>(length) > remaining()) {
            fail(rpc_code::protocol_error);
            return false;
        }
        if (length != 0) {
            out.bytes.assign(data_.data() + offset_, data_.data() + offset_ + length);
        }
        offset_ += static_cast<std::size_t>(length);
        break;
    }
    default:
        // An unknown type octet makes the payload length unknowable, so there is
        // no safe way to continue decoding this table.
        fail(rpc_code::protocol_error);
        return false;
    }
    return ok();
}

bool reader::read_table(field_table& out) noexcept {
    out.clear();
    const std::uint32_t length = u32();
    if (!ok()) {
        return false;
    }
    if (static_cast<std::size_t>(length) > remaining()) {
        fail(rpc_code::protocol_error);
        return false;
    }
    const span<const std::byte> content = data_.subspan(offset_, length);
    offset_ += static_cast<std::size_t>(length);
    reader inner(content);
    while (inner.remaining() > 0) {
        const std::string_view key = inner.short_string();
        if (!inner.ok()) {
            fail(inner.error());
            return false;
        }
        field_value value;
        if (!inner.read_value(value)) {
            fail(inner.error());
            return false;
        }
        out.set(std::string(key), std::move(value));
    }
    return true;
}

bool reader::read_properties(message_properties& out) noexcept {
    const std::uint16_t flags = u16();
    if (!ok()) {
        return false;
    }
    if ((flags & 0x0003u) != 0) {
        fail(rpc_code::protocol_error);
        return false;
    }
    if ((flags & property_content_type) != 0) {
        out.content_type = std::string(short_string());
    }
    if ((flags & property_content_encoding) != 0) {
        out.content_encoding = std::string(short_string());
    }
    if ((flags & property_headers) != 0) {
        if (!read_table(out.headers)) {
            return false;
        }
    }
    if ((flags & property_delivery_mode) != 0) {
        out.delivery_mode = u8();
    }
    if ((flags & property_priority) != 0) {
        out.priority = u8();
    }
    if ((flags & property_correlation_id) != 0) {
        out.correlation_id = std::string(short_string());
    }
    if ((flags & property_reply_to) != 0) {
        out.reply_to = std::string(short_string());
    }
    if ((flags & property_expiration) != 0) {
        out.expiration = std::string(short_string());
    }
    if ((flags & property_message_id) != 0) {
        out.message_id = std::string(short_string());
    }
    if ((flags & property_timestamp) != 0) {
        out.timestamp = u64();
    }
    if ((flags & property_type) != 0) {
        out.type = std::string(short_string());
    }
    if ((flags & property_user_id) != 0) {
        out.user_id = std::string(short_string());
    }
    if ((flags & property_app_id) != 0) {
        out.app_id = std::string(short_string());
    }
    if ((flags & property_cluster_id) != 0) {
        out.cluster_id = std::string(short_string());
    }
    return ok();
}

// --- Content -----------------------------------------------------------------

status append_content_frames(std::vector<std::byte>& out, std::uint16_t channel,
                             std::uint16_t content_class,
                             const message_properties& properties,
                             span<const std::byte> body, std::uint32_t frame_max) {
    if (frame_max <= frame_overhead) {
        return status{rpc_code::encode_error,
                      "frame_max must leave room for the frame overhead"};
    }
    writer header;
    header.u16(content_class);
    header.u16(0); // weight, always zero in AMQP 0-9-1
    header.u64(static_cast<std::uint64_t>(body.size()));
    header.properties(properties);
    if (!header.ok()) {
        return status{header.error(), "the content header properties do not fit"};
    }
    append_frame(out, frame_type::header, channel,
                 span<const std::byte>(header.data().data(), header.data().size()));

    const std::size_t max_body = static_cast<std::size_t>(frame_max) - frame_overhead;
    std::size_t offset = 0;
    while (offset < body.size()) {
        const std::size_t take = std::min(max_body, body.size() - offset);
        append_frame(out, frame_type::body, channel, body.subspan(offset, take));
        offset += take;
    }
    return status{};
}

std::size_t content_frame_count(std::size_t body_size, std::uint32_t frame_max) noexcept {
    if (frame_max <= frame_overhead) {
        return 0;
    }
    const std::size_t max_body = static_cast<std::size_t>(frame_max) - frame_overhead;
    if (body_size == 0) {
        return 1;
    }
    return 1 + (body_size + max_body - 1) / max_body;
}

// --- Errors ------------------------------------------------------------------

rpc_code map_reply_code(std::uint16_t reply_code) noexcept {
    switch (reply_code) {
    case 200: // reply success (only sent by a client in this client)
        return rpc_code::ok;
    case 311: // content too large
        return rpc_code::invalid_argument;
    case 312: // no route
        return rpc_code::not_found;
    case 313: // no consumers
        return rpc_code::unavailable;
    case 320: // connection forced
        return rpc_code::connection_reset;
    case 402: // invalid path
        return rpc_code::invalid_argument;
    case 403: // access refused
        return rpc_code::application;
    case 404: // not found
        return rpc_code::not_found;
    case 405: // resource locked
        return rpc_code::failed_precondition;
    case 406: // precondition failed
        return rpc_code::failed_precondition;
    case 501: // frame error
    case 502: // syntax error
    case 504: // channel error
    case 505: // unexpected frame
        return rpc_code::protocol_error;
    case 503: // command invalid
        return rpc_code::no_such_method;
    case 506: // resource error
        return rpc_code::resource_exhausted;
    case 530: // not allowed
        return rpc_code::failed_precondition;
    case 540: // not implemented
        return rpc_code::no_such_method;
    case 541: // internal error
        return rpc_code::internal;
    default:
        return rpc_code::application;
    }
}

} // namespace amqp
} // namespace mq
} // namespace crpc