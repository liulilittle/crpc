// CRPC -- stored procedure CALL with OUT/INOUT parameters.
//
// MySQL requires a procedure's OUT argument to be a session variable, so the client
// rewrites that placeholder, seeds INOUT values with a prepared SET, and reads the
// variables back with one SELECT. Three things can go wrong and each is pinned
// here: rewriting a '?' inside a string literal, leaving the trailing OK of the
// call in the stream, and not draining it at all. All three are silent at runtime.
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/db/mysql/channel.hpp>
#include <crpc/db/mysql/connection.hpp>
#include <crpc/db/mysql/protocol.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace {

using crpc::db::mysql::capability;
using crpc::db::mysql::connection;
using crpc::db::mysql::connection_options;
using crpc::db::mysql::mask;
using crpc::db::parameter;
using crpc::db::parameter_direction;

[[nodiscard]] std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> out(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        out[i] = static_cast<std::byte>(static_cast<unsigned char>(text[i]));
    }
    return out;
}

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
    const std::vector<std::byte> encoded = bytes_of(text);
    out.insert(out.end(), encoded.begin(), encoded.end());
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

[[nodiscard]] std::vector<std::byte> ok_payload(std::uint16_t status) {
    std::vector<std::byte> payload;
    payload.push_back(std::byte{0});
    crpc::db::mysql::append_lenenc_int(payload, 0);
    crpc::db::mysql::append_lenenc_int(payload, 0);
    append_u16(payload, status);
    append_u16(payload, 0);
    return payload;
}

[[nodiscard]] std::vector<std::byte> eof_payload(std::uint16_t status) {
    std::vector<std::byte> payload;
    payload.push_back(std::byte{0xfe});
    append_u16(payload, 0);
    append_u16(payload, status);
    return payload;
}

[[nodiscard]] std::vector<std::byte> prepare_ok_payload(std::uint32_t id, std::uint16_t columns,
                                                       std::uint16_t params) {
    std::vector<std::byte> payload;
    payload.push_back(std::byte{0});
    append_u32(payload, id);
    append_u16(payload, columns);
    append_u16(payload, params);
    payload.push_back(std::byte{0});
    append_u16(payload, 0);
    return payload;
}

[[nodiscard]] std::vector<std::byte> column_payload(std::string_view name, std::uint8_t type) {
    std::vector<std::byte> payload;
    crpc::db::mysql::append_lenenc_string(payload, "def");
    crpc::db::mysql::append_lenenc_string(payload, "app");
    crpc::db::mysql::append_lenenc_string(payload, "t");
    crpc::db::mysql::append_lenenc_string(payload, "t");
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
        raw->push(packet(ok_payload(2), 2));
        connection_options options;
        options.user = "root";
        options.password = "";
        session.reset(new connection(options, std::move(scripted)));
    }
};

} // namespace

