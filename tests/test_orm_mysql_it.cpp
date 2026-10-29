// CRPC -- ORM integration against a real MySQL 8.x server.
//
// Skipped unless CRPC_MYSQL_IT=1. These cases are the ones a fake connection
// cannot prove: an auto-increment id really comes from the server, an UPDATE
// really carries only the dirty columns (proven by a second connection writing
// a sentinel that the ORM's own transcript must not clobber), and an injection
// string really stays a bound value.
//
// Connection details come from the environment (same defaults as the MySQL IT
// suite): CRPC_MYSQL_IT=1, CRPC_MYSQL_HOST (127.0.0.1), CRPC_MYSQL_PORT (3306),
// CRPC_MYSQL_USER (crpc), CRPC_MYSQL_PASSWORD (crpc_pw), CRPC_MYSQL_DB (crpc_it).
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "test_framework.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <crpc/db/client.hpp>
#include <crpc/db/mysql/connection.hpp>
#include <crpc/orm/orm.hpp>
#include "test_tcp_backend.hpp"

// Entities are registered at global scope: RTTI_DEFINE_TYPE opens `namespace
// rtti` for the specialization, which must resolve to ::rtti.
enum class orm_it_role : std::int32_t { none = 0, admin = 1, user = 2 };

RTTI_DEFINE_ENUM(orm_it_role, none, admin, user);

struct orm_account {
    std::int64_t id = 0;
    std::string email;
    std::string display_name;
    std::optional<std::int64_t> age;
    std::optional<std::string> nickname;
    orm_it_role role = orm_it_role::none;
    std::chrono::system_clock::time_point created_at{};
    std::string note;
};

CRPC_ORM_ENTITY(orm_account, "orm_accounts",
    CRPC_ORM_FIELD(id, CRPC_ORM_PK, CRPC_ORM_AUTO),
    CRPC_ORM_FIELD(email, CRPC_ORM_NOT_NULL, CRPC_ORM_LENGTH(128)),
    CRPC_ORM_FIELD(display_name, CRPC_ORM_NOT_NULL),
    CRPC_ORM_FIELD(age, CRPC_ORM_NULLABLE),
    CRPC_ORM_FIELD(nickname, CRPC_ORM_NULLABLE),
    CRPC_ORM_FIELD(role, CRPC_ORM_NOT_NULL),
    CRPC_ORM_FIELD(created_at, CRPC_ORM_NOT_NULL),
    CRPC_ORM_FIELD(note, CRPC_ORM_NOT_NULL));

namespace {

[[nodiscard]] std::string env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::string(value) : std::string(fallback);
}

[[nodiscard]] bool it_enabled() {
    const char* value = std::getenv("CRPC_MYSQL_IT");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

const char* const k_create =
    "CREATE TABLE orm_accounts ("
    " id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
    " email VARCHAR(128) NOT NULL,"
    " display_name VARCHAR(255) NOT NULL,"
    " age BIGINT NULL,"
    " nickname VARCHAR(255) NULL,"
    " role TINYINT NOT NULL,"
    " created_at DATETIME NOT NULL,"
    " note TEXT NOT NULL"
    ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4";

struct it_fixture {
    tcp_backend backend;
    std::unique_ptr<crpc::db::mysql::connection_factory> factory;
    std::unique_ptr<crpc::db::pool> session_pool;
    std::unique_ptr<crpc::db::client> raw;

    it_fixture() {
        crpc::db::mysql::connection_options options;
        options.user = env_or("CRPC_MYSQL_USER", "crpc");
        options.password = env_or("CRPC_MYSQL_PASSWORD", "crpc_pw");
        options.database = env_or("CRPC_MYSQL_DB", "crpc_it");
        const std::string host = env_or("CRPC_MYSQL_HOST", "127.0.0.1");
        const std::string port = env_or("CRPC_MYSQL_PORT", "3306");
        factory.reset(new crpc::db::mysql::connection_factory(
            options, backend,
            crpc::endpoint::tcp(host, static_cast<std::uint16_t>(std::stoi(port)))));

        crpc::db::pool_options pool_options;
        pool_options.max_size = 1;
        pool_options.validate_on_borrow = false;
        session_pool.reset(new crpc::db::pool(pool_options, *factory));

        crpc::db::client_options client_options;
        client_options.pool.max_size = 1;
        client_options.pool.validate_on_borrow = false;
        raw.reset(new crpc::db::client(*factory, client_options));
    }

    [[nodiscard]] bool usable() const {
        auto probe = raw->query("SELECT 1");
        const bool ok = probe.ok();
        if (ok) {
            delete probe.value();
        }
        return ok;
    }

    void reset_table() {
        (void)raw->execute("DROP TABLE IF EXISTS orm_accounts");
        const crpc::status created = raw->execute(k_create);
        if (!created.ok()) {
            std::printf("  create table error: %s\n", created.message().c_str());
        }
    }
};

// The value the ORM must not clobber: written on a second connection while the
// tracked entity is held, so an UPDATE that mentioned `note` would overwrite it.
const char* const k_sentinel = "written-by-another-connection";

[[nodiscard]] std::chrono::system_clock::time_point fixed_time() {
    std::chrono::system_clock::time_point point;
    (void)crpc::orm::values::parse_system_time("2026-03-08 15:04:05", point);
    return point;
}

} // namespace

