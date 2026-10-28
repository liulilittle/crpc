// CRPC -- Oracle TNS packet framing tests.
//
// The golden vectors here are bytes that Oracle's python-oracledb (thin mode)
// actually emitted; they were not derived from a prose description of the
// format. docs/oracle-ttc-evidence.md records how each was captured.
//
// There is no Oracle server in this environment, so nothing here proves that a
// database accepts these bytes. What it does prove is that CRPC's encoder
// reproduces the reference client's output byte for byte and that the decoder
// reads that output back.
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/db/oracle/tns.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace {

using crpc::db::oracle::tns_accept;
using crpc::db::oracle::tns_assemble;
using crpc::db::oracle::tns_assembler;
using crpc::db::oracle::tns_connect;
using crpc::db::oracle::tns_header;
using crpc::db::oracle::tns_marker;
using crpc::db::oracle::tns_packet;
using crpc::db::oracle::tns_packet_type;
using crpc::db::oracle::tns_refuse;

[[nodiscard]] crpc::span<const std::byte> view(const std::vector<std::byte>& data) noexcept {
    return crpc::span<const std::byte>(data.data(), data.size());
}

[[nodiscard]] std::string hex(const std::vector<std::byte>& data) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (std::byte b : data) {
        const auto value = static_cast<std::uint8_t>(b);
        out.push_back(digits[value >> 4]);
        out.push_back(digits[value & 0x0f]);
    }
    return out;
}

[[nodiscard]] std::vector<std::byte> from_hex(std::string_view text) {
    auto value_of = [](char c) -> unsigned {
        if (c >= '0' && c <= '9') {
            return static_cast<unsigned>(c - '0');
        }
        if (c >= 'a' && c <= 'f') {
            return static_cast<unsigned>(c - 'a') + 10u;
        }
        return 0u;
    };
    std::vector<std::byte> out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        out.push_back(static_cast<std::byte>((value_of(text[i]) << 4) | value_of(text[i + 1])));
    }
    return out;
}

[[nodiscard]] std::string text_of(crpc::span<const std::byte> data) {
    std::string out;
    out.reserve(data.size());
    for (std::byte b : data) {
        out.push_back(static_cast<char>(static_cast<unsigned char>(b)));
    }
    return out;
}

// Captured from the installed reference client; see section 7 of the evidence
// document. The descriptor is 199 bytes, the whole packet 273.
constexpr std::string_view kConnectHex =
    "0111000001000000013f012c0401200020004f980000000100c7004a000000"
    "00848400000000000000000000000000000000000000000000000000002000"
    "000020000000000000000001284445534352495054494f4e3d284144445245"
    "53533d2850524f544f434f4c3d7463702928484f53543d3132372e302e302e"
    "312928504f52543d3435343239292928434f4e4e4543545f444154413d2853"
    "4552564943455f4e414d453d58455044423129284349443d2850524f475241"
    "4d3d2f7573722f62696e2f707974686f6e332928484f53543d4c49554c494c"
    "54452928555345523d6a6179292928434f4e4e454354494f4e5f49443d534a"
    "71617a6d683050586254642f2f7850646f2b67773d3d292929";



constexpr std::string_view kConnectDescriptor =
    "(DESCRIPTION=(ADDRESS=(PROTOCOL=tcp)(HOST=127.0.0.1)(PORT=45429))"
    "(CONNECT_DATA=(SERVICE_NAME=XEPDB1)"
    "(CID=(PROGRAM=/usr/bin/python3)(HOST=LIULILTE)(USER=jay))"
    "(CONNECTION_ID=SJqazmh0PXbTd//xPdo+gw==)))";

// A 300-character service name pushed the reference past TNS_MAX_CONNECT_DATA
// (230): the CONNECT packet stops after the 66-byte fixed part and the
// descriptor travels in a DATA packet (impl/thin/messages/connect.pyx:156-159).
constexpr std::string_view kConnectLongHeadHex =
    "004a000001000000013f012c0401200020004f980000000101ed004a000000"
    "00848400000000000000000000000000000000000000000000000000002000"
    "000020000000000000000001";



// Captured after the ACCEPT below was accepted by the reference client.
constexpr std::string_view kMarkerResetHex = "0000000b0c000000010002";
constexpr std::string_view kDataProtocolHex = "0000001d060000000000010600707974686f6e2d6f7261636c65646200";

// The ACCEPT fed to the reference client. It continued with the two packets
// above, which is what makes these offsets trustworthy: a wrong offset would
// have produced an exception or silence.
constexpr std::string_view kAcceptBodyHex =
    "013f0400000000000000000000000000000000000000000000002000000000"
    "000002000001";



