// CRPC -- MySQL connection: handshake, authentication, queries, transactions.
#ifndef CRPC_DB_MYSQL_CONNECTION_HPP
#define CRPC_DB_MYSQL_CONNECTION_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/db/bulk.hpp>
#include <crpc/db/connection.hpp>
#include <crpc/db/mysql/channel.hpp>
#include <crpc/db/mysql/protocol.hpp>
#include <crpc/error.hpp>
#include <crpc/io.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace db {
namespace mysql {

// Tunables for one MySQL connection.
struct connection_options {
    std::string user;
    std::string password;
    // Selected with `USE` after authenticating; empty leaves the server default.
    std::string database;

    // utf8mb4_general_ci. The server translates to its own collation.
    std::uint8_t charset = 45;

    // Largest message accepted from the server, before reassembly is refused.
    std::size_t max_message_size = default_max_message_size;

    // A result set is materialised before `query()` returns, because the cursor
    // outlives the connection lease (the pool would otherwise hand the connection
    // to another caller mid-result). These bound that materialisation: exceeding
    // either fails the query with `resource_exhausted` instead of growing memory
    // until the process dies.
    std::size_t max_result_rows = 1'000'000;
    std::size_t max_result_bytes = 256u * 1024u * 1024u;

    // Client capabilities to request, intersected with what the server offers.
    // The defaults are what a modern client needs; a caller should not normally
    // touch this.
    std::uint32_t capabilities =
        mask(capability::protocol_41) | mask(capability::long_password) |
        mask(capability::secure_connection) | mask(capability::plugin_auth) |
        mask(capability::plugin_auth_lenenc_data) | mask(capability::transactions) |
        mask(capability::multi_results) | mask(capability::deprecate_eof);
};

// A server-side prepared statement. Declared before `connection` because
// `prepare()` returns one by value.
class prepared_statement;

// One authenticated MySQL/MariaDB session.
class CRPC_API connection final : public db::connection {
public:
    // Performs the handshake and authentication over `link`. On failure the link
    // is closed and the error carries the server's own code and message.
    connection(connection_options options, std::unique_ptr<channel> link);
    ~connection() override;

    [[nodiscard]] status ping() override;
    [[nodiscard]] bool in_transaction() const noexcept override;

    [[nodiscard]] status begin() override;
    [[nodiscard]] status commit() override;
    [[nodiscard]] status rollback() override;

    [[nodiscard]] status query(std::string_view sql, span<const parameter> params,
                               result_set** out) override;

    // Prepares `sql` on this session. The returned handle owns a server-side
    // statement id; it is closed when the handle dies, and any statement still open
    // when the session closes is closed by the server.
    //
    // A prepared statement is per-session state: it cannot outlive the connection,
    // and reusing a statement id on another connection addresses nothing.
    [[nodiscard]] result<prepared_statement> prepare(std::string_view sql);

    // Executes a stored procedure call, writing OUT/INOUT values back into params.
    //
    // MySQL requires a procedure's OUT argument to be a session variable, so an
    // output placeholder is rewritten to @crpc_out_<n>, INOUT values are assigned
    // first with a prepared SET, and the variables are read back with one SELECT
    // after the call. That is the documented mechanism; there is no way to bind an
    // OUT parameter directly.
    [[nodiscard]] status call(std::string_view call_sql, span<parameter> params,
                              result_set** out);

    // Inserts ows with the prepared-statement protocol, one statement per batch
    // planned by plan_insert_batches. Every value is bound; identifiers are
    // validated because they cannot be. ffected accumulates the server's count.
    [[nodiscard]] status bulk_insert(const db::insert_target& target,
                                     span<const std::vector<parameter>> rows,
                                     const db::bulk_options& options,
                                     std::uint64_t& affected);

    void close() noexcept override;
    [[nodiscard]] bool is_open() const noexcept override;

    // Diagnostics.
    [[nodiscard]] std::string_view server_version() const noexcept { return server_version_; }
    [[nodiscard]] std::uint32_t server_capabilities() const noexcept { return server_capabilities_; }
    [[nodiscard]] std::uint32_t negotiated_capabilities() const noexcept {
        return capabilities_;
    }
    [[nodiscard]] const std::string& auth_plugin() const noexcept { return auth_plugin_; }

private:
    friend class prepared_statement;

