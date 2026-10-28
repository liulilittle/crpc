// CRPC -- MySQL integration against a real server.
//
// Skipped unless CRPC_MYSQL_IT=1: a unit suite that requires a database on every
// developer machine is a suite people disable. The cases are the ones that cannot
// be proven with a scripted channel — a real handshake, a real prepared statement,
// a real stored procedure, and a real bulk load.
//
// Connection details come from the environment:
//   CRPC_MYSQL_IT=1
//   CRPC_MYSQL_HOST (default 127.0.0.1)
//   CRPC_MYSQL_PORT (default 3306)
//   CRPC_MYSQL_USER (default crpc)
//   CRPC_MYSQL_PASSWORD (default crpc_pw)
//   CRPC_MYSQL_DB (default crpc_it)
// The harness reads environment variables with std::getenv; MSVC's secure-CRT
// deprecation warning is noise for a test binary.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/db/channel.hpp>
#include <crpc/db/client.hpp>
#include <crpc/db/connection.hpp>
#include <crpc/db/mysql/connection.hpp>
#include <crpc/db/types.hpp>
#include <crpc/error.hpp>
#include <crpc/io/iocp_backend.hpp>

namespace {

[[nodiscard]] std::string env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::string(value) : std::string(fallback);
}

[[nodiscard]] bool it_enabled() {
    const char* value = std::getenv("CRPC_MYSQL_IT");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

struct it_session {
    crpc::iocp_backend backend;
    std::unique_ptr<crpc::db::mysql::connection_factory> factory;
    std::unique_ptr<crpc::db::client> db;

    it_session() {
        crpc::db::mysql::connection_options options;
        options.user = env_or("CRPC_MYSQL_USER", "crpc");
        options.password = env_or("CRPC_MYSQL_PASSWORD", "crpc_pw");
        options.database = env_or("CRPC_MYSQL_DB", "crpc_it");
        const std::string host = env_or("CRPC_MYSQL_HOST", "127.0.0.1");
        const std::string port = env_or("CRPC_MYSQL_PORT", "3306");
        factory.reset(new crpc::db::mysql::connection_factory(
            options, backend, crpc::endpoint::tcp(host, static_cast<std::uint16_t>(std::stoi(port)))));

        crpc::db::client_options client_options;
        client_options.pool.max_size = 2;
        client_options.pool.validate_on_borrow = false;
        db.reset(new crpc::db::client(*factory, client_options));
    }

    [[nodiscard]] bool usable() const { return db != nullptr; }
};

// Every case shares the same guard: no server, no run.
[[nodiscard]] const char* require_server() {
    if (!it_enabled()) {
        crpc_test::skip("CRPC_MYSQL_IT is not 1");
    }
    return "";
}

} // namespace

CRPC_TEST(mysql_it_select_one) {
    (void)require_server();
    it_session session;
    auto rows = session.db->query("SELECT 1");
    CRPC_CHECK(rows.ok());
    if (!rows.ok()) {
        std::printf("  server error: %s\n", rows.error().message.c_str());
        return;
    }
    std::unique_ptr<crpc::db::result_set> guard(rows.value());
    CRPC_CHECK(guard != nullptr);
    if (guard != nullptr) {
        CRPC_CHECK(guard->next());
        CRPC_CHECK_EQ(static_cast<std::int64_t>(1), guard->get_int64(0).value());
    }
}

