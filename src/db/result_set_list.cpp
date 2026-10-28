// CRPC -- materialised multi-result-set wrapper.
#include <crpc/db/result_set_list.hpp>

#include <utility>

namespace crpc {
namespace db {

void result_set_list::add(std::unique_ptr<result_set> set) {
    if (!set) {
        return;
    }
    closed_ = false;
    sets_.push_back(std::move(set));
}

result_set* result_set_list::current() const noexcept {
    if (sets_.empty() || closed_) {
        return nullptr;
    }
    if (cursor_ >= sets_.size()) {
        return nullptr;
    }
    return sets_[cursor_].get();
}

std::size_t result_set_list::column_count() const noexcept {
    const result_set* set = current();
    return set != nullptr ? set->column_count() : 0;
}

column_info result_set_list::column(std::size_t index) const {
    const result_set* set = current();
    return set != nullptr ? set->column(index) : column_info{};
}

bool result_set_list::next() {
    result_set* set = current();
    return set != nullptr ? set->next() : false;
}

bool result_set_list::next_result() {
    if (closed_ || sets_.empty()) {
        return false;
    }
    // Stepping to a later set is unconditional: a caller that wants the next set
    // does not have to first exhaust the current one with `next()`.
    if (cursor_ + 1 >= sets_.size()) {
        return false;
    }
    ++cursor_;
    return true;
}

bool result_set_list::is_null(std::size_t index) const noexcept {
    const result_set* set = current();
    return set != nullptr ? set->is_null(index) : true;
}

result<std::int64_t> result_set_list::get_int64(std::size_t index) {
    result_set* set = current();
    return set != nullptr ? set->get_int64(index)
                          : rpc_error{rpc_code::invalid_argument, "the cursor has no row"};
}

result<std::uint64_t> result_set_list::get_uint64(std::size_t index) {
    result_set* set = current();
    return set != nullptr ? set->get_uint64(index)
                          : rpc_error{rpc_code::invalid_argument, "the cursor has no row"};
}

result<double> result_set_list::get_double(std::size_t index) {
    result_set* set = current();
    return set != nullptr ? set->get_double(index)
                          : rpc_error{rpc_code::invalid_argument, "the cursor has no row"};
}

result<bool> result_set_list::get_bool(std::size_t index) {
    result_set* set = current();
    return set != nullptr ? set->get_bool(index)
                          : rpc_error{rpc_code::invalid_argument, "the cursor has no row"};
}

result<std::string_view> result_set_list::get_text(std::size_t index) {
    result_set* set = current();
    return set != nullptr ? set->get_text(index)
                          : rpc_error{rpc_code::invalid_argument, "the cursor has no row"};
}

result<std::vector<std::byte>> result_set_list::get_blob(std::size_t index) {
    result_set* set = current();
    return set != nullptr ? set->get_blob(index)
                          : rpc_error{rpc_code::invalid_argument, "the cursor has no row"};
}

std::int64_t result_set_list::affected_rows() const noexcept {
    const result_set* set = current();
    return set != nullptr ? set->affected_rows() : -1;
}

std::size_t result_set_list::rows_decoded() const noexcept {
    const result_set* set = current();
    return set != nullptr ? set->rows_decoded() : 0;
}

std::size_t result_set_list::rows_buffered() const noexcept {
    const result_set* set = current();
    return set != nullptr ? set->rows_buffered() : 0;
}

void result_set_list::close() noexcept {
    if (closed_) {
        return;
    }
    for (std::unique_ptr<result_set>& set : sets_) {
        if (set != nullptr) {
            set->close();
        }
    }
    sets_.clear();
    cursor_ = 0;
    closed_ = true;
}

} // namespace db
} // namespace crpc