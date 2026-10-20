// CRPC -- frame-level compression (G29a).
//
// The container, the fail-closed table and the sender policy are specified in
// docs/protocol.md section 24. This translation unit owns the prefix, the
// limits, the fallback decision and the header normalisation; the LZ4 block
// codec itself lives in lz4_block.cpp.
#include <algorithm>
#include <cstring>
#include <limits>

#include <crpc/frame_compression.hpp>
#include <crpc/wire.hpp>

namespace crpc {
namespace detail {

// Defined in lz4_block.cpp. Both work on bare codec streams (no prefix).
[[nodiscard]] status lz4_block_compress(span<const std::byte> in, span<std::byte> out,
                                        std::size_t& written) noexcept;
[[nodiscard]] status lz4_block_decompress(span<const std::byte> in, std::size_t expected_size,
                                          span<std::byte> out, std::size_t& written) noexcept;

} // namespace detail

namespace {

// Exact "plain / wire > ratio" test without overflowing a size_t product.
[[nodiscard]] bool ratio_exceeded(std::size_t plain, std::size_t wire,
                                  std::size_t ratio) noexcept {
    if (ratio == 0 || wire == 0) {
        return false;
    }
    const std::size_t quotient = plain / wire;
    if (quotient > ratio) {
        return true;
    }
    return quotient == ratio && (plain % wire) != 0;
}

[[nodiscard]] std::size_t effective_payload_ceiling(std::size_t max_frame,
                                                    const compression_limits& limits) noexcept {
    return std::min(max_frame, limits.max_payload);
}

[[nodiscard]] u32 load_prefix(span<const std::byte> payload) noexcept {
    return static_cast<u32>(static_cast<u8>(payload[0])) |
           (static_cast<u32>(static_cast<u8>(payload[1])) << 8) |
           (static_cast<u32>(static_cast<u8>(payload[2])) << 16) |
           (static_cast<u32>(static_cast<u8>(payload[3])) << 24);
}

// crpc::span has no implicit qualification conversion, so a writable view of
// freshly produced bytes is narrowed explicitly where a read-only span is
// needed.
[[nodiscard]] span<const std::byte> as_const(span<std::byte> bytes) noexcept {
    return span<const std::byte>(bytes.data(), bytes.size());
}

} // namespace

bool is_supported_compression(compression_algorithm algorithm) noexcept {
    return algorithm == compression_algorithm::none || algorithm == compression_algorithm::lz4;
}

std::size_t compressed_payload_bound(compression_algorithm algorithm,
                                     std::size_t plain_size) noexcept {
    switch (algorithm) {
    case compression_algorithm::lz4:
        // Worst case of the block encoder: every 255 literal bytes cost one
        // extra length byte, plus the token and the trailing sequence.
        return compressed_payload_prefix_size + plain_size + plain_size / 255 + 16;
    case compression_algorithm::none:
    case compression_algorithm::zstd:
    case compression_algorithm::gzip:
        break;
    }
    // An unusable algorithm ends up as an uncompressed payload, which carries
    // no prefix.
    return plain_size;
}

status compress_payload(compression_algorithm algorithm, span<const std::byte> in,
                        span<std::byte> out, std::size_t& written,
                        const compression_limits& limits) noexcept {
    written = 0;
    if (algorithm != compression_algorithm::lz4) {
        return rpc_code::invalid_argument;
    }
    if (in.empty() || in.size() > limits.max_payload) {
        return rpc_code::invalid_argument;
    }
    return detail::lz4_block_compress(in, out, written);
}

status decompress_payload(compression_algorithm algorithm, span<const std::byte> in,
                          std::size_t expected_size, span<std::byte> out, std::size_t& written,
                          const compression_limits& limits,
                          compression_statistics* stats) noexcept {
    written = 0;
    if (algorithm != compression_algorithm::lz4) {
        if (stats != nullptr) {
            ++stats->rejected_algorithm;
        }
        return rpc_code::protocol_error;
    }
    if (expected_size == 0 || expected_size > limits.max_payload) {
        if (stats != nullptr) {
            ++stats->rejected_size;
        }
        return rpc_code::protocol_error;
    }
    if (expected_size > out.size()) {
        if (stats != nullptr) {
            ++stats->rejected_size;
        }
        return rpc_code::resource_exhausted;
    }
    if (ratio_exceeded(expected_size, in.size(), limits.max_compression_ratio)) {
        if (stats != nullptr) {
            ++stats->rejected_ratio;
        }
        return rpc_code::resource_exhausted;
    }

    const status result = detail::lz4_block_decompress(in, expected_size, out, written);
    if (!result.ok()) {
        if (stats != nullptr) {
            ++stats->decode_failures;
        }
        return result;
    }
    return status();
}

std::size_t encoded_frame_size_compressed(const frame& value, compression_algorithm preferred,
                                          const compression_limits& limits) noexcept {
    const std::size_t extension_bytes =
        (value.header.flags & frame_flag_ordered) != 0 ? ordered_extension_size : 0;
    const std::size_t plain = value.payload.size();
    const std::size_t encoded =
        frame_type_carries_compression(value.header.type) && is_supported_compression(preferred)
            ? compressed_payload_bound(preferred, plain)
            : plain;
    (void)limits;
    return frame_header_size + extension_bytes + std::max(plain, encoded);
}

status encode_frame_compressed(span<std::byte> out, const frame& value,
                               compression_algorithm preferred, const compression_limits& limits,
                               std::size_t& written, compression_algorithm* chosen,
                               compression_statistics* stats) noexcept {
    written = 0;
    if (chosen != nullptr) {
        *chosen = compression_algorithm::none;
    }
    if ((value.header.flags & (frame_flag_compressed | frame_flag_algo_mask)) != 0) {
        // Only this function may set those bits; a caller-supplied COMPRESSED
        // flag would claim a stream it did not produce.
        return rpc_code::invalid_argument;
    }

    const std::size_t ceiling = effective_payload_ceiling(max_frame_default, limits);
    if (value.payload.size() > ceiling ||
        value.payload.size() > static_cast<std::size_t>(std::numeric_limits<u32>::max())) {
        return rpc_code::invalid_argument;
    }

    const auto fall_back = [&]() noexcept {
        return encode_frame(out, value, written);
    };

    // An unusable algorithm is a configuration error, not a hint: silently
    // sending lz4 when the caller asked for gzip would be the same kind of
    // "flag does not describe the payload" failure this slice removes. `none`
    // is the explicit identity request and is always legal.
    if (preferred != compression_algorithm::none && !is_supported_compression(preferred)) {
        return rpc_code::invalid_argument;
    }

    if (value.payload.empty() || !frame_type_carries_compression(value.header.type) ||
        preferred == compression_algorithm::none) {
        return fall_back();
    }
    if (value.payload.size() < limits.min_payload) {
        if (stats != nullptr) {
            ++stats->skipped_below_min;
        }
        return fall_back();
    }

    const std::size_t extension_bytes =
        (value.header.flags & frame_flag_ordered) != 0 ? ordered_extension_size : 0;
    const std::size_t offset = frame_header_size + extension_bytes;
    if (out.size() < offset + compressed_payload_bound(preferred, value.payload.size())) {
        return rpc_code::encode_error;
    }

    // Compress straight into the final location of the codec stream: the bound
    // reserved above is exactly the encoder's worst case.
    span<std::byte> stream_space = out.subspan(offset + compressed_payload_prefix_size);
    std::size_t stream_size = 0;
    const status compress_status =
        compress_payload(preferred, value.payload, stream_space, stream_size, limits);
    if (!compress_status.ok()) {
        return compress_status;
    }

    const std::size_t wire_size = compressed_payload_prefix_size + stream_size;
    if (wire_size >= value.payload.size()) {
        if (stats != nullptr) {
            ++stats->fallbacks_not_smaller;
        }
        return fall_back();
    }

    // The codec stream is already at its final position; prepend the logical
    // length so the whole on-the-wire payload is contiguous for CRC32C.
    byte_writer prefix_writer(out.subspan(offset, compressed_payload_prefix_size));
    prefix_writer.put_u32(static_cast<u32>(value.payload.size()));
    if (!prefix_writer.ok()) {
        return rpc_code::encode_error;
    }
    const span<const std::byte> wire_payload = as_const(out.subspan(offset, wire_size));
    const u32 checksum =
        (value.header.flags & frame_flag_crc32c) != 0 ? crc32c(wire_payload) : u32{0};

    const std::size_t total = offset + wire_size;
    byte_writer writer(out.first(frame_header_size));
    writer.put_u32(static_cast<u32>(wire_size));
    writer.put_u8(static_cast<u8>(value.header.type));
    writer.put_u8(static_cast<u8>(value.header.flags | frame_flag_compressed |
                                  (static_cast<u8>(preferred) << frame_flag_algo_shift)));
    writer.put_u16(0);
    writer.put_u32(value.header.stream_id);
    writer.put_u32(checksum);
    if (!writer.ok()) {
        written = 0;
        return rpc_code::encode_error;
    }
    if (extension_bytes != 0) {
        byte_writer extension_writer(
            out.subspan(frame_header_size, ordered_extension_size));
        extension_writer.put_u64(value.ordered.lane_id);
        extension_writer.put_u64(value.ordered.lane_seq);
        if (!extension_writer.ok()) {
            written = 0;
            return rpc_code::encode_error;
        }
    }

    written = total;
    if (chosen != nullptr) {
        *chosen = preferred;
    }
    if (stats != nullptr) {
        ++stats->frames_compressed;
        stats->bytes_plain += value.payload.size();
        stats->bytes_wire += wire_size;
    }
    return status();
}

status decode_frame_compressed(span<const std::byte> in, frame& value, span<std::byte> payload_out,
                               u32 max_frame, const compression_limits& limits,
                               compression_algorithm* wire_algorithm,
                               compression_statistics* stats) noexcept {
    if (wire_algorithm != nullptr) {
        *wire_algorithm = compression_algorithm::none;
    }

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

    if ((header.flags & frame_flag_compressed) == 0) {
        if ((header.flags & frame_flag_crc32c) != 0 && crc32c(payload) != header.ext) {
            return rpc_code::protocol_error;
        }
        value.header = header;
        value.ordered = ordered;
        value.payload = payload;
        return status();
    }

    // --- Compressed frame ----------------------------------------------------
    const compression_algorithm algorithm = frame_compression(header);
    if (!frame_type_carries_compression(header.type)) {
        return rpc_code::protocol_error;
    }
    if (!is_supported_compression(algorithm) || algorithm == compression_algorithm::none) {
        if (stats != nullptr) {
            ++stats->rejected_algorithm;
        }
        return rpc_code::protocol_error;
    }
    if (payload.size() < min_compressed_payload_size) {
        return rpc_code::protocol_error;
    }

    const std::size_t logical_size = load_prefix(payload);
    if (logical_size == 0 || logical_size > static_cast<std::size_t>(max_frame)) {
        return rpc_code::protocol_error;
    }
    const std::size_t ceiling =
        std::min(effective_payload_ceiling(max_frame, limits), payload_out.size());
    if (logical_size > ceiling) {
        if (stats != nullptr) {
            ++stats->rejected_size;
        }
        return rpc_code::resource_exhausted;
    }
    if (ratio_exceeded(logical_size, payload.size() - compressed_payload_prefix_size,
                       limits.max_compression_ratio)) {
        if (stats != nullptr) {
            ++stats->rejected_ratio;
        }
        return rpc_code::resource_exhausted;
    }

    // Integrity first: never spend decompression CPU on corrupt bytes.
    if ((header.flags & frame_flag_crc32c) != 0 && crc32c(payload) != header.ext) {
        if (header.type == frame_type::one_way) {
            if (stats != nullptr) {
                ++stats->decode_failures;
                ++stats->frames_dropped_unreliable;
            }
            return rpc_code::decode_error;
        }
        return rpc_code::protocol_error;
    }

    std::size_t decoded_size = 0;
    const status decode_status = decompress_payload(
        algorithm, payload.subspan(compressed_payload_prefix_size), logical_size,
        payload_out.first(logical_size), decoded_size, limits, stats);
    if (!decode_status.ok()) {
        if (header.type == frame_type::one_way) {
            if (stats != nullptr) {
                ++stats->frames_dropped_unreliable;
            }
            return rpc_code::decode_error;
        }
        return decode_status.code() == rpc_code::protocol_error ? rpc_code::protocol_error
                                                                : decode_status.code();
    }
    if (decoded_size != logical_size) {
        if (stats != nullptr) {
            ++stats->decode_failures;
        }
        return rpc_code::protocol_error;
    }

    header.flags = static_cast<u8>(header.flags & static_cast<u8>(~(frame_flag_compressed |
                                                                 frame_flag_algo_mask)));
    header.payload_len = static_cast<u32>(logical_size);
    if ((header.flags & frame_flag_crc32c) != 0) {
        // The normalised header describes the logical payload, so its checksum
        // must too.
        header.ext = crc32c(as_const(payload_out.first(logical_size)));
    } else {
        header.ext = 0;
    }

    value.header = header;
    value.ordered = ordered;
    value.payload = as_const(payload_out.first(logical_size));
    if (wire_algorithm != nullptr) {
        *wire_algorithm = algorithm;
    }
    if (stats != nullptr) {
        ++stats->frames_decompressed;
        stats->bytes_plain += logical_size;
        stats->bytes_wire += payload.size();
    }
    return status();
}

} // namespace crpc
