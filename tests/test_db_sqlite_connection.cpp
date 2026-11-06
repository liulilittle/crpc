// CRPC -- SQLite backend unit tests.
//
// These run against a real SQLite engine, always in memory, so they need no file and no
// server. The expected values are SQLite's own: the schema, the row counts and the
// extended error codes below were observed against the engine, not assumed.
//
// The cases that a wrapper script could not prove are the ones that matter here: a bound
// parameter never reaching the SQL text, an embedded NUL surviving a round trip,
// `in_transaction()` reading the engine's autocommit state rather than a local flag, and a
// rollback really discarding rows.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/db/connection.hpp>
#include <crpc/db/sqlite/connection.hpp>
#include <crpc/db/types.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace {

using crpc::db::parameter;
using crpc::db::result_set;
using crpc::db::sqlite::connection;
using crpc::db::sqlite::connection_options;

[[nodiscard]] crpc::span<const parameter> view_of(const std::vector<parameter>& params) {
    return crpc::span<const parameter>(params.data(), params.size());
}

[[nodiscard]] crpc::status run(connection& session, std::string_view sql,
                               const std::vector<parameter>& params = {}) {
    return session.execute(sql, view_of(params));
}

// A cursor plus the status that produced it. The status is kept because `query` can
// succeed and still leave `cursor == nullptr` is never expected, so a test asserts both.
struct query_outcome {
    std::unique_ptr<result_set> cursor;
    crpc::status state;
};

[[nodiscard]] query_outcome run_query(connection& session, std::string_view sql,
                                      const std::vector<parameter>& params = {}) {
    result_set* raw = nullptr;
    crpc::status state = session.query(sql, view_of(params), &raw);
    return query_outcome{std::unique_ptr<result_set>(raw), std::move(state)};
}

[[nodiscard]] query_outcome run_streaming(connection& session, std::string_view sql,
                                          const std::vector<parameter>& params = {}) {
    result_set* raw = nullptr;
    crpc::status state = session.query_streaming(sql, view_of(params), &raw);
    return query_outcome{std::unique_ptr<result_set>(raw), std::move(state)};
}

// The backend is compiled in on the machines this suite is expected to run on; a build
// without it skips rather than failing, matching the other backend suites.
void require_backend() {
    if (!crpc::db::sqlite::available()) {
        crpc_test::skip("this build has no SQLite backend");
    }
}

[[nodiscard]] bool is_code(const crpc::status& state, crpc::rpc_code code) {
    return static_cast<int>(state.code()) == static_cast<int>(code);
}

[[nodiscard]] std::string metadata_or_empty(const crpc::status& state, const char* key) {
    const std::string* value = state.error().metadata.find(key);
    return value != nullptr ? *value : std::string();
}

// A text value built with an explicit length, so an embedded NUL is part of the value.
[[nodiscard]] std::string text_with_nul() { return std::string("a\0b", 3); }

} // namespace

// --- availability ------------------------------------------------------------

CRPC_TEST(sqlite_backend_is_available_and_reports_a_version) {
    require_backend();
    // The version string comes from the engine itself; a backend that answered with an
    // empty string would make every bug report ambiguous.
    const std::string_view version = crpc::db::sqlite::version();
    CRPC_CHECK_FALSE(version.empty());
    CRPC_CHECK(version != "unavailable");
    // Answering the question must not require opening a database.
    CRPC_CHECK(crpc::db::sqlite::available());
}

