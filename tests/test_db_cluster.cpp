// CRPC -- primary/replica cluster: routing, health, transactions, consistency.
//
// The `cluster` sits on pools over fake connections, so every case here is
// deterministic: which factory served a statement is asserted by inspecting the
// executed-statement log of the fake sessions, not by timing.
#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/db/cluster.hpp>
#include <crpc/db/connection.hpp>
#include <crpc/db/mysql/cluster.hpp>
#include <crpc/db/tds/cluster.hpp>
#include <crpc/db/types.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace {

using crpc::db::cluster;
using crpc::db::cluster_options;
using crpc::db::cluster_txn;
using crpc::db::connection;
using crpc::db::connection_factory;
using crpc::db::parameter;
using crpc::db::pool_options;
using crpc::db::read_route;
using crpc::db::replica_consistency;
using crpc::db::replica_depleted_policy;
using crpc::db::result_set;
using crpc::db::route_hint;
using crpc::db::statement_kind;
using crpc::result;
using crpc::rpc_code;
using crpc::rpc_error;
using crpc::span;
using crpc::status;

class fake_result_set final : public result_set {
public:
    explicit fake_result_set(std::uint64_t value = 7) : value_(value) {}
    std::size_t column_count() const noexcept override { return 1; }
    crpc::db::column_info column(std::size_t) const override {
        crpc::db::column_info info;
        info.name = "v";
        return info;
    }
    bool next() override {
        if (read_) {
            return false;
        }
        read_ = true;
        return true;
    }
    bool is_null(std::size_t) const noexcept override { return false; }
    crpc::result<std::int64_t> get_int64(std::size_t) override {
        return static_cast<std::int64_t>(value_);
    }
    crpc::result<std::uint64_t> get_uint64(std::size_t) override { return value_; }
    crpc::result<double> get_double(std::size_t) override { return 0.0; }
    crpc::result<bool> get_bool(std::size_t) override { return false; }
    crpc::result<std::string_view> get_text(std::size_t) override { return std::string_view{}; }
    crpc::result<std::vector<std::byte>> get_blob(std::size_t) override {
        return std::vector<std::byte>{};
    }
    std::int64_t affected_rows() const noexcept override { return 0; }
    void close() noexcept override {}

private:
    std::uint64_t value_;
    bool read_ = false;
};

class fake_connection final : public connection {
public:
    std::string name;
    // Transport behaviour: -1 = never fails; >=0 = succeed that many calls,
    // then every query/ping fails with `fail_code`.
    std::int64_t successes_remaining = -1;
    rpc_code fail_code = rpc_code::unavailable;
    bool in_tx = false;
    bool open = true;
    std::vector<std::string> executed;
    std::uint64_t query_count = 0;
    std::uint64_t pings = 0;
    std::uint64_t rollbacks = 0;
    // An application-level error the query returns instead of succeeding.
    rpc_code app_fail = rpc_code::ok;

    [[nodiscard]] bool down() const noexcept { return successes_remaining == 0; }
    [[nodiscard]] rpc_error transport_error() const {
        return rpc_error{fail_code, name + " transport failure"};
    }

    void tick() {
        if (successes_remaining > 0) {
            --successes_remaining;
        }
    }

    status ping() override {
        ++pings;
        if (down()) {
            return transport_error();
        }
        tick();
        return status{};
    }

    bool in_transaction() const noexcept override { return in_tx; }

    status begin() override {
        if (down()) {
            return transport_error();
        }
        if (in_tx) {
            return status{rpc_code::failed_precondition, "nested transaction"};
        }
        in_tx = true;
        tick();
        executed.push_back("BEGIN");
        return status{};
    }

    status commit() override {
        if (down()) {
            return transport_error();
        }
        in_tx = false;
        tick();
        executed.push_back("COMMIT");
        return status{};
    }

    status rollback() override {
        ++rollbacks;
        in_tx = false;
        executed.push_back("ROLLBACK");
        return down() ? transport_error() : status{};
    }

    status query(std::string_view sql, span<const parameter>, result_set** out) override {
        ++query_count;
        executed.push_back(std::string(sql));
        if (down()) {
            return transport_error();
        }
        tick();
        if (app_fail != rpc_code::ok) {
            return status{app_fail, name + " application error"};
        }
        if (out != nullptr) {
            *out = new fake_result_set();
        }
        return status{};
    }

    void close() noexcept override { open = false; }
    bool is_open() const noexcept override { return open; }
};

class fake_factory final : public connection_factory {
public:
    std::string tag;
    bool fail_opens = false;
    std::uint64_t open_count = 0;
    std::vector<fake_connection*> created;

    result<connection*> open() override {
        ++open_count;
        if (fail_opens) {
            return rpc_error{rpc_code::connection_refused, tag + " refused"};
        }
        auto conn = std::unique_ptr<fake_connection>(new fake_connection);
        conn->name = tag;
        fake_connection* raw = conn.get();
        created.push_back(raw);
        return static_cast<connection*>(conn.release());
    }

    std::string_view name() const noexcept override { return tag; }
};

class fake_consistency final : public replica_consistency {
public:
    std::vector<std::string> captured_on;
    std::vector<std::string> waited_on;
    std::vector<std::string> tokens;
    bool fail_wait = false;

    status capture(connection& conn, std::string& token) override {
        captured_on.push_back(static_cast<fake_connection&>(conn).name);
        token = "tok-" + std::to_string(tokens.size());
        tokens.push_back(token);
        return status{};
    }

