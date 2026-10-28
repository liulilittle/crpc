// CRPC -- tests for the arena (bump) allocator.
#include <crpc/memory/arena.hpp>

#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "test_framework.hpp"

using namespace crpc;
using namespace crpc::memory;

namespace {

bool is_aligned_to(const void* p, std::size_t alignment) {
    return reinterpret_cast<std::uintptr_t>(p) % alignment == 0;
}

bool ranges_overlap(const void* a, std::size_t a_size, const void* b, std::size_t b_size) {
    const std::uintptr_t a_lo = reinterpret_cast<std::uintptr_t>(a);
    const std::uintptr_t b_lo = reinterpret_cast<std::uintptr_t>(b);
    const std::uintptr_t a_hi = a_lo + a_size;
    const std::uintptr_t b_hi = b_lo + b_size;
    return a_lo < b_hi && b_lo < a_hi;
}

} // namespace

CRPC_TEST(arena_basic_bump_allocations) {
    arena a(arena_options{4096});
    void* p1 = a.allocate(128, 8);
    void* p2 = a.allocate(128, 8);
    void* p3 = a.allocate(256, 8);
    CRPC_CHECK(p1 != nullptr);
    CRPC_CHECK(p2 != nullptr);
    CRPC_CHECK(p3 != nullptr);
    CRPC_CHECK(is_aligned_to(p1, 8));
    CRPC_CHECK(is_aligned_to(p2, 8));
    CRPC_CHECK(is_aligned_to(p3, 8));
    CRPC_CHECK_FALSE(ranges_overlap(p1, 128, p2, 128));
    CRPC_CHECK_FALSE(ranges_overlap(p2, 128, p3, 256));

    // The bump allocations are contiguous (p2 follows p1).
    CRPC_CHECK_EQ(reinterpret_cast<std::uintptr_t>(p2),
                  reinterpret_cast<std::uintptr_t>(p1) + 128);

    std::memset(p1, 0xAB, 128);
    CRPC_CHECK_EQ(static_cast<unsigned char>(0xAB),
                  static_cast<unsigned char*>(p1)[0]);
    CRPC_CHECK_EQ(static_cast<unsigned char>(0xAB),
                  static_cast<unsigned char*>(p1)[127]);
}

CRPC_TEST(arena_zero_and_bad_alignment_refuse) {
    arena a(arena_options{1024});
    CRPC_CHECK_EQ(nullptr, a.allocate(0, 8));
    CRPC_CHECK_EQ(nullptr, a.allocate(64, 0));
    CRPC_CHECK_EQ(nullptr, a.allocate(64, 3)); // not a power of two
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(0), a.live_allocations());
}

CRPC_TEST(arena_honours_requested_alignments) {
    arena a(arena_options{4096});
    // First a size-37 allocation with align 1 shifts the bump cursor to a
    // guaranteed-misaligned address, then the aligned one must still honour 8
    // and 64-byte alignment.
    void* p0 = a.allocate(37, 1);
    CRPC_CHECK(p0 != nullptr);
    void* p1 = a.allocate(200, 8);
    CRPC_CHECK(is_aligned_to(p1, 8));
    void* p2 = a.allocate(200, 64);
    CRPC_CHECK(is_aligned_to(p2, 64));
    void* p3 = a.allocate(200, 8);
    CRPC_CHECK(is_aligned_to(p3, 8));
    CRPC_CHECK_FALSE(ranges_overlap(p1, 200, p2, 200));
    CRPC_CHECK_FALSE(ranges_overlap(p2, 200, p3, 200));
}

CRPC_TEST(arena_large_objects_fallback_to_dedicated_blocks) {
    // Requests beyond the block size must be served by a dedicated block.
    const std::size_t block_bytes = 1024;
    arena a(arena_options{block_bytes});
    const std::size_t before = a.block_count();

    void* big = a.allocate(4 * block_bytes + 123, 16);
    CRPC_CHECK(big != nullptr);
    CRPC_CHECK(is_aligned_to(big, 16));
    std::memset(big, 0x5A, 4 * block_bytes + 123);
    CRPC_CHECK_EQ(static_cast<unsigned char>(0x5A),
                  static_cast<unsigned char*>(big)[4 * block_bytes + 122]);

    // A dedicated block was added.
    CRPC_CHECK_EQ(before + 1, a.block_count());
    CRPC_CHECK(a.used() >= 4 * block_bytes + 123);

    a.reset();
    // After reset the dedicated block is released; only the normal-block
    // count may remain.
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(0), a.live_allocations());
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), a.used());
}

