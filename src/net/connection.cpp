// CRPC Redis -- asynchronous connection management.
//
// The transport is a thin, protocol-free layer over CRPC's `io::source` /
// `io::sink` pair. Three properties are implemented here and tested in
// tests/test_redis_connection.cpp:
//
//   * Cooperative suspension. A read or write called from a coroutine of the
//     bound shard parks through `runtime::park()`; the source callback runs on
//     the backend thread and resumes it with `runtime::notify()`. A plain
//     thread -- and a non-coroutine task running on the shard thread -- waits
//     on a condition variable bounded by the operation timeout.
//   * Backpressure. Incoming bytes go into a bounded buffer. When the buffer is
//     full `on_data` returns a short credit, which pauses the backend; the
//     connection calls `resume_read()` once the application drains the buffer.
//   * Reconnect. A terminal error/EOF closes the connection and wakes waiters;
//     the next operation re-establishes it through the same backend with a
//     bounded exponential backoff. An explicit `reconnect()` always tears the
//     current link down and builds a new one; `close()` is terminal.
#include <crpc/net/connection.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace crpc {
namespace net {
namespace {

// Translates a backend `std::error_code` into the RPC error model. The mapping
// is total: an unrecognised code becomes `internal`, never a wrong category.
rpc_code code_from_io(const std::error_code& ec) noexcept {
    if (!ec) {
        return rpc_code::ok;
    }
    const std::errc value = std::errc(ec.value());
    switch (value) {
    case std::errc::broken_pipe:
        return rpc_code::broken_pipe;
    case std::errc::connection_reset:
    case std::errc::connection_aborted:
        return rpc_code::connection_reset;
    case std::errc::connection_refused:
        return rpc_code::connection_refused;
    case std::errc::timed_out:
        return rpc_code::deadline_exceeded;
    case std::errc::operation_canceled:
        return rpc_code::canceled;
    case std::errc::not_enough_memory:
        return rpc_code::resource_exhausted;
    default:
        return rpc_code::internal;
    }
}

// Saturating shift used by the reconnect backoff. Capped at six doublings so a
// long outage cannot overflow the nanosecond count.
std::chrono::nanoseconds backoff_for(std::size_t attempts,
                                      std::chrono::nanoseconds floor_value,
                                      std::chrono::nanoseconds ceiling) noexcept {
    if (attempts == 0) {
        return floor_value;
    }
    const std::size_t shift = std::min<std::size_t>(attempts - 1, 6);
    // The shift runs on a 64-bit operand: on x86 `size_t` is 32-bit, so
    // `std::size_t{1} << shift` would be a 32-bit result widened to the
    // nanoseconds rep and trip C4334 under /W4 /WX.
    const auto factor = static_cast<std::chrono::nanoseconds::rep>(
        std::uint64_t{1} << shift);
    const auto grown = floor_value * factor;
    if (grown > ceiling) {
        return ceiling;
    }
    return grown;
}

} // namespace

struct connection::impl {
    // The framework-side receive callback. Every link gets its own adapter so a
    // late callback from a replaced connection can be recognised by its epoch
    // and ignored instead of corrupting the new link's state.
    class source_adapter final : public io::source {
    public:
        source_adapter(impl& owner, std::uint64_t epoch) noexcept
            : owner_(owner), epoch_(epoch) {}

        std::size_t on_data(span<const std::byte> data) noexcept override;
        void on_eof() noexcept override;
        void on_error(std::error_code ec) noexcept override;

    private:
        impl& owner_;
        std::uint64_t epoch_;
    };

    // One established transport link. The adapter is declared before the
    // connection so the backend connection (which holds a raw pointer to the
    // adapter) is destroyed before the adapter is.
    struct link {
        source_adapter adapter;
        std::shared_ptr<crpc::connection> conn;
        std::uint64_t epoch;

        link(impl& owner, std::uint64_t link_epoch)
            : adapter(owner, link_epoch), epoch(link_epoch) {}
    };

    // One outstanding sink push. `done`/`code`/`accepted` are guarded by
    // `impl::mutex` so a waiter can use the same `wait_for_pred` helper as the
    // read path.
    struct write_request {
        std::vector<std::byte> data;
        bool done = false;
        rpc_code code = rpc_code::ok;
        std::size_t accepted = 0;
    };

