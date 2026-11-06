// CRPC -- the SQLite backend.
//
// SQLite is an embedded engine, not a server, so this adapter has no wire protocol and no
// network failure model: every call either runs against a local file or an in-memory
// database. What it does share with the other backends is the `db::connection` contract,
// and that contract is what this header implements.
//
// Design notes
// ------------
// * The C API is called directly (`sqlite3_*`), never through a wrapper. There is no SQL
//   rewriting here: a caller's text reaches `sqlite3_prepare_v2` unchanged.
// * Parameters are bound with `sqlite3_bind_*` at the statement's `?` placeholders. A
//   value is never concatenated into SQL text. Text and blob length are always passed
//   explicitly, so an embedded NUL byte survives a round trip.
// * `in_transaction()` reads `sqlite3_get_autocommit()`; it is not a flag this class
//   maintains. A transaction started with raw `BEGIN` through `query()` is therefore
//   visible too. The pool relies on this answer before returning a session to the idle
//   set, and a local guess would eventually disagree with the engine.
// * `begin`/`commit`/`rollback` send the SQL statements. SQLite itself decides whether
//   the state transition is legal, and a failed statement is reported as an error without
//   this class pretending the state changed.
//
// Threading
// ---------
// One connection object is not safe for concurrent use. Separate connections may be used
// from separate threads; SQLite serialises access to a shared database file. A streaming
// cursor borrows its connection until it is drained or closed and must not be read at the
// same time as another statement on that connection.
#ifndef CRPC_DB_SQLITE_CONNECTION_HPP
#define CRPC_DB_SQLITE_CONNECTION_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <crpc/config.hpp>
#include <crpc/db/connection.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace db {
namespace sqlite {

// How to open the database.
struct connection_options {
    // The database file, or ":memory:" for a private in-memory database. The value is
    // passed to SQLite as UTF-8.
    std::string path = ":memory:";

    // Open with SQLITE_OPEN_READONLY. `create_if_missing` is ignored in this mode: a
    // read-only connection cannot create a file. WAL is not requested either, because a
    // read-only connection cannot change the journal mode; an existing WAL database stays
    // readable.
    bool read_only = false;

    // Add SQLITE_OPEN_CREATE so a missing file is created. Turning this off makes opening
    // a missing database fail with `failed_precondition` instead of silently creating an
    // empty one.
    bool create_if_missing = true;

    // Passed to `sqlite3_busy_timeout`, so a statement waiting on another connection's
    // lock retries for this long before reporting `unavailable`. Zero disables the wait.
    // A negative value is rejected when the connection is constructed.
    int busy_timeout_ms = 5000;

    // For an on-disk, writable database, run `PRAGMA journal_mode=WAL`. In-memory and
    // read-only databases are exempt (the pragma cannot apply there). If enabling WAL
    // fails for a writable file database, the connection fails to open rather than
    // silently running with a different durability and concurrency model.
    bool enable_wal = true;

    // Ceiling on rows materialised by `query()`. Exceeding it returns
    // `resource_exhausted` and no cursor. Zero allows no rows at all. `query_streaming()`
    // is not bounded: it never holds more than the current row, which is the point of
    // streaming.
    std::size_t max_result_rows = 1'000'000;
};

// True when this build has the SQLite backend. Answering does not open a database.
[[nodiscard]] CRPC_API bool available() noexcept;

// The SQLite library version this build links against, or "unavailable" when the backend
// was compiled out. Useful in a bug report: behaviour differs between SQLite releases.
[[nodiscard]] CRPC_API std::string_view version() noexcept;

// Maps a SQLite primary or extended result code to the library code. The primary code is
// the low byte of an extended code, so `map_errcode(2067)` (SQLITE_CONSTRAINT_UNIQUE)
// and `map_errcode(19)` (SQLITE_CONSTRAINT) agree. Exposed for tests and for callers that
// keep their own error records.
[[nodiscard]] CRPC_API rpc_code map_errcode(int sqlite_errcode) noexcept;

// One SQLite database handle.
//
// `query()` materialises the whole result (bounded by `max_result_rows`); use
// `query_streaming()` for a large result. Failed calls carry the SQLite primary and
// extended codes in `rpc_error::metadata` under "sqlite_code" and "sqlite_extended_code".
class CRPC_API connection final : public db::connection {
public:
    // Opens the database described by `options`. Failure is reported by `is_open() ==
    // false` plus `connect_error()`; this constructor never throws.
    explicit connection(connection_options options);
    ~connection() override;

    connection(const connection&) = delete;
    connection& operator=(const connection&) = delete;

    [[nodiscard]] status ping() override;

    // Reads `sqlite3_get_autocommit()`: true means the engine considers a transaction
    // open, whether it was started by `begin()` or by raw SQL.
    [[nodiscard]] bool in_transaction() const noexcept override;

    [[nodiscard]] status begin() override;
    [[nodiscard]] status commit() override;
    // Rolling back when no transaction is open is a no-op success, so a defensive caller
    // need not ask first.
    [[nodiscard]] status rollback() override;

    // Installs a progress handler that aborts a running statement once the timeout
    // elapses. The interrupted statement reports `deadline_exceeded`; the connection stays
    // usable. A timeout of zero (or less) disables it. SQLite checks the handler every few
    // thousand virtual-machine steps, so the bound is approximate.
    [[nodiscard]] status set_command_timeout(std::chrono::milliseconds timeout) noexcept override;

    // Runs one SQL statement with bound parameters. Only one statement is accepted per
    // call: trailing SQL text is rejected with `invalid_argument` instead of being
    // silently ignored, which is what `sqlite3_prepare_v2` would otherwise do.
    //
    // A statement that produces no columns (INSERT/UPDATE/DELETE/DDL) still yields a
    // cursor with zero columns and an affected-row count.
    [[nodiscard]] status query(std::string_view sql, span<const parameter> params,
                               result_set** out) override;

    // Same preparation and binding, but rows are pulled from SQLite on each `next()`.
    // The cursor finalises the statement when it is closed. A statement with no columns
    // cannot stream anything, so it is executed immediately and returned as a
    // materialised zero-column cursor.
    [[nodiscard]] status query_streaming(std::string_view sql, span<const parameter> params,
                                         result_set** out) override;

    // Ends an open transaction with ROLLBACK, then closes the handle. Idempotent. A
    // still-live streaming cursor keeps the underlying handle alive until it is closed,
    // because SQLite defers the real close (`sqlite3_close_v2`) until outstanding
    // statements are finalised.
    void close() noexcept override;
    [[nodiscard]] bool is_open() const noexcept override;

    // Set when construction failed; `ok` on a successfully opened connection.
    [[nodiscard]] const rpc_error& connect_error() const noexcept { return connect_error_; }

    // The journal mode SQLite reports for this connection ("memory", "wal", "delete",
    // ...). Empty when the connection is not open. This is the engine's answer, not the
    // requested option.
    [[nodiscard]] std::string_view journal_mode() const noexcept;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
    rpc_error connect_error_;
};

// Opens SQLite connections with one set of options. The pool owns whatever `open()`
// returns.
class CRPC_API connection_factory final : public db::connection_factory {
public:
    explicit connection_factory(connection_options options);
    ~connection_factory() override;

    [[nodiscard]] result<db::connection*> open() override;
    [[nodiscard]] std::string_view name() const noexcept override;

private:
    connection_options options_;
};

} // namespace sqlite
} // namespace db
} // namespace crpc

#endif // CRPC_DB_SQLITE_CONNECTION_HPP
