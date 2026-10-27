// CRPC -- bulk insert planning and execution tests.
//
// A bulk path has two ways to be unsafe or wrong: formatting values into the SQL
// text (injection), and letting a statement exceed the server's packet limit so the
// whole batch fails with no indication of which row was bad. Both are pinned here:
// every value is a placeholder, and the plan bounds rows and bytes per statement.
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/db/bulk.hpp>
#include <crpc/db/mysql/channel.hpp>
#include <crpc/db/mysql/connection.hpp>
#include <crpc/db/mysql/protocol.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace {

using crpc::db::bulk_options;
using crpc::db::insert_target;
using crpc::db::make_insert_sql;
using crpc::db::plan_insert_batches;
using crpc::db::validate_identifier;
using crpc::db::mysql::capability;
using crpc::db::mysql::connection;
using crpc::db::mysql::connection_options;
using crpc::db::mysql::mask;
using crpc::db::parameter;

[[nodiscard]] insert_target simple_target() {
    insert_target target;
    target.table = "users";
    target.columns = {"id", "name"};
    return target;
}

} // namespace

// --- SQL shape ---------------------------------------------------------------

CRPC_TEST(bulk_insert_sql_uses_placeholders_only) {
    const auto sql = make_insert_sql(simple_target(), 2);
    CRPC_CHECK(sql.ok());
    if (sql.ok()) {
        // Identifiers are quoted, values are never inlined.
        CRPC_CHECK_EQ(std::string("INSERT INTO `users` (`id`, `name`) VALUES (?, ?), (?, ?)"),
                      sql.value());
    }
}

CRPC_TEST(bulk_insert_sql_rejects_bad_identifiers) {
    {
        insert_target target;
        target.table = "users; DROP TABLE x";
        target.columns = {"id"};
        CRPC_CHECK_FALSE(make_insert_sql(target, 1).ok());
    }
    {
        insert_target target;
        target.table = "users";
        target.columns = {"id` , (SELECT 1)"};
        CRPC_CHECK_FALSE(make_insert_sql(target, 1).ok());
    }
    {
        insert_target target;
        target.table = "users";
        target.columns = {};
        CRPC_CHECK_FALSE(make_insert_sql(target, 1).ok());
    }
    {
        // Zero rows is a programming error, not an empty statement.
        CRPC_CHECK_FALSE(make_insert_sql(simple_target(), 0).ok());
    }
}

CRPC_TEST(bulk_insert_accepts_quoted_identifiers) {
    insert_target target;
    target.table = "`schema`.`weird name`";
    target.columns = {"`col one`"};
    const auto sql = make_insert_sql(target, 1);
    CRPC_CHECK(sql.ok());
    if (sql.ok()) {
        CRPC_CHECK_EQ(std::string("INSERT INTO `schema`.`weird name` (`col one`) VALUES (?)"),
                      sql.value());
    }
}

CRPC_TEST(bulk_identifier_validation_rules) {
    CRPC_CHECK(validate_identifier("users").ok());
    CRPC_CHECK(validate_identifier("schema.users").ok());
    CRPC_CHECK(validate_identifier("`a b`").ok());
    CRPC_CHECK(validate_identifier("`a``b`").ok()); // doubled backtick is literal
    CRPC_CHECK_FALSE(validate_identifier("").ok());
    CRPC_CHECK_FALSE(validate_identifier("a b").ok());
    CRPC_CHECK_FALSE(validate_identifier("a;b").ok());
    CRPC_CHECK_FALSE(validate_identifier("a'b").ok());
    CRPC_CHECK_FALSE(validate_identifier("`a`b`").ok());
    CRPC_CHECK_FALSE(validate_identifier("`unterminated").ok());
}

// --- Batching ----------------------------------------------------------------

