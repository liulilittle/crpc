// CRPC -- TLS decorator tests.
//
// Without CRPC_HAVE_OPENSSL the only case prints the documented skip line and
// exits zero.  With OpenSSL the cases exercise the decorator against an
// in-memory byte pipe: no sockets, no threads, deterministic scheduling.
#include <crpc/io/tls.hpp>

#include "test_framework.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if !defined(CRPC_HAVE_OPENSSL)

CRPC_TEST(tls_openssl_not_available) {
    std::printf("SKIP test_tls: OpenSSL not available\n");
}

#else

#include <filesystem>
#include <fstream>

namespace {

using namespace crpc;
using namespace crpc::io;

// --- Test certificate --------------------------------------------------------
//
// A self-signed RSA-2048 certificate for CN=localhost with
// subjectAltName=DNS:localhost,IP:127.0.0.1, valid for ten years.  Written to
// the temporary directory because the public configuration takes file paths.
const char* const k_certificate_pem = R"PEM(-----BEGIN CERTIFICATE-----
MIIDJTCCAg2gAwIBAgIUSVax/lUOko/4wf8wfSFXnkmnYMcwDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MTAxODE5NDgyNloXDTM2MTAx
NTE5NDgyNlowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF
AAOCAQ8AMIIBCgKCAQEAwtVMZ5c03tzrbfYVTVlVPL5VCgiYExGbDYTiCo2gWJDW
+saByjshliss/ClsVoFhpXnfN/V2kR+Qnyt0UJSdqop8RZNMjMwX1QXwavnWwnm+
pKZimBv4iBhDAsNfzTLSLPOMnhD4ftc/yUuAJuyHp0noeZgp3BAz5ADbpmpy79fK
AsWngKpRvOVfM7U1TWkaQ9drveB+k/JRAB5AefsQ/RNQeTtNmKJzn4EJtNvJHWR3
U1bsRSDZTi60dikGTUK6K7uSzcLwAaBKNZ8mMSBq4GCbXluWzQtrfv5qZgqwIpmM
uN4v/b1ckWBTowRKWqz2BUfHmCtKlFXGWGwFIBXLMwIDAQABo28wbTAdBgNVHQ4E
FgQUqSLZfJ8oNw8+Yx88UUDrLksbV/kwHwYDVR0jBBgwFoAUqSLZfJ8oNw8+Yx88
UUDrLksbV/kwDwYDVR0TAQH/BAUwAwEB/zAaBgNVHREEEzARgglsb2NhbGhvc3SH
BH8AAAEwDQYJKoZIhvcNAQELBQADggEBAFVk6BxPdMobMgtr0XIp032H5UnbIUIg
e4GxqsDeI+OCiGsMxNgwFcRHa0InLV8cA9wtQsVteTnPPkB8A979F43p+tFFwo01
yFbsfHxbOOdj8WRqtnJKF2xHL5aJWrqSuKLaRlNpqF8y3S0WonetjV8DqrGgDoO9
IqtsMlZvtQIt5ZwoQpYfw1zY+fYMte/F95o0hvFZPvtALif9EsWmjZ0zDUYVhQLY
cMNxmeN4Auxql8tNAX40nOv2specsbXqGV6UM/qyZXUarWVSDLn/mOHu8uFSSora
ZRqZd8mEpodmtGRWyup61t1rBtcBeAR8Q5PijHwuLfsrPjQX0bU8Qs0=
-----END CERTIFICATE-----
)PEM";

