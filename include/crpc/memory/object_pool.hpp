// CRPC -- fixed-size object pool with RAII borrow handles.
//
// This is public infrastructure for third-party users, not a private
// optimization: any module that repeatedly constructs and destroys a fixed-size
// type (a wire packet, an AOI event, a hit result) can draw them from a pool
// instead of paying for new/delete every time.
//
// Design rules:
//
//   * acquire() hands out the object through a move-only RAII handle. The
//     object is returned automatically when the handle is destroyed, so an
//     early `return` or an error path can never leak it. There is no
//     forgettable manual-release step.
//   * the pool owns a preallocated slot array (one aligned storage cell per
//     object). acquire() placement-new-constructs a fresh T, release destroys
//     it, so every borrow observes a freshly-constructed object and no state
//     survives between borrowers. T must be default-constructible and must have
//     a non-throwing destructor (checked at compile time).
//   * exhaustion is configurable and spelled out: with
//     `options::block_on_exhaustion == false` (the default) an exhausted
//     acquire() fails with `resource_exhausted` immediately; with `true` it
//     parks on a condition variable until a release makes a slot available.
//     A zero-capacity pool always reports `resource_exhausted` in both modes.
//   * double-release and cross-pool releases are detected, never silently
//     dropped: every borrow carries the slot index plus the slot's generation,
//     and release validates the pair against the slot state. A stale or foreign
//     token fails with `invalid_argument` and the free list is left untouched,
//     so a double return cannot corrupt the freelist or orphan a slot.
//
// Synchronisation primitive and why:
//
//   A recursive mutex would be overkill; we use one plain std::mutex guarding
//   the slot array, the free-list stack and the counters, plus one
//   std::condition_variable to implement blocking acquire().
//
//   Why a mutex instead of the lock-free mpmc_queue or a lock-free stack:
//   (1) the pool owns preallocated aligned slots, so there is no node
//       allocation to avoid and therefore no reclamation problem (hazard
//       pointers / epoch lists) to pay for; the mpmc_queue would allocate a
//       node per release, defeating the "no allocation on the hot path" goal;
//   (2) blocking acquire() needs a wait primitive; doing it with a lock-free
//       queue plus a spin/backoff would burn CPU while an empty pool is
//       contended; a condition variable parks the waiter properly; (3) the
//       critical section is a pop or push of one index plus bookkeeping, i.e.
//       a few dozen instructions on the uncontended fast path, and the
//       double-release / generation check needs a place where slot state is
//       exclusive anyway.
//
//   A lock-free stack or a per-thread-object cache would reduce contention on
//   many cores; both remain future optimisations on top of this API and are
//   deliberately not the v1 implementation, where exact exhaustion semantics,
//   deterministic double-release detection and a tiny, auditable critical
//   section matter more than the last few nanoseconds.
//
// Lifetime:
//
//   The pool and every borrow share the slot state (shared_ptr). Destroying a
//   pool while borrows are still outstanding is safe: the slots stay alive as
//   long as any handle exists, a later handle destruction still destroys its
//   object, and the destructor reports the number of handles that were still
//   outstanding at destruction time on stderr. A truly leaked (never-destroyed)
//   handle keeps the state alive -- the same trade as the db::connection pool.
//
// Exception contract:
//
//   The pool API never throws. The only exception that can reach a caller is
//   one thrown by T's own default constructor; the slot is then restored to
//   the free list before the exception propagates. The pool destructor and the
//   handle destructor are noexcept.
#ifndef CRPC_MEMORY_OBJECT_POOL_HPP
#define CRPC_MEMORY_OBJECT_POOL_HPP

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc {

namespace memory {

namespace object_pool_detail {

// Shared slot state. Lives as long as the pool or any outstanding borrow.
// All members except the immutable capacity are guarded by `mutex_`.
template <typename T>
class state {
public:
    static_assert(std::is_default_constructible<T>::value,
                  "crpc::memory::object_pool<T> requires T to be "
                  "default-constructible (each borrow gets a fresh object)");
    static_assert(std::is_nothrow_destructible<T>::value,
                  "crpc::memory::object_pool<T> requires T to have a "
                  "non-throwing destructor");

    struct borrow {
        std::size_t index = 0;
        u64 generation = 0;
        T* object = nullptr;
    };

    state(std::size_t capacity, bool block_on_exhaustion)
        : block_on_exhaustion_(block_on_exhaustion) {
        if (capacity == 0) {
            return;
        }
        slot* slots = new (std::nothrow) slot[capacity];
        if (slots == nullptr) {
            // Out of memory: degrade to a zero-capacity pool instead of
            // throwing. Every acquire reports resource_exhausted.
            std::fprintf(stderr,
                         "[crpc] object_pool: slot allocation failed; "
                         "operating as a zero-capacity pool\n");
            return;
        }
        slots_.reset(slots);
        capacity_ = capacity;
        free_.reset(new std::size_t[capacity]);
        for (std::size_t i = 0; i < capacity; ++i) {
            free_[i] = i;
        }
        free_count_ = capacity;
    }

    state(const state&) = delete;
    state& operator=(const state&) = delete;

    // Acquires one slot. Blocks when the pool is exhausted and blocking mode
    // is on; reports resource_exhausted immediately otherwise; reports
    // shutting_down after close().
    status acquire(borrow& out) {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            if (capacity_ == 0) {
                return status{rpc_code::resource_exhausted,
                              "object pool has zero capacity"};
            }
            if (closed_) {
                return status{rpc_code::shutting_down,
                              "object pool has been shut down"};
            }
            if (free_count_ != 0) {
                const std::size_t index = free_[--free_count_];
                T* object = object_at(index);
                try {
                    new (static_cast<void*>(object)) T();
                } catch (...) {
                    // T's constructor threw: restore the slot so the pool
                    // stays consistent, then let the caller see the error.
                    free_[free_count_++] = index;
                    throw;
                }
                out.index = index;
                out.generation = slots_[index].generation;
                out.object = object;
                ++in_use_;
                if (in_use_ > peak_in_use_) {
                    peak_in_use_ = in_use_;
                }
                ++total_acquires_;
                return status{};
            }
            if (!block_on_exhaustion_) {
                return status{rpc_code::resource_exhausted,
                              "object pool is exhausted"};
            }
            // Waiter-count pattern: increment the waiter count under the lock,
            // then wait. A release that runs between the increment and the
            // wait cannot miss the wake-up: it also holds the lock, and the
            // waiter does not release it until wait() blocks.
            ++waiting_;
            cv_.wait(lock);
            --waiting_;
        }
    }

    // Returns a borrowed slot. Validates the token so a double return or a
    // stale handle cannot corrupt the free list. Idempotent failure.
    status return_object(std::size_t index, u64 generation) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index >= capacity_ || slots_ == nullptr) {
            return status{rpc_code::invalid_argument,
                          "object pool handle does not name a slot of this pool"};
        }
        if (slots_[index].generation != generation) {
            return status{rpc_code::invalid_argument,
                          "object pool handle is stale: the slot was already returned"};
        }
        object_at(index)->~T();
        ++slots_[index].generation;
        // free_count_ is bounded by capacity_: this push is the mirror of the
        // pop that issued the borrow.
        free_[free_count_++] = index;
        --in_use_;
        ++total_releases_;
        if (waiting_ != 0) {
            cv_.notify_one();
        }
        return status{};
    }

    // Idempotent. Marks the pool closed (acquire() then reports
    // shutting_down), wakes every blocked acquire, and reports how many
    // borrows were still outstanding.
    void close() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return;
        }
        closed_ = true;
        shutdown_outstanding_ = in_use_;
        if (shutdown_outstanding_ != 0) {
            std::fprintf(stderr,
                         "[crpc] object_pool destroyed with %zu outstanding "
                         "handle(s)\n",
                         shutdown_outstanding_);
        }
        if (waiting_ != 0) {
            cv_.notify_all();
        }
    }

    [[nodiscard]] bool is_closed() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return capacity_;
    }

    [[nodiscard]] std::size_t in_use() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return in_use_;
    }

    [[nodiscard]] std::size_t available() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return capacity_ < in_use_ ? 0 : capacity_ - in_use_;
    }

    [[nodiscard]] std::size_t peak_in_use() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return peak_in_use_;
    }

    [[nodiscard]] u64 total_acquires() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return total_acquires_;
    }

    [[nodiscard]] u64 total_releases() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return total_releases_;
    }

