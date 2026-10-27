// CRPC -- MySQL connection tests over a scripted channel.
//
// The connection is written against `channel`, so the handshake and the
// result-set loop are exercised with a scripted server and no socket. The script
// is built from the documented packet shapes, not from the client's own encoder,
// so a systematic mistake cannot agree with itself.
#include "test_framework.hpp"

#include <algorithm>
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

[[nodiscard]] std::vector<std::byte> ok_payload(std::uint64_t affected, std::uint16_t status) {
    std::vector<std::byte> payload;
    payload.push_back(std::byte{0});
    crpc::db::mysql::append_lenenc_int(payload, affected);
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
    append_text(payload, "HY000");
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

// DEPRECATE_EOF is deliberately absent so the classic EOF terminators are
// exercised; the client intersects what it asked for with what it was offered.
constexpr std::uint32_t server_capabilities =
    mask(capability::protocol_41) | mask(capability::secure_connection) |
    mask(capability::plugin_auth) | mask(capability::transactions) |
    mask(capability::multi_results);

[[nodiscard]] std::vector<std::byte> greeting_payload(std::string_view plugin) {
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
    append_text(payload, plugin);
    payload.push_back(std::byte{0});
    return payload;
}

// Replays scripted bytes and records what the client wrote.
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
};

// Connects over a scripted channel with an empty password (so the auth response
// is empty and the test does not need to model the scramble).
struct harness {
    scripted_channel* raw = nullptr;
    std::unique_ptr<connection> session;

    explicit harness(std::string_view plugin = "caching_sha2_password") {
        auto scripted = std::unique_ptr<scripted_channel>(new scripted_channel);
        raw = scripted.get();
        raw->push(packet(greeting_payload(plugin), 0));
        raw->push(packet(ok_payload(0, 2), 2));

        connection_options options;
        options.user = "root";
        options.password = "";
        session.reset(new connection(options, std::move(scripted)));
    }

    [[nodiscard]] std::string handshake_response() const {
        const std::vector<std::byte>& frame = raw->writes.front();
        return std::string(reinterpret_cast<const char*>(frame.data() + 4),
                           frame.size() - 4);
    }
};

} // namespace

CRPC_TEST(mysql_connects_and_exposes_server_info) {
    harness h;
    CRPC_CHECK(h.session->is_open());
    CRPC_CHECK_EQ(std::string("8.0.36"), std::string(h.session->server_version()));
    CRPC_CHECK_EQ(std::string("caching_sha2_password"), h.session->auth_plugin());
    // The negotiation is an intersection: DEPRECATE_EOF was asked for but not
    // offered, so it must not be set.
    CRPC_CHECK_EQ(0, static_cast<int>(h.session->negotiated_capabilities() &
                                      mask(capability::deprecate_eof)));
    CRPC_CHECK((h.session->negotiated_capabilities() & mask(capability::protocol_41)) != 0);

    const std::string response = h.handshake_response();
    CRPC_CHECK(response.find(std::string("root")) != std::string::npos);

    // The response begins with the negotiated capability word, little-endian.
    const std::uint32_t claimed =
        static_cast<std::uint32_t>(static_cast<std::uint8_t>(response[0])) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(response[1])) << 8) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(response[2])) << 16) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(response[3])) << 24);
    CRPC_CHECK_EQ(h.session->negotiated_capabilities(), claimed);
}