    status wait(connection& conn, const std::string& token) override {
        waited_on.push_back(static_cast<fake_connection&>(conn).name);
        tokens.push_back(token);
        if (fail_wait) {
            return status{rpc_code::deadline_exceeded, "replica lagging"};
        }
        return status{};
    }
};

// Deletes a cursor, mirroring `statement_guard`.
void drop(result<result_set*> rows) {
    if (rows.ok()) {
        delete rows.value();
    }
}

std::vector<std::string>& log(connection& conn) {
    return static_cast<fake_connection&>(conn).executed;
}

std::vector<std::string>& log(fake_factory& factory) {
    return factory.created.front()->executed;
}

// A canned multi-column, multi-row result set for the topology parsers.
class row_result_set final : public result_set {
public:
    void add_column(std::string name) { columns_.push_back(std::move(name)); }
    void add_row(std::vector<std::string> row, std::vector<char> nulls) {
        rows_.push_back(std::move(row));
        nulls_.push_back(std::move(nulls));
    }

    std::size_t column_count() const noexcept override { return columns_.size(); }
    crpc::db::column_info column(std::size_t index) const override {
        crpc::db::column_info info;
        info.name = columns_.at(index);
        return info;
    }
    bool next() override {
        ++position_;
        return position_ >= 0 && static_cast<std::size_t>(position_) < rows_.size();
    }
    bool is_null(std::size_t index) const noexcept override {
        const std::vector<char>& nulls = nulls_.at(static_cast<std::size_t>(position_));
        return index < nulls.size() && nulls[index] != 0;
    }
    crpc::result<std::int64_t> get_int64(std::size_t index) override {
        const std::string& value = rows_.at(static_cast<std::size_t>(position_)).at(index);
        return std::stoll(value);
    }
    crpc::result<std::uint64_t> get_uint64(std::size_t index) override {
        return static_cast<std::uint64_t>(get_int64(index).value());
    }
    crpc::result<double> get_double(std::size_t) override { return 0.0; }
    crpc::result<bool> get_bool(std::size_t) override { return false; }
    crpc::result<std::string_view> get_text(std::size_t index) override {
        const std::string& value = rows_.at(static_cast<std::size_t>(position_)).at(index);
        return std::string_view(value);
    }
    crpc::result<std::vector<std::byte>> get_blob(std::size_t) override {
        return std::vector<std::byte>{};
    }
    std::int64_t affected_rows() const noexcept override { return 0; }
    void close() noexcept override {}

private:
    std::vector<std::string> columns_;
    std::vector<std::vector<std::string>> rows_;
    std::vector<std::vector<char>> nulls_;
    std::ptrdiff_t position_ = -1;
};

// Builds a cluster, transferring ownership of the raw factories into it. The
// raw pointers stay valid (the cluster owns the objects) until `db` dies.
struct built {
    fake_factory* primary = nullptr;
    std::vector<fake_factory*> replicas;
    std::unique_ptr<cluster> db;
};

built make_cluster(fake_factory* primary, std::vector<fake_factory*> replicas,
                   std::shared_ptr<replica_consistency> consistency = {},
                   cluster_options options = {}) {
    std::vector<std::unique_ptr<connection_factory>> replica_bases;
    replica_bases.reserve(replicas.size());
    for (fake_factory* replica : replicas) {
        replica_bases.push_back(std::unique_ptr<connection_factory>(replica));
    }
    built out;
    out.primary = primary;
    out.replicas = replicas;
    out.db = std::make_unique<cluster>(
        std::unique_ptr<connection_factory>(primary), std::move(replica_bases),
        std::move(consistency), options);
    return out;
}

cluster_options base_options() {
    cluster_options options;
    options.pool.max_size = 4;
    options.pool.max_idle = 4;
    options.pool.validate_on_borrow = false;
    options.replica_retry_cooldown = std::chrono::milliseconds::zero();
    return options;
}

void consume(cluster& db, std::string_view sql, route_hint hint = {}) {
    drop(db.query(sql, {}, hint));
}

} // namespace

CRPC_TEST(cluster_classify_read_statements) {
    CRPC_CHECK_EQ(static_cast<int>(statement_kind::read),
                  static_cast<int>(crpc::db::classify_statement("SELECT 1")));
    CRPC_CHECK_EQ(static_cast<int>(statement_kind::read),
                  static_cast<int>(crpc::db::classify_statement("  /* hi */ SELECT 1")));
    CRPC_CHECK_EQ(static_cast<int>(statement_kind::read),
                  static_cast<int>(crpc::db::classify_statement("-- comment\nSELECT 1")));
    CRPC_CHECK_EQ(static_cast<int>(statement_kind::read),
                  static_cast<int>(crpc::db::classify_statement("# c\nSELECT 1")));
    CRPC_CHECK_EQ(static_cast<int>(statement_kind::read),
                  static_cast<int>(crpc::db::classify_statement("SHOW TABLES")));
    CRPC_CHECK_EQ(static_cast<int>(statement_kind::read),
                  static_cast<int>(crpc::db::classify_statement("DESCRIBE t")));
    CRPC_CHECK_EQ(static_cast<int>(statement_kind::read),
                  static_cast<int>(crpc::db::classify_statement("DESC t")));
    CRPC_CHECK_EQ(static_cast<int>(statement_kind::read),
                  static_cast<int>(crpc::db::classify_statement("EXPLAIN SELECT 1")));
    CRPC_CHECK_EQ(
        static_cast<int>(statement_kind::read),
        static_cast<int>(crpc::db::classify_statement("WITH c AS (SELECT 1) SELECT * FROM c")));
    CRPC_CHECK_EQ(static_cast<int>(statement_kind::read),
                  static_cast<int>(crpc::db::classify_statement(
                      "SELECT 'insert into nowhere values(1)' FROM t")));
    CRPC_CHECK_EQ(static_cast<int>(statement_kind::read),
                  static_cast<int>(crpc::db::classify_statement(
                      "SELECT /* update is in a comment */ 1")));
    CRPC_CHECK_EQ(static_cast<int>(statement_kind::read),
                  static_cast<int>(crpc::db::classify_statement(
                      "SELECT CASE WHEN 1 THEN 'a' ELSE 'b' END")));
}

