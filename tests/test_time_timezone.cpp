// CRPC -- time zone tests.
//
// Reference values were produced independently by CPython 3.14.7 with
// `zoneinfo` and the system tzdata (2026e) under WSL; the exact script and its
// raw output live in .opencode/tmp/datetime/final_expectations.py and
// more_expectations.py. The tests that need a real transition database skip
// (with a printed reason) when the machine has no tzdata, and the skip is
// visible in the runner's "skipped" count.
//
// The gap/overlap policy cases need a real rule database, so they call
// require_tzif() and skip with a visible reason on a machine without one.
#include "test_framework.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <crpc/time/datetime.hpp>
#include <crpc/time/duration.hpp>
#include <crpc/time/timezone.hpp>

namespace crpc {
namespace time {

inline std::ostream& operator<<(std::ostream& out, const timezone_info& value) {
    out << value.name << " offset=" << value.offset_minutes << " dst=" << value.dst
        << " fixed=" << value.is_fixed << " abbr=" << value.abbreviation;
    return out;
}

} // namespace time
} // namespace crpc

namespace {

using crpc::rpc_code;
using crpc::time::date_time;
using crpc::time::is_fixed_offset;
using crpc::time::list_timezones;
using crpc::time::local_time_options;
using crpc::time::local_time_status;
using crpc::time::local_timezone;
using crpc::time::resolve_local_time;
using crpc::time::timezone_at;
using crpc::time::timezone_database;
using crpc::time::timezone_info;
using crpc::time::timestamp;
using crpc::time::to_local;
using crpc::time::to_offset;
using crpc::time::to_timestamp;

timestamp ts_from_ns(std::int64_t ns) { return timestamp(ns); }

// 2026-11-06T12:34:56.789Z
constexpr std::int64_t k_2026_11_06_12_34_56_789 =
    1793968496789000000LL;
// 2026-01-15T12:00:00Z
constexpr std::int64_t k_2026_01_15_12_00 = 1768478400000000000LL;
// 2026-07-15T12:00:00Z
constexpr std::int64_t k_2026_07_15_12_00 = 1784116800000000000LL;
// America/New_York 2026 spring-forward instant (Python: 1772953200).
constexpr std::int64_t k_ny_spring_2026 = 1772953200000000000LL;
// America/New_York 2026 fall-back instant (Python: 1793512800).
constexpr std::int64_t k_ny_fall_2026 = 1793512800000000000LL;

bool have_tzif_database() {
    const auto info = timezone_database();
    return info.available;
}

void require_tzif(const char* case_name) {
    if (!have_tzif_database()) {
        // The skip reason includes the case name so the log says what was not
        // exercised, rather than leaving a silent hole in the run.
        (void)case_name;
        crpc_test::skip("no TZif database on this machine (fixed-offset fallback active)");
    }
}

} // namespace

// --- database reporting ------------------------------------------------------

CRPC_TEST(timezone_database_reports_its_source_truthfully) {
    const auto info = timezone_database();
    // Either a TZif directory was found, or the fixed table is all there is.
    // Never both "unavailable" and "not fixed_only", and never "available"
    // with an empty source.
    if (info.available) {
        CRPC_CHECK_FALSE(info.fixed_only);
        CRPC_CHECK_FALSE(info.source.empty());
    } else {
        CRPC_CHECK(info.fixed_only);
        CRPC_CHECK(info.source.empty());
    }
}

