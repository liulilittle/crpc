// CRPC -- MySQL primary/replica cluster, against two real servers.
//
// Requires CRPC_MYSQL_IT=1 and a real replica reachable at
// CRPC_MYSQL_HOST:CRPC_MYSQL_REPLICA_PORT (default 127.0.0.1:3307) that
// replicates from the primary at CRPC_MYSQL_HOST:CRPC_MYSQL_PORT and runs with
// read_only=ON. The evidence produced here is what a scripted channel cannot:
// real replication, real server error 1290 on a read-only replica, and a real
// WAIT_FOR_EXECUTED_GTID_SET round trip.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/db/channel.hpp>
#include <crpc/db/cluster.hpp>
#include <crpc/db/connection.hpp>
#include <crpc/db/mysql/cluster.hpp>
#include <crpc/db/mysql/connection.hpp>
#include <crpc/db/types.hpp>
#include <crpc/error.hpp>
#include <crpc/io.hpp>
#include "test_tcp_backend.hpp"

namespace {

[[nodiscard]] std::string env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::string(value) : std::string(fallback);
}

[[nodiscard]] bool it_enabled() {
    const char* value = std::getenv("CRPC_MYSQL_IT");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

[[nodiscard]] const char* require_server() {
    if (!it_enabled()) {
        crpc_test::skip("CRPC_MYSQL_IT is not 1");
    }
    return "";
}

[[nodiscard]] std::string primary_host() { return env_or("CRPC_MYSQL_HOST", "127.0.0.1"); }
[[nodiscard]] std::uint16_t primary_port() {
    return static_cast<std::uint16_t>(std::stoi(env_or("CRPC_MYSQL_PORT", "3306")));
}
[[nodiscard]] std::uint16_t replica_port() {
    return static_cast<std::uint16_t>(
        std::stoi(env_or("CRPC_MYSQL_REPLICA_PORT", "3307")));
}
[[nodiscard]] std::uint16_t dead_port() {
    return static_cast<std::uint16_t>(
        std::stoi(env_or("CRPC_MYSQL_DEAD_PORT", "3399")));
}

// One raw session to an explicit endpoint, used to write directly to the
// replica and to control its replication threads.
class raw_session {
public:
    explicit raw_session(std::uint16_t port) {
        crpc::db::mysql::connection_options options;
        options.user = env_or("CRPC_MYSQL_USER", "crpc");
        options.password = env_or("CRPC_MYSQL_PASSWORD", "crpc_pw");
        options.database = env_or("CRPC_MYSQL_DB", "crpc_it");
        crpc::net::connection_config config;
        config.ep = crpc::endpoint::tcp(primary_host(), port);
        config.operation_timeout = std::chrono::seconds(15);
        auto link = std::make_unique<crpc::db::socket_channel>(std::move(config), backend_);
        conn_ = std::unique_ptr<crpc::db::mysql::connection>(
            new crpc::db::mysql::connection(options, std::move(link)));
    }

    [[nodiscard]] crpc::db::mysql::connection& get() const { return *conn_; }
    [[nodiscard]] bool ok() const { return conn_ != nullptr && conn_->is_open(); }
    // The handshake runs in the constructor, so a failed connection carries the
    // reason. An integration failure is only actionable with it.
    [[nodiscard]] std::string error() const {
        if (conn_ == nullptr) {
            return "no connection";
        }
        const crpc::rpc_error& err = conn_->connect_error();
        return err.message.empty() ? std::string("connection is closed") : err.message;
    }

private:
    tcp_backend backend_;
    std::unique_ptr<crpc::db::mysql::connection> conn_;
};

void drop(crpc::result<crpc::db::result_set*> rows) {
    if (rows.ok()) {
        delete rows.value();
    }
}

// A cluster built against the real primary plus the given replica ports.
struct it_cluster {
    tcp_backend backend;
    std::unique_ptr<crpc::db::cluster> db;

    // `tune` runs last so a case can override any option the defaults set below.
    it_cluster(crpc::db::read_route reads = crpc::db::read_route::primary,
               crpc::db::replica_depleted_policy depleted =
                   crpc::db::replica_depleted_policy::fallback_to_primary,
               std::vector<std::uint16_t> replica_ports = {},
               bool discovery = false,
               const std::function<void(crpc::db::cluster_options&)>& tune = {}) {
        crpc::db::mysql::cluster_config config;
        config.connection.user = env_or("CRPC_MYSQL_USER", "crpc");
        config.connection.password = env_or("CRPC_MYSQL_PASSWORD", "crpc_pw");
        config.connection.database = env_or("CRPC_MYSQL_DB", "crpc_it");
        config.primary_host = primary_host();
        config.primary_port = primary_port();
        for (const std::uint16_t port : replica_ports) {
            config.replicas.push_back(crpc::endpoint::tcp(config.primary_host, port));
        }
        config.discover_replicas = discovery;
        config.wait_for_gtid_timeout = std::chrono::seconds(2);
        config.options.reads = reads;
        config.options.depleted = depleted;
        config.options.pool.max_size = 4;
        config.options.pool.validate_on_borrow = false;
        config.options.replica_retry_cooldown = std::chrono::milliseconds{200};
        if (tune) {
            tune(config.options);
        }
        crpc::result<std::unique_ptr<crpc::db::cluster>> built =
            crpc::db::mysql::make_cluster(config, backend);
        if (built) {
            db = std::move(built.value());
        }
    }

    [[nodiscard]] bool usable() const { return db != nullptr; }
};

} // namespace