private:
    struct slot {
        // The aligned cell the pooled object lives in; `generation` counts
        // how many times this slot has been reissued so stale handles can be
        // recognised. alignas(T) (rather than std::aligned_storage) supports
        // over-aligned T on every toolchain without a global macro.
        alignas(T) unsigned char storage[sizeof(T)];
        u64 generation = 1;
    };

    T* object_at(std::size_t index) noexcept {
        return reinterpret_cast<T*>(&slots_[index].storage);
    }

    std::unique_ptr<slot[]> slots_;
    std::unique_ptr<std::size_t[]> free_;
    std::size_t free_count_ = 0;
    std::size_t capacity_ = 0;
    const bool block_on_exhaustion_ = false;

    // Number of acquirers parked on `cv_`. notify_one()/notify_all() are only
    // worth their cost when someone is actually waiting: a release in the
    // common uncontended case does not need to touch the condition variable
    // at all.
    std::size_t waiting_ = 0;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool closed_ = false;
    std::size_t in_use_ = 0;
    std::size_t peak_in_use_ = 0;
    u64 total_acquires_ = 0;
    u64 total_releases_ = 0;
    std::size_t shutdown_outstanding_ = 0;
};

} // namespace object_pool_detail

// Fixed-size object pool. See the header comment for the contract.
template <typename T>
class object_pool {
public:
    struct options {
        // Number of objects the pool hands out before it is exhausted.
        std::size_t capacity = 0;
        // false: an exhausted acquire() fails immediately with
        //        resource_exhausted.
        // true:  an exhausted acquire() blocks until a release or until the
        //        pool is destroyed (then shutting_down).
        bool block_on_exhaustion = false;
    };

