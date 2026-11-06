// CRPC -- duration, timestamp and Gregorian calendar tests.
//
// Every expected value in this file was produced by an independent
// implementation, never by the code under test:
//   * CPython 3.14.7 `datetime` (run under WSL, which has tzdata 2026e);
//   * the exact reproduction script and its raw output are stored in
//     .opencode/tmp/datetime/final_expectations.py and more_expectations.py.
// The comment next to a group of values names the exact expression that was
// evaluated. Months are added with the System.DateTime clamp rule, which the
// Python script reproduces explicitly with calendar.monthrange.
#include "test_framework.hpp"

#include <cstdint>
#include <string>
#include <utility>

#include <crpc/time/datetime.hpp>
#include <crpc/time/duration.hpp>

namespace crpc {
namespace time {

inline std::ostream& operator<<(std::ostream& out, const date_time& value) {
    out << value.year << '-' << value.month << '-' << value.day << 'T' << value.hour
        << ':' << value.minute << ':' << value.second << '.' << value.nanosecond
        << (value.is_utc ? "Z" : (value.offset_minutes == 0 ? "[local]" : "[offset]"));
    return out;
}

inline std::ostream& operator<<(std::ostream& out, const weekday& value) {
    out << static_cast<int>(value);
    return out;
}

} // namespace time
} // namespace crpc

namespace {

using crpc::rpc_code;
using crpc::time::add;
using crpc::time::add_days;
using crpc::time::add_duration;
using crpc::time::add_hours;
using crpc::time::add_minutes;
using crpc::time::add_months;
using crpc::time::add_nanoseconds;
using crpc::time::add_seconds;
using crpc::time::add_years;
using crpc::time::date_time;
using crpc::time::day_of_week;
using crpc::time::day_of_year;
using crpc::time::days_in_month;
using crpc::time::duration;
using crpc::time::format_iso8601;
using crpc::time::format_rfc3339;
using crpc::time::is_leap_year;
using crpc::time::iso_week_of_year;
using crpc::time::monotonic_now;
using crpc::time::parse_iso8601;
using crpc::time::parse_rfc3339;
using crpc::time::subtract;
using crpc::time::system_now;
using crpc::time::timestamp;
using crpc::time::to_offset;
using crpc::time::to_timestamp;
using crpc::time::to_utc;
using crpc::time::weekday;

// A UTC value; the parameter order is the struct order.
date_time utc(int y, int mo, int d, int h = 0, int mi = 0, int s = 0, int ns = 0) {
    return date_time::utc(y, mo, d, h, mi, s, ns);
}

date_time zoned(int y, int mo, int d, int h, int mi, int s, int ns, int offset) {
    return date_time::with_offset(y, mo, d, h, mi, s, ns, offset);
}

// A value parsed without a zone designator: not UTC, offset 0.
date_time naive(int y, int mo, int d, int h = 0, int mi = 0, int s = 0,
                int ns = 0) {
    return date_time{y, mo, d, h, mi, s, ns, false, 0};
}

} // namespace

// --- duration ----------------------------------------------------------------

CRPC_TEST(duration_factories_and_totals) {
    // 1 h 1 min 1 s 500 ms through every factory. The whole-unit factories
    // cannot express 3661.5 s exactly, so they are checked by composition;
    // the subsecond factories express the full span directly.
    const auto hour = duration::from_hours(1);
    const auto minute = duration::from_minutes(61);
    const auto second = duration::from_seconds(3661);
    const auto milli = duration::from_milliseconds(3661500);
    const auto micro = duration::from_microseconds(3661500000LL);
    const auto nano = duration::from_nanoseconds(3661500000000LL);
    CRPC_REQUIRE(hour.ok());
    CRPC_REQUIRE(minute.ok());
    CRPC_REQUIRE(second.ok());
    CRPC_REQUIRE(milli.ok());
    CRPC_REQUIRE(micro.ok());
    CRPC_CHECK_EQ(minute.value(),
                  hour.value() + duration::from_minutes(1).value());
    CRPC_CHECK_EQ(second.value(),
                  minute.value() + duration::from_seconds(1).value());
    const auto span =
        second.value() + duration::from_milliseconds(500).value();
    CRPC_CHECK_EQ(span, milli.value());
    CRPC_CHECK_EQ(span, micro.value());
    CRPC_CHECK_EQ(span, nano);

    // Truncation toward zero (TimeSpan.Total* semantics), verified in Python:
    // 3661.5 s // 3600 == 1, 3661.5 // 60 == 61.
    CRPC_CHECK_EQ(std::int64_t{3661}, milli.value().total_seconds());
    CRPC_CHECK_EQ(std::int64_t{61}, milli.value().total_minutes());
    CRPC_CHECK_EQ(std::int64_t{1}, milli.value().total_hours());
    CRPC_CHECK_EQ(std::int64_t{0}, milli.value().total_days());
    CRPC_CHECK_EQ(std::int64_t{3661500}, milli.value().total_milliseconds());

    CRPC_CHECK_EQ(std::int64_t{1}, milli.value().hours());
    CRPC_CHECK_EQ(std::int64_t{1}, milli.value().minutes());
    CRPC_CHECK_EQ(std::int64_t{1}, milli.value().seconds());
    CRPC_CHECK_EQ(std::int64_t{500}, milli.value().milliseconds());
    CRPC_CHECK_EQ(std::int64_t{0}, milli.value().microseconds());
    CRPC_CHECK_EQ(std::int64_t{0}, milli.value().nanoseconds());

    CRPC_CHECK(duration::zero().is_zero());
    CRPC_CHECK_FALSE(duration::zero().is_negative());
    CRPC_CHECK(duration::from_seconds(-1).value().is_negative());
}

