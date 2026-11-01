// CRPC -- a single-producer single-consumer byte ring for cross-process use.
//
// This is the data structure under a shared-memory IPC channel: one process writes
// frames into it, the other reads them out, and neither takes a lock. It is a plain
// struct of integer indices and a caller-owned byte buffer, so it can live inside a
// `fs::shared_memory` mapping and be addressed identically from both sides -- which
// is the whole point. Nothing here allocates.
//
// Why the indices are what they are
// ---------------------------------
// A ring needs to tell "empty" from "full", and the usual ways to do that are all
// subtly wrong across processes:
//
//   * A `size` counter shared by both sides is a second owner of the same fact. Two
//     writers to that counter race, and a torn read of it makes the buffer look
//     simultaneously larger and smaller than it is.
//   * Wrapping `head == tail` as "full" costs one usable slot and makes the maximum
//     frame one byte smaller than the buffer, which is easy to get wrong by one.
//
// So both indices are free-running 64-bit counters that only their own side writes:
// the producer ever writes `write_`, the consumer ever writes `read_`. Neither is
// ever reset. Occupancy is `write_ - read_`, and a 64-bit counter at one byte per
// nanosecond wraps after 584 years, so "free-running" costs nothing to reason about.
// Every index is reduced modulo the buffer size only at the point of use.
//
// Memory ordering
// ---------------
// The producer publishes bytes and then the index; the consumer reads the index and
// then the bytes. The store of `write_` is `release` and the load of it is `acquire`,
// which is what makes "the reader can see every byte the index covers" true on a
// weakly-ordered CPU. The symmetric pair guards `read_`. Relaxed ordering on the
// opposite direction is deliberate: a stale `read_` only makes the producer think the
// ring is fuller than it is, which costs throughput but can never corrupt data.
//
// One producer and one consumer, and only one of each. Two producers would need a
// compare-exchange loop and would break the "only I write my index" invariant above;
// the ring therefore refuses to pretend it supports that.
#ifndef CRPC_IPC_RING_BUFFER_HPP
#define CRPC_IPC_RING_BUFFER_HPP

#include <atomic>
#include <cstddef>
#include <cstring>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace ipc {

// The header a ring needs, laid out so it is valid inside shared memory on both
// platforms: only integers, no pointers, `alignas(64)` so the two indices do not
// share a cache line with each other or with the payload. `capacity` is stored
// because the two sides must not disagree about it -- a peer that mapped the object
// with a different size would otherwise scribble outside its own view.
struct ring_control {
    // Free-running; written only by the producer.
    std::atomic<std::uint64_t> write{0};
    // Free-running; written only by the consumer.
    std::atomic<std::uint64_t> read{0};
    // Payload bytes following the control block. Set by the creator.
    std::uint64_t capacity = 0;
    // Set to a fixed value by the creator so a peer can tell "this region really is
    // a ring" from "this region happens to be zeroed".
    std::uint64_t magic = 0;
};

// Value written into `ring_control::magic`. Chosen so an all-zero region -- the
// usual content of a fresh mapping -- does not pass the check.
inline constexpr std::uint64_t ring_magic = 0x4352504352494E31ull; // "CRPCRIN1"

// Bytes that a caller must reserve before the payload.
inline constexpr std::size_t ring_control_size = sizeof(ring_control);

// A view over one side of the ring. `attach_producer` / `attach_consumer` bind it to
// a control block and payload; the constructor does not validate, so misuse (mapping
// a region that is not a ring) is a programming error rather than an error path.
class ring_view {
public:
    ring_view() = default;

    // Binds to a control block and its payload. `control` and `payload` must stay
    // alive for the view's whole life; `payload` must be `control->capacity` bytes.
    static ring_view attach_producer(ring_control* control, span<std::byte> payload) noexcept {
        ring_view out;
        out.control_ = control;
        out.payload_ = payload;
        out.producer_ = true;
        return out;
    }

    static ring_view attach_consumer(ring_control* control, span<std::byte> payload) noexcept {
        ring_view out;
        out.control_ = control;
        out.payload_ = payload;
        out.producer_ = false;
        return out;
    }

    // Initialises a fresh control block and returns its capacity. Called by the
    // creator only, before any view exists in either process.
    static void initialise(ring_control* control, std::size_t capacity) noexcept {
        control->capacity = capacity;
        control->magic = ring_magic;
        control->write.store(0, std::memory_order_relaxed);
        control->read.store(0, std::memory_order_relaxed);
    }

    // True when `control` looks like a ring this class created.
    [[nodiscard]] static bool looks_like_ring(const ring_control* control) noexcept {
        return control != nullptr && control->magic == ring_magic &&
               control->capacity != 0;
    }

