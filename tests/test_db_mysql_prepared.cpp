// CRPC -- MySQL prepared-statement lifecycle tests.
//
// The lifecycle is where the stream can silently desynchronise: a prepare reply is
// followed by parameter and column definitions that must be consumed by count, an
// execute reply carries binary rows, and COM_STMT_CLOSE has no reply at all — read
// one and the next command's answer is lost. Each case scripts the exact packet
// sequence and asserts both the decode and the number of reads.
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

void append_u64(std::vector<std::byte>& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
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

[[nodiscard]] std::vector<std::byte> err_payload(std::uint16_t code, std::string_view message) {
    std::vector<std::byte> payload;
    payload.push_back(std::byte{0xff});
    append_u16(payload, code);
    payload.push_back(std::byte{'#'});
    append_text(payload, "42000");
    append_text(payload, message);
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

[[nodiscard]] std::vector<std::byte> column_payload(std::string_view name, std::uint8_t type,
                                                    std::uint16_t flags) {
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
    append_u16(payload, flags);
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
        // writes[i] = 4-byte header + payload; the command is the first payload byte.
        return static_cast<std::uint8_t>(writes[index][4]);
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

    // One parameter definition and one column definition, each closed by an EOF.
    void script_prepare(std::uint32_t id) {
        raw->push(packet(prepare_ok_payload(id, 1, 1), 1));
        raw->push(packet(column_payload("p1", 8, 0), 2)); // param definition
        raw->push(packet(eof_payload(2), 3));
        raw->push(packet(column_payload("id", 8, 0), 4)); // column definition
        raw->push(packet(eof_payload(2), 5));
    }

    // One column, one binary row (id = 9), then EOF.
    void script_execute(std::uint64_t value) {
        raw->push(packet({std::byte{1}}, 1)); // column count
        raw->push(packet(column_payload("id", 8, 0), 2));
        raw->push(packet(eof_payload(2), 3));
        std::vector<std::byte> row;
        row.push_back(std::byte{0}); // binary row header
        row.push_back(std::byte{0}); // null bitmap: one byte, no nulls
        append_u64(row, value);
        raw->push(packet(row, 4));
        raw->push(packet(eof_payload(2), 5));
    }
};

} // namespace

CRPC_TEST(mysql_prepare_reads_definitions_by_count) {
    harness h;
    h.script_prepare(11);

    auto prepared = h.session->prepare("SELECT id FROM t WHERE id = ?");
    CRPC_CHECK(prepared.ok());
    if (!prepared.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::uint32_t{11}, prepared.value().id());
    CRPC_CHECK_EQ(std::size_t{1}, prepared.value().param_count());
    CRPC_CHECK_EQ(std::size_t{1}, prepared.value().column_count());
    CRPC_CHECK(prepared.value().valid());

    // The prepare command carried the SQL text.
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::mysql::com_stmt_prepare),
                  static_cast<int>(h.raw->command_of(1)));
    const std::vector<std::byte>& payload = h.raw->writes[1];
    const std::string sql(reinterpret_cast<const char*>(payload.data() + 5),
                          payload.size() - 5);
    CRPC_CHECK_EQ(std::string("SELECT id FROM t WHERE id = ?"), sql);

    // Closing sends COM_STMT_CLOSE and reads nothing: the reply script is empty now,
    // and reading would return EOF and kill the session.
    prepared.value().close();
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::mysql::com_stmt_close),
                  static_cast<int>(h.raw->command_of(2)));
    CRPC_CHECK(h.session->is_open());
}

CRPC_TEST(mysql_prepare_error_is_mapped) {
    harness h;
    h.raw->push(packet(err_payload(1146, "Table 'app.t' doesn't exist"), 1));
    auto prepared = h.session->prepare("SELECT * FROM t");
    CRPC_CHECK_FALSE(prepared.ok());
    if (!prepared.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                      static_cast<int>(prepared.error().code));
    }
}

