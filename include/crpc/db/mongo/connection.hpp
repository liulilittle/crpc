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
};

// A decoded reply to one command.
struct command_result {
    bson_document reply;
    bool ok = false;
    std::string error_message;
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

private:
    [[nodiscard]] status perform_handshake();
    [[nodiscard]] status authenticate();
    // Sends one OP_MSG request and reads the reply whose `responseTo` matches.
    [[nodiscard]] result<bson_document> round_trip(const bson_document& command);
    [[nodiscard]] status send_message(span<const std::byte> message);
    [[nodiscard]] result<std::vector<std::byte>> read_message();
    void fail(const status& error) noexcept;

    connection_options options_;
    std::unique_ptr<db::channel> link_;
    std::int32_t next_request_id_ = 1;
    std::int32_t max_wire_version_ = 0;
    std::int32_t max_bson_size_ = 16 * 1024 * 1024;
    std::string server_version_;
    std::string mechanism_used_;
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