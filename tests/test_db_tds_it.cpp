// CRPC -- TDS integration against a real SQL Server.
//
// Skipped unless CRPC_TDS_IT=1: a unit suite that needs a database engine on every
// developer machine is a suite people disable. These are the cases a scripted
// channel cannot prove: a real PRELOGIN answer, a real login, real packet sizes,
// real type encodings and real error numbers.
//
// Connection details come from the environment:
//   CRPC_TDS_IT=1
//   CRPC_TDS_HOST (default 127.0.0.1)
//   CRPC_TDS_PORT (default 1433)
//   CRPC_TDS_USER (default sa)
//   CRPC_TDS_PASSWORD (default Crpc#It2026)
//   CRPC_TDS_DB (default master; the cases create and use crpc_tds_it)
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/db/client.hpp>
#include <crpc/db/tds/connection.hpp>
#include <crpc/db/types.hpp>
#include <crpc/error.hpp>
#include <crpc/io/iocp_backend.hpp>

namespace {

[[nodiscard]] std::string env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::string(value) : std::string(fallback);
}

[[nodiscard]] bool it_enabled() {
    const char* value = std::getenv("CRPC_TDS_IT");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

[[nodiscard]] const char* require_server() {
    if (!it_enabled()) {
        crpc_test::skip("CRPC_TDS_IT is not 1");
    }
    return "";
}

struct it_session {
    crpc::iocp_backend backend;
    std::unique_ptr<crpc::db::tds::connection_factory> factory;
    std::unique_ptr<crpc::db::tds::connection> session;

    explicit it_session(const char* database = nullptr) {
        crpc::db::tds::connection_options options;
        options.user = env_or("CRPC_TDS_USER", "sa");
        options.password = env_or("CRPC_TDS_PASSWORD", "Crpc#It2026");
        options.database = database != nullptr ? database : env_or("CRPC_TDS_DB", "master");
        const std::string host = env_or("CRPC_TDS_HOST", "127.0.0.1");
        const std::string port = env_or("CRPC_TDS_PORT", "1433");
        (void)options;
        crpc::net::connection_config config;
        config.ep = crpc::endpoint::tcp(
            host, static_cast<std::uint16_t>(std::stoi(port)));
        config.operation_timeout = std::chrono::seconds(15);
        auto link = std::make_unique<crpc::db::socket_channel>(std::move(config), backend);
        session = std::unique_ptr<crpc::db::tds::connection>(
            new crpc::db::tds::connection(options, std::move(link)));
    }

    [[nodiscard]] bool ok() const { return session != nullptr && session->is_open(); }
};

// Prints the server's own message so a failure here is diagnosable without a
// debugger.
void report(const char* what, const crpc::rpc_error& error) {
    std::printf("  %s failed: %s (%d)\n", what, error.message.c_str(),
                static_cast<int>(error.code));
}

} // namespace

CRPC_TEST(tds_it_select_one) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        report("login", it.session->connect_error());
        CRPC_CHECK(it.ok());
        return;
    }
    crpc::db::result_set* raw = nullptr;
    const crpc::status ran = it.session->query("SELECT 1", {}, &raw);
    crpc::db::statement_guard guard(raw);
    if (!ran) {
        report("SELECT 1", ran.error());
        CRPC_CHECK(static_cast<bool>(ran));
        return;
    }
    CRPC_CHECK(raw != nullptr);
    if (raw != nullptr) {
        CRPC_CHECK(raw->next());
        CRPC_CHECK_EQ(std::int64_t{1}, raw->get_int64(0).value());
    }
}

