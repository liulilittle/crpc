// CRPC -- arena (bump) allocator.
//
// A bump allocator for per-frame / per-request temporary memory. One large
// block (or a few) is carved up in strict sequence, an allocate() is just an
// alignment-adjusted pointer add, and whole-block reuse happens at reset(): the
// used cursor of every normal block is rewound to zero instead of the memory
// being returned to the system. There is no per-object free; a caller gives
// everything back with one `reset()`.
//
// Semantics that matter to a caller:
//
//   * objects are not constructed or destroyed. `allocate()` returns raw,
//     suitably-aligned storage. Use it for trivially-destructible data (the
//     common per-frame case), or destroy objects in reverse order yourself.
//   * a request larger than one block (`options::block_bytes`) is served from
//     a dedicated block sized for the request and freed at reset(), because
//     keeping variably-sized blocks around for reuse is almost never worth the
//     retained memory.
//   * `reset()` reuses every normal block and releases every dedicated block.
//     After a reset the live counters (`live_allocations`, `used`) are zero.
//   * `alignment` must be a power of two; invalid arguments and failures
//     return nullptr. The arena never throws and never calls back into its
//     caller.
//   * the arena is NOT thread-safe. It is intended to be owned by one worker
//     (one frame, one request processor). A caller that shares an arena across
//     threads must provide its own synchronisation.
//
// The statistics exist so a caller can prove where the memory came from: e.g.
// a `pool_allocator<T>` backed by this arena shows `used` / `live_allocations`
// move when a `std::vector` grows and return to zero after `reset()`.
#ifndef CRPC_MEMORY_ARENA_HPP
#define CRPC_MEMORY_ARENA_HPP

#include <cstddef>
#include <cstdint>
#include <memory>

#include <crpc/config.hpp>

namespace crpc {
namespace memory {

struct arena_options {
    // Size of each normal block the arena carves up. Requests larger than
    // this are served from a dedicated block that is freed at reset().
    std::size_t block_bytes = 64 * 1024;
};

class CRPC_API arena {
public:
    explicit arena(arena_options opt = arena_options{});
    ~arena();

    arena(const arena&) = delete;
    arena& operator=(const arena&) = delete;

    // Returns `bytes` of aligned storage. `alignment` must be a power of two.
    // Returns nullptr on invalid arguments, on allocation failure and for a
    // zero-byte request. Never throws.
    void* allocate(std::size_t bytes,
                   std::size_t alignment = alignof(std::max_align_t));

    // Rewinds every normal block for reuse and releases every dedicated
    // block. Live counters return to zero. Existing pointers must not be
    // dereferenced afterwards.
    void reset() noexcept;

    // --- Statistics ---------------------------------------------------------
    // All reads are single-threaded snapshots (the arena is not thread-safe).

    // Configured size of a normal block.
    [[nodiscard]] std::size_t block_bytes() const noexcept;

    // Total bytes currently held in all blocks (normal + dedicated).
    [[nodiscard]] std::size_t capacity() const noexcept;

    // Bytes handed out since the last reset().
    [[nodiscard]] std::size_t used() const noexcept;

    // allocate() calls served since the last reset(). Zero after reset().
    [[nodiscard]] std::uint64_t live_allocations() const noexcept;

    // allocate() calls served since construction. Never reset.
    [[nodiscard]] std::uint64_t total_allocations() const noexcept;

    // Number of underlying blocks currently held (normal + dedicated).
    [[nodiscard]] std::size_t block_count() const noexcept;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace memory
} // namespace crpc

#endif // CRPC_MEMORY_ARENA_HPP