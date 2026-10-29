// CRPC -- TDS login-phase codec tests.
//
// Every case is a byte-level statement about the protocol: the header is
// big-endian where the login body is little-endian, LOGIN7 offsets are measured in
// bytes while string lengths are measured in characters, and a password is
// obfuscated before it is sent. Each of those is silent when wrong.
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/db/tds/protocol.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace {

using crpc::db::tds::done_token;
using crpc::db::tds::encryption_mode;
using crpc::db::tds::login7;
using crpc::db::tds::packet_header;
using crpc::db::tds::packet_type;
using crpc::db::tds::prelogin;
using crpc::db::tds::prelogin_option;
using crpc::db::tds::token;
using crpc::db::tds::token_reader;
using crpc::db::tds::token_type;

[[nodiscard]] crpc::span<const std::byte> view(const std::vector<std::byte>& data) noexcept {
    return crpc::span<const std::byte>(data.data(), data.size());
}

[[nodiscard]] std::string hex(const std::vector<std::byte>& data, std::size_t from = 0,
                              std::size_t count = static_cast<std::size_t>(-1)) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    const std::size_t end = count == static_cast<std::size_t>(-1)
                                ? data.size()
                                : std::min(data.size(), from + count);
    for (std::size_t i = from; i < end; ++i) {
        const auto byte = static_cast<std::uint8_t>(data[i]);
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0f]);
    }
    return out;
}

void put_be16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>((value >> 8) & 0xffu));
    out.push_back(static_cast<std::byte>(value & 0xffu));
}

void put_u16_le(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value & 0xffu));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xffu));
}

void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xffu));
    }
}

void put_be32(std::vector<std::byte>& out, std::uint32_t value) {
    out.push_back(static_cast<std::byte>((value >> 24) & 0xffu));
    out.push_back(static_cast<std::byte>((value >> 16) & 0xffu));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xffu));
    out.push_back(static_cast<std::byte>(value & 0xffu));
}

void put_text(std::vector<std::byte>& out, std::string_view text) {
    const std::vector<std::byte> encoded = crpc::db::tds::utf16le(text);
    out.insert(out.end(), encoded.begin(), encoded.end());
}

} // namespace

CRPC_TEST(tds_header_is_big_endian) {
    std::vector<std::byte> buffer(crpc::db::tds::header_size);
    packet_header header;
    header.type = packet_type::login7;
    header.status = static_cast<std::uint8_t>(crpc::db::tds::packet_status::eom);
    header.length = 0x0123;
    header.spid = 0x4567;
    header.packet_id = 1;

    std::size_t written = 0;
    CRPC_CHECK(crpc::db::tds::encode_header(crpc::span<std::byte>(buffer.data(), buffer.size()),
                                            header, written)
                   .ok());
    CRPC_CHECK_EQ(std::size_t{8}, written);
    // Type, status, then the length big-endian.
    CRPC_CHECK_EQ(std::string("1001012345670100"), hex(buffer));

    packet_header decoded;
    CRPC_CHECK(crpc::db::tds::decode_header(view(buffer), decoded).ok());
    CRPC_CHECK_EQ(static_cast<int>(packet_type::login7), static_cast<int>(decoded.type));
    CRPC_CHECK_EQ(0x0123, static_cast<int>(decoded.length));
    CRPC_CHECK_EQ(0x4567, static_cast<int>(decoded.spid));
    CRPC_CHECK(decoded.is_eom());
}

CRPC_TEST(tds_header_rejects_a_lying_length) {
    std::vector<std::byte> short_length = {std::byte{0x10}, std::byte{0}, std::byte{0},
                                           std::byte{0x04}, std::byte{0}, std::byte{0},
                                           std::byte{1}, std::byte{0}};
    packet_header header;
    CRPC_CHECK_FALSE(crpc::db::tds::decode_header(view(short_length), header).ok());

    // A header may be decoded before its payload has arrived: the length is reported,
    // not enforced, because the caller reads the payload separately. A length below
    // the header size is still refused above, since no packet can be that short.
    std::vector<std::byte> payload_pending = {std::byte{0x10}, std::byte{0}, std::byte{0x10},
                                              std::byte{0x00}, std::byte{0}, std::byte{0},
                                              std::byte{1}, std::byte{0}};
    CRPC_CHECK(crpc::db::tds::decode_header(view(payload_pending), header).ok());
    CRPC_CHECK_EQ(0x1000, static_cast<int>(header.length));
}

