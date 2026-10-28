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

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
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

    explicit it_session(const char* database = nullptr, bool require_encryption = false,
                        bool login_only = false) {
        crpc::db::tds::connection_options options;
        options.require_encryption = require_encryption;
        options.encrypt_login_only = login_only;
        // The test server uses a self-signed certificate; verification is what a
        // production deployment turns on, and the case that exercises it is separate.
        options.verify_server_certificate = false;
        options.user = env_or("CRPC_TDS_USER", "sa");
        options.password = env_or("CRPC_TDS_PASSWORD", "Crpc#It2026");
        options.database = database != nullptr ? database : env_or("CRPC_TDS_DB", "master");
        const std::string host = env_or("CRPC_TDS_HOST", "127.0.0.1");
        const std::string port = env_or("CRPC_TDS_PORT", "1433");
        // The same name is used for SNI and for LOGIN7's ServerName. It is not
        // strictly required by this server, but a TLS client without SNI cannot be
        // verified against a certificate.
        if (options.server_name.empty()) {
            options.server_name = host;
        }
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
        // `N'héllo'` is a Unicode literal. A plain 'héllo' is a varchar literal whose code
        // page is the current database's default collation, so on a database with a non-Unicode
        // collation the server turns the character into '?' before the client ever sees it --
        // a failure with nothing to do with the codec.
        "SELECT CAST(-7 AS int) AS i, CAST(N'héllo' AS nvarchar(20)) AS s, "
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

CRPC_TEST(tds_it_attention_cancels_a_running_batch) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        report("login", it.session->connect_error());
        CRPC_CHECK(it.ok());
        return;
    }

    // A three-second WAITFOR on the session, cancelled from another thread after a
    // moment. The blocked query must return `canceled` rather than the delay's result,
    // and the session must stay usable afterwards: a cancel that desynchronises the
    // stream would be worse than no cancel at all.
    std::atomic<bool> started{false};
    crpc::status outcome;
    std::thread canceller([&] {
        while (!started.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const crpc::status asked = it.session->cancel();
        if (!asked) {
            report("cancel", asked.error());
        }
    });

    started.store(true);
    const auto began = std::chrono::steady_clock::now();
    crpc::db::result_set* raw = nullptr;
    outcome = it.session->query("WAITFOR DELAY '00:00:03'; SELECT 1 AS n", {}, &raw);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - began).count();
    // The cancel must land well inside the three-second delay; if it did not, the test
    // would pass on a server that simply ignored the ATTENTION.
    CRPC_CHECK(elapsed < 2000);
    CRPC_CHECK(outcome.ok() == false);
    crpc::db::statement_guard guard(raw);
    canceller.join();

    CRPC_CHECK_FALSE(static_cast<bool>(outcome));
    if (!outcome) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::canceled),
                      static_cast<int>(outcome.error().code));
    }
    CRPC_CHECK(it.session->is_open());

    // The session must still answer: the attention acknowledgement was consumed.
    crpc::db::result_set* after = nullptr;
    const crpc::status ran = it.session->query("SELECT 123 AS n", {}, &after);
    crpc::db::statement_guard after_guard(after);
    if (!ran) {
        report("query after cancel", ran.error());
    }
    CRPC_CHECK(static_cast<bool>(ran));
    if (after != nullptr) {
        CRPC_CHECK(after->next());
        CRPC_CHECK_EQ(std::int64_t{123}, after->get_int64(0).value());
    }
}

