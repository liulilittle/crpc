// CRPC -- ORM SQL generation.
//
// The rules that matter: every value is a bound parameter (so the injection
// string can never leave a trail in the statement text), the auto-increment
// column is excluded from INSERT, UPDATE carries the row locator from the
// ORIGINAL snapshot, and identifiers are quoted per dialect.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "test_framework.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <crpc/orm/orm.hpp>

// Entities are registered at global scope: RTTI_DEFINE_TYPE opens `namespace
// rtti` for the specialization, which must resolve to ::rtti.
enum class account_role : std::int32_t { none = 0, admin = 1, user = 2 };

RTTI_DEFINE_ENUM(account_role, none, admin, user);

struct account {
    std::int64_t id = 0;
    std::string email;
    std::string display_name;
    std::optional<std::int64_t> age;
    std::optional<std::string> nickname;
    account_role role = account_role::none;
};

CRPC_ORM_ENTITY(account, "accounts",
    CRPC_ORM_FIELD(id, CRPC_ORM_PK, CRPC_ORM_AUTO),
    CRPC_ORM_FIELD(email, CRPC_ORM_COLUMN("email_addr"), CRPC_ORM_NOT_NULL),
    CRPC_ORM_FIELD(display_name),
    CRPC_ORM_FIELD(age, CRPC_ORM_NULLABLE),
    CRPC_ORM_FIELD(nickname),
    CRPC_ORM_FIELD(role));

// A composite primary key, declared as two CRPC_ORM_PK fields.
struct order_item {
    std::int64_t order_id = 0;
    std::int64_t line_no = 0;
    std::string sku;
};

CRPC_ORM_ENTITY(order_item, "order_items",
    CRPC_ORM_FIELD(order_id, CRPC_ORM_PK),
    CRPC_ORM_FIELD(line_no, CRPC_ORM_PK),
    CRPC_ORM_FIELD(sku, CRPC_ORM_PRECISION(20)));

namespace {

std::size_t placeholder_count(const std::string& sql) {
    std::size_t count = 0;
    for (const char c : sql) {
        if (c == '?') {
            ++count;
        }
    }
    return count;
}

} // namespace

CRPC_TEST(sql_identifier_quoting) {
    CRPC_CHECK_EQ(std::string("`accounts`"),
                  crpc::orm::sql::quote_identifier(crpc::orm::dialect::mysql, "accounts"));
    CRPC_CHECK_EQ(std::string("[accounts]"),
                  crpc::orm::sql::quote_identifier(crpc::orm::dialect::tsql, "accounts"));
    CRPC_CHECK_EQ(std::string("\"accounts\""),
                  crpc::orm::sql::quote_identifier(crpc::orm::dialect::ansi, "accounts"));

    // The dialect's own quote character inside an identifier is escaped, not
    // trusted.
    CRPC_CHECK_EQ(std::string("`weird``name`"),
                  crpc::orm::sql::quote_identifier(crpc::orm::dialect::mysql, "weird`name"));
    CRPC_CHECK_EQ(std::string("[we]]ird]"),
                  crpc::orm::sql::quote_identifier(crpc::orm::dialect::tsql, "we]ird"));
    CRPC_CHECK_EQ(std::string("\"we\"\"ird\""),
                  crpc::orm::sql::quote_identifier(crpc::orm::dialect::ansi, "we\"ird"));
}

