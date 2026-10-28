// CRPC -- primary/replica database cluster with read-write routing.
//
// One logical database described by one `primary` endpoint and any number of
// `replica` endpoints is owned by `cluster`. The pool each endpoint sits on is
// the ordinary `crpc::db::pool`; what this header adds is the routing decision
// that picks which pool a statement lands on, the health state machine that
// takes an unresponsive replica out of the read set, and the transactional
// affinity rules that keep every statement of one transaction on the same
// connection.
//
// The rules that must never be violated (each has a test):
//
//   * a write statement (INSERT/UPDATE/DELETE/DDL/transaction control, plus
//     anything `classify_statement` cannot prove to be a read) is sent to the
//     primary and never to a replica;
//   * once a transaction is open on the primary's connection, *every* statement
//     in it -- including SELECT -- stays on that same connection (`cluster_txn`).
//     The pool layer already rolls back a connection released mid-transaction
//     (P3); this class never releases it mid-transaction at all;
//   * a read may go to a replica only under the configured `read_route`, and a
//     per-call `route_hint` can force either target for one statement;
//   * a broken primary is reported to the caller, never silently replaced. This
//     client *never* promotes a replica to primary: failover is the DBA's or an
//     orchestrator's job, and a client that promotes on its own runs the risk of
//     splitting the cluster (two primaries accepting writes). See the docs for
//     the full reasoning.
//
// Replica reads are documented as *potentially stale*: replication is
// asynchronous, so a replica can answer a read with data older than the last
// observed write. When replication lags, the caller can request a bounded
// consistency check through `route_hint::consistent`, implemented per backend
// (MySQL waits on `WAIT_FOR_EXECUTED_GTID_SET`, see `mysql::gtid_replica_guard`).
// The generic layer only knows that a consistency witness exists; the backend
// supplies it. With no witness (`consistency == nullptr`) `consistent` reads
// behave exactly like ordinary reads.
//
// Concurrency: `cluster` serialises no caller traffic. Its only shared state is
// the health counters, the round-robin cursor and the last-write consistency
// token, all behind one mutex. Leases are acquired and released on the calling
// thread; a transaction's connection is owned by the `cluster_txn` object and
// is therefore naturally confined to the caller that holds it.
#ifndef CRPC_DB_CLUSTER_HPP
#define CRPC_DB_CLUSTER_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/db/connection.hpp>
#include <crpc/db/types.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace db {

// Where a read statement is routed. Writes are never affected: they always go
// to the primary.
enum class read_route : std::uint8_t {
    // Every read uses the primary. The safest default: no replica is ever read,
    // so there is no staleness and no replica to heal.
    primary,
    // Every read uses one of the replicas (round-robin across the live ones).
    // With zero replicas, or when every replica is unusable, the
    // `replica_depleted_policy` decides between failing and falling back to the
    // primary.
    replica,
    // Reads prefer a live replica and fall back to the primary when none is
    // available. `replica_depleted_policy` does not apply.
    prefer_replica,
    // Reads rotate across the primary and every live replica together, so the
    // primary still serves a share of reads.
    round_robin
};

[[nodiscard]] CRPC_API const char* to_string(read_route route) noexcept;

// What a read does when a `replica`/`round_robin` read finds no usable replica.
enum class replica_depleted_policy : std::uint8_t {
    // The read runs on the primary. The result is never duplicated or
    // corrupted; it is simply served by the freshest node.
    fallback_to_primary,
    // The read fails with `unavailable` rather than silently changing where it
    // ran.
    error
};

[[nodiscard]] CRPC_API const char* to_string(replica_depleted_policy policy) noexcept;

// What a statement is, for routing. The classifier is deliberately
// conservative: anything it cannot prove to be a read is a write, so a write
// can never reach a replica through misclassification. That comes at the cost
// of routing some read-shaped statements (for example `SHOW CREATE TABLE`) to
// the primary, which is always correct.
enum class statement_kind : std::uint8_t { read, write };

// Classifies `sql`. Comment and string literals are ignored, so a write verb
// that appears only inside a string cannot flip a read to a write. A batch is
// classified by its whole envelope, so "SELECT 1; INSERT ..." is a write even
// though it starts with SELECT.
[[nodiscard]] CRPC_API statement_kind classify_statement(std::string_view sql) noexcept;

// A per-call routing override for one statement.
struct route_hint {
    // Forced target for this call. `none` lets the configured `read_route`
    // decide. Forcing a `replica` target for a statement classified as a write
    // is refused (`invalid_argument`): writes must never reach a replica, not
    // even when the caller asks.
    enum class target : std::uint8_t { none, primary, replica };
    target reads = target::none;