// A REFUSE emitted by a fake listener, which the reference client decoded into
// "Service X is not registered with the listener".
constexpr std::string_view kRefuseHex =
    "003100000400000000000025544e532d31323531343a2043616e6e6f742063"
    "6f6e6e6563740a284552523d313235313429";



// --- 1. Header ---------------------------------------------------------------

CRPC_TEST(tns_header_large_layout_matches_the_reference) {
    // The marker packet is the shortest real packet: 8-byte header, 3-byte
    // body, 32-bit length because the negotiated protocol version is 319.
    std::vector<std::byte> encoded;
    CRPC_CHECK(crpc::db::oracle::encode_marker(encoded, tns_marker::reset, true).ok());
    CRPC_CHECK_EQ(kMarkerResetHex, std::string_view(hex(encoded)));

    tns_header header;
    CRPC_CHECK(crpc::db::oracle::decode_tns_header(view(encoded), true, header).ok());
    CRPC_CHECK_EQ(11u, header.length);
    CRPC_CHECK_EQ(static_cast<int>(tns_packet_type::marker), static_cast<int>(header.type));
    CRPC_CHECK_EQ(0, static_cast<int>(header.flags));

    std::vector<std::byte> head;
    tns_header value;
    value.type = tns_packet_type::data;
    value.length = 10;
    value.data_flags = crpc::db::oracle::tns_data_flag_end_of_response;
    CRPC_CHECK(crpc::db::oracle::encode_tns_header(head, value, true).ok());
    CRPC_CHECK_EQ(std::size_t{10}, head.size());
    CRPC_CHECK_EQ(std::string_view("0000000a060000002000"), hex(head));

    CRPC_CHECK_EQ(std::size_t{8}, crpc::db::oracle::tns_header_size);
    CRPC_CHECK_EQ(std::size_t{10}, crpc::db::oracle::tns_data_header_size);
}

CRPC_TEST(tns_header_small_layout_has_a_checksum_word) {
    std::vector<std::byte> encoded;
    CRPC_CHECK(crpc::db::oracle::encode_marker(encoded, tns_marker::break_request, false).ok());
    // be16 length, be16 checksum (=0), type 12, flags 0, reserved 0, then 01 00 01.
    CRPC_CHECK_EQ(std::string_view("000b00000c000000010001"), hex(encoded));

    tns_header header;
    CRPC_CHECK(crpc::db::oracle::decode_tns_header(view(encoded), false, header).ok());
    CRPC_CHECK_EQ(11u, header.length);
    CRPC_CHECK_EQ(static_cast<int>(tns_packet_type::marker), static_cast<int>(header.type));

    // Both layouts keep type and flags at offsets 4 and 5; what changes is the
    // length. Read as a large layout the same bytes claim 0x000b0000 = 720896
    // bytes, which is why picking the wrong layout cannot be detected from the
    // type byte alone.
    tns_header wrong;
    CRPC_CHECK(crpc::db::oracle::decode_tns_header(view(encoded), true, wrong).ok());
    CRPC_CHECK_EQ(720896u, wrong.length);
    CRPC_CHECK_EQ(static_cast<int>(tns_packet_type::marker), static_cast<int>(wrong.type));
    CRPC_CHECK_EQ(11u, header.length);
}

CRPC_TEST(tns_header_rejects_a_nonzero_checksum) {
    const std::vector<std::byte> encoded = from_hex("000b12340c000000010001");
    tns_header header;
    const crpc::status result =
        crpc::db::oracle::decode_tns_header(view(encoded), false, header);
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(result.code()));
}

CRPC_TEST(tns_header_rejects_an_illegal_length) {
    tns_header header;
    const std::vector<std::byte> too_short = from_hex("00000007060000000000");
    CRPC_CHECK_FALSE(crpc::db::oracle::decode_tns_header(view(too_short), true, header).ok());

    const std::vector<std::byte> too_large = from_hex("00200000060000000000");
    CRPC_CHECK_FALSE(crpc::db::oracle::decode_tns_header(view(too_large), true, header).ok());

    // Encoding refuses the same cases instead of emitting a frame no peer can
    // parse.
    std::vector<std::byte> out;
    tns_header value;
    value.type = tns_packet_type::data;
    value.length = 7;
    CRPC_CHECK_FALSE(crpc::db::oracle::encode_tns_header(out, value, true).ok());
    CRPC_CHECK(out.empty());

    value.length = 0x100001;
    CRPC_CHECK_FALSE(crpc::db::oracle::encode_tns_header(out, value, true).ok());
    CRPC_CHECK(out.empty());

    // A DATA header shorter than 10 cannot hold its own data flags.
    value.length = 9;
    CRPC_CHECK_FALSE(crpc::db::oracle::encode_tns_header(out, value, true).ok());
    CRPC_CHECK(out.empty());

    // A small-layout length that does not fit 16 bits is refused, not truncated.
    value.length = 0x10000;
    CRPC_CHECK_FALSE(crpc::db::oracle::encode_tns_header(out, value, false).ok());
    CRPC_CHECK(out.empty());
}