const char* const k_private_key_pem = R"PEM(-----BEGIN PRIVATE KEY-----
MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQDC1UxnlzTe3Ott
9hVNWVU8vlUKCJgTEZsNhOIKjaBYkNb6xoHKOyGWKyz8KWxWgWGled839XaRH5Cf
K3RQlJ2qinxFk0yMzBfVBfBq+dbCeb6kpmKYG/iIGEMCw1/NMtIs84yeEPh+1z/J
S4Am7IenSeh5mCncEDPkANumanLv18oCxaeAqlG85V8ztTVNaRpD12u94H6T8lEA
HkB5+xD9E1B5O02YonOfgQm028kdZHdTVuxFINlOLrR2KQZNQroru5LNwvABoEo1
nyYxIGrgYJteW5bNC2t+/mpmCrAimYy43i/9vVyRYFOjBEparPYFR8eYK0qUVcZY
bAUgFcszAgMBAAECggEADpIPZo3cfTBDW760ECFL/8DHhk+8jmyEzn7aEr+jDAyP
me3fhfq5w8u9z6vfYhwcloNQlL7thjk4l8FoWksK/Cvi0vyK6dqs4O2n8bAhMKZp
11FpfTEA+1iT0QXrHmLxwefYOSKmxYGeemou5CT7Q3ZXsERCGSbjDTxH26BesR4D
ZdhnmTmI2IibmhFghQ3RmEnDkSQUsJQH/sichLloh7T7JQw7hc78X3gMFbrN1un7
ztYHKBAdHzAEfPsYOA70iE+ZEk/CswXDHVGoLm5ob4ralaz2BAAPSnMnRC3uVrMB
vQbd/r6/oynsxVYbXqJTCoMCiVIJK34Wo/EQmC2q9QKBgQD7fhw+ZR2fKfZfdbYH
69EFDkuMSAATNiMSwA//DzSPqZyUHjKDp67i9gblZv1inB+Ijq+0cXID23k2gOee
MPdNb9dU64Rua3M1uUrJUO1W8nbBXOc0gwbpVixVDDhhaWxrNnYSSUVeLQdUT+sL
qZltBsTUTEKTfNyPNhTFzNPmfQKBgQDGUzmwMqxEtUdi+77QR8y3ePE80EQSA7bj
6kaUx6y3FanclxlwSDSPFHq/leT4KURNkiB54zMECoYmynSTSkJA0ZQhyZTxD+E2
w2mWKVZ0cuZNRcFAXb15eJCrGfLE4ATqR9BTzZ+THHANffhuoOZ1RIAXrTSk8H6d
TtdCL3A3bwKBgQCXAY8F7AMQScr0YKKPOQpFZeL49VaOMoh2vrocJU4jIO2yhjtA
kBPTlXRolCF671GD1mEsEJ4sMMEkd9wbQT2M0qXSCKR5BK2lyX9dR+lqtqWVzynI
fxBMVQDDhFXqL7n42jelHTe4p4LqHh583zYPn+vZqXykQGnS1987MZTepQKBgGyE
lYhH8eMWnh0XSu2EnAu4baes/dItniFPy5cVOvhjLjK5qv25RKkVt6SlTEhNrEYV
sxd+rieD6qvHE8qUU3RMORXTy1+sJLtmaZv0uo21YTbe+ENUB50zz1UQObpoKchX
yzJPzQUeBmIbj5hZCkEYXk6WQ7wMq/s/bKGWe3yZAoGAY5cSINXJh9m3HJkjwHJr
h58rfZGiCy23Gl3QQ/veFaXF+DiTjzOajwrfj6Y3YczUjXuDboZtW2jNoAzVzQ7j
+HK7wJJuL+WYGbJDyEQeqP/c+05xPjR2fAMViIYXsvjrcyN2TNribUuPWQh8hX6k
EuiE3CkD964BIkYwKf5Ia9U=
-----END PRIVATE KEY-----
)PEM";

struct pem_files {
    std::string certificate;
    std::string private_key;

    pem_files() {
        const std::filesystem::path dir = std::filesystem::temp_directory_path();
        certificate = (dir / "crpc_test_tls_certificate.pem").string();
        private_key = (dir / "crpc_test_tls_private_key.pem").string();
        write(certificate, k_certificate_pem);
        write(private_key, k_private_key_pem);
    }

    static void write(const std::string& path, const char* data) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(data, static_cast<std::streamsize>(std::strlen(data)));
    }
};

const pem_files& certificates() {
    static const pem_files files;
    return files;
}

// --- In-memory byte pipe -----------------------------------------------------

struct wire {
    std::vector<std::byte> inbox;
};

class wire_sink final : public sink {
public:
    explicit wire_sink(wire& peer) noexcept : peer_(peer) {}

    void push(span<const std::byte> data, completion* c) noexcept override {
        if (!data.empty()) {
            peer_.inbox.insert(peer_.inbox.end(), data.begin(), data.end());
        }
        if (c != nullptr) {
            c->complete(io_result{{}, data.size()});
        }
    }

private:
    wire& peer_;
};

class inline_completion final : public completion {
public:
    void complete(io_result value) noexcept override {
        ++calls;
        result = value;
    }

    int calls = 0;
    io_result result{};
};

class recording_source final : public source {
public:
    std::size_t on_data(span<const std::byte> data) noexcept override {
        bytes.insert(bytes.end(), data.begin(), data.end());
        return data.size();
    }

