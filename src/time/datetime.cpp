// CRPC -- proleptic Gregorian calendar implementation.
//
// The day-number conversion is Howard Hinnant's `days_from_civil` /
// `civil_from_days` pair, which is exact for the whole proleptic Gregorian
// range and needs no lookup tables. The epoch is 1970-01-01.
#include <crpc/time/datetime.hpp>

#include <cstdio>
#include <string>

namespace crpc {
namespace time {
namespace {

constexpr std::int64_t k_seconds_per_day = 86400LL;
constexpr std::int64_t k_nanoseconds_per_second = 1000000000LL;
constexpr std::int64_t k_nanoseconds_per_day =
    k_seconds_per_day * k_nanoseconds_per_second;

// Days from 1970-01-01 to year 1 and to the last day of year 9999; the
// supported civil range.
constexpr std::int64_t k_min_day = -719162;  // 0001-01-01
constexpr std::int64_t k_max_day = 2932896;  // 9999-12-31

constexpr int k_month_days[12] = {31, 28, 31, 30, 31, 30,
                                  31, 31, 30, 31, 30, 31};

int days_in_month_raw(int year, int month) noexcept {
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return k_month_days[month - 1];
}

bool in_supported_range(int year) noexcept { return year >= 1 && year <= 9999; }

status invalid(const char* field) {
    return status(rpc_code::invalid_argument, std::string("invalid ") + field);
}

// Ordered civil fields -> day number + second of day + nanosecond. The caller
// has validated the fields.
void split_civil(const date_time& value, std::int64_t& day, std::int64_t& second_of_day,
                 std::int32_t& nanos) noexcept {
    day = detail::days_from_civil(value.year, value.month, value.day);
    second_of_day = static_cast<std::int64_t>(value.hour) * 3600 +
                    static_cast<std::int64_t>(value.minute) * 60 + value.second;
    nanos = value.nanosecond;
}

// Rebuilds a date_time from a day number and second of day, preserving the
// zone designation of `shape`.
result<date_time> from_split(std::int64_t day, std::int64_t second_of_day,
                             std::int32_t nanos, const date_time& shape) {
    if (day < k_min_day || day > k_max_day) {
        return rpc_error(rpc_code::invalid_argument,
                         "date is outside the supported year range 1..9999");
    }
    date_time out = shape;
    detail::civil_from_days(day, out.year, out.month, out.day);
    out.hour = static_cast<int>(second_of_day / 3600);
    out.minute = static_cast<int>((second_of_day / 60) % 60);
    out.second = static_cast<int>(second_of_day % 60);
    out.nanosecond = nanos;
    return out;
}


// Overflow-safe int64 multiply (no signed overflow in the check).
bool checked_mul_int64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
    if (a == 0 || b == 0) {
        out = 0;
        return true;
    }
    if (a == -1 && b == INT64_MIN) {
        return false;
    }
    if (b == -1 && a == INT64_MIN) {
        return false;
    }
    if (a > 0) {
        if (b > 0) {
            if (a > INT64_MAX / b) {
                return false;
            }
        } else if (b < INT64_MIN / a) {
            return false;
        }
    } else {
        if (b > 0) {
            if (a < INT64_MIN / b) {
                return false;
            }
        } else if (a < INT64_MAX / b) {
            return false;
        }
    }
    out = a * b;
    return true;
}

bool checked_add_int64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
    if (b > 0 && a > INT64_MAX - b) {
        return false;
    }
    if (b < 0 && a < INT64_MIN - b) {
        return false;
    }
    out = a + b;
    return true;
}
// Normalises (day, second_of_day, nanos). `nanos` may be any int64 value; it
// is folded into whole seconds first, then the seconds are folded into days.
// Both folds use floor division so a negative span borrows correctly.
result<date_time> normalise(std::int64_t day, std::int64_t second_of_day,
                            std::int64_t nanos, const date_time& shape) {
    std::int64_t second_carry = detail::floor_div(nanos, k_nanoseconds_per_second);
    std::int64_t subsecond = detail::floor_mod(nanos, k_nanoseconds_per_second);
    std::int64_t seconds = 0;
    if (!checked_add_int64(second_of_day, second_carry, seconds)) {
        return rpc_error(rpc_code::invalid_argument, "date arithmetic overflows");
    }
    std::int64_t extra_days = detail::floor_div(seconds, k_seconds_per_day);
    const std::int64_t second_in_day =
        detail::floor_mod(seconds, k_seconds_per_day);
    std::int64_t target = 0;
    if (!checked_add_int64(day, extra_days, target)) {
        return rpc_error(rpc_code::invalid_argument, "date arithmetic overflows");
    }
    return from_split(target, second_in_day,
                      static_cast<std::int32_t>(subsecond), shape);
}

// ISO weekday: Monday == 1 ... Sunday == 7.
int iso_weekday_from_sunday0(int sunday0) noexcept {
    return ((sunday0 + 6) % 7) + 1;
}

int weeks_in_iso_year(int year) noexcept {
    // A year has 53 ISO weeks when 1 January is a Thursday, or it is a leap
    // year and 1 January is a Wednesday.
    const int sunday0 = static_cast<int>(
        detail::floor_mod(detail::days_from_civil(year, 1, 1) + 4, 7));
    const int iso_jan1 = iso_weekday_from_sunday0(sunday0);
    if (iso_jan1 == 4) {
        return 53;
    }
    if (iso_jan1 == 3 && is_leap_year(year)) {
        return 53;
    }
    return 52;
}

std::string two(int value) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%02d", value);
    return buffer;
}

} // namespace