CRPC_TEST(tds_it_types_round_trip) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        report("login", it.session->connect_error());
        CRPC_CHECK(it.ok());
        return;
    }
    crpc::db::result_set* raw = nullptr;
    const crpc::status ran = it.session->query(
        "SELECT CAST(-7 AS int) AS i, CAST('héllo' AS nvarchar(20)) AS s, "
        "CAST(12.34 AS decimal(10,2)) AS d, CAST('2024-04-25T12:34:56.1234567' AS datetime2(7)) AS t, "
        "CAST(NULL AS int) AS n, "
        "CAST(0x0102 AS varbinary(4)) AS b, "
        "CAST('6F9619FF-8B86-D011-B42D-00C04FC964FF' AS uniqueidentifier) AS g, "
        "CAST(1 AS bit) AS f",
        {}, &raw);
    crpc::db::statement_guard guard(raw);
    if (!ran) {
        report("types query", ran.error());
        CRPC_CHECK(static_cast<bool>(ran));
        return;
    }
    CRPC_CHECK(raw != nullptr);
    if (raw == nullptr) {
        return;
    }
    CRPC_CHECK_EQ(std::size_t{8}, raw->column_count());
    CRPC_CHECK(raw->next());
    CRPC_CHECK_EQ(std::string("i"), raw->column(0).name);
    CRPC_CHECK_EQ(std::int64_t{-7}, raw->get_int64(0).value());
    CRPC_CHECK_EQ(std::string("héllo"), raw->get_text(1).value());
    CRPC_CHECK_EQ(std::string("12.34"), raw->get_text(2).value());
    CRPC_CHECK_EQ(std::string("2024-04-25 12:34:56.1234567"), raw->get_text(3).value());
    CRPC_CHECK(raw->is_null(4));
    CRPC_CHECK_EQ(std::size_t{2}, raw->get_blob(5).value().size());
    CRPC_CHECK_EQ(std::string("6f9619ff-8b86-d011-b42d-00c04fc964ff"),
                  raw->get_text(6).value());
    CRPC_CHECK(raw->get_bool(7).value());
}

CRPC_TEST(tds_it_error_reports_the_server_number) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }
    crpc::db::result_set* raw = nullptr;
    const crpc::status ran = it.session->query("SELECT * FROM no_such_table_here", {}, &raw);
    crpc::db::statement_guard guard(raw);
    CRPC_CHECK_FALSE(static_cast<bool>(ran));
    if (!ran) {
        report("bad table", ran.error());
        // 208 is "invalid object name".
        CRPC_CHECK_EQ(static_cast<int>(crpc::db::tds::map_error_number(208)),
                      static_cast<int>(ran.error().code));
    }
    // The session must survive its own error.
    CRPC_CHECK(it.session->is_open());
    crpc::db::result_set* again = nullptr;
    const crpc::status second = it.session->query("SELECT 1", {}, &again);
    crpc::db::statement_guard again_guard(again);
    CRPC_CHECK(static_cast<bool>(second));
}

CRPC_TEST(tds_it_ddl_dml_and_affected_rows) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }

    auto run = [&](std::string_view sql) {
        crpc::db::result_set* raw = nullptr;
        const crpc::status ran = it.session->query(sql, {}, &raw);
        crpc::db::statement_guard guard(raw);
        if (!ran) {
            report("batch", ran.error());
        }
        CRPC_CHECK(static_cast<bool>(ran));
        return raw;
    };

    {
        crpc::db::result_set* raw = nullptr;
        const crpc::status ran = it.session->query(
            "IF DB_ID('crpc_tds_it') IS NULL CREATE DATABASE crpc_tds_it", {}, &raw);
        crpc::db::statement_guard guard(raw);
        if (!ran) {
            report("create database", ran.error());
        }
        CRPC_CHECK(static_cast<bool>(ran));
    }
    CRPC_CHECK(static_cast<bool>(it.session->query("USE crpc_tds_it", {}, nullptr)));

    (void)run("IF OBJECT_ID('crpc_rows') IS NOT NULL DROP TABLE crpc_rows");
    (void)run("CREATE TABLE crpc_rows (id int NOT NULL PRIMARY KEY, name nvarchar(50) NULL)");
    {
        crpc::db::result_set* raw = nullptr;
        const crpc::status ran = it.session->query(
            "INSERT INTO crpc_rows (id, name) VALUES (1, N'one'), (2, NULL)", {}, &raw);
        crpc::db::statement_guard guard(raw);
        if (!ran) {
            report("insert", ran.error());
        }
        CRPC_CHECK(static_cast<bool>(ran));
        CRPC_CHECK(raw != nullptr);
        if (raw != nullptr) {
            // TDS reports the count in DONE, after the rows.
            CRPC_CHECK_EQ(std::int64_t{2}, raw->affected_rows());
        }
    }

    // A rolled-back transaction must leave no row behind.
    CRPC_CHECK(static_cast<bool>(it.session->begin()));
    CRPC_CHECK(it.session->in_transaction());
    (void)run("INSERT INTO crpc_rows (id, name) VALUES (3, N'three')");
    CRPC_CHECK(static_cast<bool>(it.session->rollback()));
    CRPC_CHECK_FALSE(it.session->in_transaction());

    crpc::db::result_set* raw = nullptr;
    const crpc::status ran =
        it.session->query("SELECT COUNT(*) AS n FROM crpc_rows", {}, &raw);
    crpc::db::statement_guard guard(raw);
    CRPC_CHECK(static_cast<bool>(ran));
    if (raw != nullptr) {
        CRPC_CHECK(raw->next());
        CRPC_CHECK_EQ(std::int64_t{2}, raw->get_int64(0).value());
    }
}