    void on_eof() noexcept override { eof = true; }

    void on_error(std::error_code ec) noexcept override {
        error = ec;
        ++failures;
    }

    std::vector<std::byte> bytes;
    bool eof = false;
    std::error_code error{};
    int failures = 0;
};

class echo_source final : public source {
public:
    explicit echo_source(sink& out) noexcept : out_(&out) {}

    std::size_t on_data(span<const std::byte> data) noexcept override {
        bytes.insert(bytes.end(), data.begin(), data.end());
        if (out_ != nullptr) {
            out_->push(data, &completion_);
        }
        return data.size();
    }

    void on_eof() noexcept override { eof = true; }
    void on_error(std::error_code ec) noexcept override { error = ec; }

    std::vector<std::byte> bytes;
    bool eof = false;
    std::error_code error{};

private:
    sink* out_;
    inline_completion completion_;
};

// Accepts at most `credit` bytes per call.  Used to exercise the R1
// "short count pauses" path and the explicit resume_read() hook.
class trickle_source final : public source {
public:
    trickle_source(sink& out, std::size_t credit) noexcept : out_(&out), credit_(credit) {}

    std::size_t on_data(span<const std::byte> data) noexcept override {
        const std::size_t count = (std::min)(credit_, data.size());
        bytes.insert(bytes.end(), data.begin(),
                     data.begin() + static_cast<std::ptrdiff_t>(count));
        if (count != 0) {
            out_->push(span<const std::byte>(data.data(), count), &completion_);
        }
        return count;
    }

    void on_eof() noexcept override { eof = true; }
    void on_error(std::error_code ec) noexcept override { error = ec; }

    std::vector<std::byte> bytes;
    bool eof = false;
    std::error_code error{};

private:
    sink* out_;
    std::size_t credit_;
    inline_completion completion_;
};
// The two wires carry bytes in opposite directions: client_wire is fed by the
// server transport and delivered to the client TLS source; server_wire is fed
// by the client transport and delivered to the server TLS source.
struct harness {
    wire client_wire;
    wire server_wire;
    wire_sink client_transport;
    wire_sink server_transport;

    harness() noexcept : client_transport(server_wire), server_transport(client_wire) {}

    static void deliver(wire& incoming, source& target) {
        std::vector<std::byte> data;
        data.swap(incoming.inbox);
        std::size_t offset = 0;
        while (offset < data.size()) {
            const std::size_t used = target.on_data(span<const std::byte>(
                data.data() + offset, data.size() - offset));
            if (used == 0) {
                incoming.inbox.assign(data.begin() + static_cast<std::ptrdiff_t>(offset),
                                      data.end());
                return;
            }
            const std::size_t advance = (std::min)(used, data.size() - offset);
            offset += advance;
        }
    }

    template <class Predicate>
    bool run(source& client_source, source& server_source, Predicate predicate,
             int rounds = 4000) {
        for (int i = 0; i < rounds; ++i) {
            if (predicate()) {
                return true;
            }
            bool moved = false;
            if (!client_wire.inbox.empty()) {
                deliver(client_wire, client_source);
                moved = true;
            }
            if (!server_wire.inbox.empty()) {
                deliver(server_wire, server_source);
                moved = true;
            }
            if (!moved && client_wire.inbox.empty() && server_wire.inbox.empty()) {
                return predicate();
            }
        }
        return predicate();
    }
};

// --- Context helpers ---------------------------------------------------------

std::unique_ptr<tls_context> make_context(tls_role role, tls_verify_mode verify,
                                          const std::string& ca_bundle,
                                          tls_version minimum = tls_version::tls_1_2,
                                          tls_version maximum = tls_version::tls_1_3) {
    tls_config config;
    config.role = role;
    config.verify = verify;
    config.certificate_chain = certificates().certificate;
    config.private_key = certificates().private_key;
    config.ca_bundle = ca_bundle;
    config.min_version = minimum;
    config.max_version = maximum;

    std::error_code ec;
    std::unique_ptr<tls_context> context = tls_context::create(config, ec);
    CRPC_CHECK(!ec);
    CRPC_CHECK(context != nullptr);
    return context;
}

std::unique_ptr<tls_context> make_client_context(const std::string& ca_bundle,
                                                 tls_version minimum = tls_version::tls_1_2,
                                                 tls_version maximum = tls_version::tls_1_3) {
    return make_context(tls_role::client, tls_verify_mode::peer, ca_bundle, minimum,
                        maximum);
}

