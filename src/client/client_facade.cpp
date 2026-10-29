// CRPC -- client facade implementation.
#include <crpc/client_facade.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <crpc/endian.hpp>
#include <crpc/resilience/admission.hpp>

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

[[nodiscard]] bool transport_result_code(rpc_code code) noexcept {
    switch (code) {
    case rpc_code::connection_refused:
    case rpc_code::connection_reset:
    case rpc_code::broken_pipe:
    case rpc_code::eof:
    case rpc_code::protocol_error:
    case rpc_code::shutting_down:
        return true;
    default:
        return false;
    }
}

constexpr const char* k_local_timeout_metadata = "crpc.local_timeout";

[[nodiscard]] std::uint64_t mix64(std::uint64_t value) noexcept {
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

} // namespace

namespace client_detail {

breaker_record breaker_record_for(failure_origin origin, rpc_code code) noexcept {
    switch (origin) {
    case failure_origin::local_lifecycle:
    case failure_origin::local_gate:
    case failure_origin::local_capacity:
    case failure_origin::endpoint_open:
    case failure_origin::cancel:
        return breaker_record::ignore;
    case failure_origin::timeout:
    case failure_origin::endpoint_pre_send:
    case failure_origin::endpoint_sent:
        return breaker_record::failure;
    case failure_origin::remote:
        if (code == rpc_code::ok) {
            return breaker_record::by_code;
        }
        return counts_as_breaker_failure(code) ? breaker_record::failure
                                               : breaker_record::ignore;
    }
    return breaker_record::ignore;
}

failure_origin result_origin(const rpc_error& error) noexcept {
    if (error.metadata.find(k_local_timeout_metadata) != nullptr) {
        return failure_origin::timeout;
    }
    if (error.code == rpc_code::canceled) {
        return failure_origin::cancel;
    }
    if (transport_result_code(error.code)) {
        return failure_origin::endpoint_sent;
    }
    return failure_origin::remote;
}

} // namespace client_detail

namespace {

// The timer gate whose callback is running on this thread, if any. It is the
// callback context, not a client pointer: a callback may destroy the client.
thread_local const void* timer_thread_local = nullptr;

// One sink per fallback attempt: it accumulates the seven counters on the
// stack and forwards to an optional caller sink, so the client counters are
// updated once per attempt instead of once per counter (no cache-line
// contention between concurrent get() callers, no partially applied batch).
struct fallback_tally {
    resilience::fallback_statistics values{};
    resilience::fallback_counter_sink* extra = nullptr;
};

void fallback_tally_add(void* self, resilience::fallback_counter counter) noexcept {
    auto* tally = static_cast<fallback_tally*>(self);
    switch (counter) {
    case resilience::fallback_counter::ineligible:
        ++tally->values.ineligible;
        break;
    case resilience::fallback_counter::skipped_deadline:
        ++tally->values.skipped_deadline;
        break;
    case resilience::fallback_counter::attempted:
        ++tally->values.attempted;
        break;
    case resilience::fallback_counter::failed:
        ++tally->values.failed;
        break;
    case resilience::fallback_counter::canceled:
        ++tally->values.canceled;
        break;
    case resilience::fallback_counter::overrun:
        ++tally->values.overrun;
        break;
    case resilience::fallback_counter::applied:
        ++tally->values.applied;
        break;
    }
    if (tally->extra != nullptr) {
        resilience::fallback_record(tally->extra, counter);
    }
}

} // namespace

// Derives the admission stage of a terminal failure from its origin and code.
//
// `call_core` has no `stage` field, and adding one would be a layout change.
// The mapping is therefore an inverse of the single `reject_code()` table, and
// it validates itself against that table: a candidate is only returned when
// `reject_code(candidate)` reproduces the observed code. A pair the table
// cannot reproduce returns `nullopt`, which makes the fallback ineligible
// (fail closed) rather than guessing.
//
// The remote side is the only place where the stage is inferred rather than
// recorded: the client sees just the code and the `crpc-degradable` marker, so
// the marker is the real gate (see `degradable()` below) and the inference only
// satisfies `fallback_admissible`'s signature.
namespace {

[[nodiscard]] std::optional<resilience::stage_kind> stage_for(
    client_detail::failure_origin origin, rpc_code code,
    const rpc_error& error) noexcept {
    using client_detail::failure_origin;
    using resilience::stage_kind;

    stage_kind candidate = stage_kind::dispatch;
    bool known = false;
    switch (origin) {
    case failure_origin::endpoint_open:
        // The only producer of this origin answers `unavailable`.
        candidate = stage_kind::breaker;
        known = code == rpc_code::unavailable;
        break;
    case failure_origin::local_gate:
        // Two producers with disjoint codes: the rate bucket answers
        // `resource_exhausted` / `deadline_exceeded`, the bulkhead `overload`.
        if (code == rpc_code::overload) {
            candidate = stage_kind::endpoint_bulkhead;
            known = true;
        } else {
            candidate = stage_kind::endpoint_rate;
            known = code == rpc_code::resource_exhausted ||
                    code == rpc_code::deadline_exceeded;
        }
        break;
    case failure_origin::local_capacity:
        candidate = stage_kind::pool_capacity;
        known = code == rpc_code::resource_exhausted;
        break;
    case failure_origin::local_lifecycle:
        // `dispatch` is the only pre-send lifecycle refusal with a stage; every
        // other code on this origin (shutting_down, the bitmap allocation
        // failure, an expired deadline) stays undecidable.
        candidate = stage_kind::dispatch;
        known = code == rpc_code::unavailable;
        break;
    case failure_origin::remote:
        // The marker is mandatory: a handler that returns the same code must
        // not be degradable. Only three codes can carry it.
        if (!resilience::degradable(error)) {
            return std::nullopt;
        }
        if (code == rpc_code::unavailable) {
            candidate = stage_kind::breaker;
        } else if (code == rpc_code::overload) {
            // P3 and P5 share this code and both are eligible; either stage is
            // a valid witness for `fallback_eligible`.
            candidate = stage_kind::service_bulkhead;
        } else if (code == rpc_code::resource_exhausted) {
            candidate = stage_kind::service_rate;
        } else {
            return std::nullopt;
        }
        known = true;
        break;
    case failure_origin::timeout:
    case failure_origin::cancel:
    case failure_origin::endpoint_pre_send:
    case failure_origin::endpoint_sent:
        // The stage table covers admission refusals only; a transport or
        // timeout failure has no stage and must never be degraded.
        return std::nullopt;
    }
    if (!known) {
        return std::nullopt;
    }
    // Self-check against the single authority: the candidate must reproduce the
    // observed code under at least one `deadline_met` value.
    if (resilience::reject_code(candidate, true) != code &&
        resilience::reject_code(candidate, false) != code) {
        return std::nullopt;
    }
    return candidate;
}

// True when the request may already have reached the peer, which is the
// `sent` gate of `fallback_admissible`.
[[nodiscard]] bool request_was_sent(client_detail::failure_origin origin) noexcept {
    return origin == client_detail::failure_origin::endpoint_sent ||
           origin == client_detail::failure_origin::remote;
}

[[nodiscard]] resilience::fallback_context build_fallback_context(
    const client_detail::call_core& core, const rpc_error& error) noexcept {
    resilience::fallback_context ctx;
    ctx.code = error.code;
    ctx.stage = stage_for(core.origin, error.code, error);
    ctx.retry_after = error.retry_after;
    ctx.one_way = core.ctx.mode != rpc::delivery_mode::reliable;
    ctx.sent = request_was_sent(core.origin);
    ctx.idempotent = core.idempotent;
    ctx.has_deadline = core.has_deadline;
    ctx.deadline = core.deadline;
    return ctx;
}

} // namespace

