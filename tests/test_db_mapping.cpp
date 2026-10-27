// CRPC -- column-to-field mapping tests.
//
// The rules here decide whether an ORM reads the right data. Each case is a query
// shape that breaks a naive implementation: an alias, a case difference, a query
// that grew a duplicate column, a partial SELECT, and a decode failure that must not
// leave half an object behind.
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <crpc/db/connection.hpp>
#include <crpc/db/mapping.hpp>
#include <crpc/db/types.hpp>
#include <crpc/error.hpp>

namespace {

using crpc::db::bind_current_row;
using crpc::db::column_info;
using crpc::db::db_type;
using crpc::db::field_binding;
using crpc::db::for_each_row;
using crpc::db::mapping_options;
using crpc::db::resolve_bindings;
using crpc::db::unresolved;

// A cursor over scripted text rows.
class fake_cursor final : public crpc::db::result_set {
public:
    fake_cursor(std::vector<column_info> columns, std::vector<std::vector<std::string>> rows)
        : columns_(std::move(columns)), rows_(std::move(rows)) {}

    std::size_t column_count() const noexcept override { return columns_.size(); }
    column_info column(std::size_t index) const override { return columns_[index]; }

    bool next() override {
        if (next_ >= rows_.size()) {
            return false;
        }
        current_ = next_++;
        return true;
    }

    bool is_null(std::size_t) const noexcept override { return false; }

    crpc::result<std::int64_t> get_int64(std::size_t index) override {
        const std::string& text = rows_[current_][index];
        if (text.empty()) {
            return crpc::rpc_error{crpc::rpc_code::decode_error, "empty"};
        }
        std::int64_t value = 0;
        for (const char c : text) {
            if (c < '0' || c > '9') {
                return crpc::rpc_error{crpc::rpc_code::decode_error, "not a number"};
            }
            value = value * 10 + (c - '0');
        }
        return value;
    }
    crpc::result<std::uint64_t> get_uint64(std::size_t index) override {
        auto value = get_int64(index);
        if (!value.ok()) {
            return value.error();
        }
        return static_cast<std::uint64_t>(value.value());
    }
    crpc::result<double> get_double(std::size_t) override {
        return crpc::rpc_error{crpc::rpc_code::no_such_method, "unused"};
    }
    crpc::result<bool> get_bool(std::size_t) override {
        return crpc::rpc_error{crpc::rpc_code::no_such_method, "unused"};
    }
    crpc::result<std::string_view> get_text(std::size_t index) override {
        return std::string_view(rows_[current_][index]);
    }
    crpc::result<std::vector<std::byte>> get_blob(std::size_t) override {
        return crpc::rpc_error{crpc::rpc_code::no_such_method, "unused"};
    }
    std::int64_t affected_rows() const noexcept override { return -1; }
    void close() noexcept override {}

private:
    std::vector<column_info> columns_;
    std::vector<std::vector<std::string>> rows_;
    std::size_t next_ = 0;
    std::size_t current_ = 0;
};

[[nodiscard]] column_info column(const char* name, const char* original,
                                 db_type type = db_type::text) {
    column_info info;
    info.name = name;
    info.original_name = original;
    info.type = type;
    return info;
}

} // namespace