std::unique_ptr<tls_context> make_server_context() {
    return make_context(tls_role::server, tls_verify_mode::none, std::string());
}

span<const std::byte> as_span(const char* text) noexcept {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text),
                                 std::strlen(text));
}

// --- Tests -------------------------------------------------------------------

CRPC_TEST(handshake_echo_and_alpn) {
    std::unique_ptr<tls_context> client_context = make_client_context(certificates().certificate);
    std::unique_ptr<tls_context> server_context = make_server_context();
    if (!client_context || !server_context) {
        return;
    }

    harness pipe;
    std::error_code ec;
    std::unique_ptr<tls_stream> client =
        tls_stream::create(pipe.client_transport, *client_context, "localhost", ec);
    CRPC_CHECK(!ec);
    if (!client) {
        return;
    }
    std::unique_ptr<tls_stream> server =
        tls_stream::create(pipe.server_transport, *server_context, "", ec);
    CRPC_CHECK(!ec);
    if (!server) {
        return;
    }

    recording_source client_application;
    echo_source server_application(*server);
    client->bind_source(&client_application);
    server->bind_source(&server_application);

    const bool established = pipe.run(*client, *server, [&] {
        return client->handshake_complete() && server->handshake_complete();
    });
    CRPC_CHECK(established);
    CRPC_CHECK_EQ(std::string("h2"), client->alpn_protocol());
    CRPC_CHECK_EQ(std::string("h2"), server->alpn_protocol());
    CRPC_CHECK_FALSE(client->session_reused());
    CRPC_CHECK_FALSE(server->session_reused());

    const char payload[] = "hello over tls";
    inline_completion written;
    client->push(as_span(payload), &written);

    const bool echoed = pipe.run(*client, *server, [&] {
        return client_application.bytes.size() >= std::strlen(payload);
    });
    CRPC_CHECK(echoed);
    CRPC_CHECK_EQ(std::strlen(payload), client_application.bytes.size());
    CRPC_CHECK(std::memcmp(client_application.bytes.data(), payload,
                           std::strlen(payload)) == 0);
    CRPC_CHECK_EQ(std::strlen(payload), server_application.bytes.size());
    CRPC_CHECK_EQ(1, written.calls);
    CRPC_CHECK(written.result.ok());
}

CRPC_TEST(verification_failure) {
    std::unique_ptr<tls_context> client_context = make_client_context(certificates().certificate);
    std::unique_ptr<tls_context> server_context = make_server_context();
    if (!client_context || !server_context) {
        return;
    }

    harness pipe;
    std::error_code ec;
    // The certificate is valid for localhost; the client expects another name,
    // so host-name verification must fail.
    std::unique_ptr<tls_stream> client =
        tls_stream::create(pipe.client_transport, *client_context, "wrong.example", ec);
    CRPC_CHECK(!ec);
    if (!client) {
        return;
    }
    std::unique_ptr<tls_stream> server =
        tls_stream::create(pipe.server_transport, *server_context, "", ec);
    CRPC_CHECK(!ec);
    if (!server) {
        return;
    }

    recording_source application;
    client->bind_source(&application);

    const bool terminal = pipe.run(*client, *server, [&] {
        return client->handshake_failed() || application.failures != 0;
    });
    CRPC_CHECK(terminal);
    CRPC_CHECK(application.failures >= 1);
    CRPC_CHECK_EQ(make_error_code(tls_errc::ssl_verification_failed), application.error);
}

CRPC_TEST(clean_eof) {
    std::unique_ptr<tls_context> client_context = make_client_context(certificates().certificate);
    std::unique_ptr<tls_context> server_context = make_server_context();
    if (!client_context || !server_context) {
        return;
    }

    harness pipe;
    std::error_code ec;
    std::unique_ptr<tls_stream> client =
        tls_stream::create(pipe.client_transport, *client_context, "localhost", ec);
    std::unique_ptr<tls_stream> server =
        tls_stream::create(pipe.server_transport, *server_context, "", ec);
    if (!client || !server) {
        return;
    }

    recording_source application;
    client->bind_source(&application);
    recording_source server_application;
    server->bind_source(&server_application);

    CRPC_CHECK(pipe.run(*client, *server, [&] {
        return client->handshake_complete() && server->handshake_complete();
    }));

    server->close();
    const bool eof = pipe.run(*client, *server, [&] { return application.eof; });
    CRPC_CHECK(eof);
    CRPC_CHECK_EQ(0, application.failures);
    CRPC_CHECK_EQ(0, application.error.value());
}

