// CRPC -- MySQL TLS transport tests (CLIENT_SSL / SSLRequest / TLS upgrade).
//
// The unit cases run over a scripted channel, no socket. The TLS cases need a
// *real* TLS handshake, so the OpenSSL-build tests wire a server-side
// `io::tls_stream` (the same object the library itself uses) to the scripted
// channel: the client's TLS records are fed to it, its records are fed back,
// and its plaintext side plays the MySQL server. A byte-level SSLRequest
// assertion and a "the password only exists inside the encrypted stream"
// assertion pin the protocol down; "the wire never carries the password"
// proves encryption instead of assuming it.
//
// Without OpenSSL the cases assert the fail-closed rule: enabling TLS must
// fail with an explicit error before any byte is written, never fall back to
// cleartext.
#include "test_framework.hpp"
#include "tls_test_certificate.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <crpc/db/mysql/channel.hpp>
#include <crpc/db/mysql/connection.hpp>
#include <crpc/db/mysql/protocol.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

#if defined(CRPC_HAVE_OPENSSL)
#include <crpc/io/tls.hpp>
#endif

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

// Auth More Data carrying the caching_sha2_password "full authentication"
// marker: the cache was cold and the client must prove the password again.
[[nodiscard]] std::vector<std::byte> full_auth_request_payload() {
    std::vector<std::byte> payload;
    payload.push_back(std::byte{0x01});
    payload.push_back(std::byte{0x04});
    return payload;
}

// The greeting's capability set is a parameter: the TLS cases need
// CLIENT_SSL (0x800) offered, the downgrade cases must not offer it.
constexpr std::uint32_t base_server_capabilities =
    mask(capability::protocol_41) | mask(capability::secure_connection) |
    mask(capability::plugin_auth) | mask(capability::transactions);

[[nodiscard]] std::vector<std::byte> greeting_payload(std::string_view plugin,
                                                      std::uint32_t capabilities) {
    std::vector<std::byte> payload;
    payload.push_back(std::byte{10});
    append_text(payload, "8.0.36");
    payload.push_back(std::byte{0});
    append_u32(payload, 42);
    for (int i = 0; i < 8; ++i) {
        payload.push_back(static_cast<std::byte>(i + 1));
    }
    payload.push_back(std::byte{0});
    append_u16(payload, static_cast<std::uint16_t>(capabilities & 0xffffu));
    payload.push_back(std::byte{45});
    append_u16(payload, 2);
    append_u16(payload, static_cast<std::uint16_t>((capabilities >> 16) & 0xffffu));
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

// Replays scripted bytes and records what the client wrote. Writes are always
// recorded (they are the byte-level evidence); when `tls_forward` is set the
// write is also fed to the test's server-side TLS layer.
class scripted_channel final : public crpc::db::mysql::channel {
public:
    std::deque<std::vector<std::byte>> incoming;
    std::vector<std::vector<std::byte>> writes;
    bool closed = false;
    std::function<void(crpc::span<const std::byte>)> tls_forward;

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
        if (tls_forward) {
            tls_forward(data);
        }
        return crpc::status{};
    }

    void close() noexcept override { closed = true; }

    [[nodiscard]] bool is_secure() const noexcept override { return false; }

    void push(std::vector<std::byte> data) { incoming.push_back(std::move(data)); }
};

// The documented SSLRequest payload: 4-byte client flags (with CLIENT_SSL),
// 4-byte max packet size, 1-byte charset, 23 reserved zero bytes.
constexpr std::size_t ssl_request_payload_size = 32;
// Only the OpenSSL build can observe an SSLRequest (without TLS the client
// never sends one), so this constant is unused in a no-OpenSSL translation
// unit. clang's -Wunused-const-variable is part of the -Werror gate there;
// MSVC's /W4 does not diagnose it, which is why the CI clang job found this
// and the Windows jobs did not.
#if defined(CRPC_HAVE_OPENSSL)
constexpr std::uint32_t ssl_request_max_packet_size = 16u * 1024u * 1024u;
#endif