CRPC_TEST(tds_it_require_encryption_logs_in_and_queries) {
    (void)require_server();
#if defined(CRPC_HAVE_OPENSSL)
    // The whole session runs inside TLS: PRELOGIN offers ENCRYPT_ON, the handshake
    // records travel inside PRELOGIN packets, and LOGIN7 plus the query are written
    // as ordinary TDS packets inside the encrypted stream.
    it_session it(nullptr, true);
    if (!it.ok()) {
        report("encrypted login", it.session->connect_error());
        CRPC_CHECK(it.ok());
        return;
    }
    CRPC_CHECK(it.session->encryption_negotiated());
    CRPC_CHECK_FALSE(it.session->login_only_encryption());

    crpc::db::result_set* raw = nullptr;
    const crpc::status ran = it.session->query("SELECT 42 AS n", {}, &raw);
    crpc::db::statement_guard guard(raw);
    if (!ran) {
        report("encrypted SELECT 42", ran.error());
    }
    CRPC_CHECK(static_cast<bool>(ran));
    if (raw != nullptr) {
        CRPC_CHECK(raw->next());
        CRPC_CHECK_EQ(std::int64_t{42}, raw->get_int64(0).value());
    }

    // A second query proves the TLS record/frame boundary stayed aligned after the
    // login reply: a channel that desynchronised would fail here, not on the first.
    crpc::db::result_set* again = nullptr;
    const crpc::status second = it.session->query("SELECT 43 AS n", {}, &again);
    crpc::db::statement_guard again_guard(again);
    if (!second) {
        report("encrypted SELECT 43", second.error());
    }
    CRPC_CHECK(static_cast<bool>(second));
    if (again != nullptr) {
        CRPC_CHECK(again->next());
        CRPC_CHECK_EQ(std::int64_t{43}, again->get_int64(0).value());
    }

    // The server's own verdict, not the client's flag: sys.dm_exec_connections reports
    // per-connection encryption state. This is what makes "the session really is
    // encrypted" an observation instead of an assumption about our own code path.
    crpc::db::result_set* state = nullptr;
    const crpc::status inspected = it.session->query(
        "SELECT encrypt_option, protocol_type, auth_scheme FROM sys.dm_exec_connections "
        "WHERE session_id = @@SPID",
        {}, &state);
    crpc::db::statement_guard state_guard(state);
    if (!inspected) {
        report("sys.dm_exec_connections", inspected.error());
    }
    CRPC_CHECK(static_cast<bool>(inspected));
    const bool have_row = state != nullptr && state->next();
    CRPC_CHECK(have_row);
    if (have_row) {
        CRPC_CHECK_EQ(std::string("TRUE"), state->get_text(0).value());
        CRPC_CHECK_EQ(std::string("TSQL"), state->get_text(1).value());
        CRPC_CHECK_EQ(std::string("SQL"), state->get_text(2).value());
    }
#else
    // Without a TLS backend the adapter must refuse before writing a byte rather
    // than send a plaintext login the caller believes is encrypted.
    it_session it(nullptr, true);
    CRPC_CHECK_FALSE(it.ok());
    const crpc::rpc_error& why = it.session->connect_error();
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(why.code));
    CRPC_CHECK(why.message.find("TLS") != std::string::npos);

    // A plaintext session against the same server still works, which is what proves the
    // refusal is about the missing backend and not about the endpoint.
    it_session plain;
    CRPC_CHECK(plain.ok());
    if (plain.ok()) {
        crpc::db::result_set* raw = nullptr;
        const crpc::status ran = plain.session->query("SELECT 1 AS n", {}, &raw);
        crpc::db::statement_guard guard(raw);
        CRPC_CHECK(static_cast<bool>(ran));
    }
#endif
}

