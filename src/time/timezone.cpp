// CRPC -- time zone implementation.
//
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
//
// Three layers, tried in order:
//
//   1. A TZif directory (RFC 8536). This is the real IANA database. Both the
//      version 1 and version 2+ blocks are parsed; for a version 2+ file the
//      64-bit block is the one used, because the 32-bit block cannot describe
//      anything after 2038. The POSIX TZ footer is deliberately not used: the
//      footer states the current rule, not the historical record, and a wrong
//      extrapolation into the future would be worse than an honest range
//      limit. Instants after the last transition use the last transition's
//      type, which is what zoneinfo implementations do for files whose
//      transition tables end in 2037.
//   2. The Windows dynamic zone rules for `local_timezone` only. Named zones
//      are not mapped to Windows keys: the mapping is lossy (one Windows zone
//      covers several IANA zones) and a wrong mapping would fabricate DST
//      behaviour, which is exactly what this module refuses to do.
//   3. The built-in fixed-offset table (UTC, GMT, the Etc/GMT family). An
//      answer is reported fixed when the source has no transitions at all,
//      which is true for this table and also for a TZif file that declares a
//      constant zone (its transition count is zero).
#include <crpc/time/timezone.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#if defined(CRPC_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace crpc {
namespace time {
namespace {

// --- fixed table -------------------------------------------------------------

struct fixed_entry {
    const char* name;
    int offset_minutes;
    const char* abbreviation;
};

// A deliberately small table. Every entry is a constant offset for all time,
// which is true by definition for UTC/GMT and the Etc/GMT family. Named IANA
// zones are never listed here: the TZif database is the only honest source for
// them, and a zone that cannot be read reports an error rather than a made-up
// offset.
constexpr fixed_entry k_fixed_table[] = {
    {"UTC", 0, "UTC"},        {"UCT", 0, "UTC"},
    {"Universal", 0, "UTC"},  {"Zulu", 0, "UTC"},
    {"GMT", 0, "GMT"},        {"GMT0", 0, "GMT"},
    {"GMT+0", 0, "GMT"},      {"GMT-0", 0, "GMT"},
    {"Greenwich", 0, "GMT"},  {"Etc/UTC", 0, "UTC"},
    {"Etc/UCT", 0, "UTC"},    {"Etc/Universal", 0, "UTC"},
    {"Etc/Zulu", 0, "UTC"},   {"Etc/GMT", 0, "GMT"},
    {"Etc/GMT0", 0, "GMT"},   {"Etc/GMT+0", 0, "GMT"},
    {"Etc/GMT-0", 0, "GMT"},  {"Etc/Greenwich", 0, "GMT"},
};

constexpr fixed_entry k_etc_gmt_table[] = {
    {"Etc/GMT+1", -60, "-01"},   {"Etc/GMT+2", -120, "-02"},
    {"Etc/GMT+3", -180, "-03"},  {"Etc/GMT+4", -240, "-04"},
    {"Etc/GMT+5", -300, "-05"},  {"Etc/GMT+6", -360, "-06"},
    {"Etc/GMT+7", -420, "-07"},  {"Etc/GMT+8", -480, "-08"},
    {"Etc/GMT+9", -540, "-09"},  {"Etc/GMT+10", -600, "-10"},
    {"Etc/GMT+11", -660, "-11"}, {"Etc/GMT+12", -720, "-12"},
    {"Etc/GMT+13", -780, "-13"}, {"Etc/GMT+14", -840, "-14"},
    {"Etc/GMT-1", 60, "+01"},    {"Etc/GMT-2", 120, "+02"},
    {"Etc/GMT-3", 180, "+03"},   {"Etc/GMT-4", 240, "+04"},
    {"Etc/GMT-5", 300, "+05"},   {"Etc/GMT-6", 360, "+06"},
    {"Etc/GMT-7", 420, "+07"},   {"Etc/GMT-8", 480, "+08"},
    {"Etc/GMT-9", 540, "+09"},   {"Etc/GMT-10", 600, "+10"},
    {"Etc/GMT-11", 660, "+11"},  {"Etc/GMT-12", 720, "+12"},
    {"Etc/GMT-13", 780, "+13"},  {"Etc/GMT-14", 840, "+14"},
};

const fixed_entry* find_fixed(std::string_view name) noexcept {
    for (const fixed_entry& entry : k_fixed_table) {
        if (name == entry.name) {
            return &entry;
        }
    }
    for (const fixed_entry& entry : k_etc_gmt_table) {
        if (name == entry.name) {
            return &entry;
        }
    }
    return nullptr;
}

bool is_safe_zone_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 255) {
        return false;
    }
    // IANA names are components of letters, digits, '_', '-', '+', and '.',
    // separated by '/'. A leading '/', an empty component or a ".." component
    // is a path escape attempt or simply not a zone name.
    if (name.front() == '/' || name.back() == '/') {
        return false;
    }
    std::size_t component_length = 0;
    for (char c : name) {
        if (c == '/') {
            if (component_length == 0) {
                return false;
            }
            component_length = 0;
            continue;
        }
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                        c == '+' || c == '.';
        if (!ok) {
            return false;
        }
        ++component_length;
    }
    if (component_length == 0) {
        return false;
    }
    return name.find("..") == std::string_view::npos;
}

