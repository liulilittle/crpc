// CRPC -- MySQL prepared-statement wire codec tests.
//
// Two shapes are easy to get subtly wrong and impossible to notice until a real
// server answers: the parameter null bitmap starts at bit 0, while the row null
// bitmap starts at bit 2; and binary temporals are length-prefixed tuples, not
// strings. Both are pinned byte-for-byte here.
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

using crpc::db::mysql::binary_row;
using crpc::db::mysql::column_type;
using crpc::db::mysql::prepare_ok;
using crpc::db::parameter;

[[nodiscard]] std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> out(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
    }
    return out;
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

void push_u32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xffu));
    }
}

void push_u64(std::vector<std::byte>& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xffu));
    }
}

} // namespace

CRPC_TEST(mysql_decode_prepare_ok) {
    std::vector<std::byte> payload;
    payload.push_back(std::byte{0});
    push_u32(payload, 17);
    payload.push_back(std::byte{3}); // column count 3
    payload.push_back(std::byte{0});
    payload.push_back(std::byte{2}); // param count 2
    payload.push_back(std::byte{0});
    payload.push_back(std::byte{0}); // filler
    payload.push_back(std::byte{1}); // warnings 1
    payload.push_back(std::byte{0});

    prepare_ok ok;
    CRPC_CHECK(crpc::db::mysql::decode_prepare_ok(view(payload), ok).ok());
    CRPC_CHECK_EQ(std::uint32_t{17}, ok.statement_id);
    CRPC_CHECK_EQ(std::uint16_t{3}, ok.column_count);
    CRPC_CHECK_EQ(std::uint16_t{2}, ok.param_count);
    CRPC_CHECK_EQ(std::uint16_t{1}, ok.warnings);
}

CRPC_TEST(mysql_decode_prepare_ok_rejects_zero_id) {
    // Statement id 0 is not a valid handle; accepting it would make every later
    // execute address nothing.
    std::vector<std::byte> payload;
    payload.push_back(std::byte{0});
    push_u32(payload, 0);
    for (int i = 0; i < 9; ++i) {
        payload.push_back(std::byte{0});
    }
    prepare_ok ok;
    const crpc::status decoded = crpc::db::mysql::decode_prepare_ok(view(payload), ok);
    CRPC_CHECK_FALSE(decoded.ok());
    if (!decoded.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                      static_cast<int>(decoded.code()));
    }
}

CRPC_TEST(mysql_decode_prepare_ok_rejects_short_payload) {
    const std::vector<std::byte> payload{std::byte{0}, std::byte{1}};
    prepare_ok ok;
    CRPC_CHECK_FALSE(crpc::db::mysql::decode_prepare_ok(view(payload), ok).ok());
}

CRPC_TEST(mysql_encode_stmt_execute_no_params) {
    std::vector<std::byte> out;
    CRPC_CHECK(crpc::db::mysql::encode_stmt_execute(out, 7, {}).ok());
    // 0x17, id, flags, iteration.
    CRPC_CHECK_EQ(std::string("17") + "07000000" + "00" + "01000000", hex(out));
}