    [[nodiscard]] bool valid() const noexcept {
        return control_ != nullptr && payload_.size() == control_->capacity;
    }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return control_ != nullptr ? static_cast<std::size_t>(control_->capacity) : 0;
    }

    // Bytes currently queued. Computed from the two counters, so it is a snapshot:
    // a caller that needs it to be exact must be the only party touching that
    // direction.
    [[nodiscard]] std::size_t size() const noexcept {
        if (control_ == nullptr) {
            return 0;
        }
        const std::uint64_t write = control_->write.load(std::memory_order_acquire);
        const std::uint64_t read = control_->read.load(std::memory_order_acquire);
        return static_cast<std::size_t>(write - read);
    }

    [[nodiscard]] std::size_t free_space() const noexcept {
        const std::size_t cap = capacity();
        const std::size_t used = size();
        return cap > used ? cap - used : 0;
    }

    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    // --- Producer side -------------------------------------------------------

    // Appends `data` as one contiguous run. Returns false and writes nothing when
    // the run does not fit, so a caller never gets a partial frame it did not ask
    // for. `write_bytes`/`write_available` exist for the two-part case: a producer
    // that has already published part of a frame can finish it, or skip it only by
    // inserting a length the consumer will also skip.
    [[nodiscard]] bool try_write(span<const std::byte> data) noexcept {
        if (!producer_ || control_ == nullptr) {
            return false;
        }
        const std::size_t cap = capacity();
        if (cap == 0 || data.size() > cap) {
            return false;
        }
        const std::uint64_t write = control_->write.load(std::memory_order_relaxed);
        const std::uint64_t read = control_->read.load(std::memory_order_acquire);
        if (static_cast<std::size_t>(write - read) + data.size() > cap) {
            return false;
        }
        const std::size_t offset = static_cast<std::size_t>(write % cap);
        const std::size_t first = cap - offset;
        if (data.size() <= first) {
            if (!data.empty()) {
                std::memcpy(payload_.data() + offset, data.data(), data.size());
            }
        } else {
            std::memcpy(payload_.data() + offset, data.data(), first);
            std::memcpy(payload_.data(), data.data() + first, data.size() - first);
        }
        // Publish only after the bytes are in place: a consumer that sees the new
        // index must be able to read everything it covers.
        control_->write.store(write + data.size(), std::memory_order_release);
        return true;
    }

    // --- Consumer side -------------------------------------------------------

    // Reads at most `out.size()` bytes into `out` as one contiguous run. Returns the
    // number of bytes read; `0` when the ring is empty. Never blocks and never
    // returns a partial frame that spans the wrap point as two calls the caller
    // cannot tell apart from one frame -- the wrap is copied into `out` contiguously.
    [[nodiscard]] std::size_t try_read(span<std::byte> out) noexcept {
        if (control_ == nullptr || out.empty()) {
            return 0;
        }
        const std::size_t cap = capacity();
        if (cap == 0) {
            return 0;
        }
        const std::uint64_t write = control_->write.load(std::memory_order_acquire);
        const std::uint64_t read = control_->read.load(std::memory_order_relaxed);
        const std::size_t available = static_cast<std::size_t>(write - read);
        if (available == 0) {
            return 0;
        }
        const std::size_t take = out.size() < available ? out.size() : available;
        const std::size_t offset = static_cast<std::size_t>(read % cap);
        const std::size_t first = cap - offset;
        if (take <= first) {
            std::memcpy(out.data(), payload_.data() + offset, take);
        } else {
            std::memcpy(out.data(), payload_.data() + offset, first);
            std::memcpy(out.data() + first, payload_.data(), take - first);
        }
        // Release the bytes only after they were copied out.
        control_->read.store(read + take, std::memory_order_release);
        return take;
    }

    // The number of bytes a single `try_read` can take without a wrap: the consumer
    // can use this to ask for exactly one run instead of guessing.
    [[nodiscard]] std::size_t readable_run() const noexcept {
        if (control_ == nullptr) {
            return 0;
        }
        const std::size_t cap = capacity();
        if (cap == 0) {
            return 0;
        }
        const std::uint64_t write = control_->write.load(std::memory_order_acquire);
        const std::uint64_t read = control_->read.load(std::memory_order_relaxed);
        const std::size_t available = static_cast<std::size_t>(write - read);
        const std::size_t offset = static_cast<std::size_t>(read % cap);
        const std::size_t run = cap - offset;
        return available < run ? available : run;
    }

private:
    ring_control* control_ = nullptr;
    span<std::byte> payload_{};
    // One flag, not two: a view is either the producer or the consumer, and making
    // both operations callable from one view is how a ring gets two producers by
    // accident.
    bool producer_ = false;
};

} // namespace ipc
} // namespace crpc

#endif // CRPC_IPC_RING_BUFFER_HPP
