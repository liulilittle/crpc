// CRPC -- ORM entity metadata derivation.
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
// rtti` for the specialization, which must resolve to ::rtti, not to a nested
// copy hidden in an anonymous namespace.
enum class account_role_meta : std::int32_t { none = 0, admin = 1, user = 2 };

RTTI_DEFINE_ENUM(account_role_meta, none, admin, user);

struct account {
    std::int64_t id = 0;
    std::string email;
    std::string display_name;
    std::optional<std::int64_t> age;
    std::optional<std::string> nickname;
    account_role_meta role = account_role_meta::none;
};

CRPC_ORM_ENTITY(account, "accounts",
    CRPC_ORM_FIELD(id, CRPC_ORM_PK, CRPC_ORM_AUTO),
    CRPC_ORM_FIELD(email, CRPC_ORM_COLUMN("email_addr"), CRPC_ORM_NOT_NULL,
                   CRPC_ORM_LENGTH(128)),
    CRPC_ORM_FIELD(display_name),
    CRPC_ORM_FIELD(age, CRPC_ORM_NULLABLE),
    CRPC_ORM_FIELD(nickname),
    CRPC_ORM_FIELD(role));

CRPC_TEST(metadata_table_and_columns) {
    const crpc::orm::entity_meta* meta = crpc::orm::entity_metadata<account>();
    CRPC_CHECK(meta != nullptr);
    CRPC_CHECK(meta->valid);
    CRPC_CHECK_EQ(std::string("accounts"), meta->table);

    CRPC_CHECK_EQ(std::size_t{6}, meta->columns.size());

    CRPC_CHECK_EQ(std::string("id"), meta->columns[0].field);
    CRPC_CHECK_EQ(std::string("id"), meta->columns[0].column);
    CRPC_CHECK(meta->columns[0].primary_key);
    CRPC_CHECK(meta->columns[0].auto_increment);
    CRPC_CHECK_FALSE(meta->columns[0].optional_field);

    CRPC_CHECK_EQ(std::string("email"), meta->columns[1].field);
    CRPC_CHECK_EQ(std::string("email_addr"), meta->columns[1].column);
    CRPC_CHECK_FALSE(meta->columns[1].nullable);
    CRPC_CHECK_EQ(std::uint32_t{128}, meta->columns[1].length);

    CRPC_CHECK_EQ(std::string("display_name"), meta->columns[2].column);
    CRPC_CHECK(meta->columns[2].nullable);

    CRPC_CHECK(meta->columns[3].optional_field);
    CRPC_CHECK(meta->columns[3].nullable);

    CRPC_CHECK(meta->columns[4].optional_field);
}

CRPC_TEST(metadata_primary_key) {
    const crpc::orm::entity_meta* meta = crpc::orm::entity_metadata<account>();
    CRPC_CHECK_EQ(std::size_t{1}, meta->primary_key.size());
    CRPC_CHECK_EQ(std::size_t{0}, meta->primary_key[0]);
    CRPC_CHECK_EQ(std::size_t{0}, meta->auto_increment_index);
}

CRPC_TEST(metadata_field_types) {
    const crpc::orm::entity_meta* meta = crpc::orm::entity_metadata<account>();
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::int64),
                  static_cast<int>(meta->columns[0].type));
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::text),
                  static_cast<int>(meta->columns[1].type));
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::int64),
                  static_cast<int>(meta->columns[3].type));
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::text),
                  static_cast<int>(meta->columns[4].type));
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::int64),
                  static_cast<int>(meta->columns[5].type));
}

CRPC_TEST(metadata_member_index_lookup) {
    CRPC_CHECK_EQ(std::size_t{0}, crpc::orm::member_field_index<account>(&account::id));
    CRPC_CHECK_EQ(std::size_t{1}, crpc::orm::member_field_index<account>(&account::email));
    CRPC_CHECK_EQ(std::size_t{5}, crpc::orm::member_field_index<account>(&account::role));
    // A member of the wrong type must not be reported as present.
    CRPC_CHECK_EQ(crpc::orm::orm_npos,
                  crpc::orm::member_field_index<account>(static_cast<int account::*>(nullptr)));
}

CRPC_TEST(metadata_rejects_missing_table) {
    crpc::orm::column_meta column;
    column.index = 0;
    column.field = "id";
    column.column = "id";
    column.primary_key = true;

    crpc::orm::entity_meta meta;
    meta.columns.push_back(column);
    crpc::orm::finalize_entity_metadata(meta);
    CRPC_CHECK_FALSE(meta.valid);
    CRPC_CHECK(meta.error.find("table") != std::string::npos);
}

CRPC_TEST(metadata_rejects_auto_without_key) {
    crpc::orm::column_meta column;
    column.index = 0;
    column.field = "id";
    column.column = "id";
    column.auto_increment = true;

    crpc::orm::entity_meta meta;
    meta.table = "t";
    meta.columns.push_back(column);
    crpc::orm::finalize_entity_metadata(meta);
    CRPC_CHECK_FALSE(meta.valid);
}

CRPC_TEST(metadata_rejects_two_auto_increments) {
    crpc::orm::column_meta first;
    first.index = 0;
    first.field = "id";
    first.column = "id";
    first.primary_key = true;
    first.auto_increment = true;

    crpc::orm::column_meta second;
    second.index = 1;
    second.field = "other";
    second.column = "other";
    second.primary_key = true;
    second.auto_increment = true;

    crpc::orm::entity_meta meta;
    meta.table = "t";
    meta.columns.push_back(first);
    meta.columns.push_back(second);
    crpc::orm::finalize_entity_metadata(meta);
    CRPC_CHECK_FALSE(meta.valid);
}

CRPC_TEST(metadata_forward_unknown_attribute_is_ignored) {
    // An unknown attribute must be ignored, not a hard error.
    crpc::orm::column_meta column;
    column.index = 0;
    column.field = "id";
    column.column = "id";
    column.primary_key = true;

    const rtti::attr foreign = rtti::make_attr("generated_column", "virtual");
    crpc::orm::apply_column_attributes(column, crpc::span<const rtti::attr>(&foreign, 1));
    CRPC_CHECK_EQ(std::string("id"), column.column);
    CRPC_CHECK_FALSE(column.auto_increment);
}

CRPC_TEST_MAIN()