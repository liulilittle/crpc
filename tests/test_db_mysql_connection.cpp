// CRPC -- MySQL connection tests over a scripted channel.
//
// The connection is written against `channel`, so the handshake and the
// result-set loop are exercised with a scripted server and no socket. The script
// is built from the documented packet shapes, not from the client's own encoder,
// so a systematic mistake cannot agree with itself.
#include "rsa_test_support.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/db/connection.hpp>
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

// DEPRECATE_EOF is deliberately absent so the classic EOF terminators are
// exercised; the client intersects what it asked for with what it was offered.
constexpr std::uint32_t server_capabilities =
    mask(capability::protocol_41) | mask(capability::secure_connection) |
    mask(capability::plugin_auth) | mask(capability::transactions) |
    mask(capability::multi_results);

[[nodiscard]] std::vector<std::byte> greeting_payload(std::string_view plugin,
                                                     std::uint32_t server_caps = server_capabilities) {
    std::vector<std::byte> payload;
    payload.push_back(std::byte{10});
    append_text(payload, "8.0.36");
    payload.push_back(std::byte{0});
    append_u32(payload, 42);
    for (int i = 0; i < 8; ++i) {
        payload.push_back(static_cast<std::byte>(i + 1));
    }
    payload.push_back(std::byte{0});
    append_u16(payload, static_cast<std::uint16_t>(server_caps & 0xffffu));
    payload.push_back(std::byte{45});
    append_u16(payload, 2);
    append_u16(payload, static_cast<std::uint16_t>((server_caps >> 16) & 0xffffu));
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
    // True models a link that protects bytes end to end (established TLS, or a
    // Unix-domain socket); it drives the cleartext-password branch of
    // caching_sha2 full authentication.
    bool secure = false;

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

    [[nodiscard]] bool is_secure() const noexcept override { return secure; }

    void push(std::vector<std::byte> data) { incoming.push_back(std::move(data)); }
};

// Connects over a scripted channel with an empty password (so the auth response
// is empty and the test does not need to model the scramble).
struct harness {
    scripted_channel* raw = nullptr;
    std::unique_ptr<connection> session;

