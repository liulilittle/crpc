// CRPC -- duration and timestamp implementation.
//
// The arithmetic here is deliberately explicit about overflow. `__int128` is
// not available on every supported compiler, so the checks use magnitude
// arithmetic in uint64_t, which is well defined and answers the same question.
#include <crpc/time/duration.hpp>

#include <chrono>
#include <cstdint>
#include <limits>

namespace crpc {
namespace time {
namespace {

constexpr std::int64_t k_ns_per_second = 1000000000LL;
constexpr std::int64_t k_ns_per_millisecond = 1000000LL;
constexpr std::int64_t k_ns_per_microsecond = 1000LL;

std::uint64_t magnitude(std::int64_t value) noexcept {
    return value < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(value)
                     : static_cast<std::uint64_t>(value);
}

bool checked_mul_int64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
    if (a == 0 || b == 0) {
        out = 0;
        return true;
    }
    const bool negative = (a < 0) != (b < 0);
    const std::uint64_t ua = magnitude(a);
    const std::uint64_t ub = magnitude(b);
    if (ub > std::numeric_limits<std::uint64_t>::max() / ua) {
        return false;
    }
    const std::uint64_t product = ua * ub;
    if (negative) {
        // The negative range has one more value than the positive one.
        if (product > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) +
                          1ULL) {
            return false;
        }
        out = static_cast<std::int64_t>(std::uint64_t{0} - product);
    } else {
        if (product >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        out = static_cast<std::int64_t>(product);
    }
    return true;
}

bool checked_add_int64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
    if (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) {
        return false;
    }
    if (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b) {
        return false;
    }
    out = a + b;
    return true;
}

bool checked_sub_int64(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
    if (b > 0 && a < std::numeric_limits<std::int64_t>::min() + b) {
        return false;
    }
    if (b < 0 && a > std::numeric_limits<std::int64_t>::max() + b) {
        return false;
    }
    out = a - b;
    return true;
}

result<duration> scaled(std::int64_t value, std::int64_t scale,
                        const char* what) {
    std::int64_t product = 0;
    if (!checked_mul_int64(value, scale, product)) {
        return rpc_error(rpc_code::invalid_argument,
                         std::string(what) + " is out of the 64-bit nanosecond range");
    }
    return duration::from_nanoseconds(product);
}

result<duration> scaled_seconds_floor(std::int64_t seconds) {
    // The smallest and largest whole seconds representable as int64 ns.
    if (seconds < -9223372036LL || seconds > 9223372036LL) {
        return rpc_error(rpc_code::invalid_argument,
                         "seconds are out of the 64-bit nanosecond range");
    }
    return duration::from_nanoseconds(seconds * k_ns_per_second);
}

} // namespace

// --- timestamp ---------------------------------------------------------------

result<timestamp> timestamp::from_seconds(std::int64_t seconds) {
    const auto span = scaled_seconds_floor(seconds);
    if (!span.ok()) {
        return span.error();
    }
    return timestamp(span.value().total_nanoseconds());
}

result<timestamp> timestamp::from_milliseconds(std::int64_t milliseconds) {
    if (milliseconds < -9223372036854LL || milliseconds > 9223372036854LL) {
        return rpc_error(rpc_code::invalid_argument,
                         "milliseconds are out of the 64-bit nanosecond range");
    }
    return timestamp(milliseconds * k_ns_per_millisecond);
}

result<timestamp> timestamp::from_microseconds(std::int64_t microseconds) {
    if (microseconds < -9223372036854775LL ||
        microseconds > 9223372036854775LL) {
        return rpc_error(rpc_code::invalid_argument,
                         "microseconds are out of the 64-bit nanosecond range");
    }
    return timestamp(microseconds * k_ns_per_microsecond);
}

result<timestamp> add(timestamp at, duration span) {
    std::int64_t sum = 0;
    if (!checked_add_int64(at.nanoseconds, span.total_nanoseconds(), sum)) {
        return rpc_error(rpc_code::invalid_argument,
                         "timestamp + duration overflows 64-bit nanoseconds");
    }
    return timestamp(sum);
}

result<timestamp> subtract(timestamp at, duration span) {
    std::int64_t difference = 0;
    if (!checked_sub_int64(at.nanoseconds, span.total_nanoseconds(), difference)) {
        return rpc_error(rpc_code::invalid_argument,
                         "timestamp - duration overflows 64-bit nanoseconds");
    }
    return timestamp(difference);
}

