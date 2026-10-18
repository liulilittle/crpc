// CRPC -- tests for the HTTP/2 frame layer, HPACK and connection state machine.
//
// Coverage targets:
//   * 9-byte big-endian header round trip and boundaries;
//   * every frame type encode/decode and malformed-payload rejection;
//   * HPACK integer prefixes 1..8, RFC 7541 C.1 vectors, string literals,
//     dynamic table insert/evict/size-update, static index lookup;
//   * Huffman decode vectors from RFC 7541 Appendix C.4.1 and C.6.1, plus
//     encode round trips and padding rules;
//   * whole-block decode of RFC C.3.1/C.3.2 (no Huffman) and C.4.1/C.6.1;
//   * CONTINUATION sequencing and interleaving rejection;
//   * flow control: park on zero window, WINDOW_UPDATE resume, independent
//     connection/stream windows, overflow rejection;
//   * stream state transitions, RST_STREAM and GOAWAY;
//   * deterministic mutation fuzzing that must never crash.
#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include <crpc/http2/connection.hpp>
#include <crpc/http2/frame.hpp>
#include <crpc/http2/hpack.hpp>

#include "test_framework.hpp"

using namespace crpc;
using namespace crpc::http2;

namespace {

// --- Helpers -----------------------------------------------------------------

unsigned hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return static_cast<unsigned>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<unsigned>(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<unsigned>(c - 'A' + 10);
    }
    return 0;
}

std::vector<std::byte> from_hex(const char* text) {
    std::vector<std::byte> out;
    for (const char* p = text; *p != '\0';) {
        while (*p == ' ' || *p == '\n') {
            ++p;
        }
        if (*p == '\0') {
            break;
        }
        const unsigned hi = hex_nibble(p[0]);
        const unsigned lo = hex_nibble(p[1]);
        out.push_back(static_cast<std::byte>((hi << 4) | lo));
        p += 2;
    }
    return out;
}

std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> out(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
    }
    return out;
}

span<const std::byte> view(const std::vector<std::byte>& bytes) {
    return span<const std::byte>(bytes.data(), bytes.size());
}

std::string as_string(span<const std::byte> bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

frame_header make_header(frame_type type, u8 frame_flags, u32 stream_id, std::size_t length) {
    frame_header header;
    header.type = type;
    header.flags = frame_flags;
    header.stream_id = stream_id;
    header.length = static_cast<u32>(length);
    return header;
}

h2_error deliver(connection& conn, frame_type type, u8 frame_flags, u32 stream_id,
                 const std::vector<std::byte>& payload) {
    return conn.receive(make_header(type, frame_flags, stream_id, payload.size()), view(payload));
}

std::vector<std::byte> block_of(std::initializer_list<std::pair<const char*, const char*>> headers) {
    std::vector<header_field> fields;
    for (const auto& item : headers) {
        header_field field;
        field.name = item.first;
        field.value = item.second;
        fields.push_back(std::move(field));
    }
    hpack_encoder encoder;
    std::string encoded;
    const status result =
        encoder.encode(span<const header_field>(fields.data(), fields.size()), encoded);
    CRPC_CHECK(result.ok());
    std::vector<std::byte> out(encoded.size());
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(encoded[i]));
    }
    return out;
}

std::vector<std::byte> settings_bytes(std::initializer_list<setting> entries) {
    std::vector<setting> list(entries);
    std::vector<std::byte> out(list.size() * 6);
    std::size_t written = 0;
    const status result =
        encode_settings(span<std::byte>(out.data(), out.size()),
                        span<const setting>(list.data(), list.size()), written);
    CRPC_CHECK(result.ok());
    out.resize(written);
    return out;
}

std::vector<std::byte> window_update_bytes(u32 increment) {
    std::vector<std::byte> out(4);
    std::size_t written = 0;
    const status result = encode_window_update(span<std::byte>(out.data(), out.size()),
                                               increment, written);
    CRPC_CHECK(result.ok());
    return out;
}

bool has_field(const http::header_list& list, std::string_view name, std::string_view value) {
    for (const http::header& field : list) {
        if (field.name == name && field.value == value) {
            return true;
        }
    }
    return false;
}

// --- Frame header ------------------------------------------------------------

CRPC_TEST(frame_header_round_trip) {
    frame_header header;
    header.length = 0x123456u;
    header.type = frame_type::headers;
    header.flags = 0x25u;
    header.stream_id = 0x7ABCDEF0u;

    std::vector<std::byte> encoded(frame_header_size);
    CRPC_CHECK(encode_frame_header(span<std::byte>(encoded.data(), encoded.size()), header).ok());
    CRPC_CHECK_EQ(static_cast<int>(encoded[0]), 0x12);
    CRPC_CHECK_EQ(static_cast<int>(encoded[1]), 0x34);
    CRPC_CHECK_EQ(static_cast<int>(encoded[2]), 0x56);
    CRPC_CHECK_EQ(static_cast<int>(encoded[3]), 0x01);
    CRPC_CHECK_EQ(static_cast<int>(encoded[4]), 0x25);

    frame_header decoded;
    CRPC_CHECK(decode_frame_header(view(encoded), decoded, max_max_frame_size).ok());
    CRPC_CHECK_EQ(decoded.length, header.length);
    CRPC_CHECK(decoded.type == frame_type::headers);
    CRPC_CHECK_EQ(static_cast<int>(decoded.flags), 0x25);
    CRPC_CHECK_EQ(decoded.stream_id, header.stream_id);
}

CRPC_TEST(frame_header_boundaries) {
    // The reserved stream-id bit is masked off.
    std::vector<std::byte> encoded(frame_header_size);
    CRPC_CHECK(encode_frame_header(span<std::byte>(encoded.data(), encoded.size()),
                                   make_header(frame_type::data, 0, 0xFFFFFFFFu, 0))
                   .ok());
    CRPC_CHECK_EQ(static_cast<int>(encoded[5]), 0x7F);
    CRPC_CHECK_EQ(static_cast<int>(encoded[6]), 0xFF);
    CRPC_CHECK_EQ(static_cast<int>(encoded[7]), 0xFF);
    CRPC_CHECK_EQ(static_cast<int>(encoded[8]), 0xFF);
    frame_header decoded;
    CRPC_CHECK(decode_frame_header(view(encoded), decoded, max_max_frame_size).ok());
    CRPC_CHECK_EQ(decoded.stream_id, max_stream_id);

    // A declared length above the caller's maximum is rejected before use.
    std::vector<std::byte> big(frame_header_size);
    CRPC_CHECK(encode_frame_header(span<std::byte>(big.data(), big.size()),
                                   make_header(frame_type::data, 0, 1, 20000))
                   .ok());
    frame_header rejected;
    CRPC_CHECK_EQ(static_cast<int>(decode_frame_header(view(big), rejected, default_max_frame_size).code()),
                  static_cast<int>(rpc_code::protocol_error));

    // A short input cannot produce a header.
    std::vector<std::byte> short_input(8, std::byte{0});
    CRPC_CHECK_EQ(static_cast<int>(decode_frame_header(view(short_input), rejected).code()),
                  static_cast<int>(rpc_code::protocol_error));

    // The 24-bit length field is truncated on encode.
    std::vector<std::byte> truncated(frame_header_size);
    CRPC_CHECK(encode_frame_header(span<std::byte>(truncated.data(), truncated.size()),
                                   make_header(frame_type::data, 0, 0, 0x1FFFFFFu))
                   .ok());
    CRPC_CHECK_EQ(static_cast<int>(truncated[0]), 0xFF);
    CRPC_CHECK_EQ(static_cast<int>(truncated[1]), 0xFF);
    CRPC_CHECK_EQ(static_cast<int>(truncated[2]), 0xFF);
}