CRPC_TEST(tds_prelogin_round_trip) {
    prelogin value;
    value.encryption = encryption_mode::not_supported;
    value.thread_id = 0x11223344;
    value.mars = 0;

    std::vector<std::byte> payload;
    CRPC_CHECK(crpc::db::tds::encode_prelogin(payload, value).ok());
    // Four options plus the terminator.
    CRPC_CHECK_EQ(std::size_t{4 * 5 + 1}, payload.size() - (6 + 1 + 4 + 1));

    prelogin decoded;
    CRPC_CHECK(crpc::db::tds::decode_prelogin(view(payload), decoded).ok());
    CRPC_CHECK_EQ(static_cast<int>(encryption_mode::not_supported),
                  static_cast<int>(decoded.encryption));
    CRPC_CHECK_EQ(std::uint32_t{0x11223344}, decoded.thread_id);
    CRPC_CHECK_EQ(static_cast<int>(0x0f), static_cast<int>(decoded.version_major));
}

CRPC_TEST(tds_password_obfuscation) {
    // "abc" = 61 62 63 -> nibble swap 16 26 36 -> XOR A5 = B3 83 93.
    const std::vector<std::uint8_t> encoded = crpc::db::tds::obfuscate_password("abc");
    // Two bytes per character: swap each byte of the UTF-16LE unit, then XOR 0xA5.
    // The high byte of an ASCII character is zero, so it becomes 0xA5.
    CRPC_CHECK_EQ(std::size_t{6}, encoded.size());
    CRPC_CHECK_EQ(0xB3, static_cast<int>(encoded[0]));
    CRPC_CHECK_EQ(0xA5, static_cast<int>(encoded[1]));
    CRPC_CHECK_EQ(0x83, static_cast<int>(encoded[2]));
    CRPC_CHECK_EQ(0xA5, static_cast<int>(encoded[3]));
    CRPC_CHECK_EQ(0x93, static_cast<int>(encoded[4]));
    CRPC_CHECK_EQ(0xA5, static_cast<int>(encoded[5]));
}

CRPC_TEST(tds_utf16_round_trip) {
    const std::string text = "crpc\xE5\xAF\x86\xE7\xA0\x81"; // "crpc" + two CJK chars
    const std::vector<std::byte> encoded = crpc::db::tds::utf16le(text);
    CRPC_CHECK_EQ(std::size_t{12}, encoded.size());
    CRPC_CHECK_EQ(text, crpc::db::tds::from_utf16le(view(encoded)));

    // A code point above the BMP becomes a surrogate pair and comes back intact.
    const std::string emoji = "\xF0\x9F\x98\x80"; // U+1F600
    const std::vector<std::byte> pair = crpc::db::tds::utf16le(emoji);
    CRPC_CHECK_EQ(std::size_t{4}, pair.size());
    CRPC_CHECK_EQ(emoji, crpc::db::tds::from_utf16le(view(pair)));
}

CRPC_TEST(tds_login7_layout) {
    login7 value;
    value.host_name = "host";
    value.user_name = "sa";
    value.password = "pw";
    value.application_name = "crpc";
    value.server_name = "srv";
    value.database = "db";

    std::vector<std::byte> packet;
    CRPC_CHECK(crpc::db::tds::encode_login7(packet, value).ok());
    CRPC_CHECK(packet.size() > 94);

    // Total length is a 32-bit little-endian field at offset 0.
    const std::uint32_t total = static_cast<std::uint32_t>(static_cast<std::uint8_t>(packet[0])) |
                                (static_cast<std::uint32_t>(static_cast<std::uint8_t>(packet[1])) << 8) |
                                (static_cast<std::uint32_t>(static_cast<std::uint8_t>(packet[2])) << 16) |
                                (static_cast<std::uint32_t>(static_cast<std::uint8_t>(packet[3])) << 24);
    CRPC_CHECK_EQ(static_cast<std::uint32_t>(packet.size()), total);

    // TDS version 7.4 little-endian.
    CRPC_CHECK_EQ(std::uint32_t{0x74000004u},
                  static_cast<std::uint32_t>(static_cast<std::uint8_t>(packet[4])) |
                      (static_cast<std::uint32_t>(static_cast<std::uint8_t>(packet[5])) << 8) |
                      (static_cast<std::uint32_t>(static_cast<std::uint8_t>(packet[6])) << 16) |
                      (static_cast<std::uint32_t>(static_cast<std::uint8_t>(packet[7])) << 24));

    const auto pair_at = [&](std::size_t offset) {
        const std::uint16_t field = static_cast<std::uint16_t>(
            static_cast<std::uint8_t>(packet[offset]) |
            (static_cast<std::uint16_t>(static_cast<std::uint8_t>(packet[offset + 1])) << 8));
        return field;
    };
    // Host name: offset 94 (nothing precedes it), four characters.
    CRPC_CHECK_EQ(0x005E, static_cast<int>(pair_at(36)));
    CRPC_CHECK_EQ(4, static_cast<int>(pair_at(38)));
    // The password length is in characters, and its bytes are obfuscated: the
    // plaintext "pw" must not appear anywhere in the packet.
    CRPC_CHECK_EQ(2, static_cast<int>(pair_at(46)));
    for (std::size_t i = 0; i + 1 < packet.size(); ++i) {
        const bool plain = static_cast<std::uint8_t>(packet[i]) == 'p' &&
                           static_cast<std::uint8_t>(packet[i + 1]) == 'w';
        CRPC_CHECK_FALSE(plain);
    }
}