CRPC_TEST(mapping_prefers_the_original_column_name) {
    // The query aliases `id` as `user_id`; the struct field is named after the
    // column, so binding must use the original name.
    const std::vector<column_info> columns = {column("user_id", "id", db_type::int64),
                                              column("name", "name")};
    std::int64_t id = 0;
    std::string name;
    std::vector<field_binding> bindings;
    bindings.push_back({"id", [&id](crpc::db::result_set& set, std::size_t index) {
                            auto value = set.get_int64(index);
                            if (!value.ok()) {
                                return crpc::status{value.error()};
                            }
                            id = value.value();
                            return crpc::status{};
                        }});
    bindings.push_back({"name", [&name](crpc::db::result_set& set, std::size_t index) {
                            auto value = set.get_text(index);
                            if (!value.ok()) {
                                return crpc::status{value.error()};
                            }
                            name = std::string(value.value());
                            return crpc::status{};
                        }});

    const auto indices = resolve_bindings(crpc::span<const column_info>(columns.data(), columns.size()),
                                          crpc::span<const field_binding>(bindings.data(), bindings.size()));
    CRPC_CHECK(indices.ok());
    if (!indices.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::size_t{2}, indices.value().size());
    CRPC_CHECK_EQ(std::size_t{0}, indices.value()[0]);

    fake_cursor cursor(columns, {{"7", "alice"}});
    CRPC_CHECK(cursor.next());
    CRPC_CHECK(bind_current_row(cursor, crpc::span<const field_binding>(bindings.data(), bindings.size()),
                                crpc::span<const std::size_t>(indices.value().data(),
                                                              indices.value().size()))
                   .ok());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(7), id);
    CRPC_CHECK_EQ(std::string("alice"), name);
}

CRPC_TEST(mapping_falls_back_to_the_alias_and_ignores_case) {
    const std::vector<column_info> columns = {column("User_Id", "", db_type::int64)};
    std::int64_t id = 0;
    std::vector<field_binding> bindings;
    bindings.push_back({"user_id", [&id](crpc::db::result_set& set, std::size_t index) {
                            auto value = set.get_int64(index);
                            if (!value.ok()) {
                                return crpc::status{value.error()};
                            }
                            id = value.value();
                            return crpc::status{};
                        }});

    const auto indices = resolve_bindings(crpc::span<const column_info>(columns.data(), columns.size()),
                                          crpc::span<const field_binding>(bindings.data(), bindings.size()));
    CRPC_CHECK(indices.ok());
    if (!indices.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::size_t{0}, indices.value()[0]);
}

CRPC_TEST(mapping_rejects_an_ambiguous_match) {
    // Two columns claim the same original name: taking the leftmost would bind
    // whichever the query happened to emit first.
    const std::vector<column_info> columns = {column("a", "id"), column("b", "id")};
    std::vector<field_binding> bindings;
    bindings.push_back({"id", [](crpc::db::result_set&, std::size_t) { return crpc::status{}; }});

    const auto indices = resolve_bindings(crpc::span<const column_info>(columns.data(), columns.size()),
                                          crpc::span<const field_binding>(bindings.data(), bindings.size()));
    CRPC_CHECK_FALSE(indices.ok());
    if (!indices.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                      static_cast<int>(indices.error().code));
    }
}

CRPC_TEST(mapping_allows_a_partial_select) {
    // `notes` is not selected; unless the caller demands every field, the mapping
    // succeeds and the field is left untouched.
    const std::vector<column_info> columns = {column("id", "id")};
    std::vector<field_binding> bindings;
    bindings.push_back({"id", [](crpc::db::result_set&, std::size_t) { return crpc::status{}; }});
    bindings.push_back({"notes", [](crpc::db::result_set&, std::size_t) {
                            return crpc::status{crpc::rpc_code::internal, "must not run"};
                        }});

    const auto lenient = resolve_bindings(
        crpc::span<const column_info>(columns.data(), columns.size()),
        crpc::span<const field_binding>(bindings.data(), bindings.size()));
    CRPC_CHECK(lenient.ok());
    if (lenient.ok()) {
        CRPC_CHECK_EQ(unresolved, lenient.value()[1]);
    }

    mapping_options strict;
    strict.require_every_field = true;
    const auto strict_result = resolve_bindings(
        crpc::span<const column_info>(columns.data(), columns.size()),
        crpc::span<const field_binding>(bindings.data(), bindings.size()), strict);
    CRPC_CHECK_FALSE(strict_result.ok());
    if (!strict_result.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                      static_cast<int>(strict_result.error().code));
    }
}

CRPC_TEST(mapping_can_reject_unbound_columns) {
    const std::vector<column_info> columns = {column("id", "id"), column("debug_info", "debug_info")};
    std::vector<field_binding> bindings;
    bindings.push_back({"id", [](crpc::db::result_set&, std::size_t) { return crpc::status{}; }});

    mapping_options options;
    options.reject_unbound_columns = true;
    const auto indices = resolve_bindings(
        crpc::span<const column_info>(columns.data(), columns.size()),
        crpc::span<const field_binding>(bindings.data(), bindings.size()), options);
    CRPC_CHECK_FALSE(indices.ok());
}