    // Exactly one `complete` per push. It never starts another push (contract
    // I3): it publishes the result, frees the write slot and then deletes
    // itself -- the io contract gives the completion to the callee.
    class write_completion final : public io::completion {
    public:
        write_completion(impl& owner, std::shared_ptr<write_request> req,
                         std::uint64_t epoch) noexcept
            : owner_(owner), req_(std::move(req)), epoch_(epoch) {}

        void complete(io::io_result result) noexcept override;

    private:
        impl& owner_;
        std::shared_ptr<write_request> req_;
        std::uint64_t epoch_;
    };

    connection_config cfg;
    crpc::backend_factory& backend;
    runtime* shard;

    mutable std::mutex mutex;
    std::condition_variable cv;
    // Serialises the connect/install sequence. Public operations only take
    // `mutex`, so the lock order is always reconnect_mutex -> mutex.
    std::mutex reconnect_mutex;
    std::deque<std::byte> buffer;
    bool closed = true;
    // Set by close(): no operation may reconnect after it. Only an explicit
    // reconnect() clears it.
    bool terminal = false;
    bool read_paused = false;
    bool write_busy = false;
    rpc_code error_code = rpc_code::ok;
    std::uint64_t reconnects = 0;
    bool established_once = false;
    std::uint64_t bytes_read = 0;
    std::uint64_t bytes_written = 0;
    // Consecutive failed connects drive the backoff. Reconnect failures made
    // after the initial connect attempt drive the max_reconnects cap; the
    // initial attempt itself is not a "reconnect".
    std::size_t connect_failures = 0;
    std::size_t reconnect_failures = 0;
    bool initial_attempted = false;
    std::chrono::steady_clock::time_point next_reconnect{};
    // Invalidates callbacks and completions belonging to a replaced link.
    std::uint64_t epoch = 0;
    // Declared last so it is destroyed first: its backend connection may still
    // invoke source callbacks while it is torn down, and those callbacks need
    // `mutex`/`cv` to be alive.
    std::shared_ptr<link> active;

    impl(connection_config config, crpc::backend_factory& factory, runtime* rt)
        : cfg(std::move(config)), backend(factory), shard(rt) {
        if (cfg.max_buffered_bytes == 0) {
            cfg.max_buffered_bytes = 1;
        }
        if (cfg.reconnect_max_backoff < cfg.reconnect_backoff) {
            cfg.reconnect_max_backoff = cfg.reconnect_backoff;
        }
        if (cfg.operation_timeout < std::chrono::nanoseconds::zero()) {
            cfg.operation_timeout = std::chrono::nanoseconds::zero();
        }
    }

    void notify_waiters() noexcept {
        cv.notify_all();
        if (shard != nullptr) {
            shard->notify();
        }
    }

    // Waits until `pred()` holds. On the shard thread it parks the calling
    // coroutine; a non-coroutine task on that thread (park reports
    // failed_precondition) and any other thread fall back to the condition
    // variable with the operation timeout.
    template <class Pred>
    status wait_for_pred(std::unique_lock<std::mutex>& lk, Pred pred,
                         std::chrono::nanoseconds timeout) {
        for (;;) {
            if (pred()) {
                return status{};
            }
            if (shard != nullptr && shard->loop_mode() && shard->is_owner_thread()) {
                lk.unlock();
                const status parked = shard->park();
                lk.lock();
                if (parked.ok()) {
                    continue;
                }
                if (parked.code() != rpc_code::failed_precondition) {
                    return parked;
                }
                // Not running inside a coroutine of this runtime: fall through
                // to the blocking path instead of surfacing a spurious error.
            }
            if (!cv.wait_for(lk, timeout, pred)) {
                return status{rpc_code::deadline_exceeded};
            }
        }
    }

    // True when the read side should be resumed. Must be called with the lock.
    bool resume_pending() const noexcept {
        return read_paused && buffer.size() < cfg.max_buffered_bytes;
    }

