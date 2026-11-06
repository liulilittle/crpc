// CRPC -- proleptic Gregorian calendar dates and timestamps.
//
// `date_time` is a civil (wall clock) date and time with an optional zone
// designation:
//
//   * `is_utc == true`,  `offset_minutes == 0`  -- a UTC instant;
//   * `is_utc == false`, `offset_minutes != 0`  -- local time at that offset;
//   * `is_utc == false`, `offset_minutes == 0`  -- local time with no zone.
//
// The explicit third state is what makes a round trip through ISO 8601
// lossless: `2026-11-06T12:34:56` (no zone) and `2026-11-06T12:34:56Z` are
// different values and neither is treated as "local time of this machine".
//
// Supported civil range: years 1..9999. Timestamp conversions cover the range
// representable as signed 64-bit nanoseconds (1677-09-21T00:12:43.145224192Z
// through 2262-04-11T23:47:16.854775807Z); values outside it are
// `rpc_code::invalid_argument`, never a silent wrap.
//
// Calendar arithmetic (`add_months`, `add_days`, ...) works on civil fields; it
// deliberately does not consult a time zone. Adding one day across a DST
// transition is ambiguous by nature (23 or 25 hours); callers that want an
// exact elapsed span convert to `timestamp` first, add a `duration`, and
// convert back with `to_offset`. See timezone.hpp for the explicit
// gap/overlap policies.
#ifndef CRPC_TIME_DATETIME_HPP
#define CRPC_TIME_DATETIME_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/time/duration.hpp>

namespace crpc {
namespace time {

// Day of week. `sunday == 0` matches both the C `tm_wday` and the ISO `%w`
// numbering; `iso_weekday()` returns 1..7 with Monday == 1.
enum class weekday : std::uint8_t {
    sunday = 0,
    monday = 1,
    tuesday = 2,
    wednesday = 3,
    thursday = 4,
    friday = 5,
    saturday = 6
};

// English names, as produced by the C locale. The formatting layer is
// locale-independent on purpose; see format.hpp.
[[nodiscard]] CRPC_API const char* to_string(weekday value) noexcept;
[[nodiscard]] CRPC_API const char* to_short_string(weekday value) noexcept;

struct date_time {
    int year = 1;
    int month = 1;
    int day = 1;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int nanosecond = 0;
    bool is_utc = false;
    // Offset east of UTC in minutes, in [-1439, 1439]. Ignored (and required
    // to be zero) when `is_utc` is true.
    int offset_minutes = 0;

    // The subsecond part is always in [0, 999999999].
    [[nodiscard]] bool is_valid() const noexcept;
    // `invalid_argument` with a message naming the offending field.
    [[nodiscard]] CRPC_API status validate() const;

