// CRPC -- frame codec implementation.
//
// Everything here is byte-wise: no struct memcpy, no unaligned integer access,
// no bitfields. Multi-byte values go through endian::store_le*/load_le* and the
// bounds-checked byte_reader/byte_writer.
#include <algorithm>
#include <limits>

#include <crpc/frame.hpp>
#include <crpc/frame_compression.hpp>
#include <crpc/wire.hpp>

#include "frame_internal.hpp"

namespace crpc {
namespace {

// --- CRC32C ------------------------------------------------------------------

constexpr u32 crc32c_polynomial = 0x82F63B78u;

constexpr u32 make_crc32c_entry(u32 index) noexcept {
    u32 value = index;
    for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1u) != 0 ? (crc32c_polynomial ^ (value >> 1)) : (value >> 1);
    }
    return value;
}

struct crc32c_table {
    u32 entries[256];
};

constexpr crc32c_table make_crc32c_table() noexcept {
    crc32c_table table{};
    for (u32 i = 0; i < 256; ++i) {
        table.entries[i] = make_crc32c_entry(i);
    }
    return table;
}

constexpr crc32c_table crc32c_lookup = make_crc32c_table();

} // namespace

namespace detail {

// --- Validation --------------------------------------------------------------

// Validates every semantic rule of a header that can be checked without the
// payload. Returns rpc_code::ok or rpc_code::protocol_error.
[[nodiscard]] rpc_code validate_header(const frame_header& header, u32 max_frame) noexcept {
    if (!is_known_frame_type(header.type)) {
        return rpc_code::protocol_error;
    }
    if ((header.stream_id & 0x80000000u) != 0) {
        return rpc_code::protocol_error;
    }
    if (header.payload_len > max_frame) {
        return rpc_code::protocol_error;
    }

    const u8 algorithm = static_cast<u8>((header.flags & frame_flag_algo_mask) >> frame_flag_algo_shift);
    const bool compressed = (header.flags & frame_flag_compressed) != 0;
    if (algorithm > static_cast<u8>(compression_algorithm::gzip)) {
        return rpc_code::protocol_error;
    }
    if (!compressed && algorithm != static_cast<u8>(compression_algorithm::none)) {
        return rpc_code::protocol_error;
    }
    if (compressed && algorithm == static_cast<u8>(compression_algorithm::none)) {
        return rpc_code::protocol_error;
    }

    if ((header.flags & frame_flag_crc32c) == 0 && header.ext != 0) {
        return rpc_code::protocol_error;
    }

    if (header.type == frame_type::one_way && (header.flags & frame_flag_unreliable) == 0) {
        return rpc_code::protocol_error;
    }

    switch (header.type) {
    case frame_type::request:
    case frame_type::response:
    case frame_type::data:
    case frame_type::one_way:
    case frame_type::push:
        if (header.stream_id == 0) {
            return rpc_code::protocol_error;
        }
        break;
    case frame_type::settings:
    case frame_type::settings_ack:
    case frame_type::ping:
    case frame_type::pong:
    case frame_type::goaway:
        if (header.stream_id != 0) {
            return rpc_code::protocol_error;
        }
        break;
    case frame_type::rst_stream:
    case frame_type::push_ack:
        if (header.stream_id == 0) {
            return rpc_code::protocol_error;
        }
        break;
    case frame_type::window_update:
        // WINDOW_UPDATE is the only type valid for both a stream and the
        // connection.
        break;
    }
    return rpc_code::ok;
}

[[nodiscard]] rpc_code encode_error_for(rpc_code code) noexcept {
    return code == rpc_code::protocol_error ? rpc_code::invalid_argument : code;
}

} // namespace detail

