// CRPC -- arena (bump) allocator implementation.
#include "crpc/memory/arena.hpp"

#include <cstdint>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace crpc {
namespace memory {

namespace {

// Aligns `value` up to `alignment` (a power of two).
inline std::uintptr_t align_up(std::uintptr_t value, std::size_t alignment) {
    const std::uintptr_t mask =
        static_cast<std::uintptr_t>(alignment) - 1;
    return (value + mask) & ~mask;
}

} // namespace

struct arena::impl {
    // The implementation is deliberately simple and single-threaded: a list of
    // normal (block_bytes_) blocks that are bumped into and rewound on reset,
    // plus a list of dedicated blocks for oversized requests that are released
    // on reset.

    explicit impl(std::size_t block_bytes)
        : block_bytes_(block_bytes > 0 ? block_bytes : 4096) {}

    void* allocate(std::size_t bytes, std::size_t alignment) {
        if (bytes == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0) {
            return nullptr;
        }
        if (bytes > std::numeric_limits<std::size_t>::max() - alignment) {
            return nullptr; // requests near SIZE_MAX cannot be served safely
        }

        if (bytes > block_bytes_) {
            return allocate_dedicated(bytes, alignment);
        }

        // Carve out of the first normal block with room. After a reset() every
        // block is rewound, so later cycles reuse the same blocks instead of
        // growing the pool forever.
        for (block& b : normal_) {
            const std::uintptr_t aligned = try_bump(b, bytes, alignment);
            if (aligned != 0) {
                return record(bytes, aligned);
            }
        }
        std::unique_ptr<std::byte[]> data(
            new (std::nothrow) std::byte[block_bytes_]);
        if (data == nullptr) {
            return nullptr;
        }
        block b{std::move(data), block_bytes_, 0};
        const std::uintptr_t aligned = try_bump(b, bytes, alignment);
        if (aligned == 0) {
            return nullptr; // unreachable for bytes <= block_bytes_
        }
        normal_.push_back(std::move(b));
        return record(bytes, aligned);
    }

    void reset() noexcept {
        for (block& b : normal_) {
            b.used = 0;
        }
        huge_.clear();
        used_ = 0;
        live_allocations_ = 0;
    }

    std::size_t block_bytes() const noexcept { return block_bytes_; }
    std::size_t used() const noexcept { return used_; }
    std::uint64_t live_allocations() const noexcept { return live_allocations_; }
    std::uint64_t total_allocations() const noexcept { return total_allocations_; }

    std::size_t capacity() const noexcept {
        std::size_t total = 0;
        for (const block& b : normal_) {
            total += b.size;
        }
        for (const block& b : huge_) {
            total += b.size;
        }
        return total;
    }

    std::size_t block_count() const noexcept {
        return normal_.size() + huge_.size();
    }

private:
    struct block {
        std::unique_ptr<std::byte[]> data;
        std::size_t size = 0; // bytes of `data`
        std::size_t used = 0; // bytes handed out from this block
    };

    // Tries to bump `bytes` (aligned) out of `b`. Returns the aligned start
    // address, or 0 when the request does not fit.
    std::uintptr_t try_bump(block& b, std::size_t bytes,
                            std::size_t alignment) const {
        const std::uintptr_t base =
            reinterpret_cast<std::uintptr_t>(b.data.get());
        const std::uintptr_t aligned = align_up(base + b.used, alignment);
        const std::size_t offset = static_cast<std::size_t>(aligned - base);
        if (offset > b.size || bytes > b.size - offset) {
            return 0;
        }
        b.used = offset + bytes;
        return aligned;
    }

    void* allocate_dedicated(std::size_t bytes, std::size_t alignment) {
        // The extra `alignment` bytes guarantee the aligned range fits even
        // when the raw base address is misaligned.
        std::unique_ptr<std::byte[]> data(
            new (std::nothrow) std::byte[bytes + alignment]);
        if (data == nullptr) {
            return nullptr;
        }
        const std::uintptr_t base =
            reinterpret_cast<std::uintptr_t>(data.get());
        const std::uintptr_t aligned = align_up(base, alignment);
        const std::size_t offset = static_cast<std::size_t>(aligned - base);
        block b{std::move(data), bytes + alignment, offset + bytes};
        huge_.push_back(std::move(b));
        return record(bytes, aligned);
    }

    void* record(std::size_t bytes, std::uintptr_t aligned) noexcept {
        used_ += bytes;
        ++live_allocations_;
        ++total_allocations_;
        return reinterpret_cast<void*>(aligned);
    }

    std::size_t block_bytes_ = 0;
    std::vector<block> normal_;
    std::vector<block> huge_;
    std::size_t used_ = 0;
    std::uint64_t live_allocations_ = 0;
    std::uint64_t total_allocations_ = 0;
};

arena::arena(arena_options opt) : impl_(new impl(opt.block_bytes)) {}
arena::~arena() = default;

void* arena::allocate(std::size_t bytes, std::size_t alignment) {
    return impl_->allocate(bytes, alignment);
}

void arena::reset() noexcept { impl_->reset(); }

std::size_t arena::block_bytes() const noexcept { return impl_->block_bytes(); }
std::size_t arena::capacity() const noexcept { return impl_->capacity(); }
std::size_t arena::used() const noexcept { return impl_->used(); }
std::uint64_t arena::live_allocations() const noexcept {
    return impl_->live_allocations();
}
std::uint64_t arena::total_allocations() const noexcept {
    return impl_->total_allocations();
}
std::size_t arena::block_count() const noexcept { return impl_->block_count(); }

} // namespace memory
} // namespace crpc