// CRPC -- a fully materialised result set.
//
// The rows are copied at query time on purpose: a cursor that lazily read from the
// socket would keep the connection busy for as long as the caller holds it, and the
// pool would have to know that. Materialising bounds the memory (via the adapter's
// options) and lets the connection go back to the pool immediately.
//
// This lives in `crpc::db` because every synchronous adapter stores rows the same
// way: the values arrive as canonical text, and the typed accessors parse that text
// on demand. A second copy would be duplicate knowledge — and the MySQL version
// already had to fix the "parse 5 as a signed 64-bit value" overflow once.
#ifndef CRPC_DB_MEMORY_RESULT_SET_HPP
#define CRPC_DB_MEMORY_RESULT_SET_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/db/connection.hpp>
#include <crpc/db/types.hpp>

namespace crpc {
namespace db {

class CRPC_API memory_result_set final : public result_set {
public:
    memory_result_set(std::vector<column_info> columns, std::int64_t affected) noexcept;

    // Appends one row. `values` and `nulls` must be the same length; the nulls are
    // kept rather than encoded as an empty string, because an empty string and a
    // NULL are different values in every database here.
    void add_row(std::vector<std::string> values, std::vector<bool> nulls);

    [[nodiscard]] std::size_t column_count() const noexcept override;
    [[nodiscard]] column_info column(std::size_t index) const override;

    // Forward-only cursor: `next()` moves to the following row and every accessor
    // reads that row. Before the first `next()` there is no current row, and the
    // accessors report NULL rather than reading row 0 behind the caller's back.
    [[nodiscard]] bool next() override;

    [[nodiscard]] bool is_null(std::size_t index) const noexcept override;
    [[nodiscard]] result<std::int64_t> get_int64(std::size_t index) override;
    [[nodiscard]] result<std::uint64_t> get_uint64(std::size_t index) override;
    [[nodiscard]] result<double> get_double(std::size_t index) override;
    [[nodiscard]] result<bool> get_bool(std::size_t index) override;
    [[nodiscard]] result<std::string_view> get_text(std::size_t index) override;
    [[nodiscard]] result<std::vector<std::byte>> get_blob(std::size_t index) override;

    [[nodiscard]] std::int64_t affected_rows() const noexcept override;

    void close() noexcept override;

    // Diagnostics for adapters that enforce row/byte ceilings while decoding.
    [[nodiscard]] std::size_t row_count() const noexcept { return values_.size(); }

private:
    // A missing cursor position or a NULL both yield a null `data()` pointer, and
    // every accessor checks it first.
    [[nodiscard]] std::string_view value_at(std::size_t index) const;

    std::vector<column_info> columns_;
    std::vector<std::vector<std::string>> values_;
    std::vector<std::vector<bool>> nulls_;
    std::int64_t affected_ = -1;
    std::size_t next_ = 0;
    std::size_t cursor_ = 0;
    bool has_row_ = false;
    bool closed_ = false;
};

} // namespace db
} // namespace crpc

#endif // CRPC_DB_MEMORY_RESULT_SET_HPP