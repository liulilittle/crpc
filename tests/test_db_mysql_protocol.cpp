// CRPC -- MySQL/MariaDB protocol codec tests.
//
// The vectors are the protocol's own shapes, written by hand from the documented
// layout: a codec that agrees with itself proves nothing, so each case states the
// bytes a server would send and asserts the decoded fields, and for the encoders
// states the exact bytes the client must produce.
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/db/mysql/protocol.hpp>
#include <crpc/db/types.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace {

using crpc::db::mysql::capability;
using crpc::db::mysql::column_type;
using crpc::db::mysql::handshake;
using crpc::db::mysql::mask;
using crpc::db::mysql::packet_kind;

[[nodiscard]] std::vector<std::byte> bytes(std::initializer_list<std::uint8_t> values) {
    std::vector<std::byte> out;
    out.reserve(values.size());
    for (const std::uint8_t value : values) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

[[nodiscard]] std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> out(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
    }
    return out;
}

void append_text(std::vector<std::byte>& out, std::string_view text) {
    const std::vector<std::byte> encoded = bytes_of(text);
    out.insert(out.end(), encoded.begin(), encoded.end());
}

[[nodiscard]] crpc::span<const std::byte> view(const std::vector<std::byte>& data) noexcept {
    return crpc::span<const std::byte>(data.data(), data.size());
}

[[nodiscard]] std::string hex(const std::vector<std::byte>& data) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (const std::byte value : data) {
        const auto byte = static_cast<std::uint8_t>(value);
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0f]);
    }
    return out;
}

} // namespace

CRPC_TEST(mysql_lenenc_int_roundtrip) {
    struct sample {
        std::uint64_t value;
        const char* wire;
    };
    // 250 and 251 are the boundary: 251 is the first value that needs a prefix.
    const sample samples[] = {
        {0, "00"},
        {1, "01"},
        {250, "fa"},
        {251, "fcfb00"},
        {65535, "fcffff"},
        {65536, "fd000001"},
        {16777215, "fdffffff"},
        {16777216, "fe0000000100000000"},
        {18446744073709551615ull, "feffffffffffffffff"},
    };
    for (const sample& item : samples) {
        std::vector<std::byte> encoded;
        crpc::db::mysql::append_lenenc_int(encoded, item.value);
        CRPC_CHECK_EQ(std::string(item.wire), hex(encoded));

        std::size_t offset = 0;
        std::uint64_t value = 0;
        bool is_null = true;
        const crpc::status read =
            crpc::db::mysql::read_lenenc_int(view(encoded), offset, false, value, is_null);
        CRPC_CHECK(read.ok());
        CRPC_CHECK_EQ(item.value, value);
        CRPC_CHECK_FALSE(is_null);
        CRPC_CHECK_EQ(encoded.size(), offset);
    }
}

CRPC_TEST(mysql_lenenc_null_only_in_row_context) {
    const std::vector<std::byte> marker = bytes({0xfb});
    std::size_t offset = 0;
    std::uint64_t value = 0;
    bool is_null = false;

    // In a row, 0xfb is SQL NULL.
    CRPC_CHECK(crpc::db::mysql::read_lenenc_int(view(marker), offset, true, value, is_null).ok());
    CRPC_CHECK(is_null);

    // Outside a row it is a protocol violation, not a zero.
    offset = 0;
    is_null = false;
    const crpc::status outside =
        crpc::db::mysql::read_lenenc_int(view(marker), offset, false, value, is_null);
    CRPC_CHECK_FALSE(outside.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(outside.code()));
}

CRPC_TEST(mysql_lenenc_int_rejects_truncation) {
    {
        // 0xfc announces two bytes but only one follows.
        const std::vector<std::byte> truncated = bytes({0xfc, 0x01});
        std::size_t offset = 0;
        std::uint64_t value = 0;
        bool is_null = false;
        CRPC_CHECK_FALSE(
            crpc::db::mysql::read_lenenc_int(view(truncated), offset, false, value, is_null).ok());
    }
    {
        // 0xfe announces eight bytes but only three follow.
        const std::vector<std::byte> truncated = bytes({0xfe, 1, 2, 3});
        std::size_t offset = 0;
        std::uint64_t value = 0;
        bool is_null = false;
        CRPC_CHECK_FALSE(
            crpc::db::mysql::read_lenenc_int(view(truncated), offset, false, value, is_null).ok());
    }
    {
        // 0xff is never a length prefix.
        const std::vector<std::byte> invalid = bytes({0xff});
        std::size_t offset = 0;
        std::uint64_t value = 0;
        bool is_null = false;
        CRPC_CHECK_FALSE(
            crpc::db::mysql::read_lenenc_int(view(invalid), offset, false, value, is_null).ok());
    }
}

