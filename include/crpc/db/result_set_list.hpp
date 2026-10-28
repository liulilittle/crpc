// CRPC -- a materialised cursor that owns several result sets.
//
// Some protocols deliver a batch's reply as a sequence of result sets (SQL Server
// `SELECT 1; SELECT 2`, MySQL multi-statements). An eager decoder reads and
// materialises every set anyway to keep the stream aligned; this wrapper keeps
// them all and hands them out through `next_result()` instead of discarding all
// but the first. Every member vanishes as before -- the only new surface is that
// the caller may step through the sets it was sent.
#ifndef CRPC_DB_RESULT_SET_LIST_HPP
#define CRPC_DB_RESULT_SET_LIST_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/db/connection.hpp>
#include <crpc/db/types.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace db {

class CRPC_API result_set_list final : public result_set {
public:
    result_set_list() noexcept = default;

    // Takes ownership of `set`, appended after the sets already held. A list with
    // no member reports no columns and no rows.
    void add(std::unique_ptr<result_set> set);

    [[nodiscard]] std::size_t column_count() const noexcept override;
    [[nodiscard]] column_info column(std::size_t index) const override;

    [[nodiscard]] bool next() override;
    [[nodiscard]] bool next_result() override;

    [[nodiscard]] bool is_null(std::size_t index) const noexcept override;
    [[nodiscard]] result<std::int64_t> get_int64(std::size_t index) override;
    [[nodiscard]] result<std::uint64_t> get_uint64(std::size_t index) override;
    [[nodiscard]] result<double> get_double(std::size_t index) override;
    [[nodiscard]] result<bool> get_bool(std::size_t index) override;
    [[nodiscard]] result<std::string_view> get_text(std::size_t index) override;
    [[nodiscard]] result<std::vector<std::byte>> get_blob(std::size_t index) override;

    [[nodiscard]] std::int64_t affected_rows() const noexcept override;

    // Delegated to the current member, so a list of materialised sets reports what
    // the set being read actually holds.
    [[nodiscard]] std::size_t rows_decoded() const noexcept override;
    [[nodiscard]] std::size_t rows_buffered() const noexcept override;

    void close() noexcept override;

private:
    [[nodiscard]] result_set* current() const noexcept;

    std::vector<std::unique_ptr<result_set>> sets_;
    std::size_t cursor_ = 0;
    bool closed_ = false;
};

} // namespace db
} // namespace crpc

#endif // CRPC_DB_RESULT_SET_LIST_HPP