    // Ask for read-your-writes consistency when this read goes to a replica:
    // the backend's consistency witness waits until the replica is caught up
    // before the SELECT runs. Ignored when the read lands on the primary (the
    // primary is fresh by definition) and when no witness is configured.
    bool consistent = false;

    [[nodiscard]] static route_hint primary() noexcept {
        route_hint hint;
        hint.reads = target::primary;
        return hint;
    }
    [[nodiscard]] static route_hint replica() noexcept {
        route_hint hint;
        hint.reads = target::replica;
        return hint;
    }
    [[nodiscard]] static route_hint read_your_writes() noexcept {
        route_hint hint;
        hint.consistent = true;
        return hint;
    }
};

// Per-backend consistency witness behind `route_hint::consistent`.
//
// A backend that can prove a replica has caught up implements this and hands it
// to the `cluster`; a backend without such a mechanism passes a null shared_ptr
// and consistent reads degrade to plain reads. The two hooks bracket a write on
// the primary and a read on a replica:
//
//     write(primary)  -> capture(primary, token)
//     read(replica)   -> wait(replica, token)  then SELECT
//
// `token` is an opaque backend-defined marker (MySQL: the primary's executed
// GTID set). Its only obligations are: after `capture` on the primary that
// accepted a write, `wait` on a replica returns `ok` exactly once the replica
// has applied every write the caller already observed through this cluster.
class CRPC_API replica_consistency {
public:
    virtual ~replica_consistency();

    replica_consistency(const replica_consistency&) = delete;
    replica_consistency& operator=(const replica_consistency&) = delete;

    // Samples the primary's write position into `token`, overwriting it.
    [[nodiscard]] virtual status capture(connection& primary, std::string& token) = 0;

    // Blocks until `replica` has reached `token`. A timeout or a replica that
    // cannot reach the token is a failure: `wait` must return an error, never a
    // silently satisfied wait.
    [[nodiscard]] virtual status wait(connection& replica, const std::string& token) = 0;

protected:
    replica_consistency() = default;
};

// Tunables for a `cluster`.
struct cluster_options {
    // Applied to the pool of every endpoint, primary and replicas alike.
    pool_options pool{};

    // Where reads go. See `read_route`.
    read_route reads = read_route::primary;

    // What a `replica`/`round_robin` read does when no replica is usable.
    replica_depleted_policy depleted = replica_depleted_policy::fallback_to_primary;

    // Replica health (the memcached-router state machine, applied to reads).
    // Consecutive transport failures before a replica is dropped from the read
    // set. 0 disables ejection; the counters and `replica_health()` still update.
    std::uint32_t replica_failure_threshold = 3;

    // How long a dead replica stays out of the read set before a probe is
    // attempted. Zero means "probe on the next read".
    std::chrono::milliseconds replica_retry_cooldown{30000};

    // Display names for diagnostics. `primary_name` defaults to "primary";
    // `replica_names` defaults to "replica:<i>". A caller (a backend builder)
    // normally sets them from the endpoints it was configured with.
    std::string primary_name;
    std::vector<std::string> replica_names;
};

// One replica's point-in-time health, for operations and diagnostics.
struct CRPC_API replica_health {
    // The display name supplied at construction (conventionally "host:port").
    std::string name;
    // In the read set (`true`) or ejected after `replica_failure_threshold`
    // transport failures (`false`). Re-admitted only after a successful probe.
    bool alive = true;
    // Transport failures since the last successful exchange. Application errors
    // prove the endpoint is reachable and reset this to zero.
    std::uint32_t consecutive_failures = 0;
    // The most recent transport failure. Retained after recovery; a
    // `consecutive_failures` of zero means the replica is currently healthy.
    rpc_code last_error = rpc_code::ok;
    std::string last_error_message;
    // Time left before the next recovery probe. Zero while alive, or once the
    // cooldown has elapsed and a probe is due.
    std::chrono::milliseconds retry_in{0};
};

// One server-side transaction, pinned to one primary connection.
//
// The pool's P3 rule rolls back a connection released mid-transaction; this
// class never releases it until commit or rollback, so every statement of the
// transaction -- SELECT included -- provably runs on the same connection. It is
// move-only, and its destructor rolls back an unfinished transaction and
// returns the connection to the pool.
class CRPC_API cluster_txn {
public:
    cluster_txn() noexcept = default;
    ~cluster_txn();

