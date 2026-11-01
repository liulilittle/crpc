// CRPC -- a full-duplex channel between two processes on one machine.
//
// This is the piece that makes `fs::shared_memory` and `ipc::ring_buffer` useful:
// one process creates a named channel, another joins it, and each side gets a
// send/receive pair. The transport is two single-producer/single-consumer rings in
// one mapping, so neither direction is multiplexed and each ring keeps the invariant
// its header argues for -- the creator produces into one and consumes from the other,
// and the joiner does the reverse. That is why this is not "one ring with a lock":
// a shared ring would have two producers, which is exactly what a SPSC ring cannot
// be.
//
// Layout of the mapping's payload
// -------------------------------
//   channel_header         magic, capacity, readiness
//   ring_control  outbound (creator -> joiner)
//   ring_control  inbound  (joiner -> creator)
//   outbound payload       capacity bytes
//   inbound payload        capacity bytes
//
// Every field is an integer; no pointers are stored, because the two processes map
// the region at unrelated addresses.
//
// Async first, and no blocking on the shard
// -----------------------------------------
// The transport is polled, not blocking: `try_send` / `try_receive` are the only
// operations and they never sleep. Everything above them is derived from those two,
// so a coroutine can drive a channel without ever occupying a shard thread:

//   * `receive(span, timeout)` / `send(span, timeout)` are the *synchronous* forms.
//     On a shard thread inside a coroutine they call `runtime::park()` and are
//     resumed by the peer's `notify()`; they fall back to a bounded wait only on a
//     plain thread, where blocking is what the caller asked for. They must never
//     spin or block on the shard thread -- that would stop every other task on it,
//     which is the deadlock this rule exists to prevent.
//   * `operator co_await` (C++20) suspends until data arrives and resumes with the
//     count. That is the form to prefer inside a coroutine.

// A channel cannot observe the other process's memory directly, so the notify that
// wakes a parked coroutine has to come from this side: whichever process is reading
// must be woken by its own event loop or a poller, not by the writer. `send` rings
// the local `runtime` (when one is bound) so a reader parked on the *same* runtime
// makes progress; a cross-process wake needs the poller the caller already runs for
// its sockets, and `poll_ready()` exists for that loop to test.

// The readiness flag, and why it exists
// -------------------------------------
// Creating a channel is several stores (two ring controls, two payload areas). A
// joiner that attaches in the middle would see a half-initialised region and could
// read a capacity that has not been published yet. So the creator writes everything
// else first and sets `ready` last with release ordering; a joiner acquire-loads it
// and gets `unavailable` -- "still being created, retry" -- rather than a silently
// wrong view. The name is visible as soon as the mapping exists, which is a much
// larger window than the initialisation itself.
#ifndef CRPC_IPC_CHANNEL_HPP
#define CRPC_IPC_CHANNEL_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <crpc/config.hpp>
#include <crpc/runtime.hpp>
#include <crpc/error.hpp>
#include <crpc/fs/shared_memory.hpp>
#include <crpc/ipc/ring_buffer.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace ipc {

struct channel_header {
    std::uint64_t magic = 0;
    // Bytes in each direction's ring. Both rings are the same size so a caller has
    // one number to reason about.
    std::uint64_t capacity = 0;
    // 0 while the creator is still writing the controls, 1 once the whole region is
    // usable. Published with release, read with acquire.
    std::atomic<std::uint64_t> ready{0};
};

inline constexpr std::uint64_t channel_magic = 0x4352504343484E31ull; // "CRPCCHN1"

// Bytes a channel reserves before the first payload byte.
inline constexpr std::size_t channel_fixed_size =
    sizeof(channel_header) + 2 * sizeof(ring_control);

// Number of payload bytes `create` must ask `shared_memory` for, given a ring
// capacity. Exposed so a caller sizing a mapping knows the real length.
[[nodiscard]] constexpr std::size_t channel_mapping_size(std::size_t capacity) noexcept {
    return channel_fixed_size + 2 * capacity;
}