    // A borrowed object. Returning it is automatic: the destructor returns the
    // object to the pool (an early `return` cannot leak it). Explicit
    // `release()` exists for callers that want the object back before the
    // handle's scope ends; a second `release()` is reported as an error.
    class handle {
    public:
        handle() noexcept = default;
        ~handle() {
            (void)return_to_pool();
        }

        handle(const handle&) = delete;
        handle& operator=(const handle&) = delete;
        handle(handle&& other) noexcept
            : state_(std::move(other.state_)), object_(other.object_),
              index_(other.index_), generation_(other.generation_) {
            other.state_.reset();
            other.object_ = nullptr;
            other.index_ = npos;
            other.generation_ = 0;
        }
        handle& operator=(handle&& other) noexcept {
            if (this != &other) {
                (void)return_to_pool();
                state_ = std::move(other.state_);
                object_ = other.object_;
                index_ = other.index_;
                generation_ = other.generation_;
                other.state_.reset();
                other.object_ = nullptr;
                other.index_ = npos;
                other.generation_ = 0;
            }
            return *this;
        }

        [[nodiscard]] T* get() noexcept { return valid() ? object_ : nullptr; }
        [[nodiscard]] const T* get() const noexcept {
            return valid() ? object_ : nullptr;
        }
        [[nodiscard]] T* operator->() noexcept { return get(); }
        [[nodiscard]] const T* operator->() const noexcept { return get(); }
        [[nodiscard]] T& operator*() noexcept { return *get(); }
        [[nodiscard]] const T& operator*() const noexcept { return *get(); }

        [[nodiscard]] bool valid() const noexcept {
            return state_ != nullptr && object_ != nullptr;
        }