CRPC_TEST(cluster_classify_write_statements) {
    CRPC_CHECK_EQ(static_cast<int>(statement_kind::write),
                  static_cast<int>(crpc::db::classify_statement("INSERT INTO t VALUES (1)")));
    CRPC_CHECK_EQ(static_cast<int>(statement_kind::write),
                  static_cast<int>(crpc::db::classify_statement("UPDATE t SET a = 1")));
    CRPC_CHECK_EQ(static_cast<int>(statement_kind::write),
                  static_cast<int>(crpc::db::classify_statement("DELETE FROM t")));
    CRPC_CHECK_EQ(static_cast<int>(statement_kind::write),
                  static_cast<int>(crpc::db::classify_statement("CREATE TABLE t (a INT)")));
    CRPC_CHECK_EQ(static_cast<int>(statement_kind::write),
                  static_cast<int>(crpc::db::classify_statement("DROP TABLE t")));
    CRPC_CHECK_EQ(static_cast<int>(statement_kind::write),
                  static_cast<int>(crpc::db::classify_statement("SELECT 1; INSERT INTO t VALUES (1)")));
    CRPC_CHECK_EQ(static_cast<int>(statement_kind::write),
                  static_cast<int>(crpc::db::classify_statement(
                      "WITH c AS (SELECT 1) UPDATE t SET a = 1")));
    CRPC_CHECK_EQ(static_cast<int>(statement_kind::write),
                  static_cast<int>(crpc::db::classify_statement("SET @a = 1")));
    CRPC_CHECK_EQ(static_cast<int>(statement_kind::write),
                  static_cast<int>(crpc::db::classify_statement("BEGIN")));
    CRPC_CHECK_EQ(static_cast<int>(statement_kind::write),
                  static_cast<int>(crpc::db::classify_statement("COMMIT")));
    CRPC_CHECK_EQ(static_cast<int>(statement_kind::write),
                  static_cast<int>(crpc::db::classify_statement("REPLACE INTO t VALUES (1)")));
    CRPC_CHECK_EQ(static_cast<int>(statement_kind::write),
                  static_cast<int>(crpc::db::classify_statement("CALL p()")));
    CRPC_CHECK_EQ(static_cast<int>(statement_kind::write),
                  static_cast<int>(crpc::db::classify_statement("GRANT SELECT ON t TO u")));
    CRPC_CHECK_EQ(static_cast<int>(statement_kind::write),
                  static_cast<int>(crpc::db::classify_statement("SHOW CREATE TABLE t")));
    CRPC_CHECK_EQ(static_cast<int>(statement_kind::write),
                  static_cast<int>(crpc::db::classify_statement("GARBAGE DIRECTIVE")));
}

CRPC_TEST(cluster_writes_always_land_on_the_primary) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    auto* replica = new fake_factory;
    replica->tag = "r";
    cluster_options options = base_options();
    options.reads = read_route::replica; // strictest mode: reads must use replicas
    built b = make_cluster(primary, {replica}, {}, options);

    CRPC_CHECK(b.db->execute("INSERT INTO t VALUES (1)").ok());
    consume(*b.db, "SELECT 1");

    const std::vector<std::string>& primary_log = log(*primary);
    const std::vector<std::string>& replica_log = log(*replica->created.front());
    CRPC_CHECK(!primary_log.empty());
    CRPC_CHECK(std::string(primary_log.back()) == "INSERT INTO t VALUES (1)");
    CRPC_CHECK_EQ("SELECT 1", replica_log.back());
    for (const std::string& statement : replica_log) {
        CRPC_CHECK(statement.find("INSERT") == std::string::npos);
    }
}

CRPC_TEST(cluster_default_reads_stay_on_primary) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    auto* replica = new fake_factory;
    replica->tag = "r";
    built b = make_cluster(primary, {replica}); // default read_route::primary

    consume(*b.db, "SELECT 1");
    CRPC_CHECK(b.db->execute("INSERT INTO t VALUES (1)").ok());

    const std::vector<std::string>& primary_log = log(*primary);
    CRPC_CHECK_EQ("SELECT 1", primary_log.at(0));
    CRPC_CHECK_EQ("INSERT INTO t VALUES (1)", primary_log.at(1));
    CRPC_CHECK(replica->created.empty());
}

CRPC_TEST(cluster_replica_route_round_robins_across_replicas) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    auto* r0 = new fake_factory;
    r0->tag = "r0";
    auto* r1 = new fake_factory;
    r1->tag = "r1";
    cluster_options options = base_options();
    options.reads = read_route::replica;
    built b = make_cluster(primary, {r0, r1}, {}, options);

    consume(*b.db, "SELECT 1");
    consume(*b.db, "SELECT 2");
    consume(*b.db, "SELECT 3");

    CRPC_CHECK_EQ("SELECT 1", log(*r0->created.front()).at(0));
    CRPC_CHECK_EQ("SELECT 2", log(*r1->created.front()).at(0));
    CRPC_CHECK_EQ("SELECT 3", log(*r0->created.front()).at(1));
    CRPC_CHECK(primary->created.empty());
}

