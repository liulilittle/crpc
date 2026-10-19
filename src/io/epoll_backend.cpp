// CRPC -- epoll edge-triggered TCP transport backend (Linux only).
//
// See the header for the design summary and the thread contract.  This file is
// compiled only on Linux; on every other platform it is an empty translation
// unit.
//
// Shape of the implementation:
//
//   * `impl` owns the epoll fd, the doorbell `eventfd`, the `timerfd`, the slot
//     registry and the loop thread.
//   * `epoll_connection` owns one socket plus a bounded send ring and a receive
//     scratch buffer.  Its public interface methods may be called from any
//     thread; all socket I/O runs on the loop thread.
//   * `listener_impl` accepts to `EAGAIN` and hands each new connection to the
//     accept sink on the loop thread.
//
// Lock order (never nested): `work_mutex`, `reg_mutex`, `timer_mutex`,
// per-connection `mtx_`.  Callbacks (`on_data`, `on_eof`, `on_error`,
// `completion::complete`) always run with no connection lock held.
#include <crpc/io/epoll_backend.hpp>

#if defined(CRPC_OS_LINUX)

#include "bound_guard.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

namespace crpc {
namespace io {

namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

std::error_code errno_ec(int e) noexcept {
    return std::error_code(e, std::generic_category());
}

std::uint64_t monotonic_now_ns() noexcept {
    timespec ts{};
    const int rc = ::clock_gettime(CLOCK_MONOTONIC, &ts);
    (void)rc;
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

// Token = (slot_index << 32) | generation.  The index is widened to 64 bits
// before shifting, so the shift is well-defined on every target.
constexpr std::uint64_t make_token(std::uint32_t index,
                                   std::uint32_t generation) noexcept {
    return (static_cast<std::uint64_t>(index) << 32) |
           static_cast<std::uint64_t>(generation);
}
constexpr std::uint32_t token_index(std::uint64_t token) noexcept {
    return static_cast<std::uint32_t>(token >> 32);
}
constexpr std::uint32_t token_generation(std::uint64_t token) noexcept {
    return static_cast<std::uint32_t>(token & 0xffffffffull);
}

enum : std::uint32_t {
    kind_free = 0,
    kind_conn = 1,
    kind_listener = 2,
    kind_eventfd = 3,
    kind_timerfd = 4,
};

void set_socket_buffers(int fd, std::size_t recv_bytes,
                        std::size_t send_bytes) noexcept {
    if (recv_bytes != 0) {
        const int v = static_cast<int>(std::min<std::size_t>(
            recv_bytes, static_cast<std::size_t>(INT_MAX)));
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &v, sizeof(v));
    }
    if (send_bytes != 0) {
        const int v = static_cast<int>(std::min<std::size_t>(
            send_bytes, static_cast<std::size_t>(INT_MAX)));
        (void)::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof(v));
    }
}

endpoint peer_endpoint(int fd) noexcept {
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
        return endpoint::tcp(std::string(), 0);
    }
    char host[INET6_ADDRSTRLEN] = {0};
    std::uint16_t port = 0;
    if (ss.ss_family == AF_INET) {
        auto* in4 = reinterpret_cast<sockaddr_in*>(&ss);
        (void)::inet_ntop(AF_INET, &in4->sin_addr, host, sizeof(host));
        port = ntohs(in4->sin_port);
    } else if (ss.ss_family == AF_INET6) {
        auto* in6 = reinterpret_cast<sockaddr_in6*>(&ss);
        (void)::inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof(host));
        port = ntohs(in6->sin6_port);
    } else {
        return endpoint::tcp(std::string(), 0);
    }
    return endpoint::tcp(std::string(host), port);
}

class epoll_connection;

} // namespace

// ---------------------------------------------------------------------------
// impl
// ---------------------------------------------------------------------------

struct epoll_backend::impl {
    explicit impl(epoll_options o) noexcept : opt(o) {}
    ~impl();

    impl(const impl&) = delete;
    impl& operator=(const impl&) = delete;

    [[nodiscard]] rpc_error start() noexcept;
    void stop() noexcept;
    void wake() noexcept;

    // Registry ------------------------------------------------------------
    struct slot {
        std::uint32_t kind = kind_free;
        std::uint32_t generation = 0;
        void* ptr = nullptr;
    };

    [[nodiscard]] std::uint64_t alloc_slot(std::uint32_t kind,
                                           void* ptr) noexcept;
    void free_slot(std::uint64_t token, std::uint32_t expect_kind) noexcept;
    [[nodiscard]] bool add_fd(int fd, std::uint64_t token,
                              std::uint32_t events) noexcept;

    // Factory operations ---------------------------------------------------
    std::unique_ptr<connection> do_connect(const endpoint& ep,
                                           const connect_options& opt) noexcept;
    std::unique_ptr<listener> do_listen(const endpoint& ep,
                                        const listen_options& opt) noexcept;
    [[nodiscard]] epoll_connection* make_connection(int fd,
                                                    endpoint remote) noexcept;
    void detach_conn(epoll_connection* c) noexcept;

    // Service queue --------------------------------------------------------
    void request_service(std::uint64_t token) noexcept;

    // Accept backoff: on EMFILE/ENFILE the listener token is retried after a
    // short delay through a timer whose argument is `this` (stable), never a
    // listener pointer.  Stale tokens fail the generation check in drain_work.
    void schedule_accept_backoff(std::uint64_t token) noexcept;
    static void accept_backoff_tick(void* arg) noexcept;