CRPC_TEST(error_codes_map_to_library_codes) {
    // The primary code is the low byte of an extended code, so both spellings agree.
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::ok),
                  static_cast<int>(crpc::db::sqlite::map_errcode(0)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(crpc::db::sqlite::map_errcode(19)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(crpc::db::sqlite::map_errcode(2067)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable),
                  static_cast<int>(crpc::db::sqlite::map_errcode(5)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::permission_denied),
                  static_cast<int>(crpc::db::sqlite::map_errcode(8)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(crpc::db::sqlite::map_errcode(1)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::resource_exhausted),
                  static_cast<int>(crpc::db::sqlite::map_errcode(13)));
}

// --- opening and basic statements --------------------------------------------

CRPC_TEST(in_memory_connection_opens_and_pings) {
    require_backend();
    connection session{connection_options{}};
    CRPC_REQUIRE(session.is_open());
    CRPC_CHECK(session.connect_error().ok());
    CRPC_CHECK(session.ping().ok());
    // An in-memory database reports the "memory" journal mode; WAL cannot apply to it.
    CRPC_CHECK_EQ(std::string_view("memory"), session.journal_mode());
    CRPC_CHECK_FALSE(session.in_transaction());
}

CRPC_TEST(create_insert_select_round_trip) {
    require_backend();
    connection session{connection_options{}};
    CRPC_REQUIRE(session.is_open());
    CRPC_REQUIRE(run(session, "CREATE TABLE people(id INTEGER PRIMARY KEY, name TEXT, age INTEGER)")
                     .ok());
    CRPC_REQUIRE(run(session, "INSERT INTO people(id, name, age) VALUES(?, ?, ?)",
                     {parameter::from_int64(1), parameter::from_text("Ada"),
                      parameter::from_int64(36)})
                     .ok());
    auto rows = run_query(session, "SELECT id, name, age FROM people");
    CRPC_REQUIRE(rows.state.ok());
    CRPC_REQUIRE(rows.cursor != nullptr);
    CRPC_CHECK_EQ(static_cast<std::size_t>(3), rows.cursor->column_count());
    CRPC_CHECK_EQ(std::string_view("id"), rows.cursor->column(0).name);
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::int64),
                  static_cast<int>(rows.cursor->column(0).type));
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::text),
                  static_cast<int>(rows.cursor->column(1).type));
    CRPC_REQUIRE(rows.cursor->next());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(1), rows.cursor->get_int64(0).value());
    CRPC_CHECK_EQ(std::string_view("Ada"), rows.cursor->get_text(1).value());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(36), rows.cursor->get_int64(2).value());
    CRPC_CHECK_FALSE(rows.cursor->next());
    // A SELECT reports no affected-row count.
    CRPC_CHECK_EQ(static_cast<std::int64_t>(-1), rows.cursor->affected_rows());
}

CRPC_TEST(affected_rows_are_reported_for_dml) {
    require_backend();
    connection session{connection_options{}};
    CRPC_REQUIRE(session.is_open());
    CRPC_REQUIRE(run(session, "CREATE TABLE d(x INTEGER)").ok());
    auto inserted = run_query(session, "INSERT INTO d VALUES(1),(2),(3)");
    CRPC_REQUIRE(inserted.state.ok());
    CRPC_REQUIRE(inserted.cursor != nullptr);
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), inserted.cursor->column_count());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(3), inserted.cursor->affected_rows());

    auto updated = run_query(session, "UPDATE d SET x = x + 1 WHERE x > ?",
                             {parameter::from_int64(1)});
    CRPC_REQUIRE(updated.state.ok());
    CRPC_REQUIRE(updated.cursor != nullptr);
    CRPC_CHECK_EQ(static_cast<std::int64_t>(2), updated.cursor->affected_rows());
}

// --- parameters --------------------------------------------------------------

CRPC_TEST(parameters_are_bound_not_spliced_into_sql) {
    require_backend();
    connection session{connection_options{}};
    CRPC_REQUIRE(session.is_open());
    CRPC_REQUIRE(run(session, "CREATE TABLE users(name TEXT)").ok());

    // If the adapter spliced this into the statement text, the quote would end the
    // literal and the trailing `--` would comment out the rest of the statement: the
    // INSERT would fail or the table would be dropped.
    const std::string attack = "Robert'); DROP TABLE users;--";
    CRPC_REQUIRE(run(session, "INSERT INTO users(name) VALUES(?)",
                     {parameter::from_text(attack)})
                     .ok());

    auto rows = run_query(session, "SELECT name FROM users");
    CRPC_REQUIRE(rows.state.ok());
    CRPC_REQUIRE(rows.cursor != nullptr);
    CRPC_REQUIRE(rows.cursor->next());
    CRPC_CHECK_EQ(std::string_view(attack), rows.cursor->get_text(0).value());

    // The table still exists and still holds exactly one row.
    auto count = run_query(session, "SELECT count(*) FROM users");
    CRPC_REQUIRE(count.state.ok());
    CRPC_REQUIRE(count.cursor != nullptr);
    CRPC_REQUIRE(count.cursor->next());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(1), count.cursor->get_int64(0).value());
}