CRPC_TEST(mysql_it_prepared_insert_and_select) {
    (void)require_server();
    it_session session;
    CRPC_CHECK(session.db->execute("DROP TABLE IF EXISTS it_users").ok());
    CRPC_CHECK(session.db
                   ->execute("CREATE TABLE it_users (id BIGINT PRIMARY KEY, name VARCHAR(64), "
                             "amount DECIMAL(10,2), note TEXT)")
                   .ok());

    std::vector<crpc::db::parameter> insert;
    insert.push_back(crpc::db::parameter::from_int64(1));
    insert.push_back(crpc::db::parameter::from_text("alice"));
    insert.push_back(crpc::db::parameter::from_text("12.50"));
    insert.push_back(crpc::db::parameter::null());
    const crpc::status inserted =
        session.db->execute("INSERT INTO it_users (id, name, amount, note) VALUES (?, ?, ?, ?)",
                            insert);
    CRPC_CHECK(inserted.ok());
    if (!inserted.ok()) {
        std::printf("  insert error: %s\n", inserted.message().c_str());
        return;
    }

    std::vector<crpc::db::parameter> where;
    where.push_back(crpc::db::parameter::from_int64(1));
    auto rows = session.db->query("SELECT id, name, amount, note FROM it_users WHERE id = ?", where);
    CRPC_CHECK(rows.ok());
    if (!rows.ok()) {
        return;
    }
    std::unique_ptr<crpc::db::result_set> guard(rows.value());
    CRPC_CHECK(guard != nullptr);
    if (guard != nullptr) {
        CRPC_CHECK(guard->next());
        CRPC_CHECK_EQ(static_cast<std::int64_t>(1), guard->get_int64(0).value());
        CRPC_CHECK_EQ(std::string("alice"), std::string(guard->get_text(1).value()));
        // A DECIMAL is carried as text so no digits are lost.
        CRPC_CHECK_EQ(std::string("12.50"), std::string(guard->get_text(2).value()));
        // A NULL is reported as NULL, not as an empty string.
        CRPC_CHECK(guard->is_null(3));
        // Column metadata reaches the caller for ORM mapping.
        CRPC_CHECK_EQ(std::string("id"), guard->column(0).name);
        CRPC_CHECK_FALSE(guard->column(0).nullable);
    }
    CRPC_CHECK(session.db->execute("DROP TABLE IF EXISTS it_users").ok());
}

CRPC_TEST(mysql_it_stored_procedure_out_parameter) {
    (void)require_server();
    it_session session;
    CRPC_CHECK(session.db->execute("DROP PROCEDURE IF EXISTS it_double").ok());
    const crpc::status created = session.db->execute(
        "CREATE PROCEDURE it_double(IN input_value BIGINT, OUT doubled BIGINT) "
        "BEGIN SET doubled = input_value * 2; END");
    CRPC_CHECK(created.ok());
    if (!created.ok()) {
        std::printf("  create procedure error: %s\n", created.message().c_str());
        return;
    }

    std::vector<crpc::db::parameter> params;
    params.push_back(crpc::db::parameter::from_int64(21));
    params.push_back(crpc::db::parameter::out("doubled", crpc::db::db_type::int64));

// A CALL with an OUT parameter must go through `call()`: `query()` would bind the
    // output slot as a value nobody reads.
    auto rows = session.db->call("CALL it_double(?, ?)", params);
    CRPC_CHECK(rows.ok());
    if (rows.ok()) {
        delete rows.value();
    }
    // The OUT value was written back into the caller's parameter.
    CRPC_CHECK_FALSE(params[1].is_null);
    CRPC_CHECK_EQ(static_cast<std::int64_t>(42), params[1].int_value);

    CRPC_CHECK(session.db->execute("DROP PROCEDURE IF EXISTS it_double").ok());
}

CRPC_TEST(mysql_it_bulk_insert) {
    (void)require_server();
    it_session session;
    CRPC_CHECK(session.db->execute("DROP TABLE IF EXISTS it_bulk").ok());
    CRPC_CHECK(session.db
                   ->execute("CREATE TABLE it_bulk (id BIGINT PRIMARY KEY, name VARCHAR(64))")
                   .ok());
    CRPC_CHECK(session.db->execute(
        "INSERT INTO it_bulk (id, name) VALUES (1,'a'),(2,'b'),(3,'c'),(4,'d'),(5,'e')").ok());

    auto rows = session.db->query("SELECT COUNT(*) FROM it_bulk");
    CRPC_CHECK(rows.ok());
    if (!rows.ok()) {
        return;
    }
    std::unique_ptr<crpc::db::result_set> guard(rows.value());
    CRPC_CHECK(guard != nullptr);
    if (guard != nullptr) {
        CRPC_CHECK(guard->next());
        CRPC_CHECK_EQ(static_cast<std::int64_t>(5), guard->get_int64(0).value());
    }
    CRPC_CHECK(session.db->execute("DROP TABLE IF EXISTS it_bulk").ok());
}

