// CRPC -- TiDB adapter over the MySQL wire protocol.
//
// TiDB serves the MySQL client protocol, so this adapter deliberately reuses
// `crpc::db::mysql::connection` for the transport, the handshake, prepared
// statements, result decoding and transactions, and layers on the parts TiDB
// actually differs on:
//
//   * capability detection: after the handshake the server is identified from
//     `@@version_comment` ("TiDB Server ...") / `@@version` ("...-TiDB-v"),
//     which is the same evidence the server itself advertises (measured on
//     TiDB v7.5.0: `8.0.11-TiDB-v7.5.0` / `TiDB Server (Apache License 2.0)
//     Community Edition, MySQL 8.0 compatible`).
//   * session transaction mode: `@@tidb_txn_mode` (optimistic/pessimistic).
//     The default is pessimistic since TiDB 5.0, so callers must read the
//     actual session value instead of assuming a mode. Newer defaults are
//     reported by the server; `is_tidb()` is false and the mode is `unknown`
//     when the server is a plain MySQL or MariaDB.
//   * error semantics: the MySQL layer maps TiDB 9007 (optimistic write
//     conflict) and 8027 (schema changed) to a retryable `rpc_code` and stamps
//     the raw SQL code into the error metadata, so a caller can tell them from
//     the MySQL codes (e.g. 1205 lock wait timeout). See
//     `mysql::map_error_code` and `crpc::db::tidb::is_transaction_conflict`.
#ifndef CRPC_DB_TIDB_CONNECTION_HPP
#define CRPC_DB_TIDB_CONNECTION_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <crpc/config.hpp>
#include <crpc/db/connection.hpp>
#include <crpc/db/mysql/channel.hpp>
#include <crpc/db/mysql/connection.hpp>
#include <crpc/error.hpp>
#include <crpc/io.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace db {
namespace tidb {

// The session transaction mode, as reported by `@@tidb_txn_mode` at connect
// time. `unknown` means the server is not TiDB (the variable does not exist on
// MySQL) or it could not be read during capability detection.
enum class txn_mode : std::uint8_t {
    unknown = 0,
    optimistic = 1,
    pessimistic = 2,
};

[[nodiscard]] inline const char* to_string(txn_mode mode) noexcept {
    switch (mode) {
    case txn_mode::optimistic:
        return "optimistic";
    case txn_mode::pessimistic:
        return "pessimistic";
    default:
        return "unknown";
    }
}

// A TiDB session. Everything about the wire behaviour lives in the MySQL
// implementation this derives from; the TiDB layer only adds the capability
// surface (`is_tidb()`, `txn_mode()`) that the MySQL protocol cannot express.
class CRPC_API connection final : public crpc::db::mysql::connection {
public:
    connection(crpc::db::mysql::connection_options options,
               std::unique_ptr<crpc::db::mysql::channel> link);
    ~connection() override;

    // True when the connected server identified itself as TiDB through
    // `@@version_comment` (contains "TiDB Server") or `@@version` (contains
    // "-TiDB-"). A MySQL/MariaDB server reports false.
    [[nodiscard]] bool is_tidb() const noexcept { return is_tidb_; }

    // The session transaction mode at connect time. Only meaningful when
    // `is_tidb()` is true; otherwise `unknown`.
    [[nodiscard]] crpc::db::tidb::txn_mode txn_mode() const noexcept { return txn_mode_; }

private:
    // Runs the capability queries after the handshake. Failures are swallowed:
    // a confusing server must not turn every open into an error, it just
    // reports not-tidb/unknown.
    void detect_capabilities() noexcept;

    bool is_tidb_ = false;
    // Fully qualified: the member function `txn_mode()` hides the enum name in
    // class scope, so an unqualified `txn_mode` would not name the type here.
    crpc::db::tidb::txn_mode txn_mode_ = crpc::db::tidb::txn_mode::unknown;
};

// Opens TiDB connections over a caller-supplied channel. The injectable form
// of the endpoint factory: tests and custom transports supply the channel.
class CRPC_API channel_factory final : public db::connection_factory {
public:
    using opener = std::function<result<std::unique_ptr<crpc::db::mysql::channel>>()>;

    channel_factory(crpc::db::mysql::connection_options options, opener open);
    ~channel_factory() override;

    [[nodiscard]] result<db::connection*> open() override;
    [[nodiscard]] std::string_view name() const noexcept override;

private:
    crpc::db::mysql::connection_options options_;
    opener open_;
};

// Opens one connection to one endpoint. A pool owns the result. The factory
// borrows the backend and the shard, exactly like its MySQL counterpart.
class CRPC_API connection_factory final : public db::connection_factory {
public:
    connection_factory(crpc::db::mysql::connection_options options,
                       crpc::backend_factory& backend, crpc::endpoint endpoint,
                       crpc::runtime* shard = nullptr);
    ~connection_factory() override;

    [[nodiscard]] result<db::connection*> open() override;
    [[nodiscard]] std::string_view name() const noexcept override;

private:
    crpc::db::mysql::connection_options options_;
    crpc::backend_factory* backend_ = nullptr;
    crpc::endpoint endpoint_;
    crpc::runtime* shard_ = nullptr;
};

} // namespace tidb
} // namespace db
} // namespace crpc

#endif // CRPC_DB_TIDB_CONNECTION_HPP