CRPC_TEST(mysql_lenenc_string) {
    std::vector<std::byte> encoded;
    crpc::db::mysql::append_lenenc_string(encoded, "hello");
    CRPC_CHECK_EQ(std::string("0568656c6c6f"), hex(encoded));

    std::size_t offset = 0;
    std::string_view value;
    bool is_null = true;
    CRPC_CHECK(
        crpc::db::mysql::read_lenenc_string(view(encoded), offset, false, value, is_null).ok());
    CRPC_CHECK_EQ(std::string("hello"), std::string(value));
    CRPC_CHECK_FALSE(is_null);

    // An empty string is not NULL.
    std::vector<std::byte> empty;
    crpc::db::mysql::append_lenenc_string(empty, "");
    CRPC_CHECK_EQ(std::string("00"), hex(empty));
    offset = 0;
    CRPC_CHECK(crpc::db::mysql::read_lenenc_string(view(empty), offset, true, value, is_null).ok());
    CRPC_CHECK_FALSE(is_null);
    CRPC_CHECK(value.empty());
}

CRPC_TEST(mysql_write_message_frames_one_packet) {
    const std::vector<std::byte> payload = bytes_of("SELECT 1");
    std::vector<std::byte> out;
    std::uint8_t sequence = 0;
    CRPC_CHECK(crpc::db::mysql::write_message(out, view(payload), sequence).ok());
    // 3-byte length, sequence 0, then the payload.
    CRPC_CHECK_EQ(std::string("08000000") + hex(payload), hex(out));
    CRPC_CHECK_EQ(std::uint8_t{1}, sequence);
}

CRPC_TEST(mysql_write_message_splits_and_terminates) {
    // A payload of exactly the maximum must be followed by an empty packet, or the
    // server keeps waiting for the rest of the message.
    std::vector<std::byte> payload(crpc::db::mysql::max_packet_payload, std::byte{'x'});
    std::vector<std::byte> out;
    std::uint8_t sequence = 3;
    CRPC_CHECK(crpc::db::mysql::write_message(out, view(payload), sequence).ok());

    const std::size_t expected = (payload.size() + 4) + 4;
    CRPC_CHECK_EQ(expected, out.size());
    // First packet: 0xffffff length, sequence 3.
    CRPC_CHECK_EQ(std::string("ffffff03"), hex(std::vector<std::byte>(out.begin(), out.begin() + 4)));
    // Final empty packet: zero length, sequence 4.
    CRPC_CHECK_EQ(std::string("00000004"), hex(std::vector<std::byte>(out.end() - 4, out.end())));
    CRPC_CHECK_EQ(std::uint8_t{5}, sequence);
}

CRPC_TEST(mysql_encode_command) {
    const std::vector<std::byte> query = crpc::db::mysql::encode_command(0x03, "SELECT 1");
    // COM_QUERY is 0x03.
    CRPC_CHECK_EQ(std::string("0353454c4543542031"), hex(query));

    const std::vector<std::byte> ping = crpc::db::mysql::encode_command(0x0e, "");
    CRPC_CHECK_EQ(std::string("0e"), hex(ping));
}