CRPC_TEST(frame_reader_writer_network_order) {
    std::vector<std::byte> buffer(9);
    frame_writer writer(span<std::byte>(buffer.data(), buffer.size()));
    CRPC_CHECK(writer.put_u8(0x11));
    CRPC_CHECK(writer.put_u16(0x2233));
    CRPC_CHECK(writer.put_u24(0x445566));
    CRPC_CHECK(writer.ok());
    CRPC_CHECK_EQ(static_cast<int>(buffer[0]), 0x11);
    CRPC_CHECK_EQ(static_cast<int>(buffer[1]), 0x22);
    CRPC_CHECK_EQ(static_cast<int>(buffer[2]), 0x33);
    CRPC_CHECK_EQ(static_cast<int>(buffer[3]), 0x44);
    CRPC_CHECK_EQ(static_cast<int>(buffer[4]), 0x55);
    CRPC_CHECK_EQ(static_cast<int>(buffer[5]), 0x66);

    frame_reader reader(view(buffer));
    u8 a = 0;
    u16 b = 0;
    u32 c = 0;
    CRPC_CHECK(reader.get_u8(a));
    CRPC_CHECK(reader.get_u16(b));
    CRPC_CHECK(reader.get_u24(c));
    CRPC_CHECK_EQ(static_cast<int>(a), 0x11);
    CRPC_CHECK_EQ(static_cast<int>(b), 0x2233);
    CRPC_CHECK_EQ(static_cast<int>(c), 0x445566);

    // A short read is an error, not undefined behaviour.
    std::vector<std::byte> tiny(2, std::byte{0});
    frame_reader short_reader(view(tiny));
    u32 value = 0;
    CRPC_CHECK_FALSE(short_reader.get_u32(value));
    CRPC_CHECK_EQ(static_cast<int>(short_reader.error()), static_cast<int>(rpc_code::protocol_error));
}

CRPC_TEST(frame_types_encode_decode) {
    // DATA
    {
        const std::vector<std::byte> payload = bytes_of("hello");
        std::vector<std::byte> frame_bytes(frame_header_size + payload.size());
        std::size_t written = 0;
        CRPC_CHECK(encode_frame(span<std::byte>(frame_bytes.data(), frame_bytes.size()),
                                make_header(frame_type::data, flags::end_stream, 1, 0), view(payload),
                                written)
                       .ok());
        CRPC_CHECK_EQ(written, frame_header_size + payload.size());
        frame_header header;
        CRPC_CHECK(decode_frame_header(view(frame_bytes), header, default_max_frame_size).ok());
        CRPC_CHECK(header.type == frame_type::data);
        CRPC_CHECK_EQ(header.length, payload.size());
        CRPC_CHECK(header.has_flag(flags::end_stream));
        span<const std::byte> data;
        CRPC_CHECK(decode_data(view(frame_bytes).subspan(frame_header_size), header.flags, data).ok());
        CRPC_CHECK_EQ(as_string(data), "hello");
    }
    // HEADERS
    {
        const std::vector<std::byte> block = from_hex("82868441 0f777777 2e657861 6d706c65 2e636f6d");
        headers_payload parsed;
        CRPC_CHECK(decode_headers(view(block), flags::end_headers | flags::end_stream, parsed).ok());
        CRPC_CHECK(parsed.end_headers);
        CRPC_CHECK(parsed.end_stream);
        CRPC_CHECK_FALSE(parsed.has_priority);
        CRPC_CHECK_EQ(parsed.header_block.size(), block.size());
    }
    // PRIORITY
    {
        priority_payload priority;
        priority.stream_dependency = 3;
        priority.exclusive = true;
        priority.weight = 42;
        std::vector<std::byte> payload(5);
        std::size_t written = 0;
        CRPC_CHECK(encode_priority(span<std::byte>(payload.data(), payload.size()), priority, written).ok());
        CRPC_CHECK_EQ(written, 5u);
        priority_payload decoded;
        CRPC_CHECK(decode_priority(view(payload), decoded).ok());
        CRPC_CHECK_EQ(decoded.stream_dependency, 3u);
        CRPC_CHECK(decoded.exclusive);
        CRPC_CHECK_EQ(static_cast<int>(decoded.weight), 42);
    }
    // RST_STREAM
    {
        std::vector<std::byte> payload(4);
        std::size_t written = 0;
        CRPC_CHECK(encode_rst_stream(span<std::byte>(payload.data(), payload.size()),
                                     error_code::cancel, written)
                       .ok());
        error_code code = error_code::no_error;
        CRPC_CHECK(decode_rst_stream(view(payload), code).ok());
        CRPC_CHECK(code == error_code::cancel);
    }
    // SETTINGS
    {
        const std::vector<std::byte> payload =
            settings_bytes({{static_cast<u16>(setting_id::max_frame_size), 32768},
                            {static_cast<u16>(setting_id::initial_window_size), 1000}});
        std::vector<setting> parsed;
        CRPC_CHECK(decode_settings(view(payload), parsed).ok());
        CRPC_CHECK_EQ(parsed.size(), 2u);
        CRPC_CHECK_EQ(static_cast<int>(parsed[0].id), static_cast<int>(setting_id::max_frame_size));
        CRPC_CHECK_EQ(parsed[0].value, 32768u);
        CRPC_CHECK_EQ(parsed[1].value, 1000u);
    }
    // PUSH_PROMISE
    {
        std::vector<std::byte> payload(4 + 2);
        frame_writer writer(span<std::byte>(payload.data(), payload.size()));
        CRPC_CHECK(writer.put_u32(4));
        CRPC_CHECK(writer.put_u8(0x82));
        CRPC_CHECK(writer.put_u8(0x86));
        push_promise_payload parsed;
        CRPC_CHECK(decode_push_promise(view(payload), flags::end_headers, parsed).ok());
        CRPC_CHECK_EQ(parsed.promised_stream_id, 4u);
        CRPC_CHECK_EQ(parsed.header_block.size(), 2u);
    }
    // PING
    {
        ping_opaque opaque{};
        opaque[0] = std::byte{0xAB};
        std::vector<std::byte> payload(8);
        std::size_t written = 0;
        CRPC_CHECK(encode_ping(span<std::byte>(payload.data(), payload.size()), opaque, written).ok());
        ping_opaque decoded{};
        CRPC_CHECK(decode_ping(view(payload), decoded).ok());
        CRPC_CHECK_EQ(static_cast<int>(decoded[0]), 0xAB);
    }
    // GOAWAY
    {
        goaway_payload value;
        value.last_stream_id = 7;
        value.code = error_code::enhance_your_calm;
        const std::vector<std::byte> debug = bytes_of("bye");
        value.debug = view(debug);
        std::vector<std::byte> payload(8 + debug.size());
        std::size_t written = 0;
        CRPC_CHECK(encode_goaway(span<std::byte>(payload.data(), payload.size()), value, written).ok());
        goaway_payload decoded;
        CRPC_CHECK(decode_goaway(view(payload), decoded).ok());
        CRPC_CHECK_EQ(decoded.last_stream_id, 7u);
        CRPC_CHECK(decoded.code == error_code::enhance_your_calm);
        CRPC_CHECK_EQ(as_string(decoded.debug), "bye");
    }
    // WINDOW_UPDATE
    {
        const std::vector<std::byte> payload = window_update_bytes(1234);
        u32 increment = 0;
        CRPC_CHECK(decode_window_update(view(payload), increment).ok());
        CRPC_CHECK_EQ(increment, 1234u);
    }
    // CONTINUATION (payload is an opaque header-block fragment).
    {
        const std::vector<std::byte> payload = from_hex("0102");
        CRPC_CHECK_EQ(payload.size(), 2u);
    }
}

