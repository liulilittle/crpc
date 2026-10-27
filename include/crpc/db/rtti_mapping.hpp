// CRPC -- reading a result set into reflected structs.
//
// The mapping rules live in `mapping.hpp`, which has no reflection dependency; this
// header only builds the bindings from `rtti::fields_of<T>()`. Keeping the split
// means the rules stay testable without RTTI, and this file stays small enough to
// be obviously correct.
//
// Each field is stored through rtti's `set()`, which does the type conversion, so
// this layer does not reimplement numeric/string conversion. A conversion the field
// cannot accept is an error, not a silently defaulted value.
#ifndef CRPC_DB_RTTI_MAPPING_HPP
#define CRPC_DB_RTTI_MAPPING_HPP

#include <crpc/config.hpp>

#if defined(CRPC_HAVE_RTTI)

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <rtti/reflection.hpp>

#include <crpc/db/connection.hpp>
#include <crpc/db/mapping.hpp>
#include <crpc/db/types.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace db {

namespace detail {

// Converts one cursor cell into a `field_value` using the column's reported type.
// The column type decides the carrier; the field's own type is applied by `set()`.
[[nodiscard]] inline status cell_to_value(result_set& set, std::size_t index,
                                          rtti::field_value& out, std::string& owned) {
    if (set.is_null(index)) {
        // rtti has no null carrier. A null is reported as an absent value, and a
        // field that cannot represent absence fails rather than keeping a stale
        // value from a previous row.
        out = rtti::field_value{};
        return status{};
    }
    const db_type type = set.column(index).type;
    switch (type) {
    case db_type::boolean: {
        auto value = set.get_bool(index);
        if (!value) {
            return value.error();
        }
        out = rtti::field_value::of(value.value());
        return status{};
    }
    case db_type::int64:
    case db_type::timestamp: {
        auto value = set.get_int64(index);
        if (!value) {
            return value.error();
        }
        out = rtti::field_value::of(value.value());
        return status{};
    }
    case db_type::uint64: {
        auto value = set.get_uint64(index);
        if (!value) {
            return value.error();
        }
        // A value above INT64_MAX cannot be carried as a signed integer; carrying it
        // as text would let the field's conversion decide (and fail visibly if the
        // target is signed) instead of wrapping silently here.
        if (value.value() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            owned = std::to_string(value.value());
            out = rtti::field_value::of(owned);
            return status{};
        }
        out = rtti::field_value::of(static_cast<std::int64_t>(value.value()));
        return status{};
    }
    case db_type::real: {
        auto value = set.get_double(index);
        if (!value) {
            return value.error();
        }
        out = rtti::field_value::of(value.value());
        return status{};
    }
    case db_type::blob: {
        auto value = set.get_blob(index);
        if (!value) {
            return value.error();
        }
        owned.assign(reinterpret_cast<const char*>(value.value().data()),
                     value.value().size());
        out = rtti::field_value::of(owned);
        return status{};
    }
    default: {
        auto value = set.get_text(index);
        if (!value) {
            return value.error();
        }
        owned.assign(value.value().data(), value.value().size());
        out = rtti::field_value::of(owned);
        return status{};
    }
    }
}

} // namespace detail

// Bindings for every reflected field of T, writing into `out`. The returned
// bindings borrow `out`, so they must not outlive it.
template <typename T>
[[nodiscard]] std::vector<field_binding> bindings_for(T& out) {
    std::vector<field_binding> bindings;
    const std::size_t count = rtti::field_count<T>();
    const rtti::field_access_base* const* accessors = rtti::fields<T>();
    bindings.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const rtti::field_access_base* accessor = accessors[i];
        field_binding binding;
        binding.column = accessor->name;
        binding.assign = [&out, accessor](result_set& set, std::size_t index) -> status {
            rtti::field_value value;
            // Holds the text/blob for the duration of set(); field_value stores a
            // view, not a copy.
            std::string owned;
            const status converted = detail::cell_to_value(set, index, value, owned);
            if (!converted) {
                return converted;
            }
            if (!accessor->set(&out, value)) {
                // The column does not fit the field: a decode failure, never a
                // default value, because a default is indistinguishable from data.
                return status{rpc_code::decode_error,
                              std::string("column does not fit field '") + accessor->name + "'"};
            }
            return status{};
        };
        bindings.push_back(std::move(binding));
    }
    return bindings;
}

namespace detail {

template <typename T>
[[nodiscard]] inline result<std::vector<std::size_t>> prepare_bindings(
    result_set& set, std::vector<field_binding>& bindings, T& out) {
    std::vector<column_info> columns;
    columns.reserve(set.column_count());
    for (std::size_t i = 0; i < set.column_count(); ++i) {
        columns.push_back(set.column(i));
    }
    bindings = bindings_for(out);
    return resolve_bindings(span<const column_info>(columns.data(), columns.size()),
                            span<const field_binding>(bindings.data(), bindings.size()));
}

} // namespace detail

// Reads one row (the caller has already advanced the cursor).
template <typename T>
[[nodiscard]] status read_current(result_set& set, T& out) {
    std::vector<field_binding> bindings;
    auto indices = detail::prepare_bindings(set, bindings, out);
    if (!indices) {
        return indices.error();
    }
    return bind_current_row(set, span<const field_binding>(bindings.data(), bindings.size()),
                            span<const std::size_t>(indices.value().data(),
                                                    indices.value().size()));
}

// Reads every row into a vector of T. The resolution runs once; only the object
// pointer is rebound per row, so a large result does not pay for a per-row mapping
// search.
template <typename T>
[[nodiscard]] result<std::vector<T>> read_all(result_set& set) {
    std::vector<column_info> columns;
    columns.reserve(set.column_count());
    for (std::size_t i = 0; i < set.column_count(); ++i) {
        columns.push_back(set.column(i));
    }

    T probe{};
    std::vector<field_binding> bindings = bindings_for(probe);
    auto indices = resolve_bindings(span<const column_info>(columns.data(), columns.size()),
                                    span<const field_binding>(bindings.data(), bindings.size()));
    if (!indices) {
        return indices.error();
    }

    std::vector<T> rows;
    for (;;) {
        const bool has_row = set.next();
        if (!has_row) {
            break;
        }
        T object{};
        // Rebind each setter to this row's object; the column resolution is reused.
        bindings = bindings_for(object);
        const status bound =
            bind_current_row(set, span<const field_binding>(bindings.data(), bindings.size()),
                             span<const std::size_t>(indices.value().data(),
                                                     indices.value().size()));
        if (!bound) {
            return bound.error();
        }
        rows.push_back(std::move(object));
    }
    return rows;
}

} // namespace db
} // namespace crpc

#endif // CRPC_HAVE_RTTI

#endif // CRPC_DB_RTTI_MAPPING_HPP
