// CRPC -- tests for the RFC 6455 wire codec (crpc/ws/frame.hpp).
//
// The expected values are the RFC's own worked examples (section 5.7): the
// single-frame unmasked "Hello", the masked "Hello", the 256-byte and 64 KiB
// binary frames. Everything else is the error surface the decoder must refuse
// (reserved bits, reserved length encodings, fragmented control frames,
// control payloads over 125 bytes, mask-direction violations, and a declared
// length that must not be trusted enough to allocate).
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/ws/frame.hpp>

#include "test_framework.hpp"
#include <ostream>
#include <type_traits>

// check_equal() streams both sides of a failed comparison; this library's
// enum-classes have no operator<<, so the integer form is provided for ADL.
namespace crpc {
template <typename E,
          typename std::enable_if<std::is_enum<E>::value, int>::type = 0>
std::ostream& operator<<(std::ostream& os, E value) {
    return os << static_cast<long long>(value);
}
} // namespace crpc

using namespace crpc;
using namespace crpc::ws;

namespace {

[[nodiscard]] std::string from_hex(std::string_view text) {
    std::string out;
    auto nibble = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') {
            return static_cast<unsigned>(c - '0');
        }
        if (c >= 'a' && c <= 'f') {
            return static_cast<unsigned>(c - 'a' + 10);
        }
        return static_cast<unsigned>(c - 'A' + 10);
    };
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        out.push_back(static_cast<char>((nibble(text[i]) << 4) | nibble(text[i + 1])));
    }
    return out;
}

[[nodiscard]] std::string to_hex(std::string_view bytes) {
    std::string out;
    static const char* digits = "0123456789abcdef";
    for (const unsigned char c : bytes) {
        out.push_back(digits[(c >> 4) & 0x0f]);
        out.push_back(digits[c & 0x0f]);
    }
    return out;
}

[[nodiscard]] span<const std::byte> as_bytes(std::string_view text) noexcept {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                 text.size());
}

[[nodiscard]] std::string decode_payload(std::string_view wire, bool expect_masked,
                                         decoded_frame& out) {
    frame_decoder decoder(
        frame_decoder::options{expect_masked, 4u * 1024u * 1024u});
    std::string leftover(wire);
    // Feed one byte at a time: incremental parsing must not depend on frame
    // boundaries aligning with feed boundaries.
    std::size_t offset = 0;
    frame_status status = frame_status::need_more;
    while (offset < leftover.size()) {
        status = decoder.feed(
            span<const std::byte>(reinterpret_cast<const std::byte*>(leftover.data()) + offset,
                                  1),
            out);
        ++offset;
        if (status != frame_status::need_more) {
            break;
        }
    }
    if (status == frame_status::need_more) {
        status = decoder.feed(span<const std::byte>(), out);
    }
    if (status != frame_status::frame) {
        return std::string();
    }
    return to_hex(out.payload);
}

CRPC_TEST(ws_frame_encode_unmasked_text_hello) {
    std::string out;
    frame_header header;
    header.fin = true;
    header.op = opcode::text;
    const status ok = encode_frame(header, as_bytes("Hello"), out);
    CRPC_CHECK(ok.ok());
    CRPC_CHECK_EQ("810548656c6c6f", to_hex(out));
}

CRPC_TEST(ws_frame_encode_prefix_length_encodings) {
    {
        std::string out;
        frame_header header;
        header.fin = true;
        header.op = opcode::binary;
        std::string payload(256, 'x');
        CRPC_CHECK(encode_frame(header, as_bytes(payload), out).ok());
        CRPC_CHECK_EQ("827e0100", to_hex(std::string_view(out).substr(0, 4)));
        CRPC_CHECK_EQ(out.size(), 2u + 2u + 256u);
    }
    {
        std::string out;
        frame_header header;
        header.fin = true;
        header.op = opcode::binary;
        std::string payload(65536, 'x');
        CRPC_CHECK(encode_frame(header, as_bytes(payload), out).ok());
        CRPC_CHECK_EQ("827f0000000000010000", to_hex(std::string_view(out).substr(0, 10)));
        CRPC_CHECK_EQ(out.size(), 2u + 8u + 65536u);
    }
}