    // Timers ---------------------------------------------------------------
    [[nodiscard]] timer_id do_arm_timer(std::chrono::nanoseconds delay,
                                        timer_callback fn, void* arg) noexcept;
    bool do_cancel_timer(timer_id id) noexcept;
    void process_timers() noexcept;
    void rearm_timerfd_locked() noexcept;

    // Loop -----------------------------------------------------------------
    void run() noexcept;
    void dispatch(const epoll_event& ev) noexcept;
    void drain_work() noexcept;

    struct listener_impl;

    epoll_options opt;
    int epfd_ = -1;
    int eventfd_ = -1;
    int timerfd_ = -1;
    std::uint64_t eventfd_token_ = 0;
    std::uint64_t timerfd_token_ = 0;

    std::thread loop_;
    std::atomic<std::thread::id> loop_tid_{};
    std::atomic<bool> running_{false};

    mutable std::mutex reg_mutex_;
    std::vector<slot> slots_;
    std::vector<std::uint32_t> free_slots_;
    std::uint32_t next_generation_ = 1;

    std::mutex work_mutex_;
    std::vector<std::uint64_t> work_tokens_;

    std::mutex timer_mutex_;
    struct timer_entry {
        timer_id id = 0;
        std::uint64_t deadline = 0;
        timer_callback fn = nullptr;
        void* arg = nullptr;
    };
    std::multimap<std::uint64_t, timer_entry> by_deadline_;
    std::unordered_map<timer_id, std::uint64_t> id_to_deadline_;
    timer_id next_timer_id_ = 1;

    std::atomic<std::uint64_t> conn_count_{0};
    std::atomic<std::uint64_t> r4_self_posts_{0};

    std::mutex accept_mutex_;
    std::vector<std::uint64_t> accept_backoff_;
    timer_id accept_backoff_timer_ = invalid_timer;
};

// ---------------------------------------------------------------------------
// epoll_connection
// ---------------------------------------------------------------------------

namespace {

class epoll_connection final : public connection {
public:
    epoll_connection(epoll_backend::impl& owner, int fd, endpoint remote,
                     const epoll_options& opt) noexcept
        : owner_(owner),
          fd_(fd),
          remote_(std::move(remote)),
          send_cap_(opt.max_queued_bytes != 0 ? opt.max_queued_bytes : 1),
          write_budget_(opt.write_budget_bytes != 0 ? opt.write_budget_bytes : 1),
          rx_cap_(opt.read_buffer_bytes != 0 ? opt.read_buffer_bytes : 1),
          source_adapter_(*this),
          sink_adapter_(*this) {
        rx_.reset(new (std::nothrow) std::byte[rx_cap_]);
        tx_.reset(new (std::nothrow) std::byte[send_cap_]);
        valid_ = rx_ && tx_;
    }

    ~epoll_connection() override {
        // The source is borrowed: wait for calls already running into it
        // before the connection is torn down.
        bound_.retire();
        terminate(std::error_code{}, false);
        ensure_quiescent();
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] int fd() const noexcept { return fd_; }
    void set_token(std::uint64_t t) noexcept { token_ = t; }
    [[nodiscard]] std::uint64_t token() const noexcept { return token_; }

    io::source& source() noexcept override { return source_adapter_; }
    io::sink& sink() noexcept override { return sink_adapter_; }

    void bind_source(io::source* src) noexcept override {
        bound_.rebind(src);
        if (src == nullptr) {
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            read_paused_ = false;
        }
        // Schedule unconditionally. A level-triggered edge may already have
        // been consumed by read_loop() while no source was bound; in
        // edge-triggered mode no further edge arrives for the buffered bytes,
        // so pausing by itself would stall delivery forever.
        schedule();
    }

    void resume_read() noexcept override {
        bool was_paused = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            was_paused = read_paused_;
            if (was_paused) {
                read_paused_ = false;
            }
        }
        if (was_paused) {
            schedule();
        }
    }

    void close() noexcept override {
        terminate(std::error_code{}, false);
        ensure_quiescent();
    }

    [[nodiscard]] bool is_open() const noexcept override {
        return open_.load(std::memory_order_acquire);
    }

    [[nodiscard]] endpoint remote() const override { return remote_; }

    // --- loop-thread entry points ----------------------------------------

    void on_event(std::uint32_t events) noexcept {
        if ((events & EPOLLERR) != 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
                err = errno;
            }
            fail(err != 0 ? errno_ec(err)
                          : std::make_error_code(std::errc::connection_aborted));
            return;
        }
        const bool half = (events & (EPOLLHUP | EPOLLRDHUP)) != 0;
        if (half) {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!closed_) {
                peer_eof_ = true;
            }
        }
        if ((events & (EPOLLIN | EPOLLHUP | EPOLLRDHUP)) != 0) {
            read_loop();
        }
        if ((events & EPOLLOUT) != 0) {
            write_loop();
        }
        if (half) {
            deliver_eof_if_ready();
        }
    }

    void service() noexcept {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (closed_) {
                return;
            }
        }
        read_loop();
        write_loop();
    }

    // --- cross-thread state used by the registry -------------------------

    std::atomic<int> cb_inflight_{0};
    std::atomic<bool> queued_{false};