CRPC_TEST(backpressure_and_resume) {
    std::unique_ptr<tls_context> client_context = make_client_context(certificates().certificate);
    std::unique_ptr<tls_context> server_context = make_server_context();
    if (!client_context || !server_context) {
        return;
    }

    harness pipe;
    std::error_code ec;
    std::unique_ptr<tls_stream> client =
        tls_stream::create(pipe.client_transport, *client_context, "localhost", ec);
    std::unique_ptr<tls_stream> server =
        tls_stream::create(pipe.server_transport, *server_context, "", ec);
    if (!client || !server) {
        CRPC_CHECK(false);
        return;
    }

    recording_source client_application;
    trickle_source server_application(*server, 4);
    client->bind_source(&client_application);
    server->bind_source(&server_application);

    CRPC_CHECK(pipe.run(*client, *server, [&] {
        return client->handshake_complete() && server->handshake_complete();
    }));

    const char payload[] = "0123456789abcdefghij";
    const std::size_t payload_size = std::strlen(payload);
    inline_completion written;
    client->push(as_span(payload), &written);

    // The first pass decrypts the record but the consumer accepts only four
    // bytes; the rest stays parked inside the decorator.
    CRPC_CHECK(pipe.run(*client, *server, [&] {
        return server_application.bytes.size() >= std::size_t{4};
    }));

    for (int i = 0; i < 64 && client_application.bytes.size() < payload_size; ++i) {
        server->resume_read();
        pipe.run(*client, *server, [&] { return false; }, 64);
    }

    CRPC_CHECK_EQ(payload_size, server_application.bytes.size());
    CRPC_CHECK_EQ(payload_size, client_application.bytes.size());
    CRPC_CHECK(std::memcmp(client_application.bytes.data(), payload,
                           payload_size) == 0);
    CRPC_CHECK_EQ(1, written.calls);
}

CRPC_TEST(truncated_record_is_an_error) {
    std::unique_ptr<tls_context> client_context = make_client_context(certificates().certificate);
    if (!client_context) {
        return;
    }

    harness pipe;
    std::error_code ec;
    std::unique_ptr<tls_stream> client =
        tls_stream::create(pipe.client_transport, *client_context, "localhost", ec);
    CRPC_CHECK(!ec);
    if (!client) {
        return;
    }

    recording_source application;
    client->bind_source(&application);

    // A TLS record header that claims 16 payload bytes, followed by three.
    const unsigned char partial[] = {0x16, 0x03, 0x03, 0x00, 0x10,
                                     0x01, 0x02, 0x03};
    client->on_data(span<const std::byte>(
        reinterpret_cast<const std::byte*>(partial), sizeof(partial)));
    client->on_eof();

    CRPC_CHECK(application.failures >= 1);
    CRPC_CHECK_FALSE(application.eof);
    const std::error_code expected_handshake =
        make_error_code(tls_errc::ssl_handshake_failed);
    const std::error_code expected_protocol =
        make_error_code(tls_errc::ssl_protocol_error);
    CRPC_CHECK(application.error == expected_handshake ||
               application.error == expected_protocol);
}

CRPC_TEST(session_resumption) {
    std::unique_ptr<tls_context> client_context =
        make_client_context(certificates().certificate, tls_version::tls_1_2,
                            tls_version::tls_1_2);
    std::unique_ptr<tls_context> server_context = make_server_context();
    if (!client_context || !server_context) {
        return;
    }

    // First connection: full handshake, one round trip, then close.  TLS 1.2 is
    // used deliberately because its session-id resumption is synchronous and
    // therefore deterministic in a test harness.
    {
        harness pipe;
        std::error_code ec;
        std::unique_ptr<tls_stream> client =
            tls_stream::create(pipe.client_transport, *client_context, "localhost", ec);
        std::unique_ptr<tls_stream> server =
            tls_stream::create(pipe.server_transport, *server_context, "", ec);
        if (!client || !server) {
            CRPC_CHECK(false);
            return;
        }
        recording_source client_application;
        echo_source server_application(*server);
        client->bind_source(&client_application);
        server->bind_source(&server_application);

        CRPC_CHECK(pipe.run(*client, *server, [&] {
            return client->handshake_complete() && server->handshake_complete();
        }));
        CRPC_CHECK_FALSE(client->session_reused());

        inline_completion written;
        client->push(as_span("session"), &written);
        CRPC_CHECK(pipe.run(*client, *server, [&] {
            return client_application.bytes.size() >= std::size_t{7};
        }));

        client->close();
        server->close();
        // The stream destructors store the latest session snapshot.
    }

    {
        harness pipe;
        std::error_code ec;
        std::unique_ptr<tls_stream> client =
            tls_stream::create(pipe.client_transport, *client_context, "localhost", ec);
        std::unique_ptr<tls_stream> server =
            tls_stream::create(pipe.server_transport, *server_context, "", ec);
        if (!client || !server) {
            CRPC_CHECK(false);
            return;
        }
        recording_source client_application;
        echo_source server_application(*server);
        client->bind_source(&client_application);
        server->bind_source(&server_application);

        CRPC_CHECK(pipe.run(*client, *server, [&] {
            return client->handshake_complete() && server->handshake_complete();
        }));
        CRPC_CHECK(client->session_reused());
        CRPC_CHECK(server->session_reused());
    }
}

