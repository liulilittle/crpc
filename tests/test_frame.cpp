// CRPC -- tests for the frame codec.
//
// Coverage targets: every header field, flag independence, stream scope,
// reserved field, compression flag consistency, ordered extension, golden
// bytes from section 7.9, CRC32C known vectors and corruption detection,
// call-envelope round trip and rejection paths, and the incremental decoder
// (byte-at-a-time, split header, split payload, several frames per feed,
// zero-length payload, hostile length, unknown type, CRC mismatch, payload
// buffer copying, reset). The final two cases are deterministic mutation
// loops that assert the decoder never crashes on hostile input.
#include <algorithm>
#include <cstring>
#include <vector>

#include <crpc/frame.hpp>
#include <crpc/wire.hpp>

#include "test_framework.hpp"

using namespace crpc;

namespace {

std::vector<std::byte> bytes_of(const char* text) {
    const std::size_t size = std::strlen(text);
    std::vector<std::byte> out(size);
    for (std::size_t i = 0; i < size; ++i) {
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
    }
    return out;
}

span<const std::byte> as_span(const std::vector<std::byte>& bytes) {
    return span<const std::byte>(bytes.data(), bytes.size());
}

span<std::byte> as_writable_span(std::vector<std::byte>& bytes) {
    return span<std::byte>(bytes.data(), bytes.size());
}

std::vector<std::byte> raw_header(u32 payload_len, u8 type, u8 flags, u16 reserved,
                                  u32 stream_id, u32 ext) {
    std::vector<std::byte> out(frame_header_size);
    byte_writer writer(as_writable_span(out));
    writer.put_u32(payload_len);
    writer.put_u8(type);
    writer.put_u8(flags);
    writer.put_u16(reserved);
    writer.put_u32(stream_id);
    writer.put_u32(ext);
    CRPC_CHECK(writer.ok());
    return out;
}

std::vector<std::byte> encode_simple(frame_type type, u32 stream_id, u8 flags,
                                     const std::vector<std::byte>& payload,
                                     ordered_extension ordered = ordered_extension{}) {
    frame value;
    value.header.type = type;
    value.header.stream_id = stream_id;
    value.header.flags = flags;
    value.ordered = ordered;
    value.payload = as_span(payload);

    std::vector<std::byte> out(encoded_frame_size(value));
    std::size_t written = 0;
    const status result = encode_frame(as_writable_span(out), value, written);
    CRPC_CHECK(result.ok());
    out.resize(written);
    return out;
}

void check_bytes(const std::vector<std::byte>& actual, const std::vector<std::byte>& expected) {
    CRPC_CHECK_EQ(actual.size(), expected.size());
    const std::size_t count = std::min(actual.size(), expected.size());
    for (std::size_t i = 0; i < count; ++i) {
        CRPC_CHECK_EQ(static_cast<int>(actual[i]), static_cast<int>(expected[i]));
    }
}

frame_decoder::result next_result(frame_decoder& decoder, frame& value) {
    return decoder.next(value);
}

} // namespace

// --- Header ------------------------------------------------------------------

CRPC_TEST(frame_header_round_trip_all_fields) {
    frame_header header;
    header.payload_len = 0x00010203u;
    header.type = frame_type::response;
    header.flags = frame_flag_end_stream | frame_flag_crc32c;
    header.stream_id = 0x0A0B0C0Du;
    header.ext = 0x11223344u;

    std::byte raw[frame_header_size] = {};
    CRPC_CHECK(encode_frame_header(span<std::byte>(raw, sizeof(raw)), header).ok());

    frame_header decoded;
    CRPC_CHECK(decode_frame_header(span<const std::byte>(raw, sizeof(raw)), decoded, 0xFFFFFFFFu).ok());
    CRPC_CHECK_EQ(decoded.payload_len, header.payload_len);
    CRPC_CHECK_EQ(static_cast<int>(decoded.type), static_cast<int>(header.type));
    CRPC_CHECK_EQ(static_cast<int>(decoded.flags), static_cast<int>(header.flags));
    CRPC_CHECK_EQ(decoded.stream_id, header.stream_id);
    CRPC_CHECK_EQ(decoded.ext, header.ext);
}

CRPC_TEST(frame_header_round_trip_every_type) {
    const frame_type types[] = {
        frame_type::data,       frame_type::request,      frame_type::response,
        frame_type::one_way,    frame_type::rst_stream,   frame_type::settings,
        frame_type::settings_ack, frame_type::ping,       frame_type::pong,
        frame_type::goaway,     frame_type::window_update, frame_type::push,
        frame_type::push_ack};

    for (frame_type type : types) {
        const bool needs_stream = type == frame_type::data || type == frame_type::request ||
                                  type == frame_type::response || type == frame_type::one_way ||
                                  type == frame_type::rst_stream || type == frame_type::push ||
                                  type == frame_type::push_ack;
        u8 flags = 0;
        if (type == frame_type::one_way) {
            flags = frame_flag_unreliable;
        }
        const std::vector<std::byte> raw =
            raw_header(0, static_cast<u8>(type), flags, 0, needs_stream ? 1u : 0u, 0);
        frame_header decoded;
        CRPC_CHECK(decode_frame_header(as_span(raw), decoded).ok());
        CRPC_CHECK_EQ(static_cast<int>(decoded.type), static_cast<int>(type));
    }
}

