// CRPC -- nanosecond durations and Unix timestamps.
//
// `timestamp` is an instant: a signed count of nanoseconds from
// 1970-01-01T00:00:00Z (the Unix epoch). `duration` is a signed span between
// two instants. Both store 64 bits of nanoseconds, so their representable
// range is the closed interval [INT64_MIN, INT64_MAX] nanoseconds, roughly
// 1677-09-21T00:12:43.145224192Z through 2262-04-11T23:47:16.854775807Z.
//
// Two rules make the negative half of that range behave the same as the
// positive half:
//
//   * `timestamp::seconds()` and friends round toward negative infinity (floor),
//     so -1 ns is second -1 with subsecond part 999999999, not second 0. This
//     is the rule Python uses and the rule that keeps
//     from_timestamp(ts).to_timestamp() exact for every representable ts.
//
//   * Duration component accessors (`days()`, `hours()`, ...) follow the
//     `System.TimeSpan` convention: every component carries the sign of the
//     whole value and is bounded by the next larger unit. `total_*` accessors
//     truncate toward zero.
//
// Overflow policy: checked factories and helpers return `result<head>`;
// unchecked arithmetic wraps in two's complement (defined through unsigned
// arithmetic, never undefined behaviour). Callers that can hit the ends of the
// range use the checked forms.
#ifndef CRPC_TIME_DURATION_HPP
#define CRPC_TIME_DURATION_HPP

#include <chrono>
#include <cstdint>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace time {

class duration;

// --- timestamp ---------------------------------------------------------------
//
// An instant on the UTC timeline. The `nanoseconds` member is public on
// purpose: serialisation and arithmetic code reads and writes it directly, and
// the type is a plain two's complement cell with no invariants beyond that.

struct timestamp {
    std::int64_t nanoseconds = 0;

    constexpr timestamp() noexcept = default;
    explicit constexpr timestamp(std::int64_t unix_nanoseconds) noexcept
        : nanoseconds(unix_nanoseconds) {}

    static constexpr timestamp from_nanoseconds(std::int64_t unix_nanoseconds) noexcept {
        return timestamp(unix_nanoseconds);
    }
    // Checked because seconds/milliseconds/microseconds can exceed the 64-bit
    // nanosecond range; the failure is `rpc_code::invalid_argument`.
    [[nodiscard]] static result<timestamp> from_seconds(std::int64_t seconds);
    [[nodiscard]] static result<timestamp> from_milliseconds(std::int64_t milliseconds);
    [[nodiscard]] static result<timestamp> from_microseconds(std::int64_t microseconds);

    // Floor division: the returned value is the second that contains the
    // instant, matching Python's `int(dt.timestamp())` for negative instants.
    [[nodiscard]] constexpr std::int64_t seconds() const noexcept;
    [[nodiscard]] constexpr std::int64_t milliseconds() const noexcept;
    [[nodiscard]] constexpr std::int64_t microseconds() const noexcept;
    [[nodiscard]] constexpr std::int64_t total_nanoseconds() const noexcept {
        return nanoseconds;
    }
    // The part of the instant inside its second: always in [0, 999999999].
    [[nodiscard]] constexpr std::int32_t subsecond_nanoseconds() const noexcept;

    [[nodiscard]] static constexpr timestamp min() noexcept {
        return timestamp(INT64_MIN);
    }
    [[nodiscard]] static constexpr timestamp max() noexcept {
        return timestamp(INT64_MAX);
    }
};

[[nodiscard]] constexpr bool operator==(timestamp a, timestamp b) noexcept {
    return a.nanoseconds == b.nanoseconds;
}
[[nodiscard]] constexpr bool operator!=(timestamp a, timestamp b) noexcept {
    return !(a == b);
}
[[nodiscard]] constexpr bool operator<(timestamp a, timestamp b) noexcept {
    return a.nanoseconds < b.nanoseconds;
}
[[nodiscard]] constexpr bool operator>(timestamp a, timestamp b) noexcept {
    return b < a;
}
[[nodiscard]] constexpr bool operator<=(timestamp a, timestamp b) noexcept {
    return !(b < a);
}
[[nodiscard]] constexpr bool operator>=(timestamp a, timestamp b) noexcept {
    return !(a < b);
}

// Adds/removes a span, checking the 64-bit nanosecond range.
[[nodiscard]] CRPC_API result<timestamp> add(timestamp at, duration span);
[[nodiscard]] CRPC_API result<timestamp> subtract(timestamp at, duration span);
// Exact difference: computes the seconds and subsecond parts separately, so
// the full range of two timestamps (about 584 years apart) fits even though
// the difference in nanoseconds does not fit a single int64. Returns
// `invalid_argument` when the span is not representable.
[[nodiscard]] CRPC_API result<duration> subtract(timestamp later, timestamp earlier);

