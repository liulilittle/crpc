// CRPC -- message-driven actors: bounded mailbox, executor, lifecycle.
//
// An actor is a single logical consumer with a bounded inbox. `post` hands a
// message to an `async::executor` task, which drains the mailbox in order and
// calls the handler. The three rules that make it safe to use from a result-
// returning library:
//
//   * **bounded inbox, refusal instead of growth.** A full mailbox makes
//     `post` return `rpc_code::resource_exhausted` immediately. The actor never
//     blocks the producer, never grows without limit and never retries.
//   * **stop drains.** `stop()` refuses new posts, then processes every message
//     that was accepted before it. It is idempotent: calling it twice, or from
//     inside a handler, is safe and processes nothing twice.
//   * **a failing handler is data, not a crash.** A handler that returns a
//     non-ok status or throws is counted in `handler_failures()` and the actor
//     continues with the next message.
//
// Execution model
// ---------------
// There is no dedicated actor thread. Posting schedules one coalesced "drain"
// task on the borrowed executor; any number of posts share that one task, so N
// messages cost one submission, not N. If the executor refuses the task (it is
// shut down, or its own queue is full), the posting thread drains inline
// instead of losing the message: thread affinity is best effort, message
// delivery is not.
//
// `async::executor` requires a non-void result, so the drain task returns an
// `int` sentinel; that is a quirk of the existing executor, not of the actor.
//
// Handlers are serialized by a per-actor recursive mutex (recursive so a
// handler may call `stop()`); different actors run concurrently. `stop()`
// called from another thread waits for a running handler and then drains the
// rest itself. `stop()` called from inside a handler marks shutdown and lets
// the drain loop already on that thread finish the queue -- the same thread is
// still inside the drain, so no message can be stranded.
//
// Thread contract
// ---------------
//   * `post` may be called from any thread and from inside a handler.
//   * `stop` may be called from any thread and from inside a handler.
//   * The handler runs on an executor worker, or on a thread that called
//     `post`/`stop` in the inline fallback, never concurrently with itself for
//     one actor.
//   * `pending`, `handler_failures` and `running` may be called from any
//     thread.
//
// Lifetime
// --------
// The executor is borrowed and must outlive the actor. Destroying the actor
// stops it (draining first). The mailbox and handler live in a shared state
// that a queued drain task keeps alive, so a task left behind after the actor
// is gone only pops an empty mailbox and exits.
//
// ABI: header-only template, no exported symbols, additive (MINOR).
#ifndef CRPC_ACTOR_ACTOR_HPP
#define CRPC_ACTOR_ACTOR_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include <crpc/actor/mailbox.hpp>
#include <crpc/async/executor.hpp>
#include <crpc/cache_line.hpp>
#include <crpc/config.hpp>
#include <crpc/error.hpp>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

namespace crpc {
namespace actor {

// A single-consumer message handler with a bounded inbox.
template <typename Message>
class actor {
public:
    using handler = std::function<status(Message&&)>;

    actor(async::executor& exec, std::size_t mailbox_capacity, handler h)
        : state_(std::make_shared<state>(exec, mailbox_capacity, std::move(h))) {}

    actor(const actor&) = delete;
    actor& operator=(const actor&) = delete;
    actor(actor&&) = delete;
    actor& operator=(actor&&) = delete;

    ~actor() { stop(); }

    // Accepts one message. Failures:
    //   * shutting_down       -- `stop()` was called (before or meanwhile);
    //   * resource_exhausted  -- the bounded mailbox is full;
    //   * internal            -- the message's move constructor threw.
    // A rejected message is not enqueued and never reaches the handler.
    [[nodiscard]] status post(Message m) {
        const std::shared_ptr<state> st = state_;
        {
            // The gate pairs the stop flag with the enqueue: once stop() has
            // set the flag under this mutex, no post can slip a message in
            // behind the drain.
            std::lock_guard<std::mutex> gate(st->post_mutex);
            if (st->stopped.load(std::memory_order_seq_cst)) {
                return rpc_error{rpc_code::shutting_down,
                                 "actor: the actor has been stopped"};
            }
            const status queued = st->box.try_post(std::move(m));
            if (!queued.ok()) {
                return queued;
            }
        }
        st->schedule_drain();
        return {};
    }

    // Refuses new posts, processes what was accepted so far, and returns.
    // Idempotent; safe from a handler. After the first call `running()` is
    // false and later posts fail with `shutting_down`.
    void stop() {
        const std::shared_ptr<state> st = state_;
        try {
            {
                std::lock_guard<std::mutex> gate(st->post_mutex);
                st->stopped.store(true, std::memory_order_seq_cst);
            }
            st->drain_all();
        } catch (...) {
            // A destructor calls this; nothing may escape.
        }
    }

