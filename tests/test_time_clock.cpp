// CRPC -- wall-clock, tick and steady-clock reading tests.
//
// These cases describe the contract callers rely on, not a particular
// implementation: every reading is plausible for the current wall clock, the
// two clock families agree with the standard library within scheduling slack,
// ticks never go backwards, and the documented units mean what they say.
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <vector>

#include <crpc/time/clock.hpp>
#include <crpc/time/datetime.hpp>
#include <crpc/time/duration.hpp>

namespace {

using crpc::time::date_time;
using crpc::time::gmt_now;
using crpc::time::now_unix;
using crpc::time::process_tick;
using crpc::time::steady_frequency;
using crpc::time::steady_now_ticks;
using crpc::time::system_tick;
using crpc::time::to_timestamp;
using crpc::time::unix_time;
using crpc::time::utc_now;

constexpr std::uint32_t k_nanoseconds_per_second = 1000000000u;

// The contract is part of the ABI: exact return types, noexcept everywhere.
static_assert(std::is_same<decltype(now_unix()), unix_time>::value,
              "now_unix must return unix_time");
static_assert(std::is_same<decltype(utc_now()), date_time>::value,
              "utc_now must return date_time");
static_assert(std::is_same<decltype(gmt_now()), date_time>::value,
              "gmt_now must return date_time");
static_assert(std::is_same<decltype(system_tick()), std::uint64_t>::value,
              "system_tick must return uint64 milliseconds");
static_assert(std::is_same<decltype(process_tick()), std::uint64_t>::value,
              "process_tick must return uint64 nanoseconds");
static_assert(std::is_same<decltype(steady_now_ticks()), std::uint64_t>::value,
              "steady_now_ticks must return uint64 ticks");
static_assert(std::is_same<decltype(steady_frequency()), std::uint64_t>::value,
              "steady_frequency must return uint64 ticks per second");
static_assert(noexcept(now_unix()) && noexcept(utc_now()) && noexcept(gmt_now()),
              "wall-clock readers must be noexcept");
static_assert(noexcept(system_tick()) && noexcept(process_tick()),
              "tick readers must be noexcept");
static_assert(noexcept(steady_now_ticks()) && noexcept(steady_frequency()),
              "steady-clock readers must be noexcept");
static_assert(std::is_trivially_copyable<unix_time>::value,
              "unix_time is a plain value type crossing the ABI");

// The same instant read two ways cannot differ by more than this; the two
// reads are not atomic with respect to each other, so a second boundary may
// fall between them.
constexpr std::int64_t k_cross_read_slack_seconds = 2;

std::int64_t absolute_difference(std::int64_t a, std::int64_t b) noexcept {
    return a >= b ? a - b : b - a;
}

} // namespace

// --- Unix wall clock ---------------------------------------------------------

CRPC_TEST(unix_time_reads_the_current_wall_clock) {
    const auto before = std::chrono::system_clock::now().time_since_epoch();
    const unix_time value = now_unix();
    const auto after = std::chrono::system_clock::now().time_since_epoch();

    // Plausible for a machine running this suite: after 2020-09-13 and before
    // 2100-01-01. A wrong epoch (1601, 1970-relative-to-something-else) or a
    // unit mix-up lands outside this window.
    CRPC_CHECK(value.seconds >= 1600000000LL);
    CRPC_CHECK(value.seconds <= 4102444800LL);

    // Normalisation: the subsecond field is a fraction, never a second.
    CRPC_CHECK(value.nanoseconds < k_nanoseconds_per_second);

    const auto after_seconds = std::chrono::duration_cast<std::chrono::seconds>(after);
    const auto before_seconds = std::chrono::duration_cast<std::chrono::seconds>(before);
    CRPC_CHECK(absolute_difference(value.seconds, after_seconds.count()) <=
               k_cross_read_slack_seconds);
    CRPC_CHECK(absolute_difference(value.seconds, before_seconds.count()) <=
               k_cross_read_slack_seconds);
}

CRPC_TEST(utc_now_is_the_same_instant_with_offset_zero) {
    const unix_time unix_read = now_unix();
    const date_time utc = utc_now();

    CRPC_CHECK(utc.is_utc);
    CRPC_CHECK_EQ(0, utc.offset_minutes);
    CRPC_CHECK(utc.year >= 2020 && utc.year <= 2100);
    CRPC_CHECK(utc.nanosecond >= 0);
    CRPC_CHECK(utc.nanosecond < static_cast<int>(k_nanoseconds_per_second));

    const auto converted = to_timestamp(utc);
    CRPC_REQUIRE(converted.ok());
    CRPC_CHECK(absolute_difference(converted.value().seconds(), unix_read.seconds) <=
               k_cross_read_slack_seconds);
}

