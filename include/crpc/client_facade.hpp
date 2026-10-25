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
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// `operator co_await` below needs `std::coroutine_handle`; only include the
// header (and declare the operator) when the compiler is in C++20 mode and
// `<coroutine>` is available. `__cpp_coroutines` is deliberately not used as
// the test: MSVC (19.x) does not define it even under `/std:c++20` (it defines
// `__cpp_impl_coroutine` instead), which would silently disable the operator on
// the project's primary compiler. `__has_include(<coroutine>)` is the portable
// form of the "and `<coroutine>` is available" condition.
#if __cplusplus >= 202002L && __has_include(<coroutine>)
#include <coroutine>
#endif

#include <crpc/buffer.hpp>
#include <crpc/config.hpp>
#include <crpc/diagnostics.hpp>
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

// Fixed-capacity open-addressing hash map with linear probing, used for the
// client's active-call registry. The registry is written on every attempt, so a
// `std::unordered_map` there put one system allocation on the call path; this
// container allocates its table once and grows only while the in-flight set is
// growing, so a steady-state call reaches the allocator nowhere.
template <typename Key, typename Value>
class flat_pool_map {
private:
    struct entry {
        Key first{};
        Value second{};
        bool used = false;
    };

public:
    flat_pool_map() { init(16); }
    ~flat_pool_map() = default;
    flat_pool_map(const flat_pool_map&) = delete;
    flat_pool_map& operator=(const flat_pool_map&) = delete;

    void init(std::size_t capacity) {
        std::size_t size = 2;
        while (size < capacity * 2) {
            size <<= 1;
        }
        capacity_ = size;
        entries_.reset(new entry[capacity_]());
        size_ = 0;
    }

    class iterator {
    public:
        iterator() = default;
        iterator(flat_pool_map* owner, std::size_t index) : owner_(owner), index_(index) {}
        entry& operator*() const { return owner_->entries_[index_]; }
        entry* operator->() const { return &owner_->entries_[index_]; }
        iterator& operator++() {
            index_ = owner_->next_used(index_ + 1);
            return *this;
        }
        bool operator==(const iterator& other) const {
            return owner_ == other.owner_ && index_ == other.index_;
        }
        bool operator!=(const iterator& other) const { return !(*this == other); }

    private:
        friend class flat_pool_map;
        flat_pool_map* owner_ = nullptr;
        std::size_t index_ = 0;
    };

    [[nodiscard]] Value* find(const Key& key) {
        if (capacity_ == 0) {
            return nullptr;
        }
        std::size_t index = hash_key(key) & (capacity_ - 1);
        for (std::size_t probe = 0; probe < capacity_; ++probe) {
            if (!entries_[index].used) {
                return nullptr;
            }
            if (entries_[index].first == key) {
                return &entries_[index].second;
            }
            index = (index + 1) & (capacity_ - 1);
        }
        return nullptr;
    }

    void emplace(const Key& key, Value value) {
        if (size_ + 1 > capacity_ * 3 / 4) {
            grow();
        }
        std::size_t index = hash_key(key) & (capacity_ - 1);
        for (;;) {
            if (!entries_[index].used) {
                entries_[index].used = true;
                entries_[index].first = key;
                entries_[index].second = std::move(value);
                ++size_;
                return;
            }
            if (entries_[index].first == key) {
                entries_[index].second = std::move(value);
                return;
            }
            index = (index + 1) & (capacity_ - 1);
        }
    }