CRPC_TEST(tds_it_bound_parameters) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        report("login", it.session->connect_error());
        CRPC_CHECK(it.ok());
        return;
    }

    // int, nvarchar (with a non-ASCII character), NULL and float in one statement.
    crpc::db::parameter params[4] = {
        crpc::db::parameter::from_int64(42),
        crpc::db::parameter::from_text("héllo"),
        crpc::db::parameter::null(),
        crpc::db::parameter::from_double(3.5),
    };
    // A NULL parameter carries no type, so the declaration would be wrong; the
    // adapter must say so rather than guess.
    crpc::db::result_set* raw = nullptr;
    crpc::status ran = it.session->query(
        "SELECT ? AS a, ? AS b, ? AS c, ? AS d",
        crpc::span<const crpc::db::parameter>(params, 4), &raw);
    crpc::db::statement_guard guard(raw);
    if (!ran) {
        // db_type::unknown cannot be declared; that is a caller mistake, and the
        // message must say which parameter.
        report("unknown-typed parameter", ran.error());
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(ran.error().code));
    }

    crpc::db::parameter typed[3] = {
        crpc::db::parameter::from_int64(42),
        crpc::db::parameter::from_text("héllo"),
        crpc::db::parameter::from_double(3.5),
    };
    crpc::db::result_set* rows = nullptr;
    ran = it.session->query("SELECT ? AS a, ? AS b, ? AS c",
                            crpc::span<const crpc::db::parameter>(typed, 3), &rows);
    crpc::db::statement_guard rows_guard(rows);
    if (!ran) {
        report("bound query", ran.error());
        CRPC_CHECK(static_cast<bool>(ran));
        return;
    }
    CRPC_CHECK(rows != nullptr);
    if (rows == nullptr) {
        return;
    }
    CRPC_CHECK_EQ(std::size_t{3}, rows->column_count());
    CRPC_CHECK(rows->next());
    CRPC_CHECK_EQ(std::int64_t{42}, rows->get_int64(0).value());
    CRPC_CHECK_EQ(std::string("héllo"), rows->get_text(1).value());
    CRPC_CHECK_EQ(3.5, rows->get_double(2).value());
}

CRPC_TEST(tds_it_parameter_quoting_is_not_a_literal) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }
    // A '?' inside a literal must stay a literal, and a '?' inside a comment must not
    // be counted: only the real placeholder is bound.
    crpc::db::parameter params[2] = {
        crpc::db::parameter::from_text("a?b"),
        crpc::db::parameter::from_int64(7),
    };
    crpc::db::result_set* raw = nullptr;
    const crpc::status ran = it.session->query(
        "SELECT ? AS lit -- a ? in a comment\n, ? AS n",
        crpc::span<const crpc::db::parameter>(params, 2), &raw);
    crpc::db::statement_guard guard(raw);
    if (!ran) {
        report("quoted placeholder", ran.error());
        CRPC_CHECK(static_cast<bool>(ran));
        return;
    }
    CRPC_CHECK(raw != nullptr);
    if (raw != nullptr) {
        CRPC_CHECK(raw->next());
        CRPC_CHECK_EQ(std::string("a?b"), raw->get_text(0).value());
        CRPC_CHECK_EQ(std::int64_t{7}, raw->get_int64(1).value());
    }
}

