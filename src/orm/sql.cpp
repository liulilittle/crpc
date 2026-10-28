// CRPC -- parameterised SQL generation (see sql.hpp for the contract).
#include <crpc/orm/sql.hpp>

#include <cstdint>
#include <limits>
#include <string>

namespace crpc {
namespace orm {
namespace sql {
namespace {

[[nodiscard]] bool dialect_supports_default_values(dialect d) noexcept {
    // MySQL and ANSI accept an empty value list; T-SQL spells it DEFAULT VALUES.
    return d == dialect::mysql || d == dialect::ansi;
}

[[nodiscard]] const char* comp_text(comp op) noexcept {
    switch (op) {
    case comp::eq:
        return "=";
    case comp::ne:
        return "<>";
    case comp::gt:
        return ">";
    case comp::ge:
        return ">=";
    case comp::lt:
        return "<";
    case comp::le:
        return "<=";
    case comp::in_:
        return "IN";
    case comp::is_null:
        return "IS NULL";
    }
    return "=";
}

[[nodiscard]] std::string column_list(const entity_meta& meta, dialect d,
                                      bool qualified) {
    std::string out;
    for (const column_meta& column : meta.columns) {
        if (!out.empty()) {
            out += ", ";
        }
        if (qualified) {
            out += quote_identifier(d, meta.table);
            out += ".";
        }
        out += quote_identifier(d, column.column);
    }
    return out;
}

[[nodiscard]] bool valid_column(const entity_meta& meta, std::size_t index) noexcept {
    return index < meta.columns.size();
}

// Appends one condition and its bound parameters. Returns false when the
// condition names an unknown column.
[[nodiscard]] bool append_condition(const entity_meta& meta, dialect d,
                                    const condition& cond, std::string& text,
                                    std::vector<parameter>& params) {
    if (!valid_column(meta, cond.column)) {
        return false;
    }
    const std::string quoted = quote_identifier(d, meta.columns[cond.column].column);
    text += quoted;
    if (cond.op == comp::is_null) {
        text += " IS NULL";
        return true;
    }
    text += " ";
    text += comp_text(cond.op);
    text += " ";
    if (cond.op == comp::in_) {
        text += "(";
        for (std::size_t i = 0; i < cond.params.size(); ++i) {
            if (i != 0) {
                text += ", ";
            }
            text += "?";
        }
        text += ")";
        params.insert(params.end(), cond.params.begin(), cond.params.end());
        return true;
    }
    text += "?";
    if (cond.params.size() != 1) {
        params.push_back(parameter::null());
    } else {
        params.push_back(cond.params[0]);
    }
    return true;
}

[[nodiscard]] status append_where(const entity_meta& meta, dialect d,
                                  span<const condition> conditions, std::string& text,
                                  std::vector<parameter>& params) {
    std::string where;
    for (const condition& cond : conditions) {
        if (!where.empty()) {
            where += " AND ";
        }
        if (!append_condition(meta, d, cond, where, params)) {
            return status{rpc_code::invalid_argument,
                          "a query condition names an unknown column"};
        }
    }
    if (meta.columns.empty()) {
        return status{rpc_code::invalid_argument, "entity has no columns"};
    }
    if (!where.empty()) {
        text += " WHERE ";
        text += where;
    }
    return status{};
}

} // namespace

bool dialect_supports_limit(dialect d) noexcept {
    return d == dialect::mysql || d == dialect::ansi;
}

std::string quote_identifier(dialect d, std::string_view identifier) {
    std::string out;
    if (d == dialect::mysql) {
        out.reserve(identifier.size() + 2);
        out.push_back('`');
        for (const char c : identifier) {
            out.push_back(c);
            if (c == '`') {
                out.push_back('`');
            }
        }
        out.push_back('`');
    } else if (d == dialect::tsql) {
        out.reserve(identifier.size() + 2);
        out.push_back('[');
        for (const char c : identifier) {
            if (c == ']') {
                out.push_back(']');
            }
            out.push_back(c);
        }
        out.push_back(']');
    } else {
        out.reserve(identifier.size() + 2);
        out.push_back('"');
        for (const char c : identifier) {
            if (c == '"') {
                out.push_back('"');
            }
            out.push_back(c);
        }
        out.push_back('"');
    }
    return out;
}

status build_select(const entity_meta& meta, dialect d, span<const condition> conditions,
                    span<const order_term> order, bool limited, std::size_t limit_value,
                    std::size_t offset_value, statement& out) {
    if (!meta.valid) {
        return status{rpc_code::invalid_argument, meta.error};
    }
    out.text = "SELECT ";
    out.text += column_list(meta, d, false);
    out.text += " FROM ";
    out.text += quote_identifier(d, meta.table);

    const status where = append_where(meta, d, conditions, out.text, out.params);
    if (!where) {
        return where;
    }

    if (order.size() > 0) {
        std::string order_text;
        for (const order_term& term : order) {
            if (!valid_column(meta, term.column)) {
                return status{rpc_code::invalid_argument,
                              "ORDER BY names an unknown column"};
            }
            if (!order_text.empty()) {
                order_text += ", ";
            }
            order_text += quote_identifier(d, meta.columns[term.column].column);
            order_text += term.descending ? " DESC" : " ASC";
        }
        out.text += " ORDER BY ";
        out.text += order_text;
    }

    if (limited) {
        if (!dialect_supports_limit(d)) {
            return status{rpc_code::failed_precondition,
                          "this dialect does not support LIMIT/OFFSET"};
        }
        // Both are bound parameters, so a caller-supplied limit cannot be
        // spliced into the text either.
        out.text += " LIMIT ?";
        out.params.push_back(parameter::from_int64(static_cast<std::int64_t>(limit_value)));
        out.text += " OFFSET ?";
        out.params.push_back(parameter::from_int64(static_cast<std::int64_t>(offset_value)));
    } else if (offset_value != 0) {
        return status{rpc_code::invalid_argument,
                      "offset() requires limit(); there is no unbounded OFFSET"};
    }
    return status{};
}

status build_select(const entity_meta& meta, dialect d,
                    const std::vector<condition>& conditions,
                    const std::vector<order_term>& order, bool limited,
                    std::size_t limit_value, std::size_t offset_value,
                    statement& out) {
    return build_select(meta, d,
                        span<const condition>(conditions.data(), conditions.size()),
                        span<const order_term>(order.data(), order.size()), limited,
                        limit_value, offset_value, out);
}

status build_count(const entity_meta& meta, dialect d,
                   span<const condition> conditions, statement& out) {
    if (!meta.valid) {
        return status{rpc_code::invalid_argument, meta.error};
    }
    out.text = "SELECT COUNT(*) FROM ";
    out.text += quote_identifier(d, meta.table);
    return append_where(meta, d, conditions, out.text, out.params);
}

status build_count(const entity_meta& meta, dialect d,
                   const std::vector<condition>& conditions, statement& out) {
    return build_count(meta, d,
                       span<const condition>(conditions.data(), conditions.size()), out);
}

status build_select_by_key(const entity_meta& meta, dialect d, std::size_t pk_column,
                           const parameter& key, statement& out) {
    if (!meta.valid) {
        return status{rpc_code::invalid_argument, meta.error};
    }
    if (!valid_column(meta, pk_column)) {
        return status{rpc_code::invalid_argument, "primary key column is unknown"};
    }
    out.text = "SELECT ";
    out.text += column_list(meta, d, false);
    out.text += " FROM ";
    out.text += quote_identifier(d, meta.table);
    out.text += " WHERE ";
    out.text += quote_identifier(d, meta.columns[pk_column].column);
    out.text += " = ?";
    out.params.push_back(key);
    return status{};
}

status build_insert(const entity_meta& meta, dialect d, const void* entity,
                    span<const std::size_t> columns, statement& out) {
    if (!meta.valid) {
        return status{rpc_code::invalid_argument, meta.error};
    }
    if (entity == nullptr) {
        return status{rpc_code::invalid_argument, "cannot insert a null entity"};
    }
    std::string names;
    for (const std::size_t index : columns) {
        if (!valid_column(meta, index)) {
            return status{rpc_code::invalid_argument, "insert column is unknown"};
        }
        if (!names.empty()) {
            names += ", ";
        }
        names += quote_identifier(d, meta.columns[index].column);
    }

    out.text = "INSERT INTO ";
    out.text += quote_identifier(d, meta.table);
    if (columns.size() == 0) {
        if (dialect_supports_default_values(d)) {
            out.text += " () VALUES ()";
        } else {
            out.text += " DEFAULT VALUES";
        }
        return status{};
    }
    out.text += " (";
    out.text += names;
    out.text += ") VALUES (";
    for (std::size_t i = 0; i < columns.size(); ++i) {
        if (i != 0) {
            out.text += ", ";
        }
        out.text += "?";
    }
    out.text += ")";

    out.params.reserve(columns.size());
    for (const std::size_t index : columns) {
        parameter param;
        const status encoded = meta.columns[index].encode(entity, param);
        if (!encoded) {
            return encoded;
        }
        out.params.push_back(std::move(param));
    }
    return status{};
}

status build_insert(const entity_meta& meta, dialect d, const void* entity,
                    const std::vector<std::size_t>& columns, statement& out) {
    return build_insert(meta, d, entity,
                        span<const std::size_t>(columns.data(), columns.size()), out);
}

status build_update(const entity_meta& meta, dialect d, const void* current,
                    const void* original, const std::vector<std::size_t>& dirty_columns,
                    statement& out) {
    return build_update(meta, d, current, original,
                        span<const std::size_t>(dirty_columns.data(),
                                                dirty_columns.size()),
                        out);
}

status build_update(const entity_meta& meta, dialect d, const void* current,
                    const void* original, span<const std::size_t> dirty_columns,
                    statement& out) {
    if (!meta.valid) {
        return status{rpc_code::invalid_argument, meta.error};
    }
    if (current == nullptr || original == nullptr) {
        return status{rpc_code::invalid_argument, "cannot update a null entity"};
    }
    if (meta.primary_key.empty()) {
        return status{rpc_code::failed_precondition,
                      "UPDATE requires an entity with a primary key"};
    }
    if (dirty_columns.size() == 0) {
        return status{rpc_code::failed_precondition,
                      "UPDATE requires at least one dirty column"};
    }
    for (const std::size_t index : dirty_columns) {
        if (!valid_column(meta, index)) {
            return status{rpc_code::invalid_argument, "update column is unknown"};
        }
    }

    out.text = "UPDATE ";
    out.text += quote_identifier(d, meta.table);
    out.text += " SET ";
    for (std::size_t i = 0; i < dirty_columns.size(); ++i) {
        if (i != 0) {
            out.text += ", ";
        }
        out.text += quote_identifier(d, meta.columns[dirty_columns[i]].column);
        out.text += " = ?";
    }

    out.text += " WHERE ";
    for (std::size_t i = 0; i < meta.primary_key.size(); ++i) {
        if (i != 0) {
            out.text += " AND ";
        }
        out.text += quote_identifier(d, meta.columns[meta.primary_key[i]].column);
        out.text += " = ?";
    }

    out.params.reserve(dirty_columns.size() + meta.primary_key.size());
    for (const std::size_t index : dirty_columns) {
        parameter param;
        const status encoded = meta.columns[index].encode(current, param);
        if (!encoded) {
            return encoded;
        }
        out.params.push_back(std::move(param));
    }
    // The row locator comes from the ORIGINAL snapshot, so a changed primary
    // key cannot redirect the UPDATE to a different row.
    for (const std::size_t index : meta.primary_key) {
        parameter param;
        const status encoded = meta.columns[index].encode(original, param);
        if (!encoded) {
            return encoded;
        }
        out.params.push_back(std::move(param));
    }
    return status{};
}

status build_delete(const entity_meta& meta, dialect d, const void* entity,
                    statement& out) {
    if (!meta.valid) {
        return status{rpc_code::invalid_argument, meta.error};
    }
    if (entity == nullptr) {
        return status{rpc_code::invalid_argument, "cannot delete a null entity"};
    }
    if (meta.primary_key.empty()) {
        return status{rpc_code::failed_precondition,
                      "DELETE requires an entity with a primary key"};
    }
    out.text = "DELETE FROM ";
    out.text += quote_identifier(d, meta.table);
    out.text += " WHERE ";
    for (std::size_t i = 0; i < meta.primary_key.size(); ++i) {
        if (i != 0) {
            out.text += " AND ";
        }
        const std::size_t index = meta.primary_key[i];
        out.text += quote_identifier(d, meta.columns[index].column);
        out.text += " = ?";
        parameter param;
        const status encoded = meta.columns[index].encode(entity, param);
        if (!encoded) {
            return encoded;
        }
        out.params.push_back(std::move(param));
    }
    return status{};
}

status build_last_insert_id(dialect d, statement& out) {
    if (d == dialect::tsql) {
        out.text = "SELECT SCOPE_IDENTITY()";
        return status{};
    }
    out.text = "SELECT LAST_INSERT_ID()";
    return status{};
}

namespace {

[[nodiscard]] type_family family_of(db_type type) noexcept {
    switch (type) {
    case db_type::boolean:
        return type_family::boolean;
    case db_type::int64:
    case db_type::uint64:
        return type_family::integral;
    case db_type::real:
        return type_family::real;
    case db_type::text:
    case db_type::decimal:
    case db_type::date:
    case db_type::time:
    case db_type::timestamp:
    case db_type::uuid:
    case db_type::json:
    case db_type::xml:
        return type_family::textual;
    case db_type::blob:
        return type_family::blob;
    default:
        return type_family::other;
    }
}

} // namespace

status check_key_compatible(const parameter& key, db_type column_type) {
    const type_family key_family = family_of(key.type);
    const type_family column_family = family_of(column_type);
    if (column_family == type_family::other) {
        // The metadata does not know the column's value kind; let the server
        // enforce the match rather than guess.
        return status{};
    }
    if (key_family != column_family) {
        return status{rpc_code::invalid_argument,
                      "key type does not match the primary key column type"};
    }
    if (key_family == type_family::integral) {
        if (key.type == db_type::int64 && column_type == db_type::uint64 && key.int_value < 0) {
            return status{rpc_code::invalid_argument,
                          "negative key cannot address an unsigned primary key column"};
        }
        if (key.type == db_type::uint64 && column_type == db_type::int64 &&
            key.uint_value >
                static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
            return status{rpc_code::invalid_argument,
                          "key exceeds the signed range of the primary key column"};
        }
    }
    return status{};
}

} // namespace sql
} // namespace orm
} // namespace crpc