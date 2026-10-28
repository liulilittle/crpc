// CRPC -- ORM value conversion.
//
// The conversions guarded here are the ones an ORM must get wrong loudly: NULL
// into a non-optional field, a decimal column into an integer field, an
// out-of-range integer, an unparseable timestamp. Every one of those is a
// status, never a silently defaulted value.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "test_framework.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <crpc/db/memory_result_set.hpp>
#include <crpc/orm/orm.hpp>

namespace {

enum class job : std::int32_t { none = 0, admin = 1, user = 2 };
enum class tag : std::uint8_t { a = 1, b = 2 };

crpc::db::column_info column_of(std::string name, crpc::db::db_type type) {
    crpc::db::column_info column;
    column.name = std::move(name);
    column.type = type;
    column.nullable = true;
    return column;
}

// memory_result_set is neither copyable nor movable (it derives from
// result_set), so a helper that "returns" one must allocate.
std::unique_ptr<crpc::db::memory_result_set> one_row(
    std::vector<crpc::db::column_info> columns, std::vector<std::string> values,
    std::vector<bool> nulls) {
    auto set = std::make_unique<crpc::db::memory_result_set>(std::move(columns), 1);
    set->add_row(std::move(values), std::move(nulls));
    return set;
}

crpc::status parse_time(const char* text,
                        std::chrono::system_clock::time_point& out) {
    return crpc::orm::values::parse_system_time(text, out);
}

} // namespace

CRPC_TEST(value_encode_scalars) {
    crpc::db::parameter int_param;
    CRPC_CHECK(crpc::orm::values::encode_value(std::int64_t{42}, int_param).ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::int64),
                  static_cast<int>(int_param.type));
    CRPC_CHECK_FALSE(int_param.is_null);
    CRPC_CHECK_EQ(std::int64_t{42}, int_param.int_value);

    crpc::db::parameter uint_param;
    CRPC_CHECK(
        crpc::orm::values::encode_value(std::uint64_t{3000000000ULL}, uint_param).ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::uint64),
                  static_cast<int>(uint_param.type));
    CRPC_CHECK_EQ(std::uint64_t{3000000000ULL}, uint_param.uint_value);

    crpc::db::parameter bool_param;
    CRPC_CHECK(crpc::orm::values::encode_value(true, bool_param).ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::boolean),
                  static_cast<int>(bool_param.type));
    CRPC_CHECK_EQ(std::int64_t{1}, bool_param.int_value);

    crpc::db::parameter real_param;
    CRPC_CHECK(crpc::orm::values::encode_value(1.5, real_param).ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::real),
                  static_cast<int>(real_param.type));
    CRPC_CHECK_EQ(1.5, real_param.real_value);

    crpc::db::parameter text_param;
    CRPC_CHECK(crpc::orm::values::encode_value(std::string("hi"), text_param).ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::text),
                  static_cast<int>(text_param.type));
    CRPC_CHECK_EQ(std::string("hi"), text_param.text);

    crpc::db::parameter enum_param;
    CRPC_CHECK(crpc::orm::values::encode_value(job::admin, enum_param).ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::int64),
                  static_cast<int>(enum_param.type));
    CRPC_CHECK_EQ(std::int64_t{1}, enum_param.int_value);

    crpc::db::parameter enum_u_param;
    CRPC_CHECK(crpc::orm::values::encode_value(tag::b, enum_u_param).ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::uint64),
                  static_cast<int>(enum_u_param.type));
    CRPC_CHECK_EQ(std::uint64_t{2}, enum_u_param.uint_value);
}

CRPC_TEST(value_encode_optional) {
    std::optional<std::string> empty;
    crpc::db::parameter param;
    const crpc::status st = crpc::orm::values::encode_value(empty, param);
    CRPC_CHECK(st.ok());
    CRPC_CHECK(param.is_null);
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::text),
                  static_cast<int>(param.type));

    std::optional<std::string> filled("x");
    const crpc::status st2 = crpc::orm::values::encode_value(filled, param);
    CRPC_CHECK(st2.ok());
    CRPC_CHECK_FALSE(param.is_null);
    CRPC_CHECK_EQ(std::string("x"), param.text);
}

