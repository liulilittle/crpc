// CRPC -- strict number parsing and safe formatting.
// See include/crpc/text/number.hpp.
//
// Parsing uses std::from_chars: it is locale-independent, does not accept
// leading/trailing whitespace on its own, stops at the first non-digit so the
// caller can demand the whole input was consumed, and reports overflow
// explicitly. Formatting uses std::to_chars into a fixed work buffer, which
// cannot overrun by construction.
#include <charconv>
#include <cerrno>
#if defined(__ANDROID__)
#include <locale.h>
#endif
#include <cstddef>
#include <system_error>

#include <crpc/config.hpp>
#include <crpc/text/number.hpp>

namespace crpc {
namespace text {

namespace {

rpc_error invalid(const char* what) {
    return rpc_error{rpc_code::invalid_argument, what};
}

// The strict decimal grammar: [+-]? (digits [. digits?] | . digits)
// ([eE] [+-]? digits)?. Everything else -- inf, nan, hex floats, trailing
// garbage -- is rejected. MSVC's std::from_chars also parses "inf"/"nan",
// which a strict text parser must not accept, so we gate the input first.
bool valid_decimal_syntax(std::string_view text) noexcept {
    std::size_t i = 0;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
        ++i;
    }
    bool digits_before_dot = false;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
        ++i;
        digits_before_dot = true;
    }
    bool digits_after_dot = false;
    if (i < text.size() && text[i] == '.') {
        ++i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            ++i;
            digits_after_dot = true;
        }
    }
    if (!digits_before_dot && !digits_after_dot) {
        return false;
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            ++i;
        }
        bool exponent_digits = false;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            ++i;
            exponent_digits = true;
        }
        if (!exponent_digits) {
            return false;
        }
    }
    return i == text.size();
}

template <typename T>
result<T> parse_integer(std::string_view text) {
    if (text.empty()) {
        return invalid("expected an integer, got an empty string");
    }
    T value{};
    const auto outcome = std::from_chars(text.data(), text.data() + text.size(),
                                         value);
    if (outcome.ec == std::errc::result_out_of_range) {
        return invalid("integer literal is out of range");
    }
    if (outcome.ec == std::errc::invalid_argument ||
        outcome.ptr != text.data() + text.size()) {
        return invalid("not a complete integer literal");
    }
    return value;
}

} // namespace

result<std::int64_t> parse_i64(std::string_view text) {
    return parse_integer<std::int64_t>(text);
}

result<std::uint64_t> parse_u64(std::string_view text) {
    return parse_integer<std::uint64_t>(text);
}

result<std::int32_t> parse_int(std::string_view text) {
    return parse_integer<std::int32_t>(text);
}

result<std::uint32_t> parse_uint(std::string_view text) {
    return parse_integer<std::uint32_t>(text);
}

result<double> parse_double(std::string_view text) {
    if (text.empty()) {
        return invalid("expected a number, got an empty string");
    }
    if (!valid_decimal_syntax(text)) {
        return invalid("not a complete decimal literal");
    }
    double value = 0.0;
#if defined(__ANDROID__)
    // libc++ in the Android NDK deletes the floating-point `from_chars`
    // overload, so the portable path below cannot be used. `strtod_l` with an
    // explicit "C" locale is the replacement: it is locale-independent (the
    // process locale may use ',' as the decimal point, which would make a
    // plain `strtod` stop early) and correctly rounded, like `from_chars`.
    static const locale_t c_locale = ::newlocale(LC_ALL_MASK, "C", nullptr);
    if (c_locale == nullptr) {
        return invalid("no C locale available to parse a number");
    }
    std::string terminated(text);
    char* end = nullptr;
    errno = 0;
    const double parsed = ::strtod_l(terminated.c_str(), &end, c_locale);
    if (end != terminated.c_str() + terminated.size()) {
        return invalid("not a complete decimal literal");
    }
    if (errno == ERANGE) {
        return invalid("floating-point literal is out of range");
    }
    value = parsed;
#else
    const auto outcome = std::from_chars(text.data(), text.data() + text.size(),
                                         value);
    if (outcome.ec == std::errc::result_out_of_range) {
        return invalid("floating-point literal is out of range");
    }
    if (outcome.ec == std::errc::invalid_argument ||
        outcome.ptr != text.data() + text.size()) {
        return invalid("not a complete decimal literal");
    }
#endif
    return value;
}

std::string format(std::int64_t value) {
    char buffer[24];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return std::string(buffer, result.ptr);
}

std::string format(std::uint64_t value) {
    char buffer[24];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return std::string(buffer, result.ptr);
}

std::string format(double value) {
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return std::string(buffer, result.ptr);
}

} // namespace text
} // namespace crpc