// --- TZif (RFC 8536) ---------------------------------------------------------

struct zone_type {
    std::int32_t offset_seconds = 0;
    bool dst = false;
    std::string abbreviation;
};

struct zone_data {
    std::string name;
    std::vector<std::int64_t> transitions;  // UTC seconds, ascending
    // One type per transition: the type that transition put into effect.
    std::vector<zone_type> types;
    // The type in effect before the first transition.
    zone_type initial{};
    // True when the source declares no transitions: constant for all instants.
    bool fixed = false;
};

std::uint32_t read_u32(const std::vector<char>& bytes, std::size_t at) noexcept {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at])) << 24) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 2])) << 8) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + 3]));
}

std::int64_t read_i32(const std::vector<char>& bytes, std::size_t at) noexcept {
    return static_cast<std::int32_t>(read_u32(bytes, at));
}

std::int64_t read_i64(const std::vector<char>& bytes, std::size_t at) noexcept {
    const std::uint64_t high = read_u32(bytes, at);
    const std::uint64_t low = read_u32(bytes, at + 4);
    return static_cast<std::int64_t>((high << 32) | low);
}

struct tzif_header {
    char version = '\0';
    std::uint32_t isutcnt = 0;
    std::uint32_t isstdcnt = 0;
    std::uint32_t leapcnt = 0;
    std::uint32_t timecnt = 0;
    std::uint32_t typecnt = 0;
    std::uint32_t charcnt = 0;
};

bool read_header(const std::vector<char>& bytes, std::size_t at,
                 tzif_header& header) noexcept {
    if (at > bytes.size() || bytes.size() - at < 44) {
        return false;
    }
    if (std::memcmp(bytes.data() + at, "TZif", 4) != 0) {
        return false;
    }
    header.version = bytes[at + 4];
    header.isutcnt = read_u32(bytes, at + 20);
    header.isstdcnt = read_u32(bytes, at + 24);
    header.leapcnt = read_u32(bytes, at + 28);
    header.timecnt = read_u32(bytes, at + 32);
    header.typecnt = read_u32(bytes, at + 36);
    header.charcnt = read_u32(bytes, at + 40);
    // RFC 8536 section 3.1: typecnt is at least one; every count is bounded so
    // a corrupt file cannot make the parser allocate gigabytes.
    if (header.typecnt == 0 || header.typecnt > 256 || header.charcnt == 0 ||
        header.charcnt > 1024 || header.timecnt > 100000 ||
        header.leapcnt > 10000 || header.isutcnt > 256 || header.isstdcnt > 256) {
        return false;
    }
    return true;
}

