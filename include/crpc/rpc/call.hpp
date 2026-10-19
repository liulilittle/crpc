// CRPC -- call state machine, call context and the typed call handle.
//
// This header is the RPC core's smallest unit of state. It contains four
// pieces:
//
//   * `call_state`      -- the one-word lifecycle of a call;
//   * `call_context`    -- the per-call deadline, trace, idempotency and lane
//                          metadata handed to a client and to a handler;
//   * `call_slot`       -- the pooled, generation-tagged record for one
//                          in-flight call, plus `call_slot_pool`;
//   * `call<T>`         -- the move-only RAII handle returned to a caller.
//
// State machine
// -------------
// A call slot holds a single `std::atomic<u32>` state word:
//
//   idle -> queued -> sent -> (completed | timed_out | canceled | failed)
//
// Only the single-writer client thread performs the non-terminal transitions
// (`idle -> queued -> sent`). The terminal transition may be attempted by any
// of four racing parties: a response, a timeout, a cancel and a connection
// loss. Exactly one of them wins the compare-exchange in `try_claim_terminal`;
// every loser observes a terminal state and must not touch the slot's result
// fields again. `finish_slot` writes the terminal result only after it has won
// the claim, so exactly one thread ever writes `code`, `error` and `response`.
//
// `result_ready` is a second atomic that a reader waits on: it is stored with
// release ordering after the result fields are written, so an acquire load of
// `result_ready` makes those writes visible. `ready()` and `get()` use it; pure
// state-machine users may look at the state word alone.
//
// Thread safety and allocation
// ----------------------------
//   * `call_state` helpers are safe for concurrent use: the only mutating
//     operation is the compare-exchange in `try_claim_terminal`.
//   * `call_slot` is owned by one client at a time. Its result fields are
//     written by exactly one thread (the claim winner) and read by the waiter
//     after `result_ready`. `reset()` is only called when no thread references
//     the slot.
//   * `call_slot_pool` is safe for concurrent acquire/release.
//   * `call<T>` is owned by one thread at a time; it is move-only.
//   * The handle itself never allocates. A slot is taken from the client's
//     preallocated pool and returned on destruction. The response body is
//     stored in the slot's reusable `buffer`; the only allocation on the
//     response path is the buffer's first growth, which is kept for the life of
//     the slot (and released when a large buffer is recycled).
#ifndef CRPC_RPC_CALL_HPP
#define CRPC_RPC_CALL_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <crpc/buffer.hpp>
#include <crpc/config.hpp>
#include <crpc/endian.hpp>
#include <crpc/error.hpp>
#include <crpc/observability/context.hpp>
#include <crpc/span.hpp>

namespace crpc::rpc {

class client;

// The codebase spells the value-or-error type `result<T>`; the design document
// and the RPC API refer to it as `rpc_result<T>`. They are the same type.
template <typename T>
using rpc_result = result<T>;

// --- State machine -----------------------------------------------------------

enum class call_state : u32 {
    // Freshly acquired from the pool; no frame has been written.
    idle = 0,
    // Placed in the writer queue but not yet handed to the sink.
    queued = 1,
    // The request bytes were accepted by the sink.
    sent = 2,
    // A RESPONSE frame arrived (success or application error).
    completed = 3,
    // The deadline elapsed before a response arrived.
    timed_out = 4,
    // The caller or the peer cancelled the call.
    canceled = 5,
    // The connection was lost or a protocol failure occurred.
    failed = 6
};

[[nodiscard]] constexpr bool is_terminal(call_state state) noexcept {
    return state == call_state::completed || state == call_state::timed_out ||
           state == call_state::canceled || state == call_state::failed;
}

// A stable, human-readable name. Never returns nullptr.
[[nodiscard]] CRPC_API const char* to_string(call_state state) noexcept;

// The rpc_code that a terminal state implies when the result carries no more
// specific code.
[[nodiscard]] CRPC_API rpc_code default_terminal_code(call_state state) noexcept;

// --- Identifiers -------------------------------------------------------------

// The idempotency key is the R6 triple `(client_id, client_epoch, seq)`. It is
// assigned by the owning client when a call is created and is reused unchanged
// on every retry, so a server can deduplicate a replayed request.
struct call_id {
    std::array<std::byte, 16> client_id{};
    u64 client_epoch = 0;
    u64 seq = 0;

