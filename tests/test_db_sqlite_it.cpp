// CRPC -- SQLite integration tests against real database files.
//
// Skipped unless CRPC_SQLITE_IT=1: the unit suite already proves the adapter against the
// engine in memory, and these cases add what only a file can show -- WAL mode, a second
// connection reading while the first holds a write transaction, persistence across a
// reopen, and the read-only open mode.
//
// Where the files live:
//   CRPC_SQLITE_IT_DIR       directory for the database files (default: the working
//                            directory, i.e. the build root when run by the test runner)
//   CRPC_SQLITE_IT=1         required to run at all
//
// Each run uses a unique file name and removes its files on the way out, so a failure
// leaves the database behind for inspection while a success does not accumulate garbage.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
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

[[nodiscard]] bool it_enabled() {
    const char* value = std::getenv("CRPC_SQLITE_IT");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

[[nodiscard]] std::string env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::string(value) : std::string(fallback);
}

void require_it() {
    if (!it_enabled()) {
        crpc_test::skip("CRPC_SQLITE_IT is not 1");
    }
    if (!crpc::db::sqlite::available()) {
        crpc_test::skip("this build has no SQLite backend");
    }
}

// A database file that removes itself, its -wal and its -shm on destruction. The paths
// are passed through a connection as-is; SQLite on Windows accepts '/' separators, but
// the file removal below uses the same string.
struct database_file {
    std::string path;

    explicit database_file(const char* tag) {
        const std::string dir = env_or("CRPC_SQLITE_IT_DIR", ".");
        const auto unique = static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        path = dir + "/crpc_sqlite_it_" + tag + "_" + std::to_string(unique) + ".db";
        remove_files();
    }

    ~database_file() { remove_files(); }

    database_file(const database_file&) = delete;
    database_file& operator=(const database_file&) = delete;

    void remove_files() const {
        std::remove(path.c_str());
        std::remove((path + "-wal").c_str());
        std::remove((path + "-shm").c_str());
    }
};

[[nodiscard]] crpc::span<const parameter> view_of(const std::vector<parameter>& params) {
    return crpc::span<const parameter>(params.data(), params.size());
}

[[nodiscard]] crpc::status run(connection& session, std::string_view sql,
                               const std::vector<parameter>& params = {}) {
    return session.execute(sql, view_of(params));
}

// Runs a count through the interface the pool hands out, so the case works on a lease's
// base-class pointer as well as on a concrete connection.
[[nodiscard]] std::int64_t count_rows(crpc::db::connection& session, std::string_view table) {
    result_set* raw = nullptr;
    const std::string sql = "SELECT count(*) FROM " + std::string(table);
    const crpc::status state = session.query(sql, {}, &raw);
    crpc::db::statement_guard guard(raw);
    if (!state.ok() || raw == nullptr || !raw->next()) {
        return -1;
    }
    return raw->get_int64(0).value_or(-1);
}

} // namespace

CRPC_TEST(file_database_uses_wal_and_survives_reopen) {
    require_it();
    database_file file{"wal"};
    {
        connection_options options;
        options.path = file.path;
        connection session{options};
        CRPC_REQUIRE(session.is_open());
        // The engine's own answer, not the requested option.
        CRPC_CHECK_EQ(std::string_view("wal"), session.journal_mode());
        CRPC_REQUIRE(run(session, "CREATE TABLE t(x INTEGER)").ok());
        CRPC_REQUIRE(run(session, "INSERT INTO t VALUES(1),(2),(3)").ok());
    }

    {
        connection_options options;
        options.path = file.path;
        connection session{options};
        CRPC_REQUIRE(session.is_open());
        // WAL is a persistent property of the file, so a fresh connection still sees it.
        CRPC_CHECK_EQ(std::string_view("wal"), session.journal_mode());
        CRPC_CHECK_EQ(static_cast<std::int64_t>(3), count_rows(session, "t"));
    }
}

CRPC_TEST(create_if_missing_false_refuses_a_missing_file) {
    require_it();
    database_file file{"missing"};
    connection_options options;
    options.path = file.path;
    options.create_if_missing = false;
    connection session{options};
    CRPC_CHECK_FALSE(session.is_open());
    // A missing file is a precondition the caller chose, not a protocol failure.
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(session.connect_error().code));
    CRPC_CHECK_FALSE(session.connect_error().message.empty());
}

