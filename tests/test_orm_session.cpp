// CRPC -- ORM unit of work over a fake connection.
//
// These cases prove the change-tracking contract without a server: INSERT
// excludes the auto-increment column, a modified entity emits one UPDATE whose
// SET list holds only dirty columns, an untouched entity emits nothing, several
// edits to one entity still emit one UPDATE, and DELETE is keyed on the primary
// key. The real-server counterpart lives in test_orm_mysql_it.cpp.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "test_framework.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/db/connection.hpp>
#include <crpc/db/memory_result_set.hpp>
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

namespace {

struct fake_state {
    std::vector<std::string> statements;
    std::int64_t next_id = 100;
    std::int64_t row_id = 1;
    std::string row_email = "alice@example.com";
    std::string row_display_name = "alice";
    bool in_tx = false;
    bool open = true;
};

crpc::db::column_info column_of(std::string name, crpc::db::db_type type) {
    crpc::db::column_info column;
    column.name = std::move(name);
    column.type = type;
    return column;
}

class fake_connection final : public crpc::db::connection {
public:
    explicit fake_connection(std::shared_ptr<fake_state> state) : state_(std::move(state)) {}

    crpc::status ping() override { return crpc::status{}; }
    bool in_transaction() const noexcept override { return state_->in_tx; }
    crpc::status begin() override {
        state_->in_tx = true;
        return crpc::status{};
    }
    crpc::status commit() override {
        if (!state_->in_tx) {
            return crpc::status{crpc::rpc_code::failed_precondition};
        }
        state_->in_tx = false;
        return crpc::status{};
    }
    crpc::status rollback() override {
        state_->in_tx = false;
        return crpc::status{};
    }

    crpc::status query(std::string_view sql, crpc::span<const crpc::db::parameter> params,
                       crpc::db::result_set** out) override {
        (void)params;
        const std::string statement(sql.data(), sql.size());
        state_->statements.push_back(statement);
        if (out != nullptr) {
            *out = respond(statement);
        }
        return crpc::status{};
    }

    void close() noexcept override { state_->open = false; }
    bool is_open() const noexcept override { return state_->open; }

private:
    crpc::db::result_set* respond(const std::string& sql) {
        if (sql == "SELECT LAST_INSERT_ID()") {
            auto* set = new crpc::db::memory_result_set(
                {column_of("LAST_INSERT_ID()", crpc::db::db_type::int64)}, 1);
            set->add_row({std::to_string(state_->next_id)}, {false});
            return set;
        }
        if (sql.rfind("SELECT", 0) == 0) {
            return make_account_row();
        }
        return new crpc::db::memory_result_set({}, 1);
    }

    crpc::db::result_set* make_account_row() const {
        const crpc::orm::entity_meta* meta = crpc::orm::entity_metadata<account>();
        std::vector<crpc::db::column_info> columns;
        columns.reserve(meta->columns.size());
        for (const crpc::orm::column_meta& column : meta->columns) {
            crpc::db::column_info info;
            info.name = column.column;
            info.type = column.type;
            info.nullable = column.nullable;
            columns.push_back(std::move(info));
        }
        auto* set = new crpc::db::memory_result_set(std::move(columns), 1);
        // Entity column order: id, email_addr, display_name, age, nickname, role.
        set->add_row({std::to_string(state_->row_id), state_->row_email,
                      state_->row_display_name, "", "", "2"},
                     {false, false, false, true, true, false});
        return set;
    }

    std::shared_ptr<fake_state> state_;
};

class fake_factory final : public crpc::db::connection_factory {
public:
    explicit fake_factory(std::shared_ptr<fake_state> state) : state_(std::move(state)) {}

    crpc::result<crpc::db::connection*> open() override {
        return static_cast<crpc::db::connection*>(new fake_connection(state_));
    }
    std::string_view name() const noexcept override { return "orm-fake"; }

private:
    std::shared_ptr<fake_state> state_;
};

crpc::db::pool_options fake_pool_options() {
    crpc::db::pool_options options;
    options.min_size = 0;
    options.max_size = 1;
    options.max_idle = 1;
    options.validate_on_borrow = false;
    return options;
}

// The statement most recently issued that starts with `prefix`, or nullptr.
const std::string* last_statement_with(const fake_state& state, const char* prefix) {
    for (auto it = state.statements.rbegin(); it != state.statements.rend(); ++it) {
        if (it->rfind(prefix, 0) == 0) {
            return &*it;
        }
    }
    return nullptr;
}

std::size_t count_statements_with(const fake_state& state, const char* prefix) {
    std::size_t count = 0;
    for (const std::string& statement : state.statements) {
        if (statement.rfind(prefix, 0) == 0) {
            ++count;
        }
    }
    return count;
}

} // namespace