    [[nodiscard]] static constexpr date_time utc(int y, int mo, int d, int h = 0,
                                                 int mi = 0, int s = 0,
                                                 int ns = 0) noexcept {
        return date_time{y, mo, d, h, mi, s, ns, true, 0};
    }
    // Local time at `offset_minutes` east of UTC.
    [[nodiscard]] static constexpr date_time with_offset(int y, int mo, int d, int h,
                                                         int mi, int s, int ns,
                                                         int offset) noexcept {
        return date_time{y, mo, d, h, mi, s, ns, offset == 0, offset};
    }
};

[[nodiscard]] constexpr bool operator==(const date_time& a,
                                        const date_time& b) noexcept {
    return a.year == b.year && a.month == b.month && a.day == b.day &&
           a.hour == b.hour && a.minute == b.minute && a.second == b.second &&
           a.nanosecond == b.nanosecond && a.is_utc == b.is_utc &&
           a.offset_minutes == b.offset_minutes;
}
[[nodiscard]] constexpr bool operator!=(const date_time& a,
                                        const date_time& b) noexcept {
    return !(a == b);
}
// Ordering compares the instant each value denotes. Two values with different
// zones are compared after applying their offsets; values with no zone are
// treated as UTC wall time.
[[nodiscard]] CRPC_API bool operator<(const date_time& a, const date_time& b) noexcept;

// --- Calendar rules ----------------------------------------------------------

// Proleptic Gregorian: divisible by 4, and by 400 when divisible by 100.
[[nodiscard]] constexpr bool is_leap_year(int year) noexcept {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

// `invalid_argument` when `month` is outside 1..12.
[[nodiscard]] CRPC_API result<int> days_in_month(int year, int month);

// 1..365 / 1..366. `invalid_argument` for an invalid date.
[[nodiscard]] CRPC_API result<int> day_of_year(const date_time& value);

// Sunday == 0. `invalid_argument` for an invalid date.
[[nodiscard]] CRPC_API result<weekday> day_of_week(const date_time& value);

// ISO 8601 week of the year and ISO weekday (Monday == 1).
[[nodiscard]] CRPC_API result<int> iso_week_of_year(const date_time& value);

// --- Calendar arithmetic -----------------------------------------------------
//
// All operations validate their input and clamp the day-of-month when adding
// years or months (January 31 + 1 month == February 28/29), which is the
// System.DateTime rule. The zone designation of the input is preserved
// verbatim.

[[nodiscard]] CRPC_API result<date_time> add_years(const date_time& value,
                                                  int years);
[[nodiscard]] CRPC_API result<date_time> add_months(const date_time& value,
                                                   int months);
[[nodiscard]] CRPC_API result<date_time> add_days(const date_time& value,
                                                 std::int64_t days);
[[nodiscard]] CRPC_API result<date_time> add_hours(const date_time& value,
                                                  std::int64_t hours);
[[nodiscard]] CRPC_API result<date_time> add_minutes(const date_time& value,
                                                    std::int64_t minutes);
[[nodiscard]] CRPC_API result<date_time> add_seconds(const date_time& value,
                                                    std::int64_t seconds);
[[nodiscard]] CRPC_API result<date_time> add_nanoseconds(const date_time& value,
                                                        std::int64_t nanoseconds);
[[nodiscard]] CRPC_API result<date_time> add_duration(const date_time& value,
                                                     duration span);

// `later - earlier` as a `duration`. Values without a zone are treated as UTC
// wall time. `invalid_argument` when either side is out of range or the span
// does not fit in 64 bits of nanoseconds.
[[nodiscard]] CRPC_API result<duration> subtract(const date_time& later,
                                                const date_time& earlier);

// --- Timestamp conversions ---------------------------------------------------

// Interprets `at` as an instant and renders it at `offset_minutes` east of UTC.
// The resulting value is marked UTC exactly when the offset is zero.
[[nodiscard]] CRPC_API result<date_time> to_offset(timestamp at,
                                                   int offset_minutes = 0);
// Equivalent to `to_offset(at, 0)`.
[[nodiscard]] CRPC_API result<date_time> to_utc(timestamp at);

// Interprets the civil fields at the value's own offset (UTC for `is_utc`, no
// offset otherwise). `invalid_argument` for an invalid date or an instant
// outside the 64-bit nanosecond range.
[[nodiscard]] CRPC_API result<timestamp> to_timestamp(const date_time& value);
// Interprets the civil fields at an explicit offset, ignoring the value's own
// zone designation.
[[nodiscard]] CRPC_API result<timestamp> to_timestamp(const date_time& value,
                                                     int offset_minutes);

// --- ISO 8601 / RFC 3339 -----------------------------------------------------
//
// Accepted grammar (case-insensitive `T`/`Z`, exactly four year digits):
//
//   date          = 4DIGIT "-" 2DIGIT "-" 2DIGIT
//   time          = 2DIGIT ":" 2DIGIT ":" 2DIGIT [ "." 1*9DIGIT ]
//   zone          = "Z" / ("+" / "-") 2DIGIT ":" 2DIGIT
//   date-time     = date [ ("T" / "t" / " ") time [zone] ]
//
// A value without a zone is returned with `is_utc == false` and
// `offset_minutes == 0`. `+00:00` is normalised to `is_utc == true`.
// Calendar-invalid input (February 30, month 13, hour 25, a non-leap
// February 29) is rejected with `invalid_argument`, never normalised.
//
// Formatting emits `YYYY-MM-DDTHH:MM:SS`, a fraction only when non-zero
// (3, 6 or 9 digits, the shortest that is exact), and then `Z`, `+HH:MM` or
// nothing. This is the inverse of the parser for every value it accepts.

[[nodiscard]] CRPC_API result<date_time> parse_iso8601(std::string_view text);
[[nodiscard]] CRPC_API result<date_time> parse_rfc3339(std::string_view text);
[[nodiscard]] CRPC_API result<std::string> format_iso8601(const date_time& value);
// RFC 3339 section 5.6 requires a zone designator; a value with no zone is an
// error here (`format_iso8601` is the lenient form).
[[nodiscard]] CRPC_API result<std::string> format_rfc3339(const date_time& value);

// --- Internal helpers shared with timezone.cpp/format.cpp --------------------
//
// These are not part of the supported public surface; they live here because
// creating a private header would add a file to the build for four functions.
// Their contracts are still exact and unit-tested indirectly.

namespace detail {

// Days from 1970-01-01 to `year-month-day` in the proleptic Gregorian
// calendar. Valid for 1 <= year <= 9999 and a valid month/day.
[[nodiscard]] CRPC_API std::int64_t days_from_civil(int year, int month, int day) noexcept;

// Inverse of days_from_civil. The output date is always valid.
CRPC_API void civil_from_days(std::int64_t days, int& year, int& month,
                              int& day) noexcept;

// Number of whole seconds from the epoch to the civil fields at
// `offset_minutes` east of UTC. Returns false when the value is invalid or
// the result does not fit in int64 seconds.
[[nodiscard]] CRPC_API bool civil_to_unix_seconds(const date_time& value,
                                                  int offset_minutes,
                                                  std::int64_t& seconds) noexcept;

// Splits an instant into floor(seconds) and the non-negative subsecond part.
CRPC_API void split_timestamp(timestamp at, std::int64_t& seconds,
                              std::int32_t& nanoseconds) noexcept;

// UTC instant from Unix seconds plus a subsecond part. Returns false on
// 64-bit nanosecond overflow.
[[nodiscard]] CRPC_API bool make_timestamp(std::int64_t seconds,
                                           std::int64_t nanoseconds,
                                           timestamp& out) noexcept;

} // namespace detail

} // namespace time
} // namespace crpc

#endif // CRPC_TIME_DATETIME_HPP
