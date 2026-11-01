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

#include <cctype>
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
void run_quietly(crpc::db::connection& session, std::string_view sql) {
    crpc::db::result_set* out = nullptr;
    const crpc::status ran = session.query(sql, {}, &out);
    crpc::db::statement_guard guard(out);
    (void)ran;
}

// True when the connection is talking to Jet/ACE. The bridge exists for Access, but the
// same suite has to run against whatever DSN the machine offers, and the two dialects
// disagree on the type names for exactly the columns that carry the interesting cases:
// a long text and a binary blob. Adapting the DDL by driver name is what lets one file
// cover both instead of passing on one and failing on the other by construction.
[[nodiscard]] bool is_jet(const crpc::db::odbc::connection& session) {
    // SQL_DBMS_NAME for Jet comes back as "ACCESS" in capitals, so a case-sensitive
    // search for "Access" silently answered "not Jet" and the DDL below used T-SQL type
    // names on a Jet file -- the failure looked like a driver limitation rather than the
    // missed comparison it was.
    std::string name;
    name.reserve(session.dbms_name().size());
    for (const char c : session.dbms_name()) {
        name.push_back(static_cast<char>(
            std::toupper(static_cast<unsigned char>(c))));
    }
    return name.find("ACCESS") != std::string::npos;
}

// A long text column: Jet spells it MEMO, T-SQL has VARCHAR(MAX).
[[nodiscard]] const char* long_text_type(const crpc::db::odbc::connection& session) {
    return is_jet(session) ? "MEMO" : "VARCHAR(MAX)";
}

// A binary column: Jet spells it LONGBINARY, T-SQL has VARBINARY(MAX).
[[nodiscard]] const char* long_binary_type(const crpc::db::odbc::connection& session) {
    return is_jet(session) ? "LONGBINARY" : "VARBINARY(MAX)";
}

// A floating point column: Jet understands DOUBLE, T-SQL needs FLOAT (bare DOUBLE is a
// syntax error there).
[[nodiscard]] const char* double_type(const crpc::db::odbc::connection& session) {
    return is_jet(session) ? "DOUBLE" : "FLOAT";
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
            std::string("CREATE TABLE crpc_odbc (id INTEGER, name VARCHAR(50), amount ") +
                double_type(*it.session) + ", extra VARCHAR(50))",
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
            std::string("CREATE TABLE crpc_memo (id INTEGER, body ") +
                long_text_type(*it.session) + ")",
            {}, &out);
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
            std::string("CREATE TABLE crpc_bin (id INTEGER, body ") +
                long_binary_type(*it.session) + ")",
            {}, &out);
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

// Counts the rows of a table the caller created. Used to tell a real rollback from a
// transaction flag that merely reports itself closed.
[[nodiscard]] std::int64_t count_rows(crpc::db::connection& session, const char* table) {
    crpc::db::result_set* out = nullptr;
    const std::string sql = std::string("SELECT COUNT(*) FROM ") + table;
    const crpc::status selected = session.query(sql, {}, &out);
    crpc::db::statement_guard guard(out);
    if (!selected || guard.get() == nullptr || !guard->next()) {
        return -1;
    }
    const auto value = guard->get_int64(0);
    return value ? value.value() : -1;
}

void insert_one(crpc::db::connection& session, std::int64_t id) {
    crpc::db::parameter params[1] = {crpc::db::parameter::from_int64(id)};
    crpc::db::result_set* out = nullptr;
    const crpc::status inserted = session.query(
        "INSERT INTO crpc_txn (id) VALUES (?)", crpc::span<const crpc::db::parameter>(params, 1),
        &out);
    crpc::db::statement_guard guard(out);
    CRPC_CHECK(static_cast<bool>(inserted));
    if (!inserted) {
        report("insert", inserted.error());
    }
}