    cluster_txn(const cluster_txn&) = delete;
    cluster_txn& operator=(const cluster_txn&) = delete;
    cluster_txn(cluster_txn&& other) noexcept;
    cluster_txn& operator=(cluster_txn&& other) noexcept;

    // Every statement in a transaction runs on the pinned connection, so a
    // SELECT here goes to the primary that owns the transaction -- never to a
    // replica.
    [[nodiscard]] result<result_set*> query(std::string_view sql,
                                            span<const parameter> params = {},
                                            route_hint hint = {});

    // Convenience over `query` for a statement that returns no rows.
    [[nodiscard]] status execute(std::string_view sql, span<const parameter> params = {});

    // Ends the transaction. A transaction that saw a transport failure refuses
    // commit and rolls back instead (`rollback()`), because committing an
    // interrupted transaction is exactly the state a two-phase commit exists to
    // prevent.
    [[nodiscard]] status commit();
    [[nodiscard]] status rollback();

    // True from a successful `cluster::begin_transaction` until commit or
    // rollback.
    [[nodiscard]] bool in_transaction() const noexcept { return lease_.valid() && !finished_; }

    // The underlying session, for a caller that needs backend-specific state
    // (e.g. `SELECT CONNECTION_ID()` on the same connection). Null when the
    // transaction is not open.
    [[nodiscard]] connection* session() const noexcept {
        return finished_ ? nullptr : lease_.get();
    }

private:
    friend class cluster;
    cluster_txn(pool::lease lease, std::shared_ptr<replica_consistency> consistency,
                std::shared_ptr<void> write_state);

    // Ends the transaction with the given outcome; `ok` decides between commit
    // and rollback. Returns the connection to the pool and samples the
    // primary's write position for later consistent reads.
    [[nodiscard]] status finish(bool ok);

    pool::lease lease_;
    std::shared_ptr<replica_consistency> consistency_;
    std::shared_ptr<void> write_state_;
    bool finished_ = true;
};

// The primary/replica facade. Owns the endpoint factories and their pools.
class CRPC_API cluster {
public:
    // Takes ownership of the factories. `consistency` may be null; see
    // `replica_consistency`.
    cluster(std::unique_ptr<connection_factory> primary_factory,
            std::vector<std::unique_ptr<connection_factory>> replica_factories,
            std::shared_ptr<replica_consistency> consistency,
            cluster_options options = {});
    ~cluster();

    cluster(const cluster&) = delete;
    cluster& operator=(const cluster&) = delete;
    cluster(cluster&&) = delete;
    cluster& operator=(cluster&&) = delete;

    // Runs `sql`, routed by statement kind, the configured `read_route` and
    // `hint`. The returned cursor is owned by the caller, as with
    // `client::query`.
    [[nodiscard]] result<result_set*> query(std::string_view sql,
                                            span<const parameter> params = {},
                                            route_hint hint = {});

    // A stored procedure may write, so it always runs on the primary.
    [[nodiscard]] result<result_set*> call(std::string_view sql, span<parameter> params);

    [[nodiscard]] status execute(std::string_view sql, span<const parameter> params = {},
                                 route_hint hint = {});

    // Opens a transaction on a primary connection. All subsequent statements go
    // through the returned `cluster_txn`, never through this cluster's routing.
    [[nodiscard]] result<cluster_txn> begin_transaction();

    // --- Replica health ------------------------------------------------------
    // A snapshot in replica order; cheap and local, never contacts a server.
    [[nodiscard]] std::vector<replica_health> replica_health() const;

    // Probes one replica immediately, regardless of the cooldown. Returns true
    // when the replica answered a ping. An out-of-range index is
    // `invalid_argument`. A re-admitted replica re-enters the read set.
    [[nodiscard]] result<bool> probe_replica(std::size_t index);

    // The primary is never ejected and never replaced; this reports its
    // current name and whether the cluster is closed.
    [[nodiscard]] std::size_t replica_count() const noexcept;

    // Number of connections opened against each pool, for tests and
    // diagnostics. `primary_opened()` is always valid; `replica_opened(i)` is
    // `invalid_argument` when `i` is out of range.
    [[nodiscard]] std::uint64_t primary_opened() const noexcept;
    [[nodiscard]] result<std::uint64_t> replica_opened(std::size_t index) const;

    // P4/P15: further calls fail with `shutting_down`.
    void close() noexcept;
    [[nodiscard]] bool is_closed() const noexcept;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace db
} // namespace crpc

#endif // CRPC_DB_CLUSTER_HPP