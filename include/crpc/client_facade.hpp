// CRPC -- client facade: connection pool, load balancing, breaker and retry.
//
// A `client` owns one pool of connections per configured endpoint. A call
// selects an endpoint through a `resilience::load_balancer`, admits it through
// that endpoint's circuit breaker, takes a pooled connection and hands the
// request to the low-level `rpc::client`. A failed connection is dropped from
// the pool and counted; the next attempt re-selects an endpoint.
//
// Coroutine suspension
// --------------------
// `call<Resp>::get()` suspends when called from a coroutine of the runtime the
// client was bound to with `builder::on_shard()`. The response completion runs
// on whichever thread received the bytes; it sets the call's ready flag and
// rings the shard doorbell. Inside the shard the coroutine parks through
// `runtime::park()`; a plain thread instead waits on a condition variable for
// `options::plain_thread_timeout` and then fails with `deadline_exceeded`.
// Calling `get()` from a shard thread that is not inside a coroutine fails with
// `failed_precondition` rather than stalling the shard.
//
// Retry
// -----
// A retry happens only when the failure code is `is_retryable`, the shared
// retry budget admits it, and the overall deadline has not passed. A failure
// observed before anything was sent (`local_capacity`, `endpoint_open`,
// `endpoint_pre_send`) is retried even for a non-idempotent call, because no
// side effect can have happened. Once an attempt has been sent (`endpoint_sent`
// or a `remote` result) a retry additionally requires the call to be declared
// idempotent or to carry an idempotency key. Every attempt re-selects an
// endpoint through the load balancer and re-admits through the breaker. The
// same idempotency key is reused across attempts.
//
// Thread safety
// -------------
// Public methods are intended to be called from the shard bound with
// `on_shard()` (or from a plain thread when no shard is bound). The reader
// path may run on any thread; per-connection access is serialized by an
// internal recursive lock. `close()` and `stats()` are safe from any thread.
#ifndef CRPC_CLIENT_FACADE_HPP
#define CRPC_CLIENT_FACADE_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <crpc/buffer.hpp>
#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/frame.hpp>
#include <crpc/io.hpp>
#include <crpc/resilience/bulkhead.hpp>
#include <crpc/resilience/circuit_breaker.hpp>
#include <crpc/resilience/fallback.hpp>
#include <crpc/resilience/load_balancer.hpp>
#include <crpc/resilience/rate_limiter.hpp>
#include <crpc/resilience/retry.hpp>
#include <crpc/rpc/call.hpp>
#include <crpc/rpc/client.hpp>
#include <crpc/runtime.hpp>
#include <crpc/span.hpp>

namespace crpc {

class client;

namespace client_detail {

// One pooled connection plus its serialization lock.
struct connection_slot {
    struct guarded_source final : io::source {
        explicit guarded_source(connection_slot* owner) noexcept : self(owner) {}

        // The low-level `rpc::client` now serializes its own entry points with
        // an internal lock, so the facade must not additionally hold
        // `connection_slot::mutex` here: an in-process push can synchronously
        // deliver queued bytes back into this source while the writer already
        // holds that lock, which would invert the lock order (CF-1 follow-up).
        //
        // The transport keeps a raw pointer to this object and may call it from
        // another thread, including while the slot is being destroyed. `active`
        // counts calls in progress and `stopping` closes the gate; the slot
        // destructor waits for `active` to reach zero before it releases
        // anything the callbacks use. Reading `self->conn` directly was a real
        // data race with `conn.reset()` (TSan: ~connection_slot vs on_data).
        std::size_t on_data(span<const std::byte> data) noexcept override {
            if (!enter()) {
                return 0;
            }
            rpc::client* target = conn.load(std::memory_order_acquire);
            const std::size_t used =
                target != nullptr ? target->source().on_data(data) : 0;
            leave();
            return used;
        }
        void on_eof() noexcept override {
            if (!enter()) {
                return;
            }
            rpc::client* target = conn.load(std::memory_order_acquire);
            if (target != nullptr) {
                target->source().on_eof();
            }
            leave();
        }
        void on_error(std::error_code ec) noexcept override {
            if (!enter()) {
                return;
            }
            rpc::client* target = conn.load(std::memory_order_acquire);
            if (target != nullptr) {
                target->source().on_error(ec);
            }
            leave();
        }

        // Blocks new calls, then waits for the ones already running.
        void retire() noexcept {
            {
                std::lock_guard<std::mutex> lk(gate);
                stopping = true;
            }
            std::unique_lock<std::mutex> lk(gate);
            idle.wait(lk, [this] { return active == 0; });
        }

        void detach(rpc::client* target) noexcept {
            conn.store(target, std::memory_order_release);
        }

        connection_slot* self;

    private:
        bool enter() noexcept {
            std::lock_guard<std::mutex> lk(gate);
            if (stopping) {
                return false;
            }
            ++active;
            return true;
        }

        void leave() noexcept {
            std::lock_guard<std::mutex> lk(gate);
            if (--active == 0) {
                idle.notify_all();
            }
        }

