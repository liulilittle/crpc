// CRPC -- TiDB integration against a real server, compared with MySQL 8.
//
// Skipped unless CRPC_TIDB_IT=1; the MySQL comparison part additionally
// requires CRPC_MYSQL_IT=1. Everything here needs a live server:
//
//   CRPC_TIDB_IT=1
//   CRPC_TIDB_HOST (default 127.0.0.1)
//   CRPC_TIDB_PORT (default 4000)
//   CRPC_TIDB_USER (default root)
//   CRPC_TIDB_PASSWORD (default empty)
//   CRPC_TIDB_DB (default crpc_tidb_it)
//
// The MySQL half of the comparison reuses the CRPC_MYSQL_* variables.
//
// Measured facts this suite pins down (TiDB v7.5.0, standalone tidb-server
// over unistore):
//   * `SELECT VERSION()` -> "8.0.11-TiDB-v7.5.0",
//     `SELECT @@version_comment` ->
//     "TiDB Server (Apache License 2.0) Community Edition, MySQL 8.0 compatible".
//   * `@@tidb_txn_mode` defaults to "pessimistic" on 7.x; optimistic must be
//     requested per session.
//   * optimistic write conflict surfaces at COMMIT with 9007:
//     "Write conflict, txnStartTS=..., reason=Optimistic [try again later]".
//   * pessimistic lock wait times out with 1205 at the UPDATE.
//   * information_schema.TABLES carries TIDB_* columns MySQL does not have.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <crpc/db/mysql/connection.hpp>
#include <crpc/db/tidb/connection.hpp>
#include <crpc/db/tidb/retry.hpp>
#include <crpc/db/types.hpp>
#include <crpc/error.hpp>
#include <crpc/io/iocp_backend.hpp>
#include <crpc/span.hpp>

namespace {

[[nodiscard]] std::string env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::string(value) : std::string(fallback);
}