CRPC_TEST(mysql_encode_stmt_execute_mixed_params) {
    std::vector<parameter> params;
    params.push_back(parameter::null());
    params.push_back(parameter::from_int64(-2));
    params.push_back(parameter::from_uint64(42));
    params.push_back(parameter::from_double(1.5));
    params.push_back(parameter::from_text("ab"));
    params.push_back(parameter::from_blob(bytes_of("Z")));

    std::vector<std::byte> out;
    const crpc::status encoded =
        crpc::db::mysql::encode_stmt_execute(out, 1, crpc::span<const parameter>(params.data(), params.size()));
    CRPC_CHECK(encoded.ok());
    if (!encoded.ok()) {
        return;
    }

    // header: 0x17, id=1, flags=0, iteration=1
    // 0x17 + id + flags + iteration = 10 bytes, so the bitmap starts at index 10.
    CRPC_CHECK_EQ(std::string("17") + "01000000" + "00" + "01000000", hex(out).substr(0, 20));

    // null bitmap: bit 0 set (parameter 0 is NULL)
    CRPC_CHECK_EQ(static_cast<int>(0x01), static_cast<int>(out[10]));
// new-params-bound flag
    CRPC_CHECK_EQ(static_cast<int>(0x01), static_cast<int>(out[11]));

    // Type array: NULL, LONGLONG, LONGLONG|unsigned, DOUBLE, VAR_STRING, BLOB.
    const std::string types = hex(std::vector<std::byte>(out.begin() + 12, out.begin() + 24));
    CRPC_CHECK_EQ(std::string("0600080008800500fd00fc00"), types);

    // Values, in parameter order, skipping the null.
    std::size_t offset = 24;
    // int64 -2
    CRPC_CHECK_EQ(std::string("feffffffffffffff"), hex(std::vector<std::byte>(
                                                       out.begin() + static_cast<std::ptrdiff_t>(offset),
                                                       out.begin() + static_cast<std::ptrdiff_t>(offset + 8))));
    offset += 8;
    // uint64 42
    CRPC_CHECK_EQ(std::string("2a00000000000000"), hex(std::vector<std::byte>(
                                                       out.begin() + static_cast<std::ptrdiff_t>(offset),
                                                       out.begin() + static_cast<std::ptrdiff_t>(offset + 8))));
    offset += 8;
    // double 1.5 = 0x3FF8000000000000
    CRPC_CHECK_EQ(std::string("000000000000f83f"), hex(std::vector<std::byte>(
                                                       out.begin() + static_cast<std::ptrdiff_t>(offset),
                                                       out.begin() + static_cast<std::ptrdiff_t>(offset + 8))));
    offset += 8;
    // text "ab": length 2 then bytes
    CRPC_CHECK_EQ(std::string("026162"), hex(std::vector<std::byte>(
                                             out.begin() + static_cast<std::ptrdiff_t>(offset),
                                             out.begin() + static_cast<std::ptrdiff_t>(offset + 3))));
    offset += 3;
    // blob "Z": length 1 then byte
    CRPC_CHECK_EQ(std::string("015a"), hex(std::vector<std::byte>(
                                         out.begin() + static_cast<std::ptrdiff_t>(offset),
                                         out.begin() + static_cast<std::ptrdiff_t>(offset + 2))));
    offset += 2;
    CRPC_CHECK_EQ(out.size(), offset);
}

CRPC_TEST(mysql_encode_stmt_close) {
    const std::vector<std::byte> payload = crpc::db::mysql::encode_stmt_close(9);
    CRPC_CHECK_EQ(std::string("19") + "09000000", hex(payload));
}

CRPC_TEST(mysql_encode_stmt_execute_rejects_zero_id) {
    std::vector<std::byte> out;
    const crpc::status result = crpc::db::mysql::encode_stmt_execute(out, 0, {});
    CRPC_CHECK_FALSE(result.ok());
    if (!result.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(result.code()));
    }
}

CRPC_TEST(mysql_decode_binary_row_ints_and_null) {
    // Three columns: LONGLONG, VAR_STRING (NULL), DOUBLE.
    const std::vector<column_type> types = {column_type::longlong, column_type::var_string,
                                            column_type::double_};
    std::vector<std::byte> payload;
    payload.push_back(std::byte{0});   // binary row header
    payload.push_back(std::byte{0x08}); // null bitmap: bit (1 + 2) = 3
    push_u64(payload, static_cast<std::uint64_t>(7));
    push_u64(payload, 0x4008000000000000ull); // 3.0

    binary_row row;
    CRPC_CHECK(crpc::db::mysql::decode_binary_row(
                   view(payload), crpc::span<const column_type>(types.data(), types.size()), row)
                   .ok());
    CRPC_CHECK_EQ(std::size_t{3}, row.values.size());
    CRPC_CHECK_EQ(std::string("7"), row.values[0]);
    CRPC_CHECK(row.nulls[1]);
    CRPC_CHECK_EQ(std::string("3"), row.values[2]);
}

