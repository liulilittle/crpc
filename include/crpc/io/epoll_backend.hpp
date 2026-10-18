// CRPC -- epoll edge-triggered TCP transport backend (Linux only).
//
// This header is guarded by `CRPC_OS_LINUX`; on any other platform it expands
// to nothing so a Windows build of the library still compiles.  The whole
// implementation lives in `src/io/epoll_backend.cpp`.
//
// Design summary (see `docs/superpowers/specs/2026-10-19-crpc-design.md` §5 and
// `.opencode/ruling.md` R3/R4/R18):
//
//   * Every fd is registered edge-triggered and is drained to `EAGAIN`:
//     connections use `EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP`, listeners
//     use `EPOLLIN | EPOLLET`.  There is no level-triggered mode.
//   * `epoll_event.data.u64` carries a token `(slot_index << 32) | generation`,
//     never a raw pointer.  A stale event for a closed connection fails the
//     generation check and is dropped, so it can never touch freed memory.
//   * A connection owns a bounded send ring.  `push` copies into the ring and
//     completes immediately when the whole span fits (the in-process analogue
//     of a successful kernel write); when the ring is full the completion is
//     deferred and the span tail stays borrowed, exactly as inproc does.
//   * The write loop sends until `EAGAIN`.  If it stops because a per-turn byte
//     budget ran out while the socket is still writable, it self-posts a single
//     write wake (R4): edge-triggered `EPOLLOUT` will not report the still
//     writable socket again.  The wake is coalesced through the connection's
//     `queued` flag, so a slow peer cannot cause a doorbell storm.
//   * Sub-millisecond deadlines are carried by a `timerfd` registered in the
//     same epoll set, so the nearest armed deadline drives `epoll_wait`
//     (`-1` timeout).  There is deliberately **no** millisecond fallback: a
//     silent fallback would break the p99 contract.  If `timerfd_create`
//     fails, `create()` returns `rpc_code::internal` naming `timerfd`.
//     Linux 2.6.27+ is therefore a hard requirement.
//
// Thread contract:
//
//   * `epoll_backend` starts exactly one loop thread.  All socket I/O and all
//     `io::source` callbacks (`on_data`, `on_eof`, `on_error`) run on that
//     thread, so source callbacks are serialized per connection.
//   * `connection::close`, `connection::resume_read`, `connection::is_open`,
//     `connection::remote` and `listener::stop` are safe to call from any
//     thread.
//   * `sink::push` is safe from the loop thread and from any other single
//     thread, but the caller must not issue two concurrent `push` calls on the
//     same connection (contract I5 already forbids a second in-flight push).
//     A `push` whose span fits completes on the calling thread; a deferred
//     `push` completes on the loop thread.
//   * `bind_source` must be called before traffic starts.
//   * Connections and listeners must be destroyed before the backend that owns
//     them.
#ifndef CRPC_IO_EPOLL_BACKEND_HPP
#define CRPC_IO_EPOLL_BACKEND_HPP

#include <crpc/config.hpp>

#if defined(CRPC_OS_LINUX)

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <crpc/error.hpp>
#include <crpc/io.hpp>

namespace crpc {
namespace io {

// Tunables for the epoll backend.  All are fixed at construction; there is no
// process-global state.
struct epoll_options {
    // Bound on bytes buffered per connection on the send side.  A `push` whose
    // span does not fit defers its completion until the ring drains.
    std::size_t max_queued_bytes = 64 * 1024;

    // Bytes the write loop may hand to the kernel in one turn.  Hitting this
    // budget self-posts one write wake (R4).
    std::size_t write_budget_bytes = 256 * 1024;

    // Size of the per-connection receive scratch buffer.
    std::size_t read_buffer_bytes = 64 * 1024;

    // Hard cap on simultaneously registered connections.  Accepts past the cap
    // are closed immediately.
    std::size_t max_connections = 4096;

    // Number of `epoll_event`s fetched per `epoll_wait`.
    std::size_t max_events = 256;
};

// A timer callback runs on the loop thread and must not throw or block.
using timer_callback = void (*)(void* arg) noexcept;
using timer_id = std::uint64_t;
inline constexpr timer_id invalid_timer = 0;

// The Linux epoll backend.  It owns one epoll fd, one doorbell eventfd, one
// timerfd and one loop thread, and implements `backend_factory` so the RPC
// layer never sees the platform.
//
// Construction is fallible and never throws: `create()` returns a `result`
// carrying `rpc_code::internal` when the epoll fd, doorbell or timerfd cannot
// be created.
class epoll_backend final : public backend_factory {
public:
    [[nodiscard]] static result<std::unique_ptr<epoll_backend>> create(
        epoll_options opt = epoll_options{});

    ~epoll_backend() override;

    epoll_backend(const epoll_backend&) = delete;
    epoll_backend& operator=(const epoll_backend&) = delete;
    epoll_backend(epoll_backend&&) = delete;
    epoll_backend& operator=(epoll_backend&&) = delete;

    std::unique_ptr<connection> connect(const endpoint& ep,
                                        const connect_options& opt) override;
    std::unique_ptr<listener> listen(const endpoint& ep,
                                     const listen_options& opt) override;

    // Arms a one-shot timer `delay` from now; the callback runs once on the
    // loop thread.  Returns `invalid_timer` for a null callback or a non-positive
    // delay.  Safe from any thread.
    [[nodiscard]] timer_id arm_timer(std::chrono::nanoseconds delay,
                                     timer_callback fn, void* arg) noexcept;

    // Cancels a timer.  Returns false if it already fired or was never armed.
    // Idempotent.  Safe from any thread.
    bool cancel_timer(timer_id id) noexcept;

    [[nodiscard]] const epoll_options& options() const noexcept;

    // Diagnostics.  `r4_self_post_count` counts the write wakes posted by the
    // R4 budget path; it is used by the coalescing test and is cheap to read.
    [[nodiscard]] std::uint64_t r4_self_post_count() const noexcept;
    [[nodiscard]] std::uint64_t connection_count() const noexcept;

    // Opaque implementation; public only so the connection type defined in the
    // .cpp can name it.
    struct impl;

private:
    explicit epoll_backend(epoll_options opt) noexcept;

    std::unique_ptr<impl> impl_;
};

} // namespace io
} // namespace crpc

#endif // CRPC_OS_LINUX
#endif // CRPC_IO_EPOLL_BACKEND_HPP
