// CRPC -- an ODBC bridge: the driver-backed adapter for engines without a usable public
// wire protocol.
//
// Microsoft Access is the reason this exists. Jet/ACE has no published wire protocol, and
// reverse-engineering its page format would be both enormous and version-fragile; every
// maintained client goes through ODBC or OLE DB. So this adapter drives the ODBC API and
// stays honest about what it is: a bridge, not a protocol implementation.
//
// Two consequences follow from that, and both are visible in the API:
//
//   * the driver decides which SQL dialect is accepted, so nothing here rewrites or
//     validates SQL text beyond refusing to splice parameters into it;
//   * a driver reports errors as SQLSTATE plus a native code plus a message, and those are
//     what a caller needs, so they are carried through instead of being collapsed into a
//     single "query failed".
//
// Parameter binding uses ODBC's own parameter arrays, so a value never appears in the SQL
// text. `?` is ODBC's placeholder, which is also what this API documents.
#ifndef CRPC_DB_ODBC_CONNECTION_HPP
#define CRPC_DB_ODBC_CONNECTION_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/db/connection.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

#if defined(CRPC_HAVE_ODBC)
// The ODBC types stay out of this header: callers only see the shapes below, and a build
// without ODBC still compiles everything that includes it.
#endif

namespace crpc {
namespace db {
namespace odbc {

struct connection_options {
    // The connection string handed to the driver verbatim, for example
    //   "Driver={Microsoft Access Driver (*.mdb)};Dbq=C:\\data\\app.mdb;"
    // It is passed through rather than assembled from parts: every driver has its own
    // keywords, and guessing them would break the ones that differ.
    std::string connection_string;

    std::string user;
    std::string password;

    // Login timeout in seconds; 0 waits indefinitely, which is never what a caller wants
    // in a service, so the default is finite.
    std::uint32_t login_timeout_seconds = 15;

    // How long a single statement may run, in seconds. 0 means the driver's default.
    std::uint32_t query_timeout_seconds = 0;

    // A result set is materialised before `query()` returns, so these bound it. Exceeding
    // either fails with `resource_exhausted` instead of growing memory.
    std::size_t max_result_rows = 1'000'000;
    std::size_t max_result_bytes = 256u * 1024u * 1024u;
};

// True when this build has the ODBC bridge at all.
[[nodiscard]] CRPC_API bool available() noexcept;

// The drivers this machine offers, by name. Useful for building a connection string
// without guessing which driver is installed, which is the usual reason an ODBC
// connection fails on a machine that looks correctly configured.
[[nodiscard]] CRPC_API result<std::vector<std::string>> list_drivers();

// One ODBC session. Transactions use ODBC's own model rather than a driver's SQL dialect:
// `begin()` puts the connection into manual commit, `commit()` and `rollback()` end the
// transaction with `SQLEndTran` and restore autocommit, and `in_transaction()` reads the
// commit mode back from the driver. Sending `BEGIN TRANSACTION` as text instead would be
// one dialect among many, and the connection's state would be a local guess rather than
// something the driver reports. A driver that refuses manual commit reports its own error
// through `last_sqlstate()`.
class CRPC_API connection final : public db::connection {
public:
    // Connects with `options`. On failure the driver's SQLSTATE and message are in
    // `connect_error()`.
    explicit connection(connection_options options);
    ~connection() override;

    [[nodiscard]] status ping() override;
    [[nodiscard]] bool in_transaction() const noexcept override;

    [[nodiscard]] status begin() override;
    [[nodiscard]] status commit() override;
    [[nodiscard]] status rollback() override;

    // Runs `sql`. `?` placeholders are bound from `params` in order; a value is never
    // spliced into the text. `*out` receives the first result set, or a cursor with an
    // affected-row count and no columns.
    [[nodiscard]] status query(std::string_view sql, span<const parameter> params,
                               result_set** out) override;

    void close() noexcept override;
    [[nodiscard]] bool is_open() const noexcept override;

    // Diagnostics.
    [[nodiscard]] const rpc_error& connect_error() const noexcept { return connect_error_; }
    [[nodiscard]] std::string_view driver_name() const noexcept { return driver_name_; }
    [[nodiscard]] std::string_view driver_version() const noexcept { return driver_version_; }
    [[nodiscard]] std::string_view dbms_name() const noexcept { return dbms_name_; }

    // The SQLSTATE of the last failure. ODBC's own classification, kept separate from the
    // library code because "the driver said 23000" is more actionable than "constraint
    // violation" when the driver is the only source of truth.
    [[nodiscard]] std::string_view last_sqlstate() const noexcept { return last_sqlstate_; }

private:
    struct impl;
    std::unique_ptr<impl> impl_;
    rpc_error connect_error_;
    std::string driver_name_;
    std::string driver_version_;
    std::string dbms_name_;
    std::string last_sqlstate_;
};

// Opens connections with one connection string. A pool owns the result.
class CRPC_API connection_factory final : public db::connection_factory {
public:
    explicit connection_factory(connection_options options);
    ~connection_factory() override;

    [[nodiscard]] result<db::connection*> open() override;
    [[nodiscard]] std::string_view name() const noexcept override;

private:
    connection_options options_;
};

// Maps a SQLSTATE to the library code. The five-character class prefix is what carries the
// meaning: 23xxx is an integrity constraint, 42xxx a syntax or access error, 08xxx a
// connection failure, 40xxx a deadlock or serialisation failure.
[[nodiscard]] CRPC_API rpc_code map_sqlstate(std::string_view sqlstate) noexcept;

} // namespace odbc
} // namespace db
} // namespace crpc

#endif // CRPC_DB_ODBC_CONNECTION_HPP