CRPC_TEST(mysql_it_multiple_result_sets_in_one_command) {
    (void)require_server();
    it_session session;
    auto rows = session.db->query("SELECT 1 AS a; SELECT 2 AS b");
    CRPC_CHECK(rows.ok());
    if (!rows.ok()) {
        std::printf("  multi-result error: %s\n", rows.error().message.c_str());
        return;
    }
    std::unique_ptr<crpc::db::result_set> guard(rows.value());
    CRPC_CHECK(guard != nullptr);
    if (guard == nullptr) {
        return;
    }
    CRPC_CHECK_EQ(std::string("a"), guard->column(0).name);
    CRPC_CHECK(guard->next());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(1), guard->get_int64(0).value());
    CRPC_CHECK_FALSE(guard->next());

    CRPC_CHECK(guard->next_result());
    CRPC_CHECK_EQ(std::string("b"), guard->column(0).name);
    CRPC_CHECK(guard->next());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(2), guard->get_int64(0).value());
    CRPC_CHECK_FALSE(guard->next());
    CRPC_CHECK_FALSE(guard->next_result());

    // The whole reply was consumed; the pooled connection is usable again.
    auto again = session.db->query("SELECT 9 AS n");
    CRPC_CHECK(again.ok());
    if (again.ok()) {
        std::unique_ptr<crpc::db::result_set> again_guard(again.value());
        CRPC_CHECK(again_guard->next());
        CRPC_CHECK_EQ(static_cast<std::int64_t>(9), again_guard->get_int64(0).value());
    }
}

CRPC_TEST(mysql_it_named_parameters) {
    (void)require_server();
    it_session session;
    std::vector<crpc::db::parameter> params;
    crpc::db::parameter hi = crpc::db::parameter::from_text("héllo");
    hi.name = "hi";
    crpc::db::parameter lo = crpc::db::parameter::from_int64(40);
    lo.name = "lo";
    params.push_back(lo);
    params.push_back(hi);
    auto rows = session.db->query("SELECT @lo + 2 AS n, @hi AS s", params);
    CRPC_CHECK(rows.ok());
    if (!rows.ok()) {
        std::printf("  named parameter error: %s\n", rows.error().message.c_str());
        return;
    }
    std::unique_ptr<crpc::db::result_set> guard(rows.value());
    CRPC_CHECK(guard != nullptr);
    if (guard != nullptr) {
        CRPC_CHECK(guard->next());
        CRPC_CHECK_EQ(static_cast<std::int64_t>(42), guard->get_int64(0).value());
        CRPC_CHECK_EQ(std::string("héllo"), std::string(guard->get_text(1).value()));
    }

    // A `@name` that is not bound is a session variable and is left untouched by
    // the rewrite, even inside a multi-statement command.
    std::vector<crpc::db::parameter> empty;
    auto vars = session.db->query("SET @x := 3; SELECT @x AS n", empty);
    CRPC_CHECK(vars.ok());
    if (vars.ok() && vars.value() != nullptr) {
        std::unique_ptr<crpc::db::result_set> vars_guard(vars.value());
        if (vars_guard->next_result()) {
            CRPC_CHECK(vars_guard->next());
            CRPC_CHECK_EQ(static_cast<std::int64_t>(3), vars_guard->get_int64(0).value());
        }
    }
}