CRPC_TEST(sql_insert_excludes_auto_increment) {
    const crpc::orm::entity_meta* meta = crpc::orm::entity_metadata<account>();

    account row;
    row.email = "a@example.com";
    row.display_name = "alice";
    row.role = account_role::user;

    std::vector<std::size_t> columns;
    for (const crpc::orm::column_meta& column : meta->columns) {
        if (!column.auto_increment) {
            columns.push_back(column.index);
        }
    }

    crpc::orm::sql::statement stmt;
    const crpc::status built = crpc::orm::sql::build_insert(
        *meta, crpc::orm::dialect::mysql, &row, columns, stmt);
    CRPC_CHECK(built.ok());
    if (!built.ok()) {
        return;
    }

    CRPC_CHECK_EQ(std::string("INSERT INTO `accounts` (`email_addr`, `display_name`, "
                              "`age`, `nickname`, `role`) VALUES (?, ?, ?, ?, ?)"),
                  stmt.text);
    CRPC_CHECK_EQ(std::size_t{5}, stmt.params.size());
    CRPC_CHECK_EQ(std::size_t{5}, placeholder_count(stmt.text));
    CRPC_CHECK_EQ(std::string("a@example.com"), stmt.params[0].text);
    CRPC_CHECK_EQ(std::string("alice"), stmt.params[1].text);
    CRPC_CHECK(stmt.params[2].is_null);
    CRPC_CHECK(stmt.params[3].is_null);
    CRPC_CHECK_EQ(static_cast<std::int64_t>(account_role::user), stmt.params[4].int_value);
}

CRPC_TEST(sql_insert_binds_injection_value) {
    const crpc::orm::entity_meta* meta = crpc::orm::entity_metadata<account>();
    const std::string payload = "x' OR 1=1 --";

    account row;
    row.display_name = payload;

    std::vector<std::size_t> columns;
    for (const crpc::orm::column_meta& column : meta->columns) {
        if (!column.auto_increment) {
            columns.push_back(column.index);
        }
    }

    crpc::orm::sql::statement stmt;
    CRPC_CHECK(crpc::orm::sql::build_insert(*meta, crpc::orm::dialect::mysql, &row,
                                            columns, stmt)
                   .ok());
    // The payload only ever travels as a parameter.
    CRPC_CHECK(stmt.text.find(payload) == std::string::npos);
    CRPC_CHECK(stmt.text.find("OR 1=1") == std::string::npos);
    bool seen = false;
    for (const crpc::db::parameter& param : stmt.params) {
        if (!param.is_null && param.text == payload) {
            seen = true;
        }
    }
    CRPC_CHECK(seen);
    CRPC_CHECK_EQ(stmt.params.size(), placeholder_count(stmt.text));
}

CRPC_TEST(sql_update_only_dirty_columns) {
    const crpc::orm::entity_meta* meta = crpc::orm::entity_metadata<account>();

    account original;
    original.id = 7;
    original.email = "old@example.com";
    original.display_name = "old"; // unchanged
    original.role = account_role::admin;

    account current = original;
    current.email = "new@example.com"; // the only dirty column

    std::vector<std::size_t> dirty;
    for (const crpc::orm::column_meta& column : meta->columns) {
        if (column.differs(&current, &original)) {
            dirty.push_back(column.index);
        }
    }
    CRPC_CHECK_EQ(std::size_t{1}, dirty.size());
    CRPC_CHECK_EQ(std::size_t{1}, dirty[0]);

    crpc::orm::sql::statement stmt;
    CRPC_CHECK(crpc::orm::sql::build_update(*meta, crpc::orm::dialect::mysql, &current,
                                            &original, dirty, stmt)
                   .ok());
    CRPC_CHECK_EQ(std::string("UPDATE `accounts` SET `email_addr` = ? WHERE `id` = ?"),
                  stmt.text);
    CRPC_CHECK_EQ(std::size_t{2}, stmt.params.size());
    CRPC_CHECK_EQ(std::string("new@example.com"), stmt.params[0].text);
    // The row locator is the ORIGINAL id, so a changed key cannot redirect.
    CRPC_CHECK_EQ(std::int64_t{7}, stmt.params[1].int_value);
}

