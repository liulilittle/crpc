// CRPC -- TiDB connection implementation. TiDB serves the MySQL wire
// protocol, so the session comes from `crpc::db::mysql::connection`; this file
// only adds the capability detection (is_tidb, session transaction mode) and
// the two factories.
#include <crpc/db/tidb/connection.hpp>

#include <algorithm>
#include <cctype>
#include <memory>
#include <utility>

namespace crpc {
namespace db {
namespace tidb {
namespace {

// Reads the single value `sql` returns, as text. Detection must never fail the
// connection: on an error, or a missing/shape-mismatched result, `out` is
// left untouched and false is returned.
bool read_scalar(crpc::db::connection& connection, const char* sql, std::string& out) {
    crpc::db::result_set* rows = nullptr;
    const crpc::status ran = connection.query(sql, {}, &rows);
    if (!ran) {
        return false;
    }
    std::unique_ptr<crpc::db::result_set> guard(rows);
    if (guard == nullptr || !guard->next()) {
        return false;
    }
    const auto text = guard->get_text(0);
    if (!text) {
        return false;
    }
    out.assign(text.value().data(), text.value().size());
    return true;
}

bool contains_ci(std::string_view haystack, std::string_view needle) noexcept {
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                       [](unsigned char lhs, unsigned char rhs) {
                           return std::tolower(lhs) == std::tolower(rhs);
                       }) != haystack.end();
}

} // namespace

connection::connection(crpc::db::mysql::connection_options options,
                       std::unique_ptr<crpc::db::mysql::channel> link)
    : crpc::db::mysql::connection(std::move(options), std::move(link)) {
    detect_capabilities();
}

connection::~connection() = default;

void connection::detect_capabilities() noexcept {
    if (!is_open()) {
        return;
    }
    // `@@version_comment` is the primary evidence (v7.5.0:
    // "TiDB Server (Apache License 2.0) Community Edition, MySQL 8.0
    // compatible"); `@@version` is the backstop ("8.0.11-TiDB-v7.5.0").
    std::string comment;
    std::string version;
    const bool have_comment = read_scalar(*this, "SELECT @@version_comment", comment);
    const bool have_version = read_scalar(*this, "SELECT @@version", version);
    if ((have_comment && contains_ci(comment, "TiDB")) ||
        (have_version && contains_ci(version, "-TiDB-"))) {
        is_tidb_ = true;
    }
    if (!is_tidb_) {
        return;
    }
    // MySQL does not know this variable, so it is only queried once the server
    // identified itself as TiDB. The mode is a per-session setting; the value
    // cached here is the one observed at connect time.
    std::string mode;
    if (read_scalar(*this, "SELECT @@tidb_txn_mode", mode)) {
        if (contains_ci(mode, "optimistic")) {
            txn_mode_ = txn_mode::optimistic;
        } else if (contains_ci(mode, "pessimistic")) {
            txn_mode_ = txn_mode::pessimistic;
        }
    }
}

channel_factory::channel_factory(crpc::db::mysql::connection_options options, opener open)
    : options_(std::move(options)), open_(std::move(open)) {}

channel_factory::~channel_factory() = default;

result<db::connection*> channel_factory::open() {
    if (!open_) {
        return rpc_error{rpc_code::failed_precondition, "the channel factory has no opener"};
    }
    result<std::unique_ptr<crpc::db::mysql::channel>> opened = open_();
    if (!opened) {
        return opened.error();
    }
    auto session = std::unique_ptr<connection>(
        new connection(options_, std::move(opened.value())));
    if (!session->is_open()) {
        rpc_error error = session->connect_error();
        if (error.code == rpc_code::ok) {
            error = rpc_error{rpc_code::unavailable, "tidb handshake failed"};
        }
        return error;
    }
    return static_cast<db::connection*>(session.release());
}

std::string_view channel_factory::name() const noexcept { return "tidb-injected"; }

connection_factory::connection_factory(crpc::db::mysql::connection_options options,
                                       crpc::backend_factory& backend,
                                       crpc::endpoint endpoint, crpc::runtime* shard)
    : options_(std::move(options)),
      backend_(&backend),
      endpoint_(std::move(endpoint)),
      shard_(shard) {}

connection_factory::~connection_factory() = default;

result<db::connection*> connection_factory::open() {
    crpc::net::connection_config config;
    config.ep = endpoint_;
    // The handshake and the capability probes are round trips; the transport's
    // operation timeout bounds them on a plain thread, while a coroutine parks
    // until the server answers or the link dies.
    config.operation_timeout = std::chrono::seconds(15);

    auto session = std::unique_ptr<connection>(new connection(
        options_, std::unique_ptr<crpc::db::mysql::channel>(
                      new crpc::db::mysql::socket_channel(std::move(config), *backend_,
                                                          shard_))));
    if (!session->is_open()) {
        rpc_error error = session->connect_error();
        if (error.code == rpc_code::ok) {
            error = rpc_error{rpc_code::unavailable, "tidb handshake failed"};
        }
        return error;
    }
    return static_cast<db::connection*>(session.release());
}

std::string_view connection_factory::name() const noexcept { return "tidb"; }

} // namespace tidb
} // namespace db
} // namespace crpc