void create_txn_table(crpc::db::connection& session) {
    run_quietly(session, "DROP TABLE crpc_txn");
    crpc::db::result_set* out = nullptr;
    const crpc::status created =
        session.query("CREATE TABLE crpc_txn (id INTEGER NOT NULL PRIMARY KEY)", {}, &out);
    crpc::db::statement_guard guard(out);
    CRPC_CHECK(static_cast<bool>(created));
    if (!created) {
        report("create txn table", created.error());
    }
}

// `in_transaction()` used to return a hard false no matter what the connection was doing,
// so `db::pool` never rolled a dirty session back before handing it to the next caller --
// the next caller inherited an open transaction and could commit another caller's work.
// The flag alone is not enough to prove the fix: the driver has to really be in manual
// commit, or `rollback` below would discard nothing.
CRPC_TEST(odbc_it_transaction_state_follows_the_connection) {
    (void)require_bridge();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }
    create_txn_table(*it.session);

    CRPC_CHECK_FALSE(it.session->in_transaction());

    const crpc::status begun = it.session->begin();
    if (!begun) {
        report("begin", begun.error());
    }
    CRPC_CHECK(static_cast<bool>(begun));
    CRPC_CHECK(it.session->in_transaction());

    // A second begin must be refused rather than silently reported as a nested transaction
    // that does not exist. The transaction that is already open stays open.
    const crpc::status again = it.session->begin();
    CRPC_CHECK_FALSE(static_cast<bool>(again));
    if (!again) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                      static_cast<int>(again.error().code));
    }
    CRPC_CHECK(it.session->in_transaction());

    const crpc::status committed = it.session->commit();
    if (!committed) {
        report("commit", committed.error());
    }
    CRPC_CHECK(static_cast<bool>(committed));
    CRPC_CHECK_FALSE(it.session->in_transaction());

    CRPC_CHECK(static_cast<bool>(it.session->begin()));
    CRPC_CHECK(it.session->in_transaction());
    const crpc::status rolled = it.session->rollback();
    if (!rolled) {
        report("rollback", rolled.error());
    }
    CRPC_CHECK(static_cast<bool>(rolled));
    CRPC_CHECK_FALSE(it.session->in_transaction());

    run_quietly(*it.session, "DROP TABLE crpc_txn");
}

CRPC_TEST(odbc_it_commit_persists_the_work) {
    (void)require_bridge();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }
    create_txn_table(*it.session);

    CRPC_CHECK(static_cast<bool>(it.session->begin()));
    insert_one(*it.session, 1);
    // Inside the transaction the row is visible to this session.
    CRPC_CHECK_EQ(std::int64_t{1}, count_rows(*it.session, "crpc_txn"));
    const crpc::status committed = it.session->commit();
    if (!committed) {
        report("commit", committed.error());
    }
    CRPC_CHECK(static_cast<bool>(committed));
    CRPC_CHECK_EQ(std::int64_t{1}, count_rows(*it.session, "crpc_txn"));

    run_quietly(*it.session, "DROP TABLE crpc_txn");
}

CRPC_TEST(odbc_it_rollback_discards_the_work) {
    (void)require_bridge();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }
    create_txn_table(*it.session);

    CRPC_CHECK(static_cast<bool>(it.session->begin()));
    insert_one(*it.session, 1);
    CRPC_CHECK_EQ(std::int64_t{1}, count_rows(*it.session, "crpc_txn"));
    const crpc::status rolled = it.session->rollback();
    if (!rolled) {
        report("rollback", rolled.error());
    }
    CRPC_CHECK(static_cast<bool>(rolled));
    // Zero, not one: the row was written inside the transaction, so a driver still in
    // autocommit would leave it behind.
    CRPC_CHECK_EQ(std::int64_t{0}, count_rows(*it.session, "crpc_txn"));

    run_quietly(*it.session, "DROP TABLE crpc_txn");
}