CRPC_TEST(duration_negative_components_carry_the_sign) {
    // -90061.5 s == -(1 d 1 h 1 min 1.5 s). System.TimeSpan renders this as
    // days -1, hours -1, minutes -1, seconds -1, milliseconds -500.
    const auto value = duration::from_milliseconds(-90061500LL);
    CRPC_REQUIRE(value.ok());
    CRPC_CHECK_EQ(std::int64_t{-1}, value.value().days());
    CRPC_CHECK_EQ(std::int64_t{-1}, value.value().hours());
    CRPC_CHECK_EQ(std::int64_t{-1}, value.value().minutes());
    CRPC_CHECK_EQ(std::int64_t{-1}, value.value().seconds());
    CRPC_CHECK_EQ(std::int64_t{-500}, value.value().milliseconds());
    // total_* truncate toward zero, so -1.042 days is -1.
    CRPC_CHECK_EQ(std::int64_t{-1}, value.value().total_days());

    // A span smaller than one second has no whole component at all.
    const auto tiny = duration::from_nanoseconds(-1);
    CRPC_CHECK_EQ(std::int64_t{0}, tiny.seconds());
    CRPC_CHECK_EQ(std::int64_t{-1}, tiny.nanoseconds());
    CRPC_CHECK_EQ(std::int64_t{0}, tiny.total_seconds());
    CRPC_CHECK_EQ(std::int64_t{-1}, tiny.total_nanoseconds());
}

CRPC_TEST(duration_checked_factories_reject_overflow) {
    // INT64_MAX ns is 9223372036 s + 854775807 ns; INT64_MIN ns is
    // -9223372036 s - 854775808 ns (Python: 2**63-1 and -2**63).
    CRPC_CHECK(duration::from_seconds(9223372036LL).ok());
    CRPC_CHECK_EQ(rpc_code::invalid_argument,
                  duration::from_seconds(9223372037LL).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument,
                  duration::from_seconds(INT64_MIN).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument,
                  duration::from_milliseconds(INT64_MAX).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument,
                  duration::from_microseconds(INT64_MAX).code());
    // 106751 days == 9,223,286,400 s fits; 106752 days does not.
    CRPC_CHECK(duration::from_days(106751).ok());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, duration::from_days(106752).code());
    // 2562047 h == 9,223,369,200 s fits; 2562048 h does not.
    CRPC_CHECK(duration::from_hours(2562047).ok());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, duration::from_hours(2562048).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument,
                  duration::from_minutes(INT64_MAX).code());
}

CRPC_TEST(duration_checked_arithmetic_and_absolute) {
    const auto one = duration::from_seconds(1).value();
    const auto max = duration::max();
    const auto min = duration::min();

    CRPC_CHECK_EQ(rpc_code::invalid_argument, max.checked_add(one).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, min.checked_subtract(one).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument,
                  duration::from_seconds(2).value().checked_multiply(INT64_MAX).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, one.checked_divide(0).code());
    CRPC_CHECK_EQ(one, one.checked_divide(1).value());
    CRPC_CHECK_EQ(duration::from_seconds(2).value(),
                  one.checked_add(one).value());
    CRPC_CHECK_EQ(duration::zero(), one.checked_subtract(one).value());

    CRPC_CHECK_EQ(rpc_code::invalid_argument, min.absolute().code());
    CRPC_CHECK_EQ(duration::from_seconds(5).value(),
                  duration::from_seconds(-5).value().absolute().value());

    // Unchecked arithmetic wraps in two's complement: max + 1 ns == min.
    const auto tick = duration::from_nanoseconds(1);
    CRPC_CHECK_EQ(min, max + tick);
    CRPC_CHECK_EQ(max, min - tick);
    CRPC_CHECK_EQ(min, -min);
    CRPC_CHECK_EQ(duration::from_seconds(-2).value(),
                  duration::from_seconds(2).value() * -1);
    CRPC_CHECK_EQ(duration::from_seconds(-2).value(),
                  -1 * duration::from_seconds(2).value());
}

CRPC_TEST(duration_ordering_and_equality) {
    const auto a = duration::from_milliseconds(1500).value();
    // from_nanoseconds cannot overflow and returns the duration directly;
    // the checked factories return a result.
    const auto b = duration::from_nanoseconds(1500000000);
    const auto c = duration::from_seconds(2).value();
    CRPC_CHECK(a == b);
    CRPC_CHECK_FALSE(a != b);
    CRPC_CHECK(a < c);
    CRPC_CHECK(c > a);
    CRPC_CHECK(a <= b);
    CRPC_CHECK(b >= a);
    CRPC_CHECK(duration::from_nanoseconds(-1) < duration::zero());
}

// --- timestamp ---------------------------------------------------------------

CRPC_TEST(timestamp_accessors_floor_negative_instants) {
    // Python: (-1) // 1_000_000_000 == -1 and (-1) % 1_000_000_000 == 999999999.
    const timestamp before_epoch(-1);
    CRPC_CHECK_EQ(std::int64_t{-1}, before_epoch.seconds());
    CRPC_CHECK_EQ(std::int64_t{999999999}, before_epoch.subsecond_nanoseconds());
    CRPC_CHECK_EQ(std::int64_t{-1}, timestamp(-1000000000).seconds());
    CRPC_CHECK_EQ(std::int64_t{0}, timestamp(-1000000000).subsecond_nanoseconds());
    // Python: (-1000000001) // 1_000_000_000 == -2 (floor), remainder 999999999.
    CRPC_CHECK_EQ(std::int64_t{-2}, timestamp(-1000000001).seconds());
    CRPC_CHECK_EQ(std::int64_t{999999999}, timestamp(-1000000001).subsecond_nanoseconds());
    CRPC_CHECK_EQ(std::int64_t{0}, timestamp(1).seconds());
    CRPC_CHECK_EQ(std::int32_t{1}, timestamp(1).subsecond_nanoseconds());

    CRPC_CHECK_EQ(std::int64_t{-1}, timestamp(-1).milliseconds());
    CRPC_CHECK_EQ(std::int64_t{-1}, timestamp(-1).microseconds());
    // Python: -1 ns is -1 microsecond with a 999 ns remainder (floor division).
    CRPC_CHECK_EQ(std::int64_t{0}, timestamp::from_nanoseconds(0).seconds());
}

