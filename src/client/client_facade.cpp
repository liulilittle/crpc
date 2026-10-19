// CRPC -- client facade implementation.
#include <crpc/client_facade.hpp>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <crpc/endian.hpp>

namespace crpc {
namespace {

[[nodiscard]] std::chrono::steady_clock::time_point steady_now() noexcept {
    return std::chrono::steady_clock::now();
}

[[nodiscard]] bool transport_failure(rpc_code code) noexcept {
    return code == rpc_code::connection_reset || code == rpc_code::broken_pipe ||
           code == rpc_code::eof || code == rpc_code::connection_refused;
}

// Only dependency/transport failures count against an endpoint breaker.
// Application results (not_found, no_such_method, invalid_argument,
// decode_error, a server-reported deadline_exceeded, cancellation) say nothing
// about the endpoint's health and must not open it, while internal errors and
// unacknowledged calls are dependency failures. A locally observed timeout is
// recorded by its call site as a failure before reaching this classifier.
[[nodiscard]] bool counts_as_breaker_failure(rpc_code code) noexcept {
    switch (code) {
    case rpc_code::connection_refused:
    case rpc_code::connection_reset:
    case rpc_code::broken_pipe:
    case rpc_code::eof:
    case rpc_code::unavailable:
    case rpc_code::overload:
    case rpc_code::resource_exhausted:
    case rpc_code::internal:
    case rpc_code::not_acknowledged:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] std::uint64_t mix64(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

} // namespace

namespace {

// The timer gate whose callback is running on this thread, if any. It is the
// callback context, not a client pointer: a callback may destroy the client.
thread_local const void* timer_thread_local = nullptr;

} // namespace

struct client::impl {
    struct endpoint_pool {
        endpoint_pool(endpoint ep, backend_factory* backend,
                      const resilience::breaker_config& config)
            : ep(std::move(ep)), backend(backend), breaker(config) {}

        endpoint ep;
        backend_factory* backend = nullptr;
        std::mutex mutex;
        std::vector<std::shared_ptr<client_detail::connection_slot>> slots;
        resilience::circuit_breaker breaker;
        bool had_connection = false;
        std::size_t failed_connections = 0;
    };

    struct instance_view final : resilience::instance_set {
        const std::vector<resilience::instance>* instances = nullptr;
        std::size_t size() const noexcept override {
            return instances != nullptr ? instances->size() : 0;
        }
        resilience::instance at(std::size_t index) const noexcept override {
            return instances != nullptr ? (*instances)[index] : resilience::instance{};
        }
        u64 version() const noexcept override { return 1; }
    };

    std::vector<std::unique_ptr<endpoint_pool>> pools;
    std::vector<resilience::instance> instances;
    instance_view view;
    std::unique_ptr<resilience::load_balancer> balancer;

    // Live call cores that may still have a shard timer armed. Guarded by
    // `calls_mutex`. The value is a strong reference to the type-erased state:
    // close() snapshots these and finalizes them after releasing the mutex, so
    // without ownership a handle destroyed on another thread could free the
    // core while close() is still writing to it (a real race, found by TSan).
    std::mutex calls_mutex;
    std::unordered_map<client_detail::call_core*,
                       std::shared_ptr<client_detail::call_state_base>>
        active_calls;

    // Shared with every call this client issues; expires when the client is
    // destroyed so a handle that outlives it never calls into freed memory.
    std::shared_ptr<client_detail::client_liveness> liveness =
        std::make_shared<client_detail::client_liveness>();
};

// Timer bookkeeping that outlives the client on a callback's stack. The
// runtime keeps the gate alive until the callback returns, so a callback that
// destroys its own client can still decrement the count and notify the
// destructor without touching the freed client.
struct client::timer_gate {
    std::shared_ptr<impl> owner;
    std::chrono::nanoseconds idle_eviction{0};
    bool stopping = false;
    int in_flight = 0;
    std::mutex m;
    std::condition_variable cv;
};

// --- call_core ---------------------------------------------------------------

namespace client_detail {

void call_core::signal() noexcept {
    done.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(mutex);
    }
    cv.notify_all();
    client* const who = owner.load(std::memory_order_acquire);
    if (who != nullptr) {
        who->notify_waiter();
    }
}

} // namespace client_detail

// --- construction ------------------------------------------------------------

client::client(options opt, std::vector<endpoint_config> endpoints)
    : timer_gate_(std::make_shared<timer_gate>()),
      options_(std::move(opt)),
      endpoints_(std::move(endpoints)),
      impl_(std::make_shared<impl>()),
      budget_(resilience::budget_config_from(options_.retry)) {
    timer_gate_->owner = impl_;
    timer_gate_->idle_eviction = options_.idle_eviction;
    const std::uint64_t seed =
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(this)) ^
        static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    std::uint64_t state = seed;
    for (std::size_t i = 0; i < client_id_.size(); i += 8) {
        state = mix64(state);
        endian::store_le64(client_id_.data() + i, state);
    }

