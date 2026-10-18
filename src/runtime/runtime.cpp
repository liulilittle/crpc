// CRPC -- shard runtime implementation.
#include <crpc/runtime.hpp>

#include <deque>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(CRPC_HAVE_COROCC)
#include <corocc/coro.h>
#include <corocc/coro_rt.h>
#include <corocc/coro_rt_driver.h>
#endif

namespace crpc {
namespace {

struct task_slot {
    runtime::task_fn fn = nullptr;
    void* ctx = nullptr;
};

struct inbox_entry {
    enum class kind : std::uint8_t { task, spawn };
    kind k = kind::task;
    runtime::task_fn task = nullptr;
    runtime::spawn_fn spawn = nullptr;
    void* ctx = nullptr;
};

struct timer_entry {
    void* key = nullptr;
    runtime::timer_fn fn = nullptr;
    void* arg = nullptr;
};

} // namespace

struct runtime::impl {
    runtime* self = nullptr;
    runtime_options opt;

    std::thread::id owner{};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> running{false};
    std::atomic<bool> wake_flag{false};

    // Cross-shard inbox.
    mutable std::mutex inbox_mutex;
    std::deque<inbox_entry> inbox;

    // Handler ready queue.
    mutable std::mutex ready_mutex;
    std::deque<task_slot> ready;

    // Timer wheel.
    mutable std::mutex timer_mutex;
    std::map<std::pair<std::uint64_t, std::uint64_t>, timer_entry> timers;
    std::unordered_map<void*, std::pair<std::uint64_t, std::uint64_t>> timer_index;
    std::uint64_t timer_seq = 0;

    // Doorbell.
    std::mutex wait_mutex;
    std::condition_variable wait_cv;

    // Tick hook.
    std::atomic<tick_fn> tick{nullptr};
    std::atomic<void*> tick_ctx{nullptr};

    // Native coroutine runtime.
    std::mutex native_mutex;
    bool native_alive = false;
    bool pump_mode = false;

    std::thread worker;

#if defined(CRPC_HAVE_COROCC)
    coro_rt_t* rt = nullptr;
    coro_rt_driver_t* driver = nullptr;
    coro_rt_sem_t ready_sem{};
    coro_rt_sem_t wake_sem{};
#endif

    impl(runtime* owner, runtime_options options) : self(owner), opt(std::move(options)) {
#if defined(CRPC_HAVE_COROCC)
        // Each live runtime gets a unique registration name; the object
        // address is unique among live runtimes and needs no shared counter.
        const std::string name = "crpc-shard-" +
                                 std::to_string(reinterpret_cast<std::uintptr_t>(this));

        coro_rt_driver_ops ops{};
        ops.struct_size = static_cast<std::uint32_t>(sizeof(ops));
        ops.flags = CORO_DRIVER_FLAG_LOOP;
        ops.now = &impl::driver_now;
        ops.wake = &impl::driver_wake;
        ops.timer_arm = nullptr;
        ops.timer_cancel = nullptr;

        if (coro_rt_driver_register(name.c_str(), &ops, this, &driver) != CORO_OK) {
            driver = nullptr;
            return;
        }
        rt = coro_rt_create(1);
        if (rt == nullptr) {
            coro_rt_driver_unregister(driver);
            driver = nullptr;
            return;
        }
        if (coro_rt_attach_driver(rt, driver) == CORO_OK) {
            pump_mode = true;
        }
        native_alive = true;
        coro_rt_sem_init(&ready_sem, 0);
        coro_rt_sem_init(&wake_sem, 0);
#endif
    }

    ~impl() {
        // The owning runtime joins and destroys first; this is only a guard for
        // a runtime that was never run.
#if defined(CRPC_HAVE_COROCC)
        std::lock_guard<std::mutex> lk(native_mutex);
        if (rt != nullptr) {
            native_alive = false;
            (void)coro_rt_destroy(rt);
            rt = nullptr;
        }
        if (driver != nullptr) {
            (void)coro_rt_driver_unregister(driver);
            driver = nullptr;
        }
#endif
    }