        std::atomic<rpc::client*> conn{nullptr};
        std::mutex gate;
        std::condition_variable idle;
        int active = 0;
        bool stopping = false;
    };

    // The transport holds a raw pointer to `guarded` (bind_source) and may be
    // calling it from another thread right now. Retire it first (blocks new
    // calls and waits for running ones), then destroy the connection and the
    // adapter. Destroying in the other order raced `conn.reset()` against a
    // concurrent `on_data`.
    ~connection_slot() {
        if (guarded) {
            guarded->retire();
        }
        conn.reset();
        guarded.reset();
    }

    std::unique_ptr<rpc::client> conn;
    std::unique_ptr<guarded_source> guarded;
    std::recursive_mutex mutex;
    std::atomic<std::uint32_t> in_flight{0};
    std::atomic<bool> healthy{true};
    std::chrono::steady_clock::time_point idle_since{};
    void* pool = nullptr;
};

// Liveness token shared by a client and the calls it issued. A handle may
// outlive the client (cancel() then destroy the client, then destroy the
// handle); the weak token lets that path detect the death instead of calling a
// member function on a freed client.
struct client_liveness {
    std::atomic<bool> alive{true};
};

// How a finished attempt feeds the endpoint breaker. A logical call records
// exactly one outcome: intermediate attempts of a retry are ignored, and the
// terminal result is classified or, for a locally observed timeout, recorded as
// a failure. Caller cancellations and abandoned handles are ignored.
enum class breaker_record : u8 {
    ignore,
    by_code,
    failure
};

// Where a failed attempt failed. The origin decides whether the endpoint
// breaker is touched at all, whether a retry is legal without idempotency, and
// whether `unknown_side_effect` can apply. Local admission and lifecycle
// failures say nothing about the endpoint.
enum class failure_origin : u8 {
    local_lifecycle,
    local_gate,
    local_capacity,
    endpoint_open,
    endpoint_pre_send,
    endpoint_sent,
    remote,
    timeout,
    cancel
};

// Maps an origin and the terminal code to the single breaker outcome.
[[nodiscard]] breaker_record breaker_record_for(failure_origin origin,
                                                rpc_code code) noexcept;

// Classifies a completed low-level call. A local timeout carries the
// `crpc.local_timeout` metadata marker; a peer deadline does not.
[[nodiscard]] failure_origin result_origin(const rpc_error& error) noexcept;

// Candidate bookkeeping for the endpoint selection loop. One logical attempt
// must visit each endpoint at most once, so the loop keeps one bit per
// endpoint. A pool of up to `k_inline_bits` endpoints uses the inline bitmap
// and allocates nothing; a larger pool grows a heap bitmap sized to the pool.
// The previous fixed 512-bit array silently ignored higher indexes, which made
// every endpoint at or above index 512 unreachable once the lower candidates
// were tried (the linear fallback then kept re-selecting index 512).
class tried_endpoints {
public:
    static constexpr std::size_t k_inline_bits = 512;
    static constexpr std::size_t k_inline_words = k_inline_bits / 64;

    // Returns false only when a pool larger than `k_inline_bits` cannot allocate
    // its bitmap. The caller must then fail the attempt explicitly instead of
    // running a selection loop that cannot track its candidates.
    [[nodiscard]] bool reset(std::size_t count) {
        count_ = count;
        heap_.reset();
        words_ = inline_;
        std::memset(inline_, 0, sizeof(inline_));
        if (count_ <= k_inline_bits) {
            return true;
        }
        // Reject counts whose word count would overflow before dividing. The
        // caller cannot reach this today, but the arithmetic must not wrap.
        if (count_ > static_cast<std::size_t>(-1) - 63) {
            count_ = 0;
            return false;
        }
        const std::size_t words = (count_ + 63) / 64;
        // `new (std::nothrow)` rather than `make_unique`: this header must build
        // with exceptions disabled, and the value-initialising form also clears
        // the storage, so no second memset is needed.
        heap_.reset(new (std::nothrow) std::uint64_t[words]());
        if (!heap_) {
            count_ = 0;
            return false;
        }
        words_ = heap_.get();
        return true;
    }