[[nodiscard]] bool wire_contains(const std::vector<std::byte>& haystack,
                                 const std::string_view needle) {
    const std::vector<std::byte> pattern = bytes_of(needle);
    return std::search(haystack.begin(), haystack.end(), pattern.begin(), pattern.end()) !=
           haystack.end();
}

struct harness {
    scripted_channel* raw = nullptr;
    std::unique_ptr<connection> session;

    harness(std::string_view plugin, std::uint32_t server_capabilities) {
        auto scripted = std::unique_ptr<scripted_channel>(new scripted_channel);
        raw = scripted.get();
        raw->push(packet(greeting_payload(plugin, server_capabilities), 0));
        raw->push(packet(ok_payload(0, 2), 2));
        connection_options options;
        options.user = "root";
        options.password = "";
        session.reset(new connection(options, std::move(scripted)));
    }
};

#if defined(CRPC_HAVE_OPENSSL)

// --- A real TLS server over the scripted channel -----------------------------

std::string write_test_certificate() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path();
    const std::string certificate = (dir / "crpc_test_mysql_tls_certificate.pem").string();
    const std::string key = (dir / "crpc_test_mysql_tls_key.pem").string();
    const auto write = [](const std::string& path, const char* data) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(data, static_cast<std::streamsize>(std::strlen(data)));
    };
    write(certificate, crpc_test_tls::k_certificate_pem);
    write(key, crpc_test_tls::k_private_key_pem);
    return certificate;
}

class wire_sink final : public crpc::io::sink {
public:
    explicit wire_sink(scripted_channel& peer) noexcept : peer_(peer) {}

    void push(crpc::span<const std::byte> data, crpc::io::completion* c) noexcept override {
        if (!data.empty()) {
            peer_.incoming.emplace_back(data.begin(), data.end());
        }
        if (c != nullptr) {
            c->complete(crpc::io::io_result{std::error_code{}, data.size()});
        }
    }

private:
    scripted_channel& peer_;
};

class inline_completion final : public crpc::io::completion {
public:
    void complete(crpc::io::io_result value) noexcept override {
        ++calls;
        result = value;
    }
    int calls = 0;
    crpc::io::io_result result{};
};

// The MySQL server side behind the TLS record layer: plaintext MySQL packets
// arrive here; each one is answered with the next scripted reply, framed with
// the sequence that follows the packet just received.
class server_protocol final : public crpc::io::source {
public:
    server_protocol(crpc::io::tls_stream& stream, std::deque<std::vector<std::byte>> replies)
        : stream_(&stream), replies_(std::move(replies)) {}

    std::size_t on_data(crpc::span<const std::byte> data) noexcept override {
        const auto* first = reinterpret_cast<const std::byte*>(data.data());
        plaintext_.insert(plaintext_.end(), first, first + data.size());
        std::size_t consumed = 0;
        for (;;) {
            if (buffered_.size() < 4) {
                const std::size_t take = std::min(std::size_t{4} - buffered_.size(),
                                                  plaintext_.size() - consumed);
                buffered_.insert(buffered_.end(), plaintext_.begin() + static_cast<std::ptrdiff_t>(consumed),
                                 plaintext_.begin() + static_cast<std::ptrdiff_t>(consumed + take));
                consumed += take;
                if (buffered_.size() < 4) {
                    break;
                }
            }
            const std::size_t length = static_cast<std::size_t>(buffered_[0]) |
                                       (static_cast<std::size_t>(buffered_[1]) << 8) |
                                       (static_cast<std::size_t>(buffered_[2]) << 16);
            const std::uint8_t sequence = static_cast<std::uint8_t>(buffered_[3]);
            if (buffered_.size() < 4 + length) {
                const std::size_t need = 4 + length - buffered_.size();
                const std::size_t take = std::min(need, plaintext_.size() - consumed);
                buffered_.insert(buffered_.end(),
                                 plaintext_.begin() + static_cast<std::ptrdiff_t>(consumed),
                                 plaintext_.begin() + static_cast<std::ptrdiff_t>(consumed + take));
                consumed += take;
                if (buffered_.size() < 4 + length) {
                    break;
                }
            }
            // One complete packet: record it, answer with the next scripted
            // reply framed at the following sequence id.
            std::vector<std::byte> received(buffered_.begin() + 4, buffered_.end());
            received_packets_.push_back(std::move(received));
            buffered_.clear();
            if (!replies_.empty()) {
                std::vector<std::byte> reply = packet(replies_.front(),
                                                      static_cast<std::uint8_t>(sequence + 1));
                replies_.pop_front();
                push_reply(reply);
            }
        }
        plaintext_.erase(plaintext_.begin(),
                         plaintext_.begin() + static_cast<std::ptrdiff_t>(consumed));
        return data.size();
    }