CRPC_TEST(cluster_prefer_replica_with_zero_replicas_uses_primary) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    cluster_options options = base_options();
    options.reads = read_route::prefer_replica;
    built b = make_cluster(primary, {}, {}, options);

    consume(*b.db, "SELECT 1");
    CRPC_CHECK_EQ("SELECT 1", log(*primary->created.front()).at(0));
}

CRPC_TEST(cluster_replica_route_with_zero_replicas_fails) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    cluster_options options = base_options();
    options.reads = read_route::replica;
    built b = make_cluster(primary, {}, {}, options);

    const result<result_set*> rows = b.db->query("SELECT 1");
    CRPC_CHECK_FALSE(rows.ok());
    if (!rows.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(rpc_code::failed_precondition),
                      static_cast<int>(rows.error().code));
    }
    CRPC_CHECK(primary->created.empty());
}

CRPC_TEST(cluster_replica_depleted_fallback_serves_the_primary) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    auto* replica = new fake_factory;
    replica->tag = "r";
    replica->fail_opens = true;
    cluster_options options = base_options();
    options.reads = read_route::replica;
    options.depleted = replica_depleted_policy::fallback_to_primary;
    options.replica_failure_threshold = 1;
    built b = make_cluster(primary, {replica}, {}, options);

    consume(*b.db, "SELECT 1");
    CRPC_CHECK_EQ("SELECT 1", log(*primary->created.front()).at(0));
    const std::vector<crpc::db::replica_health> health = b.db->replica_health();
    CRPC_CHECK_EQ(std::size_t{1}, health.size());
    CRPC_CHECK_FALSE(health.at(0).alive);
    CRPC_CHECK_EQ(std::uint32_t{1}, health.at(0).consecutive_failures);
}

CRPC_TEST(cluster_replica_depleted_error_fails_the_read) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    auto* replica = new fake_factory;
    replica->tag = "r";
    replica->fail_opens = true;
    cluster_options options = base_options();
    options.reads = read_route::replica;
    options.depleted = replica_depleted_policy::error;
    options.replica_failure_threshold = 1;
    built b = make_cluster(primary, {replica}, {}, options);

    const result<result_set*> first = b.db->query("SELECT 1");
    CRPC_CHECK_FALSE(first.ok());
    if (!first.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(rpc_code::connection_refused),
                      static_cast<int>(first.error().code));
    }
    // Ejected: the routed read fails without a statement being served anywhere
    // except the probe's ping. The fail-open replica answered nothing at all.
    const result<result_set*> second = b.db->query("SELECT 2");
    CRPC_CHECK_FALSE(second.ok());
    if (!second.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(rpc_code::unavailable),
                      static_cast<int>(second.error().code));
    }
    CRPC_CHECK(primary->created.empty());
    CRPC_CHECK(replica->created.empty());
}

CRPC_TEST(cluster_prefer_replica_falls_back_when_all_replicas_die) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    auto* replica = new fake_factory;
    replica->tag = "r";
    replica->fail_opens = true;
    cluster_options options = base_options();
    options.reads = read_route::prefer_replica;
    options.replica_failure_threshold = 1;
    built b = make_cluster(primary, {replica}, {}, options);

    // prefer_replica always falls back, regardless of the depleted policy.
    (void)b.db->replica_health();
    consume(*b.db, "SELECT 1");
    consume(*b.db, "SELECT 2");
    CRPC_CHECK_EQ("SELECT 1", log(*primary->created.front()).at(0));
    const std::vector<crpc::db::replica_health> health = b.db->replica_health();
    CRPC_CHECK_FALSE(health.at(0).alive);
}

CRPC_TEST(cluster_round_robin_includes_the_primary) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    auto* replica = new fake_factory;
    replica->tag = "r";
    cluster_options options = base_options();
    options.reads = read_route::round_robin;
    built b = make_cluster(primary, {replica}, {}, options);

    for (int i = 1; i <= 4; ++i) {
        consume(*b.db, "SELECT " + std::to_string(i));
    }
    CRPC_CHECK_EQ("SELECT 1", log(*primary->created.front()).at(0));
    CRPC_CHECK_EQ("SELECT 2", log(*replica->created.front()).at(0));
    CRPC_CHECK_EQ("SELECT 3", log(*primary->created.front()).at(1));
    CRPC_CHECK_EQ("SELECT 4", log(*replica->created.front()).at(1));
}

CRPC_TEST(cluster_hint_forces_primary_over_a_replica_route) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    auto* replica = new fake_factory;
    replica->tag = "r";
    cluster_options options = base_options();
    options.reads = read_route::replica;
    built b = make_cluster(primary, {replica}, {}, options);

    consume(*b.db, "SELECT 1", route_hint::primary());
    CRPC_CHECK_EQ("SELECT 1", log(*primary->created.front()).at(0));
    CRPC_CHECK(replica->created.empty());
}

CRPC_TEST(cluster_hint_forces_replica_over_a_primary_route) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    auto* replica = new fake_factory;
    replica->tag = "r";
    built b = make_cluster(primary, {replica}); // default read_route::primary

    consume(*b.db, "SELECT 1", route_hint::replica());
    CRPC_CHECK_EQ("SELECT 1", log(*replica->created.front()).at(0));
    CRPC_CHECK(primary->created.empty());
}