CRPC_TEST(tns_header_rejects_an_unknown_packet_type) {
    const std::vector<std::byte> encoded = from_hex("0000000b090000000000000000");
    tns_header header;
    const crpc::status result =
        crpc::db::oracle::decode_tns_header(view(encoded), true, header);
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(result.code()));
}

// --- 2. Connect --------------------------------------------------------------

CRPC_TEST(tns_connect_reproduces_the_reference_bytes) {
    tns_connect request;
    request.connect_data = std::string(kConnectDescriptor);
    CRPC_CHECK_EQ(std::size_t{199}, request.connect_data.size());

    std::vector<std::byte> encoded;
    CRPC_CHECK(crpc::db::oracle::encode_connect(encoded, request).ok());
    CRPC_CHECK_EQ(std::size_t{273}, encoded.size());
    CRPC_CHECK_EQ(kConnectHex, std::string_view(hex(encoded)));

    // Fields that are easy to get wrong, read from the packet. The header is 8
    // bytes, so body offset N is packet byte 8 + N.
    auto be16 = [&encoded](std::size_t at) {
        return static_cast<int>((static_cast<unsigned>(encoded[at]) << 8) |
                                static_cast<unsigned>(encoded[at + 1]));
    };
    auto be32 = [&encoded](std::size_t at) {
        return (static_cast<unsigned>(encoded[at]) << 24) |
               (static_cast<unsigned>(encoded[at + 1]) << 16) |
               (static_cast<unsigned>(encoded[at + 2]) << 8) |
               static_cast<unsigned>(encoded[at + 3]);
    };
    CRPC_CHECK_EQ(0x013f, be16(8 + 0));  // version desired = 319
    CRPC_CHECK_EQ(0x012c, be16(8 + 2));  // version minimum = 300
    CRPC_CHECK_EQ(0x0401, be16(8 + 4));  // service options
    CRPC_CHECK_EQ(0x2000, be16(8 + 6));  // SDU, 16-bit
    CRPC_CHECK_EQ(0x2000, be16(8 + 8));  // TDU, 16-bit (the reference repeats SDU)
    CRPC_CHECK_EQ(0x4f98, be16(8 + 10)); // characteristics
    CRPC_CHECK_EQ(199, be16(8 + 16));    // connect data length
    CRPC_CHECK_EQ(74, be16(8 + 18));     // connect data offset, from packet start
    CRPC_CHECK_EQ(8192u, be32(8 + 50));  // SDU, 32-bit
    CRPC_CHECK_EQ(8192u, be32(8 + 54));  // TDU, 32-bit
    CRPC_CHECK_EQ(0u, be32(8 + 58));     // connect flags 1
    CRPC_CHECK_EQ(1u, be32(8 + 62));     // connect flags 2 = CHECK_OOB

    // The offset field does not describe the layout: the body before the
    // descriptor is 66 bytes while the field says 74. Both numbers are asserted
    // so a "fix" to 66 fails loudly.
    CRPC_CHECK_EQ(std::size_t{66}, crpc::db::oracle::tns_connect_fixed_body);
    CRPC_CHECK_EQ(74, static_cast<int>(crpc::db::oracle::tns_connect_data_offset));
}

CRPC_TEST(tns_connect_rejects_an_out_of_range_sdu) {
    std::vector<std::byte> out;
    tns_connect request;
    request.sdu = 0;
    CRPC_CHECK_FALSE(crpc::db::oracle::encode_connect(out, request).ok());
    CRPC_CHECK(out.empty());

    request.sdu = 70000;
    CRPC_CHECK_FALSE(crpc::db::oracle::encode_connect(out, request).ok());
    CRPC_CHECK(out.empty());

    request.sdu = 9;
    CRPC_CHECK_FALSE(crpc::db::oracle::encode_connect(out, request).ok());
    CRPC_CHECK(out.empty());

    CRPC_CHECK_EQ(230u, static_cast<unsigned>(crpc::db::oracle::tns_max_connect_data));
}