    void on_eof() noexcept override { eof_ = true; }
    void on_error(std::error_code ec) noexcept override { error_ = ec; }

    [[nodiscard]] const std::vector<std::vector<std::byte>>& received() const noexcept {
        return received_packets_;
    }

    bool eof_ = false;
    std::error_code error_{};

private:
    // One reply at a time. push() may park the completion and fire it after
    // returning -- from inside on_data the stream's drive loop defers it to
    // the outer pass, so waiting here would deadlock against ourselves. The
    // member completion outlives that deferral; the reply order is preserved
    // because the next packet (and so the next reply) only arrives after the
    // client saw this one.
    void push_reply(const std::vector<std::byte>& reply) {
        completion_.calls = 0;
        stream_->push(crpc::span<const std::byte>(reply.data(), reply.size()), &completion_);
    }

    crpc::io::tls_stream* stream_;
    std::deque<std::vector<std::byte>> replies_;
    inline_completion completion_;
    std::vector<std::byte> plaintext_;
    std::vector<std::byte> buffered_;
    std::vector<std::vector<std::byte>> received_packets_;
};

// TLS-enabled session harness: the scripted transport carries the handshake
// and then the encrypted stream; the server TLS layer decrypts it and plays a
// scripted MySQL server.
struct tls_harness {
    scripted_channel* raw = nullptr;
    std::unique_ptr<crpc::io::tls_context> server_context;
    std::unique_ptr<crpc::io::tls_stream> server_stream;
    std::unique_ptr<wire_sink> server_wire;
    std::unique_ptr<server_protocol> protocol;
    // Declared last so it is destroyed first: the session's teardown sends a
    // TLS close_notify through the scripted channel, and the server-side TLS
    // stream must still be alive to receive it.
    std::unique_ptr<connection> session;

    tls_harness(const connection_options& options, std::uint32_t server_capabilities,
                std::deque<std::vector<std::byte>> replies) {
        auto scripted = std::unique_ptr<scripted_channel>(new scripted_channel);
        raw = scripted.get();
        raw->push(packet(greeting_payload("caching_sha2_password", server_capabilities), 0));

        crpc::io::tls_config config;
        config.role = crpc::io::tls_role::server;
        config.verify = crpc::io::tls_verify_mode::none;
        config.certificate_chain = write_test_certificate();
        config.private_key = (std::filesystem::temp_directory_path() /
                              "crpc_test_mysql_tls_key.pem")
                                 .string();
        config.alpn_protocols = {};  // MySQL does not use ALPN
        config.require_alpn = false;
        std::error_code ec;
        server_context = crpc::io::tls_context::create(config, ec);
        CRPC_CHECK(!ec);
        if (ec) {
            return;
        }

        server_wire.reset(new wire_sink(*scripted));
        server_stream = crpc::io::tls_stream::create(*server_wire, *server_context, "", ec);
        CRPC_CHECK(!ec);
        if (ec) {
            return;
        }
        protocol.reset(new server_protocol(*server_stream, std::move(replies)));
        server_stream->bind_source(protocol.get());
        // The SSLRequest is cleartext MySQL framing; only the TLS records that
        // follow it belong to the server TLS layer.
        raw->tls_forward = [this](crpc::span<const std::byte> data) {
            if (raw->writes.size() > 1) {
                (void)server_stream->on_data(data);
            }
        };

        session.reset(new connection(options, std::move(scripted)));
    }
};