CRPC_TEST(gmt_now_uses_the_utc_time_scale) {
    const date_time gmt = gmt_now();
    CRPC_CHECK(gmt.is_utc);
    CRPC_CHECK_EQ(0, gmt.offset_minutes);
    CRPC_CHECK(gmt.year >= 2020 && gmt.year <= 2100);
    CRPC_CHECK(gmt.nanosecond >= 0);
    CRPC_CHECK(gmt.nanosecond < static_cast<int>(k_nanoseconds_per_second));

    const auto gmt_converted = to_timestamp(gmt);
    CRPC_REQUIRE(gmt_converted.ok());

    // GMT is the same time scale as UTC: two independent readings of the two
    // names must agree within the same slack as two Unix readings.
    const date_time utc = utc_now();
    const auto utc_converted = to_timestamp(utc);
    CRPC_REQUIRE(utc_converted.ok());
    CRPC_CHECK(absolute_difference(gmt_converted.value().seconds(),
                                   utc_converted.value().seconds()) <=
               k_cross_read_slack_seconds);

    const unix_time unix_read = now_unix();
    CRPC_CHECK(absolute_difference(gmt_converted.value().seconds(), unix_read.seconds) <=
               k_cross_read_slack_seconds);
}

// --- System tick ------------------------------------------------------------

CRPC_TEST(system_tick_is_monotonic_and_counted_in_milliseconds) {
    const std::uint64_t first = system_tick();
    const std::uint64_t second = system_tick();

    // Two consecutive readings never go backwards, and the counter has been
    // running since boot.
    CRPC_CHECK(first > 0);
    CRPC_CHECK(second >= first);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const std::uint64_t after_sleep = system_tick();
    CRPC_CHECK(after_sleep >= second);

    // 50 ms of wall time must show up as roughly 50 ticks. If the unit were
    // seconds the delta would be 0 or 1; if it were microseconds it would be
    // tens of thousands. The lower bound covers the coarsest Windows timer.
    const std::uint64_t delta = after_sleep - second;
    CRPC_CHECK(delta >= 30);
    CRPC_CHECK(delta <= 10000);
}

// --- Process tick -----------------------------------------------------------

CRPC_TEST(process_tick_counts_cpu_time_in_nanoseconds) {
    const std::uint64_t before = process_tick();

    // Burn CPU until 100 ms of wall time has passed. The process consumes the
    // same order of CPU time; the exact amount depends on scheduling, so the
    // assertion below is a floor, not an equality.
    volatile std::uint64_t sink = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (std::chrono::steady_clock::now() < deadline) {
        for (int i = 0; i < 10000; ++i) {
            sink = sink + static_cast<std::uint64_t>(i);
        }
    }
    const std::uint64_t observed = sink;

    const std::uint64_t after = process_tick();
    CRPC_CHECK(after >= before);
    const std::uint64_t consumed = after - before;

    // At least 20 ms of CPU time, expressed in nanoseconds. A millisecond or
    // 100 ns unit implementation cannot reach this floor after 100 ms of work.
    CRPC_CHECK(consumed >= 20000000ull);
    // Sanity ceiling: the case cannot consume ten seconds of CPU.
    CRPC_CHECK(consumed <= 10000000000ull);
    CRPC_CHECK(observed != 0xFFFFFFFFFFFFFFFFull);
}

// --- Steady clock -----------------------------------------------------------

CRPC_TEST(steady_clock_ticks_measure_elapsed_wall_time) {
    const std::uint64_t frequency = steady_frequency();
    CRPC_REQUIRE(frequency > 0);

    const std::uint64_t first = steady_now_ticks();
    const std::uint64_t second = steady_now_ticks();
    CRPC_CHECK(second >= first);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const std::uint64_t after_sleep = steady_now_ticks();
    CRPC_CHECK(after_sleep >= second);

    // ticks / frequency is a duration in seconds. This is the property RTT
    // measurement depends on; a wrong frequency breaks it immediately.
    const double elapsed = static_cast<double>(after_sleep - first) /
                           static_cast<double>(frequency);
    CRPC_CHECK(elapsed >= 0.005);
    CRPC_CHECK(elapsed <= 5.0);
}

// --- Thread safety ----------------------------------------------------------

CRPC_TEST(readers_are_safe_from_many_threads) {
    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    threads.reserve(4);
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&failed] {
            std::uint64_t last_system = system_tick();
            std::uint64_t last_steady = steady_now_ticks();
            for (int i = 0; i < 500; ++i) {
                const std::uint64_t system = system_tick();
                if (system < last_system) {
                    failed.store(true);
                }
                last_system = system;

                const std::uint64_t steady = steady_now_ticks();
                if (steady < last_steady) {
                    failed.store(true);
                }
                last_steady = steady;

                const unix_time unix_read = now_unix();
                if (unix_read.nanoseconds >= k_nanoseconds_per_second) {
                    failed.store(true);
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    CRPC_CHECK_FALSE(failed.load());
}

CRPC_TEST_MAIN()