// --- detail helpers ----------------------------------------------------------

namespace detail {

std::int64_t days_from_civil(int year, int month, int day) noexcept {
    const std::int64_t y = static_cast<std::int64_t>(year) - (month <= 2 ? 1 : 0);
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;                              // [0, 399]
    const std::int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 +
                             day - 1;                                    // [0, 365]
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;      // [0, 146096]
    return era * 146097 + doe - 719468;
}

void civil_from_days(std::int64_t days, int& year, int& month, int& day) noexcept {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const std::int64_t doe = days - era * 146097;                 // [0, 146096]
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = yoe + era * 400;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100); // [0, 365]
    const std::int64_t mp = (5 * doy + 2) / 153;                       // [0, 11]
    day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    year = static_cast<int>(y + (month <= 2 ? 1 : 0));
}

bool civil_to_unix_seconds(const date_time& value, int offset_minutes,
                           std::int64_t& seconds) noexcept {
    if (!value.is_valid() || offset_minutes < -1439 || offset_minutes > 1439) {
        return false;
    }
    const std::int64_t day = days_from_civil(value.year, value.month, value.day);
    const std::int64_t second_of_day =
        static_cast<std::int64_t>(value.hour) * 3600 +
        static_cast<std::int64_t>(value.minute) * 60 + value.second;
    std::int64_t local = day * k_seconds_per_day + second_of_day;
    // seconds = local - shift. Overflow is possible only in one direction per
    // shift sign, and the bound on the other side is representable, so the
    // checks below never overflow themselves.
    const std::int64_t shift = static_cast<std::int64_t>(offset_minutes) * 60;
    if (shift >= 0 && local < INT64_MIN + shift) {
        return false;
    }
    if (shift < 0 && local > INT64_MAX + shift) {
        return false;
    }
    seconds = local - shift;
    return true;
}

void split_timestamp(timestamp at, std::int64_t& seconds,
                     std::int32_t& nanoseconds) noexcept {
    seconds = at.seconds();
    nanoseconds = at.subsecond_nanoseconds();
}