private:
    class source_adapter_t final : public io::source {
    public:
        explicit source_adapter_t(epoll_connection& owner) noexcept
            : owner_(owner) {}
        std::size_t on_data(span<const std::byte> data) noexcept override {
            if (io::source* s = owner_.bound_.enter()) {
                const std::size_t used = s->on_data(data);
                owner_.bound_.leave();
                return used;
            }
            return 0;
        }
        void on_eof() noexcept override {
            if (io::source* s = owner_.bound_.enter()) {
                s->on_eof();
                owner_.bound_.leave();
            }
        }
        void on_error(std::error_code ec) noexcept override {
            if (io::source* s = owner_.bound_.enter()) {
                s->on_error(ec);
                owner_.bound_.leave();
            }
        }

    private:
        epoll_connection& owner_;
    };

    class sink_adapter_t final : public io::sink {
    public:
        explicit sink_adapter_t(epoll_connection& owner) noexcept
            : owner_(owner) {}
        void push(span<const std::byte> data, io::completion* c) noexcept override {
            owner_.push_impl(data, c);
        }

    private:
        epoll_connection& owner_;
    };

    // --- scheduling -------------------------------------------------------

    void schedule() noexcept {
        bool expected = false;
        if (queued_.compare_exchange_strong(expected, true,
                                            std::memory_order_acq_rel)) {
            owner_.request_service(token_);
        }
    }

    void schedule_r4() noexcept {
        bool expected = false;
        if (queued_.compare_exchange_strong(expected, true,
                                            std::memory_order_acq_rel)) {
            owner_.r4_self_posts_.fetch_add(1, std::memory_order_relaxed);
            owner_.request_service(token_);
        }
    }

    // --- send ring (guarded by mtx_) -------------------------------------

    void compact_locked() noexcept {
        const std::size_t used = tx_tail_ - tx_head_;
        if (tx_head_ != 0) {
            if (used != 0) {
                std::memmove(tx_.get(), tx_.get() + tx_head_, used);
            }
            tx_head_ = 0;
            tx_tail_ = used;
        }
    }

    std::size_t ring_append_locked(span<const std::byte> data) noexcept {
        compact_locked();
        const std::size_t used = tx_tail_ - tx_head_;
        const std::size_t space = send_cap_ - used;
        const std::size_t n = std::min(space, data.size());
        if (n != 0) {
            std::memcpy(tx_.get() + tx_tail_, data.data(), n);
        }
        tx_tail_ += n;
        return n;
    }

    // Moves as much of the borrowed tail into the ring as fits; returns a
    // completion that must run outside the lock, or nullptr.
    io::completion* promote_locked(io::io_result& out) noexcept {
        if (pending_c_ == nullptr) {
            return nullptr;
        }
        compact_locked();
        const std::size_t used = tx_tail_ - tx_head_;
        const std::size_t space = send_cap_ - used;
        if (space == 0) {
            return nullptr;
        }
        const std::size_t n = std::min(space, pending_remaining_);
        std::memcpy(tx_.get() + tx_tail_, pending_data_, n);
        tx_tail_ += n;
        pending_data_ += n;
        pending_remaining_ -= n;
        if (pending_remaining_ == 0) {
            io::completion* c = pending_c_;
            pending_c_ = nullptr;
            out = io::io_result{std::error_code{}, pending_accepted_};
            return c;
        }
        return nullptr;
    }

    // --- push -------------------------------------------------------------

    void push_impl(span<const std::byte> data, io::completion* c) noexcept {
        if (c == nullptr) {
            return;
        }
        io::completion* immediate = nullptr;
        io::io_result immediate_result{};
        bool need_service = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (closed_ || !open_.load(std::memory_order_relaxed)) {
                immediate = c;
                immediate_result = io::io_result{
                    std::make_error_code(std::errc::broken_pipe), 0};
            } else if (pending_c_ != nullptr) {
                // I5: a second push while one is still in flight.
                immediate = c;
                immediate_result = io::io_result{
                    std::make_error_code(std::errc::operation_not_supported), 0};
            } else {
                const std::size_t copied = ring_append_locked(data);
                if (copied == data.size()) {
                    immediate = c;
                    immediate_result = io::io_result{std::error_code{}, data.size()};
                } else {
                    pending_data_ = data.data() + copied;
                    pending_remaining_ = data.size() - copied;
                    pending_accepted_ = data.size();
                    pending_c_ = c;
                }
                need_service = (tx_tail_ != tx_head_) || (pending_c_ != nullptr);
            }
        }
        if (immediate != nullptr) {
            immediate->complete(immediate_result);
        }
        if (need_service) {
            schedule();
        }
    }

    // --- read path (loop thread) -----------------------------------------

    void read_loop() noexcept {
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (closed_ || error_delivered_ || read_paused_) {
                    return;
                }
            }
            // Announce one call for this iteration. The source is borrowed, so
            // it must not be held across iterations.
            io::source* src = bound_.enter();
            if (src == nullptr) {
                return;
            }
            if (rx_tail_ != 0) {
                std::size_t used =
                    src->on_data(span<const std::byte>(rx_.get(), rx_tail_));
                if (used > rx_tail_) {
                    used = rx_tail_; // I9: clamp
                }
                if (used < rx_tail_) {
                    std::memmove(rx_.get(), rx_.get() + used, rx_tail_ - used);
                    rx_tail_ -= used;
                    bound_.leave();
                    std::lock_guard<std::mutex> lk(mtx_);
                    if (!closed_ && !error_delivered_) {
                        read_paused_ = true;
                    }
                    return;
                }
                rx_tail_ = 0;
            }
            const ssize_t k = ::recv(fd_, rx_.get(), rx_cap_, 0);
            if (k > 0) {
                const std::size_t got = static_cast<std::size_t>(k);
                std::size_t used =
                    src->on_data(span<const std::byte>(rx_.get(), got));
                if (used > got) {
                    used = got; // I9: clamp
                }
                if (used < got) {
                    std::memmove(rx_.get(), rx_.get() + used, got - used);
                    rx_tail_ = got - used;
                    bound_.leave();
                    std::lock_guard<std::mutex> lk(mtx_);
                    if (!closed_ && !error_delivered_) {
                        read_paused_ = true;
                    }
                    return;
                }
                bound_.leave();
                continue;
            }
            bound_.leave();
            if (k == 0) {
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    if (!closed_) {
                        peer_eof_ = true;
                    }
                }
                deliver_eof_if_ready();
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            fail(errno_ec(errno));
            return;
        }
    }

    void deliver_eof_if_ready() noexcept {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (closed_ || error_delivered_ || eof_delivered_ || !peer_eof_) {
                return;
            }
            if (read_paused_ || rx_tail_ != 0) {
                return;
            }
            eof_delivered_ = true;
        }
        if (io::source* src = bound_.enter()) {
            src->on_eof();
            bound_.leave();
        }
    }

    // --- write path (loop thread) ----------------------------------------

    void write_loop() noexcept {
        io::completion* done = nullptr;
        io::io_result done_result{};
        std::error_code err{};
        bool failed = false;
        bool budget_stop = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (closed_ || error_delivered_) {
                return;
            }
            std::size_t budget = write_budget_;
            for (;;) {
                if (tx_head_ == tx_tail_) {
                    io::completion* p = promote_locked(done_result);
                    if (p != nullptr) {
                        done = p;
                    }
                    if (tx_head_ == tx_tail_) {
                        break; // nothing left to send
                    }
                }
                const std::size_t avail = tx_tail_ - tx_head_;
                if (budget == 0) {
                    want_write_ = true;
                    budget_stop = true;
                    break;
                }
                const std::size_t n = std::min(budget, avail);
                const ssize_t k =
                    ::send(fd_, tx_.get() + tx_head_, n, MSG_NOSIGNAL);
                if (k > 0) {
                    tx_head_ += static_cast<std::size_t>(k);
                    budget -= static_cast<std::size_t>(k);
                    if (tx_head_ == tx_tail_) {
                        io::completion* p = promote_locked(done_result);
                        if (p != nullptr) {
                            done = p;
                        }
                    }
                    continue;
                }
                if (k < 0 && errno == EINTR) {
                    continue;
                }
                if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    want_write_ = true;
                    break; // resume on the next EPOLLOUT edge
                }
                err = errno_ec(errno);
                failed = true;
                break;
            }
            if (tx_head_ == tx_tail_ && pending_c_ == nullptr) {
                want_write_ = false;
            }
        }
        if (done != nullptr) {
            done->complete(done_result);
        }
        if (failed) {
            fail(err);
            return;
        }
        if (budget_stop && !closed_) {
            schedule_r4();
        }
    }

    // --- terminal paths ---------------------------------------------------

    void fail(std::error_code ec) noexcept { terminate(ec, true); }

    void terminate(std::error_code ec, bool is_error) noexcept {
        io::completion* pending = nullptr;
        bool deliver_error = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (closed_) {
                return;
            }
            closed_ = true;
            open_.store(false, std::memory_order_release);
            pending = pending_c_;
            pending_c_ = nullptr;
            pending_data_ = nullptr;
            pending_remaining_ = 0;
            tx_head_ = 0;
            tx_tail_ = 0;
            if (is_error && !error_delivered_ && !eof_delivered_) {
                error_delivered_ = true;
                deliver_error = true;
            }
        }
        if (pending != nullptr) {
            pending->complete(io::io_result{
                std::make_error_code(std::errc::operation_canceled), 0});
        }
        if (deliver_error) {
            if (io::source* src = bound_.enter()) {
                src->on_error(ec);
                bound_.leave();
            }
        }

        const int fd = fd_;
        owner_.detach_conn(this);
        if (fd >= 0) {
            if (std::this_thread::get_id() !=
                owner_.loop_tid_.load(std::memory_order_acquire)) {
                wait_quiescent();
            }
            // A plain close sends FIN when the receive queue is empty (the peer
            // observes EOF) and RST when it still holds unread bytes (the peer
            // observes a reset).  Do not shutdown(SHUT_WR) first: that would
            // force a FIN and mask an abortive close.
            (void)::close(fd);
            fd_ = -1;
        }
    }

    void ensure_quiescent() noexcept {
        // The loop thread cannot wait on itself; every other caller must not
        // return from close()/the destructor while a dispatch still references
        // this object.
        if (std::this_thread::get_id() ==
            owner_.loop_tid_.load(std::memory_order_acquire)) {
            return;
        }
        wait_quiescent();
    }

    void wait_quiescent() noexcept {
        while (cb_inflight_.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }
    }

    epoll_backend::impl& owner_;
    int fd_ = -1;
    endpoint remote_;
    const std::size_t send_cap_;
    const std::size_t write_budget_;
    const std::size_t rx_cap_;

    std::unique_ptr<std::byte[]> rx_;
    std::unique_ptr<std::byte[]> tx_;
    bool valid_ = false;
    std::uint64_t token_ = 0;

    io::detail::bound_guard bound_;
    std::atomic<bool> open_{true};

    mutable std::mutex mtx_;
    // send side
    std::size_t tx_head_ = 0;
    std::size_t tx_tail_ = 0;
    const std::byte* pending_data_ = nullptr;
    std::size_t pending_remaining_ = 0;
    std::size_t pending_accepted_ = 0;
    io::completion* pending_c_ = nullptr;
    bool want_write_ = false;
    // receive side
    std::size_t rx_tail_ = 0;
    bool read_paused_ = false;
    bool peer_eof_ = false;
    bool eof_delivered_ = false;
    bool error_delivered_ = false;
    bool closed_ = false;

    source_adapter_t source_adapter_;
    sink_adapter_t sink_adapter_;
};

} // namespace