CRPC_TEST(frame_header_flags_are_independent) {
    struct flag_case {
        u8 flags;
        u32 ext;
    };
    const flag_case cases[] = {{frame_flag_end_stream, 0},
                               {frame_flag_crc32c, 0xDEADBEEFu},
                               {frame_flag_ordered, 0},
                               {frame_flag_unreliable, 0},
                               {frame_flag_compressed | 0x20u, 0}, // lz4
                               {frame_flag_compressed | 0x40u, 0}, // zstd
                               {frame_flag_compressed | 0x60u, 0}, // gzip
                               {0x3Fu, 0xA5A5A5A5u}};

    for (const flag_case& item : cases) {
        const std::vector<std::byte> raw =
            raw_header(0, static_cast<u8>(frame_type::request), item.flags, 0, 1, item.ext);
        frame_header decoded;
        CRPC_CHECK(decode_frame_header(as_span(raw), decoded, 0xFFFFFFFFu).ok());
        CRPC_CHECK_EQ(static_cast<int>(decoded.flags), static_cast<int>(item.flags));
        CRPC_CHECK_EQ(decoded.ext, item.ext);
    }

    frame_header compressed;
    compressed.flags = frame_flag_compressed | 0x60u;
    CRPC_CHECK_EQ(static_cast<int>(frame_compression(compressed)),
                  static_cast<int>(compression_algorithm::gzip));
}

CRPC_TEST(frame_header_rejects_reserved_nonzero) {
    const std::vector<std::byte> raw =
        raw_header(0, static_cast<u8>(frame_type::ping), 0, 1, 0, 0);
    frame_header decoded;
    const status result = decode_frame_header(as_span(raw), decoded);
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(static_cast<int>(result.code()), static_cast<int>(rpc_code::protocol_error));
}

CRPC_TEST(frame_header_rejects_unknown_type) {
    const std::vector<std::byte> raw = raw_header(0, 0x0D, 0, 0, 0, 0);
    frame_header decoded;
    const status result = decode_frame_header(as_span(raw), decoded);
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(static_cast<int>(result.code()), static_cast<int>(rpc_code::protocol_error));
}

CRPC_TEST(frame_header_rejects_stream_scope_violations) {
    frame_header decoded;

    const std::vector<std::byte> request_zero =
        raw_header(0, static_cast<u8>(frame_type::request), 0, 0, 0, 0);
    CRPC_CHECK_FALSE(decode_frame_header(as_span(request_zero), decoded).ok());

    const std::vector<std::byte> ping_nonzero =
        raw_header(0, static_cast<u8>(frame_type::ping), 0, 0, 1, 0);
    CRPC_CHECK_FALSE(decode_frame_header(as_span(ping_nonzero), decoded).ok());

    const std::vector<std::byte> high_bit =
        raw_header(0, static_cast<u8>(frame_type::request), 0, 0, 0x80000001u, 0);
    CRPC_CHECK_FALSE(decode_frame_header(as_span(high_bit), decoded).ok());

    const std::vector<std::byte> rst_zero =
        raw_header(0, static_cast<u8>(frame_type::rst_stream), 0, 0, 0, 0);
    CRPC_CHECK_FALSE(decode_frame_header(as_span(rst_zero), decoded).ok());

    const std::vector<std::byte> window_connection =
        raw_header(0, static_cast<u8>(frame_type::window_update), 0, 0, 0, 0);
    CRPC_CHECK(decode_frame_header(as_span(window_connection), decoded).ok());

    const std::vector<std::byte> window_stream =
        raw_header(0, static_cast<u8>(frame_type::window_update), 0, 0, 5, 0);
    CRPC_CHECK(decode_frame_header(as_span(window_stream), decoded).ok());
}

CRPC_TEST(frame_header_rejects_compression_inconsistency) {
    frame_header decoded;

    const std::vector<std::byte> algo_without_compressed =
        raw_header(0, static_cast<u8>(frame_type::request), 0x20, 0, 1, 0);
    CRPC_CHECK_FALSE(decode_frame_header(as_span(algo_without_compressed), decoded).ok());

    const std::vector<std::byte> compressed_without_algo =
        raw_header(0, static_cast<u8>(frame_type::request), frame_flag_compressed, 0, 1, 0);
    CRPC_CHECK_FALSE(decode_frame_header(as_span(compressed_without_algo), decoded).ok());

    const std::vector<std::byte> reserved_algo =
        raw_header(0, static_cast<u8>(frame_type::request), 0xE0, 0, 1, 0);
    CRPC_CHECK_FALSE(decode_frame_header(as_span(reserved_algo), decoded).ok());

    const std::vector<std::byte> valid_gzip =
        raw_header(0, static_cast<u8>(frame_type::request), 0x62, 0, 1, 0);
    CRPC_CHECK(decode_frame_header(as_span(valid_gzip), decoded).ok());
}

CRPC_TEST(frame_header_rejects_nonzero_ext_without_crc) {
    const std::vector<std::byte> raw =
        raw_header(0, static_cast<u8>(frame_type::request), 0, 0, 1, 1);
    frame_header decoded;
    const status result = decode_frame_header(as_span(raw), decoded);
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(static_cast<int>(result.code()), static_cast<int>(rpc_code::protocol_error));
}