    [[nodiscard]] bool empty() const noexcept {
        if (client_epoch != 0 || seq != 0) {
            return false;
        }
        for (const std::byte b : client_id) {
            if (b != std::byte{0}) {
                return false;
            }
        }
        return true;
    }

    // Canonical 32-byte wire form: 16-byte client id, then client epoch and seq
    // as little-endian u64.
    [[nodiscard]] std::array<std::byte, 32> encode() const noexcept {
        std::array<std::byte, 32> out{};
        for (std::size_t i = 0; i < client_id.size(); ++i) {
            out[i] = client_id[i];
        }
        endian::store_le64(out.data() + 16, client_epoch);
        endian::store_le64(out.data() + 24, seq);
        return out;
    }

    // Accepts any key length; a short key is zero-extended, which keeps a
    // malformed peer from producing an out-of-range read.
    [[nodiscard]] static call_id decode(span<const std::byte> bytes) noexcept {
        call_id value;
        for (std::size_t i = 0; i < bytes.size() && i < value.client_id.size(); ++i) {
            value.client_id[i] = bytes[i];
        }
        if (bytes.size() >= 24) {
            value.client_epoch = endian::load_le64(bytes.data() + 16);
        }
        if (bytes.size() >= 32) {
            value.seq = endian::load_le64(bytes.data() + 24);
        }
        return value;
    }
};

// Ordered-lane identity: a client-assigned lane and the sequence allocated for
// this call. Present only when the caller opts into ordered delivery.
struct lane_token {
    u64 lane_id = 0;
    u64 seq = 0;
};

// Delivery guarantee. The v1 core implements `reliable` (request/response) and
// `unreliable` (fire-and-forget ONE_WAY). Ordered reliable lanes are carried by
// the lane token but serial execution is a server concern.
enum class delivery_mode : u8 {
    reliable = 0,
    unreliable = 1
};

// --- Call context ------------------------------------------------------------

// Everything a call carries besides its request body. The deadline is an
// absolute steady-clock time point inside one process, which is what the
// client's injected monotonic clock and the server's deadline check compare
// against. On the wire it travels as `remaining_ns`, a relative duration, so
// the value stays meaningful across processes and hosts.
struct call_context {
    // Absolute local deadline. Ignored unless `has_deadline` is set.
    std::chrono::steady_clock::time_point deadline{};
    bool has_deadline = false;

    // W3C trace context. `trace.valid()` is required before either id is sent.
    obs::trace_context trace{};

    // Application metadata. Ordered; copied into the request envelope when the
    // protocol supports it.
    metadata_list metadata;

    // Scheduling priority hint. Local only: it is not carried on the wire in
    // the v1 frame format.
    u8 priority = 0;

    delivery_mode mode = delivery_mode::reliable;

    // A call is idempotent when the caller declares it so *or* when it carries
    // an idempotency key. A timeout observed after the request was written is
    // reported as an unknown side effect for a non-idempotent call.
    bool idempotent = false;
    bool has_idempotency_key = false;
    call_id idempotency{};

    // Optional ordered lane.
    std::optional<lane_token> lane;

    [[nodiscard]] bool expired(std::chrono::steady_clock::time_point now) const noexcept {
        return has_deadline && now >= deadline;
    }