bool make_timestamp(std::int64_t seconds, std::int64_t nanoseconds,
                    timestamp& out) noexcept {
    if (nanoseconds < 0 || nanoseconds >= k_nanoseconds_per_second) {
        return false;
    }
    // The representable second range is [-9223372037, 9223372036]. The lower
    // second is only complete from 145224192 ns onward, because int64 min is
    // -9223372036854775808. The upper second ends at 854775807 ns.
    if (seconds < -9223372037LL || seconds > 9223372036LL) {
        return false;
    }
    if (seconds == 9223372036LL && nanoseconds > 854775807LL) {
        return false;
    }
    if (seconds == -9223372037LL && nanoseconds < 145224192LL) {
        return false;
    }
    std::int64_t value = 0;
    if (seconds >= 0) {
        value = seconds * k_nanoseconds_per_second + nanoseconds;
    } else {
        // Compute in two steps so the intermediate product stays in range even
        // for the lowest second: (seconds + 1) * 1e9 is representable, and the
        // subtraction of (1e9 - nanoseconds) cannot underflow after the bound
        // checks above.
        value = (seconds + 1) * k_nanoseconds_per_second -
                (k_nanoseconds_per_second - nanoseconds);
    }
    out = timestamp(value);
    return true;
}

} // namespace detail

// --- names -------------------------------------------------------------------

const char* to_string(weekday value) noexcept {
    switch (value) {
    case weekday::sunday:
        return "Sunday";
    case weekday::monday:
        return "Monday";
    case weekday::tuesday:
        return "Tuesday";
    case weekday::wednesday:
        return "Wednesday";
    case weekday::thursday:
        return "Thursday";
    case weekday::friday:
        return "Friday";
    case weekday::saturday:
        return "Saturday";
    }
    return "?";
}

const char* to_short_string(weekday value) noexcept {
    switch (value) {
    case weekday::sunday:
        return "Sun";
    case weekday::monday:
        return "Mon";
    case weekday::tuesday:
        return "Tue";
    case weekday::wednesday:
        return "Wed";
    case weekday::thursday:
        return "Thu";
    case weekday::friday:
        return "Fri";
    case weekday::saturday:
        return "Sat";
    }
    return "?";
}

// --- validation --------------------------------------------------------------

bool date_time::is_valid() const noexcept {
    if (!in_supported_range(year) || month < 1 || month > 12) {
        return false;
    }
    if (day < 1 || day > days_in_month_raw(year, month)) {
        return false;
    }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 ||
        second > 59) {
        return false;
    }
    if (nanosecond < 0 || nanosecond > 999999999) {
        return false;
    }
    if (offset_minutes < -1439 || offset_minutes > 1439) {
        return false;
    }
    if (is_utc && offset_minutes != 0) {
        return false;
    }
    return true;
}

status date_time::validate() const {
    if (!in_supported_range(year)) {
        return invalid("year (supported range is 1..9999)");
    }
    if (month < 1 || month > 12) {
        return invalid("month (supported range is 1..12)");
    }
    if (day < 1 || day > days_in_month_raw(year, month)) {
        return invalid("day (out of range for this month)");
    }
    if (hour < 0 || hour > 23) {
        return invalid("hour (supported range is 0..23)");
    }
    if (minute < 0 || minute > 59) {
        return invalid("minute (supported range is 0..59)");
    }
    if (second < 0 || second > 59) {
        return invalid("second (supported range is 0..59)");
    }
    if (nanosecond < 0 || nanosecond > 999999999) {
        return invalid("nanosecond (supported range is 0..999999999)");
    }
    if (offset_minutes < -1439 || offset_minutes > 1439) {
        return invalid("offset (supported range is -1439..1439 minutes)");
    }
    if (is_utc && offset_minutes != 0) {
        return invalid("offset (a UTC value must have offset 0)");
    }
    return status{};
}

bool operator<(const date_time& a, const date_time& b) noexcept {
    if (!a.is_valid() || !b.is_valid()) {
        return false;
    }
    std::int64_t a_seconds = 0;
    std::int64_t b_seconds = 0;
    if (!detail::civil_to_unix_seconds(a, a.is_utc ? 0 : a.offset_minutes,
                                       a_seconds) ||
        !detail::civil_to_unix_seconds(b, b.is_utc ? 0 : b.offset_minutes,
                                       b_seconds)) {
        return false;
    }
    if (a_seconds != b_seconds) {
        return a_seconds < b_seconds;
    }
    return a.nanosecond < b.nanosecond;
}

// --- calendar queries --------------------------------------------------------