CRPC_TEST(value_decode_integral) {
    auto set = one_row({column_of("v", crpc::db::db_type::int64)}, {"42"}, {false});
    CRPC_CHECK(set->next());
    std::int64_t out = 0;
    CRPC_CHECK(crpc::orm::values::decode_cell(*set, 0, out).ok());
    CRPC_CHECK_EQ(std::int64_t{42}, out);

    std::int32_t narrow = 0;
    CRPC_CHECK(crpc::orm::values::decode_cell(*set, 0, narrow).ok());
    CRPC_CHECK_EQ(42, narrow);

    std::uint64_t unsigned_out = 0;
    CRPC_CHECK(crpc::orm::values::decode_cell(*set, 0, unsigned_out).ok());
    CRPC_CHECK_EQ(std::uint64_t{42}, unsigned_out);
}

CRPC_TEST(value_decode_unsigned_large) {
    auto set = one_row({column_of("v", crpc::db::db_type::uint64)},
                       {"3000000000"}, {false});
    CRPC_CHECK(set->next());
    std::uint32_t out = 0;
    CRPC_CHECK(crpc::orm::values::decode_cell(*set, 0, out).ok());
    CRPC_CHECK_EQ(3000000000u, out);

    std::uint64_t wide = 0;
    CRPC_CHECK(crpc::orm::values::decode_cell(*set, 0, wide).ok());
    CRPC_CHECK_EQ(std::uint64_t{3000000000ULL}, wide);
}

CRPC_TEST(value_decode_negative_errors) {
    auto neg = one_row({column_of("v", crpc::db::db_type::int64)}, {"-5"}, {false});
    CRPC_CHECK(neg->next());
    std::uint64_t out = 0;
    const crpc::status st = crpc::orm::values::decode_cell(*neg, 0, out);
    CRPC_CHECK_FALSE(st.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::decode_error),
                  static_cast<int>(st.code()));

    std::int8_t tiny = 0;
    auto big = one_row({column_of("v", crpc::db::db_type::int64)}, {"500"}, {false});
    CRPC_CHECK(big->next());
    const crpc::status st2 = crpc::orm::values::decode_cell(*big, 0, tiny);
    CRPC_CHECK_FALSE(st2.ok());
}

CRPC_TEST(value_decode_bad_integer_text) {
    auto set = one_row({column_of("v", crpc::db::db_type::int64)}, {"abc"}, {false});
    CRPC_CHECK(set->next());
    std::int64_t out = 0;
    const crpc::status st = crpc::orm::values::decode_cell(*set, 0, out);
    CRPC_CHECK_FALSE(st.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::decode_error),
                  static_cast<int>(st.code()));
}

CRPC_TEST(value_decode_null_discipline) {
    auto set = one_row({column_of("v", crpc::db::db_type::int64)}, {"7"}, {true});
    CRPC_CHECK(set->next());

    std::optional<std::int64_t> optional_out;
    optional_out = 9;
    CRPC_CHECK(crpc::orm::values::decode_cell(*set, 0, optional_out).ok());
    CRPC_CHECK_FALSE(optional_out.has_value());

    std::int64_t plain = -1;
    const crpc::status st = crpc::orm::values::decode_cell(*set, 0, plain);
    CRPC_CHECK_FALSE(st.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::decode_error),
                  static_cast<int>(st.code()));
}

CRPC_TEST(value_decode_optional_value) {
    auto set = one_row({column_of("v", crpc::db::db_type::int64)}, {"7"}, {false});
    CRPC_CHECK(set->next());
    std::optional<std::int64_t> out;
    CRPC_CHECK(crpc::orm::values::decode_cell(*set, 0, out).ok());
    CRPC_CHECK(out.has_value());
    CRPC_CHECK_EQ(std::int64_t{7}, *out);
}

CRPC_TEST(value_decode_decimal_stays_text) {
    auto set = one_row({column_of("amount", crpc::db::db_type::decimal)}, {"12.50"},
                       {false});
    CRPC_CHECK(set->next());
    std::string out;
    CRPC_CHECK(crpc::orm::values::decode_cell(*set, 0, out).ok());
    CRPC_CHECK_EQ(std::string("12.50"), out);
}

