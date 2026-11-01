// CRPC -- single-producer single-consumer queue of fixed-size values.
//
// `ipc::ring_buffer` moves bytes; a server also needs to move *values* without
// hand-serialising each one into a byte run. This is that queue: a ring of `T` with
// the same index scheme, so it is lock-free, allocation-free after construction, and
// -- the reason it lives beside the byte ring -- laid out only with integers, so it
// can sit inside a shared mapping for a cross-process hand-off.
//
// Why not `std::queue` with a mutex: the producer and the consumer are one thread
// each and neither ever waits for the other. A mutex would make the fast path take a
// lock, which is the thing a per-shard work queue cannot afford. The ring gives the
// producer a store and the consumer a load in the uncontended case.
//
// Index scheme, inherited from `ring_buffer` and for the same reasons documented
// there: two free-running 64-bit counters, each written by exactly one side, never
// reset. Occupancy is `write - read`. There is no sentinel slot, so a full queue holds
// exactly `capacity()` values.
//
// Requirements on `T`
// -------------------
//   * trivially copyable. A queue that can live in shared memory cannot own a
//     pointer, and moving a non-trivial type in and out of a mapped region would run
//     constructors on memory the other process also sees. This is enforced with a
//     static_assert rather than a comment, because the failure mode (a `std::string`
//     in shared memory) is a crash two processes away from the mistake.
//   * default-constructible, so the storage can be a plain array.
//
// One producer and one consumer, and only one of each; the queue cannot detect a
// second producer and does not try. That is the same contract as `ring_buffer`.
#ifndef CRPC_IPC_SPSC_QUEUE_HPP
#define CRPC_IPC_SPSC_QUEUE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include <crpc/config.hpp>

namespace crpc {
namespace ipc {

// The control block of an SPSC value queue. Same shape as `ring_control`, plus the
// element size so a peer can refuse a mapping created for a different `T` -- mapping a
// queue of `int` as a queue of `double` would otherwise read plausible nonsense.
struct spsc_control {
    std::atomic<std::uint64_t> write{0};
    std::atomic<std::uint64_t> read{0};
    std::uint64_t capacity = 0;
    std::uint64_t element_size = 0;
    std::uint64_t magic = 0;
};

inline constexpr std::uint64_t spsc_magic = 0x4352504353513131ull; // "CRPCSQ11"

// Initialises a control block for `capacity` elements of `element_size` bytes.
inline void spsc_initialise(spsc_control* control, std::size_t capacity,
                            std::size_t element_size) noexcept {
    control->capacity = capacity;
    control->element_size = element_size;
    control->magic = spsc_magic;
    control->write.store(0, std::memory_order_relaxed);
    control->read.store(0, std::memory_order_relaxed);
}

// True when `control` was initialised for exactly this element type and a non-zero
// capacity. A mismatch is a hard error for the caller, not something to reinterpret.
inline bool spsc_matches(const spsc_control* control, std::size_t element_size) noexcept {
    return control != nullptr && control->magic == spsc_magic &&
           control->capacity != 0 && control->element_size == element_size;
}

// A queue over caller-owned storage: `control` plus an array of `capacity` `T`.
//
// Two attach functions, like `ring_view`: one end pushes, the other pops. The same
// object is not both -- a queue whose single instance could push and pop would be a
// queue with two producers the moment two threads used it.
template <typename T>
class spsc_view {
    static_assert(std::is_trivially_copyable<T>::value,
                  "ipc::spsc_view requires a trivially copyable T: it may live in "
                  "shared memory, where a value owning a pointer is meaningless to the "
                  "other process");
    static_assert(std::is_default_constructible<T>::value,
                  "ipc::spsc_view stores its slots in a plain array");

public:
    spsc_view() = default;

    static spsc_view attach_producer(spsc_control* control, T* slots) noexcept {
        spsc_view out;
        out.control_ = control;
        out.slots_ = slots;
        out.producer_ = true;
        return out;
    }

    static spsc_view attach_consumer(spsc_control* control, T* slots) noexcept {
        spsc_view out;
        out.control_ = control;
        out.slots_ = slots;
        out.producer_ = false;
        return out;
    }

