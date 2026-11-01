// CRPC -- the database abstraction and connection pool.
//
// Scope of this header: the interfaces a backend implements and the pool that
// owns connections. No wire protocol lives here; each backend implements
// `connection` and `connection_factory` and the pool is protocol-agnostic, which
// is what lets the pool be tested with a fake connection and no server.
//
// Exported signatures stay ABI-clean: results are `result<T>` of scalars,
// `std::string_view` and `std::vector<std::byte>` (the same shapes the transport
// already exports), and a cursor is handed out through a `result_set**`
// out-parameter rather than a `unique_ptr` by value.
//
// Pool contract (each rule has a test in tests/test_db_pool.cpp)
// -------------------------------------------------------------
// P1  1:1            every successful `acquire()` corresponds to exactly one
//                    return, and `lease` is the only way to hold a connection.
// P2  idempotent     releasing twice returns the connection once; the second
//                    call is a no-op, not a second insert.
// P3  transaction    a connection still in a transaction is rolled back before it
//                    may go back to the idle set; if the rollback fails the
//                    connection is closed instead.
// P4  close          `close()` rejects further acquires with `shutting_down` and
//                    closes idle connections; a lease released afterwards is
//                    closed, never returned.
// P5  bounded        at most `max_size` connections exist; a caller beyond that
//                    waits up to `acquire_timeout` and then gets
//                    `resource_exhausted`, which is retryable.
// P6  single writer  every counter and every container is mutated under one
//                    mutex; a lease never mutates pool state except through
//                    `release()`.
// P7  monotonic      each connection carries an increasing id, and a connection
//                    older than `max_lifetime` is closed instead of reused.
// P8  max_idle       a release beyond `max_idle` closes the connection.
// P9  validated      an idle connection is pinged before it is handed out; a
//                    failed ping discards it and the next idle one is tried.
// P10 lifetime       a lease outliving its pool is safe: it holds the shared
//                    state, and releasing into a destroyed pool closes the
//                    connection.
// P11 isolation      two pools never share state, even over one factory.
// P12 cancel         not modelled yet: an acquire cannot be cancelled, only
//                    timed out. Documented so a caller does not assume otherwise.
// P13 statistics     idle/in_use/total/open_count are readable at any time.
// P14 poisoned       a connection whose protocol is desynchronised cannot be
//                    detected by the pool; the caller reports it with
//                    `lease::discard()`, which closes it instead of returning it.
// P15 idempotent     `close()` may be called any number of times, from any thread.
#ifndef CRPC_DB_CONNECTION_HPP
#define CRPC_DB_CONNECTION_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/db/types.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace db {

// A forward-only cursor over one statement's results.
//
// `next()` advances and returns false at the end; the accessors read the current
// row. A value the caller does not read is not materialised beyond what the
// driver already buffered, so a large result is consumed row by row.
class CRPC_API result_set {
public:
    virtual ~result_set();

    result_set(const result_set&) = delete;
    result_set& operator=(const result_set&) = delete;

    [[nodiscard]] virtual std::size_t column_count() const noexcept = 0;
    [[nodiscard]] virtual column_info column(std::size_t index) const = 0;

    // Advances to the next row. False means end of results.
    [[nodiscard]] virtual bool next() = 0;

    [[nodiscard]] virtual bool is_null(std::size_t index) const noexcept = 0;
    // A value that does not fit the requested type is `invalid_argument`; a value
    // the driver cannot represent at all is `decode_error`. A null is reported as
    // `is_null`, never as a zero.
    [[nodiscard]] virtual result<std::int64_t> get_int64(std::size_t index) = 0;
    [[nodiscard]] virtual result<std::uint64_t> get_uint64(std::size_t index) = 0;
    [[nodiscard]] virtual result<double> get_double(std::size_t index) = 0;
    [[nodiscard]] virtual result<bool> get_bool(std::size_t index) = 0;
    [[nodiscard]] virtual result<std::string_view> get_text(std::size_t index) = 0;
    [[nodiscard]] virtual result<std::vector<std::byte>> get_blob(std::size_t index) = 0;

    // Rows affected by the statement that produced this cursor (INSERT/UPDATE/
    // DELETE), or -1 when the backend does not report it.
    [[nodiscard]] virtual std::int64_t affected_rows() const noexcept = 0;

