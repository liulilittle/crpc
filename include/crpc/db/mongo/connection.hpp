// CRPC -- MongoDB connection: OP_MSG round trips, SCRAM authentication, cursors.
//
// The connection owns one TCP link and speaks only OP_MSG. Two things about the protocol
// shape the design:
//
//   * every request carries a request id and every reply echoes it in `responseTo`, so a
//     reply that does not match the request in flight is a protocol error rather than
//     something to accept because it arrived;
//   * a command's reply can say `moreToCome`, meaning the server keeps sending without
//     being asked. A client that waits for "one reply per request" then hangs, so the
//     flag is read and the extra replies are consumed.
#ifndef CRPC_DB_MONGO_CONNECTION_HPP
#define CRPC_DB_MONGO_CONNECTION_HPP

#include <cstddef>
#include <functional>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/db/channel.hpp>
#include <crpc/db/connection.hpp>
#include <crpc/db/mongo/auth.hpp>
#include <crpc/db/mongo/bson.hpp>
#include <crpc/db/mongo/protocol.hpp>
#include <crpc/error.hpp>
#include <crpc/io.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace db {
namespace mongo {

// Re-opens the transport after a transport failure. Set it and a retryable write can
// be replayed once with the same lsid and txnNumber, which is what the server uses to
// deduplicate.
using reconnector = std::function<result<std::unique_ptr<db::channel>>()>;

struct connection_options {
    std::string user;
    std::string password;
    // Authenticated against this database; the users live in `admin` unless created in a
    // specific database. `$db` on the saslStart command is the authentication source.
    std::string auth_database = "admin";
    // The database commands run against.
    std::string database = "test";
    std::string application_name = "crpc";

    auth_mechanism mechanism = auth_mechanism::scram_sha_256;

    // Bound on one reply, before it is refused. A reply larger than this fails with
    // `resource_exhausted` instead of growing until the process dies.
    std::size_t max_message_size = 48u * 1024u * 1024u;

    // Distributed semantics. Each is a command-wide default; the wire field appears
    // only when the settings say the server must be told.
    //
    // Reads only: how the driver may prefer a node. `primary` (the default) is the
    // server's own default and sends no `$readPreference`. The field is sent as a
    // `$readPreference` top-level command field, positioned like `$db`.
    read_preference_mode read_preference = read_preference_mode::primary;
    // Reads only: how fresh the snapshot must be. `local` (the default) sends nothing.
    // The wire field is `readConcern` -- no dollar sign; `$readConcern` is refused.
    read_concern_level read_concern = read_concern_level::local;
    // Writes only: how many nodes must acknowledge. Nothing is sent when unset.
    write_concern_options write_concern;
    // Send `lsid` plus `txnNumber` on writes so a transport failure may be retried
    // once without duplicating the document. A standalone refuses `txnNumber`
    // outright, so it is only sent when `hello` reported a replica set.
    bool retryable_writes = true;
    // Whichever link a retryable write replays on, set before the connection opens.
    reconnector reconnect;
};

// A decoded reply to one command.
struct command_result {
    bson_document reply;
    bool ok = false;
    std::string error_message;
    // The reply carried a `writeConcernError`: the write ran but the durability
    // requirement was not met. This is separate from `ok`, which stays true.
    bool has_write_concern_error = false;
    std::int32_t write_concern_code = 0;
    std::string write_concern_message;
    // A retryable write was sent twice (same lsid and txnNumber) after a transport
    // failure; the server deduplicated the second, and the caller may want to know.
    bool retried = false;
};

// One open cursor. `id` of 0 means the server sent everything in the first batch.
struct cursor {
    std::int64_t id = 0;
    std::string collection;
    std::vector<bson_document> batch;
    // Index of the next document to hand out from `batch`.
    std::size_t position = 0;
};

class CRPC_API connection final : public db::connection {
public:
    // Connects, runs `hello` and authenticates. On failure the link is closed and
    // `connect_error()` carries the server's own message.
    connection(connection_options options, std::unique_ptr<db::channel> link);
    ~connection() override;

    // Runs one command and returns its reply. A command that fails is reported through
    // `command_result`, because a MongoDB "error" is a normal reply with `ok: 0`.
    [[nodiscard]] result<command_result> run_command(const bson_document& command);

    // `find` and the cursor that may follow it.
    [[nodiscard]] result<cursor> find(const find_request& request);
    // `aggregate` and the cursor over its output. The reply carries the same
    // {cursor: {id, ns, firstBatch}} document as `find`, so `next_batch` and
    // `close_cursor` continue and release it through the one cursor path.
    [[nodiscard]] result<cursor> aggregate(const aggregate_request& request);
    // Fetches the next batch for `state`. Returns false when the cursor is exhausted, and
    // releases it on the server in that case.
    [[nodiscard]] result<bool> next_batch(cursor& state);
    // Releases a cursor the caller will not read to the end. Idempotent.
    [[nodiscard]] status close_cursor(cursor& state);

