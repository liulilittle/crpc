// CRPC -- materialised result set implementation.
#include <crpc/db/memory_result_set.hpp>

#include <cstdlib>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

#include <crpc/error.hpp>

namespace crpc {
namespace db {
namespace {

// Parses a decimal integer the server sent as text. The text form of an integer
// column is exactly the value, so no rounding is involved; a malformed one is a
// protocol error rather than a zero.
[[nodiscard]] result<std::int64_t> parse_int(std::string_view text) {
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

} // namespace

memory_result_set::memory_result_set(std::vector<column_info> columns,
                                     std::int64_t affected) noexcept
    : columns_(std::move(columns)), affected_(affected) {}

void memory_result_set::add_row(std::vector<std::string> values, std::vector<bool> nulls) {
    values_.push_back(std::move(values));
    nulls_.push_back(std::move(nulls));
}

std::size_t memory_result_set::column_count() const noexcept { return columns_.size(); }

column_info memory_result_set::column(std::size_t index) const {
    if (index >= columns_.size()) {
        return {};
    }
    return columns_[index];
}

bool memory_result_set::next() {
    if (next_ >= values_.size()) {
        has_row_ = false;
        return false;
    }
    cursor_ = next_++;
    has_row_ = true;
    return true;
}

bool memory_result_set::is_null(std::size_t index) const noexcept {
    if (!has_row_ || cursor_ >= nulls_.size() || index >= nulls_[cursor_].size()) {
        return true;
    }
    return nulls_[cursor_][index];
}

result<std::int64_t> memory_result_set::get_int64(std::size_t index) {
    const std::string_view text = value_at(index);
    if (text.data() == nullptr) {
        return rpc_error{rpc_code::invalid_argument, "column is NULL"};
    }
    return parse_int(text);
}

result<std::uint64_t> memory_result_set::get_uint64(std::size_t index) {
    const std::string_view text = value_at(index);
    if (text.data() == nullptr) {
        return rpc_error{rpc_code::invalid_argument, "column is NULL"};
    }
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

result<double> memory_result_set::get_double(std::size_t index) {
    const std::string_view text = value_at(index);
    if (text.data() == nullptr) {
        return rpc_error{rpc_code::invalid_argument, "column is NULL"};
    }
    // `strtod` needs a NUL-terminated buffer; the stored value is a std::string,
    // so a copy of at most one value is made here rather than parsing by hand
    // and getting the exponent/NaN rules wrong.
    const std::string copy(text);
    char* end = nullptr;
    const double value = std::strtod(copy.c_str(), &end);
    if (end == copy.c_str() || *end != '\0') {
        return rpc_error{rpc_code::decode_error, "value is not a number"};
    }
    return value;
}

result<bool> memory_result_set::get_bool(std::size_t index) {
    const std::string_view text = value_at(index);
    if (text.data() == nullptr) {
        return rpc_error{rpc_code::invalid_argument, "column is NULL"};
    }
    // The adapters all spell booleans 0/1; anything else is a decode error rather
    // than C-style truthiness, which would accept "false" as true.
    if (text == "0") {
        return false;
    }
    if (text == "1") {
        return true;
    }
    return rpc_error{rpc_code::decode_error, "value is not a boolean"};
}

result<std::string_view> memory_result_set::get_text(std::size_t index) {
    const std::string_view text = value_at(index);
    if (text.data() == nullptr) {
        return rpc_error{rpc_code::invalid_argument, "column is NULL"};
    }
    return text;
}

result<std::vector<std::byte>> memory_result_set::get_blob(std::size_t index) {
    const std::string_view text = value_at(index);
    if (text.data() == nullptr) {
        return rpc_error{rpc_code::invalid_argument, "column is NULL"};
    }
    std::vector<std::byte> out(text.size());
    if (!text.empty()) {
        std::memcpy(out.data(), text.data(), text.size());
    }
    return out;
}

std::int64_t memory_result_set::affected_rows() const noexcept { return affected_; }

void memory_result_set::set_affected_rows(std::int64_t count) noexcept { affected_ = count; }

void memory_result_set::close() noexcept { closed_ = true; }

std::string_view memory_result_set::value_at(std::size_t index) const {
    if (!has_row_ || cursor_ >= values_.size() || index >= values_[cursor_].size()) {
        return {};
    }
    if (index < nulls_[cursor_].size() && nulls_[cursor_][index]) {
        return {};
    }
    return values_[cursor_][index];
}

} // namespace db
} // namespace crpc