CRPC_TEST(tds_it_legacy_types) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        report("login", it.session->connect_error());
        CRPC_CHECK(it.ok());
        return;
    }
    // The types a real schema still contains: money, the pre-2008 datetime pair, and
    // datetimeoffset. Each has its own encoding, and the epoch for the legacy pair is
    // 1900-01-01 rather than 0001-01-01.
    crpc::db::result_set* raw = nullptr;
    const crpc::status ran = it.session->query(
        "SELECT CAST(12.3456 AS money) AS m, CAST(-0.5 AS smallmoney) AS sm, "
        "CAST('2024-04-25T12:34:56.123' AS datetime) AS dt, "
        "CAST('2024-04-25T12:34:00' AS smalldatetime) AS sdt, "
        "CAST('2024-04-25T12:34:56.1234567+02:30' AS datetimeoffset(7)) AS dto",
        {}, &raw);
    crpc::db::statement_guard guard(raw);
    if (!ran) {
        report("legacy types", ran.error());
        CRPC_CHECK(static_cast<bool>(ran));
        return;
    }
    CRPC_CHECK(raw != nullptr);
    if (raw == nullptr) {
        return;
    }
    CRPC_CHECK(raw->next());
    CRPC_CHECK_EQ(std::string("12.3456"), raw->get_text(0).value());
    CRPC_CHECK_EQ(std::string("-0.5000"), raw->get_text(1).value());
    // datetime is exact to 3.33 ms, so the stored value is what the server rounded to.
    CRPC_CHECK_EQ(std::string("2024-04-25 12:34:56.123"), raw->get_text(2).value());
    CRPC_CHECK_EQ(std::string("2024-04-25 12:34"), raw->get_text(3).value());
    CRPC_CHECK_EQ(std::string("2024-04-25 12:34:56.1234567 +02:30"),
                  raw->get_text(4).value());
}

CRPC_TEST(tds_it_bulk_insert) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        report("login", it.session->connect_error());
        CRPC_CHECK(it.ok());
        return;
    }
    {
        crpc::db::result_set* raw = nullptr;
        const crpc::status created =
            it.session->query("IF DB_ID('crpc_tds_it') IS NULL CREATE DATABASE crpc_tds_it", {},
                              &raw);
        crpc::db::statement_guard guard(raw);
        CRPC_CHECK(static_cast<bool>(created));
    }
    CRPC_CHECK(static_cast<bool>(it.session->query("USE crpc_tds_it", {}, nullptr)));
    {
        crpc::db::result_set* raw = nullptr;
        const crpc::status made = it.session->query(
            "IF OBJECT_ID('crpc_bulk') IS NOT NULL DROP TABLE crpc_bulk; "
            "CREATE TABLE crpc_bulk (id int NOT NULL PRIMARY KEY, name nvarchar(50) NULL)",
            {}, &raw);
        crpc::db::statement_guard guard(raw);
        if (!made) {
            report("create bulk table", made.error());
        }
        CRPC_CHECK(static_cast<bool>(made));
    }

    // Enough rows to need more than one statement at the 2100-parameter bound: two
    // columns allow 1050 rows per statement, so 2500 rows is three statements.
    constexpr std::size_t row_count = 2500;
    std::vector<std::vector<crpc::db::parameter>> rows;
    rows.reserve(row_count);
    for (std::size_t i = 0; i < row_count; ++i) {
        std::vector<crpc::db::parameter> row;
        row.push_back(crpc::db::parameter::from_int64(static_cast<std::int64_t>(i)));
        row.push_back(crpc::db::parameter::from_text("row " + std::to_string(i)));
        rows.push_back(std::move(row));
    }

    crpc::db::insert_target target;
    target.table = "crpc_bulk";
    target.columns = {"id", "name"};
    crpc::db::bulk_options options;
    // Ask for more rows per statement than the parameter limit allows; the adapter must
    // clamp it rather than let the server reject the request.
    options.max_rows_per_statement = 5000;

    std::uint64_t affected = 0;
    const crpc::status ran =
        it.session->bulk_insert(target, crpc::span<const std::vector<crpc::db::parameter>>(
                                            rows.data(), rows.size()),
                                options, affected);
    if (!ran) {
        report("bulk insert", ran.error());
    }
    CRPC_CHECK(static_cast<bool>(ran));
    CRPC_CHECK_EQ(std::uint64_t{row_count}, affected);

    crpc::db::result_set* raw = nullptr;
    const crpc::status counted =
        it.session->query("SELECT COUNT(*) AS n, MIN(id) AS lo, MAX(id) AS hi FROM crpc_bulk",
                          {}, &raw);
    crpc::db::statement_guard guard(raw);
    CRPC_CHECK(static_cast<bool>(counted));
    if (raw != nullptr) {
        CRPC_CHECK(raw->next());
        CRPC_CHECK_EQ(std::int64_t{static_cast<std::int64_t>(row_count)},
                      raw->get_int64(0).value());
        CRPC_CHECK_EQ(std::int64_t{0}, raw->get_int64(1).value());
        CRPC_CHECK_EQ(std::int64_t{static_cast<std::int64_t>(row_count) - 1},
                      raw->get_int64(2).value());
    }
}