CRPC_TEST(tns_connect_spills_a_long_descriptor_into_a_data_packet) {
    tns_connect request;
    request.connect_data = std::string(493, 'S');

    std::vector<std::byte> encoded;
    CRPC_CHECK(crpc::db::oracle::encode_connect(encoded, request).ok());
    // A 74-byte CONNECT plus a DATA packet of 10 + 493 bytes.
    CRPC_CHECK_EQ(std::size_t{74 + 503}, encoded.size());
    const std::vector<std::byte> head(encoded.begin(), encoded.begin() + 74);
    CRPC_CHECK_EQ(kConnectLongHeadHex, std::string_view(hex(head)));

    // The CONNECT packet declares only itself (74 = 8-byte header + 66).
    CRPC_CHECK_EQ(74, static_cast<int>((static_cast<unsigned>(encoded[0]) << 8) |
                                       static_cast<unsigned>(encoded[1])));
    // The descriptor length field still carries the whole descriptor (493).
    CRPC_CHECK_EQ(493, static_cast<int>((static_cast<unsigned>(encoded[8 + 16]) << 8) |
                                        static_cast<unsigned>(encoded[8 + 17])));

    // The spill packet is DATA, declares 503 bytes and starts with two zero
    // data-flag bytes, exactly as captured.
    CRPC_CHECK_EQ(503, static_cast<int>((static_cast<unsigned>(encoded[74]) << 8) |
                                        static_cast<unsigned>(encoded[75])));
    // The DATA header starts at 74: 4 length + type + flags + 2 reserved +
    // 2 data flags = 10 bytes, so the descriptor begins at byte 84.
    CRPC_CHECK_EQ(static_cast<int>(tns_packet_type::data), static_cast<int>(encoded[78]));
    CRPC_CHECK_EQ(0, static_cast<int>(encoded[79]));
    CRPC_CHECK_EQ(0, static_cast<int>(encoded[82]));
    CRPC_CHECK_EQ(0, static_cast<int>(encoded[83]));
    CRPC_CHECK_EQ('S', static_cast<int>(encoded[84]));
    CRPC_CHECK_EQ('S', static_cast<int>(encoded[84 + 492]));
}

// --- 3. Accept / Refuse / Redirect -------------------------------------------

CRPC_TEST(tns_accept_decodes_the_captured_reply) {
    std::vector<std::byte> body = from_hex(kAcceptBodyHex);
    CRPC_CHECK_EQ(std::size_t{37}, body.size());
    tns_accept accept;
    CRPC_CHECK(crpc::db::oracle::decode_accept(view(body), accept).ok());
    CRPC_CHECK_EQ(319u, accept.version);
    CRPC_CHECK_EQ(0x0400u, accept.options);
    CRPC_CHECK_EQ(0, static_cast<int>(accept.nsi_flags));
    CRPC_CHECK_EQ(8192u, accept.sdu);
    CRPC_CHECK_EQ(crpc::db::oracle::tns_accept_flag_end_of_response |
                      crpc::db::oracle::tns_accept_flag_check_oob,
                  accept.flags2);
}

CRPC_TEST(tns_accept_rejects_old_versions_and_short_bodies) {
    std::vector<std::byte> body = from_hex(kAcceptBodyHex);
    body[0] = std::byte{0x01};
    body[1] = std::byte{0x2c}; // 300 < 315
    tns_accept accept;
    CRPC_CHECK_FALSE(crpc::db::oracle::decode_accept(view(body), accept).ok());

    std::vector<std::byte> short_body = from_hex(kAcceptBodyHex);
    short_body.resize(20);
    CRPC_CHECK_FALSE(crpc::db::oracle::decode_accept(view(short_body), accept).ok());

    // Version 318+ requires the flags2 word.
    std::vector<std::byte> no_flags = from_hex(kAcceptBodyHex);
    no_flags.resize(30);
    CRPC_CHECK_FALSE(crpc::db::oracle::decode_accept(view(no_flags), accept).ok());

    // An SDU that cannot hold a DATA header is refused.
    std::vector<std::byte> tiny_sdu = from_hex(kAcceptBodyHex);
    tiny_sdu[24] = std::byte{0x00};
    tiny_sdu[25] = std::byte{0x00};
    tiny_sdu[26] = std::byte{0x00};
    tiny_sdu[27] = std::byte{0x04};
    CRPC_CHECK_FALSE(crpc::db::oracle::decode_accept(view(tiny_sdu), accept).ok());
}

