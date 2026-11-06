// CRPC -- locale-independent strftime-style formatting.
//
// The week-number definitions are the C ones, spelled out because a sign or
// off-by-one here is invisible until a boundary date:
//
//   %U = (day_of_year_0 + 7 - weekday_sunday0) / 7
//   %W = (day_of_year_0 + 7 - weekday_monday0) / 7
//
// Both are integer divisions, and both yield week 00 for the partial week
// before the first Sunday/Monday. Verified against CPython's `strftime`
// (which delegates to glibc) for the boundary dates listed in the tests.
#include <crpc/time/format.hpp>

#include <cstdio>
#include <string>

namespace crpc {
namespace time {
namespace {

const char* const k_short_weekdays[7] = {"Sun", "Mon", "Tue", "Wed",
                                         "Thu", "Fri", "Sat"};
const char* const k_long_weekdays[7] = {"Sunday",   "Monday", "Tuesday",
                                        "Wednesday", "Thursday", "Friday",
                                        "Saturday"};
const char* const k_short_months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
const char* const k_long_months[12] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December"};

void append_number(std::string& out, int value, int width) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%0*d", width, value);
    out += buffer;
}

} // namespace

status format_to(std::string& out, std::string_view pattern,
                 const date_time& value) {
    if (const status check = value.validate(); !check.ok()) {
        return check;
    }
    // The weekday and day-of-year are shared by several specifiers; compute
    // them once rather than per specifier.
    const auto weekday_value = day_of_week(value);
    const auto ordinal = day_of_year(value);
    if (!weekday_value.ok() || !ordinal.ok()) {
        return status(rpc_code::invalid_argument, "date is not computable");
    }
    const int sunday0 = static_cast<int>(weekday_value.value());
    const int monday0 = (sunday0 + 6) % 7;
    const int yday0 = ordinal.value() - 1;

    std::string scratch;
    scratch.reserve(pattern.size() + 16);
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c != '%') {
            scratch += c;
            continue;
        }
        if (i + 1 >= pattern.size()) {
            return status(rpc_code::invalid_argument,
                          "pattern ends with a lone '%'");
        }
        const char specifier = pattern[++i];
        switch (specifier) {
        case 'Y':
            append_number(scratch, value.year, 4);
            break;
        case 'm':
            append_number(scratch, value.month, 2);
            break;
        case 'd':
            append_number(scratch, value.day, 2);
            break;
        case 'H':
            append_number(scratch, value.hour, 2);
            break;
        case 'M':
            append_number(scratch, value.minute, 2);
            break;
        case 'S':
            append_number(scratch, value.second, 2);
            break;
        case 'f':
            append_number(scratch, value.nanosecond, 9);
            break;
        case 'j':
            append_number(scratch, ordinal.value(), 3);
            break;
        case 'U':
            append_number(scratch, (yday0 + 7 - sunday0) / 7, 2);
            break;
        case 'W':
            append_number(scratch, (yday0 + 7 - monday0) / 7, 2);
            break;
        case 'a':
            scratch += k_short_weekdays[sunday0];
            break;
        case 'A':
            scratch += k_long_weekdays[sunday0];
            break;
        case 'b':
            scratch += k_short_months[value.month - 1];
            break;
        case 'B':
            scratch += k_long_months[value.month - 1];
            break;
        case 'z': {
            // No zone information: an empty expansion, never the process zone.
            if (value.is_utc) {
                scratch += "+0000";
            } else if (value.offset_minutes != 0) {
                const bool negative = value.offset_minutes < 0;
                const int magnitude =
                    negative ? -value.offset_minutes : value.offset_minutes;
                scratch += negative ? '-' : '+';
                append_number(scratch, magnitude / 60, 2);
                append_number(scratch, magnitude % 60, 2);
            }
            break;
        }
        case 'Z':
            // Only UTC has a name this type can state exactly; an offset has
            // no abbreviation, and inventing "GMT+8" would not round-trip.
            if (value.is_utc) {
                scratch += "UTC";
            }
            break;
        case '%':
            scratch += '%';
            break;
        case 'c':
        case 'x':
        case 'X':
        case 'p':
        case 'I':
        case 'e':
        case 'D':
        case 'F':
        case 'T':
        case 'n':
        case 't':
        case 'C':
        case 'g':
        case 'G':
        case 'h':
        case 'k':
        case 'l':
        case 'r':
        case 'R':
        case 's':
        case 'u':
        case 'V':
        case 'w':
        case 'y':
        case 'E':
        case 'O':
        default:
            return status(rpc_code::invalid_argument,
                          std::string("unsupported format specifier '%") +
                              specifier + "'");
        }
    }
    // Commit only after the whole pattern succeeded: a partial expansion must
    // never reach the caller's buffer.
    out += scratch;
    return status{};
}

result<std::string> format(std::string_view pattern, const date_time& value) {
    std::string out;
    const status appended = format_to(out, pattern, value);
    if (!appended.ok()) {
        return appended.error();
    }
    return out;
}

} // namespace time
} // namespace crpc