CRPC_TEST(ws_frame_decode_masked_hello_vector) {
    // RFC 6455 section 5.7: the masked "Hello" example.
    const std::string wire = from_hex("818537fa213d7f9f4d5158");
    decoded_frame out;
    CRPC_CHECK_EQ("48656c6c6f", decode_payload(wire, true, out));
    CRPC_CHECK(out.fin);
    CRPC_CHECK_EQ(opcode::text, out.op);
}

CRPC_TEST(ws_frame_decode_rfc_binary_vectors) {
    {
        const std::string wire =
            from_hex("827e0100") + std::string(256, 'z');
        decoded_frame out;
        (void)decode_payload(wire, false, out);
        CRPC_CHECK_EQ("827e0100", to_hex(std::string_view(wire).substr(0, 4)));
        CRPC_CHECK_EQ("7a", to_hex(std::string_view(out.payload).substr(0, 1)));
        CRPC_CHECK_EQ(256u, out.payload.size());
    }
    {
        std::string wire = from_hex("827f0000000000010000");
        wire.append(65536, 'y');
        decoded_frame out;
        (void)decode_payload(wire, false, out);
        CRPC_CHECK_EQ("827f0000000000010000", to_hex(std::string_view(wire).substr(0, 10)));
        CRPC_CHECK_EQ(65536u, out.payload.size());
    }
}

CRPC_TEST(ws_frame_roundtrip_masked) {
    // The mask is random per frame, so the bytes cannot be pinned; the round
    // trip must reproduce the payload with the mask bit set.
    std::string out;
    frame_header header;
    header.fin = true;
    header.op = opcode::binary;
    header.masked = true;
    const std::string payload("the masked payload");
    CRPC_CHECK(encode_frame(header, as_bytes(payload), out).ok());
    CRPC_CHECK(!out.empty());
    CRPC_CHECK((static_cast<unsigned char>(out[1]) & 0x80u) != 0u);
    decoded_frame decoded;
    (void)decode_payload(out, true, decoded);
    CRPC_CHECK_EQ(payload.size(), decoded.payload.size());
    CRPC_CHECK_EQ(payload, decoded.payload);
}

CRPC_TEST(ws_frame_decode_split_across_feeds) {
    // A frame delivered in uneven chunks, including a single byte at a time
    // through the header, must decode identically to one contiguous feed.
    frame_decoder decoder(frame_decoder::options{false, 4096});
    const std::string wire = from_hex("8104") + "okay";
    decoded_frame out;
    frame_status status = frame_status::need_more;
    std::size_t pos = 0;
    while (pos < wire.size() && status == frame_status::need_more) {
        const std::size_t step = (pos % 3) + 1;
        status = decoder.feed(
            span<const std::byte>(
                reinterpret_cast<const std::byte*>(wire.data()) + pos, step),
            out);
        pos += step;
    }
    CRPC_CHECK_EQ(frame_status::frame, status);
    CRPC_CHECK_EQ("okay", out.payload);
}

CRPC_TEST(ws_frame_need_more_on_partial) {
    frame_decoder decoder;
    decoded_frame out;
    CRPC_CHECK_EQ(frame_status::need_more, decoder.feed(span<const std::byte>(), out));
    const std::string head = from_hex("8105");
    CRPC_CHECK_EQ(frame_status::need_more,
                  decoder.feed(as_bytes(head), out));
    CRPC_CHECK_EQ(2u, decoder.buffered());
}

CRPC_TEST(ws_frame_rejects_rsv_bits) {
    // 0xC1 is FIN with RSV1 set plus opcode 1; the RSV check fires once both
    // header bytes are present.
    decoded_frame out;
    frame_decoder decoder;
    const std::string wire = from_hex("c101");
    CRPC_CHECK_EQ(frame_status::error, decoder.feed(as_bytes(wire), out));
    CRPC_CHECK_EQ(rpc_code::protocol_error, decoder.error().code);
}