// ---------------------------------------------------------------------------
// listener_impl
// ---------------------------------------------------------------------------

struct epoll_backend::impl::listener_impl final : listener {
    listener_impl(impl& owner, int fd, listen_options o) noexcept
        : owner_(owner), fd_(fd), opt_(o) {}
    ~listener_impl() override {
        if (fd_ >= 0) {
            if (owner_.epfd_ >= 0) {
                (void)::epoll_ctl(owner_.epfd_, EPOLL_CTL_DEL, fd_, nullptr);
            }
            (void)::close(fd_);
            fd_ = -1;
        }
        owner_.free_slot(token_, kind_listener);
    }

    void start(accept_sink* sink) override {
        sink_ = sink;
        if (owner_.running_.load(std::memory_order_acquire)) {
            bool expected = false;
            if (queued_.compare_exchange_strong(expected, true,
                                                std::memory_order_acq_rel)) {
                owner_.request_service(token_);
            }
        }
    }

    void stop() noexcept override {
        stopped_ = true;
        if (fd_ >= 0 && owner_.epfd_ >= 0) {
            (void)::epoll_ctl(owner_.epfd_, EPOLL_CTL_DEL, fd_, nullptr);
        }
    }

    void on_acceptable() noexcept {
        for (;;) {
            const int cfd =
                ::accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (cfd >= 0) {
                if (stopped_ || sink_ == nullptr) {
                    (void)::close(cfd);
                    continue;
                }
                set_socket_buffers(cfd, opt_.recv_buffer_bytes,
                                   opt_.send_buffer_bytes);
                epoll_connection* c =
                    owner_.make_connection(cfd, peer_endpoint(cfd));
                if (c == nullptr) {
                    (void)::close(cfd);
                    continue;
                }
                sink_->on_accept(std::unique_ptr<connection>(c));
                continue;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            if (errno == ECONNABORTED) {
                continue;
            }
            if (errno == EMFILE || errno == ENFILE) {
                // Retry after a short delay; the token is validated by
                // generation, so a destroyed listener is simply skipped.
                owner_.schedule_accept_backoff(token_);
                return;
            }
            // Transient error: leave the pending connection for the next edge.
            return;
        }
    }

    impl& owner_;
    int fd_ = -1;
    listen_options opt_;
    accept_sink* sink_ = nullptr;
    bool stopped_ = false;
    std::uint64_t token_ = 0;
    std::atomic<bool> queued_{false};
};

// ---------------------------------------------------------------------------
// impl: lifecycle
// ---------------------------------------------------------------------------

epoll_backend::impl::~impl() {
    stop();
    if (timerfd_ >= 0) {
        (void)::close(timerfd_);
        timerfd_ = -1;
    }
    if (eventfd_ >= 0) {
        (void)::close(eventfd_);
        eventfd_ = -1;
    }
    if (epfd_ >= 0) {
        (void)::close(epfd_);
        epfd_ = -1;
    }
}

rpc_error epoll_backend::impl::start() noexcept {
    epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epfd_ < 0) {
        return rpc_error{rpc_code::internal,
                         "crpc epoll_backend: epoll_create1 failed: " +
                             std::generic_category().message(errno)};
    }
    eventfd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (eventfd_ < 0) {
        return rpc_error{rpc_code::internal,
                         "crpc epoll_backend: eventfd failed: " +
                             std::generic_category().message(errno)};
    }
    timerfd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd_ < 0) {
        const int e = errno;
        return rpc_error{rpc_code::internal,
                         "crpc epoll_backend: timerfd_create(CLOCK_MONOTONIC) "
                         "failed (Linux 2.6.27+ required): " +
                             std::generic_category().message(e)};
    }

    eventfd_token_ = alloc_slot(kind_eventfd, nullptr);
    timerfd_token_ = alloc_slot(kind_timerfd, nullptr);
    if (eventfd_token_ == 0 || timerfd_token_ == 0) {
        return rpc_error{rpc_code::internal,
                         "crpc epoll_backend: out of connection slots"};
    }
    if (!add_fd(eventfd_, eventfd_token_, EPOLLIN | EPOLLET)) {
        return rpc_error{rpc_code::internal,
                         "crpc epoll_backend: registering the doorbell failed"};
    }
    if (!add_fd(timerfd_, timerfd_token_, EPOLLIN | EPOLLET)) {
        return rpc_error{rpc_code::internal,
                         "crpc epoll_backend: registering the timerfd failed"};
    }

    running_.store(true, std::memory_order_release);
    loop_ = std::thread([this] { run(); });
    return rpc_error{};
}

