// CRPC -- MySQL connection tests over a scripted channel.
//
// The connection is written against `channel`, so the handshake and the
// result-set loop are exercised with a scripted server and no socket. The script
// is built from the documented packet shapes, not from the client's own encoder,
// so a systematic mistake cannot agree with itself.
#include "rsa_test_support.hpp"
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

CRPC_TEST_MAIN()
