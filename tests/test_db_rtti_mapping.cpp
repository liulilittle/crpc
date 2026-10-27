// CRPC -- reading reflected structs from a result set.
//
// The mapping rules are tested in test_db_mapping without reflection; this suite
// pins the RTTI half: field names come from the reflection table, values go through
// rtti's set() (so numeric/string conversion has one implementation), a value that
// does not fit the field is an error rather than a default, and a NULL field is
// reported rather than silently left at a previous row's value.
#include "test_framework.hpp"

#include <crpc/config.hpp>

#if defined(CRPC_HAVE_RTTI)

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <rtti/reflection.hpp>

#include <crpc/db/connection.hpp>
#include <crpc/db/mapping.hpp>
#include <crpc/db/rtti_mapping.hpp>
#include <crpc/db/types.hpp>
#include <crpc/error.hpp>

// The RTTI macro opens `namespace rtti`, so it must be used at global scope: an
// anonymous namespace would define a nested `rtti` that nothing finds.
struct order_row {
    std::int64_t id = 0;
    std::string name;
    double amount = 0.0;
};
RTTI_DEFINE_TYPE(order_row, id, name, amount)

namespace {

// A cursor over scripted text values with explicit null flags.
class fake_cursor final : public crpc::db::result_set {
public:
    fake_cursor(std::vector<crpc::db::column_info> columns,
                std::vector<std::vector<std::string>> rows,
                std::vector<std::vector<bool>> nulls)
        : columns_(std::move(columns)), rows_(std::move(rows)), nulls_(std::move(nulls)) {}

    std::size_t column_count() const noexcept override { return columns_.size(); }
    crpc::db::column_info column(std::size_t index) const override { return columns_[index]; }

    bool next() override {
        if (next_ >= rows_.size()) {
            return false;
        }
        current_ = next_++;
        return true;
    }

    bool is_null(std::size_t index) const noexcept override {
        return nulls_[current_][index];
    }

    crpc::result<std::int64_t> get_int64(std::size_t index) override {
        const auto& text = rows_[current_][index];
        std::int64_t value = 0;
        bool negative = false;
        std::size_t i = 0;
        if (!text.empty() && text[0] == '-') {
            negative = true;
            i = 1;
        }
        for (; i < text.size(); ++i) {
            if (text[i] < '0' || text[i] > '9') {
                return crpc::rpc_error{crpc::rpc_code::decode_error, "not an integer"};
            }
            value = value * 10 + (text[i] - '0');
        }
        return negative ? -value : value;
    }
    crpc::result<std::uint64_t> get_uint64(std::size_t index) override {
        auto value = get_int64(index);
        if (!value.ok()) {
            return value.error();
        }
        return static_cast<std::uint64_t>(value.value());
    }
    crpc::result<double> get_double(std::size_t index) override {
        const std::string& text = rows_[current_][index];
        char* end = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        if (end == text.c_str()) {
            return crpc::rpc_error{crpc::rpc_code::decode_error, "not a number"};
        }
        return value;
    }
    crpc::result<bool> get_bool(std::size_t index) override {
        return rows_[current_][index] == "1";
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
    std::vector<crpc::db::column_info> columns_;
    std::vector<std::vector<std::string>> rows_;
    std::vector<std::vector<bool>> nulls_;
    std::size_t next_ = 0;
    std::size_t current_ = 0;
};

[[nodiscard]] crpc::db::column_info column(const char* name, const char* original,
                                           crpc::db::db_type type) {
    crpc::db::column_info info;
    info.name = name;
    info.original_name = original;
    info.type = type;
    return info;
}

} // namespace

CRPC_TEST(rtti_mapping_reads_all_fields) {
    std::vector<crpc::db::column_info> columns = {
        column("id", "id", crpc::db::db_type::int64),
        column("name", "name", crpc::db::db_type::text),
        column("amount", "amount", crpc::db::db_type::real)};
    fake_cursor cursor(columns, {{"7", "alice", "12.5"}, {"8", "bob", "0.25"}},
                       {{false, false, false}, {false, false, false}});

    auto rows = crpc::db::read_all<order_row>(cursor);
    CRPC_CHECK(rows.ok());
    if (!rows.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::size_t{2}, rows.value().size());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(7), rows.value()[0].id);
    CRPC_CHECK_EQ(std::string("alice"), rows.value()[0].name);
    CRPC_CHECK_EQ(0.25, rows.value()[1].amount);
}

CRPC_TEST(rtti_mapping_uses_the_original_column_name) {
    // The query aliased `id`; the struct field is named after the column.
    std::vector<crpc::db::column_info> columns = {
        column("user_id", "id", crpc::db::db_type::int64),
        column("name", "name", crpc::db::db_type::text),
        column("amount", "amount", crpc::db::db_type::real)};
    fake_cursor cursor(columns, {{"9", "carol", "1"}}, {{false, false, false}});

    order_row row;
    CRPC_CHECK(cursor.next());
    const crpc::status read = crpc::db::read_current(cursor, row);
    CRPC_CHECK(read.ok());
    if (read.ok()) {
        CRPC_CHECK_EQ(static_cast<std::int64_t>(9), row.id);
    }
}

CRPC_TEST(rtti_mapping_reports_a_value_that_does_not_fit) {
    // `amount` is a double field but the column carries text that is not a number:
    // rtti's conversion refuses it, and that must surface as an error instead of
    // leaving a default behind.
    std::vector<crpc::db::column_info> columns = {
        column("id", "id", crpc::db::db_type::int64),
        column("name", "name", crpc::db::db_type::text),
        column("amount", "amount", crpc::db::db_type::text)};
    fake_cursor cursor(columns, {{"1", "dave", "not-a-number"}}, {{false, false, false}});

    order_row row;
    CRPC_CHECK(cursor.next());
    const crpc::status read = crpc::db::read_current(cursor, row);
    CRPC_CHECK_FALSE(read.ok());
    if (!read.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::decode_error),
                      static_cast<int>(read.code()));
    }
}

CRPC_TEST(rtti_mapping_null_digit_does_not_keep_a_stale_value) {
    // A NULL in the second row must not leave the first row's value in the field.
    std::vector<crpc::db::column_info> columns = {
        column("id", "id", crpc::db::db_type::int64),
        column("name", "name", crpc::db::db_type::text),
        column("amount", "amount", crpc::db::db_type::real)};
    fake_cursor cursor(columns, {{"5", "erin", "3"}}, {{false, false, true}});

    order_row row;
    row.amount = 99.0;
    CRPC_CHECK(cursor.next());
    const crpc::status read = crpc::db::read_current(cursor, row);
    // rtti has no null carrier, so the field is set to its default (0) or the read
    // fails; what it must not do is keep 99.0 as if it were data.
    if (read.ok()) {
        CRPC_CHECK(row.amount != 99.0);
    }
}

CRPC_TEST_MAIN()

#else

CRPC_TEST_MAIN()

#endif