CRPC_TEST(bulk_plan_splits_by_row_limit) {
    bulk_options options;
    options.max_rows_per_statement = 100;
    options.max_bytes_per_statement = 1u << 20;
    const auto plan = plan_insert_batches(simple_target(), 250, options);
    CRPC_CHECK(plan.ok());
    if (!plan.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::size_t{3}, plan.value().size());
    CRPC_CHECK_EQ(std::size_t{100}, plan.value()[0]);
    CRPC_CHECK_EQ(std::size_t{100}, plan.value()[1]);
    CRPC_CHECK_EQ(std::size_t{50}, plan.value()[2]);
}

CRPC_TEST(bulk_plan_splits_by_byte_budget) {
    // One row is roughly 8 bytes of SQL; a 64-byte budget must produce many small
    // statements rather than one oversized one.
    bulk_options options;
    options.max_rows_per_statement = 1000000;
    options.max_bytes_per_statement = 64;
    const auto plan = plan_insert_batches(simple_target(), 100, options);
    CRPC_CHECK(plan.ok());
    if (!plan.ok()) {
        return;
    }
    CRPC_CHECK(plan.value().size() > 5);
    std::size_t total = 0;
    for (const std::size_t rows : plan.value()) {
        CRPC_CHECK(rows >= 1);
        total += rows;
    }
    CRPC_CHECK_EQ(std::size_t{100}, total);
}

CRPC_TEST(bulk_plan_always_makes_progress) {
    // A budget smaller than a single statement still emits one row per batch: the
    // alternative is an empty plan and a silent no-op import.
    bulk_options options;
    options.max_bytes_per_statement = 1;
    const auto plan = plan_insert_batches(simple_target(), 3, options);
    CRPC_CHECK(plan.ok());
    if (plan.ok()) {
        CRPC_CHECK_EQ(std::size_t{3}, plan.value().size());
        for (const std::size_t rows : plan.value()) {
            CRPC_CHECK_EQ(std::size_t{1}, rows);
        }
    }
}

CRPC_TEST(bulk_plan_rejects_empty_input) {
    bulk_options options;
    CRPC_CHECK_FALSE(plan_insert_batches(simple_target(), 0, options).ok());
    insert_target empty;
    empty.table = "t";
    CRPC_CHECK_FALSE(plan_insert_batches(empty, 1, options).ok());
    options.max_rows_per_statement = 0;
    CRPC_CHECK_FALSE(plan_insert_batches(simple_target(), 1, options).ok());
}

// --- Execution over a scripted server ----------------------------------------

namespace {

void append_u16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value & 0xffu));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xffu));
}

void append_u32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xffu));
    }
}

void append_text(std::vector<std::byte>& out, std::string_view text) {
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
}