CRPC_TEST(timestamp_checked_constructors) {
    CRPC_CHECK_EQ(rpc_code::invalid_argument,
                  timestamp::from_seconds(9223372037LL).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument,
                  timestamp::from_seconds(INT64_MIN).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument,
                  timestamp::from_milliseconds(INT64_MAX).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument,
                  timestamp::from_microseconds(INT64_MAX).code());

    const auto epoch = timestamp::from_seconds(0);
    CRPC_REQUIRE(epoch.ok());
    CRPC_CHECK_EQ(std::int64_t{0}, epoch.value().nanoseconds);
    const auto one_second = timestamp::from_seconds(1);
    CRPC_REQUIRE(one_second.ok());
    CRPC_CHECK_EQ(std::int64_t{1000000000}, one_second.value().nanoseconds);
    const auto one_milli = timestamp::from_milliseconds(1);
    CRPC_REQUIRE(one_milli.ok());
    CRPC_CHECK_EQ(std::int64_t{1000000}, one_milli.value().nanoseconds);
    const auto one_micro = timestamp::from_microseconds(1);
    CRPC_REQUIRE(one_micro.ok());
    CRPC_CHECK_EQ(std::int64_t{1000}, one_micro.value().nanoseconds);
}

CRPC_TEST(timestamp_addition_and_exact_difference) {
    const auto base = timestamp(1793968496789000000LL); // 2026-11-06T12:34:56.789Z
    const auto plus_day = add(base, duration::from_days(1).value());
    CRPC_REQUIRE(plus_day.ok());
    CRPC_CHECK_EQ(std::int64_t{1793968496789000000LL + 86400000000000LL},
                  plus_day.value().nanoseconds);
    const auto back = subtract(plus_day.value(), duration::from_days(1).value());
    CRPC_REQUIRE(back.ok());
    CRPC_CHECK_EQ(base, back.value());

    // Python: (datetime(2026,11,6,12,34,56) - datetime(2026,11,5,12,34,56))
    //         .total_seconds() == 86400.
    const auto diff = subtract(timestamp(1793968496000000000LL),
                               timestamp(1793968496000000000LL - 86400000000000LL));
    CRPC_REQUIRE(diff.ok());
    CRPC_CHECK_EQ(duration::from_days(1).value(), diff.value());

    // int64 ns extremes are about 584 years apart, which does not fit the
    // 64-bit nanosecond span: this must be reported, not wrapped.
    CRPC_CHECK_EQ(rpc_code::invalid_argument,
                  subtract(timestamp::max(), timestamp::min()).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument,
                  add(timestamp::max(), duration::from_nanoseconds(1)).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument,
                  subtract(timestamp::min(), duration::from_nanoseconds(1)).code());
}

CRPC_TEST(monotonic_clock_never_goes_backwards) {
    const auto first = monotonic_now();
    const auto second = monotonic_now();
    CRPC_CHECK(second >= first);
    // The wall clock must be in a plausible range for a machine running this
    // suite: after 2020-01-01 and before 2100-01-01.
    const auto now = system_now();
    CRPC_CHECK(now >= timestamp(1577836800000000000LL));
    CRPC_CHECK(now < timestamp(4102444800000000000LL));
}

// --- calendar ----------------------------------------------------------------

CRPC_TEST(leap_year_rule) {
    // Python: calendar.isleap for each year (verified in the script):
    // 2000 True, 1900 False, 2024 True, 2100 False, 1600 True, 2400 True.
    CRPC_CHECK(is_leap_year(2000));
    CRPC_CHECK_FALSE(is_leap_year(1900));
    CRPC_CHECK(is_leap_year(2024));
    CRPC_CHECK_FALSE(is_leap_year(2100));
    CRPC_CHECK(is_leap_year(1600));
    CRPC_CHECK(is_leap_year(2400));
    CRPC_CHECK_FALSE(is_leap_year(2023));
    CRPC_CHECK(is_leap_year(2026 - 2)); // 2024 again, as a sanity anchor
}