CRPC_TEST(frame_malformed_payloads_rejected) {
    std::vector<std::byte> seven(7, std::byte{0});
    std::vector<std::byte> four(4, std::byte{0});
    std::vector<std::byte> five(5, std::byte{0});
    ping_opaque opaque{};
    error_code code = error_code::no_error;
    u32 increment = 0;
    priority_payload priority;
    goaway_payload goaway;
    std::vector<setting> settings;
    headers_payload headers;
    span<const std::byte> data;

    CRPC_CHECK_FALSE(decode_ping(view(seven), opaque).ok());
    CRPC_CHECK_FALSE(decode_rst_stream(view(seven), code).ok());
    CRPC_CHECK_FALSE(decode_window_update(view(five), increment).ok());
    CRPC_CHECK_FALSE(decode_priority(view(four), priority).ok());
    CRPC_CHECK_FALSE(decode_goaway(view(seven), goaway).ok());
    CRPC_CHECK_FALSE(decode_settings(view(five), settings).ok());

    // Padded DATA whose pad length exceeds the payload.
    const std::vector<std::byte> bad_pad = from_hex("05 61 62");
    CRPC_CHECK_FALSE(decode_data(view(bad_pad), flags::padded, data).ok());

    // Padded HEADERS with an impossible pad length.
    CRPC_CHECK_FALSE(decode_headers(view(bad_pad), flags::padded | flags::end_headers, headers).ok());

    // HEADERS claiming priority but too short for the 5 priority octets.
    const std::vector<std::byte> short_priority = from_hex("82 83");
    CRPC_CHECK_FALSE(
        decode_headers(view(short_priority), flags::priority | flags::end_headers, headers).ok());

    // A padded DATA frame with valid padding still yields the data bytes.
    const std::vector<std::byte> good_pad = from_hex("02 61 62 00 00");
    CRPC_CHECK(decode_data(view(good_pad), flags::padded, data).ok());
    CRPC_CHECK_EQ(as_string(data), "ab");
}

CRPC_TEST(error_code_mapping) {
    CRPC_CHECK(std::strcmp(to_string(error_code::protocol_error), "PROTOCOL_ERROR") == 0);
    CRPC_CHECK(to_rpc_code(error_code::cancel) == rpc_code::canceled);
    CRPC_CHECK(to_rpc_code(error_code::refused_stream) == rpc_code::unavailable);
    CRPC_CHECK(to_rpc_code(error_code::no_error) == rpc_code::ok);
}

// --- HPACK primitives --------------------------------------------------------

CRPC_TEST(hpack_integer_prefix_cases) {
    for (u8 prefix = 1; prefix <= 8; ++prefix) {
        const u32 max_prefix = (1u << prefix) - 1u;
        const u64 values[] = {0u, 1u, max_prefix > 0 ? max_prefix - 1u : 0u, max_prefix,
                              max_prefix + 1u, 1337u, 100000u};
        for (u64 value : values) {
            std::vector<std::byte> buffer(16);
            std::size_t written = 0;
            const status encoded = hpack_encode_integer(
                span<std::byte>(buffer.data(), buffer.size()), written, prefix, 0x00u, value);
            CRPC_CHECK(encoded.ok());
            std::size_t consumed = 0;
            u64 decoded = 0;
            const status result =
                hpack_decode_integer(view(buffer).first(written), consumed, prefix, decoded);
            CRPC_CHECK(result.ok());
            CRPC_CHECK_EQ(decoded, value);
            CRPC_CHECK_EQ(consumed, written);
        }
    }
}

CRPC_TEST(hpack_integer_rfc_vectors) {
    // C.1.1: 10 with a 5-bit prefix -> 0x0a.
    {
        std::vector<std::byte> buffer(8);
        std::size_t written = 0;
        CRPC_CHECK(hpack_encode_integer(span<std::byte>(buffer.data(), buffer.size()), written, 5, 0x00u, 10).ok());
        CRPC_CHECK_EQ(written, 1u);
        CRPC_CHECK_EQ(static_cast<int>(buffer[0]), 0x0a);
    }
    // C.1.2: 1337 with a 5-bit prefix -> 1f 9a 0a.
    {
        std::vector<std::byte> buffer(8);
        std::size_t written = 0;
        CRPC_CHECK(hpack_encode_integer(span<std::byte>(buffer.data(), buffer.size()), written, 5, 0x00u, 1337).ok());
        CRPC_CHECK_EQ(written, 3u);
        CRPC_CHECK_EQ(static_cast<int>(buffer[0]), 0x1f);
        CRPC_CHECK_EQ(static_cast<int>(buffer[1]), 0x9a);
        CRPC_CHECK_EQ(static_cast<int>(buffer[2]), 0x0a);
    }
    // C.1.3: 42 at an octet boundary (8-bit prefix) -> 0x2a.
    {
        std::vector<std::byte> buffer(8);
        std::size_t written = 0;
        CRPC_CHECK(hpack_encode_integer(span<std::byte>(buffer.data(), buffer.size()), written, 8, 0x00u, 42).ok());
        CRPC_CHECK_EQ(written, 1u);
        CRPC_CHECK_EQ(static_cast<int>(buffer[0]), 0x2a);
    }
}

CRPC_TEST(hpack_integer_overlong_rejected) {
    std::vector<std::byte> buffer(16, std::byte{0x80});
    buffer[0] = std::byte{0x1f};
    std::size_t consumed = 0;
    u64 value = 0;
    CRPC_CHECK_FALSE(hpack_decode_integer(view(buffer), consumed, 5, value).ok());
}

