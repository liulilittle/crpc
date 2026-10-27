// CRPC -- ADO-style parameter direction and ORM column metadata tests.
//
// Two things an ORM cannot work without: knowing which direction a parameter flows
// (a stored procedure writes back through OUT/INOUT) and knowing where a result
// column came from. The second is subtle: mapping on the alias instead of the
// original column name silently changes which struct field binds when a query adds
// an alias.
#include "test_framework.hpp"

#include <cstdint>
#include <string>

#include <crpc/db/mysql/protocol.hpp>
#include <crpc/db/types.hpp>
#include <crpc/error.hpp>

namespace {

using crpc::db::column_info;
using crpc::db::db_type;
using crpc::db::parameter;
using crpc::db::parameter_direction;

} // namespace

CRPC_TEST(direction_classifies_flow) {
    const parameter input = parameter::from_int64(1);
    CRPC_CHECK(input.direction == parameter_direction::input);
    CRPC_CHECK(input.sends_value());
    CRPC_CHECK_FALSE(input.is_output());

    const parameter output =
        parameter::out("total", db_type::int64, parameter_direction::output);
    CRPC_CHECK_FALSE(output.sends_value());
    CRPC_CHECK(output.is_output());
    CRPC_CHECK(output.is_null);
    CRPC_CHECK_EQ(std::string("total"), output.name);
    CRPC_CHECK_EQ(static_cast<int>(db_type::int64), static_cast<int>(output.type));

    const parameter inout =
        parameter::out("counter", db_type::int64, parameter_direction::input_output);
    // INOUT both sends and receives.
    CRPC_CHECK(inout.sends_value());
    CRPC_CHECK(inout.is_output());

    const parameter ret =
        parameter::out("", db_type::int64, parameter_direction::return_value);
    CRPC_CHECK_FALSE(ret.sends_value());
    CRPC_CHECK(ret.is_output());
}

CRPC_TEST(execute_rejects_output_only_parameter) {
    // MySQL binds OUT parameters through session variables in the SQL text, so an
    // output-only parameter must never reach the execute encoder. Accepting it would
    // shift every later value by one slot.
    std::vector<parameter> params;
    params.push_back(parameter::from_int64(1));
    params.push_back(parameter::out("result", db_type::int64));

    std::vector<std::byte> out;
    const crpc::status encoded = crpc::db::mysql::encode_stmt_execute(
        out, 5, crpc::span<const parameter>(params.data(), params.size()));
    CRPC_CHECK_FALSE(encoded.ok());
    if (!encoded.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(encoded.code()));
    }
    CRPC_CHECK(out.empty());
}

CRPC_TEST(execute_accepts_input_output_parameter) {
    // INOUT carries a value in, so it is encoded like an input parameter.
    std::vector<parameter> params;
    params.push_back(parameter::from_int64(7));
    params.back().direction = parameter_direction::input_output;
    params.back().name = "counter";

    std::vector<std::byte> out;
    const crpc::status encoded = crpc::db::mysql::encode_stmt_execute(
        out, 5, crpc::span<const parameter>(params.data(), params.size()));
    CRPC_CHECK(encoded.ok());
    if (encoded.ok()) {
        // header (10) + bitmap (1) + flag (1) + one type (2) + value (8)
        CRPC_CHECK_EQ(std::size_t{22}, out.size());
    }
}

CRPC_TEST(column_info_maps_original_names_for_orm) {
    crpc::db::mysql::column_definition definition;
    definition.catalog = "def";
    definition.schema = "shop";
    definition.table = "orders_alias";
    definition.org_table = "orders";
    definition.name = "user_id";       // the alias the query produced
    definition.org_name = "id";        // the column as declared
    definition.charset = 63;
    definition.length = 20;
    definition.type = crpc::db::mysql::column_type::longlong;
    definition.flags = static_cast<std::uint16_t>(crpc::db::mysql::column_flag::not_null) |
                       static_cast<std::uint16_t>(crpc::db::mysql::column_flag::primary_key);
    definition.decimals = 0;

    const column_info info = crpc::db::mysql::to_column_info(definition);
    CRPC_CHECK_EQ(std::string("user_id"), info.name);
    // An ORM binds on the original name; binding on the alias loses the field when
    // the alias changes.
    CRPC_CHECK_EQ(std::string("id"), info.original_name);
    CRPC_CHECK_EQ(std::string("shop"), info.schema);
    CRPC_CHECK_EQ(std::string("orders"), info.table);
    CRPC_CHECK_EQ(static_cast<int>(db_type::int64), static_cast<int>(info.type));
    CRPC_CHECK_EQ(std::uint32_t{20}, info.precision);
    CRPC_CHECK_FALSE(info.nullable);
    CRPC_CHECK_EQ(definition.flags, info.flags);
}

CRPC_TEST(column_info_marks_nullable_and_text_types) {
    crpc::db::mysql::column_definition definition;
    definition.schema = "shop";
    definition.table = "items";
    definition.org_table = "items";
    definition.name = "note";
    definition.org_name = "note";
    definition.length = 255;
    definition.type = crpc::db::mysql::column_type::var_string;
    definition.flags = 0; // nullable, not binary

    const column_info info = crpc::db::mysql::to_column_info(definition);
    CRPC_CHECK(info.nullable);
    CRPC_CHECK_EQ(static_cast<int>(db_type::text), static_cast<int>(info.type));

    // A BLOB with the binary flag is bytes, not text: an ORM must not decode it as a
    // string on the way into a std::string field.
    definition.type = crpc::db::mysql::column_type::blob;
    definition.flags = static_cast<std::uint16_t>(crpc::db::mysql::column_flag::binary);
    const column_info binary = crpc::db::mysql::to_column_info(definition);
    CRPC_CHECK_EQ(static_cast<int>(db_type::blob), static_cast<int>(binary.type));
}

CRPC_TEST(column_info_falls_back_to_table_when_org_table_is_empty) {
    // Derived tables and some computed columns report no org_table; the mapping must
    // still name a table rather than going blank.
    crpc::db::mysql::column_definition definition;
    definition.schema = "shop";
    definition.table = "derived";
    definition.org_table = "";
    definition.name = "total";
    definition.org_name = "";
    definition.type = crpc::db::mysql::column_type::newdecimal;
    definition.flags = 0;

    const column_info info = crpc::db::mysql::to_column_info(definition);
    CRPC_CHECK_EQ(std::string("derived"), info.table);
    CRPC_CHECK_EQ(static_cast<int>(db_type::decimal), static_cast<int>(info.type));
    // A decimal is carried as text so no digits are lost; the ORM decides how to
    // represent it in the struct.
    CRPC_CHECK(crpc::db::is_textual(info.type));
}

CRPC_TEST_MAIN()