    for (std::size_t i = 0; i < endpoints_.size(); ++i) {
        impl_->pools.push_back(std::make_unique<impl::endpoint_pool>(
            endpoints_[i].ep, endpoints_[i].backend, options_.breaker));
        resilience::instance entry;
        entry.id = i;
        entry.weight = 1;
        entry.healthy = true;
        entry.inflight = 0;
        impl_->instances.push_back(entry);
    }
    impl_->view.instances = &impl_->instances;
    impl_->balancer = std::make_unique<resilience::load_balancer>(options_.lb);
}

client::~client() {
    // A deadline timer may be running on the shard thread right now, or may
    // have been popped from the wheel and be about to run. Block new callbacks
    // (and let an already-popped callback observe `stopping`), then wait for a
    // running one. The timer entry holds the gate alive, and the callback only
    // touches the gate, so the client can be torn down once in_flight is zero.
    // A callback that destroys its own client (same thread) is exempt.
    std::shared_ptr<timer_gate> gate;
    {
        // `timer_mutex_` is a leaf: release it before taking the gate lock so a
        // future path that takes them in the other order cannot deadlock.
        std::lock_guard<std::mutex> lk(timer_mutex_);
        gate = timer_gate_;
    }
    if (gate) {
        std::unique_lock<std::mutex> lk(gate->m);
        gate->stopping = true;
        if (timer_thread_local != gate.get()) {
            gate->cv.wait(lk, [&gate] { return gate->in_flight == 0; });
        }
    }
    close();
}

void client::close() noexcept {
    // The timer callback may destroy the client and return into
    // deadline_tick(), which still uses the implementation. Keep it alive for
    // the duration of this call.
    const std::shared_ptr<impl> impl = impl_;
    // Setting `closed_` and snapshotting the live states happen in one critical
    // section. `register_core_if_open()` takes the same mutex, so a concurrent
    // attempt is either inserted before this snapshot (and is finalized below)
    // or observes `closed_` and never arms a timer (R1). The snapshot owns each
    // state, so a handle destroyed on another thread cannot free a core that
    // finalize_active() is about to write.
    std::vector<std::shared_ptr<client_detail::call_state_base>> active;
    {
        std::lock_guard<std::mutex> lk(impl->calls_mutex);
        if (closed_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        active.reserve(impl->active_calls.size());
        for (auto& entry : impl->active_calls) {
            active.push_back(std::move(entry.second));
        }
        impl->active_calls.clear();
    }
    for (std::unique_ptr<impl::endpoint_pool>& pool : impl->pools) {
        std::vector<std::shared_ptr<client_detail::connection_slot>> snapshot;
        {
            std::lock_guard<std::mutex> lk(pool->mutex);
            snapshot.swap(pool->slots);
        }
        for (const std::shared_ptr<client_detail::connection_slot>& slot : snapshot) {
            std::lock_guard<std::recursive_mutex> lk(slot->mutex);
            slot->healthy.store(false, std::memory_order_release);
            if (slot->conn) {
                slot->conn->close();
            }
        }
    }
    // Cancel the deadline timer and release the slot for every attempt that is
    // still active; detach its owner so a handle that outlives the client can
    // never call back into it.
    finalize_active(active);
}

std::shared_ptr<client_detail::client_liveness>
client::liveness_token() const noexcept {
    return impl_->liveness;
}

bool client::register_core_if_open(
    client_detail::call_core& core,
    const std::shared_ptr<client_detail::call_state_base>& keep) noexcept {
    // The closed decision and the insertion are one atomic step under
    // `calls_mutex` (R1). close() sets `closed_` and snapshots `active_calls`
    // under the same mutex, so an attempt is either visible to finalize_active
    // (and has its timer cancelled) or rejected here before it can arm one.
    std::lock_guard<std::mutex> lk(impl_->calls_mutex);
    if (closed_.load(std::memory_order_acquire)) {
        return false;
    }
    impl_->active_calls.emplace(&core, keep);
    return true;
}

void client::unregister_core(client_detail::call_core& core) noexcept {
    std::lock_guard<std::mutex> lk(impl_->calls_mutex);
    impl_->active_calls.erase(&core);
}

void client::finalize_active(
    std::vector<std::shared_ptr<client_detail::call_state_base>>& active) noexcept {
    for (const std::shared_ptr<client_detail::call_state_base>& keep : active) {
        client_detail::call_core* core = &keep->core_ref();
        // A concurrent finish_attempt/abandon_attempt must win exactly once;
        // only the winner releases the timer and slot.
        if (core->finalized.exchange(true, std::memory_order_acq_rel)) {
            continue;
        }
        if (options_.shard != nullptr) {
            (void)options_.shard->cancel_timer(core);
        }
        core->permit = resilience::breaker_permit{};
        if (core->slot) {
            // Release the in-flight accounting exactly once, but keep the
            // `shared_ptr` in `core->slot` (NEW-1). The owning handle still
            // holds `st->inner`, a low-level rpc::call with a raw call_slot*
            // into the connection's pool; dropping the last connection_slot
            // here would free that pool while the handle can still outlive the
            // client. `release()` resets `inner` before the core is destroyed,
            // and because `inner` is declared after `core` it also runs before
            // the retained `shared_ptr` is released.
            release_slot(core->slot, rpc_code::shutting_down);
        }
        // R2: wake a blocked get() promptly with `shutting_down` instead of
        // letting it sit until the plain-thread timeout or a cancelled timer.
        core->error = rpc_error(rpc_code::shutting_down, "client is closed");
        core->terminal.store(rpc_code::shutting_down, std::memory_order_release);
        core->done.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(core->mutex);
        }
        core->cv.notify_all();
        client* const who = core->owner.load(std::memory_order_acquire);
        if (who != nullptr) {
            who->notify_waiter();
        }
        core->owner.store(nullptr, std::memory_order_release);
    }
}

bool client::closed() const noexcept {
    return closed_.load(std::memory_order_acquire);
}

client::statistics client::stats() const noexcept {
    statistics out;
    for (const std::unique_ptr<impl::endpoint_pool>& pool : impl_->pools) {
        std::lock_guard<std::mutex> lk(pool->mutex);
        out.active_connections += pool->slots.size();
        out.failed_connections += pool->failed_connections;
        for (const std::shared_ptr<client_detail::connection_slot>& slot : pool->slots) {
            out.in_flight_calls += slot->in_flight.load(std::memory_order_acquire);
        }
    }
    out.calls_started = calls_started_.load(std::memory_order_relaxed);
    out.retries = retries_.load(std::memory_order_relaxed);
    out.breaker_rejections = breaker_rejections_.load(std::memory_order_relaxed);
    return out;
}

// --- admission and pools -----------------------------------------------------

bool client::begin_attempt(
    client_detail::call_core& core,
    const std::shared_ptr<client_detail::call_state_base>& keep,
    rpc_error& failure) {
    const auto now = steady_now();
    if (core.has_deadline && now >= core.deadline) {
        failure = rpc_error(rpc_code::deadline_exceeded, "deadline already passed");
        return false;
    }
    if (impl_->pools.empty()) {
        failure = rpc_error(rpc_code::unavailable, "no endpoint configured");
        return false;
    }
    const result<resilience::instance> selected = impl_->balancer->select(impl_->view);
    if (!selected.ok()) {
        failure = rpc_error(rpc_code::unavailable, "no usable endpoint");
        return false;
    }
    const std::size_t index = static_cast<std::size_t>(selected.value().id);
    if (index >= impl_->pools.size()) {
        failure = rpc_error(rpc_code::unavailable, "load balancer returned an unknown endpoint");
        return false;
    }
    impl::endpoint_pool& pool = *impl_->pools[index];
    resilience::breaker_permit permit = pool.breaker.allow();
    if (!permit.allowed()) {
        breaker_rejections_.fetch_add(1, std::memory_order_relaxed);
        failure = rpc_error(rpc_code::unavailable, "endpoint circuit breaker is open");
        failure.retry_after = permit.retry_after();
        return false;
    }
    core.permit = std::move(permit);
    core.pool = &pool;
    // The closed decision and the tracking insertion are one atomic step
    // (R1). If close()/``~client`` already set `closed_` (or will do so before
    // this returns), either the insert wins and the snapshot in close() sees
    // the core, or this returns false and no timer is ever armed.
    if (!register_core_if_open(core, keep)) {
        core.permit = resilience::breaker_permit{};
        core.pool = nullptr;
        failure = rpc_error(rpc_code::shutting_down, "client is closed");
        return false;
    }
    core.attempts += 1;
    calls_started_.fetch_add(1, std::memory_order_relaxed);
    if (options_.shard != nullptr && core.has_deadline) {
        const std::uint64_t deadline_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                core.deadline.time_since_epoch())
                .count());
        std::shared_ptr<timer_gate> gate;
        {
            std::lock_guard<std::mutex> lk(timer_mutex_);
            gate = timer_gate_;
        }
        if (gate) {
            // The timer context is the gate, not `this`, and the wheel keeps
            // it alive across the pop/call window.
            (void)options_.shard->arm_timer(&core, deadline_ns,
                                            &client::deadline_thunk, gate.get(),
                                            gate);
        }
        // Ordering: close() sets `finalized` (via finalize_active) and cancels
        // the timer under `calls_mutex`; register_core_if_open() inserts under
        // the same mutex. So the attempt is in exactly one of two states here:
        // either close() snapshotted the core and its finalize_active either
        // cancelled this timer or will run next, or it set `finalized` before
        // this arm, in which case the exchange below cancels the timer. There
        // is no interleaving that leaves a timer pointing at a freed client.
        if (core.finalized.load(std::memory_order_acquire)) {
            (void)options_.shard->cancel_timer(&core);
        }
    }
    return true;
}