CRPC_TEST(orm_mysql_it_insert_find_update_delete) {
    if (!it_enabled()) {
        crpc_test::skip("CRPC_MYSQL_IT is not 1");
    }
    it_fixture fixture;
    if (!fixture.usable()) {
        crpc_test::skip("mysql server is not reachable");
    }
    fixture.reset_table();

    auto open = crpc::orm::session::open(*fixture.session_pool);
    CRPC_CHECK(open.ok());
    if (!open.ok()) {
        return;
    }
    crpc::orm::session& ses = open.value();

    // --- insert, get the server-generated id --------------------------------
    orm_account row;
    row.email = "carol@example.com";
    row.display_name = "carol";
    row.role = orm_it_role::user;
    row.created_at = fixed_time();
    row.note = "hello";
    crpc::orm::entity_ref<orm_account> ref = ses.add<orm_account>(row);
    CRPC_CHECK(ses.commit().ok());
    CRPC_CHECK(ref->id > 0);
    CRPC_CHECK(ref.state() == crpc::orm::entity_state::clean);

    const std::int64_t id = ref->id;

    // --- the row really exists with the mapped values ------------------------
    {
        std::vector<crpc::db::parameter> params;
        params.push_back(crpc::db::parameter::from_int64(id));
        auto rows = fixture.raw->query(
            "SELECT email, display_name, age, nickname, role, created_at, note "
            "FROM orm_accounts WHERE id = ?",
            params);
        CRPC_CHECK(rows.ok());
        if (rows.ok()) {
            std::unique_ptr<crpc::db::result_set> guard(rows.value());
            CRPC_CHECK(guard->next());
            CRPC_CHECK_EQ(std::string("carol@example.com"),
                          std::string(guard->get_text(0).value()));
            CRPC_CHECK_EQ(std::string("carol"),
                          std::string(guard->get_text(1).value()));
            CRPC_CHECK(guard->is_null(2));
            CRPC_CHECK(guard->is_null(3));
            CRPC_CHECK_EQ(std::int64_t{2}, guard->get_int64(4).value());
            CRPC_CHECK_EQ(std::string("2026-03-08 15:04:05"),
                          std::string(guard->get_text(5).value()));
        }
    }

    // --- find maps the row back into a struct --------------------------------
    auto found = ses.find<orm_account>(id);
    CRPC_CHECK(found.ok());
    if (found.ok()) {
        CRPC_CHECK_EQ(id, found.value()->id);
        CRPC_CHECK_EQ(std::string("carol@example.com"), found.value()->email);
        CRPC_CHECK_FALSE(found.value()->age.has_value());
        CRPC_CHECK_FALSE(found.value()->nickname.has_value());
        CRPC_CHECK(found.value()->role == orm_it_role::user);
        CRPC_CHECK_EQ(std::string("2026-03-08 15:04:05"),
                      crpc::orm::values::format_system_time(found.value()->created_at));
    }

    // --- edit several fields, commit, verify ---------------------------------
    CRPC_CHECK(found.ok());
    if (found.ok()) {
        found.value()->display_name = "carol-reloaded";
        found.value()->age = 34;
        found.value()->nickname = "cj";
        CRPC_CHECK(ses.commit().ok());

        std::vector<crpc::db::parameter> params;
        params.push_back(crpc::db::parameter::from_int64(id));
        auto rows = fixture.raw->query(
            "SELECT display_name, age, nickname FROM orm_accounts WHERE id = ?", params);
        CRPC_CHECK(rows.ok());
        if (rows.ok()) {
            std::unique_ptr<crpc::db::result_set> guard(rows.value());
            CRPC_CHECK(guard->next());
            CRPC_CHECK_EQ(std::string("carol-reloaded"),
                          std::string(guard->get_text(0).value()));
            CRPC_CHECK_EQ(std::int64_t{34}, guard->get_int64(1).value());
            CRPC_CHECK_EQ(std::string("cj"),
                          std::string(guard->get_text(2).value()));
        }
    }

    // --- delete --------------------------------------------------------------
    CRPC_CHECK(found.ok());
    if (found.ok()) {
        CRPC_CHECK(ses.erase(found.value()).ok());
        CRPC_CHECK(ses.commit().ok());
        std::vector<crpc::db::parameter> params;
        params.push_back(crpc::db::parameter::from_int64(id));
        auto rows = fixture.raw->query("SELECT COUNT(*) FROM orm_accounts WHERE id = ?",
                                       params);
        CRPC_CHECK(rows.ok());
        if (rows.ok()) {
            std::unique_ptr<crpc::db::result_set> guard(rows.value());
            CRPC_CHECK(guard->next());
            CRPC_CHECK_EQ(std::int64_t{0}, guard->get_int64(0).value());
        }
    }
}