CRPC_TEST(mysql_cluster_it_write_never_reaches_the_read_only_replica) {
    (void)require_server();
    // The replica must actually be in the cluster, otherwise "a write never
    // reaches the replica" is vacuous: with no replica configured the routing
    // layer has nowhere to send anything, and the case would pass while proving
    // nothing. `depleted = error` keeps the read path honest too: a replica that
    // is not there must be an error, not a silent fallback.
    it_cluster cluster(crpc::db::read_route::replica,
                       crpc::db::replica_depleted_policy::error,
                       {replica_port()});
    CRPC_CHECK(cluster.usable());
    if (!cluster.usable()) {
        return;
    }

    // Control: a direct write to the read-only replica fails server-side (1290).
    raw_session replica(replica_port());
    CRPC_CHECK(replica.ok());
    if (!replica.ok()) {
        std::printf("  replica %u: %s\n", static_cast<unsigned>(replica_port()),
                    replica.error().c_str());
    }
    if (replica.ok()) {
        const crpc::status direct_write = replica.get().execute("CREATE TABLE clu_ro (id INT)");
        CRPC_CHECK_FALSE(direct_write.ok());
        if (!direct_write.ok()) {
            CRPC_CHECK(direct_write.error().message.find("read-only") != std::string::npos);
        }
    }

    // The same DDL and DML through the cluster succeed: they went to the
    // primary, which is the only place that can accept writes.
    CRPC_CHECK(cluster.db->execute("DROP TABLE IF EXISTS clu_ro").ok());
    CRPC_CHECK(cluster.db->execute("CREATE TABLE clu_ro (id BIGINT PRIMARY KEY)").ok());
    CRPC_CHECK(cluster.db->execute("INSERT INTO clu_ro VALUES (1), (2)").ok());

    // Replication is real: the replica serves the rows written on the primary
    // even though it rejected direct writes. Replication is asynchronous, so a
    // plain read could land on the replica before the DDL/DML above arrive and
    // fail with "table doesn't exist" -- that is a race in the test, not a
    // routing bug. `read_your_writes` makes the cluster wait for the replica to
    // reach the primary's GTID set first, which is exactly the guarantee this
    // case is about.
    const crpc::result<crpc::db::result_set*> counted =
        cluster.db->query("SELECT COUNT(*) FROM clu_ro", {},
                          crpc::db::route_hint::read_your_writes());
    CRPC_CHECK(counted.ok());
    if (!counted.ok()) {
        std::printf("  consistent replica read: code=%d %s\n",
                    static_cast<int>(counted.error().code),
                    counted.error().message.c_str());
    }
    if (counted.ok() && counted.value() != nullptr && counted.value()->next()) {
        CRPC_CHECK_EQ(std::int64_t{2}, counted.value()->get_int64(0).value());
    }
    drop(counted);
    CRPC_CHECK(cluster.db->execute("DROP TABLE IF EXISTS clu_ro").ok());
}

CRPC_TEST(mysql_cluster_it_route_evidence_by_server_port) {
    (void)require_server();
    it_cluster replica_route(crpc::db::read_route::replica,
                             crpc::db::replica_depleted_policy::error,
                             {replica_port()});
    CRPC_CHECK(replica_route.usable());
    if (!replica_route.usable()) {
        return;
    }
    // The replica route really picked the replica: the server reports 3307.
    const crpc::result<crpc::db::result_set*> rows =
        replica_route.db->query("SELECT @@port AS p");
    CRPC_CHECK(rows.ok());
    if (rows.ok() && rows.value() != nullptr) {
        CRPC_CHECK(rows.value()->next());
        CRPC_CHECK_EQ(static_cast<std::int64_t>(replica_port()),
                      rows.value()->get_int64(0).value());
    }
    drop(rows);
}