CRPC_TEST(tds_it_bulk_insert_refuses_a_shape_mismatch) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }
    crpc::db::insert_target target;
    target.table = "crpc_bulk";
    target.columns = {"id", "name"};
    std::vector<std::vector<crpc::db::parameter>> rows;
    std::vector<crpc::db::parameter> short_row;
    short_row.push_back(crpc::db::parameter::from_int64(1));
    rows.push_back(std::move(short_row));

    std::uint64_t affected = 12345;
    const crpc::status ran =
        it.session->bulk_insert(target, crpc::span<const std::vector<crpc::db::parameter>>(
                                            rows.data(), rows.size()),
                                crpc::db::bulk_options{}, affected);
    CRPC_CHECK_FALSE(static_cast<bool>(ran));
    if (!ran) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(ran.error().code));
        report("shape mismatch", ran.error());
    }
    // A refused batch must report zero, not leave a stale count from a previous call.
    CRPC_CHECK_EQ(std::uint64_t{0}, affected);
}

CRPC_TEST(tds_it_multiple_result_sets_in_one_batch) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        report("login", it.session->connect_error());
        CRPC_CHECK(it.ok());
        return;
    }
    // One batch, two result sets. Both must come back, in order, through
    // `next_result()` -- the second set was previously drained and discarded.
    crpc::db::result_set* raw = nullptr;
    const crpc::status ran = it.session->query("SELECT 1 AS a; SELECT 2 AS b", {}, &raw);
    crpc::db::statement_guard guard(raw);
    if (!ran) {
        report("multi-result batch", ran.error());
        CRPC_CHECK(static_cast<bool>(ran));
        return;
    }
    CRPC_CHECK(raw != nullptr);
    if (raw == nullptr) {
        return;
    }
    CRPC_CHECK_EQ(std::string("a"), raw->column(0).name);
    CRPC_CHECK(raw->next());
    CRPC_CHECK_EQ(std::int64_t{1}, raw->get_int64(0).value());
    CRPC_CHECK_FALSE(raw->next());

    CRPC_CHECK(raw->next_result());
    CRPC_CHECK_EQ(std::string("b"), raw->column(0).name);
    CRPC_CHECK(raw->next());
    CRPC_CHECK_EQ(std::int64_t{2}, raw->get_int64(0).value());
    CRPC_CHECK_FALSE(raw->next());
    CRPC_CHECK_FALSE(raw->next_result());

    // The session must still answer: the whole reply, not just the first set, was
    // consumed.
    crpc::db::result_set* again = nullptr;
    const crpc::status second = it.session->query("SELECT 7 AS n", {}, &again);
    crpc::db::statement_guard again_guard(again);
    CRPC_CHECK(static_cast<bool>(second));
    if (again != nullptr) {
        CRPC_CHECK(again->next());
        CRPC_CHECK_EQ(std::int64_t{7}, again->get_int64(0).value());
    }
}

CRPC_TEST(tds_it_named_parameters) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        report("login", it.session->connect_error());
        CRPC_CHECK(it.ok());
        return;
    }
    // A caller-written @lo/@hi pair bound by name, with a non-ASCII value to prove
    // the encoding. The list order need not match the text order.
    crpc::db::parameter hi = crpc::db::parameter::from_text("héllo");
    hi.name = "hi";
    crpc::db::parameter lo = crpc::db::parameter::from_int64(40);
    lo.name = "lo";
    crpc::db::parameter params[2] = {lo, hi};
    crpc::db::result_set* raw = nullptr;
    const crpc::status ran = it.session->query(
        "SELECT @lo + 2 AS n, @hi AS s",
        crpc::span<const crpc::db::parameter>(params, 2), &raw);
    crpc::db::statement_guard guard(raw);
    if (!ran) {
        report("named parameters", ran.error());
        CRPC_CHECK(static_cast<bool>(ran));
        return;
    }
    CRPC_CHECK(raw != nullptr);
    if (raw != nullptr) {
        CRPC_CHECK(raw->next());
        CRPC_CHECK_EQ(std::int64_t{42}, raw->get_int64(0).value());
        CRPC_CHECK_EQ(std::string("héllo"), raw->get_text(1).value());
    }
}

