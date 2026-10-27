// CRPC -- mapping result columns to application fields.
//
// This is the seam an ORM is built on, and it is deliberately free of reflection:
// it takes a list of "field name + setter" and resolves each name against the
// column metadata. An RTTI adapter can build that list from `fields_of<T>()`; the
// resolution rules themselves are what needs testing, and they do not.
//
// The rules exist because every naive implementation gets them wrong:
//
//   * match `original_name` first, `name` second. A query may alias a column
//     (`SELECT id AS user_id`), and binding on the alias only works until the query
//     changes. The original name is the stable identity.
//   * matching is case-insensitive, because a database folds unquoted identifiers
//     and a struct field rarely mirrors that folding.
//   * two result columns matching one field is an ambiguity, not a first-match win:
//     silently taking the leftmost binds the wrong data whenever the query grows a
//     column.
//   * an unresolved field is either an error or a skip, and that choice belongs to
//     the caller: a partial SELECT is normal, a typo in a struct field is not.
#ifndef CRPC_DB_MAPPING_HPP
#define CRPC_DB_MAPPING_HPP

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/db/connection.hpp>
#include <crpc/db/types.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace db {

// One target field. `assign` receives the cursor and the resolved column index, so
// the setter decides how to read the value (text, integer, null) and what to do
// when the value does not fit the field type.
struct field_binding {
    std::string column;
    std::function<status(result_set&, std::size_t)> assign;
};

struct mapping_options {
    // When true, a field that matches no column fails the mapping. When false it is
    // left untouched, which is what a partial SELECT needs.
    bool require_every_field = false;

    // When true, a result column that no field claims fails the mapping. Useful for
    // a strict read model, wrong for a query that selects extra diagnostics.
    bool reject_unbound_columns = false;
};

// Resolves each binding to a column index. The returned vector has one entry per
// binding; `unresolved` marks a field that matched nothing and was allowed to be
// left alone.
inline constexpr std::size_t unresolved = static_cast<std::size_t>(-1);

[[nodiscard]] CRPC_API result<std::vector<std::size_t>> resolve_bindings(
    span<const column_info> columns, span<const field_binding> bindings,
    mapping_options options = {});

// Applies the setters to the cursor's current row. Unresolved bindings are skipped;
// a setter failure aborts the row and is returned unchanged (a decode error must not
// be turned into a partial object).
[[nodiscard]] CRPC_API status bind_current_row(result_set& set,
                                               span<const field_binding> bindings,
                                               span<const std::size_t> indices);

// Reads every row, applying the bindings to a fresh `std::vector<row_type>`.
// `on_row` is called once per row after the bindings ran, so a caller that needs
// the whole set can append it.
[[nodiscard]] CRPC_API status for_each_row(result_set& set,
                                           span<const field_binding> bindings,
                                           span<const std::size_t> indices,
                                           const std::function<void()>& on_row);

} // namespace db
} // namespace crpc

#endif // CRPC_DB_MAPPING_HPP