CRPC_TEST(bound_text_keeps_an_embedded_nul) {
    require_backend();
    connection session{connection_options{}};
    CRPC_REQUIRE(session.is_open());
    CRPC_REQUIRE(run(session, "CREATE TABLE t(v TEXT)").ok());

    const std::string value = text_with_nul();
    CRPC_REQUIRE(run(session, "INSERT INTO t(v) VALUES(?)",
                     {parameter::from_text(value)})
                     .ok());

    auto rows = run_query(session, "SELECT v FROM t");
    CRPC_REQUIRE(rows.state.ok());
    CRPC_REQUIRE(rows.cursor != nullptr);
    CRPC_REQUIRE(rows.cursor->next());
    const std::string_view got = rows.cursor->get_text(0).value();
    CRPC_CHECK_EQ(static_cast<std::size_t>(3), got.size());
    CRPC_CHECK_EQ(0, std::memcmp(got.data(), "a\0b", 3));

    // The length must also come from `char()` rather than from a NUL scan.
    auto literal = run_query(session, "SELECT char(97, 0, 98)");
    CRPC_REQUIRE(literal.state.ok());
    CRPC_REQUIRE(literal.cursor != nullptr);
    CRPC_REQUIRE(literal.cursor->next());
    const std::string_view produced = literal.cursor->get_text(0).value();
    CRPC_CHECK_EQ(static_cast<std::size_t>(3), produced.size());
    CRPC_CHECK_EQ(0, std::memcmp(produced.data(), "a\0b", 3));
}

CRPC_TEST(parameter_count_must_match_the_statement) {
    require_backend();
    connection session{connection_options{}};
    CRPC_REQUIRE(session.is_open());

    auto too_few = run_query(session, "SELECT ?", {});
    CRPC_CHECK_FALSE(too_few.state.ok());
    CRPC_CHECK(is_code(too_few.state, crpc::rpc_code::invalid_argument));
    CRPC_CHECK(too_few.cursor == nullptr);

    auto too_many = run_query(session, "SELECT ?",
                              {parameter::from_int64(1), parameter::from_int64(2)});
    CRPC_CHECK_FALSE(too_many.state.ok());
    CRPC_CHECK(is_code(too_many.state, crpc::rpc_code::invalid_argument));
    CRPC_CHECK(too_many.cursor == nullptr);

    auto none_expected = run_query(session, "SELECT 1", {parameter::from_int64(1)});
    CRPC_CHECK_FALSE(none_expected.state.ok());
    CRPC_CHECK(is_code(none_expected.state, crpc::rpc_code::invalid_argument));
}

CRPC_TEST(only_one_statement_is_accepted_per_call) {
    require_backend();
    connection session{connection_options{}};
    CRPC_REQUIRE(session.is_open());
    // `sqlite3_prepare_v2` would run the first statement and silently drop the second;
    // that is exactly the kind of quiet truncation this adapter refuses.
    auto two = run_query(session, "SELECT 1; SELECT 2");
    CRPC_CHECK_FALSE(two.state.ok());
    CRPC_CHECK(is_code(two.state, crpc::rpc_code::invalid_argument));
    CRPC_CHECK(two.cursor == nullptr);

    auto empty = run_query(session, "");
    CRPC_CHECK_FALSE(empty.state.ok());
    CRPC_CHECK(is_code(empty.state, crpc::rpc_code::invalid_argument));
}

// --- type mapping ------------------------------------------------------------

