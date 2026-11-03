// CRPC -- Windows IOCP transport backend implementation.
//
// This file is compiled only on Windows; on every other target it is an empty
// translation unit.  The design is summarised in the public header; the notes
// here cover the parts that only matter to the implementation.
//
// Threading and lifetime
// ----------------------
// Every connection has a `conn_state` whose lifetime is owned jointly by the
// caller's `iocp_connection` and by an entry in `impl::conns`.  The registry
// entry is erased only after the socket has been closed and every outstanding
// operation has been observed.  An operation only ever runs on the pump thread,
// so the state a completion refers to is always alive when it is dispatched.
// The user object may be destroyed at any time; its destructor merely
// *requests* a close.
//
// The connection state machine is `open -> closing -> closed`.  `close()` is
// only a request: it cancels the outstanding operations, and finalisation (the
// `DisconnectEx`/`closesocket` step) runs once no operation is in flight.  A
// canceled operation still produces exactly one completion and is translated to
// `operation_canceled`, never dropped.
//
// Read delivery
// -------------
// There is at most one outstanding `WSARecv` per connection.  When it completes
// the bytes land in the connection-owned receive buffer and are handed to the
// framework source.  A short or zero return pauses delivery and retains the
// tail; only `resume_read()` restarts it.  Delivery runs under a single-writer
// `delivering` flag, exactly like the in-process backend's drain loop, so a
// re-entrant `resume_read` from inside `on_data` cannot recurse.  Demands for
// delivery (a resume, a first `bind_source`, a completed receive) are counted
// in `pending_wakes`; a delivery pass consumes them under the connection mutex
// in the same critical section that releases `delivering`, so a wake can never
// fall into an ownerless gap between "decided to stop" and "flag released".  A
// paused reader keeps no completion outstanding, so the pump blocks instead of
// spinning; a zero-byte completion is EOF and never re-posts a read.
//
// Write path
// ----------
// `push` appends a borrowed span to a bounded queue and starts a `WSASend` for
// the head when none is in flight.  A partial write re-posts the remainder of
// the same chunk; the chunk completes exactly once, when all of its bytes have
// been accepted or when the connection fails or is canceled.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#if defined(_WIN32)

#include <crpc/error.hpp>
#include <crpc/io/iocp_backend.hpp>

#include "bound_guard.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace crpc {
namespace {


std::error_code map_wsa_error(unsigned long code) noexcept {
    switch (code) {
    case ERROR_OPERATION_ABORTED: // 995, cancel of an overlapped operation
        return std::make_error_code(std::errc::operation_canceled);
    case ERROR_CONNECTION_ABORTED: // 1236
        return std::make_error_code(std::errc::connection_aborted);
    case ERROR_HANDLE_EOF: // 38, peer closed its side
        return make_error_code(rpc_code::eof);
    case WSAECONNRESET:
        return std::make_error_code(std::errc::connection_reset);
    case WSAECONNREFUSED:
        return std::make_error_code(std::errc::connection_refused);
    case WSAENETUNREACH:
    case WSAEHOSTUNREACH:
        return std::make_error_code(std::errc::host_unreachable);
    case WSAENOBUFS:
        return make_error_code(rpc_code::resource_exhausted);
    case WSAENOTCONN:
        return std::make_error_code(std::errc::not_connected);
    case WSAESHUTDOWN:
    case WSAECONNABORTED:
        return std::make_error_code(std::errc::broken_pipe);
    default:
        return std::error_code(static_cast<int>(code), std::system_category());
    }
}

// --- Operation records -------------------------------------------------------

enum class op_kind : std::uint8_t { none, recv, send, accept, disconnect };

// The OVERLAPPED must be the first member: the pump recovers the record from
// the pointer the kernel hands back with CONTAINING_RECORD.  Records live in a
// per-backend pool, so posting an operation never allocates on the hot path.
struct overlapped_op {
    OVERLAPPED ov;
    op_kind kind;
    void* owner; // conn_state*, accept_slot* ...
    // Registry id of the owning connection (0 for accept operations). Stored
    // here because `owner` may already be freed when the completion is
    // dispatched: an earlier completion ran a user callback that closed and
    // unregistered the connection, leaving later queued completions pointing at
    // freed memory. The id is a plain value, so it stays readable, and it lets
    // `dispatch` take a strong reference before touching the state.
    std::uint64_t conn_id;
    std::size_t got;
    unsigned long error;
};
static_assert(offsetof(overlapped_op, ov) == 0,
              "OVERLAPPED must be the first member of overlapped_op");

class op_pool {
public:
    explicit op_pool(std::size_t initial) {
        free_.reserve(initial);
        owned_.reserve(initial);
        for (std::size_t i = 0; i < initial; ++i) {
            overlapped_op* p = new (std::nothrow) overlapped_op();
            if (p == nullptr) {
                break;
            }
            owned_.push_back(p);
            free_.push_back(p);
        }
    }

    ~op_pool() {
        for (overlapped_op* p : owned_) {
            delete p;
        }
    }

    op_pool(const op_pool&) = delete;
    op_pool& operator=(const op_pool&) = delete;

    overlapped_op* acquire() noexcept {
        std::lock_guard<std::mutex> lk(m_);
        if (!free_.empty()) {
            overlapped_op* p = free_.back();
            free_.pop_back();
            return p;
        }
        overlapped_op* p = new (std::nothrow) overlapped_op();
        if (p != nullptr) {
            owned_.push_back(p);
        }
        return p;
    }

    void release(overlapped_op* op) noexcept {
        std::lock_guard<std::mutex> lk(m_);
        free_.push_back(op);
    }

private:
    std::mutex m_;
    std::vector<overlapped_op*> owned_;
    std::vector<overlapped_op*> free_;
};

// --- Connection state --------------------------------------------------------

struct send_chunk {
    const std::byte* data;
    std::size_t size;
    std::size_t sent;
    io::completion* c;
};

enum class conn_phase : std::uint8_t { open, closing, closed };

struct conn_state {
    iocp_backend::impl* backend = nullptr;
    std::uint64_t id = 0;
    SOCKET sock = INVALID_SOCKET;
    endpoint remote_ep;
    io::detail::bound_guard bound;

    std::mutex m;
    conn_phase phase = conn_phase::open;
    bool recv_inflight = false;
    bool send_inflight = false;
    bool disc_inflight = false;
    bool disconnecting = false;
    bool paused = false;
    bool eof_delivered = false;
    bool error_delivered = false;

    std::unique_ptr<std::byte[]> rx;
    std::size_t rx_cap = 0;
    std::size_t rx_off = 0;
    std::size_t rx_got = 0;

    std::deque<send_chunk> sendq;
    std::size_t queued_bytes = 0;
    std::size_t send_limit = 0;

    overlapped_op* recv_op = nullptr;
    overlapped_op* send_op = nullptr;
    overlapped_op* disc_op = nullptr;

    std::atomic<bool> delivering{false};

    // Number of queued delivery requests that no delivery attempt has
    // fulfilled yet. `resume_read()` and `bind_source()` queue one when (and
    // only when) retained bytes are waiting, and a completed receive queues one
    // for its fresh bytes; all under `m`. The delivery loop fulfils a request
    // with an actual delivery attempt and may not stop while one is visible:
    // its single exit decision runs under `m` in the same critical section
    // that releases `delivering`, consuming a visible request and continuing
    // instead. A wake arriving after that critical section finds
    // `delivering == false` and runs its own pass, so no wake can be swallowed
    // by a pass that already decided to stop, and none is left lingering after
    // the flag is released. This replaces an earlier "generation" counter: a
    // generation captured after the wake made the wake invisible to the pass,
    // and a pass that observed only the stale `paused` flag stopped with data
    // retained forever.
    std::uint64_t pending_wakes = 0;

