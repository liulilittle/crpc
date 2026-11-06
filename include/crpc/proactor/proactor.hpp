// CRPC -- completion-driven I/O (the proactor pattern).
//
// A proactor is the "completion" half of event-driven I/O: the caller submits
// an operation and is told when it finished, rather than being told when it
// can be attempted. This header adapts the existing `io::` contract
// (`connection`, `sink`, `source`, `completion`) instead of inventing a second
// I/O layer:
//
//   * `open` establishes a connection through the backend's
//     `backend_factory::connect` and installs the caller's `io::source`
//     with `bind_source`, so incoming bytes are delivered to exactly the same
//     source type the rest of the framework already uses;
//   * `submit` hands the span to the connection's `io::sink::push` and records
//     the operation as in flight;
//   * every submitted operation calls its `io::completion` exactly once --
//     success, failure or cancellation -- and never twice. The user completion
//     is only ever reached through one atomic gate, so a backend that (against
//     I1) completed twice could not double-complete the caller; the second call
//     is counted in `double_completions()` instead.
//
// Cancellation
// ------------
// `connection` has no per-operation cancel (io.hpp deliberately keeps
// establishment / close / cancel off `sink`). The contract's abortive
// `close()` *does* cancel the in-flight write, and I4 guarantees that the
// cancellation still runs the completion exactly once. `cancel()` therefore
// closes the connection; the pending completion observes
// `std::errc::operation_canceled`. A connection with no write in flight has
// nothing to cancel and returns `not_found`.
//
// Lifetime contract
// -----------------
//   * The proactor owns every connection returned by `open` and destroys them
//     in its destructor. The raw pointer stays valid until then.
//   * The `io::source` passed to `open` is borrowed (the backend stores a raw
//     pointer), so it must outlive the proactor.
//   * The span passed to `submit` is borrowed until the completion runs (I7),
//     so the buffer must stay alive and unmodified until then.
//   * The `io::completion` passed to `submit` is borrowed until it runs.
//   * The destructor closes every connection and waits for the in-flight
//     completions the close promised. It therefore must not run on a thread
//     that is itself blocked inside a completion of this proactor.
//
// Thread contract
// ---------------
//   * `open`, `submit`, `cancel`, `pump` and the diagnostics may be called
//     from any thread; the proactor's own bookkeeping is mutex-guarded.
//   * `io::completion::complete` may run on the submitting thread (a write
//     that fits), on the backend's I/O thread, or on the thread that closes
//     the connection. A completion runs with no proactor lock held, so it may
//     call back into `submit` (subject to I5: not synchronously on the same
//     connection).
//   * `pump` blocks on a condition variable for at most its timeout; it is not
//     a spin loop.
//
// Rejection vs. submission
// ------------------------
// `submit` returning a non-ok status means the operation was *rejected*: it
// was never handed to the backend and no completion will run. `submit`
// returning ok is the exactly-once promise: one completion, eventually.
//
// ABI: header-only template, no exported symbols, additive (MINOR).
#ifndef CRPC_PROACTOR_PROACTOR_HPP
#define CRPC_PROACTOR_PROACTOR_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/io.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace proactor {

// A completion-aware facade over an `backend_factory` implementation.
template <typename Backend>
class proactor {
    static_assert(std::is_base_of<backend_factory, Backend>::value,
                  "a proactor Backend must implement backend_factory");

public:
    explicit proactor(Backend& backend) : backend_(backend) {}

    proactor(const proactor&) = delete;
    proactor& operator=(const proactor&) = delete;
    proactor(proactor&&) = delete;
    proactor& operator=(proactor&&) = delete;

    // Closes and destroys every connection this proactor opened, then waits
    // for the canceled in-flight operations to report their single completion.
    ~proactor() { close_all(); }