#endif // CRPC_HAVE_OPENSSL

} // namespace

// --- Both builds -------------------------------------------------------------

CRPC_TEST(tls_disabled_plain_handshake_is_unchanged) {
    harness h("caching_sha2_password", base_server_capabilities);
    CRPC_CHECK(h.session->is_open());
    CRPC_CHECK_FALSE(h.session->tls_active());
    CRPC_CHECK_FALSE(h.session->tls_downgraded());
    // One write before the OK reply: the HandshakeResponse41, not SSLRequest.
    CRPC_CHECK_EQ(std::size_t{1}, h.raw->writes.size());
    const std::vector<std::byte>& frame = h.raw->writes.front();
    CRPC_CHECK(frame.size() > 4 + ssl_request_payload_size);
    const std::uint32_t flags = static_cast<std::uint32_t>(frame[4]) |
                                (static_cast<std::uint32_t>(frame[5]) << 8) |
                                (static_cast<std::uint32_t>(frame[6]) << 16) |
                                (static_cast<std::uint32_t>(frame[7]) << 24);
    CRPC_CHECK_EQ(std::uint32_t{0}, flags & mask(capability::ssl));
}

CRPC_TEST(tls_enabled_server_without_ssl_required_fails_closed) {
    auto scripted = std::unique_ptr<scripted_channel>(new scripted_channel);
    scripted_channel* raw = scripted.get();
    raw->push(packet(greeting_payload("caching_sha2_password", base_server_capabilities), 0));
    connection_options options;
    options.user = "root";
    options.password = "";
    options.tls_options.enabled = true;  // require defaults to true
    connection session(options, std::move(scripted));
    CRPC_CHECK_FALSE(session.is_open());
    const crpc::rpc_error& why = session.connect_error();
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(why.code));
    CRPC_CHECK(why.message.find("TLS") != std::string::npos);
    // Fail closed: no SSLRequest, no cleartext handshake response.
    CRPC_CHECK(raw->writes.empty());
}

#if !defined(CRPC_HAVE_OPENSSL)

// Without a TLS backend an enabled TLS request must fail explicitly before any
// byte is written -- never a silent cleartext downgrade.
CRPC_TEST(tls_enabled_without_backend_fails_explicitly) {
    auto scripted = std::unique_ptr<scripted_channel>(new scripted_channel);
    scripted_channel* raw = scripted.get();
    raw->push(packet(
        greeting_payload("caching_sha2_password",
                         base_server_capabilities | mask(capability::ssl)),
        0));
    connection_options options;
    options.user = "root";
    options.password = "";
    options.tls_options.enabled = true;
    connection session(options, std::move(scripted));
    CRPC_CHECK_FALSE(session.is_open());
    const crpc::rpc_error& why = session.connect_error();
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(why.code));
    CRPC_CHECK(why.message.find("TLS") != std::string::npos);
    CRPC_CHECK(why.message.find("CRPC_WITH_OPENSSL") != std::string::npos);
    // The refusal happened before the SSLRequest left the client.
    CRPC_CHECK(raw->writes.empty());
}

// The "prefer" downgrade only applies to a server that cannot do TLS; a
// missing backend is a configuration error even then.
CRPC_TEST(tls_prefer_without_backend_still_fails_explicitly) {
    auto scripted = std::unique_ptr<scripted_channel>(new scripted_channel);
    scripted_channel* raw = scripted.get();
    raw->push(packet(
        greeting_payload("caching_sha2_password",
                         base_server_capabilities | mask(capability::ssl)),
        0));
    connection_options options;
    options.user = "root";
    options.password = "";
    options.tls_options.enabled = true;
    options.tls_options.require = false;
    connection session(options, std::move(scripted));
    CRPC_CHECK_FALSE(session.is_open());
    const crpc::rpc_error& why = session.connect_error();
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(why.code));
    CRPC_CHECK(raw->writes.empty());
}