    [[nodiscard]] std::uint64_t now_ns() const noexcept {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    static std::uint64_t driver_now(void* ctx) noexcept {
        return static_cast<impl*>(ctx)->now_ns();
    }

    static void driver_wake(void* ctx) noexcept {
        static_cast<impl*>(ctx)->self->wake();
    }

    void wake() noexcept {
        // The store must be serialized with the waiter's predicate evaluation.
        // The loop clears the flag from inside the wait predicate while holding
        // wait_mutex (the mutex `wait_for` uses). Storing the flag without that
        // lock leaves a lost-wakeup window: the waiter can evaluate the
        // predicate (false), the producer then stores the flag and calls
        // notify_all() before the waiter has registered, and the waiter then
        // blocks until the timeout -- sleeping the full idle_wait after work
        // became available. Taking wait_mutex here means the store is either
        // observed by the predicate or happens after the waiter is enqueued on
        // the condition variable, so the notification is never lost.
        {
            std::lock_guard<std::mutex> lk(wait_mutex);
            wake_flag.store(true, std::memory_order_release);
        }
        wait_cv.notify_all();
    }

    bool push_inbox(inbox_entry entry) noexcept {
        if (stop_requested.load(std::memory_order_acquire)) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(inbox_mutex);
            if (inbox.size() >= opt.inbox_capacity) {
                return false;
            }
            inbox.push_back(entry);
        }
        wake();
        return true;
    }

    bool push_ready(task_slot slot) noexcept {
        if (stop_requested.load(std::memory_order_acquire)) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(ready_mutex);
            if (ready.size() >= opt.ready_capacity) {
                return false;
            }
            ready.push_back(slot);
        }
#if defined(CRPC_HAVE_COROCC)
        {
            std::lock_guard<std::mutex> lk(native_mutex);
            if (native_alive && rt != nullptr) {
                (void)coro_rt_sem_post(rt, &ready_sem);
            }
        }
#endif
        wake();
        return true;
    }

    [[nodiscard]] bool pop_ready(task_slot& out) noexcept {
        std::lock_guard<std::mutex> lk(ready_mutex);
        if (ready.empty()) {
            return false;
        }
        out = ready.front();
        ready.pop_front();
        return true;
    }

    void drain_inbox() noexcept {
        std::deque<inbox_entry> local;
        {
            std::lock_guard<std::mutex> lk(inbox_mutex);
            if (inbox.empty()) {
                return;
            }
            local.swap(inbox);
        }
        for (inbox_entry& entry : local) {
            if (entry.k == inbox_entry::kind::task) {
                entry.task(entry.ctx);
                continue;
            }
#if defined(CRPC_HAVE_COROCC)
            if (pump_mode && rt != nullptr) {
                coro_attr_t attr = coro_attr_default();
                attr.stack_size = opt.handler_stack_bytes;
                attr.name = "crpc-task";
                if (coro_rt_spawn(rt, &attr, entry.spawn, entry.ctx) == CORO_OK) {
                    continue;
                }
            }
#endif
            // Fallback: run the entry as a plain call on the shard thread.
            entry.spawn(entry.ctx);
        }
    }

    void run_timers() noexcept {
        const std::uint64_t now = now_ns();
        for (;;) {
            timer_entry entry;
            {
                std::lock_guard<std::mutex> lk(timer_mutex);
                if (timers.empty()) {
                    return;
                }
                auto it = timers.begin();
                if (it->first.first > now) {
                    return;
                }
                entry = it->second;
                timer_index.erase(entry.key);
                timers.erase(it);
            }
            entry.fn(entry.arg);
        }
    }

    [[nodiscard]] std::uint64_t next_deadline() noexcept {
        std::uint64_t next = runtime::no_deadline;
        {
            std::lock_guard<std::mutex> lk(timer_mutex);
            if (!timers.empty()) {
                next = timers.begin()->first.first;
            }
        }
        const tick_fn fn = tick.load(std::memory_order_acquire);
        if (fn != nullptr) {
            const std::uint64_t hint = fn(tick_ctx.load(std::memory_order_acquire));
            if (hint < next) {
                next = hint;
            }
        }
        return next;
    }

    void handler_main() {
        for (;;) {
            if (stop_requested.load(std::memory_order_acquire)) {
                return;
            }
            task_slot slot;
            if (!pop_ready(slot)) {
#if defined(CRPC_HAVE_COROCC)
                if (rt != nullptr) {
                    (void)coro_rt_sem_wait(rt, &ready_sem);
                }
#else
                std::this_thread::yield();
#endif
                continue;
            }
            if (slot.fn != nullptr) {
                slot.fn(slot.ctx);
            }
        }
    }

    static void handler_entry(void* ctx) {
        static_cast<impl*>(ctx)->handler_main();
    }

