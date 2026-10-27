// CRPC -- database pool contract tests.
//
// The pool is the one part of the database layer that can be fully verified
// without a server, so it is verified here rather than left to an integration
// test: a fake connection counts opens, pings, rollbacks and closes, and each case
// pins one rule of the contract in `connection.hpp` (P1-P15).
//
// The cases are deterministic: no sleeps for correctness (only a bounded wait to
// observe a timeout), no reliance on scheduling order.
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/db/connection.hpp>
#include <crpc/db/types.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace {

using crpc::db::connection;
using crpc::db::connection_factory;
using crpc::db::parameter;
using crpc::db::pool;
using crpc::db::pool_options;
using crpc::db::result_set;

// Counters live in a shared block, not in the connection object: the pool owns
// and deletes connections, so a test that reads a counter after the pool closed
// the connection must not touch the connection. The factory keeps the blocks.
struct conn_stats {
    int id = 0;
    int pings = 0;
    int rollbacks = 0;
    int closes = 0;
    int queries = 0;
    std::size_t bound = 0;
    std::string last_sql;
    bool in_tx = false;
    bool open = true;
    bool fail_ping = false;
    bool fail_rollback = false;
    bool result_closed = false;
};

// A cursor over no rows: enough for `execute()` and for a case that only needs a
// cursor to be owned and closed.
class empty_result_set final : public result_set {
public:
    explicit empty_result_set(std::shared_ptr<conn_stats> stats) : stats_(std::move(stats)) {}
    ~empty_result_set() override { close(); }

    std::size_t column_count() const noexcept override { return 0; }
    crpc::db::column_info column(std::size_t) const override { return {}; }
    bool next() override { return false; }
    bool is_null(std::size_t) const noexcept override { return true; }
    crpc::result<std::int64_t> get_int64(std::size_t) override {
        return crpc::rpc_error{crpc::rpc_code::invalid_argument, "no columns"};
    }
    crpc::result<std::uint64_t> get_uint64(std::size_t) override {
        return crpc::rpc_error{crpc::rpc_code::invalid_argument, "no columns"};
    }
    crpc::result<double> get_double(std::size_t) override {
        return crpc::rpc_error{crpc::rpc_code::invalid_argument, "no columns"};
    }
    crpc::result<bool> get_bool(std::size_t) override {
        return crpc::rpc_error{crpc::rpc_code::invalid_argument, "no columns"};
    }
    crpc::result<std::string_view> get_text(std::size_t) override {
        return crpc::rpc_error{crpc::rpc_code::invalid_argument, "no columns"};
    }
    crpc::result<std::vector<std::byte>> get_blob(std::size_t) override {
        return crpc::rpc_error{crpc::rpc_code::invalid_argument, "no columns"};
    }
    std::int64_t affected_rows() const noexcept override { return 0; }
    void close() noexcept override { stats_->result_closed = true; }

private:
    std::shared_ptr<conn_stats> stats_;
};

// One fake connection. Every observable action is counted so a case can assert
// exactly what the pool did to it.
class fake_connection final : public connection {
public:
    explicit fake_connection(std::shared_ptr<conn_stats> stats)
        : stats_(std::move(stats)) {}

    crpc::status ping() override {
        ++stats_->pings;
        if (stats_->fail_ping) {
            // The server dropped the link. `open` stays true: the connection has
            // not been closed client-side yet, which is exactly what the pool has
            // to discover and fix.
            return crpc::status{crpc::rpc_code::connection_reset};
        }
        return crpc::status{};
    }

    bool in_transaction() const noexcept override { return stats_->in_tx; }

    crpc::status begin() override {
        if (!stats_->open) {
            return crpc::status{crpc::rpc_code::shutting_down};
        }
        stats_->in_tx = true;
        return crpc::status{};
    }

    crpc::status commit() override {
        if (!stats_->in_tx) {
            return crpc::status{crpc::rpc_code::failed_precondition};
        }
        stats_->in_tx = false;
        return crpc::status{};
    }

    crpc::status rollback() override {
        ++stats_->rollbacks;
        if (stats_->fail_rollback) {
            return crpc::status{crpc::rpc_code::connection_reset};
        }
        stats_->in_tx = false;
        return crpc::status{};
    }

