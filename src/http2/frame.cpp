// CRPC -- HTTP/2 frame layer implementation.
#include <crpc/http2/frame.hpp>

namespace crpc::http2 {
namespace {

constexpr u32 stream_id_mask = 0x7FFFFFFFu;
constexpr u32 length_mask = 0x00FFFFFFu;

} // namespace

const char* to_string(error_code code) noexcept {
    switch (code) {
    case error_code::no_error:
        return "NO_ERROR";
    case error_code::protocol_error:
        return "PROTOCOL_ERROR";
    case error_code::internal_error:
        return "INTERNAL_ERROR";
    case error_code::flow_control_error:
        return "FLOW_CONTROL_ERROR";
    case error_code::settings_timeout:
        return "SETTINGS_TIMEOUT";
    case error_code::stream_closed:
        return "STREAM_CLOSED";
    case error_code::frame_size_error:
        return "FRAME_SIZE_ERROR";
    case error_code::refused_stream:
        return "REFUSED_STREAM";
    case error_code::cancel:
        return "CANCEL";
    case error_code::compression_error:
        return "COMPRESSION_ERROR";
    case error_code::connect_error:
        return "CONNECT_ERROR";
    case error_code::enhance_your_calm:
        return "ENHANCE_YOUR_CALM";
    case error_code::inadequate_security:
        return "INADEQUATE_SECURITY";
    case error_code::http_1_1_required:
        return "HTTP_1_1_REQUIRED";
    }
    return "UNKNOWN";
}

rpc_code to_rpc_code(error_code code) noexcept {
    switch (code) {
    case error_code::no_error:
        return rpc_code::ok;
    case error_code::protocol_error:
        return rpc_code::protocol_error;
    case error_code::internal_error:
        return rpc_code::internal;
    case error_code::flow_control_error:
    case error_code::frame_size_error:
        return rpc_code::protocol_error;
    case error_code::settings_timeout:
        return rpc_code::deadline_exceeded;
    case error_code::stream_closed:
        return rpc_code::eof;
    case error_code::refused_stream:
        return rpc_code::unavailable;
    case error_code::cancel:
        return rpc_code::canceled;
    case error_code::compression_error:
        return rpc_code::decode_error;
    case error_code::connect_error:
        return rpc_code::failed_precondition;
    case error_code::enhance_your_calm:
        return rpc_code::resource_exhausted;
    case error_code::inadequate_security:
        return rpc_code::failed_precondition;
    case error_code::http_1_1_required:
        return rpc_code::failed_precondition;
    }
    return rpc_code::internal;
}

status encode_frame_header(span<std::byte> out, const frame_header& header) noexcept {
    if (out.size() < frame_header_size) {
        return rpc_code::encode_error;
    }
    frame_writer writer(out);
    const bool ok = writer.put_u24(header.length & length_mask) &&
                    writer.put_u8(static_cast<u8>(header.type)) &&
                    writer.put_u8(header.flags) &&
                    writer.put_u32(header.stream_id & stream_id_mask);
    return ok ? status{} : status(rpc_code::encode_error);
}

status decode_frame_header(span<const std::byte> in, frame_header& header,
                           u32 max_frame_size) noexcept {
    if (in.size() < frame_header_size) {
        return rpc_code::protocol_error;
    }
    frame_reader reader(in);
    u32 length = 0;
    u8 type = 0;
    u8 frame_flags = 0;
    u32 stream_id = 0;
    if (!reader.get_u24(length) || !reader.get_u8(type) || !reader.get_u8(frame_flags) ||
        !reader.get_u32(stream_id)) {
        return rpc_code::protocol_error;
    }
    if (length > max_frame_size) {
        return rpc_code::protocol_error;
    }
    frame_header decoded;
    decoded.length = length;
    decoded.type = static_cast<frame_type>(type);
    decoded.flags = frame_flags;
    decoded.stream_id = stream_id & stream_id_mask;
    header = decoded;
    return status{};
}

status encode_frame(span<std::byte> out, frame_header header,
                    span<const std::byte> payload, std::size_t& written) noexcept {
    written = 0;
    if (payload.size() > max_max_frame_size) {
        return rpc_code::encode_error;
    }
    if (out.size() < frame_header_size + payload.size()) {
        return rpc_code::encode_error;
    }
    header.length = static_cast<u32>(payload.size());
    const status header_status = encode_frame_header(out, header);
    if (!header_status) {
        return header_status;
    }
    for (std::size_t i = 0; i < payload.size(); ++i) {
        out[frame_header_size + i] = payload[i];
    }
    written = frame_header_size + payload.size();
    return status{};
}

status strip_padding(span<const std::byte> payload, bool padded,
                     span<const std::byte>& out) noexcept {
    if (!padded) {
        out = payload;
        return status{};
    }
    if (payload.empty()) {
        return rpc_code::protocol_error;
    }
    const std::size_t pad_length = static_cast<std::size_t>(payload[0]);
    if (pad_length + 1 > payload.size()) {
        return rpc_code::protocol_error;
    }
    out = payload.subspan(1, payload.size() - 1 - pad_length);
    return status{};
}

status decode_data(span<const std::byte> payload, u8 frame_flags,
                   span<const std::byte>& out) noexcept {
    return strip_padding(payload, (frame_flags & flags::padded) != 0, out);
}

status decode_headers(span<const std::byte> payload, u8 frame_flags,
                      headers_payload& out) noexcept {
    span<const std::byte> body;
    const status padding = strip_padding(payload, (frame_flags & flags::padded) != 0, body);
    if (!padding) {
        return padding;
    }
    out.end_stream = (frame_flags & flags::end_stream) != 0;
    out.end_headers = (frame_flags & flags::end_headers) != 0;
    out.has_priority = (frame_flags & flags::priority) != 0;
    out.priority = priority_payload{};
    if (out.has_priority) {
        if (body.size() < 5) {
            return rpc_code::protocol_error;
        }
        frame_reader reader(body);
        u32 dependency = 0;
        u8 weight = 0;
        if (!reader.get_u32(dependency) || !reader.get_u8(weight)) {
            return rpc_code::protocol_error;
        }
        out.priority.exclusive = (dependency & 0x80000000u) != 0;
        out.priority.stream_dependency = dependency & stream_id_mask;
        out.priority.weight = weight;
        out.header_block = body.subspan(5);
    } else {
        out.header_block = body;
    }
    return status{};
}

status decode_push_promise(span<const std::byte> payload, u8 frame_flags,
                           push_promise_payload& out) noexcept {
    span<const std::byte> body;
    const status padding = strip_padding(payload, (frame_flags & flags::padded) != 0, body);
    if (!padding) {
        return padding;
    }
    if (body.size() < 4) {
        return rpc_code::protocol_error;
    }
    frame_reader reader(body);
    u32 promised = 0;
    if (!reader.get_u32(promised)) {
        return rpc_code::protocol_error;
    }
    out.promised_stream_id = promised & stream_id_mask;
    out.header_block = body.subspan(4);
    return status{};
}

status decode_priority(span<const std::byte> payload, priority_payload& out) noexcept {
    if (payload.size() != 5) {
        return rpc_code::protocol_error;
    }
    frame_reader reader(payload);
    u32 dependency = 0;
    u8 weight = 0;
    if (!reader.get_u32(dependency) || !reader.get_u8(weight)) {
        return rpc_code::protocol_error;
    }
    priority_payload decoded;
    decoded.exclusive = (dependency & 0x80000000u) != 0;
    decoded.stream_dependency = dependency & stream_id_mask;
    decoded.weight = weight;
    out = decoded;
    return status{};
}

status encode_priority(span<std::byte> out, const priority_payload& value,
                       std::size_t& written) noexcept {
    written = 0;
    if (out.size() < 5) {
        return rpc_code::encode_error;
    }
    frame_writer writer(out);
    u32 dependency = value.stream_dependency & stream_id_mask;
    if (value.exclusive) {
        dependency |= 0x80000000u;
    }
    if (!writer.put_u32(dependency) || !writer.put_u8(value.weight)) {
        return rpc_code::encode_error;
    }
    written = writer.written();
    return status{};
}

status decode_rst_stream(span<const std::byte> payload, error_code& code) noexcept {
    if (payload.size() != 4) {
        return rpc_code::protocol_error;
    }
    frame_reader reader(payload);
    u32 raw = 0;
    if (!reader.get_u32(raw)) {
        return rpc_code::protocol_error;
    }
    code = static_cast<error_code>(raw);
    return status{};
}

status encode_rst_stream(span<std::byte> out, error_code code,
                         std::size_t& written) noexcept {
    written = 0;
    if (out.size() < 4) {
        return rpc_code::encode_error;
    }
    frame_writer writer(out);
    if (!writer.put_u32(static_cast<u32>(code))) {
        return rpc_code::encode_error;
    }
    written = writer.written();
    return status{};
}

status decode_goaway(span<const std::byte> payload, goaway_payload& out) noexcept {
    if (payload.size() < 8) {
        return rpc_code::protocol_error;
    }
    frame_reader reader(payload);
    u32 last = 0;
    u32 raw = 0;
    if (!reader.get_u32(last) || !reader.get_u32(raw)) {
        return rpc_code::protocol_error;
    }
    goaway_payload decoded;
    decoded.last_stream_id = last & stream_id_mask;
    decoded.code = static_cast<error_code>(raw);
    decoded.debug = payload.subspan(8);
    out = decoded;
    return status{};
}

status encode_goaway(span<std::byte> out, const goaway_payload& value,
                     std::size_t& written) noexcept {
    written = 0;
    if (out.size() < 8 + value.debug.size()) {
        return rpc_code::encode_error;
    }
    frame_writer writer(out);
    if (!writer.put_u32(value.last_stream_id & stream_id_mask) ||
        !writer.put_u32(static_cast<u32>(value.code)) ||
        !writer.put_bytes(value.debug)) {
        return rpc_code::encode_error;
    }
    written = writer.written();
    return status{};
}

status decode_window_update(span<const std::byte> payload, u32& increment) noexcept {
    if (payload.size() != 4) {
        return rpc_code::protocol_error;
    }
    frame_reader reader(payload);
    u32 raw = 0;
    if (!reader.get_u32(raw)) {
        return rpc_code::protocol_error;
    }
    increment = raw & stream_id_mask;
    return status{};
}

status encode_window_update(span<std::byte> out, u32 increment,
                            std::size_t& written) noexcept {
    written = 0;
    if (out.size() < 4) {
        return rpc_code::encode_error;
    }
    frame_writer writer(out);
    if (!writer.put_u32(increment & stream_id_mask)) {
        return rpc_code::encode_error;
    }
    written = writer.written();
    return status{};
}

status decode_ping(span<const std::byte> payload, ping_opaque& opaque) noexcept {
    if (payload.size() != 8) {
        return rpc_code::protocol_error;
    }
    for (std::size_t i = 0; i < 8; ++i) {
        opaque[i] = payload[i];
    }
    return status{};
}

status encode_ping(span<std::byte> out, const ping_opaque& opaque,
                   std::size_t& written) noexcept {
    written = 0;
    if (out.size() < 8) {
        return rpc_code::encode_error;
    }
    for (std::size_t i = 0; i < 8; ++i) {
        out[i] = opaque[i];
    }
    written = 8;
    return status{};
}

status decode_settings(span<const std::byte> payload,
                       std::vector<setting>& out) noexcept {
    if (payload.size() % 6 != 0) {
        return rpc_code::protocol_error;
    }
    std::vector<setting> parsed;
    parsed.reserve(payload.size() / 6);
    for (std::size_t offset = 0; offset < payload.size(); offset += 6) {
        frame_reader reader(payload.subspan(offset, 6));
        u16 id = 0;
        u32 value = 0;
        if (!reader.get_u16(id) || !reader.get_u32(value)) {
            return rpc_code::protocol_error;
        }
        parsed.push_back(setting{id, value});
    }
    out = std::move(parsed);
    return status{};
}

status encode_settings(span<std::byte> out, span<const setting> entries,
                       std::size_t& written) noexcept {
    written = 0;
    if (out.size() < entries.size() * 6) {
        return rpc_code::encode_error;
    }
    frame_writer writer(out);
    for (const setting& entry : entries) {
        if (!writer.put_u16(entry.id) || !writer.put_u32(entry.value)) {
            return rpc_code::encode_error;
        }
    }
    written = writer.written();
    return status{};
}

} // namespace crpc::http2
