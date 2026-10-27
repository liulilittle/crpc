// CRPC -- column-to-field mapping.
#include <crpc/db/mapping.hpp>

#include <utility>

namespace crpc {
namespace db {
namespace {

[[nodiscard]] char fold(char c) noexcept {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

[[nodiscard]] bool same_name(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (fold(lhs[i]) != fold(rhs[i])) {
            return false;
        }
    }
    return true;
}

// Collects every column whose name (or original name) equals `wanted`.
void collect_matches(span<const column_info> columns, std::string_view wanted,
                     std::size_t& by_original, std::size_t& by_name,
                     std::size_t& original_count, std::size_t& name_count) {
    by_original = unresolved;
    by_name = unresolved;
    original_count = 0;
    name_count = 0;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (!columns[i].original_name.empty() && same_name(columns[i].original_name, wanted)) {
            ++original_count;
            if (by_original == unresolved) {
                by_original = i;
            }
        }
        if (same_name(columns[i].name, wanted)) {
            ++name_count;
            if (by_name == unresolved) {
                by_name = i;
            }
        }
    }
}

} // namespace

result<std::vector<std::size_t>> resolve_bindings(span<const column_info> columns,
                                                  span<const field_binding> bindings,
                                                  mapping_options options) {
    std::vector<std::size_t> indices;
    indices.reserve(bindings.size());

    std::vector<std::size_t> claimed(columns.size(), 0);

    for (const field_binding& binding : bindings) {
        if (binding.column.empty()) {
            return rpc_error{rpc_code::invalid_argument, "a field binding needs a column name"};
        }
        std::size_t by_original = unresolved;
        std::size_t by_name = unresolved;
        std::size_t original_count = 0;
        std::size_t name_count = 0;
        collect_matches(columns, binding.column, by_original, by_name, original_count,
                        name_count);

        // Two columns with the same original name is an ambiguity: taking one would
        // bind whatever the query happened to put first.
        if (original_count > 1) {
            return rpc_error{rpc_code::failed_precondition,
                             "more than one result column matches field '" + binding.column + "'"};
        }
        if (original_count == 1) {
            indices.push_back(by_original);
            ++claimed[by_original];
            continue;
        }
        if (name_count > 1) {
            return rpc_error{rpc_code::failed_precondition,
                             "more than one result column matches field '" + binding.column + "'"};
        }
        if (name_count == 1) {
            indices.push_back(by_name);
            ++claimed[by_name];
            continue;
        }
        if (options.require_every_field) {
            return rpc_error{rpc_code::failed_precondition,
                             "no result column matches field '" + binding.column + "'"};
        }
        indices.push_back(unresolved);
    }

    if (options.reject_unbound_columns) {
        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (claimed[i] == 0) {
                return rpc_error{rpc_code::failed_precondition,
                                 "result column '" + columns[i].name + "' is not bound to a field"};
            }
        }
    }
    return indices;
}

status bind_current_row(result_set& set, span<const field_binding> bindings,
                        span<const std::size_t> indices) {
    if (bindings.size() != indices.size()) {
        return status{rpc_code::invalid_argument,
                      "bindings and resolved indices have different sizes"};
    }
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        if (indices[i] == unresolved) {
            continue;
        }
        // A setter failure aborts the row: a partially filled object is worse than
        // no object, because the caller cannot tell which fields are real.
        const status assigned = bindings[i].assign(set, indices[i]);
        if (!assigned) {
            return assigned;
        }
    }
    return status{};
}

status for_each_row(result_set& set, span<const field_binding> bindings,
                    span<const std::size_t> indices,
                    const std::function<void()>& on_row) {
    for (;;) {
        const bool has_row = set.next();
        if (!has_row) {
            return status{};
        }
        const status bound = bind_current_row(set, bindings, indices);
        if (!bound) {
            return bound;
        }
        if (on_row) {
            on_row();
        }
    }
}

} // namespace db
} // namespace crpc