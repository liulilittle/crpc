// CRPC -- IANA time zones, offsets and DST-aware local time conversion.
//
// Data sources, in priority order:
//
//   1. A TZif directory (RFC 8536). The search list is `CRPC_TZDATA_DIR`,
//      `TZDIR`, `/usr/share/zoneinfo`, `/usr/lib/zoneinfo`, `/etc/zoneinfo`,
//      and on Windows the zoneinfo copies shipped by Git for Windows, MSYS2
//      and Cygwin (`%ProgramFiles%\Git\mingw64\share\zoneinfo`, ...).
//   2. The platform's local-zone rules: `GetDynamicTimeZoneInformation` +
//      `GetTimeZoneInformationForYear` on Windows, `/etc/localtime` on POSIX.
//   3. A small built-in fixed-offset table (UTC/GMT/Etc/GMT+N and a handful of
//      zones that have had no DST for decades, e.g. Asia/Shanghai).
//
// `is_fixed == true` means exactly that the answer came from the built-in
// fixed-offset table (or from a platform fallback that could not read any rule
// data): the offset is constant for every instant and no DST was applied. An
// answer that came from a TZif file or from the Windows rule database reports
// `is_fixed == false`, even when the zone has no DST this year, because the
// transition database was consulted. When no IANA data can be read, a named
// zone that is not in the fixed table is an error (`not_found`); the library
// never fabricates a DST rule. `timezone_database()` reports which source is
// active so callers can surface that honestly.
//
// Local-time conversion never guesses silently. A civil time can be:
//   * unique      -- exactly one UTC instant maps to it;
//   * ambiguous   -- a fall-back overlap maps two instants to it;
//   * nonexistent -- a spring-forward gap skips it.
// `resolve_local_time` rejects both ambiguous and nonexistent times by default;
// the explicit options select the earlier/later occurrence or shift a gap
// forward by the gap length.
#ifndef CRPC_TIME_TIMEZONE_HPP
#define CRPC_TIME_TIMEZONE_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/time/datetime.hpp>
#include <crpc/time/duration.hpp>

namespace crpc {
namespace time {

struct timezone_info {
    // The requested IANA name (or the platform's zone key for local_timezone).
    std::string name;
    // Offset east of UTC in whole minutes. For the historical TZif offsets
    // that are not a multiple of 60 seconds this is the floor of
    // `offset_seconds / 60`; the exact value is in `offset_seconds`.
    int offset_minutes = 0;
    bool dst = false;
    // True when the value came from a fixed-offset source: the offset is
    // constant for all instants and no transition rules were applied.
    bool is_fixed = false;
    // Additional information; the four fields above are the documented API.
    int offset_seconds = 0;
    std::string abbreviation;
};

// `is_fixed_offset()` is the predicate spelling used in the documentation.
[[nodiscard]] inline bool is_fixed_offset(const timezone_info& zone) noexcept {
    return zone.is_fixed;
}

// The offset in effect for `iana_name` at the instant `at`.
[[nodiscard]] CRPC_API result<timezone_info> timezone_at(std::string_view iana_name,
                                                         timestamp at);

// The operating system's local zone at `at`. On Windows this uses the dynamic
// DST rules (`GetTimeZoneInformationForYear`); on POSIX it reads
// `/etc/localtime` (or `TZ`), falling back to a fixed offset with
// `is_fixed == true` when no rule data is readable.
[[nodiscard]] CRPC_API result<timezone_info> local_timezone(timestamp at);

// All zone names the active source can serve, sorted and deduplicated. With a
// TZif directory this is the IANA database (leap-second and `posix/` variants
// excluded); with the fixed table it is that table's names.
[[nodiscard]] CRPC_API result<std::vector<std::string>> list_timezones();

// Which database is active, for diagnostics and honest reporting.
struct timezone_database_info {
    // True when a TZif directory (or the Windows rule database) was found.
    bool available = false;
    // True when the answers come from the built-in fixed-offset table only.
    bool fixed_only = true;
    // The directory backing named-zone lookups, empty when unavailable.
    std::string source;
    // True when the Windows dynamic-zone rules are used for `local_timezone`.
    bool platform_rules = false;
};
[[nodiscard]] CRPC_API timezone_database_info timezone_database();

// Convert an instant to local civil time in a named zone.
[[nodiscard]] CRPC_API result<date_time> to_local(std::string_view iana_name,
                                                  timestamp at);

enum class local_time_status : std::uint8_t {
    unique,
    ambiguous,
    nonexistent
};

struct local_time_options {
    enum class on_ambiguous : std::uint8_t {
        reject,  // return an error; the caller must choose
        earlier, // the first occurrence (the larger offset, usually DST)
        later    // the second occurrence
    };
    enum class on_nonexistent : std::uint8_t {
        reject,        // return an error
        shift_forward  // move forward by the gap length
    };

    on_ambiguous ambiguous = on_ambiguous::reject;
    on_nonexistent nonexistent = on_nonexistent::reject;

    [[nodiscard]] static constexpr local_time_options strict() noexcept {
        return local_time_options{};
    }
    [[nodiscard]] static constexpr local_time_options prefer_earlier() noexcept {
        return local_time_options{on_ambiguous::earlier, on_nonexistent::reject};
    }
    [[nodiscard]] static constexpr local_time_options prefer_later() noexcept {
        return local_time_options{on_ambiguous::later, on_nonexistent::reject};
    }
};

struct local_time_result {
    timestamp utc;
    local_time_status status = local_time_status::unique;
    // True when a nonexistent time was shifted forward per policy.
    bool adjusted = false;
    // The rule in effect at the resulting instant.
    timezone_info zone;
};

// Converts local civil fields in `iana_name` to a UTC instant. The value's own
// `is_utc`/`offset_minutes` are ignored. Errors:
//   * `invalid_argument` for invalid fields or an unrepresentable instant;
//   * `failed_precondition` for an ambiguous/nonexistent time under `reject`.
[[nodiscard]] CRPC_API result<local_time_result> resolve_local_time(
    std::string_view iana_name, const date_time& local,
    local_time_options options = {});

// Convenience form that rejects both DST irregularities.
[[nodiscard]] CRPC_API result<timestamp> local_to_utc(std::string_view iana_name,
                                                      const date_time& local);

} // namespace time
} // namespace crpc

#endif // CRPC_TIME_TIMEZONE_HPP