    crpc::status query(std::string_view sql, crpc::span<const parameter> params,
                       result_set** out) override {
        ++stats_->queries;
        stats_->last_sql.assign(sql.data(), sql.size());
        stats_->bound = params.size();
        if (!stats_->open) {
            return crpc::status{crpc::rpc_code::shutting_down};
        }
        if (out != nullptr) {
            *out = new empty_result_set(stats_);
        }
        return crpc::status{};
    }

    void close() noexcept override {
        if (stats_->open) {
            stats_->open = false;
            ++stats_->closes;
        }
    }

    bool is_open() const noexcept override { return stats_->open; }

private:
    std::shared_ptr<conn_stats> stats_;
};

// Opens connections and keeps only their counter blocks, so the pool is the sole
// owner of the connection objects. It never deletes them.
class fake_factory final : public connection_factory {
public:
    crpc::result<connection*> open() override {
        ++opens;
        if (fail_open) {
            return crpc::rpc_error{crpc::rpc_code::connection_refused, "fake server is down"};
        }
        auto stats = std::make_shared<conn_stats>();
        stats->id = next_id_++;
        stats_.push_back(stats);
        return static_cast<connection*>(new fake_connection(stats));
    }

    std::string_view name() const noexcept override { return "fake"; }

    [[nodiscard]] conn_stats& stats(std::size_t index) { return *stats_.at(index); }
    [[nodiscard]] std::size_t opened_connections() const noexcept { return stats_.size(); }

    [[nodiscard]] int total_pings() const {
        int total = 0;
        for (const auto& item : stats_) {
            total += item->pings;
        }
        return total;
    }

    [[nodiscard]] int total_closes() const {
        int total = 0;
        for (const auto& item : stats_) {
            total += item->closes;
        }
        return total;
    }

    std::vector<std::shared_ptr<conn_stats>> stats_;
    int opens = 0;
    bool fail_open = false;

private:
    int next_id_ = 0;
};

[[nodiscard]] pool_options small_pool() {
    pool_options options;
    options.min_size = 0;
    options.max_size = 2;
    options.max_idle = 2;
    options.acquire_timeout = std::chrono::milliseconds(120);
    options.idle_timeout = std::chrono::seconds(60);
    options.max_lifetime = std::chrono::minutes(30);
    options.validate_on_borrow = true;
    return options;
}
} // namespace

CRPC_TEST(pool_acquire_opens_one_connection) {
    fake_factory factory;
    pool connections(small_pool(), factory);
    CRPC_CHECK_EQ(std::size_t{0}, connections.total());

    auto lease = connections.acquire();
    CRPC_CHECK(lease.ok());
    if (!lease.ok()) {
        return;
    }
    CRPC_CHECK(lease.value().valid());
    CRPC_CHECK_EQ(1, factory.opens);
    CRPC_CHECK_EQ(std::size_t{1}, connections.in_use());
    CRPC_CHECK_EQ(std::size_t{0}, connections.idle());
    CRPC_CHECK_EQ(std::size_t{1}, connections.total());
}

CRPC_TEST(pool_release_returns_and_reuses) {
    fake_factory factory;
    pool connections(small_pool(), factory);
    {
        auto lease = connections.acquire();
        CRPC_CHECK(lease.ok());
        if (!lease.ok()) {
            return;
        }
        CRPC_CHECK(lease.value().release().ok());
        // P2: releasing again is a no-op, not a second insert.
        CRPC_CHECK(lease.value().release().ok());
    }
    CRPC_CHECK_EQ(std::size_t{1}, connections.idle());
    CRPC_CHECK_EQ(std::size_t{0}, connections.in_use());
    CRPC_CHECK_EQ(std::size_t{1}, connections.total());

    // The next acquire reuses the idle connection instead of opening a new one.
    auto second = connections.acquire();
    CRPC_CHECK(second.ok());
    CRPC_CHECK_EQ(1, factory.opens);
    CRPC_CHECK_EQ(std::size_t{1}, connections.in_use());
}