    // Time left before the deadline; zero once expired. With no deadline it
    // reports the maximum duration so a caller that adds it never wraps.
    [[nodiscard]] std::chrono::nanoseconds remaining(
        std::chrono::steady_clock::time_point now) const noexcept {
        if (!has_deadline) {
            return std::chrono::nanoseconds::max();
        }
        if (now >= deadline) {
            return std::chrono::nanoseconds{0};
        }
        return std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
    }
};

// --- Call slot ---------------------------------------------------------------

// Longest inline `then` callback. A callback larger than this is a compile
// error, never a silent heap allocation.
inline constexpr std::size_t call_callback_inline_size = 32;

// Inline, heap-free storage for a `then` callback. A union with `max_align_t`
// gives the storage the alignment a small functor needs without an
// `alignas` member (which would pad the slot and trip MSVC C4324).
union call_callback_storage {
    std::max_align_t align;
    std::byte bytes[call_callback_inline_size];
};

class call_slot;

// Invokes a stored callback: `fn(owner, slot)`. `owner` is the `call<T>*` on
// which `then` was called.
using call_callback_invoker = void (*)(void* storage, void* owner, call_slot& slot) noexcept;

// One pooled record. Public fields are intentional: the client, the state
// machine helpers and the tests all operate on the record directly, and hiding
// them behind accessors would only add indirection to the terminal race.
class CRPC_API call_slot {
public:
    call_slot() = default;
    call_slot(const call_slot&) = delete;
    call_slot& operator=(const call_slot&) = delete;
    call_slot(call_slot&&) = delete;
    call_slot& operator=(call_slot&&) = delete;
    ~call_slot() = default;

    // Clears everything except `generation`. Called by the pool when the slot
    // is handed back; the caller must guarantee no other thread references the
    // slot.
    void reset() noexcept;

    // Invokes the armed `then` callback, if any. Safe to call more than once;
    // the callback runs at most once.
    void invoke_callback() noexcept;

    // --- State ---
    std::atomic<u32> state{static_cast<u32>(call_state::idle)};
    // Bumped on every recycle so a stale handle is detectable.
    u32 generation = 0;
    // Assignment from the client.
    u32 stream_id = 0;
    u64 service_id = 0;
    u64 method_id = 0;

    // --- Context projection ---
    std::chrono::steady_clock::time_point deadline{};
    bool has_deadline = false;
    bool idempotent = false;
    bool one_way = false;
    call_id idem{};

    // True once the request bytes were handed to the writer queue (used to
    // decide whether a timeout implies an unknown side effect).
    bool written = false;

    // --- Terminal result (written by the claim winner only) ---
    rpc_code code = rpc_code::ok;
    rpc_error error;
    buffer response;
    std::atomic<bool> result_ready{false};

    // --- Wait primitive ---
    std::mutex wait_mutex;
    std::condition_variable wait_cv;

    // --- `then` callback ---
    call_callback_storage callback_storage{};
    call_callback_invoker callback_invoke = nullptr;
    void* callback_owner = nullptr;
    // Claimed exactly once: by the completion path or by `then()` when the call
    // already completed. Atomic so a handle destructor can disarm it without a
    // race against a backend-thread invocation (CF-1).
    std::atomic<bool> callback_armed{false};

    // --- Owning client (opaque to keep this header client-independent) ---
    void* owner = nullptr;
    void (*owner_cancel)(void* owner, call_slot* slot) noexcept = nullptr;
    // Optional: synchronizes with any in-flight callback and disarms it. Called
    // unconditionally when the handle is destroyed, so the slot can be recycled
    // only after no callback can still touch the handle.
    void (*owner_detach)(void* owner, call_slot* slot) noexcept = nullptr;
    // Optional: returns the slot to its owner's pool. When set, it runs instead
    // of `call_slot_pool::release` so the reset happens under the owner's lock:
    // a response thread finalizing the same slot holds that lock while it reads
    // `stream_id`/`has_deadline`, and an unguarded reset raced with it.
    void (*owner_release)(void* owner, call_slot* slot) noexcept = nullptr;
};

// Loads the state word with acquire ordering.
[[nodiscard]] CRPC_API call_state load_state(const call_slot& slot) noexcept;

// Sets a non-terminal state. Precondition: single client writer.
CRPC_API void set_state(call_slot& slot, call_state state) noexcept;

// Attempts the single terminal transition. Returns true for the winner and
// false when another party already claimed a terminal state. Does not touch the
// result fields.
[[nodiscard]] CRPC_API bool try_claim_terminal(call_slot& slot, call_state terminal) noexcept;

// Claims the terminal transition and, on success, publishes the result. The
// loser returns false without modifying the slot. Returns true on success.
[[nodiscard]] CRPC_API bool finish_slot(call_slot& slot, call_state terminal, rpc_code code,
                                        rpc_error error, span<const std::byte> body) noexcept;

// Acquire-load of the result-ready flag.
[[nodiscard]] CRPC_API bool result_ready(const call_slot& slot) noexcept;

// --- Slot pool ---------------------------------------------------------------

// Fixed-capacity pool with a free list. Slots are allocated once at
// construction; acquire/release perform no allocation. Thread-safe.
class CRPC_API call_slot_pool {
public:
    // Default capacity matches the design budget for one connection.
    explicit call_slot_pool(u32 capacity = 65536);
    ~call_slot_pool();