CRPC_TEST(mysql_it_mixing_named_and_positional_is_rejected) {
    (void)require_server();
    it_session session;
    std::vector<crpc::db::parameter> params;
    crpc::db::parameter named = crpc::db::parameter::from_int64(1);
    named.name = "a";
    crpc::db::parameter positional = crpc::db::parameter::from_int64(2);
    params.push_back(named);
    params.push_back(positional);
    auto rows = session.db->query("SELECT @a + ? AS n", params);
    CRPC_CHECK_FALSE(rows.ok());
    if (!rows.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(rows.error().code));
    }
}

CRPC_TEST(mysql_it_isolation_level_and_command_timeout_surface) {
    (void)require_server();
    // The pool facade has no transaction entry point, so these run over one raw
    // connection, exactly like the scripted cases they complement.
    crpc::db::mysql::connection_options options;
    options.user = env_or("CRPC_MYSQL_USER", "crpc");
    options.password = env_or("CRPC_MYSQL_PASSWORD", "crpc_pw");
    options.database = env_or("CRPC_MYSQL_DB", "crpc_it");
    const std::string host = env_or("CRPC_MYSQL_HOST", "127.0.0.1");
    const std::string port = env_or("CRPC_MYSQL_PORT", "3306");
    crpc::iocp_backend backend;
    crpc::net::connection_config config;
    config.ep = crpc::endpoint::tcp(host, static_cast<std::uint16_t>(std::stoi(port)));
    config.operation_timeout = std::chrono::seconds(15);
    auto link = std::make_unique<crpc::db::socket_channel>(std::move(config), backend);
    std::unique_ptr<crpc::db::mysql::connection> conn(
        new crpc::db::mysql::connection(options, std::move(link)));
    CRPC_CHECK(conn->is_open());
    if (!conn->is_open()) {
        return;
    }

    // SERIALIZABLE is the strongest level. `begin(level)` emits
    // `SET TRANSACTION ISOLATION LEVEL ...; START TRANSACTION`, and that scoped
    // form applies to the *next* transaction only -- it deliberately does not
    // change the session variable. Reading @@transaction_isolation therefore
    // proves nothing about the running transaction (it still answers
    // REPEATABLE-READ). The level that actually applies is what InnoDB recorded
    // for this transaction, so ask it directly.
    const crpc::status began =
        conn->begin(crpc::db::isolation_level::serializable);
    CRPC_CHECK(began.ok());
    if (!began.ok()) {
        std::printf("  begin(serializable): %s\n", began.message().c_str());
        return;
    }
    CRPC_CHECK(conn->in_transaction());
    // Touch a table first: InnoDB only lists a transaction in innodb_trx once it
    // has started doing work.
    crpc::db::result_set* touched = nullptr;
    const crpc::status warm = conn->query("SELECT COUNT(*) FROM iso_probe", {}, &touched);
    crpc::db::statement_guard touched_guard(touched);
    CRPC_CHECK(warm.ok());
    crpc::db::result_set* state = nullptr;
    const crpc::status inspected = conn->query(
        "SELECT trx_isolation_level FROM information_schema.innodb_trx "
        "WHERE trx_mysql_thread_id = CONNECTION_ID()",
        {}, &state);
    crpc::db::statement_guard state_guard(state);
    CRPC_CHECK(inspected.ok());
    if (state != nullptr && state->next()) {
        CRPC_CHECK_EQ(std::string("SERIALIZABLE"), std::string(state->get_text(0).value()));
    } else {
        CRPC_CHECK(false);
    }
    const crpc::status rolled = conn->rollback();
    CRPC_CHECK(rolled.ok());
    CRPC_CHECK_FALSE(conn->in_transaction());

    // MySQL cannot interrupt a running statement without a second session (KILL
    // QUERY), so a non-zero timeout is refused explicitly rather than silently
    // ignored.
    const crpc::status zero = conn->set_command_timeout(std::chrono::milliseconds(0));
    CRPC_CHECK(zero.ok());
    const crpc::status refused = conn->set_command_timeout(std::chrono::seconds(1));
    CRPC_CHECK_FALSE(refused.ok());
    if (!refused.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::no_such_method),
                      static_cast<int>(refused.error().code));
    }
}

CRPC_TEST_MAIN()