CRPC_TEST(certificate_reload) {
    const pem_files& files = certificates();
    tls_config config;
    config.role = tls_role::server;
    config.verify = tls_verify_mode::none;
    config.certificate_chain = files.certificate;
    config.private_key = files.private_key;

    std::error_code ec;
    std::unique_ptr<tls_context> context = tls_context::create(config, ec);
    CRPC_CHECK(!ec);
    if (!context) {
        return;
    }
    CRPC_CHECK(context->valid());

    // A bad replacement must leave the old context intact.
    tls_config broken = config;
    broken.certificate_chain = (std::filesystem::temp_directory_path() /
                                "crpc_test_tls_missing_certificate.pem")
                                   .string();
    CRPC_CHECK_FALSE(context->reload(broken, ec));
    CRPC_CHECK(static_cast<bool>(ec));
    CRPC_CHECK(context->valid());

    // A good replacement is swapped in atomically; new handshakes use it.
    CRPC_CHECK(context->reload(config, ec));
    CRPC_CHECK(!ec);

    std::unique_ptr<tls_context> client_context =
        make_client_context(files.certificate);
    if (!client_context) {
        return;
    }
    harness pipe;
    std::unique_ptr<tls_stream> client =
        tls_stream::create(pipe.client_transport, *client_context, "localhost", ec);
    std::unique_ptr<tls_stream> server =
        tls_stream::create(pipe.server_transport, *context, "", ec);
    if (!client || !server) {
        CRPC_CHECK(false);
        return;
    }
    recording_source client_application;
    client->bind_source(&client_application);
    CRPC_CHECK(pipe.run(*client, *server, [&] {
        return client->handshake_complete() && server->handshake_complete();
    }));
}

CRPC_TEST(paired_decorator_entry_points) {
    std::unique_ptr<tls_context> client_context =
        make_client_context(certificates().certificate);
    std::unique_ptr<tls_context> server_context = make_server_context();
    if (!client_context || !server_context) {
        return;
    }

    harness pipe;
    std::error_code ec;
    std::unique_ptr<tls_stream> client =
        tls_stream::create(pipe.client_transport, *client_context, "localhost", ec);
    if (!client) {
        CRPC_CHECK(false);
        return;
    }

    std::unique_ptr<source> server_source =
        make_tls_source(pipe.server_transport, *server_context, "");
    std::unique_ptr<sink> server_sink =
        make_tls_sink(pipe.server_transport, *server_context, "");
    CRPC_CHECK(server_source != nullptr);
    CRPC_CHECK(server_sink != nullptr);
    if (!server_source || !server_sink) {
        return;
    }

    echo_source server_application(*server_sink);
    bind_tls_source(*server_source, &server_application);

    recording_source client_application;
    client->bind_source(&client_application);

    CRPC_CHECK(pipe.run(*client, *server_source, [&] {
        return client->handshake_complete();
    }));

    inline_completion written;
    client->push(as_span("decorated"), &written);
    CRPC_CHECK(pipe.run(*client, *server_source, [&] {
        return client_application.bytes.size() >= std::size_t{9};
    }));
    CRPC_CHECK_EQ(std::size_t{9}, client_application.bytes.size());
    CRPC_CHECK_EQ(1, written.calls);
}

} // namespace

#endif // CRPC_HAVE_OPENSSL

CRPC_TEST_MAIN()