CRPC_TEST(value_types_map_from_the_engine_storage_class) {
    require_backend();
    connection session{connection_options{}};
    CRPC_REQUIRE(session.is_open());
    CRPC_REQUIRE(run(session,
                     "CREATE TABLE types(i INTEGER, u INTEGER, r REAL, t TEXT, b BLOB, "
                     "flag INTEGER, missing TEXT)")
                     .ok());
    const std::string text = text_with_nul();
    const std::vector<std::byte> blob{std::byte{0x00}, std::byte{0xff}, std::byte{0x7f}};
    CRPC_REQUIRE(run(session,
                     "INSERT INTO types VALUES(?, ?, ?, ?, ?, ?, NULL)",
                     {parameter::from_int64(-9223372036854775807LL - 1),
                      parameter::from_int64(9223372036854775807LL),
                      parameter::from_double(3.5), parameter::from_text(text),
                      parameter::from_blob(blob), parameter::from_int64(1)})
                     .ok());

    auto rows = run_query(session, "SELECT i, u, r, t, b, flag, missing FROM types");
    CRPC_REQUIRE(rows.state.ok());
    CRPC_REQUIRE(rows.cursor != nullptr);
    CRPC_REQUIRE(rows.cursor->next());

    CRPC_CHECK_EQ(std::numeric_limits<std::int64_t>::min(),
                  rows.cursor->get_int64(0).value());
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(9223372036854775807ULL),
                  rows.cursor->get_uint64(1).value());
    CRPC_CHECK_EQ(3.5, rows.cursor->get_double(2).value());
    CRPC_CHECK_EQ(static_cast<std::size_t>(3), rows.cursor->get_text(3).value().size());
    CRPC_CHECK_EQ(0, std::memcmp(rows.cursor->get_text(3).value().data(), "a\0b", 3));
    const std::vector<std::byte> got_blob = rows.cursor->get_blob(4).value();
    CRPC_REQUIRE(got_blob.size() == 3);
    CRPC_CHECK_EQ(static_cast<int>(0x00), static_cast<int>(got_blob[0]));
    CRPC_CHECK_EQ(static_cast<int>(0xff), static_cast<int>(got_blob[1]));
    CRPC_CHECK(rows.cursor->get_bool(5).value());
    CRPC_CHECK(rows.cursor->is_null(6));
    // A NULL is not a zero: asking for an integer has to fail, not answer 0.
    CRPC_CHECK_FALSE(rows.cursor->get_int64(6).ok());
    CRPC_CHECK_FALSE(rows.cursor->get_text(6).ok());
}

CRPC_TEST(column_types_come_from_the_declared_type) {
    require_backend();
    connection session{connection_options{}};
    CRPC_REQUIRE(session.is_open());
    CRPC_REQUIRE(run(session,
                     "CREATE TABLE m(a INTEGER, b REAL, c TEXT, d BLOB, e BOOLEAN, "
                     "f DATETIME, g DECIMAL(10,2))")
                     .ok());
    auto rows = run_query(session, "SELECT a, b, c, d, e, f, g FROM m");
    CRPC_REQUIRE(rows.state.ok());
    CRPC_REQUIRE(rows.cursor != nullptr);
    CRPC_REQUIRE(rows.cursor->column_count() == 7);
    const crpc::db::db_type expected[] = {
        crpc::db::db_type::int64,     crpc::db::db_type::real,   crpc::db::db_type::text,
        crpc::db::db_type::blob,      crpc::db::db_type::boolean, crpc::db::db_type::timestamp,
        crpc::db::db_type::decimal};
    for (std::size_t i = 0; i < 7; ++i) {
        CRPC_CHECK_EQ(static_cast<int>(expected[i]),
                      static_cast<int>(rows.cursor->column(i).type));
    }
    // An expression column has no declared type and is reported as unknown.
    auto expression = run_query(session, "SELECT 1 + 1");
    CRPC_REQUIRE(expression.state.ok());
    CRPC_REQUIRE(expression.cursor != nullptr);
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::unknown),
                  static_cast<int>(expression.cursor->column(0).type));
}

// --- transactions ------------------------------------------------------------

CRPC_TEST(transaction_commit_persists_rows) {
    require_backend();
    connection session{connection_options{}};
    CRPC_REQUIRE(session.is_open());
    CRPC_REQUIRE(run(session, "CREATE TABLE t(x INTEGER)").ok());

    CRPC_CHECK_FALSE(session.in_transaction());
    CRPC_REQUIRE(session.begin().ok());
    // The state must come from the engine: the transaction is open right now, before any
    // statement has run.
    CRPC_CHECK(session.in_transaction());
    CRPC_REQUIRE(run(session, "INSERT INTO t VALUES(?)", {parameter::from_int64(7)}).ok());
    CRPC_REQUIRE(session.commit().ok());
    CRPC_CHECK_FALSE(session.in_transaction());

    auto rows = run_query(session, "SELECT count(*) FROM t");
    CRPC_REQUIRE(rows.state.ok());
    CRPC_REQUIRE(rows.cursor != nullptr);
    CRPC_REQUIRE(rows.cursor->next());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(1), rows.cursor->get_int64(0).value());

    // Committing when nothing is open is a caller error, not a success.
    CRPC_CHECK(is_code(session.commit(), crpc::rpc_code::failed_precondition));
}