    // Precondition: `index < size()`. Every index below `size()` is tracked.
    void mark(std::size_t index) noexcept {
        if (index < count_) {
            words_[index / 64] |= (std::uint64_t{1} << (index % 64));
        }
    }
    [[nodiscard]] bool test(std::size_t index) const noexcept {
        return index < count_ &&
               (words_[index / 64] & (std::uint64_t{1} << (index % 64))) != 0;
    }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }
    // True while the bitmap lives in the object (no heap allocation). Pools up
    // to `k_inline_bits` endpoints always stay on this path.
    [[nodiscard]] bool uses_inline_storage() const noexcept {
        return words_ == inline_;
    }

private:
    std::uint64_t inline_[k_inline_words] = {};
    std::unique_ptr<std::uint64_t[]> heap_;
    std::uint64_t* words_ = inline_;
    std::size_t count_ = 0;
};

// Type-independent per-call state.
struct call_core {
    // Atomic: the completion callback may read it on a backend thread while
    // close()/~client detaches it (CF-1).
    std::atomic<client*> owner{nullptr};
    // Expires when the client is destroyed. Always test it before using
    // `owner`: an abandoned call is not in the client's registry, so close()
    // never nulls its `owner`.
    std::weak_ptr<client_liveness> owner_liveness;
    u64 service_id = 0;
    u64 method_id = 0;
    buffer request;
    rpc::call_context ctx;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> done{false};
    // Set by whichever of finish_attempt/abandon_attempt wins the attempt's
    // terminal transition. Guards exactly-once release of the deadline timer,
    // the connection slot and the breaker permit.
    std::atomic<bool> finalized{false};
    std::atomic<rpc_code> terminal{rpc_code::ok};
    // Set by `call::cancel()`, with or without an in-flight low-level call.
    // The retry wait polls it so a backoff can be interrupted.
    std::atomic<bool> canceled{false};
    rpc_error error;
    std::uint32_t attempts = 0;
    bool idempotent = false;
    bool has_deadline = false;
    failure_origin origin = failure_origin::local_lifecycle;
    std::chrono::steady_clock::time_point deadline{};
    void* pool = nullptr;
    std::shared_ptr<connection_slot> slot;
    // Serializes the admission resources an attempt owns between the thread
    // that admits the attempt (begin_attempt / issue) and the thread that ends
    // it (finish_attempt / abandon_attempt / finalize_active). Without it,
    // `cancel()` could clear `permit` while `begin_attempt` was assigning the
    // next attempt's permit (a data race on a non-atomic member, and a permit
    // that could be dropped without ever being recorded). Lock order:
    // `attempt_mutex` may be taken before `pool->mutex`/`slot->mutex` is
    // released, never after.
    std::mutex attempt_mutex;
    resilience::breaker_permit permit;
    std::optional<resilience::bulkhead_permit> bulkhead;
    std::unique_ptr<resilience::retry_sequence> retry;

    void signal() noexcept;
    // True when the last completed attempt was answered by a fallback value.
    // The flag is cleared when the next attempt is armed.
    [[nodiscard]] bool degraded() const noexcept {
        return degraded_.load(std::memory_order_acquire);
    }
    void reset_for_attempt() noexcept {
        done.store(false, std::memory_order_release);
        finalized.store(false, std::memory_order_release);
        terminal.store(rpc_code::ok, std::memory_order_release);
        degraded_.store(false, std::memory_order_release);
    }

private:
    friend class crpc::client;

    // Set by the future fallback consumer after `fallback_execute` returns
    // `applied` (S4). Private so only the client can observe the terminal
    // outcome; the public handle exposes the read side.
    void mark_degraded() noexcept {
        degraded_.store(true, std::memory_order_release);
    }

    // Called at the top of every attempt, so a new attempt never inherits the
    // previous one's flag. `reset_for_attempt()` also clears it, but the
    // success exit of get() returns without going through that path.
    void clear_degraded() noexcept {
        degraded_.store(false, std::memory_order_relaxed);
    }

    std::atomic<bool> degraded_{false};
};

// Type-erased owner for one call. The client keeps a strong reference to it
// while the attempt is registered, so close() can finalize an attempt without
// racing the handle that is dropping the same state on another thread (a raw
// pointer snapshot used to leave close() writing into a freed core).
struct call_state_base {
    virtual ~call_state_base() = default;
    [[nodiscard]] virtual call_core& core_ref() noexcept = 0;
};

template <typename Resp>
struct call_state final : call_state_base {
    call_core core;
    // Guards the `inner` pointer between the call owner and a concurrent
    // cancel() from another thread. The pointed-to low-level handle has its
    // own synchronization; this mutex only protects the pointer lifetime.
    std::mutex inner_mutex;
    std::unique_ptr<rpc::call<Resp>> inner;

    [[nodiscard]] call_core& core_ref() noexcept override { return core; }
};

// Returns the owner only while the client is alive. `core.owner` alone is not
// enough: an abandoned call leaves the registry, so a destroyed client never
// nulls that pointer, and a handle released afterwards would call a member
// function on freed memory.
[[nodiscard]] inline client* live_owner(const call_core& core) noexcept {
    const std::shared_ptr<client_liveness> live = core.owner_liveness.lock();
    if (!live || !live->alive.load(std::memory_order_acquire)) {
        return nullptr;
    }
    return core.owner.load(std::memory_order_acquire);
}

template <typename Resp>
struct wake_functor {
    call_core* core = nullptr;
    void operator()(rpc::call<Resp>&) const noexcept {
        if (core != nullptr) {
            core->signal();
        }
    }
};

} // namespace client_detail

class CRPC_API client {
public:
    struct endpoint_config {
        endpoint ep;
        backend_factory* backend = nullptr;
    };