std::size_t block_size(const tzif_header& header, std::size_t time_size) noexcept {
    return 44 + static_cast<std::size_t>(header.timecnt) * time_size +
           static_cast<std::size_t>(header.timecnt) +
           static_cast<std::size_t>(header.typecnt) * 6 +
           static_cast<std::size_t>(header.charcnt) +
           static_cast<std::size_t>(header.leapcnt) * (time_size + 4) +
           static_cast<std::size_t>(header.isstdcnt) +
           static_cast<std::size_t>(header.isutcnt);
}

// Parses one block; `time_size` is 4 for version 1 and 8 for version 2+.
bool parse_block(const std::vector<char>& bytes, std::size_t at,
                 const tzif_header& header, std::size_t time_size, zone_data& out) {
    if (at > bytes.size() || bytes.size() - at < block_size(header, time_size)) {
        return false;
    }
    std::size_t cursor = at + 44;
    const std::size_t timecnt = header.timecnt;
    const std::size_t typecnt = header.typecnt;
    const std::size_t charcnt = header.charcnt;

    std::vector<std::int64_t> transitions(timecnt);
    for (std::size_t i = 0; i < timecnt; ++i) {
        const std::int64_t value = time_size == 8 ? read_i64(bytes, cursor)
                                                  : read_i32(bytes, cursor);
        cursor += time_size;
        if (i > 0 && value <= transitions[i - 1]) {
            return false;  // not ascending: reject rather than mis-search
        }
        transitions[i] = value;
    }
    std::vector<std::uint8_t> indices(timecnt);
    for (std::size_t i = 0; i < timecnt; ++i) {
        indices[i] = static_cast<std::uint8_t>(bytes[cursor + i]);
        if (indices[i] >= typecnt) {
            return false;
        }
    }
    cursor += timecnt;

    std::vector<zone_type> declared(typecnt);
    std::vector<std::uint8_t> abbreviation_index(typecnt);
    for (std::size_t i = 0; i < typecnt; ++i) {
        declared[i].offset_seconds =
            static_cast<std::int32_t>(read_i32(bytes, cursor));
        declared[i].dst = static_cast<std::uint8_t>(bytes[cursor + 4]) != 0;
        abbreviation_index[i] = static_cast<std::uint8_t>(bytes[cursor + 5]);
        cursor += 6;
    }
    const std::size_t abbreviations_at = cursor;
    cursor += charcnt;

    for (std::size_t i = 0; i < typecnt; ++i) {
        if (abbreviation_index[i] >= charcnt) {
            return false;
        }
        const char* text = bytes.data() + abbreviations_at + abbreviation_index[i];
        const std::size_t limit = charcnt - abbreviation_index[i];
        const std::size_t length = ::strnlen(text, limit);
        if (length == limit) {
            return false;  // abbreviation run is not NUL-terminated
        }
        declared[i].abbreviation.assign(text, length);
    }

    // The type before the first transition: the first standard-time type
    // (RFC 8536 section 4.1). If no type is standard, type 0 is the choice.
    std::size_t initial_candidate = 0;
    for (std::size_t i = 0; i < typecnt; ++i) {
        if (!declared[i].dst) {
            initial_candidate = i;
            break;
        }
    }

    out.transitions = std::move(transitions);
    out.initial = declared[initial_candidate];
    out.types.clear();
    out.types.reserve(timecnt);
    for (std::size_t i = 0; i < timecnt; ++i) {
        out.types.push_back(declared[indices[i]]);
    }
    out.fixed = out.transitions.empty();
    return true;
}

bool parse_tzif(const std::vector<char>& bytes, zone_data& out) {
    tzif_header first{};
    if (!read_header(bytes, 0, first)) {
        return false;
    }
    if (first.version == '2' || first.version == '3' || first.version == '4') {
        const std::size_t second_at = block_size(first, 4);
        tzif_header second{};
        if (second_at > bytes.size() || !read_header(bytes, second_at, second)) {
            return false;
        }
        return parse_block(bytes, second_at, second, 8, out);
    }
    if (first.version == '\0') {
        return parse_block(bytes, 0, first, 4, out);
    }
    return false;
}