result<duration> subtract(timestamp later, timestamp earlier) {
    // Compute through seconds to cover the full 584-year span; a direct
    // nanosecond subtraction would wrap. `seconds` is exact for every
    // representable instant (its magnitude is below 2^34).
    const std::int64_t later_seconds = later.seconds();
    const std::int64_t earlier_seconds = earlier.seconds();
    const std::int64_t later_nanos = later.subsecond_nanoseconds();
    const std::int64_t earlier_nanos = earlier.subsecond_nanoseconds();
    const std::int64_t second_difference = later_seconds - earlier_seconds;
    const std::int64_t nano_difference = later_nanos - earlier_nanos;
    if (second_difference < -9223372036LL || second_difference > 9223372036LL) {
        return rpc_error(rpc_code::invalid_argument,
                         "timestamp difference overflows 64-bit nanoseconds");
    }
    std::int64_t scaled_seconds = 0;
    if (!checked_mul_int64(second_difference, k_ns_per_second, scaled_seconds)) {
        return rpc_error(rpc_code::invalid_argument,
                         "timestamp difference overflows 64-bit nanoseconds");
    }
    std::int64_t total = 0;
    if (!checked_add_int64(scaled_seconds, nano_difference, total)) {
        return rpc_error(rpc_code::invalid_argument,
                         "timestamp difference overflows 64-bit nanoseconds");
    }
    return duration::from_nanoseconds(total);
}

timestamp system_now() noexcept {
    const auto now = std::chrono::system_clock::now();
    const auto since_epoch = now.time_since_epoch();
    const auto seconds =
        std::chrono::duration_cast<std::chrono::seconds>(since_epoch);
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
        since_epoch - seconds);
    return timestamp(static_cast<std::int64_t>(seconds.count()) * k_ns_per_second +
                     static_cast<std::int64_t>(nanos.count()));
}

std::chrono::steady_clock::time_point monotonic_now() noexcept {
    return std::chrono::steady_clock::now();
}

// --- duration ----------------------------------------------------------------

result<duration> duration::from_microseconds(std::int64_t microseconds) {
    return scaled(microseconds, k_ns_per_microsecond, "microseconds");
}

result<duration> duration::from_milliseconds(std::int64_t milliseconds) {
    return scaled(milliseconds, k_ns_per_millisecond, "milliseconds");
}

result<duration> duration::from_seconds(std::int64_t seconds) {
    return scaled_seconds_floor(seconds);
}

result<duration> duration::from_minutes(std::int64_t minutes) {
    return scaled(minutes, 60LL * k_ns_per_second, "minutes");
}

result<duration> duration::from_hours(std::int64_t hours) {
    return scaled(hours, 3600LL * k_ns_per_second, "hours");
}

result<duration> duration::from_days(std::int64_t days) {
    return scaled(days, 86400LL * k_ns_per_second, "days");
}

result<duration> duration::absolute() const {
    if (nanoseconds_ == std::numeric_limits<std::int64_t>::min()) {
        return rpc_error(rpc_code::invalid_argument,
                         "the absolute value of duration::min() is not representable");
    }
    return duration(nanoseconds_ < 0 ? -nanoseconds_ : nanoseconds_);
}

result<duration> duration::checked_add(duration other) const {
    std::int64_t sum = 0;
    if (!checked_add_int64(nanoseconds_, other.nanoseconds_, sum)) {
        return rpc_error(rpc_code::invalid_argument, "duration addition overflows");
    }
    return duration(sum);
}

result<duration> duration::checked_subtract(duration other) const {
    std::int64_t difference = 0;
    if (!checked_sub_int64(nanoseconds_, other.nanoseconds_, difference)) {
        return rpc_error(rpc_code::invalid_argument,
                         "duration subtraction overflows");
    }
    return duration(difference);
}

result<duration> duration::checked_multiply(std::int64_t factor) const {
    std::int64_t product = 0;
    if (!checked_mul_int64(nanoseconds_, factor, product)) {
        return rpc_error(rpc_code::invalid_argument,
                         "duration multiplication overflows");
    }
    return duration(product);
}

result<duration> duration::checked_divide(std::int64_t divisor) const {
    if (divisor == 0) {
        return rpc_error(rpc_code::invalid_argument, "duration division by zero");
    }
    if (nanoseconds_ == std::numeric_limits<std::int64_t>::min() && divisor == -1) {
        return rpc_error(rpc_code::invalid_argument,
                         "duration division overflows");
    }
    return duration(nanoseconds_ / divisor);
}

} // namespace time
} // namespace crpc