    struct options {
        // Codec advertised in the call envelope. Only crpc-binary is wired.
        codec_id codec = codec_id::crpc_binary;
        // Connections kept per endpoint.
        std::size_t pool_size = 1;
        // Call slots preallocated per connection.
        std::size_t slot_capacity = 4096;
        // In-flight cap per connection. Zero disables the cap.
        std::size_t max_inflight_per_connection = 1024;
        // Load balancing across endpoints.
        resilience::lb_strategy lb = resilience::lb_strategy::round_robin;
        // Circuit breaker per endpoint.
        resilience::breaker_config breaker{};
        // Retry policy for the whole client.
        resilience::retry_policy retry{};
        // Per-endpoint resilience gates. Empty by default: no object, no lock
        // and no allocation on the call path.
        bool resilience_enabled = true;
        std::optional<resilience::rate_limit_config> per_endpoint_rate;
        std::optional<resilience::bulkhead_config> per_endpoint_bulkhead;
        // Applied when a call carries no explicit deadline.
        std::chrono::nanoseconds default_deadline{std::chrono::seconds(1)};
        bool default_idempotent = false;
        // Plain-thread blocking bound for `get()`.
        std::chrono::nanoseconds plain_thread_timeout{std::chrono::seconds(5)};
        // Drop connections idle longer than this, keeping one warm connection.
        // Zero disables idle eviction.
        std::chrono::milliseconds idle_eviction{std::chrono::milliseconds(0)};
        // Shard that drives the connections and the deadline tick. Optional.
        runtime* shard = nullptr;
    };

    struct statistics {
        std::size_t active_connections = 0;
        std::size_t in_flight_calls = 0;
        std::size_t failed_connections = 0;
        std::size_t calls_started = 0;
        std::size_t retries = 0;
        std::size_t breaker_rejections = 0;
    };

    // Additive observability for the client resilience pipeline. This is a new
    // value type; `client::statistics` keeps its existing layout and ABI.
    struct resilience_statistics {
        std::size_t rate_limited = 0;
        std::size_t bulkhead_rejected = 0;
        std::size_t local_gate_rejected = 0;
        std::size_t endpoint_open = 0;
        std::size_t lb_unavailable = 0;
        std::size_t pre_send_retries = 0;
        std::size_t probe_admitted = 0;
        std::size_t retries_delayed_by_hint = 0;
        // Current per-endpoint bulkhead occupancy (0 when no gate is
        // configured). Lets tests observe close/cancel releasing a permit.
        std::size_t bulkhead_in_use = 0;
    };

    class builder {
    public:
        builder& add_endpoint(endpoint ep, backend_factory& backend);
        builder& pool_size(std::size_t value);
        builder& slot_capacity(std::size_t value);
        builder& max_inflight_per_connection(std::size_t value);
        builder& load_balancer(resilience::lb_strategy strategy);
        builder& breaker(resilience::breaker_config config);
        builder& retry(resilience::retry_policy policy);
        builder& resilience_enabled(bool value);
        builder& per_endpoint_rate(resilience::rate_limit_config config);
        builder& per_endpoint_bulkhead(resilience::bulkhead_config config);
        builder& clear_per_endpoint_rate();
        builder& clear_per_endpoint_bulkhead();
        builder& default_deadline(std::chrono::nanoseconds value);
        builder& default_idempotent(bool value);
        builder& plain_thread_timeout(std::chrono::nanoseconds value);
        builder& idle_eviction(std::chrono::milliseconds value);
        builder& on_shard(runtime& shard);
        [[nodiscard]] std::unique_ptr<client> build();

    private:
        options options_{};
        std::vector<endpoint_config> endpoints_;
    };

    template <typename Resp>
    class [[nodiscard]] call {
    public:
        call() noexcept = default;
        call(call&& other) noexcept : state_(std::move(other.state_)) {}
        call& operator=(call&& other) noexcept {
            if (this != &other) {
                // Releasing a still-active old state_ must abandon its attempt
                // (cancel timer, release slot) before taking over the new one.
                release();
                state_ = std::move(other.state_);
            }
            return *this;
        }
        call(const call&) = delete;
        call& operator=(const call&) = delete;
        ~call() { release(); }

        [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }
        [[nodiscard]] bool ready() const noexcept {
            return state_ != nullptr && state_->core.done.load(std::memory_order_acquire);
        }
        [[nodiscard]] std::uint32_t attempts() const noexcept {
            return state_ != nullptr ? state_->core.attempts : 0;
        }

        // True when the terminal result was produced by a fallback function.
        // Always false in this slice: nothing calls mark_degraded() yet. It is
        // only meaningful after get() returned; try_get() moves the state out,
        // so a later call reads false.
        [[nodiscard]] bool degraded() const noexcept {
            return state_ != nullptr && state_->core.degraded();
        }