CRPC_TEST(tns_refuse_decodes_the_error_code) {
    std::vector<std::byte> packet = from_hex(kRefuseHex);
    CRPC_CHECK_EQ(std::size_t{49}, packet.size());
    // Skip the 8-byte header; the body starts with two pad bytes and a length.
    const crpc::span<const std::byte> body(packet.data() + 8, packet.size() - 8);
    tns_refuse refuse;
    CRPC_CHECK(crpc::db::oracle::decode_refuse(body, refuse).ok());
    CRPC_CHECK_EQ(12514u, refuse.code);
    CRPC_CHECK_EQ(std::string("TNS-12514: Cannot connect\n(ERR=12514)"), refuse.message);

    // Text without a marker decodes but yields no code.
    std::vector<std::byte> plain = from_hex("000000026e6f");
    tns_refuse other;
    CRPC_CHECK(crpc::db::oracle::decode_refuse(view(plain), other).ok());
    CRPC_CHECK_EQ(0u, other.code);

    // A declared length past the packet is refused, not silently clamped.
    const std::vector<std::byte> lying = from_hex("0000001078");
    CRPC_CHECK_FALSE(crpc::db::oracle::decode_refuse(view(lying), other).ok());

    // A body shorter than the two pad bytes plus the length is refused.
    const std::vector<std::byte> tiny = from_hex("000000");
    CRPC_CHECK_FALSE(crpc::db::oracle::decode_refuse(view(tiny), other).ok());
}

CRPC_TEST(tns_redirect_decodes_its_text) {
    const std::string text =
        "(ADDRESS=(HOST=10.0.0.2)(PORT=1521))\0(CONNECT_DATA=(SID=ORCL))";
    std::vector<std::byte> body;
    body.push_back(std::byte{static_cast<std::uint8_t>(text.size() >> 8)});
    body.push_back(std::byte{static_cast<std::uint8_t>(text.size() & 0xff)});
    for (char c : text) {
        body.push_back(std::byte{static_cast<std::uint8_t>(static_cast<unsigned char>(c))});
    }
    std::string decoded;
    CRPC_CHECK(crpc::db::oracle::decode_redirect(view(body), decoded).ok());
    CRPC_CHECK_EQ(text, decoded);
    CRPC_CHECK_EQ(text.size(), decoded.size());

    std::vector<std::byte> truncated = body;
    truncated.resize(4);
    CRPC_CHECK_FALSE(crpc::db::oracle::decode_redirect(view(truncated), decoded).ok());

    const std::vector<std::byte> one = from_hex("00");
    CRPC_CHECK_FALSE(crpc::db::oracle::decode_redirect(view(one), decoded).ok());
}

// --- 4. Splitting and reassembly ---------------------------------------------

CRPC_TEST(tns_message_splits_across_packet_boundaries) {
    // A 30-byte DATA body with an SDU of 20 leaves 10 body bytes per packet:
    // 8 header + 2 data flags. Three packets, each length fully declared.
    std::vector<std::byte> body(30);
    for (std::size_t i = 0; i < body.size(); ++i) {
        body[i] = static_cast<std::byte>(i + 1);
    }
    std::vector<std::byte> packets;
    CRPC_CHECK(crpc::db::oracle::encode_tns_message(
                   packets, tns_packet_type::data, view(body), 20, true,
                   crpc::db::oracle::tns_data_flag_end_of_response)
                   .ok());
    CRPC_CHECK_EQ(std::size_t{60}, packets.size());
    CRPC_CHECK_EQ(0, static_cast<int>(packets[0]));
    CRPC_CHECK_EQ(0, static_cast<int>(packets[1]));
    CRPC_CHECK_EQ(0, static_cast<int>(packets[2]));
    CRPC_CHECK_EQ(20, static_cast<int>(packets[3]));
    // Each packet is header(8) + data flags(2) + 10 body bytes = 20. The data
    // flags are at packet bytes 8..9; only the last packet carries
    // END_OF_RESPONSE (0x2000).
    CRPC_CHECK_EQ(0, static_cast<int>(packets[8]));
    CRPC_CHECK_EQ(0, static_cast<int>(packets[9]));
    CRPC_CHECK_EQ(1, static_cast<int>(packets[10]));
    CRPC_CHECK_EQ(0, static_cast<int>(packets[28]));
    CRPC_CHECK_EQ(0, static_cast<int>(packets[29]));
    CRPC_CHECK_EQ(0x20, static_cast<int>(packets[48]));
    CRPC_CHECK_EQ(0x00, static_cast<int>(packets[49]));
    CRPC_CHECK_EQ(21, static_cast<int>(packets[50]));

    tns_assembler assembler(true);
    CRPC_CHECK(assembler.feed(view(packets)).ok());
    tns_packet message;
    CRPC_CHECK_EQ(static_cast<int>(tns_assemble::ready),
                  static_cast<int>(assembler.next_message(message)));
    CRPC_CHECK(message.body == body);
    CRPC_CHECK_EQ(std::size_t{0}, assembler.buffered());

    // An SDU that cannot hold a DATA header is refused rather than producing
    // zero-length packets.
    std::vector<std::byte> none;
    CRPC_CHECK_FALSE(crpc::db::oracle::encode_tns_message(
                         none, tns_packet_type::data, view(body), 9, true)
                         .ok());
    CRPC_CHECK(none.empty());
    CRPC_CHECK_FALSE(crpc::db::oracle::encode_tns_message(
                         none, tns_packet_type::data, view(body), 70000, true)
                         .ok());
    CRPC_CHECK(none.empty());
}