[[nodiscard]] bool it_enabled() {
    const char* value = std::getenv("CRPC_TIDB_IT");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

[[nodiscard]] bool mysql_it_enabled() {
    const char* value = std::getenv("CRPC_MYSQL_IT");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

// Every TiDB case shares the same guard: no server, no run.
[[nodiscard]] const char* require_server() {
    if (!it_enabled()) {
        crpc_test::skip("CRPC_TIDB_IT is not 1");
    }
    return "";
}

// One TiDB backend: an IO backend, a factory over it and a pool.
struct it_session {
    crpc::iocp_backend backend;
    std::unique_ptr<crpc::db::tidb::connection_factory> factory;
    std::unique_ptr<crpc::db::pool> pool;
    std::string database;

    explicit it_session(std::size_t max_connections = 2)
        : database(env_or("CRPC_TIDB_DB", "crpc_tidb_it")) {
        crpc::db::mysql::connection_options options;
        options.user = env_or("CRPC_TIDB_USER", "root");
        options.password = env_or("CRPC_TIDB_PASSWORD", "");
        // Tables are fully qualified, so no default schema is selected.
        options.database.clear();
        const std::string host = env_or("CRPC_TIDB_HOST", "127.0.0.1");
        const std::string port = env_or("CRPC_TIDB_PORT", "4000");
        factory.reset(new crpc::db::tidb::connection_factory(
            options, backend,
            crpc::endpoint::tcp(host, static_cast<std::uint16_t>(std::stoi(port)))));
        crpc::db::pool_options pool_options;
        pool_options.max_size = max_connections;
        pool_options.max_idle = max_connections;
        pool_options.validate_on_borrow = false;
        // An integration server over a VNet can open a connection slowly; a
        // five-second borrow limit would turn that into a false failure.
        pool_options.acquire_timeout = std::chrono::seconds(30);
        pool.reset(new crpc::db::pool(pool_options, *factory));
    }
};

// One MySQL 8 pool for the comparison half.
struct mysql_session {
    crpc::iocp_backend backend;
    std::unique_ptr<crpc::db::mysql::connection_factory> factory;
    std::unique_ptr<crpc::db::pool> pool;
    std::string database;

    explicit mysql_session(std::size_t max_connections = 2) {
        crpc::db::mysql::connection_options options;
        options.user = env_or("CRPC_MYSQL_USER", "crpc");
        options.password = env_or("CRPC_MYSQL_PASSWORD", "crpc_pw");
        options.database.clear();
        database = env_or("CRPC_MYSQL_DB", "crpc_it");
        const std::string host = env_or("CRPC_MYSQL_HOST", "127.0.0.1");
        const std::string port = env_or("CRPC_MYSQL_PORT", "3306");
        factory.reset(new crpc::db::mysql::connection_factory(
            options, backend,
            crpc::endpoint::tcp(host, static_cast<std::uint16_t>(std::stoi(port)))));
        crpc::db::pool_options pool_options;
        pool_options.max_size = max_connections;
        pool_options.max_idle = max_connections;
        pool_options.validate_on_borrow = false;
        // An integration server over a VNet can open a connection slowly; a
        // five-second borrow limit would turn that into a false failure.
        pool_options.acquire_timeout = std::chrono::seconds(30);
        pool.reset(new crpc::db::pool(pool_options, *factory));
    }
};

[[nodiscard]] crpc::status ensure_database(crpc::db::pool& pool, const std::string& database) {
    auto lease = pool.acquire();
    if (!lease) {
        return lease.error();
    }
    return lease.value()->execute("CREATE DATABASE IF NOT EXISTS " + database);
}

// Runs `sql` and returns the first column of the first row as a string
// (empty when the query fails or returns nothing). Test helper only.
[[nodiscard]] std::string scalar(crpc::db::connection& c, std::string_view sql) {
    crpc::db::result_set* rows = nullptr;
    const crpc::status ran = c.query(sql, {}, &rows);
    if (!ran) {
        return "";
    }
    std::unique_ptr<crpc::db::result_set> guard(rows);
    if (guard == nullptr || !guard->next()) {
        return "";
    }
    const auto text = guard->get_text(0);
    return text ? std::string(text.value()) : "";
}

// Builds a const span over a parameter vector for the connection-level calls.
[[nodiscard]] crpc::span<const crpc::db::parameter> span_of(
    const std::vector<crpc::db::parameter>& values) noexcept {
    return crpc::span<const crpc::db::parameter>(values.data(), values.size());
}

// Runs `sql` and returns the server error, or ok status when the query
// unexpectedly succeeded.
[[nodiscard]] crpc::rpc_error query_error(crpc::db::connection& c, std::string_view sql) {
    crpc::db::result_set* rows = nullptr;
    const crpc::status ran = c.query(sql, {}, &rows);
    if (!ran) {
        return ran.error();
    }
    delete rows;
    return crpc::rpc_error{crpc::rpc_code::ok, "unexpectedly succeeded"};
}

// One session per binary, shared by every case. Opening and closing a fresh
// pool inside each case is the pattern that makes an on-demand VNet setup
// flaky: the suite churns dozens of new connections per run, and a refused or
// slow handshake on any one of them fails the case. A shared pool keeps the
// connection count constant (three for TiDB, three for the comparison MySQL)
// and every borrow/return is thereafter a fast reuse, so transient connect
// failures cannot strike a case midway.
// The tests are sequential within the binary, so the shared state needs no
// locking; each case terminates its own transactions before returning (an
// abandoned transaction is rolled back by the pool on release, contract P3).

// Opens `target` connections and returns them to the idle set, so the cases
// never trigger a mid-test handshake over the lossy NAT. A connect failure
// that would otherwise fail a case is eaten here and retried across rounds.
void warm_pool(crpc::db::pool& pool, std::size_t target) {
    for (unsigned round = 0; round < 3 && pool.total() < target; ++round) {
        std::vector<crpc::db::pool::lease> held;
        for (std::size_t i = pool.total(); i < target; ++i) {
            auto lease = pool.acquire();
            if (!lease) {
                held.clear();
                break;
            }
            held.push_back(std::move(lease.value()));
        }
        for (auto& lease : held) {
            (void)lease.release();
        }
    }
}

[[nodiscard]] it_session& shared_tidb() {
    static it_session instance(3);
    static const bool warmed = [] {
        warm_pool(*instance.pool, 3);
        return true;
    }();
    (void)warmed;
    return instance;
}

[[nodiscard]] mysql_session& shared_mysql() {
    static mysql_session instance(3);
    static const bool warmed = [] {
        warm_pool(*instance.pool, 3);
        return true;
    }();
    (void)warmed;
    return instance;
}

} // namespace

CRPC_TEST(tidb_it_detect_capabilities) {
    (void)require_server();
    it_session& session = shared_tidb();
    CRPC_CHECK(ensure_database(*session.pool, session.database).ok());
    auto lease = session.pool->acquire();
    CRPC_CHECK(lease.ok());
    if (!lease.ok()) {
        return;
    }
    auto* conn = dynamic_cast<crpc::db::tidb::connection*>(lease.value().get());
    CRPC_CHECK(conn != nullptr);
    if (conn == nullptr) {
        return;
    }
    CRPC_CHECK(conn->is_tidb());
    // The session mode is one of the two real values, never unknown on TiDB.
    CRPC_CHECK(conn->txn_mode() == crpc::db::tidb::txn_mode::optimistic ||
               conn->txn_mode() == crpc::db::tidb::txn_mode::pessimistic);
    // Cross-check the detection surface against the live server variables.
    const std::string comment = scalar(*conn, "SELECT @@version_comment");
    const std::string version = std::string(conn->server_version());
    std::printf("  version=%s comment=%s txn_mode=%s\n", version.c_str(),
                comment.c_str(), crpc::db::tidb::to_string(conn->txn_mode()));
    CRPC_CHECK(comment.find("TiDB") != std::string::npos);
    CRPC_CHECK(version.find("TiDB") != std::string::npos);
}

CRPC_TEST(tidb_it_crud_prepared) {
    (void)require_server();
    it_session& session = shared_tidb();
    CRPC_CHECK(ensure_database(*session.pool, session.database).ok());
    const std::string table = session.database + ".it_crud";
    auto lease = session.pool->acquire();
    CRPC_CHECK(lease.ok());
    if (!lease.ok()) {
        return;
    }
    crpc::db::connection& conn = *lease.value().get();
    crpc::status st;
    st = conn.execute("DROP TABLE IF EXISTS " + table);
    CRPC_CHECK(st.ok());
    st = conn.execute("CREATE TABLE " + table +
                      " (id BIGINT PRIMARY KEY, name VARCHAR(64), amount DECIMAL(10,2), "
                      "note TEXT)");
    CRPC_CHECK(st.ok());
    if (!st.ok()) {
        std::printf("  create error: %s\n", st.message().c_str());
        return;
    }

    std::vector<crpc::db::parameter> insert;
    insert.push_back(crpc::db::parameter::from_int64(1));
    insert.push_back(crpc::db::parameter::from_text("alice"));
    insert.push_back(crpc::db::parameter::from_text("12.50"));
    insert.push_back(crpc::db::parameter::null());
    st = conn.execute("INSERT INTO " + table +
                          " (id, name, amount, note) VALUES (?, ?, ?, ?)",
                      span_of(insert));
    CRPC_CHECK(st.ok());
    if (!st.ok()) {
        std::printf("  insert error: %s\n", st.message().c_str());
        return;
    }

    std::vector<crpc::db::parameter> where;
    where.push_back(crpc::db::parameter::from_int64(1));
    crpc::db::result_set* rows = nullptr;
    st = conn.query("SELECT id, name, amount, note FROM " + table + " WHERE id = ?",
                    span_of(where), &rows);
    CRPC_CHECK(st.ok());
    if (!st.ok()) {
        return;
    }
    std::unique_ptr<crpc::db::result_set> guard(rows);
    if (guard != nullptr) {
        CRPC_CHECK(guard->next());
        CRPC_CHECK_EQ(static_cast<std::int64_t>(1), guard->get_int64(0).value());
        CRPC_CHECK_EQ(std::string("alice"), std::string(guard->get_text(1).value()));
        CRPC_CHECK_EQ(std::string("12.50"), std::string(guard->get_text(2).value()));
        CRPC_CHECK(guard->is_null(3));
        CRPC_CHECK_EQ(std::string("id"), guard->column(0).name);
    }

    std::vector<crpc::db::parameter> rename;
    rename.push_back(crpc::db::parameter::from_text("bob"));
    rename.push_back(crpc::db::parameter::from_int64(1));
    st = conn.execute("UPDATE " + table + " SET name = ? WHERE id = ?", span_of(rename));
    CRPC_CHECK(st.ok());
    rows = nullptr;
    st = conn.query("SELECT name FROM " + table + " WHERE id = 1", {}, &rows);
    CRPC_CHECK(st.ok());
    if (st.ok()) {
        std::unique_ptr<crpc::db::result_set> updated(rows);
        if (updated != nullptr && updated->next()) {
            CRPC_CHECK_EQ(std::string("bob"), std::string(updated->get_text(0).value()));
        }
    }
    st = conn.execute("DELETE FROM " + table + " WHERE id = 1");
    CRPC_CHECK(st.ok());
    rows = nullptr;
    st = conn.query("SELECT COUNT(*) FROM " + table, {}, &rows);
    CRPC_CHECK(st.ok());
    if (st.ok()) {
        std::unique_ptr<crpc::db::result_set> counted(rows);
        if (counted != nullptr && counted->next()) {
            CRPC_CHECK_EQ(static_cast<std::int64_t>(0), counted->get_int64(0).value());
        }
    }
    CRPC_CHECK(conn.execute("DROP TABLE IF EXISTS " + table).ok());
}

CRPC_TEST(tidb_it_optimistic_write_conflict_9007) {
    (void)require_server();
    it_session& session = shared_tidb();
    CRPC_CHECK(ensure_database(*session.pool, session.database).ok());
    const std::string table = session.database + ".it_conflict";

    auto la = session.pool->acquire();
    auto lb = session.pool->acquire();
    if (!la.ok()) {
        std::printf("  conflict borrow a failed: %s\n", la.error().message.c_str());
    }
    if (!lb.ok()) {
        std::printf("  conflict borrow b failed: %s\n", lb.error().message.c_str());
    }
    CRPC_CHECK(la.ok());
    CRPC_CHECK(lb.ok());
    if (!la.ok() || !lb.ok()) {
        return;
    }
    crpc::db::connection& a = *la.value().get();
    crpc::db::connection& b = *lb.value().get();
    CRPC_CHECK(a.execute("SET SESSION tidb_txn_mode='optimistic'").ok());
    CRPC_CHECK(b.execute("SET SESSION tidb_txn_mode='optimistic'").ok());
    CRPC_CHECK(a.execute("DROP TABLE IF EXISTS " + table).ok());
    CRPC_CHECK(a.execute("CREATE TABLE " + table + " (id INT PRIMARY KEY, v INT)").ok());
    CRPC_CHECK(a.execute("INSERT INTO " + table + " VALUES (1, 0)").ok());

    // The optimistic model lets both writers proceed; the loser finds out at
    // COMMIT. A begins, updates, and stays open while B commits the same key.
    CRPC_CHECK(a.begin().ok());
    CRPC_CHECK(a.execute("UPDATE " + table + " SET v = 1 WHERE id = 1").ok());
    crpc::status worker;
    std::thread other([&] {
        crpc::status st;
        st = b.begin();
        if (!st) {
            worker = st;
            return;
        }
        st = b.execute("UPDATE " + table + " SET v = 2 WHERE id = 1");
        if (!st) {
            worker = st;
            return;
        }
        worker = b.commit();
    });
    other.join();

    const crpc::status committed = a.commit();
    CRPC_CHECK_FALSE(committed.ok());
    if (!committed.ok()) {
        const crpc::rpc_error& error = committed.error();
        std::printf("  commit error: %s\n", error.message.c_str());
        // 9007 maps to `unavailable` (retryable); the raw code stays in the
        // metadata so it is distinguishable from MySQL 1205.
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable),
                      static_cast<int>(error.code));
        const std::string* sql = error.metadata.find("sql_code");
        CRPC_CHECK(sql != nullptr);
        if (sql != nullptr) {
            CRPC_CHECK_EQ(std::string("9007"), *sql);
        }
        CRPC_CHECK(crpc::db::tidb::is_transaction_conflict(error));
        CRPC_CHECK(crpc::is_retryable(error.code));
        CRPC_CHECK(error.message.find("Write conflict") != std::string::npos);
    }
    // The loser's transaction is over; the pool rolls nothing back.
    (void)a.rollback();
    // B committed its value.
    crpc::db::result_set* rows = nullptr;
    CRPC_CHECK(a.query("SELECT v FROM " + table + " WHERE id = 1", {}, &rows).ok());
    if (rows != nullptr) {
        std::unique_ptr<crpc::db::result_set> guard(rows);
        if (guard->next()) {
            CRPC_CHECK_EQ(static_cast<std::int64_t>(2), guard->get_int64(0).value());
        }
    }
    CRPC_CHECK(a.execute("DROP TABLE IF EXISTS " + table).ok());
}