    // `insert`, reporting how many documents the server accepted.
    [[nodiscard]] result<std::int64_t> insert(std::string_view collection,
                                              std::vector<bson_document> documents,
                                              bool ordered = true);

    // db::connection. `sql` is not SQL here: it is a collection name for `query` is
    // meaningless, so both are refused rather than misinterpreted.
    [[nodiscard]] status ping() override;
    [[nodiscard]] bool in_transaction() const noexcept override { return false; }
    [[nodiscard]] status begin() override;
    [[nodiscard]] status commit() override;
    [[nodiscard]] status rollback() override;
    [[nodiscard]] status query(std::string_view sql, span<const parameter> params,
                               result_set** out) override;
    void close() noexcept override;
    [[nodiscard]] bool is_open() const noexcept override;

    // Diagnostics.
    [[nodiscard]] const rpc_error& connect_error() const noexcept { return connect_error_; }
    [[nodiscard]] std::int32_t max_wire_version() const noexcept { return max_wire_version_; }
    [[nodiscard]] std::int32_t max_bson_size() const noexcept { return max_bson_size_; }
    [[nodiscard]] bool authenticated() const noexcept { return authenticated_; }
    [[nodiscard]] std::string_view server_version() const noexcept { return server_version_; }
    [[nodiscard]] const std::string& auth_mechanism_used() const noexcept {
        return mechanism_used_;
    }

    // Topology learned from the handshake `hello`. On a standalone every field except
    // `writable_primary` stays empty and `is_replica_set()` is false, which is also
    // what keeps retryable writes off (a standalone refuses `txnNumber`).
    [[nodiscard]] const replica_set_topology& topology() const noexcept { return topology_; }
    [[nodiscard]] bool is_replica_set() const noexcept { return topology_.replica_set; }

    // Logical sessions. A session is a client-generated 16-byte UUID sent as `lsid` on
    // every command; `start_session` replaces the current one (ending it on the
    // server), `end_session` sends `endSessions` and forgets it. The id is empty
    // between `end_session` and the next command's lazy re-creation.
    [[nodiscard]] bool has_session() const noexcept { return !session_id_.empty(); }
    [[nodiscard]] const std::vector<std::byte>& session_id() const noexcept {
        return session_id_;
    }
    [[nodiscard]] status start_session();
    [[nodiscard]] status end_session();

private:
    [[nodiscard]] status perform_handshake();
    [[nodiscard]] status authenticate();
    // Sends one OP_MSG request and reads the reply whose `responseTo` matches.
    [[nodiscard]] result<bson_document> round_trip(const bson_document& command);
    [[nodiscard]] status send_message(span<const std::byte> message);
    [[nodiscard]] result<std::vector<std::byte>> read_message();
    void fail(const status& error) noexcept;
    // Opens a fresh link and re-runs the handshake and authentication on it, so a
    // retryable write can be replayed with the same lsid and txnNumber.
    [[nodiscard]] status reconnect();
    // Creates the session if none exists; every application command has one.
    [[nodiscard]] status ensure_session();
    void new_session_id() noexcept;

    connection_options options_;
    std::unique_ptr<db::channel> link_;
    std::int32_t next_request_id_ = 1;
    std::int32_t max_wire_version_ = 0;
    std::int32_t max_bson_size_ = 16 * 1024 * 1024;
    std::string server_version_;
    std::string mechanism_used_;
    replica_set_topology topology_;
    std::vector<std::byte> session_id_;
    std::int64_t txn_number_ = 0;
    bool authenticated_ = false;
    bool open_ = false;
    rpc_error connect_error_;
};

// Opens connections over a caller-supplied channel, so tests and custom transports can be
// verified without a server.
class CRPC_API channel_factory final : public db::connection_factory {
public:
    using opener = std::function<result<std::unique_ptr<db::channel>>()>;

    channel_factory(connection_options options, opener open);
    ~channel_factory() override;

    [[nodiscard]] result<db::connection*> open() override;
    [[nodiscard]] std::string_view name() const noexcept override;

private:
    connection_options options_;
    opener open_;
};

// Opens one connection to one endpoint. A pool owns the result.
class CRPC_API connection_factory final : public db::connection_factory {
public:
    connection_factory(connection_options options, crpc::backend_factory& backend,
                       crpc::endpoint endpoint, crpc::runtime* shard = nullptr);
    ~connection_factory() override;

    [[nodiscard]] result<db::connection*> open() override;
    [[nodiscard]] std::string_view name() const noexcept override;

private:
    connection_options options_;
    crpc::backend_factory* backend_ = nullptr;
    crpc::endpoint endpoint_;
    crpc::runtime* shard_ = nullptr;
};

} // namespace mongo
} // namespace db
} // namespace crpc

#endif // CRPC_DB_MONGO_CONNECTION_HPP