namespace {

[[nodiscard]] u32 clamp_max_frame(u32 value) noexcept {
    return value > max_frame_hard_cap ? max_frame_hard_cap : value;
}

[[nodiscard]] std::size_t decoder_buffer_capacity(u32 max_frame) noexcept {
    const std::size_t frame_bytes =
        static_cast<std::size_t>(max_frame) + frame_header_size + ordered_extension_size;
    return frame_bytes > buffer::default_max_capacity ? frame_bytes : buffer::default_max_capacity;
}

// Envelope validation shared by the encoder and the decoder. The returned code
// is protocol_error on the wire path and invalid_argument on the encode path;
// the caller maps it.
[[nodiscard]] rpc_code validate_envelope(const call_envelope& value) noexcept {
    if ((value.call_flags & static_cast<u8>(~call_flag_known_mask)) != 0) {
        return rpc_code::protocol_error;
    }
    if (!is_known_codec(value.codec)) {
        return rpc_code::protocol_error;
    }

    const bool has_extensions = (value.call_flags & call_flag_has_extensions) != 0;
    if (!has_extensions) {
        if (value.extensions.flags != 0 || !value.extensions.idempotency_key.empty()) {
            return rpc_code::protocol_error;
        }
        return rpc_code::ok;
    }

    const trace_extension& ext = value.extensions;
    if ((ext.flags & static_cast<u8>(~trace_extension::known_mask)) != 0) {
        return rpc_code::protocol_error;
    }
    if (ext.has(trace_extension::has_span_id) && !ext.has(trace_extension::has_trace_id)) {
        return rpc_code::protocol_error;
    }
    if (ext.has(trace_extension::has_parent_span_id) && !ext.has(trace_extension::has_span_id)) {
        return rpc_code::protocol_error;
    }
    if (ext.has(trace_extension::has_idempotency_key)) {
        if (ext.idempotency_key.size() > max_idempotency_key) {
            return rpc_code::protocol_error;
        }
    } else if (!ext.idempotency_key.empty()) {
        return rpc_code::protocol_error;
    }
    return rpc_code::ok;
}

// Whole-frame compression gate, shared by encode_frame/decode_frame and the
// incremental decoder. It is deliberately not part of validate_header:
// decode_frame_header must keep accepting a COMPRESSED header (including on a
// control frame) so the whole-frame paths can decide with the payload in hand.
//
// Returns rpc_code::ok when the frame carries no COMPRESSED flag, or when it
// carries one on a type whose payload is application data. A COMPRESSED control
// frame is a protocol violation: those payloads are fixed-size control fields
// where compression can only inflate (PUSH_ACK is also size-checked by the
// server), so the compressed form would be misread as a malformed one.
[[nodiscard]] bool compression_allowed_for(const frame_header& header) noexcept {
    if ((header.flags & frame_flag_compressed) == 0) {
        return true;
    }
    return frame_type_carries_compression(header.type);
}

} // namespace

// --- CRC32C ------------------------------------------------------------------

u32 crc32c(span<const std::byte> data) noexcept {
    u32 value = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < data.size(); ++i) {
        const u8 byte = static_cast<u8>(data[i]);
        value = crc32c_lookup.entries[(value ^ static_cast<u32>(byte)) & 0xFFu] ^ (value >> 8);
    }
    return value ^ 0xFFFFFFFFu;
}

// --- Header codec ------------------------------------------------------------

status encode_frame_header(span<std::byte> out, const frame_header& header) noexcept {
    if (out.size() < frame_header_size) {
        return rpc_code::encode_error;
    }
    byte_writer writer(out.first(frame_header_size));
    writer.put_u32(header.payload_len);
    writer.put_u8(static_cast<u8>(header.type));
    writer.put_u8(header.flags);
    writer.put_u16(0);
    writer.put_u32(header.stream_id);
    writer.put_u32(header.ext);
    return writer.ok() ? status() : status(rpc_code::encode_error);
}

