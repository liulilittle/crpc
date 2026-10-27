// CRPC -- asynchronous byte-transport connection and pool.
//
// This header is the transport half of every protocol client in the library
// (Redis, memcached, the database adapters, the message queues). It
// deliberately knows nothing about any wire format: a `connection` moves bytes
// on top of a CRPC `io::source`/`io::sink` pair, so a protocol layer is built on
// top without the transport caring how requests and replies are framed.
// Everything the caller
// needs to know is the following contract.
//
// Execution model
// ---------------
// A `connection` is bound to an optional `crpc::runtime` (the shard). Reads and
// writes are *cooperative*: when the caller is a coroutine on the shard thread
// (or a coroutine of a plain thread for the synchronous path), the operation
// parks through `runtime::park()` and the runtime resumes it when the backend
// delivers bytes. On a plain thread with no shard -- and for a non-coroutine
// task that happens to run on the shard thread -- the operation waits on a
// condition variable bounded by `connection_config::operation_timeout`.
//
// Backpressure
// ------------
// `on_data` never blocks. Incoming bytes are appended to a bounded buffer; when
// the buffer is full the read side returns 0 to the backend (a short credit)
// which pauses delivery. The connection calls `io::connection::resume_read()`
// once the application has drained the buffer below the high-water mark, so a
// slow consumer cannot grow memory without bound and a fast consumer is not
// throttled.
//
// Reconnect
// ---------
// A terminal `on_error`/`on_eof` marks the connection closed and wakes every
// waiter with the backend's error code (`connection_reset`, `broken_pipe`, ...)
// or `eof`. The next operation lazily re-establishes the underlying connection
// through the same `backend_factory` before sending, with a bounded exponential
// backoff. An explicit `reconnect()` always tears down the current link, even
// when it is still open.
//
// `close()` is terminal: it drops buffered bytes, closes the link and makes
// every later read/write fail with `shutting_down` without reconnecting. Only
// an explicit `reconnect()` revives a closed connection.
//
// Thread safety
// -------------
// Every public method is safe to call from any thread. The backend may invoke
// the source callbacks from its own thread; they only append to the buffer and
// wake waiters. A write or read in flight keeps its underlying link alive even
// if another thread closes or reconnects concurrently.
#ifndef CRPC_NET_CONNECTION_HPP
#define CRPC_NET_CONNECTION_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/io.hpp>
#include <crpc/runtime.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace net {

// Tunables for one logical connection. Every field is a plain value: the whole
// configuration can be set at run time and needs no compile-time constant.
struct connection_config {
    // Where to connect. Passed straight to `backend_factory::connect`.
    crpc::endpoint ep;

    // Transport options handed to `backend_factory::connect`.
    crpc::connect_options connect{};

    // Upper bound on the application read buffer, in bytes. When the buffer is
    // full the connection stops consuming from the backend (backpressure).
    std::size_t max_buffered_bytes = 256 * 1024;

    // How long a plain-thread read/write may block before it fails with
    // `deadline_exceeded`. Ignored on the coroutine path, which parks until the
    // connection delivers or closes. A write that times out aborts the
    // underlying link (a partial frame must not precede the next command); the
    // following operation reconnects.
    std::chrono::nanoseconds operation_timeout{std::chrono::seconds(5)};

    // Delay before the first reconnect attempt, and the ceiling the exponential
    // backoff grows towards. A successful connect resets the backoff to the
    // floor.
    std::chrono::nanoseconds reconnect_backoff{std::chrono::milliseconds(50)};
    std::chrono::nanoseconds reconnect_max_backoff{std::chrono::seconds(2)};

    // Zero means unlimited reconnect attempts. A non-zero value caps the number
    // of consecutive failed reconnects after the initial connect attempt; once
    // it is reached, operations fail with `unavailable` instead of trying
    // again. A successful connect resets the count.
    std::size_t max_reconnects = 0;
};

// One asynchronous connection. It owns one CRPC `io::connection`; the caller
// selects the backend once and this type hides it completely.
//
// The object is cheap to move in the sense of a handle: it is neither copyable
// nor movable because the backend holds a raw pointer to its source adapter
// until `close()`.
class CRPC_API connection {
public:
    // Establishes the underlying connection. If the backend refuses (for
    // example no listener is bound for an in-process name), the object is still
    // constructed but closed: the first read/write will retry the connect with
    // the configured backoff.
    connection(connection_config config, crpc::backend_factory& backend,
               runtime* shard = nullptr);
    ~connection();

    connection(const connection&) = delete;
    connection& operator=(const connection&) = delete;
    connection(connection&&) = delete;
    connection& operator=(connection&&) = delete;

    // Async send. Returns `ok` once the backend has accepted every byte (the
    // kernel queue / in-process queue), not once the peer processed them.
    [[nodiscard]] status write(span<const std::byte> data);

    // Async read of exactly `count` bytes. Fails with the backend's error code
    // (`connection_reset`, `broken_pipe`, ...) or `eof` when the peer closes
    // before `count` bytes arrive, and `deadline_exceeded` when the plain-thread
    // timeout elapses.
    [[nodiscard]] result<std::vector<std::byte>> read_exact(std::size_t count);