CRPC_TEST(frame_header_rejects_payload_over_max) {
    const std::vector<std::byte> raw =
        raw_header(17, static_cast<u8>(frame_type::ping), 0, 0, 0, 0);
    frame_header decoded;
    CRPC_CHECK_FALSE(decode_frame_header(as_span(raw), decoded, 16).ok());
    CRPC_CHECK(decode_frame_header(as_span(raw), decoded, 17).ok());
}

CRPC_TEST(frame_header_rejects_one_way_without_unreliable) {
    frame_header decoded;
    const std::vector<std::byte> reliable_one_way =
        raw_header(0, static_cast<u8>(frame_type::one_way), 0, 0, 1, 0);
    CRPC_CHECK_FALSE(decode_frame_header(as_span(reliable_one_way), decoded).ok());

    const std::vector<std::byte> unreliable_one_way =
        raw_header(0, static_cast<u8>(frame_type::one_way), frame_flag_unreliable, 0, 1, 0);
    CRPC_CHECK(decode_frame_header(as_span(unreliable_one_way), decoded).ok());
}

// --- Golden bytes ------------------------------------------------------------

CRPC_TEST(frame_golden_header_and_ping_bytes) {
    frame_header request;
    request.payload_len = 16;
    request.type = frame_type::request;
    request.stream_id = 1;

    std::vector<std::byte> header_bytes(frame_header_size);
    CRPC_CHECK(encode_frame_header(as_writable_span(header_bytes), request).ok());
    const std::vector<std::byte> expected_header = {
        static_cast<std::byte>(0x10), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x00), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x01), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x00), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x01), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x00), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x00), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x00), static_cast<std::byte>(0x00)};
    check_bytes(header_bytes, expected_header);

    const std::vector<std::byte> empty_payload(16, static_cast<std::byte>(0x00));
    const std::vector<std::byte> ordered_frame =
        encode_simple(frame_type::request, 1, frame_flag_ordered, empty_payload,
                      ordered_extension{7, 9});
    CRPC_CHECK_EQ(ordered_frame.size(), static_cast<std::size_t>(48));
    const std::vector<std::byte> expected_ordered = {
        static_cast<std::byte>(0x10), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x00), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x01), static_cast<std::byte>(0x08),
        static_cast<std::byte>(0x00), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x01), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x00), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x00), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x00), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x07), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x00), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x00), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x00), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x09), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x00), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x00), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x00), static_cast<std::byte>(0x00)};
    check_bytes(std::vector<std::byte>(ordered_frame.begin(), ordered_frame.begin() + 32),
                expected_ordered);

    std::vector<std::byte> ping_payload(8);
    for (std::size_t i = 0; i < ping_payload.size(); ++i) {
        ping_payload[i] = static_cast<std::byte>(i + 1);
    }
    const std::vector<std::byte> ping_frame =
        encode_simple(frame_type::ping, 0, 0, ping_payload);
    const std::vector<std::byte> expected_ping = {
        static_cast<std::byte>(0x08), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x00), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x07), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x00), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x00), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x00), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x00), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x00), static_cast<std::byte>(0x00),
        static_cast<std::byte>(0x01), static_cast<std::byte>(0x02),
        static_cast<std::byte>(0x03), static_cast<std::byte>(0x04),
        static_cast<std::byte>(0x05), static_cast<std::byte>(0x06),
        static_cast<std::byte>(0x07), static_cast<std::byte>(0x08)};
    check_bytes(ping_frame, expected_ping);
}

// --- Ordered extension -------------------------------------------------------

CRPC_TEST(frame_ordered_extension_round_trip) {
    const std::vector<std::byte> payload = bytes_of("abc");
    const std::vector<std::byte> encoded =
        encode_simple(frame_type::request, 3, frame_flag_ordered, payload,
                      ordered_extension{0x0102030405060708ull, 0x1112131415161718ull});
    CRPC_CHECK_EQ(encoded.size(), static_cast<std::size_t>(16 + 16 + 3));

    frame decoded;
    CRPC_CHECK(decode_frame(as_span(encoded), decoded).ok());
    CRPC_CHECK_EQ(decoded.header.payload_len, 3u);
    CRPC_CHECK_EQ(decoded.ordered.lane_id, 0x0102030405060708ull);
    CRPC_CHECK_EQ(decoded.ordered.lane_seq, 0x1112131415161718ull);
    CRPC_CHECK_EQ(decoded.payload.size(), payload.size());
    for (std::size_t i = 0; i < payload.size(); ++i) {
        CRPC_CHECK_EQ(static_cast<int>(decoded.payload[i]), static_cast<int>(payload[i]));
    }
}

CRPC_TEST(frame_ordered_extension_absent) {
    const std::vector<std::byte> payload = bytes_of("xy");
    const std::vector<std::byte> encoded = encode_simple(frame_type::response, 4, 0, payload);
    CRPC_CHECK_EQ(encoded.size(), static_cast<std::size_t>(16 + 2));

    frame decoded;
    CRPC_CHECK(decode_frame(as_span(encoded), decoded).ok());
    CRPC_CHECK_EQ(decoded.ordered.lane_id, 0ull);
    CRPC_CHECK_EQ(decoded.ordered.lane_seq, 0ull);
    CRPC_CHECK_EQ(decoded.payload.size(), payload.size());
}

// --- CRC32C ------------------------------------------------------------------