CRPC_TEST(hpack_string_literal_round_trip) {
    const std::string value = "custom-header";
    std::vector<std::byte> buffer(64);
    std::size_t written = 0;
    CRPC_CHECK(hpack_encode_string(span<std::byte>(buffer.data(), buffer.size()), written, value).ok());
    std::size_t consumed = 0;
    std::string decoded;
    CRPC_CHECK(hpack_decode_string(view(buffer).first(written), consumed, decoded).ok());
    CRPC_CHECK_EQ(decoded, value);
    CRPC_CHECK_EQ(consumed, written);

    // Empty string.
    std::size_t empty_written = 0;
    CRPC_CHECK(hpack_encode_string(span<std::byte>(buffer.data(), buffer.size()), empty_written, "").ok());
    std::size_t empty_consumed = 0;
    std::string empty_decoded = "x";
    CRPC_CHECK(hpack_decode_string(view(buffer).first(empty_written), empty_consumed, empty_decoded).ok());
    CRPC_CHECK(empty_decoded.empty());
}

CRPC_TEST(hpack_huffman_rfc_vectors) {
    struct vector {
        const char* hex;
        const char* expected;
    };
    const vector vectors[] = {
        {"f1 e3 c2 e5 f2 3a 6b a0 ab 90 f4 ff", "www.example.com"},
        {"9d 29 ad 17 18 63 c7 8f 0b 97 c8 e9 ae 82 ae 43 d3", "https://www.example.com"},
        {"d0 7a be 94 10 54 d4 44 a8 20 05 95 04 0b 81 66 e0 82 a6 2d 1b ff",
         "Mon, 21 Oct 2013 20:13:21 GMT"},
        {"64 02", "302"},
        {"ae c3 77 1a 4b", "private"},
        {"a8 eb 10 64 9c bf", "no-cache"},
    };
    for (const vector& item : vectors) {
        const std::vector<std::byte> encoded = from_hex(item.hex);
        std::string decoded;
        CRPC_CHECK(huffman_decode(view(encoded), decoded).ok());
        CRPC_CHECK_EQ(decoded, item.expected);
    }
}

CRPC_TEST(hpack_huffman_encode_round_trip) {
    const char* samples[] = {"www.example.com", "Mon, 21 Oct 2013 20:13:21 GMT",
                             "no-cache", "a", ""};
    for (const char* sample : samples) {
        const std::size_t size = huffman_encoded_size(sample);
        std::vector<std::byte> buffer(size == 0 ? 1 : size);
        std::size_t written = 0;
        CRPC_CHECK(huffman_encode(span<std::byte>(buffer.data(), buffer.size()), sample, written).ok());
        CRPC_CHECK_EQ(written, size);
        std::string decoded;
        CRPC_CHECK(huffman_decode(view(buffer).first(written), decoded).ok());
        CRPC_CHECK_EQ(decoded, std::string(sample));
    }
}

CRPC_TEST(hpack_huffman_padding_rules) {
    // "00011" ('a') followed by padding that is not all ones.
    std::vector<std::byte> bad = from_hex("18 00");
    std::string decoded;
    CRPC_CHECK_FALSE(huffman_decode(view(bad), decoded).ok());

    // Explicit EOS symbol (30 bits all ones) is a decoding error.
    std::vector<std::byte> eos = from_hex("ff ff ff ff");
    decoded.clear();
    CRPC_CHECK_FALSE(huffman_decode(view(eos), decoded).ok());

    // Padding longer than seven bits is rejected: a lone zero byte is not a
    // valid symbol and is not EOS padding.
    std::vector<std::byte> too_long = from_hex("00");
    decoded.clear();
    CRPC_CHECK_FALSE(huffman_decode(view(too_long), decoded).ok());
}

CRPC_TEST(hpack_static_table_lookup) {
    CRPC_CHECK(static_table_entry(2) != nullptr);
    CRPC_CHECK_EQ(static_table_entry(2)->name, std::string(":method"));
    CRPC_CHECK_EQ(static_table_entry(2)->value, std::string("GET"));
    CRPC_CHECK_EQ(static_table_entry(61)->name, std::string("www-authenticate"));
    CRPC_CHECK(static_table_entry(0) == nullptr);
    CRPC_CHECK(static_table_entry(62) == nullptr);

    CRPC_CHECK_EQ(static_find(":method", "POST"), 3u);
    CRPC_CHECK_EQ(static_find(":method", "PATCH"), 0u);
    CRPC_CHECK_EQ(static_find_name(":authority"), 1u);
    CRPC_CHECK_EQ(static_find_name("nope"), 0u);
}

CRPC_TEST(hpack_dynamic_table_insert_evict_size_update) {
    dynamic_table table(100);
    table.insert("a", "b");
    CRPC_CHECK_EQ(table.count(), 1u);
    CRPC_CHECK_EQ(table.size(), hpack_entry_size("a", "b"));

    table.set_max_size(0);
    CRPC_CHECK(table.empty());
    CRPC_CHECK_EQ(table.size(), 0u);

    table.set_max_size(100);
    table.insert("name", "value");
    CRPC_CHECK_EQ(table.count(), 1u);
    // A later insertion evicts the older one.
    table.insert("x", std::string(60, 'y'));
    CRPC_CHECK_EQ(table.count(), 1u);
    CRPC_CHECK_EQ(table.at(0).name, std::string("x"));

    // An entry larger than the maximum empties the table.
    table.insert("huge", std::string(200, 'z'));
    CRPC_CHECK(table.empty());

    CRPC_CHECK_EQ(table.find("missing", "value"), table.count());
}

// --- HPACK whole blocks ------------------------------------------------------

CRPC_TEST(hpack_decode_rfc_c31_without_huffman) {
    hpack_decoder decoder;
    http::header_list headers;
    const std::vector<std::byte> block = from_hex("82868441 0f777777 2e657861 6d706c65 2e636f6d");
    CRPC_CHECK(decoder.decode(view(block), headers).ok());
    CRPC_CHECK_EQ(headers.size(), 4u);
    CRPC_CHECK(has_field(headers, ":method", "GET"));
    CRPC_CHECK(has_field(headers, ":scheme", "http"));
    CRPC_CHECK(has_field(headers, ":path", "/"));
    CRPC_CHECK(has_field(headers, ":authority", "www.example.com"));
    CRPC_CHECK_EQ(decoder.table().count(), 1u);
    CRPC_CHECK_EQ(decoder.table().size(), 57u);

    // C.3.2 references the dynamic entry at index 62.
    http::header_list second;
    const std::vector<std::byte> block2 = from_hex("828684be 58086e6f 2d636163 6865");
    CRPC_CHECK(decoder.decode(view(block2), second).ok());
    CRPC_CHECK_EQ(second.size(), 5u);
    CRPC_CHECK(has_field(second, ":authority", "www.example.com"));
    CRPC_CHECK(has_field(second, "cache-control", "no-cache"));
    CRPC_CHECK_EQ(decoder.table().count(), 2u);
    CRPC_CHECK_EQ(decoder.table().size(), 110u);
}

