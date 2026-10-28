// CRPC -- TDS (SQL Server) primary/replica cluster: endpoint roles and
// AlwaysOn-aware discovery.
//
// The generic `crpc::db::cluster` does the routing and health work; this header
// adds the SQL Server shape: a `primary` endpoint, an optional list of replica
// endpoints, and a best-effort discovery of availability-group roles through
// `sys.dm_hadr_availability_replica_states`.
//
// What is and is not validated: multi-endpoint routing, role annotation and the
// replica health machine are exercised against the real SQL Server in the
// integration suite (a primary-only cluster, plus a dead second endpoint for
// the health side). *Automatic AlwaysOn discovery is not validated against a
// real availability group*: it needs Windows Server Failover Clustering, which
// this test environment does not provide. The parser is unit-tested against
// canned rows from `sys.dm_hadr_availability_replica_states`, and the SQL is
// documented below; a deployment that actually runs AlwaysOn should wire the
// availability-group listener explicitly and treat discovery as a validation
// aid, because the DMVs report the WSFC node names, which are not always the
// endpoints a SQL client can connect to.
#ifndef CRPC_DB_TDS_CLUSTER_HPP
#define CRPC_DB_TDS_CLUSTER_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/db/cluster.hpp>
#include <crpc/db/connection.hpp>
#include <crpc/db/tds/connection.hpp>
#include <crpc/error.hpp>
#include <crpc/io.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace db {
namespace tds {

// Configuration for one logical SQL Server database with a primary and replicas.
struct cluster_config {
    // Credentials, database, encryption, packet size... shared by every
    // endpoint in the cluster.
    connection_options connection{};

    std::string primary_host = "127.0.0.1";
    std::uint16_t primary_port = 1433;

    // Explicit replica endpoints.
    std::vector<crpc::endpoint> replicas;

    // Routing, health and pool tunables for the underlying `db::cluster`.
    db::cluster_options options{};
};

// One row of the availability-group role query.
struct CRPC_API discovered_replica {
    // The WSFC node name (`replica_server_name`), used as the connect host.
    std::string host;
    // PRIMARY or SECONDARY, as reported by `role_desc`.
    std::string role;
};

// Parses the result of the availability-group role query into `out`. A
// SECONDARY role becomes a replica endpoint; a PRIMARY row in the replica list
// is skipped (the primary endpoint is configured explicitly). Rows are matched
// by column name, so the caller may select any projection that carries
// `replica_server_name` and `role_desc`.
[[nodiscard]] CRPC_API status parse_availability_roles(result_set& rows,
                                                       std::vector<discovered_replica>& out);

// The SQL behind `parse_availability_roles`, exposed so a caller can run the
// same probe. It is intentionally a diagnosis rather than a listener lookup:
// without a WSFC the DMVs are empty, and with one the node names are what the
// prospective client must reach.
inline constexpr std::string_view availability_role_query =
    "SELECT ar.replica_server_name, ars.role_desc "
    "FROM sys.dm_hadr_availability_replica_states ars "
    "JOIN sys.availability_replicas ar ON ar.replica_id = ars.replica_id";

// Builds a `db::cluster` over the configured SQL Server topology. The primary
// and every replica share `connection` and `options.pool`. TDS intentionally
// has no consistency witness: outside an availability group there is no
// synchronous read-your-writes guarantee to wait on, so consistent reads behave
// as plain reads (see `db::cluster`).              
[[nodiscard]] CRPC_API result<std::unique_ptr<db::cluster>> make_cluster(
    const cluster_config& config, crpc::backend_factory& backend,
    crpc::runtime* shard = nullptr);

} // namespace tds
} // namespace db
} // namespace crpc

#endif // CRPC_DB_TDS_CLUSTER_HPP