// The cleartext full-auth path (is_secure() == false) still asks for the
// server's RSA public key instead of sending the password.
CRPC_TEST(plain_full_auth_requests_rsa_key_not_password) {
    auto scripted = std::unique_ptr<scripted_channel>(new scripted_channel);
    scripted_channel* raw = scripted.get();
    raw->push(packet(greeting_payload("caching_sha2_password", base_server_capabilities), 0));
    raw->push(packet(full_auth_request_payload(), 2));
    connection_options options;
    options.user = "root";
    options.password = "pw";
    connection session(options, std::move(scripted));
    // The script ends after the 0x02 key request: the client asked for the
    // RSA key (is_secure() was false), so the password was never sent. The
    // full-auth exchange never completed, hence eof and no "fast auth".
    CRPC_CHECK_FALSE(session.is_open());
    CRPC_CHECK_EQ(std::size_t{2}, raw->writes.size());
    const std::vector<std::byte>& request = raw->writes[1];
    CRPC_CHECK_EQ(std::size_t{5}, request.size());  // header + one 0x02 byte
    CRPC_CHECK_EQ(0x02u, static_cast<unsigned>(request[4]));
    CRPC_CHECK_FALSE(wire_contains(request, "pw"));
}

#else

// --- OpenSSL build -----------------------------------------------------------

CRPC_TEST(tls_ssl_request_packet_shape_and_sequence) {
    connection_options options;
    options.user = "root";
    options.password = "";
    options.tls_options.enabled = true;
    options.tls_options.server_name = "localhost";
    options.tls_options.ca_bundle = write_test_certificate();
    // require defaults to true; verification defaults to on.
    tls_harness h(options, base_server_capabilities | mask(capability::ssl),
                  {ok_payload(0, 2)});
    CRPC_CHECK(h.session != nullptr);
    if (h.session == nullptr) {
        return;
    }
    CRPC_CHECK(h.session->is_open());
    if (!h.session->is_open()) {
        std::printf("  connect error: %s\n", h.session->connect_error().message.c_str());
        return;
    }
    CRPC_CHECK(h.session->tls_active());
    CRPC_CHECK_FALSE(h.session->tls_downgraded());

    // Packet 1: the SSLRequest. Header says 32 bytes at sequence 1.
    CRPC_CHECK(h.raw->writes.size() >= 2);
    if (h.raw->writes.size() < 2) {
        return;
    }
    const std::vector<std::byte>& ssl_request = h.raw->writes[0];
    CRPC_CHECK_EQ(std::size_t{4 + ssl_request_payload_size}, ssl_request.size());
    CRPC_CHECK_EQ(32u, static_cast<unsigned>(ssl_request[0]));
    CRPC_CHECK_EQ(0u, static_cast<unsigned>(ssl_request[1]));
    CRPC_CHECK_EQ(0u, static_cast<unsigned>(ssl_request[2]));
    CRPC_CHECK_EQ(1u, static_cast<unsigned>(ssl_request[3]));
    // Client flags include CLIENT_SSL and match the negotiated response flags.
    const std::uint32_t flags = static_cast<std::uint32_t>(ssl_request[4]) |
                                (static_cast<std::uint32_t>(ssl_request[5]) << 8) |
                                (static_cast<std::uint32_t>(ssl_request[6]) << 16) |
                                (static_cast<std::uint32_t>(ssl_request[7]) << 24);
    CRPC_CHECK_EQ(mask(capability::ssl), flags & mask(capability::ssl));
    CRPC_CHECK_EQ(mask(capability::protocol_41), flags & mask(capability::protocol_41));
    CRPC_CHECK_EQ(static_cast<std::uint32_t>(h.session->negotiated_capabilities()), flags);
    // Max packet size and charset.
    CRPC_CHECK_EQ(ssl_request_max_packet_size,
                  static_cast<std::uint32_t>(ssl_request[8]) |
                      (static_cast<std::uint32_t>(ssl_request[9]) << 8) |
                      (static_cast<std::uint32_t>(ssl_request[10]) << 16) |
                      (static_cast<std::uint32_t>(ssl_request[11]) << 24));
    CRPC_CHECK_EQ(std::uint8_t{45}, static_cast<std::uint8_t>(ssl_request[12]));
    // 23 reserved bytes are zero.
    for (std::size_t i = 13; i < ssl_request_payload_size; ++i) {
        CRPC_CHECK_EQ(0u, static_cast<unsigned>(ssl_request[i]));
    }
    // Packet 2: a TLS ClientHello, not another MySQL packet.
    CRPC_CHECK_EQ(0x16u, static_cast<unsigned>(h.raw->writes[1][0]));
    // The negotiated capability set carries CLIENT_SSL for the diagnostics.
    CRPC_CHECK_EQ(mask(capability::ssl),
                  h.session->negotiated_capabilities() & mask(capability::ssl));
    // The server side received the auth response inside the decrypted stream.
    CRPC_CHECK_FALSE(h.protocol->received().empty());
    if (!h.protocol->received().empty()) {
        const std::vector<std::byte>& response = h.protocol->received().front();
        CRPC_CHECK_FALSE(response.empty());
        CRPC_CHECK(wire_contains(response, "root"));
    }
}

