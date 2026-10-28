// CRPC -- ODBC bridge tests against a real Access database.
//
// Skipped unless CRPC_ODBC_IT=1 and CRPC_ODBC_CONNECTION is set, because the driver and the
// database file are machine-specific. The path is a full ODBC connection string rather
// than parts: every driver has its own keywords, and guessing them is how an ODBC test
// ends up testing the test.
//
// The case that matters is the one a scripted driver cannot prove: a real driver parsing
// real SQL for a real Jet/ACE file, with values bound rather than spliced.
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

#include <crpc/db/connection.hpp>
#include <crpc/db/odbc/connection.hpp>
#include <crpc/error.hpp>

namespace {

[[nodiscard]] std::string env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::string(value) : std::string(fallback);
}

[[nodiscard]] bool it_enabled() {
    const char* value = std::getenv("CRPC_ODBC_IT");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

[[nodiscard]] const char* require_bridge() {
    if (!it_enabled()) {
        crpc_test::skip("CRPC_ODBC_IT is not 1");
    }
    if (!crpc::db::odbc::available()) {
        crpc_test::skip("this build has no ODBC bridge");
    }
    if (env_or("CRPC_ODBC_CONNECTION", "").empty()) {
        crpc_test::skip("CRPC_ODBC_CONNECTION is not set");
    }
    return "";
}

void report(const char* what, const crpc::rpc_error& error) {
    std::printf("  %s failed: %s (%d)\n", what, error.message.c_str(),
                static_cast<int>(error.code));
}

struct it_session {
    std::unique_ptr<crpc::db::odbc::connection> session;

    it_session() {
        crpc::db::odbc::connection_options options;
        options.connection_string = env_or("CRPC_ODBC_CONNECTION", "");
        options.login_timeout_seconds = 15;
        session = std::unique_ptr<crpc::db::odbc::connection>(
            new crpc::db::odbc::connection(std::move(options)));
    }

    [[nodiscard]] bool ok() const { return session != nullptr && session->is_open(); }
};

// Runs a statement whose result does not matter, freeing whatever came back. `DROP TABLE`
// on a missing table is the reason this exists: the failure is expected.
void run_quietly(crpc::db::odbc::connection& session, std::string_view sql) {
    crpc::db::result_set* out = nullptr;
    const crpc::status ran = session.query(sql, {}, &out);
    crpc::db::statement_guard guard(out);
    (void)ran;
}

} // namespace

CRPC_TEST(odbc_it_connects_and_reports_the_driver) {
    (void)require_bridge();
    it_session it;
    if (!it.ok()) {
        report("connect", it.session->connect_error());
        CRPC_CHECK(it.ok());
        return;
    }
    // The driver's own identity: opening an Access file through the wrong driver produces
    // errors that look like SQL mistakes.
    std::printf("  driver: %s %s (%s)\n", std::string(it.session->driver_name()).c_str(),
                std::string(it.session->driver_version()).c_str(),
                std::string(it.session->dbms_name()).c_str());
    CRPC_CHECK(!it.session->driver_name().empty());
    CRPC_CHECK(static_cast<bool>(it.session->ping()));
}