    // Reports a backend failure once. A clean eof (or no error) returns ok so
    // the caller falls through to the lazy reconnect; a real error is handed to
    // the first reader and cleared, so the next operation re-establishes the
    // link instead of hiding the failure behind a reconnect. Must be called
    // with the lock.
    rpc_code take_pending_error_locked() noexcept {
        if (error_code != rpc_code::ok && error_code != rpc_code::eof) {
            const rpc_code code = error_code;
            error_code = rpc_code::ok;
            return code;
        }
        return rpc_code::ok;
    }

    // Establishes a new link. `force` is used by the public reconnect(): it
    // tears down an open link too. The automatic path only acts on a closed
    // link, honours the backoff and the max_reconnects cap, and refuses to
    // revive a terminal connection.
    status reconnect_now(bool force);
};

std::size_t connection::impl::source_adapter::on_data(
    span<const std::byte> data) noexcept {
    auto& self = owner_;
    if (data.empty()) {
        return 0;
    }
    std::size_t consumed = 0;
    {
        std::lock_guard<std::mutex> lk(self.mutex);
        if (epoch_ != self.epoch) {
            // A replaced link is draining its last callback. Consume the bytes
            // so its backend can finish closing; they no longer belong to the
            // application stream.
            return data.size();
        }
        const std::size_t free = self.cfg.max_buffered_bytes - self.buffer.size();
        if (free == 0) {
            self.read_paused = true;
            return 0;
        }
        consumed = std::min(data.size(), free);
        self.buffer.insert(self.buffer.end(), data.begin(),
                           data.begin() + static_cast<std::ptrdiff_t>(consumed));
        self.bytes_read += consumed;
        if (consumed < data.size()) {
            self.read_paused = true;
        }
    }
    self.notify_waiters();
    return consumed;
}

void connection::impl::source_adapter::on_eof() noexcept {
    auto& self = owner_;
    {
        std::lock_guard<std::mutex> lk(self.mutex);
        if (epoch_ != self.epoch) {
            return;
        }
        self.closed = true;
        self.error_code = rpc_code::eof;
    }
    self.notify_waiters();
}

void connection::impl::source_adapter::on_error(std::error_code ec) noexcept {
    auto& self = owner_;
    const rpc_code code = code_from_io(ec);
    {
        std::lock_guard<std::mutex> lk(self.mutex);
        if (epoch_ != self.epoch) {
            return;
        }
        self.closed = true;
        self.error_code = code;
    }
    self.notify_waiters();
}

void connection::impl::write_completion::complete(io::io_result result) noexcept {
    auto& self = owner_;
    const rpc_code code = code_from_io(result.ec);
    {
        std::lock_guard<std::mutex> lk(self.mutex);
        req_->done = true;
        req_->code = code;
        req_->accepted = result.bytes;
        if (epoch_ == self.epoch) {
            self.write_busy = false;
            if (result.ec) {
                self.closed = true;
                self.error_code = code;
            }
        }
    }
    self.notify_waiters();
    delete this;
}

status connection::impl::reconnect_now(bool force) {
    std::lock_guard<std::mutex> reconnect_lock(reconnect_mutex);

    std::shared_ptr<link> old;
    bool initial = false;
    {
        std::unique_lock<std::mutex> lk(mutex);
        if (!force) {
            if (terminal) {
                return status{rpc_code::shutting_down};
            }
            if (active != nullptr && active->conn != nullptr &&
                active->conn->is_open() && !closed) {
                return status{};
            }
            if (cfg.max_reconnects != 0 &&
                reconnect_failures >= cfg.max_reconnects) {
                closed = true;
                error_code = rpc_code::unavailable;
                return status{rpc_code::unavailable};
            }
            const auto now = std::chrono::steady_clock::now();
            if (connect_failures > 0 && now < next_reconnect) {
                error_code = rpc_code::unavailable;
                return status{rpc_code::unavailable};
            }
            initial = !initial_attempted;
            initial_attempted = true;
        } else {
            terminal = false;
            old = std::move(active);
            closed = true;
            error_code = rpc_code::ok;
            buffer.clear();
            read_paused = false;
            write_busy = false;
            ++epoch;
            initial = !initial_attempted;
            initial_attempted = true;
        }
    }

    if (old != nullptr && old->conn != nullptr) {
        old->conn->close();
    }

    std::unique_ptr<crpc::connection> fresh =
        backend.connect(cfg.ep, cfg.connect);

    if (fresh == nullptr) {
        {
            std::lock_guard<std::mutex> lk(mutex);
            ++connect_failures;
            if (!initial) {
                ++reconnect_failures;
            }
            next_reconnect =
                std::chrono::steady_clock::now() +
                backoff_for(connect_failures, cfg.reconnect_backoff,
                            cfg.reconnect_max_backoff);
            closed = true;
            error_code = rpc_code::connection_refused;
        }
        notify_waiters();
        return status{rpc_code::connection_refused};
    }

    std::uint64_t link_epoch = 0;
    bool discard = false;
    {
        std::lock_guard<std::mutex> lk(mutex);
        if (terminal) {
            discard = true;
        } else {
            link_epoch = ++epoch;
        }
    }
    if (discard) {
        fresh->close();
        return status{rpc_code::shutting_down};
    }

    std::shared_ptr<link> new_link;
    try {
        new_link = std::make_shared<link>(*this, link_epoch);
    } catch (...) {
        fresh->close();
        return status{rpc_code::resource_exhausted};
    }
    fresh->bind_source(&new_link->adapter);
    try {
        new_link->conn = std::shared_ptr<crpc::connection>(std::move(fresh));
    } catch (...) {
        // A failed control-block allocation leaves the unique_ptr owning the
        // link (the standard's unique_ptr overload does not modify it on
        // exception); close it before unwinding.
        if (fresh != nullptr) {
            fresh->close();
        }
        return status{rpc_code::resource_exhausted};
    }

    std::shared_ptr<link> stale;
    {
        std::lock_guard<std::mutex> lk(mutex);
        if (terminal || link_epoch != epoch) {
            stale = std::move(new_link);
        } else {
            active = std::move(new_link);
            buffer.clear();
            closed = false;
            error_code = rpc_code::ok;
            read_paused = false;
            write_busy = false;
            connect_failures = 0;
            reconnect_failures = 0;
            next_reconnect = {};
            // Count only re-establishments after the first successful connect,
            // so `reconnect_count()` reports recoveries, not the initial link.
            if (established_once) {
                ++reconnects;
            }
            established_once = true;
        }
    }
    if (stale != nullptr) {
        if (stale->conn != nullptr) {
            stale->conn->close();
        }
        return status{rpc_code::shutting_down};
    }
    notify_waiters();
    return status{};
}

// --- connection --------------------------------------------------------------

connection::connection(connection_config config, crpc::backend_factory& backend,
                       runtime* shard)
    : impl_(std::make_unique<impl>(std::move(config), backend, shard)) {
    // Best effort: a refused connect leaves the object closed, and the first
    // operation retries it with backoff.
    (void)impl_->reconnect_now(true);
}

connection::~connection() = default;

status connection::write(span<const std::byte> data) {
    if (data.empty()) {
        return status{};
    }
    auto& self = *impl_;

    std::shared_ptr<impl::write_request> req;
    try {
        req = std::make_shared<impl::write_request>();
        req->data.assign(data.begin(), data.end());
    } catch (...) {
        return status{rpc_code::resource_exhausted};
    }

    std::shared_ptr<impl::link> link;
    std::uint64_t link_epoch = 0;
    {
        std::unique_lock<std::mutex> lk(self.mutex);
        if (self.terminal) {
            return status{rpc_code::shutting_down};
        }
        if (self.closed || self.active == nullptr || self.active->conn == nullptr ||
            !self.active->conn->is_open()) {
            lk.unlock();
            const status re = self.reconnect_now(false);
            if (!re.ok()) {
                return re;
            }
            lk.lock();
        }
        if (self.terminal) {
            return status{rpc_code::shutting_down};
        }
        const status slot = self.wait_for_pred(
            lk, [&self] { return !self.write_busy; }, self.cfg.operation_timeout);
        if (!slot.ok()) {
            return slot;
        }
        if (self.terminal) {
            return status{rpc_code::shutting_down};
        }
        if (self.active == nullptr || self.active->conn == nullptr) {
            return status{rpc_code::connection_refused};
        }
        self.write_busy = true;
        link = self.active;
        link_epoch = link->epoch;
    }

    auto* completion =
        new (std::nothrow) impl::write_completion(self, req, link_epoch);
    if (completion == nullptr) {
        std::lock_guard<std::mutex> lk(self.mutex);
        self.write_busy = false;
        return status{rpc_code::resource_exhausted};
    }

    // The span must outlive `complete`; `req` is owned by the completion too.
    // `link` keeps the backend connection alive even if another thread closes
    // or replaces it while the push is in flight.
    link->conn->sink().push(
        span<const std::byte>(req->data.data(), req->data.size()), completion);

    std::unique_lock<std::mutex> lk(self.mutex);
    const status done = self.wait_for_pred(
        lk, [&req] { return req->done; }, self.cfg.operation_timeout);
    if (!done.ok()) {
        // A timed-out push may have accepted only a prefix. Leaving the stream
        // alive would put the next command behind a partial frame, so abort the
        // link; the next operation starts on a fresh connection.
        std::shared_ptr<impl::link> aborted;
        if (self.active == link) {
            self.closed = true;
            self.error_code = done.code();
            self.buffer.clear();
            self.read_paused = false;
            self.write_busy = false;
            ++self.epoch;
            aborted = std::move(self.active);
        }
        lk.unlock();
        if (aborted != nullptr && aborted->conn != nullptr) {
            aborted->conn->close();
        }
        self.notify_waiters();
        return done;
    }

    const rpc_code code = req->code;
    self.bytes_written += req->accepted;
    lk.unlock();
    return status{code};
}

result<std::vector<std::byte>> connection::read_exact(std::size_t count) {
    auto& self = *impl_;
    std::unique_lock<std::mutex> lk(self.mutex);
    for (;;) {
        if (self.buffer.size() >= count) {
            std::vector<std::byte> out;
            out.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                out.push_back(self.buffer.front());
                self.buffer.pop_front();
            }
            std::shared_ptr<impl::link> link;
            if (self.resume_pending()) {
                self.read_paused = false;
                link = self.active;
            }
            lk.unlock();
            if (link != nullptr && link->conn != nullptr) {
                link->conn->resume_read();
            }
            return out;
        }
        if (self.closed) {
            if (self.buffer.empty()) {
                if (self.terminal) {
                    return rpc_error(rpc_code::shutting_down,
                                     "connection is closed");
                }
                const rpc_code pending = self.take_pending_error_locked();
                if (pending != rpc_code::ok) {
                    return rpc_error(pending, "connection error");
                }
                lk.unlock();
                const status re = self.reconnect_now(false);
                if (!re.ok()) {
                    return rpc_error(re.code(), "reconnect failed");
                }
                lk.lock();
                continue;
            }
            const rpc_code code = self.error_code != rpc_code::ok
                                      ? self.error_code
                                      : rpc_code::eof;
            return rpc_error(code, "connection closed mid-message");
        }
        const status waited = self.wait_for_pred(
            lk, [&self] { return !self.buffer.empty() || self.closed; },
            self.cfg.operation_timeout);
        if (!waited.ok()) {
            return rpc_error(waited.code(), "read wait failed");
        }
    }
}

