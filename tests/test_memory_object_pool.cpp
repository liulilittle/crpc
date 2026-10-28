// CRPC -- tests for the fixed-size object pool.
#include <crpc/memory/object_pool.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "test_framework.hpp"

using namespace crpc;
using namespace crpc::memory;

namespace {

// A type that counts construction and destruction, so a test can prove that
// every pool-constructed object is destroyed exactly once.
class counted {
public:
    counted() {
        ++constructed;
        ++alive;
        value = 33;
    }
    ~counted() {
        ++destructed;
        --alive;
    }
    int value = 0;

    static int constructed;
    static int destructed;
    static int alive;
};
int counted::constructed = 0;
int counted::destructed = 0;
int counted::alive = 0;

// C4324 fires for any over-aligned struct because the compiler must pad it;
// here the padding is the whole point, so the warning is noise.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
struct alignas(64) over_align {
    int x = 7;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

bool is_aligned_to(const void* p, std::size_t alignment) {
    return reinterpret_cast<std::uintptr_t>(p) % alignment == 0;
}

} // namespace

CRPC_TEST(pool_acquire_and_raii_return) {
    object_pool<int> pool(object_pool<int>::options{4, false});
    CRPC_CHECK_EQ(static_cast<std::size_t>(4), pool.capacity());
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), pool.in_use());
    CRPC_CHECK_EQ(static_cast<std::size_t>(4), pool.available());

    {
        object_pool<int>::handle h = pool.acquire().value();
        CRPC_CHECK(h.valid());
        CRPC_CHECK(h.get() != nullptr);
        *h = 42;
        CRPC_CHECK_EQ(42, *h);
        CRPC_CHECK_EQ(static_cast<std::size_t>(1), pool.in_use());
        CRPC_CHECK_EQ(static_cast<std::size_t>(3), pool.available());
    } // h destructs here and returns the object
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), pool.in_use());
    CRPC_CHECK_EQ(static_cast<std::size_t>(4), pool.available());
}

CRPC_TEST(pool_early_return_still_returns) {
    // The point: a function that returns before explicitly releasing must not
    // leak the object. The RAII handle does the return on the early path.
    int checked = 0;
    auto worker = [&]() {
        object_pool<int> pool(object_pool<int>::options{8, false});
        auto borrow = [&](int i) {
            object_pool<int>::handle h = pool.acquire().value();
            *h = i;
            if (i == 3) {
                return; // early return: handle destructs, object returns
            }
            *h += 1;
            ++checked;
        };
        for (int i = 0; i < 40; ++i) {
            borrow(i % 4);
        }
        CRPC_CHECK_EQ(static_cast<std::size_t>(0), pool.in_use());
        CRPC_CHECK_EQ(pool.capacity(), pool.available());
        CRPC_CHECK_EQ(pool.total_acquires(), pool.total_releases());
    };
    worker();
    CRPC_CHECK_EQ(30, checked);
}

CRPC_TEST(pool_move_handle_transfers_ownership) {
    object_pool<int> pool(object_pool<int>::options{2, false});
    object_pool<int>::handle h1 = pool.acquire().value();
    *h1 = 5;
    object_pool<int>::handle h2 = std::move(h1);
    CRPC_CHECK_FALSE(h1.valid());
    CRPC_CHECK(h2.valid());
    CRPC_CHECK_EQ(5, *h2);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), pool.in_use());
    CRPC_CHECK(h2.release().ok());
    CRPC_CHECK_FALSE(h2.valid());
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), pool.in_use());
    CRPC_CHECK_EQ(static_cast<std::size_t>(2), pool.available());
}

CRPC_TEST(pool_exhaustion_returns_an_error) {
    object_pool<int> pool(object_pool<int>::options{2, false});
    object_pool<int>::handle h1 = pool.acquire().value();
    object_pool<int>::handle h2 = pool.acquire().value();

    result<object_pool<int>::handle> r3 = pool.acquire();
    CRPC_CHECK_FALSE(r3.ok());
    CRPC_CHECK_EQ(rpc_code::resource_exhausted, r3.code());

    // Releasing one object makes one acquire succeed again.
    CRPC_CHECK(h1.release().ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), pool.in_use());
    object_pool<int>::handle h3 = pool.acquire().value();
    CRPC_CHECK(h3.valid());
    *h3 = 9;
    CRPC_CHECK_EQ(9, *h3);
    CRPC_CHECK_EQ(static_cast<std::size_t>(2), pool.in_use());
}