        // Suspends (or blocks) until a terminal result is available. Returns
        // the decoded value, or the last error when retries are exhausted.
        [[nodiscard]] rpc::rpc_result<Resp> get() {
            if (state_ == nullptr ||
                client_detail::live_owner(state_->core) == nullptr) {
                return rpc::rpc_result<Resp>(
                    rpc_error(rpc_code::failed_precondition, "invalid call handle"));
            }
            std::shared_ptr<client_detail::call_state<Resp>> st = state_;
            for (;;) {
                // Re-read the owner every turn: close()/``~client`` detaches it
                // in finalize_active(), so a cached pointer would be stale (R1
                // follow-up). A detached attempt reports `shutting_down`
                // delivered by close() rather than touching the freed client.
                client* const owner = client_detail::live_owner(st->core);
                if (owner == nullptr) {
                    return rpc::rpc_result<Resp>(
                        rpc_error(rpc_code::shutting_down, "client is closed"));
                }
                // Presence is read under `inner_mutex` so a concurrent cancel()
                // (which takes the same lock to reach the low-level handle) has
                // a happens-before edge on the pointer. The lock is released
                // before wait_ready(): cancel() must be able to reach the
                // handle while this thread waits for it.
                bool has_inner = false;
                {
                    std::lock_guard<std::mutex> lk(st->inner_mutex);
                    has_inner = st->inner != nullptr;
                }
                if (has_inner) {
                    const status waited = owner->wait_ready(st->core);
                    client* const current = client_detail::live_owner(st->core);
                    if (current == nullptr) {
                        // close() finalized and detached the attempt while we
                        // waited; `inner` is reset here (before the retained
                        // connection slot is dropped) so the low-level handle
                        // never outlives its call-slot pool.
                        release();
                        return rpc::rpc_result<Resp>(
                            rpc_error(rpc_code::shutting_down, "client is closed"));
                    }
                    if (!waited.ok()) {
                        rpc_error failure(rpc_code::deadline_exceeded,
                                          "call did not complete in time");
                        if (waited.code() == rpc_code::failed_precondition) {
                            failure = waited.error();
                        }
                        // A locally observed timeout is a dependency failure,
                        // even though a server-produced `deadline_exceeded`
                        // application result is ignored by the code table.
                        const client_detail::failure_origin origin =
                            client_detail::failure_origin::timeout;
                        {
                            std::lock_guard<std::mutex> lk(st->inner_mutex);
                            st->inner.reset();
                        }
                        current->finish_attempt(st->core, failure.code, origin,
                                                client_detail::breaker_record::failure);
                        return rpc::rpc_result<Resp>(std::move(failure));
                    }
                    rpc::rpc_result<Resp> result(
                        rpc_error(rpc_code::internal, "call produced no result"));
                    rpc::call<Resp>* inner = nullptr;
                    {
                        std::lock_guard<std::mutex> lk(st->inner_mutex);
                        inner = st->inner.get();
                    }
                    if (inner == nullptr) {
                        return rpc::rpc_result<Resp>(
                            rpc_error(rpc_code::shutting_down, "client is closed"));
                    }
                    const bool decoded = inner->try_get(result);
                    const rpc_code code = inner->code();
                    rpc_error terminal_error;
                    if (!(decoded && code == rpc_code::ok)) {
                        terminal_error = result.error();
                    }
                    const client_detail::failure_origin origin =
                        client_detail::result_origin(terminal_error);
                    {
                        std::lock_guard<std::mutex> lk(st->inner_mutex);
                        st->inner.reset();
                    }
                    std::chrono::nanoseconds delay{0};
                    const bool retry =
                        !(decoded && code == rpc_code::ok) &&
                        current->should_retry(st->core, terminal_error, origin, delay);
                    // One logical call feeds the breaker once: a retried
                    // attempt is ignored and only the terminal result counts.
                    current->finish_attempt(
                        st->core, code, origin,
                        retry ? client_detail::breaker_record::ignore
                              : client_detail::breaker_record_for(origin, code));
                    if (decoded && code == rpc_code::ok) {
                        return result;
                    }
                    if (!retry) {
                        return result;
                    }
                    current->sleep_for(st->core, delay);
                    if (st->core.canceled.load(std::memory_order_acquire)) {
                        // This attempt produced no fallback value: it ended
                        // before the next attempt was armed, so the flag from a
                        // previous attempt must not survive into a `canceled`
                        // result (same rule as begin_attempt).
                        st->core.clear_degraded();
                        return rpc::rpc_result<Resp>(
                            rpc_error(rpc_code::canceled, "call canceled"));
                    }
                    st->core.reset_for_attempt();
                    continue;
                }

                if (st->core.canceled.load(std::memory_order_acquire)) {
                    st->core.clear_degraded();
                    return rpc::rpc_result<Resp>(
                        rpc_error(rpc_code::canceled, "call canceled"));
                }

                rpc_error failure;
                const status issued = owner->issue(st, failure);
                if (issued.ok()) {
                    continue;
                }
                const client_detail::failure_origin origin = st->core.origin;
                std::chrono::nanoseconds delay{0};
                const bool retry =
                    owner->should_retry(st->core, failure, origin, delay);
                owner->finish_attempt(
                    st->core, failure.code, origin,
                    retry ? client_detail::breaker_record::ignore
                          : client_detail::breaker_record_for(origin, failure.code));
                if (!retry) {
                    return rpc::rpc_result<Resp>(std::move(failure));
                }
                owner->sleep_for(st->core, delay);
                if (st->core.canceled.load(std::memory_order_acquire)) {
                    st->core.clear_degraded();
                    return rpc::rpc_result<Resp>(
                        rpc_error(rpc_code::canceled, "call canceled"));
                }
                // The failed issue finalized this attempt (and dropped its
                // permit); re-arm it for the next one, as the decoded path
                // above does. Without this the next issue sees `finalized`
                // and reports shutting_down instead of retrying.
                st->core.reset_for_attempt();
            }
        }