CRPC_TEST(tidb_it_retry_wrapper_succeeds_after_conflict) {
    (void)require_server();
    it_session& session = shared_tidb();
    CRPC_CHECK(ensure_database(*session.pool, session.database).ok());
    const std::string table = session.database + ".it_retry";
    {
        auto setup = session.pool->acquire();
        CRPC_CHECK(setup.ok());
        if (!setup.ok()) {
            return;
        }
        CRPC_CHECK(setup.value()->execute("DROP TABLE IF EXISTS " + table).ok());
        CRPC_CHECK(setup.value()
                       ->execute("CREATE TABLE " + table + " (id INT PRIMARY KEY, v INT)")
                       .ok());
        CRPC_CHECK(setup.value()->execute("INSERT INTO " + table + " VALUES (1, 0)").ok());
    }

    std::atomic<int> attempts{0};
    const auto work = [&](crpc::db::connection& c) -> crpc::status {
        ++attempts;
        std::vector<crpc::db::parameter> value;
        value.push_back(crpc::db::parameter::from_int64(100 + attempts.load()));
        crpc::status st = c.execute("UPDATE " + table + " SET v = ? WHERE id = 1", span_of(value));
        if (!st) {
            return st;
        }
        if (attempts.load() == 1) {
            // A concurrent writer commits the same key between our snapshot and
            // our COMMIT, exactly once. The retry path must then succeed.
            auto lease = session.pool->acquire();
            if (!lease) {
                return lease.error();
            }
            crpc::status setup =
                lease.value()->execute("SET SESSION tidb_txn_mode='optimistic'");
            if (!setup) {
                return setup;
            }
            setup = lease.value()->begin();
            if (!setup) {
                return setup;
            }
            std::vector<crpc::db::parameter> other;
            other.push_back(crpc::db::parameter::from_int64(999));
            setup = lease.value()->execute("UPDATE " + table + " SET v = ? WHERE id = 1",
                                           span_of(other));
            if (!setup) {
                return setup;
            }
            setup = lease.value()->commit();
            if (!setup) {
                return setup;
            }
        }
        return crpc::status{};
    };

    crpc::db::tidb::transaction_retry_options options;
    options.setup_sql = "SET SESSION tidb_txn_mode='optimistic'";
    options.max_attempts = 4;
    const crpc::status outcome =
        crpc::db::tidb::retry_transaction(*session.pool, work, options);
    CRPC_CHECK(outcome.ok());
    if (!outcome.ok()) {
        std::printf("  retry error: %s\n", outcome.message().c_str());
        return;
    }
    // First attempt conflicted, the second committed: exactly two invocations.
    CRPC_CHECK_EQ(2, attempts.load());

    auto verify = session.pool->acquire();
    CRPC_CHECK(verify.ok());
    if (verify.ok()) {
        CRPC_CHECK_EQ(std::string("102"),
                      scalar(*verify.value().get(),
                             "SELECT v FROM " + table + " WHERE id = 1"));
    }
    {
        auto teardown = session.pool->acquire();
        if (teardown.ok()) {
            (void)teardown.value()->execute("DROP TABLE IF EXISTS " + table);
        }
    }
}

