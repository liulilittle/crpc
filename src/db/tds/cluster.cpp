// CRPC -- TDS (SQL Server) cluster: explicit endpoints + AlwaysOn role parse.
//
// Automatic AlwaysOn discovery is intentionally not executed at build time: it
// needs WSFC and the DMVs are empty on a standalone server (see the header).
// The SQL and the parser are kept as a tested unit so a deployment with an
// availability group can wire the listener explicitly and use
// `parse_availability_roles` to validate its role layout.
#include <crpc/db/tds/cluster.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/db/cluster.hpp>
#include <crpc/db/connection.hpp>
#include <crpc/db/tds/connection.hpp>
#include <crpc/db/types.hpp>
#include <crpc/error.hpp>
#include <crpc/io.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace db {
namespace tds {

status parse_availability_roles(result_set& rows, std::vector<discovered_replica>& out) {
    int node = -1;
    int role = -1;
    const std::size_t columns = rows.column_count();
    for (std::size_t i = 0; i < columns; ++i) {
        const std::string& name = rows.column(i).name;
        if (name == "replica_server_name") {
            node = static_cast<int>(i);
        } else if (name == "role_desc") {
            role = static_cast<int>(i);
        }
    }
    if (node < 0 || role < 0) {
        return status{rpc_code::protocol_error,
                      "availability role result is missing replica_server_name or "
                      "role_desc"};
    }
    while (rows.next()) {
        if (rows.is_null(static_cast<std::size_t>(node))) {
            continue;
        }
        const auto node_text = rows.get_text(static_cast<std::size_t>(node));
        if (!node_text) {
            return node_text.error();
        }
        const auto role_text = rows.get_text(static_cast<std::size_t>(role));
        if (!role_text) {
            return role_text.error();
        }
        discovered_replica replica;
        replica.host.assign(node_text.value().data(), node_text.value().size());
        replica.role.assign(role_text.value().data(), role_text.value().size());
        // A PRIMARY row is the availability group's writer; the caller
        // configures the primary endpoint explicitly, so only SECONDARY rows
        // become replicas.
        if (replica.role == "SECONDARY" && !replica.host.empty()) {
            out.push_back(std::move(replica));
        }
    }
    return status{};
}

result<std::unique_ptr<db::cluster>> make_cluster(const cluster_config& config,
                                                  crpc::backend_factory& backend,
                                                  crpc::runtime* shard) {
    const crpc::endpoint primary_endpoint =
        crpc::endpoint::tcp(config.primary_host, config.primary_port);

    auto primary_storage =
        std::unique_ptr<db::connection_factory>(new connection_factory(
            config.connection, backend, primary_endpoint, shard));

    std::vector<std::unique_ptr<db::connection_factory>> replica_factories;
    replica_factories.reserve(config.replicas.size());
    std::vector<std::string> replica_names;
    replica_names.reserve(config.replicas.size());
    for (const crpc::endpoint& replica : config.replicas) {
        replica_factories.push_back(std::unique_ptr<db::connection_factory>(
            new connection_factory(config.connection, backend, replica, shard)));
        replica_names.push_back(replica.to_string());
    }

    db::cluster_options options = config.options;
    if (options.primary_name.empty()) {
        options.primary_name = primary_endpoint.to_string();
    }
    if (options.replica_names.empty()) {
        options.replica_names = std::move(replica_names);
    }

    // TDS has no consistency witness outside an availability group, so
    // consistent reads degrade to plain reads (documented in the header).
    return std::unique_ptr<db::cluster>(new db::cluster(
        std::move(primary_storage), std::move(replica_factories), nullptr, options));
}

} // namespace tds
} // namespace db
} // namespace crpc