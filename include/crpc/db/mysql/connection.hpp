// CRPC -- MySQL connection: handshake, authentication, queries, transactions.
#ifndef CRPC_DB_MYSQL_CONNECTION_HPP
#define CRPC_DB_MYSQL_CONNECTION_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
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
    // touch this. `multi_statements`/`multi_results` let one COM_QUERY answer with
    // several result sets, which `next_result()` walks.
    std::uint32_t capabilities =
        mask(capability::protocol_41) | mask(capability::long_password) |
        mask(capability::secure_connection) | mask(capability::plugin_auth) |
        mask(capability::plugin_auth_lenenc_data) | mask(capability::transactions) |
        mask(capability::multi_results) | mask(capability::multi_statements) |
        mask(capability::deprecate_eof);

    // TLS transport policy for the connection phase. When `enabled`, the client
    // sends the 32-byte SSLRequest packet (the response header with CLIENT_SSL)
    // after the greeting, replaces the wire with a TLS handshake, and then sends
    // the authentication response inside the encrypted session.
    struct tls {
        // Request the TLS upgrade. Default off: the connection stays cleartext,
        // exactly as before this option existed.
        bool enabled = false;

        // Fail closed. When the server does not offer CLIENT_SSL, a required
        // TLS session is refused before any byte is written; with `require ==
        // false` ("prefer") the session continues in cleartext and
        // `connection::tls_downgraded()` reports it. A build without the TLS
        // backend is refused either way: the downgrade rule covers what the
        // server can do, never what this binary was built with.
        bool require = true;

        // Verify the server certificate (chain and host name) during the
        // handshake. On by default; a test against a self-signed server may
        // turn it off, a production deployment must not.
        bool verify_certificate = true;

        // Sent as SNI and checked against the certificate's host names. Empty
        // omits SNI and the host-name check (the endpoint factory fills this
        // from the endpoint host unless that host is an IP literal).
        std::string server_name;

        // PEM trust anchors. Empty uses the system default store.
        std::string ca_bundle;
    };
    tls tls_options;
};

// Client-supplied bytes for `LOAD DATA LOCAL INFILE`.
//
// The security rule is absolute: the client decides what is sent. The SQL's path
// literal is never opened, because a client that serves whatever path the *server*
// names turns a compromised or hostile server into an arbitrary local file read.
class CRPC_API local_data_source {
public:
    virtual ~local_data_source();

    local_data_source(const local_data_source&) = delete;
    local_data_source& operator=(const local_data_source&) = delete;

    // Fills `buffer` and returns the byte count; 0 means end of data.
    [[nodiscard]] virtual result<std::size_t> read(span<std::byte> buffer) = 0;

protected:
    local_data_source() = default;
};

// A source over bytes the caller already holds.
class CRPC_API slice_data_source final : public local_data_source {
public:
    explicit slice_data_source(span<const std::byte> data) noexcept : data_(data) {}

    [[nodiscard]] result<std::size_t> read(span<std::byte> buffer) override;

private:
    span<const std::byte> data_;
    std::size_t offset_ = 0;
};

// A server-side prepared statement. Declared before `connection` because
// `prepare()` returns one by value.
class prepared_statement;

// One authenticated MySQL/MariaDB session.
//
// Not `final`: the TiDB adapter (`crpc::db::tidb::connection`) derives from
// this class because TiDB speaks the same wire protocol; it layers the TiDB
// capability surface on top without reimplementing the transport.
class CRPC_API connection : public db::connection {
public:
    // Performs the handshake and authentication over `link`. On failure the link
    // is closed and the error carries the server's own code and message.
    connection(connection_options options, std::unique_ptr<channel> link);
    ~connection() override;

    [[nodiscard]] status ping() override;
    [[nodiscard]] bool in_transaction() const noexcept override;
    [[nodiscard]] status reset_session() override;

    [[nodiscard]] status begin() override;
    [[nodiscard]] status begin(isolation_level level) override;
    [[nodiscard]] status commit() override;
    [[nodiscard]] status rollback() override;