CRPC_TEST(tls_full_auth_sends_password_inside_encrypted_stream_only) {
    connection_options options;
    options.user = "root";
    options.password = "s3cret";
    options.tls_options.enabled = true;
    options.tls_options.server_name = "localhost";
    options.tls_options.ca_bundle = write_test_certificate();
    // The cache is cold: 0x01 0x04 asks for the password again. Over TLS the
    // answer is the cleartext password + NUL, and an OK follows.
    tls_harness h(options, base_server_capabilities | mask(capability::ssl),
                  {full_auth_request_payload(), ok_payload(0, 2)});
    CRPC_CHECK(h.session != nullptr);
    if (h.session == nullptr) {
        return;
    }
    CRPC_CHECK(h.session->is_open());
    if (!h.session->is_open()) {
        std::printf("  connect error: %s\n", h.session->connect_error().message.c_str());
        return;
    }
    CRPC_CHECK(h.session->used_full_authentication());
    CRPC_CHECK(h.protocol->received().size() >= 2);
    if (h.protocol->received().size() < 2) {
        return;
    }
    // The password travelled as plaintext *inside* the TLS stream ...
    const std::vector<std::byte>& secret = h.protocol->received()[1];
    CRPC_CHECK_EQ(std::size_t{options.password.size() + 1}, secret.size());
    CRPC_CHECK(wire_contains(secret, "s3cret"));
    // ... and never on the wire: no recorded write carries it.
    for (const std::vector<std::byte>& frame : h.raw->writes) {
        CRPC_CHECK_FALSE(wire_contains(frame, "s3cret"));
    }
}

CRPC_TEST(tls_handshake_garbage_reply_fails_explicitly) {
    auto scripted = std::unique_ptr<scripted_channel>(new scripted_channel);
    scripted_channel* raw = scripted.get();
    raw->push(packet(
        greeting_payload("caching_sha2_password",
                         base_server_capabilities | mask(capability::ssl)),
        0));
    // Not a TLS record: the handshake must fail with a named error instead of
    // being swallowed into a generic connect failure.
    std::vector<std::byte> garbage;
    for (int i = 0; i < 16; ++i) {
        garbage.push_back(static_cast<std::byte>(0xA0 + i));
    }
    raw->push(std::move(garbage));
    connection_options options;
    options.user = "root";
    options.password = "";
    options.tls_options.enabled = true;
    connection session(options, std::move(scripted));
    CRPC_CHECK_FALSE(session.is_open());
    const crpc::rpc_error& why = session.connect_error();
    CRPC_CHECK(why.message.find("TLS") != std::string::npos);
    CRPC_CHECK(why.message.find("handshake") != std::string::npos);
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(why.code));
    CRPC_CHECK_FALSE(raw->writes.empty());
}