CRPC_TEST(crc32c_known_vectors) {
    const std::vector<std::byte> empty;
    const std::vector<std::byte> digits = bytes_of("123456789");
    const std::vector<std::byte> zero32(32, static_cast<std::byte>(0x00));
    const std::vector<std::byte> ff32(32, static_cast<std::byte>(0xFF));
    std::vector<std::byte> incrementing(32);
    for (std::size_t i = 0; i < incrementing.size(); ++i) {
        incrementing[i] = static_cast<std::byte>(i);
    }

    CRPC_CHECK_EQ(crc32c(as_span(empty)), 0u);
    CRPC_CHECK_EQ(crc32c(as_span(digits)), 0xE3069283u);
    CRPC_CHECK_EQ(crc32c(as_span(zero32)), 0x8A9136AAu);
    CRPC_CHECK_EQ(crc32c(as_span(ff32)), 0x62A8AB43u);
    CRPC_CHECK_EQ(crc32c(as_span(incrementing)), 0x46DD794Eu);
}

CRPC_TEST(frame_crc_round_trip_and_corruption_detection) {
    const std::vector<std::byte> payload = bytes_of("hello");
    const std::vector<std::byte> encoded =
        encode_simple(frame_type::request, 9, frame_flag_crc32c, payload);

    frame decoded;
    CRPC_CHECK(decode_frame(as_span(encoded), decoded).ok());
    CRPC_CHECK_EQ(decoded.header.ext, crc32c(as_span(payload)));
    CRPC_CHECK_EQ(decoded.payload.size(), payload.size());

    std::vector<std::byte> corrupted_payload = encoded;
    corrupted_payload[16] = static_cast<std::byte>(
        static_cast<unsigned>(corrupted_payload[16]) ^ 0x01u);
    frame rejected;
    const status payload_status = decode_frame(as_span(corrupted_payload), rejected);
    CRPC_CHECK_FALSE(payload_status.ok());
    CRPC_CHECK_EQ(static_cast<int>(payload_status.code()),
                  static_cast<int>(rpc_code::protocol_error));

    std::vector<std::byte> corrupted_crc = encoded;
    corrupted_crc[12] = static_cast<std::byte>(
        static_cast<unsigned>(corrupted_crc[12]) ^ 0x01u);
    CRPC_CHECK_FALSE(decode_frame(as_span(corrupted_crc), rejected).ok());
}

// --- One-shot frame decode ---------------------------------------------------

CRPC_TEST(frame_decode_rejects_length_mismatch) {
    const std::vector<std::byte> payload = bytes_of("1234");
    const std::vector<std::byte> encoded = encode_simple(frame_type::request, 1, 0, payload);

    frame decoded;
    std::vector<std::byte> longer = encoded;
    longer.push_back(static_cast<std::byte>(0x00));
    CRPC_CHECK_FALSE(decode_frame(as_span(longer), decoded).ok());

    std::vector<std::byte> shorter = encoded;
    shorter.pop_back();
    CRPC_CHECK_FALSE(decode_frame(as_span(shorter), decoded).ok());

    CRPC_CHECK_FALSE(decode_frame(span<const std::byte>(), decoded).ok());
}

CRPC_TEST(frame_encode_rejects_small_output_buffer) {
    frame_header header;
    header.type = frame_type::ping;
    header.stream_id = 0;
    std::byte small[4] = {};
    const status header_status = encode_frame_header(span<std::byte>(small, sizeof(small)), header);
    CRPC_CHECK_FALSE(header_status.ok());
    CRPC_CHECK_EQ(static_cast<int>(header_status.code()), static_cast<int>(rpc_code::encode_error));

    std::byte payload[8] = {};
    frame value;
    value.header.type = frame_type::ping;
    value.header.stream_id = 0;
    value.payload = span<const std::byte>(payload, sizeof(payload));
    std::size_t written = 123;
    const status frame_status = encode_frame(span<std::byte>(small, sizeof(small)), value, written);
    CRPC_CHECK_FALSE(frame_status.ok());
    CRPC_CHECK_EQ(static_cast<int>(frame_status.code()), static_cast<int>(rpc_code::encode_error));
    CRPC_CHECK_EQ(written, static_cast<std::size_t>(0));
}

CRPC_TEST(frame_encode_rejects_invalid_scope) {
    std::byte out[64] = {};
    std::size_t written = 0;

    frame request;
    request.header.type = frame_type::request;
    request.header.stream_id = 0;
    const status request_status =
        encode_frame(span<std::byte>(out, sizeof(out)), request, written);
    CRPC_CHECK_FALSE(request_status.ok());
    CRPC_CHECK_EQ(static_cast<int>(request_status.code()),
                  static_cast<int>(rpc_code::invalid_argument));

    frame one_way;
    one_way.header.type = frame_type::one_way;
    one_way.header.stream_id = 1;
    const status one_way_status =
        encode_frame(span<std::byte>(out, sizeof(out)), one_way, written);
    CRPC_CHECK_FALSE(one_way_status.ok());
    CRPC_CHECK_EQ(static_cast<int>(one_way_status.code()),
                  static_cast<int>(rpc_code::invalid_argument));
}

// --- Call envelope -----------------------------------------------------------