        [[nodiscard]] bool try_get(rpc::rpc_result<Resp>& out) {
            if (state_ == nullptr ||
                !state_->core.done.load(std::memory_order_acquire)) {
                return false;
            }
            // Snapshot the handle under `inner_mutex`: a concurrent cancel() may
            // be using it at the same time, and only the owning thread ever
            // resets it (from get()/try_get()/release()), so the pointer stays
            // valid for the duration of this call.
            rpc::call<Resp>* inner = nullptr;
            {
                std::lock_guard<std::mutex> lk(state_->inner_mutex);
                inner = state_->inner.get();
            }
            if (inner == nullptr) {
                return false;
            }
            const bool decoded = inner->try_get(out);
            if (decoded) {
                // The handle has delivered its terminal result: record it once
                // and release the timer and slot.
                const rpc_code code = inner->code();
                rpc_error terminal_error;
                if (code != rpc_code::ok) {
                    terminal_error = out.error();
                }
                const client_detail::failure_origin origin =
                    client_detail::result_origin(terminal_error);
                std::shared_ptr<client_detail::call_state<Resp>> st = std::move(state_);
                {
                    std::lock_guard<std::mutex> lk(st->inner_mutex);
                    st->inner.reset();
                }
                client* const owner = client_detail::live_owner(st->core);
                if (owner != nullptr) {
                    owner->finish_attempt(
                        st->core, code, origin,
                        client_detail::breaker_record_for(origin, code));
                }
            }
            return decoded;
        }

        // Cancels the in-flight attempt. Idempotent.
        void cancel() noexcept {
            if (state_ == nullptr) {
                return;
            }
            // Set the flag even when there is no low-level inner call: during a
            // retry backoff there is nothing to cancel at the transport, but
            // the waiter must stop.
            state_->core.canceled.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lk(state_->inner_mutex);
                if (state_->inner) {
                    state_->inner->cancel();
                }
            }
            client* const owner = client_detail::live_owner(state_->core);
            if (owner != nullptr) {
                // Release the attempt's timer and slot now; `inner` is kept so
                // a subsequent get()/try_get() can still observe `canceled`.
                owner->abandon_attempt(state_->core);
                owner->notify_waiter();
            }
            {
                std::lock_guard<std::mutex> lk(state_->core.mutex);
            }
            state_->core.cv.notify_all();
        }

    private:
        friend class crpc::client;

        // Ends the attempt if one is active: drops the inner low-level call
        // (which cancels it when still pending) and abandons the facade-level
        // attempt. Leaves the handle ready for destruction.
        void release() noexcept {
            if (state_ == nullptr) {
                return;
            }
            std::shared_ptr<client_detail::call_state<Resp>> st = std::move(state_);
            {
                std::lock_guard<std::mutex> lk(st->inner_mutex);
                if (st->inner) {
                    // Resetting `inner` runs the low-level handle destructor,
                    // which detaches (and synchronizes with) any in-flight
                    // callback under the connection lock before the call_state
                    // can be freed.
                    st->inner.reset();
                }
            }
            client* const owner = client_detail::live_owner(st->core);
            if (owner != nullptr) {
                owner->abandon_attempt(st->core);
            }
        }