    [[nodiscard]] bool valid() const noexcept {
        return control_ != nullptr && slots_ != nullptr && control_->capacity != 0;
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return control_ != nullptr ? static_cast<std::size_t>(control_->capacity) : 0;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        if (control_ == nullptr) {
            return 0;
        }
        const std::uint64_t write = control_->write.load(std::memory_order_acquire);
        const std::uint64_t read = control_->read.load(std::memory_order_acquire);
        return static_cast<std::size_t>(write - read);
    }

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    [[nodiscard]] std::size_t free_space() const noexcept {
        const std::size_t cap = capacity();
        const std::size_t used = size();
        return cap > used ? cap - used : 0;
    }

    // Publishes `value`. False when full; nothing is written in that case, so a caller
    // can retry the same value.
    [[nodiscard]] bool try_push(const T& value) noexcept {
        if (!producer_ || control_ == nullptr) {
            return false;
        }
        const std::size_t cap = capacity();
        if (cap == 0) {
            return false;
        }
        const std::uint64_t write = control_->write.load(std::memory_order_relaxed);
        const std::uint64_t read = control_->read.load(std::memory_order_acquire);
        if (static_cast<std::size_t>(write - read) >= cap) {
            return false;
        }
        slots_[static_cast<std::size_t>(write % cap)] = value;
        // Publish the index only after the slot is written.
        control_->write.store(write + 1, std::memory_order_release);
        return true;
    }

    bool try_push(T&& value) noexcept { return try_push(static_cast<const T&>(value)); }

    // Takes one value into `out`. False when empty; `out` is untouched then, so a
    // caller does not have to distinguish "no value" from "a default-constructed one".
    [[nodiscard]] bool try_pop(T& out) noexcept {
        if (!(control_ != nullptr && slots_ != nullptr)) {
            return false;
        }
        const std::size_t cap = capacity();
        if (cap == 0) {
            return false;
        }
        const std::uint64_t write = control_->write.load(std::memory_order_acquire);
        const std::uint64_t read = control_->read.load(std::memory_order_relaxed);
        if (write == read) {
            return false;
        }
        out = slots_[static_cast<std::size_t>(read % cap)];
        // Release the slot only after the copy is out of it.
        control_->read.store(read + 1, std::memory_order_release);
        return true;
    }

private:
    spsc_control* control_ = nullptr;
    T* slots_ = nullptr;
    bool producer_ = false;
};

// A self-contained SPSC queue that owns its storage. Not shareable across processes --
// use the view form with a mapping for that -- but this is what a per-shard work queue
// uses, and it is the form the tests drive directly.
template <typename T>
class spsc_queue {
    static_assert(std::is_trivially_copyable<T>::value,
                  "ipc::spsc_queue requires a trivially copyable T");

public:
    explicit spsc_queue(std::size_t capacity)
        : capacity_(capacity == 0 ? 1 : capacity),
          slots_(new T[capacity_]) {
        spsc_initialise(&control_, capacity_, sizeof(T));
        producer_ = spsc_view<T>::attach_producer(&control_, slots_.get());
        consumer_ = spsc_view<T>::attach_consumer(&control_, slots_.get());
    }

    spsc_queue(const spsc_queue&) = delete;
    spsc_queue& operator=(const spsc_queue&) = delete;

    [[nodiscard]] bool push(const T& value) noexcept { return producer_.try_push(value); }
    [[nodiscard]] bool pop(T& out) noexcept { return consumer_.try_pop(out); }
    [[nodiscard]] std::size_t size() const noexcept { return consumer_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t free_space() const noexcept { return consumer_.free_space(); }
    [[nodiscard]] bool empty() const noexcept { return consumer_.empty(); }
    [[nodiscard]] bool full() const noexcept { return consumer_.free_space() == 0; }

    // The two ends, for driving the queue from two threads without adding a mutex.
    [[nodiscard]] spsc_view<T> producer() const noexcept { return producer_; }
    [[nodiscard]] spsc_view<T> consumer() const noexcept { return consumer_; }

private:
    std::size_t capacity_ = 0;
    std::unique_ptr<T[]> slots_;
    spsc_control control_{};
    spsc_view<T> producer_{};
    spsc_view<T> consumer_{};
};

} // namespace ipc
} // namespace crpc

#endif // CRPC_IPC_SPSC_QUEUE_HPP
