// CRPC -- bounded, lock-free SPSC mailbox (the actor pattern's queue).
//
// This header is the queue half of the actor pattern. It is deliberately
// independent of `crpc::async`: a mailbox is just "post a message, pop a
// message, refuse instead of growing". The actor layer adds the executor and
// the lifecycle on top.
//
// Concurrency model
// -----------------
// `mailbox` is single-producer / single-consumer. Exactly one thread may call
// `try_post` and exactly one (different) thread may call `try_pop`; `size` and
// `capacity` are diagnostics and may be read from either side.
//
// The producer owns the head cursor and the consumer owns the tail cursor, so
// the two never write the same cache line: each cursor is
// CRPC_CACHE_LINE_ALIGN-aligned and they live on separate lines. That is the
// point of the alignment: an unaligned pair makes every operation bounce the
// line between the two cores.
//
// Synchronisation uses the ring invariant `head - tail <= capacity` with
// release stores on the publishing cursor and acquire loads on the observing
// one. The producer writes only the slot at `head % capacity`, the consumer
// reads only the slot at `tail % capacity`, and the two indices name distinct
// slots whenever the queue is neither empty nor full -- so the slot storage
// never needs a lock or a per-slot atomic flag.
//
// Multi-producer use
// ------------------
// A mailbox with several producers is *not* safe: two producers would race on
// the head cursor and on the same slot. `mpsc_mailbox` below is the documented
// fallback -- it serialises both sides with one mutex while keeping the
// bounded, refuse-when-full policy. The actor uses exactly this wrapper,
// because `actor::post` is multi-producer by nature.
//
// Failure policy
// --------------
// A full mailbox returns `rpc_code::resource_exhausted`; it never grows, never
// blocks and never retries. The caller decides whether to drop, delay or shed.
// That is the backpressure contract the actor exposes unchanged.
//
// A `Message` move constructor/destructor is expected to be non-throwing for
// the lock-free path. If it does throw anyway, the message is dropped and the
// cursor still advances: stranding the queue would turn one bad message into a
// permanent stall. `try_post` reports `internal` for a throwing move; the
// consumer side cannot report it (the signature is `bool`), so a drop there is
// silent by design and only a throwing message type can cause it.
#ifndef CRPC_ACTOR_MAILBOX_HPP
#define CRPC_ACTOR_MAILBOX_HPP

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <crpc/cache_line.hpp>
#include <crpc/config.hpp>
#include <crpc/error.hpp>

// C4324: "structure was padded due to alignment specifier". That padding is
// the whole purpose here, so the warning is disabled for the mailbox types
// only; the rest of the translation unit keeps /WX.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

namespace crpc {
namespace actor {

// A bounded SPSC queue. See the header comment for the memory model.
template <typename Message>
class mailbox {
public:
    // `capacity` is clamped to at least 1: a zero-capacity mailbox could never
    // accept anything and would turn every post into a permanent failure.
    explicit mailbox(std::size_t capacity)
        : capacity_(capacity == 0 ? 1 : capacity), ring_(capacity_) {}

    mailbox(const mailbox&) = delete;
    mailbox& operator=(const mailbox&) = delete;
    mailbox(mailbox&&) = delete;
    mailbox& operator=(mailbox&&) = delete;

    ~mailbox() = default;

    // Producer side. Returns `resource_exhausted` when the queue is full.
    [[nodiscard]] status try_post(Message m) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= capacity_) {
            return rpc_error{rpc_code::resource_exhausted,
                             "mailbox: the bounded queue is full"};
        }
        try {
            ring_[head % capacity_].emplace(std::move(m));
        } catch (...) {
            // The slot stays empty and the cursor does not move: the message
            // is lost but the queue is still consistent.
            return rpc_error{rpc_code::internal,
                             "mailbox: the message move constructor threw"};
        }
        head_.store(head + 1, std::memory_order_release);
        return {};
    }

    // Consumer side. Returns false when the queue is empty. `out` is left
    // untouched when the queue is empty; if the move-assignment into `out`
    // throws, the message is dropped and the exception propagates.
    [[nodiscard]] bool try_pop(Message& out) {
        std::optional<Message> popped = try_pop_value();
        if (!popped) {
            return false;
        }
        out = std::move(*popped);
        return true;
    }

    // Consumer side, without requiring `Message` to be default-constructible
    // or move-assignable. Returns nullopt when the queue is empty. A throwing
    // move constructor drops that one message and yields nullopt, so the
    // caller cannot distinguish it from "empty"; a message type whose move
    // throws is outside the intended contract (see the header comment).
    [[nodiscard]] std::optional<Message> try_pop_value() {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (tail == head) {
            return std::nullopt;
        }
        std::optional<Message>& slot = ring_[tail % capacity_];
        std::optional<Message> out;
        try {
            out.emplace(std::move(*slot));
        } catch (...) {
            out.reset();
        }
        slot.reset();
        tail_.store(tail + 1, std::memory_order_release);
        return out;
    }

    // Approximate: the other side may move between the two loads. Diagnostic
    // only; nothing in the queue's correctness reads this.
    [[nodiscard]] std::size_t size() const noexcept {
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return head - tail;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    // Diagnostics for the alignment test: the addresses of the two cursors,
    // without exposing the private layout. They must sit on different cache
    // lines or the "separated cursors" claim above is false.
    [[nodiscard]] const void* head_cursor_address() const noexcept {
        return static_cast<const void*>(&head_);
    }
    [[nodiscard]] const void* tail_cursor_address() const noexcept {
        return static_cast<const void*>(&tail_);
    }

private:
    std::size_t capacity_ = 1;
    std::vector<std::optional<Message>> ring_;

    // The producer's cursor and the consumer's cursor. Each is aligned to its
    // own cache line so that posting and popping never invalidate each other's
    // line.
    CRPC_CACHE_LINE_ALIGN std::atomic<std::size_t> head_{0};
    CRPC_CACHE_LINE_ALIGN std::atomic<std::size_t> tail_{0};
};

// The documented multi-producer fallback. One mutex serialises both sides, so
// the bounded SPSC queue underneath is only ever touched by one thread at a
// time. The policy is unchanged: a full queue refuses with resource_exhausted.
template <typename Message>
class mpsc_mailbox {
public:
    explicit mpsc_mailbox(std::size_t capacity) : box_(capacity) {}

    mpsc_mailbox(const mpsc_mailbox&) = delete;
    mpsc_mailbox& operator=(const mpsc_mailbox&) = delete;
    mpsc_mailbox(mpsc_mailbox&&) = delete;
    mpsc_mailbox& operator=(mpsc_mailbox&&) = delete;

    [[nodiscard]] status try_post(Message m) {
        std::lock_guard<std::mutex> lock(mutex_);
        return box_.try_post(std::move(m));
    }

    [[nodiscard]] bool try_pop(Message& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        return box_.try_pop(out);
    }

    [[nodiscard]] std::optional<Message> try_pop_value() {
        std::lock_guard<std::mutex> lock(mutex_);
        return box_.try_pop_value();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return box_.size();
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return box_.capacity(); }

    // Diagnostics: forwarded to the underlying SPSC queue, whose cursor
    // alignment is what the cache-line test pins.
    [[nodiscard]] const void* head_cursor_address() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return box_.head_cursor_address();
    }
    [[nodiscard]] const void* tail_cursor_address() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return box_.tail_cursor_address();
    }

private:
    mutable std::mutex mutex_;
    mailbox<Message> box_;
};

} // namespace actor
} // namespace crpc

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#endif // CRPC_ACTOR_MAILBOX_HPP