status decode_frame_header(span<const std::byte> in, frame_header& header, u32 max_frame) noexcept {
    if (in.size() < frame_header_size) {
        return rpc_code::protocol_error;
    }

    byte_reader reader(in.first(frame_header_size));
    u32 payload_len = 0;
    u8 type = 0;
    u8 flags = 0;
    u16 reserved = 0;
    u32 stream_id = 0;
    u32 ext = 0;
    if (!reader.get_u32(payload_len) || !reader.get_u8(type) || !reader.get_u8(flags) ||
        !reader.get_u16(reserved) || !reader.get_u32(stream_id) || !reader.get_u32(ext)) {
        return rpc_code::protocol_error;
    }
    if (reserved != 0) {
        return rpc_code::protocol_error;
    }
    if (type > static_cast<u8>(frame_type::push_ack)) {
        return rpc_code::protocol_error;
    }

    frame_header parsed;
    parsed.payload_len = payload_len;
    parsed.type = static_cast<frame_type>(type);
    parsed.flags = flags;
    parsed.stream_id = stream_id;
    parsed.ext = ext;

    const rpc_code code = detail::validate_header(parsed, max_frame);
    if (code != rpc_code::ok) {
        return code;
    }
    header = parsed;
    return status();
}

// --- Frame codec -------------------------------------------------------------

std::size_t encoded_frame_size(const frame& value) noexcept {
    const std::size_t extension_bytes =
        (value.header.flags & frame_flag_ordered) != 0 ? ordered_extension_size : 0;
    return frame_header_size + extension_bytes + value.payload.size();
}

status encode_frame(span<std::byte> out, const frame& value, std::size_t& written, u32 max_frame) noexcept {
    written = 0;
    if ((value.header.flags & (frame_flag_compressed | frame_flag_algo_mask)) != 0) {
        // A frame that claims a compressed payload it does not contain is worse
        // than no compression at all: the peer would either misparse it or
        // silently deliver it as plaintext. Only encode_frame_compressed may set
        // those bits, and only after actually compressing the payload.
        return rpc_code::invalid_argument;
    }
    if (value.payload.size() > static_cast<std::size_t>(max_frame) ||
        value.payload.size() > static_cast<std::size_t>(std::numeric_limits<u32>::max())) {
        return rpc_code::invalid_argument;
    }

    frame_header header = value.header;
    header.payload_len = static_cast<u32>(value.payload.size());
    if ((header.flags & frame_flag_crc32c) != 0) {
        header.ext = crc32c(value.payload);
    }

    const rpc_code code = detail::validate_header(header, max_frame);
    if (code != rpc_code::ok) {
        return detail::encode_error_for(code);
    }

    const std::size_t extension_bytes =
        (header.flags & frame_flag_ordered) != 0 ? ordered_extension_size : 0;
    const std::size_t total = frame_header_size + extension_bytes + value.payload.size();
    if (out.size() < total) {
        return rpc_code::encode_error;
    }

    byte_writer writer(out.first(total));
    writer.put_u32(header.payload_len);
    writer.put_u8(static_cast<u8>(header.type));
    writer.put_u8(header.flags);
    writer.put_u16(0);
    writer.put_u32(header.stream_id);
    writer.put_u32(header.ext);
    if (extension_bytes != 0) {
        writer.put_u64(value.ordered.lane_id);
        writer.put_u64(value.ordered.lane_seq);
    }
    writer.put_bytes(value.payload);
    if (!writer.ok()) {
        return rpc_code::encode_error;
    }
    written = writer.written();
    return status();
}

