// CRPC -- bulk insert planning.
#include <crpc/db/bulk.hpp>

#include <functional>

namespace crpc {
namespace db {
namespace {

[[nodiscard]] bool is_bare_char(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '$';
}

// One dot-separated part: a bare name or a backtick-quoted one.
[[nodiscard]] status validate_segment(std::string_view segment) {
    if (segment.empty()) {
        return status{rpc_code::invalid_argument, "an identifier segment cannot be empty"};
    }
    if (segment.front() == '`') {
        if (segment.size() < 3 || segment.back() != '`') {
            return status{rpc_code::invalid_argument, "an unterminated quoted identifier"};
        }
        const std::string_view body = segment.substr(1, segment.size() - 2);
        for (std::size_t i = 0; i < body.size(); ++i) {
            if (body[i] != '`') {
                continue;
            }
            // Inside a quoted segment a backtick must be doubled; a lone one ends
            // the identifier early and lets the rest of the text become SQL.
            if (i + 1 < body.size() && body[i + 1] == '`') {
                ++i;
                continue;
            }
            return status{rpc_code::invalid_argument, "an embedded backtick must be doubled"};
        }
        return status{};
    }
    for (const char c : segment) {
        if (!is_bare_char(c)) {
            return status{rpc_code::invalid_argument,
                          "an identifier contains a character that is not allowed bare"};
        }
    }
    return status{};
}

// Splits on '.' outside a quoted segment.
[[nodiscard]] status for_each_segment(std::string_view name,
                                     const std::function<void(std::string_view)>& visit) {
    if (name.empty()) {
        return status{rpc_code::invalid_argument, "an identifier cannot be empty"};
    }
    std::size_t start = 0;
    bool quoted = false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (quoted) {
            if (c != '`') {
                continue;
            }
            if (i + 1 < name.size() && name[i + 1] == '`') {
                ++i;
                continue;
            }
            quoted = false;
            continue;
        }
        if (c == '`') {
            quoted = true;
            continue;
        }
        if (c == '.') {
            const status segment = validate_segment(name.substr(start, i - start));
            if (!segment) {
                return segment;
            }
            visit(name.substr(start, i - start));
            start = i + 1;
        }
    }
    if (quoted) {
        return status{rpc_code::invalid_argument, "an unterminated quoted identifier"};
    }
    const std::string_view last = name.substr(start);
    const status segment = validate_segment(last);
    if (!segment) {
        return segment;
    }
    visit(last);
    return status{};
}

} // namespace

status validate_identifier(std::string_view name) {
    return for_each_segment(name, [](std::string_view) {});
}

std::string quote_identifier(std::string_view name) {
    // Each dot-separated segment is quoted on its own: a qualified name must stay
    // qualified, and wrapping the whole thing would make the dots part of one
    // identifier.
    std::string out;
    out.reserve(name.size() + 8);
    bool quoted = false;
    bool first = true;
    if (!name.empty() && name.front() != '`') {
        // Fall through: build segment by segment below.
    }
    std::size_t start = 0;
    const auto append_quoted = [&out](std::string_view segment) {
        out.push_back('`');
        for (const char c : segment) {
            if (c == '`') {
                out.push_back('`');
            }
            out.push_back(c);
        }
        out.push_back('`');
    };
    for (std::size_t i = 0; i <= name.size(); ++i) {
        const bool at_end = i == name.size();
        const char c = at_end ? '\0' : name[i];
        if (!at_end && quoted) {
            if (c == '`' && !(i + 1 < name.size() && name[i + 1] == '`')) {
                quoted = false;
            } else if (c == '`') {
                ++i;
            }
            continue;
        }
        if (!at_end && c == '`') {
            quoted = true;
            continue;
        }
        if (at_end || c == '.') {
            const std::string_view segment = name.substr(start, (at_end ? name.size() : i) - start);
            if (!first) {
                out.push_back('.');
            }
            first = false;
            if (!segment.empty() && segment.front() == '`') {
                // Already quoted by the caller: keep it as written.
                out.append(segment.data(), segment.size());
            } else {
                append_quoted(segment);
            }
            start = i + 1;
        }
    }
    return out;
}

result<std::string> make_insert_sql(const insert_target& target, std::size_t rows) {
    if (rows == 0) {
        return rpc_error{rpc_code::invalid_argument, "an insert batch needs at least one row"};
    }
    if (target.columns.empty()) {
        return rpc_error{rpc_code::invalid_argument, "an insert needs at least one column"};
    }
    const status table_ok = validate_identifier(target.table);
    if (!table_ok) {
        return table_ok.error();
    }
    for (const std::string& column : target.columns) {
        const status column_ok = validate_identifier(column);
        if (!column_ok) {
            return column_ok.error();
        }
    }

    std::string sql;
    sql.reserve(32 + target.table.size() + target.columns.size() * 16 + rows * 8);
    sql += "INSERT INTO ";
    sql += quote_identifier(target.table);
    sql += " (";
    for (std::size_t i = 0; i < target.columns.size(); ++i) {
        if (i != 0) {
            sql += ", ";
        }
        sql += quote_identifier(target.columns[i]);
    }
    sql += ") VALUES ";

    for (std::size_t row = 0; row < rows; ++row) {
        if (row != 0) {
            sql += ", ";
        }
        sql += '(';
        for (std::size_t column = 0; column < target.columns.size(); ++column) {
            if (column != 0) {
                sql += ", ";
            }
            // A placeholder, never a literal: the values are bound.
            sql += '?';
        }
        sql += ')';
    }
    return sql;
}

result<std::vector<std::size_t>> plan_insert_batches(const insert_target& target,
                                                     std::size_t row_count,
                                                     const bulk_options& options) {
    if (row_count == 0) {
        return rpc_error{rpc_code::invalid_argument, "there is nothing to insert"};
    }
    if (target.columns.empty()) {
        return rpc_error{rpc_code::invalid_argument, "an insert needs at least one column"};
    }
    if (options.max_rows_per_statement == 0) {
        return rpc_error{rpc_code::invalid_argument,
                         "max_rows_per_statement must be at least 1"};
    }
    result<std::string> one = make_insert_sql(target, 1);
    if (!one) {
        return one.error();
    }
    const std::size_t base = one.value().size();
    // Each extra row adds ", " plus one '?' and a ", " per column.
    // An extra row adds ,  plus ( + n placeholders + ,  between them + );
    // that is 2 + 3n for n columns.
    const std::size_t per_extra_row = 2 + target.columns.size() * 3;

    std::size_t rows_by_bytes = 1;
    if (options.max_bytes_per_statement > base) {
        rows_by_bytes = 1 + (options.max_bytes_per_statement - base) / per_extra_row;
    }

    std::size_t batch = options.max_rows_per_statement;
    if (rows_by_bytes < batch) {
        batch = rows_by_bytes;
    }
    if (batch == 0) {
        batch = 1;
    }

    std::vector<std::size_t> plan;
    plan.reserve(row_count / batch + 1);
    std::size_t remaining = row_count;
    while (remaining != 0) {
        const std::size_t take = remaining < batch ? remaining : batch;
        plan.push_back(take);
        remaining -= take;
    }
    return plan;
}

} // namespace db
} // namespace crpc