// The current wall clock. `std::chrono::system_clock` is used and its
// time_point is converted to Unix nanoseconds.
[[nodiscard]] CRPC_API timestamp system_now() noexcept;

// The current tick of `std::chrono::steady_clock`. The origin is unspecified;
// only differences between two readings are meaningful. Never compare this
// with `system_now()` or with `timestamp` values.
[[nodiscard]] CRPC_API std::chrono::steady_clock::time_point monotonic_now() noexcept;

// --- duration ----------------------------------------------------------------

// A signed span, stored as 64-bit nanoseconds. All arithmetic that is marked
// "checked" reports 64-bit overflow as `rpc_code::invalid_argument`.
class duration {
public:
    constexpr duration() noexcept = default;

    // Total factories. from_nanoseconds cannot overflow; the others are
    // checked because multiplying the requested count can exceed INT64_MAX.
    static constexpr duration from_nanoseconds(std::int64_t nanoseconds) noexcept {
        return duration(nanoseconds);
    }
    [[nodiscard]] static result<duration> from_microseconds(std::int64_t microseconds);
    [[nodiscard]] static result<duration> from_milliseconds(std::int64_t milliseconds);
    [[nodiscard]] static result<duration> from_seconds(std::int64_t seconds);
    [[nodiscard]] static result<duration> from_minutes(std::int64_t minutes);
    [[nodiscard]] static result<duration> from_hours(std::int64_t hours);
    [[nodiscard]] static result<duration> from_days(std::int64_t days);

    // Unit helpers that always fit (one day is 86400 s and a day count is at
    // most INT64_MAX / 86400 for any useful value; the checked factory is the
    // safe entry point for arbitrary input).
    static constexpr duration zero() noexcept { return duration(0); }
    static constexpr duration min() noexcept { return duration(INT64_MIN); }
    static constexpr duration max() noexcept { return duration(INT64_MAX); }

    [[nodiscard]] constexpr std::int64_t total_nanoseconds() const noexcept {
        return nanoseconds_;
    }
    // Truncation is toward zero, matching TimeSpan.Total*.
    [[nodiscard]] constexpr std::int64_t total_microseconds() const noexcept {
        return nanoseconds_ / 1000;
    }
    [[nodiscard]] constexpr std::int64_t total_milliseconds() const noexcept {
        return nanoseconds_ / 1000000;
    }
    [[nodiscard]] constexpr std::int64_t total_seconds() const noexcept {
        return nanoseconds_ / 1000000000;
    }
    [[nodiscard]] constexpr std::int64_t total_minutes() const noexcept {
        return nanoseconds_ / 60000000000LL;
    }
    [[nodiscard]] constexpr std::int64_t total_hours() const noexcept {
        return nanoseconds_ / 3600000000000LL;
    }
    [[nodiscard]] constexpr std::int64_t total_days() const noexcept {
        return nanoseconds_ / 86400000000000LL;
    }

    // TimeSpan-style components: each carries the sign of the whole span.
    [[nodiscard]] constexpr std::int64_t days() const noexcept {
        return nanoseconds_ / 86400000000000LL;
    }
    [[nodiscard]] constexpr std::int64_t hours() const noexcept {
        return (nanoseconds_ / 3600000000000LL) % 24;
    }
    [[nodiscard]] constexpr std::int64_t minutes() const noexcept {
        return (nanoseconds_ / 60000000000LL) % 60;
    }
    [[nodiscard]] constexpr std::int64_t seconds() const noexcept {
        return (nanoseconds_ / 1000000000LL) % 60;
    }
    [[nodiscard]] constexpr std::int64_t milliseconds() const noexcept {
        return (nanoseconds_ / 1000000LL) % 1000;
    }
    [[nodiscard]] constexpr std::int64_t microseconds() const noexcept {
        return (nanoseconds_ / 1000LL) % 1000;
    }
    [[nodiscard]] constexpr std::int64_t nanoseconds() const noexcept {
        return nanoseconds_ % 1000LL;
    }

    [[nodiscard]] constexpr bool is_zero() const noexcept { return nanoseconds_ == 0; }
    [[nodiscard]] constexpr bool is_negative() const noexcept { return nanoseconds_ < 0; }

    // Absolute value; INT64_MIN has no positive counterpart and is an error.
    [[nodiscard]] result<duration> absolute() const;

    // Checked arithmetic: overflow is reported instead of wrapping.
    [[nodiscard]] result<duration> checked_add(duration other) const;
    [[nodiscard]] result<duration> checked_subtract(duration other) const;
    [[nodiscard]] result<duration> checked_multiply(std::int64_t factor) const;
    [[nodiscard]] result<duration> checked_divide(std::int64_t divisor) const;