CRPC_TEST(pool_lease_destructor_returns) {
    fake_factory factory;
    pool connections(small_pool(), factory);
    {
        auto lease = connections.acquire();
        CRPC_CHECK(lease.ok());
        if (!lease.ok()) {
            return;
        }
        CRPC_CHECK_EQ(std::size_t{1}, connections.in_use());
    }
    // P1: the scope ending is the return, so an early return cannot leak.
    CRPC_CHECK_EQ(std::size_t{0}, connections.in_use());
    CRPC_CHECK_EQ(std::size_t{1}, connections.idle());
}

CRPC_TEST(pool_moved_lease_returns_once) {
    fake_factory factory;
    pool connections(small_pool(), factory);
    auto lease = connections.acquire();
    CRPC_CHECK(lease.ok());
    if (!lease.ok()) {
        return;
    }
    crpc::db::pool::lease moved = std::move(lease.value());
    CRPC_CHECK(moved.valid());
    CRPC_CHECK_FALSE(lease.value().valid());
    CRPC_CHECK_EQ(std::size_t{1}, connections.in_use());

    // Moving into another lease must not double-return.
    crpc::db::pool::lease other;
    other = std::move(moved);
    CRPC_CHECK(other.valid());
    CRPC_CHECK_FALSE(moved.valid());
    (void)other.release();
    CRPC_CHECK_EQ(std::size_t{0}, connections.in_use());
    CRPC_CHECK_EQ(std::size_t{1}, connections.idle());
    CRPC_CHECK_EQ(std::size_t{1}, connections.total());
}

CRPC_TEST(pool_validates_on_borrow) {
    fake_factory factory;
    pool connections(small_pool(), factory);
    {
        // A fresh connection is not pinged: it was just opened.
        auto lease = connections.acquire();
        CRPC_CHECK(lease.ok());
    }
    CRPC_CHECK_EQ(0, factory.total_pings());

    {
        // Borrowing an idle connection is validated, because the server may have
        // dropped it while it sat in the pool.
        auto lease = connections.acquire();
        CRPC_CHECK(lease.ok());
    }
    CRPC_CHECK_EQ(1, factory.total_pings());

    // A connection the server dropped while idle is discarded, not handed out.
    CRPC_CHECK_EQ(std::size_t{1}, connections.idle());
    factory.stats(0).fail_ping = true;

    auto third = connections.acquire();
    CRPC_CHECK(third.ok());
    if (!third.ok()) {
        return;
    }
    // The dead one was closed and a fresh connection was opened.
    CRPC_CHECK_EQ(2, factory.opens);
    CRPC_CHECK_EQ(1, factory.stats(0).closes);
    CRPC_CHECK_EQ(std::size_t{1}, connections.total());
    CRPC_CHECK_EQ(1, factory.stats(1).id);
}

CRPC_TEST(pool_validation_can_be_disabled) {
    pool_options options = small_pool();
    options.validate_on_borrow = false;
    fake_factory factory;
    pool connections(options, factory);
    {
        auto lease = connections.acquire();
        CRPC_CHECK(lease.ok());
    }
    CRPC_CHECK_EQ(0, factory.total_pings());
    auto second = connections.acquire();
    CRPC_CHECK(second.ok());
    CRPC_CHECK_EQ(1, factory.opens);
}

CRPC_TEST(pool_rolls_back_a_leaked_transaction) {
    fake_factory factory;
    pool connections(small_pool(), factory);
    {
        auto lease = connections.acquire();
        CRPC_CHECK(lease.ok());
        if (!lease.ok()) {
            return;
        }
        CRPC_CHECK(lease.value()->begin().ok());
        CRPC_CHECK(lease.value()->in_transaction());
        // Released while still in a transaction: the pool must roll it back before
        // the connection can be reused (P3).
    }
    CRPC_CHECK_EQ(1, factory.stats(0).rollbacks);
    CRPC_CHECK_FALSE(factory.stats(0).in_tx);
    CRPC_CHECK_EQ(std::size_t{1}, connections.idle());

    auto second = connections.acquire();
    CRPC_CHECK(second.ok());
    if (second.ok()) {
        CRPC_CHECK_FALSE(second.value()->in_transaction());
    }
    CRPC_CHECK_EQ(1, factory.opens);
}