result<std::vector<std::byte>> connection::read_until(std::byte delim) {
    auto& self = *impl_;
    std::unique_lock<std::mutex> lk(self.mutex);
    for (;;) {
        const auto it = std::find(self.buffer.begin(), self.buffer.end(), delim);
        if (it != self.buffer.end()) {
            const std::size_t count =
                static_cast<std::size_t>(it - self.buffer.begin()) + 1;
            std::vector<std::byte> out;
            out.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                out.push_back(self.buffer.front());
                self.buffer.pop_front();
            }
            std::shared_ptr<impl::link> link;
            if (self.resume_pending()) {
                self.read_paused = false;
                link = self.active;
            }
            lk.unlock();
            if (link != nullptr && link->conn != nullptr) {
                link->conn->resume_read();
            }
            return out;
        }
        if (self.closed) {
            if (self.buffer.empty()) {
                if (self.terminal) {
                    return rpc_error(rpc_code::shutting_down,
                                     "connection is closed");
                }
                const rpc_code pending = self.take_pending_error_locked();
                if (pending != rpc_code::ok) {
                    return rpc_error(pending, "connection error");
                }
                lk.unlock();
                const status re = self.reconnect_now(false);
                if (!re.ok()) {
                    return rpc_error(re.code(), "reconnect failed");
                }
                lk.lock();
                continue;
            }
            const rpc_code code = self.error_code != rpc_code::ok
                                      ? self.error_code
                                      : rpc_code::eof;
            return rpc_error(code, "connection closed before delimiter");
        }
        const status waited = self.wait_for_pred(
            lk, [&self] { return !self.buffer.empty() || self.closed; },
            self.cfg.operation_timeout);
        if (!waited.ok()) {
            return rpc_error(waited.code(), "read wait failed");
        }
    }
}