CRPC_TEST(mysql_call_with_out_parameter_reads_it_back) {
    harness h;
    // prepare "CALL p(?, @crpc_out_1)": one bound parameter, no result columns.
    h.raw->push(packet(prepare_ok_payload(61, 0, 1), 1));
    h.raw->push(packet(column_payload("p1", 8), 2));
    h.raw->push(packet(eof_payload(2), 3));
    // execute: OK with MORE_RESULTS, then the trailing OK.
    h.raw->push(packet(ok_payload(0x0008), 1));
    h.raw->push(packet(ok_payload(0x0002), 2));
    // SELECT @crpc_out_1: one column, one row "42".
    h.raw->push(packet({std::byte{1}}, 1));
    h.raw->push(packet(column_payload("@crpc_out_1", 253), 2));
    h.raw->push(packet(eof_payload(2), 3));
    std::vector<std::byte> row;
    crpc::db::mysql::append_lenenc_string(row, "42");
    h.raw->push(packet(row, 4));
    h.raw->push(packet(eof_payload(2), 5));

    std::vector<parameter> params;
    params.push_back(parameter::from_int64(7));
    params.push_back(parameter::out("result", crpc::db::db_type::int64));

    crpc::db::result_set* rows = nullptr;
    const crpc::status called = h.session->call(
        "CALL p(?, ?)", crpc::span<parameter>(params.data(), params.size()), &rows);
    CRPC_CHECK(called.ok());
    if (!called.ok()) {
        return;
    }
    delete rows;

    // The OUT placeholder became a session variable; the IN placeholder stayed "?".
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::mysql::com_stmt_prepare),
                  static_cast<int>(h.raw->command_of(1)));
    CRPC_CHECK_EQ(std::string("CALL p(?, @crpc_out_1)"), h.raw->text_of(1));

    // The OUT value came back into the caller's parameter.
    CRPC_CHECK_FALSE(params[1].is_null);
    CRPC_CHECK_EQ(static_cast<std::int64_t>(42), params[1].int_value);

    // writes[0] is the handshake response; then prepare, execute, close and the
    // SELECT. Counting from zero without the handshake is the easiest way to make a
    // test pass against the wrong command.
    CRPC_CHECK_EQ(std::size_t{5}, h.raw->writes.size());
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::mysql::com_stmt_execute),
                  static_cast<int>(h.raw->command_of(2)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::mysql::com_stmt_close),
                  static_cast<int>(h.raw->command_of(3)));
    CRPC_CHECK_EQ(static_cast<int>(0x03), static_cast<int>(h.raw->command_of(4)));
    CRPC_CHECK_EQ(std::string("SELECT @crpc_out_1"), h.raw->text_of(4));
}

CRPC_TEST(mysql_call_seeds_an_inout_parameter_first) {
    harness h;
    // 1. SET @crpc_out_1 = ?  (prepare + execute + close)
    h.raw->push(packet(prepare_ok_payload(71, 0, 1), 1));
    h.raw->push(packet(column_payload("p1", 8), 2));
    h.raw->push(packet(eof_payload(2), 3));
    h.raw->push(packet(ok_payload(0), 1));
    // 2. CALL p(@crpc_out_1) (no parameters to bind)
    h.raw->push(packet(prepare_ok_payload(72, 0, 0), 1));
    h.raw->push(packet(ok_payload(0x0008), 1));
    h.raw->push(packet(ok_payload(0x0002), 2));
    // 3. SELECT @crpc_out_1
    h.raw->push(packet({std::byte{1}}, 1));
    h.raw->push(packet(column_payload("@crpc_out_1", 253), 2));
    h.raw->push(packet(eof_payload(2), 3));
    std::vector<std::byte> row;
    crpc::db::mysql::append_lenenc_string(row, "8");
    h.raw->push(packet(row, 4));
    h.raw->push(packet(eof_payload(2), 5));

    std::vector<parameter> params;
    params.push_back(parameter::from_int64(1));
    params.back().direction = parameter_direction::input_output;
    params.back().type = crpc::db::db_type::int64;

    crpc::db::result_set* rows = nullptr;
    const crpc::status called = h.session->call(
        "CALL bump(?)", crpc::span<parameter>(params.data(), params.size()), &rows);
    CRPC_CHECK(called.ok());
    if (!called.ok()) {
        return;
    }
    delete rows;

    // The INOUT value is seeded before the call, then the call carries no binds.
    CRPC_CHECK_EQ(std::string("SET @crpc_out_0 = ?"), h.raw->text_of(1));
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::mysql::com_stmt_execute),
                  static_cast<int>(h.raw->command_of(2)));
    // The second prepare is the CALL itself.
    std::size_t call_prepare = 0;
    std::size_t prepares = 0;
    for (std::size_t i = 0; i < h.raw->writes.size(); ++i) {
        if (h.raw->command_of(i) == static_cast<std::uint8_t>(crpc::db::mysql::com_stmt_prepare)) {
            ++prepares;
            call_prepare = i;
        }
    }
    CRPC_CHECK_EQ(std::size_t{2}, prepares);
    CRPC_CHECK_EQ(std::string("CALL bump(@crpc_out_0)"), h.raw->text_of(call_prepare));

    CRPC_CHECK_FALSE(params[0].is_null);
    CRPC_CHECK_EQ(static_cast<std::int64_t>(8), params[0].int_value);
}

