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
// A retry happens only when the failure code is `is_retryable`, the call is
// declared idempotent (or carries an idempotency key), the shared retry budget
// admits it, and the overall deadline has not passed. Every attempt re-selects
// an endpoint through the load balancer and re-admits through the breaker. The
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
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <crpc/buffer.hpp>
#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/frame.hpp>
#include <crpc/io.hpp>
#include <crpc/resilience/circuit_breaker.hpp>
#include <crpc/resilience/load_balancer.hpp>
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
    rpc_error error;
    std::uint32_t attempts = 0;
    bool idempotent = false;
    bool has_deadline = false;
    std::chrono::steady_clock::time_point deadline{};
    void* pool = nullptr;
    std::shared_ptr<connection_slot> slot;
    resilience::breaker_permit permit;
    std::unique_ptr<resilience::retry_sequence> retry;

    void signal() noexcept;
    void reset_for_attempt() noexcept {
        done.store(false, std::memory_order_release);
        finalized.store(false, std::memory_order_release);
        terminal.store(rpc_code::ok, std::memory_order_release);
    }
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

    class builder {
    public:
        builder& add_endpoint(endpoint ep, backend_factory& backend);
        builder& pool_size(std::size_t value);
        builder& slot_capacity(std::size_t value);
        builder& max_inflight_per_connection(std::size_t value);
        builder& load_balancer(resilience::lb_strategy strategy);
        builder& breaker(resilience::breaker_config config);
        builder& retry(resilience::retry_policy policy);
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
                if (st->inner != nullptr) {
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
                        // The attempt is over: cancel the timer and release the
                        // connection slot before returning without a finalize.
                        release();
                        return rpc::rpc_result<Resp>(std::move(failure));
                    }
                    rpc::rpc_result<Resp> result(
                        rpc_error(rpc_code::internal, "call produced no result"));
                    const bool decoded = st->inner->try_get(result);
                    const rpc_code code = st->inner->code();
                    st->inner.reset();
                    current->finish_attempt(st->core, code);
                    if (decoded && code == rpc_code::ok) {
                        return result;
                    }
                    std::chrono::nanoseconds delay{0};
                    if (!current->should_retry(st->core, code, delay)) {
                        return result;
                    }
                    current->sleep_for(st->core, delay);
                    st->core.reset_for_attempt();
                    continue;
                }

                rpc_error failure;
                const status issued = owner->issue(st, failure);
                if (issued.ok()) {
                    continue;
                }
                std::chrono::nanoseconds delay{0};
                if (!owner->should_retry(st->core, failure.code, delay)) {
                    return rpc::rpc_result<Resp>(std::move(failure));
                }
                owner->sleep_for(st->core, delay);
            }
        }

        [[nodiscard]] bool try_get(rpc::rpc_result<Resp>& out) {
            if (state_ == nullptr || state_->inner == nullptr ||
                !state_->core.done.load(std::memory_order_acquire)) {
                return false;
            }
            const bool decoded = state_->inner->try_get(out);
            if (decoded) {
                // The handle has delivered its terminal result: finalize the
                // attempt so the timer and slot are released.
                release();
            }
            return decoded;
        }

        // Cancels the in-flight attempt. Idempotent.
        void cancel() noexcept {
            if (state_ != nullptr && state_->inner) {
                state_->inner->cancel();
                // Release the attempt's timer and slot now; `inner` is kept so
                // a subsequent get()/try_get() can still observe `canceled`.
                client* const owner = client_detail::live_owner(state_->core);
                if (owner != nullptr) {
                    owner->abandon_attempt(state_->core);
                }
            }
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
            if (st->inner) {
                // Resetting `inner` runs the low-level handle destructor, which
                // detaches (and synchronizes with) any in-flight callback under
                // the connection lock before the call_state can be freed.
                st->inner.reset();
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

        rpc_error failure;
        const status issued = issue(st, failure);
        if (!issued.ok()) {
            return rpc::rpc_result<client::call<Resp>>(std::move(failure));
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
    void finish_attempt(client_detail::call_core& core, rpc_code code) noexcept;
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
    [[nodiscard]] bool should_retry(client_detail::call_core& core, rpc_code code,
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
    std::shared_ptr<client_detail::connection_slot> slot = acquire_slot(core, failure);
    if (!slot) {
        finish_attempt(core, failure.code);
        return status{failure};
    }
    // close() may have finalized the attempt between the registration and this
    // point. finalize_active() then saw a null `core.slot` and could not
    // release it, so release the freshly acquired slot here and do not start a
    // call against a closed client.
    if (core.finalized.load(std::memory_order_acquire)) {
        release_slot(slot, rpc_code::shutting_down);
        failure = rpc_error(rpc_code::shutting_down, "client is closed");
        return status{failure};
    }
    core.slot = slot;

    std::lock_guard<std::recursive_mutex> lk(slot->mutex);
    if (!slot->healthy.load(std::memory_order_acquire) || !slot->conn ||
        !slot->conn->is_open()) {
        rpc_error unavailable(rpc_code::unavailable, "connection is not usable");
        failure = unavailable;
        finish_attempt(core, unavailable.code);
        return status{failure};
    }
    rpc::rpc_result<rpc::call<Resp>> started = slot->conn->template start_call<Resp>(
        core.service_id, core.method_id, core.request.readable(), core.ctx);
    if (!started.ok()) {
        failure = started.error();
        finish_attempt(core, failure.code);
        return status{failure};
    }
    st->inner = std::make_unique<rpc::call<Resp>>(std::move(started).value());
    st->inner->then(client_detail::wake_functor<Resp>{&core});
    return status{};
}

} // namespace crpc

#endif // CRPC_CLIENT_FACADE_HPP