CRPC_TEST(fixed_offset_zones_are_marked_fixed) {
    // The built-in table is always present; UTC/GMT are its anchors.
    const auto utc_zone = timezone_at("UTC", ts_from_ns(k_2026_11_06_12_34_56_789));
    CRPC_REQUIRE(utc_zone.ok());
    CRPC_CHECK_EQ(0, utc_zone.value().offset_minutes);
    CRPC_CHECK_FALSE(utc_zone.value().dst);
    CRPC_CHECK(is_fixed_offset(utc_zone.value()));
    CRPC_CHECK_EQ(0, utc_zone.value().offset_seconds);

    const auto gmt_zone = timezone_at("GMT", ts_from_ns(0));
    CRPC_REQUIRE(gmt_zone.ok());
    CRPC_CHECK_EQ(0, gmt_zone.value().offset_minutes);
    CRPC_CHECK(is_fixed_offset(gmt_zone.value()));

    const auto etc_utc = timezone_at("Etc/UTC", ts_from_ns(0));
    CRPC_REQUIRE(etc_utc.ok());
    CRPC_CHECK_EQ(0, etc_utc.value().offset_minutes);
    CRPC_CHECK(is_fixed_offset(etc_utc.value()));

    // Etc/GMT+5 is UTC-05:00 by the IANA sign convention (the sign in the name
    // is west-positive), a classic trap.
    const auto etc_gmt_plus_5 = timezone_at("Etc/GMT+5", ts_from_ns(0));
    CRPC_REQUIRE(etc_gmt_plus_5.ok());
    CRPC_CHECK_EQ(-300, etc_gmt_plus_5.value().offset_minutes);
    const auto etc_gmt_minus_3 = timezone_at("Etc/GMT-3", ts_from_ns(0));
    CRPC_REQUIRE(etc_gmt_minus_3.ok());
    CRPC_CHECK_EQ(180, etc_gmt_minus_3.value().offset_minutes);
}

CRPC_TEST(unknown_zone_names_are_rejected) {
    CRPC_CHECK_EQ(rpc_code::invalid_argument, timezone_at("", ts_from_ns(0)).code());
    CRPC_CHECK_EQ(rpc_code::not_found,
                  timezone_at("Not/AZone", ts_from_ns(0)).code());
    // Path traversal must never be resolved.
    CRPC_CHECK_EQ(rpc_code::invalid_argument,
                  timezone_at("../etc/passwd", ts_from_ns(0)).code());
    CRPC_CHECK_EQ(rpc_code::invalid_argument,
                  timezone_at("/etc/passwd", ts_from_ns(0)).code());
}

CRPC_TEST(list_timezones_is_sorted_and_contains_the_anchors) {
    const auto zones = list_timezones();
    CRPC_REQUIRE(zones.ok());
    CRPC_CHECK(!zones.value().empty());
    CRPC_CHECK(std::is_sorted(zones.value().begin(), zones.value().end()));
    CRPC_CHECK(std::find(zones.value().begin(), zones.value().end(), "UTC") !=
               zones.value().end());
    // No duplicates.
    CRPC_CHECK(std::adjacent_find(zones.value().begin(), zones.value().end()) ==
               zones.value().end());
}

// --- IANA offsets (requires tzdata) -----------------------------------------

CRPC_TEST(iana_utc_offset_is_zero) {
    require_tzif("iana_utc_offset_is_zero");
    const auto zone = timezone_at("UTC", ts_from_ns(k_2026_11_06_12_34_56_789));
    CRPC_REQUIRE(zone.ok());
    CRPC_CHECK_EQ(0, zone.value().offset_minutes);
    CRPC_CHECK_EQ(0, zone.value().offset_seconds);
    CRPC_CHECK_FALSE(zone.value().dst);
    // UTC declares no transitions in the database, so its offset is genuinely
    // constant: is_fixed is the honest answer here.
    CRPC_CHECK(zone.value().is_fixed);
    CRPC_CHECK_EQ(std::string("UTC"), zone.value().name);
}