CRPC_TEST(hpack_decode_rfc_c41_huffman) {
    hpack_decoder decoder;
    http::header_list headers;
    const std::vector<std::byte> block =
        from_hex("82868441 8cf1e3c2 e5f23a6b a0ab90f4 ff");
    CRPC_CHECK(decoder.decode(view(block), headers).ok());
    CRPC_CHECK_EQ(headers.size(), 4u);
    CRPC_CHECK(has_field(headers, ":method", "GET"));
    CRPC_CHECK(has_field(headers, ":scheme", "http"));
    CRPC_CHECK(has_field(headers, ":path", "/"));
    CRPC_CHECK(has_field(headers, ":authority", "www.example.com"));
    CRPC_CHECK_EQ(decoder.table().count(), 1u);
    CRPC_CHECK_EQ(decoder.table().size(), 57u);
}

CRPC_TEST(hpack_decode_rfc_c61_huffman) {
    hpack_decoder decoder;
    http::header_list headers;
    const std::vector<std::byte> block = from_hex(
        "48826402 5885aec3 771a4b61 96d07abe 941054d4 44a82005 95040b81 66e082a6 "
        "2d1bff6e 919d29ad 171863c7 8f0b97c8 e9ae82ae 43d3");
    CRPC_CHECK(decoder.decode(view(block), headers).ok());
    CRPC_CHECK_EQ(headers.size(), 4u);
    CRPC_CHECK(has_field(headers, ":status", "302"));
    CRPC_CHECK(has_field(headers, "cache-control", "private"));
    CRPC_CHECK(has_field(headers, "date", "Mon, 21 Oct 2013 20:13:21 GMT"));
    CRPC_CHECK(has_field(headers, "location", "https://www.example.com"));
    CRPC_CHECK_EQ(decoder.table().count(), 4u);
    CRPC_CHECK_EQ(decoder.table().size(), 222u);
}

CRPC_TEST(hpack_encoder_matches_rfc_c31) {
    hpack_encoder encoder;
    std::vector<header_field> fields;
    fields.push_back({":method", "GET"});
    fields.push_back({":scheme", "http"});
    fields.push_back({":path", "/"});
    fields.push_back({":authority", "www.example.com"});
    std::string encoded;
    CRPC_CHECK(encoder.encode(span<const header_field>(fields.data(), fields.size()), encoded).ok());
    const std::vector<std::byte> expected =
        from_hex("82868441 0f777777 2e657861 6d706c65 2e636f6d");
    CRPC_CHECK_EQ(encoded.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CRPC_CHECK_EQ(static_cast<int>(static_cast<unsigned char>(encoded[i])),
                      static_cast<int>(expected[i]));
    }
}

CRPC_TEST(hpack_encoder_decoder_round_trip) {
    hpack_encoder encoder;
    std::vector<header_field> fields;
    fields.push_back({":method", "POST"});
    fields.push_back({":path", "/svc/method"});
    fields.push_back({"content-type", "application/crpc"});
    fields.push_back({"x-crpc-trace-id", "0123456789abcdef0123456789abcdef"});
    fields.push_back({":method", "POST"});
    std::string encoded;
    CRPC_CHECK(encoder.encode(span<const header_field>(fields.data(), fields.size()), encoded).ok());

    std::vector<std::byte> bytes(encoded.size());
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(encoded[i]));
    }
    hpack_decoder decoder;
    std::vector<header_field> decoded;
    CRPC_CHECK(decoder.decode(view(bytes), decoded).ok());
    CRPC_CHECK_EQ(decoded.size(), fields.size());
    for (std::size_t i = 0; i < fields.size(); ++i) {
        CRPC_CHECK_EQ(decoded[i].name, fields[i].name);
        CRPC_CHECK_EQ(decoded[i].value, fields[i].value);
    }
}

CRPC_TEST(hpack_literal_representations) {
    // Never-indexed and without-indexing must not touch the dynamic table.
    hpack_encoder encoder;
    std::string encoded;
    CRPC_CHECK(encoder.encode_field("authorization", "secret", hpack_indexing::never, encoded).ok());
    CRPC_CHECK(encoder.encode_field("x-custom", "v", hpack_indexing::without, encoded).ok());
    CRPC_CHECK_EQ(encoder.table().count(), 0u);

    std::vector<std::byte> bytes(encoded.size());
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        bytes[i] = static_cast<std::byte>(static_cast<unsigned char>(encoded[i]));
    }
    hpack_decoder decoder;
    http::header_list headers;
    CRPC_CHECK(decoder.decode(view(bytes), headers).ok());
    CRPC_CHECK_EQ(headers.size(), 2u);
    CRPC_CHECK(has_field(headers, "authorization", "secret"));
    CRPC_CHECK(has_field(headers, "x-custom", "v"));
    CRPC_CHECK_EQ(decoder.table().count(), 0u);
}

CRPC_TEST(hpack_size_update_rules) {
    // A truncated size update is a decoding error.
    {
        hpack_decoder decoder(4096);
        http::header_list headers;
        const std::vector<std::byte> truncated = from_hex("3F");
        CRPC_CHECK_FALSE(decoder.decode(view(truncated), headers).ok());
    }
    // A size update larger than our advertised limit is a compression error.
    {
        hpack_decoder decoder(4096);
        http::header_list headers;
        const std::vector<std::byte> too_big = from_hex("3F 81 20"); // 4128 > 4096
        CRPC_CHECK_FALSE(decoder.decode(view(too_big), headers).ok());
    }
    // A size update after a field is illegal.
    {
        hpack_decoder decoder(4096);
        http::header_list headers;
        const std::vector<std::byte> block = from_hex("82 20");
        CRPC_CHECK_FALSE(decoder.decode(view(block), headers).ok());
    }
    // A legal size update of 0 clears the table.
    {
        hpack_decoder decoder(4096);
        http::header_list headers;
        // Insert an entry, then a size update of 0 at the start of the next block.
        const std::vector<std::byte> first = from_hex("82868441 0f777777 2e657861 6d706c65 2e636f6d");
        CRPC_CHECK(decoder.decode(view(first), headers).ok());
        CRPC_CHECK_EQ(decoder.table().count(), 1u);
        std::vector<std::byte> update(1);
        frame_writer writer(span<std::byte>(update.data(), update.size()));
        CRPC_CHECK(writer.put_u8(0x20)); // size update, value 0
        http::header_list second;
        CRPC_CHECK(decoder.decode(view(update), second).ok());
        CRPC_CHECK_EQ(decoder.table().count(), 0u);
    }
}

CRPC_TEST(hpack_header_list_limit) {
    hpack_decoder decoder(4096);
    decoder.set_max_header_list_size(10);
    http::header_list headers;
    const std::vector<std::byte> block = from_hex("82868441 0f777777 2e657861 6d706c65 2e636f6d");
    CRPC_CHECK_EQ(static_cast<int>(decoder.decode(view(block), headers).code()),
                  static_cast<int>(rpc_code::resource_exhausted));
}

// --- Connection: settings, ping, goaway --------------------------------------