CRPC_TEST(ws_frame_rejects_unknown_opcode) {
    const std::string wire = from_hex("830148"); // opcode 0x3
    decoded_frame out;
    frame_decoder decoder;
    CRPC_CHECK_EQ(frame_status::error, decoder.feed(as_bytes(wire), out));
    CRPC_CHECK_EQ(rpc_code::protocol_error, decoder.error().code);
}

CRPC_TEST(ws_frame_rejects_fragmented_control) {
    // A ping with FIN=0 (0x09, masked bit 0).
    const std::string wire = from_hex("090148");
    decoded_frame out;
    frame_decoder decoder;
    CRPC_CHECK_EQ(frame_status::error, decoder.feed(as_bytes(wire), out));
    CRPC_CHECK_EQ(rpc_code::protocol_error, decoder.error().code);
}

CRPC_TEST(ws_frame_rejects_control_payload_over_125) {
    // A pong claiming 126 bytes of payload.
    const std::string wire = from_hex("8a7e" "007e");
    decoded_frame out;
    frame_decoder decoder;
    CRPC_CHECK_EQ(frame_status::error, decoder.feed(as_bytes(wire), out));
    CRPC_CHECK_EQ(rpc_code::protocol_error, decoder.error().code);
}

CRPC_TEST(ws_frame_rejects_redundant_length_encoding) {
    {
        // 5 bytes encoded with the 16-bit length form.
        const std::string wire = from_hex("817e0005")
            + std::string("hello");
        decoded_frame out;
        frame_decoder decoder;
        CRPC_CHECK_EQ(frame_status::error, decoder.feed(as_bytes(wire), out));
        CRPC_CHECK_EQ(rpc_code::protocol_error, decoder.error().code);
    }
    {
        // 5 bytes encoded with the 64-bit length form.
        const std::string wire = from_hex("817f0000000000000005")
            + std::string("hello");
        decoded_frame out;
        frame_decoder decoder;
        CRPC_CHECK_EQ(frame_status::error, decoder.feed(as_bytes(wire), out));
        CRPC_CHECK_EQ(rpc_code::protocol_error, decoder.error().code);
    }
}

CRPC_TEST(ws_frame_rejects_64bit_msb_length) {
    // b1 & 0x7f == 127, then a length whose top bit is set.
    const std::string wire = from_hex("817f8000000000000000");
    decoded_frame out;
    frame_decoder decoder;
    CRPC_CHECK_EQ(frame_status::error, decoder.feed(as_bytes(wire), out));
    CRPC_CHECK_EQ(rpc_code::protocol_error, decoder.error().code);
}

CRPC_TEST(ws_frame_mask_direction_enforced) {
    // A client-side decoder must refuse masked frames; a server-side decoder
    // must refuse unmasked ones.
    const std::string masked = from_hex("818537fa213d7f9f4d5158");
    const std::string unmasked = from_hex("810548656c6c6f");
    {
        decoded_frame out;
        frame_decoder client_decoder(frame_decoder::options{false, 4096});
        CRPC_CHECK_EQ(frame_status::error,
                      client_decoder.feed(as_bytes(masked), out));
        CRPC_CHECK_EQ(rpc_code::protocol_error, client_decoder.error().code);
    }
    {
        decoded_frame out;
        frame_decoder server_decoder(frame_decoder::options{true, 4096});
        CRPC_CHECK_EQ(frame_status::error,
                      server_decoder.feed(as_bytes(unmasked), out));
        CRPC_CHECK_EQ(rpc_code::protocol_error, server_decoder.error().code);
    }
}

CRPC_TEST(ws_frame_rejects_huge_declared_length) {
    // A 16-bit length that claims 60 KiB against a 4 KiB ceiling must be
    // refused before any payload is buffered.
    decoded_frame out;
    frame_decoder decoder(frame_decoder::options{false, 4096});
    const std::string wire = from_hex("827e" "f000") + std::string(8, 'q');
    CRPC_CHECK_EQ(frame_status::error, decoder.feed(as_bytes(wire), out));
    CRPC_CHECK_EQ(rpc_code::resource_exhausted, decoder.error().code);
    CRPC_CHECK(decoder.buffered() < 24u);
}