    // `handshake_status` is the server-status word on the authentication OK. The
    // default (0x0002, autocommit on) is what the other cases assume; a session
    // started with `--autocommit=0` reports 0x0000 instead. `server_caps` narrows
    // or widens what the scripted server offers, so a case can negotiate
    // DEPRECATE_EOF (the default greeting withholds it).
    explicit harness(std::string_view plugin = "caching_sha2_password",
                     std::uint16_t handshake_status = 2,
                     std::uint32_t server_caps = server_capabilities) {
        auto scripted = std::unique_ptr<scripted_channel>(new scripted_channel);
        raw = scripted.get();
        raw->push(packet(greeting_payload(plugin, server_caps), 0));
        raw->push(packet(ok_payload(0, handshake_status), 2));

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

// --- caching_sha2_password full authentication -------------------------------
//
// The server answers the fast-auth scramble with AuthMoreData packets: 0x03
// (fast auth succeeded, OK follows), 0x04 (full authentication: the server's
// password cache was cold), or 0x01 || PEM (the public key answering our 0x02
// request). Over a link that protects bytes end to end the cleartext password
// plus its NUL terminator may be sent directly; anywhere else it must be
// RSA-OAEP-SHA1 encrypted under the server key.

// The RSA test key: the official RSA Labs PKCS#1 v2.1 OAEP example key
// (public data, published in 2001; see rsa_test_support.hpp for provenance).
constexpr std::size_t full_auth_key_bytes = 128; // 1024-bit modulus

// The greeting nonce the scripted server announces: bytes 1..20 (see
// greeting_payload: part 1 is 1..8, part 2 is 9..20).
[[nodiscard]] std::vector<std::byte> greeting_nonce() {
    std::vector<std::byte> nonce;
    for (int i = 1; i <= 20; ++i) {
        nonce.push_back(static_cast<std::byte>(i));
    }
    return nonce;
}

[[nodiscard]] std::vector<std::byte> auth_more_data(std::byte marker) {
    return {std::byte{0x01}, marker};
}

// The reply to a 0x02 public-key request: 0x01 followed by the PEM text.
[[nodiscard]] std::vector<std::byte> public_key_reply(std::string_view pem) {
    std::vector<std::byte> payload;
    payload.push_back(std::byte{0x01});
    for (const char c : pem) {
        payload.push_back(static_cast<std::byte>(c));
    }
    return payload;
}

[[nodiscard]] std::vector<std::byte> frame_payload(const std::vector<std::byte>& frame) {
    return std::vector<std::byte>(frame.begin() + 4, frame.end());
}

// The SQL text of a COM_QUERY frame (the payload's first byte is the command).
[[nodiscard]] std::string frame_sql(const std::vector<std::byte>& frame) {
    const std::vector<std::byte> payload = frame_payload(frame);
    if (payload.empty() || payload[0] != std::byte{0x03}) {
        return std::string();
    }
    return std::string(reinterpret_cast<const char*>(payload.data() + 1), payload.size() - 1);
}

// Queues one OK reply for the next command (a command's replies start at
// sequence 1 because the client reset its sequence to 0 when it sent it).
void push_ok(scripted_channel* raw, std::uint64_t affected, std::uint16_t status,
             std::uint8_t sequence = 1) {
    raw->push(packet(ok_payload(affected, status), sequence));
}

// (password || NUL) XOR cyclic scramble -- the full-auth RSA message.
[[nodiscard]] std::vector<std::byte> expected_rsa_message(std::string_view password,
                                                          const std::vector<std::byte>& nonce) {
    std::vector<std::byte> message(password.size() + 1, std::byte{0});
    for (std::size_t i = 0; i < password.size(); ++i) {
        message[i] = static_cast<std::byte>(password[i]);
    }
    for (std::size_t i = 0; i < message.size(); ++i) {
        message[i] = message[i] ^ nonce[i % nonce.size()];
    }
    return message;
}

CRPC_TEST(mysql_full_auth_sends_cleartext_password_on_a_secure_channel) {
    auto scripted = std::unique_ptr<scripted_channel>(new scripted_channel);
    scripted_channel* raw = scripted.get();
    raw->secure = true;
    raw->push(packet(greeting_payload("caching_sha2_password"), 0));
    raw->push(packet(auth_more_data(std::byte{0x04}), 2));
    raw->push(packet(ok_payload(0, 2), 4));

    connection_options options;
    options.user = "root";
    options.password = "s3cret";
    auto session = std::unique_ptr<connection>(
        new connection(options, std::move(scripted)));
    CRPC_CHECK(session->is_open());
    // writes[0] is the handshake response; writes[1] must be the password with
    // its NUL terminator, unencrypted, on the protected link.
    CRPC_CHECK_EQ(std::size_t{2}, raw->writes.size());
    if (raw->writes.size() == 2) {
        const std::vector<std::byte> payload = frame_payload(raw->writes[1]);
        CRPC_CHECK_EQ(std::size_t{7}, payload.size());
        // `s3cret` followed by the NUL terminator, in the clear.
        const std::string expected = "s3cret";
        bool matches = payload.size() == expected.size() + 1;
        for (std::size_t i = 0; matches && i < expected.size(); ++i) {
            matches = payload[i] == static_cast<std::byte>(static_cast<unsigned char>(expected[i]));
        }
        CRPC_CHECK(matches);
        CRPC_CHECK(payload[6] == std::byte{0});
    }
    CRPC_CHECK(session->used_full_authentication());
}

CRPC_TEST(mysql_full_auth_requests_public_key_on_a_plaintext_channel) {
    auto scripted = std::unique_ptr<scripted_channel>(new scripted_channel);
    scripted_channel* raw = scripted.get();
    raw->push(packet(greeting_payload("caching_sha2_password"), 0));
    raw->push(packet(auth_more_data(std::byte{0x04}), 2));
    raw->push(packet(public_key_reply(rsa_test::vector_pem), 4));
    raw->push(packet(ok_payload(0, 2), 6));

    connection_options options;
    options.user = "root";
    options.password = "s3cret";
    auto session = std::unique_ptr<connection>(
        new connection(options, std::move(scripted)));
    CRPC_CHECK(session->is_open());
    CRPC_CHECK_EQ(std::size_t{3}, raw->writes.size());
    if (raw->writes.size() != 3) {
        return;
    }
    // First the one-byte public-key request.
    const std::vector<std::byte> request = frame_payload(raw->writes[1]);
    CRPC_CHECK_EQ(std::size_t{1}, request.size());
    CRPC_CHECK(request[0] == std::byte{0x02});
    // Then a ciphertext the size of the modulus, which must decrypt to
    // (password || NUL) XOR scramble under the test key.
    const std::vector<std::byte> cipher = frame_payload(raw->writes[2]);
    CRPC_CHECK_EQ(full_auth_key_bytes, cipher.size());
    const std::vector<std::byte> em = rsa_test::reference_decrypt(cipher);
    std::vector<std::byte> message;
    CRPC_CHECK(rsa_test::reference_oaep_decode(em, message));
    CRPC_CHECK(expected_rsa_message("s3cret", greeting_nonce()) == message);
    CRPC_CHECK(session->used_full_authentication());
}

CRPC_TEST(mysql_fast_auth_more_data_zero_three_is_unchanged) {
    auto scripted = std::unique_ptr<scripted_channel>(new scripted_channel);
    scripted_channel* raw = scripted.get();
    raw->push(packet(greeting_payload("caching_sha2_password"), 0));
    raw->push(packet(auth_more_data(std::byte{0x03}), 2));
    raw->push(packet(ok_payload(0, 2), 3));

    connection_options options;
    options.user = "root";
    options.password = "s3cret";
    auto session = std::unique_ptr<connection>(
        new connection(options, std::move(scripted)));
    CRPC_CHECK(session->is_open());
    // Fast auth (cache hit): one write only, no full-auth exchange.
    CRPC_CHECK_EQ(std::size_t{1}, raw->writes.size());
    CRPC_CHECK_FALSE(session->used_full_authentication());
}

CRPC_TEST(mysql_full_auth_uses_the_auth_switch_nonce_without_trailing_nuls) {
    auto scripted = std::unique_ptr<scripted_channel>(new scripted_channel);
    scripted_channel* raw = scripted.get();
    raw->push(packet(greeting_payload("caching_sha2_password"), 0));
    // Auth switch to the same plugin with a fresh challenge whose wire form
    // carries two trailing NULs; only the 20 bytes before them may be used.
    std::vector<std::byte> switch_payload;
    switch_payload.push_back(std::byte{0xfe});
    append_text(switch_payload, "caching_sha2_password");
    switch_payload.push_back(std::byte{0});
    for (int i = 0; i < 20; ++i) {
        switch_payload.push_back(static_cast<std::byte>(0x41 + i));
    }
    switch_payload.push_back(std::byte{0});
    switch_payload.push_back(std::byte{0});
    raw->push(packet(switch_payload, 2));
    raw->push(packet(auth_more_data(std::byte{0x04}), 4));
    raw->push(packet(public_key_reply(rsa_test::vector_pem), 6));
    raw->push(packet(ok_payload(0, 2), 8));

    connection_options options;
    options.user = "root";
    options.password = "s3cret";
    auto session = std::unique_ptr<connection>(
        new connection(options, std::move(scripted)));
    CRPC_CHECK(session->is_open());
    CRPC_CHECK_EQ(std::string("caching_sha2_password"), session->auth_plugin());
    // writes: handshake response, auth-switch response, 0x02 request, ciphertext.
    CRPC_CHECK_EQ(std::size_t{4}, raw->writes.size());
    if (raw->writes.size() != 4) {
        return;
    }
    const std::vector<std::byte> request = frame_payload(raw->writes[2]);
    CRPC_CHECK_EQ(std::size_t{1}, request.size());
    CRPC_CHECK(request[0] == std::byte{0x02});
    const std::vector<std::byte> cipher = frame_payload(raw->writes[3]);
    CRPC_CHECK_EQ(full_auth_key_bytes, cipher.size());
    const std::vector<std::byte> em = rsa_test::reference_decrypt(cipher);
    std::vector<std::byte> message;
    CRPC_CHECK(rsa_test::reference_oaep_decode(em, message));
    std::vector<std::byte> switch_nonce;
    for (int i = 0; i < 20; ++i) {
        switch_nonce.push_back(static_cast<std::byte>(0x41 + i));
    }
    CRPC_CHECK(expected_rsa_message("s3cret", switch_nonce) == message);
    CRPC_CHECK(session->used_full_authentication());
}

CRPC_TEST(mysql_full_auth_refuses_a_password_too_long_for_the_key) {
    auto scripted = std::unique_ptr<scripted_channel>(new scripted_channel);
    scripted_channel* raw = scripted.get();
    raw->push(packet(greeting_payload("caching_sha2_password"), 0));
    raw->push(packet(auth_more_data(std::byte{0x04}), 2));
    raw->push(packet(public_key_reply(rsa_test::vector_pem), 4));

    connection_options options;
    options.user = "root";
    // 200 password bytes + NUL exceed RSAES-OAEP's bound for a 1024-bit key
    // (k - 2*20 - 2 = 86); the failure must be a clear error, never silence.
    options.password = std::string(200, 'x');
    auto session = std::unique_ptr<connection>(
        new connection(options, std::move(scripted)));
    CRPC_CHECK_FALSE(session->is_open());
    CRPC_CHECK(session->connect_error().message.find("too long") != std::string::npos);
    CRPC_CHECK(raw->closed);
}

CRPC_TEST(mysql_full_auth_refuses_a_malformed_public_key) {
    auto scripted = std::unique_ptr<scripted_channel>(new scripted_channel);
    scripted_channel* raw = scripted.get();
    raw->push(packet(greeting_payload("caching_sha2_password"), 0));
    raw->push(packet(auth_more_data(std::byte{0x04}), 2));
    raw->push(packet(public_key_reply("not a pem"), 4));

    connection_options options;
    options.user = "root";
    options.password = "s3cret";
    auto session = std::unique_ptr<connection>(
        new connection(options, std::move(scripted)));
    CRPC_CHECK_FALSE(session->is_open());
    CRPC_CHECK(session->connect_error().message.find("PUBLIC KEY") != std::string::npos);
    CRPC_CHECK(raw->closed);
}

// --- transaction state comes from the server's status word -------------------
//
// `in_transaction()` must answer "is there work a COMMIT or ROLLBACK has to
// finish?", which is *not* the same as "did the caller call begin()": with
// autocommit=0 an INSERT opens a transaction the caller never asked for. The
// cases below pin both projections: the latch of our own begin() and the
// IN_TRANS bit of the latest status word.

// Autocommit off: the handshake reports no transaction, the INSERT's OK opens
// one. The caller runs `begin()` afterwards and must still be able to commit.
CRPC_TEST(mysql_autocommit_off_insert_begin_commit_writes_commit) {
    harness h("caching_sha2_password", 0x0000);
    crpc::db::result_set* inserted = nullptr;
    push_ok(h.raw, 1, 0x0001);
    const crpc::status ran = h.session->query("INSERT INTO t VALUES (1)", {}, &inserted);
    std::unique_ptr<crpc::db::result_set> owned(inserted);
    CRPC_CHECK(ran.ok());

    push_ok(h.raw, 0, 0x0001);
    CRPC_CHECK(h.session->begin().ok());
    CRPC_CHECK(h.session->in_transaction());

    push_ok(h.raw, 0, 0x0000);
    CRPC_CHECK(h.session->commit().ok());
    CRPC_CHECK_EQ(std::string("COMMIT"), frame_sql(h.raw->writes.back()));
}

CRPC_TEST(mysql_autocommit_off_insert_reports_in_transaction) {
    harness h("caching_sha2_password", 0x0000);
    crpc::db::result_set* inserted = nullptr;
    push_ok(h.raw, 1, 0x0001);
    const crpc::status ran = h.session->query("INSERT INTO t VALUES (1)", {}, &inserted);
    std::unique_ptr<crpc::db::result_set> owned(inserted);
    CRPC_CHECK(ran.ok());
    // The pool's P3 decision reads this: an uncommitted INSERT must not look
    // like a reusable connection.
    CRPC_CHECK(h.session->in_transaction());
}

CRPC_TEST(mysql_autocommit_off_insert_commit_succeeds) {
    harness h("caching_sha2_password", 0x0000);
    crpc::db::result_set* inserted = nullptr;
    push_ok(h.raw, 1, 0x0001);
    const crpc::status ran = h.session->query("INSERT INTO t VALUES (1)", {}, &inserted);
    std::unique_ptr<crpc::db::result_set> owned(inserted);
    CRPC_CHECK(ran.ok());

    push_ok(h.raw, 0, 0x0000);
    const crpc::status committed = h.session->commit();
    CRPC_CHECK(committed.ok());
    CRPC_CHECK_EQ(std::string("COMMIT"), frame_sql(h.raw->writes.back()));
}

CRPC_TEST(mysql_ddl_implicit_commit_ends_the_transaction) {
    harness h;
    push_ok(h.raw, 0, 0x0001);
    CRPC_CHECK(h.session->begin().ok());
    CRPC_CHECK(h.session->in_transaction());

    // DDL implicitly commits server-side; the status word says so.
    crpc::db::result_set* created = nullptr;
    push_ok(h.raw, 0, 0x0000);
    CRPC_CHECK(h.session->query("CREATE TABLE t (a INT)", {}, &created).ok());
    std::unique_ptr<crpc::db::result_set> owned(created);

    CRPC_CHECK_FALSE(h.session->in_transaction());
    const std::size_t before = h.raw->writes.size();
    const crpc::status committed = h.session->commit();
    CRPC_CHECK_FALSE(committed.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(committed.code()));
    // No phantom COMMIT frame for a transaction that already ended.
    CRPC_CHECK_EQ(before, h.raw->writes.size());
}

CRPC_TEST(mysql_empty_transaction_commits) {
    harness h;
    push_ok(h.raw, 0, 0x0001);
    CRPC_CHECK(h.session->begin().ok());
    // No statement in between: the latch alone must keep the transaction open.
    push_ok(h.raw, 0, 0x0000);
    CRPC_CHECK(h.session->commit().ok());
}

CRPC_TEST(mysql_short_eof_keeps_the_transaction_state) {
    harness h;
    push_ok(h.raw, 0, 0x0001);
    CRPC_CHECK(h.session->begin().ok());

    h.raw->push(packet({std::byte{1}}, 1));
    h.raw->push(packet(column_payload("id", 8), 2));
    h.raw->push(packet(eof_payload(0x0001), 3));
    // A one-byte 0xfe carries no status word.
    h.raw->push(packet({std::byte{0xfe}}, 4));

    crpc::db::result_set* rows = nullptr;
    CRPC_CHECK(h.session->query("SELECT id FROM t", {}, &rows).ok());
    std::unique_ptr<crpc::db::result_set> owned(rows);
    // A stale "in transaction" costs one harmless ROLLBACK; a wiped one costs
    // the rollback itself.
    CRPC_CHECK(h.session->in_transaction());
}

CRPC_TEST(mysql_full_eof_updates_the_transaction_state) {
    harness h;
    push_ok(h.raw, 0, 0x0001);
    CRPC_CHECK(h.session->begin().ok());

    h.raw->push(packet({std::byte{1}}, 1));
    h.raw->push(packet(column_payload("id", 8), 2));
    h.raw->push(packet(eof_payload(0x0001), 3));
    // The server says the transaction is over.
    h.raw->push(packet(eof_payload(0x0000), 4));

    crpc::db::result_set* rows = nullptr;
    CRPC_CHECK(h.session->query("SELECT id FROM t", {}, &rows).ok());
    std::unique_ptr<crpc::db::result_set> owned(rows);
    CRPC_CHECK_FALSE(h.session->in_transaction());
}

CRPC_TEST(mysql_query_ok_status_keeps_the_transaction_open) {
    harness h;
    push_ok(h.raw, 0, 0x0001);
    CRPC_CHECK(h.session->begin().ok());

    crpc::db::result_set* inserted = nullptr;
    push_ok(h.raw, 1, 0x0001);
    CRPC_CHECK(h.session->query("INSERT INTO t VALUES (2)", {}, &inserted).ok());
    std::unique_ptr<crpc::db::result_set> owned(inserted);
    CRPC_CHECK(h.session->in_transaction());
}

CRPC_TEST(mysql_begin_with_isolation_level_latches_after_run_query) {
    harness h;
    // SET TRANSACTION ...; START TRANSACTION rides in one COM_QUERY: the middle
    // chunk announces more results without IN_TRANS, the last one reports it.
    h.raw->push(packet(ok_payload(0, 0x0008), 1));
    h.raw->push(packet(ok_payload(0, 0x0001), 2));
    CRPC_CHECK(h.session->begin(crpc::db::isolation_level::repeatable_read).ok());
    CRPC_CHECK(h.session->in_transaction());
}

CRPC_TEST(mysql_begin_with_isolation_level_survives_a_clearing_last_chunk) {
    harness h;
    h.raw->push(packet(ok_payload(0, 0x0008), 1));
    // An anomalous last chunk with no IN_TRANS must not undo the latch that is
    // taken after `run_query` returns.
    h.raw->push(packet(ok_payload(0, 0x0000), 2));
    CRPC_CHECK(h.session->begin(crpc::db::isolation_level::repeatable_read).ok());
    CRPC_CHECK(h.session->in_transaction());
}

CRPC_TEST(mysql_middle_result_chunk_does_not_clear_the_explicit_transaction) {
    harness h;
    push_ok(h.raw, 0, 0x0001);
    CRPC_CHECK(h.session->begin().ok());

    // A multi-statement command: the first chunk is not the command's
    // conclusion, so its missing IN_TRANS must not clear the explicit latch.
    h.raw->push(packet(ok_payload(0, 0x0008), 1));
    h.raw->push(packet(ok_payload(0, 0x0001), 2));
    crpc::db::result_set* results = nullptr;
    CRPC_CHECK(h.session->query("SET @a = 1; START TRANSACTION", {}, &results).ok());
    std::unique_ptr<crpc::db::result_set> owned(results);

    CRPC_CHECK(h.session->in_transaction());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(h.session->begin().code()));
}

// These three pin the guards that keep a malformed terminator from overwriting the
// transaction state. Each one was written against a live mutation: delete the
// `.ok()` guard it names and the case goes red, because `apply_status(0)` clears
// both the IN_TRANS bit and the explicit latch.
CRPC_TEST(mysql_truncated_deprecate_eof_keeps_the_transaction_state) {
    harness h("caching_sha2_password", 2,
              server_capabilities | mask(capability::deprecate_eof));
    CRPC_CHECK((h.session->negotiated_capabilities() & mask(capability::deprecate_eof)) != 0);
    push_ok(h.raw, 0, 0x0001);
    CRPC_CHECK(h.session->begin().ok());
    CRPC_CHECK(h.session->in_transaction());

    // The terminator the packet shape suggests first: header plus a 0xfc prefix that
    // declares two more bytes and then stops. It is two bytes long, so the short-EOF
    // branch takes it; no status word is decoded and the previous one stands.
    h.raw->push(packet({std::byte{1}}, 1));
    h.raw->push(packet(column_payload("id", 8), 2));
    h.raw->push(packet({std::byte{0xfe}, std::byte{0xfc}}, 3));
    crpc::db::result_set* rows = nullptr;
    const crpc::status short_run = h.session->query("SELECT id FROM t", {}, &rows);
    std::unique_ptr<crpc::db::result_set> owned(rows);
    CRPC_CHECK(short_run.ok());
    CRPC_CHECK(h.session->in_transaction());

    // Nine bytes reach the DEPRECATE_EOF decoder itself (the short-EOF branch stops
    // at eight): header, then a lone 0xfb -- a NULL marker where an integer is
    // required -- then zero padding. Both length-encoded reads fail, so the four
    // bytes at the offset are not a status word and must not be applied.
    std::vector<std::byte> malformed;
    malformed.push_back(std::byte{0xfe});
    malformed.push_back(std::byte{0xfb});
    for (int i = 0; i < 7; ++i) {
        malformed.push_back(std::byte{0});
    }
    h.raw->push(packet({std::byte{1}}, 1));
    h.raw->push(packet(column_payload("id", 8), 2));
    h.raw->push(packet(malformed, 3));
    crpc::db::result_set* after = nullptr;
    const crpc::status long_run = h.session->query("SELECT id FROM t", {}, &after);
    std::unique_ptr<crpc::db::result_set> owned_after(after);
    CRPC_CHECK(long_run.ok());
    CRPC_CHECK(h.session->in_transaction());
}

CRPC_TEST(mysql_truncated_binary_trailing_ok_keeps_the_transaction_state) {
    harness h;
    push_ok(h.raw, 0, 0x0001);
    CRPC_CHECK(h.session->begin().ok());
    CRPC_CHECK(h.session->in_transaction());

    // `call()` is the public way to reach the binary result path with
    // `allow_more_results` set, which is the only shape that reads a trailing OK.
    // prepare "CALL p()": one result column, no parameters.
    h.raw->push(packet(prepare_ok_payload(31, 1, 0), 1));
    h.raw->push(packet(column_payload("id", 8), 2));
    h.raw->push(packet(eof_payload(2), 3));
    // execute: one binary column, one NULL row, an EOF announcing a trailing packet,
    // then that packet truncated to its 0x00 header, so `decode_ok` fails.
    h.raw->push(packet({std::byte{1}}, 1));
    h.raw->push(packet(column_payload("id", 8), 2));
    h.raw->push(packet(eof_payload(2), 3));
    h.raw->push(packet({std::byte{0x00}, std::byte{0x04}}, 4));
    h.raw->push(packet(eof_payload(0x0009), 5));
    h.raw->push(packet({std::byte{0x00}}, 6));

    crpc::db::result_set* rows = nullptr;
    const crpc::status called = h.session->call("CALL p()", {}, &rows);
    std::unique_ptr<crpc::db::result_set> owned(rows);
    CRPC_CHECK(called.ok());
    CRPC_CHECK(h.session->in_transaction());

    // The same trailing OK is also drained by the OK-reply branch of
    // `execute_prepared` (a procedure with no result set), and it has the same
    // guard: a truncated packet must not be decoded as status 0.
    harness no_rows;
    push_ok(no_rows.raw, 0, 0x0001);
    CRPC_CHECK(no_rows.session->begin().ok());
    no_rows.raw->push(packet(prepare_ok_payload(32, 0, 0), 1));
    no_rows.raw->push(packet(ok_payload(0, 0x0008), 1));
    no_rows.raw->push(packet({std::byte{0x00}}, 2));
    crpc::db::result_set* none = nullptr;
    const crpc::status executed = no_rows.session->call("CALL p()", {}, &none);
    std::unique_ptr<crpc::db::result_set> owned_none(none);
    CRPC_CHECK(executed.ok());
    CRPC_CHECK(no_rows.session->in_transaction());
}

// The binary twin of `mysql_short_eof_keeps_the_transaction_state`. A one-byte
// 0xfe closes the binary row stream, and `read_binary_result` must size-check it
// before decoding: `decode_eof` reports success for a short EOF (it is legal for
// very old servers) and leaves the flags zeroed, so decoding it anyway would apply
// status 0, clear the transaction and make the pool skip its ROLLBACK.
CRPC_TEST(mysql_binary_short_eof_keeps_the_transaction_state) {
    // DEPRECATE_EOF is withheld (the default greeting), so the binary row stream is
    // closed by a classic EOF -- and this one is a single 0xfe byte, which carries
    // no status word.
    harness h;
    CRPC_CHECK((h.session->negotiated_capabilities() & mask(capability::deprecate_eof)) == 0);
    push_ok(h.raw, 0, 0x0001);
    CRPC_CHECK(h.session->begin().ok());
    CRPC_CHECK(h.session->in_transaction());

    h.raw->push(packet(prepare_ok_payload(91, 1, 0), 1));
    h.raw->push(packet(column_payload("id", 8), 2));
    h.raw->push(packet(eof_payload(2), 3));          // prepare: column-definition EOF
    h.raw->push(packet({std::byte{1}}, 1));           // execute: column count
    h.raw->push(packet(column_payload("id", 8), 2));
    h.raw->push(packet(eof_payload(0x0001), 3));      // execute: column EOF, IN_TRANS set
    h.raw->push(packet({std::byte{0x00}, std::byte{0x04}}, 4)); // one binary row (NULL)
    h.raw->push(packet({std::byte{0xfe}}, 5));        // ONE-BYTE terminator: no status word

    crpc::db::result_set* rows = nullptr;
    CRPC_CHECK(h.session->call("CALL p()", {}, &rows).ok());
    std::unique_ptr<crpc::db::result_set> owned(rows);
    // read_binary_result's short-EOF branch must keep the previous status word.
    CRPC_CHECK(h.session->in_transaction());
}

// The binary twin of `mysql_truncated_deprecate_eof_keeps_the_transaction_state`.
// `read_binary_result` decodes the DEPRECATE_EOF terminator with the same
// two-length-encoded-integer walk, so it needs the same guards: without them the
// failed read leaves `ok_offset` past the bogus prefix and the four bytes there are
// applied as a status word, which would clear the transaction and make the pool
// skip its ROLLBACK.
CRPC_TEST(mysql_truncated_binary_deprecate_eof_keeps_the_transaction_state) {
    harness h("caching_sha2_password", 2,
              server_capabilities | mask(capability::deprecate_eof));
    CRPC_CHECK((h.session->negotiated_capabilities() & mask(capability::deprecate_eof)) != 0);
    push_ok(h.raw, 0, 0x0001);
    CRPC_CHECK(h.session->begin().ok());
    CRPC_CHECK(h.session->in_transaction());

    // `call()` is the public way into the binary result path. With DEPRECATE_EOF the
    // prepare reply and the execute reply carry no closing EOF after their column
    // definitions, so neither script queues one.
    h.raw->push(packet(prepare_ok_payload(76, 1, 0), 1));
    h.raw->push(packet(column_payload("id", 8), 2));
    h.raw->push(packet({std::byte{1}}, 1));
    h.raw->push(packet(column_payload("id", 8), 2));
    // Nine bytes: header, then a lone 0xfb -- a NULL marker where an integer is
    // required -- then zero padding. The first length-encoded read fails, so the
    // four bytes at the resulting offset are not a status word and must not be
    // applied.
    std::vector<std::byte> malformed;
    malformed.push_back(std::byte{0xfe});
    malformed.push_back(std::byte{0xfb});
    for (int i = 0; i < 7; ++i) {
        malformed.push_back(std::byte{0});
    }
    h.raw->push(packet(malformed, 3));

    crpc::db::result_set* rows = nullptr;
    const crpc::status called = h.session->call("CALL p()", {}, &rows);
    std::unique_ptr<crpc::db::result_set> owned(rows);
    CRPC_CHECK(called.ok());
    CRPC_CHECK(h.session->in_transaction());

    // The other direction: a structurally complete OK terminator -- nine bytes so it
    // reaches the DEPRECATE_EOF branch rather than the short-EOF one -- that reports
    // IN_TRANS=0 must still be applied. Otherwise "keep the previous value" would
    // degenerate into "never update".
    harness closing("caching_sha2_password", 2,
                    server_capabilities | mask(capability::deprecate_eof));
    push_ok(closing.raw, 0, 0x0001);
    CRPC_CHECK(closing.session->begin().ok());
    CRPC_CHECK(closing.session->in_transaction());

    closing.raw->push(packet(prepare_ok_payload(77, 1, 0), 1));
    closing.raw->push(packet(column_payload("id", 8), 2));
    closing.raw->push(packet({std::byte{1}}, 1));
    closing.raw->push(packet(column_payload("id", 8), 2));
    // 0xfe header, affected=0, last_insert_id=0, status=0x0000 (IN_TRANS clear),
    // warnings=0, then padding up to the nine-byte threshold.
    std::vector<std::byte> terminator;
    terminator.push_back(std::byte{0xfe});
    for (int i = 0; i < 8; ++i) {
        terminator.push_back(std::byte{0});
    }
    closing.raw->push(packet(terminator, 3));

    crpc::db::result_set* closed_rows = nullptr;
    const crpc::status closed_call = closing.session->call("CALL p()", {}, &closed_rows);
    std::unique_ptr<crpc::db::result_set> owned_closed(closed_rows);
    CRPC_CHECK(closed_call.ok());
    CRPC_CHECK_FALSE(closing.session->in_transaction());
}

CRPC_TEST(mysql_error_packet_keeps_the_transaction_state_of_an_idle_autocommit_session) {
    // autocommit=0 as the server reports it on the authentication OK.
    harness off("caching_sha2_password", 0x0000);
    CRPC_CHECK_FALSE(off.session->in_transaction());
    off.raw->push(packet(err_payload(1205, "Lock wait timeout exceeded"), 1));
    crpc::db::result_set* rows = nullptr;
    CRPC_CHECK_FALSE(off.session->query("INSERT INTO t VALUES (1)", {}, &rows).ok());
    std::unique_ptr<crpc::db::result_set> owned(rows);
    // 1205 rolls back the statement only: InnoDB keeps the transaction and its
    // locks, so the pool must send a ROLLBACK rather than hand the session on.
    // (A deadlock, 1213, would roll back the whole transaction; the client cannot
    // tell the two apart from an ERR packet, so the inference stays conservative
    // and a deadlock costs one harmless ROLLBACK.)
    CRPC_CHECK(off.session->in_transaction());

    // The same error in an autocommit=1 session is not an inferred transaction.
    harness on;
    on.raw->push(packet(err_payload(1205, "Lock wait timeout exceeded"), 1));
    crpc::db::result_set* on_rows = nullptr;
    CRPC_CHECK_FALSE(on.session->query("INSERT INTO t VALUES (1)", {}, &on_rows).ok());
    std::unique_ptr<crpc::db::result_set> owned_on(on_rows);
    CRPC_CHECK_FALSE(on.session->in_transaction());
}

// --- R1: session state is restored before a connection is reused -------------
//
// `autocommit` is a session variable: ROLLBACK ends the transaction, it does not
// put the session back to a clean state. A pooled `autocommit=0` session would
// make the next borrower's statements join an implicit transaction, and the next
// release would roll them back -- silent data loss. `reset_session()` closes that
// gap: it always targets `autocommit=1`, deliberately not the server's global
// default (a server started with `--autocommit=0` would hand the next borrower the
// same trap). These cases pin its shape.

CRPC_TEST(mysql_reset_session_restores_autocommit) {
    // The handshake reports status 0x0000 (autocommit off).
    harness h("caching_sha2_password", 0x0000);
    const std::size_t before = h.raw->writes.size();
    // `SET autocommit=1` also commits, so the pool rolls back first; this case
    // only pins the frame the reset itself writes.
    push_ok(h.raw, 0, 0x0002);
    const crpc::status reset = h.session->reset_session();
    CRPC_CHECK(reset.ok());
    CRPC_CHECK_EQ(before + 1, h.raw->writes.size());
    if (h.raw->writes.size() == before + 1) {
        CRPC_CHECK_EQ(std::string("SET autocommit=1"),
                      frame_sql(h.raw->writes.back()));
    }
}

CRPC_TEST(mysql_reset_session_is_free_when_autocommit_is_on) {
    // The handshake reports status 0x0002 (autocommit on), which is the common
    // case. Resetting must not turn every release into an extra round trip.
    harness h;
    const std::size_t before = h.raw->writes.size();
    CRPC_CHECK(h.session->reset_session().ok());
    CRPC_CHECK_EQ(before, h.raw->writes.size());
}

CRPC_TEST(mysql_reset_session_after_close_is_refused) {
    harness h;
    h.session->close();
    const std::size_t before = h.raw->writes.size();
    const crpc::status reset = h.session->reset_session();
    CRPC_CHECK_FALSE(reset.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::shutting_down),
                  static_cast<int>(reset.code()));
    CRPC_CHECK_EQ(before, h.raw->writes.size());
}