CRPC_TEST(sql_update_locates_by_original_primary_key) {
    const crpc::orm::entity_meta* meta = crpc::orm::entity_metadata<account>();

    account original;
    original.id = 7;
    original.display_name = "old";
    account current = original;
    current.id = 9;     // the primary key itself changed
    current.display_name = "new";

    std::vector<std::size_t> dirty{1, 2};
    crpc::orm::sql::statement stmt;
    CRPC_CHECK(crpc::orm::sql::build_update(*meta, crpc::orm::dialect::mysql, &current,
                                            &original, dirty, stmt)
                   .ok());
    CRPC_CHECK_EQ(std::string(
                      "UPDATE `accounts` SET `email_addr` = ?, `display_name` = ? "
                      "WHERE `id` = ?"),
                  stmt.text);
    CRPC_CHECK_EQ(std::size_t{3}, stmt.params.size());
    CRPC_CHECK_EQ(std::int64_t{7}, stmt.params[2].int_value);
}

CRPC_TEST(sql_composite_primary_key) {
    const crpc::orm::entity_meta* meta = crpc::orm::entity_metadata<order_item>();
    CRPC_CHECK(meta->valid);
    CRPC_CHECK_EQ(std::size_t{2}, meta->primary_key.size());
    CRPC_CHECK_EQ(std::size_t{0}, meta->primary_key[0]);
    CRPC_CHECK_EQ(std::size_t{1}, meta->primary_key[1]);
    CRPC_CHECK_EQ(std::uint32_t{20}, meta->columns[2].precision);

    order_item current;
    current.order_id = 3;
    current.line_no = 7;
    current.sku = "ABC";
    order_item original = current;
    original.sku = "OLD";

    std::vector<std::size_t> dirty{2};
    crpc::orm::sql::statement update;
    CRPC_CHECK(crpc::orm::sql::build_update(*meta, crpc::orm::dialect::mysql, &current,
                                            &original, dirty, update)
                   .ok());
    CRPC_CHECK_EQ(std::string("UPDATE `order_items` SET `sku` = ? "
                              "WHERE `order_id` = ? AND `line_no` = ?"),
                  update.text);
    CRPC_CHECK_EQ(std::size_t{3}, update.params.size());
    CRPC_CHECK_EQ(std::int64_t{3}, update.params[1].int_value);
    CRPC_CHECK_EQ(std::int64_t{7}, update.params[2].int_value);

    crpc::orm::sql::statement remove;
    CRPC_CHECK(crpc::orm::sql::build_delete(*meta, crpc::orm::dialect::mysql, &original,
                                            remove)
                   .ok());
    CRPC_CHECK_EQ(std::string("DELETE FROM `order_items` "
                              "WHERE `order_id` = ? AND `line_no` = ?"),
                  remove.text);
}

CRPC_TEST(sql_delete_by_primary_key) {
    const crpc::orm::entity_meta* meta = crpc::orm::entity_metadata<account>();

    account row;
    row.id = 11;
    crpc::orm::sql::statement stmt;
    CRPC_CHECK(crpc::orm::sql::build_delete(*meta, crpc::orm::dialect::mysql, &row, stmt)
                   .ok());
    CRPC_CHECK_EQ(std::string("DELETE FROM `accounts` WHERE `id` = ?"), stmt.text);
    CRPC_CHECK_EQ(std::size_t{1}, stmt.params.size());
    CRPC_CHECK_EQ(std::int64_t{11}, stmt.params[0].int_value);
}

CRPC_TEST(sql_select_with_conditions) {
    const crpc::orm::entity_meta* meta = crpc::orm::entity_metadata<account>();

    std::vector<crpc::orm::sql::condition> conditions;
    conditions.push_back(
        {0, crpc::orm::sql::comp::eq, {crpc::db::parameter::from_int64(5)}});
    conditions.push_back({1, crpc::orm::sql::comp::in_,
                          {crpc::db::parameter::from_text("a@example.com"),
                           crpc::db::parameter::from_text("b@example.com")}});
    conditions.push_back({4, crpc::orm::sql::comp::is_null, {}});
    conditions.push_back(
        {5, crpc::orm::sql::comp::ne, {crpc::db::parameter::from_int64(0)}});

    std::vector<crpc::orm::sql::order_term> order;
    order.push_back({2, false});

    crpc::orm::sql::statement stmt;
    CRPC_CHECK(crpc::orm::sql::build_select(*meta, crpc::orm::dialect::mysql, conditions,
                                            order, true, 10, 20, stmt)
                   .ok());
    CRPC_CHECK_EQ(std::string(
                      "SELECT `id`, `email_addr`, `display_name`, `age`, `nickname`, "
                      "`role` FROM `accounts` "
                      "WHERE `id` = ? AND `email_addr` IN (?, ?) AND `nickname` IS NULL "
                      "AND `role` <> ? ORDER BY `display_name` ASC LIMIT ? OFFSET ?"),
                  stmt.text);
    CRPC_CHECK_EQ(std::size_t{6}, stmt.params.size());
    CRPC_CHECK_EQ(std::size_t{6}, placeholder_count(stmt.text));
}