    void erase(const Key& key) {
        if (capacity_ == 0) {
            return;
        }
        std::size_t index = hash_key(key) & (capacity_ - 1);
        for (std::size_t probe = 0; probe < capacity_; ++probe) {
            if (!entries_[index].used) {
                return;
            }
            if (entries_[index].first == key) {
                entries_[index].second = Value{};
                entries_[index].used = false;
                --size_;
                // Backward-shift deletion, as in `rpc::stream_map`: scan
                // forward and pull forward every entry whose ideal slot is at
                // or before the hole.
                std::size_t i = index;
                std::size_t j = index;
                for (;;) {
                    j = (j + 1) & (capacity_ - 1);
                    if (!entries_[j].used) {
                        break;
                    }
                    const std::size_t ideal = hash_key(entries_[j].first) & (capacity_ - 1);
                    if (cyclically_between(ideal, i, j)) {
                        entries_[i] = entries_[j];
                        entries_[j].second = Value{};
                        entries_[j].used = false;
                        i = j;
                    }
                }
                entries_[i].second = Value{};
                entries_[i].used = false;
                return;
            }
            index = (index + 1) & (capacity_ - 1);
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    void clear() noexcept {
        if (capacity_ == 0) {
            return;
        }
        for (std::size_t i = 0; i < capacity_; ++i) {
            entries_[i].second = Value{};
            entries_[i].used = false;
        }
        size_ = 0;
    }

    [[nodiscard]] iterator begin() { return iterator(this, next_used(0)); }
    [[nodiscard]] iterator end() { return iterator(this, capacity_); }

private:
    static std::size_t hash_key(const Key& key) {
        return std::hash<Key>{}(key);
    }

    // True when the hole `i` lies in the entry's probe chain, i.e. in the
    // cyclic half-open range `[ideal, j)` for the entry at `j`.
    static bool cyclically_between(std::size_t ideal, std::size_t i, std::size_t j) {
        if (ideal <= j) {
            return ideal <= i && i < j;
        }
        return i >= ideal || i < j;
    }

    void grow() {
        const std::size_t new_capacity = capacity_ == 0 ? 16 : capacity_ * 2;
        std::unique_ptr<entry[]> fresh(new entry[new_capacity]());
        for (std::size_t i = 0; i < capacity_; ++i) {
            if (!entries_[i].used) {
                continue;
            }
            std::size_t index = hash_key(entries_[i].first) & (new_capacity - 1);
            while (fresh[index].used) {
                index = (index + 1) & (new_capacity - 1);
            }
            fresh[index] = entries_[i];
        }
        entries_ = std::move(fresh);
        capacity_ = new_capacity;
    }

    [[nodiscard]] std::size_t next_used(std::size_t from) const {
        for (std::size_t i = from; i < capacity_; ++i) {
            if (entries_[i].used) {
                return i;
            }
        }
        return capacity_;
    }

    std::unique_ptr<entry[]> entries_;
    std::size_t capacity_ = 0;
    std::size_t size_ = 0;
};

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
//
// The enum moved to `error.hpp` (the core error header) so that `rpc_error` can
// carry it without a dependency cycle; this alias keeps the existing
// `client_detail::failure_origin` spelling valid.
using failure_origin = crpc::failure_origin;

// Maps an origin and the terminal code to the single breaker outcome.
[[nodiscard]] CRPC_API breaker_record breaker_record_for(failure_origin origin,
                                                rpc_code code) noexcept;

// Classifies a completed low-level call. A local timeout carries the
// `crpc.local_timeout` metadata marker; a peer deadline does not.
[[nodiscard]] CRPC_API failure_origin result_origin(const rpc_error& error) noexcept;

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

// Invokes a `then` callback armed on a `call_core`. `owner` is the
// `client::call<Resp>*` on which `then` was called. Mirrors the low-level
// `call_callback_invoker` but for the facade call state.
struct call_core;
using call_core_callback_invoker = void (*)(void* storage, void* owner,
                                            call_core& core) noexcept;

// Type-independent per-call state.
struct CRPC_API call_core {
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
    // Set only by the paths that hand a terminal result to the caller
    // (`get()`/`try_get()`, close, cancel). Unlike `done`, which `signal()` sets
    // on every attempt completion, this stays false while a retry backoff is in
    // progress, so `cancel()` can still interrupt it. `cancel()` treats it as
    // "already terminal" and leaves the recorded outcome untouched.
    std::atomic<bool> completed{false};
    std::atomic<rpc_code> terminal{rpc_code::ok};
    // Set by `record_terminal` once the terminal outcome is published. `ready()`
    // reports `done` (the low-level call completed); `terminal_ready()` reports
    // this, which becomes true only when a consumer (`get()`/`try_get()`)
    // processes the attempt. `signal()` sets `done` before the terminal
    // snapshot exists, so `ready()` alone does not imply the accessors are
    // readable — use `terminal_ready()` for that.
    std::atomic<bool> terminal_recorded{false};
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
    // Per-call retry state machine. Non-owning: the `retry_sequence` lives
    // inline in `call_state<Resp>::retry_value`, so a call no longer needs a
    // second per-call `make_unique`. Null until `start_encoded` emplaces the
    // sequence from the client's policy and shared budget. A raw pointer keeps
    // `call_core`'s layout identical to the `unique_ptr` it replaces.
    resilience::retry_sequence* retry = nullptr;

    void signal() noexcept;
    // Invokes the armed `then` callback exactly once, on the thread that
    // terminates the call. Safe to call more than once; the callback runs at
    // most once. Public so the terminal paths (close/cancel) can reach it.
    void invoke_callback() noexcept {
        if (callback_armed.exchange(false, std::memory_order_acq_rel)) {
            call_core_callback_invoker invoke = callback_invoke;
            void* owner_ptr = callback_owner;
            callback_invoke = nullptr;
            callback_owner = nullptr;
            if (invoke != nullptr) {
                invoke(callback_storage.bytes, owner_ptr, *this);
            }
        }
    }
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
        terminal_recorded.store(false, std::memory_order_release);
    }
    // Publishes the terminal outcome for the non-blocking `code()`/`origin()`/
    // `error()` accessors. `terminal` is atomic; `error`/`origin` are guarded by
    // `mutex` (the accessors take the same lock). The terminal transition that
    // wins the attempt's `finalized` exchange is the only writer, so the
    // snapshot a reader observes is the terminal one, not an intermediate
    // attempt's.
    void record_terminal(rpc_code code, rpc_error err,
                         failure_origin org) noexcept {
        terminal.store(code, std::memory_order_release);
        std::lock_guard<std::mutex> lk(mutex);
        this->error = std::move(err);
        this->origin = org;
        terminal_recorded.store(true, std::memory_order_release);
    }

    // --- `then` callback (inline, heap-free) ---
    rpc::call_callback_storage callback_storage{};
    call_core_callback_invoker callback_invoke = nullptr;
    void* callback_owner = nullptr;
    // Claimed exactly once by the completion path or by `then()` when the call
    // already terminated. Atomic so a handle destructor can disarm it without a
    // race against a backend-thread invocation.
    std::atomic<bool> callback_armed{false};

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

// The pool is forward-declared here because `call_state` needs to hand its
// request buffer back to it (see the constructor/destructor below).
class call_state_pool;

template <typename Resp>
struct call_state final : call_state_base {
    call_core core;
    // Inline per-call retry state machine. `core.retry` points here once
    // `start_encoded` emplaces it, so no per-call `make_unique<retry_sequence>`
    // remains. `core`'s destructor does not touch the pointer, so the member
    // order only has to keep the sequence inside this object.
    std::optional<resilience::retry_sequence> retry_value;
    // Guards the `inner` handle between the call owner and a concurrent
    // cancel() from another thread. The handle has its own synchronization;
    // this mutex only protects its lifetime.
    std::mutex inner_mutex;
    // The low-level handle lives inline in the pooled block, so a call no
    // longer allocates it with `make_unique` (P3b). An empty optional means
    // no low-level call is in flight (between attempts, or after the terminal
    // result was taken).
    std::optional<rpc::call<Resp>> inner;
    // The terminal outcome once `get()` has produced it. `get()` is idempotent:
    // a repeat call returns this cached result instead of re-entering `issue()`,
    // which would refuse with `shutting_down` because the winning attempt left
    // `finalized == true` and the retry path never ran `reset_for_attempt()`.
    // Written only on a terminal hand-off, read on every later `get()`.
    std::optional<rpc::rpc_result<Resp>> terminal_result;

    // `pool` is the call-state pool this block belongs to. When set, the
    // constructor takes the request buffer's storage from the pool and the
    // destructor hands it back, so a call's request buffer is reused across
    // calls instead of being freed and reallocated on every append. The default
    // argument keeps the pool-free construction used by the pool's own test.
    explicit call_state(call_state_pool* pool = nullptr);
    ~call_state();

    [[nodiscard]] call_core& core_ref() noexcept override { return core; }

private:
    call_state_pool* pool_ = nullptr;
};

// --- per-call storage pool ---------------------------------------------------

// Process-wide counters for the facade call-state pool. The pool itself is
// private to each client, but the regression test and the benchmark need to
// observe whether a call reached the system allocator; these counters are that
// observation point. They are never read on the call path.
struct call_state_pool_counters {
    std::atomic<std::size_t> acquire{0};
    std::atomic<std::size_t> reuse{0};
    std::atomic<std::size_t> system_alloc{0};
};
inline call_state_pool_counters g_call_state_pool_counters{};

// Type-erased pool of storage blocks for `call_state` objects. Every block is
// the same size, because `call_state<Resp>` is the same width for every `Resp`
// (`inner` is an inline `std::optional<rpc::call<Resp>>`, not a `Resp`). Blocks
// are handed to `std::allocate_shared` through `call_state_allocator`, so the
// object and its control block share one pooled block and no per-call heap
// allocation remains for the call state or the low-level handle it carries.
//
// The pool is owned by the client's `impl`, but the allocator stored in each
// control block holds a `shared_ptr` to it, so a block that outlives the client
// (a handle held after close()) still has a live pool to return to.
class call_state_pool {
public:
    call_state_pool() { free_buffers_.reserve(kMaxFreeBuffers); }
    call_state_pool(const call_state_pool&) = delete;
    call_state_pool& operator=(const call_state_pool&) = delete;

    ~call_state_pool() {
        // Every block is returned before the pool dies (each control block
        // holds a `shared_ptr`), so the free list holds them all.
        for (void* block : free_) {
            ::operator delete(block);
        }
    }

    void* acquire(std::size_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        g_call_state_pool_counters.acquire.fetch_add(1, std::memory_order_relaxed);
        if (bytes == block_bytes_ && !free_.empty()) {
            void* block = free_.back();
            free_.pop_back();
            ++live_;
            g_call_state_pool_counters.reuse.fetch_add(1, std::memory_order_relaxed);
            return block;
        }
        if (block_bytes_ == 0) {
            block_bytes_ = bytes;
        }
        ++live_;
        g_call_state_pool_counters.system_alloc.fetch_add(1, std::memory_order_relaxed);
        return ::operator new(bytes);
    }

    void release(void* block, std::size_t bytes) noexcept {
        if (block == nullptr) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        --live_;
        if (bytes == block_bytes_) {
            free_.push_back(block);
        } else {
            ::operator delete(block);
        }
    }

    // Request-buffer storage. A `call_state` block is reconstructed on every
    // use, so its `buffer` member would otherwise start empty and grow (allocate)
    // on the first append of each call. Taking the storage from the pool on
    // construction and returning it on destruction keeps the capacity across
    // calls, so the append never reaches the allocator after warm-up.
    [[nodiscard]] buffer acquire_request_buffer() {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        if (!free_buffers_.empty()) {
            buffer result = std::move(free_buffers_.back());
            free_buffers_.pop_back();
            // The pooled buffer keeps its capacity but must not keep its
            // contents: the next call starts from an empty request.
            result.clear();
            return result;
        }
        return buffer{};
    }

    void release_request_buffer(buffer&& value) noexcept {
        if (value.capacity() == 0) {
            return;
        }
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        if (free_buffers_.size() >= kMaxFreeBuffers) {
            return;
        }
        try {
            free_buffers_.push_back(std::move(value));
        } catch (...) {
            // The buffer is dropped; its storage is freed by its destructor.
        }
    }

private:
    static constexpr std::size_t kMaxFreeBuffers = 64;
    mutable std::mutex mutex_;
    std::vector<void*> free_;
    std::size_t block_bytes_ = 0;
    std::size_t live_ = 0;
    std::mutex buffer_mutex_;
    std::vector<buffer> free_buffers_;
};

// Defined here, after `call_state_pool` is complete, because the bodies call its
// request-buffer methods.
template <typename Resp>
call_state<Resp>::call_state(call_state_pool* pool) : pool_(pool) {
    if (pool_ != nullptr) {
        core.request = pool_->acquire_request_buffer();
    }
}

template <typename Resp>
call_state<Resp>::~call_state() {
    if (pool_ != nullptr) {
        pool_->release_request_buffer(std::move(core.request));
    }
}

// Allocator adapter that draws `std::allocate_shared`'s combined object and
// control block from a `call_state_pool`. `allocate_shared` rebinds this to its
// control-block type, so `sizeof(value_type)` is exactly the block size the
// pool hands out, and the deleter returns the block to the same pool.
template <typename T>
struct call_state_allocator {
    using value_type = T;

    call_state_allocator() = default;
    explicit call_state_allocator(std::shared_ptr<call_state_pool> pool)
        : pool_(std::move(pool)) {}

    template <typename U>
    call_state_allocator(const call_state_allocator<U>& other) : pool_(other.pool_) {}

    T* allocate(std::size_t count) {
        return static_cast<T*>(pool_->acquire(count * sizeof(T)));
    }

    void deallocate(T* block, std::size_t count) {
        pool_->release(block, count * sizeof(T));
    }

    template <typename U>
    bool operator==(const call_state_allocator<U>& other) const noexcept {
        return pool_ == other.pool_;
    }
    template <typename U>
    bool operator!=(const call_state_allocator<U>& other) const noexcept {
        return !(*this == other);
    }

    std::shared_ptr<call_state_pool> pool_;
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

template <typename Resp>
class response;

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

    class CRPC_API builder {
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

        // Deprecated aliases for the canonical names above. They exist so a
        // caller that used the earlier spelling keeps compiling; prefer
        // `default_deadline` and `max_inflight_per_connection`.
        [[deprecated("use builder::default_deadline")]]
        builder& default_timeout(std::chrono::nanoseconds value);
        [[deprecated("use builder::max_inflight_per_connection")]]
        builder& per_connection_inflight(std::size_t value);

        [[nodiscard]] std::unique_ptr<client> build();

    private:
        options options_{};
        std::vector<endpoint_config> endpoints_;
    };

    template <typename Resp>
    class [[nodiscard]] call {
    public:
        call() noexcept = default;
        call(call&& other) noexcept : state_(std::move(other.state_)) {
            rebind_owner(&other);
        }
        call& operator=(call&& other) noexcept {
            if (this != &other) {
                // Releasing a still-active old state_ must abandon its attempt
                // (cancel timer, release slot) before taking over the new one.
                release();
                state_ = std::move(other.state_);
                rebind_owner(&other);
            }
            return *this;
        }
        call(const call&) = delete;
        call& operator=(const call&) = delete;
        ~call() { release(); }

        [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }
        // True once the low-level call has completed (`signal()` sets `done`).
        // The non-blocking accessors (`code()`/`origin()`/`error()`) become
        // meaningful only after a consumer processes the attempt (`get()`/
        // `try_get()` records the terminal snapshot); before that they hold
        // defaults. `terminal_ready()` reports the stricter condition.
        [[nodiscard]] bool ready() const noexcept {
            return state_ != nullptr && state_->core.done.load(std::memory_order_acquire);
        }
        [[nodiscard]] bool terminal_ready() const noexcept {
            return state_ != nullptr && state_->core.terminal_recorded.load(std::memory_order_acquire);
        }
        [[nodiscard]] std::uint32_t attempts() const noexcept {
            return state_ != nullptr ? state_->core.attempts : 0;
        }

        // Non-blocking read of the terminal outcome. `code()` reads the atomic
        // terminal code; `origin()`/`error()` read the snapshot the terminal
        // transition recorded under `core.mutex`. An invalid handle answers the
        // `invalid_argument` sentinel pair rather than dereferencing null.
        //
        // A fallback that answered the call is a success from the caller's point
        // of view: `get()` returns the fallback value, so `code()` must report
        // `ok` rather than the underlying failure that triggered the fallback
        // (`degraded()` distinguishes it). Otherwise `code()` and
        // `get().code()` disagree for a degraded call.
        [[nodiscard]] rpc_code code() const noexcept {
            if (state_ == nullptr) {
                return rpc_code::invalid_argument;
            }
            if (state_->core.degraded()) {
                return rpc_code::ok;
            }
            return state_->core.terminal.load(std::memory_order_acquire);
        }

        [[nodiscard]] client_detail::failure_origin origin() const noexcept {
            if (state_ == nullptr) {
                return client_detail::failure_origin::local_lifecycle;
            }
            std::lock_guard<std::mutex> lk(state_->core.mutex);
            return state_->core.origin;
        }

        [[nodiscard]] rpc_error error() const noexcept {
            if (state_ == nullptr) {
                return rpc_error(rpc_code::invalid_argument, "invalid call handle");
            }
            std::lock_guard<std::mutex> lk(state_->core.mutex);
            return state_->core.error;
        }

        // True when the terminal result was produced by a fallback function.
        // Only a successful decode of the fallback value sets it, so `applied`
        // with a body that does not decode leaves it false. It is meaningful
        // only after get() returned; try_get() moves the state out and does not
        // run a fallback at all, so a later call reads false.
        [[nodiscard]] bool degraded() const noexcept {
            return state_ != nullptr && state_->core.degraded();
        }

        // Suspends (or blocks) until a terminal result is available. Returns
        // the decoded value, or the last error when retries are exhausted.
        [[nodiscard]] rpc::rpc_result<Resp> get() {
            if (state_ == nullptr) {
                // An invalid handle is a programming error (`invalid_argument`),
                // deliberately distinct from a wait that failed because the
                // caller is on a shard thread outside a coroutine, which is
                // reported as `failed_precondition` below. Both used to answer
                // `failed_precondition`.
                return rpc::rpc_result<Resp>(
                    rpc_error(rpc_code::invalid_argument, "invalid call handle"));
            }
            // Idempotent: once a terminal outcome has been produced, return the
            // cached copy. Without this a second `get()` re-enters `issue()`,
            // whose `finalized` guard refuses with `shutting_down` (the winning
            // attempt never ran `reset_for_attempt()`), so the caller sees a
            // different result than the first call.
            if (state_->terminal_result.has_value()) {
                return *state_->terminal_result;
            }
            if (client_detail::live_owner(state_->core) == nullptr) {
                state_->core.record_terminal(
                    rpc_code::failed_precondition,
                    rpc_error(rpc_code::failed_precondition, "invalid call handle"),
                    client_detail::failure_origin::local_lifecycle);
                state_->terminal_result = rpc::rpc_result<Resp>(
                    rpc_error(rpc_code::failed_precondition, "invalid call handle"));
                return *state_->terminal_result;
            }
            std::shared_ptr<client_detail::call_state<Resp>> st = state_;
            // Every return below is terminal. The guard runs the `then` callback
            // exactly once on the thread that observes the terminal result, so a
            // callback fires even for a terminal reached locally (a pre-send
            // refusal or a local timeout) where no backend thread rang the
            // completion path. `invoke_callback` is idempotent, so a callback
            // already run by `signal()` is not run twice.
            //
            // The guard holds the `shared_ptr`, not a raw pointer into the state:
            // a terminal path may call `release()`, which moves `state_` out and
            // can drop the last reference to the call state. A raw `&core` would
            // then dangle when the guard runs.
            //
            // `result` carries the terminal outcome so the guard can publish it
            // into `terminal_result` *before* the `then` callback runs: the
            // callback may call `get()` again, and it must observe the cached
            // result rather than re-entering `issue()`.
            struct terminal_guard {
                std::shared_ptr<client_detail::call_state<Resp>> st;
                std::optional<rpc::rpc_result<Resp>> result;
                ~terminal_guard() {
                    if (st) {
                        if (result.has_value()) {
                            st->terminal_result = *result;
                        }
                        // Mark the call terminal for `ready()`/`try_get()` and
                        // run the `then` callback exactly once. `completed`
                        // distinguishes this terminal hand-off from the
                        // per-attempt `done` so a later cancel() is a no-op.
                        st->core.done.store(true, std::memory_order_release);
                        st->core.completed.store(true, std::memory_order_release);
                        st->core.invoke_callback();
                    }
                }
            } guard{st};
            // Publishes a terminal outcome into the guard (and, through it, the
            // call state) and returns it. `Resp` is copied into the cache only
            // when it is copyable; a move-only `Resp` still returns by value.
            auto finish = [&guard](rpc::rpc_result<Resp> r) -> rpc::rpc_result<Resp> {
                if constexpr (std::is_copy_constructible_v<Resp>) {
                    guard.result = r;
                }
                return r;
            };
            // Records the terminal outcome for the non-blocking accessors on the
            // exits that do not pass through `finish_attempt` (a dead owner, a
            // shutdown during the wait, and the cancel returns).
            auto record = [&st](rpc_code code, rpc_error error,
                                client_detail::failure_origin origin) noexcept {
                st->core.record_terminal(code, std::move(error), origin);
            };
            for (;;) {
                // Re-read the owner every turn: close()/``~client`` detaches it
                // in finalize_active(), so a cached pointer would be stale (R1
                // follow-up). A detached attempt reports `shutting_down`
                // delivered by close() rather than touching the freed client.
                client* const owner = client_detail::live_owner(st->core);
                if (owner == nullptr) {
                    record(rpc_code::shutting_down,
                           rpc_error(rpc_code::shutting_down, "client is closed"),
                           client_detail::failure_origin::local_lifecycle);
                    return finish(rpc::rpc_result<Resp>(
                        rpc_error(rpc_code::shutting_down, "client is closed")));
                }
                // Presence is read under `inner_mutex` so a concurrent cancel()
                // (which takes the same lock to reach the low-level handle) has
                // a happens-before edge on the pointer. The lock is released
                // before wait_ready(): cancel() must be able to reach the
                // handle while this thread waits for it.
                bool has_inner = false;
                {
                    std::lock_guard<std::mutex> lk(st->inner_mutex);
                    has_inner = st->inner.has_value();
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
                        record(rpc_code::shutting_down,
                               rpc_error(rpc_code::shutting_down, "client is closed"),
                               client_detail::failure_origin::local_lifecycle);
                        return finish(rpc::rpc_result<Resp>(
                            rpc_error(rpc_code::shutting_down, "client is closed")));
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
                        current->finish_attempt(st->core, failure.code, failure,
                                                origin,
                                                client_detail::breaker_record::failure);
                        // A locally observed timeout has no admission stage, so
                        // this counts as `ineligible` and the original error is
                        // returned unchanged. Calling it keeps every terminal
                        // failure on one counting path.
                        return finish(current->fallback_for<Resp>(
                            st->core, std::move(failure), origin));
                    }
                    rpc::rpc_result<Resp> result(
                        rpc_error(rpc_code::internal, "call produced no result"));
                    rpc::call<Resp>* inner = nullptr;
                    {
                        std::lock_guard<std::mutex> lk(st->inner_mutex);
                        inner = &*st->inner;
                    }
                    if (inner == nullptr) {
                        record(rpc_code::shutting_down,
                               rpc_error(rpc_code::shutting_down, "client is closed"),
                               client_detail::failure_origin::local_lifecycle);
                        return finish(rpc::rpc_result<Resp>(
                            rpc_error(rpc_code::shutting_down, "client is closed")));
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
                        st->core, code, terminal_error, origin,
                        retry ? client_detail::breaker_record::ignore
                              : client_detail::breaker_record_for(origin, code));
                    if (decoded && code == rpc_code::ok) {
                        return finish(result);
                    }
                    if (!retry) {
                        // Last attempt failed: this is the one place a fallback
                        // may answer, and it runs after finish_attempt() so the
                        // breaker already saw the real outcome.
                        return finish(current->fallback_for<Resp>(
                            st->core, std::move(terminal_error), origin));
                    }
                    current->sleep_for(st->core, delay);
                    if (st->core.canceled.load(std::memory_order_acquire)) {
                        // This attempt produced no fallback value: it ended
                        // before the next attempt was armed, so the flag from a
                        // previous attempt must not survive into a `canceled`
                        // result (same rule as begin_attempt).
                        st->core.clear_degraded();
                        record(rpc_code::canceled,
                               rpc_error(rpc_code::canceled, "call canceled"),
                               client_detail::failure_origin::cancel);
                        return finish(rpc::rpc_result<Resp>(
                            rpc_error(rpc_code::canceled, "call canceled")));
                    }
                    st->core.reset_for_attempt();
                    continue;
                }

                if (st->core.canceled.load(std::memory_order_acquire)) {
                    st->core.clear_degraded();
                    record(rpc_code::canceled,
                           rpc_error(rpc_code::canceled, "call canceled"),
                           client_detail::failure_origin::cancel);
                    return finish(rpc::rpc_result<Resp>(
                        rpc_error(rpc_code::canceled, "call canceled")));
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
                    st->core, failure.code, failure, origin,
                    retry ? client_detail::breaker_record::ignore
                          : client_detail::breaker_record_for(origin, failure.code));
                if (!retry) {
                    // Last attempt failed before the peer answered; a fallback
                    // may still answer for a pre-send refusal.
                    return finish(owner->fallback_for<Resp>(st->core, std::move(failure),
                                                            origin));
                }
                owner->sleep_for(st->core, delay);
                if (st->core.canceled.load(std::memory_order_acquire)) {
                    st->core.clear_degraded();
                    record(rpc_code::canceled,
                           rpc_error(rpc_code::canceled, "call canceled"),
                           client_detail::failure_origin::cancel);
                    return finish(rpc::rpc_result<Resp>(
                        rpc_error(rpc_code::canceled, "call canceled")));
                }
                // The failed issue finalized this attempt (and dropped its
                // permit); re-arm it for the next one, as the decoded path
                // above does. Without this the next issue sees `finalized`
                // and reports shutting_down instead of retrying.
                st->core.reset_for_attempt();
            }
        }

        [[nodiscard]] bool try_get(rpc::rpc_result<Resp>& out) {
            if (state_ == nullptr) {
                return false;
            }
            // Idempotent: a repeat read returns the cached terminal outcome.
            // `try_get` moves `state_` out when it delivers a result, so a
            // second read would otherwise see a null handle and answer false.
            if (state_->terminal_result.has_value()) {
                out = *state_->terminal_result;
                return true;
            }
            if (!state_->core.done.load(std::memory_order_acquire)) {
                return false;
            }
            // Snapshot the handle under `inner_mutex`: a concurrent cancel() may
            // be using it at the same time, and only the owning thread ever
            // resets it (from get()/try_get()/release()), so the pointer stays
            // valid for the duration of this call.
            rpc::call<Resp>* inner = nullptr;
            {
                std::lock_guard<std::mutex> lk(state_->inner_mutex);
                inner = &*state_->inner;
            }
            if (inner == nullptr) {
                return false;
            }
            const bool decoded = inner->try_get(out);
            if (decoded) {
                // Publish the terminal outcome before `state_` is moved out.
                if constexpr (std::is_copy_constructible_v<Resp>) {
                    state_->terminal_result = out;
                }
                // The handle has delivered its terminal result: record it once
                // and release the timer and slot.
                const rpc_code code = inner->code();
                rpc_error terminal_error;
                if (code != rpc_code::ok) {
                    terminal_error = out.error();
                }
                const client_detail::failure_origin origin =
                    client_detail::result_origin(terminal_error);
                // Keep `state_` engaged: `try_get` finalizes the attempt but
                // must leave the handle valid so a `then` body that calls
                // `get()` observes the recorded result instead of the
                // `invalid_argument` sentinel of a null handle.
                std::shared_ptr<client_detail::call_state<Resp>> st = state_;
                {
                    std::lock_guard<std::mutex> lk(st->inner_mutex);
                    st->inner.reset();
                }
                client* const owner = client_detail::live_owner(st->core);
                if (owner != nullptr) {
                    owner->finish_attempt(
                        st->core, code, terminal_error, origin,
                        client_detail::breaker_record_for(origin, code));
                }
                // The call is terminal: run the `then` callback exactly once.
                st->core.invoke_callback();
            }
            return decoded;
        }

        // Cancels the in-flight attempt. Idempotent.
        void cancel() noexcept {
            if (state_ == nullptr) {
                return;
            }
            // A terminal call must not be touched: `get()`'s terminal guard (or
            // close/cancel) set `completed` when the result was handed over, so
            // a cancel after a successful get() is a no-op and cannot turn
            // `code()` from ok into canceled. `done` alone is not enough: it is
            // set on every attempt, including one that is about to be retried.
            if (state_->core.completed.load(std::memory_order_acquire)) {
                return;
            }
            // Set the flag even when there is no low-level inner call: during a
            // retry backoff there is nothing to cancel at the transport, but
            // the waiter must stop.
            state_->core.canceled.store(true, std::memory_order_release);
            // Publish the terminal outcome: `cancel()` is a terminal transition
            // that does not go through `finish_attempt`, so the accessors would
            // otherwise read a stale or reset code.
            state_->core.record_terminal(
                rpc_code::canceled,
                rpc_error(rpc_code::canceled, "call canceled"),
                client_detail::failure_origin::cancel);
            state_->core.completed.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lk(state_->inner_mutex);
                if (state_->inner.has_value()) {
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
            // A `then` callback registered before cancel() still fires: the
            // cancel is a terminal transition and the handle it receives will
            // answer `canceled` from get().
            state_->core.invoke_callback();
        }

        // Registers a completion callback. `fn` is invoked as `fn(call&)` once
        // the call terminates, or immediately on this thread when it has already
        // terminated. The callback must be trivially copyable and fit in
        // `rpc::call_callback_inline_size`; it must not destroy the handle it
        // belongs to.
        //
        // This is the facade-level counterpart of `rpc::call::then`: the
        // callback runs on the thread that terminates the call (the reader, the
        // deadline timer or close/cancel), exactly once.
        template <typename F>
        void then(F&& fn) {
            if (state_ == nullptr) {
                return;
            }
            using fn_type = std::decay_t<F>;
            static_assert(std::is_trivially_copyable_v<fn_type>,
                          "call<Resp>::then callback must be trivially copyable");
            static_assert(sizeof(fn_type) <= rpc::call_callback_inline_size,
                          "call<Resp>::then callback exceeds the inline storage budget");
            static_assert(alignof(fn_type) <= alignof(std::max_align_t),
                          "call<Resp>::then callback has an unsupported alignment");
            client_detail::call_core& core = state_->core;
            std::memcpy(core.callback_storage.bytes,
                        static_cast<const void*>(std::addressof(fn)), sizeof(fn_type));
            core.callback_invoke = &then_thunk<fn_type>;
            core.callback_owner = this;
            // Publish the callback before arming it; the completion path claims
            // it with an acquire exchange. If completion already happened, claim
            // it here instead so the callback runs exactly once.
            core.callback_armed.store(true, std::memory_order_release);
            if (core.done.load(std::memory_order_acquire) &&
                core.callback_armed.exchange(false, std::memory_order_acq_rel)) {
                core.callback_invoke = nullptr;
                core.callback_owner = nullptr;
                fn(*this);
            }
        }

    private:
        friend class crpc::client;

        template <typename F>
        static void then_thunk(void* storage, void* owner,
                               client_detail::call_core&) noexcept {
            F& fn = *static_cast<F*>(storage);
            call& self = *static_cast<call*>(owner);
            fn(self);
        }

        // After a move, a callback armed on the source handle must target the
        // destination, or it would run against a moved-from object.
        void rebind_owner(call* old) noexcept {
            if (state_ != nullptr &&
                state_->core.callback_armed.load(std::memory_order_acquire) &&
                state_->core.callback_owner == old) {
                state_->core.callback_owner = this;
            }
        }

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
                if (st->inner.has_value()) {
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

        // Strongly typed overload: the ids cannot be swapped.
        stub(client& owner, service_id service, method_id method) noexcept
            : owner_(&owner), service_id_(service.value), method_id_(method.value) {}

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

    // Strongly typed overload: the ids cannot be swapped.
    template <typename Resp, typename Req>
    [[nodiscard]] stub<Resp, Req> service(service_id service, method_id method) {
        return stub<Resp, Req>(*this, service, method);
    }

    // Convenience entry: encodes `request`, starts the call and wraps the result
    // in a `response<Resp>`. The encoding path is the same `rpc::call_codec<Req>`
    // that `stub::call` uses. A serialization failure is reported as an invalid
    // response carrying the encode error. Defined after `response<Resp>` at the
    // end of the header because it names the complete wrapper type.
    template <typename Resp, typename Req>
    [[nodiscard]] response<Resp> request(service_id service, method_id method,
                                         const Req& request,
                                         rpc::call_context ctx = {});

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
    // Additive fallback counters. They are fed by `run_fallback` once per
    // fallback attempt, from a stack tally, so a concurrent reader never sees a
    // half-applied batch. The accessor is a MINOR ABI addition.
    [[nodiscard]] resilience::fallback_statistics fallback_stats() const noexcept;

    // Installs a fallback. `retain`/`release` keep `self` alive for as long as
    // this client owns the policy, so a non-null `self` is rejected without
    // them: an unowned `self` would dangle and terminate inside the noexcept
    // fallback path. Both callbacks must be non-null together or both null, and
    // both are `noexcept` so no exception can escape the destructor path; the
    // type system enforces that (a function that may throw does not convert).
    // Never `reinterpret_cast` a throwing function into these types.
    //
    // `retain` runs once per installed policy, outside every library lock, and
    // `release` runs once when that policy is replaced or the client is
    // destroyed. A `retain` that returns null means "the reference could not be
    // taken": the call is rejected and nothing is installed.
    //
    // `min_remaining` is the G1 floor ("is the degraded value still
    // deliverable"). Pass `std::chrono::nanoseconds(resilience::min_remaining_ns)`
    // to match the server's admission floor; do not introduce a second literal.
    //
    // A rejected call installs nothing, so fallback_stats() stays all-zero and a
    // later call reports its original error, not a degraded value. Passing a
    // null `function` uninstalls the current policy.
    [[nodiscard]] bool set_fallback(resilience::fallback_fn function, void* self,
                                    std::chrono::nanoseconds min_remaining,
                                    void* (*retain)(void*) noexcept,
                                    void (*release)(void*) noexcept) noexcept;

    // High-level RAII form of `set_fallback`. `self` is owned by a `shared_ptr`,
    // so the caller no longer supplies a matching `retain`/`release` pair: the
    // shared_ptr is released when the policy is replaced or the client is
    // destroyed. Passing a null `function` uninstalls the current policy.
    //
    // This is the form the `fallback_install` guard uses. The low-level
    // five-argument `set_fallback` stays for a caller that already owns `self`
    // and wants to keep the explicit retain/release contract.
    [[nodiscard]] bool set_fallback_owned(resilience::fallback_fn function,
                                          std::shared_ptr<void> self,
                                          std::chrono::nanoseconds min_remaining) noexcept;

    // Adds one fallback tally to the client counters. `run_fallback` calls it
    // once per attempt; it is public so a future header-side consumer can flush
    // its own tally without a second counting path.
    void record_fallbacks(const resilience::fallback_statistics& tally) noexcept;

    // Runs the installed fallback for one terminal failure. Returns
    // `not_configured` when no policy is installed (the fast path, no lock).
    // `error` may be rewritten by G3; `out` receives the encoded value on
    // `applied`. The caller decodes `out` and only then marks the call
    // degraded: `applied` does not promise that the body decodes.
    [[nodiscard]] resilience::fallback_outcome run_fallback(
        client_detail::call_core& core, buffer& out, rpc_error& error,
        resilience::fallback_counter_sink* sink) noexcept;

    // Low-level path used by `stub`. Encodes are the caller's responsibility.
    template <typename Resp>
    [[nodiscard]] rpc::rpc_result<client::call<Resp>> start_encoded(
        u64 service_id, u64 method_id, span<const std::byte> request_body,
        rpc::call_context ctx = {}) {
        if (closed_.load(std::memory_order_acquire)) {
            return rpc::rpc_result<client::call<Resp>>(
                rpc_error(rpc_code::shutting_down, "client is closed"));
        }
        // The object and its shared_ptr control block come from the client's
        // pool, so a call does not reach the system allocator (the pool grows
        // only until it has seen the first call).
        client_detail::call_state_allocator<client_detail::call_state<Resp>> alloc(
            call_state_pool());
        auto st = std::allocate_shared<client_detail::call_state<Resp>>(alloc,
                                                                       call_state_pool().get());
        client_detail::call_core& core = st->core;
        core.owner.store(this, std::memory_order_release);
        core.owner_liveness = liveness_token();
        core.service_id = service_id;
        core.method_id = method_id;
        if (!core.request.append(request_body)) {
            return rpc::rpc_result<client::call<Resp>>(rpc_error(rpc_code::resource_exhausted));
        }
        normalize_context(core, ctx);
        core.retry = &st->retry_value.emplace(options_.retry, &budget_);
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
            finish_attempt(core, failure.code, failure, origin,
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

    // Strongly typed overload of the low-level path used by `stub`.
    template <typename Resp>
    [[nodiscard]] rpc::rpc_result<client::call<Resp>> start_encoded(
        service_id service, method_id method, span<const std::byte> request_body,
        rpc::call_context ctx = {}) {
        return start_encoded<Resp>(service.value, method.value, request_body,
                                   std::move(ctx));
    }

    struct impl;

private:
    friend class builder;
    friend struct client_detail::call_core;
    client(options opt, std::vector<endpoint_config> endpoints);

    template <typename Resp>
    status issue(const std::shared_ptr<client_detail::call_state<Resp>>& st,
                 rpc_error& failure);

    // The pool `start_encoded` draws `call_state` storage from. Defined out of
    // line because `impl` is incomplete here.
    [[nodiscard]] std::shared_ptr<client_detail::call_state_pool> call_state_pool()
        const noexcept;

    void normalize_context(client_detail::call_core& core, const rpc::call_context& ctx);
    bool begin_attempt(client_detail::call_core& core,
                      const std::shared_ptr<client_detail::call_state_base>& keep,
                      rpc_error& failure);
    std::shared_ptr<client_detail::connection_slot> acquire_slot(
        client_detail::call_core& core, rpc_error& failure);
    void release_slot(const std::shared_ptr<client_detail::connection_slot>& slot,
                      rpc_code code) noexcept;
    void finish_attempt(client_detail::call_core& core, rpc_code code,
                        rpc_error error, client_detail::failure_origin origin,
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
    // Runs the installed fallback for a terminal failure and, when the value
    // decodes, reports it as the call's result. Used by the two terminal
    // failure exits of get(); it is never used before the last attempt.
    template <typename Resp>
    rpc::rpc_result<Resp> fallback_for(
        client_detail::call_core& core, rpc_error error,
        client_detail::failure_origin origin);
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

// RAII scoped fallback installer.
//
// The low-level `client::set_fallback` takes five arguments and a matching
// `retain`/`release` pair: the caller must promise that `self` outlives the
// policy, and a forgotten `release` leaks the reference while a forgotten
// `retain` leaves a dangling pointer that the noexcept fallback path would
// dereference. This guard removes that contract: `self` is owned by a
// `shared_ptr`, and destroying the guard uninstalls the policy.
//
//   crpc::client::fallback_install fb(cli, &my_fallback,
//                                     std::shared_ptr<void>(&state, [](void*){}),
//                                     50ms);
//   // the fallback applies here; at the end of the scope it is gone
//
// The guard must be destroyed before the client it was installed on. It is
// move-only and non-copyable, like the call handle.
class CRPC_API fallback_install {
public:
    fallback_install() noexcept = default;

    fallback_install(crpc::client& owner, resilience::fallback_fn function,
                     std::shared_ptr<void> self,
                     std::chrono::nanoseconds min_remaining) noexcept
        : owner_(&owner) {
        static_cast<void>(
            owner_->set_fallback_owned(function, std::move(self), min_remaining));
    }

    ~fallback_install() { reset(); }

    fallback_install(fallback_install&& other) noexcept : owner_(other.owner_) {
        other.owner_ = nullptr;
    }

    fallback_install& operator=(fallback_install&& other) noexcept {
        if (this != &other) {
            reset();
            owner_ = other.owner_;
            other.owner_ = nullptr;
        }
        return *this;
    }

    fallback_install(const fallback_install&) = delete;
    fallback_install& operator=(const fallback_install&) = delete;

    // Uninstalls the policy this guard installed. Idempotent and safe to call
    // more than once.
    void reset() noexcept {
        if (owner_ != nullptr) {
            static_cast<void>(owner_->set_fallback(
                nullptr, nullptr, std::chrono::nanoseconds{0}, nullptr, nullptr));
            owner_ = nullptr;
        }
    }

    [[nodiscard]] bool active() const noexcept { return owner_ != nullptr; }

private:
    crpc::client* owner_ = nullptr;
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
        st->inner.emplace(std::move(started).value());
        st->inner->then(client_detail::wake_functor<Resp>{&core});
    }
    return status{};
}

template <typename Resp>
rpc::rpc_result<Resp> client::fallback_for(
    client_detail::call_core& core, rpc_error error,
    client_detail::failure_origin origin) {
    // The stage inference reads the origin, so publish the terminal origin
    // first. `run_fallback` is a library member: the policy and the counters
    // never leave the library.
    core.origin = origin;
    buffer body;
    const resilience::fallback_outcome outcome =
        run_fallback(core, body, error, nullptr);
    if (outcome == resilience::fallback_outcome::applied) {
        Resp value{};
        // `applied` only promises "not late", not "decodable": mark the call
        // degraded only after the value decoded (fallback.hpp contract).
        if (rpc::call_codec<Resp>::decode(body.readable(), value).ok()) {
            core.mark_degraded();
            return rpc::rpc_result<Resp>(std::move(value));
        }
    }
    // `overrun` rewrote `error` to deadline_exceeded; every other outcome
    // leaves it untouched. Either way the caller answers with the original
    // failure unless a value was applied.
    return rpc::rpc_result<Resp>(std::move(error));
}

namespace client_detail {
// True for the value types the composition helpers produce: a single
// `rpc_result` (`when_any`) or a vector of them (`when_all`). Such a type is
// never a wire-decodable call value, so a `response` holding one is always a
// computed result; the call-handle path (`client::call<Resp>::get`, which
// requires a default-constructible, codec-decodable `Resp`) must not be
// instantiated for it.
template <typename T>
struct is_composition_value : std::false_type {};
template <typename T>
struct is_composition_value<rpc::rpc_result<T>> : std::true_type {};
template <typename T>
struct is_composition_value<std::vector<rpc::rpc_result<T>>> : std::true_type {};

template <typename T>
inline constexpr bool is_composition_value_v = is_composition_value<T>::value;
} // namespace client_detail

// Type-erased terminal callback. `ctx` is the caller-supplied cookie; `error`
// is the terminal outcome (empty on success). The typed value is not part of
// the erased interface, so a callback registered through `response_base`
// receives only the terminal error; a caller that needs the value names
// `response<Resp>`.
using response_callback = void (*)(void* ctx, rpc_error error) noexcept;

// Type-erased view of a `response<Resp>`, for code that must observe a response
// without naming its value type. It exposes the diagnostics plus a
// type-erased consuming half: `get()`/`try_get()`/`then()`.
//
// The type-erased consuming methods are named `get`/`try_get`/`then` to match
// the typed API, but they cannot be virtual with the typed `get()`'s return
// type (`rpc_result<Resp>`), which the base does not know. They are therefore
// non-virtual forwarders over the private virtual `*_erased` hooks; the hooks
// carry the erased signature and are implemented by `response<Resp>`.
class response_base {
public:
    virtual ~response_base() = default;
    [[nodiscard]] virtual bool valid() const noexcept = 0;
    [[nodiscard]] virtual bool ready() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t attempts() const noexcept = 0;
    [[nodiscard]] virtual bool degraded() const noexcept = 0;
    [[nodiscard]] virtual rpc_code code() const noexcept = 0;
    [[nodiscard]] virtual client_detail::failure_origin origin() const noexcept = 0;
    [[nodiscard]] virtual rpc_error error() const noexcept = 0;
    virtual void cancel() noexcept = 0;

    // Type-erased consumption. A `response_base&` can consume a response
    // without naming `Resp`: `get()` waits and returns the terminal error
    // (empty on success), `try_get()` is the non-blocking form, and `then()`
    // registers a callback that fires once with the terminal error. The typed
    // value is not part of the erased interface; a caller that needs it names
    // `response<Resp>`.
    [[nodiscard]] rpc_error get() { return get_erased(); }
    [[nodiscard]] bool try_get(rpc_error& out) { return try_get_erased(out); }
    void then(response_callback cb, void* ctx) { then_erased(cb, ctx); }

private:
    [[nodiscard]] virtual rpc_error get_erased() = 0;
    [[nodiscard]] virtual bool try_get_erased(rpc_error& out) = 0;
    virtual void then_erased(response_callback cb, void* ctx) = 0;
};

// Unified result wrapper over the `rpc::rpc_result<client::call<Resp>>` that a
// call start returns. A start failure (the result holds an `rpc_error`) answers
// from the error; a started call answers from the handle.
template <typename Resp>
class [[nodiscard]] response final : public response_base {
public:
    // Non-explicit so a `rpc_result<client::call<Resp>>` (what `stub::call` and
    // `start_encoded` still return) converts implicitly to a `response<Resp>`:
    // `response<Resp> r = stub.call(...);` compiles without a cast (spec:96).
    response(rpc::rpc_result<client::call<Resp>> result)
        : result_(std::move(result)) {}

    // A response that already holds a terminal value rather than a call handle.
    // The composition helpers (`when_all`/`when_any`) build their result from
    // other responses, so there is no handle left to wait on. `get()` then
    // returns the stored result instead of delegating to a call.
    explicit response(rpc::rpc_result<Resp> result)
        : computed_(std::move(result)) {}

    // An invalid response carrying a start failure. `rpc::rpc_result` has no
    // default constructor, so this is the way to build the "nothing started"
    // value. A composition value type cannot back a started call, so for those
    // the failure is already terminal and is stored directly (computed mode),
    // which makes `ready()` agree with `code()`.
    [[nodiscard]] static response error(rpc_error err) {
        if constexpr (client_detail::is_composition_value_v<Resp>) {
            return response(rpc::rpc_result<Resp>(std::move(err)));
        } else {
            return response(rpc::rpc_result<client::call<Resp>>(std::move(err)));
        }
    }

    [[nodiscard]] bool valid() const noexcept override {
        if (computed_) {
            return computed_->ok();
        }
        return result_->ok() && result_->value().valid();
    }
    // Follows the terminal snapshot, so `ready()` agrees with `code()`: the
    // accessors are meaningful only once a consumer recorded the outcome. A
    // computed response is terminal by construction. A start failure has no
    // handle but is terminal, so it is ready too: otherwise `ready()` would
    // disagree with `try_get()`/`get()`, and a caller that gates on `ready()`
    // would never observe the error.
    [[nodiscard]] bool ready() const noexcept override {
        if (computed_) {
            return true;
        }
        if (!result_->ok()) {
            return true;
        }
        return result_->value().terminal_ready();
    }
    [[nodiscard]] std::uint32_t attempts() const noexcept override {
        if (computed_) {
            return 0;
        }
        return result_->ok() ? result_->value().attempts() : 0;
    }
    [[nodiscard]] bool degraded() const noexcept override {
        if (computed_) {
            return false;
        }
        return result_->ok() && result_->value().degraded();
    }
    [[nodiscard]] rpc_code code() const noexcept override {
        if (computed_) {
            return computed_->code();
        }
        return result_->ok() ? result_->value().code() : result_->error().code;
    }
    [[nodiscard]] client_detail::failure_origin origin() const noexcept override {
        if (computed_) {
            return computed_->ok() ? client_detail::failure_origin::remote
                                   : client_detail::result_origin(computed_->error());
        }
        return result_->ok() ? result_->value().origin()
                             : client_detail::result_origin(result_->error());
    }
    [[nodiscard]] rpc_error error() const noexcept override {
        if (computed_) {
            return computed_->ok() ? rpc_error(rpc_code::ok) : computed_->error();
        }
        if (!result_->ok()) {
            return result_->error();
        }
        // A fallback-answered call is a success: report the same `ok` that
        // `code()` reports instead of the refusal that triggered the fallback.
        if (result_->value().degraded()) {
            return rpc_error(rpc_code::ok);
        }
        return result_->value().error();
    }
    void cancel() noexcept override {
        if (computed_) {
            return;
        }
        if (result_->ok()) {
            result_->value().cancel();
        }
    }

    // Registers a completion callback that receives the terminal result. The
    // callback is `void(rpc_result<Resp>)` (spec:73), runs on the thread that
    // terminates the call (or immediately on this thread when it already
    // terminated), exactly once, and is delivered for a start failure and for a
    // computed response too (both are terminal, so both invoke immediately).
    // Returns `*this` so callers can chain.
    template <typename F>
    [[nodiscard]] response& then(F&& fn) {
        static_assert(
            std::is_invocable_v<std::decay_t<F>, rpc::rpc_result<Resp>>,
            "response<Resp>::then callback must be callable as void(rpc_result<Resp>)");
        if (computed_) {
            fn(*computed_);
            return *this;
        }
        if (!result_->ok()) {
            fn(rpc::rpc_result<Resp>(result_->error()));
            return *this;
        }
        if constexpr (!client_detail::is_composition_value_v<Resp>) {
            result_->value().then(
                [fn = std::forward<F>(fn)](client::call<Resp>& c) mutable {
                    // The callback runs once the call is terminal, so `get()`
                    // returns immediately with the recorded result instead of
                    // re-entering `issue()`.
                    fn(c.get());
                });
        }
        return *this;
    }

    // Suspends (or blocks) until a terminal result is available and returns the
    // decoded value, or the terminal error. A start failure answers directly
    // from the wrapped error. `wait()` and `park()` are aliases. Idempotent: the
    // first terminal result is cached and a repeat call returns it without
    // touching the underlying handle (spec:69).
    //
    // The first call that observes a terminal outcome also notifies the
    // process-wide `diagnostics::on_completion` hook, on every path (computed
    // value, start failure, completed call). A repeat read of the same response
    // is not a new completion and does not notify again.
    [[nodiscard]] rpc::rpc_result<Resp> get() {
        if (computed_) {
            notify_completion(*computed_);
            return *computed_;
        }
        if (cached_) {
            notify_completion(*cached_);
            return *cached_;
        }
        rpc::rpc_result<Resp> result_value = read_result();
        cache(result_value);
        notify_completion(result_value);
        return result_value;
    }

    [[nodiscard]] rpc::rpc_result<Resp> wait() { return get(); }
    [[nodiscard]] rpc::rpc_result<Resp> park() { return get(); }

    // C++20 coroutine integration. `await_ready()` reports `ready()` so an
    // already-terminal response (a composition value, or a call that finished
    // before the await) resumes immediately; an in-flight call suspends the
    // awaiting coroutine and is resumed by the call's `then` callback on the
    // thread that terminates it. `await_resume()` returns `get()`. Absent from
    // a C++17 build, where `co_await` is not a language feature.
#if __cplusplus >= 202002L && __has_include(<coroutine>)
    auto operator co_await() {
        struct awaiter {
            response& r;
            bool await_ready() const noexcept { return r.ready() || !r.valid(); }
            void await_suspend(std::coroutine_handle<> h) {
                // `(void)`: `response::then` is [[nodiscard]], and a discarded
                // return would be a warning promoted to an error under /WX.
                (void)r.then([h](rpc::rpc_result<Resp>) mutable { h.resume(); });
            }
            rpc::rpc_result<Resp> await_resume() { return r.get(); }
        };
        return awaiter{*this};
    }
#endif

    // Non-blocking read. A start failure is already terminal, so it writes the
    // wrapped error and returns true; an in-flight call that has not terminated
    // returns false and leaves `out` untouched; a completed call delegates to
    // the handle's `try_get`. A computed response is always ready. Idempotent:
    // a terminal result is cached and returned on later calls.
    [[nodiscard]] bool try_get(rpc::rpc_result<Resp>& out) {
        if (computed_) {
            out = *computed_;
            return true;
        }
        if (cached_) {
            out = *cached_;
            return true;
        }
        if constexpr (client_detail::is_composition_value_v<Resp>) {
            out = rpc::rpc_result<Resp>(result_->error());
            cache(out);
            return true;
        } else {
            if (!result_->ok()) {
                out = rpc::rpc_result<Resp>(result_->error());
                cache(out);
                return true;
            }
            if (result_->value().try_get(out)) {
                if (!out.ok()) {
                    out = rpc::rpc_result<Resp>(
                        with_call_diagnostics(out.error()));
                }
                cache(out);
                return true;
            }
            return false;
        }
    }

    // Returns the decoded value, or throws the terminal `rpc_error`.
    [[nodiscard]] Resp value() {
        auto r = get();
        if (!r.ok()) {
            throw r.error();
        }
        return std::move(r).value();
    }

    // Returns the decoded value, or `fallback` when the call failed.
    [[nodiscard]] Resp value_or(Resp fallback) {
        auto r = get();
        return r.ok() ? std::move(r).value() : std::move(fallback);
    }

private:
    // Computes the terminal result without default-constructing `rpc_result<Resp>`
    // (which has no default constructor). `get()` caches the value this returns.
    [[nodiscard]] rpc::rpc_result<Resp> read_result() {
        if constexpr (client_detail::is_composition_value_v<Resp>) {
            // A composition value cannot back a started call handle, so an
            // engaged handle here is the start-failure shape built by
            // `error()`: report the wrapped error.
            return rpc::rpc_result<Resp>(result_->error());
        } else {
            if (!result_->ok()) {
                return rpc::rpc_result<Resp>(result_->error());
            }
            rpc::rpc_result<Resp> result = result_->value().get();
            if (!result.ok()) {
                return rpc::rpc_result<Resp>(
                    with_call_diagnostics(result.error()));
            }
            return result;
        }
    }

    // Attaches the call handle's terminal origin/attempts to an error from a
    // started call. The handle keeps those in a separate snapshot from the
    // `rpc_error`, so without this copy the returned error (and the completion
    // hook's record) would carry the `rpc_error` defaults and the classification
    // predicates would be wrong. Start failures and computed results have no
    // live handle and keep the diagnostics they already carry.
    [[nodiscard]] rpc_error with_call_diagnostics(rpc_error error) const {
        if (!computed_ && result_->ok()) {
            error.origin = result_->value().origin();
            error.attempts = result_->value().attempts();
        }
        return error;
    }

    // Records a terminal result for a repeat `get()`/`try_get()`. Skipped for a
    // move-only `Resp`, which cannot be copied into the cache.
    void cache(const rpc::rpc_result<Resp>& value) {
        if constexpr (std::is_copy_constructible_v<Resp>) {
            cached_ = value;
        }
    }

    // Notifies the process-wide completion hook the first time this response
    // observes a terminal outcome. The hook receives `ok` for a success and the
    // terminal error (including a cancel) otherwise. `on_completion` does not
    // hold the hook lock while the callback runs.
    void notify_completion(const rpc::rpc_result<Resp>& value) {
        if (completion_notified_) {
            return;
        }
        completion_notified_ = true;
        diagnostics::on_completion(with_call_diagnostics(
            value.ok() ? rpc_error(rpc_code::ok) : value.error()));
    }

    // Type-erased consuming hooks (`response_base`). `get_erased` returns the
    // terminal error (empty on success); the typed value is not part of the
    // erased interface.
    [[nodiscard]] rpc_error get_erased() override {
        const rpc::rpc_result<Resp> r = get();
        return r.ok() ? rpc_error(rpc_code::ok) : r.error();
    }
    [[nodiscard]] bool try_get_erased(rpc_error& out) override {
        // `rpc_result<Resp>` has no default constructor, so start from a
        // success placeholder; `try_get` overwrites it or leaves it untouched.
        rpc::rpc_result<Resp> value{rpc_error(rpc_code::ok)};
        if (!try_get(value)) {
            return false;
        }
        out = value.ok() ? rpc_error(rpc_code::ok) : value.error();
        return true;
    }
    void then_erased(response_callback cb, void* ctx) override {
        // `(void)`: the return value is unused here, and `then` is [[nodiscard]].
        (void)then([cb, ctx](rpc::rpc_result<Resp> value) {
            cb(ctx, value.ok() ? rpc_error(rpc_code::ok) : value.error());
        });
    }

    // Exactly one of `result_` (a started call handle) and `computed_` (an
    // already-terminal value) is engaged.
    std::optional<rpc::rpc_result<client::call<Resp>>> result_;
    std::optional<rpc::rpc_result<Resp>> computed_;
    // Cached terminal result for idempotent `get()`/`try_get()`.
    std::optional<rpc::rpc_result<Resp>> cached_;
    // Set once the completion hook has been notified for this response.
    bool completion_notified_ = false;
};

// `client::request` encodes `request` with the same `rpc::call_codec<Req>` path
// as `stub::call`, then starts the call and wraps the `rpc_result` in a
// `response<Resp>`. A serialization failure answers with an invalid response
// carrying the encode error. Defined here, after `response<Resp>`, because the
// body names the complete wrapper type.
template <typename Resp, typename Req>
[[nodiscard]] response<Resp> client::request(service_id service, method_id method,
                                             const Req& request,
                                             rpc::call_context ctx) {
    buffer encoded;
    const status encoded_status = rpc::call_codec<Req>::encode(request, encoded);
    if (!encoded_status.ok()) {
        return response<Resp>::error(encoded_status.error());
    }
    auto result = start_encoded<Resp>(service.value, method.value,
                                      encoded.readable(), std::move(ctx));
    return response<Resp>(std::move(result));
}

// Composition helpers over a set of `response<Resp>`. Both return a
// `response` that holds an already-terminal value rather than a call handle,
// so the caller can `get()` the composition result directly.
//
// `when_all` is a sequential wait: it calls `get()` on every response, so it
// blocks until the last one terminates and reports the first failure.
//
// `when_any` is deliberately NON-BLOCKING. It polls each response with
// `try_get()` and returns the first one that is already terminal. If none is
// terminal it returns immediately with a computed error response
// (`failed_precondition`, "no response ready") -- it does NOT wait for a
// completion and does not block the caller. That result is terminal, so
// `ready()` is true. A caller that wants "first completion" semantics must
// drive the poll (or use `when_all`/`get` on the individual responses). This is
// the brief's reference behaviour and is intentionally not a blocking wait.
template <typename Resp>
[[nodiscard]] response<std::vector<rpc::rpc_result<Resp>>> when_all(
    std::vector<response<Resp>>& responses) {
    std::vector<rpc::rpc_result<Resp>> results;
    results.reserve(responses.size());
    rpc_error first_error;
    bool has_error = false;
    for (auto& r : responses) {
        auto res = r.get();
        if (!res.ok() && !has_error) {
            first_error = res.error();
            has_error = true;
        }
        results.push_back(std::move(res));
    }
    if (has_error) {
        return response<std::vector<rpc::rpc_result<Resp>>>::error(first_error);
    }
    return response<std::vector<rpc::rpc_result<Resp>>>(
        rpc::rpc_result<std::vector<rpc::rpc_result<Resp>>>(std::move(results)));
}

template <typename Resp>
[[nodiscard]] response<rpc::rpc_result<Resp>> when_any(
    std::vector<response<Resp>>& responses) {
    for (auto& r : responses) {
        rpc::rpc_result<Resp> out(rpc_code::ok);
        if (r.try_get(out)) {
            return response<rpc::rpc_result<Resp>>(
                rpc::rpc_result<rpc::rpc_result<Resp>>(std::move(out)));
        }
    }
    return response<rpc::rpc_result<Resp>>::error(
        rpc_error(rpc_code::failed_precondition, "no response ready"));
}

} // namespace crpc

#endif // CRPC_CLIENT_FACADE_HPP
