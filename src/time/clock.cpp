// CRPC -- high-precision clock implementation.
//
// Each reader maps to exactly one documented platform source; see clock.hpp
// for the unit, monotonicity and precision contract of every function.
#include <crpc/time/clock.hpp>

#include <cstdint>

#if defined(CRPC_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#else
#include <time.h>
#endif

namespace crpc {
namespace time {
namespace {

constexpr std::int64_t k_seconds_per_day = 86400LL;
constexpr std::int64_t k_seconds_per_hour = 3600LL;
constexpr std::int64_t k_seconds_per_minute = 60LL;

constexpr std::uint64_t k_nanoseconds_per_second = 1000000000ull;
constexpr std::uint64_t k_nanoseconds_per_millisecond = 1000000ull;

#if defined(CRPC_OS_WINDOWS)
// 1601-01-01T00:00:00Z (the FILETIME epoch) to 1970-01-01T00:00:00Z in whole
// seconds. FILETIME counts 100 ns intervals.
constexpr std::int64_t k_filetime_epoch_offset_seconds = 11644473600LL;
constexpr std::uint64_t k_filetime_ticks_per_second = 10000000ull;
#endif

// Converts an already split Unix instant to the civil calendar without
// allocating: floor division keeps pre-epoch instants on the correct day.
date_time civil_utc(std::int64_t seconds, std::uint32_t nanoseconds) noexcept {
    const std::int64_t days = detail::floor_div(seconds, k_seconds_per_day);
    const std::int64_t second_of_day = seconds - days * k_seconds_per_day;
    int year = 1;
    int month = 1;
    int day = 1;
    detail::civil_from_days(days, year, month, day);
    return date_time{
        year,
        month,
        day,
        static_cast<int>(second_of_day / k_seconds_per_hour),
        static_cast<int>((second_of_day / k_seconds_per_minute) %
                         k_seconds_per_minute),
        static_cast<int>(second_of_day % k_seconds_per_minute),
        static_cast<int>(nanoseconds),
        true,
        0};
}

} // namespace

unix_time now_unix() noexcept {
#if defined(CRPC_OS_WINDOWS)
    FILETIME file_time = {};
    GetSystemTimePreciseAsFileTime(&file_time);

    ULARGE_INTEGER ticks;
    ticks.LowPart = file_time.dwLowDateTime;
    ticks.HighPart = file_time.dwHighDateTime;

    const std::uint64_t since_1601 = ticks.QuadPart;
    const std::uint64_t whole_seconds = since_1601 / k_filetime_ticks_per_second;
    const std::uint32_t subsecond = static_cast<std::uint32_t>(
        (since_1601 % k_filetime_ticks_per_second) * 100ull);
    return unix_time(
        static_cast<std::int64_t>(whole_seconds) -
            k_filetime_epoch_offset_seconds,
        subsecond);
#else
    timespec value = {};
    const int rc = ::clock_gettime(CLOCK_REALTIME, &value);
    (void)rc;
    return unix_time(static_cast<std::int64_t>(value.tv_sec),
                     static_cast<std::uint32_t>(value.tv_nsec));
#endif
}

date_time utc_now() noexcept {
    const unix_time at = now_unix();
    return civil_utc(at.seconds, at.nanoseconds);
}

date_time gmt_now() noexcept {
    // GMT shares the UTC time scale; the platform source and conversion are
    // intentionally identical.
    return utc_now();
}

std::uint64_t system_tick() noexcept {
#if defined(CRPC_OS_WINDOWS)
    return static_cast<std::uint64_t>(GetTickCount64());
#else
    timespec value = {};
    const int rc = ::clock_gettime(CLOCK_MONOTONIC, &value);
    (void)rc;
    return static_cast<std::uint64_t>(value.tv_sec) * 1000ull +
           static_cast<std::uint64_t>(value.tv_nsec) /
               k_nanoseconds_per_millisecond;
#endif
}

std::uint64_t process_tick() noexcept {
#if defined(CRPC_OS_WINDOWS)
    FILETIME creation = {};
    FILETIME exit_time = {};
    FILETIME kernel_time = {};
    FILETIME user_time = {};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit_time,
                        &kernel_time, &user_time) == 0) {
        return 0;
    }

    ULARGE_INTEGER kernel;
    kernel.LowPart = kernel_time.dwLowDateTime;
    kernel.HighPart = kernel_time.dwHighDateTime;
    ULARGE_INTEGER user;
    user.LowPart = user_time.dwLowDateTime;
    user.HighPart = user_time.dwHighDateTime;

    // 100 ns units to nanoseconds.
    return (kernel.QuadPart + user.QuadPart) * 100ull;
#else
    timespec value = {};
    const int rc = ::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value);
    (void)rc;
    return static_cast<std::uint64_t>(value.tv_sec) *
               k_nanoseconds_per_second +
           static_cast<std::uint64_t>(value.tv_nsec);
#endif
}

std::uint64_t steady_now_ticks() noexcept {
#if defined(CRPC_OS_WINDOWS)
    LARGE_INTEGER counter = {};
    (void)QueryPerformanceCounter(&counter);
    return static_cast<std::uint64_t>(counter.QuadPart);
#else
    timespec value = {};
    const int rc = ::clock_gettime(CLOCK_MONOTONIC, &value);
    (void)rc;
    return static_cast<std::uint64_t>(value.tv_sec) *
               k_nanoseconds_per_second +
           static_cast<std::uint64_t>(value.tv_nsec);
#endif
}

std::uint64_t steady_frequency() noexcept {
#if defined(CRPC_OS_WINDOWS)
    LARGE_INTEGER frequency = {};
    if (QueryPerformanceFrequency(&frequency) == 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(frequency.QuadPart);
#else
    // steady_now_ticks() counts CLOCK_MONOTONIC nanoseconds on POSIX.
    return k_nanoseconds_per_second;
#endif
}

} // namespace time
} // namespace crpc
