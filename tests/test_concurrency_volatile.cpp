// CRPC -- acquire/release accessors for ordinary memory.
//
// The release/acquire pair is exercised the way it is meant to be used: a
// non-atomic payload is written, then a separately stored flag is published
// with release semantics; the reader observes the flag with acquire semantics
// and then reads the payload. On x86 a broken ordering cannot be observed at
// run time (loads are not reordered with loads, stores with stores, and an
// aligned word access is atomic), so the barrier aspect is additionally
// covered by the ThreadSanitizer run recorded with the delivery: removing the
// acquire/release order turns the payload accesses into a reported data race.
//
// Round-trip cases pin the value semantics for every supported width (1/2/4/8)
// and for the common non-integral 4/8-byte types. The fences must at least be
// callable from ordinary code; their ordering effect is not observable in a
// process that has to keep running, and a wrong `seq_cst` vs `signal` choice
// is a compile-time/API concern rather than a runtime assertion.
#include "test_framework.hpp"

#include <chrono>
#include <cstdint>
#include <thread>

#include <crpc/concurrency/volatile.hpp>

namespace vc = crpc::concurrency;

namespace {

struct payload {
    std::uint64_t first;
    std::uint64_t second;
    std::uint64_t third;
};

bool same(const payload& lhs, const payload& rhs) {
    return lhs.first == rhs.first && lhs.second == rhs.second &&
           lhs.third == rhs.third;
}

// Spins on the flag with a hard budget. A bounded wait keeps a broken
// implementation from hanging the whole suite: the case then fails on the
// "observed" assertion instead.
bool wait_for_flag(const std::uint32_t& flag, std::chrono::milliseconds budget) {
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + budget;
    unsigned spins = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        if (vc::volatile_read(flag) != 0) {
            return true;
        }
        if (++spins < 4000) {
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return false;
}

} // namespace

CRPC_TEST(volatile_round_trips_every_supported_width) {
    std::uint8_t u8 = 0;
    vc::volatile_write(u8, std::uint8_t{0xA5});
    CRPC_CHECK_EQ(std::uint8_t{0xA5}, vc::volatile_read(u8));

    std::uint16_t u16 = 0;
    vc::volatile_write(u16, std::uint16_t{0xBEEF});
    CRPC_CHECK_EQ(std::uint16_t{0xBEEF}, vc::volatile_read(u16));

    std::uint32_t u32 = 0;
    vc::volatile_write(u32, std::uint32_t{0xDEADBEEF});
    CRPC_CHECK_EQ(std::uint32_t{0xDEADBEEF}, vc::volatile_read(u32));

    std::uint64_t u64 = 0;
    vc::volatile_write(u64, std::uint64_t{0x0123456789ABCDEFULL});
    CRPC_CHECK_EQ(std::uint64_t{0x0123456789ABCDEFULL}, vc::volatile_read(u64));

    std::int32_t i32 = 0;
    vc::volatile_write(i32, std::int32_t{-123456789});
    CRPC_CHECK_EQ(std::int32_t{-123456789}, vc::volatile_read(i32));

    float f = 0.0f;
    vc::volatile_write(f, 3.5f);
    CRPC_CHECK_EQ(3.5f, vc::volatile_read(f));

    double d = 0.0;
    vc::volatile_write(d, -2.25);
    CRPC_CHECK_EQ(-2.25, vc::volatile_read(d));

    bool b = true;
    vc::volatile_write(b, false);
    CRPC_CHECK_FALSE(vc::volatile_read(b));
    vc::volatile_write(b, true);
    CRPC_CHECK(vc::volatile_read(b));
}

CRPC_TEST(fences_are_callable) {
    std::uint32_t value = 0;
    std::thread writer([&value] {
        value = 7;
        vc::full_fence();
    });
    writer.join();

    vc::full_fence();
    vc::signal_fence();
    CRPC_CHECK_EQ(std::uint32_t{7}, vc::volatile_read(value));
}

CRPC_TEST(release_acquire_publishes_a_non_atomic_payload) {
    // Several rounds keep a scheduling accident in one round from looking like
    // a systematic ordering failure, and they also exercise the flag being
    // reset (through volatile_write) between publications.
    constexpr int rounds = 200;
    int failed_rounds = 0;
    int unpublished_rounds = 0;

    for (int round = 0; round < rounds; ++round) {
        payload data{0, 0, 0};
        std::uint32_t flag = 0;
        const payload expected{
            static_cast<std::uint64_t>(round) + 1,
            static_cast<std::uint64_t>(round) * 3 + 7,
            static_cast<std::uint64_t>(round) * 5 + 11};

        std::thread writer([&data, &flag, &expected] {
            data = expected;
            vc::volatile_write(flag, std::uint32_t{1});
        });

        payload observed{0, 0, 0};
        if (wait_for_flag(flag, std::chrono::milliseconds(5000))) {
            // Read after the acquire load; this is the access the fence has to
            // order after the flag observation.
            observed = data;
        } else {
            ++unpublished_rounds;
        }
        writer.join();

        if (!same(observed, expected)) {
            ++failed_rounds;
        }
    }

    CRPC_CHECK_EQ(0, unpublished_rounds);
    CRPC_CHECK_EQ(0, failed_rounds);
}

CRPC_TEST(writes_are_visible_to_a_second_thread) {
    // A single publication in the opposite direction (main thread writes,
    // helper thread reads) so the ordering is not only exercised one way.
    payload data{0, 0, 0};
    std::uint32_t flag = 0;
    payload observed{0, 0, 0};
    bool seen = false;

    std::thread reader([&flag, &data, &observed, &seen] {
        if (wait_for_flag(flag, std::chrono::milliseconds(5000))) {
            observed = data;
            seen = true;
        }
    });

    std::this_thread::yield();
    const payload expected{101, 202, 303};
    data = expected;
    vc::volatile_write(flag, std::uint32_t{1});
    reader.join();

    CRPC_CHECK(seen);
    CRPC_CHECK(same(observed, expected));
}

CRPC_TEST_MAIN()