CRPC_TEST(orm_mysql_it_update_only_dirty_columns) {
    if (!it_enabled()) {
        crpc_test::skip("CRPC_MYSQL_IT is not 1");
    }
    it_fixture fixture;
    if (!fixture.usable()) {
        crpc_test::skip("mysql server is not reachable");
    }
    fixture.reset_table();

    auto open = crpc::orm::session::open(*fixture.session_pool);
    CRPC_CHECK(open.ok());
    if (!open.ok()) {
        return;
    }
    crpc::orm::session& ses = open.value();

    orm_account row;
    row.email = "dirty@example.com";
    row.display_name = "before";
    row.role = orm_it_role::admin;
    row.created_at = fixed_time();
    row.note = "original-note";
    crpc::orm::entity_ref<orm_account> ref = ses.add<orm_account>(row);
    CRPC_CHECK(ses.commit().ok());
    const std::int64_t id = ref->id;

    // The ORM holds a tracked copy of the row.
    auto found = ses.find<orm_account>(id);
    CRPC_CHECK(found.ok());
    if (!found.ok()) {
        return;
    }

    // A second connection modifies `note` behind the ORM's back.
    {
        std::vector<crpc::db::parameter> params;
        params.push_back(crpc::db::parameter::from_text(k_sentinel));
        params.push_back(crpc::db::parameter::from_int64(id));
        CRPC_CHECK(fixture.raw->execute(
                       "UPDATE orm_accounts SET note = ? WHERE id = ?", params)
                       .ok());
    }

    // The ORM updates one column, `display_name`.
    found.value()->display_name = "after";
    CRPC_CHECK(ses.commit().ok());

    // Transcript evidence: the UPDATE's SET list names exactly `display_name`.
    // commit_log reflects the statements of the last commit only.
    bool saw_update = false;
    bool update_mentions_only_display_name = false;
    for (const std::string& statement : ses.commit_log()) {
        if (statement.rfind("UPDATE", 0) == 0) {
            saw_update = true;
            update_mentions_only_display_name =
                statement.find("`display_name` = ?") != std::string::npos &&
                statement.find("`note`") == std::string::npos &&
                statement.find("`email_addr`") == std::string::npos;
        }
    }
    CRPC_CHECK(saw_update);
    CRPC_CHECK(update_mentions_only_display_name);
    CRPC_CHECK_FALSE(ses.commit_log().empty());

    // Value evidence: the sentinel survived, so `note` was never written.
    {
        std::vector<crpc::db::parameter> params;
        params.push_back(crpc::db::parameter::from_int64(id));
        auto rows = fixture.raw->query(
            "SELECT display_name, note FROM orm_accounts WHERE id = ?", params);
        CRPC_CHECK(rows.ok());
        if (rows.ok()) {
            std::unique_ptr<crpc::db::result_set> guard(rows.value());
            CRPC_CHECK(guard->next());
            CRPC_CHECK_EQ(std::string("after"),
                          std::string(guard->get_text(0).value()));
            CRPC_CHECK_EQ(std::string(k_sentinel),
                          std::string(guard->get_text(1).value()));
        }
    }
}

