// CRPC -- MySQL primary/replica cluster: endpoint roles, auto-discovery,
// GTID consistency.
//
// `crpc::db::cluster` is backend-agnostic; this header is the part of it that
// is MySQL-shaped:
//
//   * endpoint roles -- the primary endpoint plus the list of replica
//     endpoints, supplied explicitly or discovered from the primary with
//     `SHOW REPLICAS` (8.0.22+; `SHOW SLAVE HOSTS` on older servers);
//   * a consistency witness built on `WAIT_FOR_EXECUTED_GTID_SET`, which makes
//     `route_hint::consistent` reads actually wait for the primary's writes.
//     It requires a GTID-enabled (`gtid_mode=ON`) topology; on a non-GTID
//     topology `capture` yields an empty token and consistent reads degrade to
//     plain reads without failing.
//
// Discovery (`discover_replicas`): at build time the primary is queried with
// `SHOW REPLICAS`, which lists every connected replica's reported host and
// port. The connecting user needs the `REPLICATION CLIENT` privilege for the
// query and network reachability to the reported host:port (the replica's own
// `report_host`/`report_port`). A replica the client cannot reach is handled by
// the normal health machine -- ejection, not a build failure.
#ifndef CRPC_DB_MYSQL_CLUSTER_HPP
#define CRPC_DB_MYSQL_CLUSTER_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/db/cluster.hpp>
#include <crpc/db/connection.hpp>
#include <crpc/db/mysql/connection.hpp>
#include <crpc/error.hpp>
#include <crpc/io.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace db {
namespace mysql {

// Configuration for one logical MySQL database with a primary and replicas.
struct cluster_config {
    // Credentials, database, charset, materialisation bounds... shared by every
    // endpoint in the cluster.
    connection_options connection{};

    std::string primary_host = "127.0.0.1";
    std::uint16_t primary_port = 3306;

    // Explicit replica endpoints. Ignored (overwritten) when `discover_replicas`
    // is enabled.
    std::vector<crpc::endpoint> replicas;

    // Enumerate the replicas with `SHOW REPLICAS` against the primary at build
    // time instead of using `replicas`. The connection user needs
    // `REPLICATION CLIENT`; a failure to run the query fails the build with a
    // message that says so.
    bool discover_replicas = false;

    // Install the GTID-based consistency witness so `route_hint::consistent`
    // reads wait for the primary's writes before reading a replica. Requires a
    // GTID-enabled topology to be effective.
    bool stale_read_protection = true;

    // How long `WAIT_FOR_EXECUTED_GTID_SET` may block before the consistent
    // read fails.
    std::chrono::milliseconds wait_for_gtid_timeout{std::chrono::seconds(10)};

    // Routing, health and pool tunables for the underlying `db::cluster`.
    db::cluster_options options{};
};

// One row of `SHOW REPLICAS` / `SHOW SLAVE HOSTS`.
struct CRPC_API discovered_replica {
    std::string host;
    std::uint16_t port = 0;
    std::uint32_t server_id = 0;
};

// Parses a `SHOW REPLICAS` / `SHOW SLAVE HOSTS` result set into `out`. The two
// statement shapes share the same columns (Server_id, Host, Port, Source_id,
// Replica_UUID); rows are matched by column name so both parse identically. A
// row whose port is not reported is dropped, because an endpoint with no port
// is unusable. A missing Host column is a protocol error rather than a silently
// partial result.
[[nodiscard]] CRPC_API status parse_show_replicas(result_set& rows,
                                                  std::vector<discovered_replica>& out);

// The GTID consistency witness. `capture` reads `@@GLOBAL.gtid_executed` from
// the primary; `wait` runs `SELECT WAIT_FOR_EXECUTED_GTID_SET(?, <timeout>)`
// on the replica and fails with an error naming the timeout when the replica
// cannot catch up.
class CRPC_API gtid_replica_consistency final : public db::replica_consistency {
public:
    explicit gtid_replica_consistency(std::chrono::milliseconds wait_timeout);
    ~gtid_replica_consistency() override;

    [[nodiscard]] status capture(crpc::db::connection& primary, std::string& token) override;
    [[nodiscard]] status wait(crpc::db::connection& replica, const std::string& token) override;

private:
    std::chrono::milliseconds wait_timeout_;
};

// Builds a `db::cluster` over the configured MySQL topology. The primary and
// every replica share `connection` (user, password, database) and `options.pool`.
// `stale_read_protection` installs the GTID witness.
[[nodiscard]] CRPC_API result<std::unique_ptr<db::cluster>> make_cluster(
    const cluster_config& config, crpc::backend_factory& backend,
    crpc::runtime* shard = nullptr);

} // namespace mysql
} // namespace db
} // namespace crpc

#endif // CRPC_DB_MYSQL_CLUSTER_HPP