// --- database discovery ------------------------------------------------------

std::vector<std::string> candidate_directories() {
    std::vector<std::string> dirs;
    auto append = [&dirs](const char* value) {
        if (value != nullptr && value[0] != '\0') {
            dirs.emplace_back(value);
        }
    };
    append(std::getenv("CRPC_TZDATA_DIR"));
    append(std::getenv("TZDIR"));
#if defined(CRPC_OS_WINDOWS)
    if (const char* base = std::getenv("ProgramFiles")) {
        dirs.push_back(std::string(base) + "\\Git\\mingw64\\share\\zoneinfo");
        dirs.push_back(std::string(base) + "\\Git\\usr\\share\\zoneinfo");
    }
    if (const char* base = std::getenv("ProgramFiles(x86)")) {
        dirs.push_back(std::string(base) + "\\Git\\mingw64\\share\\zoneinfo");
    }
#else
    dirs.emplace_back("/usr/share/zoneinfo");
    dirs.emplace_back("/usr/lib/zoneinfo");
    dirs.emplace_back("/etc/zoneinfo");
#endif
    return dirs;
}

bool looks_like_zoneinfo(const std::string& directory) {
    std::error_code ec;
    const std::filesystem::path probe =
        std::filesystem::path(directory) / std::filesystem::path("UTC");
    return std::filesystem::is_regular_file(probe, ec) && !ec;
}

const std::string& active_directory() {
    static const std::string directory = []() {
        for (const std::string& candidate : candidate_directories()) {
            if (looks_like_zoneinfo(candidate)) {
                return candidate;
            }
        }
        return std::string();
    }();
    return directory;
}

bool read_file(const std::filesystem::path& path, std::vector<char>& bytes) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.good()) {
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(stream),
                 std::istreambuf_iterator<char>());
    return !bytes.empty();
}

bool load_zone_file(std::string_view name, zone_data& out) {
    const std::string& directory = active_directory();
    if (directory.empty() || !is_safe_zone_name(name)) {
        return false;
    }
    std::vector<char> bytes;
    if (!read_file(std::filesystem::path(directory) /
                       std::filesystem::path(std::string(name)),
                   bytes) ||
        !parse_tzif(bytes, out)) {
        return false;
    }
    out.name.assign(name.data(), name.size());
    return true;
}

bool load_fixed(std::string_view name, zone_data& out) {
    const fixed_entry* entry = find_fixed(name);
    if (entry == nullptr) {
        return false;
    }
    out.name.assign(name.data(), name.size());
    out.transitions.clear();
    out.types.clear();
    out.initial.offset_seconds = entry->offset_minutes * 60;
    out.initial.dst = false;
    out.initial.abbreviation = entry->abbreviation;
    out.fixed = true;
    return true;
}

bool load_named_zone(std::string_view name, zone_data& out) {
    return load_zone_file(name, out) || load_fixed(name, out);
}

const zone_type& type_at(const zone_data& zone, std::int64_t utc_seconds) noexcept {
    if (zone.transitions.empty()) {
        return zone.initial;
    }
    const auto it = std::upper_bound(zone.transitions.begin(),
                                     zone.transitions.end(), utc_seconds);
    if (it == zone.transitions.begin()) {
        return zone.initial;
    }
    const std::size_t index =
        static_cast<std::size_t>(it - zone.transitions.begin()) - 1;
    return zone.types[index];
}

int floor_minutes(std::int64_t offset_seconds) noexcept {
    if (offset_seconds >= 0) {
        return static_cast<int>(offset_seconds / 60);
    }
    return -static_cast<int>((-offset_seconds + 59) / 60);
}