struct client::impl {
    // Fallback primitive counters. They live in the implementation, not in
    // `client`, so the public `client` layout does not change. `run_fallback`
    // flushes one attempt's stack tally here.
    std::atomic<std::size_t> fallback_ineligible{0};
    std::atomic<std::size_t> fallback_skipped_deadline{0};
    std::atomic<std::size_t> fallback_attempted{0};
    std::atomic<std::size_t> fallback_failed{0};
    std::atomic<std::size_t> fallback_canceled{0};
    std::atomic<std::size_t> fallback_overrun{0};
    std::atomic<std::size_t> fallback_applied{0};

    // One installed fallback. Immutable once published, and it owns exactly one
    // reference: `token` is what `retain` returned at install time and
    // `~fallback_slot` is the only place `release` runs. Copying the owning
    // shared_ptr does not call `retain` again.
    struct fallback_slot {
        resilience::fallback_fn function = nullptr;
        void* self = nullptr;                     // the caller's pointer
        std::chrono::nanoseconds min_remaining{0};
        void* token = nullptr;                    // what retain returned
        void (*release)(void*) noexcept = nullptr;
        // Owned self for the RAII installer (`set_fallback_owned`). Released with
        // the slot, so a non-null `self` does not need a matching retain/release
        // pair. Empty for the low-level five-argument contract.
        std::shared_ptr<void> owned_self;

        fallback_slot() = default;
        fallback_slot(const fallback_slot&) = delete;
        fallback_slot& operator=(const fallback_slot&) = delete;

        ~fallback_slot() noexcept {
            if (release != nullptr && token != nullptr) {
                // Outside every library lock: the last owner may drop this slot
                // on the reader thread, in set_fallback, or in ~client.
                release(token);
            }
        }
    };

    // Guards only the shared_ptr below. No consumer callback ever runs under it.
    std::mutex fallback_mutex;
    std::shared_ptr<fallback_slot> fallback;
    // Fast path for the unconfigured client: get() must not take a lock when no
    // fallback was ever installed, so an unwired client keeps the exact
    // pre-S4 call path.
    std::atomic<bool> fallback_configured{false};

    struct endpoint_pool {
        endpoint_pool(endpoint ep, backend_factory* backend,
                      const resilience::breaker_config& config,
                      const std::optional<resilience::rate_limit_config>& rate_config,
                      const std::optional<resilience::bulkhead_config>& bulkhead_config,
                      bool resilience_enabled)
            : ep(std::move(ep)), backend(backend), breaker(config) {
            if (resilience_enabled) {
                if (rate_config.has_value()) {
                    rate.emplace(*rate_config);
                }
                if (bulkhead_config.has_value()) {
                    bulkhead.emplace(*bulkhead_config);
                }
            }
        }

        ~endpoint_pool() {
            // Debug builds catch a leaked gate permit before the pool (and the
            // objects permits point at) is destroyed.
            if (bulkhead.has_value()) {
                assert(bulkhead->in_use() == 0);
            }
        }

        endpoint ep;
        backend_factory* backend = nullptr;
        std::mutex mutex;
        std::vector<std::shared_ptr<client_detail::connection_slot>> slots;
        resilience::circuit_breaker breaker;
        std::optional<resilience::rate_limiter> rate;
        std::optional<resilience::bulkhead> bulkhead;
        // Live in-flight attempts for this endpoint, read by the load balancer
        // (least-inflight) without taking `mutex`.
        std::atomic<std::uint32_t> in_flight{0};
        bool had_connection = false;
        std::size_t failed_connections = 0;
    };

    struct instance_view final : resilience::instance_set {
        const std::vector<std::unique_ptr<endpoint_pool>>* pools = nullptr;
        std::size_t size() const noexcept override {
            return pools != nullptr ? pools->size() : 0;
        }
        resilience::instance at(std::size_t index) const noexcept override {
            resilience::instance entry;
            if (pools == nullptr || index >= pools->size()) {
                return entry;
            }
            entry.id = index;
            entry.weight = 1;
            entry.healthy = true;
            entry.inflight = (*pools)[index]->in_flight.load(std::memory_order_acquire);
            return entry;
        }
        u64 version() const noexcept override { return 1; }
    };