    // Advances to the next result set of this batch, when the backend produced
    // more than one. Returns true when the cursor now exposes a fresh result set
    // whose columns are readable immediately and whose rows come from `next()`.
    // Returns false when there are no further result sets; the cursor is then at
    // end and every accessor reports NULL / 0 as before.
    //
    // A cursor that already decoded every set (a materialised one) switches
    // instantly; a streaming one drains the remainder of the current set first, so
    // a caller that wants a set's remaining rows must read them before calling
    // `next_result()`. The default returns false: one result per cursor.
    [[nodiscard]] virtual bool next_result() { return false; }

    // True when this cursor is fed lazily from the channel instead of from a
    // materialised copy. A streaming cursor keeps the connection busy until it is
    // consumed to the end or closed (closing drains the rest), so it must not be
    // read at the same time as another statement on its connection.
    [[nodiscard]] virtual bool is_streaming() const noexcept { return false; }

    // When `next()` ends, the failure that stopped the read, or `ok` when the
    // cursor ended on its own (last row of the last set). Materialised cursors
    // always report `ok`; a streaming read that lost the socket or was cancelled
    // reports it here so `next() == false` can be told apart from "no more rows".
    [[nodiscard]] virtual rpc_error last_error() const noexcept { return {}; }

    // Streaming diagnostics. `rows_decoded` counts every row this cursor has taken
    // from the channel; `rows_buffered` how many it holds at rest. A materialised
    // cursor buffers its whole result, a streaming one never more than the current
    // row -- which is how a caller proves a large result is not being materialised.
    [[nodiscard]] virtual std::size_t rows_decoded() const noexcept { return 0; }
    [[nodiscard]] virtual std::size_t rows_buffered() const noexcept { return 0; }

    // Releases the server-side cursor. Idempotent; called by the destructor. For a
    // streaming cursor this also drains any reply still in flight, so the
    // connection stays aligned for the next statement.
    virtual void close() noexcept = 0;

protected:
    result_set() = default;
};

// One connection to one database.
//
// Every method is synchronous in the coroutine sense: inside a CRPC coroutine it
// parks the calling coroutine, on a plain thread it blocks. Nothing here spawns a
// worker thread.
class CRPC_API connection {
public:
    virtual ~connection();

    connection(const connection&) = delete;
    connection& operator=(const connection&) = delete;

    // Cheap liveness probe. Used by the pool before handing an idle connection
    // out, so it must be a round trip the server answers cheaply (`SELECT 1`).
    [[nodiscard]] virtual status ping() = 0;

    // True while a transaction this connection started is still open. The pool consults
    // it before returning the connection to the idle set (P3).
    //
    // A backend that can only observe transactions it opened itself reports nothing for
    // one started with raw SQL; the contract is scoped to `begin()`. Where the answer has
    // to be a guess it must guess "open": a spurious true costs one rollback that ends
    // nothing, while a spurious false hands the next borrower a transaction full of
    // uncommitted work.
    [[nodiscard]] virtual bool in_transaction() const noexcept = 0;

    [[nodiscard]] virtual status begin() = 0;
    [[nodiscard]] virtual status commit() = 0;
    [[nodiscard]] virtual status rollback() = 0;

    // Restores the session state the next borrower is entitled to assume: a known
    // clean state, not whatever the last borrower left behind. It need not be the
    // state a freshly opened connection would have -- a server started with
    // `--autocommit=0` hands out sessions that are already surprising, and copying
    // that default would put the next borrower back in the same trap.
    //
    // The pool calls this before a connection goes back to the idle set, after it
    // has rolled back any open transaction. A backend that cannot change session
    // state, or that has none, keeps the default and costs nothing.
    [[nodiscard]] virtual status reset_session() { return status{}; }

    // Starts a transaction at a requested isolation level. The default forwards to
    // `begin()` for `unspecified` and reports `no_such_method` otherwise, so a
    // backend that does not model isolation levels keeps compiling and working
    // without knowing the extension exists.
    [[nodiscard]] virtual status begin(isolation_level level) {
        if (level == isolation_level::unspecified) {
            return begin();
        }
        return status{rpc_code::no_such_method,
                      "this backend does not support transaction isolation levels"};
    }