CRPC_TEST(transaction_rollback_discards_rows) {
    require_backend();
    connection session{connection_options{}};
    CRPC_REQUIRE(session.is_open());
    CRPC_REQUIRE(run(session, "CREATE TABLE t(x INTEGER)").ok());
    CRPC_REQUIRE(run(session, "INSERT INTO t VALUES(1)").ok());

    CRPC_REQUIRE(session.begin().ok());
    CRPC_CHECK(session.in_transaction());
    CRPC_REQUIRE(run(session, "INSERT INTO t VALUES(2)").ok());
    CRPC_REQUIRE(session.rollback().ok());
    CRPC_CHECK_FALSE(session.in_transaction());

    auto rows = run_query(session, "SELECT count(*) FROM t");
    CRPC_REQUIRE(rows.state.ok());
    CRPC_REQUIRE(rows.cursor != nullptr);
    CRPC_REQUIRE(rows.cursor->next());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(1), rows.cursor->get_int64(0).value());

    // Rolling back nothing is a no-op success: the pool calls it defensively.
    CRPC_CHECK(session.rollback().ok());
}

CRPC_TEST(in_transaction_sees_a_transaction_started_with_raw_sql) {
    require_backend();
    connection session{connection_options{}};
    CRPC_REQUIRE(session.is_open());
    CRPC_REQUIRE(run(session, "CREATE TABLE t(x INTEGER)").ok());

    // A local flag would miss this; `sqlite3_get_autocommit` does not.
    CRPC_REQUIRE(run(session, "BEGIN").ok());
    CRPC_CHECK(session.in_transaction());
    CRPC_REQUIRE(run(session, "INSERT INTO t VALUES(1)").ok());
    CRPC_REQUIRE(run(session, "ROLLBACK").ok());
    CRPC_CHECK_FALSE(session.in_transaction());

    // A failed COMMIT (no transaction is open) must not fabricate a transaction.
    CRPC_CHECK_FALSE(session.commit().ok());
    CRPC_CHECK_FALSE(session.in_transaction());
}

CRPC_TEST(begin_twice_is_rejected_without_disturbing_the_transaction) {
    require_backend();
    connection session{connection_options{}};
    CRPC_REQUIRE(session.is_open());
    CRPC_REQUIRE(run(session, "CREATE TABLE t(x INTEGER)").ok());
    CRPC_REQUIRE(session.begin().ok());
    CRPC_CHECK(is_code(session.begin(), crpc::rpc_code::failed_precondition));
    CRPC_CHECK(session.in_transaction());
    CRPC_REQUIRE(run(session, "INSERT INTO t VALUES(1)").ok());
    CRPC_REQUIRE(session.commit().ok());
}

// --- errors ------------------------------------------------------------------

CRPC_TEST(sql_errors_carry_code_extended_code_and_message) {
    require_backend();
    connection session{connection_options{}};
    CRPC_REQUIRE(session.is_open());

    auto missing = run_query(session, "SELECT * FROM no_such_table");
    CRPC_REQUIRE(!missing.state.ok());
    CRPC_CHECK(missing.cursor == nullptr);
    CRPC_CHECK(is_code(missing.state, crpc::rpc_code::invalid_argument));
    CRPC_CHECK_FALSE(missing.state.message().empty());
    // The raw engine codes are kept, primary and extended separately.
    CRPC_CHECK_EQ(std::string("1"), metadata_or_empty(missing.state, "sqlite_code"));
    CRPC_CHECK_EQ(std::string("1"),
                  metadata_or_empty(missing.state, "sqlite_extended_code"));

    CRPC_REQUIRE(run(session, "CREATE TABLE u(v TEXT UNIQUE)").ok());
    CRPC_REQUIRE(run(session, "INSERT INTO u VALUES('a')").ok());
    auto duplicate = run_query(session, "INSERT INTO u VALUES('a')");
    CRPC_REQUIRE(!duplicate.state.ok());
    CRPC_CHECK(is_code(duplicate.state, crpc::rpc_code::failed_precondition));
    // SQLITE_CONSTRAINT_UNIQUE is 2067; its primary code is 19.
    CRPC_CHECK_EQ(std::string("19"), metadata_or_empty(duplicate.state, "sqlite_code"));
    CRPC_CHECK_EQ(std::string("2067"),
                  metadata_or_empty(duplicate.state, "sqlite_extended_code"));

    // The connection stays usable after a failed statement.
    CRPC_CHECK(session.ping().ok());
}