        // Returns the object to the pool now. The handle becomes invalid on the
        // first call; a second call (double release) is reported as
        // `invalid_argument` and is a no-op on the pool.
        [[nodiscard]] status release() noexcept {
            if (!valid()) {
                return status{rpc_code::invalid_argument,
                              "object pool handle is empty or already released"};
            }
            return return_to_pool();
        }

    private:
        friend class object_pool;
        static constexpr std::size_t npos = static_cast<std::size_t>(-1);

        handle(std::shared_ptr<object_pool_detail::state<T>> state, T* object,
               std::size_t index, u64 generation) noexcept
            : state_(std::move(state)), object_(object), index_(index),
              generation_(generation) {}

        // Invalidates the handle first, then returns the object. Invalidating
        // first makes the destructor of an already-released handle a no-op and
        // makes a re-entrant release (T's destructor releasing the handle it
        // lives in) safe.
        status return_to_pool() noexcept {
            if (!state_) {
                return status{}; // already returned
            }
            auto state = std::move(state_);
            const std::size_t index = index_;
            const u64 generation = generation_;
            state_.reset();
            object_ = nullptr;
            index_ = npos;
            generation_ = 0;
            return state->return_object(index, generation);
        }

        std::shared_ptr<object_pool_detail::state<T>> state_;
        T* object_ = nullptr;
        std::size_t index_ = npos;
        u64 generation_ = 0;
    };

    explicit object_pool(options opt) {
        // Slot storage is allocated with nothrow new inside the state; a
        // failure degrades to a zero-capacity pool instead of throwing. The
        // state's own control block can still throw under process-memory
        // exhaustion; this is the pool's only throwing path and it happens at
        // construction, before any borrow exists.
        object_pool_detail::state<T>* raw =
            new (std::nothrow) object_pool_detail::state<T>(opt.capacity,
                                                           opt.block_on_exhaustion);
        if (raw != nullptr) {
            impl_ = std::shared_ptr<object_pool_detail::state<T>>(raw);
        } else {
            impl_ = std::make_shared<object_pool_detail::state<T>>(
                0, opt.block_on_exhaustion);
        }
    }
    ~object_pool() {
        if (impl_) {
            impl_->close();
        }
    }

    object_pool(const object_pool&) = delete;
    object_pool& operator=(const object_pool&) = delete;
    object_pool(object_pool&&) = delete;
    object_pool& operator=(object_pool&&) = delete;

    // Borrows one object. Returns resource_exhausted when the pool is drained
    // (or too small to serve anything) and shutting_down after destruction.
    [[nodiscard]] result<handle> acquire() {
        typename object_pool_detail::state<T>::borrow b;
        status s = impl_->acquire(b);
        if (!s.ok()) {
            return s.error();
        }
        return handle(impl_, b.object, b.index, b.generation);
    }

    // Explicit return path. `h` must have been acquired from this pool; a
    // foreign handle (acquired from another pool, or already released) is
    // reported as `invalid_argument` and is not consumed.
    [[nodiscard]] status release(handle& h) noexcept {
        if (!h.valid()) {
            return status{rpc_code::invalid_argument,
                          "object pool handle is empty or already released"};
        }
        if (h.state_.get() != impl_.get()) {
            return status{rpc_code::invalid_argument,
                          "object pool handle does not belong to this pool"};
        }
        return h.release();
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return impl_->capacity();
    }
    [[nodiscard]] std::size_t in_use() const noexcept { return impl_->in_use(); }
    [[nodiscard]] std::size_t available() const noexcept {
        return impl_->available();
    }
    [[nodiscard]] std::size_t peak_in_use() const noexcept {
        return impl_->peak_in_use();
    }
    [[nodiscard]] std::uint64_t total_acquires() const noexcept {
        return impl_->total_acquires();
    }
    [[nodiscard]] std::uint64_t total_releases() const noexcept {
        return impl_->total_releases();
    }
    [[nodiscard]] bool is_closed() const noexcept { return impl_->is_closed(); }

private:
    std::shared_ptr<object_pool_detail::state<T>> impl_;
};

} // namespace memory
} // namespace crpc

#endif // CRPC_MEMORY_OBJECT_POOL_HPP