    // Async read up to and including the first `delim`. The delimiter is part
    // of the returned payload. Useful for line-oriented framing. A closed
    // stream that ends before the delimiter reports the backend error code (or
    // `eof`), not a bare end-of-file.
    [[nodiscard]] result<std::vector<std::byte>> read_until(std::byte delim);

    // Non-blocking drain of whatever is buffered. Returns an empty vector when
    // no complete unit is buffered yet.
    [[nodiscard]] std::vector<std::byte> read_available();

    // Drops any buffered bytes, closes the underlying connection and wakes
    // waiters with `shutting_down`. Terminal: later reads/writes fail with
    // `shutting_down` and never reconnect; only `reconnect()` revives the
    // object. Idempotent and safe from any thread.
    void close() noexcept;

    // True while the underlying connection is established.
    [[nodiscard]] bool is_open() const noexcept;

    // Forces a reconnect now: closes the current link (even if it is open) and
    // establishes a new one. Returns the backend error on failure (the object
    // stays closed). Counts as a reconnect once the first link has succeeded.
    [[nodiscard]] status reconnect();

    [[nodiscard]] const connection_config& config() const noexcept;

    // Diagnostics.
    [[nodiscard]] std::size_t buffered() const noexcept;
    [[nodiscard]] std::uint64_t reconnect_count() const noexcept;
    [[nodiscard]] std::uint64_t bytes_read() const noexcept;
    [[nodiscard]] std::uint64_t bytes_written() const noexcept;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

// Tunables for a pool of connections to one endpoint.
struct pool_config {
    // Configuration cloned into every connection the pool creates.
    connection_config connection{};

    // Connections the pool tries to keep warm. A warm connection is created on
    // the first acquire; the pool never blocks to reach this target.
    std::size_t min_size = 1;

    // Hard ceiling on simultaneously established connections. A caller that
    // arrives while `max_size` connections are checked out waits for a release
    // or fails with `resource_exhausted` after `acquire_timeout`.
    std::size_t max_size = 8;

    // Bound on the plain-thread wait for a free connection. The coroutine path
    // parks until a connection is released.
    std::chrono::nanoseconds acquire_timeout{std::chrono::seconds(5)};

    // Ceiling on idle connections. A release beyond it closes the connection, which
    // is how a burst of concurrency shrinks back instead of leaving the pool at its
    // high-water mark forever. Zero means "same as max_size".
    std::size_t max_idle = 0;

    // Close an idle connection that has been unused for longer. Zero disables it.
    // This is what recovers capacity a quiet server would otherwise hold open.
    std::chrono::nanoseconds idle_timeout{std::chrono::seconds(60)};

    // Close a connection older than this, measured from when it was opened. Zero
    // disables it. It exists to recycle a link that may have accumulated
    // server-side state (a stale plan, a per-connection cache) even while in use.
    std::chrono::nanoseconds max_lifetime{std::chrono::minutes(30)};
};

// A fixed-capacity pool of connections to a single endpoint.
//
// `acquire()` returns a connection and marks it checked out; `release()` returns
// it. A released connection that is no longer open is closed and not reused.
// Connections are created lazily, so constructing the pool never blocks on the
// network.
class CRPC_API connection_pool {
public:
    connection_pool(pool_config config, crpc::backend_factory& backend,
                    runtime* shard = nullptr);
    ~connection_pool();

    connection_pool(const connection_pool&) = delete;
    connection_pool& operator=(const connection_pool&) = delete;
    connection_pool(connection_pool&&) = delete;
    connection_pool& operator=(connection_pool&&) = delete;

    // Takes a connection out of the pool, creating one when below `max_size`.
    // Idle connections are re-checked for liveness; a link whose peer closed
    // while it was idle is discarded, never handed to the caller. Parks the
    // calling coroutine when the pool is exhausted; a plain thread waits up to
    // `pool_config::acquire_timeout`.
    [[nodiscard]] result<std::shared_ptr<connection>> acquire();

    // Returns a connection. If it is closed or the pool is full it is closed
    // instead of being reused; otherwise it becomes available for the next
    // acquire.
    void release(std::shared_ptr<connection> conn);

    // Best-effort pre-creation of up to `pool_config::min_size` connections.
    // Stops as soon as a connect fails (an unreachable endpoint cannot be
    // warmed); a connection that fails to establish is not retained. Safe to
    // call from any thread.
    void warmup();

    // Closes every idle connection and rejects further acquires with
    // `shutting_down`. Idempotent and safe from any thread.
    void close() noexcept;

    [[nodiscard]] std::size_t idle() const noexcept;
    [[nodiscard]] std::size_t in_use() const noexcept;
    [[nodiscard]] std::size_t total() const noexcept;

    // Connections opened since construction, including those since closed. Used to
    // prove that a dead link was really replaced rather than silently reused.
    [[nodiscard]] std::uint64_t opened() const noexcept;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace net
} // namespace crpc

#endif // CRPC_NET_CONNECTION_HPP
