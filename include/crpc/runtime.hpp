// CRPC -- shard runtime: one thread, one coroutine runtime, one timer wheel.
//
// Thread model
// ------------
// A `runtime` is the unit of affinity described by the design (section 4): the
// owning thread runs `run()`, which drives one corocc runtime in LOOP mode, a
// timer wheel, a bounded cross-shard inbox and a fixed handler-coroutine pool.
// Coroutines only ever execute on the owning thread; a foreign thread influences
// the shard exclusively through `post()`, `submit()`, `spawn()`, `arm_timer()`
// and `notify()`, each of which enqueues work or rings the doorbell.
//
// Bounded work and no stall
// -------------------------
//   * `post()` and `spawn()` target the inbox (default 65536 entries).
//   * `submit()` targets the handler ready queue (default 65536 entries).
//   * Every enqueue is non-blocking. A full queue returns false; callers map
//     that to `rpc_code::resource_exhausted`. No path waits forever.
//
// Timer wheel
// -----------
// Timers are keyed by an opaque pointer and ordered by absolute monotonic
// nanosecond deadline. `next_deadline_ns()` lets the loop compute its wait
// timeout, so an idle shard sleeps until the nearest deadline instead of
// polling. Timer callbacks run on the owning thread.
//
// Coroutine availability
// ----------------------
// When corocc is unavailable (`CRPC_HAVE_COROCC` undefined) the same API runs
// in a documented thread-per-shard fallback: `post`/`submit` invoke the task
// directly on the shard thread, `spawn` invokes the function as a plain call,
// and `park`/`notify` degenerate to failures/no-ops. The RPC facade detects
// that and uses its blocking wait path, so the library still works; it simply
// does not gain cooperative suspension.
#ifndef CRPC_RUNTIME_HPP
#define CRPC_RUNTIME_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

extern "C" {
struct coro_rt;
}

namespace crpc {

struct runtime_options {
    // Long-lived handler coroutines per shard.
    std::size_t handler_workers = 64;
    // Stack size requested for handler coroutines.
    std::size_t handler_stack_bytes = 64 * 1024;
    // Bounded cross-shard inbox, in entries.
    std::size_t inbox_capacity = 65536;
    // Bounded handler ready queue, in entries.
    std::size_t ready_capacity = 65536;
    // Maximum runnable coroutine nodes driven per pump turn.
    std::size_t pump_budget = 256;
    // Longest single sleep when nothing is armed.
    std::chrono::nanoseconds idle_wait{std::chrono::milliseconds(100)};
    // Set false for a shard that only needs the inbox and timer wheel.
    bool start_handler_pool = true;
};

// Runs one shard. Non-copyable, non-movable.
class CRPC_API runtime {
public:
    // Executed on the shard thread. Must not throw.
    using task_fn = void (*)(void* ctx) noexcept;
    // Timer callback, executed on the shard thread.
    using timer_fn = void (*)(void* ctx) noexcept;
    // Returns the next absolute monotonic deadline the shard should honour, or
    // `no_deadline` when it has none. Called on the shard thread every turn.
    using tick_fn = std::uint64_t (*)(void* ctx) noexcept;
    // Coroutine entry point (matches coro_fn).
    using spawn_fn = void (*)(void* ctx);

    static constexpr std::uint64_t no_deadline = ~std::uint64_t{0};

    explicit runtime(runtime_options options = {});
    ~runtime();

    runtime(const runtime&) = delete;
    runtime& operator=(const runtime&) = delete;
    runtime(runtime&&) = delete;
    runtime& operator=(runtime&&) = delete;

    // Spawns a thread that runs `run()`; idempotent.
    void start_thread();

    // Blocks the calling thread and pumps until `stop()` is requested and the
    // shard has quiesced.
    void run();

    // Requests shutdown. Idempotent and callable from any thread. Does not
    // block (join from another thread with `join()`).
    void stop() noexcept;

    // Joins the thread started by `start_thread()`. Idempotent.
    void join();