    // Set while a send completion is still owned by the backend (posted but its
    // user callback has not run yet). `close()` waits for it so a caller that
    // destroys the completion object after close() returns can never race the
    // callback: the backend borrows the completion, it does not own it.
    std::condition_variable delivery_cv;
    bool send_delivery_done = true;
};

// --- Listener state ----------------------------------------------------------

struct listener_state;

struct accept_slot {
    listener_state* owner = nullptr;
    overlapped_op* op = nullptr;
    SOCKET sock = INVALID_SOCKET;
    std::unique_ptr<std::byte[]> addr;
};

struct listener_state {
    iocp_backend::impl* backend = nullptr;
    std::uint64_t id = 0;
    SOCKET lsock = INVALID_SOCKET;
    int family = AF_UNSPEC;
    accept_sink* sink = nullptr;
    std::atomic<bool> started{false};
    std::atomic<bool> stopping{false};
    std::atomic<bool> closed{false};
    std::atomic<std::size_t> pending{0};
    std::vector<std::unique_ptr<accept_slot>> slots;
    std::mutex m;
};

// --- Control completions -----------------------------------------------------

struct control_node {
    enum class kind : std::uint8_t { stop, timers } k;
};

// --- Timers ------------------------------------------------------------------

struct timer_slot {
    std::uint64_t id = 0;
    std::uint64_t deadline_ns = 0;
    iocp_backend::timer_callback fn = nullptr;
    void* arg = nullptr;
    bool canceled = false;
};

struct timer_heap_cmp {
    bool operator()(const timer_slot* a, const timer_slot* b) const noexcept {
        return a->deadline_ns > b->deadline_ns;
    }
};

std::uint64_t monotonic_now_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

timeval to_timeval(std::chrono::nanoseconds timeout) noexcept {
    timeval tv{};
    if (timeout < std::chrono::nanoseconds::zero()) {
        timeout = std::chrono::nanoseconds::zero();
    }
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    const auto usecs =
        std::chrono::duration_cast<std::chrono::microseconds>(timeout - secs);
    tv.tv_sec = static_cast<long>(secs.count());
    tv.tv_usec = static_cast<long>(usecs.count());
    if (tv.tv_sec == 0 && tv.tv_usec == 0) {
        tv.tv_usec = 1;
    }
    return tv;
}

// --- Helpers -----------------------------------------------------------------

constexpr std::size_t k_addr_len = sizeof(sockaddr_storage) + 16;

endpoint endpoint_from_sockaddr(const sockaddr* sa) noexcept {
    if (sa == nullptr) {
        return endpoint::tcp(std::string(), 0);
    }
    char host[NI_MAXHOST] = {0};
    char serv[NI_MAXSERV] = {0};
    const int len = sa->sa_family == AF_INET6
                        ? static_cast<int>(sizeof(sockaddr_in6))
                        : static_cast<int>(sizeof(sockaddr_in));
    if (::getnameinfo(sa, static_cast<socklen_t>(len), host, sizeof(host), serv,
                      sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return endpoint::tcp(std::string(), 0);
    }
    const unsigned long port = std::strtoul(serv, nullptr, 10);
    return endpoint::tcp(std::string(host), static_cast<u16>(port));
}

// Reads the result of a completed operation.  `entry_bytes` is the kernel's
// count, used only when the socket is already gone.
void read_op_result(SOCKET sock, overlapped_op* op, DWORD entry_bytes,
                    std::size_t* bytes_out, unsigned long* error_out) noexcept {
    if (sock != INVALID_SOCKET) {
        DWORD flags = 0;
        DWORD bytes = 0;
        if (::WSAGetOverlappedResult(sock, &op->ov, &bytes, FALSE, &flags)) {
            *bytes_out = static_cast<std::size_t>(bytes);
            *error_out = 0;
            return;
        }
        *bytes_out = 0;
        *error_out = static_cast<unsigned long>(::WSAGetLastError());
        return;
    }
    *bytes_out = static_cast<std::size_t>(entry_bytes);
    *error_out = ERROR_OPERATION_ABORTED;
}

// --- Forward declarations ----------------------------------------------------

void initiate_close(conn_state* st) noexcept;
void finalize_close(conn_state* st) noexcept;
void hard_close(conn_state* st) noexcept;
bool post_recv(conn_state* st) noexcept;
bool post_send(conn_state* st) noexcept;
void deliver_read(conn_state* st) noexcept;
void handle_read_error(conn_state* st, unsigned long wsa_error) noexcept;
void handle_recv_complete(overlapped_op* op, std::size_t bytes,
                          unsigned long error) noexcept;
void handle_send_complete(overlapped_op* op, std::size_t bytes,
                          unsigned long error) noexcept;
void handle_disconnect_complete(overlapped_op* op) noexcept;
bool post_accept(accept_slot* slot) noexcept;
void handle_accept(overlapped_op* op) noexcept;
void finalize_listener(listener_state* ls) noexcept;
void push_impl(conn_state* st, span<const std::byte> data,
               io::completion* c) noexcept;
std::unique_ptr<connection> make_iocp_connection(
    std::shared_ptr<conn_state> st) noexcept;

} // namespace

// --- Backend implementation --------------------------------------------------

// `impl` is a member of the public class, so it must be defined in `crpc` and
// not in the unnamed namespace.  Its methods are defined below; they can still
// use the unnamed-namespace helpers above by ordinary name lookup.

struct iocp_backend::impl {
    iocp_options opt;
    HANDLE iocp = nullptr;
    std::atomic<bool> stopping{false};
    std::atomic<bool> stopped{false};
    std::atomic<bool> valid{false};
    std::atomic<bool> wsa_started{false};
    std::thread pump_thread;
    std::atomic<DWORD> pump_tid{0};

    op_pool pool;

    std::mutex registry_mutex;
    std::unordered_map<std::uint64_t, std::shared_ptr<conn_state>> conns;
    std::unordered_map<std::uint64_t, std::shared_ptr<listener_state>> listeners;
    std::atomic<std::uint64_t> next_conn_id{1};
    std::atomic<std::uint64_t> next_listener_id{1};

    LPFN_ACCEPTEX accept_ex = nullptr;
    LPFN_DISCONNECTEX disconnect_ex = nullptr;

    control_node stop_node{control_node::kind::stop};
    control_node timer_node{control_node::kind::timers};

    HANDLE timer_handle = nullptr;
    HANDLE timer_wake = nullptr;
    // Set when the high-resolution waitable timer was created; cleared when the
    // host predates Windows 10 1803 and the standard timer is used instead.
    std::atomic<bool> high_resolution_timer{false};
    std::thread timer_thread;
    std::atomic<bool> timer_stopping{false};
    std::mutex tmutex;
    std::vector<timer_slot*> heap;
    std::vector<timer_slot*> ready;
    std::vector<std::unique_ptr<timer_slot>> timer_storage;
    std::vector<timer_slot*> timer_free;
    std::unordered_map<std::uint64_t, timer_slot*> live;
    std::atomic<std::uint64_t> next_timer_id{1};

    explicit impl(iocp_options options);
    ~impl();

    impl(const impl&) = delete;
    impl& operator=(const impl&) = delete;

    void init() noexcept;
    void stop() noexcept;

    bool associate(SOCKET sock) noexcept;
    std::shared_ptr<conn_state> make_connection(SOCKET sock, endpoint remote,
                                                std::size_t recv_cap,
                                                std::size_t send_limit,
                                                std::size_t sndbuf,
                                                bool no_delay) noexcept;

    std::unique_ptr<connection> do_connect(const endpoint& ep,
                                           const connect_options& co);
    std::unique_ptr<listener> do_listen(const endpoint& ep,
                                        const listen_options& lo);

    void pump_loop() noexcept;
    void dispatch(OVERLAPPED_ENTRY& entry) noexcept;
    void dispatch_control(control_node* node) noexcept;

    timer_slot* acquire_timer_slot() noexcept;
    std::uint64_t do_arm_timer(std::chrono::nanoseconds delay,
                               iocp_backend::timer_callback fn, void* arg) noexcept;
    bool do_cancel_timer(std::uint64_t id) noexcept;
    void timer_loop() noexcept;
    void drain_timers() noexcept;

