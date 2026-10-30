// CRPC -- Windows IOCP (I/O completion port) transport backend.
//
// This header is the public entry point for the Windows backend.  It is only
// visible on Windows; on every other target the include is a no-op so that a
// translation unit that mentions it still compiles and the backend simply is
// not there.
//
// Design summary (see the specification, section 6):
//
//   * One `CreateIoCompletionPort` per backend instance, harvested by exactly
//     one pump thread with `GetQueuedCompletionStatusEx` (batch default 64).
//     `NumberOfConcurrentThreads` is 1 because this backend owns a single
//     dequeue thread; that value limits the number of *threads* the kernel
//     lets dequeue completions, never the number of in-flight overlapped
//     operations, which stays bounded only by the socket and the op pool.
//
//   * Per connection there is at most one outstanding receive and one
//     outstanding send.  `WSARecv`/`WSASend` point at operation records whose
//     `OVERLAPPED` is embedded by value and which come from a small per-backend
//     pool, so no per-operation heap allocation happens on the hot path.
//
//   * Accepts use `AcceptEx`.  A new socket is created with
//     `WSA_FLAG_OVERLAPPED`, and it is bound to the completion port *before*
//     any receive is posted on it.
//
//   * Timers.  `GetQueuedCompletionStatusEx`'s timeout is expressed in whole
//     milliseconds and cannot represent a sub-millisecond deadline.  The
//     backend therefore owns a timer thread that waits on a
//     `CreateWaitableTimerExW(..., CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, ...)`
//     handle armed with a relative 100 ns interval, and that posts an ordinary
//     completion packet to the pump with `PostQueuedCompletionStatus` when a
//     deadline elapses.  `SetWaitableTimer` takes 100 ns units and the
//     high-resolution flag removes the legacy ~15.6 ms timer resolution
//     dependence, so a deadline a few hundred microseconds out is honoured at
//     the same precision the pump itself runs at.  The pump never sleeps in
//     milliseconds; it blocks indefinitely and is woken by real completions.
//
//   * On close the backend calls `CancelIoEx`; the canceled operation still
//     produces exactly one completion, which is observed and translated to
//     `operation_canceled` rather than dropped.  A `DisconnectEx(...,
//     TF_REUSE_SOCKET, ...)` is attempted for a graceful close and an abortive
//     `closesocket` is used when the extension is unavailable.
//
// Thread contract
// ---------------
//   * `iocp_backend::connect`, `listen`, `arm_timer`, `cancel_timer` and
//     `stop` may be called from any thread.
//   * The pump thread runs every source callback (`on_data`, `on_eof`,
//     `on_error`) and every sink completion (`completion::complete`).  Timer
//     callbacks also run on the pump thread.  No library callback ever runs on
//     the timer thread.
//   * `connection::bind_source`, `resume_read`, `close`, `is_open` and
//     `sink::push` may be called from any thread.
//   * After `close()` no new source delivery or sink callback starts; in-flight
//     completions still run exactly once as `operation_canceled`.
#ifndef CRPC_IO_IOCP_BACKEND_HPP
#define CRPC_IO_IOCP_BACKEND_HPP

#include <crpc/io.hpp>

#if defined(_WIN32)

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace crpc {

// Tunables for the IOCP backend.  Every field is a plain value so the backend
// can be configured statically or at run time; nothing here is a compile-time
// constant that would have to be edited to change behaviour.
struct iocp_options {
    // Completions harvested per `GetQueuedCompletionStatusEx` call.
    std::size_t completion_batch = 64;

    // Default receive buffer size per connection.
    std::size_t recv_buffer_bytes = 64 * 1024;

    // Bounded per-connection send queue, in bytes.  A single write larger than
    // the bound is still accepted when the queue is empty so that one frame can
    // never be rejected for size alone.
    std::size_t send_queue_bytes = 256 * 1024;

    // Number of `AcceptEx` operations pre-posted per listener.
    std::size_t accept_posts = 64;

    // Operation records pre-allocated in the pool.  The pool grows on demand;
    // this only sets the initial size.
    std::size_t initial_op_pool = 256;

    // Whether to set TCP_NODELAY on established connections.
    bool tcp_no_delay = true;
};

// A Windows IOCP backend.  It implements `backend_factory` and additionally
// exposes a sub-millisecond timer facility.
class CRPC_API iocp_backend final : public backend_factory {
public:
    // A one-shot timer callback.  It runs on the backend pump thread.
    using timer_callback = void (*)(void* arg) noexcept;

    explicit iocp_backend(iocp_options options = iocp_options{});
    ~iocp_backend() override;

    iocp_backend(const iocp_backend&) = delete;
    iocp_backend& operator=(const iocp_backend&) = delete;
    iocp_backend(iocp_backend&&) = delete;
    iocp_backend& operator=(iocp_backend&&) = delete;

    std::unique_ptr<connection> connect(const endpoint& ep,
                                        const connect_options& opt) override;
    std::unique_ptr<listener> listen(const endpoint& ep,
                                     const listen_options& opt) override;

    // Arms a one-shot timer that fires after `delay` and invokes `callback`.
    // Returns a non-zero id, or 0 when the backend is not usable.  `delay` is
    // clamped to zero; a non-positive delay fires on the next pump turn.
    [[nodiscard]] std::uint64_t arm_timer(std::chrono::nanoseconds delay,
                                          timer_callback callback,
                                          void* arg) noexcept;

    // Cancels a timer by id.  Returns true when the timer was still pending;
    // false when it had already fired or was never armed.  A callback whose
    // timer is canceled after expiry but before it runs is suppressed.
    bool cancel_timer(std::uint64_t id) noexcept;

    // Stops the pump and timer threads and closes every live connection and
    // listener.  Idempotent; the destructor calls it.
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept;

    // Whether this backend arms its timer with
    // `CreateWaitableTimerExW(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)`.  It is
    // false when the host predates Windows 10 1803 and the backend fell back to
    // a standard millisecond timer: deadlines are still honoured, only the
    // resolution degrades to the legacy ~15.6 ms tick.  The value is derived
    // from the flags the backend requested, not from the existence of a timer
    // handle (that API also succeeds with flags == 0), so dropping the flag is
    // observable.  A caller that needs to reason about sub-millisecond timer
    // accuracy asks this instead of timing the timer and guessing.
    [[nodiscard]] bool high_resolution_timer() const noexcept;

    // The implementation type is public only so the out-of-line definitions in
    // the translation unit can name it; it has no public members and no user of
    // this header ever needs to mention it.
    struct impl;

private:
    std::unique_ptr<impl> impl_;
};

} // namespace crpc

#endif // _WIN32

#endif // CRPC_IO_IOCP_BACKEND_HPP