CRPC_TEST(mysql_handshake_response_shape) {
    const std::vector<std::byte> auth(20, std::byte{0xaa});
    const std::uint32_t capabilities =
        mask(capability::protocol_41) | mask(capability::long_password) |
        mask(capability::secure_connection) | mask(capability::plugin_auth) |
        mask(capability::connect_with_db);

    crpc::db::mysql::handshake_response_options options;
    options.capabilities = capabilities;
    options.max_packet_size = 16777216;
    options.charset = 45;
    options.username = "root";
    options.auth_response = view(auth);
    options.database = "app";
    options.auth_plugin = "caching_sha2_password";

    std::vector<std::byte> out(crpc::db::mysql::handshake_response_size(options));
    std::size_t written = 0;
    CRPC_CHECK(crpc::db::mysql::encode_handshake_response(
                   crpc::span<std::byte>(out.data(), out.size()), options, written)
                   .ok());
    CRPC_CHECK_EQ(out.size(), written);

    std::size_t offset = 0;
    CRPC_CHECK_EQ(capabilities, crpc::db::mysql::read_u32(view(out), offset));
    offset += 4;
    CRPC_CHECK_EQ(std::uint32_t{16777216}, crpc::db::mysql::read_u32(view(out), offset));
    offset += 4;
    CRPC_CHECK_EQ(45, static_cast<int>(out[offset]));
    ++offset;
    // 23 reserved bytes.
    for (int i = 0; i < 23; ++i) {
        CRPC_CHECK_EQ(0, static_cast<int>(out[offset + static_cast<std::size_t>(i)]));
    }
    offset += 23;
    CRPC_CHECK_EQ(std::string("root"), std::string(reinterpret_cast<const char*>(out.data() + offset), 4));
    offset += 5;
    // One-byte auth length, then the 20 bytes.
    CRPC_CHECK_EQ(20, static_cast<int>(out[offset]));
    ++offset;
    CRPC_CHECK_EQ(std::string(20, 'a'), hex(std::vector<std::byte>(
                                            out.begin() + static_cast<std::ptrdiff_t>(offset),
                                            out.begin() + static_cast<std::ptrdiff_t>(offset + 20)))
                                           .substr(0, 20));
    offset += 20;
    CRPC_CHECK_EQ(std::string("app"), std::string(reinterpret_cast<const char*>(out.data() + offset), 3));
    offset += 4;
    CRPC_CHECK_EQ(std::string("caching_sha2_password"),
                  std::string(reinterpret_cast<const char*>(out.data() + offset),
                              out.size() - offset - 1));
}

CRPC_TEST(mysql_handshake_response_requires_protocol_41) {
    crpc::db::mysql::handshake_response_options options;
    options.capabilities = 0;
    options.username = "root";
    std::vector<std::byte> out(crpc::db::mysql::handshake_response_size(options) + 8);
    std::size_t written = 99;
    const crpc::status result = crpc::db::mysql::encode_handshake_response(
        crpc::span<std::byte>(out.data(), out.size()), options, written);
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(result.code()));
    CRPC_CHECK_EQ(std::size_t{0}, written);
}

CRPC_TEST(mysql_decode_handshake) {
    // Build a handshake 10 payload: the nonce is split 8 + 12 by the protocol, and
    // the server version is NUL-terminated.
    std::vector<std::byte> payload;
    payload.push_back(std::byte{10});
    const std::string version = "8.0.36";
    append_text(payload, version);
    payload.push_back(std::byte{0});
    // connection id 42
    payload.push_back(std::byte{42});
    payload.push_back(std::byte{0});
    payload.push_back(std::byte{0});
    payload.push_back(std::byte{0});
    // nonce part 1: 8 bytes
    for (int i = 0; i < 8; ++i) {
        payload.push_back(std::byte{static_cast<std::uint8_t>(i + 1)});
    }
    payload.push_back(std::byte{0}); // filler
    const std::uint32_t capabilities =
        mask(capability::protocol_41) | mask(capability::secure_connection) |
        mask(capability::plugin_auth) | mask(capability::deprecate_eof);
    payload.push_back(std::byte{static_cast<std::uint8_t>(capabilities & 0xffu)});
    payload.push_back(std::byte{static_cast<std::uint8_t>((capabilities >> 8) & 0xffu)});
    payload.push_back(std::byte{45}); // charset
    payload.push_back(std::byte{2});  // status: autocommit
    payload.push_back(std::byte{0});
    payload.push_back(std::byte{static_cast<std::uint8_t>((capabilities >> 16) & 0xffu)});
    payload.push_back(std::byte{static_cast<std::uint8_t>((capabilities >> 24) & 0xffu)});
    payload.push_back(std::byte{21}); // auth data length: 8 + 13 (12 + NUL)
    for (int i = 0; i < 10; ++i) {
        payload.push_back(std::byte{0});
    }
    // nonce part 2: 12 bytes then NUL
    for (int i = 0; i < 12; ++i) {
        payload.push_back(std::byte{static_cast<std::uint8_t>(i + 9)});
    }
    payload.push_back(std::byte{0});
    const std::string plugin = "caching_sha2_password";
    append_text(payload, plugin);
    payload.push_back(std::byte{0});

    handshake decoded;
    CRPC_CHECK(crpc::db::mysql::decode_handshake(view(payload), decoded).ok());
    CRPC_CHECK_EQ(10, static_cast<int>(decoded.protocol_version));
    CRPC_CHECK_EQ(version, decoded.server_version);
    CRPC_CHECK_EQ(std::uint32_t{42}, decoded.connection_id);
    CRPC_CHECK_EQ(std::size_t{20}, decoded.auth_plugin_data.size());
    CRPC_CHECK_EQ(std::string(plugin), decoded.auth_plugin_name);
    CRPC_CHECK_EQ(45, static_cast<int>(decoded.charset));
    CRPC_CHECK_EQ(static_cast<int>(capability::deprecate_eof),
                  static_cast<int>(decoded.capabilities & mask(capability::deprecate_eof)));
    // The two nonce halves must join in order: 1..8 then 9..20.
    for (int i = 0; i < 20; ++i) {
        CRPC_CHECK_EQ(i + 1, static_cast<int>(decoded.auth_plugin_data[static_cast<std::size_t>(i)]));
    }
}