CRPC_TEST(tds_tokens_login_phase) {
    // LOGINACK, then ENVCHANGE(database), then DONE.
    std::vector<std::byte> payload;
    payload.push_back(static_cast<std::byte>(token_type::loginack));
    std::vector<std::byte> ack;
    ack.push_back(std::byte{1});  // interface
    put_be32(ack, 0x74000004u);   // server TDS version
    ack.push_back(std::byte{4});  // program name, four characters
    put_text(ack, "srv1");
    ack.push_back(std::byte{0x0f}); // version 15.0.2000 (four bytes)
    ack.push_back(std::byte{0x00});
    ack.push_back(std::byte{0x07});
    ack.push_back(std::byte{0xd0});
    put_u16_le(payload, static_cast<std::uint16_t>(ack.size()));
    payload.insert(payload.end(), ack.begin(), ack.end());

    payload.push_back(static_cast<std::byte>(token_type::envchange));
    std::vector<std::byte> env;
    env.push_back(std::byte{1}); // database
    env.push_back(std::byte{2}); // new value, two characters
    put_text(env, "db");
    env.push_back(std::byte{0}); // old value, empty
    put_u16_le(payload, static_cast<std::uint16_t>(env.size()));
    payload.insert(payload.end(), env.begin(), env.end());

    payload.push_back(static_cast<std::byte>(token_type::done));
    put_be16(payload, 0x0000); // status
    put_be16(payload, 0x0000); // current command
    put_u32(payload, 0);      // row count high
    put_u32(payload, 0);      // row count low

    token_reader reader(view(payload));
    token first;
    CRPC_CHECK_EQ(static_cast<int>(token_reader::result::token_ready),
                  static_cast<int>(reader.next(first)));
    CRPC_CHECK_EQ(static_cast<int>(token::kind::login_ack), static_cast<int>(first.type));
    CRPC_CHECK_EQ(std::uint32_t{0x74000004u}, first.ack.tds_version);
    CRPC_CHECK_EQ(std::string("srv1"), first.ack.program_name);
    CRPC_CHECK_EQ(std::size_t{4}, first.ack.server_version.size());

    token second;
    CRPC_CHECK_EQ(static_cast<int>(token_reader::result::token_ready),
                  static_cast<int>(reader.next(second)));
    CRPC_CHECK_EQ(static_cast<int>(token::kind::env_change), static_cast<int>(second.type));
    CRPC_CHECK_EQ(std::string("db"), second.env.new_value);

    token third;
    CRPC_CHECK_EQ(static_cast<int>(token_reader::result::token_ready),
                  static_cast<int>(reader.next(third)));
    CRPC_CHECK_EQ(static_cast<int>(token::kind::done), static_cast<int>(third.type));
    CRPC_CHECK_FALSE(third.done.more_results());
    CRPC_CHECK_EQ(static_cast<int>(token_reader::result::end),
                  static_cast<int>(reader.next(third)));
}

CRPC_TEST(tds_error_token_is_decoded) {
    std::vector<std::byte> payload;
    payload.push_back(static_cast<std::byte>(token_type::error));
    std::vector<std::byte> body;
    put_u32(body, 18456); // ERROR numerics are little-endian
    body.push_back(std::byte{1}); // state
    body.push_back(std::byte{14}); // severity
    put_u16_le(body, 9); // US_VARCHAR: a little-endian character count
    put_text(body, "bad login");
    body.push_back(std::byte{3});
    put_text(body, "srv");
    body.push_back(std::byte{0}); // procedure, empty
    put_u32(body, 1);             // line, little-endian
    put_u16_le(payload, static_cast<std::uint16_t>(body.size()));
    payload.insert(payload.end(), body.begin(), body.end());

    token_reader reader(view(payload));
    token decoded;
    CRPC_CHECK_EQ(static_cast<int>(token_reader::result::token_ready),
                  static_cast<int>(reader.next(decoded)));
    CRPC_CHECK_EQ(static_cast<int>(token::kind::error), static_cast<int>(decoded.type));
    CRPC_CHECK_EQ(std::uint32_t{18456}, decoded.error.number);
    CRPC_CHECK_EQ(static_cast<int>(14), static_cast<int>(decoded.error.severity));
    CRPC_CHECK_EQ(std::string("bad login"), decoded.error.message);
    CRPC_CHECK_EQ(std::string("srv"), decoded.error.server_name);
    CRPC_CHECK_EQ(std::uint32_t{1}, decoded.error.line);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(crpc::db::tds::map_error_number(decoded.error.number)));
}