void epoll_backend::impl::stop() noexcept {
    const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
    if (was_running) {
        wake();
    }
    if (loop_.joinable()) {
        loop_.join();
    }
}

void epoll_backend::impl::wake() noexcept {
    if (eventfd_ < 0) {
        return;
    }
    const std::uint64_t one = 1;
    const ssize_t r = ::write(eventfd_, &one, sizeof(one));
    (void)r;
}

void epoll_backend::impl::request_service(std::uint64_t token) noexcept {
    {
        std::lock_guard<std::mutex> lk(work_mutex_);
        work_tokens_.push_back(token);
    }
    wake();
}

// ---------------------------------------------------------------------------
// impl: registry
// ---------------------------------------------------------------------------

std::uint64_t epoll_backend::impl::alloc_slot(std::uint32_t kind,
                                              void* ptr) noexcept {
    std::lock_guard<std::mutex> lk(reg_mutex_);
    std::uint32_t index = 0;
    if (!free_slots_.empty()) {
        index = free_slots_.back();
        free_slots_.pop_back();
    } else {
        if (slots_.size() >= 0x7fffffffull) {
            return 0;
        }
        index = static_cast<std::uint32_t>(slots_.size());
        slots_.emplace_back();
    }
    slot& s = slots_[index];
    s.kind = kind;
    s.ptr = ptr;
    s.generation = next_generation_++;
    if (next_generation_ == 0) {
        next_generation_ = 1;
    }
    return make_token(index, s.generation);
}