    void quiesce() noexcept {
        // Release the handler coroutines: each one observes `stop_requested`
        // after its next wake and returns.
#if defined(CRPC_HAVE_COROCC)
        {
            std::lock_guard<std::mutex> lk(native_mutex);
            if (rt != nullptr) {
                for (std::size_t i = 0; i < opt.handler_workers; ++i) {
                    (void)coro_rt_sem_post(rt, &ready_sem);
                }
            }
        }
        const std::uint64_t deadline = now_ns() + 2'000'000'000ull;
        while (rt != nullptr && coro_rt_pending(rt) > 0 && now_ns() < deadline) {
            drain_inbox();
            run_timers();
            if (pump_mode) {
                (void)coro_rt_pump(rt, 64);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
#endif
    }

    void destroy_native() noexcept {
#if defined(CRPC_HAVE_COROCC)
        std::lock_guard<std::mutex> lk(native_mutex);
        native_alive = false;
        if (rt != nullptr) {
            (void)coro_rt_destroy(rt);
            rt = nullptr;
        }
        if (driver != nullptr) {
            (void)coro_rt_driver_unregister(driver);
            driver = nullptr;
        }
#endif
    }

    void run_loop() {
        owner = std::this_thread::get_id();
        running.store(true, std::memory_order_release);

#if defined(CRPC_HAVE_COROCC)
        if (rt != nullptr && opt.start_handler_pool) {
            coro_attr_t attr = coro_attr_default();
            attr.stack_size = opt.handler_stack_bytes;
            attr.name = "crpc-handler";
            for (std::size_t i = 0; i < opt.handler_workers; ++i) {
                if (coro_rt_spawn(rt, &attr, &impl::handler_entry, this) != CORO_OK) {
                    break;
                }
            }
        }
#endif

        while (!stop_requested.load(std::memory_order_acquire)) {
            drain_inbox();
            run_timers();
#if defined(CRPC_HAVE_COROCC)
            if (rt != nullptr && pump_mode) {
                (void)coro_rt_pump(rt, opt.pump_budget);
            }
#endif
            // Compute the wait deadline after pumping: a coroutine may have
            // armed a timer during this turn.
            const std::uint64_t next = next_deadline();
            const std::uint64_t now = now_ns();
            std::chrono::nanoseconds wait = opt.idle_wait;
            if (next <= now) {
                wait = std::chrono::nanoseconds{0};
            } else if (next != runtime::no_deadline) {
                wait = std::chrono::nanoseconds(static_cast<std::int64_t>(next - now));
                if (wait > opt.idle_wait) {
                    wait = opt.idle_wait;
                }
            }
            std::unique_lock<std::mutex> lk(wait_mutex);
            wait_cv.wait_for(lk, wait, [this] {
                return stop_requested.load(std::memory_order_acquire) ||
                       wake_flag.exchange(false, std::memory_order_acq_rel);
            });
        }

        quiesce();
        running.store(false, std::memory_order_release);
        destroy_native();
    }
};

// --- runtime -----------------------------------------------------------------

runtime::runtime(runtime_options options)
    : impl_(std::make_unique<impl>(this, std::move(options))) {}

runtime::~runtime() {
    stop();
    join();
    // run() destroys the native runtime; a runtime that was never run is
    // cleaned up by impl's destructor, which also unregisters the driver.
}

void runtime::start_thread() {
    if (impl_->worker.joinable()) {
        return;
    }
    impl_->worker = std::thread([this] { run(); });
}

void runtime::run() {
    impl_->run_loop();
}

void runtime::stop() noexcept {
    if (impl_->stop_requested.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    impl_->wake();
}

void runtime::join() {
    if (impl_->worker.joinable() &&
        impl_->worker.get_id() != std::this_thread::get_id()) {
        impl_->worker.join();
    }
}

bool runtime::stopped() const noexcept {
    return impl_->stop_requested.load(std::memory_order_acquire);
}

bool runtime::is_owner_thread() const noexcept {
    return impl_->owner == std::this_thread::get_id();
}

bool runtime::running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

std::uint64_t runtime::now_ns() const noexcept {
    return impl_->now_ns();
}

bool runtime::post(task_fn fn, void* ctx) noexcept {
    if (fn == nullptr) {
        return false;
    }
    inbox_entry entry;
    entry.k = inbox_entry::kind::task;
    entry.task = fn;
    entry.ctx = ctx;
    return impl_->push_inbox(entry);
}

bool runtime::submit(task_fn fn, void* ctx) noexcept {
#if defined(CRPC_HAVE_COROCC)
    if (fn == nullptr) {
        return false;
    }
    task_slot slot;
    slot.fn = fn;
    slot.ctx = ctx;
    return impl_->push_ready(slot);
#else
    // Thread-per-shard fallback: no handler coroutines exist, so run the task
    // on the shard thread through the inbox.
    return post(fn, ctx);
#endif
}

bool runtime::spawn(spawn_fn fn, void* ctx) noexcept {
    if (fn == nullptr) {
        return false;
    }
    inbox_entry entry;
    entry.k = inbox_entry::kind::spawn;
    entry.spawn = fn;
    entry.ctx = ctx;
    return impl_->push_inbox(entry);
}

bool runtime::arm_timer(void* key, std::uint64_t deadline_ns, timer_fn fn,
                        void* arg) noexcept {
    if (key == nullptr || fn == nullptr) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(impl_->timer_mutex);
        auto existing = impl_->timer_index.find(key);
        if (existing != impl_->timer_index.end()) {
            impl_->timers.erase(existing->second);
            impl_->timer_index.erase(existing);
        }
        const std::uint64_t seq = ++impl_->timer_seq;
        const auto map_key = std::make_pair(deadline_ns, seq);
        timer_entry entry;
        entry.key = key;
        entry.fn = fn;
        entry.arg = arg;
        impl_->timers.emplace(map_key, entry);
        impl_->timer_index.emplace(key, map_key);
    }
    impl_->wake();
    return true;
}

bool runtime::cancel_timer(void* key) noexcept {
    if (key == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lk(impl_->timer_mutex);
    auto it = impl_->timer_index.find(key);
    if (it == impl_->timer_index.end()) {
        return false;
    }
    impl_->timers.erase(it->second);
    impl_->timer_index.erase(it);
    return true;
}

std::uint64_t runtime::next_deadline_ns() const noexcept {
    std::lock_guard<std::mutex> lk(impl_->timer_mutex);
    if (impl_->timers.empty()) {
        return no_deadline;
    }
    return impl_->timers.begin()->first.first;
}

void runtime::set_tick(tick_fn fn, void* ctx) noexcept {
    impl_->tick_ctx.store(ctx, std::memory_order_release);
    impl_->tick.store(fn, std::memory_order_release);
    impl_->wake();
}

void runtime::wake() noexcept {
    impl_->wake();
}

status runtime::park() noexcept {
#if defined(CRPC_HAVE_COROCC)
    coro_rt_t* rt = impl_->rt;
    if (rt == nullptr) {
        return status{rpc_code::failed_precondition};
    }
    const coro_status result = coro_rt_sem_wait(rt, &impl_->wake_sem);
    switch (result) {
    case CORO_OK:
        return status{};
    case CORO_ECANCELED:
        return status{rpc_code::canceled};
    case CORO_ESTALE:
        // Not running inside a coroutine of this runtime.
        return status{rpc_code::failed_precondition};
    default:
        return status{rpc_code::internal};
    }
#else
    return status{rpc_code::failed_precondition};
#endif
}

status runtime::sleep_until(std::uint64_t deadline_ns) noexcept {
#if defined(CRPC_HAVE_COROCC)
    coro_rt_t* rt = impl_->rt;
    if (rt == nullptr) {
        return status{rpc_code::failed_precondition};
    }
    const coro_status result = coro_rt_sleep(rt, deadline_ns);
    switch (result) {
    case CORO_OK:
        return status{};
    case CORO_ECANCELED:
        return status{rpc_code::canceled};
    case CORO_ESTALE:
        return status{rpc_code::failed_precondition};
    default:
        return status{rpc_code::internal};
    }
#else
    (void)deadline_ns;
    return status{rpc_code::failed_precondition};
#endif
}

void runtime::notify() noexcept {
#if defined(CRPC_HAVE_COROCC)
    {
        std::lock_guard<std::mutex> lk(impl_->native_mutex);
        if (impl_->native_alive && impl_->rt != nullptr) {
            (void)coro_rt_sem_post(impl_->rt, &impl_->wake_sem);
        }
    }
#endif
    wake();
}

std::size_t runtime::pending_coroutines() const noexcept {
#if defined(CRPC_HAVE_COROCC)
    coro_rt_t* rt = impl_->rt;
    return rt != nullptr ? coro_rt_pending(rt) : 0;
#else
    return 0;
#endif
}

std::size_t runtime::inbox_size() const noexcept {
    std::lock_guard<std::mutex> lk(impl_->inbox_mutex);
    return impl_->inbox.size();
}

std::size_t runtime::ready_size() const noexcept {
    std::lock_guard<std::mutex> lk(impl_->ready_mutex);
    return impl_->ready.size();
}

struct coro_rt* runtime::native() const noexcept {
#if defined(CRPC_HAVE_COROCC)
    return reinterpret_cast<struct coro_rt*>(impl_->rt);
#else
    return nullptr;
#endif
}

bool runtime::loop_mode() const noexcept {
#if defined(CRPC_HAVE_COROCC)
    return impl_->pump_mode && impl_->rt != nullptr;
#else
    return false;
#endif
}

} // namespace crpc