    // The longest a command may run before the backend interrupts it server-side.
    // Zero (the default) means no client-side bound. Backends that can stop the
    // server really do; a backend that cannot interrupt does not fake success,
    // `set_command_timeout` reports `no_such_method` for a non-zero value instead.
    [[nodiscard]] virtual status set_command_timeout(std::chrono::milliseconds timeout) noexcept {
        command_timeout_ = timeout;
        return status{};
    }
    [[nodiscard]] virtual std::chrono::milliseconds command_timeout() const noexcept {
        return command_timeout_;
    }

    // Runs `sql` with `params` and, on success, stores a new cursor in `*out`.
    // The caller owns it and must delete it (or use `statement_guard`).
    //
    // `params` is a span of `parameter`: a backend that supports prepared
    // statements must bind them, never splice them into the text. A backend
    // without prepared statements must refuse a non-empty `params` rather than
    // escape by hand.
    [[nodiscard]] virtual status query(std::string_view sql, span<const parameter> params,
                                       result_set** out) = 0;

    // A statement with no result cursor (DDL, or a DML the caller does not need
    // to read). Implemented in terms of `query` so a backend only writes one.
    // The parameter span defaults to empty, matching `client::execute`.
    [[nodiscard]] status execute(std::string_view sql, span<const parameter> params = {});

    // Runs `sql` and returns a cursor that is NOT materialised: `next()` pulls
    // rows from the channel as the caller reads them, so a large result never
    // lives in memory at once. A streaming cursor keeps the connection busy until
    // it is drained or closed, so it must be driven to the end before the
    // connection is reused.
    //
    // The default reports `no_such_method`: a backend opts in, so a caller that
    // asks for streaming never silently gets a materialised copy (which is exactly
    // what streaming exists to avoid).
    [[nodiscard]] virtual status query_streaming(std::string_view sql,
                                                 span<const parameter> params,
                                                 result_set** out) {
        (void)sql;
        (void)params;
        if (out != nullptr) {
            *out = nullptr;
        }
        return status{rpc_code::no_such_method,
                      "this backend does not support streaming result sets"};
    }

    // Runs a stored procedure, writing OUT/INOUT values back into `params`.
    //
    // It is a separate entry point rather than a flag on `query` because the
    // mechanism is backend-specific: MySQL cannot bind a placeholder for a value the
    // procedure writes, so it rewrites those arguments to session variables and
    // reads them back. A caller that sent an output parameter through `query` would
    // be binding a value nobody reads.
    virtual status call(std::string_view sql, span<parameter> params, result_set** out) {
        // A backend without procedure support reports it instead of silently
        // ignoring the output parameters.
        (void)sql;
        (void)params;
        if (out != nullptr) {
            *out = nullptr;
        }
        return status{rpc_code::no_such_method,
                      "this backend does not implement stored procedures"};
    }

    // Closes the link. Idempotent. A closed connection must fail every later call
    // with `shutting_down`.
    virtual void close() noexcept = 0;

    [[nodiscard]] virtual bool is_open() const noexcept = 0;

protected:
    connection() = default;
    // Read by a backend's request path to arm its timeout watchdog. Stored in the
    // base so the pool layer has one way to discover what a connection was asked
    // for; a backend that overrides `set_command_timeout` may keep its own copy.
    std::chrono::milliseconds command_timeout_{0};
};

// Owns a cursor and deletes it, so a caller can return early without leaking.
class statement_guard {
public:
    statement_guard() noexcept = default;
    explicit statement_guard(result_set* set) noexcept : set_(set) {}
    ~statement_guard() {
        if (set_ != nullptr) {
            set_->close();
            delete set_;
        }
    }

    statement_guard(const statement_guard&) = delete;
    statement_guard& operator=(const statement_guard&) = delete;

    statement_guard(statement_guard&& other) noexcept : set_(other.set_) {
        other.set_ = nullptr;
    }
    statement_guard& operator=(statement_guard&& other) noexcept {
        if (this != &other) {
            if (set_ != nullptr) {
                set_->close();
                delete set_;
            }
            set_ = other.set_;
            other.set_ = nullptr;
        }
        return *this;
    }