CRPC_TEST(tidb_it_retry_wrapper_exhausts_attempts) {
    (void)require_server();
    it_session& session = shared_tidb();
    CRPC_CHECK(ensure_database(*session.pool, session.database).ok());
    const std::string table = session.database + ".it_retry_give_up";
    {
        auto setup = session.pool->acquire();
        CRPC_CHECK(setup.ok());
        if (!setup.ok()) {
            return;
        }
        CRPC_CHECK(setup.value()->execute("DROP TABLE IF EXISTS " + table).ok());
        CRPC_CHECK(setup.value()
                       ->execute("CREATE TABLE " + table + " (id INT PRIMARY KEY, v INT)")
                       .ok());
        CRPC_CHECK(setup.value()->execute("INSERT INTO " + table + " VALUES (1, 0)").ok());
    }

    std::atomic<int> attempts{0};
    const auto work = [&](crpc::db::connection& c) -> crpc::status {
        ++attempts;
        std::vector<crpc::db::parameter> value;
        value.push_back(crpc::db::parameter::from_int64(500 + attempts.load()));
        crpc::status st = c.execute("UPDATE " + table + " SET v = ? WHERE id = 1", span_of(value));
        if (!st) {
            return st;
        }
        // A new concurrent writer commits on every attempt, so each COMMIT
        // conflicts and the budget is exhausted.
        auto lease = session.pool->acquire();
        if (!lease) {
            return lease.error();
        }
        crpc::status setup = lease.value()->execute("SET SESSION tidb_txn_mode='optimistic'");
        if (!setup) {
            return setup;
        }
        setup = lease.value()->begin();
        if (!setup) {
            return setup;
        }
        std::vector<crpc::db::parameter> other;
        other.push_back(crpc::db::parameter::from_int64(7000 + attempts.load()));
        setup =
            lease.value()->execute("UPDATE " + table + " SET v = ? WHERE id = 1", span_of(other));
        if (!setup) {
            return setup;
        }
        setup = lease.value()->commit();
        if (!setup) {
            return setup;
        }
        return crpc::status{};
    };

    crpc::db::tidb::transaction_retry_options options;
    options.setup_sql = "SET SESSION tidb_txn_mode='optimistic'";
    options.max_attempts = 2;
    const crpc::status outcome =
        crpc::db::tidb::retry_transaction(*session.pool, work, options);
    CRPC_CHECK_FALSE(outcome.ok());
    if (!outcome.ok()) {
        const crpc::rpc_error& error = outcome.error();
        const std::string* sql = error.metadata.find("sql_code");
        CRPC_CHECK(sql != nullptr);
        if (sql != nullptr) {
            CRPC_CHECK_EQ(std::string("9007"), *sql);
        }
        CRPC_CHECK(crpc::db::tidb::is_transaction_conflict(error));
    }
    CRPC_CHECK_EQ(2, attempts.load());
    {
        auto teardown = session.pool->acquire();
        if (teardown.ok()) {
            (void)teardown.value()->execute("DROP TABLE IF EXISTS " + table);
        }
    }
}