CRPC_TEST(mysql_pool_release_rolls_back_then_resets_the_session) {
    // One scripted server session: the handshake reports status 0x0000, so this
    // connection starts with autocommit off.
    auto handshake = std::make_shared<std::vector<std::vector<std::byte>>>();
    handshake->push_back(packet(greeting_payload("caching_sha2_password"), 0));
    handshake->push_back(packet(ok_payload(0, 0x0000), 2));

    scripted_channel* raw = nullptr;
    connection_options options;
    options.user = "root";
    options.password = "";
    crpc::db::mysql::channel_factory factory(
        options,
        [&raw, handshake]() -> crpc::result<std::unique_ptr<crpc::db::mysql::channel>> {
            auto link = std::unique_ptr<scripted_channel>(new scripted_channel);
            for (const std::vector<std::byte>& frame : *handshake) {
                link->push(frame);
            }
            raw = link.get();
            return crpc::result<std::unique_ptr<crpc::db::mysql::channel>>(
                std::unique_ptr<crpc::db::mysql::channel>(link.release()));
        });

    crpc::db::pool_options pool_options;
    pool_options.min_size = 0;
    pool_options.max_size = 1;
    pool_options.max_idle = 1;
    // Validation would add a COM_PING frame; this case is about the release
    // path, so it is disabled rather than scripted around.
    pool_options.validate_on_borrow = false;
    pool_options.acquire_timeout = std::chrono::seconds(5);
    crpc::db::pool connections(pool_options, factory);

    crpc::db::connection* first_connection = nullptr;
    {
        auto lease = connections.acquire();
        CRPC_CHECK(lease.ok());
        if (!lease.ok()) {
            return;
        }
        first_connection = lease.value().get();
        CRPC_CHECK_FALSE(lease.value()->in_transaction());

        crpc::db::result_set* inserted = nullptr;
        // Autocommit is off: this INSERT opens the transaction the caller never
        // asked for, and the server says so.
        push_ok(raw, 1, 0x0001);
        CRPC_CHECK(
            lease.value()->query("INSERT INTO t VALUES (1)", {}, &inserted).ok());
        std::unique_ptr<crpc::db::result_set> owned(inserted);
        CRPC_CHECK(lease.value()->in_transaction());

        // ROLLBACK ends the transaction but leaves autocommit off; the reset
        // must follow it with SET autocommit=1.
        push_ok(raw, 0, 0x0000);
        push_ok(raw, 0, 0x0002);
        const std::size_t before = raw->writes.size();
        CRPC_CHECK(lease.value().release().ok());
        CRPC_CHECK_EQ(before + 2, raw->writes.size());
        if (raw->writes.size() == before + 2) {
            CRPC_CHECK_EQ(std::string("ROLLBACK"), frame_sql(raw->writes[before]));
            CRPC_CHECK_EQ(std::string("SET autocommit=1"),
                          frame_sql(raw->writes[before + 1]));
        }
    }

    auto second = connections.acquire();
    CRPC_CHECK(second.ok());
    if (!second.ok()) {
        return;
    }
    // The pool reused the connection instead of opening another one.
    CRPC_CHECK_EQ(std::uint64_t{1}, connections.opened());
    CRPC_CHECK(second.value().get() == first_connection);
    CRPC_CHECK_FALSE(second.value()->in_transaction());

    // The session really was restored: resetting again is a zero-round-trip
    // no-op, because the status word now carries autocommit=1.
    const std::size_t steady = raw->writes.size();
    CRPC_CHECK(second.value()->reset_session().ok());
    CRPC_CHECK_EQ(steady, raw->writes.size());
}