CRPC_TEST(days_in_month_table) {
    const int common[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    for (int month = 1; month <= 12; ++month) {
        const auto days = days_in_month(2023, month);
        CRPC_REQUIRE(days.ok());
        CRPC_CHECK_EQ(common[month - 1], days.value());
    }
    // Python: calendar.monthrange(2024, 2)[1] == 29.
    const auto leap_feb = days_in_month(2024, 2);
    CRPC_REQUIRE(leap_feb.ok());
    CRPC_CHECK_EQ(29, leap_feb.value());
    const auto century = days_in_month(1900, 2);
    CRPC_REQUIRE(century.ok());
    CRPC_CHECK_EQ(28, century.value());

    CRPC_CHECK_EQ(rpc_code::invalid_argument, days_in_month(2024, 0).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, days_in_month(2024, 13).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, days_in_month(0, 1).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, days_in_month(10000, 1).code());
}

CRPC_TEST(day_of_year_and_weekday) {
    // Python: dt.timetuple().tm_yday, (dt.weekday()+1)%7 (Sunday == 0).
    CRPC_CHECK_EQ(1, day_of_year(utc(1900, 1, 1)).value());
    CRPC_CHECK_EQ(365, day_of_year(utc(1969, 12, 31)).value());
    CRPC_CHECK_EQ(310, day_of_year(utc(2026, 11, 6)).value());
    CRPC_CHECK_EQ(60, day_of_year(utc(2024, 2, 29)).value());
    CRPC_CHECK_EQ(60, day_of_year(utc(2023, 3, 1)).value());
    CRPC_CHECK_EQ(366, day_of_year(utc(2024, 12, 31)).value());

    CRPC_CHECK_EQ(weekday::monday, day_of_week(utc(1900, 1, 1)).value());
    CRPC_CHECK_EQ(weekday::saturday, day_of_week(utc(2000, 1, 1)).value());
    CRPC_CHECK_EQ(weekday::sunday, day_of_week(utc(2023, 1, 1)).value());
    CRPC_CHECK_EQ(weekday::monday, day_of_week(utc(2024, 1, 1)).value());
    CRPC_CHECK_EQ(weekday::friday, day_of_week(utc(2026, 11, 6)).value());
    CRPC_CHECK_EQ(weekday::thursday, day_of_week(utc(1970, 1, 1)).value());
    CRPC_CHECK_EQ(weekday::wednesday, day_of_week(utc(1969, 12, 31)).value());

    CRPC_CHECK_EQ(rpc_code::invalid_argument, day_of_week(utc(2023, 2, 29)).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, day_of_year(utc(2023, 4, 31)).code());
}

CRPC_TEST(iso_week_number) {
    // Python: dt.isocalendar()[1].
    CRPC_CHECK_EQ(45, iso_week_of_year(utc(2026, 11, 6)).value());
    // 2024-12-31 belongs to ISO week 1 (of ISO year 2025); the function
    // returns the week number, not the ISO year.
    CRPC_CHECK_EQ(1, iso_week_of_year(utc(2024, 12, 31)).value());
    CRPC_CHECK_EQ(52, iso_week_of_year(utc(2023, 1, 1)).value());
    CRPC_CHECK_EQ(1, iso_week_of_year(utc(2024, 1, 1)).value());
    CRPC_CHECK_EQ(1, iso_week_of_year(utc(2026, 1, 1)).value());
    CRPC_CHECK_EQ(1, iso_week_of_year(utc(1970, 1, 1)).value());
    CRPC_CHECK_EQ(1, iso_week_of_year(utc(1969, 12, 31)).value());
    CRPC_CHECK_EQ(52, iso_week_of_year(utc(9999, 12, 31)).value());
    CRPC_CHECK_EQ(52, iso_week_of_year(utc(2000, 1, 1)).value());
}

CRPC_TEST(validation_rejects_impossible_fields) {
    CRPC_CHECK(utc(2024, 2, 29).is_valid());
    CRPC_CHECK_FALSE(utc(2023, 2, 29).is_valid());
    CRPC_CHECK_FALSE(utc(2024, 2, 30).is_valid());
    CRPC_CHECK_FALSE(utc(2024, 4, 31).is_valid());
    CRPC_CHECK_FALSE(utc(2024, 13, 1).is_valid());
    CRPC_CHECK_FALSE(utc(2024, 0, 1).is_valid());
    CRPC_CHECK_FALSE(utc(2024, 1, 0).is_valid());
    CRPC_CHECK_FALSE(utc(2024, 1, 32).is_valid());
    CRPC_CHECK_FALSE(utc(2024, 1, 1, 24).is_valid());
    CRPC_CHECK_FALSE(utc(2024, 1, 1, 0, 60).is_valid());
    CRPC_CHECK_FALSE(utc(2024, 1, 1, 0, 0, 60).is_valid());
    CRPC_CHECK_FALSE(utc(2024, 1, 1, 0, 0, 0, 1000000000).is_valid());
    CRPC_CHECK_FALSE(utc(2024, 1, 1, 0, 0, 0, -1).is_valid());
    CRPC_CHECK_FALSE(utc(0, 1, 1).is_valid());
    CRPC_CHECK_FALSE(utc(10000, 1, 1).is_valid());
    CRPC_CHECK_FALSE(zoned(2024, 1, 1, 0, 0, 0, 0, 1440).is_valid());
    CRPC_CHECK_FALSE(zoned(2024, 1, 1, 0, 0, 0, 0, -1440).is_valid());
    CRPC_CHECK(utc(2024, 1, 1, 23, 59, 59, 999999999).is_valid());
    CRPC_CHECK(zoned(2024, 1, 1, 0, 0, 0, 0, -1439).is_valid());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, utc(2023, 2, 29).validate().code());
    CRPC_CHECK(utc(2024, 2, 29).validate().ok());
}