    // Unchecked arithmetic. Overflow wraps in two's complement; use the
    // checked forms when either operand may be near duration::min()/max().
    friend constexpr duration operator+(duration a, duration b) noexcept {
        return duration(wrap_add(a.nanoseconds_, b.nanoseconds_));
    }
    friend constexpr duration operator-(duration a, duration b) noexcept {
        return duration(wrap_subtract(a.nanoseconds_, b.nanoseconds_));
    }
    friend constexpr duration operator-(duration value) noexcept {
        return duration(wrap_negate(value.nanoseconds_));
    }
    friend constexpr duration operator*(duration value, std::int64_t factor) noexcept {
        return duration(wrap_multiply(value.nanoseconds_, factor));
    }
    friend constexpr duration operator*(std::int64_t factor, duration value) noexcept {
        return value * factor;
    }
    friend constexpr duration operator/(duration value, std::int64_t divisor) noexcept {
        return duration(wrap_divide(value.nanoseconds_, divisor));
    }

    constexpr duration& operator+=(duration other) noexcept {
        nanoseconds_ = wrap_add(nanoseconds_, other.nanoseconds_);
        return *this;
    }
    constexpr duration& operator-=(duration other) noexcept {
        nanoseconds_ = wrap_subtract(nanoseconds_, other.nanoseconds_);
        return *this;
    }

    friend constexpr bool operator==(duration a, duration b) noexcept {
        return a.nanoseconds_ == b.nanoseconds_;
    }
    friend constexpr bool operator!=(duration a, duration b) noexcept {
        return !(a == b);
    }
    friend constexpr bool operator<(duration a, duration b) noexcept {
        return a.nanoseconds_ < b.nanoseconds_;
    }
    friend constexpr bool operator>(duration a, duration b) noexcept {
        return b < a;
    }
    friend constexpr bool operator<=(duration a, duration b) noexcept {
        return !(b < a);
    }
    friend constexpr bool operator>=(duration a, duration b) noexcept {
        return !(a < b);
    }

private:
    explicit constexpr duration(std::int64_t nanoseconds) noexcept
        : nanoseconds_(nanoseconds) {}

    // Well-defined two's complement wrap: the operation is performed in the
    // corresponding unsigned type and converted back.
    static constexpr std::int64_t wrap_add(std::int64_t a, std::int64_t b) noexcept {
        return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) +
                                         static_cast<std::uint64_t>(b));
    }
    static constexpr std::int64_t wrap_subtract(std::int64_t a, std::int64_t b) noexcept {
        return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) -
                                         static_cast<std::uint64_t>(b));
    }
    static constexpr std::int64_t wrap_negate(std::int64_t value) noexcept {
        return static_cast<std::int64_t>(std::uint64_t{0} -
                                         static_cast<std::uint64_t>(value));
    }
    static constexpr std::int64_t wrap_multiply(std::int64_t value,
                                                std::int64_t factor) noexcept {
        return static_cast<std::int64_t>(static_cast<std::uint64_t>(value) *
                                         static_cast<std::uint64_t>(factor));
    }
    static constexpr std::int64_t wrap_divide(std::int64_t value,
                                              std::int64_t divisor) noexcept {
        // Division by zero is a precondition violation; the checked form
        // reports it. The unchecked form returns the value unchanged rather
        // than invoking undefined behaviour.
        if (divisor == 0) {
            return value;
        }
        return value / divisor;
    }

    std::int64_t nanoseconds_ = 0;
};

// --- floor helpers used by the public accessors ------------------------------

namespace detail {

constexpr std::int64_t floor_div(std::int64_t value, std::int64_t divisor) noexcept {
    std::int64_t quotient = value / divisor;
    const std::int64_t remainder = value % divisor;
    if (remainder != 0 && ((remainder < 0) != (divisor < 0))) {
        --quotient;
    }
    return quotient;
}

constexpr std::int64_t floor_mod(std::int64_t value, std::int64_t divisor) noexcept {
    return value - floor_div(value, divisor) * divisor;
}

} // namespace detail

constexpr std::int64_t timestamp::seconds() const noexcept {
    return detail::floor_div(nanoseconds, 1000000000LL);
}

constexpr std::int64_t timestamp::milliseconds() const noexcept {
    return detail::floor_div(nanoseconds, 1000000LL);
}

constexpr std::int64_t timestamp::microseconds() const noexcept {
    return detail::floor_div(nanoseconds, 1000LL);
}

constexpr std::int32_t timestamp::subsecond_nanoseconds() const noexcept {
    return static_cast<std::int32_t>(detail::floor_mod(nanoseconds, 1000000000LL));
}

} // namespace time
} // namespace crpc

#endif // CRPC_TIME_DURATION_HPP