CRPC_TEST(sql_select_dialects) {
    const crpc::orm::entity_meta* meta = crpc::orm::entity_metadata<account>();
    std::vector<crpc::orm::sql::condition> conditions;
    conditions.push_back(
        {0, crpc::orm::sql::comp::eq, {crpc::db::parameter::from_int64(1)}});

    crpc::orm::sql::statement tsql;
    CRPC_CHECK(crpc::orm::sql::build_select(*meta, crpc::orm::dialect::tsql, conditions,
                                            {}, false, 0, 0, tsql)
                   .ok());
    CRPC_CHECK(tsql.text.find("[accounts]") != std::string::npos);
    CRPC_CHECK(tsql.text.find("[id]") != std::string::npos);

    crpc::orm::sql::statement ansi;
    CRPC_CHECK(crpc::orm::sql::build_select(*meta, crpc::orm::dialect::ansi, conditions,
                                            {}, false, 0, 0, ansi)
                   .ok());
    CRPC_CHECK(ansi.text.find("\"accounts\"") != std::string::npos);

    // T-SQL has no LIMIT in this abstraction.
    crpc::orm::sql::statement limited;
    const crpc::status st = crpc::orm::sql::build_select(
        *meta, crpc::orm::dialect::tsql, conditions, {}, true, 5, 0, limited);
    CRPC_CHECK_FALSE(st.ok());
    CRPC_CHECK(crpc::orm::sql::dialect_supports_limit(crpc::orm::dialect::mysql));
    CRPC_CHECK_FALSE(crpc::orm::sql::dialect_supports_limit(crpc::orm::dialect::tsql));
}

CRPC_TEST(sql_last_insert_id_per_dialect) {
    crpc::orm::sql::statement mysql;
    CRPC_CHECK(crpc::orm::sql::build_last_insert_id(crpc::orm::dialect::mysql, mysql).ok());
    CRPC_CHECK_EQ(std::string("SELECT LAST_INSERT_ID()"), mysql.text);

    crpc::orm::sql::statement tsql;
    CRPC_CHECK(crpc::orm::sql::build_last_insert_id(crpc::orm::dialect::tsql, tsql).ok());
    CRPC_CHECK_EQ(std::string("SELECT SCOPE_IDENTITY()"), tsql.text);
}

CRPC_TEST(sql_key_compatibility) {
    CRPC_CHECK(crpc::orm::sql::check_key_compatible(
                   crpc::db::parameter::from_int64(3), crpc::db::db_type::int64)
                   .ok());
    CRPC_CHECK_FALSE(crpc::orm::sql::check_key_compatible(
                         crpc::db::parameter::from_int64(-3), crpc::db::db_type::uint64)
                         .ok());
    CRPC_CHECK_FALSE(crpc::orm::sql::check_key_compatible(
                         crpc::db::parameter::from_text("abc"), crpc::db::db_type::int64)
                         .ok());
    CRPC_CHECK(crpc::orm::sql::check_key_compatible(
                   crpc::db::parameter::from_text("abc"), crpc::db::db_type::text)
                   .ok());
    CRPC_CHECK(crpc::orm::sql::check_key_compatible(
                   crpc::db::parameter::from_uint64(5), crpc::db::db_type::uint64)
                   .ok());
}

CRPC_TEST_MAIN()