CRPC_TEST(pool_closes_a_connection_whose_rollback_failed) {
    fake_factory factory;
    pool connections(small_pool(), factory);
    {
        auto lease = connections.acquire();
        CRPC_CHECK(lease.ok());
        if (!lease.ok()) {
            return;
        }
        CRPC_CHECK(lease.value()->begin().ok());
        factory.stats(0).fail_rollback = true;
    }
    // The transaction could not be rolled back, so the connection must not be
    // reused: it is closed instead.
    CRPC_CHECK_EQ(1, factory.stats(0).closes);
    CRPC_CHECK_EQ(std::size_t{0}, connections.idle());
    CRPC_CHECK_EQ(std::size_t{0}, connections.total());
}

CRPC_TEST(pool_discard_closes_and_frees_the_slot) {
    fake_factory factory;
    pool connections(small_pool(), factory);
    {
        auto lease = connections.acquire();
        CRPC_CHECK(lease.ok());
        if (!lease.ok()) {
            return;
        }
        // P14: the caller knows the protocol is broken and says so.
        lease.value().discard();
        CRPC_CHECK_FALSE(lease.value().valid());
        // Idempotent.
        lease.value().discard();
        CRPC_CHECK(lease.value().release().ok());
    }
    CRPC_CHECK_EQ(1, factory.stats(0).closes);
    CRPC_CHECK_EQ(std::size_t{0}, connections.idle());
    CRPC_CHECK_EQ(std::size_t{0}, connections.in_use());
    CRPC_CHECK_EQ(std::size_t{0}, connections.total());

    // The slot was freed, so a new acquire opens a replacement rather than failing.
    auto second = connections.acquire();
    CRPC_CHECK(second.ok());
    CRPC_CHECK_EQ(2, factory.opens);
}

CRPC_TEST(pool_close_rejects_and_closes_idle) {
    fake_factory factory;
    pool connections(small_pool(), factory);
    {
        auto lease = connections.acquire();
        CRPC_CHECK(lease.ok());
    }
    CRPC_CHECK_EQ(std::size_t{1}, connections.idle());

    connections.close();
    CRPC_CHECK(connections.is_closed());
    // P4: idle connections are closed now.
    CRPC_CHECK_EQ(1, factory.stats(0).closes);
    CRPC_CHECK_EQ(std::size_t{0}, connections.idle());
    CRPC_CHECK_EQ(std::size_t{0}, connections.total());

    // P15: closing again is a no-op.
    connections.close();
    CRPC_CHECK_EQ(1, factory.stats(0).closes);

    auto lease = connections.acquire();
    CRPC_CHECK_FALSE(lease.ok());
    if (!lease.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::shutting_down),
                      static_cast<int>(lease.error().code));
    }
}

CRPC_TEST(pool_close_with_lease_outstanding) {
    fake_factory factory;
    pool connections(small_pool(), factory);
    auto lease = connections.acquire();
    CRPC_CHECK(lease.ok());
    if (!lease.ok()) {
        return;
    }
    connections.close();
    CRPC_CHECK_EQ(0, factory.stats(0).closes);

    // P4: the lease is closed on release, not returned to a closed pool.
    (void)lease.value().release();
    CRPC_CHECK_EQ(1, factory.stats(0).closes);
    CRPC_CHECK_EQ(std::size_t{0}, connections.idle());
    CRPC_CHECK_EQ(std::size_t{0}, connections.total());
}

CRPC_TEST(pool_lease_outliving_the_pool_is_safe) {
    fake_factory factory;
    crpc::db::pool::lease escaped;
    {
        pool connections(small_pool(), factory);
        auto lease = connections.acquire();
        CRPC_CHECK(lease.ok());
        if (!lease.ok()) {
            return;
        }
        escaped = std::move(lease.value());
    }
    // P10: the pool is gone; the lease still owns its connection and must close it
    // rather than touch freed pool state.
    CRPC_CHECK(escaped.valid());
    CRPC_CHECK(escaped.release().ok());
    CRPC_CHECK_EQ(1, factory.stats(0).closes);
    CRPC_CHECK_FALSE(escaped.valid());
}