    // Named `perform_handshake` rather than `handshake`: a member function called
    // `handshake` shadows the `handshake` type for the rest of the class, which
    // silently breaks the two declarations below it.
    [[nodiscard]] status perform_handshake();
    [[nodiscard]] status authenticate(const handshake& greeting);
    [[nodiscard]] status read_message(std::vector<std::byte>& payload);
    [[nodiscard]] status send_message(span<const std::byte> payload, std::uint8_t sequence);
    [[nodiscard]] status run_query(std::string_view sql, result_set** out);
// Executes a prepared statement and decodes its reply (binary protocol).
    // `allow_more_results` exists for CALL: a procedure returns its result set and
    // then a trailing OK, and refusing the trailing packet would leave the stream
    // desynchronised.
    [[nodiscard]] status execute_prepared(std::uint32_t statement_id,
                                          span<const parameter> params, result_set** out,
                                          bool allow_more_results = false);
    // Sends COM_STMT_CLOSE. The server sends no reply, so this must not read one.
    void release_statement(std::uint32_t statement_id) noexcept;
    // Reads a result set whose rows are encoded by the binary protocol.
    [[nodiscard]] status read_binary_result(std::uint64_t column_count, result_set** out,
                                           bool allow_more_results = false);
    [[nodiscard]] status set_autocommit(bool enabled);
    void fail(const status& error) noexcept;

    connection_options options_;
    std::unique_ptr<channel> link_;
    std::string server_version_;
    std::uint32_t server_capabilities_ = 0;
    std::uint32_t capabilities_ = 0;
    std::string auth_plugin_;
    std::uint16_t status_flags_ = 0;
    // Set once a transaction is open, so the pool's rollback path is exact even
    // when the server does not report `SERVER_STATUS_IN_TRANS`.
    bool in_transaction_ = false;
    bool open_ = false;
    std::uint8_t sequence_ = 0;
    // Shared with every prepared-statement handle; reset when the session closes.
    std::shared_ptr<void> session_guard_;
};

// A server-side prepared statement, owned by the session that prepared it.
//
// The statement id is session state, so the handle holds the connection it came
// from: if that connection is destroyed or closed, the handle stops trying to close
// the statement server-side (the server drops all statements when the session ends).
class CRPC_API prepared_statement {
public:
    prepared_statement() noexcept = default;
    ~prepared_statement();

    prepared_statement(const prepared_statement&) = delete;
    prepared_statement& operator=(const prepared_statement&) = delete;
    prepared_statement(prepared_statement&& other) noexcept;
    prepared_statement& operator=(prepared_statement&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return owner_ != nullptr; }
    [[nodiscard]] std::uint32_t id() const noexcept { return statement_id_; }
    [[nodiscard]] std::size_t param_count() const noexcept { return param_count_; }
    [[nodiscard]] std::size_t column_count() const noexcept { return column_count_; }

    // Executes with `params` and returns the owned cursor (may be null for a
    // statement that produces no rows).
    [[nodiscard]] result<result_set*> execute(span<const parameter> params);

    // Closes the statement server-side now. Idempotent; the destructor calls it.
    void close() noexcept;

private:
friend class connection;
    // `allow_more_results` is for CALL: the procedure's result set is followed by a
    // trailing OK packet.
    [[nodiscard]] result<result_set*> execute_impl(span<const parameter> params,
                                                   bool allow_more_results);
    prepared_statement(connection* owner, std::weak_ptr<void> guard,
                       std::uint32_t statement_id, std::size_t params,
                       std::size_t columns) noexcept;

    connection* owner_ = nullptr;
    // Expires when the session closes, so a handle that outlived its connection
    // cannot call into a destroyed object.
    std::weak_ptr<void> guard_;
    std::uint32_t statement_id_ = 0;
    std::size_t param_count_ = 0;
    std::size_t column_count_ = 0;
};

// Opens one connection to one endpoint. A pool owns the result.
//
// The factory borrows the backend and the shard: a coroutine's connection parks
// on that shard, so opening from inside a coroutine keeps the calling thread free.
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

} // namespace mysql
} // namespace db
} // namespace crpc

#endif // CRPC_DB_MYSQL_CONNECTION_HPP