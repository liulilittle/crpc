// CRPC -- bulk insert planning.
//
// A bulk import is not "one INSERT per row": that is a round trip per row and a
// transaction log entry per row. It is also not "one giant INSERT": a statement
// larger than `max_allowed_packet` is rejected by the server, and a single
// statement that fails rolls back the whole batch with no indication of which row
// was bad.
//
// This header is the part that can be verified without a server: how many rows go
// into one statement, and the exact SQL shape. Values are always placeholders — a
// bulk path that formats values into the SQL text is an injection surface, and the
// only correct answer is to bind them like any other statement.
//
// Identifiers (table and column names) cannot be bound, so they are validated
// instead: a bare name or a backtick-quoted name, and nothing else.
#ifndef CRPC_DB_BULK_HPP
#define CRPC_DB_BULK_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace db {

// Where a batch is written.
struct insert_target {
    std::string table;
    std::vector<std::string> columns;
};

struct bulk_options {
    // Rows per statement. Bounds the statement count and the amount of work a
    // single failure discards.
    std::size_t max_rows_per_statement = 1000;

    // Approximate byte ceiling per statement, used to stay under the server's
    // `max_allowed_packet`. Placeholder text is counted, not the values (they are
    // bound), so this is a stable estimate rather than an exact size.
    std::size_t max_bytes_per_statement = 4u * 1024u * 1024u;
};

// Validates one identifier: a bare name of letters, digits, `_`, `$` or `.`, or a
// backtick-quoted name whose body contains no backtick. Anything else is refused,
// because an identifier is the one part of the statement that cannot be bound.
[[nodiscard]] CRPC_API status validate_identifier(std::string_view name);

// Wraps an identifier in backticks, escaping an embedded backtick by doubling it.
// Only call this after `validate_identifier` accepted the name, or on a name the
// caller constructed itself.
[[nodiscard]] CRPC_API std::string quote_identifier(std::string_view name);

// The SQL for one batch of `rows` rows, with `?` placeholders only.
[[nodiscard]] CRPC_API result<std::string> make_insert_sql(const insert_target& target,
                                                          std::size_t rows);

// Splits `row_count` rows into statements. Each entry is the number of rows in that
// statement, so the caller can slice its parameter list without re-deriving the
// rule.
[[nodiscard]] CRPC_API result<std::vector<std::size_t>> plan_insert_batches(
    const insert_target& target, std::size_t row_count, const bulk_options& options);

} // namespace db
} // namespace crpc

#endif // CRPC_DB_BULK_HPP