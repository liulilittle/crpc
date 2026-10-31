// CRPC -- database pool implementation.
#include <crpc/db/connection.hpp>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

namespace crpc {
namespace db {

const char* to_string(db_type type) noexcept {
    switch (type) {
    case db_type::unknown:
        return "unknown";
    case db_type::boolean:
        return "boolean";
    case db_type::int64:
        return "int64";
    case db_type::uint64:
        return "uint64";
    case db_type::real:
        return "real";
    case db_type::text:
        return "text";
    case db_type::blob:
        return "blob";
    case db_type::decimal:
        return "decimal";
    case db_type::date:
        return "date";
    case db_type::time:
        return "time";
    case db_type::timestamp:
        return "timestamp";
    case db_type::uuid:
        return "uuid";
    case db_type::json:
        return "json";
    case db_type::xml:
        return "xml";
    }
    return "unknown";
}

// --- Interfaces --------------------------------------------------------------

result_set::~result_set() = default;

connection::~connection() = default;

status connection::execute(std::string_view sql, span<const parameter> params) {
    result_set* raw = nullptr;
    const status ran = query(sql, params, &raw);
    if (!ran) {
        return ran;
    }
    // The cursor is not needed, but it is owned: closing and deleting it here is
    // what keeps `execute` from leaking on a DML that returns rows.
    if (raw != nullptr) {
        raw->close();
        delete raw;
    }
    return status{};
}

connection_factory::~connection_factory() = default;

// --- pool --------------------------------------------------------------------

// The state a lease must outlive the pool for (P10). It is shared between the
// pool and every outstanding lease, so a lease released after the pool is gone
// still closes its connection instead of touching freed memory.
struct pool::impl {
    struct entry {
        connection* conn = nullptr;
        std::uint64_t id = 0;
        // When the connection became idle, for `idle_timeout`.
        std::chrono::steady_clock::time_point idle_since{};
        // When the connection was opened, for `max_lifetime`.
        std::chrono::steady_clock::time_point created{};
    };

    pool_options options;
    connection_factory& factory;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<entry> idle;
    std::size_t in_use = 0;
    std::size_t live = 0;
    std::uint64_t next_id = 0;
    std::uint64_t opened = 0;
    bool closed = false;

    impl(pool_options opts, connection_factory& owner) : options(opts), factory(owner) {}

    ~impl() {
        for (entry& item : idle) {
            item.conn->close();
            delete item.conn;
        }
        idle.clear();
    }

    // True when the connection must be closed instead of reused: it sat idle too
    // long, or it is older than the lifetime cap. Both bounds exist because they
    // fail differently: an idle timeout sheds capacity, a lifetime cap recycles a
    // connection that may have accumulated server-side state or a stale plan.
    [[nodiscard]] bool expired(const entry& item,
                               std::chrono::steady_clock::time_point now) const {
        if (options.idle_timeout.count() > 0 && now - item.idle_since >= options.idle_timeout) {
            return true;
        }
        if (options.max_lifetime.count() > 0 && now - item.created >= options.max_lifetime) {
            return true;
        }
        return false;
    }