CRPC_TEST(tidb_it_lock_wait_timeout_1205_distinct_from_9007) {
    (void)require_server();
    it_session& session = shared_tidb();
    CRPC_CHECK(ensure_database(*session.pool, session.database).ok());
    const std::string table = session.database + ".it_lock";
    auto la = session.pool->acquire();
    auto lb = session.pool->acquire();
    CRPC_CHECK(la.ok());
    CRPC_CHECK(lb.ok());
    if (!la.ok() || !lb.ok()) {
        return;
    }
    crpc::db::connection& a = *la.value().get();
    crpc::db::connection& b = *lb.value().get();
    // The pool is shared with the optimistic-mode cases, so each connection is
    // put back into the pessimistic mode this case depends on explicitly.
    CRPC_CHECK(a.execute("SET SESSION tidb_txn_mode='pessimistic'").ok());
    CRPC_CHECK(b.execute("SET SESSION tidb_txn_mode='pessimistic'").ok());
    CRPC_CHECK(a.execute("SET SESSION innodb_lock_wait_timeout=1").ok());
    CRPC_CHECK(b.execute("SET SESSION innodb_lock_wait_timeout=1").ok());
    CRPC_CHECK(a.execute("DROP TABLE IF EXISTS " + table).ok());
    CRPC_CHECK(a.execute("CREATE TABLE " + table + " (id INT PRIMARY KEY, v INT)").ok());
    CRPC_CHECK(a.execute("INSERT INTO " + table + " VALUES (1, 0)").ok());

    // Pessimistic locks behave like MySQL: the second UPDATE waits on the row
    // lock and times out with 1205, unlike the optimistic 9007 at COMMIT.
    CRPC_CHECK(a.begin().ok());
    CRPC_CHECK(a.execute("UPDATE " + table + " SET v = 1 WHERE id = 1").ok());
    crpc::rpc_error b_failure = crpc::rpc_error{crpc::rpc_code::ok, "no error"};
    bool b_had_error = false;
    std::thread other([&] {
        crpc::status st;
        st = b.begin();
        if (!st) {
            b_failure = st.error();
            b_had_error = true;
            return;
        }
        st = b.execute("UPDATE " + table + " SET v = 2 WHERE id = 1");
        if (!st) {
            b_failure = st.error();
            b_had_error = true;
        }
    });
    other.join();
    CRPC_CHECK(b_had_error);
    if (b_had_error) {
        const std::string* sql = b_failure.metadata.find("sql_code");
        CRPC_CHECK(sql != nullptr);
        if (sql != nullptr) {
            CRPC_CHECK_EQ(std::string("1205"), *sql);
        }
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::unavailable),
                      static_cast<int>(b_failure.code));
        CRPC_CHECK(crpc::db::tidb::is_transaction_conflict(b_failure));
        std::printf("  1205 message: %s\n", b_failure.message.c_str());
    }
    (void)a.rollback();
    (void)b.rollback();
    CRPC_CHECK(a.execute("DROP TABLE IF EXISTS " + table).ok());
}