    std::vector<std::unique_ptr<endpoint_pool>> pools;
    instance_view view;
    std::unique_ptr<resilience::load_balancer> balancer;

    // Live call cores that may still have a shard timer armed. Guarded by
    // `calls_mutex`. The value is a strong reference to the type-erased state:
    // close() snapshots these and finalizes them after releasing the mutex, so
    // without ownership a handle destroyed on another thread could free the
    // core while close() is still writing to it (a real race, found by TSan).
    std::mutex calls_mutex;
    client_detail::flat_pool_map<client_detail::call_core*,
                                 std::shared_ptr<client_detail::call_state_base>>
        active_calls;

    // Shared with every call this client issues; expires when the client is
    // destroyed so a handle that outlives it never calls into freed memory.
    std::shared_ptr<client_detail::client_liveness> liveness =
        std::make_shared<client_detail::client_liveness>();

    // Pool of `call_state` storage. Every call draws its object and control
    // block from here, so a call reaches the system allocator only while the
    // pool is still growing. The pool outlives the client: the allocator stored
    // in each control block holds a `shared_ptr`, so a handle held after
    // close() still has a live pool to return its block to.
    std::shared_ptr<client_detail::call_state_pool> call_pool =
        std::make_shared<client_detail::call_state_pool>();
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
    signal_thread.store(std::this_thread::get_id(),
                        std::memory_order_release);
    done.store(true, std::memory_order_release);
    // The completion callback runs before the waiters are released, so a
    // `get()` that returns after the callback has observed it. A `then` body
    // may call get() on the same handle; it sees the terminal state at once.
    invoke_callback();
    client* const who = owner.load(std::memory_order_acquire);
    // Last touch of this object: `publish_signaled()` sets `signaled` and
    // notifies under `mutex`, so a waiter that returns from here has a
    // happens-before edge with everything above. Nothing below this call
    // may dereference `this`.
    publish_signaled();
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
            endpoints_[i].ep, endpoints_[i].backend, options_.breaker,
            options_.per_endpoint_rate, options_.per_endpoint_bulkhead,
            options_.resilience_enabled));
    }
    impl_->view.pools = &impl_->pools;
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
    // finalize_one() is about to write.
    {
        std::lock_guard<std::mutex> lk(impl->calls_mutex);
        if (closed_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        // Publish the client's death to every handle that still holds a strong
        // reference to the liveness token. Without this write `alive` stayed
        // true forever, so `live_owner()`'s first gate never fired: a frame
        // that keeps `impl` alive (and therefore keeps the token alive) would
        // still be handed `core.owner`, a pointer `~client` already freed.
        // The store is under `calls_mutex`, the same lock that orders
        // `register_core_if_open()`, so a new attempt either observes
        // `closed_` or its liveness write. It is also under the liveness gate,
        // the same lock `live_owner()` takes to read `alive` and to take its
        // reader lease, so a reader either sees the death before taking the
        // lease or is counted and waited for below.
        {
            std::lock_guard<std::mutex> gate(impl->liveness->gate_mutex);
            impl->liveness->alive.store(false, std::memory_order_release);
        }
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
    //
    // The old code copied the whole registry into a `std::vector` here, which
    // allocates. This path is also reached from `~client()` (implicitly
    // `noexcept`), so an allocation failure would terminate the process. Instead
    // the registry is drained in allocation-free batches: each batch is removed
    // from the registry under `calls_mutex` before it is finalized, so a
    // concurrent `unregister_core()` cannot drop an entry before its turn, and
    // the `keep` reference in the batch keeps the state alive for `finalize_one`.
    for (;;) {
        std::array<std::shared_ptr<client_detail::call_state_base>, 64> batch{};
        std::size_t taken;
        {
            std::lock_guard<std::mutex> lk(impl->calls_mutex);
            taken = impl->active_calls.take_used(batch.size(), batch.begin());
        }
        if (taken == 0) {
            break;
        }
        for (std::size_t i = 0; i < taken; ++i) {
            finalize_one(batch[i]);
        }
    }
    // Wait for every `live_owner()` reader to release its lease before the
    // client object is freed by `~client`. A reader that observed `alive == true`
    // under the gate cannot still be holding `core.owner` when the object dies,
    // because `close()` is the last thing that touches the client. `finalize_one`
    // woke any blocked waiter, so the readers drain.
    {
        std::unique_lock<std::mutex> gate(impl->liveness->gate_mutex);
        impl->liveness->gate_cv.wait(
            gate, [&impl] { return impl->liveness->readers == 0; });
    }
}

std::shared_ptr<client_detail::client_liveness>
client::liveness_token() const noexcept {
    return impl_->liveness;
}

std::shared_ptr<client_detail::call_state_pool> client::call_state_pool()
    const noexcept {
    return impl_->call_pool;
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

void client::finalize_one(const std::shared_ptr<client_detail::call_state_base>& keep) noexcept {
    client_detail::call_core* core = &keep->core_ref();
    // A concurrent finish_attempt/abandon_attempt must win exactly once;
    // only the winner releases the timer and slot.
    if (core->finalized.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    // Building the shutdown `rpc_error` allocates (its message exceeds SSO), and
    // this function is `noexcept`. Degrade by dropping the rest of this entry's
    // finalization: the core is already marked `finalized`, so no other path
    // will touch it, and the client is closing anyway.
    try {
        if (options_.shard != nullptr) {
            (void)options_.shard->cancel_timer(core);
        }
        {
            // `permit`/`bulkhead` belong to the attempt, and the admitting
            // thread may still be inside begin_attempt/issue. Take the same
            // lock those paths use before touching them.
            std::lock_guard<std::mutex> lk(core->attempt_mutex);
            if (core->bulkhead.has_value()) {
                core->bulkhead->leave();
                core->bulkhead.reset();
            }
            core->permit = resilience::breaker_permit{};
        }
        std::shared_ptr<client_detail::connection_slot> slot;
        {
            // `issue()` publishes `slot` on the admitting thread; take the same
            // lock before reading it. Copy rather than move: NEW-1 requires the
            // `shared_ptr` to stay in `core->slot` so the call-slot pool
            // outlives the low-level handle that points into it.
            std::lock_guard<std::mutex> lk(core->attempt_mutex);
            slot = core->slot;
        }
        if (slot) {
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
        core->signal_thread.store(std::this_thread::get_id(),
                                  std::memory_order_release);
        core->done.store(true, std::memory_order_release);
        // Publish the shutdown under the lock the non-blocking accessors take,
        // so `error()`/`origin()` do not race this write.
        core->record_terminal(rpc_code::shutting_down,
                              rpc_error(rpc_code::shutting_down, "client is closed"),
                              client_detail::failure_origin::local_lifecycle);
        core->completed.store(true, std::memory_order_release);
        core->cv.notify_all();
        // A `then` callback registered before close() still fires, reporting
        // the shutdown through the handle the callback received.
        core->invoke_callback();
        client* const who = core->owner.load(std::memory_order_acquire);
        core->owner.store(nullptr, std::memory_order_release);
        // Last touch of the core (see call_core::signaled).
        core->publish_signaled();
        if (who != nullptr) {
            who->notify_waiter();
        }
    } catch (...) {
        // Allocation failure while building the shutdown error. The core is
        // already marked `finalized`; the client is closing.
        //
        // `done` was published before the throwing statement, so a handle
        // destructor may already be waiting for `signaled`; publish it here
        // or that wait never ends.
        core->publish_signaled();
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

client::resilience_statistics client::resilience_stats() const noexcept {
    resilience_statistics out;
    out.rate_limited = rate_limited_.load(std::memory_order_relaxed);
    out.bulkhead_rejected = bulkhead_rejected_.load(std::memory_order_relaxed);
    out.local_gate_rejected = local_gate_rejected_.load(std::memory_order_relaxed);
    out.endpoint_open = endpoint_open_.load(std::memory_order_relaxed);
    out.lb_unavailable = lb_unavailable_.load(std::memory_order_relaxed);
    out.pre_send_retries = pre_send_retries_.load(std::memory_order_relaxed);
    out.probe_admitted = probe_admitted_.load(std::memory_order_relaxed);
    out.retries_delayed_by_hint =
        retries_delayed_by_hint_.load(std::memory_order_relaxed);
    for (const std::unique_ptr<impl::endpoint_pool>& pool : impl_->pools) {
        if (pool->bulkhead.has_value()) {
            out.bulkhead_in_use += pool->bulkhead->in_use();
        }
    }
    return out;
}

resilience::fallback_statistics client::fallback_stats() const noexcept {
    // Same independent relaxed snapshot as resilience_stats(): the seven
    // counters are not a transaction and no cross-counter equality is promised.
    resilience::fallback_statistics out;
    out.ineligible =
        impl_->fallback_ineligible.load(std::memory_order_relaxed);
    out.skipped_deadline =
        impl_->fallback_skipped_deadline.load(std::memory_order_relaxed);
    out.attempted = impl_->fallback_attempted.load(std::memory_order_relaxed);
    out.failed = impl_->fallback_failed.load(std::memory_order_relaxed);
    out.canceled = impl_->fallback_canceled.load(std::memory_order_relaxed);
    out.overrun = impl_->fallback_overrun.load(std::memory_order_relaxed);
    out.applied = impl_->fallback_applied.load(std::memory_order_relaxed);
    return out;
}

void client::record_fallbacks(
    const resilience::fallback_statistics& tally) noexcept {
    impl_->fallback_ineligible.fetch_add(tally.ineligible,
                                         std::memory_order_relaxed);
    impl_->fallback_skipped_deadline.fetch_add(tally.skipped_deadline,
                                               std::memory_order_relaxed);
    impl_->fallback_attempted.fetch_add(tally.attempted,
                                        std::memory_order_relaxed);
    impl_->fallback_failed.fetch_add(tally.failed, std::memory_order_relaxed);
    impl_->fallback_canceled.fetch_add(tally.canceled,
                                       std::memory_order_relaxed);
    impl_->fallback_overrun.fetch_add(tally.overrun, std::memory_order_relaxed);
    impl_->fallback_applied.fetch_add(tally.applied, std::memory_order_relaxed);
}

bool client::set_fallback(resilience::fallback_fn function, void* self,
                          std::chrono::nanoseconds min_remaining,
                          void* (*retain)(void*) noexcept,
                          void (*release)(void*) noexcept) noexcept {
    // The pair is a contract: one without the other cannot keep `self` alive and
    // cannot release it, so a half-configured pair is rejected rather than
    // silently ignored. A non-null `self` without the pair would dangle inside
    // the noexcept fallback path.
    if ((retain == nullptr) != (release == nullptr)) {
        return false;
    }
    if (self != nullptr && retain == nullptr) {
        return false;
    }

    std::shared_ptr<impl::fallback_slot> fresh;
    if (function != nullptr) {
        try {
            fresh = std::make_shared<impl::fallback_slot>();
        } catch (...) {
            // A policy that cannot be allocated is not installed. The caller
            // sees `false` and keeps the previous policy (or none).
            return false;
        }
        fresh->function = function;
        fresh->self = self;
        fresh->min_remaining = min_remaining;
        if (retain != nullptr && self != nullptr) {
            // Outside the lock: a slow or lock-taking `retain` must never
            // serialise the library mutex.
            fresh->token = retain(self);
            if (fresh->token == nullptr) {
                // The reference could not be taken, so `fn` must not run with
                // this `self`. Nothing is installed; the slot destructor is a
                // no-op because `token` is null.
                return false;
            }
            fresh->release = release;
        }
    }

    std::shared_ptr<impl::fallback_slot> old;
    bool installed = false;
    {
        std::lock_guard<std::mutex> lk(impl_->fallback_mutex);
        old.swap(impl_->fallback);
        impl_->fallback = std::move(fresh);
        installed = impl_->fallback != nullptr;
    }
    // `old` dies here, outside the lock. If it was the last reference, its
    // destructor calls the consumer's `release`; if a concurrent reader still
    // holds the slot, that reader releases it instead, also outside the lock.
    impl_->fallback_configured.store(installed, std::memory_order_release);
    return true;
}

bool client::set_fallback_owned(resilience::fallback_fn function,
                                std::shared_ptr<void> self,
                                std::chrono::nanoseconds min_remaining) noexcept {
    // The RAII form: `self` is owned by the shared_ptr, so there is no
    // retain/release pair to keep in sync. A null `function` uninstalls.
    if (function == nullptr) {
        return set_fallback(nullptr, nullptr, std::chrono::nanoseconds{0}, nullptr,
                            nullptr);
    }
    if (self == nullptr) {
        return false;
    }

    std::shared_ptr<impl::fallback_slot> fresh;
    try {
        fresh = std::make_shared<impl::fallback_slot>();
    } catch (...) {
        // A policy that cannot be allocated is not installed. The caller sees
        // `false` and keeps the previous policy (or none).
        return false;
    }
    fresh->function = function;
    fresh->self = self.get();
    fresh->min_remaining = min_remaining;
    fresh->owned_self = std::move(self);

    std::shared_ptr<impl::fallback_slot> old;
    {
        std::lock_guard<std::mutex> lk(impl_->fallback_mutex);
        old.swap(impl_->fallback);
        impl_->fallback = std::move(fresh);
    }
    // `old` dies here, outside the lock, releasing the previous policy's owned
    // self and/or retain token.
    impl_->fallback_configured.store(true, std::memory_order_release);
    return true;
}

resilience::fallback_outcome client::run_fallback(
    client_detail::call_core& core, buffer& out, rpc_error& error,
    resilience::fallback_counter_sink* sink) noexcept {
    // The unconfigured client never takes a lock: `get()` keeps the exact
    // pre-S4 path when no fallback was installed.
    if (!impl_->fallback_configured.load(std::memory_order_acquire)) {
        return resilience::fallback_outcome::not_configured;
    }

    // One strong reference covers the whole `fallback_execute` call, including
    // a blocking `fn`: a concurrent set_fallback can only swap the pointer, and
    // the slot stays alive until this function returns.
    std::shared_ptr<impl::fallback_slot> snap;
    {
        std::lock_guard<std::mutex> lk(impl_->fallback_mutex);
        snap = impl_->fallback;
    }
    if (!snap || snap->function == nullptr) {
        return resilience::fallback_outcome::not_configured;
    }

    // A local copy, because `fallback_execute` takes `const fallback_policy&`
    // and must not read the published slot while set_fallback swaps it.
    // `self` is the caller's pointer; `token` never reaches `fn`.
    resilience::fallback_policy policy;
    policy.function = snap->function;
    policy.self = snap->self;
    policy.min_remaining = snap->min_remaining;

    const resilience::fallback_context ctx = build_fallback_context(core, error);

    fallback_tally tally;
    tally.extra = sink;
    resilience::fallback_counter_sink local_sink;
    local_sink.self = &tally;
    local_sink.add = &fallback_tally_add;

    // Unlocked: `fn` may block and may call into user code.
    const resilience::fallback_outcome outcome = resilience::fallback_execute(
        policy, ctx, core.canceled, out, error,
        &resilience::steady_clock_now, &local_sink);

    record_fallbacks(tally.values);
    return outcome;
}

// --- admission and pools -----------------------------------------------------

bool client::begin_attempt(
    client_detail::call_core& core,
    const std::shared_ptr<client_detail::call_state_base>& keep,
    rpc_error& failure) {
    // A new attempt starts un-degraded. This is the only point every attempt
    // passes through, including the two early returns below; clearing here (as
    // well as in reset_for_attempt()) means a second get() cannot inherit the
    // previous attempt's flag.
    core.clear_degraded();
    const auto now = steady_now();
    if (core.has_deadline && now >= core.deadline) {
        core.origin = client_detail::failure_origin::local_lifecycle;
        failure = rpc_error(resilience::reject_code(resilience::stage_kind::deadline),
                            "deadline already passed");
        return false;
    }
    if (core.canceled.load(std::memory_order_acquire)) {
        core.origin = client_detail::failure_origin::cancel;
        failure = rpc_error(rpc_code::canceled, "call canceled");
        return false;
    }
    core.attempts += 1;
    if (impl_->pools.empty()) {
        lb_unavailable_.fetch_add(1, std::memory_order_relaxed);
        core.origin = client_detail::failure_origin::local_lifecycle;
        failure = rpc_error(resilience::reject_code(resilience::stage_kind::dispatch),
                            "no endpoint configured");
        return false;
    }

    // Selection loop: ask the balancer for a candidate, admit it through its
    // breaker, and move on when that breaker is open. `allow()` itself promotes
    // an expired open endpoint to half-open and hands out a probe, so an
    // endpoint is never permanently removed from the ring. The permit returned
    // here is carried into the attempt; later stages must not call allow()
    // again for the same attempt.
    const std::size_t pool_count = impl_->pools.size();
    // One bit per endpoint so the loop visits each candidate at most once. The
    // bitmap is inline for small pools and heap-backed above
    // `tried_endpoints::k_inline_bits`; an allocation failure fails the attempt
    // explicitly rather than silently ignoring candidates (which used to make
    // every endpoint at index >= 512 unreachable).
    client_detail::tried_endpoints tried;
    if (!tried.reset(pool_count)) {
        // The bitmap could not be allocated, so this attempt cannot even track
        // its candidates. Nothing reached an endpoint (a pre-send failure), but
        // this is deliberately `local_lifecycle` rather than `local_capacity`:
        // retrying an allocation failure would amplify memory pressure across
        // the whole pool, so the call fails with `resource_exhausted` and is
        // not retried.
        core.origin = client_detail::failure_origin::local_lifecycle;
        failure = rpc_error(rpc_code::resource_exhausted,
                            "cannot track endpoint candidates");
        return false;
    }

    resilience::breaker_permit chosen;
    std::size_t chosen_index = pool_count;
    bool any_rejected = false;
    bool have_hint = false;
    std::chrono::milliseconds min_hint{0};
    // Linear fallback cursor. It only moves forward because a tried endpoint is
    // never untried within one attempt, so the whole loop stays O(pool_count)
    // instead of rescanning the tried prefix on every iteration.
    std::size_t fallback_cursor = 0;
    for (std::size_t iteration = 0; iteration < pool_count; ++iteration) {
        std::size_t index = pool_count;
        const result<resilience::instance> selected = impl_->balancer->select(impl_->view);
        if (selected.ok()) {
            const auto candidate = static_cast<std::size_t>(selected.value().id);
            if (candidate < pool_count && !tried.test(candidate)) {
                index = candidate;
            }
        }
        if (index == pool_count) {
            // consistent_hash returns the same key repeatedly, so the main
            // selection can stall on a tried endpoint. Fall back to the first
            // candidate not yet tried.
            while (fallback_cursor < pool_count && tried.test(fallback_cursor)) {
                ++fallback_cursor;
            }
            if (fallback_cursor < pool_count) {
                index = fallback_cursor;
            }
        }
        if (index == pool_count) {
            break;
        }
        tried.mark(index);
        resilience::breaker_permit permit = impl_->pools[index]->breaker.allow();
        if (permit.allowed()) {
            chosen = std::move(permit);
            chosen_index = index;
            break;
        }
        any_rejected = true;
        const std::chrono::milliseconds hint = permit.retry_after();
        if (hint.count() > 0 && (!have_hint || hint < min_hint)) {
            min_hint = hint;
            have_hint = true;
        }
    }

    if (chosen_index == pool_count) {
        if (any_rejected) {
            endpoint_open_.fetch_add(1, std::memory_order_relaxed);
            breaker_rejections_.fetch_add(1, std::memory_order_relaxed);
            core.origin = client_detail::failure_origin::endpoint_open;
            failure = rpc_error(resilience::reject_code(resilience::stage_kind::breaker),
                                "endpoint circuit breaker is open");
            // A pre-send retry must not spin: wait at least the policy's first
            // backoff (or longer when the breaker asks for it).
            std::int64_t initial_ns = options_.retry.initial_backoff.count();
            if (initial_ns < 0) {
                initial_ns = 0;
            }
            std::chrono::milliseconds initial_ms(
                (initial_ns + 999'999) / 1'000'000);
            std::chrono::milliseconds delay = initial_ms;
            if (!have_hint || min_hint > delay) {
                delay = have_hint ? min_hint : delay;
            }
            failure.retry_after = delay;
        } else {
            lb_unavailable_.fetch_add(1, std::memory_order_relaxed);
            core.origin = client_detail::failure_origin::local_lifecycle;
            failure = rpc_error(resilience::reject_code(resilience::stage_kind::dispatch),
                                "no usable endpoint");
        }
        return false;
    }

    impl::endpoint_pool& pool = *impl_->pools[chosen_index];
    const bool is_probe = chosen.is_probe();
    if (is_probe) {
        probe_admitted_.fetch_add(1, std::memory_order_relaxed);
    }
    {
        // The terminal path (cancel()/close()/a racing completion) may already
        // be dropping this attempt's resources; publish the permit under the
        // same lock so the two never touch `permit` concurrently. If the
        // attempt was already finalized, do not publish at all: `chosen` then
        // releases its admission as it goes out of scope, so a late
        // `begin_attempt` cannot leave an unrecorded permit behind.
        std::lock_guard<std::mutex> lk(core.attempt_mutex);
        if (core.finalized.load(std::memory_order_acquire)) {
            core.origin = core.canceled.load(std::memory_order_acquire)
                              ? client_detail::failure_origin::cancel
                              : client_detail::failure_origin::local_lifecycle;
            failure = core.canceled.load(std::memory_order_acquire)
                          ? rpc_error(rpc_code::canceled, "call canceled")
                          : rpc_error(rpc_code::shutting_down, "client is closed");
            return false;
        }
        core.permit = std::move(chosen);
    }
    core.pool = &pool;

    // Per-endpoint gates run after selection and breaker admission. A half-open
    // probe skips them so a saturated limiter cannot starve recovery; the pool
    // and in-flight caps still apply to probes. Gate rejection is a local
    // admission decision: the permit is ignored by finish_attempt and the
    // caller does not retry automatically.
    if (!is_probe) {
        if (pool.rate.has_value()) {
            resilience::rate_limit_result limited;
            if (core.has_deadline) {
                const auto deadline_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    core.deadline.time_since_epoch());
                limited = pool.rate->acquire_for(1, deadline_ns);
            } else {
                limited = pool.rate->try_acquire(1);
            }
            if (!limited.ok()) {
                rate_limited_.fetch_add(1, std::memory_order_relaxed);
                local_gate_rejected_.fetch_add(1, std::memory_order_relaxed);
                core.origin = client_detail::failure_origin::local_gate;
                if (limited.deadline_met) {
                    failure = rpc_error(resilience::reject_code(
                                            resilience::stage_kind::endpoint_rate,
                                            limited.deadline_met),
                                        "endpoint rate limit reached");
                    failure.retry_after = limited.retry_after;
                } else {
                    // The caller cannot wait out the bucket within its own
                    // deadline, so the hint would be meaningless here.
                    failure = rpc_error(resilience::reject_code(
                                            resilience::stage_kind::endpoint_rate,
                                            limited.deadline_met),
                                        "endpoint rate limit cannot meet the deadline");
                }
                return false;
            }
        }
        if (pool.bulkhead.has_value()) {
            resilience::bulkhead_permit permit = pool.bulkhead->try_enter();
            if (!permit.allowed()) {
                bulkhead_rejected_.fetch_add(1, std::memory_order_relaxed);
                local_gate_rejected_.fetch_add(1, std::memory_order_relaxed);
                core.origin = client_detail::failure_origin::local_gate;
                failure = rpc_error(resilience::reject_code(
                                        resilience::stage_kind::endpoint_bulkhead),
                                    "endpoint bulkhead is full");
                failure.retry_after = permit.retry_after();
                return false;
            }
            {
                std::lock_guard<std::mutex> lk(core.attempt_mutex);
                if (core.finalized.load(std::memory_order_acquire)) {
                    // The terminal path already cleared the attempt's resources;
                    // adding a bulkhead slot now would never be released.
                    core.origin = core.canceled.load(std::memory_order_acquire)
                                      ? client_detail::failure_origin::cancel
                                      : client_detail::failure_origin::local_lifecycle;
                    failure = core.canceled.load(std::memory_order_acquire)
                                  ? rpc_error(rpc_code::canceled, "call canceled")
                                  : rpc_error(rpc_code::shutting_down,
                                              "client is closed");
                    return false;
                }
                core.bulkhead.emplace(std::move(permit));
            }
        }
    }
    // The closed decision and the tracking insertion are one atomic step
    // (R1). If close()/``~client`` already set `closed_` (or will do so before
    // this returns), either the insert wins and the snapshot in close() sees
    // the core, or this returns false and no timer is ever armed.
    if (!register_core_if_open(core, keep)) {
        {
            std::lock_guard<std::mutex> lk(core.attempt_mutex);
            core.permit = resilience::breaker_permit{};
            core.bulkhead.reset();
        }
        core.pool = nullptr;
        core.origin = client_detail::failure_origin::local_lifecycle;
        failure = rpc_error(rpc_code::shutting_down, "client is closed");
        return false;
    }
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
        core.origin = client_detail::failure_origin::local_lifecycle;
        failure = rpc_error(rpc_code::internal, "attempt has no endpoint pool");
        return nullptr;
    }
    if (pool->backend == nullptr) {
        core.origin = client_detail::failure_origin::local_lifecycle;
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
        pool->in_flight.fetch_add(1, std::memory_order_acq_rel);
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
        core.origin = client_detail::failure_origin::local_capacity;
        failure = rpc_error(rpc_code::resource_exhausted,
                            "connection pool is saturated");
        return nullptr;
    }
    if (pool->slots.size() >= options_.pool_size) {
        core.origin = client_detail::failure_origin::local_capacity;
        failure = rpc_error(rpc_code::resource_exhausted,
                            "connection pool is exhausted");
        return nullptr;
    }

    std::unique_ptr<connection> raw = pool->backend->connect(pool->ep, connect_options{});
    if (!raw) {
        core.origin = client_detail::failure_origin::endpoint_pre_send;
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
    slot->idle_since_ns.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            steady_now().time_since_epoch())
            .count(),
        std::memory_order_relaxed);

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
    pool->in_flight.fetch_add(1, std::memory_order_acq_rel);
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
    slot->idle_since_ns.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            steady_now().time_since_epoch())
            .count(),
        std::memory_order_relaxed);
    auto* pool = static_cast<impl::endpoint_pool*>(slot->pool);
    if (pool != nullptr) {
        pool->in_flight.fetch_sub(1, std::memory_order_acq_rel);
    }
    if (!transport_failure(code)) {
        return;
    }
    slot->healthy.store(false, std::memory_order_release);
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
                            rpc_error error,
                            client_detail::failure_origin origin,
                            client_detail::breaker_record record) noexcept {
    if (core.finalized.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    core.record_terminal(code, std::move(error), origin);
    if (options_.shard != nullptr) {
        (void)options_.shard->cancel_timer(&core);
    }
    {
        // The admitting thread may still be assigning the attempt's permit.
        // Serialize on the same lock begin_attempt/issue use.
        std::lock_guard<std::mutex> lk(core.attempt_mutex);
        if (core.bulkhead.has_value()) {
            core.bulkhead->leave();
            core.bulkhead.reset();
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
    }
    {
        std::lock_guard<std::mutex> lk(core.attempt_mutex);
        if (core.slot) {
            release_slot(core.slot, code);
            core.slot.reset();
        }
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
    // An abandoned attempt records no breaker outcome; just drop the permits.
    {
        // `cancel()` can run while the admitting thread is still inside
        // begin_attempt/issue, so both sides take `attempt_mutex` before
        // touching the attempt's permit or bulkhead.
        std::lock_guard<std::mutex> lk(core.attempt_mutex);
        if (core.bulkhead.has_value()) {
            core.bulkhead->leave();
            core.bulkhead.reset();
        }
        core.permit = resilience::breaker_permit{};
    }
    std::shared_ptr<client_detail::connection_slot> slot;
    {
        // `issue()` publishes `slot` on the admitting thread; read it under the
        // same lock and keep a local reference so the slot outlives the release.
        std::lock_guard<std::mutex> lk(core.attempt_mutex);
        slot = core.slot;
    }
    if (slot) {
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

bool client::should_retry(client_detail::call_core& core, const rpc_error& error,
                          client_detail::failure_origin origin,
                          std::chrono::nanoseconds& delay) noexcept {
    const rpc_code code = error.code;
    if (code == rpc_code::ok) {
        return false;
    }
    const bool pre_send =
        origin == client_detail::failure_origin::local_capacity ||
        origin == client_detail::failure_origin::endpoint_open ||
        origin == client_detail::failure_origin::endpoint_pre_send;
    switch (origin) {
    case client_detail::failure_origin::local_lifecycle:
    case client_detail::failure_origin::local_gate:
    case client_detail::failure_origin::cancel:
    case client_detail::failure_origin::timeout:
        // Gate rejections are the caller's own quota (retrying bypasses the
        // limit), lifecycle failures are terminal, and a local timeout is a
        // dependency failure that must not be amplified.
        return false;
    case client_detail::failure_origin::local_capacity:
    case client_detail::failure_origin::endpoint_open:
    case client_detail::failure_origin::endpoint_pre_send:
        // Nothing was sent: retrying cannot duplicate a side effect.
        break;
    case client_detail::failure_origin::endpoint_sent:
    case client_detail::failure_origin::remote:
        if (!core.idempotent || !is_retryable(code)) {
            return false;
        }
        break;
    }
    if (core.has_deadline && steady_now() >= core.deadline) {
        return false;
    }
    if (!core.retry) {
        return false;
    }
    std::chrono::nanoseconds min_delay{0};
    if (error.retry_after.count() > 0) {
        min_delay = std::chrono::duration_cast<std::chrono::nanoseconds>(
            error.retry_after);
    }
    const std::int64_t deadline_ns =
        core.has_deadline
            ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                  core.deadline.time_since_epoch())
                  .count()
            : 0;
    std::chrono::nanoseconds computed{0};
    if (!core.retry->next(code, core.idempotent || pre_send, min_delay,
                          std::chrono::nanoseconds(deadline_ns), core.has_deadline,
                          computed)) {
        return false;
    }
    if (pre_send) {
        pre_send_retries_.fetch_add(1, std::memory_order_relaxed);
    }
    // Count only the retries whose wait the hint actually determined. `computed`
    // is `max(backoff, hint)`, so `computed == min_delay` means the advertised
    // hint (not the backoff policy) set the delay; a hint below the backoff
    // changed nothing and must not be reported as a hint-driven delay.
    if (error.retry_after.count() > 0 && min_delay.count() > 0 &&
        computed == min_delay) {
        retries_delayed_by_hint_.fetch_add(1, std::memory_order_relaxed);
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
    if (delay.count() <= 0) {
        return;
    }
    const auto until = steady_now() + delay;
    if (options_.shard != nullptr && options_.shard->loop_mode() &&
        options_.shard->is_owner_thread() && !options_.shard->stopped()) {
        // A shard has no per-call cancellation for `sleep_until`, so cut the
        // wait into 5 ms slices and re-check the cancel flag between them.
        // The upper bound on cancel latency is one slice plus scheduling.
        while (steady_now() < until) {
            if (core.canceled.load(std::memory_order_acquire) ||
                closed_.load(std::memory_order_acquire)) {
                return;
            }
            auto slice = steady_now() + std::chrono::milliseconds(5);
            if (slice > until) {
                slice = until;
            }
            const std::uint64_t deadline_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    slice.time_since_epoch())
                    .count());
            const status slept = options_.shard->sleep_until(deadline_ns);
            if (slept.ok() || slept.code() == rpc_code::canceled) {
                continue;
            }
            // Not inside a coroutine: do not block the shard.
            if (slept.code() == rpc_code::failed_precondition) {
                return;
            }
        }
        return;
    }
    std::unique_lock<std::mutex> lk(core.mutex);
    const auto canceled_or_closed = [this, &core] {
        return core.canceled.load(std::memory_order_acquire) ||
               closed_.load(std::memory_order_acquire);
    };
    while (steady_now() < until) {
        if (canceled_or_closed()) {
            return;
        }
        // Short slices bound the close() latency; cancel() notifies the same
        // condition variable and wakes this wait immediately.
        auto slice = steady_now() + std::chrono::milliseconds(5);
        if (slice > until) {
            slice = until;
        }
        if (core.cv.wait_until(lk, slice, canceled_or_closed)) {
            return;
        }
    }
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
    //
    // Snapshotting `pool->slots` allocates, and this function is noexcept (it
    // runs on the shard timer thread). An allocation failure would reach
    // `std::terminate`. Dropping this tick is safe: the next tick re-snapshots
    // and re-polls, so the only cost of a skipped tick is a delayed deadline.
    try {
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
    } catch (...) {
        // Allocation failure while snapshotting. Skip this tick; the catch body
        // performs no allocation.
    }
}

void client::evict_idle(const std::shared_ptr<impl>& impl,
                        std::chrono::nanoseconds idle_eviction) noexcept {
    // Same allocation-safety contract as `deadline_tick`: the snapshot copy can
    // throw `bad_alloc` and this function is noexcept, so the body is guarded and
    // a failure degrades to "evict nothing this tick".
    try {
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
                const std::chrono::steady_clock::time_point idle_since{
                    std::chrono::nanoseconds(slot->idle_since_ns.load(
                        std::memory_order_relaxed))};
                if (now - idle_since < idle_eviction) {
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
    } catch (...) {
        // Allocation failure while snapshotting. Evict nothing this tick.
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

client::builder& client::builder::resilience_enabled(bool value) {
    options_.resilience_enabled = value;
    return *this;
}

client::builder& client::builder::per_endpoint_rate(
    resilience::rate_limit_config config) {
    options_.per_endpoint_rate = config;
    return *this;
}

client::builder& client::builder::per_endpoint_bulkhead(
    resilience::bulkhead_config config) {
    options_.per_endpoint_bulkhead = config;
    return *this;
}

client::builder& client::builder::clear_per_endpoint_rate() {
    options_.per_endpoint_rate.reset();
    return *this;
}

client::builder& client::builder::clear_per_endpoint_bulkhead() {
    options_.per_endpoint_bulkhead.reset();
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

client::builder& client::builder::default_timeout(std::chrono::nanoseconds value) {
    return default_deadline(value);
}

client::builder& client::builder::per_connection_inflight(std::size_t value) {
    return max_inflight_per_connection(value);
}

std::unique_ptr<client> client::builder::build() {
    if (endpoints_.empty()) {
        return nullptr;
    }
    return std::unique_ptr<client>(new client(options_, endpoints_));
}

} // namespace crpc