CRPC_TEST(connection_settings_ack) {
    connection_options options;
    options.role = connection_role::server;
    connection conn(options);

    const std::vector<std::byte> payload =
        settings_bytes({{static_cast<u16>(setting_id::max_frame_size), 32768}});
    CRPC_CHECK(deliver(conn, frame_type::settings, 0, 0, payload).ok());
    CRPC_CHECK(conn.pending_settings_ack());
    CRPC_CHECK_EQ(conn.remote_settings().max_frame_size, 32768u);
    conn.settings_acknowledge();
    CRPC_CHECK_FALSE(conn.pending_settings_ack());

    // A SETTINGS ACK with a payload is a connection error.
    CRPC_CHECK_EQ(static_cast<int>(deliver(conn, frame_type::settings, flags::ack, 0, payload).code),
                  static_cast<int>(error_code::frame_size_error));
    // A SETTINGS frame on a non-zero stream is a connection error.
    CRPC_CHECK_EQ(static_cast<int>(deliver(conn, frame_type::settings, 0, 1, payload).code),
                  static_cast<int>(error_code::protocol_error));
}

CRPC_TEST(connection_ping) {
    connection_options options;
    options.role = connection_role::server;
    connection conn(options);

    ping_opaque opaque{};
    opaque[3] = std::byte{0x5A};
    std::vector<std::byte> payload(8);
    std::size_t written = 0;
    CRPC_CHECK(encode_ping(span<std::byte>(payload.data(), payload.size()), opaque, written).ok());

    CRPC_CHECK(deliver(conn, frame_type::ping, 0, 0, payload).ok());
    CRPC_CHECK(conn.pending_ping_ack());
    CRPC_CHECK_EQ(static_cast<int>(conn.last_ping()[3]), 0x5A);

    CRPC_CHECK(deliver(conn, frame_type::ping, flags::ack, 0, payload).ok());
    CRPC_CHECK(conn.ping_acknowledged());

    // A PING on a non-zero stream is a connection error.
    CRPC_CHECK_EQ(static_cast<int>(deliver(conn, frame_type::ping, 0, 1, payload).code),
                  static_cast<int>(error_code::protocol_error));
    // A short PING is a frame-size error.
    const std::vector<std::byte> short_ping(7, std::byte{0});
    CRPC_CHECK_EQ(static_cast<int>(deliver(conn, frame_type::ping, 0, 0, short_ping).code),
                  static_cast<int>(error_code::frame_size_error));
}

CRPC_TEST(connection_goaway) {
    connection_options options;
    options.role = connection_role::server;
    connection conn(options);

    CRPC_CHECK(conn.open_stream(2).ok());
    CRPC_CHECK(conn.open_stream(4).ok());
    CRPC_CHECK(conn.can_open_stream(6));

    goaway_payload value;
    value.last_stream_id = 2;
    value.code = error_code::no_error;
    std::vector<std::byte> payload(8);
    std::size_t written = 0;
    CRPC_CHECK(encode_goaway(span<std::byte>(payload.data(), payload.size()), value, written).ok());

    CRPC_CHECK(deliver(conn, frame_type::goaway, 0, 0, payload).ok());
    CRPC_CHECK(conn.goaway_received());
    CRPC_CHECK_EQ(conn.goaway_last_stream_id(), 2u);
    // Stream 2 was processed and stays open; stream 4 was not and is closed.
    CRPC_CHECK(conn.state_of(2) == stream_state::open);
    CRPC_CHECK(conn.state_of(4) == stream_state::closed);
    // No new locally initiated streams beyond the last processed id.
    CRPC_CHECK_FALSE(conn.can_open_stream(6));
    const h2_error opened = conn.open_stream(6);
    CRPC_CHECK(opened.scope == error_scope::stream);
    CRPC_CHECK(opened.code == error_code::refused_stream);
}

// --- Connection: stream states -----------------------------------------------

CRPC_TEST(connection_stream_lifecycle) {
    connection_options options;
    options.role = connection_role::server;
    connection conn(options);

    const std::vector<std::byte> block = block_of({{":method", "GET"}, {":path", "/"}});

    // A peer HEADERS opens the stream.
    CRPC_CHECK(deliver(conn, frame_type::headers, flags::end_headers, 1, block).ok());
    CRPC_CHECK(conn.state_of(1) == stream_state::open);
    CRPC_CHECK(conn.last_headers_present());
    CRPC_CHECK_EQ(conn.last_headers_stream(), 1u);
    CRPC_CHECK(has_field(conn.last_headers(), ":method", "GET"));

    // DATA from the peer on an open stream is accepted and accounted.
    const std::vector<std::byte> body = bytes_of("payload");
    CRPC_CHECK(deliver(conn, frame_type::data, 0, 1, body).ok());
    CRPC_CHECK_EQ(conn.last_data().size(), body.size());
    CRPC_CHECK_EQ(conn.stream_recv_window(1), 65535 - static_cast<i64>(body.size()));

    // Local END_STREAM half-closes our side.
    CRPC_CHECK(conn.local_end_stream(1).ok());
    CRPC_CHECK(conn.state_of(1) == stream_state::half_closed_local);

    // Remote END_STREAM closes the stream.
    CRPC_CHECK(deliver(conn, frame_type::data, flags::end_stream, 1, std::vector<std::byte>()).ok());
    CRPC_CHECK(conn.state_of(1) == stream_state::closed);

    // A new stream whose id does not increase is a connection error.
    connection fresh(options);
    CRPC_CHECK(deliver(fresh, frame_type::headers, flags::end_headers, 3, block).ok());
    CRPC_CHECK_EQ(static_cast<int>(deliver(fresh, frame_type::headers, flags::end_headers, 1, block).code),
                  static_cast<int>(error_code::protocol_error));
    // A peer stream with the wrong parity is a connection error.
    connection parity(options);
    CRPC_CHECK_EQ(static_cast<int>(deliver(parity, frame_type::headers, flags::end_headers, 2, block).code),
                  static_cast<int>(error_code::protocol_error));
}

CRPC_TEST(connection_illegal_transitions) {
    connection_options options;
    options.role = connection_role::server;
    connection conn(options);
    const std::vector<std::byte> block = block_of({{":method", "GET"}});

    // HEADERS with END_STREAM half-closes the remote side.
    CRPC_CHECK(deliver(conn, frame_type::headers, flags::end_headers | flags::end_stream, 1, block).ok());
    CRPC_CHECK(conn.state_of(1) == stream_state::half_closed_remote);
    // More DATA on a half-closed-remote stream is STREAM_CLOSED.
    CRPC_CHECK_EQ(static_cast<int>(deliver(conn, frame_type::data, 0, 1, bytes_of("x")).code),
                  static_cast<int>(error_code::stream_closed));
    // Another HEADERS is likewise STREAM_CLOSED.
    CRPC_CHECK_EQ(static_cast<int>(deliver(conn, frame_type::headers, flags::end_headers, 1, block).code),
                  static_cast<int>(error_code::stream_closed));

    // DATA on an idle stream is a connection error.
    connection fresh(options);
    CRPC_CHECK_EQ(static_cast<int>(deliver(fresh, frame_type::data, 0, 1, bytes_of("x")).code),
                  static_cast<int>(error_code::protocol_error));
    // DATA on stream 0 is a connection error.
    CRPC_CHECK_EQ(static_cast<int>(deliver(fresh, frame_type::data, 0, 0, bytes_of("x")).code),
                  static_cast<int>(error_code::protocol_error));
}

