// CRPC -- TDS (SQL Server) cluster: endpoint roles and routing against the
// real server.
//
// Requires CRPC_TDS_IT=1. The environment has exactly one SQL Server, so this
// suite proves primary-only routing, the zero-replica and depleted edge cases,
// and the health machine against a dead second endpoint. AlwaysOn automatic
// discovery is *not* exercised here (it needs a WSFC, which the environment
// does not have); the parser is unit-tested in test_db_tds_cluster.cpp and the
// unvalidated part is stated in include/crpc/db/tds/cluster.hpp.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/db/channel.hpp>
#include <crpc/db/cluster.hpp>
#include <crpc/db/connection.hpp>
#include <crpc/db/tds/cluster.hpp>
#include <crpc/db/tds/connection.hpp>
#include <crpc/db/types.hpp>
#include <crpc/error.hpp>
#include <crpc/io.hpp>
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

struct it_cluster {
    crpc::iocp_backend backend;
    std::unique_ptr<crpc::db::cluster> db;

    it_cluster(crpc::db::read_route reads = crpc::db::read_route::primary,
               crpc::db::replica_depleted_policy depleted =
                   crpc::db::replica_depleted_policy::fallback_to_primary,
               std::vector<std::uint16_t> replica_ports = {}) {
        crpc::db::tds::cluster_config config;
        config.connection.user = env_or("CRPC_TDS_USER", "sa");
        config.connection.password = env_or("CRPC_TDS_PASSWORD", "Crpc#It2026");
        config.connection.database = env_or("CRPC_TDS_DB", "master");
        config.connection.verify_server_certificate = false;
        const std::string host = env_or("CRPC_TDS_HOST", "127.0.0.1");
        config.primary_host = host;
        config.primary_port = static_cast<std::uint16_t>(
            std::stoi(env_or("CRPC_TDS_PORT", "1433")));
        if (config.connection.server_name.empty()) {
            config.connection.server_name = host;
        }
        for (const std::uint16_t port : replica_ports) {
            config.replicas.push_back(crpc::endpoint::tcp(host, port));
        }
        config.options.reads = reads;
        config.options.depleted = depleted;
        config.options.pool.max_size = 4;
        config.options.pool.validate_on_borrow = false;
        config.options.replica_retry_cooldown = std::chrono::milliseconds{200};
        crpc::result<std::unique_ptr<crpc::db::cluster>> built =
            crpc::db::tds::make_cluster(config, backend);
        if (built) {
            db = std::move(built.value());
        }
    }

    [[nodiscard]] bool usable() const { return db != nullptr; }
};

void drop(crpc::result<crpc::db::result_set*> rows) {
    if (rows.ok()) {
        delete rows.value();
    }
}

} // namespace

CRPC_TEST(tds_cluster_it_primary_only_routing) {
    (void)require_server();
    it_cluster cluster; // read_route::primary
    CRPC_CHECK(cluster.usable());
    if (!cluster.usable()) {
        return;
    }
    CRPC_CHECK(cluster.db->execute("SELECT 1").ok());
    CRPC_CHECK_EQ(std::size_t{0}, cluster.db->replica_count());

    // A single SELECT through the cluster works; with no replicas the 'replica'
    // route mode is refused and every other mode serves from the primary.
    drop(cluster.db->query("SELECT 1 AS n"));
    const crpc::result<crpc::db::result_set*> refused =
        cluster.db->query("SELECT 1 AS n", {}, crpc::db::route_hint::replica());
    CRPC_CHECK_FALSE(refused.ok());
    if (!refused.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                      static_cast<int>(refused.error().code));
    }
}

CRPC_TEST(tds_cluster_it_ddl_dml_and_reads) {
    (void)require_server();
    it_cluster cluster;
    CRPC_CHECK(cluster.usable());
    if (!cluster.usable()) {
        return;
    }
    CRPC_CHECK(cluster.db->execute("SELECT 1", {}).ok());
    CRPC_CHECK(cluster.db->execute("IF DB_ID('crpc_clu_tds') IS NULL CREATE DATABASE crpc_clu_tds").ok());
    CRPC_CHECK(cluster.db->execute("IF OBJECT_ID('crpc_clu_rows') IS NOT NULL DROP TABLE crpc_clu_rows").ok());
    CRPC_CHECK(cluster.db->execute("CREATE TABLE crpc_clu_rows (id int NOT NULL PRIMARY KEY)").ok());
    CRPC_CHECK(cluster.db->execute("INSERT INTO crpc_clu_rows VALUES (1), (2)").ok());
    const crpc::result<crpc::db::result_set*> counted =
        cluster.db->query("SELECT COUNT(*) FROM crpc_clu_rows");
    CRPC_CHECK(counted.ok());
    if (counted.ok() && counted.value() != nullptr) {
        CRPC_CHECK(counted.value()->next());
        CRPC_CHECK_EQ(std::int64_t{2}, counted.value()->get_int64(0).value());
    }
    drop(counted);
    CRPC_CHECK(cluster.db->execute("DROP TABLE crpc_clu_rows").ok());
}

CRPC_TEST(tds_cluster_it_dead_secondary_is_ejected_and_reads_fall_back) {
    (void)require_server();
    // A second endpoint nothing listens on (default 1499): the availability
    // group would put a readable secondary here, but the health machine must
    // treat an unreachable one the same way.
    it_cluster cluster(crpc::db::read_route::prefer_replica,
                       crpc::db::replica_depleted_policy::fallback_to_primary,
                       {static_cast<std::uint16_t>(std::stoi(env_or("CRPC_TDS_DEAD_PORT", "1499")))});
    CRPC_CHECK(cluster.usable());
    if (!cluster.usable()) {
        return;
    }
    const crpc::result<crpc::db::result_set*> rows = cluster.db->query("SELECT 42 AS n");
    CRPC_CHECK(rows.ok());
    if (rows.ok() && rows.value() != nullptr) {
        CRPC_CHECK(rows.value()->next());
        CRPC_CHECK_EQ(std::int64_t{42}, rows.value()->get_int64(0).value());
    }
    drop(rows);
    const std::vector<crpc::db::replica_health> health = cluster.db->replica_health();
    CRPC_CHECK_EQ(std::size_t{1}, health.size());
    CRPC_CHECK_FALSE(health.at(0).alive);

    // With the depleted policy set to 'error', the same read fails explicitly.
    it_cluster strict(crpc::db::read_route::replica,
                      crpc::db::replica_depleted_policy::error,
                      {static_cast<std::uint16_t>(std::stoi(env_or("CRPC_TDS_DEAD_PORT", "1499")))});
    CRPC_CHECK(strict.usable());
    if (!strict.usable()) {
        return;
    }
    const crpc::result<crpc::db::result_set*> refused = strict.db->query("SELECT 1 AS n");
    CRPC_CHECK(!refused.ok());
}

CRPC_TEST_MAIN()