    // Returns a strong reference to the connection state, or null when it is
    // already gone. Completion handlers take one for their whole run so a user
    // callback that closes the connection cannot free the state the handler is
    // still using (the callbacks run before the finalisation step).
    [[nodiscard]] std::shared_ptr<conn_state> retain_conn(std::uint64_t id) noexcept;
    void unregister_conn(std::uint64_t id) noexcept;
    void unregister_listener(std::uint64_t id) noexcept;
};

namespace {

// --- Connection operations ---------------------------------------------------

void initiate_close(conn_state* st) noexcept {
    std::vector<io::completion*> canceled;
    bool need_finalize = false;
    bool send_delivery_pending = false;
    {
        std::lock_guard<std::mutex> lk(st->m);
        if (st->phase != conn_phase::open) {
            return;
        }
        st->phase = conn_phase::closing;
        // `send_inflight` is cleared by `handle_send_complete` at the top of the
        // completion, *before* it runs the user callback, so `send_inflight ==
        // false` does not mean the borrowed completion is no longer in use.
        // `send_delivery_done` is the flag that becomes true only after that
        // callback has returned; it is the one that must gate the wait.
        send_delivery_pending = !st->send_delivery_done;
        if (st->send_inflight) {
            // Keep the chunk whose WSASend is outstanding; complete the rest.
            while (st->sendq.size() > 1) {
                send_chunk chunk = st->sendq[1];
                st->queued_bytes -= chunk.size;
                st->sendq.erase(st->sendq.begin() + 1);
                canceled.push_back(chunk.c);
            }
        } else {
            while (!st->sendq.empty()) {
                send_chunk chunk = st->sendq.front();
                st->queued_bytes -= chunk.size;
                st->sendq.pop_front();
                canceled.push_back(chunk.c);
            }
        }
        need_finalize = !st->recv_inflight && !st->send_inflight;
    }

    if (st->sock != INVALID_SOCKET) {
        ::CancelIoEx(reinterpret_cast<HANDLE>(st->sock), nullptr);
    }
    for (io::completion* c : canceled) {
        if (c != nullptr) {
            c->complete(
                io::io_result{std::make_error_code(std::errc::operation_canceled), 0});
        }
    }

    // The completion passed to `push` is borrowed, not owned, so a caller may
    // destroy it as soon as `close()` returns. The pump clears `send_inflight`
    // before it runs the user callback, so without this wait `close()` could
    // return while the callback is still about to run on a completion the caller
    // already freed (a real access violation, reproduced once per ~30 runs).
    // The condition is `send_delivery_pending`, not `send_inflight`: a send that
    // already completed at the kernel level but is still inside its user
    // callback must be waited for too, otherwise the caller frees the
    // completion while the pump is still touching it.
    //
    // The wait is skipped on the pump thread itself, where a completion callback
    // may legitimately close the connection; it is bounded only while the backend
    // is shutting down, because the pump may have exited without delivering.
    if (send_delivery_pending && st->backend != nullptr) {
        const DWORD self = ::GetCurrentThreadId();
        if (self != st->backend->pump_tid.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lk(st->m);
            if (st->backend->stopping.load(std::memory_order_acquire)) {
                st->delivery_cv.wait_for(lk, std::chrono::milliseconds(2000),
                                         [st] { return st->send_delivery_done; });
            } else {
                st->delivery_cv.wait(lk, [st] { return st->send_delivery_done; });
            }
        }
    }

    if (need_finalize) {
        finalize_close(st);
    }
}

void hard_close(conn_state* st) noexcept {
    SOCKET sock = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lk(st->m);
        if (st->phase == conn_phase::closed) {
            return;
        }
        st->phase = conn_phase::closed;
        sock = st->sock;
        st->sock = INVALID_SOCKET;
    }
    if (sock != INVALID_SOCKET) {
        ::shutdown(sock, SD_BOTH);
        ::closesocket(sock);
    }
    st->backend->unregister_conn(st->id);
}

void finalize_close(conn_state* st) noexcept {
    {
        std::lock_guard<std::mutex> lk(st->m);
        if (st->phase == conn_phase::closed || st->disconnecting ||
            st->disc_inflight) {
            return;
        }
        if (st->recv_inflight || st->send_inflight) {
            return;
        }
        st->phase = conn_phase::closing;
        st->disconnecting = true;
    }

    iocp_backend::impl* be = st->backend;
    if (be->disconnect_ex != nullptr && st->sock != INVALID_SOCKET &&
        !st->backend->stopping.load(std::memory_order_acquire)) {
        st->disc_op->ov = OVERLAPPED{};
        st->disc_op->kind = op_kind::disconnect;
        st->disc_op->owner = st;
        st->disc_op->conn_id = st->id;
        const BOOL ok = be->disconnect_ex(st->sock, &st->disc_op->ov,
                                          TF_REUSE_SOCKET, 0);
        if (!ok) {
            const int e = ::WSAGetLastError();
            if (e != WSA_IO_PENDING) {
                std::lock_guard<std::mutex> lk(st->m);
                st->disconnecting = false;
            } else {
                std::lock_guard<std::mutex> lk(st->m);
                st->disc_inflight = true;
            }
        } else {
            std::lock_guard<std::mutex> lk(st->m);
            st->disc_inflight = true;
        }
    } else {
        hard_close(st);
    }
}

void handle_disconnect_complete(overlapped_op* op) noexcept {
    conn_state* st = static_cast<conn_state*>(op->owner);
    {
        std::lock_guard<std::mutex> lk(st->m);
        st->disc_inflight = false;
    }
    hard_close(st);
}

bool post_recv(conn_state* st) noexcept {
    char* buf = nullptr;
    DWORD cap = 0;
    {
        std::lock_guard<std::mutex> lk(st->m);
        if (st->phase != conn_phase::open || st->recv_inflight || st->paused ||
            st->rx_off != st->rx_got) {
            return true;
        }
        st->recv_inflight = true;
        buf = reinterpret_cast<char*>(st->rx.get());
        cap = static_cast<DWORD>(st->rx_cap);
        st->recv_op->ov = OVERLAPPED{};
        st->recv_op->kind = op_kind::recv;
        st->recv_op->owner = st;
        st->recv_op->conn_id = st->id;
    }

    WSABUF wsb{};
    wsb.buf = buf;
    wsb.len = cap;
    DWORD flags = 0;
    DWORD received = 0;
    const int r = ::WSARecv(st->sock, &wsb, 1, &received, &flags,
                            &st->recv_op->ov, nullptr);
    if (r == SOCKET_ERROR) {
        const int e = ::WSAGetLastError();
        if (e != WSA_IO_PENDING) {
            {
                std::lock_guard<std::mutex> lk(st->m);
                st->recv_inflight = false;
            }
            handle_read_error(st, static_cast<unsigned long>(e));
            return false;
        }
    }
    return true;
}

bool post_send(conn_state* st) noexcept {
    WSABUF wsb{};
    {
        std::lock_guard<std::mutex> lk(st->m);
        if (st->phase != conn_phase::open || st->send_inflight ||
            st->sendq.empty()) {
            return true;
        }
        send_chunk& front = st->sendq.front();
        const std::size_t remaining = front.size - front.sent;
        if (remaining == 0) {
            return true;
        }
        st->send_inflight = true;
        wsb.buf = const_cast<char*>(
            reinterpret_cast<const char*>(front.data + front.sent));
        wsb.len = static_cast<ULONG>(remaining);
        st->send_op->ov = OVERLAPPED{};
        st->send_op->kind = op_kind::send;
        st->send_op->owner = st;
        st->send_op->conn_id = st->id;
        st->send_delivery_done = false;
    }

    DWORD sent = 0;
    const int r = ::WSASend(st->sock, &wsb, 1, &sent, 0, &st->send_op->ov,
                            nullptr);
    if (r == SOCKET_ERROR) {
        const int e = ::WSAGetLastError();
        if (e != WSA_IO_PENDING) {
            {
                std::lock_guard<std::mutex> lk(st->m);
                st->send_inflight = false;
            }
            handle_send_complete(st->send_op, 0,
                                 static_cast<unsigned long>(e));
            return false;
        }
    }
    return true;
}

void handle_read_error(conn_state* st, unsigned long wsa_error) noexcept {
    bool first = false;
    {
        std::lock_guard<std::mutex> lk(st->m);
        if (!st->error_delivered) {
            st->error_delivered = true;
            first = true;
        }
    }
    if (!first) {
        return;
    }
    if (io::source* s = st->bound.enter()) {
        s->on_error(map_wsa_error(wsa_error));
        st->bound.leave();
    }
    initiate_close(st);
}

void deliver_read(conn_state* st) noexcept {
    if (st->delivering.exchange(true, std::memory_order_acq_rel)) {
        // Another pass owns delivery. Its single exit decision below runs under
        // `m` and consumes a visible pending wake before it releases the flag,
        // so a wake issued now is either consumed by that pass or, arriving
        // after the release, re-enters here and owns a pass itself.
        return;
    }
    for (;;) {
        // Deliver bytes, pause, or re-arm the receive until this round cannot
        // make progress on its own.
        for (;;) {
            std::byte* ptr = nullptr;
            std::size_t avail = 0;
            bool post = false;
            bool stop = false;
            const char* why = "";
            {
                std::lock_guard<std::mutex> lk(st->m);
                if (st->phase != conn_phase::open) {
                    st->rx_off = 0;
                    st->rx_got = 0;
                    stop = true;
                    why = "phase";
                } else {
                    // A queued wake outranks the paused decision an earlier
                    // iteration made from a consumer that had no credit: the
                    // caller demanded delivery, so deliver.
                    if (st->paused && st->pending_wakes != 0) {
                        --st->pending_wakes;
                        st->paused = false;
                    }
                    if (st->rx_off < st->rx_got) {
                        if (st->paused) {
                            stop = true;
                            why = "paused";
                        } else if (!st->bound.active()) {
                            st->paused = true;
                            stop = true;
                            why = "unbound";
                        } else {
                            // This attempt fulfils one queued wake, if any: a
                            // request means "try to deliver", and a try the
                            // consumer rejects is still fulfilled.
                            if (st->pending_wakes != 0) {
                                --st->pending_wakes;
                            }
                            ptr = st->rx.get() + st->rx_off;
                            avail = st->rx_got - st->rx_off;
                            why = "deliver";
                        }
                    } else {
                        post = true;
                        why = "post";
                    }
                }
            }

            if (stop) {
                break;
            }
            if (post) {
                post_recv(st);
                break;
            }

            io::source* s = st->bound.enter();
            if (s == nullptr) {
                std::lock_guard<std::mutex> lk(st->m);
                st->paused = true;
                break;
            }
            std::size_t consumed = s->on_data(span<const std::byte>(ptr, avail));
            st->bound.leave();
            if (consumed > avail) {
                consumed = avail; // clamp a violating return instead of overrun
            }
            {
                std::lock_guard<std::mutex> lk(st->m);
                st->rx_off += consumed;
                if (consumed < avail) {
                    st->paused = true;
                }
            }
            if (consumed < avail || consumed == 0) {
                break;
            }
        }

        // The single exit decision. Deciding to stop and releasing the
        // single-writer flag are one critical section: an unfulfilled wake is
        // either consumed here (and this pass keeps ownership, looping back
        // above without re-claiming the flag) or it lands after the release and
        // finds `delivering == false`, running its own pass. There is no
        // ownerless gap in between, and no wake is ever left queued behind a
        // released flag.
        bool again = false;
        {
            std::lock_guard<std::mutex> lk(st->m);
            if (st->phase == conn_phase::open && st->pending_wakes != 0) {
                --st->pending_wakes;
                st->paused = false;
                again = true;
            } else {
                st->delivering.store(false, std::memory_order_release);
            }
        }
        if (!again) {
            return;
        }
    }
}

void handle_recv_complete(overlapped_op* op, std::size_t bytes,
                          unsigned long error) noexcept {
    conn_state* st = static_cast<conn_state*>(op->owner);
    bool eof = false;
    bool err = false;
    bool deliver = false;
    {
        std::lock_guard<std::mutex> lk(st->m);
        st->recv_inflight = false;
        if (st->phase != conn_phase::open) {
            // Canceled by close; fall through to finalisation.
        } else if (error != 0) {
            err = true;
        } else if (bytes == 0) {
            if (!st->eof_delivered) {
                st->eof_delivered = true;
                eof = true;
            }
        } else {
            st->rx_off = 0;
            st->rx_got = bytes;
            // New bytes demand delivery exactly like a resume does; the pass
            // consumes this request under `m` before it may stop.
            ++st->pending_wakes;
            deliver = true;
        }
    }

    if (deliver) {
        deliver_read(st);
        return;
    }
    if (err) {
        handle_read_error(st, error);
        return;
    }
    if (eof) {
        if (io::source* s = st->bound.enter()) {
            s->on_eof();
            st->bound.leave();
        }
        initiate_close(st);
        return;
    }
    finalize_close(st);
}

void handle_send_complete(overlapped_op* op, std::size_t bytes,
                          unsigned long error) noexcept {
    conn_state* st = static_cast<conn_state*>(op->owner);
    std::vector<std::pair<io::completion*, io::io_result>> outcomes;
    bool post_next = false;
    {
        std::lock_guard<std::mutex> lk(st->m);
        st->send_inflight = false;
        if (!st->sendq.empty()) {
            if (st->phase != conn_phase::open) {
                while (!st->sendq.empty()) {
                    send_chunk chunk = st->sendq.front();
                    st->queued_bytes -= chunk.size;
                    st->sendq.pop_front();
                    outcomes.emplace_back(
                        chunk.c,
                        io::io_result{
                            std::make_error_code(std::errc::operation_canceled),
                            chunk.sent});
                }
            } else if (error != 0) {
                const std::error_code ec = map_wsa_error(error);
                send_chunk head = st->sendq.front();
                st->queued_bytes -= head.size;
                st->sendq.pop_front();
                outcomes.emplace_back(head.c, io::io_result{ec, head.sent});
                while (!st->sendq.empty()) {
                    send_chunk chunk = st->sendq.front();
                    st->queued_bytes -= chunk.size;
                    st->sendq.pop_front();
                    outcomes.emplace_back(
                        chunk.c,
                        io::io_result{
                            std::make_error_code(std::errc::broken_pipe), 0});
                }
            } else {
                send_chunk& head = st->sendq.front();
                head.sent += bytes;
                if (head.sent >= head.size) {
                    outcomes.emplace_back(head.c,
                                          io::io_result{{}, head.size});
                    st->queued_bytes -= head.size;
                    st->sendq.pop_front();
                }
                post_next = true;
            }
        }
    }

    for (auto& outcome : outcomes) {
        if (outcome.first != nullptr) {
            outcome.first->complete(outcome.second);
        }
    }
    if (post_next) {
        post_send(st);
    }
    {
        // The completion callbacks have run and the next send (if any) has been
        // posted.  Recompute the flag from the live state instead of setting it
        // unconditionally: a callback may have re-entered `push` (exactly what
        // `rpc::client::on_write_complete -> flush_write` does), which posted a
        // nested send and cleared `send_delivery_done` while this handler was
        // still running.  Setting it to `true` here would let a concurrent
        // `close()` return while that nested WSASend is still in flight.
        std::lock_guard<std::mutex> lk(st->m);
        st->send_delivery_done = !st->send_inflight && st->sendq.empty();
    }
    st->delivery_cv.notify_all();

    bool need_finalize = false;
    {
        std::lock_guard<std::mutex> lk(st->m);
        need_finalize = st->phase != conn_phase::open && !st->recv_inflight &&
                        !st->send_inflight && !st->disc_inflight &&
                        !st->disconnecting;
    }
    if (need_finalize) {
        finalize_close(st);
    }
}

void push_impl(conn_state* st, span<const std::byte> data,
               io::completion* c) noexcept {
    if (c == nullptr) {
        return;
    }
    if (data.empty()) {
        c->complete(io::io_result{{}, 0});
        return;
    }

    std::error_code reject{};
    {
        std::lock_guard<std::mutex> lk(st->m);
        if (st->phase != conn_phase::open) {
            reject = std::make_error_code(std::errc::broken_pipe);
        } else if (!st->sendq.empty() &&
                   st->queued_bytes + data.size() > st->send_limit) {
            reject = std::make_error_code(std::errc::no_buffer_space);
        } else {
            st->sendq.push_back(send_chunk{data.data(), data.size(), 0, c});
            st->queued_bytes += data.size();
        }
    }
    if (reject) {
        c->complete(io::io_result{reject, 0});
        return;
    }
    post_send(st);
}

bool post_accept(accept_slot* slot) noexcept {
    listener_state* ls = slot->owner;
    iocp_backend::impl* be = ls->backend;
    if (ls->stopping.load(std::memory_order_acquire) || ls->closed.load()) {
        return false;
    }
    if (slot->sock == INVALID_SOCKET) {
        slot->sock = ::WSASocketW(ls->family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                                  WSA_FLAG_OVERLAPPED);
        if (slot->sock == INVALID_SOCKET) {
            return false;
        }
    }
    slot->op->ov = OVERLAPPED{};
    slot->op->kind = op_kind::accept;
    slot->op->owner = slot;
    DWORD bytes = 0;
    const BOOL ok = be->accept_ex(
        ls->lsock, slot->sock, reinterpret_cast<char*>(slot->addr.get()), 0,
        static_cast<DWORD>(k_addr_len), static_cast<DWORD>(k_addr_len), &bytes,
        &slot->op->ov);
    if (!ok) {
        const int e = ::WSAGetLastError();
        if (e != WSA_IO_PENDING) {
            ::closesocket(slot->sock);
            slot->sock = INVALID_SOCKET;
            return false;
        }
    }
    return true;
}

void handle_accept(overlapped_op* op) noexcept {
    accept_slot* slot = static_cast<accept_slot*>(op->owner);
    listener_state* ls = slot->owner;
    iocp_backend::impl* be = ls->backend;

    DWORD bytes = 0;
    DWORD flags = 0;
    const BOOL ok =
        ::WSAGetOverlappedResult(ls->lsock, &op->ov, &bytes, FALSE, &flags);

    if (ls->stopping.load(std::memory_order_acquire)) {
        if (slot->sock != INVALID_SOCKET) {
            ::closesocket(slot->sock);
            slot->sock = INVALID_SOCKET;
        }
        if (ls->pending.fetch_sub(1) == 1) {
            finalize_listener(ls);
        }
        return;
    }

    if (!ok) {
        if (slot->sock != INVALID_SOCKET) {
            ::closesocket(slot->sock);
            slot->sock = INVALID_SOCKET;
        }
        if (!post_accept(slot)) {
            if (ls->pending.fetch_sub(1) == 1) {
                finalize_listener(ls);
            }
        }
        return;
    }

    SOCKET accepted = slot->sock;
    slot->sock = INVALID_SOCKET;

    sockaddr* local = nullptr;
    sockaddr* remote = nullptr;
    int local_len = 0;
    int remote_len = 0;
    ::GetAcceptExSockaddrs(slot->addr.get(), 0, static_cast<DWORD>(k_addr_len),
                           static_cast<DWORD>(k_addr_len), &local, &local_len,
                           &remote, &remote_len);
    (void)local;
    (void)local_len;
    (void)remote_len;

    SOCKET listen_socket = ls->lsock;
    ::setsockopt(accepted, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                 reinterpret_cast<const char*>(&listen_socket),
                 static_cast<int>(sizeof(listen_socket)));

    // `remote()` must report the accepted socket's real peer. The raw
    // GetAcceptExSockaddrs view is unreliable for a numeric-name read, and
    // getpeername only works after SO_UPDATE_ACCEPT_CONTEXT above, so the
    // kernel itself is the source of truth (the epoll backend does the same).
    sockaddr_storage peer_ss{};
    socklen_t peer_len = sizeof(peer_ss);
    const endpoint rep =
        ::getpeername(accepted, reinterpret_cast<sockaddr*>(&peer_ss),
                      &peer_len) == 0
            ? endpoint_from_sockaddr(reinterpret_cast<const sockaddr*>(&peer_ss))
            : endpoint::tcp(std::string(), 0);

    if (!post_accept(slot)) {
        if (ls->pending.fetch_sub(1) == 1) {
            finalize_listener(ls);
        }
    }

    std::shared_ptr<conn_state> st = be->make_connection(
        accepted, rep, be->opt.recv_buffer_bytes, be->opt.send_queue_bytes, 0,
        be->opt.tcp_no_delay);
    if (!st) {
        return;
    }
    auto conn = make_iocp_connection(std::move(st));
    accept_sink* sink = ls->sink;
    if (sink != nullptr) {
        sink->on_accept(std::move(conn));
    }
}

void finalize_listener(listener_state* ls) noexcept {
    if (ls->closed.exchange(true)) {
        return;
    }
    SOCKET sock = ls->lsock;
    ls->lsock = INVALID_SOCKET;
    if (sock != INVALID_SOCKET) {
        ::closesocket(sock);
    }
    ls->backend->unregister_listener(ls->id);
}

// --- Connection / listener objects -------------------------------------------

class iocp_connection final : public connection {
public:
    explicit iocp_connection(std::shared_ptr<conn_state> st) noexcept
        : st_(std::move(st)),
          src_adapter_(st_.get()),
          snk_adapter_(st_.get()) {}