    [[nodiscard]] bool running() const noexcept {
        return !state_->stopped.load(std::memory_order_seq_cst);
    }

    // Messages accepted but not yet popped. Approximate while the actor runs.
    [[nodiscard]] std::size_t pending() const noexcept {
        return state_->box.size();
    }

    // Handler invocations that returned a non-ok status or threw. Diagnostic;
    // the actor does not retry them.
    [[nodiscard]] std::uint64_t handler_failures() const noexcept {
        return state_->handler_failures.load(std::memory_order_seq_cst);
    }

private:
    struct state : std::enable_shared_from_this<state> {
        state(async::executor& exec_, std::size_t capacity, handler h)
            : exec(&exec_), box(capacity), handle(std::move(h)) {}

        state(const state&) = delete;
        state& operator=(const state&) = delete;

        // Submits the single coalesced drain task. If the executor cannot take
        // it, the caller drains inline (see the header comment).
        void schedule_drain() {
            bool expected = false;
            if (!drain_scheduled.compare_exchange_strong(
                    expected, true, std::memory_order_seq_cst)) {
                return; // A drain is already queued or running.
            }
            std::shared_ptr<state> self;
            try {
                self = this->shared_from_this();
            } catch (...) {
                self.reset();
            }
            if (self) {
                try {
                    // The executor only accepts non-void callables (its future
                    // has no void specialisation), hence the int sentinel.
                    auto future = exec->submit([self] {
                        self->run_drain();
                        return 0;
                    });
                    if (future.valid()) {
                        return;
                    }
                } catch (...) {
                    // Fall through to the inline drain.
                }
            }
            // The executor refused (closed or full). Keep the flag claimed so
            // a handler that posts during the inline drain cannot nest another
            // drain; run_drain() clears it when the mailbox is empty.
            run_drain();
        }

        // Executor task body. Pops and handles until the mailbox is empty, then
        // releases the coalescing flag. The re-check after releasing closes the
        // window where a post enqueued just before the flag was cleared.
        void run_drain() noexcept {
            try {
                for (;;) {
                    {
                        std::lock_guard<std::recursive_mutex> lock(handler_mutex);
                        std::optional<Message> next = box.try_pop_value();
                        if (next) {
                            handle_locked(std::move(*next));
                            continue;
                        }
                    }
                    drain_scheduled.store(false, std::memory_order_seq_cst);
                    if (box.size() == 0) {
                        return;
                    }
                    bool expected = false;
                    if (!drain_scheduled.compare_exchange_strong(
                            expected, true, std::memory_order_seq_cst)) {
                        // Another task claimed the drain; it sees the message.
                        return;
                    }
                }
            } catch (...) {
                drain_scheduled.store(false, std::memory_order_seq_cst);
            }
        }

        // Synchronous drain used by stop(). Re-entrant calls (a handler calling
        // stop()) return immediately: the drain loop already active on this
        // thread will finish the queue, and the recursive mutex proves it is
        // that same thread.
        void drain_all() noexcept {
            try {
                std::lock_guard<std::recursive_mutex> lock(handler_mutex);
                if (draining) {
                    return;
                }
                draining = true;
                for (;;) {
                    std::optional<Message> next = box.try_pop_value();
                    if (!next) {
                        break;
                    }
                    handle_locked(std::move(*next));
                }
                draining = false;
            } catch (...) {
                draining = false;
            }
        }

        void handle_locked(Message&& message) noexcept {
            try {
                const status outcome = handle(std::move(message));
                if (!outcome.ok()) {
                    handler_failures.fetch_add(1, std::memory_order_relaxed);
                }
            } catch (...) {
                handler_failures.fetch_add(1, std::memory_order_relaxed);
            }
        }

        async::executor* exec;
        mpsc_mailbox<Message> box;
        handler handle;

        std::mutex post_mutex;
        std::recursive_mutex handler_mutex;
        // Serialises "a synchronous drain is running" against re-entrant
        // stop(); always accessed with handler_mutex held, so it is that
        // mutex -- not this flag -- that makes it safe.
        bool draining = false;

        // The lifecycle flags sit on their own cache lines, apart from the
        // mailbox cursors (which mailbox itself already separates).
        CRPC_CACHE_LINE_ALIGN std::atomic<bool> stopped{false};
        CRPC_CACHE_LINE_ALIGN std::atomic<bool> drain_scheduled{false};
        std::atomic<std::uint64_t> handler_failures{0};
    };

    std::shared_ptr<state> state_;
};

} // namespace actor
} // namespace crpc

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#endif // CRPC_ACTOR_ACTOR_HPP