CRPC_TEST(pool_is_bounded_and_times_out) {
    pool_options options = small_pool();
    options.max_size = 1;
    options.acquire_timeout = std::chrono::milliseconds(80);
    fake_factory factory;
    pool connections(options, factory);

    auto first = connections.acquire();
    CRPC_CHECK(first.ok());
    if (!first.ok()) {
        return;
    }

    // P5: at the ceiling the acquire waits, then reports a retryable resource
    // exhaustion rather than blocking forever or overshooting max_size.
    const auto started = std::chrono::steady_clock::now();
    auto second = connections.acquire();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CRPC_CHECK_FALSE(second.ok());
    if (!second.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::resource_exhausted),
                      static_cast<int>(second.error().code));
        CRPC_CHECK(crpc::is_retryable(second.error().code));
    }
    CRPC_CHECK(elapsed >= std::chrono::milliseconds(60));
    CRPC_CHECK_EQ(1, factory.opens);
    CRPC_CHECK_EQ(std::size_t{1}, connections.total());
}

CRPC_TEST(pool_waiter_is_woken_by_a_release) {
    pool_options options = small_pool();
    options.max_size = 1;
    options.acquire_timeout = std::chrono::seconds(5);
    fake_factory factory;
    pool connections(options, factory);

    auto first = connections.acquire();
    CRPC_CHECK(first.ok());
    if (!first.ok()) {
        return;
    }

    std::atomic<bool> acquired{false};
    std::atomic<int> code{0};
    std::thread waiter([&] {
        auto lease = connections.acquire();
        acquired.store(lease.ok(), std::memory_order_release);
        if (!lease.ok()) {
            code.store(static_cast<int>(lease.error().code), std::memory_order_release);
        }
    });

    // Give the waiter a moment to block, then release: the waiter must proceed.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    (void)first.value().release();
    waiter.join();

    CRPC_CHECK(acquired.load(std::memory_order_acquire));
    CRPC_CHECK_EQ(1, factory.opens);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::ok), code.load(std::memory_order_acquire));
}

CRPC_TEST(pool_waiter_fails_when_the_pool_closes) {
    pool_options options = small_pool();
    options.max_size = 1;
    options.acquire_timeout = std::chrono::seconds(5);
    fake_factory factory;
    pool connections(options, factory);

    auto first = connections.acquire();
    CRPC_CHECK(first.ok());
    if (!first.ok()) {
        return;
    }

    std::atomic<int> code{0};
    std::thread waiter([&] {
        auto lease = connections.acquire();
        if (!lease.ok()) {
            code.store(static_cast<int>(lease.error().code), std::memory_order_release);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    connections.close();
    waiter.join();

    // A waiter must not be left blocked by `close()`.
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::shutting_down),
                  code.load(std::memory_order_acquire));
    (void)first.value().release();
}

CRPC_TEST(pool_open_failure_is_reported) {
    fake_factory factory;
    factory.fail_open = true;
    pool connections(small_pool(), factory);

    auto lease = connections.acquire();
    CRPC_CHECK_FALSE(lease.ok());
    if (!lease.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::connection_refused),
                      static_cast<int>(lease.error().code));
    }
    // The reserved slot was released, so a later attempt can still try.
    CRPC_CHECK_EQ(std::size_t{0}, connections.total());

    factory.fail_open = false;
    auto retry = connections.acquire();
    CRPC_CHECK(retry.ok());
    CRPC_CHECK_EQ(2, factory.opens);
}

CRPC_TEST(pool_max_idle_closes_the_excess) {
    pool_options options = small_pool();
    options.max_size = 3;
    options.max_idle = 1;
    fake_factory factory;
    pool connections(options, factory);

    auto first = connections.acquire();
    auto second = connections.acquire();
    CRPC_CHECK(first.ok());
    CRPC_CHECK(second.ok());
    if (!first.ok() || !second.ok()) {
        return;
    }
    (void)first.value().release();
    CRPC_CHECK_EQ(std::size_t{1}, connections.idle());
    // P8: the second release exceeds max_idle, so that connection is closed.
    (void)second.value().release();
    CRPC_CHECK_EQ(std::size_t{1}, connections.idle());
    CRPC_CHECK_EQ(1, factory.total_closes());
    CRPC_CHECK_EQ(std::size_t{1}, connections.total());
}