        std::shared_ptr<client_detail::call_state<Resp>> state_;
    };

    // Typed stub: encodes Req, returns call<Resp>.
    template <typename Resp, typename Req>
    class [[nodiscard]] stub {
    public:
        stub(client& owner, u64 service_id, u64 method_id) noexcept
            : owner_(&owner), service_id_(service_id), method_id_(method_id) {}

        [[nodiscard]] rpc::rpc_result<client::call<Resp>> call(
            const Req& request, rpc::call_context ctx = {}) {
            buffer encoded;
            const status encoded_status = rpc::call_codec<Req>::encode(request, encoded);
            if (!encoded_status.ok()) {
                return rpc::rpc_result<client::call<Resp>>(encoded_status.error());
            }
            return owner_->template start_encoded<Resp>(service_id_, method_id_,
                                                        encoded.readable(), std::move(ctx));
        }

    private:
        client* owner_;
        u64 service_id_;
        u64 method_id_;
    };

    template <typename Resp, typename Req>
    [[nodiscard]] stub<Resp, Req> service(u64 service_id, u64 method_id) {
        return stub<Resp, Req>(*this, service_id, method_id);
    }

    ~client();
    client(const client&) = delete;
    client& operator=(const client&) = delete;
    client(client&&) = delete;
    client& operator=(client&&) = delete;

    // Closes every pooled connection and cancels in-flight calls.
    void close() noexcept;

    [[nodiscard]] bool closed() const noexcept;
    [[nodiscard]] statistics stats() const noexcept;
    [[nodiscard]] resilience_statistics resilience_stats() const noexcept;
    // Additive fallback counters. The primitives are not wired in this slice,
    // so every field is zero; the accessor is the MINOR ABI addition.
    [[nodiscard]] resilience::fallback_statistics fallback_stats() const noexcept;

    // Low-level path used by `stub`. Encodes are the caller's responsibility.
    template <typename Resp>
    [[nodiscard]] rpc::rpc_result<client::call<Resp>> start_encoded(
        u64 service_id, u64 method_id, span<const std::byte> request_body,
        rpc::call_context ctx = {}) {
        if (closed_.load(std::memory_order_acquire)) {
            return rpc::rpc_result<client::call<Resp>>(
                rpc_error(rpc_code::shutting_down, "client is closed"));
        }
        auto st = std::make_shared<client_detail::call_state<Resp>>();
        client_detail::call_core& core = st->core;
        core.owner.store(this, std::memory_order_release);
        core.owner_liveness = liveness_token();
        core.service_id = service_id;
        core.method_id = method_id;
        if (!core.request.append(request_body)) {
            return rpc::rpc_result<client::call<Resp>>(rpc_error(rpc_code::resource_exhausted));
        }
        normalize_context(core, ctx);
        core.retry = std::make_unique<resilience::retry_sequence>(options_.retry, &budget_);
        budget_.on_request();

        // The first attempt may fail before anything was sent (all candidates
        // open, connect refused, pool exhausted). Those failures are safe to
        // retry even for a non-idempotent call. Remote failures cannot happen
        // here because a successful issue returns a handle immediately.
        for (;;) {
            if (closed_.load(std::memory_order_acquire)) {
                return rpc::rpc_result<call<Resp>>(
                    rpc_error(rpc_code::shutting_down, "client is closed"));
            }
            if (core.canceled.load(std::memory_order_acquire)) {
                // A canceled result is never degraded. The state is fresh
                // here and every attempt clears the flag in begin_attempt,
                // so this is belt-and-braces: it keeps the invariant local
                // to every canceled return.
                core.clear_degraded();
                return rpc::rpc_result<call<Resp>>(
                    rpc_error(rpc_code::canceled, "call canceled"));
            }
            rpc_error failure;
            const status issued = issue(st, failure);
            if (issued.ok()) {
                break;
            }
            const client_detail::failure_origin origin = core.origin;
            std::chrono::nanoseconds delay{0};
            const bool retry = should_retry(core, failure, origin, delay);
            finish_attempt(core, failure.code, origin,
                           retry ? client_detail::breaker_record::ignore
                                 : client_detail::breaker_record_for(origin,
                                                                     failure.code));
            if (!retry) {
                return rpc::rpc_result<call<Resp>>(std::move(failure));
            }
            sleep_for(core, delay);
            if (core.canceled.load(std::memory_order_acquire)) {
                core.clear_degraded();
                return rpc::rpc_result<call<Resp>>(
                    rpc_error(rpc_code::canceled, "call canceled"));
            }
            core.reset_for_attempt();
        }
        call<Resp> handle;
        handle.state_ = std::move(st);
        return rpc::rpc_result<client::call<Resp>>(std::move(handle));
    }

    struct impl;

private:
    friend class builder;
    friend struct client_detail::call_core;
    client(options opt, std::vector<endpoint_config> endpoints);

    template <typename Resp>
    status issue(const std::shared_ptr<client_detail::call_state<Resp>>& st,
                 rpc_error& failure);

    void normalize_context(client_detail::call_core& core, const rpc::call_context& ctx);
    bool begin_attempt(client_detail::call_core& core,
                      const std::shared_ptr<client_detail::call_state_base>& keep,
                      rpc_error& failure);
    std::shared_ptr<client_detail::connection_slot> acquire_slot(
        client_detail::call_core& core, rpc_error& failure);
    void release_slot(const std::shared_ptr<client_detail::connection_slot>& slot,
                      rpc_code code) noexcept;
    void finish_attempt(client_detail::call_core& core, rpc_code code,
                        client_detail::failure_origin origin,
                        client_detail::breaker_record record) noexcept;
    // Idempotent counterpart to finish_attempt for paths that end an attempt
    // without a result (handle destruction, try_get, cancel, close).
    void abandon_attempt(client_detail::call_core& core) noexcept;
    // Atomic ("closed" check, insert) pair under `impl_->calls_mutex` (R1).
    [[nodiscard]] bool register_core_if_open(
        client_detail::call_core& core,
        const std::shared_ptr<client_detail::call_state_base>& keep) noexcept;
    void unregister_core(client_detail::call_core& core) noexcept;
    void finalize_active(
        std::vector<std::shared_ptr<client_detail::call_state_base>>& active) noexcept;
    [[nodiscard]] bool should_retry(client_detail::call_core& core,
                                    const rpc_error& error,
                                    client_detail::failure_origin origin,
                                    std::chrono::nanoseconds& delay) noexcept;
    [[nodiscard]] std::shared_ptr<client_detail::client_liveness>
    liveness_token() const noexcept;
    [[nodiscard]] status wait_ready(client_detail::call_core& core) const;
    void sleep_for(client_detail::call_core& core, std::chrono::nanoseconds delay) noexcept;
    void notify_waiter() noexcept;
    rpc::call_id make_call_id() noexcept;
    static void deadline_tick(const std::shared_ptr<impl>& impl,
                              std::chrono::nanoseconds idle_eviction) noexcept;
    static void evict_idle(const std::shared_ptr<impl>& impl,
                           std::chrono::nanoseconds idle_eviction) noexcept;
    static void deadline_thunk(void* ctx) noexcept;

    // A deadline timer runs on the shard thread. Its context is the shared
    // gate below, not `this`: the runtime keeps the gate alive across the
    // wheel-pop/callback window, and the callback never dereferences the
    // client. `timer_mutex_` guards the pointer; the gate guards its own
    // state.
    struct timer_gate;
    std::mutex timer_mutex_;
    std::shared_ptr<timer_gate> timer_gate_;

    options options_;
    std::vector<endpoint_config> endpoints_;
    std::shared_ptr<impl> impl_;
    std::atomic<bool> closed_{false};
    std::atomic<std::uint64_t> seq_{0};
    std::array<std::byte, 16> client_id_{};
    resilience::retry_budget budget_;
    std::atomic<std::size_t> calls_started_{0};
    std::atomic<std::size_t> retries_{0};
    std::atomic<std::size_t> breaker_rejections_{0};
    std::atomic<std::size_t> rate_limited_{0};
    std::atomic<std::size_t> bulkhead_rejected_{0};
    std::atomic<std::size_t> local_gate_rejected_{0};
    std::atomic<std::size_t> endpoint_open_{0};
    std::atomic<std::size_t> lb_unavailable_{0};
    std::atomic<std::size_t> pre_send_retries_{0};
    std::atomic<std::size_t> probe_admitted_{0};
    std::atomic<std::size_t> retries_delayed_by_hint_{0};
};