CRPC_TEST(tds_unknown_token_is_skipped) {
    // An unmodelled token must not desynchronise the stream: it carries a length.
    std::vector<std::byte> payload;
    payload.push_back(std::byte{0xE4}); // an unhandled token type
    put_u16_le(payload, 3);
    payload.push_back(std::byte{1});
    payload.push_back(std::byte{2});
    payload.push_back(std::byte{3});
    payload.push_back(static_cast<std::byte>(token_type::done));
    put_be16(payload, 0);
    put_be16(payload, 0);
    put_u32(payload, 7); // row count low
    put_u32(payload, 0); // row count high
    token_reader reader(view(payload));
    token skipped;
    CRPC_CHECK_EQ(static_cast<int>(token_reader::result::token_ready),
                  static_cast<int>(reader.next(skipped)));
    CRPC_CHECK_EQ(static_cast<int>(token::kind::unknown), static_cast<int>(skipped.type));

    token done;
    CRPC_CHECK_EQ(static_cast<int>(token_reader::result::token_ready),
                  static_cast<int>(reader.next(done)));
    CRPC_CHECK_EQ(static_cast<int>(token::kind::done), static_cast<int>(done.type));
    CRPC_CHECK_EQ(std::uint64_t{7}, done.done.row_count);
}

CRPC_TEST(tds_decodes_a_real_login_rejection) {
    // Bytes captured from SQL Server 2022 (16.0.4305.5) rejecting a login:
    //   AA 54 00 | number 18456, state 1, class 14, "Login failed for user 'sa'.",
    //   server "LIULILTE", no procedure, line 1 | FD DONE(error, 0 rows)
    // Kept verbatim because a hand-built token is exactly how a length convention
    // bug hides: this one is the server's own framing.
    static const unsigned char raw[] = {
        0xaa, 0x54, 0x00, 0x18, 0x48, 0x00, 0x00, 0x01, 0x0e, 0x1b, 0x00, 0x4c, 0x00, 0x6f, 0x00,
        0x67, 0x00, 0x69, 0x00, 0x6e, 0x00, 0x20, 0x00, 0x66, 0x00, 0x61, 0x00, 0x69, 0x00, 0x6c,
        0x00, 0x65, 0x00, 0x64, 0x00, 0x20, 0x00, 0x66, 0x00, 0x6f, 0x00, 0x72, 0x00, 0x20, 0x00,
        0x75, 0x00, 0x73, 0x00, 0x65, 0x00, 0x72, 0x00, 0x20, 0x00, 0x27, 0x00, 0x73, 0x00, 0x61,
        0x00, 0x27, 0x00, 0x2e, 0x00, 0x08, 0x4c, 0x00, 0x49, 0x00, 0x55, 0x00, 0x4c, 0x00, 0x49,
        0x00, 0x4c, 0x00, 0x54, 0x00, 0x45, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xfd, 0x02, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    std::vector<std::byte> payload(sizeof raw);
    for (std::size_t i = 0; i < sizeof raw; ++i) {
        payload[i] = static_cast<std::byte>(raw[i]);
    }
    token_reader reader(crpc::span<const std::byte>(payload.data(), payload.size()));
    token first;
    CRPC_CHECK_EQ(static_cast<int>(token_reader::result::token_ready),
                  static_cast<int>(reader.next(first)));
    CRPC_CHECK_EQ(static_cast<int>(token::kind::error), static_cast<int>(first.type));
    CRPC_CHECK_EQ(std::uint32_t{18456}, first.error.number);
    CRPC_CHECK_EQ(std::uint8_t{14}, first.error.severity);
    CRPC_CHECK_EQ(std::string("Login failed for user 'sa'."), first.error.message);
    CRPC_CHECK_EQ(std::string("LIULILTE"), first.error.server_name);
    CRPC_CHECK_EQ(std::uint32_t{1}, first.error.line);

    token second;
    CRPC_CHECK_EQ(static_cast<int>(token_reader::result::token_ready),
                  static_cast<int>(reader.next(second)));
    CRPC_CHECK_EQ(static_cast<int>(token::kind::done), static_cast<int>(second.type));
    CRPC_CHECK(second.done.error());
    CRPC_CHECK_EQ(std::uint64_t{0}, second.done.row_count);

    token third;
    CRPC_CHECK_EQ(static_cast<int>(token_reader::result::end),
                  static_cast<int>(reader.next(third)));
}

CRPC_TEST_MAIN()