CRPC_TEST(tns_message_split_copies_packet_flags_to_every_header) {
    // A retry after a redirect marks every packet of the message, not only the
    // first (impl/thin/messages/connect.pyx:140 sets the flag once and the
    // write buffer applies it to each header it emits).
    std::vector<std::byte> body(30, std::byte{0x5a});
    std::vector<std::byte> packets;
    CRPC_CHECK(crpc::db::oracle::encode_tns_message(
                   packets, tns_packet_type::data, view(body), 20, true, 0,
                   crpc::db::oracle::tns_packet_flag_redirect)
                   .ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::oracle::tns_packet_flag_redirect),
                  static_cast<int>(packets[5]));
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::oracle::tns_packet_flag_redirect),
                  static_cast<int>(packets[5 + 20]));
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::oracle::tns_packet_flag_redirect),
                  static_cast<int>(packets[5 + 40]));
}

CRPC_TEST(tns_assembler_accepts_arbitrary_chunking) {
    // The same 30-byte message fed one byte at a time: no message may appear
    // before the last byte, and exactly one must appear then.
    std::vector<std::byte> body(30);
    for (std::size_t i = 0; i < body.size(); ++i) {
        body[i] = static_cast<std::byte>(0xa0 + i);
    }
    std::vector<std::byte> packets;
    CRPC_CHECK(crpc::db::oracle::encode_tns_message(
                   packets, tns_packet_type::data, view(body), 20, true,
                   crpc::db::oracle::tns_data_flag_end_of_response)
                   .ok());

    tns_assembler assembler(true);
    tns_packet message;
    std::size_t produced = 0;
    for (std::size_t i = 0; i < packets.size(); ++i) {
        const std::byte one[1] = {packets[i]};
        CRPC_CHECK(assembler.feed(crpc::span<const std::byte>(one, 1)).ok());
        const tns_assemble state = assembler.next_message(message);
        if (state == tns_assemble::ready) {
            ++produced;
        }
        if (i + 1 < packets.size()) {
            CRPC_CHECK_EQ(static_cast<int>(tns_assemble::need_more),
                          static_cast<int>(state));
        }
    }
    CRPC_CHECK_EQ(std::size_t{1}, produced);
    CRPC_CHECK(message.body == body);
    CRPC_CHECK_EQ(std::size_t{0}, assembler.buffered());
}

CRPC_TEST(tns_assembler_joins_data_packets_until_the_end_flag) {
    // Two DATA packets: the first has no end flag, the second sets
    // END_OF_RESPONSE. The bodies must come out concatenated.
    const std::vector<std::byte> first = from_hex("0000000e060000000000aabbccdd");
    const std::vector<std::byte> second = from_hex("0000000f0600000020001122334455");
    tns_assembler assembler(true);
    CRPC_CHECK(assembler.feed(view(first)).ok());
    tns_packet message;
    // Nothing has ended yet: the first packet has no end flag and no second
    // packet is buffered.
    CRPC_CHECK_EQ(static_cast<int>(tns_assemble::need_more),
                  static_cast<int>(assembler.next_message(message)));
    CRPC_CHECK(assembler.feed(view(second)).ok());
    CRPC_CHECK_EQ(static_cast<int>(tns_assemble::ready),
                  static_cast<int>(assembler.next_message(message)));
    CRPC_CHECK_EQ(std::string_view("aabbccdd1122334455"),
                  std::string_view(hex(message.body)));
    CRPC_CHECK_EQ(0x2000, static_cast<int>(message.data_flags));
    CRPC_CHECK_EQ(std::size_t{0}, assembler.buffered());
}