CRPC_TEST(tidb_it_mysql_comparison) {
    (void)require_server();
    if (!mysql_it_enabled()) {
        crpc_test::skip("CRPC_MYSQL_IT is not 1");
    }
    it_session& tidb = shared_tidb();
    mysql_session& mysql = shared_mysql();
    CRPC_CHECK(ensure_database(*tidb.pool, tidb.database).ok());
    CRPC_CHECK(ensure_database(*mysql.pool, mysql.database).ok());

    auto t_lease = tidb.pool->acquire();
    auto m_lease = mysql.pool->acquire();
    CRPC_CHECK(t_lease.ok());
    CRPC_CHECK(m_lease.ok());
    if (!t_lease.ok() || !m_lease.ok()) {
        return;
    }
    crpc::db::connection& t = *t_lease.value().get();
    crpc::db::connection& m = *m_lease.value().get();

    // 1) The same SELECT @@version_comment answers differently.
    const std::string t_comment = scalar(t, "SELECT @@version_comment");
    const std::string m_comment = scalar(m, "SELECT @@version_comment");
    CRPC_CHECK(t_comment.find("TiDB") != std::string::npos);
    CRPC_CHECK(m_comment.find("TiDB") == std::string::npos);
    CRPC_CHECK(t_comment != m_comment);
    std::printf("  version_comment: TiDB=\"%s\" MySQL=\"%s\"\n", t_comment.c_str(),
                m_comment.c_str());

    // 2) @@tidb_txn_mode exists on TiDB only; MySQL rejects the variable.
    const std::string t_mode = scalar(t, "SELECT @@tidb_txn_mode");
    CRPC_CHECK(t_mode == "optimistic" || t_mode == "pessimistic");
    const crpc::rpc_error m_unknown = query_error(m, "SELECT @@tidb_txn_mode");
    CRPC_CHECK_FALSE(m_unknown.ok());
    const std::string* m_sql = m_unknown.metadata.find("sql_code");
    std::string m_sql_code = m_sql != nullptr ? *m_sql : "?";
    std::printf("  @@tidb_txn_mode: TiDB=\"%s\" MySQL error code=%s msg=\"%s\"\n",
                t_mode.c_str(), m_sql_code.c_str(), m_unknown.message.c_str());
    if (m_sql != nullptr) {
        CRPC_CHECK_EQ(std::string("1193"), *m_sql);
    }

    // 3) information_schema.TABLES gained TiDB_* columns.
    const std::string tbl_columns =
        "SELECT COUNT(*) FROM information_schema.COLUMNS "
        "WHERE TABLE_SCHEMA='information_schema' AND TABLE_NAME='TABLES' "
        "AND COLUMN_NAME LIKE 'TIDB_%'";
    crpc::db::result_set* t_rows = nullptr;
    CRPC_CHECK(t.query(tbl_columns, {}, &t_rows).ok());
    std::int64_t t_count = -1;
    if (t_rows != nullptr) {
        std::unique_ptr<crpc::db::result_set> guard(t_rows);
        if (guard->next()) {
            t_count = guard->get_int64(0).value();
        }
    }
    crpc::db::result_set* m_rows = nullptr;
    CRPC_CHECK(m.query(tbl_columns, {}, &m_rows).ok());
    std::int64_t m_count = -1;
    if (m_rows != nullptr) {
        std::unique_ptr<crpc::db::result_set> guard(m_rows);
        if (guard->next()) {
            m_count = guard->get_int64(0).value();
        }
    }
    CRPC_CHECK(t_count >= 4);
    CRPC_CHECK_EQ(static_cast<std::int64_t>(0), m_count);
    std::printf("  information_schema.TABLES TIDB_* columns: TiDB=%lld MySQL=%lld\n",
                static_cast<long long>(t_count), static_cast<long long>(m_count));

    // 4) The concurrency outcome differs for the same interleaving: on MySQL
    //    the second writer blocks on the row lock and times out with 1205 at
    //    UPDATE; on TiDB optimistic both writers proceed and the loser gets
    //    9007 at COMMIT (covered by tidb_it_optimistic_write_conflict_9007).
    const std::string table = mysql.database + ".it_comparison_lock";
    CRPC_CHECK(m.execute("DROP TABLE IF EXISTS " + table).ok());
    CRPC_CHECK(m.execute("CREATE TABLE " + table + " (id INT PRIMARY KEY, v INT)").ok());
    CRPC_CHECK(m.execute("INSERT INTO " + table + " VALUES (1, 0)").ok());
    CRPC_CHECK(m.execute("SET SESSION innodb_lock_wait_timeout=1").ok());
    auto m_la = mysql.pool->acquire();
    auto m_lb = mysql.pool->acquire();
    if (!m_la.ok()) {
        std::printf("  mysql second-writer borrow a failed: %s\n",
                    m_la.error().message.c_str());
    }
    if (!m_lb.ok()) {
        std::printf("  mysql second-writer borrow b failed: %s\n",
                    m_lb.error().message.c_str());
    }
    CRPC_CHECK(m_la.ok());
    CRPC_CHECK(m_lb.ok());
    if (m_la.ok() && m_lb.ok()) {
        crpc::db::connection& ma = *m_la.value().get();
        crpc::db::connection& mb = *m_lb.value().get();
        CRPC_CHECK(mb.execute("SET SESSION innodb_lock_wait_timeout=1").ok());
        CRPC_CHECK(ma.begin().ok());
        CRPC_CHECK(ma.execute("UPDATE " + table + " SET v = 1 WHERE id = 1").ok());
        crpc::rpc_error mysql_failure = crpc::rpc_error{crpc::rpc_code::ok, "no error"};
        bool mysql_had_error = false;
        std::thread mysql_other([&] {
            crpc::status st = mb.begin();
            if (!st) {
                mysql_failure = st.error();
                mysql_had_error = true;
                return;
            }
            st = mb.execute("UPDATE " + table + " SET v = 2 WHERE id = 1");
            if (!st) {
                mysql_failure = st.error();
                mysql_had_error = true;
            }
        });
        mysql_other.join();
        CRPC_CHECK(mysql_had_error);
        if (mysql_had_error) {
            const std::string* sql = mysql_failure.metadata.find("sql_code");
            std::string code = sql != nullptr ? *sql : "?";
            std::printf("  concurrency pair: MySQL second writer -> %s \"%s\"; "
                        "TiDB optimistic second writer succeeds, first COMMIT -> 9007\n",
                        code.c_str(), mysql_failure.message.c_str());
            if (sql != nullptr) {
                CRPC_CHECK_EQ(std::string("1205"), *sql);
            }
        }
        (void)ma.rollback();
        (void)mb.rollback();
    }
    CRPC_CHECK(m.execute("DROP TABLE IF EXISTS " + table).ok());
}

CRPC_TEST_MAIN()