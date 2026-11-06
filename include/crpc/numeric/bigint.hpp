// CRPC -- fixed-width unsigned big integers.
//
// `uint_big<Bits>` is one template so the five public widths (128, 192, 256,
// 384, 512) never diverge: every algorithm is written once against
// `limb_count = Bits / 64` and instantiated. The representation is a
// little-endian array of 64-bit limbs (`limbs_[0]` holds the least significant
// bits), the same layout the rest of the library uses for wire integers.
//
// Design decisions that differ from built-in integer types:
//
//   * Overflow wraps. `+`, `-` and `*` return the low `Bits` bits with no
//     error and no check, exactly like a built-in unsigned type.
//
//   * Division by zero cannot be undefined behaviour in a library that must
//     never throw or corrupt state. `operator/` and `operator%` assert in
//     debug builds and return `max()` (the all-ones value) in every build;
//     `divmod()` is the checked form and returns `rpc_code::invalid_argument`.
//     Callers that cannot rule out a zero divisor must use `divmod()`.
//
//   * Shifts by a count >= Bits produce zero instead of the built-in
//     undefined behaviour. Shift by zero is a no-op and is exercised by tests.
//
//   * `from_be_bytes` accepts a span shorter than the value (zero-extended,
//     which is how a big-endian integer of arbitrary precision reads) but
//     rejects a longer one, because silently truncating high bytes would lose
//     data. The checked form returns `result<uint_big>`.
//
// The constants below are part of the type, not runtime state:
//   bit_width, limb_count, byte_count, max(), zero().
//
// Construction from an integral value follows the built-in conversion rules:
// a negative source is converted modulo 2^Bits (so `uint256(-1)` is `max()`).
//
// All arithmetic is constexpr in C++17 on both toolchains. MSVC has no
// `__int128` and its `_umul128` / `_udiv128` intrinsics are not usable during
// constant evaluation, so the 64x64->128 and 128/64 primitives take a portable
// path while `__builtin_is_constant_evaluated()` is true and the intrinsic path
// otherwise. GCC/Clang use `unsigned __int128` (constexpr-capable) throughout.
#ifndef CRPC_NUMERIC_BIGINT_HPP
#define CRPC_NUMERIC_BIGINT_HPP

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace crpc {
namespace numeric {
namespace detail {

// True while a constexpr evaluation is running. MSVC exposes the builtin in
// C++17 mode even though std::is_constant_evaluated() is C++20-only, so the
// builtin is preferred whenever it exists.
#if defined(__cpp_lib_is_constant_evaluated)
inline constexpr bool is_constant_evaluated_compat() noexcept {
    return std::is_constant_evaluated();
}
#elif defined(__GNUC__) || (defined(_MSC_VER) && _MSC_VER >= 1920)
inline constexpr bool is_constant_evaluated_compat() noexcept {
    return __builtin_is_constant_evaluated();
}
#else
inline constexpr bool is_constant_evaluated_compat() noexcept { return false; }
#endif

#if defined(__SIZEOF_INT128__)
// `__extension__` keeps -Wpedantic quiet on GCC: the type is a compiler
// extension but the code that uses it is guarded by this macro.
__extension__ typedef unsigned __int128 uint128_native;
#endif

// 64x64 -> 128 multiplication. `lo` receives the low half, `hi` the high half.
constexpr void mul_64x64(std::uint64_t a, std::uint64_t b, std::uint64_t& lo,
                         std::uint64_t& hi) noexcept {
#if defined(__SIZEOF_INT128__)
    const uint128_native product = static_cast<uint128_native>(a) *
                                   static_cast<uint128_native>(b);
    lo = static_cast<std::uint64_t>(product);
    hi = static_cast<std::uint64_t>(product >> 64);
#else
    if (!is_constant_evaluated_compat()) {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
        lo = _umul128(a, b, &hi);
        return;
#else
        // Fall through to the portable path on targets without the intrinsic.
#endif
    }
    const std::uint64_t a0 = a & 0xFFFFFFFFull;
    const std::uint64_t a1 = a >> 32;
    const std::uint64_t b0 = b & 0xFFFFFFFFull;
    const std::uint64_t b1 = b >> 32;
    const std::uint64_t p00 = a0 * b0;
    const std::uint64_t p01 = a0 * b1;
    const std::uint64_t p10 = a1 * b0;
    const std::uint64_t p11 = a1 * b1;
    const std::uint64_t mid = p01 + (p00 >> 32);
    const std::uint64_t mid2 = p10 + (mid & 0xFFFFFFFFull);
    lo = (mid2 << 32) | (p00 & 0xFFFFFFFFull);
    hi = p11 + (mid >> 32) + (mid2 >> 32);
#endif
}

// Number of leading zero bits in a nonzero 64-bit value.
constexpr unsigned leading_zeros_64(std::uint64_t value) noexcept {
    unsigned count = 0;
    std::uint64_t mask = 0x8000000000000000ull;
    while ((value & mask) == 0) {
        mask >>= 1;
        ++count;
    }
    return count;
}

// Unsigned 128/64 division: divides (hi:lo) by `divisor`.
// Precondition: divisor != 0 and hi < divisor; the quotient then fits in 64
// bits. Returns the quotient and writes the remainder through `remainder`.
constexpr std::uint64_t div_2by1(std::uint64_t hi, std::uint64_t lo,
                                 std::uint64_t divisor,
                                 std::uint64_t& remainder) noexcept {
#if defined(__SIZEOF_INT128__)
    const uint128_native dividend =
        (static_cast<uint128_native>(hi) << 64) | static_cast<uint128_native>(lo);
    remainder = static_cast<std::uint64_t>(dividend % divisor);
    return static_cast<std::uint64_t>(dividend / divisor);
#else
    if (!is_constant_evaluated_compat()) {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
        return _udiv128(hi, lo, divisor, &remainder);
#else
        // Fall through to the portable path.
#endif
    }
    std::uint64_t quotient = 0;
    std::uint64_t rem = hi;
    for (int bit = 63; bit >= 0; --bit) {
        const std::uint64_t incoming = (lo >> static_cast<unsigned>(bit)) & 1ull;
        const bool top = (rem & 0x8000000000000000ull) != 0;
        rem = (rem << 1) | incoming;
        quotient <<= 1;
        if (top || rem >= divisor) {
            rem -= divisor;
            quotient |= 1ull;
        }
    }
    remainder = rem;
    return quotient;
#endif
}

constexpr std::byte byte_from_unsigned(unsigned value) noexcept {
    return static_cast<std::byte>(static_cast<unsigned char>(value & 0xFFu));
}

constexpr unsigned unsigned_from_byte(std::byte value) noexcept {
    return static_cast<unsigned>(std::to_integer<unsigned char>(value));
}

} // namespace detail

// Fixed-width unsigned integer of `Bits` bits. See the file header for the
// semantics; the short version is "a built-in unsigned type that happens to be
// wider than 64 bits", with the two documented deviations on division by zero
// and oversized shifts.
template <std::size_t Bits>
class uint_big {
    static_assert(Bits % 64 == 0, "uint_big width must be a multiple of 64");
    static_assert(Bits >= 128, "uint_big width must be at least 128 bits");

public:
    using value_type = std::uint64_t;
    using limb_array = std::array<std::uint64_t, Bits / 64>;

    static constexpr std::size_t bit_width = Bits;
    static constexpr std::size_t limb_count = Bits / 64;
    static constexpr std::size_t byte_count = Bits / 8;

    // Result of a checked divmod: both halves at once, because computing the
    // remainder separately would repeat the whole long division.
    struct divmod_result {
        uint_big quotient;
        uint_big remainder;
    };

    // Zero-initialised. A default-constructed value is 0, matching the
    // framework's "no indeterminate state" rule; built-in integers leave the
    // value indeterminate here, which is the one place that is not copied.
    constexpr uint_big() noexcept = default;

    // Non-explicit on purpose: `uint256 x = 5;` must compile like a built-in
    // integer. The source value is zero-extended.
    constexpr uint_big(std::uint64_t value) noexcept : limbs_{} {
        limbs_[0] = value;
    }

    // Any integral type converts by the usual unsigned conversion rules. A
    // negative source is sign-extended across every limb, which is the value
    // the built-in conversion to a 2^Bits-wide unsigned type would produce:
    // `uint256(-1)` is `max()`, not `2^64 - 1`.
    template <typename T,
              typename std::enable_if<std::is_integral<T>::value, int>::type = 0>
    constexpr uint_big(T value) noexcept : limbs_{} {
        if constexpr (std::is_signed<T>::value) {
            if (value < T(0)) {
                for (std::size_t i = 0; i < limb_count; ++i) {
                    limbs_[i] = ~static_cast<std::uint64_t>(0);
                }
            }
        }
        limbs_[0] = static_cast<std::uint64_t>(value);
    }

    // --- Constants -----------------------------------------------------------

    static constexpr uint_big zero() noexcept { return uint_big{}; }

    static constexpr uint_big max() noexcept {
        uint_big value;
        for (std::size_t i = 0; i < limb_count; ++i) {
            value.limbs_[i] = ~static_cast<std::uint64_t>(0);
        }
        return value;
    }

    // --- Raw limb access -----------------------------------------------------
    //
    // Read-only and bounds-clamped: out-of-range indices yield 0 so that a
    // probing caller cannot trigger undefined behaviour. The representation is
    // little-endian; limb(0) is the least significant 64 bits.
    constexpr std::uint64_t limb(std::size_t index) const noexcept {
        return index < limb_count ? limbs_[index] : 0;
    }

    // Number of significant limbs (0 for zero).
    constexpr std::size_t significant_limbs() const noexcept {
        for (std::size_t i = limb_count; i-- > 0;) {
            if (limbs_[i] != 0) {
                return i + 1;
            }
        }
        return 0;
    }

    // --- Comparison ----------------------------------------------------------

    friend constexpr bool operator==(const uint_big& lhs,
                                     const uint_big& rhs) noexcept {
        for (std::size_t i = 0; i < limb_count; ++i) {
            if (lhs.limbs_[i] != rhs.limbs_[i]) {
                return false;
            }
        }
        return true;
    }

    friend constexpr bool operator!=(const uint_big& lhs,
                                     const uint_big& rhs) noexcept {
        return !(lhs == rhs);
    }

    friend constexpr bool operator<(const uint_big& lhs,
                                    const uint_big& rhs) noexcept {
        for (std::size_t i = limb_count; i-- > 0;) {
            if (lhs.limbs_[i] != rhs.limbs_[i]) {
                return lhs.limbs_[i] < rhs.limbs_[i];
            }
        }
        return false;
    }

    friend constexpr bool operator>(const uint_big& lhs,
                                    const uint_big& rhs) noexcept {
        return rhs < lhs;
    }

    friend constexpr bool operator<=(const uint_big& lhs,
                                     const uint_big& rhs) noexcept {
        return !(rhs < lhs);
    }

    friend constexpr bool operator>=(const uint_big& lhs,
                                     const uint_big& rhs) noexcept {
        return !(lhs < rhs);
    }

    // --- Arithmetic ----------------------------------------------------------

    constexpr uint_big& operator+=(const uint_big& other) noexcept {
        std::uint64_t carry = 0;
        for (std::size_t i = 0; i < limb_count; ++i) {
            const std::uint64_t sum = limbs_[i] + other.limbs_[i];
            const std::uint64_t carry1 = sum < limbs_[i] ? 1u : 0u;
            const std::uint64_t sum2 = sum + carry;
            const std::uint64_t carry2 = sum2 < sum ? 1u : 0u;
            limbs_[i] = sum2;
            carry = carry1 | carry2;
        }
        return *this;
    }

    constexpr uint_big& operator-=(const uint_big& other) noexcept {
        std::uint64_t borrow = 0;
        for (std::size_t i = 0; i < limb_count; ++i) {
            const std::uint64_t minuend = limbs_[i];
            const std::uint64_t subtrahend = other.limbs_[i];
            const std::uint64_t diff = minuend - subtrahend;
            const std::uint64_t borrow1 = minuend < subtrahend ? 1u : 0u;
            const std::uint64_t diff2 = diff - borrow;
            const std::uint64_t borrow2 = diff < borrow ? 1u : 0u;
            limbs_[i] = diff2;
            borrow = borrow1 | borrow2;
        }
        return *this;
    }

    // Schoolbook multiplication, keeping only the low `Bits` bits. The carry
    // chain for each row is 64-bit wide; terms that land above the top limb are
    // dropped, which is exactly the modular result.
    constexpr uint_big& operator*=(const uint_big& other) noexcept {
        limb_array result{};
        for (std::size_t i = 0; i < limb_count; ++i) {
            std::uint64_t carry = 0;
            for (std::size_t j = 0; i + j < limb_count; ++j) {
                std::uint64_t lo = 0;
                std::uint64_t hi = 0;
                detail::mul_64x64(limbs_[i], other.limbs_[j], lo, hi);
                const std::uint64_t column = result[i + j];
                const std::uint64_t with_carry = column + carry;
                const std::uint64_t carry1 = with_carry < column ? 1u : 0u;
                const std::uint64_t with_lo = with_carry + lo;
                const std::uint64_t carry2 = with_lo < with_carry ? 1u : 0u;
                result[i + j] = with_lo;
                carry = hi + carry1 + carry2;
            }
        }
        limbs_ = result;
        return *this;
    }

    // Unchecked divmod. Zero divisor is a programming error; the debug build
    // asserts, and both outputs are set to max() so a release build still has
    // deterministic (if meaningless) values.
    static constexpr divmod_result divmod_unchecked(const uint_big& dividend,
                                                    const uint_big& divisor) noexcept {
        divmod_result out{};
        if (divisor.is_zero()) {
            assert(false && "uint_big: division by zero");
            out.quotient = max();
            out.remainder = max();
            return out;
        }
        if (dividend < divisor) {
            out.remainder = dividend;
            return out;
        }

        const std::size_t divisor_limbs = divisor.significant_limbs();
        if (divisor_limbs == 1) {
            // Fast path: a single-limb divisor needs no normalization, and the
            // running remainder is always < divisor, so div_2by1's precondition
            // holds on every step.
            const std::uint64_t d = divisor.limbs_[0];
            std::uint64_t remainder = 0;
            for (std::size_t i = limb_count; i-- > 0;) {
                out.quotient.limbs_[i] =
                    detail::div_2by1(remainder, dividend.limbs_[i], d, remainder);
            }
            out.remainder.limbs_[0] = remainder;
            return out;
        }

        // Knuth, TAOCP vol. 2, 4.3.1, Algorithm D.
        const std::size_t m = dividend.significant_limbs() - divisor_limbs;
        const unsigned shift = detail::leading_zeros_64(divisor.limbs_[divisor_limbs - 1]);

        // One extra limb for the normalisation carry.
        std::array<std::uint64_t, limb_count + 1> u{};
        std::array<std::uint64_t, limb_count> v{};

        if (shift == 0) {
            for (std::size_t i = 0; i < limb_count; ++i) {
                u[i] = dividend.limbs_[i];
            }
            for (std::size_t i = 0; i < divisor_limbs; ++i) {
                v[i] = divisor.limbs_[i];
            }
        } else {
            std::uint64_t carry = 0;
            for (std::size_t i = 0; i < limb_count; ++i) {
                u[i] = (dividend.limbs_[i] << shift) | carry;
                carry = dividend.limbs_[i] >> (64 - shift);
            }
            u[limb_count] = carry;
            carry = 0;
            for (std::size_t i = 0; i < divisor_limbs; ++i) {
                v[i] = (divisor.limbs_[i] << shift) | carry;
                carry = divisor.limbs_[i] >> (64 - shift);
            }
        }

        for (std::size_t j = m + 1; j-- > 0;) {
            // Estimate the quotient digit and refine it against the second
            // divisor limb before the multiply-subtract step.
            std::uint64_t qhat = 0;
            std::uint64_t rhat = 0;
            bool rhat_overflow = false;
            if (u[j + divisor_limbs] == v[divisor_limbs - 1]) {
                qhat = ~static_cast<std::uint64_t>(0);
                rhat = u[j + divisor_limbs - 1] + v[divisor_limbs - 1];
                rhat_overflow = rhat < u[j + divisor_limbs - 1];
            } else {
                qhat = detail::div_2by1(u[j + divisor_limbs],
                                        u[j + divisor_limbs - 1],
                                        v[divisor_limbs - 1], rhat);
            }

            for (;;) {
                if (rhat_overflow) {
                    break;
                }
                std::uint64_t product_lo = 0;
                std::uint64_t product_hi = 0;
                detail::mul_64x64(qhat, v[divisor_limbs - 2], product_lo,
                                  product_hi);
                const bool too_large =
                    product_hi > rhat ||
                    (product_hi == rhat &&
                     product_lo > u[j + divisor_limbs - 2]);
                if (!too_large) {
                    break;
                }
                --qhat;
                const std::uint64_t next_rhat = rhat + v[divisor_limbs - 1];
                if (next_rhat < rhat) {
                    rhat_overflow = true;
                }
                rhat = next_rhat;
            }

            // u[j .. j+n] -= qhat * v[0 .. n-1]
            std::uint64_t borrow = 0;
            for (std::size_t i = 0; i < divisor_limbs; ++i) {
                std::uint64_t product_lo = 0;
                std::uint64_t product_hi = 0;
                detail::mul_64x64(qhat, v[i], product_lo, product_hi);
                const std::uint64_t scaled = product_lo + borrow;
                const std::uint64_t scaled_hi =
                    product_hi + (scaled < product_lo ? 1u : 0u);
                const std::uint64_t column = u[j + i];
                const std::uint64_t diff = column - scaled;
                const std::uint64_t borrow2 = column < scaled ? 1u : 0u;
                u[j + i] = diff;
                borrow = scaled_hi + borrow2;
            }
            const std::uint64_t top = u[j + divisor_limbs];
            const bool negative = top < borrow;
            u[j + divisor_limbs] = top - borrow;

            if (negative) {
                // The estimate was one too large: add the divisor back.
                --qhat;
                std::uint64_t carry = 0;
                for (std::size_t i = 0; i < divisor_limbs; ++i) {
                    const std::uint64_t sum1 = u[j + i] + v[i];
                    const std::uint64_t carry1 = sum1 < u[j + i] ? 1u : 0u;
                    const std::uint64_t sum2 = sum1 + carry;
                    const std::uint64_t carry2 = sum2 < sum1 ? 1u : 0u;
                    u[j + i] = sum2;
                    carry = carry1 | carry2;
                }
                u[j + divisor_limbs] += carry;
            }

            out.quotient.limbs_[j] = qhat;
        }

        // Undo the normalisation shift to recover the remainder.
        for (std::size_t i = 0; i < divisor_limbs; ++i) {
            if (shift == 0) {
                out.remainder.limbs_[i] = u[i];
            } else {
                out.remainder.limbs_[i] = (u[i] >> shift) | (u[i + 1] << (64 - shift));
            }
        }
        return out;
    }

    // Checked division. Use this when the divisor may be zero.
    [[nodiscard]] static result<divmod_result> divmod(const uint_big& dividend,
                                                      const uint_big& divisor) noexcept {
        if (divisor.is_zero()) {
            return rpc_error(rpc_code::invalid_argument,
                             "uint_big: division by zero");
        }
        return divmod_unchecked(dividend, divisor);
    }

    // Built-in semantics for a nonzero divisor. A zero divisor is a
    // programming error: debug builds assert, release builds return max().
    constexpr uint_big operator/(const uint_big& divisor) const noexcept {
        if (divisor.is_zero()) {
            assert(false && "uint_big: division by zero (operator/)");
            return max();
        }
        return divmod_unchecked(*this, divisor).quotient;
    }

    // Same contract as operator/: a zero divisor asserts in debug and yields
    // max() in release.
    constexpr uint_big operator%(const uint_big& divisor) const noexcept {
        if (divisor.is_zero()) {
            assert(false && "uint_big: division by zero (operator%)");
            return max();
        }
        return divmod_unchecked(*this, divisor).remainder;
    }

    constexpr uint_big operator+(const uint_big& other) const noexcept {
        uint_big copy = *this;
        copy += other;
        return copy;
    }

    constexpr uint_big operator-(const uint_big& other) const noexcept {
        uint_big copy = *this;
        copy -= other;
        return copy;
    }

    constexpr uint_big operator*(const uint_big& other) const noexcept {
        uint_big copy = *this;
        copy *= other;
        return copy;
    }

    // --- Increment / decrement ----------------------------------------------

    constexpr uint_big& operator++() noexcept {
        *this += uint_big(static_cast<std::uint64_t>(1));
        return *this;
    }

    constexpr uint_big operator++(int) noexcept {
        uint_big previous = *this;
        ++*this;
        return previous;
    }

    constexpr uint_big& operator--() noexcept {
        *this -= uint_big(static_cast<std::uint64_t>(1));
        return *this;
    }

    constexpr uint_big operator--(int) noexcept {
        uint_big previous = *this;
        --*this;
        return previous;
    }

    // --- Bitwise -------------------------------------------------------------

    constexpr uint_big& operator&=(const uint_big& other) noexcept {
        for (std::size_t i = 0; i < limb_count; ++i) {
            limbs_[i] &= other.limbs_[i];
        }
        return *this;
    }

    constexpr uint_big& operator|=(const uint_big& other) noexcept {
        for (std::size_t i = 0; i < limb_count; ++i) {
            limbs_[i] |= other.limbs_[i];
        }
        return *this;
    }

    constexpr uint_big& operator^=(const uint_big& other) noexcept {
        for (std::size_t i = 0; i < limb_count; ++i) {
            limbs_[i] ^= other.limbs_[i];
        }
        return *this;
    }

    constexpr uint_big operator&(const uint_big& other) const noexcept {
        uint_big copy = *this;
        copy &= other;
        return copy;
    }

    constexpr uint_big operator|(const uint_big& other) const noexcept {
        uint_big copy = *this;
        copy |= other;
        return copy;
    }

    constexpr uint_big operator^(const uint_big& other) const noexcept {
        uint_big copy = *this;
        copy ^= other;
        return copy;
    }

    constexpr uint_big operator~() const noexcept {
        uint_big copy;
        for (std::size_t i = 0; i < limb_count; ++i) {
            copy.limbs_[i] = ~limbs_[i];
        }
        return copy;
    }

    // Shifts by `count` bits. A count >= bit_width yields zero; count 0 is a
    // no-op. Both differ from built-in UB on oversized shifts in the same
    // direction as masking: the result is well defined for every input.
    constexpr uint_big operator<<(unsigned count) const noexcept {
        if (count >= bit_width) {
            return uint_big{};
        }
        const std::size_t limb_shift = count / 64;
        const unsigned bit_shift = count % 64;
        uint_big out;
        if (bit_shift == 0) {
            for (std::size_t i = limb_count; i-- > limb_shift;) {
                out.limbs_[i] = limbs_[i - limb_shift];
            }
        } else {
            for (std::size_t i = limb_count; i-- > limb_shift;) {
                const std::uint64_t high = limbs_[i - limb_shift] << bit_shift;
                const std::uint64_t low =
                    (i - limb_shift) > 0
                        ? limbs_[i - limb_shift - 1] >> (64 - bit_shift)
                        : 0;
                out.limbs_[i] = high | low;
            }
        }
        return out;
    }

    constexpr uint_big operator>>(unsigned count) const noexcept {
        if (count >= bit_width) {
            return uint_big{};
        }
        const std::size_t limb_shift = count / 64;
        const unsigned bit_shift = count % 64;
        uint_big out;
        if (bit_shift == 0) {
            for (std::size_t i = 0; i + limb_shift < limb_count; ++i) {
                out.limbs_[i] = limbs_[i + limb_shift];
            }
        } else {
            for (std::size_t i = 0; i + limb_shift < limb_count; ++i) {
                const std::uint64_t low = limbs_[i + limb_shift] >> bit_shift;
                const std::uint64_t high =
                    (i + limb_shift + 1) < limb_count
                        ? limbs_[i + limb_shift + 1] << (64 - bit_shift)
                        : 0;
                out.limbs_[i] = low | high;
            }
        }
        return out;
    }

    constexpr uint_big& operator<<=(unsigned count) noexcept {
        *this = *this << count;
        return *this;
    }

    constexpr uint_big& operator>>=(unsigned count) noexcept {
        *this = *this >> count;
        return *this;
    }

    // --- Conversion / inspection --------------------------------------------

    explicit constexpr operator bool() const noexcept { return !is_zero(); }

    constexpr bool is_zero() const noexcept {
        for (std::size_t i = 0; i < limb_count; ++i) {
            if (limbs_[i] != 0) {
                return false;
            }
        }
        return true;
    }

    constexpr std::uint64_t low64() const noexcept { return limbs_[0]; }

    // Bit `index` (0 = least significant). Out-of-range indices yield false;
    // this is an observation helper, not a substitute for masking.
    constexpr bool bit(std::size_t index) const noexcept {
        if (index >= bit_width) {
            return false;
        }
        return ((limbs_[index / 64] >> (index % 64)) & 1ull) != 0;
    }

    // --- Text conversion -----------------------------------------------------

    // Parses `text` in `base` (2..36). Both cases of digits are accepted, an
    // optional single leading '+' is allowed, and ASCII whitespace around the
    // number is ignored. For base 16 a "0x"/"0X" prefix and for base 2 a
    // "0b"/"0B" prefix are accepted when they are followed by at least one
    // digit. A negative value, an empty string, a digit outside the base, or a
    // value that does not fit in `Bits` is rejected with invalid_argument.
    [[nodiscard]] static result<uint_big> parse(std::string_view text,
                                                int base = 10) {
        if (base < 2 || base > 36) {
            return rpc_error(rpc_code::invalid_argument,
                             "uint_big::parse: base must be in [2, 36]");
        }

        std::size_t begin = 0;
        std::size_t end = text.size();
        const auto is_space = [](char c) {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                   c == '\v' || c == '\f';
        };
        while (begin < end && is_space(text[begin])) {
            ++begin;
        }
        while (end > begin && is_space(text[end - 1])) {
            --end;
        }
        if (begin == end) {
            return rpc_error(rpc_code::invalid_argument,
                             "uint_big::parse: empty input");
        }
        if (text[begin] == '+') {
            ++begin;
        } else if (text[begin] == '-') {
            return rpc_error(rpc_code::invalid_argument,
                             "uint_big::parse: negative value");
        }
        if (begin == end) {
            return rpc_error(rpc_code::invalid_argument,
                             "uint_big::parse: no digits");
        }

        // A radix prefix is only valid for the matching base and only if a
        // digit follows it.
        if (end - begin >= 3 && text[begin] == '0') {
            const char marker = text[begin + 1];
            if (base == 16 && (marker == 'x' || marker == 'X')) {
                begin += 2;
            } else if (base == 2 && (marker == 'b' || marker == 'B')) {
                begin += 2;
            }
        }

        const uint_big radix(static_cast<std::uint64_t>(base));
        const uint_big maximum = max();
        const uint_big quotient_limit = divmod_unchecked(maximum, radix).quotient;

        uint_big accumulator;
        bool saw_digit = false;
        for (std::size_t i = begin; i < end; ++i) {
            const char c = text[i];
            unsigned digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'z') {
                digit = static_cast<unsigned>(c - 'a') + 10u;
            } else if (c >= 'A' && c <= 'Z') {
                digit = static_cast<unsigned>(c - 'A') + 10u;
            } else {
                return rpc_error(rpc_code::invalid_argument,
                                 "uint_big::parse: invalid character");
            }
            if (digit >= static_cast<unsigned>(base)) {
                return rpc_error(rpc_code::invalid_argument,
                                 "uint_big::parse: digit out of range for base");
            }
            saw_digit = true;

            if (accumulator > quotient_limit) {
                return rpc_error(rpc_code::invalid_argument,
                                 "uint_big::parse: value overflows the width");
            }
            accumulator *= radix;
            if (accumulator > maximum - uint_big(digit)) {
                return rpc_error(rpc_code::invalid_argument,
                                 "uint_big::parse: value overflows the width");
            }
            accumulator += uint_big(digit);
        }
        if (!saw_digit) {
            return rpc_error(rpc_code::invalid_argument,
                             "uint_big::parse: no digits");
        }
        return accumulator;
    }

    // Lowercase digits, base 2..36. An out-of-range base is a programming
    // error: debug builds assert, release builds return an empty string.
    [[nodiscard]] std::string to_string(int base = 10) const {
        static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
        if (base < 2 || base > 36) {
            assert(false && "uint_big::to_string: base must be in [2, 36]");
            return std::string();
        }
        if (is_zero()) {
            return std::string("0");
        }

        uint_big copy = *this;
        const std::uint64_t radix = static_cast<std::uint64_t>(base);
        char buffer[bit_width];
        std::size_t length = 0;
        while (!copy.is_zero()) {
            const std::uint64_t remainder = divmod_small(copy, radix);
            buffer[length++] = digits[remainder];
        }
        for (std::size_t i = 0; i < length / 2; ++i) {
            const char saved = buffer[i];
            buffer[i] = buffer[length - 1 - i];
            buffer[length - 1 - i] = saved;
        }
        return std::string(buffer, length);
    }

    // --- Byte order ----------------------------------------------------------

    // Big-endian (network order) representation: out[0] is the most
    // significant byte.
    [[nodiscard]] constexpr std::array<std::byte, Bits / 8> to_be_bytes() const noexcept {
        std::array<std::byte, byte_count> out{};
        for (std::size_t i = 0; i < byte_count; ++i) {
            const std::size_t position = byte_count - 1 - i;
            const std::size_t limb_index = position / 8;
            const unsigned shift = static_cast<unsigned>((position % 8) * 8);
            out[i] = detail::byte_from_unsigned(static_cast<unsigned>(
                (limbs_[limb_index] >> shift) & 0xFFull));
        }
        return out;
    }

    // Reads a big-endian integer. Spans shorter than byte_count are
    // zero-extended on the most significant side; a longer span is rejected
    // because the high bytes would otherwise be lost.
    [[nodiscard]] static result<uint_big> from_be_bytes(span<const std::byte> data) noexcept {
        if (data.size() > byte_count) {
            return rpc_error(rpc_code::invalid_argument,
                             "uint_big::from_be_bytes: input is wider than the type");
        }
        uint_big value;
        for (std::size_t i = 0; i < data.size(); ++i) {
            const std::size_t position = data.size() - 1 - i;
            const std::size_t limb_index = position / 8;
            const unsigned shift = static_cast<unsigned>((position % 8) * 8);
            value.limbs_[limb_index] |=
                static_cast<std::uint64_t>(detail::unsigned_from_byte(data[i])) << shift;
        }
        return value;
    }

private:
    // Divides `value` in place by a single limb and returns the remainder.
    static constexpr std::uint64_t divmod_small(uint_big& value,
                                                std::uint64_t divisor) noexcept {
        std::uint64_t remainder = 0;
        for (std::size_t i = limb_count; i-- > 0;) {
            value.limbs_[i] =
                detail::div_2by1(remainder, value.limbs_[i], divisor, remainder);
        }
        return remainder;
    }

    std::array<std::uint64_t, Bits / 64> limbs_{};
};

using uint128 = uint_big<128>;
using uint192 = uint_big<192>;
using uint256 = uint_big<256>;
using uint384 = uint_big<384>;
using uint512 = uint_big<512>;

} // namespace numeric
} // namespace crpc

#endif // CRPC_NUMERIC_BIGINT_HPP