[[nodiscard]] std::vector<std::byte> packet(const std::vector<std::byte>& payload,
                                            std::uint8_t sequence) {
    std::vector<std::byte> out;
    out.push_back(static_cast<std::byte>(payload.size() & 0xffu));
    out.push_back(static_cast<std::byte>((payload.size() >> 8) & 0xffu));
    out.push_back(static_cast<std::byte>((payload.size() >> 16) & 0xffu));
    out.push_back(static_cast<std::byte>(sequence));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

[[nodiscard]] std::vector<std::byte> ok_payload(std::uint64_t affected) {
    std::vector<std::byte> payload;
    payload.push_back(std::byte{0});
    crpc::db::mysql::append_lenenc_int(payload, affected);
    crpc::db::mysql::append_lenenc_int(payload, 0);
    append_u16(payload, 2);
    append_u16(payload, 0);
    return payload;
}

[[nodiscard]] std::vector<std::byte> prepare_ok_payload(std::uint32_t id, std::uint16_t params) {
    std::vector<std::byte> payload;
    payload.push_back(std::byte{0});
    append_u32(payload, id);
    append_u16(payload, 0);   // columns
    append_u16(payload, params);
    payload.push_back(std::byte{0});
    append_u16(payload, 0);
    return payload;
}

[[nodiscard]] std::vector<std::byte> eof_payload() {
    std::vector<std::byte> payload;
    payload.push_back(std::byte{0xfe});
    append_u16(payload, 0);
    append_u16(payload, 2);
    return payload;
}

[[nodiscard]] std::vector<std::byte> column_payload(std::string_view name, std::uint8_t type) {
    std::vector<std::byte> payload;
    crpc::db::mysql::append_lenenc_string(payload, "def");
    crpc::db::mysql::append_lenenc_string(payload, "app");
    crpc::db::mysql::append_lenenc_string(payload, "users");
    crpc::db::mysql::append_lenenc_string(payload, "users");
    crpc::db::mysql::append_lenenc_string(payload, name);
    crpc::db::mysql::append_lenenc_string(payload, name);
    crpc::db::mysql::append_lenenc_int(payload, 0x0c);
    append_u16(payload, 33);
    append_u32(payload, 255);
    payload.push_back(static_cast<std::byte>(type));
    append_u16(payload, 0);
    payload.push_back(std::byte{0});
    append_u16(payload, 0);
    return payload;
}

constexpr std::uint32_t server_capabilities =
    mask(capability::protocol_41) | mask(capability::secure_connection) |
    mask(capability::plugin_auth) | mask(capability::transactions) |
    mask(capability::multi_results);

[[nodiscard]] std::vector<std::byte> greeting_payload() {
    std::vector<std::byte> payload;
    payload.push_back(std::byte{10});
    append_text(payload, "8.0.36");
    payload.push_back(std::byte{0});
    append_u32(payload, 42);
    for (int i = 0; i < 8; ++i) {
        payload.push_back(static_cast<std::byte>(i + 1));
    }
    payload.push_back(std::byte{0});
    append_u16(payload, static_cast<std::uint16_t>(server_capabilities & 0xffffu));
    payload.push_back(std::byte{45});
    append_u16(payload, 2);
    append_u16(payload, static_cast<std::uint16_t>((server_capabilities >> 16) & 0xffffu));
    payload.push_back(std::byte{21});
    for (int i = 0; i < 10; ++i) {
        payload.push_back(std::byte{0});
    }
    for (int i = 0; i < 12; ++i) {
        payload.push_back(static_cast<std::byte>(i + 9));
    }
    payload.push_back(std::byte{0});
    append_text(payload, "caching_sha2_password");
    payload.push_back(std::byte{0});
    return payload;
}

class scripted_channel final : public crpc::db::mysql::channel {
public:
    std::deque<std::vector<std::byte>> incoming;
    std::vector<std::vector<std::byte>> writes;
    bool closed = false;

    crpc::result<std::vector<std::byte>> read_exact(std::size_t count) override {
        std::vector<std::byte> out;
        out.reserve(count);
        while (out.size() < count) {
            if (incoming.empty()) {
                return crpc::rpc_error{crpc::rpc_code::eof, "scripted channel ran out"};
            }
            std::vector<std::byte>& front = incoming.front();
            const std::size_t take = std::min(count - out.size(), front.size());
            out.insert(out.end(), front.begin(),
                       front.begin() + static_cast<std::ptrdiff_t>(take));
            front.erase(front.begin(), front.begin() + static_cast<std::ptrdiff_t>(take));
            if (front.empty()) {
                incoming.pop_front();
            }
        }
        return out;
    }

    crpc::status write(crpc::span<const std::byte> data) override {
        writes.emplace_back(data.begin(), data.end());
        return crpc::status{};
    }

    void close() noexcept override { closed = true; }
    void push(std::vector<std::byte> data) { incoming.push_back(std::move(data)); }

    [[nodiscard]] std::uint8_t command_of(std::size_t index) const {
        return static_cast<std::uint8_t>(writes[index][4]);
    }
    [[nodiscard]] std::string text_of(std::size_t index) const {
        const std::vector<std::byte>& frame = writes[index];
        return std::string(reinterpret_cast<const char*>(frame.data() + 5), frame.size() - 5);
    }
};

struct harness {
    scripted_channel* raw = nullptr;
    std::unique_ptr<connection> session;

    harness() {
        auto scripted = std::unique_ptr<scripted_channel>(new scripted_channel);
        raw = scripted.get();
        raw->push(packet(greeting_payload(), 0));
        raw->push(packet(ok_payload(0), 2));
        connection_options options;
        options.user = "root";
        options.password = "";
        session.reset(new connection(options, std::move(scripted)));
    }

    void script_batch(std::uint32_t id, std::uint16_t params, std::uint64_t affected) {
        raw->push(packet(prepare_ok_payload(id, params), 1));
        // One parameter definition per bound value, then EOF.
        for (std::uint16_t i = 0; i < params; ++i) {
            raw->push(packet(column_payload("p", 8), static_cast<std::uint8_t>(2 + i)));
        }
        raw->push(packet(eof_payload(), static_cast<std::uint8_t>(2 + params)));
        raw->push(packet(ok_payload(affected), 1));
    }
};

} // namespace