CRPC_TEST(mysql_decode_handshake_rejects_old_protocol) {
    std::vector<std::byte> payload{std::byte{9}, std::byte{'x'}, std::byte{0}};
    handshake decoded;
    const crpc::status result = crpc::db::mysql::decode_handshake(view(payload), decoded);
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(result.code()));
}

CRPC_TEST(mysql_classify_packets) {
    const auto kind_of = [](std::initializer_list<std::uint8_t> values, bool expect_column) {
        const std::vector<std::byte> data = bytes(values);
        return static_cast<int>(crpc::db::mysql::classify(view(data), expect_column));
    };

    CRPC_CHECK_EQ(static_cast<int>(packet_kind::ok), kind_of({0x00, 1}, true));
    CRPC_CHECK_EQ(static_cast<int>(packet_kind::err), kind_of({0xff, 1, 2}, true));
    // A short 0xfe packet is EOF; a long one is a row whose first value is huge.
    CRPC_CHECK_EQ(static_cast<int>(packet_kind::eof), kind_of({0xfe, 0, 0, 2, 0}, true));
    const std::vector<std::byte> long_packet(9, std::byte{0xfe});
    CRPC_CHECK_EQ(static_cast<int>(packet_kind::row),
                  static_cast<int>(crpc::db::mysql::classify(view(long_packet), true)));
    CRPC_CHECK_EQ(static_cast<int>(packet_kind::local_infile), kind_of({0xfb}, true));
    CRPC_CHECK_EQ(static_cast<int>(packet_kind::column), kind_of({3, 'd', 'e', 'f'}, true));
}

CRPC_TEST(mysql_decode_ok) {
    std::vector<std::byte> payload;
    payload.push_back(std::byte{0});
    crpc::db::mysql::append_lenenc_int(payload, 1); // affected rows
    crpc::db::mysql::append_lenenc_int(payload, 9); // last insert id
    payload.push_back(std::byte{0x02});             // status: autocommit
    payload.push_back(std::byte{0x00});
    payload.push_back(std::byte{0x00});             // warnings
    payload.push_back(std::byte{0x00});
    const std::string info = "Rows matched: 1";
    append_text(payload, info);

    crpc::db::mysql::ok_packet ok;
    CRPC_CHECK(crpc::db::mysql::decode_ok(view(payload), mask(capability::protocol_41), ok).ok());
    CRPC_CHECK_EQ(std::uint64_t{1}, ok.affected_rows);
    CRPC_CHECK_EQ(std::uint64_t{9}, ok.last_insert_id);
    CRPC_CHECK_EQ(2, static_cast<int>(ok.status_flags));
    CRPC_CHECK_EQ(0, static_cast<int>(ok.warnings));
    CRPC_CHECK_EQ(info, ok.info);
}

CRPC_TEST(mysql_decode_err) {
    // 0xff, code 1062, SQL state 23000, message.
    std::vector<std::byte> payload;
    payload.push_back(std::byte{0xff});
    payload.push_back(std::byte{0x26}); // 1062 = 0x0426
    payload.push_back(std::byte{0x04});
    payload.push_back(std::byte{'#'});
    const std::string state = "23000";
    append_text(payload, state);
    const std::string message = "Duplicate entry '1' for key 'PRIMARY'";
    append_text(payload, message);

    crpc::db::mysql::err_packet err;
    CRPC_CHECK(crpc::db::mysql::decode_err(view(payload), err).ok());
    CRPC_CHECK_EQ(1062, static_cast<int>(err.code));
    CRPC_CHECK_EQ(state, err.sql_state);
    CRPC_CHECK_EQ(message, err.message);
}