std::vector<std::byte> connection::read_available() {
    auto& self = *impl_;
    std::vector<std::byte> out;
    std::shared_ptr<impl::link> link;
    {
        std::unique_lock<std::mutex> lk(self.mutex);
        out.assign(self.buffer.begin(), self.buffer.end());
        self.buffer.clear();
        if (self.resume_pending()) {
            self.read_paused = false;
            link = self.active;
        }
    }
    if (link != nullptr && link->conn != nullptr) {
        link->conn->resume_read();
    }
    return out;
}

void connection::close() noexcept {
    auto& self = *impl_;
    std::shared_ptr<impl::link> old;
    {
        std::lock_guard<std::mutex> lk(self.mutex);
        self.terminal = true;
        self.closed = true;
        self.error_code = rpc_code::shutting_down;
        self.buffer.clear();
        self.read_paused = false;
        self.write_busy = false;
        ++self.epoch;
        old = std::move(self.active);
    }
    if (old != nullptr && old->conn != nullptr) {
        old->conn->close();
    }
    self.notify_waiters();
}

bool connection::is_open() const noexcept {
    auto& self = *impl_;
    std::lock_guard<std::mutex> lk(self.mutex);
    return !self.terminal && !self.closed && self.active != nullptr &&
           self.active->conn != nullptr && self.active->conn->is_open();
}