// One end of a full-duplex channel. `send` writes to the peer, `receive` reads what
// the peer sent; a single end never does both to the same ring.
class CRPC_API channel {
public:
    channel() = default;
    ~channel();
    channel(const channel&) = delete;
    channel& operator=(const channel&) = delete;
    channel(channel&& other) noexcept;
    channel& operator=(channel&& other) noexcept;

    // Creates `name` and returns the creator's end. `capacity` is the ring size in
    // each direction; `failed_precondition` when the name already exists, so two
    // creators cannot silently share one channel.
    [[nodiscard]] static result<channel> create(std::string_view name,
                                                std::size_t capacity);

    // Attaches the other end to an existing channel. `not_found` when nobody created
    // it; `unavailable` when it exists but the creator has not finished publishing it
    // (retryable, and the only reason this can fail on a live channel).
    [[nodiscard]] static result<channel> join(std::string_view name);

    // --- Non-blocking, the primitives everything else is built on -------------

    // Queues `data` for the peer as one run. False when the ring is full: the caller
    // decides whether to retry, drop, or treat it as back-pressure. Never waits.
    // Rings the bound runtime so a reader parked on the same runtime wakes up; the
    // peer process cannot be woken from here (see the file comment).
    [[nodiscard]] bool try_send(span<const std::byte> data) noexcept;

    // Copies at most `out.size()` queued bytes into `out`; 0 when nothing is queued.
    // Never waits.
    [[nodiscard]] std::size_t try_receive(span<std::byte> out) noexcept;

    // --- Synchronous forms: park on a shard, wait on a plain thread -----------

    // These are the convenience forms for callers that are not driving the channel
    // from their own loop. Their blocking behaviour is what makes them unsafe to
    // call carelessly, so it is spelled out:
    //
    //   * on the shard thread inside a coroutine: `runtime::park()` in a loop,
    //     resumed by `runtime::notify()`. The thread is never held, so other tasks
    //     on the same shard keep running.
    //   * on a plain thread (or a non-coroutine task on the shard thread): a bounded
    //     wait with the given timeout, `deadline_exceeded` when it elapses.
    //
    // A zero timeout means "do not wait": both return the non-blocking result.
    [[nodiscard]] result<std::size_t> receive(span<std::byte> out,
                                              std::chrono::nanoseconds timeout);
    [[nodiscard]] status send(span<const std::byte> data,
                              std::chrono::nanoseconds timeout);

    // Binds a runtime so `send` can wake a reader parked on it, and so the blocking
    // forms know whether they may park. Without one, a channel is usable only from
    // the non-blocking forms or from a plain thread.
    void bind_runtime(runtime* shard) noexcept { shard_ = shard; }

    // True when data is queued. Exists so a caller's own event loop can test the
    // channel the way it tests a socket, instead of polling `try_receive`.
    [[nodiscard]] bool poll_ready() const noexcept { return inbound_.size() != 0; }

    // Queued bytes waiting for this end. A snapshot, exact only if no other thread
    // is consuming this direction.
    [[nodiscard]] std::size_t pending() const noexcept;

    // Bytes free in the outbound ring.
    [[nodiscard]] std::size_t send_capacity() const noexcept;

    [[nodiscard]] bool valid() const noexcept { return mapping_.valid(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }

private:
    // Re-binds the two views to the current mapping. Called after a move, because a
    // view holds raw pointers into the mapping and the address must be re-read rather
    // than assumed.
    void bind_views();

    fs::shared_memory mapping_;
    ring_view outbound_; // this end produces here
    ring_view inbound_;  // this end consumes here
    std::size_t capacity_ = 0;
    // Which end this is. A move must rebuild the views *in the same direction*, and
    // the direction is not derivable from the mapping -- the creator and the joiner
    // map the identical bytes and bind them the opposite way round.
    bool creator_ = false;
    // Optional; never owned. When set, a shard-thread wait parks instead of
    // blocking, and a send rings it.
    runtime* shard_ = nullptr;
    std::string name_;
};

} // namespace ipc
} // namespace crpc

#endif // CRPC_IPC_CHANNEL_HPP