CRPC_TEST(pool_idle_timeout_discards_on_acquire) {
    pool_options options = small_pool();
    options.idle_timeout = std::chrono::milliseconds(20);
    fake_factory factory;
    pool connections(options, factory);
    {
        auto lease = connections.acquire();
        CRPC_CHECK(lease.ok());
    }
    CRPC_CHECK_EQ(std::size_t{1}, connections.idle());

    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    auto second = connections.acquire();
    CRPC_CHECK(second.ok());
    // P7: the expired connection was closed and a new one opened.
    CRPC_CHECK_EQ(1, factory.stats(0).closes);
    CRPC_CHECK_EQ(2, factory.opens);
}

CRPC_TEST(pool_max_lifetime_discards_on_release) {
    pool_options options = small_pool();
    options.max_lifetime = std::chrono::milliseconds(20);
    fake_factory factory;
    pool connections(options, factory);
    auto lease = connections.acquire();
    CRPC_CHECK(lease.ok());
    if (!lease.ok()) {
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    (void)lease.value().release();
    // The connection is older than the lifetime cap, so it is closed instead of
    // returned (P7).
    CRPC_CHECK_EQ(1, factory.stats(0).closes);
    CRPC_CHECK_EQ(std::size_t{0}, connections.idle());
    CRPC_CHECK_EQ(std::size_t{0}, connections.total());
}

CRPC_TEST(pool_warmup_opens_min_size) {
    pool_options options = small_pool();
    options.min_size = 2;
    options.max_size = 3;
    fake_factory factory;
    pool connections(options, factory);

    connections.warmup();
    CRPC_CHECK_EQ(2, factory.opens);
    CRPC_CHECK_EQ(std::size_t{2}, connections.idle());
    CRPC_CHECK_EQ(std::size_t{2}, connections.total());

    // Warming twice does not open more.
    connections.warmup();
    CRPC_CHECK_EQ(2, factory.opens);
}

CRPC_TEST(pool_warmup_stops_on_failure) {
    pool_options options = small_pool();
    options.min_size = 3;
    options.max_size = 3;
    fake_factory factory;
    factory.fail_open = true;
    pool connections(options, factory);

    connections.warmup();
    // An unreachable endpoint cannot be warmed; the attempt is not retained.
    CRPC_CHECK_EQ(std::size_t{0}, connections.total());
    CRPC_CHECK_EQ(std::size_t{0}, connections.idle());
}

CRPC_TEST(pool_statistics_track_opens) {
    fake_factory factory;
    pool connections(small_pool(), factory);
    CRPC_CHECK_EQ(std::uint64_t{0}, connections.opened());

    {
        auto first = connections.acquire();
        auto second = connections.acquire();
        CRPC_CHECK(first.ok());
        CRPC_CHECK(second.ok());
        CRPC_CHECK_EQ(std::uint64_t{2}, connections.opened());
        CRPC_CHECK_EQ(std::size_t{2}, connections.in_use());
    }
    CRPC_CHECK_EQ(std::uint64_t{2}, connections.opened());
    CRPC_CHECK_EQ(std::size_t{2}, connections.idle());
}

CRPC_TEST(pool_two_pools_do_not_share_connections) {
    fake_factory factory;
    pool first_pool(small_pool(), factory);
    pool second_pool(small_pool(), factory);

    auto first = first_pool.acquire();
    auto second = second_pool.acquire();
    CRPC_CHECK(first.ok());
    CRPC_CHECK(second.ok());
    if (!first.ok() || !second.ok()) {
        return;
    }
    // P11: two pools over one factory still own their own connections.
    CRPC_CHECK(first.value().get() != second.value().get());
    CRPC_CHECK_EQ(2, factory.opens);
    CRPC_CHECK_EQ(std::size_t{1}, first_pool.in_use());
    CRPC_CHECK_EQ(std::size_t{1}, second_pool.in_use());
}

// --- The abstraction itself --------------------------------------------------

CRPC_TEST(connection_execute_owns_and_closes_its_cursor) {
    fake_factory factory;
    pool connections(small_pool(), factory);
    auto lease = connections.acquire();
    CRPC_CHECK(lease.ok());
    if (!lease.ok()) {
        return;
    }
    CRPC_CHECK(lease.value()->execute("UPDATE t SET a = 1", {}).ok());
    CRPC_CHECK_EQ(1, factory.stats(0).queries);
    // `execute` must not leak the cursor it was handed.
    CRPC_CHECK(factory.stats(0).result_closed);
}

CRPC_TEST(connection_query_returns_a_guarded_cursor) {
    fake_factory factory;
    pool connections(small_pool(), factory);
    auto lease = connections.acquire();
    CRPC_CHECK(lease.ok());
    if (!lease.ok()) {
        return;
    }

    result_set* raw = nullptr;
    const std::vector<parameter> params{parameter::from_int64(7)};
    CRPC_CHECK(lease.value()
                   ->query("SELECT * FROM t WHERE id = ?",
                           crpc::span<const parameter>(params.data(), params.size()), &raw)
                   .ok());
    CRPC_CHECK(raw != nullptr);
    CRPC_CHECK_EQ(std::size_t{1}, factory.stats(0).bound);
    CRPC_CHECK_EQ(std::string("SELECT * FROM t WHERE id = ?"), factory.stats(0).last_sql);

    {
        crpc::db::statement_guard guard(raw);
        CRPC_CHECK(guard.valid());
        CRPC_CHECK_EQ(std::size_t{0}, guard->column_count());
        CRPC_CHECK_FALSE(guard->next());
    }
    CRPC_CHECK(factory.stats(0).result_closed);
}

CRPC_TEST(db_value_keeps_null_distinct_from_empty) {
    const crpc::db::db_value null_value = crpc::db::db_value::make_null();
    const crpc::db::db_value empty_text = crpc::db::db_value::from_text("");
    const crpc::db::db_value empty_blob = crpc::db::db_value::from_blob({});

    CRPC_CHECK(null_value.is_null());
    CRPC_CHECK_FALSE(empty_text.is_null());
    CRPC_CHECK_FALSE(empty_blob.is_null());
    CRPC_CHECK(empty_text.as_text().empty());
    CRPC_CHECK(empty_blob.as_blob().empty());

    // A null reports no type, which is how a caller tells "no value" from
    // "empty value of a known type".
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::unknown),
                  static_cast<int>(null_value.type()));
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::text),
                  static_cast<int>(empty_text.type()));
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::blob),
                  static_cast<int>(empty_blob.type()));
}