timezone_info make_info(const zone_data& zone, std::int64_t utc_seconds) {
    const zone_type& type = type_at(zone, utc_seconds);
    timezone_info info;
    info.name = zone.name;
    info.offset_seconds = type.offset_seconds;
    info.offset_minutes = floor_minutes(type.offset_seconds);
    info.dst = type.dst;
    info.is_fixed = zone.fixed;
    info.abbreviation = type.abbreviation;
    return info;
}

// --- Windows local-zone rules ------------------------------------------------

#if defined(CRPC_OS_WINDOWS)

// Converts a SYSTEMTIME rule to the UTC second of the transition. `bias_for_rule`
// is the total Windows bias (minutes west of UTC) in effect just before the
// transition: standard bias for a spring-forward, daylight bias for a
// fall-back. Windows expresses the rule time in local wall-clock time.
bool rule_transition_utc(const SYSTEMTIME& rule, int year, int bias_for_rule,
                         std::int64_t& out) {
    if (rule.wMonth < 1 || rule.wMonth > 12 || rule.wDay < 1 || rule.wDay > 5 ||
        rule.wDayOfWeek > 6) {
        return false;
    }
    int day = 0;
    if (rule.wDay == 5) {
        // 5 means the last occurrence of wDayOfWeek in the month.
        const auto last = days_in_month(year, rule.wMonth);
        if (!last.ok()) {
            return false;
        }
        date_time probe{};
        probe.year = year;
        probe.month = rule.wMonth;
        probe.day = last.value();
        const auto weekday_value = day_of_week(probe);
        if (!weekday_value.ok()) {
            return false;
        }
        const int delta = (static_cast<int>(weekday_value.value()) -
                           static_cast<int>(rule.wDayOfWeek) + 7) %
                          7;
        day = last.value() - delta;
    } else {
        date_time first{};
        first.year = year;
        first.month = rule.wMonth;
        first.day = 1;
        const auto weekday_value = day_of_week(first);
        if (!weekday_value.ok()) {
            return false;
        }
        const int delta = (static_cast<int>(rule.wDayOfWeek) -
                           static_cast<int>(weekday_value.value()) + 7) %
                          7;
        day = 1 + delta + (static_cast<int>(rule.wDay) - 1) * 7;
    }
    date_time when{};
    when.year = year;
    when.month = rule.wMonth;
    when.day = day;
    when.hour = rule.wHour;
    when.minute = rule.wMinute;
    when.second = rule.wSecond;
    if (!when.is_valid()) {
        return false;
    }
    std::int64_t local = 0;
    if (!detail::civil_to_unix_seconds(when, 0, local)) {
        return false;
    }
    out = local + static_cast<std::int64_t>(bias_for_rule) * 60;
    return true;
}

struct windows_zone {
    bool valid = false;
    bool has_dst_rule = false;
    int bias_minutes = 0;
    int standard_bias_minutes = 0;
    int daylight_bias_minutes = 0;
    SYSTEMTIME daylight_date{};
    SYSTEMTIME standard_date{};
    std::string key_name;
};

windows_zone current_windows_zone() {
    windows_zone zone;
    DYNAMIC_TIME_ZONE_INFORMATION dynamic{};
    const DWORD rc = GetDynamicTimeZoneInformation(&dynamic);
    if (rc == TIME_ZONE_ID_INVALID) {
        return zone;
    }
    zone.bias_minutes = dynamic.Bias;
    zone.standard_bias_minutes = dynamic.StandardBias;
    zone.daylight_bias_minutes = dynamic.DaylightBias;
    zone.daylight_date = dynamic.DaylightDate;
    zone.standard_date = dynamic.StandardDate;
    zone.has_dst_rule = dynamic.DaylightDate.wMonth != 0 &&
                        dynamic.StandardDate.wMonth != 0;
    char narrow[128] = {0};
    const int length = WideCharToMultiByte(CP_UTF8, 0, dynamic.TimeZoneKeyName, -1,
                                           narrow, sizeof(narrow) - 1, nullptr,
                                           nullptr);
    if (length > 1) {
        zone.key_name.assign(narrow, static_cast<std::size_t>(length - 1));
    }
    zone.valid = true;
    return zone;
}

