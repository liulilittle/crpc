// CRPC -- TDS result-set token tests.
//
// A ROW carries values only; the widths come from the COLMETADATA before it. That is
// why a reader must remember the metadata, and why a ROW without it is an error
// rather than a best-effort guess. Each case builds the tokens the server would send
// and asserts the decoded text form.
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

using crpc::db::tds::column_meta;
using crpc::db::tds::token;
using crpc::db::tds::token_reader;
using crpc::db::tds::token_type;

[[nodiscard]] crpc::span<const std::byte> view(const std::vector<std::byte>& data) noexcept {
    return crpc::span<const std::byte>(data.data(), data.size());
}

void put_be16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>((value >> 8) & 0xffu));
    out.push_back(static_cast<std::byte>(value & 0xffu));
}

void put_le16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value & 0xffu));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xffu));
}

void put_be32(std::vector<std::byte>& out, std::uint32_t value) {
    out.push_back(static_cast<std::byte>((value >> 24) & 0xffu));
    out.push_back(static_cast<std::byte>((value >> 16) & 0xffu));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xffu));
    out.push_back(static_cast<std::byte>(value & 0xffu));
}

void put_le32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xffu));
    }
}

void put_text(std::vector<std::byte>& out, std::string_view text) {
    const std::vector<std::byte> encoded = crpc::db::tds::utf16le(text);
    out.insert(out.end(), encoded.begin(), encoded.end());
}

// One COLMETADATA column: user type, flags, the type-specific info, then the name.
struct column_spec {
    std::uint8_t type;
    std::uint16_t max_length;
    std::uint8_t precision;
    std::uint8_t scale;
    const char* name;
};

void put_colmetadata(std::vector<std::byte>& payload, const std::vector<column_spec>& columns) {
    payload.push_back(static_cast<std::byte>(token_type::colmetadata));
    put_le16(payload, static_cast<std::uint16_t>(columns.size()));
    for (const column_spec& column : columns) {
        put_be32(payload, 0);            // user type
        put_le16(payload, 1);            // flags
        payload.push_back(static_cast<std::byte>(column.type));
        switch (column.type) {
        case 0xE7: // NVARCHAR
        case 0xA7: // BIGVARCHAR
        case 0xA5: // BIGVARBINARY
            put_le16(payload, column.max_length);
            if (column.type != 0xA5) {
                for (int i = 0; i < 5; ++i) {
                    payload.push_back(std::byte{0}); // collation
                }
            }
            break;
        case 0x6A: // DECIMALN
            payload.push_back(static_cast<std::byte>(column.max_length));
            payload.push_back(static_cast<std::byte>(column.precision));
            payload.push_back(static_cast<std::byte>(column.scale));
            break;
        case 0x26: // INTN
            payload.push_back(static_cast<std::byte>(column.max_length));
            break;
        case 0x28: // DATE: no type info at all
            break;
        case 0x2A: // DATETIME2: only the scale
            payload.push_back(static_cast<std::byte>(column.scale));
            break;
        default:
            break; // fixed-width types carry nothing
        }
        const std::string name(column.name);
        payload.push_back(static_cast<std::byte>(name.size()));
        put_text(payload, name);
    }
}

} // namespace

CRPC_TEST(tds_colmetadata_and_row_of_fixed_types) {
    std::vector<std::byte> payload;
    put_colmetadata(payload, {{0x38, 0, 0, 0, "id"},   // INT4
                              {0x7F, 0, 0, 0, "big"},  // INT8
                              {0x32, 0, 0, 0, "flag"}}); // BIT
    payload.push_back(static_cast<std::byte>(token_type::row));
    put_le32(payload, 0xFFFFFFFFu); // INT4 -1
    for (int i = 0; i < 8; ++i) {
        payload.push_back(static_cast<std::byte>(0xff)); // INT8 -1
    }
    payload.push_back(std::byte{1}); // BIT

    token_reader reader(view(payload));
    token meta;
    CRPC_CHECK_EQ(static_cast<int>(token_reader::result::token_ready),
                  static_cast<int>(reader.next(meta)));
    CRPC_CHECK_EQ(static_cast<int>(token::kind::colmetadata), static_cast<int>(meta.type));
    CRPC_CHECK_EQ(std::size_t{3}, meta.columns.size());
    CRPC_CHECK_EQ(std::string("id"), meta.columns[0].name);
    CRPC_CHECK_EQ(static_cast<int>(0x38), static_cast<int>(meta.columns[0].type));
    // A fixed-width column is never NULL.
    CRPC_CHECK_FALSE(meta.columns[0].nullable);
    CRPC_CHECK(meta.columns[2].nullable == false);

    token row;
    CRPC_CHECK_EQ(static_cast<int>(token_reader::result::token_ready),
                  static_cast<int>(reader.next(row)));
    CRPC_CHECK_EQ(static_cast<int>(token::kind::row), static_cast<int>(row.type));
    // The INT4 bytes above are 0xFFFFFFFF, i.e. -1 as a signed value.
    CRPC_CHECK_EQ(std::string("-1"), row.values[0]);
    CRPC_CHECK_EQ(std::string("-1"), row.values[1]);
    CRPC_CHECK_EQ(std::string("1"), row.values[2]);
}