CRPC_TEST(mysql_cluster_it_reads_stay_on_the_primary_by_default) {
    (void)require_server();
    it_cluster primary_route; // read_route::primary
    CRPC_CHECK(primary_route.usable());
    if (!primary_route.usable()) {
        return;
    }
    const crpc::result<crpc::db::result_set*> rows =
        primary_route.db->query("SELECT @@port AS p");
    CRPC_CHECK(rows.ok());
    if (rows.ok() && rows.value() != nullptr) {
        CRPC_CHECK(rows.value()->next());
        CRPC_CHECK_EQ(static_cast<std::int64_t>(primary_port()),
                      rows.value()->get_int64(0).value());
    }
    drop(rows);
}

CRPC_TEST(mysql_cluster_it_round_robin_reads_both_nodes) {
    (void)require_server();
    it_cluster mixed(crpc::db::read_route::round_robin,
                     crpc::db::replica_depleted_policy::fallback_to_primary,
                     {replica_port()});
    CRPC_CHECK(mixed.usable());
    if (!mixed.usable()) {
        return;
    }
    bool saw_primary = false;
    bool saw_replica = false;
    for (int i = 0; i < 6; ++i) {
        const crpc::result<crpc::db::result_set*> rows = mixed.db->query("SELECT @@port AS p");
        CRPC_CHECK(rows.ok());
        if (rows.ok() && rows.value() != nullptr && rows.value()->next()) {
            const std::int64_t port = rows.value()->get_int64(0).value();
            saw_primary = saw_primary || port == primary_port();
            saw_replica = saw_replica || port == replica_port();
        }
        drop(rows);
    }
    CRPC_CHECK(saw_primary);
    CRPC_CHECK(saw_replica);
}

CRPC_TEST(mysql_cluster_it_prefer_replica_falls_back_when_the_replica_is_silent) {
    (void)require_server();
    // The default threshold is 3 consecutive failures, so one silent endpoint
    // would still be `alive` afterwards. This case is about the fallback and the
    // ejection of a dead endpoint, so ask for ejection on the first failure.
    it_cluster cluster(crpc::db::read_route::prefer_replica,
                       crpc::db::replica_depleted_policy::error,
                       {dead_port()}, false,
                       [](crpc::db::cluster_options& options) {
                           options.replica_failure_threshold = 1;
                       });
    CRPC_CHECK(cluster.usable());
    if (!cluster.usable()) {
        return;
    }
    // Nothing listens on `dead_port`: the read must fall back to the primary.
    const crpc::result<crpc::db::result_set*> rows = cluster.db->query("SELECT @@port AS p");
    CRPC_CHECK(rows.ok());
    if (rows.ok() && rows.value() != nullptr) {
        CRPC_CHECK(rows.value()->next());
        CRPC_CHECK_EQ(static_cast<std::int64_t>(primary_port()),
                      rows.value()->get_int64(0).value());
    }
    drop(rows);
    const std::vector<crpc::db::replica_health> health = cluster.db->replica_health();
    CRPC_CHECK_EQ(std::size_t{1}, health.size());
    CRPC_CHECK_FALSE(health.at(0).alive);
    const crpc::result<bool> probe = cluster.db->probe_replica(0);
    CRPC_CHECK(probe.ok());
    if (probe.ok()) {
        CRPC_CHECK_FALSE(probe.value());
    }
}