// --- Template implementations ------------------------------------------------

template <typename Resp>
status client::issue(
    const std::shared_ptr<client_detail::call_state<Resp>>& st,
    rpc_error& failure) {
    client_detail::call_core& core = st->core;
    // The registration keeps `st` alive until the attempt is finalized, so
    // close() can walk its snapshot without racing this handle's destructor.
    if (!begin_attempt(core, st, failure)) {
        return status{failure};
    }
    // On every failure below the attempt is left registered with its permit
    // held; the caller (get() or start_encoded()) decides whether it is a
    // retried attempt (ignore) or the terminal result (record once).
    std::shared_ptr<client_detail::connection_slot> slot = acquire_slot(core, failure);
    if (!slot) {
        return status{failure};
    }
    // close() may have finalized the attempt between the registration and this
    // point. finalize_active() then saw a null `core.slot` and could not
    // release it, so release the freshly acquired slot here and do not start a
    // call against a closed client.
    {
        // Publish the slot under the same lock the terminal paths use, and
        // re-check `finalized` inside it: cancel()/close() may have run between
        // the check above and this assignment, in which case publishing here
        // would leak the slot (the terminal path already took its snapshot).
        std::lock_guard<std::mutex> lk(core.attempt_mutex);
        if (core.finalized.load(std::memory_order_acquire)) {
            release_slot(slot, rpc_code::shutting_down);
            core.origin = client_detail::failure_origin::local_lifecycle;
            failure = rpc_error(rpc_code::shutting_down, "client is closed");
            return status{failure};
        }
        core.slot = slot;
    }

    std::lock_guard<std::recursive_mutex> lk(slot->mutex);
    if (!slot->healthy.load(std::memory_order_acquire) || !slot->conn ||
        !slot->conn->is_open()) {
        core.origin = client_detail::failure_origin::endpoint_pre_send;
        rpc_error unavailable(rpc_code::unavailable, "connection is not usable");
        failure = unavailable;
        return status{failure};
    }
    rpc::rpc_result<rpc::call<Resp>> started = slot->conn->template start_call<Resp>(
        core.service_id, core.method_id, core.request.readable(), core.ctx);
    if (!started.ok()) {
        core.origin = started.error().code == rpc_code::resource_exhausted
                          ? client_detail::failure_origin::local_capacity
                          : client_detail::failure_origin::endpoint_pre_send;
        failure = started.error();
        return status{failure};
    }
    {
        // Publish the handle and arm its completion callback under the same lock
        // that cancel() takes to reach it. `rpc::call` documents single-owner
        // use: `then()` writes callback_storage/invoke/owner while a concurrent
        // cancel() -> invoke_callback() reads them, so both sides must be
        // serialized. `then()` may run the callback synchronously when the call
        // already terminated; the wake functor only touches the call core and
        // never re-enters `inner_mutex`, so holding the lock here is safe.
        std::lock_guard<std::mutex> inner_lk(st->inner_mutex);
        st->inner = std::make_unique<rpc::call<Resp>>(std::move(started).value());
        st->inner->then(client_detail::wake_functor<Resp>{&core});
    }
    return status{};
}

} // namespace crpc

#endif // CRPC_CLIENT_FACADE_HPP