    // Closes and forgets every idle connection. Called with the mutex held.
    void close_idle_locked() {
        for (entry& item : idle) {
            item.conn->close();
            delete item.conn;
            if (live > 0) {
                --live;
            }
        }
        idle.clear();
    }
};

namespace {

// Returns true when the connection may be reused at all.
[[nodiscard]] bool prepare_for_reuse(connection& conn) noexcept {
    if (!conn.is_open()) {
        return false;
    }
    // A connection handed back mid-transaction would leak its locks into whichever
    // caller borrows it next.
    if (conn.in_transaction() && !conn.rollback().ok()) {
        return false;
    }
    // Rolling back ends the transaction, not the session state that produced it:
    // an `autocommit=0` session would make the next borrower's statements join an
    // implicit transaction nobody commits, and the next release would roll them
    // back. Restore the session before the connection becomes reusable.
    return conn.reset_session().ok();
}

} // namespace

pool::lease::lease(std::shared_ptr<void> state, connection* conn,
                   std::chrono::steady_clock::time_point created) noexcept
    : state_(std::move(state)), connection_(conn), created_(created) {}

pool::lease::~lease() { (void)release(); }

pool::lease::lease(lease&& other) noexcept
    : state_(std::move(other.state_)),
      connection_(other.connection_),
      created_(other.created_) {
    other.connection_ = nullptr;
}

pool::lease& pool::lease::operator=(lease&& other) noexcept {
    if (this != &other) {
        (void)release();
        state_ = std::move(other.state_);
        connection_ = other.connection_;
        created_ = other.created_;
        other.connection_ = nullptr;
    }
    return *this;
}

status pool::lease::release() noexcept {
    if (connection_ == nullptr) {
        // P2: a second release is a no-op, not a second insert.
        return status{};
    }
    connection* conn = connection_;
    connection_ = nullptr;

    auto* state = static_cast<impl*>(state_.get());
    if (!state) {
        // The pool is gone; there is nothing to return to, so the connection is
        // closed rather than leaked (P10).
        conn->close();
        delete conn;
        state_.reset();
        return status{};
    }

    const bool reusable = prepare_for_reuse(*conn);
    const auto now = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->in_use > 0) {
            --state->in_use;
        }
        // P3/P4/P7/P8: a connection that is not reusable, or that the pool must not
        // keep, is closed here rather than at a later sweep. Closing under the lock
        // is safe because `close()` is documented as non-blocking and idempotent;
        // the alternative (a deferred close queue) would need its own ownership
        // rules for no gain, since this path is already serialised.
        const bool too_old = state->options.max_lifetime.count() > 0 &&
                             now - created_ >= state->options.max_lifetime;
        if (!reusable || too_old || state->closed ||
            state->idle.size() >= state->options.max_idle) {
            conn->close();
            delete conn;
            if (state->live > 0) {
                --state->live;
            }
        } else {
            impl::entry item;
            item.conn = conn;
            item.idle_since = now;
            // The creation stamp travels with the lease across borrow cycles, so
            // `max_lifetime` is measured from open, not from the last release.
            item.created = created_;
            state->idle.push_back(item);
        }
        state->cv.notify_one();
    }
    // The lease may hold the last reference to the state (a lease that outlived its
    // pool). Releasing it must happen after the lock guard is gone, or the state
    // would be destroyed while its own mutex is still locked.
    state_.reset();
    return status{};
}

void pool::lease::discard() noexcept {
    if (connection_ == nullptr) {
        return;
    }
    connection* conn = connection_;
    connection_ = nullptr;

    auto* state = static_cast<impl*>(state_.get());
    if (state) {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->in_use > 0) {
            --state->in_use;
        }
        if (state->live > 0) {
            --state->live;
        }
        if (!state->closed) {
            state->cv.notify_one();
        }
    }
    conn->close();
    delete conn;
    state_.reset();
}

pool::pool(pool_options options, connection_factory& factory)
    : impl_(std::make_shared<impl>(options, factory)) {}

pool::~pool() { close(); }