status decode_frame(span<const std::byte> in, frame& value, u32 max_frame) noexcept {
    frame_header header;
    const status header_status = decode_frame_header(in, header, max_frame);
    if (!header_status.ok()) {
        return header_status;
    }

    const std::size_t extension_bytes =
        (header.flags & frame_flag_ordered) != 0 ? ordered_extension_size : 0;
    const std::size_t total =
        frame_header_size + extension_bytes + static_cast<std::size_t>(header.payload_len);
    if (in.size() != total) {
        return rpc_code::protocol_error;
    }

    ordered_extension ordered;
    if (extension_bytes != 0) {
        byte_reader reader(in.subspan(frame_header_size, ordered_extension_size));
        u64 lane_id = 0;
        u64 lane_seq = 0;
        if (!reader.get_u64(lane_id) || !reader.get_u64(lane_seq)) {
            return rpc_code::protocol_error;
        }
        ordered.lane_id = lane_id;
        ordered.lane_seq = lane_seq;
    }

    const span<const std::byte> payload =
        in.subspan(frame_header_size + extension_bytes, static_cast<std::size_t>(header.payload_len));
    if (!compression_allowed_for(header)) {
        return rpc_code::protocol_error;
    }
    if ((header.flags & frame_flag_compressed) != 0) {
        // No output buffer: this entry point cannot deliver decompressed bytes
        // and must never hand the compressed stream to the caller as a payload.
        return rpc_code::protocol_error;
    }
    if ((header.flags & frame_flag_crc32c) != 0 && crc32c(payload) != header.ext) {
        return rpc_code::protocol_error;
    }

    value.header = header;
    value.ordered = ordered;
    value.payload = payload;
    return status();
}

// --- Call envelope -----------------------------------------------------------

std::size_t encoded_call_envelope_size(const call_envelope& value) noexcept {
    std::size_t size = 8 + 8 + 1 + 2;
    if ((value.call_flags & call_flag_has_extensions) == 0) {
        return size + value.body.size();
    }

    const trace_extension& ext = value.extensions;
    size += 1;
    if (ext.has(trace_extension::has_trace_id)) {
        size += ext.trace_id.size();
    }
    if (ext.has(trace_extension::has_span_id)) {
        size += ext.span_id.size();
    }
    if (ext.has(trace_extension::has_parent_span_id)) {
        size += ext.parent_span_id.size();
    }
    if (ext.has(trace_extension::has_deadline)) {
        size += 8;
    }
    if (ext.has(trace_extension::has_idempotency_key)) {
        size += 1 + ext.idempotency_key.size();
    }
    return size + value.body.size();
}

status encode_call_envelope(span<std::byte> out, const call_envelope& value, std::size_t& written) noexcept {
    written = 0;
    const rpc_code code = validate_envelope(value);
    if (code != rpc_code::ok) {
        return detail::encode_error_for(code);
    }

    const std::size_t total = encoded_call_envelope_size(value);
    if (out.size() < total) {
        return rpc_code::encode_error;
    }

    byte_writer writer(out.first(total));
    writer.put_u64(value.service_id);
    writer.put_u64(value.method_id);
    writer.put_u8(value.call_flags);
    writer.put_u16(static_cast<u16>(value.codec));

    if ((value.call_flags & call_flag_has_extensions) != 0) {
        const trace_extension& ext = value.extensions;
        writer.put_u8(ext.flags);
        if (ext.has(trace_extension::has_trace_id)) {
            writer.put_bytes(span<const std::byte>(ext.trace_id.data(), ext.trace_id.size()));
        }
        if (ext.has(trace_extension::has_span_id)) {
            writer.put_bytes(span<const std::byte>(ext.span_id.data(), ext.span_id.size()));
        }
        if (ext.has(trace_extension::has_parent_span_id)) {
            writer.put_bytes(span<const std::byte>(ext.parent_span_id.data(), ext.parent_span_id.size()));
        }
        if (ext.has(trace_extension::has_deadline)) {
            writer.put_u64(ext.deadline_ns);
        }
        if (ext.has(trace_extension::has_idempotency_key)) {
            writer.put_u8(static_cast<u8>(ext.idempotency_key.size()));
            writer.put_bytes(ext.idempotency_key);
        }
    }

    writer.put_bytes(value.body);
    if (!writer.ok()) {
        return rpc_code::encode_error;
    }
    written = writer.written();
    return status();
}