CRPC_TEST(session_insert_assigns_auto_increment) {
    auto state = std::make_shared<fake_state>();
    fake_factory factory(state);
    crpc::db::pool pool(fake_pool_options(), factory);
    auto opened = crpc::orm::session::open(pool);
    CRPC_CHECK(opened.ok());
    if (!opened.ok()) {
        return;
    }
    crpc::orm::session& ses = opened.value();

    account row;
    row.email = "new@example.com";
    row.display_name = "new";
    row.role = account_role::user;
    crpc::orm::entity_ref<account> ref = ses.add<account>(row);
    CRPC_CHECK(ref.state() == crpc::orm::entity_state::inserted);

    CRPC_CHECK(ses.commit().ok());
    CRPC_CHECK(ref.state() == crpc::orm::entity_state::clean);
    CRPC_CHECK_EQ(std::int64_t{100}, ref->id);

    const std::string* insert = last_statement_with(*state, "INSERT");
    CRPC_CHECK(insert != nullptr);
    if (insert != nullptr) {
        CRPC_CHECK(insert->find("`id`") == std::string::npos);
        CRPC_CHECK(insert->find("`email_addr`") != std::string::npos);
    }
    CRPC_CHECK(last_statement_with(*state, "SELECT LAST_INSERT_ID()") != nullptr);
}

CRPC_TEST(session_update_only_dirty_columns) {
    auto state = std::make_shared<fake_state>();
    fake_factory factory(state);
    crpc::db::pool pool(fake_pool_options(), factory);
    auto opened = crpc::orm::session::open(pool);
    CRPC_CHECK(opened.ok());
    if (!opened.ok()) {
        return;
    }
    crpc::orm::session& ses = opened.value();

    auto found = ses.find<account>(std::int64_t{1});
    CRPC_CHECK(found.ok());
    if (!found.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::int64_t{1}, found.value()->id);
    CRPC_CHECK_EQ(std::string("alice"), found.value()->display_name);

    found.value()->display_name = "changed";

    const std::size_t before = state->statements.size();
    CRPC_CHECK(ses.commit().ok());
    // Only one DML statement was added.
    CRPC_CHECK_EQ(before + 1, state->statements.size());

    const std::string* update = last_statement_with(*state, "UPDATE");
    CRPC_CHECK(update != nullptr);
    if (update != nullptr) {
        CRPC_CHECK(update->find("SET `display_name` = ?") != std::string::npos);
        CRPC_CHECK(update->find("`email_addr`") == std::string::npos);
        CRPC_CHECK(update->find("WHERE `id` = ?") != std::string::npos);
        CRPC_CHECK(update->find("`role`") == std::string::npos);
    }
}

CRPC_TEST(session_unchanged_entity_emits_nothing) {
    auto state = std::make_shared<fake_state>();
    fake_factory factory(state);
    crpc::db::pool pool(fake_pool_options(), factory);
    auto opened = crpc::orm::session::open(pool);
    CRPC_CHECK(opened.ok());
    if (!opened.ok()) {
        return;
    }
    crpc::orm::session& ses = opened.value();

    auto found = ses.find<account>(std::int64_t{1});
    CRPC_CHECK(found.ok());
    if (!found.ok()) {
        return;
    }
    const std::size_t before = state->statements.size();
    CRPC_CHECK(ses.commit().ok());
    CRPC_CHECK_EQ(before, state->statements.size());
}

CRPC_TEST(session_repeated_edits_single_update) {
    auto state = std::make_shared<fake_state>();
    fake_factory factory(state);
    crpc::db::pool pool(fake_pool_options(), factory);
    auto opened = crpc::orm::session::open(pool);
    CRPC_CHECK(opened.ok());
    if (!opened.ok()) {
        return;
    }
    crpc::orm::session& ses = opened.value();

    auto found = ses.find<account>(std::int64_t{1});
    CRPC_CHECK(found.ok());
    if (!found.ok()) {
        return;
    }
    found.value()->email = "step1@example.com";
    found.value()->email = "step2@example.com";
    found.value()->display_name = "final";

    const std::size_t before = state->statements.size();
    CRPC_CHECK(ses.commit().ok());
    CRPC_CHECK_EQ(std::size_t{1}, count_statements_with(*state, "UPDATE"));

    const std::string* update = last_statement_with(*state, "UPDATE");
    CRPC_CHECK(update != nullptr);
    if (update != nullptr) {
        CRPC_CHECK(update->find("`email_addr`") != std::string::npos);
        CRPC_CHECK(update->find("`display_name`") != std::string::npos);
        CRPC_CHECK(update->find("`nickname`") == std::string::npos);
    }
    CRPC_CHECK_EQ(before + 1, state->statements.size());
}