// Closing with a transaction open must not leave the work behind. `SQLDisconnect` is
// specified to roll back, but a pool that drops the `connection` is the case that decides
// whether an abandoned `begin()` can reach the file.
CRPC_TEST(odbc_it_close_rolls_back_an_open_transaction) {
    (void)require_bridge();
    {
        it_session it;
        if (!it.ok()) {
            CRPC_CHECK(it.ok());
            return;
        }
        create_txn_table(*it.session);
        CRPC_CHECK(static_cast<bool>(it.session->begin()));
        insert_one(*it.session, 1);
        CRPC_CHECK_EQ(std::int64_t{1}, count_rows(*it.session, "crpc_txn"));
    }
    it_session reopened;
    if (!reopened.ok()) {
        CRPC_CHECK(reopened.ok());
        return;
    }
    CRPC_CHECK_EQ(std::int64_t{0}, count_rows(*reopened.session, "crpc_txn"));
    run_quietly(*reopened.session, "DROP TABLE crpc_txn");
}

// The defect this exists for was in the pool, not in the connection: `prepare_for_reuse`
// asks `in_transaction()` and relies on the answer. Testing only the connection leaves the
// consumer -- the line that actually decides whether a dirty session is handed on --
// untested, and a change that returns the right flag while the pool ignores it would stay
// green. So this drives the whole loop: borrow, leave a transaction open, release, borrow
// again, and read what the second borrower can see.
CRPC_TEST(odbc_it_pool_rolls_back_a_session_borrowed_dirty) {
    (void)require_bridge();
    {
        it_session setup;
        if (!setup.ok()) {
            CRPC_CHECK(setup.ok());
            return;
        }
        create_txn_table(*setup.session);
    }

    crpc::db::odbc::connection_options options;
    options.connection_string = env_or("CRPC_ODBC_CONNECTION", "");
    options.login_timeout_seconds = 15;
    crpc::db::odbc::connection_factory factory(std::move(options));

    crpc::db::pool_options settings;
    settings.min_size = 0;
    settings.max_size = 1;
    settings.max_idle = 1;
    // The ping round trip proves nothing here; this keeps the case about reuse.
    settings.validate_on_borrow = false;
    // Both timers would retire the idle connection between the two leases if the machine
    // stalled, and `opened()+1` would then read +2 for a reason that has nothing to do
    // with the defect. Zero disables them (pool.cpp: expired()).
    settings.idle_timeout = std::chrono::nanoseconds{0};
    settings.max_lifetime = std::chrono::nanoseconds{0};
    crpc::db::pool sessions(settings, factory);

    const auto opened_before = sessions.opened();
    {
        auto lease = sessions.acquire();
        if (!lease) {
            report("acquire", lease.error());
        }
        CRPC_CHECK(static_cast<bool>(lease));
        if (!lease) {
            return;
        }
        crpc::db::connection* session = lease.value().get();
        CRPC_CHECK(session != nullptr);
        if (session == nullptr) {
            return;
        }
        CRPC_CHECK(static_cast<bool>(session->begin()));
        insert_one(*session, 1);
        CRPC_CHECK_EQ(std::int64_t{1}, count_rows(*session, "crpc_txn"));
        CRPC_CHECK(session->in_transaction());
        // Dropped with the transaction still open and nothing committed.
    }
    {
        auto lease = sessions.acquire();
        if (!lease) {
            report("acquire after release", lease.error());
        }
        CRPC_CHECK(static_cast<bool>(lease));
        if (!lease) {
            return;
        }
        crpc::db::connection* session = lease.value().get();
        CRPC_CHECK(session != nullptr);
        if (session == nullptr) {
            return;
        }
        // Zero, not one: the release rolled the abandoned transaction back before the
        // connection went back to the idle set.
        CRPC_CHECK_EQ(std::int64_t{0}, count_rows(*session, "crpc_txn"));
        CRPC_CHECK_FALSE(session->in_transaction());
        // And it was the same connection, so this really did exercise the reuse path
        // rather than a fresh open that would have looked clean either way.
        CRPC_CHECK_EQ(opened_before + 1, sessions.opened());
        run_quietly(*session, "DROP TABLE crpc_txn");
    }
}

CRPC_TEST_MAIN()