    ~iocp_connection() override {
        if (st_) {
            // The source is borrowed: wait for calls already running into it
            // before the connection state is torn down.
            st_->bound.retire();
            initiate_close(st_.get());
        }
    }

    io::source& source() noexcept override { return src_adapter_; }
    io::sink& sink() noexcept override { return snk_adapter_; }

    void bind_source(io::source* source) noexcept override {
        if (!st_) {
            return;
        }
        st_->bound.rebind(source);
        if (source != nullptr) {
            {
                std::lock_guard<std::mutex> lk(st_->m);
                if (st_->phase != conn_phase::open) {
                    return;
                }
                // Delivery may have paused while no source was bound; that can
                // only happen with retained bytes, so queue the demand only
                // then. With an empty buffer there is nothing to deliver and a
                // queued wake would never be fulfilled -- it would linger and
                // later override an unrelated pause.
                if (st_->rx_off < st_->rx_got) {
                    ++st_->pending_wakes;
                }
            }
            deliver_read(st_.get());
        }
    }

    void resume_read() noexcept override {
        if (!st_) {
            return;
        }
        {
            std::lock_guard<std::mutex> lk(st_->m);
            if (st_->phase != conn_phase::open) {
                return;
            }
            // Queue the demand instead of clearing `paused` here: a running
            // pass that is about to write back its own short-consumption pause
            // would otherwise overwrite this clear, and a pass that captured a
            // generation counter before this point could never see the wake.
            // The delivery loop fulfils the request with a delivery attempt in
            // the same critical section that releases `delivering`, so the wake
            // either restarts that pass or runs its own. An empty buffer needs
            // no wake: `paused` implies retained bytes, and incoming bytes
            // queue their own wake on completion.
            if (st_->rx_off < st_->rx_got) {
                ++st_->pending_wakes;
            }
        }
        deliver_read(st_.get());
    }