CRPC_TEST(bulk_insert_batches_and_binds_every_value) {
    harness h;
    h.script_batch(101, 2, 1); // row 1 (2 params)
    h.script_batch(102, 2, 1); // row 2
    h.script_batch(103, 2, 1); // row 3

    std::vector<std::vector<parameter>> rows;
    rows.push_back({parameter::from_int64(1), parameter::from_text("a")});
    rows.push_back({parameter::from_int64(2), parameter::from_text("b")});
    rows.push_back({parameter::from_int64(3), parameter::from_text("c")});

    bulk_options options;
    options.max_rows_per_statement = 1; // one row per statement: three batches

    std::uint64_t affected = 0;
    const crpc::status inserted = h.session->bulk_insert(
        simple_target(), crpc::span<const std::vector<parameter>>(rows.data(), rows.size()),
        options, affected);
    CRPC_CHECK(inserted.ok());
    if (!inserted.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::uint64_t{3}, affected);

    // Three prepare, three execute, three close — writes[0] is the handshake.
    std::size_t prepares = 0;
    std::size_t executes = 0;
    std::size_t closes = 0;
    for (std::size_t i = 0; i < h.raw->writes.size(); ++i) {
        const std::uint8_t command = h.raw->command_of(i);
        if (command == static_cast<std::uint8_t>(crpc::db::mysql::com_stmt_prepare)) {
            ++prepares;
            CRPC_CHECK_EQ(std::string("INSERT INTO `users` (`id`, `name`) VALUES (?, ?)"),
                          h.raw->text_of(i));
        } else if (command == static_cast<std::uint8_t>(crpc::db::mysql::com_stmt_execute)) {
            ++executes;
        } else if (command == static_cast<std::uint8_t>(crpc::db::mysql::com_stmt_close)) {
            ++closes;
        }
    }
    CRPC_CHECK_EQ(std::size_t{3}, prepares);
    CRPC_CHECK_EQ(std::size_t{3}, executes);
    CRPC_CHECK_EQ(std::size_t{3}, closes);
}

CRPC_TEST(bulk_insert_rejects_a_row_with_the_wrong_column_count) {
    harness h;
    std::vector<std::vector<parameter>> rows;
    rows.push_back({parameter::from_int64(1)}); // the target has two columns
    bulk_options options;
    std::uint64_t affected = 99;
    const crpc::status inserted = h.session->bulk_insert(
        simple_target(), crpc::span<const std::vector<parameter>>(rows.data(), rows.size()),
        options, affected);
    CRPC_CHECK_FALSE(inserted.ok());
    if (!inserted.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(inserted.code()));
    }
    // Nothing was sent, and the counter is not left holding a stale value.
    CRPC_CHECK_EQ(std::uint64_t{0}, affected);
    CRPC_CHECK_EQ(std::size_t{1}, h.raw->writes.size());
}

CRPC_TEST(bulk_insert_of_no_rows_is_a_no_op) {
    harness h;
    bulk_options options;
    std::uint64_t affected = 5;
    const crpc::status inserted =
        h.session->bulk_insert(simple_target(), crpc::span<const std::vector<parameter>>(),
                               options, affected);
    CRPC_CHECK(inserted.ok());
    CRPC_CHECK_EQ(std::uint64_t{0}, affected);
    CRPC_CHECK_EQ(std::size_t{1}, h.raw->writes.size());
}

CRPC_TEST_MAIN()