CRPC_TEST(call_envelope_round_trip_all_fields) {
    const std::vector<std::byte> body = bytes_of("body-bytes");
    const std::vector<std::byte> key = bytes_of("idem-123");

    call_envelope value;
    value.service_id = 0x0102030405060708ull;
    value.method_id = 0x1112131415161718ull;
    value.call_flags = call_flag_has_extensions;
    value.codec = codec_id::json;
    value.extensions.flags = trace_extension::has_trace_id | trace_extension::has_span_id |
                             trace_extension::has_parent_span_id |
                             trace_extension::has_deadline |
                             trace_extension::has_idempotency_key;
    for (std::size_t i = 0; i < value.extensions.trace_id.size(); ++i) {
        value.extensions.trace_id[i] = static_cast<std::byte>(i);
    }
    for (std::size_t i = 0; i < value.extensions.span_id.size(); ++i) {
        value.extensions.span_id[i] = static_cast<std::byte>(i + 1);
    }
    for (std::size_t i = 0; i < value.extensions.parent_span_id.size(); ++i) {
        value.extensions.parent_span_id[i] = static_cast<std::byte>(i + 9);
    }
    value.extensions.deadline_ns = 0x1122334455667788ull;
    value.extensions.idempotency_key = as_span(key);
    value.body = as_span(body);

    std::vector<std::byte> encoded(encoded_call_envelope_size(value));
    std::size_t written = 0;
    const status encode_status = encode_call_envelope(as_writable_span(encoded), value, written);
    CRPC_CHECK(encode_status.ok());
    CRPC_CHECK_EQ(written, encoded.size());

    call_envelope decoded;
    CRPC_CHECK(decode_call_envelope(as_span(encoded), decoded).ok());
    CRPC_CHECK_EQ(decoded.service_id, value.service_id);
    CRPC_CHECK_EQ(decoded.method_id, value.method_id);
    CRPC_CHECK_EQ(static_cast<int>(decoded.call_flags), static_cast<int>(value.call_flags));
    CRPC_CHECK_EQ(static_cast<int>(decoded.codec), static_cast<int>(value.codec));
    CRPC_CHECK_EQ(static_cast<int>(decoded.extensions.flags),
                  static_cast<int>(value.extensions.flags));
    CRPC_CHECK(decoded.extensions.trace_id == value.extensions.trace_id);
    CRPC_CHECK(decoded.extensions.span_id == value.extensions.span_id);
    CRPC_CHECK(decoded.extensions.parent_span_id == value.extensions.parent_span_id);
    CRPC_CHECK_EQ(decoded.extensions.deadline_ns, value.extensions.deadline_ns);
    CRPC_CHECK_EQ(decoded.extensions.idempotency_key.size(), key.size());
    for (std::size_t i = 0; i < key.size(); ++i) {
        CRPC_CHECK_EQ(static_cast<int>(decoded.extensions.idempotency_key[i]),
                      static_cast<int>(key[i]));
    }
    CRPC_CHECK_EQ(decoded.body.size(), body.size());
    for (std::size_t i = 0; i < body.size(); ++i) {
        CRPC_CHECK_EQ(static_cast<int>(decoded.body[i]), static_cast<int>(body[i]));
    }
}

CRPC_TEST(call_envelope_without_extensions) {
    const std::vector<std::byte> body = bytes_of("plain");
    call_envelope value;
    value.service_id = 1;
    value.method_id = 2;
    value.codec = codec_id::binary;
    value.body = as_span(body);

    CRPC_CHECK_EQ(encoded_call_envelope_size(value), static_cast<std::size_t>(19 + body.size()));
    std::vector<std::byte> encoded(encoded_call_envelope_size(value));
    std::size_t written = 0;
    CRPC_CHECK(encode_call_envelope(as_writable_span(encoded), value, written).ok());

    call_envelope decoded;
    CRPC_CHECK(decode_call_envelope(as_span(encoded), decoded).ok());
    CRPC_CHECK_EQ(static_cast<int>(decoded.call_flags), 0);
    CRPC_CHECK_EQ(decoded.body.size(), body.size());
}

CRPC_TEST(call_envelope_rejects_unknown_codec) {
    const std::vector<std::byte> body = bytes_of("x");
    call_envelope value;
    value.codec = static_cast<codec_id>(7);
    value.body = as_span(body);

    std::vector<std::byte> out(64);
    std::size_t written = 0;
    const status encode_status = encode_call_envelope(as_writable_span(out), value, written);
    CRPC_CHECK_FALSE(encode_status.ok());
    CRPC_CHECK_EQ(static_cast<int>(encode_status.code()),
                  static_cast<int>(rpc_code::invalid_argument));

    std::vector<std::byte> raw(19);
    byte_writer writer(as_writable_span(raw));
    writer.put_u64(0);
    writer.put_u64(0);
    writer.put_u8(0);
    writer.put_u16(7);
    CRPC_CHECK(writer.ok());

    call_envelope decoded;
    const status decode_status = decode_call_envelope(as_span(raw), decoded);
    CRPC_CHECK_FALSE(decode_status.ok());
    CRPC_CHECK_EQ(static_cast<int>(decode_status.code()),
                  static_cast<int>(rpc_code::protocol_error));
}