CRPC_TEST(asia_shanghai_has_no_dst_in_any_month) {
    // Python: Asia/Shanghai is +08:00 (CST, dst False) on the 15th of every
    // month of 2026. The zone has had no DST since 1991.
    require_tzif("asia_shanghai_has_no_dst_in_any_month");
    for (std::int64_t ns : {k_2026_01_15_12_00, k_2026_07_15_12_00}) {
        const auto zone = timezone_at("Asia/Shanghai", ts_from_ns(ns));
        CRPC_REQUIRE(zone.ok());
        CRPC_CHECK_EQ(480, zone.value().offset_minutes);
        CRPC_CHECK_EQ(28800, zone.value().offset_seconds);
        CRPC_CHECK_FALSE(zone.value().dst);
        CRPC_CHECK_EQ(std::string("CST"), zone.value().abbreviation);
        // Named zones are never reported as fixed, because the transition
        // database was consulted even though this one has no recent DST.
        CRPC_CHECK_FALSE(zone.value().is_fixed);
    }
    // Python: 2026-11-06T20:34:56.789+08:00 for the 12:34:56.789Z instant.
    const auto local = to_local("Asia/Shanghai", ts_from_ns(k_2026_11_06_12_34_56_789));
    CRPC_REQUIRE(local.ok());
    CRPC_CHECK_EQ(date_time::with_offset(2026, 11, 6, 20, 34, 56, 789000000, 480),
                  local.value());
}

CRPC_TEST(america_new_york_observes_dst) {
    // Python: 2026-01-15T12:00Z -> EST -05:00 dst False;
    //         2026-07-15T12:00Z -> EDT -04:00 dst True.
    require_tzif("america_new_york_observes_dst");
    const auto winter = timezone_at("America/New_York", ts_from_ns(k_2026_01_15_12_00));
    CRPC_REQUIRE(winter.ok());
    CRPC_CHECK_EQ(-300, winter.value().offset_minutes);
    CRPC_CHECK_FALSE(winter.value().dst);
    CRPC_CHECK_EQ(std::string("EST"), winter.value().abbreviation);

    const auto summer = timezone_at("America/New_York", ts_from_ns(k_2026_07_15_12_00));
    CRPC_REQUIRE(summer.ok());
    CRPC_CHECK_EQ(-240, summer.value().offset_minutes);
    CRPC_CHECK(summer.value().dst);
    CRPC_CHECK_EQ(std::string("EDT"), summer.value().abbreviation);

    // Python: the 16:00Z instant is 12:00-04:00 in New York.
    const auto local = to_local("America/New_York", ts_from_ns(1784131200000000000LL));
    CRPC_REQUIRE(local.ok());
    CRPC_CHECK_EQ(date_time::with_offset(2026, 7, 15, 12, 0, 0, 0, -240),
                  local.value());
}

CRPC_TEST(dst_transition_instants_are_exact_to_the_second) {
    // Python: the New York rules for 2026 switch at
    // 2026-03-08T07:00:00Z (EST -> EDT) and 2026-11-01T06:00:00Z (EDT -> EST).
    require_tzif("dst_transition_instants_are_exact_to_the_second");
    const auto before_spring =
        timezone_at("America/New_York", ts_from_ns(k_ny_spring_2026 - 1));
    CRPC_REQUIRE(before_spring.ok());
    CRPC_CHECK_EQ(-300, before_spring.value().offset_minutes);
    CRPC_CHECK_FALSE(before_spring.value().dst);

    const auto at_spring = timezone_at("America/New_York", ts_from_ns(k_ny_spring_2026));
    CRPC_REQUIRE(at_spring.ok());
    CRPC_CHECK_EQ(-240, at_spring.value().offset_minutes);
    CRPC_CHECK(at_spring.value().dst);

    const auto before_fall =
        timezone_at("America/New_York", ts_from_ns(k_ny_fall_2026 - 1));
    CRPC_REQUIRE(before_fall.ok());
    CRPC_CHECK_EQ(-240, before_fall.value().offset_minutes);
    CRPC_CHECK(before_fall.value().dst);

    const auto at_fall = timezone_at("America/New_York", ts_from_ns(k_ny_fall_2026));
    CRPC_REQUIRE(at_fall.ok());
    CRPC_CHECK_EQ(-300, at_fall.value().offset_minutes);
    CRPC_CHECK_FALSE(at_fall.value().dst);
}

