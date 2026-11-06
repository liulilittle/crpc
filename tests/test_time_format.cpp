// CRPC -- locale-independent formatting tests.
//
// Expected strings come from CPython 3.14.7 `datetime.strftime` for the same
// civil value (verified under WSL; the Python `strftime` delegates to glibc
// and uses the C locale, which is exactly the reference this module claims to
// match), with two documented exceptions that the tests state explicitly:
//
//   * `%f` here is nanoseconds (Python's is microseconds), so its expected
//     value is written out digit by digit;
//   * `%z`/`%Z` on a value with no zone are empty here (Python would consult
//     the process zone), and `%Z` on a numeric offset is empty here because a
//     date_time carries no abbreviation.
//
// The raw Python output is in .opencode/tmp/datetime/format_expectations.py.
#include "test_framework.hpp"

#include <string>
#include <string_view>

#include <crpc/time/datetime.hpp>
#include <crpc/time/format.hpp>

namespace {

using crpc::rpc_code;
using crpc::time::date_time;
using crpc::time::format;
using crpc::time::format_to;

date_time utc(int y, int mo, int d, int h = 0, int mi = 0, int s = 0, int ns = 0) {
    return date_time::utc(y, mo, d, h, mi, s, ns);
}

// Python:
//   datetime(2026,11,6,12,34,56,789000,tzinfo=timezone.utc).strftime(pat)
// produced 'UTC|+0000|789000|44|44|310|Fri|Friday|Nov|November' for
// "%Z|%z|%f|%U|%W|%j|%a|%A|%b|%B" (the %f differs by resolution; see above).
const date_time k_reference = utc(2026, 11, 6, 12, 34, 56, 789000000);

} // namespace

CRPC_TEST(format_all_supported_specifiers) {
    // %z is always the numeric offset; UTC renders +0000 (Python: %z == +0000).
    // Only %Z prints the zone name.
    CRPC_CHECK_EQ(std::string("2026-11-06T12:34:56.789000000+0000"),
                  format("%Y-%m-%dT%H:%M:%S.%f%z", k_reference).value());
    // Python: %j == 310, %U == 44, %W == 44.
    CRPC_CHECK_EQ(std::string("310 44 44"), format("%j %U %W", k_reference).value());
    // Python: %a == Fri, %A == Friday, %b == Nov, %B == November.
    CRPC_CHECK_EQ(std::string("Fri Friday Nov November"),
                  format("%a %A %b %B", k_reference).value());
    CRPC_CHECK_EQ(std::string("UTC"), format("%Z", k_reference).value());
    CRPC_CHECK_EQ(std::string("%"), format("%%", k_reference).value());
    // Unrecognised characters pass through unchanged; a '%' not starting a
    // specifier is an error (checked separately).
    CRPC_CHECK_EQ(std::string("on 2026-11-06 at 12:34 (fine)"),
                  format("on %Y-%m-%d at %H:%M (fine)", k_reference).value());
    CRPC_CHECK_EQ(std::string(), format("", k_reference).value());
}

CRPC_TEST(format_zero_padding) {
    const auto value = utc(1, 1, 1, 0, 0, 0, 0);
    // Python: datetime(1,1,1).strftime("%Y-%m-%d %H:%M:%S") == "1-01-01
    // 00:00:00" -- Python does NOT pad %Y below 1000 (glibc does pad to
    // "0001"). This module pads, which is the ISO 8601 form and the documented
    // difference from both: %Y is exactly four digits for years 1..9999.
    CRPC_CHECK_EQ(std::string("0001-01-01 00:00:00"),
                  format("%Y-%m-%d %H:%M:%S", value).value());
    CRPC_CHECK_EQ(std::string("000000000"), format("%f", utc(2026, 1, 1)).value());
    CRPC_CHECK_EQ(std::string("001"), format("%j", utc(2026, 1, 1)).value());
}

CRPC_TEST(format_zone_specifiers) {
    // Python: datetime(2026,11,6,12,34,56,789000,
    //                  tzinfo=timezone(timedelta(hours=8))).strftime("%Z|%z")
    //       == "UTC+08:00|+0800". The day/month fields are the same as above.
    CRPC_CHECK_EQ(std::string("+0800"),
                  format("%z", date_time::with_offset(2026, 11, 6, 12, 34, 56, 0, 480))
                      .value());
    CRPC_CHECK_EQ(std::string("-0530"),
                  format("%z", date_time::with_offset(2026, 11, 6, 12, 34, 56, 0, -330))
                      .value());
    CRPC_CHECK_EQ(std::string("+0000"),
                  format("%z", date_time::with_offset(2026, 11, 6, 12, 34, 56, 0, 0))
                      .value());
    // A zoned value has no abbreviation here: empty rather than a fabricated
    // one.
    CRPC_CHECK_EQ(std::string(),
                  format("%Z", date_time::with_offset(2026, 11, 6, 12, 34, 56, 0, 480))
                      .value());
    // No zone: both empty (Python would print the process zone).
    const date_time no_zone{2026, 11, 6, 12, 34, 56, 0, false, 0};
    CRPC_CHECK_EQ(std::string(), format("%z", no_zone).value());
    CRPC_CHECK_EQ(std::string(), format("%Z", no_zone).value());
    CRPC_CHECK_EQ(std::string("2026-11-06T12:34:56"),
                  format("%Y-%m-%dT%H:%M:%S%z", no_zone).value());
}