CRPC_TEST(call_envelope_rejects_invalid_extension_combinations) {
    std::vector<std::byte> out(512);
    std::size_t written = 0;

    call_envelope value;
    value.call_flags = call_flag_has_extensions;

    value.extensions.flags = trace_extension::has_span_id;
    CRPC_CHECK_EQ(static_cast<int>(encode_call_envelope(as_writable_span(out), value, written).code()),
                  static_cast<int>(rpc_code::invalid_argument));

    value.extensions.flags = trace_extension::has_parent_span_id;
    CRPC_CHECK_EQ(static_cast<int>(encode_call_envelope(as_writable_span(out), value, written).code()),
                  static_cast<int>(rpc_code::invalid_argument));

    value.extensions.flags = 0x20;
    CRPC_CHECK_EQ(static_cast<int>(encode_call_envelope(as_writable_span(out), value, written).code()),
                  static_cast<int>(rpc_code::invalid_argument));

    const std::vector<std::byte> too_long(129, static_cast<std::byte>(0x41));
    value.extensions.flags = trace_extension::has_idempotency_key;
    value.extensions.idempotency_key = as_span(too_long);
    CRPC_CHECK_EQ(static_cast<int>(encode_call_envelope(as_writable_span(out), value, written).code()),
                  static_cast<int>(rpc_code::invalid_argument));

    value.extensions = trace_extension{};
    value.call_flags = 0x80;
    CRPC_CHECK_EQ(static_cast<int>(encode_call_envelope(as_writable_span(out), value, written).code()),
                  static_cast<int>(rpc_code::invalid_argument));

    value.call_flags = 0;
    value.extensions.flags = trace_extension::has_trace_id;
    CRPC_CHECK_EQ(static_cast<int>(encode_call_envelope(as_writable_span(out), value, written).code()),
                  static_cast<int>(rpc_code::invalid_argument));

    std::vector<std::byte> raw(20);
    byte_writer writer(as_writable_span(raw));
    writer.put_u64(0);
    writer.put_u64(0);
    writer.put_u8(call_flag_has_extensions);
    writer.put_u16(0);
    writer.put_u8(0x80);
    CRPC_CHECK(writer.ok());

    call_envelope decoded;
    const status decode_status = decode_call_envelope(as_span(raw), decoded);
    CRPC_CHECK_FALSE(decode_status.ok());
    CRPC_CHECK_EQ(static_cast<int>(decode_status.code()),
                  static_cast<int>(rpc_code::protocol_error));
}

// --- Incremental decoder -----------------------------------------------------

CRPC_TEST(frame_decoder_one_byte_at_a_time) {
    const std::vector<std::byte> payload = bytes_of("abcde");
    const std::vector<std::byte> encoded = encode_simple(frame_type::request, 5, 0, payload);

    frame_decoder decoder;
    frame value;
    std::size_t ready_count = 0;
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        decoder.feed(span<const std::byte>(encoded.data() + i, 1));
        const frame_decoder::result result = next_result(decoder, value);
        if (i + 1 < encoded.size()) {
            CRPC_CHECK_EQ(static_cast<int>(result),
                          static_cast<int>(frame_decoder::result::need_more));
        } else {
            CRPC_CHECK_EQ(static_cast<int>(result),
                          static_cast<int>(frame_decoder::result::frame_ready));
            ++ready_count;
        }
    }
    CRPC_CHECK_EQ(ready_count, static_cast<std::size_t>(1));
    CRPC_CHECK_EQ(value.payload.size(), payload.size());
    CRPC_CHECK_EQ(static_cast<int>(value.payload[4]), static_cast<int>(payload[4]));
}

CRPC_TEST(frame_decoder_header_split) {
    const std::vector<std::byte> payload = bytes_of("split-header");
    const std::vector<std::byte> encoded = encode_simple(frame_type::request, 6, 0, payload);

    for (std::size_t split = 1; split < frame_header_size; ++split) {
        frame_decoder decoder;
        frame value;
        decoder.feed(span<const std::byte>(encoded.data(), split));
        CRPC_CHECK_EQ(static_cast<int>(decoder.next(value)),
                      static_cast<int>(frame_decoder::result::need_more));
        decoder.feed(span<const std::byte>(encoded.data() + split, encoded.size() - split));
        CRPC_CHECK_EQ(static_cast<int>(decoder.next(value)),
                      static_cast<int>(frame_decoder::result::frame_ready));
        CRPC_CHECK_EQ(value.payload.size(), payload.size());
    }
}

CRPC_TEST(frame_decoder_payload_split) {
    const std::vector<std::byte> payload = bytes_of("0123456789");
    const std::vector<std::byte> encoded = encode_simple(frame_type::request, 7, 0, payload);

    frame_decoder decoder;
    frame value;
    decoder.feed(span<const std::byte>(encoded.data(), frame_header_size + 4));
    CRPC_CHECK_EQ(static_cast<int>(decoder.next(value)),
                  static_cast<int>(frame_decoder::result::need_more));
    decoder.feed(span<const std::byte>(encoded.data() + frame_header_size + 4,
                                      encoded.size() - frame_header_size - 4));
    CRPC_CHECK_EQ(static_cast<int>(decoder.next(value)),
                  static_cast<int>(frame_decoder::result::frame_ready));
    CRPC_CHECK_EQ(value.payload.size(), payload.size());
}

