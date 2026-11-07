// CRPC -- high-precision clock readings: Unix/UTC/GMT wall time, system and
// process ticks, and a steady counter for elapsed-time measurement.
//
// This header is a pure addition to the time module: it defines new functions
// and one new value type and changes no existing public structure. Every
// function is `noexcept`, performs no allocation and is safe to call from any
// thread; the platform calls behind them are read-only clock queries.
//
// Two clock families are exposed and must not be mixed:
//
//   * wall clocks (`now_unix`, `utc_now`, `gmt_now`) read the system's civil
//     time. They can jump forwards or backwards when the time is adjusted, so
//     they are for timestamps, never for elapsed-time measurement.
//
//   * elapsed-time clocks (`system_tick`, `process_tick`,
//     `steady_now_ticks`) never go backwards on their own. `system_tick` and
//     `steady_now_ticks` measure wall time, `process_tick` measures CPU time.
//     Origins are unspecified; only differences are meaningful.
#ifndef CRPC_TIME_CLOCK_HPP
#define CRPC_TIME_CLOCK_HPP

#include <cstdint>

#include <crpc/config.hpp>
#include <crpc/time/datetime.hpp>

namespace crpc {
namespace time {

// An instant on the Unix timeline, split exactly the way the operating system
// reports it.
//
//   * `seconds`     -- whole seconds since 1970-01-01T00:00:00Z (the Unix
//                      epoch), signed so pre-epoch instants stay meaningful.
//   * `nanoseconds` -- the subsecond part, always in [0, 999999999].
//
// Precision, not accuracy, is the platform clock's: 100 ns units from the
// Windows FILETIME source and the full nanosecond field of
// `clock_gettime(CLOCK_REALTIME)` on Linux. No extra rounding is applied, and
// no value is ever reported as second 0 plus 1000000000 ns.
struct unix_time {
    std::int64_t seconds = 0;
    std::uint32_t nanoseconds = 0;

    constexpr unix_time() noexcept = default;
    constexpr unix_time(std::int64_t whole_seconds,
                        std::uint32_t subsecond_nanoseconds) noexcept
        : seconds(whole_seconds), nanoseconds(subsecond_nanoseconds) {}
};

// The current Unix time.
//
// Source and precision:
//   * Windows: `GetSystemTimePreciseAsFileTime` (FILETIME, 100 ns units,
//     backed by the precise interrupt-time clock; typically sub-millisecond).
//   * Linux:   `clock_gettime(CLOCK_REALTIME)`, with the full nanosecond of
//     the returned `timespec`.
//
// This is a wall clock: it follows system time adjustments and must not be
// used to measure elapsed time.
[[nodiscard]] CRPC_API unix_time now_unix() noexcept;

// The current UTC civil time: `is_utc == true`, `offset_minutes == 0`, and
// `nanosecond` in [0, 999999999]. Same instant and same platform source as
// `now_unix()`; the calendar fields are converted without allocation.
[[nodiscard]] CRPC_API date_time utc_now() noexcept;

// The current GMT civil time. GMT and UTC share one time scale here (both are
// offset 0 and read the same platform wall clock); the separate name exists so
// a call site can follow a protocol or specification that says "GMT" without
// implying a different value. Callers that need a time-zone designation
// should use `date_time::is_utc` / `offset_minutes`, which are identical for
// both functions.
[[nodiscard]] CRPC_API date_time gmt_now() noexcept;

// Milliseconds elapsed since the system's boot-time reference.
//
// Monotonicity: successive calls never go backwards within one boot, also
// across threads. The origin is unspecified (it is not the Unix epoch), so
// only differences are meaningful, and values from different machines or
// different boots are not comparable.
//
// Source and unit:
//   * Windows: `GetTickCount64()`, milliseconds.
//   * Linux:   `clock_gettime(CLOCK_MONOTONIC)` truncated to whole
//              milliseconds. CLOCK_MONOTONIC does not advance while the
//              machine is suspended.
[[nodiscard]] CRPC_API std::uint64_t system_tick() noexcept;

// Cumulative CPU time consumed by this process, in nanoseconds.
//
// This is the sum of user and kernel time of every thread the process has run;
// it advances only while the process executes and never goes backwards.
// Resolution is the platform's (100 ns units on Windows, the nominal
// `clock_gettime` resolution on Linux), so small reads may repeat.
//
// Source:
//   * Windows: `GetProcessTimes` user + kernel time, scaled from 100 ns to ns.
//   * Linux:   `clock_gettime(CLOCK_PROCESS_CPUTIME_ID)`.
[[nodiscard]] CRPC_API std::uint64_t process_tick() noexcept;

// A high-resolution steady counter for elapsed-time measurement (RTT and
// deadline maths).
//
// Contract:
//   * `steady_now_ticks()` is monotonically nondecreasing across calls and
//     threads, with an unspecified origin.
//   * `steady_frequency()` returns the number of ticks per second and is
//     strictly positive and constant for the lifetime of the process.
//   * elapsed seconds = (ticks_after - ticks_before) / steady_frequency().
//
// Never compare a steady tick with `now_unix()` or `system_tick()`.
//
// Source:
//   * Windows: `QueryPerformanceCounter` / `QueryPerformanceFrequency`.
//   * Linux:   `clock_gettime(CLOCK_MONOTONIC)` counted in nanoseconds, so the
//              frequency is 1'000'000'000; `clock_getres` is the precision
//              floor the kernel reports for that clock.
[[nodiscard]] CRPC_API std::uint64_t steady_now_ticks() noexcept;
[[nodiscard]] CRPC_API std::uint64_t steady_frequency() noexcept;

} // namespace time
} // namespace crpc

#endif // CRPC_TIME_CLOCK_HPP