result<int> days_in_month(int year, int month) {
    if (!in_supported_range(year)) {
        return rpc_error(rpc_code::invalid_argument,
                         "year is outside the supported range 1..9999");
    }
    if (month < 1 || month > 12) {
        return rpc_error(rpc_code::invalid_argument,
                         "month is outside the supported range 1..12");
    }
    return days_in_month_raw(year, month);
}

result<int> day_of_year(const date_time& value) {
    if (const status check = value.validate(); !check.ok()) {
        return check.error();
    }
    const std::int64_t first = detail::days_from_civil(value.year, 1, 1);
    const std::int64_t current =
        detail::days_from_civil(value.year, value.month, value.day);
    return static_cast<int>(current - first) + 1;
}

result<weekday> day_of_week(const date_time& value) {
    if (const status check = value.validate(); !check.ok()) {
        return check.error();
    }
    const std::int64_t day =
        detail::days_from_civil(value.year, value.month, value.day);
    // 1970-01-01 was a Thursday (4 in Sunday == 0 numbering).
    const std::int64_t weekday_index = detail::floor_mod(day + 4, 7);
    return static_cast<weekday>(weekday_index);
}

result<int> iso_week_of_year(const date_time& value) {
    if (const status check = value.validate(); !check.ok()) {
        return check.error();
    }
    const auto ordinal = day_of_year(value);
    if (!ordinal.ok()) {
        return ordinal.error();
    }
    const auto weekday_value = day_of_week(value);
    if (!weekday_value.ok()) {
        return weekday_value.error();
    }
    const int iso_day = iso_weekday_from_sunday0(
        static_cast<int>(weekday_value.value()));
    // ISO 8601: week 1 contains the first Thursday of the year.
    int week = (ordinal.value() - iso_day + 10) / 7;
    if (week < 1) {
        return weeks_in_iso_year(value.year - 1);
    }
    if (week > weeks_in_iso_year(value.year)) {
        return 1;
    }
    return week;
}

// --- calendar arithmetic -----------------------------------------------------

result<date_time> add_months(const date_time& value, int months) {
    if (const status check = value.validate(); !check.ok()) {
        return check.error();
    }
    const std::int64_t month_index =
        static_cast<std::int64_t>(value.year - 1) * 12 + (value.month - 1) +
        static_cast<std::int64_t>(months);
    if (month_index < 0 || month_index >= static_cast<std::int64_t>(9999) * 12) {
        return rpc_error(rpc_code::invalid_argument,
                         "month arithmetic leaves the supported year range 1..9999");
    }
    date_time out = value;
    out.year = static_cast<int>(month_index / 12) + 1;
    out.month = static_cast<int>(month_index % 12) + 1;
    const int limit = days_in_month_raw(out.year, out.month);
    if (out.day > limit) {
        out.day = limit;
    }
    return out;
}

result<date_time> add_years(const date_time& value, int years) {
    std::int64_t months = 0;
    if (!checked_mul_int64(static_cast<std::int64_t>(years), 12, months) ||
        months < INT32_MIN || months > INT32_MAX) {
        return rpc_error(rpc_code::invalid_argument, "year arithmetic overflows");
    }
    return add_months(value, static_cast<int>(months));
}

result<date_time> add_days(const date_time& value, std::int64_t days) {
    if (const status check = value.validate(); !check.ok()) {
        return check.error();
    }
    std::int64_t day = 0;
    std::int64_t second_of_day = 0;
    std::int32_t nanos = 0;
    split_civil(value, day, second_of_day, nanos);
    std::int64_t target = 0;
    if (!checked_add_int64(day, days, target)) {
        return rpc_error(rpc_code::invalid_argument, "day arithmetic overflows");
    }
    return from_split(target, second_of_day, nanos, value);
}

result<date_time> add_hours(const date_time& value, std::int64_t hours) {
    std::int64_t seconds = 0;
    if (!checked_mul_int64(hours, 3600, seconds)) {
        return rpc_error(rpc_code::invalid_argument, "hour arithmetic overflows");
    }
    return add_seconds(value, seconds);
}