CRPC_TEST(add_months_clamps_to_month_end) {
    // System.DateTime.AddMonths rule, reproduced in Python with
    // min(day, calendar.monthrange(year, month)[1]):
    // 2024-01-31 +1 == 2024-02-29, 2023-01-31 +1 == 2023-02-28,
    // 2024-03-31 -1 == 2024-02-29, 2024-12-31 +2 == 2025-02-28,
    // 2024-02-29 +12 == 2025-02-28, 2023-03-31 -1 == 2023-02-28.
    CRPC_CHECK_EQ(utc(2024, 2, 29), add_months(utc(2024, 1, 31), 1).value());
    CRPC_CHECK_EQ(utc(2023, 2, 28), add_months(utc(2023, 1, 31), 1).value());
    CRPC_CHECK_EQ(utc(2024, 2, 29), add_months(utc(2024, 3, 31), -1).value());
    CRPC_CHECK_EQ(utc(2025, 2, 28), add_months(utc(2024, 12, 31), 2).value());
    CRPC_CHECK_EQ(utc(2025, 2, 28), add_months(utc(2024, 2, 29), 12).value());
    CRPC_CHECK_EQ(utc(2023, 2, 28), add_months(utc(2023, 3, 31), -1).value());
    CRPC_CHECK_EQ(utc(2026, 11, 6), add_months(utc(2026, 11, 6), 0).value());
    CRPC_CHECK_EQ(utc(2028, 1, 6), add_months(utc(2026, 11, 6), 14).value());
    CRPC_CHECK_EQ(utc(2025, 12, 15), add_months(utc(2026, 1, 15), -1).value());
    // Time of day and zone designation are preserved.
    CRPC_CHECK_EQ(zoned(2026, 5, 31, 12, 34, 56, 789, 480),
                  add_months(zoned(2026, 1, 31, 12, 34, 56, 789, 480), 4).value());
    // Out of the supported year range.
    CRPC_CHECK_EQ(rpc_code::invalid_argument,
                  add_months(utc(9999, 12, 1), 1).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument,
                  add_months(utc(1, 1, 1), -1).code());
}

CRPC_TEST(add_years_clamps_february_29) {
    CRPC_CHECK_EQ(utc(2025, 2, 28), add_years(utc(2024, 2, 29), 1).value());
    CRPC_CHECK_EQ(utc(2023, 2, 28), add_years(utc(2024, 2, 29), -1).value());
    CRPC_CHECK_EQ(utc(2028, 2, 29), add_years(utc(2024, 2, 29), 4).value());
    // Python: date(2023,2,28) + relativedelta(years=1) == 2024-02-28. Clamping
    // only applies when the source day has no counterpart in the target year.
    CRPC_CHECK_EQ(utc(2024, 2, 28), add_years(utc(2023, 2, 28), 1).value());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, add_years(utc(9999, 1, 1), 1).code());
}

CRPC_TEST(add_days_crosses_month_and_year_boundaries) {
    // Python: date(2024,1,1) + timedelta(days=N).
    CRPC_CHECK_EQ(utc(2024, 1, 2), add_days(utc(2024, 1, 1), 1).value());
    CRPC_CHECK_EQ(utc(2024, 1, 31), add_days(utc(2024, 1, 1), 30).value());
    CRPC_CHECK_EQ(utc(2024, 2, 29), add_days(utc(2024, 1, 1), 59).value());
    CRPC_CHECK_EQ(utc(2024, 3, 1), add_days(utc(2024, 1, 1), 60).value());
    CRPC_CHECK_EQ(utc(2024, 12, 31), add_days(utc(2024, 1, 1), 365).value());
    CRPC_CHECK_EQ(utc(2025, 1, 1), add_days(utc(2024, 1, 1), 366).value());
    CRPC_CHECK_EQ(utc(2025, 12, 31), add_days(utc(2024, 1, 1), 730).value());
    CRPC_CHECK_EQ(utc(2026, 1, 1), add_days(utc(2024, 1, 1), 731).value());
    CRPC_CHECK_EQ(utc(2023, 12, 31), add_days(utc(2024, 1, 1), -1).value());
    CRPC_CHECK_EQ(utc(2023, 1, 1), add_days(utc(2024, 1, 1), -365).value());
    CRPC_CHECK_EQ(utc(1970, 1, 1), add_days(utc(1969, 12, 31), 1).value());
    CRPC_CHECK_EQ(utc(1969, 12, 31), add_days(utc(1970, 1, 1), -1).value());
    CRPC_CHECK_EQ(rpc_code::invalid_argument,
                  add_days(utc(9999, 12, 31), 1).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument,
                  add_days(utc(1, 1, 1), -1).code());
}

CRPC_TEST(add_hours_minutes_seconds_nanoseconds_carry) {
    // Python: datetime(2026,11,6,23,30) + timedelta(hours=1).
    CRPC_CHECK_EQ(utc(2026, 11, 7, 0, 30), add_hours(utc(2026, 11, 6, 23, 30), 1).value());
    CRPC_CHECK_EQ(utc(2026, 11, 8, 0, 0), add_hours(utc(2026, 11, 6, 23, 0), 25).value());
    CRPC_CHECK_EQ(utc(2025, 12, 30, 0, 0), add_hours(utc(2026, 1, 1), -48).value());
    CRPC_CHECK_EQ(utc(2026, 11, 7, 0, 0), add_minutes(utc(2026, 11, 6, 23, 59), 1).value());
    CRPC_CHECK_EQ(utc(2026, 11, 6, 23, 59), add_minutes(utc(2026, 11, 7), -1).value());
    CRPC_CHECK_EQ(utc(2025, 1, 1), add_seconds(utc(2024, 12, 31, 23, 59, 59), 1).value());
    CRPC_CHECK_EQ(utc(2024, 2, 29, 23, 59, 59, 999999999),
                  add_nanoseconds(utc(2024, 3, 1), -1).value());
    CRPC_CHECK_EQ(utc(2024, 3, 1), add_nanoseconds(utc(2024, 2, 29), 86400000000000LL).value());
    CRPC_CHECK_EQ(utc(2024, 1, 1, 0, 0, 0, 999999999),
                  add_nanoseconds(utc(2024, 1, 1, 0, 0, 0, 999999998), 1).value());
    CRPC_CHECK_EQ(rpc_code::invalid_argument,
                  add_nanoseconds(utc(9999, 12, 31, 23, 59, 59), 2000000000LL).code());

    const auto span = duration::from_seconds(3661).value();
    // Python: datetime(2026,11,6,12,34,56,500000) + timedelta(seconds=3661)
    // == datetime(2026,11,6,13,35,57,500000).
    CRPC_CHECK_EQ(utc(2026, 11, 6, 13, 35, 57, 500000000),
                  add_duration(utc(2026, 11, 6, 12, 34, 56, 500000000), span).value());
}