// --- streaming ---------------------------------------------------------------

CRPC_TEST(streaming_yields_rows_without_buffering_them) {
    require_backend();
    connection session{connection_options{}};
    CRPC_REQUIRE(session.is_open());
    CRPC_REQUIRE(run(session,
                     "CREATE TABLE s(x INTEGER)").ok());
    CRPC_REQUIRE(run(session,
                     "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c "
                     "WHERE x < 5000) INSERT INTO s SELECT x FROM c")
                     .ok());

    auto stream = run_streaming(session, "SELECT x FROM s ORDER BY x");
    CRPC_REQUIRE(stream.state.ok());
    CRPC_REQUIRE(stream.cursor != nullptr);
    CRPC_CHECK(stream.cursor->is_streaming());
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), stream.cursor->column_count());

    std::int64_t sum = 0;
    std::int64_t rows = 0;
    while (stream.cursor->next()) {
        sum += stream.cursor->get_int64(0).value();
        ++rows;
        // A streaming cursor never holds more than the current row.
        CRPC_CHECK(stream.cursor->rows_buffered() <= 1);
    }
    CRPC_CHECK(stream.cursor->last_error().ok());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(5000), rows);
    CRPC_CHECK_EQ(static_cast<std::int64_t>(5000) * 5001 / 2, sum);
    CRPC_CHECK_EQ(static_cast<std::size_t>(5000), stream.cursor->rows_decoded());
    // Closing is idempotent and leaves the connection usable.
    stream.cursor->close();
    stream.cursor->close();
    stream.cursor.reset();
    CRPC_CHECK(session.ping().ok());
}

CRPC_TEST(streaming_a_statement_without_columns_returns_a_materialised_cursor) {
    require_backend();
    connection session{connection_options{}};
    CRPC_REQUIRE(session.is_open());
    auto ddl = run_streaming(session, "CREATE TABLE t(x INTEGER)");
    CRPC_REQUIRE(ddl.state.ok());
    CRPC_REQUIRE(ddl.cursor != nullptr);
    CRPC_CHECK_FALSE(ddl.cursor->is_streaming());
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), ddl.cursor->column_count());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(0), ddl.cursor->affected_rows());
    CRPC_CHECK_FALSE(ddl.cursor->next());
    ddl.cursor.reset();

    CRPC_CHECK(run(session, "INSERT INTO t VALUES(1)").ok());
    auto insert = run_streaming(session, "INSERT INTO t VALUES(2)");
    CRPC_REQUIRE(insert.state.ok());
    CRPC_REQUIRE(insert.cursor != nullptr);
    CRPC_CHECK_FALSE(insert.cursor->is_streaming());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(1), insert.cursor->affected_rows());
}

CRPC_TEST(max_result_rows_bounds_a_materialised_query) {
    require_backend();
    connection_options options;
    options.max_result_rows = 2;
    connection session{options};
    CRPC_REQUIRE(session.is_open());
    CRPC_REQUIRE(run(session, "CREATE TABLE t(x INTEGER)").ok());
    CRPC_REQUIRE(run(session, "INSERT INTO t VALUES(1),(2),(3)").ok());

    auto rows = run_query(session, "SELECT x FROM t");
    CRPC_CHECK_FALSE(rows.state.ok());
    CRPC_CHECK(is_code(rows.state, crpc::rpc_code::resource_exhausted));
    CRPC_CHECK(rows.cursor == nullptr);

    // Exactly at the ceiling is allowed.
    auto exactly = run_query(session, "SELECT x FROM t LIMIT 2");
    CRPC_REQUIRE(exactly.state.ok());
    CRPC_REQUIRE(exactly.cursor != nullptr);
    std::size_t seen = 0;
    while (exactly.cursor->next()) {
        ++seen;
    }
    CRPC_CHECK_EQ(static_cast<std::size_t>(2), seen);
}