CRPC_TEST(arena_reset_reuses_blocks_and_zeroes_counts) {
    arena a(arena_options{1024});
    const std::size_t max_blocks = 30; // 30 * 300-byte allocs -> up to 10 blocks

    std::size_t grown = 0;
    for (int cycle = 0; cycle < 5; ++cycle) {
        for (std::size_t i = 0; i < max_blocks; ++i) {
            void* p = a.allocate(300, 8);
            CRPC_CHECK(p != nullptr);
            CRPC_CHECK(is_aligned_to(p, 8));
        }
        CRPC_CHECK_EQ(static_cast<std::uint64_t>(max_blocks), a.live_allocations());
        if (cycle == 0) {
            grown = a.block_count();
            CRPC_CHECK(grown > 1); // the first cycle had to grow blocks
        }
        a.reset();
        CRPC_CHECK_EQ(static_cast<std::uint64_t>(0), a.live_allocations());
        CRPC_CHECK_EQ(static_cast<std::size_t>(0), a.used());
        CRPC_CHECK_EQ(grown, a.block_count()); // reuse, never grow again
    }
    // The cumulative counter keeps going; the live counters reset.
    CRPC_CHECK(a.total_allocations() >= max_blocks * 5);
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(0), a.live_allocations());
}

CRPC_TEST(arena_cumulative_vs_live_statistics) {
    arena a(arena_options{4096});
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(0), a.live_allocations());
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(0), a.total_allocations());

    a.allocate(100, 8);
    a.allocate(200, 8);
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(2), a.live_allocations());
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(2), a.total_allocations());
    CRPC_CHECK_EQ(static_cast<std::size_t>(300), a.used());

    a.allocate(50, 1);
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(3), a.live_allocations());
    CRPC_CHECK_EQ(static_cast<std::size_t>(350), a.used());

    a.reset();
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(0), a.live_allocations());
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), a.used());
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(3), a.total_allocations());

    a.allocate(10, 8);
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(1), a.live_allocations());
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(4), a.total_allocations());
}

CRPC_TEST(arena_used_never_exceeds_capacity) {
    arena a(arena_options{512});
    for (int i = 0; i < 100; ++i) {
        a.allocate(64, 8);
        CRPC_CHECK(a.used() <= a.capacity());
    }
    CRPC_CHECK(a.capacity() >= 512);
    a.reset();
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), a.used());
    CRPC_CHECK(a.capacity() >= 512);
}

CRPC_TEST(arena_write_round_trip_across_growth) {
    arena a(arena_options{128});
    // A growing number of writes must not ever collide or be overwritten.
    std::vector<void*> ptrs;
    for (std::size_t i = 0; i < 64; ++i) {
        void* p = a.allocate(200, 8); // 200 > 128: forces dedicated blocks
        CRPC_CHECK(p != nullptr);
        CRPC_CHECK(is_aligned_to(p, 8));
        ptrs.push_back(p);
    }
    // Every pointer is distinct (no overlap).
    for (std::size_t i = 0; i < ptrs.size(); ++i) {
        for (std::size_t j = i + 1; j < ptrs.size(); ++j) {
            CRPC_CHECK_FALSE(ranges_overlap(ptrs[i], 200, ptrs[j], 200));
        }
    }
    a.reset();
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(0), a.live_allocations());
}

CRPC_TEST(arena_huge_alignment_does_not_wrap) {
    arena a(arena_options{1024});
    const std::size_t huge = std::numeric_limits<std::size_t>::max() / 2;
    void* p = a.allocate(huge, 8);
    CRPC_CHECK_EQ(nullptr, p); // refuses instead of wrapping
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(0), a.live_allocations());
}

CRPC_TEST_MAIN()