    call_slot_pool(const call_slot_pool&) = delete;
    call_slot_pool& operator=(const call_slot_pool&) = delete;
    call_slot_pool(call_slot_pool&&) = delete;
    call_slot_pool& operator=(call_slot_pool&&) = delete;

    // Returns nullptr when the pool is exhausted. A guard (returned=true) slot
    // must be returned with `release`.
    [[nodiscard]] call_slot* try_acquire() noexcept;

    // Returns a slot to the pool. Null is ignored. Bumps the generation.
    void release(call_slot* slot) noexcept;

    [[nodiscard]] u32 capacity() const noexcept { return capacity_; }
    [[nodiscard]] u32 in_use() const noexcept {
        return in_use_.load(std::memory_order_acquire);
    }
    [[nodiscard]] u32 available() const noexcept;

private:
    std::unique_ptr<call_slot[]> slots_;
    u32 capacity_ = 0;
    std::vector<call_slot*> free_;
    std::atomic<u32> in_use_{0};
    // Mutable so that `available() const` can take the lock.
    mutable std::mutex mutex_;
};

// --- Response/request codec --------------------------------------------------
//
// The typed helpers the client and the typed method trampoline use. The primary
// template has no definition for an unsupported type: instantiating it is a
// clear compile error rather than a silent empty payload. Specializations cover
// byte blobs, strings and arithmetic scalars; a user type is enabled by writing
// a `call_codec` specialization.

namespace rpc_detail {
template <typename>
struct always_false : std::false_type {};
} // namespace rpc_detail

template <typename T, typename = void>
struct call_codec {
    static status encode(const T&, buffer&) {
        static_assert(rpc_detail::always_false<T>::value,
                      "call_codec<T> is not specialized; provide a specialization "
                      "that encodes and decodes T");
        return status{rpc_code::encode_error};
    }
    static status decode(span<const std::byte>, T&) {
        static_assert(rpc_detail::always_false<T>::value,
                      "call_codec<T> is not specialized; provide a specialization "
                      "that encodes and decodes T");
        return status{rpc_code::decode_error};
    }
};

template <typename T>
struct call_codec<T, std::enable_if_t<std::is_arithmetic_v<T>>> {
    static status encode(const T& value, buffer& out) {
        if constexpr (std::is_same_v<T, float>) {
            std::byte tmp[4];
            endian::store_le32(tmp, endian::float_to_bits(value));
            return out.append(tmp, sizeof(tmp)) ? status{} : status{rpc_code::encode_error};
        } else if constexpr (std::is_same_v<T, double>) {
            std::byte tmp[8];
            endian::store_le64(tmp, endian::double_to_bits(value));
            return out.append(tmp, sizeof(tmp)) ? status{} : status{rpc_code::encode_error};
        } else if constexpr (std::is_floating_point_v<T>) {
            static_assert(rpc_detail::always_false<T>::value,
                          "call_codec: long double is not supported");
            return status{rpc_code::encode_error};
        } else {
            std::byte tmp[sizeof(T)];
            endian::store_le(tmp, value);
            return out.append(tmp, sizeof(tmp)) ? status{} : status{rpc_code::encode_error};
        }
    }