CRPC_TEST(tds_row_of_strings_and_nulls) {
    std::vector<std::byte> payload;
    put_colmetadata(payload, {{0xE7, 40, 0, 0, "name"}, {0xA5, 16, 0, 0, "blob"}});
    payload.push_back(static_cast<std::byte>(token_type::row));
    // NVARCHAR "hi": two bytes of UTF-16, length is in bytes and big-endian.
    put_le16(payload, 4);
    put_text(payload, "hi");
    // BIGVARBINARY NULL: a zero length.
    put_le16(payload, 0);

    token_reader reader(view(payload));
    token meta;
    CRPC_CHECK_EQ(static_cast<int>(token_reader::result::token_ready),
                  static_cast<int>(reader.next(meta)));
    CRPC_CHECK_EQ(std::size_t{2}, meta.columns.size());
    // A variable-length column is nullable.
    CRPC_CHECK(meta.columns[0].nullable);

    token row;
    CRPC_CHECK_EQ(static_cast<int>(token_reader::result::token_ready),
                  static_cast<int>(reader.next(row)));
    CRPC_CHECK_EQ(std::string("hi"), row.values[0]);
    CRPC_CHECK_FALSE(row.nulls[0]);
    CRPC_CHECK(row.nulls[1]);
}

CRPC_TEST(tds_row_of_decimal_and_datetime) {
    std::vector<std::byte> payload;
    put_colmetadata(payload, {{0x6A, 5, 10, 2, "amount"},  // DECIMALN(10,2)
                              {0x2A, 7, 0, 7, "at"},       // DATETIME2(7)
                              {0x28, 3, 0, 0, "day"}});    // DATE
    payload.push_back(static_cast<std::byte>(token_type::row));

    // DECIMAL 12.34: length 5, sign 1 = positive, magnitude 1234 little-endian.
    payload.push_back(std::byte{5});
    payload.push_back(std::byte{1});
    payload.push_back(std::byte{0xd2});
    payload.push_back(std::byte{0x04});
    payload.push_back(std::byte{0x00});
    payload.push_back(std::byte{0x00});

    // DATETIME2: length 8, then five bytes of 100ns ticks and three of days.
    payload.push_back(std::byte{8});
    const std::uint64_t ticks = (12ull * 3600 + 34 * 60 + 56) * 10000000ull + 1234567ull;
    for (int i = 0; i < 5; ++i) {
        payload.push_back(static_cast<std::byte>((ticks >> (8 * i)) & 0xffu));
    }
    // 2024-03-04: days since 0001-01-01.
    const std::uint32_t days = 739000; // arbitrary but valid; checked as a date shape
    payload.push_back(static_cast<std::byte>(days & 0xffu));
    payload.push_back(static_cast<std::byte>((days >> 8) & 0xffu));
    payload.push_back(static_cast<std::byte>((days >> 16) & 0xffu));

    // DATE: a length byte, then the same day encoding.
    payload.push_back(std::byte{3});
    payload.push_back(static_cast<std::byte>(days & 0xffu));
    payload.push_back(static_cast<std::byte>((days >> 8) & 0xffu));
    payload.push_back(static_cast<std::byte>((days >> 16) & 0xffu));

    token_reader reader(view(payload));
    token meta;
    CRPC_CHECK_EQ(static_cast<int>(token_reader::result::token_ready),
                  static_cast<int>(reader.next(meta)));
    token row;
    CRPC_CHECK_EQ(static_cast<int>(token_reader::result::token_ready),
                  static_cast<int>(reader.next(row)));
    CRPC_CHECK_EQ(std::string("12.34"), row.values[0]);
    // The time part is printed at the column's scale, not at full precision.
    CRPC_CHECK_EQ(std::string("2024-04-25 12:34:56.1234567"), row.values[1]);
    CRPC_CHECK_EQ(row.values[1].substr(0, 10), row.values[2]);
}

