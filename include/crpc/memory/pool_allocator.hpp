// CRPC -- arena-backed STL allocator.
//
// A C++17 allocator that makes `std::vector<T, pool_allocator<T>>`,
// `std::basic_string<..., pool_allocator<...>>`, list/map nodes and any other
// standard container draw their storage from a crpc::memory::arena instead of
// from the global heap. It exists because "give every subsystem its own wheel"
// usually fails: this is the wheel, usable by any module directly.
//
// Allocation semantics (bump semantics inherited from the arena):
//
//   * allocate() bumps bytes out of the backing arena. `deallocate()` is a
//     no-op: the arena cannot hand individual ranges back. Everything is
//     reclaimed together by `arena::reset()` (or when the arena is destroyed).
//     This fits per-frame / per-request containers that are rebuilt every
//     frame: `arena_->reset()` at frame start and every container starts clean
//     without a single malloc.
//   * a default-constructed allocator owns a private arena (created on the
//     first allocation); a `pool_allocator<T>(arena&)` borrows the caller's
//     arena. Copies and rebinds share the same backing arena. Two allocators
//     compare equal iff they share an arena, so standard container
//     optimisations that rely on allocator equality keep working.
//   * thread safety follows the arena: an arena is single-threaded. A shared
//     arena needs external synchronisation.
//
// Exception boundary: the STL allocator contract requires allocate() to signal
// failure with bad_alloc, and this is the one place the memory layer throws.
// The arena itself never throws and returns nullptr on failure; this adapter
// converts that nullptr into a std::bad_alloc, exactly as std::allocator would
// on an exhausted heap.
#ifndef CRPC_MEMORY_POOL_ALLOCATOR_HPP
#define CRPC_MEMORY_POOL_ALLOCATOR_HPP

#include <cstddef>
#include <limits>
#include <memory>
#include <new>

#include <crpc/config.hpp>
#include <crpc/memory/arena.hpp>

namespace crpc {
namespace memory {

template <typename T>
class pool_allocator {
public:
    using value_type = T;

    // Owns a private arena. Distinct default-constructed allocators do not
    // compare equal (each spans its own arena). Can throw only bad_alloc under
    // process-memory exhaustion, while creating the arena.
    pool_allocator() : owned_(new arena()), arena_(owned_.get()) {}

    // Borrows `from`. The caller owns the arena and decides when to reset().
    explicit pool_allocator(arena& from) noexcept : arena_(&from) {}

    template <typename U>
    pool_allocator(const pool_allocator<U>& other) noexcept
        : owned_(other.owned_), arena_(other.arena_) {}

    [[nodiscard]] T* allocate(std::size_t n) {
        if (n == 0) {
            return nullptr;
        }
        if (n > max_size()) {
            throw std::bad_alloc(); // the request cannot even be measured
        }
        void* p = arena_->allocate(n * sizeof(T), alignof(T));
        if (p == nullptr) {
            throw std::bad_alloc();
        }
        return static_cast<T*>(p);
    }

    // No-op by design: the arena reclaims everything at reset().
    void deallocate(T*, std::size_t) noexcept {}

    [[nodiscard]] std::size_t max_size() const noexcept {
        return std::numeric_limits<std::size_t>::max() / sizeof(T);
    }

    template <typename U>
    struct rebind {
        using other = pool_allocator<U>;
    };

    // The arena this allocator draws from; nullptr never.
    [[nodiscard]] arena* underlying() const noexcept { return arena_; }

    template <typename U>
    friend class pool_allocator;

private:
    // Non-null when this instance (and its copies) owns the arena.
    std::shared_ptr<arena> owned_;
    arena* arena_ = nullptr;
};

template <typename T, typename U>
bool operator==(const pool_allocator<T>& a, const pool_allocator<U>& b) noexcept {
    return a.underlying() == b.underlying();
}

template <typename T, typename U>
bool operator!=(const pool_allocator<T>& a, const pool_allocator<U>& b) noexcept {
    return !(a == b);
}

} // namespace memory
} // namespace crpc

#endif // CRPC_MEMORY_POOL_ALLOCATOR_HPP