result<pool::lease> pool::acquire() {
    std::unique_lock<std::mutex> lock(impl_->mutex);

    if (impl_->closed) {
        return rpc_error{rpc_code::shutting_down, "database pool is closed"};
    }

    const auto deadline = std::chrono::steady_clock::now() + impl_->options.acquire_timeout;

    for (;;) {
        if (impl_->closed) {
            return rpc_error{rpc_code::shutting_down, "database pool is closed"};
        }

        // Take the newest idle connection that survives validation. Validation is a
        // round trip, so it happens without the lock held.
        if (!impl_->idle.empty()) {
            impl::entry item = impl_->idle.back();
            impl_->idle.pop_back();

            // P7/P8: an expired connection is closed, never handed out.
            if (impl_->expired(item, std::chrono::steady_clock::now())) {
                item.conn->close();
                delete item.conn;
                if (impl_->live > 0) {
                    --impl_->live;
                }
                continue;
            }

            if (impl_->options.validate_on_borrow) {
                // A closed connection has nothing to validate.
                if (!item.conn->is_open()) {
                    item.conn->close();
                    delete item.conn;
                    if (impl_->live > 0) {
                        --impl_->live;
                    }
                    continue;
                }
                lock.unlock();
                const status pinged = item.conn->ping();
                lock.lock();
                if (!pinged) {
                    item.conn->close();
                    delete item.conn;
                    if (impl_->live > 0) {
                        --impl_->live;
                    }
                    // The mutex was released for the ping; the pool may have closed.
                    continue;
                }
            }
            ++impl_->in_use;
            return lease(impl_, item.conn, item.created);
        }

        // No idle connection: open a new one while below the ceiling. The open is
        // a blocking round trip, so it happens without the lock held.
        if (impl_->live < impl_->options.max_size) {
            // Reserve the slot so a concurrent acquire cannot overshoot max_size.
            ++impl_->live;
            ++impl_->opened;
            const std::uint64_t id = ++impl_->next_id;
            lock.unlock();
            result<connection*> opened = impl_->factory.open();
            lock.lock();

            if (!opened) {
                if (impl_->live > 0) {
                    --impl_->live;
                }
                if (impl_->closed) {
                    return rpc_error{rpc_code::shutting_down, "database pool is closed"};
                }
                return opened.error();
            }
            if (impl_->closed) {
                // The pool closed while the connection was being opened.
                opened.value()->close();
                delete opened.value();
                if (impl_->live > 0) {
                    --impl_->live;
                }
                return rpc_error{rpc_code::shutting_down, "database pool is closed"};
            }
            (void)id;
            ++impl_->in_use;
            return lease(impl_, opened.value(), std::chrono::steady_clock::now());
        }

        // At the ceiling: wait for a release.
        if (std::chrono::steady_clock::now() >= deadline) {
            return rpc_error{rpc_code::resource_exhausted,
                             "database pool is exhausted and the acquire timed out"};
        }
        if (impl_->cv.wait_until(lock, deadline) == std::cv_status::timeout) {
            return rpc_error{rpc_code::resource_exhausted,
                             "database pool is exhausted and the acquire timed out"};
        }
    }
}

void pool::warmup() {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    while (!impl_->closed && impl_->live < impl_->options.min_size) {
        ++impl_->live;
        ++impl_->opened;
        ++impl_->next_id;
        lock.unlock();
        result<connection*> opened = impl_->factory.open();
        lock.lock();
        if (!opened) {
            if (impl_->live > 0) {
                --impl_->live;
            }
            // An unreachable endpoint cannot be warmed; leave the rest to acquire.
            return;
        }
        if (impl_->closed || impl_->idle.size() >= impl_->options.max_idle) {
            opened.value()->close();
            delete opened.value();
            if (impl_->live > 0) {
                --impl_->live;
            }
            return;
        }
        impl::entry item;
        item.conn = opened.value();
        item.idle_since = std::chrono::steady_clock::now();
        impl_->idle.push_back(item);
    }
}

void pool::close() noexcept {
    if (!impl_) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->closed) {
        return;
    }
    impl_->closed = true;
    impl_->close_idle_locked();
    // Waiters observe `closed` and fail with `shutting_down`; telling one is
    // enough because each failed waiter notifies the next.
    impl_->cv.notify_all();
}

std::size_t pool::idle() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->idle.size();
}

std::size_t pool::in_use() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->in_use;
}

std::size_t pool::total() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->live;
}

std::uint64_t pool::opened() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->opened;
}

bool pool::is_closed() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->closed;
}

} // namespace db
} // namespace crpc