CRPC_TEST(subtract_two_civil_values) {
    const auto diff = subtract(utc(2026, 11, 6, 12, 34, 56), utc(2026, 11, 5, 12, 34, 56));
    CRPC_REQUIRE(diff.ok());
    CRPC_CHECK_EQ(duration::from_days(1).value(), diff.value());
    const auto negative = subtract(utc(1969, 12, 31), utc(1970, 1, 1));
    CRPC_REQUIRE(negative.ok());
    CRPC_CHECK_EQ(duration::from_days(-1).value(), negative.value());
    // The offsets are applied: 12:00+08:00 is 04:00Z.
    const auto offset_diff = subtract(zoned(2026, 1, 1, 12, 0, 0, 0, 480), utc(2026, 1, 1, 4, 0));
    CRPC_REQUIRE(offset_diff.ok());
    CRPC_CHECK_EQ(duration::zero(), offset_diff.value());
    // Whole-range distance is 584 years and does not fit in 64-bit ns.
    CRPC_CHECK_EQ(rpc_code::invalid_argument,
                  subtract(utc(9999, 12, 31), utc(1, 1, 1)).code());
}

CRPC_TEST(epoch_boundaries_and_negative_timestamps) {
    // Python: int(datetime(1970,1,1,tzinfo=utc).timestamp()) == 0,
    // int(datetime(1969,12,31,23,59,59,tzinfo=utc).timestamp()) == -1.
    CRPC_CHECK_EQ(utc(1970, 1, 1), to_utc(timestamp(0)).value());
    CRPC_CHECK_EQ(utc(1969, 12, 31, 23, 59, 59, 999999999),
                  to_utc(timestamp(-1)).value());
    CRPC_CHECK_EQ(utc(1969, 12, 31, 23, 59, 59),
                  to_utc(timestamp(-1000000000)).value());
    CRPC_CHECK_EQ(utc(1970, 1, 1, 0, 0, 0, 1), to_utc(timestamp(1)).value());
    // Python: datetime(1970,1,1,tzinfo=timezone(timedelta(hours=8)))
    //         .timestamp() == -28800.
    CRPC_CHECK_EQ(timestamp(-28800000000000LL),
                  to_timestamp(zoned(1970, 1, 1, 0, 0, 0, 0, 480)).value());
    CRPC_CHECK_EQ(timestamp(0), to_timestamp(utc(1970, 1, 1)).value());
    CRPC_CHECK_EQ(utc(1969, 12, 31, 23, 59, 59, 999999999),
                  to_offset(timestamp(-1), 0).value());
    // Python: 2026-11-06T12:34:56.789Z == 1793968496789000000 ns.
    CRPC_CHECK_EQ(timestamp(1793968496789000000LL),
                  to_timestamp(utc(2026, 11, 6, 12, 34, 56, 789000000)).value());
    // Python: 2024-02-29T23:59:59.999999999Z == 1709251199999999999 ns.
    CRPC_CHECK_EQ(timestamp(1709251199999999999LL),
                  to_timestamp(utc(2024, 2, 29, 23, 59, 59, 999999999)).value());
}

CRPC_TEST(timestamp_extremes_round_trip) {
    // Python: int64 min ns == 1677-09-21T00:12:43.145224192Z and
    //         int64 max ns == 2262-04-11T23:47:16.854775807Z.
    const auto low = to_utc(timestamp::min());
    CRPC_REQUIRE(low.ok());
    CRPC_CHECK_EQ(utc(1677, 9, 21, 0, 12, 43, 145224192), low.value());
    const auto high = to_utc(timestamp::max());
    CRPC_REQUIRE(high.ok());
    CRPC_CHECK_EQ(utc(2262, 4, 11, 23, 47, 16, 854775807), high.value());
    // Converting back is exact at both ends.
    CRPC_CHECK_EQ(timestamp::min(), to_timestamp(low.value()).value());
    CRPC_CHECK_EQ(timestamp::max(), to_timestamp(high.value()).value());
}

CRPC_TEST(timestamp_conversion_range_is_checked) {
    // Year 1600 is before the int64 nanosecond range; year 9999 is after it.
    CRPC_CHECK_EQ(rpc_code::invalid_argument, to_timestamp(utc(1600, 1, 1)).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, to_timestamp(utc(9999, 12, 31)).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, to_timestamp(utc(2262, 4, 12)).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, to_timestamp(utc(1677, 9, 20)).code());
    // The last representable whole second on each side.
    CRPC_CHECK(to_timestamp(utc(2262, 4, 11, 23, 47, 16)).ok());
    CRPC_CHECK(to_timestamp(utc(1677, 9, 21, 0, 12, 44)).ok());
}

CRPC_TEST(utc_and_offset_conversion) {
    // Python: 2026-11-06T20:34:56.789+08:00 == 2026-11-06T12:34:56.789Z.
    CRPC_CHECK_EQ(zoned(2026, 11, 6, 20, 34, 56, 789000000, 480),
                  to_offset(timestamp(1793968496789000000LL), 480).value());
    CRPC_CHECK_EQ(utc(2026, 11, 6, 12, 34, 56, 789000000),
                  to_offset(timestamp(1793968496789000000LL), 0).value());
    CRPC_CHECK_EQ(zoned(2026, 11, 6, 7, 34, 56, 789000000, -300),
                  to_offset(timestamp(1793968496789000000LL), -300).value());
    // Python: 1970-01-01T00:00:00Z at -05:00 is 1969-12-31T19:00:00-05:00.
    CRPC_CHECK_EQ(zoned(1969, 12, 31, 19, 0, 0, 0, -300),
                  to_offset(timestamp(0), -300).value());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, to_offset(timestamp(0), 1440).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, to_offset(timestamp(0), -1440).code());
}

