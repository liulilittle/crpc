// CRPC -- automatic retry of an optimistic transaction.
//
// The single biggest usage difference between TiDB and MySQL is what a write
// conflict does. Under MySQL's (and TiDB pessimistic) row locking the second
// writer waits and then proceeds once the first writer commits. Under TiDB's
// optimistic transaction model both writers proceed and the *first to commit*
// wins; the loser learns about it at `COMMIT` with error 9007
// ("Write conflict ... reason=Optimistic [try again later]"), when the whole
// transaction is already aborted. The MySQL 1205 lock-wait timeout and the
// TiDB 9007 write conflict both map to `rpc_code::unavailable` (retryable),
// and the raw code is preserved in `rpc_error::metadata["sql_code"]` so the
// two stay distinguishable.
//
// `retry_transaction` is that retry, packaged: the caller submits the body of
// the transaction as a callable, and on a conflict the wrapper rolls the
// attempt back (the pool rolls an open transaction back on release) and runs
// the whole unit again, up to `transaction_retry_options::max_attempts`, with
// exponential backoff.
#ifndef CRPC_DB_TIDB_RETRY_HPP
#define CRPC_DB_TIDB_RETRY_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>

#include <crpc/config.hpp>
#include <crpc/db/connection.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace db {
namespace tidb {

// Limits and timing for `retry_transaction`.
struct transaction_retry_options {
    // Total attempts including the first. A non-conflict failure ends the call
    // without touching this budget.
    std::size_t max_attempts = 5;

    // Exponential backoff between two attempts: base * multiplier^(attempt-1),
    // capped at `max_backoff`. The sleep runs on the calling thread.
    std::chrono::milliseconds base_backoff{10};
    std::chrono::milliseconds max_backoff{200};
    double backoff_multiplier = 2.0;

    // SQL run on the freshly borrowed connection before `begin()`, for example
    // "SET SESSION tidb_txn_mode='optimistic'". The transaction mode is a
    // per-session variable, so a pooled connection cannot be assumed to be in
    // the mode the caller wants; this is where the caller says which one.
    std::string setup_sql;
};

// True when `error` is a conflict that retrying the whole transaction can
// clear. The discriminator is the raw SQL code the MySQL layer placed in
// `error.metadata["sql_code"]`, so TiDB 9007 (optimistic write conflict) stays
// distinguishable from MySQL 1205 (lock wait timeout) even though both map to
// the same `rpc_code`. Covers TiDB 9007/8027 and the pessimistic codes 1205
// (lock wait timeout) / 1213 (deadlock).
[[nodiscard]] CRPC_API bool is_transaction_conflict(const rpc_error& error) noexcept;

// Simple exponential backoff for one retry step, as configured. Exported as a
// building block so tests can observe the policy without sleeping.
[[nodiscard]] inline std::chrono::milliseconds retry_backoff(
    std::size_t attempt, const transaction_retry_options& options) noexcept {
    // `attempt` is the number of the attempt that just failed (1 = first).
    std::chrono::milliseconds delay = options.base_backoff;
    for (std::size_t step = 1; step + 1 <= attempt; ++step) {
        const std::int64_t doubled = static_cast<std::int64_t>(delay.count() *
                                                               options.backoff_multiplier);
        if (doubled >= options.max_backoff.count()) {
            return options.max_backoff;
        }
        delay = std::chrono::milliseconds(doubled);
    }
    if (delay > options.max_backoff) {
        return options.max_backoff;
    }
    return delay;
}

// Runs `work(connection&)` inside one transaction per attempt and commits it.
//
// Each attempt borrows a fresh connection from `connections`, runs
// `options.setup_sql` if any, `begin()`, then `work`, then `commit()`. On any
// failure the lease is released (the pool rolls the open transaction back,
// pool contract P3). When the failure is a transaction conflict (see
// `is_transaction_conflict`) and the attempt budget remains, the wrapper backs
// off and retries the whole unit; any other failure is returned as-is. On
// success `status{}` is returned and the caller can rely on the COMMIT having
// been acknowledged by the server.
//
// The callable is called from the calling thread. A blocking backoff sleep is
// used between attempts, so inside a coroutine the caller should run this on a
// thread that is allowed to block, or supply its own scheduling.
template <typename Work>
[[nodiscard]] status retry_transaction(crpc::db::pool& connections, Work&& work,
                                       const transaction_retry_options& options = {}) {
    std::size_t attempt = 0;
    for (;;) {
        ++attempt;
        result<pool::lease> lease = connections.acquire();
        if (!lease) {
            return lease.error();
        }
        bool failed = false;
        rpc_error failure{};
        if (!options.setup_sql.empty()) {
            const status setup = lease.value()->execute(options.setup_sql);
            if (!setup) {
                failed = true;
                failure = setup.error();
            }
        }
        if (!failed) {
            const status began = lease.value()->begin();
            if (!began) {
                failed = true;
                failure = began.error();
            } else {
                status outcome = work(*lease.value().get());
                if (outcome) {
                    // A failed COMMIT still ends the transaction server-side, so the
                    // lease is returned clean; the pool rolls back nothing extra.
                    outcome = lease.value()->commit();
                }
                if (!outcome) {
                    failed = true;
                    failure = outcome.error();
                }
            }
        }
        // The RAII release rolls an open transaction back (pool contract P3),
        // so a conflicting attempt cannot leak a half-open session into the
        // idle set before the retry borrows again.
        (void)lease.value().release();
        if (!failed) {
            return status{};
        }
        if (attempt < options.max_attempts && is_transaction_conflict(failure)) {
            std::this_thread::sleep_for(retry_backoff(attempt, options));
            continue;
        }
        return status{failure};
    }
}

} // namespace tidb
} // namespace db
} // namespace crpc

#endif // CRPC_DB_TIDB_RETRY_HPP