    [[nodiscard]] result_set* get() const noexcept { return set_; }
    [[nodiscard]] result_set* operator->() const noexcept { return set_; }
    [[nodiscard]] bool valid() const noexcept { return set_ != nullptr; }

private:
    result_set* set_ = nullptr;
};

// Creates connections. A backend implements this; the pool owns the lifetime of
// what it returns.
class CRPC_API connection_factory {
public:
    virtual ~connection_factory();

    connection_factory(const connection_factory&) = delete;
    connection_factory& operator=(const connection_factory&) = delete;

    // Opens one connection. A failure is reported as a value; the pool counts it
    // and does not retry inside `acquire()` beyond the caller's own retry.
    [[nodiscard]] virtual result<connection*> open() = 0;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;

protected:
    connection_factory() = default;
};

// Tunables for a pool.
struct pool_options {
    // Connections kept warm. The pool never blocks to reach it: `warmup()` opens
    // them, and `acquire()` opens on demand.
    std::size_t min_size = 0;

    // Hard ceiling on live connections (idle + in use).
    std::size_t max_size = 8;

    // Ceiling on idle connections; a release beyond it closes the connection.
    std::size_t max_idle = 8;

    // How long an acquire waits for a free connection before
    // `resource_exhausted`.
    std::chrono::nanoseconds acquire_timeout{std::chrono::seconds(5)};

    // Close an idle connection that has been idle for longer. Zero disables it.
    std::chrono::nanoseconds idle_timeout{std::chrono::seconds(60)};

    // Close a connection older than this. Zero disables it.
    std::chrono::nanoseconds max_lifetime{std::chrono::minutes(30)};

    // Ping an idle connection before handing it out. Turning this off trades a
    // round trip for the risk of handing out a connection the server already
    // dropped; leave it on unless the backend's ping is expensive.
    bool validate_on_borrow = true;
};

// A fixed-capacity pool over one factory.
class CRPC_API pool {
public:
    // RAII handle. Returns the connection on destruction, so an early return or a
    // thrown exception cannot leak it. Copyable leases are deliberately not
    // offered: one lease is one borrowed connection (P1).
    class CRPC_API lease {
    public:
        lease() noexcept = default;
        ~lease();

        lease(const lease&) = delete;
        lease& operator=(const lease&) = delete;
        lease(lease&& other) noexcept;
        lease& operator=(lease&& other) noexcept;

        [[nodiscard]] bool valid() const noexcept { return connection_ != nullptr; }
        [[nodiscard]] connection* get() const noexcept { return connection_; }
        [[nodiscard]] connection* operator->() const noexcept { return connection_; }

        // Returns the connection now. Idempotent (P2).
        [[nodiscard]] status release() noexcept;

        // Closes the connection instead of returning it, for a connection whose
        // protocol is known to be broken (P14). Idempotent.
        void discard() noexcept;

    private:
        friend class pool;
        lease(std::shared_ptr<void> state, connection* conn,
              std::chrono::steady_clock::time_point created) noexcept;

        std::shared_ptr<void> state_;
        connection* connection_ = nullptr;
        // Carried across the borrow so `max_lifetime` is measured from open, not
        // from the last release.
        std::chrono::steady_clock::time_point created_{};
    };

    pool(pool_options options, connection_factory& factory);
    ~pool();

    pool(const pool&) = delete;
    pool& operator=(const pool&) = delete;
    pool(pool&&) = delete;
    pool& operator=(pool&&) = delete;

    [[nodiscard]] result<lease> acquire();

    // Best-effort pre-creation of `min_size` connections.
    void warmup();

    // P4/P15.
    void close() noexcept;

    [[nodiscard]] std::size_t idle() const noexcept;
    [[nodiscard]] std::size_t in_use() const noexcept;
    [[nodiscard]] std::size_t total() const noexcept;
    // Connections opened since construction, including closed ones. Useful to
    // prove a discarded connection was really replaced.
    [[nodiscard]] std::uint64_t opened() const noexcept;
    [[nodiscard]] bool is_closed() const noexcept;

private:
    struct impl;
    std::shared_ptr<impl> impl_;
};

} // namespace db
} // namespace crpc

#endif // CRPC_DB_CONNECTION_HPP