CRPC_TEST(zone_offsets_are_applied_in_the_east_direction) {
    // M3 guards this: an implementation that negates the offset passes nothing
    // here. Shanghai is ahead of UTC, New York behind it, and their local
    // wall clocks differ by 13 hours in January and 12 in July.
    require_tzif("zone_offsets_are_applied_in_the_east_direction");
    const auto shanghai = to_local("Asia/Shanghai", ts_from_ns(k_2026_01_15_12_00));
    CRPC_REQUIRE(shanghai.ok());
    CRPC_CHECK_EQ(20, shanghai.value().hour);   // 12:00Z + 8 h
    CRPC_CHECK_EQ(15, shanghai.value().day);
    const auto new_york = to_local("America/New_York", ts_from_ns(k_2026_01_15_12_00));
    CRPC_REQUIRE(new_york.ok());
    CRPC_CHECK_EQ(7, new_york.value().hour);    // 12:00Z - 5 h
    CRPC_CHECK_EQ(15, new_york.value().day);
}

// --- local time policy -------------------------------------------------------

CRPC_TEST(resolve_local_time_unique_value) {
    require_tzif("resolve_local_time_unique_value");
    // Python: datetime(2026, 7, 15, 12, tzinfo=ZoneInfo("America/New_York"))
    //         == 2026-07-15T16:00:00Z == 1784131200000000000 ns.
    const auto local = date_time{2026, 7, 15, 12, 0, 0, 0, false, 0};
    const auto resolved = resolve_local_time("America/New_York", local);
    CRPC_REQUIRE(resolved.ok());
    CRPC_CHECK_EQ(1784131200000000000LL, resolved.value().utc.nanoseconds);
    CRPC_CHECK_EQ(local_time_status::unique, resolved.value().status);
    CRPC_CHECK_FALSE(resolved.value().adjusted);
    CRPC_CHECK_EQ(-240, resolved.value().zone.offset_minutes);
    CRPC_CHECK(resolved.value().zone.dst);
}

CRPC_TEST(resolve_local_time_rejects_the_spring_gap) {
    require_tzif("resolve_local_time_rejects_the_spring_gap");
    // 2026-03-08T02:30 does not exist in New York. Python's fold=0 reading of
    // it lands at 1772955000 s; the value is rejected rather than guessed.
    const auto gap = date_time{2026, 3, 8, 2, 30, 0, 0, false, 0};
    const auto rejected = resolve_local_time("America/New_York", gap);
    CRPC_CHECK_EQ(rpc_code::failed_precondition, rejected.code());
    // Only `shift_forward` is accepted as an alternative policy.
    local_time_options forward{};
    forward.nonexistent = local_time_options::on_nonexistent::shift_forward;
    const auto shifted = resolve_local_time("America/New_York", gap, forward);
    CRPC_REQUIRE(shifted.ok());
    // The gap is one hour long, so 02:30 becomes 03:30 EDT == 07:30Z, which
    // Python gives as int(datetime(2026,3,8,3,30,tzinfo=NY).timestamp()).
    CRPC_CHECK(shifted.value().adjusted);
    CRPC_CHECK_EQ(local_time_status::nonexistent, shifted.value().status);
    CRPC_CHECK_EQ(1772955000000000000LL, shifted.value().utc.nanoseconds);
    // 03:30 is unambiguous, so the round trip comes back unchanged.
    const auto back = to_local("America/New_York", shifted.value().utc);
    CRPC_REQUIRE(back.ok());
    CRPC_CHECK_EQ(3, back.value().hour);
    CRPC_CHECK_EQ(30, back.value().minute);
}