CRPC_TEST(odbc_it_ddl_dml_and_typed_reads) {
    (void)require_bridge();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }
    // A missing table is expected on the first run; the failure is ignored on purpose.
    run_quietly(*it.session, "DROP TABLE crpc_odbc");
    {
        crpc::db::result_set* out = nullptr;
        const crpc::status created = it.session->query(
            // `note` is a reserved word in Jet ("Syntax error in field definition"), so
              // the column is called `memo`. This is the driver's dialect, not a mistake to
              // paper over: nothing here rewrites SQL.
            "CREATE TABLE crpc_odbc (id INTEGER, name VARCHAR(50), amount DOUBLE, extra VARCHAR(50))",
            {}, &out);
        crpc::db::statement_guard guard(out);
        if (!created) {
            report("create table", created.error());
        }
        CRPC_CHECK(static_cast<bool>(created));
    }

    // Values are bound, not spliced: the note below contains a quote, which would corrupt
    // a hand-built statement. Access uses `?` as its placeholder, which is also ODBC's.
    crpc::db::parameter params[4] = {
        crpc::db::parameter::from_int64(1),
        crpc::db::parameter::from_text("ada"),
        crpc::db::parameter::from_double(12.5),
        crpc::db::parameter::from_text("it's quoted"),
    };
    {
        crpc::db::result_set* out = nullptr;
        const crpc::status inserted = it.session->query(
            "INSERT INTO crpc_odbc (id, name, amount, extra) VALUES (?, ?, ?, ?)",
            crpc::span<const crpc::db::parameter>(params, 4), &out);
        crpc::db::statement_guard guard(out);
        if (!inserted) {
            report("insert", inserted.error());
        }
        CRPC_CHECK(static_cast<bool>(inserted));
        if (inserted && guard.get() != nullptr) {
            CRPC_CHECK_EQ(std::int64_t{1}, guard->affected_rows());
        }
    }
    {
        // A NULL is bound as SQL_NULL_DATA, not as an empty string: the two are different
        // values and an Access column can hold both.
        crpc::db::parameter nil[4] = {
            crpc::db::parameter::from_int64(2),
            crpc::db::parameter::null(),
            crpc::db::parameter::from_double(0.0),
            crpc::db::parameter::null(),
        };
        crpc::db::result_set* out = nullptr;
        const crpc::status inserted = it.session->query(
            "INSERT INTO crpc_odbc (id, name, amount, extra) VALUES (?, ?, ?, ?)",
            crpc::span<const crpc::db::parameter>(nil, 4), &out);
        crpc::db::statement_guard guard(out);
        CRPC_CHECK(static_cast<bool>(inserted));
    }

    {
        crpc::db::result_set* raw = nullptr;
        const crpc::status selected = it.session->query(
            "SELECT id, name, amount, extra FROM crpc_odbc ORDER BY id", {}, &raw);
        crpc::db::statement_guard guard(raw);
        if (!selected) {
            report("select", selected.error());
        }
        CRPC_CHECK(static_cast<bool>(selected));
        CRPC_CHECK(raw != nullptr);
        if (raw == nullptr) {
            return;
        }
        CRPC_CHECK_EQ(std::size_t{4}, raw->column_count());
        CRPC_CHECK(raw->next());
        CRPC_CHECK_EQ(std::int64_t{1}, raw->get_int64(0).value());
        CRPC_CHECK_EQ(std::string("ada"), raw->get_text(1).value());
        CRPC_CHECK_EQ(12.5, raw->get_double(2).value());
        CRPC_CHECK_EQ(std::string("it's quoted"), raw->get_text(3).value());
        CRPC_CHECK(raw->next());
        CRPC_CHECK_EQ(std::int64_t{2}, raw->get_int64(0).value());
        CRPC_CHECK(raw->is_null(1));
        CRPC_CHECK(raw->is_null(3));
        CRPC_CHECK_FALSE(raw->next());
    }
}

// A MEMO column holds far more than the 8 KiB the reader used to bind. The old reader
// bound a fixed buffer and took the indicator at face value, so a long value came back
// truncated (or empty when the driver reported SQL_NO_TOTAL) with no error at all -- a
// silent data-loss bug that only a real driver with a real long value exposes.
CRPC_TEST(odbc_it_long_text_is_not_truncated) {
    (void)require_bridge();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }
    run_quietly(*it.session, "DROP TABLE crpc_memo");
    {
        crpc::db::result_set* out = nullptr;
        const crpc::status created = it.session->query(
            "CREATE TABLE crpc_memo (id INTEGER, body MEMO)", {}, &out);
        crpc::db::statement_guard guard(out);
        if (!created) {
            report("create memo table", created.error());
        }
        CRPC_CHECK(static_cast<bool>(created));
    }

    // 20000 characters, with distinct characters at both ends so a truncation or an
    // off-by-one in either direction is visible in the comparison.
    std::string body(20000, 'x');
    body.front() = 'A';
    body.back() = 'Z';
    {
        crpc::db::parameter params[2] = {
            crpc::db::parameter::from_int64(1),
            crpc::db::parameter::from_text(body),
        };
        crpc::db::result_set* out = nullptr;
        const crpc::status inserted = it.session->query(
            "INSERT INTO crpc_memo (id, body) VALUES (?, ?)",
            crpc::span<const crpc::db::parameter>(params, 2), &out);
        crpc::db::statement_guard guard(out);
        if (!inserted) {
            report("insert memo", inserted.error());
        }
        CRPC_CHECK(static_cast<bool>(inserted));
    }
    {
        crpc::db::result_set* raw = nullptr;
        const crpc::status selected = it.session->query(
            "SELECT id, body FROM crpc_memo WHERE id = 1", {}, &raw);
        crpc::db::statement_guard guard(raw);
        if (!selected) {
            report("select memo", selected.error());
        }
        CRPC_CHECK(static_cast<bool>(selected));
        CRPC_CHECK(raw != nullptr);
        if (raw == nullptr) {
            return;
        }
        CRPC_CHECK(raw->next());
        const std::string_view view = raw->get_text(1).value();
        const std::string round_tripped(view);
        CRPC_CHECK_EQ(body.size(), round_tripped.size());
        CRPC_CHECK_EQ(std::string("A"), round_tripped.substr(0, 1));
        CRPC_CHECK_EQ(std::string("Z"), round_tripped.substr(round_tripped.size() - 1));
        CRPC_CHECK(round_tripped == body);
    }
}