CRPC_TEST(tds_nbcrow_null_bitmap) {
    std::vector<std::byte> payload;
    put_colmetadata(payload, {{0x38, 0, 0, 0, "a"}, {0x38, 0, 0, 0, "b"}});
    payload.push_back(static_cast<std::byte>(token_type::nbc_row));
    // Null bitmap: one byte, bit 1 set means the second column is NULL.
    payload.push_back(std::byte{0x02});
    put_le32(payload, 5u);

    token_reader reader(view(payload));
    token meta;
    CRPC_CHECK_EQ(static_cast<int>(token_reader::result::token_ready),
                  static_cast<int>(reader.next(meta)));
    token row;
    CRPC_CHECK_EQ(static_cast<int>(token_reader::result::token_ready),
                  static_cast<int>(reader.next(row)));
    CRPC_CHECK_EQ(std::string("5"), row.values[0]);
    CRPC_CHECK_FALSE(row.nulls[0]);
    CRPC_CHECK(row.nulls[1]);
}

CRPC_TEST(tds_row_without_metadata_is_refused) {
    // A ROW with no COLMETADATA before it cannot be decoded: the widths are unknown,
    // and guessing would produce plausible garbage.
    std::vector<std::byte> payload;
    payload.push_back(static_cast<std::byte>(token_type::row));
    put_be32(payload, 1);
    token_reader reader(view(payload));
    token row;
    CRPC_CHECK_EQ(static_cast<int>(token_reader::result::error),
                  static_cast<int>(reader.next(row)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(reader.error()));
}

CRPC_TEST(tds_colmetadata_with_no_columns) {
    std::vector<std::byte> payload;
    payload.push_back(static_cast<std::byte>(token_type::colmetadata));
    put_le16(payload, 0xFFFF); // "no metadata"
    token_reader reader(view(payload));
    token meta;
    CRPC_CHECK_EQ(static_cast<int>(token_reader::result::token_ready),
                  static_cast<int>(reader.next(meta)));
    CRPC_CHECK(meta.columns.empty());
}

CRPC_TEST(tds_decodes_real_server_column_metadata_and_row) {
    // Captured from SQL Server 2022 for
    //   SELECT CAST(-7 AS int) i, CAST(N'héllo' AS nvarchar(20)) s,
    //          CAST(12.34 AS decimal(10,2)) d, CAST(... AS datetime2(7)) t,
    //          CAST(NULL AS int) n, CAST(0x0102 AS varbinary(4)) b,
    //          CAST('6F9619FF-...' AS uniqueidentifier) g, CAST(1 AS bit) f
    // The metadata is the part that has bitten this decoder four times: the column
    // count, the flags and the maximum lengths are little-endian, DATE carries no
    // type info, and the 7.3 date/time types carry only a scale. Hand-built tokens
    // cannot prove any of that, because they encode the same assumption as the code.
    static const unsigned char meta[] = {
        0x81, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x00, 0x26, 0x04, 0x01, 0x69, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x21, 0x00, 0xe7, 0x28, 0x00, 0x09, 0x04, 0xd0, 0x00, 0x34, 0x01, 0x73,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x00, 0x6a, 0x11, 0x0a, 0x02, 0x01, 0x64, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x21, 0x00, 0x2a, 0x07, 0x01, 0x74, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21,
        0x00, 0x26, 0x04, 0x01, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x00, 0xa5, 0x04, 0x00,
        0x01, 0x62, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x00, 0x24, 0x10, 0x01, 0x67, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x21, 0x00, 0x68, 0x01, 0x01, 0x66, 0x00
    };
    std::vector<std::byte> payload;
    for (const unsigned char byte : meta) {
        payload.push_back(static_cast<std::byte>(byte));
    }

    token_reader reader(crpc::span<const std::byte>(payload.data(), payload.size()));
    token meta_token;
    CRPC_CHECK_EQ(static_cast<int>(token_reader::result::token_ready),
                  static_cast<int>(reader.next(meta_token)));
    CRPC_CHECK_EQ(static_cast<int>(token::kind::colmetadata),
                  static_cast<int>(meta_token.type));
    CRPC_CHECK_EQ(std::size_t{8}, meta_token.columns.size());
    if (meta_token.columns.size() == 8) {
        CRPC_CHECK_EQ(std::string("i"), meta_token.columns[0].name);
        CRPC_CHECK_EQ(static_cast<int>(0x26), static_cast<int>(meta_token.columns[0].type));
        CRPC_CHECK_EQ(std::uint32_t{4}, meta_token.columns[0].max_length);
        CRPC_CHECK_EQ(std::string("s"), meta_token.columns[1].name);
        CRPC_CHECK_EQ(static_cast<int>(0xE7), static_cast<int>(meta_token.columns[1].type));
        // nvarchar(20) is 40 bytes, little-endian on the wire.
        CRPC_CHECK_EQ(std::uint32_t{40}, meta_token.columns[1].max_length);
        CRPC_CHECK_EQ(std::string("d"), meta_token.columns[2].name);
        CRPC_CHECK_EQ(std::uint8_t{10}, meta_token.columns[2].precision);
        CRPC_CHECK_EQ(std::uint8_t{2}, meta_token.columns[2].scale);
        CRPC_CHECK_EQ(std::string("t"), meta_token.columns[3].name);
        // datetime2(7): scale 7, and the byte count follows from it (8).
        CRPC_CHECK_EQ(std::uint8_t{7}, meta_token.columns[3].scale);
        CRPC_CHECK_EQ(std::uint32_t{8}, meta_token.columns[3].max_length);
        // Nullable is bit 0 of the flags word (0x21 here).
        CRPC_CHECK(meta_token.columns[0].nullable);
        CRPC_CHECK_EQ(std::string("g"), meta_token.columns[6].name);
        CRPC_CHECK_EQ(static_cast<int>(0x24), static_cast<int>(meta_token.columns[6].type));
        CRPC_CHECK_EQ(std::string("f"), meta_token.columns[7].name);
        CRPC_CHECK_EQ(static_cast<int>(0x68), static_cast<int>(meta_token.columns[7].type));
    }
}

CRPC_TEST(tds_decodes_a_real_transaction_descriptor_change) {
    // ENVCHANGE(begin_transaction) captured from SQL Server 2022:
    //   E3 0B 00 | 08 | 08 05 00 00 00 36 00 00 00 | 00 | FD ...
    // The new value is length-prefixed (0x08), and the descriptor is the eight bytes
    // after that prefix. Reading the prefix as part of the descriptor makes the client
    // echo a shifted value, and the server answers "the server failed to resume the
    // transaction" with digits that look almost right.
    static const unsigned char raw[] = {
        0xe3, 0x0b, 0x00, 0x08, 0x08, 0x05, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00, 0x00, 0xfd,
        0x00, 0x00, 0xd4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    std::vector<std::byte> payload;
    for (const unsigned char byte : raw) {
        payload.push_back(static_cast<std::byte>(byte));
    }
    token_reader reader(crpc::span<const std::byte>(payload.data(), payload.size()));
    token change;
    CRPC_CHECK_EQ(static_cast<int>(token_reader::result::token_ready),
                  static_cast<int>(reader.next(change)));
    CRPC_CHECK_EQ(static_cast<int>(token::kind::env_change), static_cast<int>(change.type));
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::tds::env_change_type::begin_transaction),
                  static_cast<int>(change.env.type));
    const std::array<std::uint8_t, 8> expected{0x05, 0x00, 0x00, 0x00, 0x36, 0x00, 0x00, 0x00};
    CRPC_CHECK(change.env.new_descriptor == expected);

    token done;
    CRPC_CHECK_EQ(static_cast<int>(token_reader::result::token_ready),
                  static_cast<int>(reader.next(done)));
    CRPC_CHECK_EQ(static_cast<int>(token::kind::done), static_cast<int>(done.type));
}

CRPC_TEST_MAIN()