// --- command timeout ---------------------------------------------------------

CRPC_TEST(command_timeout_interrupts_a_long_statement) {
    require_backend();
    connection session{connection_options{}};
    CRPC_REQUIRE(session.is_open());
    const crpc::status armed = session.set_command_timeout(std::chrono::milliseconds(50));
    CRPC_REQUIRE(armed.ok());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(50),
                  static_cast<std::int64_t>(session.command_timeout().count()));

    // Bounded at five million rows so a broken timeout fails the case in a few seconds
    // instead of hanging the suite.
    auto runaway = run_query(session,
                             "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 "
                             "FROM c WHERE x < 5000000) SELECT count(*) FROM c");
    CRPC_CHECK_FALSE(runaway.state.ok());
    CRPC_CHECK(is_code(runaway.state, crpc::rpc_code::deadline_exceeded));
    CRPC_CHECK(runaway.cursor == nullptr);

    // The interrupted statement must not leave the connection unusable.
    CRPC_REQUIRE(session.set_command_timeout(std::chrono::milliseconds(0)).ok());
    CRPC_CHECK(session.ping().ok());
    auto quick = run_query(session, "SELECT 1");
    CRPC_REQUIRE(quick.state.ok());
    CRPC_REQUIRE(quick.cursor != nullptr);
    CRPC_REQUIRE(quick.cursor->next());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(1), quick.cursor->get_int64(0).value());
}

// --- lifetime ----------------------------------------------------------------

CRPC_TEST(closed_connection_rejects_every_call) {
    require_backend();
    connection session{connection_options{}};
    CRPC_REQUIRE(session.is_open());
    CRPC_REQUIRE(run(session, "CREATE TABLE t(x INTEGER)").ok());
    session.close();
    CRPC_CHECK_FALSE(session.is_open());
    // Idempotent.
    session.close();
    CRPC_CHECK_FALSE(session.is_open());
    CRPC_CHECK_FALSE(session.in_transaction());
    CRPC_CHECK(is_code(session.ping(), crpc::rpc_code::shutting_down));
    CRPC_CHECK(is_code(session.begin(), crpc::rpc_code::shutting_down));
    CRPC_CHECK(is_code(session.commit(), crpc::rpc_code::shutting_down));
    CRPC_CHECK(is_code(session.rollback(), crpc::rpc_code::shutting_down));
    auto rows = run_query(session, "SELECT 1");
    CRPC_CHECK(is_code(rows.state, crpc::rpc_code::shutting_down));
    CRPC_CHECK(rows.cursor == nullptr);
    auto stream = run_streaming(session, "SELECT 1");
    CRPC_CHECK(is_code(stream.state, crpc::rpc_code::shutting_down));
    CRPC_CHECK(stream.cursor == nullptr);
}

CRPC_TEST(two_connections_do_not_share_state) {
    require_backend();
    connection first{connection_options{}};
    connection second{connection_options{}};
    CRPC_REQUIRE(first.is_open());
    CRPC_REQUIRE(second.is_open());
    CRPC_REQUIRE(run(first, "CREATE TABLE only_here(x INTEGER)").ok());
    CRPC_REQUIRE(run(first, "INSERT INTO only_here VALUES(1)").ok());

    // A private in-memory database is not visible from another connection.
    auto missing = run_query(second, "SELECT * FROM only_here");
    CRPC_CHECK_FALSE(missing.state.ok());

    // Two open transactions are independent.
    CRPC_REQUIRE(first.begin().ok());
    CRPC_CHECK(first.in_transaction());
    CRPC_CHECK_FALSE(second.in_transaction());
    CRPC_REQUIRE(first.rollback().ok());
}

CRPC_TEST(factory_opens_connections_and_names_itself) {
    require_backend();
    crpc::db::sqlite::connection_factory factory{connection_options{}};
    CRPC_CHECK_EQ(std::string_view("sqlite"), factory.name());
    crpc::result<crpc::db::connection*> opened = factory.open();
    CRPC_REQUIRE(opened.ok());
    std::unique_ptr<crpc::db::connection> session{opened.value()};
    CRPC_REQUIRE(session != nullptr);
    CRPC_CHECK(session->is_open());
    CRPC_CHECK(session->ping().ok());
}

CRPC_TEST_MAIN()