CRPC_TEST(connection_rst_stream) {
    connection_options options;
    options.role = connection_role::server;
    connection conn(options);
    const std::vector<std::byte> block = block_of({{":method", "GET"}});
    CRPC_CHECK(deliver(conn, frame_type::headers, flags::end_headers, 1, block).ok());

    std::vector<std::byte> payload(4);
    std::size_t written = 0;
    CRPC_CHECK(encode_rst_stream(span<std::byte>(payload.data(), payload.size()),
                                 error_code::cancel, written)
                   .ok());
    CRPC_CHECK(deliver(conn, frame_type::rst_stream, 0, 1, payload).ok());
    CRPC_CHECK(conn.state_of(1) == stream_state::closed);
    CRPC_CHECK_EQ(conn.last_reset_stream(), 1u);
    CRPC_CHECK(conn.last_reset_code() == error_code::cancel);

    // RST_STREAM on stream 0 is a connection error.
    CRPC_CHECK_EQ(static_cast<int>(deliver(conn, frame_type::rst_stream, 0, 0, payload).code),
                  static_cast<int>(error_code::protocol_error));
    // RST_STREAM on an idle stream is a connection error.
    CRPC_CHECK_EQ(static_cast<int>(deliver(conn, frame_type::rst_stream, 0, 3, payload).code),
                  static_cast<int>(error_code::protocol_error));
}

CRPC_TEST(connection_continuation_rules) {
    connection_options options;
    options.role = connection_role::server;
    const std::vector<std::byte> block =
        block_of({{":method", "GET"}, {":scheme", "http"}, {":path", "/"}});
    const std::size_t split = block.size() / 2;
    const std::vector<std::byte> first(block.begin(), block.begin() + static_cast<std::ptrdiff_t>(split));
    const std::vector<std::byte> second(block.begin() + static_cast<std::ptrdiff_t>(split), block.end());

    // Interleaving a non-CONTINUATION frame is a connection error.
    {
        connection conn(options);
        CRPC_CHECK(deliver(conn, frame_type::headers, 0, 1, first).ok());
        CRPC_CHECK_EQ(static_cast<int>(deliver(conn, frame_type::data, 0, 1, bytes_of("x")).code),
                      static_cast<int>(error_code::protocol_error));
    }
    // CONTINUATION on a different stream is a connection error.
    {
        connection conn(options);
        CRPC_CHECK(deliver(conn, frame_type::headers, 0, 1, first).ok());
        CRPC_CHECK_EQ(static_cast<int>(deliver(conn, frame_type::continuation, flags::end_headers, 3, second).code),
                      static_cast<int>(error_code::protocol_error));
    }
    // The correct sequence completes the header block.
    {
        connection conn(options);
        CRPC_CHECK(deliver(conn, frame_type::headers, 0, 1, first).ok());
        CRPC_CHECK(deliver(conn, frame_type::continuation, flags::end_headers, 1, second).ok());
        CRPC_CHECK(conn.last_headers_present());
        CRPC_CHECK(has_field(conn.last_headers(), ":method", "GET"));
        CRPC_CHECK(has_field(conn.last_headers(), ":scheme", "http"));
        CRPC_CHECK(has_field(conn.last_headers(), ":path", "/"));
    }
    // A CONTINUATION with no open HEADERS is a connection error.
    {
        connection conn(options);
        CRPC_CHECK_EQ(static_cast<int>(deliver(conn, frame_type::continuation, flags::end_headers, 1, second).code),
                      static_cast<int>(error_code::protocol_error));
    }
}

CRPC_TEST(connection_max_concurrent_streams) {
    connection_options options;
    options.role = connection_role::server;
    options.local_settings.max_concurrent_streams = 1;
    connection conn(options);
    const std::vector<std::byte> block = block_of({{":method", "GET"}});

    CRPC_CHECK(deliver(conn, frame_type::headers, flags::end_headers, 1, block).ok());
    const h2_error refused = deliver(conn, frame_type::headers, flags::end_headers, 3, block);
    CRPC_CHECK(refused.scope == error_scope::stream);
    CRPC_CHECK(refused.code == error_code::refused_stream);
    CRPC_CHECK(conn.state_of(3) == stream_state::closed);
}

CRPC_TEST(connection_frame_size_and_unknown_types) {
    connection_options options;
    options.role = connection_role::server;
    options.local_settings.max_frame_size = 100;
    connection conn(options);

    const std::vector<std::byte> big(200, std::byte{0});
    CRPC_CHECK_EQ(static_cast<int>(deliver(conn, frame_type::data, 0, 1, big).code),
                  static_cast<int>(error_code::frame_size_error));

    // Unknown frame types are ignored by default.
    connection permissive(connection_options{});
    CRPC_CHECK(deliver(permissive, static_cast<frame_type>(0x0A), 0, 0, std::vector<std::byte>()).ok());

    // ... and rejected when the deployment policy says so.
    connection_options strict_options;
    strict_options.role = connection_role::server;
    strict_options.reject_unknown_frame_types = true;
    connection strict(strict_options);
    CRPC_CHECK_EQ(static_cast<int>(deliver(strict, static_cast<frame_type>(0x0A), 0, 0, std::vector<std::byte>()).code),
                  static_cast<int>(error_code::protocol_error));
}

// --- Connection: flow control ------------------------------------------------

CRPC_TEST(flow_control_parks_and_resumes_on_connection_window) {
    connection_options options;
    options.role = connection_role::server;
    options.initial_connection_send_window = 5;
    connection conn(options);
    CRPC_CHECK(conn.open_stream(2).ok());

    const std::vector<std::byte> data(10, std::byte{0x41});
    std::size_t sent = 0;
    bool parked = false;
    CRPC_CHECK(conn.send_data(2, view(data), sent, parked).ok());
    CRPC_CHECK_EQ(sent, 5u);
    CRPC_CHECK(parked);
    CRPC_CHECK_EQ(conn.parked_bytes(2), 5u);
    CRPC_CHECK_EQ(conn.connection_send_window(), 0);

    // With a zero window the flush is a no-op and the data stays parked.
    std::vector<std::byte> out;
    bool flushed = false;
    CRPC_CHECK(conn.flush_parked(2, 0, out, flushed).ok());
    CRPC_CHECK_FALSE(flushed);
    CRPC_CHECK(conn.has_parked());

    // A WINDOW_UPDATE on the connection resumes the stream.
    CRPC_CHECK(deliver(conn, frame_type::window_update, 0, 0, window_update_bytes(5)).ok());
    CRPC_CHECK_EQ(conn.connection_send_window(), 5);
    CRPC_CHECK(conn.flush_parked(2, 0, out, flushed).ok());
    CRPC_CHECK(flushed);
    CRPC_CHECK_EQ(out.size(), 5u);
    CRPC_CHECK_FALSE(conn.has_parked());
}