result<date_time> add_minutes(const date_time& value, std::int64_t minutes) {
    std::int64_t seconds = 0;
    if (!checked_mul_int64(minutes, 60, seconds)) {
        return rpc_error(rpc_code::invalid_argument, "minute arithmetic overflows");
    }
    return add_seconds(value, seconds);
}

result<date_time> add_seconds(const date_time& value, std::int64_t seconds) {
    if (const status check = value.validate(); !check.ok()) {
        return check.error();
    }
    std::int64_t day = 0;
    std::int64_t second_of_day = 0;
    std::int32_t nanos = 0;
    split_civil(value, day, second_of_day, nanos);
    const std::int64_t day_delta = detail::floor_div(seconds, k_seconds_per_day);
    const std::int64_t within = detail::floor_mod(seconds, k_seconds_per_day);
    std::int64_t target_day = 0;
    if (!checked_add_int64(day, day_delta, target_day)) {
        return rpc_error(rpc_code::invalid_argument, "second arithmetic overflows");
    }
    return normalise(target_day, second_of_day + within, nanos, value);
}

result<date_time> add_nanoseconds(const date_time& value, std::int64_t nanoseconds) {
    if (const status check = value.validate(); !check.ok()) {
        return check.error();
    }
    std::int64_t day = 0;
    std::int64_t second_of_day = 0;
    std::int32_t nanos = 0;
    split_civil(value, day, second_of_day, nanos);
    const std::int64_t day_delta =
        detail::floor_div(nanoseconds, k_nanoseconds_per_day);
    const std::int64_t within =
        detail::floor_mod(nanoseconds, k_nanoseconds_per_day);
    std::int64_t target_day = 0;
    if (!checked_add_int64(day, day_delta, target_day)) {
        return rpc_error(rpc_code::invalid_argument, "nanosecond arithmetic overflows");
    }
    const std::int64_t second_nanos =
        second_of_day * k_nanoseconds_per_second + nanos;
    return normalise(target_day, 0, second_nanos + within, value);
}

result<date_time> add_duration(const date_time& value, duration span) {
    return add_nanoseconds(value, span.total_nanoseconds());
}

result<duration> subtract(const date_time& later, const date_time& earlier) {
    const auto later_result = to_timestamp(later);
    if (!later_result.ok()) {
        return later_result.error();
    }
    const auto earlier_result = to_timestamp(earlier);
    if (!earlier_result.ok()) {
        return earlier_result.error();
    }
    return subtract(later_result.value(), earlier_result.value());
}

// --- timestamp conversions ---------------------------------------------------

result<date_time> to_offset(timestamp at, int offset_minutes) {
    if (offset_minutes < -1439 || offset_minutes > 1439) {
        return rpc_error(rpc_code::invalid_argument,
                         "offset is outside the supported range -1439..1439 minutes");
    }
    std::int64_t seconds = 0;
    std::int32_t nanos = 0;
    detail::split_timestamp(at, seconds, nanos);
    const std::int64_t local = seconds + static_cast<std::int64_t>(offset_minutes) * 60;
    const std::int64_t day = detail::floor_div(local, k_seconds_per_day);
    const std::int64_t second_of_day = detail::floor_mod(local, k_seconds_per_day);
    if (day < k_min_day || day > k_max_day) {
        return rpc_error(rpc_code::invalid_argument,
                         "instant is outside the supported year range 1..9999");
    }
    date_time out{};
    detail::civil_from_days(day, out.year, out.month, out.day);
    out.hour = static_cast<int>(second_of_day / 3600);
    out.minute = static_cast<int>((second_of_day / 60) % 60);
    out.second = static_cast<int>(second_of_day % 60);
    out.nanosecond = nanos;
    out.is_utc = offset_minutes == 0;
    out.offset_minutes = offset_minutes;
    return out;
}

result<date_time> to_utc(timestamp at) { return to_offset(at, 0); }