std::shared_ptr<client_detail::connection_slot> client::acquire_slot(
    client_detail::call_core& core, rpc_error& failure) {
    auto* pool = static_cast<impl::endpoint_pool*>(core.pool);
    if (pool == nullptr) {
        failure = rpc_error(rpc_code::internal, "attempt has no endpoint pool");
        return nullptr;
    }
    if (pool->backend == nullptr) {
        failure = rpc_error(rpc_code::invalid_argument, "endpoint has no backend");
        return nullptr;
    }
    std::lock_guard<std::mutex> lk(pool->mutex);

    std::shared_ptr<client_detail::connection_slot> best;
    std::vector<std::shared_ptr<client_detail::connection_slot>> dead;
    std::uint32_t best_in_flight = 0;
    bool saturated = false;
    for (const std::shared_ptr<client_detail::connection_slot>& candidate : pool->slots) {
        if (!candidate->healthy.load(std::memory_order_acquire)) {
            if (candidate->in_flight.load(std::memory_order_acquire) == 0) {
                dead.push_back(candidate);
            }
            continue;
        }
        const std::uint32_t in_flight =
            candidate->in_flight.load(std::memory_order_acquire);
        if (options_.max_inflight_per_connection != 0 &&
            in_flight >= options_.max_inflight_per_connection) {
            saturated = true;
            continue;
        }
        bool open = false;
        {
            std::lock_guard<std::recursive_mutex> slot_lock(candidate->mutex);
            open = candidate->conn != nullptr && candidate->conn->is_open();
        }
        if (!open) {
            if (in_flight == 0) {
                candidate->healthy.store(false, std::memory_order_release);
                dead.push_back(candidate);
            }
            continue;
        }
        if (best == nullptr || in_flight < best_in_flight) {
            best = candidate;
            best_in_flight = in_flight;
        }
    }
    if (best != nullptr) {
        best->in_flight.fetch_add(1, std::memory_order_acq_rel);
        return best;
    }
    for (const std::shared_ptr<client_detail::connection_slot>& entry : dead) {
        const auto it = std::find(pool->slots.begin(), pool->slots.end(), entry);
        if (it != pool->slots.end()) {
            pool->slots.erase(it);
            pool->failed_connections += 1;
        }
    }
    if (saturated) {
        failure = rpc_error(rpc_code::resource_exhausted,
                            "connection pool is saturated");
        return nullptr;
    }
    if (pool->slots.size() >= options_.pool_size) {
        failure = rpc_error(rpc_code::resource_exhausted,
                            "connection pool is exhausted");
        return nullptr;
    }

    std::unique_ptr<connection> raw = pool->backend->connect(pool->ep, connect_options{});
    if (!raw) {
        pool->failed_connections += 1;
        if (pool->had_connection) {
            failure = rpc_error(rpc_code::shutting_down,
                                "endpoint is no longer accepting connections");
        } else {
            failure = rpc_error(rpc_code::connection_refused,
                                "could not connect to endpoint");
        }
        return nullptr;
    }
    connection* raw_ptr = raw.get();
    auto slot = std::make_shared<client_detail::connection_slot>();
    slot->pool = pool;
    slot->idle_since = steady_now();

    rpc::client_options co;
    co.slot_capacity = static_cast<u32>(options_.slot_capacity);
    co.default_timeout = std::chrono::nanoseconds{0};
    co.client_id = client_id_;
    co.client_epoch = 1;
    slot->conn = std::make_unique<rpc::client>(std::move(raw), co);
    slot->guarded = std::make_unique<client_detail::connection_slot::guarded_source>(slot.get());
    slot->guarded->detach(slot->conn.get());
    raw_ptr->bind_source(slot->guarded.get());
    slot->in_flight.store(1, std::memory_order_release);
    pool->slots.push_back(slot);
    pool->had_connection = true;
    return slot;
}