CRPC_TEST(mysql_cluster_it_consistent_read_waits_for_the_write) {
    (void)require_server();
    it_cluster cluster(crpc::db::read_route::replica,
                       crpc::db::replica_depleted_policy::error,
                       {replica_port()});
    CRPC_CHECK(cluster.usable());
    if (!cluster.usable()) {
        return;
    }
    // The test controls real replication lag from the replica side; the app
    // user was granted REPLICATION_SLAVE_ADMIN for exactly this.
    raw_session replica(replica_port());
    CRPC_CHECK(replica.ok());
    if (!replica.ok()) {
        return;
    }
    CRPC_CHECK(cluster.db->execute("DROP TABLE IF EXISTS clu_gtid").ok());
    CRPC_CHECK(cluster.db->execute("CREATE TABLE clu_gtid (id BIGINT PRIMARY KEY)").ok());
    CRPC_CHECK(cluster.db->execute("INSERT INTO clu_gtid VALUES (10)").ok());
    CRPC_CHECK(cluster.db->execute("INSERT INTO clu_gtid VALUES (20)").ok());

    // The two inserts above are still travelling to the replica. Stop the
    // replication threads only after the replica has them: otherwise this case
    // measures "the replica never got the rows" instead of "the replica is
    // stale by exactly one write", and the expected count below would be
    // whatever the race produced.
    drop(cluster.db->query("SELECT COUNT(*) FROM clu_gtid", {},
                           crpc::db::route_hint::read_your_writes()));

    const crpc::status stopped = replica.get().execute("STOP REPLICA");
    CRPC_CHECK(stopped.ok());
    if (!stopped.ok()) {
        return;
    }

    // A plain (stale-tolerant) read on the replica now misses the newest write:
    // the documented replication-read semantics, demonstrated live.
    CRPC_CHECK(cluster.db->execute("INSERT INTO clu_gtid VALUES (30)").ok());
    const crpc::result<crpc::db::result_set*> stale =
        cluster.db->query("SELECT COUNT(*) FROM clu_gtid");
    CRPC_CHECK(stale.ok());
    if (stale.ok() && stale.value() != nullptr && stale.value()->next()) {
        CRPC_CHECK_EQ(std::int64_t{2}, stale.value()->get_int64(0).value());
    }
    drop(stale);

    // The consistent read asks the replica to catch up first. It cannot, so it
    // fails with the wait's own timeout instead of serving stale data.
    const crpc::result<crpc::db::result_set*> lagging =
        cluster.db->query("SELECT COUNT(*) FROM clu_gtid", {},
                          crpc::db::route_hint::read_your_writes());
    CRPC_CHECK_FALSE(lagging.ok());
    if (!lagging.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::deadline_exceeded),
                      static_cast<int>(lagging.error().code));
    }

    // Replication resumes; the same consistent read now returns the freshest
    // count.
    const crpc::status started = replica.get().execute("START REPLICA");
    CRPC_CHECK(started.ok());
    const crpc::result<crpc::db::result_set*> caught_up =
        cluster.db->query("SELECT COUNT(*) FROM clu_gtid", {},
                          crpc::db::route_hint::read_your_writes());
    CRPC_CHECK(caught_up.ok());
    if (caught_up.ok() && caught_up.value() != nullptr && caught_up.value()->next()) {
        CRPC_CHECK_EQ(std::int64_t{3}, caught_up.value()->get_int64(0).value());
    }
    drop(caught_up);

    CRPC_CHECK(cluster.db->execute("DROP TABLE IF EXISTS clu_gtid").ok());
}

CRPC_TEST(mysql_cluster_it_transaction_stays_on_one_primary_connection) {
    (void)require_server();
    it_cluster cluster(crpc::db::read_route::replica,
                       crpc::db::replica_depleted_policy::error,
                       {replica_port()});
    CRPC_CHECK(cluster.usable());
    if (!cluster.usable()) {
        return;
    }
    auto tx = cluster.db->begin_transaction();
    CRPC_CHECK(tx.ok());
    if (!tx.ok()) {
        return;
    }
    const crpc::result<crpc::db::result_set*> first = tx.value().query("SELECT CONNECTION_ID()");
    CRPC_CHECK(first.ok());
    std::int64_t conn_id = -1;
    if (first.ok() && first.value() != nullptr && first.value()->next()) {
        conn_id = first.value()->get_int64(0).value();
    }
    drop(first);
    CRPC_CHECK(conn_id > 0);

    const crpc::result<crpc::db::result_set*> port = tx.value().query("SELECT @@port AS p");
    CRPC_CHECK(port.ok());
    if (port.ok() && port.value() != nullptr && port.value()->next()) {
        CRPC_CHECK_EQ(static_cast<std::int64_t>(primary_port()),
                      port.value()->get_int64(0).value());
    }
    drop(port);

    // The SELECT stayed on the transaction's own connection.
    const crpc::result<crpc::db::result_set*> again =
        tx.value().query("SELECT CONNECTION_ID()");
    CRPC_CHECK(again.ok());
    if (again.ok() && again.value() != nullptr && again.value()->next()) {
        CRPC_CHECK_EQ(conn_id, again.value()->get_int64(0).value());
    }
    drop(again);
    CRPC_CHECK(tx.value().commit().ok());
}

CRPC_TEST(mysql_cluster_it_discovery_lists_the_replica) {
    (void)require_server();
    it_cluster cluster(crpc::db::read_route::replica,
                       crpc::db::replica_depleted_policy::fallback_to_primary,
                       {}, true);
    CRPC_CHECK(cluster.usable());
    if (!cluster.usable()) {
        return;
    }
    const std::vector<crpc::db::replica_health> health = cluster.db->replica_health();
    CRPC_CHECK(health.size() >= 1);
    bool saw_port = false;
    for (int i = 0; i < 4; ++i) {
        const crpc::result<crpc::db::result_set*> rows = cluster.db->query("SELECT @@port AS p");
        CRPC_CHECK(rows.ok());
        if (rows.ok() && rows.value() != nullptr && rows.value()->next()) {
            saw_port = saw_port || rows.value()->get_int64(0).value() == replica_port();
        }
        drop(rows);
    }
    CRPC_CHECK(saw_port);
}

CRPC_TEST_MAIN()