status connection::reconnect() {
    return impl_->reconnect_now(true);
}

const connection_config& connection::config() const noexcept {
    return impl_->cfg;
}

std::size_t connection::buffered() const noexcept {
    auto& self = *impl_;
    std::lock_guard<std::mutex> lk(self.mutex);
    return self.buffer.size();
}

std::uint64_t connection::reconnect_count() const noexcept {
    auto& self = *impl_;
    std::lock_guard<std::mutex> lk(self.mutex);
    return self.reconnects;
}

std::uint64_t connection::bytes_read() const noexcept {
    auto& self = *impl_;
    std::lock_guard<std::mutex> lk(self.mutex);
    return self.bytes_read;
}

std::uint64_t connection::bytes_written() const noexcept {
    auto& self = *impl_;
    std::lock_guard<std::mutex> lk(self.mutex);
    return self.bytes_written;
}

// --- connection_pool ----------------------------------------------------------

struct connection_pool::impl {
    pool_config cfg;
    crpc::backend_factory& backend;
    runtime* shard;

    // One idle slot. The creation stamp is what makes max_lifetime measurable;
    // the idle stamp is what makes idle_timeout measurable. Both are taken from
    // steady_clock, so a wall-clock jump cannot make a connection look ancient.
    struct idle_entry {
        std::shared_ptr<connection> conn;
        std::chrono::steady_clock::time_point created;
        std::chrono::steady_clock::time_point idle_since;
    };

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::vector<idle_entry> idle;
    // Creation stamps for every live connection, keyed by identity. The pool hands
    // out shared_ptr<connection>, so the stamp cannot travel inside the object.
    std::unordered_map<const connection*, std::chrono::steady_clock::time_point> created;
    std::size_t total = 0;
    std::uint64_t opened = 0;
    bool closed = false;

