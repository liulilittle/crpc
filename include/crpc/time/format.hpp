// CRPC -- locale-independent strftime-style formatting.
//
// Supported specifiers:
//
//   %Y  year, no padding below 1000                        %j  day of year 001..366
//   %m  month 01..12                                       %U  week 00..53, Sunday first
//   %d  day 01..31                                         %W  week 00..53, Monday first
//   %H  hour 00..23                                        %a  short English weekday
//   %M  minute 00..59                                      %A  full English weekday
//   %S  second 00..59                                      %b  short English month
//   %f  nanosecond 000000000                               %B  full English month
//   %z  +HHMM, or nothing when the value has no zone
//   %Z  "UTC" for a UTC value, nothing otherwise (a date_time carries no
//       abbreviation), and nothing when the value has no zone
//   %%  a literal '%'
//
// Differences from C `strftime`, all deliberate:
//
//   * `%Z`/`%z` describe the value's own `is_utc`/`offset_minutes`. A value with
//     no zone expands to an empty string instead of the process's local zone.
//     This library never reads the C locale or the local zone implicitly.
//   * `%z` is always `+HHMM` (no colon). RFC 3339's `+HH:MM` comes from
//     format_rfc3339, not from this function.
//   * `%f` is nine digits (strftime has no such specifier; it is the Python
//     `%f` idea with nanosecond, not microsecond, resolution).
//   * `%U`/`%W` follow the C definition exactly (week 00 is the partial week
//     before the first Sunday/Monday); the boundary dates in the tests were
//     verified against CPython's `strftime`, which delegates to the platform C
//     library, and against glibc's `date` for the same dates.
//   * `%c`, `%x`, `%X`, `%p`, `%I`, `%e`, `%D`, `%F`, `%T`, `%n`, `%t` and
//     every other specifier are *not* supported: they are rejected with
//     `invalid_argument` instead of being copied through, because a silent
//     passthrough turns a typo into production output.
//   * Names are always English (the C locale), never the process locale.
//
// On a formatting error (unsupported specifier, invalid date) the whole call
// fails; a partial string is never returned.
//
// Rule for a missing zone: `%z` and `%Z` on a zoned value are exact. On a
// value with `is_utc == false` and `offset_minutes == 0` both expand to an
// empty string, matching the ISO 8601 rule that a zone-less value carries no
// zone information. `%Z` on a UTC value expands to "UTC"; there is no
// abbreviation source for an arbitrary numeric offset, and inventing one
// (e.g. "GMT+8") would not round-trip, so an offset value gets an empty `%Z`.
// A caller that requires a zone uses format_rfc3339.
#ifndef CRPC_TIME_FORMAT_HPP
#define CRPC_TIME_FORMAT_HPP

#include <string>
#include <string_view>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/time/datetime.hpp>

namespace crpc {
namespace time {

// Formats `value` according to `pattern`; see the header comment for the
// supported specifiers and the deliberate differences from strftime.
[[nodiscard]] CRPC_API result<std::string> format(std::string_view pattern,
                                                  const date_time& value);

// Appends to `out`. On failure `out` is left unchanged (the function builds
// into a scratch buffer and commits only on success).
[[nodiscard]] CRPC_API status format_to(std::string& out, std::string_view pattern,
                                        const date_time& value);

} // namespace time
} // namespace crpc

#endif // CRPC_TIME_FORMAT_HPP