CRPC_TEST(mysql_decode_eof) {
    // 0xfe, warnings 0, status 0x0002 (autocommit).
    const std::vector<std::byte> payload = bytes({0xfe, 0x00, 0x00, 0x02, 0x00});
    crpc::db::mysql::eof_packet eof;
    CRPC_CHECK(crpc::db::mysql::decode_eof(view(payload), eof).ok());
    CRPC_CHECK_EQ(0, static_cast<int>(eof.warnings));
    CRPC_CHECK_EQ(2, static_cast<int>(eof.status_flags));
}

CRPC_TEST(mysql_decode_column) {
    std::vector<std::byte> payload;
    crpc::db::mysql::append_lenenc_string(payload, "def");
    crpc::db::mysql::append_lenenc_string(payload, "app");
    crpc::db::mysql::append_lenenc_string(payload, "users");
    crpc::db::mysql::append_lenenc_string(payload, "users");
    crpc::db::mysql::append_lenenc_string(payload, "id");
    crpc::db::mysql::append_lenenc_string(payload, "id");
    crpc::db::mysql::append_lenenc_int(payload, 0x0c);
    payload.push_back(std::byte{63}); // charset 63 = binary
    payload.push_back(std::byte{0});
    // length 4 (BIGINT is 20, but the value here only tests the field position)
    payload.push_back(std::byte{20});
    payload.push_back(std::byte{0});
    payload.push_back(std::byte{0});
    payload.push_back(std::byte{0});
    payload.push_back(std::byte{8}); // LONGLONG
    payload.push_back(std::byte{0x03}); // NOT_NULL | PRI_KEY
    payload.push_back(std::byte{0x00});
    payload.push_back(std::byte{0}); // decimals
    payload.push_back(std::byte{0});
    payload.push_back(std::byte{0});

    crpc::db::mysql::column_definition column;
    CRPC_CHECK(crpc::db::mysql::decode_column(view(payload), column).ok());
    CRPC_CHECK_EQ(std::string("app"), column.schema);
    CRPC_CHECK_EQ(std::string("users"), column.table);
    CRPC_CHECK_EQ(std::string("id"), column.name);
    CRPC_CHECK_EQ(63, static_cast<int>(column.charset));
    CRPC_CHECK_EQ(std::uint32_t{20}, column.length);
    CRPC_CHECK_EQ(static_cast<int>(column_type::longlong), static_cast<int>(column.type));
    CRPC_CHECK_EQ(3, static_cast<int>(column.flags));
}

CRPC_TEST(mysql_decode_text_row) {
    std::vector<std::byte> payload;
    crpc::db::mysql::append_lenenc_string(payload, "42");
    crpc::db::mysql::append_null(payload);
    crpc::db::mysql::append_lenenc_string(payload, "hello");

    crpc::db::mysql::text_row row;
    CRPC_CHECK(crpc::db::mysql::decode_text_row(view(payload), row).ok());
    CRPC_CHECK_EQ(std::size_t{3}, row.values.size());
    CRPC_CHECK_EQ(std::string("42"), std::string(row.values[0]));
    CRPC_CHECK(row.nulls[1]);
    CRPC_CHECK_EQ(std::string("hello"), std::string(row.values[2]));
}

CRPC_TEST(mysql_map_error_codes) {
    using crpc::db::mysql::map_error_code;
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(map_error_code(1062)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                  static_cast<int>(map_error_code(1146)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable),
                  static_cast<int>(map_error_code(1213)));
    CRPC_CHECK(crpc::is_retryable(map_error_code(1205)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(map_error_code(1064)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::deadline_exceeded),
                  static_cast<int>(map_error_code(3024)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::application),
                  static_cast<int>(map_error_code(9999)));
}

CRPC_TEST(mysql_map_column_types) {
    using crpc::db::mysql::map_column_type;
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::int64),
                  static_cast<int>(map_column_type(column_type::longlong, 0)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::uint64),
                  static_cast<int>(map_column_type(column_type::longlong, 0x0020)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::decimal),
                  static_cast<int>(map_column_type(column_type::newdecimal, 0)));
    // A BLOB is bytes only when the binary charset flag says so.
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::blob),
                  static_cast<int>(map_column_type(column_type::blob, 0x0080)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::text),
                  static_cast<int>(map_column_type(column_type::blob, 0)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::timestamp),
                  static_cast<int>(map_column_type(column_type::datetime, 0)));
}

CRPC_TEST_MAIN()