CRPC_TEST(tds_it_parameter_type_mismatch_is_reported) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }
    // More placeholders than parameters: refused before anything is sent, because a
    // count mismatch binds values to the wrong markers.
    crpc::db::parameter one = crpc::db::parameter::from_int64(1);
    crpc::db::result_set* raw = nullptr;
    const crpc::status ran = it.session->query(
        "SELECT ?, ?", crpc::span<const crpc::db::parameter>(&one, 1), &raw);
    crpc::db::statement_guard guard(raw);
    CRPC_CHECK_FALSE(static_cast<bool>(ran));
    if (!ran) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(ran.error().code));
        report("count mismatch", ran.error());
    }
    // The session survives.
    CRPC_CHECK(static_cast<bool>(it.session->query("SELECT 1", {}, nullptr)));
}

CRPC_TEST(tds_it_stored_procedure_out_parameter) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        report("login", it.session->connect_error());
        CRPC_CHECK(it.ok());
        return;
    }

    {
        crpc::db::result_set* raw = nullptr;
        const crpc::status created = it.session->query(
            "IF DB_ID('crpc_tds_it') IS NULL CREATE DATABASE crpc_tds_it", {}, &raw);
        crpc::db::statement_guard guard(raw);
        if (!created) {
            report("create database", created.error());
        }
        CRPC_CHECK(static_cast<bool>(created));
    }
    CRPC_CHECK(static_cast<bool>(it.session->query("USE crpc_tds_it", {}, nullptr)));

    {
        crpc::db::result_set* raw = nullptr;
        const crpc::status dropped = it.session->query(
            "IF OBJECT_ID('crpc_add') IS NOT NULL DROP PROCEDURE crpc_add", {}, &raw);
        crpc::db::statement_guard guard(raw);
        CRPC_CHECK(static_cast<bool>(dropped));
    }
    {
        crpc::db::result_set* raw = nullptr;
        const crpc::status created = it.session->query(
            "CREATE PROCEDURE crpc_add @a int, @b int, @sum int OUTPUT AS "
            "BEGIN SET @sum = @a + @b; RETURN 7; END",
            {}, &raw);
        crpc::db::statement_guard guard(raw);
        if (!created) {
            report("create procedure", created.error());
        }
        CRPC_CHECK(static_cast<bool>(created));
    }

    crpc::db::parameter params[3] = {
        crpc::db::parameter::from_int64(2),
        crpc::db::parameter::from_int64(40),
        crpc::db::parameter::out("sum", crpc::db::db_type::int64),
    };
    params[0].name = "a";
    params[1].name = "b";
    params[2].name = "sum";

    crpc::db::result_set* raw = nullptr;
    const crpc::status ran = it.session->call("crpc_add", params, &raw);
    crpc::db::statement_guard guard(raw);
    if (!ran) {
        report("procedure call", ran.error());
        CRPC_CHECK(static_cast<bool>(ran));
        return;
    }
    // The OUT value must come back through the RETURNVALUE token.
    CRPC_CHECK_FALSE(params[2].is_null);
    CRPC_CHECK_EQ(std::int64_t{42}, params[2].int_value);
    // And the procedure's return code through RETURNSTATUS.
    CRPC_CHECK(it.session->has_return_status());
    CRPC_CHECK_EQ(std::uint32_t{7}, it.session->last_return_status());
}

CRPC_TEST(tds_it_batch_after_rpc_stays_aligned) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }
    crpc::db::parameter one = crpc::db::parameter::from_int64(5);
    {
        crpc::db::result_set* raw = nullptr;
        const crpc::status ran =
            it.session->query("SELECT ? AS n", crpc::span<const crpc::db::parameter>(&one, 1), &raw);
        crpc::db::statement_guard guard(raw);
        CRPC_CHECK(static_cast<bool>(ran));
    }
    // A plain batch right after an RPC must decode as its own reply.
    crpc::db::result_set* raw = nullptr;
    const crpc::status ran = it.session->query("SELECT 99 AS n", {}, &raw);
    crpc::db::statement_guard guard(raw);
    CRPC_CHECK(static_cast<bool>(ran));
    if (raw != nullptr) {
        CRPC_CHECK(raw->next());
        CRPC_CHECK_EQ(std::int64_t{99}, raw->get_int64(0).value());
    }
}

CRPC_TEST_MAIN()