    // MySQL's classic protocol has no client-side cancel; interrupting a running
    // statement requires `KILL QUERY <id>` from a second session, which cannot be
    // opened from an injected channel. A non-zero timeout is refused rather than
    // silently ignored (a timeout that does not interrupt would be worse than none).
    [[nodiscard]] status set_command_timeout(std::chrono::milliseconds timeout) noexcept override;

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
                              result_set** out) override;

    // Inserts `rows` with the prepared-statement protocol, one statement per batch
    // planned by `plan_insert_batches`. Every value is bound; identifiers are
    // validated because they cannot be. `affected` accumulates the server's count.
    [[nodiscard]] status bulk_insert(const db::insert_target& target,
                                     span<const std::vector<parameter>> rows,
                                     const db::bulk_options& options,
                                     std::uint64_t& affected);

    // Streams `source` in answer to a LOAD DATA LOCAL INFILE request. Only the
    // statement shape is checked; the path literal is never resolved, because the
    // bytes come from the caller and never from a file the server names.
    [[nodiscard]] status load_data(std::string_view load_sql, local_data_source& source,
                                   result_set** out, std::size_t chunk_size = 64u * 1024u);

    void close() noexcept override;
    [[nodiscard]] bool is_open() const noexcept override;

    // Diagnostics.
    [[nodiscard]] std::string_view server_version() const noexcept { return server_version_; }
    [[nodiscard]] std::uint32_t server_capabilities() const noexcept { return server_capabilities_; }
    [[nodiscard]] std::uint32_t negotiated_capabilities() const noexcept {
        return capabilities_;
    }
    [[nodiscard]] const std::string& auth_plugin() const noexcept { return auth_plugin_; }
    // True when authentication completed through caching_sha2_password full
    // authentication (the server's password cache was cold, so the client had
    // to prove the password again instead of riding the fast-auth scramble).
    // Diagnostics only: it observes the exchange shape, never the password.
    [[nodiscard]] bool used_full_authentication() const noexcept { return full_auth_; }
    // Why the handshake failed, when it did. A factory that only reports "handshake
    // failed" hides the server's own message, which is the one thing that helps.
    [[nodiscard]] const rpc_error& connect_error() const noexcept { return connect_error_; }
    // True when the session runs inside an established TLS session: the
    // SSLRequest went out, the handshake completed, and every later byte --
    // including the authentication response -- is encrypted. This is the same
    // fact `link_->is_secure()` reports to the auth layer.
    [[nodiscard]] bool tls_active() const noexcept { return tls_active_; }
    // True when the caller asked for TLS ("prefer", `tls.require == false`)
    // but the server did not offer CLIENT_SSL and the session continued in
    // cleartext. Diagnostics only: a downgrade is never silent, but it is also
    // not an error the caller asked for.
    [[nodiscard]] bool tls_downgraded() const noexcept { return tls_downgraded_; }

