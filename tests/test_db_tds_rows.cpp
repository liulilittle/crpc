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
    put_be16(payload, static_cast<std::uint16_t>(columns.size()));
    for (const column_spec& column : columns) {
        put_be32(payload, 0);            // user type
        put_be16(payload, 1);            // flags
        payload.push_back(static_cast<std::byte>(column.type));
        switch (column.type) {
        case 0xE7: // NVARCHAR
        case 0xA7: // BIGVARCHAR
        case 0xA5: // BIGVARBINARY
            put_be16(payload, column.max_length);
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
        case 0x28: // DATE
            payload.push_back(static_cast<std::byte>(column.max_length));
            break;
        case 0x2A: // DATETIME2
            payload.push_back(static_cast<std::byte>(column.max_length));
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
    put_be16(payload, 4);
    put_text(payload, "hi");
    // BIGVARBINARY NULL: a zero length.
    put_be16(payload, 0);

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

    // DECIMAL 12.34: length 5, positive, magnitude 1234 little-endian.
    payload.push_back(std::byte{5});
    payload.push_back(std::byte{0});
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

    // DATE: the same day encoding.
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
    put_be16(payload, 0xFFFF); // "no metadata"
    token_reader reader(view(payload));
    token meta;
    CRPC_CHECK_EQ(static_cast<int>(token_reader::result::token_ready),
                  static_cast<int>(reader.next(meta)));
    CRPC_CHECK(meta.columns.empty());
}

CRPC_TEST_MAIN()