CRPC_TEST(mysql_release_rolls_back_after_a_packet_without_a_status_word) {
    // Same single-session scripted server as R1-d: the handshake reports status
    // 0x0000, so this connection starts with autocommit off.
    auto handshake = std::make_shared<std::vector<std::vector<std::byte>>>();
    handshake->push_back(packet(greeting_payload("caching_sha2_password"), 0));
    handshake->push_back(packet(ok_payload(0, 0x0000), 2));

    scripted_channel* raw = nullptr;
    connection_options options;
    options.user = "root";
    options.password = "";
    crpc::db::mysql::channel_factory factory(
        options,
        [&raw, handshake]() -> crpc::result<std::unique_ptr<crpc::db::mysql::channel>> {
            auto link = std::unique_ptr<scripted_channel>(new scripted_channel);
            for (const std::vector<std::byte>& frame : *handshake) {
                link->push(frame);
            }
            raw = link.get();
            return crpc::result<std::unique_ptr<crpc::db::mysql::channel>>(
                std::unique_ptr<crpc::db::mysql::channel>(link.release()));
        });

    crpc::db::pool_options pool_options;
    pool_options.min_size = 0;
    pool_options.max_size = 1;
    pool_options.max_idle = 1;
    pool_options.validate_on_borrow = false;
    pool_options.acquire_timeout = std::chrono::seconds(5);
    crpc::db::pool connections(pool_options, factory);

    auto lease = connections.acquire();
    CRPC_CHECK(lease.ok());
    if (!lease.ok()) {
        return;
    }

    // Autocommit is off: the INSERT opens the transaction and the server says so.
    crpc::db::result_set* inserted = nullptr;
    push_ok(raw, 1, 0x0001);
    CRPC_CHECK(lease.value()->query("INSERT INTO t VALUES (1)", {}, &inserted).ok());
    std::unique_ptr<crpc::db::result_set> owned_insert(inserted);
    CRPC_CHECK(lease.value()->in_transaction());

    // A result set whose terminator is a one-byte 0xfe: it carries no status word,
    // so the old one must stand.
    raw->push(packet({std::byte{1}}, 1));
    raw->push(packet(column_payload("id", 8), 2));
    raw->push(packet(eof_payload(0x0001), 3));
    std::vector<std::byte> row;
    crpc::db::mysql::append_lenenc_string(row, "1");
    raw->push(packet(row, 4));
    raw->push(packet({std::byte{0xfe}}, 5));
    crpc::db::result_set* selected = nullptr;
    CRPC_CHECK(lease.value()->query("SELECT id FROM t", {}, &selected).ok());
    std::unique_ptr<crpc::db::result_set> owned_select(selected);
    CRPC_CHECK(lease.value()->in_transaction());

    // Release: ROLLBACK ends the transaction, then SET autocommit=1 restores the
    // session. The core assertion is the first frame: if the status word had been
    // wiped, `in_transaction()` would be false and the pool would skip the ROLLBACK
    // and go straight to the implicit commit that `SET autocommit=1` performs.
    push_ok(raw, 0, 0x0000);
    push_ok(raw, 0, 0x0002);
    const std::size_t before = raw->writes.size();
    CRPC_CHECK(lease.value().release().ok());
    const std::size_t written = raw->writes.size() - before;
    CRPC_CHECK_EQ(std::size_t{2}, written);
    const std::string first_release =
        written > 0 ? frame_sql(raw->writes[before]) : std::string("<no frame>");
    CRPC_CHECK_EQ(std::string("ROLLBACK"), first_release);
    if (written == 2) {
        CRPC_CHECK_EQ(std::string("SET autocommit=1"),
                      frame_sql(raw->writes[before + 1]));
    }
}

CRPC_TEST_MAIN()