CRPC_TEST(frame_decoder_two_frames_in_one_feed) {
    const std::vector<std::byte> first_payload = bytes_of("one");
    const std::vector<std::byte> second_payload = bytes_of("two");
    const std::vector<std::byte> first =
        encode_simple(frame_type::request, 11, 0, first_payload);
    const std::vector<std::byte> second =
        encode_simple(frame_type::response, 12, 0, second_payload);

    std::vector<std::byte> both = first;
    both.insert(both.end(), second.begin(), second.end());

    frame_decoder decoder;
    frame value;
    decoder.feed(as_span(both));

    CRPC_CHECK_EQ(static_cast<int>(decoder.next(value)),
                  static_cast<int>(frame_decoder::result::frame_ready));
    CRPC_CHECK_EQ(value.header.stream_id, 11u);
    CRPC_CHECK_EQ(value.payload.size(), first_payload.size());

    CRPC_CHECK_EQ(static_cast<int>(decoder.next(value)),
                  static_cast<int>(frame_decoder::result::frame_ready));
    CRPC_CHECK_EQ(value.header.stream_id, 12u);
    CRPC_CHECK_EQ(value.payload.size(), second_payload.size());

    CRPC_CHECK_EQ(static_cast<int>(decoder.next(value)),
                  static_cast<int>(frame_decoder::result::need_more));
}

CRPC_TEST(frame_decoder_zero_length_payload) {
    const std::vector<std::byte> empty;
    const std::vector<std::byte> encoded =
        encode_simple(frame_type::settings_ack, 0, 0, empty);

    frame_decoder decoder;
    decoder.feed(as_span(encoded));
    frame value;
    CRPC_CHECK_EQ(static_cast<int>(decoder.next(value)),
                  static_cast<int>(frame_decoder::result::frame_ready));
    CRPC_CHECK_EQ(value.header.payload_len, 0u);
    CRPC_CHECK(value.payload.empty());
    CRPC_CHECK_EQ(decoder.buffered(), static_cast<std::size_t>(0));
}

CRPC_TEST(frame_decoder_rejects_hostile_length) {
    const std::vector<std::byte> raw =
        raw_header(100u * 1024u * 1024u, static_cast<u8>(frame_type::request), 0, 0, 1, 0);

    frame_decoder decoder;
    decoder.feed(as_span(raw));
    frame value;
    const frame_decoder::result result = decoder.next(value);
    CRPC_CHECK_EQ(static_cast<int>(result), static_cast<int>(frame_decoder::result::error));
    CRPC_CHECK_EQ(static_cast<int>(decoder.error()), static_cast<int>(rpc_code::protocol_error));
    CRPC_CHECK_EQ(decoder.buffered(), static_cast<std::size_t>(16));
}

CRPC_TEST(frame_decoder_rejects_unknown_type) {
    const std::vector<std::byte> raw = raw_header(0, 0x0D, 0, 0, 0, 0);
    frame_decoder decoder;
    decoder.feed(as_span(raw));
    frame value;
    CRPC_CHECK_EQ(static_cast<int>(decoder.next(value)),
                  static_cast<int>(frame_decoder::result::error));
    CRPC_CHECK_EQ(static_cast<int>(decoder.error()), static_cast<int>(rpc_code::protocol_error));
}

CRPC_TEST(frame_decoder_ordered_extension_split) {
    const std::vector<std::byte> payload = bytes_of("lane");
    const std::vector<std::byte> encoded =
        encode_simple(frame_type::request, 8, frame_flag_ordered, payload,
                      ordered_extension{0xAAAABBBBCCCCDDDDull, 0x1234567890ABCDEFull});

    frame_decoder decoder;
    frame value;
    decoder.feed(span<const std::byte>(encoded.data(), frame_header_size + 8));
    CRPC_CHECK_EQ(static_cast<int>(decoder.next(value)),
                  static_cast<int>(frame_decoder::result::need_more));
    decoder.feed(span<const std::byte>(encoded.data() + frame_header_size + 8,
                                      encoded.size() - frame_header_size - 8));
    CRPC_CHECK_EQ(static_cast<int>(decoder.next(value)),
                  static_cast<int>(frame_decoder::result::frame_ready));
    CRPC_CHECK_EQ(value.ordered.lane_id, 0xAAAABBBBCCCCDDDDull);
    CRPC_CHECK_EQ(value.ordered.lane_seq, 0x1234567890ABCDEFull);
    CRPC_CHECK_EQ(value.payload.size(), payload.size());
}

CRPC_TEST(frame_decoder_crc_corruption_detected) {
    const std::vector<std::byte> payload = bytes_of("crc-payload");
    std::vector<std::byte> encoded =
        encode_simple(frame_type::request, 13, frame_flag_crc32c, payload);
    encoded[16] = static_cast<std::byte>(static_cast<unsigned>(encoded[16]) ^ 0x80u);

    frame_decoder decoder;
    decoder.feed(as_span(encoded));
    frame value;
    CRPC_CHECK_EQ(static_cast<int>(decoder.next(value)),
                  static_cast<int>(frame_decoder::result::error));
    CRPC_CHECK_EQ(static_cast<int>(decoder.error()), static_cast<int>(rpc_code::protocol_error));
}

CRPC_TEST(frame_decoder_payload_buffer_copy) {
    std::vector<std::byte> storage(64);
    frame_decoder decoder(span<std::byte>(storage.data(), storage.size()), 64);
    CRPC_CHECK_EQ(decoder.max_frame(), 64u);

    const std::vector<std::byte> payload = bytes_of("copy-me");
    const std::vector<std::byte> encoded = encode_simple(frame_type::request, 14, 0, payload);
    decoder.feed(as_span(encoded));

    frame value;
    CRPC_CHECK_EQ(static_cast<int>(decoder.next(value)),
                  static_cast<int>(frame_decoder::result::frame_ready));
    CRPC_CHECK_EQ(value.payload.data(), storage.data());
    CRPC_CHECK_EQ(value.payload.size(), payload.size());
    for (std::size_t i = 0; i < payload.size(); ++i) {
        CRPC_CHECK_EQ(static_cast<int>(value.payload[i]), static_cast<int>(payload[i]));
    }
}