CRPC_TEST(cluster_hint_cannot_send_a_write_to_a_replica) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    auto* replica = new fake_factory;
    replica->tag = "r";
    built b = make_cluster(primary, {replica});

    const status refused = b.db->execute("INSERT INTO t VALUES (1)", {}, route_hint::replica());
    CRPC_CHECK_FALSE(refused.ok());
    if (!refused.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(rpc_code::invalid_argument),
                      static_cast<int>(refused.error().code));
    }
    CRPC_CHECK(replica->created.empty());
}

CRPC_TEST(cluster_application_error_on_replica_is_forwarded_and_not_ejected) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    auto* replica = new fake_factory;
    replica->tag = "r";
    cluster_options options = base_options();
    options.reads = read_route::replica;
    options.replica_failure_threshold = 1;
    built b = make_cluster(primary, {replica}, {}, options);

    // The replica answers, but with a server-side error. That proves it is
    // reachable, so it must not be counted as a transport failure.
    replica->created.clear();
    consume(*b.db, "SELECT 1"); // opens the replica session
    fake_connection& conn = *replica->created.front();
    conn.app_fail = rpc_code::not_found;

    const result<result_set*> failed = b.db->query("SELECT missing");
    CRPC_CHECK_FALSE(failed.ok());
    if (!failed.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(rpc_code::not_found),
                      static_cast<int>(failed.error().code));
    }
    const std::vector<crpc::db::replica_health> health = b.db->replica_health();
    CRPC_CHECK(health.at(0).alive);
    CRPC_CHECK_EQ(std::uint32_t{0}, health.at(0).consecutive_failures);
}

CRPC_TEST(cluster_transaction_keeps_every_statement_on_the_primary) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    auto* replica = new fake_factory;
    replica->tag = "r";
    cluster_options options = base_options();
    options.reads = read_route::replica; // the trap: SELECTs in the tx must NOT escape
    built b = make_cluster(primary, {replica}, {}, options);

    auto tx = b.db->begin_transaction();
    CRPC_CHECK(tx.ok());
    if (!tx.ok()) {
        return;
    }
    CRPC_CHECK(tx.value().in_transaction());
    connection* session = tx.value().session();
    CRPC_CHECK(session != nullptr);

    // The SELECT inside the transaction is a read; without transaction affinity
    // it would be routed to the replica. It must land on the same primary
    // connection that opened the transaction.
    drop(tx.value().query("SELECT 1"));
    drop(tx.value().query("SELECT CONNECTION_ID()"));

    CRPC_CHECK_EQ(std::size_t{3}, log(*session).size());
    CRPC_CHECK_EQ("BEGIN", log(*session).at(0));
    CRPC_CHECK_EQ("SELECT 1", log(*session).at(1));
    CRPC_CHECK_EQ("SELECT CONNECTION_ID()", log(*session).at(2));
    // Replication of the same INSERT runs on the pinned connection too.
    CRPC_CHECK(tx.value().execute("INSERT INTO t VALUES (1)").ok());
    CRPC_CHECK_EQ("INSERT INTO t VALUES (1)", log(*session).at(3));
    CRPC_CHECK(replica->created.empty()); // the replica never saw the tx

    const status committed = tx.value().commit();
    CRPC_CHECK(committed.ok());
    CRPC_CHECK_FALSE(tx.value().in_transaction());
    CRPC_CHECK(tx.value().session() == nullptr);
    CRPC_CHECK_EQ("COMMIT", log(*session).at(4));

    // After the transaction, plain reads are routed by the cluster again.
    consume(*b.db, "SELECT 2");
    CRPC_CHECK_EQ("SELECT 2", log(*replica->created.front()).at(0));
}

CRPC_TEST(cluster_transaction_rolls_back_or_commits) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    built b = make_cluster(primary, {});

    {
        auto tx = b.db->begin_transaction();
        CRPC_CHECK(tx.ok());
        if (!tx.ok()) {
            return;
        }
        // The cluster opens its connections lazily, so the fake session only
        // exists once the transaction has acquired one.
        CRPC_CHECK_FALSE(primary->created.empty());
        drop(tx.value().query("SELECT 1"));
        CRPC_CHECK(tx.value().rollback().ok());
        CRPC_CHECK_FALSE(tx.value().in_transaction());
        // The lease returned to the pool: the same session serves again.
    }
    // The cluster opens its connections lazily, so the fake session only
    // exists once the transaction above has acquired one.
    CRPC_CHECK_FALSE(primary->created.empty());
    fake_connection& conn = *primary->created.front();
    CRPC_CHECK_EQ("ROLLBACK", conn.executed.back());
    consume(*b.db, "SELECT 2");
    CRPC_CHECK_EQ("SELECT 2", conn.executed.back());

    {
        auto tx = b.db->begin_transaction();
        CRPC_CHECK(tx.ok());
        if (!tx.ok()) {
            return;
        }
        CRPC_CHECK(tx.value().commit().ok());
    }
    CRPC_CHECK_EQ("COMMIT", conn.executed.back());
}

CRPC_TEST(cluster_transaction_destructor_rolls_back) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    built b = make_cluster(primary, {});

    {
        auto tx = b.db->begin_transaction();
        CRPC_CHECK(tx.ok());
        if (!tx.ok()) {
            return;
        }
        drop(tx.value().query("SELECT 1"));
        // leaving scope without commit rolls back
    }
    // The cluster opens its connections lazily: the session exists only after
    // the transaction above acquired one.
    CRPC_CHECK_FALSE(primary->created.empty());
    fake_connection& conn = *primary->created.front();
    CRPC_CHECK_EQ("ROLLBACK", conn.executed.back());
    // Still usable after the leaked transaction was rolled back.
    consume(*b.db, "SELECT 2");
    CRPC_CHECK_EQ("SELECT 2", conn.executed.back());
}

