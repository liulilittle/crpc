// CRPC -- materialised result set implementation.
#include <crpc/db/memory_result_set.hpp>

#include <cstring>
#include <utility>

#include <crpc/error.hpp>

#include "text_value.hpp"

namespace crpc {
namespace db {

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
    return detail::parse_int64_text(text);
}

result<std::uint64_t> memory_result_set::get_uint64(std::size_t index) {
    const std::string_view text = value_at(index);
    if (text.data() == nullptr) {
        return rpc_error{rpc_code::invalid_argument, "column is NULL"};
    }
    return detail::parse_uint64_text(text);
}

result<double> memory_result_set::get_double(std::size_t index) {
    const std::string_view text = value_at(index);
    if (text.data() == nullptr) {
        return rpc_error{rpc_code::invalid_argument, "column is NULL"};
    }
    return detail::parse_double_text(text);
}

result<bool> memory_result_set::get_bool(std::size_t index) {
    const std::string_view text = value_at(index);
    if (text.data() == nullptr) {
        return rpc_error{rpc_code::invalid_argument, "column is NULL"};
    }
    return detail::parse_bool_text(text);
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
    return detail::blob_from_text(text);
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