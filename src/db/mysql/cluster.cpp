// CRPC -- MySQL cluster: endpoint roles, SHOW REPLICAS discovery, GTID guard.
#include <crpc/db/mysql/cluster.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/db/cluster.hpp>
#include <crpc/db/connection.hpp>
#include <crpc/db/mysql/connection.hpp>
#include <crpc/db/types.hpp>
#include <crpc/error.hpp>
#include <crpc/io.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace db {
namespace mysql {

gtid_replica_consistency::gtid_replica_consistency(std::chrono::milliseconds wait_timeout)
    : wait_timeout_(wait_timeout) {}

gtid_replica_consistency::~gtid_replica_consistency() = default;

status gtid_replica_consistency::capture(crpc::db::connection& primary, std::string& token) {
    result_set* rows = nullptr;
    const status ran = primary.query("SELECT @@GLOBAL.gtid_executed", {}, &rows);
    statement_guard guard(rows);
    if (!ran) {
        return ran;
    }
    if (rows == nullptr || !rows->next() || rows->is_null(0)) {
        // A missing GTID set is an empty token: consistency protection simply
        // cannot promise anything on this server, and the caller's read
        // proceeds as a plain (stale-tolerant) read.
        token.clear();
        return status{};
    }
    const auto value = rows->get_text(0);
    if (!value) {
        return value.error();
    }
    token.assign(value.value().data(), value.value().size());
    return status{};
}

status gtid_replica_consistency::wait(crpc::db::connection& replica, const std::string& token) {
    // WAIT_FOR_EXECUTED_GTID_SET(gtid_set[, timeout]) takes the timeout in
    // seconds and returns 0 once the set is applied, non-zero on failure and
    // NULL on timeout.
    const double timeout_seconds =
        static_cast<double>(wait_timeout_.count()) / 1000.0;
    std::vector<parameter> params;
    params.push_back(parameter::from_text(token));
    params.push_back(parameter::from_double(timeout_seconds));
    result_set* rows = nullptr;
    const status ran = replica.query(
        "SELECT WAIT_FOR_EXECUTED_GTID_SET(?, ?)",
        span<const parameter>(params.data(), params.size()), &rows);
    statement_guard guard(rows);
    if (!ran) {
        return ran;
    }
    if (rows == nullptr || !rows->next()) {
        return status{rpc_code::protocol_error,
                      "WAIT_FOR_EXECUTED_GTID_SET returned no row"};
    }
    if (rows->is_null(0)) {
        return status{rpc_code::deadline_exceeded,
                      "replica lagged behind the captured GTID set beyond the wait "
                      "timeout"};
    }
    const auto waited = rows->get_int64(0);
    if (!waited) {
        return waited.error();
    }
    if (waited.value() != 0) {
        return status{rpc_code::deadline_exceeded,
                      "replica could not reach the captured GTID set within the wait "
                      "timeout"};
    }
    return status{};
}

status parse_show_replicas(result_set& rows, std::vector<discovered_replica>& out) {
    int host = -1;
    int port = -1;
    int server_id = -1;
    const std::size_t columns = rows.column_count();
    for (std::size_t i = 0; i < columns; ++i) {
        const std::string& name = rows.column(i).name;
        if (name == "Host") {
            host = static_cast<int>(i);
        } else if (name == "Port") {
            port = static_cast<int>(i);
        } else if (name == "Server_id") {
            server_id = static_cast<int>(i);
        }
    }
    if (host < 0) {
        return status{rpc_code::protocol_error,
                      "SHOW REPLICAS result has no Host column"};
    }
    while (rows.next()) {
        const auto host_text = rows.get_text(static_cast<std::size_t>(host));
        if (!host_text) {
            return host_text.error();
        }
        discovered_replica replica;
        replica.host.assign(host_text.value().data(), host_text.value().size());
        if (port >= 0 && !rows.is_null(static_cast<std::size_t>(port))) {
            const auto port_text = rows.get_int64(static_cast<std::size_t>(port));
            if (!port_text) {
                return port_text.error();
            }
            replica.port = static_cast<std::uint16_t>(port_text.value());
        }
        if (server_id >= 0 && !rows.is_null(static_cast<std::size_t>(server_id))) {
            const auto id = rows.get_int64(static_cast<std::size_t>(server_id));
            if (!id) {
                return id.error();
            }
            replica.server_id = static_cast<std::uint32_t>(id.value());
        }
        // A row without a usable port is dropped: an endpoint with no port
        // cannot be routed to. A valid configured replica list drains the harm.
        if (replica.host.empty() || replica.port == 0) {
            continue;
        }
        out.push_back(std::move(replica));
    }
    return status{};
}

namespace {

[[nodiscard]] std::string endpoint_label(const crpc::endpoint& ep) {
    return ep.to_string();
}

// Runs `SHOW REPLICAS` (8.0.22+) and falls back to `SHOW SLAVE HOSTS` for
// older servers, returning the discovered replica endpoints.
[[nodiscard]] status discover_replicas(crpc::db::connection& session,
                                       std::vector<crpc::endpoint>& out) {
    result_set* rows = nullptr;
    status ran = session.query("SHOW REPLICAS", {}, &rows);
    if (!ran) {
        // Unknown statement names differ between versions; the return value is a
        // protocol-level error, which is what SHOW SLAVE HOSTS was invented to
        // answer.
        ran = session.query("SHOW SLAVE HOSTS", {}, &rows);
    }
    statement_guard guard(rows);
    if (!ran) {
        return rpc_error{
            ran.error().code,
            "could not list replicas (the connecting user needs the REPLICATION "
            "CLIENT privilege): " +
                ran.error().message};
    }
    if (rows == nullptr) {
        return status{rpc_code::protocol_error, "SHOW REPLICAS returned no result set"};
    }
    std::vector<discovered_replica> parsed;
    const status ok = parse_show_replicas(*rows, parsed);
    if (!ok) {
        return ok;
    }
    out.clear();
    out.reserve(parsed.size());
    for (discovered_replica& replica : parsed) {
        out.push_back(crpc::endpoint::tcp(std::move(replica.host), replica.port));
    }
    return status{};
}

} // namespace

result<std::unique_ptr<db::cluster>> make_cluster(const cluster_config& config,
                                                  crpc::backend_factory& backend,
                                                  crpc::runtime* shard) {
    const crpc::endpoint primary_endpoint =
        crpc::endpoint::tcp(config.primary_host, config.primary_port);

    // Owned factories outlive the pools that reference them.
    auto primary_storage =
        std::unique_ptr<db::connection_factory>(new connection_factory(
            config.connection, backend, primary_endpoint, shard));

    std::vector<crpc::endpoint> replica_endpoints = config.replicas;
    if (config.discover_replicas) {
        auto probe_factory = std::unique_ptr<db::connection_factory>(
            new connection_factory(config.connection, backend, primary_endpoint, shard));
        result<db::connection*> opened = probe_factory->open();
        if (!opened) {
            return rpc_error{opened.error().code,
                             "cannot discover replicas: opening the primary failed: " +
                                 opened.error().message};
        }
        std::unique_ptr<db::connection> session(opened.value());
        const status discovered = discover_replicas(*session, replica_endpoints);
        if (!discovered) {
            return discovered.error();
        }
    }

    std::vector<std::unique_ptr<db::connection_factory>> replica_factories;
    replica_factories.reserve(replica_endpoints.size());
    std::vector<std::string> replica_names;
    replica_names.reserve(replica_endpoints.size());
    for (const crpc::endpoint& replica : replica_endpoints) {
        replica_factories.push_back(std::unique_ptr<db::connection_factory>(
            new connection_factory(config.connection, backend, replica, shard)));
        replica_names.push_back(endpoint_label(replica));
    }

    db::cluster_options options = config.options;
    if (options.primary_name.empty()) {
        options.primary_name = endpoint_label(primary_endpoint);
    }
    if (options.replica_names.empty()) {
        options.replica_names = std::move(replica_names);
    }

    std::shared_ptr<db::replica_consistency> consistency;
    if (config.stale_read_protection) {
        consistency = std::make_shared<gtid_replica_consistency>(config.wait_for_gtid_timeout);
    }

    return std::unique_ptr<db::cluster>(new db::cluster(
        std::move(primary_storage), std::move(replica_factories), std::move(consistency),
        options));
}

} // namespace mysql
} // namespace db
} // namespace crpc