    void close() noexcept override {
        if (st_) {
            initiate_close(st_.get());
        }
    }

    [[nodiscard]] bool is_open() const noexcept override {
        if (!st_) {
            return false;
        }
        std::lock_guard<std::mutex> lk(st_->m);
        return st_->phase == conn_phase::open;
    }

    [[nodiscard]] endpoint remote() const override {
        return st_ ? st_->remote_ep : endpoint();
    }

private:
    class src_adapter final : public io::source {
    public:
        explicit src_adapter(conn_state* st) noexcept : st_(st) {}
        std::size_t on_data(span<const std::byte> data) noexcept override {
            if (io::source* s = st_->bound.enter()) {
                const std::size_t used = s->on_data(data);
                st_->bound.leave();
                return used;
            }
            return 0;
        }
        void on_eof() noexcept override {
            if (io::source* s = st_->bound.enter()) {
                s->on_eof();
                st_->bound.leave();
            }
        }
        void on_error(std::error_code ec) noexcept override {
            if (io::source* s = st_->bound.enter()) {
                s->on_error(ec);
                st_->bound.leave();
            }
        }

    private:
        conn_state* st_;
    };

    class snk_adapter final : public io::sink {
    public:
        explicit snk_adapter(conn_state* st) noexcept : st_(st) {}
        void push(span<const std::byte> data, io::completion* c) noexcept override {
            push_impl(st_, data, c);
        }

    private:
        conn_state* st_;
    };

    std::shared_ptr<conn_state> st_;
    src_adapter src_adapter_;
    snk_adapter snk_adapter_;
};

std::unique_ptr<connection> make_iocp_connection(
    std::shared_ptr<conn_state> st) noexcept {
    return std::unique_ptr<connection>(
        new (std::nothrow) iocp_connection(std::move(st)));
}

class iocp_listener final : public listener {
public:
    explicit iocp_listener(std::shared_ptr<listener_state> st) noexcept
        : st_(std::move(st)) {}

    ~iocp_listener() override {
        if (st_) {
            stop();
        }
    }