CRPC_TEST(tls_handshake_transport_close_fails_explicitly) {
    auto scripted = std::unique_ptr<scripted_channel>(new scripted_channel);
    scripted_channel* raw = scripted.get();
    raw->push(packet(
        greeting_payload("caching_sha2_password",
                         base_server_capabilities | mask(capability::ssl)),
        0));
    // Nothing else is pushed: the peer closes right after the SSLRequest.
    connection_options options;
    options.user = "root";
    options.password = "";
    options.tls_options.enabled = true;
    connection session(options, std::move(scripted));
    CRPC_CHECK_FALSE(session.is_open());
    const crpc::rpc_error& why = session.connect_error();
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::eof), static_cast<int>(why.code));
    // The client did attempt the upgrade: SSLRequest and ClientHello left.
    CRPC_CHECK(raw->writes.size() >= 2);
    CRPC_CHECK_EQ(0x16u, static_cast<unsigned>(raw->writes[1][0]));
}

CRPC_TEST(tls_hostname_verification_failure_is_explicit) {
    connection_options options;
    options.user = "root";
    options.password = "";
    options.tls_options.enabled = true;
    // A name the certificate does not cover: the handshake must be refused
    // with a verification-specific error, not a generic connection failure.
    options.tls_options.server_name = "wrong.example.com";
    options.tls_options.ca_bundle = write_test_certificate();
    tls_harness h(options, base_server_capabilities | mask(capability::ssl),
                  {ok_payload(0, 2)});
    CRPC_CHECK(h.session != nullptr);
    if (h.session == nullptr) {
        return;
    }
    CRPC_CHECK_FALSE(h.session->is_open());
    const crpc::rpc_error& why = h.session->connect_error();
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(why.code));
    CRPC_CHECK(why.message.find("verification") != std::string::npos);
}

CRPC_TEST(tls_server_without_ssl_prefer_downgrades_visibly) {
    auto scripted = std::unique_ptr<scripted_channel>(new scripted_channel);
    scripted_channel* raw = scripted.get();
    raw->push(packet(greeting_payload("caching_sha2_password", base_server_capabilities), 0));
    raw->push(packet(ok_payload(0, 2), 2));
    connection_options options;
    options.user = "root";
    options.password = "";
    options.tls_options.enabled = true;
    options.tls_options.require = false;  // "prefer": cleartext fallback allowed
    connection session(options, std::move(scripted));
    CRPC_CHECK(session.is_open());
    if (session.is_open()) {
        CRPC_CHECK_FALSE(session.tls_active());
        // The downgrade is visible in the diagnostics, never silent.
        CRPC_CHECK(session.tls_downgraded());
    }
    // The first write is the HandshakeResponse41 (no SSLRequest, no CLIENT_SSL).
    CRPC_CHECK_EQ(std::size_t{1}, raw->writes.size());
    const std::vector<std::byte>& frame = raw->writes.front();
    CRPC_CHECK(frame.size() > 4 + ssl_request_payload_size);
    const std::uint32_t flags = static_cast<std::uint32_t>(frame[4]) |
                                (static_cast<std::uint32_t>(frame[5]) << 8) |
                                (static_cast<std::uint32_t>(frame[6]) << 16) |
                                (static_cast<std::uint32_t>(frame[7]) << 24);
    CRPC_CHECK_EQ(std::uint32_t{0}, flags & mask(capability::ssl));
}

CRPC_TEST(tls_plain_session_reports_not_secure) {
    // The counterpoint to the full-auth case above: a cleartext session has
    // is_secure() == false, which is what steers caching_sha2 to RSA.
    harness h("caching_sha2_password", base_server_capabilities);
    CRPC_CHECK(h.session->is_open());
    CRPC_CHECK_FALSE(h.session->tls_active());
}

#endif // CRPC_HAVE_OPENSSL

CRPC_TEST_MAIN()
