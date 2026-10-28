// CRPC -- the system-time text codec.
//
// A `system_clock::time_point` becomes `YYYY-MM-DD HH:MM:SS` in UTC - the shape
// every backend in this library already speaks for timestamp columns. Parsing
// accepts the same text plus an optional fractional part and an optional
// trailing UTC marker/offset, and treats an unmarked value as UTC (there is no
// timezone information for the server's session to add on this side). The civil
// date math (Howard Hinnant's public-domain days_from_civil / civil_from_days)
// is timezone-independent, so formatting and parsing never walk through the
// local timezone and cannot be off by a DST transition.
#include <crpc/orm/value.hpp>

#include <chrono>
#include <cstdio>
#include <cstdint>
#include <string>

namespace crpc {
namespace orm {
namespace values {
namespace {

[[nodiscard]] constexpr bool is_leap(std::int64_t year) noexcept {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

[[nodiscard]] constexpr int days_in_month(std::int64_t year, int month) noexcept {
    constexpr int lengths[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return lengths[month - 1] + (month == 2 && is_leap(year) ? 1 : 0);
}

// Days since 1970-01-01 for a civil date (proleptic Gregorian).
[[nodiscard]] constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m,
                                                     unsigned d) noexcept {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// Fills `*out` with a partial `%Y-%m-%d` from an epoch-days value.
[[nodiscard]] constexpr std::int64_t civil_from_days(std::int64_t z, int& y,
                                                     unsigned& m, unsigned& d) noexcept {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t cyear = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(cyear + (m <= 2));
    return cyear;
}

[[nodiscard]] constexpr std::int64_t floor_div(std::int64_t a, std::int64_t b) noexcept {
    const std::int64_t q = a / b;
    const std::int64_t r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}

} // namespace

std::string format_system_time(std::chrono::system_clock::time_point value) {
    const std::int64_t total = std::chrono::duration_cast<std::chrono::seconds>(
                                   value.time_since_epoch())
                                   .count();
    const std::int64_t days = floor_div(total, 86400);
    const std::int64_t rem = total - days * 86400;

    int year = 1970;
    unsigned month = 1;
    unsigned day = 1;
    (void)civil_from_days(days, year, month, day);

    const std::int64_t hour = rem / 3600;
    const std::int64_t minute = (rem % 3600) / 60;
    const std::int64_t second = rem % 60;

    char buffer[24] = {0};
    (void)std::snprintf(buffer, sizeof(buffer), "%04d-%02u-%02u %02lld:%02lld:%02lld",
                        year, month, day, static_cast<long long>(hour),
                        static_cast<long long>(minute), static_cast<long long>(second));
    return std::string(buffer);
}

status parse_system_time(std::string_view text,
                         std::chrono::system_clock::time_point& out) {
    const char* pos = text.data();
    const char* const end = text.data() + text.size();

    const auto fail = [](const std::string& reason) {
        return status{rpc_code::decode_error,
                      "malformed timestamp text (" + reason + ")"};
    };

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;

    const auto read_n = [&](int n, int& value) -> bool {
        if (end - pos < n) {
            return false;
        }
        int parsed = 0;
        for (int i = 0; i < n; ++i) {
            const char c = pos[i];
            if (c < '0' || c > '9') {
                return false;
            }
            parsed = parsed * 10 + (c - '0');
        }
        pos += n;
        value = parsed;
        return true;
    };
    const auto expect = [&](char c) -> bool {
        if (pos != end && *pos == c) {
            ++pos;
            return true;
        }
        return false;
    };

    if (!read_n(4, year) || !expect('-') || !read_n(2, month) || !expect('-') ||
        !read_n(2, day)) {
        return fail("expected YYYY-MM-DD");
    }
    if (pos != end && (*pos == ' ' || *pos == 'T' || *pos == 't')) {
        ++pos;
    } else {
        return fail("expected a date-time separator");
    }
    if (!read_n(2, hour) || !expect(':') || !read_n(2, minute) || !expect(':') ||
        !read_n(2, second)) {
        return fail("expected HH:MM:SS");
    }

    std::int64_t nanoseconds = 0;
    if (pos != end && *pos == '.') {
        ++pos;
        int digits = 0;
        std::int64_t fraction = 0;
        while (pos != end && *pos >= '0' && *pos <= '9' && digits < 9) {
            fraction = fraction * 10 + (*pos - '0');
            ++pos;
            ++digits;
        }
        for (int i = digits; i < 9; ++i) {
            fraction *= 10;
        }
        nanoseconds = fraction;
        while (pos != end && *pos >= '0' && *pos <= '9') {
            ++pos; // sub-nanosecond precision is dropped, not an error
        }
    }

    int offset_minutes = 0;
    if (pos != end) {
        if (*pos == 'Z' || *pos == 'z') {
            ++pos;
        } else if (*pos == '+' || *pos == '-') {
            const bool negative = *pos == '-';
            ++pos;
            int offset_hour = 0;
            int offset_minute = 0;
            if (!read_n(2, offset_hour)) {
                return fail("expected an offset hour");
            }
            if (pos != end && *pos == ':') {
                ++pos;
            }
            if (!read_n(2, offset_minute)) {
                return fail("expected an offset minute");
            }
            if (offset_hour > 23 || offset_minute > 59) {
                return fail("offset out of range");
            }
            offset_minutes = offset_hour * 60 + offset_minute;
            if (negative) {
                offset_minutes = -offset_minutes;
            }
        } else {
            return fail("trailing junk");
        }
    }
    if (pos != end) {
        return fail("trailing junk");
    }

    if (month < 1 || month > 12) {
        return fail("month out of range");
    }
    if (day < 1 || day > days_in_month(year, month)) {
        return fail("day out of range");
    }
    if (hour > 23 || minute > 59 || second > 59) {
        return fail("clock out of range");
    }

    const std::int64_t seconds =
        days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) *
            86400 +
        hour * 3600 + minute * 60 + second - static_cast<std::int64_t>(offset_minutes) * 60;

    out = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::seconds(seconds) + std::chrono::nanoseconds(nanoseconds)));
    return status{};
}

} // namespace values
} // namespace orm
} // namespace crpc