    impl(pool_config config, crpc::backend_factory& factory, runtime* rt)
        : cfg(std::move(config)), backend(factory), shard(rt) {
        if (cfg.min_size > cfg.max_size) {
            cfg.min_size = cfg.max_size;
        }
        if (cfg.max_idle == 0 || cfg.max_idle > cfg.max_size) {
            cfg.max_idle = cfg.max_size;
        }
    }

    // True when the slot must be closed rather than reused.
    [[nodiscard]] bool expired(const idle_entry& entry,
                               std::chrono::steady_clock::time_point now) const noexcept {
        if (cfg.idle_timeout.count() > 0 && now - entry.idle_since >= cfg.idle_timeout) {
            return true;
        }
        return cfg.max_lifetime.count() > 0 && now - entry.created >= cfg.max_lifetime;
    }

    [[nodiscard]] bool too_old(std::chrono::steady_clock::time_point opened_at,
                               std::chrono::steady_clock::time_point now) const noexcept {
        return cfg.max_lifetime.count() > 0 && now - opened_at >= cfg.max_lifetime;
    }

    void notify_waiters() noexcept {
        cv.notify_all();
        if (shard != nullptr) {
            shard->notify();
        }
    }

    template <class Pred>
    status wait_for_pred(std::unique_lock<std::mutex>& lk, Pred pred,
                         std::chrono::nanoseconds timeout) {
        for (;;) {
            if (pred()) {
                return status{};
            }
            if (shard != nullptr && shard->loop_mode() && shard->is_owner_thread()) {
                lk.unlock();
                const status parked = shard->park();
                lk.lock();
                if (parked.ok()) {
                    continue;
                }
                if (parked.code() != rpc_code::failed_precondition) {
                    return parked;
                }
                // Not a coroutine: use the blocking path below.
            }
            if (!cv.wait_for(lk, timeout, pred)) {
                return status{rpc_code::deadline_exceeded};
            }
        }
    }
};

connection_pool::connection_pool(pool_config config,
                                 crpc::backend_factory& backend, runtime* shard)
    : impl_(std::make_unique<impl>(std::move(config), backend, shard)) {}

connection_pool::~connection_pool() = default;

result<std::shared_ptr<connection>> connection_pool::acquire() {
    auto& self = *impl_;
    std::unique_lock<std::mutex> lk(self.mutex);
    for (;;) {
        if (self.closed) {
            return rpc_error(rpc_code::shutting_down, "pool is closed");
        }
        while (!self.idle.empty()) {
            impl::idle_entry entry = std::move(self.idle.back());
            self.idle.pop_back();
            const auto now = std::chrono::steady_clock::now();
            if (entry.conn != nullptr && entry.conn->is_open() && !self.expired(entry, now)) {
                return std::move(entry.conn);
            }
            // A peer may have closed while the connection was idle, or the slot may
            // have outlived its idle/lifetime bound. Never hand out such a link;
            // close it and let the caller create a fresh one.
            if (entry.conn != nullptr) {
                entry.conn->close();
            }
            if (self.total > 0) {
                --self.total;
            }
        }
        if (self.total < self.cfg.max_size) {
            ++self.total;
            ++self.opened;
            lk.unlock();
            std::shared_ptr<connection> created;
            try {
                created = std::make_shared<connection>(self.cfg.connection,
                                                       self.backend, self.shard);
            } catch (...) {
                lk.lock();
                if (self.total > 0) {
                    --self.total;
                }
                return rpc_error(rpc_code::resource_exhausted,
                                 "connection allocation failed");
            }
            lk.lock();
            if (self.closed) {
                if (self.total > 0) {
                    --self.total;
                }
                return rpc_error(rpc_code::shutting_down, "pool is closed");
            }
            // Record the creation stamp before handing the connection out: the pool
            // only passes a shared_ptr, so the stamp cannot travel inside it.
            self.created[created.get()] = std::chrono::steady_clock::now();
            return created;
        }
        const status waited = self.wait_for_pred(
            lk,
            [&self] {
                return self.closed || !self.idle.empty() ||
                       self.total < self.cfg.max_size;
            },
            self.cfg.acquire_timeout);
        if (!waited.ok()) {
            if (waited.code() == rpc_code::deadline_exceeded) {
                return rpc_error(rpc_code::resource_exhausted, "pool exhausted");
            }
            return rpc_error(waited.code(), "pool acquire failed");
        }
    }
}