void epoll_backend::impl::free_slot(std::uint64_t token,
                                    std::uint32_t expect_kind) noexcept {
    std::lock_guard<std::mutex> lk(reg_mutex_);
    const std::uint32_t index = token_index(token);
    if (index >= slots_.size()) {
        return;
    }
    slot& s = slots_[index];
    if (s.generation != token_generation(token) || s.kind != expect_kind) {
        return;
    }
    s.kind = kind_free;
    s.ptr = nullptr;
    s.generation = next_generation_++;
    if (next_generation_ == 0) {
        next_generation_ = 1;
    }
    free_slots_.push_back(index);
}

bool epoll_backend::impl::add_fd(int fd, std::uint64_t token,
                                 std::uint32_t events) noexcept {
    epoll_event ev{};
    ev.events = events;
    ev.data.u64 = token;
    return ::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) == 0;
}

void epoll_backend::impl::detach_conn(epoll_connection* c) noexcept {
    const int fd = c->fd();
    const std::uint64_t tok = c->token();
    if (fd >= 0 && epfd_ >= 0) {
        (void)::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
    }
    free_slot(tok, kind_conn);
    conn_count_.fetch_sub(1, std::memory_order_acq_rel);
}

epoll_connection* epoll_backend::impl::make_connection(
    int fd, endpoint remote) noexcept {
    if (conn_count_.load(std::memory_order_acquire) >= opt.max_connections) {
        return nullptr;
    }
    auto* c =
        new (std::nothrow) epoll_connection(*this, fd, std::move(remote), opt);
    if (c == nullptr || !c->valid()) {
        delete c;
        return nullptr;
    }
    const std::uint64_t tok = alloc_slot(kind_conn, c);
    if (tok == 0) {
        delete c;
        return nullptr;
    }
    c->set_token(tok);
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLRDHUP;
    ev.data.u64 = tok;
    if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) != 0) {
        free_slot(tok, kind_conn);
        delete c;
        return nullptr;
    }
    conn_count_.fetch_add(1, std::memory_order_acq_rel);
    return c;
}

// ---------------------------------------------------------------------------
// impl: timers
// ---------------------------------------------------------------------------

void epoll_backend::impl::rearm_timerfd_locked() noexcept {
    if (timerfd_ < 0) {
        return;
    }
    itimerspec its{};
    if (!by_deadline_.empty()) {
        const std::uint64_t dl = by_deadline_.begin()->first;
        its.it_value.tv_sec = static_cast<time_t>(dl / 1000000000ull);
        its.it_value.tv_nsec = static_cast<long>(dl % 1000000000ull);
    }
    (void)::timerfd_settime(timerfd_, TFD_TIMER_ABSTIME, &its, nullptr);
}

timer_id epoll_backend::impl::do_arm_timer(std::chrono::nanoseconds delay,
                                           timer_callback fn,
                                           void* arg) noexcept {
    if (fn == nullptr || delay.count() <= 0) {
        return invalid_timer;
    }
    if (!running_.load(std::memory_order_acquire)) {
        return invalid_timer;
    }
    const std::uint64_t now = monotonic_now_ns();
    const std::uint64_t deadline =
        now + static_cast<std::uint64_t>(delay.count());
    timer_id id = 0;
    {
        std::lock_guard<std::mutex> lk(timer_mutex_);
        id = next_timer_id_++;
        by_deadline_.emplace(deadline, timer_entry{id, deadline, fn, arg});
        id_to_deadline_.emplace(id, deadline);
        rearm_timerfd_locked();
    }
    wake();
    return id;
}

bool epoll_backend::impl::do_cancel_timer(timer_id id) noexcept {
    std::lock_guard<std::mutex> lk(timer_mutex_);
    auto found = id_to_deadline_.find(id);
    if (found == id_to_deadline_.end()) {
        return false;
    }
    const std::uint64_t dl = found->second;
    auto range = by_deadline_.equal_range(dl);
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second.id == id) {
            by_deadline_.erase(it);
            break;
        }
    }
    id_to_deadline_.erase(found);
    rearm_timerfd_locked();
    return true;
}