CRPC_TEST(session_delete_by_primary_key) {
    auto state = std::make_shared<fake_state>();
    fake_factory factory(state);
    crpc::db::pool pool(fake_pool_options(), factory);
    auto opened = crpc::orm::session::open(pool);
    CRPC_CHECK(opened.ok());
    if (!opened.ok()) {
        return;
    }
    crpc::orm::session& ses = opened.value();

    auto found = ses.find<account>(std::int64_t{1});
    CRPC_CHECK(found.ok());
    if (!found.ok()) {
        return;
    }
    CRPC_CHECK(ses.erase(found.value()).ok());
    CRPC_CHECK(found.value().state() == crpc::orm::entity_state::deleted);
    CRPC_CHECK(ses.commit().ok());

    const std::string* remove = last_statement_with(*state, "DELETE");
    CRPC_CHECK(remove != nullptr);
    if (remove != nullptr) {
        CRPC_CHECK_EQ(std::string("DELETE FROM `accounts` WHERE `id` = ?"), *remove);
    }
}

CRPC_TEST(session_add_then_edit_single_insert) {
    auto state = std::make_shared<fake_state>();
    fake_factory factory(state);
    crpc::db::pool pool(fake_pool_options(), factory);
    auto opened = crpc::orm::session::open(pool);
    CRPC_CHECK(opened.ok());
    if (!opened.ok()) {
        return;
    }
    crpc::orm::session& ses = opened.value();

    account row;
    row.email = "brand@example.com";
    row.display_name = "brand";
    crpc::orm::entity_ref<account> ref = ses.add<account>(row);
    ref->display_name = "renamed";

    CRPC_CHECK(ses.commit().ok());
    CRPC_CHECK_EQ(std::size_t{1}, count_statements_with(*state, "INSERT"));
    CRPC_CHECK_EQ(std::size_t{0}, count_statements_with(*state, "UPDATE"));
}

CRPC_TEST(session_rollback_discards_changes) {
    auto state = std::make_shared<fake_state>();
    fake_factory factory(state);
    crpc::db::pool pool(fake_pool_options(), factory);
    auto opened = crpc::orm::session::open(pool);
    CRPC_CHECK(opened.ok());
    if (!opened.ok()) {
        return;
    }
    crpc::orm::session& ses = opened.value();

    auto found = ses.find<account>(std::int64_t{1});
    CRPC_CHECK(found.ok());
    if (!found.ok()) {
        return;
    }
    found.value()->display_name = "temp";
    CRPC_CHECK(ses.rollback().ok());
    CRPC_CHECK_EQ(std::string("alice"), found.value()->display_name);

    account row;
    row.email = "dropped@example.com";
    (void)ses.add<account>(row);
    CRPC_CHECK(ses.rollback().ok());

    const std::size_t before = state->statements.size();
    CRPC_CHECK(ses.commit().ok());
    CRPC_CHECK_EQ(before, state->statements.size());
}

CRPC_TEST(session_query_builder) {
    auto state = std::make_shared<fake_state>();
    fake_factory factory(state);
    crpc::db::pool pool(fake_pool_options(), factory);
    auto opened = crpc::orm::session::open(pool);
    CRPC_CHECK(opened.ok());
    if (!opened.ok()) {
        return;
    }
    crpc::orm::session& ses = opened.value();

    auto rows = ses.query<account>()
                    .where_eq(&account::display_name, std::string("alice"))
                    .where_null(&account::age)
                    .order_by(&account::id, true)
                    .limit(5)
                    .list();
    CRPC_CHECK(rows.ok());
    if (rows.ok()) {
        CRPC_CHECK_EQ(std::size_t{1}, rows.value().size());
        if (!rows.value().empty()) {
            CRPC_CHECK_EQ(std::string("alice"), rows.value().front().display_name);
        }
    }

    const std::string* select = last_statement_with(*state, "SELECT");
    CRPC_CHECK(select != nullptr);
    if (select != nullptr) {
        CRPC_CHECK(select->find("WHERE `display_name` = ? AND `age` IS NULL") !=
                   std::string::npos);
        CRPC_CHECK(select->find("ORDER BY `id` DESC") != std::string::npos);
        CRPC_CHECK(select->find("LIMIT ? OFFSET ?") != std::string::npos);
    }
}

CRPC_TEST_MAIN()