// CRPC -- parameterised SQL generation.
//
// Every generated statement keeps every value in a `parameter` and renders a
// `?` placeholder, so a value can never be spliced into the text. Identifier
// quoting is the only part of a statement that is assembled textually, and it
// is validated through the dialect abstraction (`mysql` backticks, `tsql`
// brackets, `ansi` double quotes).
#ifndef CRPC_ORM_SQL_HPP
#define CRPC_ORM_SQL_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/db/types.hpp>
#include <crpc/error.hpp>
#include <crpc/orm/fwd.hpp>
#include <crpc/orm/metadata.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace orm {
namespace sql {

struct statement {
    std::string text;
    std::vector<parameter> params;
};

enum class comp : std::uint8_t {
    eq,
    ne,
    gt,
    ge,
    lt,
    le,
    in_,
    is_null
};

struct condition {
    std::size_t column = orm_npos;
    comp op = comp::eq;
    std::vector<parameter> params;
};

struct order_term {
    std::size_t column = orm_npos;
    bool descending = false;
};

// Quotes one identifier for the dialect. Escapes the dialect's own quote
// character so a hostile annotation cannot break out of the identifier.
CRPC_API std::string quote_identifier(dialect d, std::string_view identifier);

// SELECT cols, ... FROM table WHERE c1 op ? AND ... [ORDER BY ...]
// [LIMIT ? OFFSET ?].
CRPC_API status build_select(const entity_meta& meta, dialect d,
                             span<const condition> conditions,
                             span<const order_term> order, bool limited,
                             std::size_t limit_value, std::size_t offset_value,
                             statement& out);

// Owning-container convenience overloads. The session and query builder always
// accumulate their conditions in vectors, and a vector does not convert to a
// span implicitly.
CRPC_API status build_select(const entity_meta& meta, dialect d,
                             const std::vector<condition>& conditions,
                             const std::vector<order_term>& order, bool limited,
                             std::size_t limit_value, std::size_t offset_value,
                             statement& out);

// SELECT COUNT(*) FROM table WHERE ...
CRPC_API status build_count(const entity_meta& meta, dialect d,
                            span<const condition> conditions, statement& out);
CRPC_API status build_count(const entity_meta& meta, dialect d,
                            const std::vector<condition>& conditions, statement& out);

// SELECT cols FROM table WHERE <single pk column> = ?
CRPC_API status build_select_by_key(const entity_meta& meta, dialect d,
                                    std::size_t pk_column, const parameter& key,
                                    statement& out);

// INSERT INTO table (cols) VALUES (?, ?, ...) for the given columns.
CRPC_API status build_insert(const entity_meta& meta, dialect d,
                             const void* entity, span<const std::size_t> columns,
                             statement& out);
CRPC_API status build_insert(const entity_meta& meta, dialect d, const void* entity,
                             const std::vector<std::size_t>& columns, statement& out);

// UPDATE table SET dirty = ?, ... WHERE <pk> = ? ... using `current` for values
// and `original` for the row locator (a changed primary key must not move the
// update to a new row).
CRPC_API status build_update(const entity_meta& meta, dialect d,
                             const void* current, const void* original,
                             span<const std::size_t> dirty_columns, statement& out);
CRPC_API status build_update(const entity_meta& meta, dialect d, const void* current,
                             const void* original,
                             const std::vector<std::size_t>& dirty_columns,
                             statement& out);

// DELETE FROM table WHERE <pk> = ? ...; the row locator comes from `entity`.
CRPC_API status build_delete(const entity_meta& meta, dialect d, const void* entity,
                             statement& out);

// SELECT that reads back the server-assigned identifier of the last INSERT.
CRPC_API status build_last_insert_id(dialect d, statement& out);

// True when the dialect can render LIMIT/OFFSET as bound parameters.
CRPC_API bool dialect_supports_limit(dialect d) noexcept;

// Compares a caller-supplied key's encoded type with a column's type family;
// integer ranges are honoured so `find<account>(5)` cannot silently match a
// bigint column as-if it were unsigned.
CRPC_API status check_key_compatible(const parameter& key, db_type column_type);

} // namespace sql
} // namespace orm
} // namespace crpc

#endif // CRPC_ORM_SQL_HPP