CRPC_TEST(cluster_transaction_refuses_a_replica_hint) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    auto* replica = new fake_factory;
    replica->tag = "r";
    built b = make_cluster(primary, {replica});

    auto tx = b.db->begin_transaction();
    CRPC_CHECK(tx.ok());
    if (!tx.ok()) {
        return;
    }
    const result<result_set*> refused = tx.value().query("SELECT 1", {}, route_hint::replica());
    CRPC_CHECK_FALSE(refused.ok());
    if (!refused.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(rpc_code::invalid_argument),
                      static_cast<int>(refused.error().code));
    }
    CRPC_CHECK(tx.value().rollback().ok());
}

CRPC_TEST(cluster_transaction_after_close_is_refused) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    built b = make_cluster(primary, {});

    auto tx = b.db->begin_transaction();
    CRPC_CHECK(tx.ok());
    if (!tx.ok()) {
        return;
    }
    CRPC_CHECK(tx.value().rollback().ok());
    const result<result_set*> late = tx.value().query("SELECT 1");
    CRPC_CHECK_FALSE(late.ok());
    if (!late.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(rpc_code::failed_precondition),
                      static_cast<int>(late.error().code));
    }
}

CRPC_TEST(cluster_write_captures_a_consistency_token) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    auto consistency = std::make_shared<fake_consistency>();
    built b = make_cluster(primary, {}, consistency);

    CRPC_CHECK(b.db->execute("INSERT INTO t VALUES (1)").ok());
    CRPC_CHECK_EQ(std::size_t{1}, consistency->captured_on.size());
    CRPC_CHECK_EQ("p", consistency->captured_on.front());
    CRPC_CHECK_EQ(std::size_t{1}, consistency->tokens.size());
    CRPC_CHECK(consistency->waited_on.empty()); // a plain read is not a wait
}

CRPC_TEST(cluster_consistent_read_waits_on_the_replica_before_selecting) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    auto* replica = new fake_factory;
    replica->tag = "r";
    auto consistency = std::make_shared<fake_consistency>();
    cluster_options options = base_options();
    options.reads = read_route::replica;
    built b = make_cluster(primary, {replica}, consistency, options);

    CRPC_CHECK(b.db->execute("INSERT INTO t VALUES (1)").ok());
    drop(b.db->query("SELECT BIG", {}, crpc::db::route_hint::read_your_writes()));

    // The wait ran on the replica connection that then served the SELECT.
    CRPC_CHECK_EQ(std::size_t{1}, consistency->waited_on.size());
    CRPC_CHECK_EQ("r", consistency->waited_on.front());
    CRPC_CHECK_EQ("tok-0", consistency->tokens.at(0));
    const std::vector<std::string>& replica_log = log(*replica->created.front());
    CRPC_CHECK_EQ("SELECT BIG", replica_log.back());
}

CRPC_TEST(cluster_consistent_read_on_the_primary_skips_the_wait) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    auto consistency = std::make_shared<fake_consistency>();
    built b = make_cluster(primary, {}, consistency);

    CRPC_CHECK(b.db->execute("INSERT INTO t VALUES (1)").ok());
    drop(b.db->query("SELECT 1", {}, crpc::db::route_hint::read_your_writes()));
    // The primary is fresh by definition; no wait, no extra capture.
    CRPC_CHECK(consistency->waited_on.empty());
    CRPC_CHECK_EQ(std::size_t{1}, consistency->captured_on.size());
}

CRPC_TEST(cluster_consistent_read_with_no_prior_write_skips_the_wait) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    auto* replica = new fake_factory;
    replica->tag = "r";
    auto consistency = std::make_shared<fake_consistency>();
    cluster_options options = base_options();
    options.reads = read_route::replica;
    built b = make_cluster(primary, {replica}, consistency, options);

    drop(b.db->query("SELECT 1", {}, crpc::db::route_hint::read_your_writes()));
    CRPC_CHECK(consistency->waited_on.empty());
    CRPC_CHECK_EQ("SELECT 1", log(*replica->created.front()).at(0));
}

CRPC_TEST(cluster_wait_failure_is_not_a_transport_failure) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    auto* replica = new fake_factory;
    replica->tag = "r";
    auto consistency = std::make_shared<fake_consistency>();
    consistency->fail_wait = true;
    cluster_options options = base_options();
    options.reads = read_route::replica;
    options.depleted = replica_depleted_policy::error;
    options.replica_failure_threshold = 1;
    built b = make_cluster(primary, {replica}, consistency, options);

    CRPC_CHECK(b.db->execute("INSERT INTO t VALUES (1)").ok());
    const result<result_set*> stale_lag =
        b.db->query("SELECT 1", {}, crpc::db::route_hint::read_your_writes());
    CRPC_CHECK_FALSE(stale_lag.ok());
    if (!stale_lag.ok()) {
        // The replica answered but could not catch up: the wait's own failure,
        // not a transport failure that would eject the replica.
        CRPC_CHECK_EQ(static_cast<int>(rpc_code::deadline_exceeded),
                      static_cast<int>(stale_lag.error().code));
    }
    const std::vector<crpc::db::replica_health> health = b.db->replica_health();
    CRPC_CHECK(health.at(0).alive);
    CRPC_CHECK_EQ(std::uint32_t{0}, health.at(0).consecutive_failures);
}