CRPC_TEST(mysql_call_does_not_rewrite_placeholders_inside_literals) {
    harness h;
    // CALL p('a?b', ?) has one real placeholder; the '?' inside the literal must
    // stay untouched, or the procedure receives different SQL than the caller wrote.
    h.raw->push(packet(prepare_ok_payload(81, 0, 1), 1));
    h.raw->push(packet(column_payload("p1", 8), 2));
    h.raw->push(packet(eof_payload(2), 3));
    h.raw->push(packet(ok_payload(0x0008), 1));
    h.raw->push(packet(ok_payload(0x0002), 2));

    std::vector<parameter> params;
    params.push_back(parameter::from_int64(3));
    crpc::db::result_set* rows = nullptr;
    const crpc::status called = h.session->call(
        "CALL p('a?b', ?)", crpc::span<parameter>(params.data(), params.size()), &rows);
    CRPC_CHECK(called.ok());
    if (called.ok()) {
        delete rows;
    }
    CRPC_CHECK_EQ(std::string("CALL p('a?b', ?)"), h.raw->text_of(1));
}

CRPC_TEST(mysql_call_rejects_a_placeholder_count_mismatch) {
    harness h;
    std::vector<parameter> params;
    params.push_back(parameter::from_int64(1));

    crpc::db::result_set* rows = nullptr;
    const crpc::status too_many = h.session->call(
        "CALL p(?, ?)", crpc::span<parameter>(params.data(), params.size()), &rows);
    CRPC_CHECK_FALSE(too_many.ok());
    if (!too_many.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(too_many.code()));
    }
    CRPC_CHECK_EQ(std::size_t{1}, h.raw->writes.size()); // only the handshake response

    const crpc::status too_few = h.session->call(
        "CALL p()", crpc::span<parameter>(params.data(), params.size()), &rows);
    CRPC_CHECK_FALSE(too_few.ok());
    CRPC_CHECK_EQ(std::size_t{1}, h.raw->writes.size());
}

CRPC_TEST(mysql_call_returns_the_procedure_result_set) {
    harness h;
    h.raw->push(packet(prepare_ok_payload(91, 1, 0), 1));
    h.raw->push(packet(column_payload("n", 8), 2));
    h.raw->push(packet(eof_payload(2), 3));
    // execute: one binary column, one row (n = 5), then the trailing OK.
    h.raw->push(packet({std::byte{1}}, 1));
    h.raw->push(packet(column_payload("n", 8), 2));
    h.raw->push(packet(eof_payload(2), 3));
    std::vector<std::byte> row;
    row.push_back(std::byte{0});
    row.push_back(std::byte{0});
    for (int i = 0; i < 8; ++i) {
        row.push_back(static_cast<std::byte>(i == 0 ? 5 : 0));
    }
    h.raw->push(packet(row, 4));
    h.raw->push(packet(eof_payload(0x0008), 5));
    h.raw->push(packet(ok_payload(0x0002), 6));

    std::vector<parameter> params;
    crpc::db::result_set* rows = nullptr;
    const crpc::status called = h.session->call(
        "CALL list()", crpc::span<parameter>(params.data(), params.size()), &rows);
    CRPC_CHECK(called.ok());
    if (!called.ok()) {
        return;
    }
    std::unique_ptr<crpc::db::result_set> guard(rows);
    CRPC_CHECK(guard != nullptr);
    if (guard != nullptr) {
        CRPC_CHECK(guard->next());
        CRPC_CHECK_EQ(static_cast<std::int64_t>(5), guard->get_int64(0).value());
        CRPC_CHECK_FALSE(guard->next());
    }
}

CRPC_TEST_MAIN()