result<timestamp> to_timestamp(const date_time& value, int offset_minutes) {
    if (const status check = value.validate(); !check.ok()) {
        return check.error();
    }
    if (offset_minutes < -1439 || offset_minutes > 1439) {
        return rpc_error(rpc_code::invalid_argument,
                         "offset is outside the supported range -1439..1439 minutes");
    }
    std::int64_t seconds = 0;
    if (!detail::civil_to_unix_seconds(value, offset_minutes, seconds)) {
        return rpc_error(rpc_code::invalid_argument,
                         "instant is outside the supported range");
    }
    timestamp out{};
    if (!detail::make_timestamp(seconds, value.nanosecond, out)) {
        return rpc_error(rpc_code::invalid_argument,
                         "instant is outside the 64-bit nanosecond range");
    }
    return out;
}

result<timestamp> to_timestamp(const date_time& value) {
    return to_timestamp(value, value.is_utc ? 0 : value.offset_minutes);
}

// --- ISO 8601 / RFC 3339 -----------------------------------------------------
namespace {

bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

int digit_value(char c) noexcept { return c - '0'; }

// Reads exactly `count` digits starting at `at`.
bool read_digits(std::string_view text, std::size_t at, std::size_t count,
                 int& out) noexcept {
    if (at + count > text.size()) {
        return false;
    }
    int value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const char c = text[at + i];
        if (!is_digit(c)) {
            return false;
        }
        value = value * 10 + digit_value(c);
    }
    out = value;
    return true;
}

// The separator check lives inline in parse_common: strict RFC 3339 and the
// lenient ISO 8601 form differ in whether a space is accepted, so one helper
// cannot express both.

std::string four_digits(int value) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "%04d", value);
    return buffer;
}

// "+HH:MM" / "-HH:MM"; used by the ISO and RFC 3339 formatters.
std::string format_offset(int offset_minutes) {
    const bool negative = offset_minutes < 0;
    const int magnitude = negative ? -offset_minutes : offset_minutes;
    std::string out;
    out += negative ? '-' : '+';
    out += two(magnitude / 60);
    out += ':';
    out += two(magnitude % 60);
    return out;
}