CRPC_TEST(tns_assembler_honours_the_eof_flag_and_the_short_end_of_response) {
    // EOF (0x0040) also ends a message; the reference treats it as session
    // shutdown rather than as a protocol error.
    const std::vector<std::byte> eof = from_hex("0000000c0600000000400102");
    tns_assembler assembler(true);
    CRPC_CHECK(assembler.feed(view(eof)).ok());
    tns_packet message;
    CRPC_CHECK_EQ(static_cast<int>(tns_assemble::ready),
                  static_cast<int>(assembler.next_message(message)));
    CRPC_CHECK_EQ(0x0040, static_cast<int>(message.data_flags));
    CRPC_CHECK_EQ(std::string_view("0102"), std::string_view(hex(message.body)));

    // A three-byte body that is exactly END_OF_RESPONSE (0x1d) ends the response
    // even with no flag (impl/thin/packet.pyx:71-74).
    const std::vector<std::byte> short_end = from_hex("0000000d0600000000001d0000");
    tns_assembler other(true);
    CRPC_CHECK(other.feed(view(short_end)).ok());
    CRPC_CHECK_EQ(static_cast<int>(tns_assemble::ready),
                  static_cast<int>(other.next_message(message)));
    CRPC_CHECK_EQ(std::size_t{3}, message.body.size());
    CRPC_CHECK_EQ(0x1d, static_cast<int>(message.body[0]));

    tns_header header;
    header.type = tns_packet_type::data;
    header.data_flags = crpc::db::oracle::tns_data_flag_end_of_response;
    CRPC_CHECK(crpc::db::oracle::tns_is_end_of_response(header, view(message.body)));
    header.data_flags = 0;
    // A body that is not the three-byte END_OF_RESPONSE spelling does not end a
    // response on its own.
    const std::vector<std::byte> plain = from_hex("0102");
    CRPC_CHECK_FALSE(crpc::db::oracle::tns_is_end_of_response(header, view(plain)));
    // The short spelling: a three-byte body whose first byte is
    // END_OF_RESPONSE terminates the response even with no flag.
    const std::vector<std::byte> three = from_hex("1d0000");
    CRPC_CHECK(crpc::db::oracle::tns_is_end_of_response(header, view(three)));
    // A two-byte body is not that spelling.
    const std::vector<std::byte> two = from_hex("1d00");
    CRPC_CHECK_FALSE(crpc::db::oracle::tns_is_end_of_response(header, view(two)));
    // Any non-DATA packet ends its own message.
    header.type = tns_packet_type::marker;
    CRPC_CHECK(crpc::db::oracle::tns_is_end_of_response(header, view(two)));
}

CRPC_TEST(tns_assembler_keeps_the_remainder_of_one_feed) {
    // One marker plus one complete DATA message in a single feed: the first call
    // returns the marker and leaves the DATA message buffered.
    std::vector<std::byte> stream;
    CRPC_CHECK(crpc::db::oracle::encode_marker(stream, tns_marker::reset, true).ok());
    const std::vector<std::byte> body = from_hex("0001020304");
    CRPC_CHECK(crpc::db::oracle::encode_tns_message(
                   stream, tns_packet_type::data, view(body), 8192, true,
                   crpc::db::oracle::tns_data_flag_end_of_response)
                   .ok());

    tns_assembler assembler(true);
    CRPC_CHECK(assembler.feed(view(stream)).ok());
    tns_packet message;
    CRPC_CHECK_EQ(static_cast<int>(tns_assemble::ready),
                  static_cast<int>(assembler.next_message(message)));
    CRPC_CHECK_EQ(static_cast<int>(tns_packet_type::marker), static_cast<int>(message.type));
    CRPC_CHECK_EQ(std::size_t{3}, message.body.size());
    CRPC_CHECK_EQ(std::size_t{15}, assembler.buffered());

    CRPC_CHECK_EQ(static_cast<int>(tns_assemble::ready),
                  static_cast<int>(assembler.next_message(message)));
    CRPC_CHECK_EQ(static_cast<int>(tns_packet_type::data), static_cast<int>(message.type));
    CRPC_CHECK_EQ(std::string_view("0001020304"), std::string_view(hex(message.body)));
    CRPC_CHECK_EQ(std::size_t{0}, assembler.buffered());

    CRPC_CHECK_EQ(static_cast<int>(tns_assemble::need_more),
                  static_cast<int>(assembler.next_message(message)));
}