    void start(accept_sink* sink) override {
        if (!st_ || st_->stopping.load(std::memory_order_acquire)) {
            return;
        }
        bool expected = false;
        if (!st_->started.compare_exchange_strong(expected, true)) {
            return;
        }
        st_->sink = sink;
        for (auto& slot : st_->slots) {
            if (post_accept(slot.get())) {
                st_->pending.fetch_add(1);
            }
        }
    }

    void stop() noexcept override {
        if (!st_) {
            return;
        }
        st_->stopping.store(true, std::memory_order_release);
        if (st_->lsock != INVALID_SOCKET) {
            ::CancelIoEx(reinterpret_cast<HANDLE>(st_->lsock), nullptr);
        }
        if (st_->pending.load(std::memory_order_acquire) == 0) {
            finalize_listener(st_.get());
        }
    }

private:
    std::shared_ptr<listener_state> st_;
};

} // namespace

iocp_backend::impl::impl(iocp_options options)
    : opt(options), pool(options.initial_op_pool) {
    init();
}

void iocp_backend::impl::init() noexcept {
    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return;
    }
    wsa_started.store(true, std::memory_order_release);

    iocp = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (iocp == nullptr) {
        return;
    }

    SOCKET probe = ::WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                                WSA_FLAG_OVERLAPPED);
    if (probe != INVALID_SOCKET) {
        DWORD bytes = 0;
        GUID g_accept = WSAID_ACCEPTEX;
        (void)::WSAIoctl(probe, SIO_GET_EXTENSION_FUNCTION_POINTER, &g_accept,
                         sizeof(g_accept), &accept_ex, sizeof(accept_ex), &bytes,
                         nullptr, nullptr);
        GUID g_disc = WSAID_DISCONNECTEX;
        (void)::WSAIoctl(probe, SIO_GET_EXTENSION_FUNCTION_POINTER, &g_disc,
                         sizeof(g_disc), &disconnect_ex, sizeof(disconnect_ex),
                         &bytes, nullptr, nullptr);
        ::closesocket(probe);
    }
    if (accept_ex == nullptr) {
        return;
    }

    // The capability flag must describe what was *requested*, not merely that a
    // handle came back: CreateWaitableTimerExW also succeeds with flags == 0, so
    // keying the flag on "handle != nullptr" would silently report success after
    // the high-resolution flag was dropped from the call. Deriving it from
    // `timer_flags` makes that regression observable.
    constexpr DWORD timer_flags = CREATE_WAITABLE_TIMER_HIGH_RESOLUTION;
    timer_handle = ::CreateWaitableTimerExW(
        nullptr, nullptr, timer_flags, TIMER_ALL_ACCESS);
    if (timer_handle == nullptr) {
        // Older than Windows 10 1803: fall back to a standard timer.  Deadlines
        // still work; only the resolution degrades.
        timer_handle = ::CreateWaitableTimerW(nullptr, FALSE, nullptr);
    } else {
        high_resolution_timer.store(
            (timer_flags & CREATE_WAITABLE_TIMER_HIGH_RESOLUTION) != 0,
            std::memory_order_release);
    }
    timer_wake = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (timer_handle == nullptr || timer_wake == nullptr) {
        return;
    }

    valid.store(true, std::memory_order_release);
    try {
        timer_thread = std::thread(&iocp_backend::impl::timer_loop, this);
        pump_thread = std::thread(&iocp_backend::impl::pump_loop, this);
    } catch (...) {
        valid.store(false, std::memory_order_release);
    }
}

bool iocp_backend::impl::associate(SOCKET sock) noexcept {
    return ::CreateIoCompletionPort(reinterpret_cast<HANDLE>(sock), iocp, 0, 0) !=
           nullptr;
}

std::shared_ptr<conn_state> iocp_backend::impl::make_connection(
    SOCKET sock, endpoint remote, std::size_t recv_cap, std::size_t send_limit,
    std::size_t sndbuf, bool no_delay) noexcept {
    if (!associate(sock)) {
        ::closesocket(sock);
        return nullptr;
    }

    std::shared_ptr<conn_state> st(new (std::nothrow) conn_state());
    if (!st) {
        ::closesocket(sock);
        return nullptr;
    }
    st->backend = this;
    st->id = next_conn_id.fetch_add(1, std::memory_order_relaxed);
    st->sock = sock;
    st->remote_ep = std::move(remote);
    st->send_limit = send_limit == 0 ? 1 : send_limit;
    st->rx_cap = recv_cap == 0 ? 1 : recv_cap;
    st->rx.reset(new (std::nothrow) std::byte[st->rx_cap]);
    st->recv_op = pool.acquire();
    st->send_op = pool.acquire();
    st->disc_op = pool.acquire();
    if (!st->rx || st->recv_op == nullptr || st->send_op == nullptr ||
        st->disc_op == nullptr) {
        ::closesocket(sock);
        st->sock = INVALID_SOCKET;
        return nullptr;
    }

    int rcv = static_cast<int>(st->rx_cap);
    (void)::setsockopt(sock, SOL_SOCKET, SO_RCVBUF,
                       reinterpret_cast<const char*>(&rcv),
                       static_cast<int>(sizeof(rcv)));
    if (sndbuf > 0) {
        int snd = static_cast<int>(sndbuf);
        (void)::setsockopt(sock, SOL_SOCKET, SO_SNDBUF,
                           reinterpret_cast<const char*>(&snd),
                           static_cast<int>(sizeof(snd)));
    }
    if (no_delay) {
        BOOL one = TRUE;
        (void)::setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
                           reinterpret_cast<const char*>(&one),
                           static_cast<int>(sizeof(one)));
    }

    {
        std::lock_guard<std::mutex> lk(registry_mutex);
        conns.emplace(st->id, st);
    }
    post_recv(st.get());
    return st;
}

std::unique_ptr<connection> iocp_backend::impl::do_connect(
    const endpoint& ep, const connect_options& co) {
    if (ep.type() != endpoint::kind::tcp) {
        return nullptr;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    char portstr[16] = {0};
    std::snprintf(portstr, sizeof(portstr), "%u",
                  static_cast<unsigned>(ep.port()));
    addrinfo* res = nullptr;
    if (::getaddrinfo(ep.host().c_str(), portstr, &hints, &res) != 0) {
        return nullptr;
    }

    SOCKET sock = INVALID_SOCKET;
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        SOCKET s = ::WSASocketW(ai->ai_family, ai->ai_socktype, ai->ai_protocol,
                                nullptr, 0, WSA_FLAG_OVERLAPPED);
        if (s == INVALID_SOCKET) {
            continue;
        }
        u_long nonblocking = 1;
        (void)::ioctlsocket(s, FIONBIO, &nonblocking);
        const int r = ::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        if (r != SOCKET_ERROR) {
            sock = s;
            break;
        }
        const int e = ::WSAGetLastError();
        if (e == WSAEWOULDBLOCK) {
            fd_set wf;
            fd_set ef;
            FD_ZERO(&wf);
            FD_SET(s, &wf);
            FD_ZERO(&ef);
            FD_SET(s, &ef);
            timeval tv = to_timeval(co.connect_timeout);
            const int sel = ::select(0, nullptr, &wf, &ef, &tv);
            // A writable socket that is not in the exception set has completed
            // the handshake; a failed connect is reported in exceptfds, which
            // select() has already filled in.
            //
            // The obvious refinement -- also call getsockopt(SO_ERROR) and
            // require zero -- was measured to cost a full Windows timer tick
            // (~15.6 ms) on roughly a third of loopback connects, while every
            // other call in this loop stayed under 0.5 ms. Isolated with four
            // probes against a plain Winsock listener (so crpc was not
            // involved):
            //
            //   select + SO_ERROR        p50 0.42 ms  p90 15.19 ms
            //   select only              p50 0.09 ms  p90 0.14 ms
            //   WSAEventSelect(FD_CONNECT) p50 0.10 ms p90 0.17 ms
            //   blocking connect         p50 0.08 ms  p90 0.11 ms
            //
            // The exception set was then verified to be a complete failure
            // signal on its own: 250/250 connects to a live listener succeeded,
            // and 250/250 to a closed port were reported as failures with zero
            // false successes. So the SO_ERROR call is redundant here and it is
            // the single biggest cost in the connect path.
            if (sel > 0 && FD_ISSET(s, &wf) && !FD_ISSET(s, &ef)) {
                sock = s;
                break;
            }
        }
        ::closesocket(s);
    }
    ::freeaddrinfo(res);
    if (sock == INVALID_SOCKET) {
        return nullptr;
    }

    auto st = make_connection(sock, endpoint::tcp(ep.host(), ep.port()),
                              co.recv_buffer_bytes, opt.send_queue_bytes,
                              co.send_buffer_bytes,
                              co.tcp_no_delay && opt.tcp_no_delay);
    if (!st) {
        return nullptr;
    }
    return make_iocp_connection(std::move(st));
}