void connection_pool::release(std::shared_ptr<connection> conn) {
    if (!conn) {
        return;
    }
    auto& self = *impl_;
    bool drop = false;
    {
        std::lock_guard<std::mutex> lk(self.mutex);
        // A second release of an already-idle handle must change nothing. Treating
        // it as a drop would close a connection that is still in the idle set and
        // leave `total` disagreeing with idle+in_use.
        bool already_idle = false;
        for (const impl::idle_entry& entry : self.idle) {
            if (entry.conn == conn) {
                already_idle = true;
                break;
            }
        }
        if (already_idle) {
            self.notify_waiters();
            return;
        }
        if (self.closed || !conn->is_open()) {
            drop = true;
            if (self.total > 0) {
                --self.total;
            }
            self.created.erase(conn.get());
        } else {
            const auto now = std::chrono::steady_clock::now();
            const auto stamp = self.created.find(conn.get());
            const auto created_at = stamp == self.created.end() ? now : stamp->second;
            if (self.idle.size() >= self.cfg.max_idle || self.too_old(created_at, now)) {
                drop = true;
                if (self.total > 0) {
                    --self.total;
                }
                self.created.erase(conn.get());
            } else {
                self.idle.push_back(impl::idle_entry{std::move(conn), created_at, now});
            }
        }
        self.notify_waiters();
    }
    if (drop) {
        conn->close();
    }
}

void connection_pool::warmup() {
    auto& self = *impl_;
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(self.mutex);
            if (self.closed || self.total >= self.cfg.min_size) {
                return;
            }
            ++self.total;
        }

        std::shared_ptr<connection> created;
        try {
            created = std::make_shared<connection>(self.cfg.connection,
                                                   self.backend, self.shard);
        } catch (...) {
            std::lock_guard<std::mutex> lk(self.mutex);
            if (self.total > 0) {
                --self.total;
            }
            self.notify_waiters();
            return;
        }

        const bool open = created->is_open();
        bool stop = false;
        {
            std::lock_guard<std::mutex> lk(self.mutex);
            if (self.closed || !open) {
                if (self.total > 0) {
                    --self.total;
                }
                stop = true;
            } else {
                const auto now = std::chrono::steady_clock::now();
                self.created[created.get()] = now;
                self.idle.push_back(impl::idle_entry{std::move(created), now, now});
            }
            self.notify_waiters();
        }
        if (stop) {
            // Best effort: an unreachable endpoint cannot be warmed, and
            // retrying forever would turn warmup() into an unbounded loop.
            if (created != nullptr) {
                created->close();
            }
            return;
        }
    }
}

void connection_pool::close() noexcept {
    auto& self = *impl_;
    std::vector<impl::idle_entry> dropped;
    {
        std::lock_guard<std::mutex> lk(self.mutex);
        if (self.closed) {
            return;
        }
        self.closed = true;
        dropped.swap(self.idle);
        self.created.clear();
        self.total = 0;
        self.notify_waiters();
    }
    for (auto& entry : dropped) {
        if (entry.conn != nullptr) {
            entry.conn->close();
        }
    }
}

std::size_t connection_pool::idle() const noexcept {
    auto& self = *impl_;
    std::lock_guard<std::mutex> lk(self.mutex);
    return self.idle.size();
}

std::size_t connection_pool::in_use() const noexcept {
    auto& self = *impl_;
    std::lock_guard<std::mutex> lk(self.mutex);
    return self.total >= self.idle.size() ? self.total - self.idle.size()
                                          : std::size_t{0};
}

std::uint64_t connection_pool::opened() const noexcept {
    auto& self = *impl_;
    std::lock_guard<std::mutex> lk(self.mutex);
    return self.opened;
}

std::size_t connection_pool::total() const noexcept {
    auto& self = *impl_;
    std::lock_guard<std::mutex> lk(self.mutex);
    return self.total;
}

} // namespace net
} // namespace crpc