    // Connects through the backend and binds `src`. The returned pointer is
    // owned by this proactor. Fails with:
    //   * invalid_argument      -- the endpoint is not usable (`!ep.valid()`);
    //   * connection_refused    -- the backend could not connect (unknown name,
    //                              missing listener, refused TCP, ...).
    result<connection*> open(const endpoint& ep, io::source& src) {
        if (!ep.valid()) {
            return rpc_error{rpc_code::invalid_argument,
                             "proactor: the endpoint is not valid"};
        }
        std::unique_ptr<connection> established =
            backend_.connect(ep, connect_options{});
        if (!established) {
            return rpc_error{rpc_code::connection_refused,
                             "proactor: the backend could not open the endpoint"};
        }
        established->bind_source(&src);
        connection* raw = established.get();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connections_.push_back(std::move(established));
            managed_.insert(raw);
        }
        return raw;
    }

    // Submits one write. On success exactly one completion follows (I1); on a
    // non-ok return the operation was rejected and no completion runs.
    // Rejections:
    //   * failed_precondition -- `c` was not opened by this proactor, or it
    //     already has a write in flight (I5: one push per direction).
    //   * resource_exhausted  -- the bookkeeping allocation failed.
    status submit(connection& c, span<const std::byte> data,
                  io::completion& done) {
        std::shared_ptr<operation> op;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (managed_.find(&c) == managed_.end()) {
                return rpc_error{rpc_code::failed_precondition,
                                 "proactor: the connection is not managed here"};
            }
            if (in_flight_.find(&c) != in_flight_.end()) {
                return rpc_error{rpc_code::failed_precondition,
                                 "proactor: a write is already in flight (I5)"};
            }
            try {
                op = std::make_shared<operation>(this, &c, &done);
            } catch (...) {
                return rpc_error{rpc_code::resource_exhausted,
                                 "proactor: could not allocate the operation"};
            }
            in_flight_.emplace(&c, op);
        }
        // The completion may run before push() returns (a write that fits);
        // the record is already in the map, so that path removes it normally.
        c.sink().push(data, op.get());
        return {};
    }

    // Cancels the in-flight write on `c`, producing its single completion.
    // connection has no finer-grained cancel, so this closes the
    // connection. Returns:
    //   * failed_precondition -- `c` is not managed by this proactor;
    //   * not_found           -- no write is in flight (nothing to cancel).
    status cancel(connection& c) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (managed_.find(&c) == managed_.end()) {
                return rpc_error{rpc_code::failed_precondition,
                                 "proactor: the connection is not managed here"};
            }
            if (in_flight_.find(&c) == in_flight_.end()) {
                return rpc_error{rpc_code::not_found,
                                 "proactor: no write is in flight to cancel"};
            }
        }
        // Closing outside the lock matters: the canceled completion runs
        // synchronously on this thread for most backends, and it needs the
        // proactor mutex to retire its record.
        c.close();
        return {};
    }

    // Waits up to `timeout` for completions and returns how many finished
    // (success, failure or cancellation) since the previous pump. With
    // nothing outstanding the demultiplexer equivalent -- the condition
    // variable -- is waited out rather than spun on, so an idle pump costs no
    // CPU. `timeout == 0` returns immediately with the count so far.
    std::size_t pump(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (pending_completions_ == 0 && timeout.count() > 0) {
            (void)cv_.wait_for(lock, timeout,
                               [this] { return pending_completions_ != 0; });
        }
        const std::size_t count = pending_completions_;
        pending_completions_ = 0;
        return count;
    }

    // Writes submitted and not yet completed.
    [[nodiscard]] std::size_t outstanding() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return in_flight_.size();
    }

    // Completions delivered since construction, including repeated pumps.
    [[nodiscard]] std::size_t completed() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return completed_count_;
    }

    // Second `complete` calls observed from the backend. A conforming backend
    // keeps this at zero (I1); the counter exists so a violation is visible
    // instead of silently reaching the caller twice.
    [[nodiscard]] std::uint64_t double_completions() const noexcept {
        return double_completions_.load(std::memory_order_relaxed);
    }

    // Number of connections this proactor owns.
    [[nodiscard]] std::size_t connections() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return connections_.size();
    }

private:
    // One submitted write. Its address is what the backend holds, so it lives
    // as long as the backend may still call `complete`; the in-flight map's
    // shared_ptr provides that lifetime.
    class operation final : public io::completion {
    public:
        operation(proactor* owner, connection* conn,
                  io::completion* user) noexcept
            : owner_(owner), conn_(conn), user_(user) {}

        operation(const operation&) = delete;
        operation& operator=(const operation&) = delete;

        void complete(io::io_result result) noexcept override {
            // The single gate: whoever gets here first forwards the completion,
            // everybody else is a contract violation and is only counted.
            if (completed_.exchange(true, std::memory_order_acq_rel)) {
                owner_->double_completions_.fetch_add(
                    1, std::memory_order_relaxed);
                return;
            }
            owner_->finish_one(this, result);
        }

        [[nodiscard]] connection* conn() const noexcept { return conn_; }
        [[nodiscard]] io::completion* user() const noexcept { return user_; }

    private:
        proactor* owner_;
        connection* conn_;
        io::completion* user_;
        std::atomic<bool> completed_{false};
    };

    // Runs once per operation, from whichever thread the backend chose.
    void finish_one(operation* op, io::io_result result) noexcept {
        io::completion* user = nullptr;
        // Keeps the operation alive until after the user completion runs, even
        // though the map entry below is the only owner the proactor keeps.
        std::shared_ptr<operation> keep;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = in_flight_.find(op->conn());
            if (it != in_flight_.end() && it->second.get() == op) {
                keep = std::move(it->second);
                in_flight_.erase(it);
            }
            ++completed_count_;
            ++pending_completions_;
            user = op->user();
            // Notified under the lock so the destructor's wait cannot observe
            // the CV being destroyed by a late notifier.
            cv_.notify_all();
        }
        if (user != nullptr) {
            user->complete(result);
        }
    }

    // Abortive close of every owned connection, then wait for the in-flight
    // operations those closes canceled. No lock is held while closing because
    // the cancellation completes synchronously and re-enters finish_one.
    void close_all() noexcept {
        std::vector<connection*> all;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            all.reserve(connections_.size());
            for (const auto& connection : connections_) {
                all.push_back(connection.get());
            }
        }
        for (connection* connection : all) {
            connection->close();
        }
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return in_flight_.empty(); });
        }
        std::lock_guard<std::mutex> lock(mutex_);
        managed_.clear();
        connections_.clear();
    }

    Backend& backend_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::unique_ptr<connection>> connections_;
    std::unordered_set<connection*> managed_;
    std::unordered_map<connection*, std::shared_ptr<operation>> in_flight_;
    std::size_t pending_completions_ = 0;
    std::size_t completed_count_ = 0;
    std::atomic<std::uint64_t> double_completions_{0};
};

} // namespace proactor
} // namespace crpc

#endif // CRPC_PROACTOR_PROACTOR_HPP