CRPC_TEST(mapping_aborts_the_row_when_a_setter_fails) {
    const std::vector<column_info> columns = {column("id", "id"), column("name", "name")};
    std::int64_t id = 0;
    std::string name;
    std::vector<field_binding> bindings;
    bindings.push_back({"id", [&id](crpc::db::result_set& set, std::size_t index) {
                            auto value = set.get_int64(index);
                            if (!value.ok()) {
                                return crpc::status{value.error()};
                            }
                            id = value.value();
                            return crpc::status{};
                        }});
    bindings.push_back({"name", [&name](crpc::db::result_set& set, std::size_t index) {
                            auto value = set.get_text(index);
                            if (!value.ok()) {
                                return crpc::status{value.error()};
                            }
                            name = std::string(value.value());
                            return crpc::status{};
                        }});

    const auto indices = resolve_bindings(crpc::span<const column_info>(columns.data(), columns.size()),
                                          crpc::span<const field_binding>(bindings.data(), bindings.size()));
    CRPC_CHECK(indices.ok());
    if (!indices.ok()) {
        return;
    }
    // The first row decodes as an integer where "abc" is not one.
    fake_cursor cursor(columns, {{"abc", "x"}});
    CRPC_CHECK(cursor.next());
    const crpc::status bound =
        bind_current_row(cursor, crpc::span<const field_binding>(bindings.data(), bindings.size()),
                         crpc::span<const std::size_t>(indices.value().data(),
                                                       indices.value().size()));
    CRPC_CHECK_FALSE(bound.ok());
    if (!bound.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::decode_error),
                      static_cast<int>(bound.code()));
    }
    // The name setter never ran: a half-filled object would be indistinguishable from
    // a real one.
    CRPC_CHECK(name.empty());
}

CRPC_TEST(mapping_iterates_every_row) {
    const std::vector<column_info> columns = {column("id", "id", db_type::int64)};
    std::vector<std::int64_t> ids;
    std::vector<field_binding> bindings;
    bindings.push_back({"id", [&ids](crpc::db::result_set& set, std::size_t index) {
                            auto value = set.get_int64(index);
                            if (!value.ok()) {
                                return crpc::status{value.error()};
                            }
                            ids.push_back(value.value());
                            return crpc::status{};
                        }});

    const auto indices = resolve_bindings(crpc::span<const column_info>(columns.data(), columns.size()),
                                          crpc::span<const field_binding>(bindings.data(), bindings.size()));
    CRPC_CHECK(indices.ok());
    if (!indices.ok()) {
        return;
    }
    fake_cursor cursor(columns, {{"1"}, {"2"}, {"3"}});
    const crpc::status read =
        for_each_row(cursor, crpc::span<const field_binding>(bindings.data(), bindings.size()),
                     crpc::span<const std::size_t>(indices.value().data(), indices.value().size()), {});
    CRPC_CHECK(read.ok());
    CRPC_CHECK_EQ(std::size_t{3}, ids.size());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(3), ids[2]);
}

CRPC_TEST(mapping_rejects_a_size_mismatch) {
    const std::vector<column_info> columns = {column("id", "id")};
    std::vector<field_binding> bindings;
    bindings.push_back({"id", [](crpc::db::result_set&, std::size_t) { return crpc::status{}; }});
    fake_cursor cursor(columns, {{"1"}});
    CRPC_CHECK(cursor.next());
    // Two indices for one binding is a caller bug, not a data condition.
    const std::size_t wrong[2] = {0, 0};
    CRPC_CHECK_FALSE(
        bind_current_row(cursor, crpc::span<const field_binding>(bindings.data(), bindings.size()),
                         crpc::span<const std::size_t>(wrong, 2))
            .ok());
}

CRPC_TEST_MAIN()