std::unique_ptr<listener> iocp_backend::impl::do_listen(
    const endpoint& ep, const listen_options& lo) {
    if (ep.type() != endpoint::kind::tcp) {
        return nullptr;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;
    char portstr[16] = {0};
    std::snprintf(portstr, sizeof(portstr), "%u",
                  static_cast<unsigned>(ep.port()));
    addrinfo* res = nullptr;
    if (::getaddrinfo(ep.host().c_str(), portstr, &hints, &res) != 0) {
        return nullptr;
    }

    SOCKET lsock = INVALID_SOCKET;
    int family = AF_UNSPEC;
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        SOCKET s = ::WSASocketW(ai->ai_family, ai->ai_socktype, ai->ai_protocol,
                                nullptr, 0, WSA_FLAG_OVERLAPPED);
        if (s == INVALID_SOCKET) {
            continue;
        }
        // A listener gets SO_EXCLUSIVEADDRUSE, never SO_REUSEADDR.
        //
        // On Windows SO_REUSEADDR does not mean "reuse an address in TIME_WAIT" the
        // way it does on BSD; it means "let another socket take this port". Two
        // listeners with it both bind successfully and the connections keep going to
        // the first one, so the second listener accepts nothing at all -- measured:
        // both binds succeed, and a burst of 500 connects produced 0 accepts. The
        // failure is silent, which is what makes it dangerous: the caller believes it
        // is listening.
        //
        // SO_EXCLUSIVEADDRUSE makes the conflicting bind fail with WSAEADDRINUSE
        // instead, which is the loud answer a caller can act on. It does not break
        // restart: binding the same port right after the previous listener closed was
        // measured to succeed on three consecutive rounds, including after a real
        // connection had been made to the port.
        //
        // `reuse_port` therefore cannot be honoured here. On Linux SO_REUSEPORT is a
        // real load-balancing feature; on Windows the nearest equivalent is the
        // hijacking behaviour above, which no server wants by default.
        {
            BOOL exclusive = TRUE;
            (void)::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                               reinterpret_cast<const char*>(&exclusive),
                               static_cast<int>(sizeof(exclusive)));
        }
        if (::bind(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
            lsock = s;
            family = ai->ai_family;
            break;
        }
        ::closesocket(s);
    }
    ::freeaddrinfo(res);
    if (lsock == INVALID_SOCKET) {
        return nullptr;
    }

    const std::size_t backlog = lo.backlog == 0 ? 1 : lo.backlog;
    const int backlog_i = backlog > 0x7fffffff
                              ? 0x7fffffff
                              : static_cast<int>(backlog);
    if (::listen(lsock, backlog_i) == SOCKET_ERROR) {
        ::closesocket(lsock);
        return nullptr;
    }
    if (!associate(lsock)) {
        ::closesocket(lsock);
        return nullptr;
    }

    std::shared_ptr<listener_state> ls(new (std::nothrow) listener_state());
    if (!ls) {
        ::closesocket(lsock);
        return nullptr;
    }
    ls->backend = this;
    ls->id = next_listener_id.fetch_add(1, std::memory_order_relaxed);
    ls->lsock = lsock;
    ls->family = family;

    const std::size_t posts = opt.accept_posts == 0 ? 1 : opt.accept_posts;
    for (std::size_t i = 0; i < posts; ++i) {
        std::unique_ptr<accept_slot> slot(new (std::nothrow) accept_slot());
        if (!slot) {
            break;
        }
        slot->owner = ls.get();
        slot->op = pool.acquire();
        slot->addr.reset(new (std::nothrow) std::byte[2 * k_addr_len]);
        if (slot->op == nullptr || !slot->addr) {
            break;
        }
        ls->slots.push_back(std::move(slot));
    }

    {
        std::lock_guard<std::mutex> lk(registry_mutex);
        listeners.emplace(ls->id, ls);
    }
    return std::unique_ptr<listener>(
        new (std::nothrow) iocp_listener(std::move(ls)));
}

std::shared_ptr<conn_state> iocp_backend::impl::retain_conn(
    std::uint64_t id) noexcept {
    std::lock_guard<std::mutex> lk(registry_mutex);
    const auto it = conns.find(id);
    if (it == conns.end()) {
        return nullptr;
    }
    return it->second;
}

void iocp_backend::impl::unregister_conn(std::uint64_t id) noexcept {
    std::lock_guard<std::mutex> lk(registry_mutex);
    conns.erase(id);
}

void iocp_backend::impl::unregister_listener(std::uint64_t id) noexcept {
    std::lock_guard<std::mutex> lk(registry_mutex);
    listeners.erase(id);
}

void iocp_backend::impl::pump_loop() noexcept {
    pump_tid.store(::GetCurrentThreadId(), std::memory_order_release);
    const std::size_t batch = opt.completion_batch == 0 ? 1 : opt.completion_batch;
    std::vector<OVERLAPPED_ENTRY> entries(batch);
    while (!stopping.load(std::memory_order_acquire)) {
        ULONG n = 0;
        const BOOL ok = ::GetQueuedCompletionStatusEx(
            iocp, entries.data(), static_cast<ULONG>(entries.size()), &n, INFINITE,
            FALSE);
        if (!ok) {
            const DWORD e = ::GetLastError();
            if (e == WAIT_TIMEOUT) {
                continue;
            }
            break;
        }
        for (ULONG i = 0; i < n; ++i) {
            dispatch(entries[i]);
        }
    }
}

void iocp_backend::impl::dispatch(OVERLAPPED_ENTRY& entry) noexcept {
    if (entry.lpOverlapped == nullptr) {
        dispatch_control(reinterpret_cast<control_node*>(entry.lpCompletionKey));
        return;
    }
    overlapped_op* op = CONTAINING_RECORD(entry.lpOverlapped, overlapped_op, ov);
    // Every connection completion carries the registry id, and the id is read
    // from the operation record rather than from the connection, so it stays
    // readable even when an earlier completion already closed and released the
    // connection. The strong reference below keeps the state alive for the
    // whole handler, including the finalisation step that runs after the user
    // completion callbacks.
    if (op->kind == op_kind::recv || op->kind == op_kind::send ||
        op->kind == op_kind::disconnect) {
        std::shared_ptr<conn_state> keep = retain_conn(op->conn_id);
        if (!keep) {
            return;
        }
        switch (op->kind) {
        case op_kind::recv: {
            SOCKET sock;
            {
                std::lock_guard<std::mutex> lk(keep->m);
                sock = keep->sock;
            }
            std::size_t bytes = 0;
            unsigned long error = 0;
            read_op_result(sock, op, entry.dwNumberOfBytesTransferred, &bytes, &error);
            handle_recv_complete(op, bytes, error);
            return;
        }
        case op_kind::send: {
            SOCKET sock;
            {
                std::lock_guard<std::mutex> lk(keep->m);
                sock = keep->sock;
            }
            std::size_t bytes = 0;
            unsigned long error = 0;
            read_op_result(sock, op, entry.dwNumberOfBytesTransferred, &bytes, &error);
            handle_send_complete(op, bytes, error);
            return;
        }
        case op_kind::disconnect:
            handle_disconnect_complete(op);
            return;
        default:
            return;
        }
    }
    if (op->kind == op_kind::accept) {
        handle_accept(op);
    }
}

void iocp_backend::impl::dispatch_control(control_node* node) noexcept {
    if (node == nullptr) {
        return;
    }
    switch (node->k) {
    case control_node::kind::stop:
        stopping.store(true, std::memory_order_release);
        break;
    case control_node::kind::timers:
        drain_timers();
        break;
    }
}