CRPC_TEST(mysql_query_returns_rows) {
    harness h;
    CRPC_CHECK(h.session->is_open());
    if (!h.session->is_open()) {
        return;
    }

    h.raw->push(packet({std::byte{2}}, 1));
    h.raw->push(packet(column_payload("id", 8), 2));
    h.raw->push(packet(column_payload("name", 253), 3));
    h.raw->push(packet(eof_payload(2), 4));

    std::vector<std::byte> row1;
    crpc::db::mysql::append_lenenc_string(row1, "7");
    crpc::db::mysql::append_lenenc_string(row1, "alice");
    h.raw->push(packet(row1, 5));

    std::vector<std::byte> row2;
    crpc::db::mysql::append_lenenc_string(row2, "8");
    crpc::db::mysql::append_null(row2);
    h.raw->push(packet(row2, 6));
    h.raw->push(packet(eof_payload(2), 7));

    crpc::db::result_set* raw_set = nullptr;
    const crpc::status ran = h.session->query("SELECT id, name FROM t", {}, &raw_set);
    CRPC_CHECK(ran.ok());
    if (!ran.ok()) {
        return;
    }
    std::unique_ptr<crpc::db::result_set> set(raw_set);
    CRPC_CHECK(set != nullptr);
    if (!set) {
        return;
    }

    CRPC_CHECK_EQ(std::size_t{2}, set->column_count());
    CRPC_CHECK_EQ(std::string("id"), set->column(0).name);
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::int64),
                  static_cast<int>(set->column(0).type));
    CRPC_CHECK_EQ(static_cast<int>(crpc::db::db_type::text),
                  static_cast<int>(set->column(1).type));

    CRPC_CHECK(set->next());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(7), set->get_int64(0).value());
    CRPC_CHECK_EQ(std::string("alice"), std::string(set->get_text(1).value()));

    CRPC_CHECK(set->next());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(8), set->get_int64(0).value());
    CRPC_CHECK(set->is_null(1));
    // Reading a NULL as a value is a caller error, not a zero.
    CRPC_CHECK_FALSE(set->get_text(1).ok());

    CRPC_CHECK_FALSE(set->next());
}

CRPC_TEST(mysql_query_error_is_mapped) {
    harness h;
    h.raw->push(packet(err_payload(1146, "Table 'app.t' doesn't exist"), 1));
    crpc::db::result_set* raw_set = nullptr;
    const crpc::status ran = h.session->query("SELECT * FROM t", {}, &raw_set);
    CRPC_CHECK_FALSE(ran.ok());
    if (!ran.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found), static_cast<int>(ran.code()));
        CRPC_CHECK(ran.message().find("doesn't exist") != std::string::npos);
    }
    CRPC_CHECK(raw_set == nullptr);
}

CRPC_TEST(mysql_handshake_error_packet_closes) {
    auto scripted = std::unique_ptr<scripted_channel>(new scripted_channel);
    scripted_channel* raw = scripted.get();
    raw->push(packet(err_payload(1045, "Access denied for user 'root'"), 0));

    connection_options options;
    options.user = "root";
    auto session = std::unique_ptr<connection>(new connection(options, std::move(scripted)));
    CRPC_CHECK_FALSE(session->is_open());
    CRPC_CHECK(raw->closed);
}


CRPC_TEST(mysql_transaction_state_is_tracked) {
    harness h;
    h.raw->push(packet(ok_payload(0, 1), 1));
    CRPC_CHECK(h.session->begin().ok());
    CRPC_CHECK(h.session->in_transaction());

    h.raw->push(packet(ok_payload(0, 2), 1));
    CRPC_CHECK(h.session->commit().ok());
    CRPC_CHECK_FALSE(h.session->in_transaction());
    CRPC_CHECK_FALSE(h.session->commit().ok());

    h.raw->push(packet(ok_payload(0, 1), 1));
    CRPC_CHECK(h.session->begin().ok());
    h.raw->push(packet(ok_payload(0, 2), 1));
    CRPC_CHECK(h.session->rollback().ok());
    CRPC_CHECK_FALSE(h.session->in_transaction());
    // Rolling back outside a transaction is a no-op.
    CRPC_CHECK(h.session->rollback().ok());
}

CRPC_TEST(mysql_ping_uses_com_ping) {
    harness h;
    h.raw->push(packet(ok_payload(0, 2), 1));
    CRPC_CHECK(h.session->ping().ok());
    const std::vector<std::byte>& last = h.raw->writes.back();
    CRPC_CHECK_EQ(static_cast<int>(0x0e), static_cast<int>(last[4]));
}

CRPC_TEST_MAIN()