CRPC_TEST(tds_it_streaming_reads_one_hundred_thousand_rows) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        report("login", it.session->connect_error());
        CRPC_CHECK(it.ok());
        return;
    }

    crpc::db::result_set* raw = nullptr;
    const crpc::status ran = it.session->query_streaming(
        "SELECT TOP (100000) a.object_id + b.column_id AS n "
        "FROM sys.all_columns a CROSS JOIN sys.all_columns b",
        {}, &raw);
    crpc::db::statement_guard guard(raw);
    if (!ran) {
        report("streaming query", ran.error());
        CRPC_CHECK(static_cast<bool>(ran));
        return;
    }
    CRPC_CHECK(raw != nullptr);
    if (raw == nullptr) {
        return;
    }
    CRPC_CHECK(raw->is_streaming());

    std::int64_t read = 0;
    while (raw->next()) {
        CRPC_CHECK(read < static_cast<std::int64_t>(raw->rows_decoded()));
        // At rest the cursor holds at most the row it is handing out; it must not
        // be materialising the 100k-row result.
        CRPC_CHECK(raw->rows_buffered() <= 1);
        ++read;
    }
    CRPC_CHECK_EQ(std::int64_t{100000}, read);
    CRPC_CHECK_EQ(std::size_t{100000}, raw->rows_decoded());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::ok),
                  static_cast<int>(raw->last_error().code));

    // The reply was fully drained; the connection serves the next request.
    crpc::db::result_set* after = nullptr;
    const crpc::status second = it.session->query("SELECT 3 AS n", {}, &after);
    crpc::db::statement_guard after_guard(after);
    CRPC_CHECK(static_cast<bool>(second));
    if (after != nullptr) {
        CRPC_CHECK(after->next());
        CRPC_CHECK_EQ(std::int64_t{3}, after->get_int64(0).value());
    }
}

CRPC_TEST(tds_it_isolation_level_is_accepted_by_the_server) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        report("login", it.session->connect_error());
        CRPC_CHECK(it.ok());
        return;
    }
    // SERIALIZABLE maps to transaction_isolation_level 4 in sys.dm_exec_sessions.
    // Asking the server what it is running, from the same session, is the proof
    // that the SET was accepted and in force.
    crpc::status began = it.session->begin(crpc::db::isolation_level::serializable);
    if (!began) {
        report("begin(serializable)", began.error());
    }
    CRPC_CHECK(static_cast<bool>(began));
    CRPC_CHECK(it.session->in_transaction());
    if (began) {
        crpc::db::result_set* state = nullptr;
        const crpc::status inspected = it.session->query(
            "SELECT transaction_isolation_level FROM sys.dm_exec_sessions "
            "WHERE session_id = @@SPID",
            {}, &state);
        crpc::db::statement_guard guard(state);
        CRPC_CHECK(static_cast<bool>(inspected));
        if (state != nullptr && state->next()) {
            CRPC_CHECK_EQ(std::int64_t{4}, state->get_int64(0).value());
        }
    }
    crpc::status ended = it.session->rollback();
    if (!ended) {
        report("rollback", ended.error());
    }
    CRPC_CHECK(static_cast<bool>(ended));
    CRPC_CHECK_FALSE(it.session->in_transaction());
}