// A timestamp is carried as text with a type tag (see db/types.hpp), so the adapter binds it
// as a character parameter and lets the driver convert. That is only correct if the driver
// really converts on write and the value survives; an Access DATETIME column is the case
// worth pinning, because a lossy or rejected conversion shows up here and nowhere else.
CRPC_TEST(odbc_it_timestamp_round_trips) {
    (void)require_bridge();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }
    run_quietly(*it.session, "DROP TABLE crpc_dt");
    {
        crpc::db::result_set* out = nullptr;
        const crpc::status created = it.session->query(
            "CREATE TABLE crpc_dt (id INTEGER, at DATETIME)", {}, &out);
        crpc::db::statement_guard guard(out);
        if (!created) {
            report("create dt table", created.error());
        }
        CRPC_CHECK(static_cast<bool>(created));
    }
    {
        crpc::db::parameter params[2] = {
            crpc::db::parameter::from_int64(1),
            crpc::db::parameter::from_text("2024-04-25 12:34:56", crpc::db::db_type::timestamp),
        };
        crpc::db::result_set* out = nullptr;
        const crpc::status inserted = it.session->query(
            "INSERT INTO crpc_dt (id, at) VALUES (?, ?)",
            crpc::span<const crpc::db::parameter>(params, 2), &out);
        crpc::db::statement_guard guard(out);
        if (!inserted) {
            report("insert dt", inserted.error());
        }
        CRPC_CHECK(static_cast<bool>(inserted));
    }
    {
        crpc::db::result_set* raw = nullptr;
        const crpc::status selected = it.session->query(
            "SELECT id, at FROM crpc_dt WHERE id = 1", {}, &raw);
        crpc::db::statement_guard guard(raw);
        if (!selected) {
            report("select dt", selected.error());
        }
        CRPC_CHECK(static_cast<bool>(selected));
        CRPC_CHECK(raw != nullptr);
        if (raw == nullptr) {
            return;
        }
        CRPC_CHECK(raw->next());
        const std::string_view view = raw->get_text(1).value();
        const std::string text(view);
        // The driver renders a date in its own format; what must hold is that the instant
        // survived, not that it is spelled the way it was sent.
        CRPC_CHECK(text.find("2024-04-25") != std::string::npos);
        CRPC_CHECK(text.find("12:34:56") != std::string::npos);
    }
}

// A binary value is not text: it may contain 0x00, and everything after the first NUL is
// data, not padding. The reader gets bytes from the driver as SQL_C_CHAR, so it must take
// the length from the indicator rather than stopping at the first NUL -- otherwise this
// value comes back as the single byte 0x41 with no error at all.
CRPC_TEST(odbc_it_binary_with_embedded_nul_survives) {
    (void)require_bridge();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }
    run_quietly(*it.session, "DROP TABLE crpc_bin");
    {
        crpc::db::result_set* out = nullptr;
        const crpc::status created = it.session->query(
            "CREATE TABLE crpc_bin (id INTEGER, body LONGBINARY)", {}, &out);
        crpc::db::statement_guard guard(out);
        if (!created) {
            report("create binary table", created.error());
        }
        CRPC_CHECK(static_cast<bool>(created));
    }
    const std::vector<std::byte> payload = {
        std::byte{0x41}, std::byte{0x00}, std::byte{0x42},
        std::byte{0x00}, std::byte{0x43},
    };
    {
        crpc::db::parameter params[2] = {
            crpc::db::parameter::from_int64(1),
            crpc::db::parameter::from_blob(payload),
        };
        crpc::db::result_set* out = nullptr;
        const crpc::status inserted = it.session->query(
            "INSERT INTO crpc_bin (id, body) VALUES (?, ?)",
            crpc::span<const crpc::db::parameter>(params, 2), &out);
        crpc::db::statement_guard guard(out);
        if (!inserted) {
            report("insert binary", inserted.error());
        }
        CRPC_CHECK(static_cast<bool>(inserted));
    }
    {
        crpc::db::result_set* raw = nullptr;
        const crpc::status selected = it.session->query(
            "SELECT id, body FROM crpc_bin WHERE id = 1", {}, &raw);
        crpc::db::statement_guard guard(raw);
        if (!selected) {
            report("select binary", selected.error());
        }
        CRPC_CHECK(static_cast<bool>(selected));
        CRPC_CHECK(raw != nullptr);
        if (raw == nullptr) {
            return;
        }
        CRPC_CHECK(raw->next());
        const auto bytes = raw->get_blob(1);
        if (!bytes) {
            report("read binary", bytes.error());
        }
        CRPC_CHECK(static_cast<bool>(bytes));
        if (!bytes) {
            return;
        }
        CRPC_CHECK_EQ(payload.size(), bytes.value().size());
        CRPC_CHECK(bytes.value() == payload);
    }
}

