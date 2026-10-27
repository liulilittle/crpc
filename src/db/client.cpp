// CRPC -- database client facade.
#include <crpc/db/client.hpp>

#include <utility>

namespace crpc {
namespace db {

struct client::impl {
    // The pool outlives any in-flight async task: tasks capture this object by
    // shared_ptr, so a task that outlives the client still owns a live pool.
    pool connections;
    // Borrowed, not owned: the caller's executor must outlive the client.
    async::executor* executor = nullptr;

    impl(connection_factory& factory, client_options options)
        : connections(options.pool, factory), executor(options.executor) {}
};

client::client(connection_factory& factory, client_options options)
    : impl_(std::make_shared<impl>(factory, options)) {}

client::~client() { close(); }

result<result_set*> client::query(std::string_view sql, span<const parameter> params) {
    result<pool::lease> lease = impl_->connections.acquire();
    if (!lease) {
        return lease.error();
    }
    result_set* rows = nullptr;
    const status ran = lease.value()->query(sql, params, &rows);
    if (!ran) {
        return ran.error();
    }
    // The cursor is materialised by the adapter, so the connection can go back to
    // the pool before the caller reads a row.
    (void)lease.value().release();
    return rows;
}

async::future<result_set*> client::query_async(std::string sql,
                                               std::vector<parameter> params) {
    if (impl_->executor == nullptr) {
        // An invalid future, not a hidden synchronous call: a caller that asked for
        // a handle must be able to tell that none was created.
        return async::future<result_set*>();
    }
    // The task captures the state, not `this`, so it stays valid if the client is
    // destroyed while the query is queued.
    std::shared_ptr<impl> state = impl_;
    return state->executor->submit(
        [state, sql = std::move(sql), params = std::move(params)]() -> result<result_set*> {
            result<pool::lease> lease = state->connections.acquire();
            if (!lease) {
                return lease.error();
            }
            result_set* rows = nullptr;
            const status ran = lease.value()->query(
                sql, span<const parameter>(params.data(), params.size()), &rows);
            if (!ran) {
                return ran.error();
            }
            return rows;
        });
}

status client::execute(std::string_view sql, span<const parameter> params) {
    result<result_set*> rows = query(sql, params);
    if (!rows) {
        return rows.error();
    }
    delete rows.value();
    return status{};
}

void client::close() noexcept {
    if (impl_) {
        impl_->connections.close();
    }
}

std::size_t client::idle() const noexcept { return impl_->connections.idle(); }

std::size_t client::in_use() const noexcept { return impl_->connections.in_use(); }

bool client::is_closed() const noexcept { return impl_->connections.is_closed(); }

} // namespace db
} // namespace crpc