    [[nodiscard]] bool stopped() const noexcept;
    [[nodiscard]] bool is_owner_thread() const noexcept;
    [[nodiscard]] bool running() const noexcept;

    [[nodiscard]] std::uint64_t now_ns() const noexcept;

    // Called for a queued task whose context will never be run, because the
    // shard stopped first. Without it a task posted during shutdown would leak
    // whatever `ctx` owns (the HTTP server hit exactly that: a
    // `new shared_ptr<session>` per scheduled wakeup).
    using drop_fn = void (*)(void* ctx) noexcept;

    // Cross-shard submission. Runs `fn(ctx)` inline on the shard thread.
    // Returns false when the inbox is full.
    [[nodiscard]] bool post(task_fn fn, void* ctx) noexcept;

    // Same, but `drop(ctx)` releases the context if the shard stops before the
    // task runs. It must not run `fn`: the owner may already be torn down.
    [[nodiscard]] bool post(task_fn fn, void* ctx, drop_fn drop) noexcept;

    // Handler-pool submission. Runs `fn(ctx)` on a worker coroutine, which may
    // suspend without blocking the reader. Returns false when the queue is full.
    [[nodiscard]] bool submit(task_fn fn, void* ctx) noexcept;

    // Same, with a `drop(ctx)` for the shutdown case (see `post`).
    [[nodiscard]] bool submit(task_fn fn, void* ctx, drop_fn drop) noexcept;

    // Spawns a coroutine on this shard. Returns false when the inbox is full.
    [[nodiscard]] bool spawn(spawn_fn fn, void* ctx) noexcept;

    // Timer wheel (callable from any thread; state guarded by a mutex). Arms or
    // replaces the timer identified by `key`. Returns false only for a bad
    // argument or an allocation failure.
    bool arm_timer(void* key, std::uint64_t deadline_ns, timer_fn fn, void* arg) noexcept;

    // Same, but keeps `keep` alive until the callback returns. The wheel pops
    // an entry under its lock and invokes the callback after releasing it; an
    // owner that frees the callback context in that window would otherwise
    // leave `arg` dangling.
    bool arm_timer(void* key, std::uint64_t deadline_ns, timer_fn fn, void* arg,
                   std::shared_ptr<void> keep) noexcept;

    // Cancels the timer identified by `key`. Returns true when one was armed.
    bool cancel_timer(void* key) noexcept;

    [[nodiscard]] std::uint64_t next_deadline_ns() const noexcept;

    // Installs the per-turn deadline hint (used by the client facade to poll
    // call deadlines). Pass nullptr to remove it.
    void set_tick(tick_fn fn, void* ctx) noexcept;

    // Rings the doorbell. Cheap, idempotent and callable from any thread.
    void wake() noexcept;

    // Coroutine suspension helper for the RPC facade.
    //
    // `park()` parks the current coroutine until `notify()` (or a timeout tied
    // to the shard turn). It returns failed_precondition when called outside a
    // coroutine of this runtime; callers must then use their blocking path.
    // `notify()` may be called from any thread.
    [[nodiscard]] status park() noexcept;
    // Parks the current coroutine until an absolute monotonic deadline. Returns
    // failed_precondition outside a coroutine; callers then fall back to a
    // plain-thread wait.
    [[nodiscard]] status sleep_until(std::uint64_t deadline_ns) noexcept;
    void notify() noexcept;

    [[nodiscard]] std::size_t pending_coroutines() const noexcept;
    [[nodiscard]] std::size_t inbox_size() const noexcept;
    [[nodiscard]] std::size_t ready_size() const noexcept;

    // Native corocc runtime, or nullptr in fallback mode / before start.
    [[nodiscard]] struct coro_rt* native() const noexcept;

    // True when the shard is actually driven in corocc LOOP mode (a pump owns
    // worker[0]). False for the thread-per-shard fallback.
    [[nodiscard]] bool loop_mode() const noexcept;

    struct impl;

private:
    std::unique_ptr<impl> impl_;
};

} // namespace crpc

#endif // CRPC_RUNTIME_HPP