CRPC_TEST(read_only_connection_reads_but_cannot_write) {
    require_it();
    database_file file{"ro"};
    {
        connection_options options;
        options.path = file.path;
        options.enable_wal = false;
        connection writer{options};
        CRPC_REQUIRE(writer.is_open());
        CRPC_REQUIRE(run(writer, "CREATE TABLE t(x INTEGER)").ok());
        CRPC_REQUIRE(run(writer, "INSERT INTO t VALUES(42)").ok());
    }
    {
        connection_options options;
        options.path = file.path;
        options.read_only = true;
        connection reader{options};
        CRPC_REQUIRE(reader.is_open());
        CRPC_CHECK_EQ(static_cast<std::int64_t>(1), count_rows(reader, "t"));

        const crpc::status write = run(reader, "INSERT INTO t VALUES(7)");
        CRPC_CHECK_FALSE(write.ok());
        // SQLITE_READONLY is 8: the OS/filesystem refused the change.
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::permission_denied),
                      static_cast<int>(write.code()));
        CRPC_CHECK_EQ(static_cast<std::int64_t>(1), count_rows(reader, "t"));
    }
}

CRPC_TEST(two_connections_read_while_a_writer_holds_a_transaction) {
    require_it();
    database_file file{"concurrent"};
    connection_options options;
    options.path = file.path;
    options.busy_timeout_ms = 5000;

    connection writer{options};
    CRPC_REQUIRE(writer.is_open());
    CRPC_REQUIRE(run(writer, "CREATE TABLE t(x INTEGER)").ok());
    CRPC_REQUIRE(run(writer, "INSERT INTO t VALUES(1)").ok());

    connection reader{options};
    CRPC_REQUIRE(reader.is_open());
    CRPC_CHECK_EQ(std::string_view("wal"), reader.journal_mode());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(1), count_rows(reader, "t"));

    // In WAL mode a reader is not blocked by an uncommitted writer: it sees the last
    // committed state. This is the property the whole journal-mode choice buys.
    CRPC_REQUIRE(writer.begin().ok());
    CRPC_REQUIRE(run(writer, "INSERT INTO t VALUES(2)").ok());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(1), count_rows(reader, "t"));

    // A second reader running on its own thread while the writer is still open.
    std::atomic<int> reader_failures{0};
    std::atomic<std::int64_t> reader_count{-1};
    std::thread other{[&]() {
        connection third{options};
        if (!third.is_open()) {
            reader_failures.fetch_add(1);
            return;
        }
        const std::int64_t seen = count_rows(third, "t");
        reader_count.store(seen);
        if (seen != 1) {
            reader_failures.fetch_add(1);
        }
    }};
    other.join();
    CRPC_CHECK_EQ(0, reader_failures.load());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(1), reader_count.load());

    // After the commit every connection sees both rows.
    CRPC_REQUIRE(writer.commit().ok());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(2), count_rows(reader, "t"));

    // And a rollback is invisible to the other connections.
    CRPC_REQUIRE(writer.begin().ok());
    CRPC_REQUIRE(run(writer, "INSERT INTO t VALUES(3)").ok());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(2), count_rows(reader, "t"));
    CRPC_REQUIRE(writer.rollback().ok());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(2), count_rows(reader, "t"));
}

CRPC_TEST(pool_borrows_file_connections_through_the_factory) {
    require_it();
    database_file file{"pool"};
    connection_options options;
    options.path = file.path;
    crpc::db::sqlite::connection_factory factory{options};

    crpc::db::pool_options pool_options;
    pool_options.max_size = 2;
    pool_options.max_idle = 2;
    pool_options.min_size = 1;
    CRPC_CHECK_EQ(std::string_view("sqlite"), factory.name());

    crpc::db::pool pool{pool_options, factory};
    pool.warmup();
    {
        auto lease = pool.acquire();
        CRPC_REQUIRE(lease.ok());
        CRPC_REQUIRE(lease.value()->ping().ok());
        CRPC_REQUIRE(lease.value()->execute("CREATE TABLE t(x INTEGER)").ok());
        // A transaction left open is rolled back by the pool on release (P3), so the
        // engine's autocommit answer has to be visible through the interface and the row
        // inserted inside the transaction must be gone afterwards.
        CRPC_REQUIRE(lease.value()->begin().ok());
        CRPC_CHECK(lease.value()->in_transaction());
        CRPC_REQUIRE(lease.value()->execute("INSERT INTO t VALUES(1)").ok());
    }
    {
        auto lease = pool.acquire();
        CRPC_REQUIRE(lease.ok());
        CRPC_CHECK_FALSE(lease.value()->in_transaction());
        // The table survived (it was created in autocommit), the inserted row did not.
        CRPC_CHECK_EQ(static_cast<std::int64_t>(0), count_rows(*lease.value().get(), "t"));
    }
    pool.close();
}

CRPC_TEST_MAIN()