CRPC_TEST(pool_blocking_acquire_waits_for_release) {
    object_pool<int> pool(object_pool<int>::options{1, true});
    object_pool<int>::handle h1 = pool.acquire().value();

    std::atomic<bool> in_acquire{false};
    std::atomic<bool> completed{false};
    std::atomic<bool> result_ok{false};
    std::atomic<int> got{0};
    std::thread worker([&]() {
        in_acquire = true;
        result<object_pool<int>::handle> r = pool.acquire();
        result_ok = r.ok();
        if (r.ok()) {
            object_pool<int>::handle h2 = std::move(r.value());
            got = *h2;
        }
        completed = true;
    });

    CRPC_CHECK(crpc_test::wait_for([&]() { return in_acquire.load(); }, 5000));
    // The only slot is held; the worker must be blocked, not complete.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CRPC_CHECK_FALSE(completed.load());

    CRPC_CHECK(h1.release().ok());
    CRPC_CHECK(crpc_test::wait_for([&]() { return completed.load(); }, 5000));
    worker.join();
    CRPC_CHECK(result_ok.load());
    CRPC_CHECK_EQ(0, got.load());
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), pool.in_use());
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), pool.available());
}

CRPC_TEST(pool_statistics_are_self_consistent) {
    object_pool<int> pool(object_pool<int>::options{3, false});
    object_pool<int>::handle h1 = pool.acquire().value();
    object_pool<int>::handle h2 = pool.acquire().value();
    CRPC_CHECK_EQ(static_cast<std::size_t>(2), pool.in_use());
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), pool.available());
    CRPC_CHECK_EQ(static_cast<std::size_t>(2), pool.peak_in_use());
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(2), pool.total_acquires());
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(0), pool.total_releases());

    CRPC_CHECK(h1.release().ok());
    CRPC_CHECK(h2.release().ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), pool.in_use());
    CRPC_CHECK_EQ(static_cast<std::size_t>(3), pool.available());
    CRPC_CHECK_EQ(static_cast<std::size_t>(2), pool.peak_in_use());
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(2), pool.total_releases());

    // The watermark is reached again and never drops below it afterwards.
    object_pool<int>::handle h3 = pool.acquire().value();
    object_pool<int>::handle h4 = pool.acquire().value();
    object_pool<int>::handle h5 = pool.acquire().value();
    CRPC_CHECK_EQ(static_cast<std::size_t>(3), pool.in_use());
    CRPC_CHECK_EQ(static_cast<std::size_t>(3), pool.peak_in_use());
    CRPC_CHECK(h3.release().ok());
    CRPC_CHECK(h4.release().ok());
    CRPC_CHECK(h5.release().ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(3), pool.peak_in_use());
}

CRPC_TEST(pool_double_release_is_detected) {
    object_pool<int> pool(object_pool<int>::options{4, false});
    object_pool<int>::handle h = pool.acquire().value();

    CRPC_CHECK(h.release().ok());
    // A second release of the same handle must be reported, not silently
    // corrupt the free list.
    status s2 = h.release();
    CRPC_CHECK_FALSE(s2.ok());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, s2.code());
    status s3 = h.release();
    CRPC_CHECK_FALSE(s3.ok());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, s3.code());

    // The pool is still consistent: one slot in use was returned once.
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), pool.in_use());
    CRPC_CHECK_EQ(static_cast<std::size_t>(4), pool.available());
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(1), pool.total_acquires());
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(1), pool.total_releases());

    // Releasing the same slot again through the pool API is also an error.
    object_pool<int>::handle h2 = pool.acquire().value();
    CRPC_CHECK(pool.release(h2).ok());
    CRPC_CHECK_FALSE(pool.release(h2).ok());
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(2), pool.total_acquires());
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(2), pool.total_releases());
}

CRPC_TEST(pool_foreign_handle_is_rejected) {
    object_pool<int> pool_a(object_pool<int>::options{2, false});
    object_pool<int> pool_b(object_pool<int>::options{2, false});
    object_pool<int>::handle ha = pool_a.acquire().value();
    object_pool<int>::handle hb = pool_b.acquire().value();

    // Returning pool A's handle into pool B must be an error, and must not
    // consume the handle (it still belongs to A).
    status cross = pool_b.release(ha);
    CRPC_CHECK_FALSE(cross.ok());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, cross.code());
    CRPC_CHECK(ha.valid());
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), pool_a.in_use());
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), pool_b.in_use());

    status cross_rev = pool_a.release(hb);
    CRPC_CHECK_FALSE(cross_rev.ok());
    CRPC_CHECK(hb.valid());

    // Each handle returns to its own pool.
    CRPC_CHECK(pool_a.release(ha).ok());
    CRPC_CHECK(pool_b.release(hb).ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), pool_a.in_use());
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), pool_b.in_use());
}