CRPC_TEST(tns_assembler_rejects_a_lying_length_and_a_checksum) {
    // The length field claims 20 bytes but only 11 arrive; the assembler must
    // wait rather than hand out a short message.
    const std::vector<std::byte> partial = from_hex("000000140600000020000102");
    tns_assembler assembler(true);
    CRPC_CHECK(assembler.feed(view(partial)).ok());
    tns_packet message;
    CRPC_CHECK_EQ(static_cast<int>(tns_assemble::need_more),
                  static_cast<int>(assembler.next_message(message)));
    CRPC_CHECK(assembler.error().ok());

    // Completing the packet is accepted and yields exactly the declared body.
    const std::vector<std::byte> rest = from_hex("030405060708090a");
    CRPC_CHECK(assembler.feed(view(rest)).ok());
    CRPC_CHECK_EQ(static_cast<int>(tns_assemble::ready),
                  static_cast<int>(assembler.next_message(message)));
    CRPC_CHECK_EQ(std::size_t{10}, message.body.size());
    CRPC_CHECK_EQ(std::string_view("0102030405060708090a"),
                  std::string_view(hex(message.body)));

    // A small-layout packet with a checksum is refused, and the error sticks
    // until clear().
    const std::vector<std::byte> bad = from_hex("000b00010c000000010002");
    tns_assembler other(false);
    CRPC_CHECK(other.feed(view(bad)).ok());
    CRPC_CHECK_EQ(static_cast<int>(tns_assemble::failed),
                  static_cast<int>(other.next_message(message)));
    CRPC_CHECK_FALSE(other.error().ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(other.error().code()));
    // Feeding more data while the error is set reports it instead of growing the
    // buffer.
    CRPC_CHECK_FALSE(other.feed(view(bad)).ok());
    other.clear();
    CRPC_CHECK(other.error().ok());
    CRPC_CHECK_EQ(std::size_t{0}, other.buffered());
}

CRPC_TEST(tns_assembler_rejects_an_unknown_packet_type) {
    const std::vector<std::byte> bogus = from_hex("0000000b7f0000000000000000");
    tns_assembler assembler(true);
    CRPC_CHECK(assembler.feed(view(bogus)).ok());
    tns_packet message;
    CRPC_CHECK_EQ(static_cast<int>(tns_assemble::failed),
                  static_cast<int>(assembler.next_message(message)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(assembler.error().code()));

    // The layout switch is observable: the same bytes are an 11-byte packet in
    // the large layout and a zero-length packet in the small one.
    tns_assembler small(false);
    CRPC_CHECK_FALSE(small.large_length());
    CRPC_CHECK(small.feed(view(bogus)).ok());
    CRPC_CHECK_EQ(static_cast<int>(tns_assemble::failed),
                  static_cast<int>(small.next_message(message)));
}

CRPC_TEST(tns_assembler_decodes_the_captured_data_packet) {
    const std::vector<std::byte> packet = from_hex(kDataProtocolHex);
    CRPC_CHECK_EQ(std::size_t{29}, packet.size());
    tns_assembler assembler(true);
    CRPC_CHECK(assembler.feed(view(packet)).ok());
    tns_packet message;
    // The captured Protocol message has no end-of-response flag (at that point
    // the capability had not been negotiated), so the assembler asks for more.
    CRPC_CHECK_EQ(static_cast<int>(tns_assemble::need_more),
                  static_cast<int>(assembler.next_message(message)));

    // A following marker lets the message out; the Protocol body is exactly what
    // the reference sent: type 1, version 6, terminator 0, driver name, NUL.
    std::vector<std::byte> marker;
    CRPC_CHECK(crpc::db::oracle::encode_marker(marker, tns_marker::reset, true).ok());
    CRPC_CHECK(assembler.feed(view(marker)).ok());
    CRPC_CHECK_EQ(static_cast<int>(tns_assemble::ready),
                  static_cast<int>(assembler.next_message(message)));
    CRPC_CHECK_EQ(static_cast<int>(tns_packet_type::data), static_cast<int>(message.type));
    CRPC_CHECK_EQ(std::size_t{19}, message.body.size());
    CRPC_CHECK_EQ(1, static_cast<int>(message.body[0]));
    CRPC_CHECK_EQ(6, static_cast<int>(message.body[1]));
    CRPC_CHECK_EQ(0, static_cast<int>(message.body[2]));
    CRPC_CHECK_EQ(std::string("python-oracledb"),
                  text_of(crpc::span<const std::byte>(message.body.data() + 3, 15)));
    CRPC_CHECK_EQ(0, static_cast<int>(message.body[18]));
}

} // namespace

CRPC_TEST_MAIN()