CRPC_TEST(orm_mysql_it_injection_binds_values) {
    if (!it_enabled()) {
        crpc_test::skip("CRPC_MYSQL_IT is not 1");
    }
    it_fixture fixture;
    if (!fixture.usable()) {
        crpc_test::skip("mysql server is not reachable");
    }
    fixture.reset_table();

    auto open = crpc::orm::session::open(*fixture.session_pool);
    CRPC_CHECK(open.ok());
    if (!open.ok()) {
        std::printf("  session open error: %s\n", open.error().message.c_str());
        return;
    }
    crpc::orm::session& ses = open.value();

    // A value engineered to turn `WHERE display_name = '...'` into a tautology
    // if it were spliced into the text.
    const std::string payload = "x' OR 1=1 --";

    orm_account row;
    row.email = "evil@example.com";
    row.display_name = payload;
    row.role = orm_it_role::user;
    row.created_at = fixed_time();
    row.note = "bound value";
    crpc::orm::entity_ref<orm_account> ref = ses.add<orm_account>(row);
    CRPC_CHECK(ses.commit().ok());
    CRPC_CHECK(ref->id > 0);

    // The INSERT and the lookup both have to travel as parameters.
    std::vector<crpc::db::parameter> params;
    params.push_back(crpc::db::parameter::from_text(payload));
    auto rows = fixture.raw->query(
        "SELECT COUNT(*) FROM orm_accounts WHERE display_name = ?", params);
    CRPC_CHECK(rows.ok());
    if (rows.ok()) {
        std::unique_ptr<crpc::db::result_set> guard(rows.value());
        CRPC_CHECK(guard->next());
        // Exactly the one injected row, not every row in the table.
        CRPC_CHECK_EQ(std::int64_t{1}, guard->get_int64(0).value());
    }

    // The ORM's own query builder binds the same value.
    auto matches = ses.query<orm_account>()
                       .where_eq(&orm_account::display_name, payload)
                       .list();
    CRPC_CHECK(matches.ok());
    if (matches.ok()) {
        CRPC_CHECK_EQ(std::size_t{1}, matches.value().size());
        if (!matches.value().empty()) {
            CRPC_CHECK_EQ(payload, matches.value().front().display_name);
        }
    }

    // And the total row count is still one: the condition never widened.
    auto total = fixture.raw->query("SELECT COUNT(*) FROM orm_accounts");
    CRPC_CHECK(total.ok());
    if (total.ok()) {
        std::unique_ptr<crpc::db::result_set> guard(total.value());
        CRPC_CHECK(guard->next());
        CRPC_CHECK_EQ(std::int64_t{1}, guard->get_int64(0).value());
    }
}

CRPC_TEST(orm_mysql_it_query_builder_and_optional) {
    if (!it_enabled()) {
        crpc_test::skip("CRPC_MYSQL_IT is not 1");
    }
    it_fixture fixture;
    if (!fixture.usable()) {
        crpc_test::skip("mysql server is not reachable");
    }
    fixture.reset_table();

    auto open = crpc::orm::session::open(*fixture.session_pool);
    CRPC_CHECK(open.ok());
    if (!open.ok()) {
        return;
    }
    crpc::orm::session& ses = open.value();

    const auto now = fixed_time();
    for (int i = 0; i < 3; ++i) {
        orm_account row;
        row.email = "n" + std::to_string(i) + "@example.com";
        row.display_name = "name" + std::to_string(i);
        row.age = i == 1 ? std::optional<std::int64_t>(30) : std::nullopt;
        row.nickname = i == 2 ? std::optional<std::string>("two") : std::nullopt;
        row.role = i == 0 ? orm_it_role::admin : orm_it_role::user;
        row.created_at = now;
        row.note = "note";
        (void)ses.add<orm_account>(row);
    }
    CRPC_CHECK(ses.commit().ok());

    auto rows = ses.query<orm_account>()
                    .where_in(&orm_account::display_name,
                              {std::string("name0"), std::string("name2")})
                    .order_by(&orm_account::id, false) // ascending id
                    .list();
    CRPC_CHECK(rows.ok());
    if (rows.ok()) {
        CRPC_CHECK_EQ(std::size_t{2}, rows.value().size());
        if (rows.value().size() == 2) {
            CRPC_CHECK_EQ(std::string("name0"), rows.value()[0].display_name);
            CRPC_CHECK_EQ(std::string("name2"), rows.value()[1].display_name);
            // The row with the nickname ("two") round-tripped; the other had
            // no nickname at all.
            CRPC_CHECK_FALSE(rows.value()[0].nickname.has_value());
            CRPC_CHECK(rows.value()[1].nickname.has_value());
            CRPC_CHECK_EQ(std::string("two"), *rows.value()[1].nickname);
        }
    }

    auto only_admin = ses.query<orm_account>()
                          .where_eq(&orm_account::role, orm_it_role::admin)
                          .list();
    CRPC_CHECK(only_admin.ok());
    if (only_admin.ok()) {
        CRPC_CHECK_EQ(std::size_t{1}, only_admin.value().size());
    }

    auto count = ses.query<orm_account>().where_null(&orm_account::age).count();
    CRPC_CHECK(count.ok());
    if (count.ok()) {
        CRPC_CHECK_EQ(std::int64_t{2}, count.value());
    }
}

CRPC_TEST_MAIN()