CRPC_TEST(cluster_wait_failure_falls_back_to_the_primary) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    auto* replica = new fake_factory;
    replica->tag = "r";
    auto consistency = std::make_shared<fake_consistency>();
    consistency->fail_wait = true;
    cluster_options options = base_options();
    options.reads = read_route::prefer_replica;
    built b = make_cluster(primary, {replica}, consistency, options);

    CRPC_CHECK(b.db->execute("INSERT INTO t VALUES (1)").ok());
    // prefer_replica: a lagging replica falls back to the primary, which has
    // already applied the write -- consistency holds without a wait.
    drop(b.db->query("SELECT 1", {}, crpc::db::route_hint::read_your_writes()));
    CRPC_CHECK_EQ("SELECT 1", log(*primary->created.front()).at(1));
}

CRPC_TEST(cluster_replica_ejected_after_consecutive_failures) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    auto* replica = new fake_factory;
    replica->tag = "r";
    cluster_options options = base_options();
    options.reads = read_route::replica;
    options.depleted = replica_depleted_policy::error;
    options.replica_failure_threshold = 3;
    built b = make_cluster(primary, {replica}, {}, options);

    // Open the replica session, then make it fail after one successful read.
    const result<bool> warm = b.db->probe_replica(0);
    CRPC_CHECK(warm.ok());
    fake_connection& conn = *replica->created.front();
    conn.successes_remaining = 1;

    consume(*b.db, "SELECT 1");
    for (int i = 0; i < 3; ++i) {
        const result<result_set*> failed = b.db->query("SELECT " + std::to_string(i + 2));
        CRPC_CHECK_FALSE(failed.ok());
        CRPC_CHECK(conn.down());
    }
    {
        const std::vector<crpc::db::replica_health> health = b.db->replica_health();
        CRPC_CHECK_FALSE(health.at(0).alive);
        CRPC_CHECK_EQ(std::uint32_t{3}, health.at(0).consecutive_failures);
    }
    // Ejected: the next read fails without contacting the replica at all.
    const std::uint64_t attempts = conn.query_count;
    const result<result_set*> rejected = b.db->query("SELECT 9");
    CRPC_CHECK_FALSE(rejected.ok());
    if (!rejected.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(rpc_code::unavailable),
                      static_cast<int>(rejected.error().code));
    }
    CRPC_CHECK_EQ(attempts, conn.query_count);
    CRPC_CHECK(primary->created.empty());
}

CRPC_TEST(cluster_replica_recovers_through_a_successful_probe) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    auto* replica = new fake_factory;
    replica->tag = "r";
    cluster_options options = base_options();
    options.reads = read_route::replica;
    options.depleted = replica_depleted_policy::error;
    options.replica_failure_threshold = 2;
    built b = make_cluster(primary, {replica}, {}, options);

    const result<bool> warm = b.db->probe_replica(0);
    CRPC_CHECK(warm.ok());
    fake_connection& conn = *replica->created.front();
    conn.successes_remaining = 1;

    consume(*b.db, "SELECT 1"); // succeeds; the session now starts failing
    consume(*b.db, "SELECT 2"); // first failure
    consume(*b.db, "SELECT 3"); // second failure -> ejected
    {
        const std::vector<crpc::db::replica_health> health = b.db->replica_health();
        CRPC_CHECK_FALSE(health.at(0).alive);
    }

    // While it stays down, the probe keeps failing.
    const result<bool> bad_probe = b.db->probe_replica(0);
    CRPC_CHECK(bad_probe.ok());
    if (bad_probe.ok()) {
        CRPC_CHECK_FALSE(bad_probe.value());
    }

    // The replica comes back; the next probe re-admits it.
    conn.successes_remaining = 10;
    const result<bool> good_probe = b.db->probe_replica(0);
    CRPC_CHECK(good_probe.ok());
    if (good_probe.ok()) {
        CRPC_CHECK(good_probe.value());
    }
    {
        const std::vector<crpc::db::replica_health> health = b.db->replica_health();
        CRPC_CHECK(health.at(0).alive);
        CRPC_CHECK_EQ(std::uint32_t{0}, health.at(0).consecutive_failures);
    }
    // A read is routed to the healed replica again.
    consume(*b.db, "SELECT 4");
    const std::vector<std::string>& replica_log = log(*replica->created.front());
    CRPC_CHECK_EQ("SELECT 4", replica_log.back());
}

CRPC_TEST(cluster_failure_threshold_zero_disables_ejection) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    auto* replica = new fake_factory;
    replica->tag = "r";
    cluster_options options = base_options();
    options.reads = read_route::replica;
    options.depleted = replica_depleted_policy::error;
    options.replica_failure_threshold = 0;
    built b = make_cluster(primary, {replica}, {}, options);

    const result<bool> warm = b.db->probe_replica(0);
    CRPC_CHECK(warm.ok());
    fake_connection& conn = *replica->created.front();
    conn.successes_remaining = 1;

    consume(*b.db, "SELECT 1");
    const result<result_set*> failed = b.db->query("SELECT 2");
    CRPC_CHECK_FALSE(failed.ok());
    CRPC_CHECK(conn.down());
    // Counted, but never ejected.
    const std::vector<crpc::db::replica_health> health = b.db->replica_health();
    CRPC_CHECK(health.at(0).alive);
    CRPC_CHECK_EQ(std::uint32_t{1}, health.at(0).consecutive_failures);
}

