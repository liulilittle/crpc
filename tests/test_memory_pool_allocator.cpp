// CRPC -- tests for the arena-backed STL allocator.
#include <crpc/memory/pool_allocator.hpp>

#include <list>
#include <string>
#include <vector>

#include "test_framework.hpp"

using namespace crpc;
using namespace crpc::memory;

namespace {

template <typename T>
using pa = pool_allocator<T>;

} // namespace

CRPC_TEST(pa_vector_growth_comes_from_the_pool) {
    arena src(arena_options{4096});
    const std::uint64_t allocs_before = src.total_allocations();

    pa<int> alloc(src);
    std::vector<int, pa<int>> v(alloc);
    for (int i = 0; i < 10000; ++i) {
        v.push_back(i);
    }
    CRPC_CHECK_EQ(static_cast<std::size_t>(10000), v.size());
    for (int i = 0; i < 10000; ++i) {
        CRPC_CHECK_EQ(i, v[i]);
    }

    // The vector's memory provably came from the arena: the arena allocation
    // counters moved while the vector grew.
    CRPC_CHECK(src.total_allocations() > allocs_before);
    CRPC_CHECK(src.live_allocations() > 0);
    CRPC_CHECK(src.used() >= static_cast<std::size_t>(10000) * sizeof(int));

    // Destroying the vector does not free back into the arena (bump
    // semantics: everything is released together at reset).
    std::size_t used_before = src.used();
    v.clear();
    std::size_t cap = v.capacity();
    (void)cap;
    CRPC_CHECK_EQ(used_before, src.used());

    src.reset();
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(0), src.live_allocations());
}

CRPC_TEST(pa_string_uses_the_pool_beyond_sso) {
    arena src(arena_options{4096});
    const std::uint64_t allocs_before = src.total_allocations();

    pa<char> alloc(src);
    std::basic_string<char, std::char_traits<char>, pa<char>> s(alloc);
    for (int i = 0; i < 200; ++i) {
        s.append("0123456789012345678901234567890123456789"); // 40 bytes
    }
    CRPC_CHECK_EQ(static_cast<std::size_t>(8000), s.size());

    CRPC_CHECK(src.total_allocations() > allocs_before);
    CRPC_CHECK(src.live_allocations() > 0);
    CRPC_CHECK(s.compare(0, 10, "0123456789") == 0);
}

CRPC_TEST(pa_list_rebind_works) {
    arena src(arena_options{4096});
    const std::uint64_t allocs_before = src.total_allocations();

    // std::list<U> rebinds the allocator to its internal node type; the pool
    // allocator must support the rebind and construct node storage from it.
    pa<int> alloc(src);
    std::list<int, pa<int>> lst(alloc);
    for (int i = 0; i < 5000; ++i) {
        lst.push_back(i);
    }
    CRPC_CHECK_EQ(static_cast<std::size_t>(5000), lst.size());
    int expected = 0;
    for (int value : lst) {
        CRPC_CHECK_EQ(expected, value);
        ++expected;
    }

    CRPC_CHECK(src.total_allocations() > allocs_before);
    CRPC_CHECK(src.live_allocations() >= 5000);
    src.reset();
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(0), src.live_allocations());
}

CRPC_TEST(pa_default_allocator_owns_an_arena) {
    std::vector<int, pa<int>> v;
    for (int i = 0; i < 1000; ++i) {
        v.push_back(i * 3);
    }
    CRPC_CHECK_EQ(static_cast<std::size_t>(1000), v.size());
    for (int i = 0; i < 1000; ++i) {
        CRPC_CHECK_EQ(i * 3, v[i]);
    }
}

CRPC_TEST(pa_allocator_equality_follows_the_arena) {
    arena src(arena_options{1024});
    pa<int> a1(src);
    pa<int> a2(src);
    pa<double> a3(src); // cross-type with the same arena
    CRPC_CHECK(a1 == a2);
    CRPC_CHECK(a1 == a3);
    CRPC_CHECK_FALSE(a1 != a2);

    pa<int> a4;
    // A copy must share its own arena. This compared `a4` with itself and left `a5`
    // unused, so the property named in the comment was never exercised: `a4 == a4` is
    // reflexive for any equality operator that is not outright broken, and it cannot
    // fail if the copy constructor hands out a fresh arena.
    pa<int> a5(a4);
    CRPC_CHECK(a4 == a5);
    CRPC_CHECK_FALSE(a1 == a4);         // borrowed != default-owned
    CRPC_CHECK(a1 != a4);
}

CRPC_TEST(pa_vector_reuse_after_arena_reset) {
    arena src(arena_options{4096});
    pa<int> alloc(src);
    {
        std::vector<int, pa<int>> v(alloc);
        for (int i = 0; i < 3000; ++i) {
            v.push_back(i);
        }
        CRPC_CHECK(src.live_allocations() > 0);
    }
    src.reset();
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(0), src.live_allocations());

    // After the reset the same arena serves a new vector again.
    std::vector<int, pa<int>> v2(alloc);
    for (int i = 0; i < 3000; ++i) {
        v2.push_back(i * 2);
    }
    CRPC_CHECK_EQ(static_cast<std::size_t>(3000), v2.size());
    for (int i = 0; i < 3000; ++i) {
        CRPC_CHECK_EQ(i * 2, v2[i]);
    }
    src.reset();
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(0), src.live_allocations());
}

CRPC_TEST(pa_zero_sized_allocation_is_safe) {
    arena src(arena_options{1024});
    pa<int> alloc(src);
    int* p = alloc.allocate(0);
    CRPC_CHECK_EQ(nullptr, p);
    alloc.deallocate(nullptr, 0); // no-op, must not crash
}

CRPC_TEST_MAIN()