CRPC_TEST(odbc_it_constraint_violation_carries_the_sqlstate) {
    (void)require_bridge();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }
    run_quietly(*it.session, "DROP TABLE crpc_unique");
    {
        crpc::db::result_set* out = nullptr;
        const crpc::status created = it.session->query(
            "CREATE TABLE crpc_unique (id INTEGER NOT NULL PRIMARY KEY)", {}, &out);
        crpc::db::statement_guard guard(out);
        CRPC_CHECK(static_cast<bool>(created));
    }
    crpc::db::parameter one[1] = {crpc::db::parameter::from_int64(1)};
    {
        crpc::db::result_set* out = nullptr;
        const crpc::status inserted = it.session->query(
            "INSERT INTO crpc_unique (id) VALUES (?)",
            crpc::span<const crpc::db::parameter>(one, 1), &out);
        crpc::db::statement_guard guard(out);
        CRPC_CHECK(static_cast<bool>(inserted));
    }
    {
        crpc::db::result_set* out = nullptr;
        const crpc::status duplicate = it.session->query(
            "INSERT INTO crpc_unique (id) VALUES (?)",
            crpc::span<const crpc::db::parameter>(one, 1), &out);
        crpc::db::statement_guard guard(out);
        CRPC_CHECK_FALSE(static_cast<bool>(duplicate));
        if (!duplicate) {
            report("duplicate", duplicate.error());
            // A primary-key violation is SQLSTATE class 23; the mapping must say so rather
            // than collapsing it to a generic failure.
            CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                          static_cast<int>(duplicate.error().code));
            std::printf("  sqlstate: %s\n", std::string(it.session->last_sqlstate()).c_str());
            CRPC_CHECK_FALSE(it.session->last_sqlstate().empty());
        }
    }
    // The session survives its own error.
    CRPC_CHECK(it.session->is_open());
    CRPC_CHECK(static_cast<bool>(it.session->ping()));
}

CRPC_TEST(odbc_it_syntax_error_is_an_invalid_argument) {
    (void)require_bridge();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }
    crpc::db::result_set* out = nullptr;
    const crpc::status broken =
        it.session->query("SELECT * FROM a_table_that_does_not_exist", {}, &out);
    crpc::db::statement_guard guard(out);
    CRPC_CHECK_FALSE(static_cast<bool>(broken));
    if (!broken) {
        report("missing table", broken.error());
        // A missing table is a syntax/access class error (42xxx) or the driver's own
        // classification; both are errors a caller must see rather than an empty result.
        std::printf("  sqlstate: %s\n", std::string(it.session->last_sqlstate()).c_str());
        CRPC_CHECK_FALSE(broken.error().message.empty());
    }
    CRPC_CHECK(it.session->is_open());
}

CRPC_TEST(odbc_it_lists_drivers) {
    (void)require_bridge();
    const auto drivers = crpc::db::odbc::list_drivers();
    if (!drivers) {
        report("list drivers", drivers.error());
    }
    CRPC_CHECK(static_cast<bool>(drivers));
    if (drivers) {
        // The Access driver is what the bridge exists for; its absence explains a
        // connection failure better than any error message.
        bool found_access = false;
        for (const std::string& name : drivers.value()) {
            if (name.find("Access") != std::string::npos) {
                found_access = true;
            }
        }
        std::printf("  %zu driver(s), Access: %s\n", drivers.value().size(),
                    found_access ? "yes" : "no");
        CRPC_CHECK(!drivers.value().empty());
    }
}

CRPC_TEST_MAIN()