status decode_call_envelope(span<const std::byte> in, call_envelope& value) noexcept {
    byte_reader reader(in);
    call_envelope decoded;

    u16 codec = 0;
    if (!reader.get_u64(decoded.service_id) || !reader.get_u64(decoded.method_id) ||
        !reader.get_u8(decoded.call_flags) || !reader.get_u16(codec)) {
        return rpc_code::protocol_error;
    }
    if (!is_known_codec(static_cast<codec_id>(codec))) {
        return rpc_code::protocol_error;
    }
    decoded.codec = static_cast<codec_id>(codec);

    if ((decoded.call_flags & static_cast<u8>(~call_flag_known_mask)) != 0) {
        return rpc_code::protocol_error;
    }

    if ((decoded.call_flags & call_flag_has_extensions) != 0) {
        trace_extension ext;
        if (!reader.get_u8(ext.flags)) {
            return rpc_code::protocol_error;
        }
        if ((ext.flags & static_cast<u8>(~trace_extension::known_mask)) != 0) {
            return rpc_code::protocol_error;
        }
        if (ext.has(trace_extension::has_span_id) && !ext.has(trace_extension::has_trace_id)) {
            return rpc_code::protocol_error;
        }
        if (ext.has(trace_extension::has_parent_span_id) && !ext.has(trace_extension::has_span_id)) {
            return rpc_code::protocol_error;
        }

        if (ext.has(trace_extension::has_trace_id)) {
            span<const std::byte> bytes;
            if (!reader.get_bytes(ext.trace_id.size(), bytes)) {
                return rpc_code::protocol_error;
            }
            std::copy(bytes.begin(), bytes.end(), ext.trace_id.begin());
        }
        if (ext.has(trace_extension::has_span_id)) {
            span<const std::byte> bytes;
            if (!reader.get_bytes(ext.span_id.size(), bytes)) {
                return rpc_code::protocol_error;
            }
            std::copy(bytes.begin(), bytes.end(), ext.span_id.begin());
        }
        if (ext.has(trace_extension::has_parent_span_id)) {
            span<const std::byte> bytes;
            if (!reader.get_bytes(ext.parent_span_id.size(), bytes)) {
                return rpc_code::protocol_error;
            }
            std::copy(bytes.begin(), bytes.end(), ext.parent_span_id.begin());
        }
        if (ext.has(trace_extension::has_deadline)) {
            if (!reader.get_u64(ext.deadline_ns)) {
                return rpc_code::protocol_error;
            }
        }
        if (ext.has(trace_extension::has_idempotency_key)) {
            u8 length = 0;
            if (!reader.get_u8(length)) {
                return rpc_code::protocol_error;
            }
            if (static_cast<std::size_t>(length) > max_idempotency_key) {
                return rpc_code::protocol_error;
            }
            span<const std::byte> key;
            if (!reader.get_bytes(static_cast<std::size_t>(length), key)) {
                return rpc_code::protocol_error;
            }
            ext.idempotency_key = key;
        }
        decoded.extensions = ext;
    }

    decoded.body = reader.remaining_bytes();
    value = decoded;
    return status();
}

// --- Incremental decoder -----------------------------------------------------

frame_decoder::frame_decoder(u32 max_frame) noexcept
    : frame_decoder(span<std::byte>(), max_frame) {}

frame_decoder::frame_decoder(span<std::byte> payload_buffer, u32 max_frame) noexcept
    : buffer_(std::size_t{0}, decoder_buffer_capacity(clamp_max_frame(max_frame))),
      payload_buffer_(payload_buffer),
      max_frame_(clamp_max_frame(max_frame)) {}

void frame_decoder::feed(span<const std::byte> data) noexcept {
    if (error_ != rpc_code::ok || data.empty()) {
        return;
    }
    if (!buffer_.append(data)) {
        error_ = rpc_code::resource_exhausted;
    }
}

void frame_decoder::compact() noexcept {
    if (read_offset_ == 0) {
        return;
    }
    if (read_offset_ == buffer_.size()) {
        buffer_.clear();
        read_offset_ = 0;
        return;
    }
    // Compacting on every call would turn a batch of small frames into
    // quadratic copying. Compact only once the dead prefix is large.
    if (read_offset_ >= 4096 && read_offset_ >= buffer_.size() / 2) {
        buffer_.erase_front(read_offset_);
        read_offset_ = 0;
    }
}