CRPC_TEST(frame_decoder_payload_buffer_too_small) {
    std::vector<std::byte> storage(8);
    frame_decoder decoder(span<std::byte>(storage.data(), storage.size()), 64);

    const std::vector<std::byte> payload(32, static_cast<std::byte>(0x5A));
    const std::vector<std::byte> encoded = encode_simple(frame_type::request, 15, 0, payload);
    decoder.feed(as_span(encoded));

    frame value;
    CRPC_CHECK_EQ(static_cast<int>(decoder.next(value)),
                  static_cast<int>(frame_decoder::result::error));
    CRPC_CHECK_EQ(static_cast<int>(decoder.error()),
                  static_cast<int>(rpc_code::resource_exhausted));
}

CRPC_TEST(frame_decoder_reset_after_error) {
    const std::vector<std::byte> bad = raw_header(0, 0x0D, 0, 0, 0, 0);
    frame_decoder decoder;
    decoder.feed(as_span(bad));
    frame value;
    CRPC_CHECK_EQ(static_cast<int>(decoder.next(value)),
                  static_cast<int>(frame_decoder::result::error));
    CRPC_CHECK(decoder.failed());

    decoder.reset();
    CRPC_CHECK_FALSE(decoder.failed());
    CRPC_CHECK_EQ(decoder.buffered(), static_cast<std::size_t>(0));

    const std::vector<std::byte> payload = bytes_of("again");
    const std::vector<std::byte> good = encode_simple(frame_type::request, 16, 0, payload);
    decoder.feed(as_span(good));
    CRPC_CHECK_EQ(static_cast<int>(decoder.next(value)),
                  static_cast<int>(frame_decoder::result::frame_ready));
    CRPC_CHECK_EQ(value.payload.size(), payload.size());
}

CRPC_TEST(frame_decoder_clamps_max_frame) {
    frame_decoder decoder(0xFFFFFFFFu);
    CRPC_CHECK_EQ(decoder.max_frame(), max_frame_hard_cap);
}

// --- Mutation loops ----------------------------------------------------------

CRPC_TEST(frame_decoder_fuzz_random_input_never_crashes) {
    u64 state = 0x9E3779B97F4A7C15ull;
    for (int iteration = 0; iteration < 2000; ++iteration) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        const std::size_t length = static_cast<std::size_t>(state % 257u);
        std::vector<std::byte> data(length);
        for (std::size_t i = 0; i < length; ++i) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            data[i] = static_cast<std::byte>(state & 0xFFu);
        }

        frame_decoder decoder;
        frame value;
        std::size_t offset = 0;
        int guard = 0;
        while (offset < data.size() && guard < 512 && !decoder.failed()) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            const std::size_t chunk = 1 + static_cast<std::size_t>(state % 23u);
            const std::size_t take = std::min(chunk, data.size() - offset);
            decoder.feed(span<const std::byte>(data.data() + offset, take));
            offset += take;
            for (;;) {
                const frame_decoder::result result = decoder.next(value);
                if (result != frame_decoder::result::frame_ready) {
                    break;
                }
            }
            ++guard;
        }

        frame one_shot;
        (void)decode_frame(span<const std::byte>(data.data(), data.size()), one_shot);
    }
    CRPC_CHECK(true);
}

CRPC_TEST(frame_decoder_fuzz_mutated_frames_never_crashes) {
    u64 state = 0x1234567890ABCDEFull;
    const std::vector<std::byte> payload = bytes_of("abcdefghijklmnop");

    for (int iteration = 0; iteration < 1000; ++iteration) {
        frame value;
        value.header.type = frame_type::request;
        value.header.stream_id = 1u + static_cast<u32>(iteration % 7);
        value.header.flags = (iteration % 2 == 0) ? frame_flag_crc32c : frame_flag_ordered;
        value.ordered = ordered_extension{static_cast<u64>(iteration),
                                          static_cast<u64>(iteration) * 3u};
        value.payload = as_span(payload);

        std::vector<std::byte> encoded(encoded_frame_size(value));
        std::size_t written = 0;
        const status encode_status = encode_frame(as_writable_span(encoded), value, written);
        CRPC_CHECK(encode_status.ok());
        if (!encode_status.ok()) {
            continue;
        }
        encoded.resize(written);

        state = state * 6364136223846793005ull + 1442695040888963407ull;
        const std::size_t mutations = 1 + static_cast<std::size_t>(state % 3u);
        for (std::size_t m = 0; m < mutations && !encoded.empty(); ++m) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            const std::size_t index = static_cast<std::size_t>(state % encoded.size());
            encoded[index] = static_cast<std::byte>(
                static_cast<unsigned>(encoded[index]) ^ static_cast<unsigned>(state & 0xFFu));
        }

        frame_decoder decoder;
        frame decoded;
        decoder.feed(as_span(encoded));
        while (decoder.next(decoded) == frame_decoder::result::frame_ready) {
        }

        frame one_shot;
        (void)decode_frame(as_span(encoded), one_shot);
    }
    CRPC_CHECK(true);
}

CRPC_TEST_MAIN()