CRPC_TEST(tds_it_command_timeout_interrupts_waitfor_on_the_server) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        report("login", it.session->connect_error());
        CRPC_CHECK(it.ok());
        return;
    }
    CRPC_CHECK(static_cast<bool>(it.session->query("USE crpc_tds_it", {}, nullptr)));
    crpc::db::result_set* made = nullptr;
    const crpc::status created = it.session->query(
        "IF OBJECT_ID('crpc_timeout_marks') IS NOT NULL DROP TABLE crpc_timeout_marks; "
        "CREATE TABLE crpc_timeout_marks (mark int)",
        {}, &made);
    crpc::db::statement_guard made_guard(made);
    CRPC_CHECK(static_cast<bool>(created));

    // The SPID of the session under test, read from a second session so the first
    // can stay in the query.
    it_session observer;
    if (!observer.ok()) {
        report("observer login", observer.session->connect_error());
        CRPC_CHECK(observer.ok());
        return;
    }
    crpc::db::result_set* spid_set = nullptr;
    const crpc::status spid_ran = observer.session->query("SELECT @@SPID", {}, &spid_set);
    crpc::db::statement_guard spid_guard(spid_set);
    CRPC_CHECK(static_cast<bool>(spid_ran));
    std::int64_t spid = -1;
    if (spid_set != nullptr && spid_set->next()) {
        spid = spid_set->get_int64(0).value();
    }
    CRPC_CHECK(spid > 0);

    const auto running_on_server = [&observer, spid]() {
        crpc::db::result_set* count = nullptr;
        const crpc::status ran = observer.session->query(
            "SELECT COUNT(*) FROM sys.dm_exec_requests WHERE session_id = " +
                std::to_string(spid),
            {}, &count);
        crpc::db::statement_guard guard(count);
        if (!ran || count == nullptr) {
            return -1LL;
        }
        return count->next() ? count->get_int64(0).value() : -1LL;
    };

    // A 5-second WAITFOR followed by an INSERT, with a 1-second command timeout.
    // If the timeout merely returned an error while the batch kept running, the
    // INSERT behind the WAITFOR would still land -- which is exactly the thing the
    // test exists to forbid.
    const crpc::status set = it.session->set_command_timeout(std::chrono::seconds(1));
    CRPC_CHECK(static_cast<bool>(set));

    crpc::status outcome;
    std::thread runner([&] {
        crpc::db::result_set* rows = nullptr;
        outcome = it.session->query(
            "WAITFOR DELAY '00:00:05'; INSERT INTO crpc_timeout_marks VALUES (1)", {},
            &rows);
        crpc::db::statement_guard guard(rows);
    });

    // The server must be executing the WAITFOR while the client waits.
    bool saw_running = false;
    for (int i = 0; i < 40; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (running_on_server() == 1) {
            saw_running = true;
            break;
        }
    }

    const auto began = std::chrono::steady_clock::now();
    runner.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - began)
                             .count();

    CRPC_CHECK(saw_running);
    // The cancel landed inside the 5-second delay.
    CRPC_CHECK(elapsed < 4000);
    CRPC_CHECK_FALSE(static_cast<bool>(outcome));
    if (!outcome) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::canceled),
                      static_cast<int>(outcome.error().code));
    }
    CRPC_CHECK(it.session->is_open());

    // The server really stopped: the statement after the WAITFOR never ran.
    crpc::db::result_set* count_set = nullptr;
    const crpc::status counted = observer.session->query(
        "SELECT COUNT(*) FROM crpc_timeout_marks", {}, &count_set);
    crpc::db::statement_guard count_guard(count_set);
    CRPC_CHECK(static_cast<bool>(counted));
    if (count_set != nullptr) {
        CRPC_CHECK(count_set->next());
        CRPC_CHECK_EQ(std::int64_t{0}, count_set->get_int64(0).value());
    }

    // And the interrupted session still answers.
    crpc::db::result_set* after = nullptr;
    const crpc::status second = it.session->query("SELECT 11 AS n", {}, &after);
    crpc::db::statement_guard after_guard(after);
    CRPC_CHECK(static_cast<bool>(second));
    if (after != nullptr) {
        CRPC_CHECK(after->next());
        CRPC_CHECK_EQ(std::int64_t{11}, after->get_int64(0).value());
    }
}

CRPC_TEST_MAIN()