CRPC_TEST(mysql_decode_binary_row_negative_and_float) {
    // TINY -1 and FLOAT 0.5.
    const std::vector<column_type> columns = {column_type::tiny, column_type::float_};
    std::vector<std::byte> payload;
    payload.push_back(std::byte{0});
    payload.push_back(std::byte{0}); // no nulls
    payload.push_back(std::byte{0xff}); // -1 as int8
    const std::uint32_t half_as_float_bits = 0x3f000000u;
    push_u32(payload, half_as_float_bits);

    binary_row row;
    CRPC_CHECK(crpc::db::mysql::decode_binary_row(
                   view(payload), crpc::span<const column_type>(columns.data(), columns.size()), row)
                   .ok());
    CRPC_CHECK_EQ(std::string("-1"), row.values[0]);
    CRPC_CHECK_EQ(std::string("0.5"), row.values[1]);
}

CRPC_TEST(mysql_decode_binary_row_datetime) {
    const std::vector<column_type> columns = {column_type::datetime};
    std::vector<std::byte> payload;
    payload.push_back(std::byte{0});
    payload.push_back(std::byte{0});
    payload.push_back(std::byte{7}); // length: date + time, no microseconds
    payload.push_back(std::byte{0xe8}); // year 2024 low
    payload.push_back(std::byte{0x07}); // high
    payload.push_back(std::byte{3});
    payload.push_back(std::byte{4});
    payload.push_back(std::byte{5});
    payload.push_back(std::byte{6});
    payload.push_back(std::byte{7});

    binary_row row;
    CRPC_CHECK(crpc::db::mysql::decode_binary_row(
                   view(payload), crpc::span<const column_type>(columns.data(), columns.size()), row)
                   .ok());
    CRPC_CHECK_EQ(std::string("2024-03-04 05:06:07"), row.values[0]);
}

CRPC_TEST(mysql_decode_binary_row_zero_date_and_time) {
    {
        const std::vector<column_type> columns = {column_type::date};
        std::vector<std::byte> payload;
        payload.push_back(std::byte{0});
        payload.push_back(std::byte{0});
        payload.push_back(std::byte{0}); // length 0 = zero date
        binary_row row;
        CRPC_CHECK(crpc::db::mysql::decode_binary_row(
                       view(payload), crpc::span<const column_type>(columns.data(), columns.size()),
                       row)
                       .ok());
        // A zero date is text, not NULL; conflating them loses the distinction the
        // server reported.
        CRPC_CHECK_FALSE(row.nulls[0]);
        CRPC_CHECK_EQ(std::string("0000-00-00"), row.values[0]);
    }
    {
        const std::vector<column_type> columns = {column_type::time};
        std::vector<std::byte> payload;
        payload.push_back(std::byte{0});
        payload.push_back(std::byte{0});
        payload.push_back(std::byte{8}); // days + h/m/s
        payload.push_back(std::byte{1}); // negative
        push_u32(payload, 2);            // 2 days
        payload.push_back(std::byte{3});
        payload.push_back(std::byte{4});
        payload.push_back(std::byte{5});
        binary_row row;
        CRPC_CHECK(crpc::db::mysql::decode_binary_row(
                       view(payload), crpc::span<const column_type>(columns.data(), columns.size()),
                       row)
                       .ok());
        // A TIME is a duration: 2 days + 3 hours prints as 51:04:05.
        CRPC_CHECK_EQ(std::string("-51:04:05"), row.values[0]);
    }
}

CRPC_TEST(mysql_decode_binary_row_rejects_truncation) {
    const std::vector<column_type> columns = {column_type::longlong};
    std::vector<std::byte> payload;
    payload.push_back(std::byte{0});
    payload.push_back(std::byte{0});
    payload.push_back(std::byte{1}); // only one byte of an eight-byte field
    binary_row row;
    const crpc::status decoded = crpc::db::mysql::decode_binary_row(
        view(payload), crpc::span<const column_type>(columns.data(), columns.size()), row);
    CRPC_CHECK_FALSE(decoded.ok());
    if (!decoded.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                      static_cast<int>(decoded.code()));
    }
}

CRPC_TEST_MAIN()