// Materialises the local zone's rules for the years surrounding `utc_seconds`.
zone_data materialize_windows_zone(const windows_zone& zone,
                                   std::int64_t utc_seconds) {
    zone_data out;
    out.name = zone.key_name.empty() ? "Local" : zone.key_name;
    const int standard_offset = -(zone.bias_minutes + zone.standard_bias_minutes);
    const int daylight_offset = -(zone.bias_minutes + zone.standard_bias_minutes +
                                 zone.daylight_bias_minutes);
    out.initial.offset_seconds = standard_offset * 60;
    out.initial.dst = false;
    if (!zone.has_dst_rule) {
        out.fixed = true;
        return out;
    }
    std::int64_t civil_day = detail::floor_div(utc_seconds, 86400);
    int year = 0;
    int month = 0;
    int dom = 0;
    detail::civil_from_days(civil_day, year, month, dom);
    struct pending {
        std::int64_t at;
        zone_type type;
    };
    std::vector<pending> events;
    for (int candidate = year - 1; candidate <= year + 1; ++candidate) {
        std::int64_t start = 0;
        std::int64_t end = 0;
        // A spring-forward is expressed in standard time; a fall-back in
        // daylight time. That is what Windows documents and what the IANA
        // rules also mean.
        if (!rule_transition_utc(zone.daylight_date, candidate,
                                 zone.bias_minutes,
                                 start)) {
            continue;
        }
        const int fall_bias =
            zone.bias_minutes + zone.daylight_bias_minutes;
        if (!rule_transition_utc(zone.standard_date, candidate, fall_bias, end)) {
            continue;
        }
        zone_type dst_type{};
        dst_type.offset_seconds = daylight_offset * 60;
        dst_type.dst = true;
        zone_type std_type{};
        std_type.offset_seconds = standard_offset * 60;
        std_type.dst = false;
        events.push_back({start, dst_type});
        events.push_back({end, std_type});
    }
    std::stable_sort(events.begin(), events.end(),
                     [](const pending& a, const pending& b) {
                         return a.at < b.at;
                     });
    out.transitions.reserve(events.size());
    out.types.reserve(events.size());
    for (const pending& event : events) {
        out.transitions.push_back(event.at);
        out.types.push_back(event.type);
    }
    out.fixed = out.transitions.empty();
    return out;
}

#endif // CRPC_OS_WINDOWS

// --- local zone --------------------------------------------------------------

result<timezone_info> local_zone_at(timestamp at) {
#if defined(CRPC_OS_WINDOWS)
    const windows_zone rules = current_windows_zone();
    if (!rules.valid) {
        zone_data utc;
        (void)load_fixed("UTC", utc);
        return make_info(utc, at.seconds());
    }
    const zone_data zone = materialize_windows_zone(rules, at.seconds());
    return make_info(zone, at.seconds());
#else
    // POSIX: honour `TZ` when it names a zone, then the system file.
    zone_data zone;
    bool loaded = false;
    if (const char* tz = std::getenv("TZ")) {
        std::string_view value(tz);
        if (!value.empty() && value.front() == ':') {
            value.remove_prefix(1);
        }
        if (!value.empty() && is_safe_zone_name(value)) {
            loaded = load_named_zone(value, zone);
        }
    }
    if (!loaded) {
        std::vector<char> bytes;
        if (read_file(std::filesystem::path("/etc/localtime"), bytes) &&
            parse_tzif(bytes, zone)) {
            zone.name = "Local";
            zone.fixed = zone.transitions.empty();
            loaded = true;
        }
    }
    if (!loaded) {
        (void)load_fixed("UTC", zone);
    }
    return make_info(zone, at.seconds());
#endif
}

} // namespace