CRPC_TEST(format_week_numbers_match_strftime_boundaries) {
    // Python strftime values for the same dates:
    //   2026-01-03 %U=00 %W=00, 2026-01-04 %U=01 %W=00,
    //   2026-01-05 %U=01 %W=01, 2023-01-01 %U=01 %W=00,
    //   2024-01-01 %U=00 %W=01, 2024-12-31 %U=52 %W=53,
    //   2021-01-01 %U=00 %W=00.
    struct row {
        date_time value;
        const char* u;
        const char* w;
    };
    const row rows[] = {
        {utc(2026, 1, 3), "00", "00"},
        {utc(2026, 1, 4), "01", "00"},
        {utc(2026, 1, 5), "01", "01"},
        {utc(2023, 1, 1), "01", "00"},
        {utc(2024, 1, 1), "00", "01"},
        {utc(2024, 12, 31), "52", "53"},
        {utc(2021, 1, 1), "00", "00"},
    };
    for (const row& entry : rows) {
        CRPC_CHECK_EQ(std::string(entry.u), format("%U", entry.value).value());
        CRPC_CHECK_EQ(std::string(entry.w), format("%W", entry.value).value());
    }
    // %U is Sunday-based: 2026-11-06 (Friday) is in the week starting Sunday
    // 2026-11-01, which is week 44. %W is Monday-based: week 44 starts Monday
    // 2026-11-02, also 44 (Python agrees for this date).
    CRPC_CHECK_EQ(std::string("44"), format("%U", k_reference).value());
    CRPC_CHECK_EQ(std::string("44"), format("%W", k_reference).value());
}

CRPC_TEST(format_rejects_unsupported_specifiers) {
    // Every one of these is a strftime specifier this module deliberately does
    // not support; silently passing it through would ship the literal text.
    const char* const unsupported[] = {
        "%c", "%x", "%X", "%p", "%I", "%e", "%D", "%F", "%T", "%n", "%t",
        "%C", "%g", "%G", "%h", "%k", "%l", "%r", "%R", "%s", "%u", "%V", "%w",
        "%y", "%E", "%O", "%q", "%J", "%K",
    };
    for (const char* pattern : unsupported) {
        CRPC_CHECK_EQ(rpc_code::invalid_argument, format(pattern, k_reference).code());
    }
    // A trailing '%' and an empty specifier are errors too.
    CRPC_CHECK_EQ(rpc_code::invalid_argument, format("abc%", k_reference).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, format("%", k_reference).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, format("%1", k_reference).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, format("%Y%", k_reference).code());
}

CRPC_TEST(format_rejects_invalid_dates) {
    CRPC_CHECK_EQ(rpc_code::invalid_argument, format("%Y", utc(2023, 2, 29)).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, format("%Y", utc(2024, 13, 1)).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, format("%Y", utc(2024, 1, 1, 24)).code());
}

CRPC_TEST(format_to_appends_only_on_success) {
    std::string out = "prefix:";
    const auto ok = format_to(out, "%Y/%m/%d", k_reference);
    CRPC_CHECK(ok.ok());
    CRPC_CHECK_EQ(std::string("prefix:2026/11/06"), out);
    // On failure the buffer is untouched.
    const auto failed = format_to(out, "%y", k_reference);
    CRPC_CHECK_EQ(rpc_code::invalid_argument, failed.code());
    CRPC_CHECK_EQ(std::string("prefix:2026/11/06"), out);
    // An invalid date fails before anything is appended.
    const auto failed_date = format_to(out, "%Y", utc(2023, 2, 29));
    CRPC_CHECK_EQ(rpc_code::invalid_argument, failed_date.code());
    CRPC_CHECK_EQ(std::string("prefix:2026/11/06"), out);
}

CRPC_TEST(format_leap_day_and_year_bounds) {
    CRPC_CHECK_EQ(std::string("2024-02-29"),
                  format("%Y-%m-%d", utc(2024, 2, 29)).value());
    CRPC_CHECK_EQ(std::string("366"), format("%j", utc(2024, 12, 31)).value());
    CRPC_CHECK_EQ(std::string("365"), format("%j", utc(2023, 12, 31)).value());
    CRPC_CHECK_EQ(std::string("9999-12-31"),
                  format("%Y-%m-%d", utc(9999, 12, 31)).value());
    CRPC_CHECK_EQ(std::string("1900-02-28"),
                  format("%Y-%m-%d", utc(1900, 2, 28)).value());
}

CRPC_TEST_MAIN()