CRPC_TEST(ws_frame_encode_rejects_bad_headers) {
    std::string out;
    {
        frame_header header;
        header.op = opcode::text;
        header.rsv = 1;
        CRPC_CHECK_FALSE(encode_frame(header, as_bytes("x"), out).ok());
    }
    {
        frame_header header;
        header.op = static_cast<opcode>(0x7);
        CRPC_CHECK_FALSE(encode_frame(header, as_bytes("x"), out).ok());
    }
    {
        frame_header header;
        header.op = opcode::ping;
        std::string payload(126, 'x');
        CRPC_CHECK_FALSE(encode_frame(header, as_bytes(payload), out).ok());
    }
}

CRPC_TEST(ws_frame_is_control_and_data) {
    CRPC_CHECK_FALSE(is_control(opcode::text));
    CRPC_CHECK_FALSE(is_control(opcode::binary));
    CRPC_CHECK_FALSE(is_control(opcode::continuation));
    CRPC_CHECK(is_control(opcode::close));
    CRPC_CHECK(is_control(opcode::ping));
    CRPC_CHECK(is_control(opcode::pong));
    CRPC_CHECK(is_data(opcode::text));
    CRPC_CHECK(is_data(opcode::binary));
    CRPC_CHECK_FALSE(is_data(opcode::close));
}

CRPC_TEST(ws_frame_close_code_validity) {
    CRPC_CHECK(is_valid_close_code(1000));
    CRPC_CHECK(is_valid_close_code(1001));
    CRPC_CHECK(is_valid_close_code(1002));
    CRPC_CHECK(is_valid_close_code(1003));
    CRPC_CHECK(is_valid_close_code(1007));
    CRPC_CHECK(is_valid_close_code(1008));
    CRPC_CHECK(is_valid_close_code(1009));
    CRPC_CHECK(is_valid_close_code(1010));
    CRPC_CHECK(is_valid_close_code(1011));
    CRPC_CHECK(is_valid_close_code(1012)); // IANA-registered
    CRPC_CHECK(is_valid_close_code(3000));
    CRPC_CHECK(is_valid_close_code(4999));
    CRPC_CHECK_FALSE(is_valid_close_code(1004)); // reserved
    CRPC_CHECK_FALSE(is_valid_close_code(1005)); // reserved
    CRPC_CHECK_FALSE(is_valid_close_code(1006)); // reserved
    CRPC_CHECK_FALSE(is_valid_close_code(1015)); // reserved
    CRPC_CHECK_FALSE(is_valid_close_code(1016)); // protocol-reserved range
    CRPC_CHECK_FALSE(is_valid_close_code(2999));
    CRPC_CHECK_FALSE(is_valid_close_code(5000));
    CRPC_CHECK_FALSE(is_valid_close_code(0));
}

CRPC_TEST(ws_frame_utf8_validation) {
    CRPC_CHECK(is_valid_utf8("Hello"));
    CRPC_CHECK(is_valid_utf8(""));
    CRPC_CHECK(is_valid_utf8("caf\xc3\xa9"));
    CRPC_CHECK(is_valid_utf8("\xe2\x82\xac"));           // U+20AC
    CRPC_CHECK(is_valid_utf8("\xf0\x9f\x98\x80"));       // U+1F600
    CRPC_CHECK(is_valid_utf8("\xc3\xa9taille"));

    CRPC_CHECK_FALSE(is_valid_utf8("\x80"));             // lone continuation
    CRPC_CHECK_FALSE(is_valid_utf8("\xc0\x80"));         // overlong NUL
    CRPC_CHECK_FALSE(is_valid_utf8("\xe0\x80\x80"));     // overlong
    CRPC_CHECK_FALSE(is_valid_utf8("\xc3"));             // truncated
    CRPC_CHECK_FALSE(is_valid_utf8("\xe2\x82"));         // truncated
    CRPC_CHECK_FALSE(is_valid_utf8("\xed\xa0\x80"));     // UTF-16 surrogate
    CRPC_CHECK_FALSE(is_valid_utf8("\xf4\x90\x80\x80")); // > U+10FFFF
    CRPC_CHECK_FALSE(is_valid_utf8("ok\xff"));
}

} // namespace

CRPC_TEST_MAIN()