    static status decode(span<const std::byte> in, T& value) {
        if (in.size() != sizeof(T)) {
            return status{rpc_code::decode_error};
        }
        if constexpr (std::is_same_v<T, float>) {
            value = endian::bits_to_float(endian::load_le32(in.data()));
        } else if constexpr (std::is_same_v<T, double>) {
            value = endian::bits_to_double(endian::load_le64(in.data()));
        } else if constexpr (std::is_floating_point_v<T>) {
            return status{rpc_code::decode_error};
        } else {
            value = endian::load_le<T>(in.data());
        }
        return status{};
    }
};

template <typename Alloc>
struct call_codec<std::vector<std::byte, Alloc>, void> {
    static status encode(const std::vector<std::byte, Alloc>& value, buffer& out) {
        return out.append(value.data(), value.size()) ? status{} : status{rpc_code::encode_error};
    }
    static status decode(span<const std::byte> in, std::vector<std::byte, Alloc>& value) {
        value.assign(in.begin(), in.end());
        return status{};
    }
};

template <typename Traits, typename Alloc>
struct call_codec<std::basic_string<char, Traits, Alloc>, void> {
    static status encode(const std::basic_string<char, Traits, Alloc>& value, buffer& out) {
        return out.append(value.data(), value.size()) ? status{} : status{rpc_code::encode_error};
    }
    static status decode(span<const std::byte> in, std::basic_string<char, Traits, Alloc>& value) {
        value.assign(reinterpret_cast<const char*>(in.data()), in.size());
        return status{};
    }
};

// --- Call handle -------------------------------------------------------------

// Move-only RAII handle over a pooled call slot. The handle owns the slot until
// it is destroyed; destroying a pending handle cancels it and returns the slot.
//
// `get()` waits for the terminal state. On a client whose reader runs on another
// thread the wait blocks that thread; on a single-threaded in-process client the
// response is already delivered by the time `start_call` returns, so `get()`
// does not block. There is no coroutine integration in this core: a coroutine
// would park the same way the condition variable does.
template <typename T>
class [[nodiscard]] call {
public:
    using value_type = T;

    call() noexcept = default;

    call(call&& other) noexcept
        : slot_(other.slot_), generation_(other.generation_), pool_(other.pool_) {
        other.slot_ = nullptr;
        other.pool_ = nullptr;
        rebind_owner(&other);
    }

    call& operator=(call&& other) noexcept {
        if (this != &other) {
            release_pending();
            slot_ = other.slot_;
            generation_ = other.generation_;
            pool_ = other.pool_;
            other.slot_ = nullptr;
            other.pool_ = nullptr;
            rebind_owner(&other);
        }
        return *this;
    }

    call(const call&) = delete;
    call& operator=(const call&) = delete;

    ~call() { release_pending(); }

    [[nodiscard]] bool valid() const noexcept { return slot_ != nullptr; }
    [[nodiscard]] bool ready() const noexcept { return slot_ != nullptr && result_ready(*slot_); }

    [[nodiscard]] call_state state() const noexcept {
        return slot_ != nullptr ? load_state(*slot_) : call_state::idle;
    }

    [[nodiscard]] rpc_code code() const noexcept {
        return slot_ != nullptr ? slot_->code : rpc_code::failed_precondition;
    }

    [[nodiscard]] call_slot* slot() const noexcept { return slot_; }
    [[nodiscard]] u32 generation() const noexcept { return generation_; }

    // Waits for the terminal state and returns the decoded response or error.
    [[nodiscard]] rpc_result<T> get() {
        static_assert(std::is_default_constructible_v<T>,
                      "call<T>::get requires a default-constructible T");
        if (slot_ == nullptr) {
            return rpc_result<T>(rpc_error(rpc_code::failed_precondition, "invalid call handle"));
        }
        {
            std::unique_lock<std::mutex> lock(slot_->wait_mutex);
            slot_->wait_cv.wait(lock, [this] { return result_ready(*slot_); });
        }
        return build_result();
    }

    // Non-blocking variant. Returns false when the call has not terminated.
    [[nodiscard]] bool try_get(rpc_result<T>& out) {
        if (slot_ == nullptr || !result_ready(*slot_)) {
            return false;
        }
        out = build_result();
        return true;
    }