CRPC_TEST(value_decode_enum) {
    auto set = one_row({column_of("v", crpc::db::db_type::int64)}, {"2"}, {false});
    CRPC_CHECK(set->next());
    job out = job::none;
    CRPC_CHECK(crpc::orm::values::decode_cell(*set, 0, out).ok());
    CRPC_CHECK(out == job::user);

    auto bad = one_row({column_of("v", crpc::db::db_type::int64)},
                           {"3000000000"}, {false});
    CRPC_CHECK(bad->next());
    const crpc::status st = crpc::orm::values::decode_cell(*bad, 0, out);
    CRPC_CHECK_FALSE(st.ok());
}

CRPC_TEST(value_time_roundtrip) {
    const auto origin = std::chrono::system_clock::from_time_t(0);
    CRPC_CHECK_EQ(std::string("1970-01-01 00:00:00"),
                  crpc::orm::values::format_system_time(origin));

    std::chrono::system_clock::time_point point;
    CRPC_CHECK(parse_time("2026-03-08 15:04:05", point).ok());
    CRPC_CHECK_EQ(std::string("2026-03-08 15:04:05"),
                  crpc::orm::values::format_system_time(point));

    std::chrono::system_clock::time_point with_t;
    CRPC_CHECK(parse_time("2026-03-08T15:04:05", with_t).ok());
    CRPC_CHECK(point == with_t);

    std::chrono::system_clock::time_point utc;
    CRPC_CHECK(parse_time("2026-03-08 15:04:05Z", utc).ok());
    CRPC_CHECK(point == utc);

    std::chrono::system_clock::time_point zoned;
    CRPC_CHECK(parse_time("2026-03-08 17:34:05+02:30", zoned).ok());
    CRPC_CHECK(point == zoned);

    std::chrono::system_clock::time_point fractional;
    CRPC_CHECK(parse_time("2026-03-08 15:04:05.500", fractional).ok());
    // Second-precision formatting truncates the fraction to :05.
    CRPC_CHECK(crpc::orm::values::format_system_time(fractional) >=
               std::string("2026-03-08 15:04:05"));
    CRPC_CHECK(crpc::orm::values::format_system_time(fractional) <
               std::string("2026-03-08 15:04:06"));
}

CRPC_TEST(value_time_malformed_errors) {
    std::chrono::system_clock::time_point out;
    CRPC_CHECK_FALSE(parse_time("2026-13-01 00:00:00", out).ok());
    CRPC_CHECK_FALSE(parse_time("2026-02-30 00:00:00", out).ok());
    CRPC_CHECK_FALSE(parse_time("2026-01-01 25:00:00", out).ok());
    CRPC_CHECK_FALSE(parse_time("2026/01/01 00:00:00", out).ok());
    CRPC_CHECK_FALSE(parse_time("2026-01-01T00:00:00ZZ", out).ok());
    CRPC_CHECK_FALSE(parse_time("yesterday", out).ok());
}

CRPC_TEST(value_decode_value_into) {
    std::int64_t id = 5;
    CRPC_CHECK(crpc::orm::values::decode_value_into(
                   crpc::db::db_value::from_int64(999), id)
                   .ok());
    CRPC_CHECK_EQ(std::int64_t{999}, id);

    std::optional<std::int64_t> optional_id;
    CRPC_CHECK(crpc::orm::values::decode_value_into(
                   crpc::db::db_value::from_int64(7), optional_id)
                   .ok());
    CRPC_CHECK(optional_id.has_value());
    CRPC_CHECK_EQ(std::int64_t{7}, *optional_id);

    std::string text;
    CRPC_CHECK(crpc::orm::values::decode_value_into(
                   crpc::db::db_value::from_text("abc"), text)
                   .ok());
    CRPC_CHECK_EQ(std::string("abc"), text);

    const crpc::status wrong =
        crpc::orm::values::decode_value_into(crpc::db::db_value::from_text("abc"), id);
    CRPC_CHECK_FALSE(wrong.ok());
}

CRPC_TEST_MAIN()