timer_slot* iocp_backend::impl::acquire_timer_slot() noexcept {
    std::lock_guard<std::mutex> lk(tmutex);
    if (!timer_free.empty()) {
        timer_slot* s = timer_free.back();
        timer_free.pop_back();
        return s;
    }
    timer_slot* s = new (std::nothrow) timer_slot();
    if (s != nullptr) {
        timer_storage.push_back(std::unique_ptr<timer_slot>(s));
    }
    return s;
}

std::uint64_t iocp_backend::impl::do_arm_timer(
    std::chrono::nanoseconds delay, iocp_backend::timer_callback fn,
    void* arg) noexcept {
    if (!valid.load(std::memory_order_acquire) || fn == nullptr) {
        return 0;
    }
    timer_slot* slot = acquire_timer_slot();
    if (slot == nullptr) {
        return 0;
    }
    std::int64_t d = delay.count();
    if (d < 0) {
        d = 0;
    }
    const std::uint64_t id = next_timer_id.fetch_add(1, std::memory_order_relaxed);
    slot->id = id;
    slot->deadline_ns = monotonic_now_ns() + static_cast<std::uint64_t>(d);
    slot->fn = fn;
    slot->arg = arg;
    slot->canceled = false;
    {
        std::lock_guard<std::mutex> lk(tmutex);
        live.emplace(id, slot);
        heap.push_back(slot);
        std::push_heap(heap.begin(), heap.end(), timer_heap_cmp{});
    }
    if (timer_wake != nullptr) {
        ::SetEvent(timer_wake);
    }
    return id;
}

bool iocp_backend::impl::do_cancel_timer(std::uint64_t id) noexcept {
    std::lock_guard<std::mutex> lk(tmutex);
    auto it = live.find(id);
    if (it == live.end()) {
        return false;
    }
    it->second->canceled = true;
    return true;
}

void iocp_backend::impl::timer_loop() noexcept {
    // This thread exists only to sleep until the nearest deadline, so a short
    // deadline must not be delayed by ordinary work on a loaded machine (the
    // 500 us timer test once measured >20 ms while the box was compiling).
    // Raising the priority cannot starve anything: the thread wakes only when a
    // timer is due and does no work beyond handing expired slots to the pump.
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    std::unique_lock<std::mutex> lk(tmutex);
    while (!timer_stopping.load(std::memory_order_acquire)) {
        const std::uint64_t now = monotonic_now_ns();
        bool any = false;
        while (!heap.empty() && heap.front()->deadline_ns <= now) {
            timer_slot* s = heap.front();
            std::pop_heap(heap.begin(), heap.end(), timer_heap_cmp{});
            heap.pop_back();
            ready.push_back(s);
            any = true;
        }
        if (any) {
            ::PostQueuedCompletionStatus(
                iocp, 0, reinterpret_cast<ULONG_PTR>(&timer_node), nullptr);
        }
        if (heap.empty()) {
            lk.unlock();
            ::WaitForSingleObject(timer_wake, INFINITE);
            lk.lock();
        } else {
            const std::uint64_t deadline = heap.front()->deadline_ns;
            const std::uint64_t n2 = monotonic_now_ns();
            const std::int64_t remaining =
                deadline > n2 ? static_cast<std::int64_t>(deadline - n2) : 0;
            LARGE_INTEGER due{};
            due.QuadPart = -(remaining / 100); // relative, 100 ns units
            if (due.QuadPart == 0) {
                due.QuadPart = -1;
            }
            ::SetWaitableTimer(timer_handle, &due, 0, nullptr, nullptr, FALSE);
            lk.unlock();
            HANDLE handles[2] = {timer_handle, timer_wake};
            ::WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            lk.lock();
        }
    }
}

void iocp_backend::impl::drain_timers() noexcept {
    std::vector<timer_slot*> batch;
    {
        std::lock_guard<std::mutex> lk(tmutex);
        batch.swap(ready);
    }
    for (timer_slot* s : batch) {
        bool canceled = false;
        {
            std::lock_guard<std::mutex> lk(tmutex);
            canceled = s->canceled;
        }
        if (!canceled && s->fn != nullptr) {
            s->fn(s->arg);
        }
        {
            std::lock_guard<std::mutex> lk(tmutex);
            live.erase(s->id);
            timer_free.push_back(s);
        }
    }
}

void iocp_backend::impl::stop() noexcept {
    bool expected = false;
    if (!stopped.compare_exchange_strong(expected, true)) {
        return;
    }
    stopping.store(true, std::memory_order_release);

    timer_stopping.store(true, std::memory_order_release);
    if (timer_wake != nullptr) {
        ::SetEvent(timer_wake);
    }
    if (timer_thread.joinable()) {
        timer_thread.join();
    }

    if (pump_thread.joinable()) {
        if (iocp != nullptr) {
            ::PostQueuedCompletionStatus(
                iocp, 0, reinterpret_cast<ULONG_PTR>(&stop_node), nullptr);
        }
        pump_thread.join();
    }

    std::vector<std::shared_ptr<listener_state>> ls_list;
    std::vector<std::shared_ptr<conn_state>> conn_list;
    {
        std::lock_guard<std::mutex> lk(registry_mutex);
        for (auto& kv : listeners) {
            ls_list.push_back(kv.second);
        }
        for (auto& kv : conns) {
            conn_list.push_back(kv.second);
        }
        listeners.clear();
        conns.clear();
    }
    for (auto& l : ls_list) {
        // Mark the listener terminal before closing its socket.  If the user
        // destroys the listener object after the backend, its destructor must
        // not call back into this (now dead) impl through unregister_listener.
        l->stopping.store(true, std::memory_order_release);
        l->closed.store(true, std::memory_order_release);
        SOCKET s = l->lsock;
        l->lsock = INVALID_SOCKET;
        if (s != INVALID_SOCKET) {
            ::closesocket(s);
        }
    }
    for (auto& c : conn_list) {
        SOCKET s = INVALID_SOCKET;
        {
            std::lock_guard<std::mutex> lk(c->m);
            c->phase = conn_phase::closed;
            s = c->sock;
            c->sock = INVALID_SOCKET;
        }
        if (s != INVALID_SOCKET) {
            ::closesocket(s);
        }
    }

    if (iocp != nullptr) {
        ::CloseHandle(iocp);
        iocp = nullptr;
    }
    if (timer_handle != nullptr) {
        ::CloseHandle(timer_handle);
        timer_handle = nullptr;
    }
    if (timer_wake != nullptr) {
        ::CloseHandle(timer_wake);
        timer_wake = nullptr;
    }
    if (wsa_started.exchange(false)) {
        (void)::WSACleanup();
    }
}

iocp_backend::impl::~impl() { stop(); }

// --- Public backend methods --------------------------------------------------

iocp_backend::iocp_backend(iocp_options options)
    : impl_(new (std::nothrow) impl(options)) {}

iocp_backend::~iocp_backend() {
    if (impl_) {
        impl_->stop();
    }
}

std::unique_ptr<connection> iocp_backend::connect(const endpoint& ep,
                                                  const connect_options& opt) {
    if (!impl_ || !impl_->valid.load(std::memory_order_acquire)) {
        return nullptr;
    }
    return impl_->do_connect(ep, opt);
}

std::unique_ptr<listener> iocp_backend::listen(const endpoint& ep,
                                               const listen_options& opt) {
    if (!impl_ || !impl_->valid.load(std::memory_order_acquire)) {
        return nullptr;
    }
    return impl_->do_listen(ep, opt);
}

std::uint64_t iocp_backend::arm_timer(std::chrono::nanoseconds delay,
                                      timer_callback callback,
                                      void* arg) noexcept {
    if (!impl_) {
        return 0;
    }
    return impl_->do_arm_timer(delay, callback, arg);
}

bool iocp_backend::cancel_timer(std::uint64_t id) noexcept {
    if (!impl_) {
        return false;
    }
    return impl_->do_cancel_timer(id);
}

void iocp_backend::stop() noexcept {
    if (impl_) {
        impl_->stop();
    }
}

bool iocp_backend::running() const noexcept {
    return impl_ && impl_->valid.load(std::memory_order_acquire) &&
           !impl_->stopping.load(std::memory_order_acquire);
}

bool iocp_backend::high_resolution_timer() const noexcept {
    return impl_ && impl_->high_resolution_timer.load(std::memory_order_acquire);
}
} // namespace crpc

#endif // _WIN32