    // Registers a completion callback. `fn` is invoked as `fn(call&)` once the
    // call terminates, or immediately on this thread when it has already
    // terminated. The callback must be trivially copyable and fit in
    // `call_callback_inline_size`; it must not destroy the handle it belongs to.
    template <typename F>
    void then(F&& fn) {
        if (slot_ == nullptr) {
            return;
        }
        using fn_type = std::decay_t<F>;
        static_assert(std::is_trivially_copyable_v<fn_type>,
                      "call<T>::then callback must be trivially copyable");
        static_assert(sizeof(fn_type) <= call_callback_inline_size,
                      "call<T>::then callback exceeds the inline storage budget");
        static_assert(alignof(fn_type) <= alignof(std::max_align_t),
                      "call<T>::then callback has an unsupported alignment");
        std::memcpy(slot_->callback_storage.bytes, static_cast<const void*>(std::addressof(fn)),
                    sizeof(fn_type));
        slot_->callback_invoke = &call::template then_thunk<fn_type>;
        slot_->callback_owner = this;
        // Publish the callback before arming it; the completion path claims it
        // with an acquire exchange. If completion already happened, claim it
        // here instead so the callback runs exactly once.
        slot_->callback_armed.store(true, std::memory_order_release);
        if (result_ready(*slot_) &&
            slot_->callback_armed.exchange(false, std::memory_order_acq_rel)) {
            slot_->callback_invoke = nullptr;
            slot_->callback_owner = nullptr;
            fn(*this);
        }
    }

    // Requests cancellation. Idempotent; a call that already terminated is left
    // untouched.
    void cancel() noexcept {
        if (slot_ != nullptr && slot_->owner_cancel != nullptr) {
            slot_->owner_cancel(slot_->owner, slot_);
        }
    }

private:
    template <typename F>
    static void then_thunk(void* storage, void* owner, call_slot&) noexcept {
        F& fn = *static_cast<F*>(storage);
        call& self = *static_cast<call*>(owner);
        fn(self);
    }

    [[nodiscard]] rpc_result<T> build_result() {
        if (slot_->code != rpc_code::ok) {
            return rpc_result<T>(slot_->error);
        }
        T value{};
        const status decoded = call_codec<T>::decode(slot_->response.readable(), value);
        if (!decoded.ok()) {
            const std::string message =
                decoded.message().empty() ? "response decode failed" : decoded.message();
            return rpc_result<T>(rpc_error(rpc_code::decode_error, message));
        }
        return rpc_result<T>(std::move(value));
    }

    void release_pending() noexcept {
        if (slot_ == nullptr) {
            return;
        }
        call_slot* slot = slot_;
        call_slot_pool* pool = pool_;
        slot_ = nullptr;
        generation_ = 0;
        pool_ = nullptr;
        // The handle is going away. Disarm a possibly still-armed callback and
        // synchronize with any callback that is already running. This is done
        // unconditionally: a terminal slot can still have a completion thread
        // between `finish_slot` and `invoke_callback` (CF-1). The client's
        // detach runs under the same lock every callback invocation holds, so
        // once it returns no callback can run against the freed handle.
        if (slot->owner_detach != nullptr) {
            slot->owner_detach(slot->owner, slot);
        } else {
            slot->callback_armed.store(false, std::memory_order_release);
            slot->callback_invoke = nullptr;
            slot->callback_owner = nullptr;
        }
        if (!result_ready(*slot) && slot->owner_cancel != nullptr) {
            slot->owner_cancel(slot->owner, slot);
        }
        if (slot->owner_release != nullptr) {
            slot->owner_release(slot->owner, slot);
        } else if (pool != nullptr) {
            pool->release(slot);
        }
    }

    void rebind_owner(call* old) noexcept {
        if (slot_ != nullptr && slot_->callback_armed.load(std::memory_order_acquire) &&
            slot_->callback_owner == old) {
            slot_->callback_owner = this;
        }
    }

    friend class client;

    call_slot* slot_ = nullptr;
    u32 generation_ = 0;
    call_slot_pool* pool_ = nullptr;
};

} // namespace crpc::rpc

#endif // CRPC_RPC_CALL_HPP