// --- public API --------------------------------------------------------------

result<timezone_info> timezone_at(std::string_view iana_name, timestamp at) {
    if (iana_name.empty()) {
        return rpc_error(rpc_code::invalid_argument, "zone name is empty");
    }
    if (find_fixed(iana_name) == nullptr && !is_safe_zone_name(iana_name)) {
        return rpc_error(rpc_code::invalid_argument,
                         "not a valid zone name: " + std::string(iana_name));
    }
    zone_data zone;
    if (!load_named_zone(iana_name, zone)) {
        return rpc_error(rpc_code::not_found,
                         "time zone not found: " + std::string(iana_name));
    }
    return make_info(zone, at.seconds());
}

result<timezone_info> local_timezone(timestamp at) { return local_zone_at(at); }

result<std::vector<std::string>> list_timezones() {
    std::vector<std::string> names;
    const std::string& directory = active_directory();
    if (!directory.empty()) {
        std::error_code ec;
        const std::filesystem::path root(directory);
        std::filesystem::recursive_directory_iterator it(root, ec);
        const std::filesystem::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            std::error_code file_ec;
            const bool regular = it->is_regular_file(file_ec);
            if (file_ec || !regular) {
                continue;
            }
            const std::filesystem::path& path = it->path();
            const std::string relative =
                path.lexically_relative(root).generic_string();
            if (relative.empty()) {
                continue;
            }
            // Exclude non-zone files and the duplicate trees.
            if (relative.rfind("posix/", 0) == 0 ||
                relative.rfind("right/", 0) == 0 ||
                relative == "posixrules" || relative == "Factory" ||
                relative == "leapseconds" || relative == "leap-seconds.list" ||
                relative == "tzdata.zi" || relative == "zone.tab" ||
                relative == "zone1970.tab" || relative == "zonenow.tab" ||
                relative == "iso3166.tab" || relative == "SECURITY" ||
                relative.rfind("SystemV/", 0) == 0) {
                continue;
            }
            // A zone file starts with the TZif magic; the tables do not.
            std::vector<char> bytes;
            if (!read_file(path, bytes) || bytes.size() < 4 ||
                std::memcmp(bytes.data(), "TZif", 4) != 0) {
                continue;
            }
            names.push_back(relative);
        }
    }
    for (const fixed_entry& entry : k_fixed_table) {
        names.emplace_back(entry.name);
    }
    for (const fixed_entry& entry : k_etc_gmt_table) {
        names.emplace_back(entry.name);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

timezone_database_info timezone_database() {
    timezone_database_info info;
    const std::string& directory = active_directory();
    info.available = !directory.empty();
    info.fixed_only = !info.available;
    info.source = directory;
#if defined(CRPC_OS_WINDOWS)
    info.platform_rules = true;
#else
    info.platform_rules = false;
#endif
    return info;
}

result<date_time> to_local(std::string_view iana_name, timestamp at) {
    zone_data zone;
    if (!load_named_zone(iana_name, zone)) {
        return rpc_error(rpc_code::not_found,
                         "time zone not found: " + std::string(iana_name));
    }
    const zone_type& type = type_at(zone, at.seconds());
    const int offset_minutes = floor_minutes(type.offset_seconds);
    const auto local = to_offset(at, offset_minutes);
    if (!local.ok()) {
        return local.error();
    }
    date_time value = local.value();
    value.is_utc = type.offset_seconds == 0;
    value.offset_minutes = value.is_utc ? 0 : offset_minutes;
    return value;
}

result<local_time_result> resolve_local_time(std::string_view iana_name,
                                             const date_time& local,
                                             local_time_options options) {
    if (const status check = local.validate(); !check.ok()) {
        return check.error();
    }
    zone_data zone;
    if (!load_named_zone(iana_name, zone)) {
        return rpc_error(rpc_code::not_found,
                         "time zone not found: " + std::string(iana_name));
    }
    // Interpret the civil fields as if they were UTC; this is the anchor the
    // candidate offsets are measured from.
    std::int64_t anchor = 0;
    if (!detail::civil_to_unix_seconds(local, 0, anchor)) {
        return rpc_error(rpc_code::invalid_argument,
                         "local time is outside the supported range");
    }
    const std::int64_t offset_before = type_at(zone, anchor - 86400).offset_seconds;
    const std::int64_t offset_after = type_at(zone, anchor + 86400).offset_seconds;

    struct candidate {
        std::int64_t utc_seconds;
        zone_type type;
    };
    std::vector<candidate> candidates;
    for (std::int64_t offset : {offset_before, offset_after}) {
        const std::int64_t utc_seconds = anchor - offset;
        const zone_type& type = type_at(zone, utc_seconds);
        if (type.offset_seconds == offset) {
            const bool duplicate =
                std::any_of(candidates.begin(), candidates.end(),
                            [utc_seconds](const candidate& existing) {
                                return existing.utc_seconds == utc_seconds;
                            });
            if (!duplicate) {
                candidates.push_back(candidate{utc_seconds, type});
            }
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const candidate& a, const candidate& b) {
                  return a.utc_seconds < b.utc_seconds;
              });

    local_time_status status = local_time_status::unique;
    bool adjusted = false;
    if (candidates.size() == 2) {
        status = local_time_status::ambiguous;
        if (options.ambiguous == local_time_options::on_ambiguous::reject) {
            return rpc_error(rpc_code::failed_precondition,
                             "local time is ambiguous in this zone; choose the "
                             "earlier or later occurrence explicitly");
        }
        if (options.ambiguous == local_time_options::on_ambiguous::later) {
            candidates.erase(candidates.begin());
        } else {
            candidates.erase(candidates.begin() + 1);
        }
    } else if (candidates.empty()) {
        status = local_time_status::nonexistent;
        if (options.nonexistent ==
            local_time_options::on_nonexistent::reject) {
            return rpc_error(rpc_code::failed_precondition,
                             "local time does not exist in this zone (DST gap)");
        }
        // Shift forward by the gap length: the local time is advanced by the
        // offset difference and then interpreted with the post-transition
        // offset.
        const std::int64_t gap = offset_after - offset_before;
        if (gap <= 0) {
            return rpc_error(rpc_code::failed_precondition,
                             "local time does not exist and the gap length is "
                             "not determinable");
        }
        const std::int64_t shifted = anchor + gap;
        const std::int64_t utc_seconds = shifted - offset_after;
        const zone_type& type = type_at(zone, utc_seconds);
        if (type.offset_seconds != offset_after) {
            return rpc_error(rpc_code::failed_precondition,
                             "local time does not exist and could not be shifted");
        }
        candidates.push_back(candidate{utc_seconds, type});
        adjusted = true;
    }

    const candidate& chosen = candidates.front();
    timestamp utc{};
    if (!detail::make_timestamp(chosen.utc_seconds, local.nanosecond, utc)) {
        return rpc_error(rpc_code::invalid_argument,
                         "local time is outside the 64-bit nanosecond range");
    }
    local_time_result result;
    result.utc = utc;
    result.status = status;
    result.adjusted = adjusted;
    result.zone.name = zone.name;
    result.zone.offset_seconds = chosen.type.offset_seconds;
    result.zone.offset_minutes = floor_minutes(chosen.type.offset_seconds);
    result.zone.dst = chosen.type.dst;
    result.zone.is_fixed = zone.fixed;
    result.zone.abbreviation = chosen.type.abbreviation;
    return result;
}

result<timestamp> local_to_utc(std::string_view iana_name,
                               const date_time& local) {
    const auto resolved = resolve_local_time(iana_name, local);
    if (!resolved.ok()) {
        return resolved.error();
    }
    return resolved.value().utc;
}

} // namespace time
} // namespace crpc