private:
    friend class prepared_statement;

    // Named `perform_handshake` rather than `handshake`: a member function called
    // `handshake` shadows the `handshake` type for the rest of the class, which
    // silently breaks the two declarations below it.
    [[nodiscard]] status perform_handshake();
    // Runs the TLS half of the connection phase for an enabled `tls_options`:
    // capability check, the SSLRequest packet, and the channel upgrade. Must
    // run after the greeting was decoded and before the authentication
    // response is sent.
    [[nodiscard]] status negotiate_tls(const handshake& greeting);
    [[nodiscard]] status authenticate(const handshake& greeting);
    [[nodiscard]] status read_message(std::vector<std::byte>& payload);
    [[nodiscard]] status send_message(span<const std::byte> payload, std::uint8_t sequence);
    [[nodiscard]] status run_query(std::string_view sql, result_set** out);
    // The only writer of `status_flags_` and, with `apply_error`, one of the two
    // writers of `server_txn_`; also the only place that clears `explicit_txn_`
    // from a protocol packet. (`explicit_txn_` has other writers: `begin()` sets
    // it, commit/rollback/fail/close clear it.)
    //
    // `flags` must come from a packet that really carried a status word: a short
    // EOF carries none, and the callers keep the previous value in that case (a stale
    // "in transaction" costs one harmless ROLLBACK; a wiped one costs the rollback
    // itself, which leaks uncommitted work into the pool).
    //
    // A middle packet of a multi-result command is not that command's conclusion,
    // so it updates `status_flags_` (the MORE_RESULTS test needs the fresh value)
    // but leaves the transaction state alone.
    void apply_status(std::uint16_t flags) noexcept {
        status_flags_ = flags;
        if ((flags & static_cast<std::uint16_t>(server_status::more_results)) != 0) {
            return;
        }
        const bool in_txn =
            (flags & static_cast<std::uint16_t>(server_status::in_transaction)) != 0;
        server_txn_ = in_txn;
        // The server ending the transaction ends ours too (DDL implicit commit,
        // COMMIT issued as text, autocommit=1). It never *starts* the latch.
        if (!in_txn) {
            explicit_txn_ = false;
        }
    }

    // An ERR packet carries no status word, so the previous one stands. Under
    // autocommit=0 the server keeps the transaction for a statement-level error
    // such as 1205 (lock wait timeout): InnoDB rolls back only that statement, and
    // the transaction and its locks survive. An error must therefore not make the
    // session look idle -- assume there is work to roll back rather than let the
    // pool hand it on. (A deadlock, 1213, rolls back the whole transaction; the
    // assumption then costs one harmless ROLLBACK.)
    //
    // The test reads the status word of the last *completed* command; an ERR
    // packet carries none, so a missing autocommit bit is inferred conservatively.
    // The decision errs in one direction only -- at worst one harmless extra
    // ROLLBACK -- and the next completed command's status word heals it.
    //
    // Both this and `apply_status` are the only writers of `server_txn_`, and both
    // are driven by a decoded protocol reply; `apply_error` only ever sets it
    // (never clears it).
    void apply_error() noexcept {
        if ((status_flags_ &
             static_cast<std::uint16_t>(server_status::autocommit)) == 0) {
            server_txn_ = true;
        }
    }

    // One meaning, one place: is there work a COMMIT or ROLLBACK must finish?
    // Readers: in_transaction() (the pool's P3 path), commit(), rollback().
    [[nodiscard]] bool has_uncommitted_work() const noexcept {
        return open_ && (explicit_txn_ || server_txn_);
    }
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
    void fail(const status& error) noexcept;

    connection_options options_;
    std::unique_ptr<channel> link_;
    std::string server_version_;
    std::uint32_t server_capabilities_ = 0;
    std::uint32_t capabilities_ = 0;
    std::string auth_plugin_;
    std::uint16_t status_flags_ = 0;
    // Two facts, two writers, two readers -- never one field for both.
    //
    // `explicit_txn_` is the latch of *our* begin(): set after START TRANSACTION
    // was answered, cleared by commit/rollback/fail/close and by the server
    // saying the transaction ended. Only `begin()` consults it: a server-side
    // implicit transaction must not block a new START TRANSACTION, because MySQL
    // implicitly commits it first.
    bool explicit_txn_ = false;
    // `server_txn_` is the IN_TRANS bit of the most recent packet that actually
    // carried a status word for a completed command. Written by the two decoded
    // protocol-reply paths (`apply_status`, and `apply_error` which only ever sets
    // it). It is what tells the pool there is uncommitted work when
    // the session runs with autocommit=0 and the caller never called begin().
    bool server_txn_ = false;
    bool open_ = false;
    std::uint8_t sequence_ = 0;
    // Shared with every prepared-statement handle; reset when the session closes.
    std::shared_ptr<void> session_guard_;
    rpc_error connect_error_;
    // Set when the session needed caching_sha2_password full authentication.
    bool full_auth_ = false;
    // Set once the channel has been upgraded to TLS (see `tls_active()`), and
    // when a "prefer" caller had to continue in cleartext (`tls_downgraded()`).
    bool tls_active_ = false;
    bool tls_downgraded_ = false;
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

// Opens connections over a caller-supplied channel.
//
// This is the injectable form of the endpoint factory: tests and custom transports
// (TLS wrappers, Unix sockets) supply the channel, so the pool plus this adapter can
// be verified end to end without a server.
class CRPC_API channel_factory final : public db::connection_factory {
public:
    using opener = std::function<result<std::unique_ptr<channel>>()>;

    channel_factory(connection_options options, opener open);
    ~channel_factory() override;

    [[nodiscard]] result<db::connection*> open() override;
    [[nodiscard]] std::string_view name() const noexcept override;

private:
    connection_options options_;
    opener open_;
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
