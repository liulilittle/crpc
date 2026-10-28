// CRPC -- strict number parsing and safe formatting.
//
// Parsing consumes the whole input: a leading/trailing space, a stray suffix,
// an empty string or an out-of-range value is `invalid_argument` (overflow is
// distinguishable in the message, and `invalid_argument` is the code the
// library uses for a value that failed conversion). Formatting uses
// `std::to_chars`, which is buffer-safe by construction and never truncates,
// so no sprintf-style fixed buffer is involved.
#ifndef CRPC_TEXT_NUMBER_HPP
#define CRPC_TEXT_NUMBER_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace text {

[[nodiscard]] CRPC_API result<std::int64_t> parse_i64(std::string_view text);
[[nodiscard]] CRPC_API result<std::uint64_t> parse_u64(std::string_view text);
[[nodiscard]] CRPC_API result<std::int32_t> parse_int(std::string_view text);
[[nodiscard]] CRPC_API result<std::uint32_t> parse_uint(std::string_view text);

// Decimal floating point: an optional sign, digits, an optional fraction and
// an optional exponent. `inf`, `nan` and hex floats are rejected (a strict
// text form must not smuggle non-decimal values through).
[[nodiscard]] CRPC_API result<double> parse_double(std::string_view text);

// Shortest round-trip representation via std::to_chars: formatting cannot fail
// for a finite value, so these return std::string directly.
[[nodiscard]] CRPC_API std::string format(std::int64_t value);
[[nodiscard]] CRPC_API std::string format(std::uint64_t value);
[[nodiscard]] CRPC_API std::string format(double value);

} // namespace text
} // namespace crpc

#endif // CRPC_TEXT_NUMBER_HPP