CRPC_TEST(mysql_execute_returns_binary_rows) {
    harness h;
    h.script_prepare(21);
    auto prepared = h.session->prepare("SELECT id FROM t WHERE id = ?");
    CRPC_CHECK(prepared.ok());
    if (!prepared.ok()) {
        return;
    }
    h.script_execute(9);

    std::vector<parameter> params;
    params.push_back(parameter::from_int64(9));
    auto executed = prepared.value().execute(
        crpc::span<const parameter>(params.data(), params.size()));
    CRPC_CHECK(executed.ok());
    if (!executed.ok()) {
        return;
    }
    std::unique_ptr<crpc::db::result_set> set(executed.value());
    CRPC_CHECK(set != nullptr);
    if (!set) {
        return;
    }
    CRPC_CHECK_EQ(std::size_t{1}, set->column_count());
    CRPC_CHECK_EQ(std::string("id"), set->column(0).name);
    CRPC_CHECK(set->next());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(9), set->get_int64(0).value());
    CRPC_CHECK_FALSE(set->next());

    // The execute command carried the statement id and the parameter.
    const std::vector<std::byte>& command = h.raw->writes[2];
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::mysql::com_stmt_execute),
                  static_cast<int>(command[4]));
    CRPC_CHECK_EQ(21, static_cast<int>(command[5] | (command[6] << 8)));
}

CRPC_TEST(mysql_execute_rejects_a_parameter_count_mismatch) {
    harness h;
    h.script_prepare(31);
    auto prepared = h.session->prepare("SELECT ?");
    CRPC_CHECK(prepared.ok());
    if (!prepared.ok()) {
        return;
    }
    // The statement expects one parameter; binding two would shift every value.
    std::vector<parameter> params;
    params.push_back(parameter::from_int64(1));
    params.push_back(parameter::from_int64(2));
    auto executed = prepared.value().execute(
        crpc::span<const parameter>(params.data(), params.size()));
    CRPC_CHECK_FALSE(executed.ok());
    if (!executed.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(executed.error().code));
    }
    // Nothing was written for the rejected execute.
    CRPC_CHECK_EQ(std::size_t{2}, h.raw->writes.size());
}

CRPC_TEST(mysql_query_with_parameters_prepares_executes_and_closes) {
    harness h;
    h.script_prepare(41);
    h.script_execute(5);

    std::vector<parameter> params;
    params.push_back(parameter::from_int64(5));
    crpc::db::result_set* raw_set = nullptr;
    const crpc::status ran = h.session->query(
        "SELECT id FROM t WHERE id = ?",
        crpc::span<const parameter>(params.data(), params.size()), &raw_set);
    CRPC_CHECK(ran.ok());
    if (!ran.ok()) {
        return;
    }
    std::unique_ptr<crpc::db::result_set> set(raw_set);
    CRPC_CHECK(set != nullptr);
    if (set != nullptr) {
        CRPC_CHECK(set->next());
        CRPC_CHECK_EQ(static_cast<std::int64_t>(5), set->get_int64(0).value());
    }

    // prepare, execute, close — in that order, so a pooled connection does not
    // accumulate statements.
    CRPC_CHECK_EQ(std::size_t{4}, h.raw->writes.size());
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::mysql::com_stmt_prepare),
                  static_cast<int>(h.raw->command_of(1)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::mysql::com_stmt_execute),
                  static_cast<int>(h.raw->command_of(2)));
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::mysql::com_stmt_close),
                  static_cast<int>(h.raw->command_of(3)));
    CRPC_CHECK(h.session->is_open());
}

CRPC_TEST(mysql_statement_outliving_the_connection_is_safe) {
    crpc::db::mysql::prepared_statement escaped;
    {
        harness h;
        h.script_prepare(51);
        auto prepared = h.session->prepare("SELECT 1");
        CRPC_CHECK(prepared.ok());
        if (!prepared.ok()) {
            return;
        }
        escaped = std::move(prepared.value());
        h.session->close();
    }
    // The session is gone; the handle must not dereference it or send a close on a
    // dead link.
    CRPC_CHECK_FALSE(escaped.valid() && escaped.id() != 51);
    escaped.close();
    CRPC_CHECK_FALSE(escaped.valid());
    escaped.close(); // idempotent
}

CRPC_TEST_MAIN()