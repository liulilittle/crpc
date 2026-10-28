// CRPC -- parsing of canonical text values, shared by the result-set cursors.
//
// Every synchronous adapter delivers cell values the same way: as canonical text
// that the typed accessors parse on demand. `memory_result_set` and the streaming
// cursor both need the same parsers; a second copy would be duplicate knowledge --
// and the MySQL version already had to fix the "parse 5 as a signed 64-bit value"
// overflow once.
#ifndef CRPC_SRC_DB_TEXT_VALUE_HPP
#define CRPC_SRC_DB_TEXT_VALUE_HPP

#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

#include <crpc/error.hpp>

namespace crpc {
namespace db {
namespace detail {

// Parses a decimal integer the server sent as text. The text form of an integer
// column is exactly the value, so no rounding is involved; a malformed one is a
// protocol error rather than a zero.
[[nodiscard]] inline result<std::int64_t> parse_int64_text(std::string_view text) {
    if (text.empty()) {
        return rpc_error{rpc_code::decode_error, "empty integer text"};
    }
    std::size_t index = 0;
    bool negative = false;
    if (text[0] == '-') {
        negative = true;
        index = 1;
    } else if (text[0] == '+') {
        index = 1;
    }
    if (index >= text.size()) {
        return rpc_error{rpc_code::decode_error, "integer text has no digits"};
    }
    std::uint64_t value = 0;
    for (; index < text.size(); ++index) {
        const char c = text[index];
        if (c < '0' || c > '9') {
            return rpc_error{rpc_code::decode_error, "integer text has a non-digit"};
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return rpc_error{rpc_code::decode_error, "integer text overflows"};
        }
        value = value * 10 + digit;
    }
    if (negative) {
        const std::uint64_t limit =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1u;
        if (value > limit) {
            return rpc_error{rpc_code::decode_error, "negative integer text overflows"};
        }
        if (value == limit) {
            return std::numeric_limits<std::int64_t>::min();
        }
        return -static_cast<std::int64_t>(value);
    }
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        // A BIGINT UNSIGNED above INT64_MAX is legal, so it is reported through
        // `get_uint64`; asking for a signed value is the caller's mistake.
        return rpc_error{rpc_code::invalid_argument, "value exceeds int64; read it as uint64"};
    }
    return static_cast<std::int64_t>(value);
}

// Parses an unsigned decimal integer; a value above INT64_MAX is legal here.
[[nodiscard]] inline result<std::uint64_t> parse_uint64_text(std::string_view text) {
    if (text.empty()) {
        return rpc_error{rpc_code::decode_error, "empty integer text"};
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return rpc_error{rpc_code::decode_error, "integer text has a non-digit"};
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return rpc_error{rpc_code::decode_error, "integer text overflows"};
        }
        value = value * 10 + digit;
    }
    return value;
}

// Parses a floating-point value. `strtod` needs a NUL-terminated buffer, so the
// caller's view (which may be a slice of a message) is copied once.
[[nodiscard]] inline result<double> parse_double_text(std::string_view text) {
    const std::string copy(text);
    char* end = nullptr;
    const double value = std::strtod(copy.c_str(), &end);
    if (end == copy.c_str() || *end != '\0') {
        return rpc_error{rpc_code::decode_error, "value is not a number"};
    }
    return value;
}

// Boolean columns arrive as 0/1; anything else is a decode error rather than
// C-style truthiness, which would accept "false" as true.
[[nodiscard]] inline result<bool> parse_bool_text(std::string_view text) {
    if (text == "0") {
        return false;
    }
    if (text == "1") {
        return true;
    }
    return rpc_error{rpc_code::decode_error, "value is not a boolean"};
}

// The blob form of a text value: a raw byte copy.
[[nodiscard]] inline std::vector<std::byte> blob_from_text(std::string_view text) {
    std::vector<std::byte> out(text.size());
    if (!text.empty()) {
        std::memcpy(out.data(), text.data(), text.size());
    }
    return out;
}

} // namespace detail
} // namespace db
} // namespace crpc

#endif // CRPC_SRC_DB_TEXT_VALUE_HPP