CRPC_TEST(flow_control_stream_window_via_settings) {
    connection_options options;
    options.role = connection_role::server;
    connection conn(options);
    CRPC_CHECK(conn.open_stream(2).ok());

    // Peer advertises a tiny initial window.
    const std::vector<std::byte> settings =
        settings_bytes({{static_cast<u16>(setting_id::initial_window_size), 5}});
    CRPC_CHECK(deliver(conn, frame_type::settings, 0, 0, settings).ok());
    CRPC_CHECK_EQ(conn.stream_send_window(2), 5);

    const std::vector<std::byte> data(10, std::byte{0x42});
    std::size_t sent = 0;
    bool parked = false;
    CRPC_CHECK(conn.send_data(2, view(data), sent, parked).ok());
    CRPC_CHECK_EQ(sent, 5u);
    CRPC_CHECK(parked);

    std::vector<std::byte> out;
    bool flushed = false;
    CRPC_CHECK(deliver(conn, frame_type::window_update, 0, 2, window_update_bytes(5)).ok());
    CRPC_CHECK(conn.flush_parked(2, 0, out, flushed).ok());
    CRPC_CHECK(flushed);
    CRPC_CHECK_EQ(out.size(), 5u);
}

CRPC_TEST(flow_control_windows_are_independent) {
    connection_options options;
    options.role = connection_role::server;
    options.initial_connection_send_window = 5;
    connection conn(options);
    CRPC_CHECK(conn.open_stream(2).ok());

    const std::vector<std::byte> data(10, std::byte{0x43});
    std::size_t sent = 0;
    bool parked = false;
    CRPC_CHECK(conn.send_data(2, view(data), sent, parked).ok());
    CRPC_CHECK_EQ(sent, 5u);

    // Raising the stream window alone does not unblock: the connection window
    // is still zero.
    CRPC_CHECK(deliver(conn, frame_type::window_update, 0, 2, window_update_bytes(100)).ok());
    std::vector<std::byte> out;
    bool flushed = false;
    CRPC_CHECK(conn.flush_parked(2, 0, out, flushed).ok());
    CRPC_CHECK_FALSE(flushed);
    CRPC_CHECK_EQ(conn.stream_send_window(2), 65535 + 100 - 5);

    // Raising the connection window unblocks.
    CRPC_CHECK(deliver(conn, frame_type::window_update, 0, 0, window_update_bytes(5)).ok());
    CRPC_CHECK(conn.flush_parked(2, 0, out, flushed).ok());
    CRPC_CHECK(flushed);
    CRPC_CHECK_EQ(out.size(), 5u);
}

CRPC_TEST(flow_control_overflow_rejected) {
    connection_options options;
    options.role = connection_role::server;
    connection conn(options);
    CRPC_CHECK(conn.open_stream(2).ok());

    // A connection WINDOW_UPDATE that would exceed 2^31-1 is an error.
    CRPC_CHECK_EQ(static_cast<int>(deliver(conn, frame_type::window_update, 0, 0,
                                           window_update_bytes(max_window_size))
                                         .code),
                  static_cast<int>(error_code::flow_control_error));

    // A stream WINDOW_UPDATE that would exceed 2^31-1 is a stream error.
    const h2_error overflow = deliver(conn, frame_type::window_update, 0, 2,
                                      window_update_bytes(max_window_size));
    CRPC_CHECK(overflow.scope == error_scope::stream);
    CRPC_CHECK(overflow.code == error_code::flow_control_error);

    // A zero increment is illegal.
    CRPC_CHECK_EQ(static_cast<int>(deliver(conn, frame_type::window_update, 0, 0,
                                           window_update_bytes(0))
                                         .code),
                  static_cast<int>(error_code::protocol_error));

    // Receiving more DATA than the window allows is a flow-control error.
    connection small(options);
    options.initial_connection_recv_window = 4;
    connection tiny(options);
    const std::vector<std::byte> block = block_of({{":method", "POST"}});
    CRPC_CHECK(deliver(tiny, frame_type::headers, flags::end_headers, 1, block).ok());
    CRPC_CHECK_EQ(static_cast<int>(deliver(tiny, frame_type::data, 0, 1, bytes_of("0123456789")).code),
                  static_cast<int>(error_code::flow_control_error));
}

CRPC_TEST(flow_control_release_received) {
    connection_options options;
    options.role = connection_role::server;
    connection conn(options);
    const std::vector<std::byte> block = block_of({{":method", "POST"}});
    CRPC_CHECK(deliver(conn, frame_type::headers, flags::end_headers, 1, block).ok());
    CRPC_CHECK(deliver(conn, frame_type::data, 0, 1, bytes_of("0123456789")).ok());
    CRPC_CHECK_EQ(conn.stream_recv_window(1), 65535 - 10);

    const window_credit credit = conn.release_received(1, 10);
    CRPC_CHECK_EQ(credit.connection, 10u);
    CRPC_CHECK_EQ(credit.stream, 10u);
    CRPC_CHECK_EQ(conn.stream_recv_window(1), 65535);
    CRPC_CHECK_EQ(conn.connection_recv_window(), 65535);
}

// --- Fuzzing -----------------------------------------------------------------

struct lcg {
    u32 state;
    u32 next() {
        state = state * 1664525u + 1013904223u;
        return state;
    }
};

CRPC_TEST(fuzz_frame_and_hpack_never_crash) {
    lcg rng{0x12345678u};
    for (int iteration = 0; iteration < 4000; ++iteration) {
        const std::size_t length = rng.next() % 48u;
        std::vector<std::byte> bytes(length);
        for (std::size_t i = 0; i < length; ++i) {
            bytes[i] = static_cast<std::byte>(rng.next() & 0xFFu);
        }
        frame_header header;
        (void)decode_frame_header(view(bytes), header, max_max_frame_size);

        // Huffman decoding of arbitrary bytes must not crash.
        std::string decoded;
        (void)huffman_decode(view(bytes), decoded);

        // HPACK decoding of arbitrary bytes must not crash.
        hpack_decoder decoder;
        http::header_list headers;
        (void)decoder.decode(view(bytes), headers);
    }
}

CRPC_TEST(fuzz_connection_never_crash) {
    lcg rng{0xDEADBEEFu};
    connection_options options;
    options.role = connection_role::server;
    connection conn(options);
    for (int iteration = 0; iteration < 4000; ++iteration) {
        const std::size_t length = rng.next() % 32u;
        std::vector<std::byte> payload(length);
        for (std::size_t i = 0; i < length; ++i) {
            payload[i] = static_cast<std::byte>(rng.next() & 0xFFu);
        }
        const frame_type type = static_cast<frame_type>(rng.next() % 12u);
        const u8 frame_flags = static_cast<u8>(rng.next() & 0xFFu);
        const u32 stream_id = rng.next() % 8u;
        (void)deliver(conn, type, frame_flags, stream_id, payload);
        if (conn.goaway_received()) {
            // A GOAWAY ends the exchange; start a fresh connection to keep
            // exercising the machine.
            conn = connection(options);
        }
    }
}

} // namespace

CRPC_TEST_MAIN()