void client::release_slot(
    const std::shared_ptr<client_detail::connection_slot>& slot,
    rpc_code code) noexcept {
    if (slot == nullptr) {
        return;
    }
    slot->in_flight.fetch_sub(1, std::memory_order_acq_rel);
    slot->idle_since = steady_now();
    if (!transport_failure(code)) {
        return;
    }
    slot->healthy.store(false, std::memory_order_release);
    auto* pool = static_cast<impl::endpoint_pool*>(slot->pool);
    if (pool == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lk(pool->mutex);
    const auto it = std::find(pool->slots.begin(), pool->slots.end(), slot);
    if (it != pool->slots.end()) {
        pool->slots.erase(it);
        pool->failed_connections += 1;
    }
}

void client::finish_attempt(client_detail::call_core& core, rpc_code code,
                            client_detail::breaker_record record) noexcept {
    if (core.finalized.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (options_.shard != nullptr) {
        (void)options_.shard->cancel_timer(&core);
    }
    if (core.permit.allowed()) {
        switch (record) {
        case client_detail::breaker_record::ignore:
            core.permit.ignore();
            break;
        case client_detail::breaker_record::failure:
            core.permit.failure();
            break;
        case client_detail::breaker_record::by_code:
            if (code == rpc_code::ok) {
                core.permit.success();
            } else if (counts_as_breaker_failure(code)) {
                core.permit.failure();
            } else {
                core.permit.ignore();
            }
            break;
        }
    }
    core.permit = resilience::breaker_permit{};
    if (core.slot) {
        release_slot(core.slot, code);
        core.slot.reset();
    }
    unregister_core(core);
}

void client::abandon_attempt(client_detail::call_core& core) noexcept {
    if (core.finalized.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (options_.shard != nullptr) {
        (void)options_.shard->cancel_timer(&core);
    }
    // An abandoned attempt records no breaker outcome; just drop the permit.
    core.permit = resilience::breaker_permit{};
    if (core.slot) {
        // Release the in-flight accounting but keep the connection slot alive
        // (NEW-1, cancel path). `call::cancel()` keeps the low-level call so a
        // later get()/try_get() can still observe `canceled`; that call holds a
        // raw pointer into the connection slot pool. Dropping the last
        // connection_slot here would free the pool before the handle resets the
        // inner call. The shared_ptr is released with the core, after `inner`
        // has been destroyed (declaration order in call_state).
        release_slot(core.slot, rpc_code::canceled);
    }
    unregister_core(core);
}

bool client::should_retry(client_detail::call_core& core, rpc_code code,
                          std::chrono::nanoseconds& delay) noexcept {
    if (code == rpc_code::ok || !core.idempotent || !is_retryable(code)) {
        return false;
    }
    if (core.has_deadline && steady_now() >= core.deadline) {
        return false;
    }
    if (!core.retry) {
        return false;
    }
    std::chrono::nanoseconds computed{0};
    if (!core.retry->next(code, core.idempotent, computed)) {
        return false;
    }
    if (core.has_deadline && steady_now() + computed >= core.deadline) {
        return false;
    }
    delay = computed;
    retries_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

status client::wait_ready(client_detail::call_core& core) const {
    if (core.done.load(std::memory_order_acquire)) {
        return status{};
    }
    if (options_.shard != nullptr && options_.shard->loop_mode() &&
        options_.shard->is_owner_thread()) {
        for (;;) {
            if (core.done.load(std::memory_order_acquire)) {
                return status{};
            }
            const status parked = options_.shard->park();
            if (parked.ok()) {
                continue;
            }
            if (parked.code() == rpc_code::canceled) {
                if (core.done.load(std::memory_order_acquire)) {
                    return status{};
                }
                return status{rpc_code::canceled};
            }
            return parked;
        }
    }
    std::unique_lock<std::mutex> lk(core.mutex);
    // Without a shard there is no timer wheel to drive the low-level poll, so
    // the wait itself must honour the call deadline (A-3). A call with no
    // deadline keeps the documented plain-thread bound.
    std::chrono::nanoseconds bound = options_.plain_thread_timeout;
    if (core.has_deadline) {
        const auto now = steady_now();
        if (now >= core.deadline) {
            return status{rpc_code::deadline_exceeded};
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::nanoseconds>(core.deadline - now);
        if (remaining < bound) {
            bound = remaining;
        }
    }
    const bool ready = core.cv.wait_for(lk, bound, [&core] {
        return core.done.load(std::memory_order_acquire);
    });
    if (!ready) {
        return status{rpc_code::deadline_exceeded};
    }
    return status{};
}

void client::sleep_for(client_detail::call_core& core,
                       std::chrono::nanoseconds delay) noexcept {
    (void)core;
    if (delay.count() <= 0) {
        return;
    }
    if (options_.shard != nullptr && options_.shard->loop_mode() &&
        options_.shard->is_owner_thread() && !options_.shard->stopped()) {
        const std::uint64_t deadline =
            options_.shard->now_ns() + static_cast<std::uint64_t>(delay.count());
        const status slept = options_.shard->sleep_until(deadline);
        if (slept.ok() || slept.code() == rpc_code::canceled) {
            return;
        }
        // Not inside a coroutine: do not block the shard.
        if (slept.code() == rpc_code::failed_precondition) {
            return;
        }
    }
    std::this_thread::sleep_for(delay);
}

void client::notify_waiter() noexcept {
    if (options_.shard != nullptr) {
        options_.shard->notify();
    }
}

rpc::call_id client::make_call_id() noexcept {
    rpc::call_id id;
    id.client_id = client_id_;
    id.client_epoch = 1;
    id.seq = seq_.fetch_add(1, std::memory_order_relaxed) + 1;
    return id;
}

void client::normalize_context(client_detail::call_core& core,
                               const rpc::call_context& ctx) {
    core.ctx = ctx;
    core.idempotent = ctx.idempotent || options_.default_idempotent ||
                      ctx.has_idempotency_key;
    core.ctx.idempotent = core.idempotent;

    core.has_deadline = ctx.has_deadline;
    if (core.has_deadline) {
        core.deadline = ctx.deadline;
    } else if (options_.default_deadline.count() > 0) {
        core.has_deadline = true;
        core.deadline = steady_now() + options_.default_deadline;
    }
    if (core.has_deadline) {
        core.ctx.has_deadline = true;
        core.ctx.deadline = core.deadline;
    }
    if (core.idempotent && !core.ctx.has_idempotency_key) {
        core.ctx.has_idempotency_key = true;
        core.ctx.idempotency = make_call_id();
    }
}

// --- deadline tick -----------------------------------------------------------

void client::deadline_thunk(void* ctx) noexcept {
    // `ctx` is the timer gate. The runtime holds a keep-alive reference to it
    // across the wheel pop and this call, so it stays valid even if the client
    // is destroyed concurrently. Only the gate is touched here.
    auto* gate = static_cast<timer_gate*>(ctx);
    std::shared_ptr<impl> impl;
    std::chrono::nanoseconds idle_eviction{0};
    {
        std::lock_guard<std::mutex> lk(gate->m);
        if (gate->stopping) {
            return;
        }
        ++gate->in_flight;
        impl = gate->owner;
        idle_eviction = gate->idle_eviction;
    }
    // Marks this thread as being inside this gate's callback, so a destructor
    // that runs from here does not wait for itself.
    const void* const previous = timer_thread_local;
    timer_thread_local = gate;
    deadline_tick(impl, idle_eviction);
    timer_thread_local = previous;
    {
        std::lock_guard<std::mutex> lk(gate->m);
        if (--gate->in_flight == 0) {
            gate->cv.notify_all();
        }
    }
}

void client::deadline_tick(const std::shared_ptr<impl>& impl,
                           std::chrono::nanoseconds idle_eviction) noexcept {
    // A user callback below may destroy the client. `impl` keeps the
    // implementation alive and every scalar is already copied, so nothing here
    // dereferences the client.
    for (const std::unique_ptr<impl::endpoint_pool>& pool : impl->pools) {
        std::vector<std::shared_ptr<client_detail::connection_slot>> snapshot;
        {
            std::lock_guard<std::mutex> lk(pool->mutex);
            snapshot = pool->slots;
        }
        for (const std::shared_ptr<client_detail::connection_slot>& slot : snapshot) {
            std::lock_guard<std::recursive_mutex> lk(slot->mutex);
            if (!slot->conn) {
                continue;
            }
            slot->conn->poll();
        }
    }
    if (idle_eviction.count() > 0) {
        evict_idle(impl, idle_eviction);
    }
}

void client::evict_idle(const std::shared_ptr<impl>& impl,
                        std::chrono::nanoseconds idle_eviction) noexcept {
    const auto now = steady_now();
    for (const std::unique_ptr<impl::endpoint_pool>& pool : impl->pools) {
        std::vector<std::shared_ptr<client_detail::connection_slot>> snapshot;
        std::size_t count = 0;
        {
            std::lock_guard<std::mutex> lk(pool->mutex);
            snapshot = pool->slots;
            count = pool->slots.size();
        }
        for (const std::shared_ptr<client_detail::connection_slot>& slot : snapshot) {
            if (count <= 1) {
                break;
            }
            if (slot->in_flight.load(std::memory_order_acquire) != 0) {
                continue;
            }
            if (now - slot->idle_since < idle_eviction) {
                continue;
            }
            bool closed_now = false;
            {
                std::lock_guard<std::recursive_mutex> lk(slot->mutex);
                if (slot->conn && slot->conn->in_flight() == 0) {
                    slot->conn->close();
                    slot->healthy.store(false, std::memory_order_release);
                    closed_now = true;
                }
            }
            if (!closed_now) {
                continue;
            }
            std::lock_guard<std::mutex> lk(pool->mutex);
            const auto it = std::find(pool->slots.begin(), pool->slots.end(), slot);
            if (it != pool->slots.end()) {
                pool->slots.erase(it);
                --count;
            }
        }
    }
}

// --- builder -----------------------------------------------------------------

client::builder& client::builder::add_endpoint(endpoint ep, backend_factory& backend) {
    endpoint_config config;
    config.ep = std::move(ep);
    config.backend = &backend;
    endpoints_.push_back(std::move(config));
    return *this;
}

client::builder& client::builder::pool_size(std::size_t value) {
    options_.pool_size = value == 0 ? 1 : value;
    return *this;
}

client::builder& client::builder::slot_capacity(std::size_t value) {
    options_.slot_capacity = value == 0 ? 1 : value;
    return *this;
}

client::builder& client::builder::max_inflight_per_connection(std::size_t value) {
    options_.max_inflight_per_connection = value;
    return *this;
}

client::builder& client::builder::load_balancer(resilience::lb_strategy strategy) {
    options_.lb = strategy;
    return *this;
}

client::builder& client::builder::breaker(resilience::breaker_config config) {
    options_.breaker = config;
    return *this;
}

client::builder& client::builder::retry(resilience::retry_policy policy) {
    options_.retry = policy;
    return *this;
}

client::builder& client::builder::default_deadline(std::chrono::nanoseconds value) {
    options_.default_deadline = value;
    return *this;
}

client::builder& client::builder::default_idempotent(bool value) {
    options_.default_idempotent = value;
    return *this;
}

client::builder& client::builder::plain_thread_timeout(std::chrono::nanoseconds value) {
    options_.plain_thread_timeout = value;
    return *this;
}

client::builder& client::builder::idle_eviction(std::chrono::milliseconds value) {
    options_.idle_eviction = value;
    return *this;
}

client::builder& client::builder::on_shard(runtime& shard) {
    options_.shard = &shard;
    return *this;
}

std::unique_ptr<client> client::builder::build() {
    if (endpoints_.empty()) {
        return nullptr;
    }
    return std::unique_ptr<client>(new client(options_, endpoints_));
}

} // namespace crpc