CRPC_TEST(db_value_keeps_the_full_unsigned_range) {
    // A BIGINT UNSIGNED value above INT64_MAX must survive without a cast.
    const std::uint64_t big = 18446744073709551615ull;
    const crpc::db::db_value value = crpc::db::db_value::from_uint64(big);
    CRPC_CHECK_EQ(big, value.as_uint64());
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::uint64),
                  static_cast<int>(value.type()));
}

CRPC_TEST(db_type_names_are_stable) {
    CRPC_CHECK_EQ(std::string("int64"),
                  std::string(crpc::db::to_string(crpc::db::db_type::int64)));
    CRPC_CHECK_EQ(std::string("blob"),
                  std::string(crpc::db::to_string(crpc::db::db_type::blob)));
    CRPC_CHECK_EQ(std::string("unknown"),
                  std::string(crpc::db::to_string(crpc::db::db_type::unknown)));
    // A decimal is carried as text, which `is_textual` must agree with.
    CRPC_CHECK(crpc::db::is_textual(crpc::db::db_type::decimal));
    CRPC_CHECK_FALSE(crpc::db::is_textual(crpc::db::db_type::blob));
    CRPC_CHECK_FALSE(crpc::db::is_textual(crpc::db::db_type::int64));
}

CRPC_TEST(parameter_null_binds_null) {
    const crpc::db::parameter null_param = crpc::db::parameter::null();
    CRPC_CHECK(null_param.is_null);

    const crpc::db::parameter text_param = crpc::db::parameter::from_text("x");
    CRPC_CHECK_FALSE(text_param.is_null);
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::text),
                  static_cast<int>(text_param.type));
}

CRPC_TEST_MAIN()