CRPC_TEST(ordering_compares_instants) {
    CRPC_CHECK(utc(2026, 1, 1, 12, 0) < utc(2026, 1, 1, 13, 0));
    CRPC_CHECK(utc(2025, 12, 31, 23, 0) < utc(2026, 1, 1, 0, 0));
    // 12:00+08:00 is 04:00Z, so it is before 05:00Z.
    CRPC_CHECK(zoned(2026, 1, 1, 12, 0, 0, 0, 480) < utc(2026, 1, 1, 5, 0));
    // 13:00+08:00 is 05:00Z, which is after 04:59Z (Python-verified).
    CRPC_CHECK(utc(2026, 1, 1, 4, 59) < zoned(2026, 1, 1, 13, 0, 0, 0, 480));
    CRPC_CHECK_FALSE(zoned(2026, 1, 1, 12, 0, 0, 0, 480) < utc(2026, 1, 1, 4, 0));
}

// --- ISO 8601 / RFC 3339 -----------------------------------------------------

CRPC_TEST(iso_parse_utc_and_offset_forms) {
    // Python: ns values from the epoch delta, script final_expectations.py.
    const auto utc_value = parse_iso8601("2026-11-06T12:34:56.789Z");
    CRPC_REQUIRE(utc_value.ok());
    CRPC_CHECK_EQ(utc(2026, 11, 6, 12, 34, 56, 789000000), utc_value.value());
    CRPC_CHECK(utc_value.value().is_utc);

    const auto offset_value = parse_iso8601("2026-11-06T12:34:56.789+08:00");
    CRPC_REQUIRE(offset_value.ok());
    CRPC_CHECK_EQ(zoned(2026, 11, 6, 12, 34, 56, 789000000, 480),
                  offset_value.value());
    CRPC_CHECK_EQ(timestamp(1793939696789000000LL),
                  to_timestamp(offset_value.value()).value());

    const auto negative = parse_iso8601("2026-11-06T07:34:56.789-05:00");
    CRPC_REQUIRE(negative.ok());
    CRPC_CHECK_EQ(-300, negative.value().offset_minutes);

    // A fraction with nine digits keeps every nanosecond.
    const auto nanos = parse_iso8601("2026-11-06T12:34:56.123456789Z");
    CRPC_REQUIRE(nanos.ok());
    CRPC_CHECK_EQ(123456789, nanos.value().nanosecond);

    // A fraction is left-padded: .5 is 500000000 ns.
    const auto short_fraction = parse_iso8601("2026-11-06T12:34:56.5Z");
    CRPC_REQUIRE(short_fraction.ok());
    CRPC_CHECK_EQ(500000000, short_fraction.value().nanosecond);

    // Space separator and lowercase designators are accepted.
    const auto spaced = parse_iso8601("2026-11-06 12:34:56");
    CRPC_REQUIRE(spaced.ok());
    // No zone designator: not UTC, offset 0.
    CRPC_CHECK_EQ(naive(2026, 11, 6, 12, 34, 56), spaced.value());
    CRPC_CHECK_FALSE(spaced.value().is_utc);
    const auto lower = parse_iso8601("2026-11-06t12:34:56z");
    CRPC_REQUIRE(lower.ok());
    CRPC_CHECK(lower.value().is_utc);

    // Date only, and "+00:00" normalises to UTC.
    const auto date_only = parse_iso8601("2026-11-06");
    CRPC_REQUIRE(date_only.ok());
    CRPC_CHECK_EQ(naive(2026, 11, 6), date_only.value());
    CRPC_CHECK_FALSE(date_only.value().is_utc);
    const auto plus_zero = parse_iso8601("2026-11-06T12:34:56+00:00");
    CRPC_REQUIRE(plus_zero.ok());
    CRPC_CHECK(plus_zero.value().is_utc);
    CRPC_CHECK_EQ(0, plus_zero.value().offset_minutes);

    const auto minus_zero = parse_iso8601("2026-11-06T12:34:56-00:00");
    CRPC_REQUIRE(minus_zero.ok());
    CRPC_CHECK(minus_zero.value().is_utc);
}

CRPC_TEST(iso_parse_rejects_invalid_input) {
    const char* const bad[] = {
        "",
        "2026-11-06T12:34:56",      // valid, checked separately below
        "2026-02-30T00:00:00Z",     // February 30
        "2023-02-29T00:00:00Z",     // not a leap year
        "2026-13-01T00:00:00Z",     // month 13
        "2026-00-01T00:00:00Z",     // month 0
        "2026-01-00T00:00:00Z",     // day 0
        "2026-01-32T00:00:00Z",     // day 32
        "2026-01-01T25:00:00Z",     // hour 25
        "2026-01-01T24:00:00Z",     // hour 24
        "2026-01-01T12:60:00Z",     // minute 60
        "2026-01-01T12:00:60Z",     // second 60 (no leap seconds)
        "2026-01-01T12:00:00.1234567890Z", // 10 fractional digits
        "2026-01-01T12:00:00+24:00",       // offset hour 24
        "2026-01-01T12:00:00+08:60",       // offset minute 60
        "2026-01-01T12:00:00+8:00",        // offset needs two digits
        "2026-1-01T12:00:00Z",             // month needs two digits
        "2026-01-01T12:00Z",               // time needs seconds
        "2026-01-01T12:00:00ZZ",           // trailing garbage
        "2026-01-01T12:00:00.123Zx",       // trailing garbage
        "0000-01-01T00:00:00Z",            // year 0 is out of range
        "10000-01-01T00:00:00Z",           // five-digit year
        "2026-01-01Z",                     // zone without a time part
        "2026/01/01T00:00:00Z",            // wrong separators
        "2026-01-01T12:00:00+08",          // offset needs minutes
        "2026-01-01T12:00:00 ",            // trailing space
        " 2026-01-01T12:00:00Z",           // leading space
    };
    for (const char* text : bad) {
        // The one valid entry in the table is documented in place: it is
        // covered by the positive tests, so it must fail here only because
        // this list is meant to be all-invalid. Remove it to keep intent
        // clear instead of silently skipping.
        if (std::string(text) == "2026-11-06T12:34:56") {
            CRPC_CHECK(parse_iso8601(text).ok());
            continue;
        }
        CRPC_CHECK_EQ(rpc_code::invalid_argument, parse_iso8601(text).code());
    }
}