frame_decoder::result frame_decoder::next(frame& out) noexcept {
    return next(out, nullptr);
}

frame_decoder::result frame_decoder::next(frame& out, frame_compression_info* info) noexcept {
    if (info != nullptr) {
        *info = frame_compression_info{};
    }
    if (error_ != rpc_code::ok) {
        return result::error;
    }

    compact();
    const std::size_t available = buffer_.size() - read_offset_;
    if (available < frame_header_size) {
        return result::need_more;
    }

    const std::byte* const base = buffer_.data() + read_offset_;
    frame_header header;
    const status header_status =
        decode_frame_header(span<const std::byte>(base, frame_header_size), header, max_frame_);
    if (!header_status.ok()) {
        error_ = header_status.code();
        return result::error;
    }

    const std::size_t extension_bytes =
        (header.flags & frame_flag_ordered) != 0 ? ordered_extension_size : 0;
    const std::size_t total =
        frame_header_size + extension_bytes + static_cast<std::size_t>(header.payload_len);
    if (available < total) {
        return result::need_more;
    }

    ordered_extension ordered;
    if (extension_bytes != 0) {
        byte_reader reader(span<const std::byte>(base + frame_header_size, ordered_extension_size));
        u64 lane_id = 0;
        u64 lane_seq = 0;
        if (!reader.get_u64(lane_id) || !reader.get_u64(lane_seq)) {
            error_ = rpc_code::protocol_error;
            return result::error;
        }
        ordered.lane_id = lane_id;
        ordered.lane_seq = lane_seq;
    }

    const span<const std::byte> payload(
        base + frame_header_size + extension_bytes,
        static_cast<std::size_t>(header.payload_len));

    // Consumes the frame just parsed. A drop is a normal, non-latching outcome:
    // the bytes are gone and the decoder stays usable, which is why error_ is
    // not touched here.
    const auto consume = [&]() noexcept {
        read_offset_ += total;
        if (read_offset_ == buffer_.size()) {
            buffer_.clear();
            read_offset_ = 0;
        }
    };

    if (!compression_allowed_for(header)) {
        error_ = rpc_code::protocol_error;
        return result::error;
    }

    if ((header.flags & frame_flag_compressed) != 0) {
        if (payload_buffer_.empty()) {
            // Borrowing mode has nowhere to put the decompressed bytes and must
            // never hand the compressed stream to the caller.
            error_ = rpc_code::protocol_error;
            return result::error;
        }
        const compression_algorithm algorithm = frame_compression(header);
        if (!is_supported_compression(algorithm) || algorithm == compression_algorithm::none) {
            error_ = rpc_code::protocol_error;
            return result::error;
        }
        if (payload.size() < min_compressed_payload_size) {
            error_ = rpc_code::protocol_error;
            return result::error;
        }

        const std::size_t logical_size =
            static_cast<std::size_t>(static_cast<u8>(payload[0])) |
            (static_cast<std::size_t>(static_cast<u8>(payload[1])) << 8) |
            (static_cast<std::size_t>(static_cast<u8>(payload[2])) << 16) |
            (static_cast<std::size_t>(static_cast<u8>(payload[3])) << 24);
        if (logical_size == 0 || logical_size > static_cast<std::size_t>(max_frame_)) {
            error_ = rpc_code::protocol_error;
            return result::error;
        }
        if (logical_size > payload_buffer_.size()) {
            // Bounded before any decompression: the buffer is the real ceiling.
            error_ = rpc_code::resource_exhausted;
            return result::error;
        }

        // Frame-local failures are an explicit drop on ONE_WAY only. The
        // criterion is the frame type, not the UNRELIABLE flag: a peer can set
        // that flag on a reliable frame, and it must not be able to downgrade a
        // fatal protocol violation into a silently lost request.
        const bool droppable = header.type == frame_type::one_way;
        const auto drop_or_fail = [&](rpc_code reason) noexcept {
            if (droppable) {
                consume();
                if (info != nullptr) {
                    info->algorithm = algorithm;
                    info->wire_size = header.payload_len;
                    info->logical_size = static_cast<std::uint32_t>(logical_size);
                    info->dropped = true;
                    // The frame was discarded because its payload could not be
                    // decoded; error() stays ok and the caller learns the reason
                    // from here. decode_error is the documented drop reason.
                    info->drop_reason = rpc_code::decode_error;
                }
                return result::frame_dropped;
            }
            error_ = reason;
            return result::error;
        };

        if ((header.flags & frame_flag_crc32c) != 0 && crc32c(payload) != header.ext) {
            return drop_or_fail(rpc_code::protocol_error);
        }

        // The decoder carries no policy member (ABI), so its ceiling is the
        // frame limit it was constructed with; the payload buffer was already
        // checked above and is the stricter bound. The ratio guard uses the
        // default, which admits every legal LZ4 stream.
        compression_limits limits;
        limits.max_payload = max_frame_;

        std::size_t decoded_size = 0;
        const status decode_status = decompress_payload(
            algorithm, payload.subspan(compressed_payload_prefix_size), logical_size,
            payload_buffer_.first(logical_size), decoded_size, limits);
        if (!decode_status.ok()) {
            // Algorithm and limit failures stay fatal on every frame type; the
            // codec has already distinguished them by its status code.
            if (decode_status.code() == rpc_code::protocol_error) {
                return drop_or_fail(rpc_code::protocol_error);
            }
            error_ = decode_status.code();
            return result::error;
        }
        if (decoded_size != logical_size) {
            return drop_or_fail(rpc_code::protocol_error);
        }

        header.flags = static_cast<u8>(
            header.flags & static_cast<u8>(~(frame_flag_compressed | frame_flag_algo_mask)));
        header.payload_len = static_cast<u32>(logical_size);
        header.ext = (header.flags & frame_flag_crc32c) != 0
                         ? crc32c(span<const std::byte>(payload_buffer_.data(), logical_size))
                         : u32{0};

        out.header = header;
        out.ordered = ordered;
        out.payload = span<const std::byte>(payload_buffer_.data(), logical_size);

        consume();
        if (info != nullptr) {
            info->algorithm = algorithm;
            info->wire_size = static_cast<std::uint32_t>(payload.size());
            info->logical_size = static_cast<std::uint32_t>(logical_size);
        }
        return result::frame_ready;
    }

    if ((header.flags & frame_flag_crc32c) != 0 && crc32c(payload) != header.ext) {
        error_ = rpc_code::protocol_error;
        return result::error;
    }

    span<const std::byte> output_payload;
    if (!payload_buffer_.empty()) {
        if (payload.size() > payload_buffer_.size()) {
            error_ = rpc_code::resource_exhausted;
            return result::error;
        }
        std::copy(payload.begin(), payload.end(), payload_buffer_.begin());
        output_payload = span<const std::byte>(payload_buffer_.data(), payload.size());
    } else {
        output_payload = payload;
    }

    out.header = header;
    out.ordered = ordered;
    out.payload = output_payload;

    consume();
    if (info != nullptr) {
        info->wire_size = header.payload_len;
        info->logical_size = header.payload_len;
    }
    return result::frame_ready;
}

rpc_code frame_decoder::error() const noexcept { return error_; }

bool frame_decoder::failed() const noexcept { return error_ != rpc_code::ok; }

std::size_t frame_decoder::buffered() const noexcept {
    return buffer_.size() - read_offset_;
}

u32 frame_decoder::max_frame() const noexcept { return max_frame_; }

void frame_decoder::reset() noexcept {
    buffer_.clear();
    read_offset_ = 0;
    error_ = rpc_code::ok;
}

} // namespace crpc