// Shared parser. `strict_rfc3339` rejects the space separator and a missing
// zone designator, both of which RFC 3339 section 5.6 forbids while ISO 8601
// allows them.
result<date_time> parse_common(std::string_view text, bool strict_rfc3339) {
    // A date alone is ISO 8601 but not RFC 3339.
    if (strict_rfc3339 && text.size() <= 10) {
        return rpc_error(rpc_code::invalid_argument,
                         "RFC 3339 requires a time part");
    }

    int year = 0;
    int month = 0;
    int day = 0;
    if (text.size() < 10 || !read_digits(text, 0, 4, year) || text[4] != '-' ||
        !read_digits(text, 5, 2, month) || text[7] != '-' ||
        !read_digits(text, 8, 2, day)) {
        return rpc_error(rpc_code::invalid_argument,
                         "expected a YYYY-MM-DD date");
    }

    date_time value{};
    value.year = year;
    value.month = month;
    value.day = day;
    value.is_utc = false;
    value.offset_minutes = 0;

    if (text.size() == 10) {
        if (const status check = value.validate(); !check.ok()) {
            return check.error();
        }
        return value;
    }

    const char separator = text[10];
    // RFC 3339 section 5.6 allows only 'T' (or its lowercase form); the space
    // separator is an ISO 8601 leniency that strict parsing must reject.
    if (separator != 'T' && separator != 't' &&
        (strict_rfc3339 || separator != ' ')) {
        return rpc_error(rpc_code::invalid_argument,
                         "expected a 'T' separator between date and time");
    }
    if (text.size() < 19) {
        return rpc_error(rpc_code::invalid_argument,
                         "expected HH:MM:SS after the separator");
    }
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!read_digits(text, 11, 2, hour) || text[13] != ':' ||
        !read_digits(text, 14, 2, minute) || text[16] != ':' ||
        !read_digits(text, 17, 2, second)) {
        return rpc_error(rpc_code::invalid_argument,
                         "expected HH:MM:SS after the separator");
    }
    value.hour = hour;
    value.minute = minute;
    value.second = second;

    std::size_t at = 19;
    if (at < text.size() && text[at] == '.') {
        ++at;
        const std::size_t start = at;
        while (at < text.size() && is_digit(text[at])) {
            ++at;
        }
        const std::size_t digits = at - start;
        if (digits == 0 || digits > 9) {
            return rpc_error(rpc_code::invalid_argument,
                             "a fraction needs between 1 and 9 digits");
        }
        int fraction = 0;
        (void)read_digits(text, start, digits, fraction);
        // Left-pad to nine digits: ".5" is half a second.
        int scale = 1;
        for (std::size_t i = digits; i < 9; ++i) {
            scale *= 10;
        }
        value.nanosecond = fraction * scale;
    }

    if (at < text.size()) {
        const char designator = text[at];
        if (designator == 'Z' || designator == 'z') {
            ++at;
            value.is_utc = true;
            value.offset_minutes = 0;
        } else if (designator == '+' || designator == '-') {
            if (at + 6 > text.size() || text[at + 3] != ':') {
                return rpc_error(rpc_code::invalid_argument,
                                 "expected an offset of the form +HH:MM");
            }
            int offset_hour = 0;
            int offset_minute = 0;
            if (!read_digits(text, at + 1, 2, offset_hour) ||
                !read_digits(text, at + 4, 2, offset_minute)) {
                return rpc_error(rpc_code::invalid_argument,
                                 "expected an offset of the form +HH:MM");
            }
            if (offset_hour > 23 || offset_minute > 59) {
                return rpc_error(rpc_code::invalid_argument,
                                 "offset is out of range");
            }
            int offset = offset_hour * 60 + offset_minute;
            if (designator == '-') {
                offset = -offset;
            }
            value.offset_minutes = offset;
            // "+00:00" denotes UTC; RFC 3339 uses "Z" for the same thing.
            value.is_utc = offset == 0;
            at += 6;
        }
    }

    if (at != text.size()) {
        return rpc_error(rpc_code::invalid_argument,
                         "unexpected trailing characters");
    }
    if (strict_rfc3339 && !value.is_utc && value.offset_minutes == 0) {
        return rpc_error(rpc_code::invalid_argument,
                         "RFC 3339 requires a zone designator");
    }
    if (const status check = value.validate(); !check.ok()) {
        return check.error();
    }
    return value;
}

} // namespace

result<date_time> parse_iso8601(std::string_view text) {
    return parse_common(text, false);
}

result<date_time> parse_rfc3339(std::string_view text) {
    return parse_common(text, true);
}

result<std::string> format_iso8601(const date_time& value) {
    if (const status check = value.validate(); !check.ok()) {
        return check.error();
    }
    std::string out;
    out.reserve(35);
    out += four_digits(value.year);
    out += '-';
    out += two(value.month);
    out += '-';
    out += two(value.day);
    out += 'T';
    out += two(value.hour);
    out += ':';
    out += two(value.minute);
    out += ':';
    out += two(value.second);
    if (value.nanosecond != 0) {
        char fraction[16];
        if (value.nanosecond % 1000000 == 0) {
            std::snprintf(fraction, sizeof(fraction), ".%03d",
                          value.nanosecond / 1000000);
        } else if (value.nanosecond % 1000 == 0) {
            std::snprintf(fraction, sizeof(fraction), ".%06d",
                          value.nanosecond / 1000);
        } else {
            std::snprintf(fraction, sizeof(fraction), ".%09d", value.nanosecond);
        }
        out += fraction;
    }
    if (value.is_utc) {
        out += 'Z';
    } else if (value.offset_minutes != 0) {
        out += format_offset(value.offset_minutes);
    }
    return out;
}

result<std::string> format_rfc3339(const date_time& value) {
    if (const status check = value.validate(); !check.ok()) {
        return check.error();
    }
    if (!value.is_utc && value.offset_minutes == 0) {
        return rpc_error(rpc_code::invalid_argument,
                         "RFC 3339 requires a zone designator");
    }
    return format_iso8601(value);
}

} // namespace time
} // namespace crpc
