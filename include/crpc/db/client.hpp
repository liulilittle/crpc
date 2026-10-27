// CRPC -- database client facade with the three calling styles.
//
// The framework's primary style is the coroutine: every blocking call parks the
// calling coroutine, so a shard thread is never held while a database answers.
// The same signature is the synchronous style when called from an ordinary thread
// (it waits on a condition variable), and `query_async` is the handle style for a
// caller that wants to do something else in the meantime.
//
// There is deliberately no separate "coroutine" entry point: the transport already
// distinguishes the two, and duplicating it would let the two paths drift. The only
// real difference is where the work runs for the async form, which is why an
// executor must be supplied explicitly — silently running "async" work on the
// caller's thread would be a lie.
#ifndef CRPC_DB_CLIENT_HPP
#define CRPC_DB_CLIENT_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/async/executor.hpp>
#include <crpc/config.hpp>
#include <crpc/db/connection.hpp>
#include <crpc/db/types.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace db {

struct client_options {
    pool_options pool{};

    // The executor the async form runs on. Without it `query_async` returns an
    // invalid future rather than pretending to be asynchronous.
    async::executor* executor = nullptr;
};

class CRPC_API client {
public:
    client(connection_factory& factory, client_options options = {});
    ~client();

    client(const client&) = delete;
    client& operator=(const client&) = delete;
    client(client&&) = delete;
    client& operator=(client&&) = delete;

    // Runs `sql` with bound parameters and returns the owned cursor (null when the
    // statement produced no rows). Parks a coroutine, blocks a plain thread.
    [[nodiscard]] result<result_set*> query(std::string_view sql,
                                            span<const parameter> params = {});

    // The handle form. The returned future owns the connection lease until it
    // completes, and `.get()` yields the same `result<result_set*>` the direct call
    // would have returned.
    [[nodiscard]] async::future<result_set*> query_async(std::string sql,
                                                         std::vector<parameter> params = {});

    // Convenience over `query` for a statement that returns no rows.
    [[nodiscard]] status execute(std::string_view sql, span<const parameter> params = {});

    // Closes the pool; outstanding leases are closed when released.
    void close() noexcept;

    [[nodiscard]] std::size_t idle() const noexcept;
    [[nodiscard]] std::size_t in_use() const noexcept;
    [[nodiscard]] bool is_closed() const noexcept;

private:
    struct impl;
    std::shared_ptr<impl> impl_;
};

} // namespace db
} // namespace crpc

#endif // CRPC_DB_CLIENT_HPP