void epoll_backend::impl::process_timers() noexcept {
    std::vector<timer_entry> expired;
    {
        std::lock_guard<std::mutex> lk(timer_mutex_);
        const std::uint64_t now = monotonic_now_ns();
        auto it = by_deadline_.begin();
        while (it != by_deadline_.end() && it->first <= now) {
            expired.push_back(it->second);
            id_to_deadline_.erase(it->second.id);
            it = by_deadline_.erase(it);
        }
        rearm_timerfd_locked();
    }
    for (timer_entry& e : expired) {
        e.fn(e.arg);
    }
}

// ---------------------------------------------------------------------------
// impl: loop
// ---------------------------------------------------------------------------

void epoll_backend::impl::run() noexcept {
    loop_tid_.store(std::this_thread::get_id(), std::memory_order_release);
    const std::size_t cap = opt.max_events != 0 ? opt.max_events : 256;
    std::vector<epoll_event> events(cap);
    while (running_.load(std::memory_order_acquire)) {
        const int n = ::epoll_wait(epfd_, events.data(),
                                   static_cast<int>(events.size()), -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        for (int i = 0; i < n; ++i) {
            dispatch(events[static_cast<std::size_t>(i)]);
        }
        process_timers();
        drain_work();
    }
}

void epoll_backend::impl::dispatch(const epoll_event& ev) noexcept {
    const std::uint64_t tok = ev.data.u64;
    const std::uint32_t index = token_index(tok);
    const std::uint32_t generation = token_generation(tok);

    std::uint32_t kind = kind_free;
    void* ptr = nullptr;
    epoll_connection* conn = nullptr;
    {
        std::lock_guard<std::mutex> lk(reg_mutex_);
        if (index >= slots_.size()) {
            return;
        }
        slot& s = slots_[index];
        if (s.kind == kind_free || s.generation != generation) {
            return; // stale event for a closed/recycled slot
        }
        kind = s.kind;
        ptr = s.ptr;
        if (kind == kind_conn && ptr != nullptr) {
            conn = static_cast<epoll_connection*>(ptr);
            conn->cb_inflight_.fetch_add(1, std::memory_order_acquire);
        }
    }

    switch (kind) {
    case kind_conn:
        if (conn != nullptr) {
            conn->on_event(ev.events);
            conn->cb_inflight_.fetch_sub(1, std::memory_order_release);
        }
        break;
    case kind_listener:
        if (ptr != nullptr) {
            static_cast<listener_impl*>(ptr)->on_acceptable();
        }
        break;
    case kind_eventfd: {
        std::uint64_t v = 0;
        while (::read(eventfd_, &v, sizeof(v)) ==
               static_cast<ssize_t>(sizeof(v))) {
            v = 0;
        }
        break;
    }
    case kind_timerfd: {
        std::uint64_t v = 0;
        const ssize_t r = ::read(timerfd_, &v, sizeof(v));
        (void)r;
        break;
    }
    default:
        break;
    }
}

void epoll_backend::impl::drain_work() noexcept {
    std::vector<std::uint64_t> local;
    {
        std::lock_guard<std::mutex> lk(work_mutex_);
        local.swap(work_tokens_);
    }
    for (const std::uint64_t tok : local) {
        const std::uint32_t index = token_index(tok);
        const std::uint32_t generation = token_generation(tok);
        std::uint32_t kind = kind_free;
        void* ptr = nullptr;
        epoll_connection* conn = nullptr;
        {
            std::lock_guard<std::mutex> lk(reg_mutex_);
            if (index >= slots_.size()) {
                continue;
            }
            slot& s = slots_[index];
            if (s.kind == kind_free || s.generation != generation) {
                continue;
            }
            kind = s.kind;
            ptr = s.ptr;
            if (kind == kind_conn && ptr != nullptr) {
                conn = static_cast<epoll_connection*>(ptr);
                conn->queued_.store(false, std::memory_order_release);
                conn->cb_inflight_.fetch_add(1, std::memory_order_acquire);
            }
        }
        if (kind == kind_conn && conn != nullptr) {
            conn->service();
            conn->cb_inflight_.fetch_sub(1, std::memory_order_release);
        } else if (kind == kind_listener && ptr != nullptr) {
            auto* l = static_cast<listener_impl*>(ptr);
            l->queued_.store(false, std::memory_order_release);
            l->on_acceptable();
        }
    }
}

void epoll_backend::impl::schedule_accept_backoff(
    std::uint64_t token) noexcept {
    std::lock_guard<std::mutex> lk(accept_mutex_);
    if (std::find(accept_backoff_.begin(), accept_backoff_.end(), token) ==
        accept_backoff_.end()) {
        accept_backoff_.push_back(token);
    }
    if (accept_backoff_timer_ == invalid_timer) {
        accept_backoff_timer_ = do_arm_timer(std::chrono::milliseconds(2),
                                             &impl::accept_backoff_tick, this);
    }
}

void epoll_backend::impl::accept_backoff_tick(void* arg) noexcept {
    auto* self = static_cast<impl*>(arg);
    std::vector<std::uint64_t> tokens;
    {
        std::lock_guard<std::mutex> lk(self->accept_mutex_);
        tokens.swap(self->accept_backoff_);
        self->accept_backoff_timer_ = invalid_timer;
    }
    for (const std::uint64_t token : tokens) {
        self->request_service(token);
    }
}

// ---------------------------------------------------------------------------
// impl: connect / listen
// ---------------------------------------------------------------------------

namespace {

int open_connected_socket(const std::string& host, std::uint16_t port,
                          const connect_options& o) noexcept {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_NUMERICSERV;
    addrinfo* res = nullptr;
    const std::string port_str = std::to_string(port);
    if (::getaddrinfo(host.empty() ? nullptr : host.c_str(), port_str.c_str(),
                      &hints, &res) != 0) {
        return -1;
    }
    const int timeout_ms = static_cast<int>(std::max<std::int64_t>(
        1, std::min<std::int64_t>(o.connect_timeout.count() / 1000000,
                                  INT_MAX)));
    int fd = -1;
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                      ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno == EINTR) {
            rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        }
        if (rc == 0) {
            break;
        }
        if (errno == EINPROGRESS) {
            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;
            const int pr = ::poll(&pfd, 1, timeout_ms);
            if (pr > 0) {
                int soerr = 0;
                socklen_t len = sizeof(soerr);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) == 0 &&
                    soerr == 0) {
                    break; // connected
                }
            }
        }
        (void)::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd < 0) {
        return -1;
    }
    set_socket_buffers(fd, o.recv_buffer_bytes, o.send_buffer_bytes);
    if (o.tcp_no_delay) {
        int one = 1;
        (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    return fd;
}

int open_listen_socket(const std::string& host, std::uint16_t port,
                       const listen_options& o) noexcept {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    addrinfo* res = nullptr;
    const std::string port_str = std::to_string(port);
    if (::getaddrinfo(host.empty() ? nullptr : host.c_str(), port_str.c_str(),
                      &hints, &res) != 0) {
        return -1;
    }
    int fd = -1;
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                      ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        int one = 1;
        (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (o.reuse_port) {
            (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
        }
        set_socket_buffers(fd, o.recv_buffer_bytes, o.send_buffer_bytes);
        const int backlog = static_cast<int>(
            std::min<std::size_t>(o.backlog, static_cast<std::size_t>(INT_MAX)));
        if (::bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 &&
            ::listen(fd, backlog == 0 ? 1 : backlog) == 0) {
            break;
        }
        (void)::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    return fd;
}

} // namespace

std::unique_ptr<connection> epoll_backend::impl::do_connect(
    const endpoint& ep, const connect_options& o) noexcept {
    if (ep.type() != endpoint::kind::tcp) {
        return nullptr;
    }
    const int fd = open_connected_socket(ep.host(), ep.port(), o);
    if (fd < 0) {
        return nullptr;
    }
    epoll_connection* c = make_connection(fd, ep);
    if (c == nullptr) {
        (void)::close(fd);
        return nullptr;
    }
    return std::unique_ptr<connection>(c);
}

std::unique_ptr<listener> epoll_backend::impl::do_listen(
    const endpoint& ep, const listen_options& o) noexcept {
    if (ep.type() != endpoint::kind::tcp) {
        return nullptr;
    }
    const int lfd = open_listen_socket(ep.host(), ep.port(), o);
    if (lfd < 0) {
        return nullptr;
    }
    auto* l = new (std::nothrow) listener_impl(*this, lfd, o);
    if (l == nullptr) {
        (void)::close(lfd);
        return nullptr;
    }
    const std::uint64_t tok = alloc_slot(kind_listener, l);
    if (tok == 0) {
        delete l; // destructor closes lfd and frees a (zero) token
        return nullptr;
    }
    l->token_ = tok;
    if (!add_fd(lfd, tok, EPOLLIN | EPOLLET)) {
        delete l;
        return nullptr;
    }
    return std::unique_ptr<listener>(l);
}

// ---------------------------------------------------------------------------
// epoll_backend public surface
// ---------------------------------------------------------------------------

epoll_backend::epoll_backend(epoll_options opt) noexcept
    : impl_(new (std::nothrow) impl(opt)) {}

epoll_backend::~epoll_backend() = default;

result<std::unique_ptr<epoll_backend>> epoll_backend::create(
    epoll_options opt) {
    std::unique_ptr<epoll_backend> backend(
        new (std::nothrow) epoll_backend(opt));
    if (!backend || !backend->impl_) {
        return rpc_error{rpc_code::internal,
                         "crpc epoll_backend: allocation failed"};
    }
    rpc_error err = backend->impl_->start();
    if (!err.ok()) {
        return err;
    }
    return backend;
}

std::unique_ptr<connection> epoll_backend::connect(
    const endpoint& ep, const connect_options& opt) {
    if (!impl_) {
        return nullptr;
    }
    return impl_->do_connect(ep, opt);
}

std::unique_ptr<listener> epoll_backend::listen(const endpoint& ep,
                                                const listen_options& opt) {
    if (!impl_) {
        return nullptr;
    }
    return impl_->do_listen(ep, opt);
}

timer_id epoll_backend::arm_timer(std::chrono::nanoseconds delay,
                                  timer_callback fn, void* arg) noexcept {
    if (!impl_) {
        return invalid_timer;
    }
    return impl_->do_arm_timer(delay, fn, arg);
}

bool epoll_backend::cancel_timer(timer_id id) noexcept {
    if (!impl_) {
        return false;
    }
    return impl_->do_cancel_timer(id);
}

const epoll_options& epoll_backend::options() const noexcept {
    static const epoll_options k_default{};
    return impl_ ? impl_->opt : k_default;
}

std::uint64_t epoll_backend::r4_self_post_count() const noexcept {
    return impl_ ? impl_->r4_self_posts_.load(std::memory_order_relaxed) : 0;
}

std::uint64_t epoll_backend::connection_count() const noexcept {
    return impl_ ? impl_->conn_count_.load(std::memory_order_relaxed) : 0;
}

} // namespace io
} // namespace crpc

#endif // CRPC_OS_LINUX