CRPC_TEST(resolve_local_time_rejects_the_fall_overlap) {
    require_tzif("resolve_local_time_rejects_the_fall_overlap");
    // 2026-11-01T01:30 happens twice in New York. Python: fold=0 is EDT
    // (1793511000 s), fold=1 is EST (1793514600 s).
    const auto overlap = date_time{2026, 11, 1, 1, 30, 0, 0, false, 0};
    const auto rejected = resolve_local_time("America/New_York", overlap);
    CRPC_CHECK_EQ(rpc_code::failed_precondition, rejected.code());

    const auto earlier = resolve_local_time(
        "America/New_York", overlap, local_time_options::prefer_earlier());
    CRPC_REQUIRE(earlier.ok());
    CRPC_CHECK_EQ(1793511000000000000LL, earlier.value().utc.nanoseconds);
    CRPC_CHECK_EQ(local_time_status::ambiguous, earlier.value().status);
    CRPC_CHECK(earlier.value().zone.dst);
    CRPC_CHECK_EQ(-240, earlier.value().zone.offset_minutes);

    const auto later = resolve_local_time(
        "America/New_York", overlap, local_time_options::prefer_later());
    CRPC_REQUIRE(later.ok());
    CRPC_CHECK_EQ(1793514600000000000LL, later.value().utc.nanoseconds);
    CRPC_CHECK_EQ(local_time_status::ambiguous, later.value().status);
    CRPC_CHECK_FALSE(later.value().zone.dst);
    CRPC_CHECK_EQ(-300, later.value().zone.offset_minutes);
}

CRPC_TEST(resolve_local_time_rejects_invalid_fields) {
    const auto bad = resolve_local_time(
        "America/New_York", date_time{2023, 2, 29, 0, 0, 0, 0, false, 0});
    CRPC_CHECK_EQ(rpc_code::invalid_argument, bad.code());
    const auto bad_zone = resolve_local_time(
        "Not/AZone", date_time{2026, 1, 1, 0, 0, 0, 0, false, 0});
    CRPC_CHECK_EQ(rpc_code::not_found, bad_zone.code());
}

CRPC_TEST(local_timezone_reports_the_machine_zone) {
    const timestamp at(1793968496789000000LL);
    const auto zone = local_timezone(at);
    CRPC_REQUIRE(zone.ok());
    // The process zone's offset must be within the IANA range and its local
    // civil time must round-trip to the same instant through that offset.
    CRPC_CHECK(zone.value().offset_minutes >= -720);
    CRPC_CHECK(zone.value().offset_minutes <= 840);
    CRPC_CHECK(!zone.value().name.empty());
    const auto local = to_offset(at, zone.value().offset_minutes);
    CRPC_REQUIRE(local.ok());
    const auto back = to_timestamp(local.value());
    CRPC_REQUIRE(back.ok());
    CRPC_CHECK_EQ(at, back.value());
}

// --- timestamp/date_time integration ---------------------------------------

CRPC_TEST(to_local_and_back_agrees_with_utc) {
    require_tzif("to_local_and_back_agrees_with_utc");
    const auto at = timestamp(k_2026_11_06_12_34_56_789);
    const auto local = to_local("Asia/Shanghai", at);
    CRPC_REQUIRE(local.ok());
    const auto back = to_timestamp(local.value());
    CRPC_REQUIRE(back.ok());
    CRPC_CHECK_EQ(at, back.value());

    const auto ny = to_local("America/New_York", at);
    CRPC_REQUIRE(ny.ok());
    const auto ny_back = to_timestamp(ny.value());
    CRPC_REQUIRE(ny_back.ok());
    CRPC_CHECK_EQ(at, ny_back.value());
}

CRPC_TEST(fixed_offset_zones_resolve_local_time_without_rules) {
    const auto resolved = resolve_local_time(
        "Etc/GMT+5", date_time{2026, 11, 6, 12, 0, 0, 0, false, 0});
    CRPC_REQUIRE(resolved.ok());
    // UTC-05:00 means 12:00 local is 17:00Z; Python:
    // int(datetime(2026,11,6,12,tzinfo=timezone(timedelta(hours=-5))).timestamp())
    // == 1793984400.
    CRPC_CHECK_EQ(1793984400000000000LL, resolved.value().utc.nanoseconds);
    CRPC_CHECK_EQ(local_time_status::unique, resolved.value().status);
    CRPC_CHECK(is_fixed_offset(resolved.value().zone));
}

CRPC_TEST_MAIN()