CRPC_TEST(cluster_primary_failure_is_reported_and_no_replica_is_promoted) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    primary->fail_opens = true;
    auto* replica = new fake_factory;
    replica->tag = "r0";
    cluster_options options = base_options();
    options.reads = read_route::prefer_replica;
    built b = make_cluster(primary, {replica}, {}, options);

    // Writes fail with the primary's transport error...
    const status write = b.db->execute("INSERT INTO t VALUES (1)");
    CRPC_CHECK_FALSE(write.ok());
    if (!write.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(rpc_code::connection_refused),
                      static_cast<int>(write.error().code));
    }
    // ...reads keep working off the replicas (no parallel split)...
    consume(*b.db, "SELECT 1");
    CRPC_CHECK_EQ("SELECT 1", log(*replica->created.front()).at(0));
    // ...and nothing was relabelled: the replica is still a replica.
    const std::vector<crpc::db::replica_health> health = b.db->replica_health();
    CRPC_CHECK_EQ(std::size_t{1}, health.size());
    CRPC_CHECK_EQ("r0", health.at(0).name);
    CRPC_CHECK_EQ(std::size_t{1}, b.db->replica_count());
}

CRPC_TEST(cluster_zero_replicas_is_a_valid_configuration) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    built b = make_cluster(primary, {});
    CRPC_CHECK_EQ(std::size_t{0}, b.db->replica_count());
    CRPC_CHECK(b.db->replica_health().empty());
    // Only the 'replica' route mode is affected; every other mode serves reads.
    CRPC_CHECK(b.db->execute("INSERT INTO t VALUES (1)").ok());
    consume(*b.db, "SELECT 1");
    CRPC_CHECK_EQ("SELECT 1", log(*primary->created.front()).at(1));
}

CRPC_TEST(cluster_probe_out_of_range_is_invalid_argument) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    built b = make_cluster(primary, {});
    const result<bool> probe = b.db->probe_replica(3);
    CRPC_CHECK_FALSE(probe.ok());
    if (!probe.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(rpc_code::invalid_argument),
                      static_cast<int>(probe.error().code));
    }
}

CRPC_TEST(cluster_close_rejects_further_queries) {
    auto* primary = new fake_factory;
    primary->tag = "p";
    built b = make_cluster(primary, {});
    b.db->close();
    CRPC_CHECK(b.db->is_closed());
    const result<result_set*> rows = b.db->query("SELECT 1");
    CRPC_CHECK_FALSE(rows.ok());
    if (!rows.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(rpc_code::shutting_down),
                      static_cast<int>(rows.error().code));
    }
}

CRPC_TEST(cluster_parse_show_replicas) {
    row_result_set rows;
    rows.add_column("Server_id");
    rows.add_column("Host");
    rows.add_column("Port");
    rows.add_column("Source_id");
    rows.add_row({"2", "127.0.0.1", "3307", "1"}, {0, 0, 0, 0});
    rows.add_row({"3", "replica.example", "3308", "1"}, {0, 0, 0, 0});
    rows.add_row({"4", "noport.example", "", "1"}, {0, 0, 1, 0});

    std::vector<crpc::db::mysql::discovered_replica> out;
    const crpc::status parsed = crpc::db::mysql::parse_show_replicas(rows, out);
    CRPC_CHECK(parsed.ok());
    CRPC_CHECK_EQ(std::size_t{2}, out.size());
    CRPC_CHECK_EQ(std::string("127.0.0.1"), out.at(0).host);
    CRPC_CHECK_EQ(std::uint16_t{3307}, out.at(0).port);
    CRPC_CHECK_EQ(std::uint32_t{2}, out.at(0).server_id);
    CRPC_CHECK_EQ(std::string("replica.example"), out.at(1).host);
    CRPC_CHECK_EQ(std::uint16_t{3308}, out.at(1).port);
}

CRPC_TEST(cluster_parse_show_replicas_requires_host) {
    row_result_set rows;
    rows.add_column("Server_id");
    rows.add_column("Port");
    rows.add_row({"2", "3307"}, {0, 0});
    std::vector<crpc::db::mysql::discovered_replica> out;
    const crpc::status parsed = crpc::db::mysql::parse_show_replicas(rows, out);
    CRPC_CHECK_FALSE(parsed.ok());
    if (!parsed.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(rpc_code::protocol_error),
                      static_cast<int>(parsed.error().code));
    }
}

CRPC_TEST(cluster_parse_availability_roles_takes_secondaries) {
    row_result_set rows;
    rows.add_column("replica_server_name");
    rows.add_column("role_desc");
    rows.add_row({"NODE-A", "PRIMARY"}, {0, 0});
    rows.add_row({"NODE-B", "SECONDARY"}, {0, 0});
    rows.add_row({"NODE-C", "SECONDARY"}, {0, 0});

    std::vector<crpc::db::tds::discovered_replica> out;
    const crpc::status parsed = crpc::db::tds::parse_availability_roles(rows, out);
    CRPC_CHECK(parsed.ok());
    // The PRIMARY row names the writer; only SECONDARY rows become replicas.
    CRPC_CHECK_EQ(std::size_t{2}, out.size());
    CRPC_CHECK_EQ(std::string("NODE-B"), out.at(0).host);
    CRPC_CHECK_EQ(std::string("SECONDARY"), out.at(0).role);
}

CRPC_TEST(cluster_parse_availability_roles_requires_columns) {
    row_result_set rows;
    rows.add_column("role_desc");
    rows.add_row({"SECONDARY"}, {0});
    std::vector<crpc::db::tds::discovered_replica> out;
    const crpc::status parsed = crpc::db::tds::parse_availability_roles(rows, out);
    CRPC_CHECK_FALSE(parsed.ok());
    if (!parsed.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(rpc_code::protocol_error),
                      static_cast<int>(parsed.error().code));
    }
}

CRPC_TEST_MAIN()