CRPC_TEST(iso_format_uses_shortest_exact_fraction) {
    CRPC_CHECK_EQ(std::string("2026-11-06T12:34:56.789Z"),
                  format_iso8601(utc(2026, 11, 6, 12, 34, 56, 789000000)).value());
    CRPC_CHECK_EQ(std::string("2026-11-06T12:34:56Z"),
                  format_iso8601(utc(2026, 11, 6, 12, 34, 56)).value());
    CRPC_CHECK_EQ(std::string("2026-11-06T12:34:56.123456Z"),
                  format_iso8601(utc(2026, 11, 6, 12, 34, 56, 123456000)).value());
    CRPC_CHECK_EQ(std::string("2026-11-06T12:34:56.000000001Z"),
                  format_iso8601(utc(2026, 11, 6, 12, 34, 56, 1)).value());
    CRPC_CHECK_EQ(std::string("2026-11-06T12:34:56.000001Z"),
                  format_iso8601(utc(2026, 11, 6, 12, 34, 56, 1000)).value());
    CRPC_CHECK_EQ(std::string("2026-11-06T12:34:56+08:00"),
                  format_iso8601(zoned(2026, 11, 6, 12, 34, 56, 0, 480)).value());
    CRPC_CHECK_EQ(std::string("2026-11-06T12:34:56-05:00"),
                  format_iso8601(zoned(2026, 11, 6, 12, 34, 56, 0, -300)).value());
    CRPC_CHECK_EQ(std::string("2026-11-06T12:34:56"),
                  format_iso8601(date_time{2026, 11, 6, 12, 34, 56, 0, false, 0}).value());
    CRPC_CHECK_EQ(std::string("0001-01-01T00:00:00Z"),
                  format_iso8601(utc(1, 1, 1)).value());
    CRPC_CHECK_EQ(std::string("9999-12-31T23:59:59.999999999Z"),
                  format_iso8601(utc(9999, 12, 31, 23, 59, 59, 999999999)).value());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, format_iso8601(utc(2023, 2, 29)).code());
}

CRPC_TEST(iso_round_trip_is_lossless) {
    const date_time values[] = {
        utc(2026, 11, 6, 12, 34, 56, 789000000),
        utc(1970, 1, 1),
        utc(1969, 12, 31, 23, 59, 59, 999999999),
        utc(2000, 2, 29, 0, 0, 0, 1),
        utc(2262, 4, 11, 23, 47, 16, 854775807),
        utc(1677, 9, 21, 0, 12, 43, 145224192),
        zoned(2026, 7, 15, 12, 0, 0, 0, -240),
        zoned(2026, 1, 15, 12, 0, 0, 0, 330),
        date_time{2024, 2, 29, 12, 0, 0, 123000000, false, 0},
    };
    for (const date_time& value : values) {
        const auto text = format_iso8601(value);
        CRPC_REQUIRE(text.ok());
        const auto reparsed = parse_iso8601(text.value());
        CRPC_REQUIRE(reparsed.ok());
        CRPC_CHECK_EQ(value, reparsed.value());
        CRPC_CHECK_EQ(to_timestamp(value).value(), to_timestamp(reparsed.value()).value());
    }
}

CRPC_TEST(rfc3339_requires_a_zone) {
    const auto zulu = parse_rfc3339("2026-11-06T12:34:56.789Z");
    CRPC_REQUIRE(zulu.ok());
    CRPC_CHECK_EQ(utc(2026, 11, 6, 12, 34, 56, 789000000), zulu.value());
    const auto offset = parse_rfc3339("2026-11-06T12:34:56.789+08:00");
    CRPC_REQUIRE(offset.ok());
    CRPC_CHECK_EQ(480, offset.value().offset_minutes);
    // RFC 3339 section 5.6 requires a zone; a space separator is not allowed
    // there (only 'T' / 't').
    CRPC_CHECK_EQ(rpc_code::invalid_argument,
                  parse_rfc3339("2026-11-06T12:34:56").code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument,
                  parse_rfc3339("2026-11-06 12:34:56Z").code());

    CRPC_CHECK_EQ(std::string("2026-11-06T12:34:56.789Z"),
                  format_rfc3339(utc(2026, 11, 6, 12, 34, 56, 789000000)).value());
    CRPC_CHECK_EQ(std::string("2026-11-06T12:34:56+08:00"),
                  format_rfc3339(zoned(2026, 11, 6, 12, 34, 56, 0, 480)).value());
    CRPC_CHECK_EQ(rpc_code::invalid_argument,
                  format_rfc3339(date_time{2026, 11, 6, 12, 34, 56, 0, false, 0}).code());
}

CRPC_TEST_MAIN()