CRPC_TEST(pool_capacity_zero_always_exhausts) {
    object_pool<int> pool(object_pool<int>::options{0, false});
    for (int i = 0; i < 5; ++i) {
        result<object_pool<int>::handle> r = pool.acquire();
        CRPC_CHECK_FALSE(r.ok());
        CRPC_CHECK_EQ(rpc_code::resource_exhausted, r.code());
    }

    // Blocking mode with zero capacity must not deadlock; it reports
    // exhaustion immediately.
    object_pool<int> pool_b(object_pool<int>::options{0, true});
    result<object_pool<int>::handle> r = pool_b.acquire();
    CRPC_CHECK_FALSE(r.ok());
    CRPC_CHECK_EQ(rpc_code::resource_exhausted, r.code());
}

CRPC_TEST(pool_capacity_one_reuses_the_slot) {
    object_pool<int> pool(object_pool<int>::options{1, false});
    for (int i = 0; i < 100; ++i) {
        object_pool<int>::handle h = pool.acquire().value();
        *h = i;
        CRPC_CHECK_EQ(i, *h);
        CRPC_CHECK_EQ(static_cast<std::size_t>(1), pool.in_use());
        CRPC_CHECK(h.release().ok());
        CRPC_CHECK_EQ(static_cast<std::size_t>(0), pool.in_use());
    }
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(100), pool.total_acquires());
    CRPC_CHECK_EQ(static_cast<std::uint64_t>(100), pool.total_releases());
}

CRPC_TEST(pool_destruction_with_outstanding_handles_is_safe) {
    counted::constructed = 0;
    counted::destructed = 0;
    counted::alive = 0;

    std::shared_ptr<object_pool<counted>> pool(
        new object_pool<counted>(object_pool<counted>::options{4, false}));
    object_pool<counted>::handle h1 = pool->acquire().value();
    object_pool<counted>::handle h2 = pool->acquire().value();
    CRPC_CHECK_EQ(2, counted::alive);
    CRPC_CHECK_EQ(static_cast<std::size_t>(2), pool->in_use());

    pool.reset(); // destroys the pool while two handles are still outstanding
    CRPC_CHECK(h1.valid());
    CRPC_CHECK(h2.valid());
    CRPC_CHECK_EQ(33, h1->value);
    CRPC_CHECK_EQ(2, counted::alive); // the outstanding objects were not freed

    // Releasing into a destroyed pool is safe and still destroys the object.
    CRPC_CHECK(h1.release().ok());
    CRPC_CHECK(h2.release().ok());
    CRPC_CHECK_EQ(0, counted::alive);
    CRPC_CHECK_EQ(2, counted::constructed);
    CRPC_CHECK_EQ(2, counted::destructed);
    // Everything constructed was destroyed exactly once: nothing leaked and
    // nothing was destroyed twice.
    CRPC_CHECK_EQ(0, counted::constructed - counted::destructed);
}

CRPC_TEST(pool_object_lifecycle_is_exact) {
    counted::constructed = 0;
    counted::destructed = 0;
    counted::alive = 0;
    {
        object_pool<counted> pool(object_pool<counted>::options{3, false});
        for (int i = 0; i < 10; ++i) {
            object_pool<counted>::handle h = pool.acquire().value();
            CRPC_CHECK_EQ(33, h->value);
            h->value = 5;
            CRPC_CHECK_EQ(5, h->value);
        }
        CRPC_CHECK_EQ(0, counted::alive);
    }
    // Every construction matched a destruction; nothing leaked.
    CRPC_CHECK_EQ(counted::constructed, counted::destructed);
}

CRPC_TEST(pool_respects_object_alignment) {
    object_pool<over_align> pool(object_pool<over_align>::options{4, false});
    for (int i = 0; i < 200; ++i) {
        object_pool<over_align>::handle h = pool.acquire().value();
        CRPC_CHECK(is_aligned_to(h.get(), alignof(over_align)));
        CRPC_CHECK_EQ(7, h->x);
    }
}

CRPC_TEST_MAIN()