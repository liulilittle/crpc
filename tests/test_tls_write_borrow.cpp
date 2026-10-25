// CRPC -- TLS write-path borrow/materialise tests (D3a).
//
// tls_stream::push() now borrows the caller's plaintext span for the common
// synchronous write instead of copying it into write_buffer_. The contract ("the
// caller may reuse the span afterwards") is preserved by materialising the
// unwritten remainder into write_buffer_ before push() returns. These cases
// drive that materialise path: a transport that defers its completion forces the
// plaintext write to park after the first chunk, so push() must copy the rest.
//
// Without CRPC_HAVE_OPENSSL the only case prints the documented skip line and
// exits zero, exactly like test_tls.cpp.
#include <crpc/io/tls.hpp>

#include "test_framework.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if !defined(CRPC_HAVE_OPENSSL)

CRPC_TEST(tls_write_borrow_openssl_not_available) {
    std::printf("SKIP test_tls_write_borrow: OpenSSL not available\n");
}

#else

#include <atomic>
#include <filesystem>
#include <fstream>

// --- Fault injection for the materialise OOM path (D6) -----------------------
//
// The test executable replaces the global allocation functions, the same pattern
// as test_frame_compression.cpp / test_observability.cpp. Armed with a size
// threshold, every allocation at or above it throws bad_alloc. This makes
// materialise_borrowed_locked()'s copy of a large parked write fail
// deterministically without exhausting real memory. The threshold is only armed
// around a single push(); the default of 0 disables the fault.
namespace {

std::atomic<std::size_t> g_fail_allocation_at{0};

} // namespace

void* operator new(std::size_t size) {
    const std::size_t threshold =
        g_fail_allocation_at.load(std::memory_order_acquire);
    if (threshold != 0 && size >= threshold) {
        throw std::bad_alloc();
    }
    if (void* pointer = std::malloc(size == 0 ? 1 : size)) {
        return pointer;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

namespace {

using namespace crpc;
using namespace crpc::io;

// --- Test certificate (same self-signed CN=localhost pair as test_tls.cpp) ----

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
        certificate = (dir / "crpc_test_tls_write_borrow_certificate.pem").string();
        private_key = (dir / "crpc_test_tls_write_borrow_private_key.pem").string();
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

// Transport sink that can defer its completion. While paused it still forwards
// the ciphertext but holds the completion, so the TLS layer's one-in-flight
// transport slot stays busy and the plaintext write parks.
class pausable_sink final : public sink {
public:
    explicit pausable_sink(wire& peer) noexcept : peer_(peer) {}

    void push(span<const std::byte> data, completion* c) noexcept override {
        if (!data.empty()) {
            peer_.inbox.insert(peer_.inbox.end(), data.begin(), data.end());
        }
        accepted_ += data.size();
        if (paused_.load(std::memory_order_acquire)) {
            pending_ = c;
            pending_bytes_ = data.size();
        } else if (c != nullptr) {
            c->complete(io_result{{}, data.size()});
        }
    }

    void set_paused(bool value) noexcept {
        paused_.store(value, std::memory_order_release);
    }
    [[nodiscard]] bool has_pending() const noexcept { return pending_ != nullptr; }

    // Total ciphertext bytes this sink accepted, paused or not. A stalled
    // transport accepts at most one in-flight record, which is the observable
    // half of the decorator's encrypted-buffer backpressure bound.
    [[nodiscard]] std::size_t accepted() const noexcept { return accepted_; }

    // Completes the deferred transport write (if any). Safe to call when none is
    // pending.
    void resume() noexcept {
        paused_.store(false, std::memory_order_release);
        completion* c = pending_;
        const std::size_t bytes = pending_bytes_;
        pending_ = nullptr;
        pending_bytes_ = 0;
        if (c != nullptr) {
            c->complete(io_result{{}, bytes});
        }
    }

private:
    wire& peer_;
    std::atomic<bool> paused_{false};
    completion* pending_ = nullptr;
    std::size_t pending_bytes_ = 0;
    std::size_t accepted_ = 0;
};

class recording_source final : public source {
public:
    std::size_t on_data(span<const std::byte> data) noexcept override {
        bytes.insert(bytes.end(), data.begin(), data.end());
        return data.size();
    }
    void on_eof() noexcept override { eof = true; }
    void on_error(std::error_code ec) noexcept override { error = ec; }
    std::vector<std::byte> bytes;
    bool eof = false;
    std::error_code error{};
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

// Delivers one wire's inbox to `target` until the target stops consuming.
void deliver(wire& incoming, source& target) {
    std::vector<std::byte> data;
    data.swap(incoming.inbox);
    std::size_t offset = 0;
    while (offset < data.size()) {
        const std::size_t used = target.on_data(
            span<const std::byte>(data.data() + offset, data.size() - offset));
        if (used == 0) {
            incoming.inbox.assign(data.begin() + static_cast<std::ptrdiff_t>(offset),
                                  data.end());
            return;
        }
        offset += (std::min)(used, data.size() - offset);
    }
}

std::unique_ptr<tls_context> make_context(tls_role role, tls_verify_mode verify,
                                          const std::string& ca_bundle) {
    tls_config config;
    config.role = role;
    config.verify = verify;
    config.certificate_chain = certificates().certificate;
    config.private_key = certificates().private_key;
    config.ca_bundle = ca_bundle;
    config.min_version = tls_version::tls_1_2;
    config.max_version = tls_version::tls_1_3;

    std::error_code ec;
    std::unique_ptr<tls_context> context = tls_context::create(config, ec);
    CRPC_CHECK(!ec);
    CRPC_CHECK(context != nullptr);
    return context;
}

// Runs the handshake to completion, pumping both wires.
bool complete_handshake(wire& client_wire, wire& server_wire, tls_stream& client,
                        tls_stream& server) {
    for (int i = 0; i < 4000; ++i) {
        if (client.handshake_complete() && server.handshake_complete()) {
            return true;
        }
        bool moved = false;
        if (!client_wire.inbox.empty()) {
            deliver(client_wire, client);
            moved = true;
        }
        if (!server_wire.inbox.empty()) {
            deliver(server_wire, server);
            moved = true;
        }
        if (!moved && client_wire.inbox.empty() && server_wire.inbox.empty()) {
            return client.handshake_complete() && server.handshake_complete();
        }
    }
    return client.handshake_complete() && server.handshake_complete();
}

// A payload larger than one TLS record so a parked write has a substantial
// unwritten remainder to materialise.
constexpr std::size_t k_parked_payload = 512 * 1024;

// A client/server pair whose client transport is paused and whose large
// plaintext write is parked mid-flight. push() already materialised the
// unwritten remainder into write_buffer_, so write_completion_ is still set:
// exactly the state a terminal notification must cancel (D5), the state the
// encrypted-buffer bound holds (D7), and the state a materialise OOM aborts
// (D6).
struct parked_client {
    wire client_wire;
    wire server_wire;
    pausable_sink client_transport;   // client -> server
    pausable_sink server_transport;   // server -> client
    std::unique_ptr<tls_context> client_context;
    std::unique_ptr<tls_context> server_context;
    std::unique_ptr<tls_stream> client;
    std::unique_ptr<tls_stream> server;
    recording_source server_application;
    inline_completion written;
    std::vector<std::byte> payload;

    parked_client()
        : client_transport(server_wire), server_transport(client_wire) {
        client_context = make_context(tls_role::client, tls_verify_mode::peer,
                                      certificates().certificate);
        server_context = make_context(tls_role::server, tls_verify_mode::none,
                                      std::string());
        if (!client_context || !server_context) {
            return;
        }

        std::error_code ec;
        client = tls_stream::create(client_transport, *client_context,
                                    "localhost", ec);
        CRPC_CHECK(!ec);
        server = tls_stream::create(server_transport, *server_context, "", ec);
        CRPC_CHECK(!ec);
        if (!client || !server) {
            return;
        }

        server->bind_source(&server_application);
        CRPC_CHECK(complete_handshake(client_wire, server_wire, *client, *server));

        payload.resize(k_parked_payload);
        for (std::size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<std::byte>(i * 17u + 3u);
        }

        client_transport.set_paused(true);
        client->push(span<const std::byte>(payload.data(), payload.size()),
                     &written);
    }
};

} // namespace

// A large client write while the transport defers its completion: the write
// parks after the first chunk and push() must materialise the unwritten
// remainder into write_buffer_ before it returns. The plaintext span is then
// deliberately clobbered, and the peer must still receive the original bytes
// intact and the completion must report the full length.
CRPC_TEST(tls_write_borrow_parked_materializes) {
    std::unique_ptr<tls_context> client_context =
        make_context(tls_role::client, tls_verify_mode::peer,
                     certificates().certificate);
    std::unique_ptr<tls_context> server_context =
        make_context(tls_role::server, tls_verify_mode::none, std::string());
    if (!client_context || !server_context) {
        return;
    }

    wire client_wire;
    wire server_wire;
    pausable_sink client_transport(server_wire);   // client -> server
    pausable_sink server_transport(client_wire);   // server -> client

    std::error_code ec;
    std::unique_ptr<tls_stream> client =
        tls_stream::create(client_transport, *client_context, "localhost", ec);
    CRPC_CHECK(!ec);
    if (!client) {
        return;
    }
    std::unique_ptr<tls_stream> server =
        tls_stream::create(server_transport, *server_context, "", ec);
    CRPC_CHECK(!ec);
    if (!server) {
        return;
    }

    recording_source client_application;
    recording_source server_application;
    client->bind_source(&client_application);
    server->bind_source(&server_application);

    CRPC_CHECK(complete_handshake(client_wire, server_wire, *client, *server));

    // A payload larger than the decorator's encrypted backlog bound
    // (k_max_encrypted, 256 KiB). With the transport paused the ciphertext
    // backlog fills, SSL_write stops being called, and the plaintext write must
    // park; push() then has to copy the unwritten remainder out of the caller's
    // span.
    constexpr std::size_t k_payload = 512 * 1024;
    std::vector<std::byte> payload(k_payload);
    for (std::size_t i = 0; i < k_payload; ++i) {
        payload[i] = static_cast<std::byte>(i * 31u + 7u);
    }

    client_transport.set_paused(true);
    inline_completion written;
    client->push(span<const std::byte>(payload.data(), payload.size()), &written);

    // The write parked: the completion has not run, and the transport holds the
    // first chunk's completion.
    CRPC_CHECK_EQ(written.calls, 0);
    CRPC_CHECK(client_transport.has_pending());

    // Clobber the caller's buffer. If push() had kept borrowing it, the rest of
    // the frame would be corrupted; materialise_borrowed_locked() must have
    // copied it out already.
    std::fill(payload.begin(), payload.end(), std::byte{0xFF});
    payload.clear();
    payload.shrink_to_fit();

    // Release the transport and pump until the server has all the plaintext.
    client_transport.resume();
    for (int i = 0; i < 8000; ++i) {
        if (server_application.bytes.size() >= k_payload && written.calls == 1) {
            break;
        }
        if (!server_wire.inbox.empty()) {
            deliver(server_wire, *server);
        }
        if (!client_wire.inbox.empty()) {
            deliver(client_wire, *client);
        }
        // Nudge the decorator: the write is driven by transport completions,
        // which in this manual pipe only happen when we pump.
        client->resume_read();
    }

    CRPC_CHECK_EQ(written.calls, 1);
    CRPC_CHECK(written.result.ok());
    CRPC_CHECK_EQ(written.result.bytes, k_payload);
    CRPC_CHECK_EQ(server_application.bytes.size(), k_payload);
    if (server_application.bytes.size() == k_payload) {
        bool same = true;
        for (std::size_t i = 0; i < k_payload; ++i) {
            if (server_application.bytes[i] != static_cast<std::byte>(i * 31u + 7u)) {
                same = false;
                break;
            }
        }
        CRPC_CHECK(same);
    }
}

// A small write completes synchronously out of the borrowed span: the completion
// fires inside push() and the peer receives the exact bytes.
CRPC_TEST(tls_write_borrow_synchronous) {
    std::unique_ptr<tls_context> client_context =
        make_context(tls_role::client, tls_verify_mode::peer,
                     certificates().certificate);
    std::unique_ptr<tls_context> server_context =
        make_context(tls_role::server, tls_verify_mode::none, std::string());
    if (!client_context || !server_context) {
        return;
    }

    wire client_wire;
    wire server_wire;
    pausable_sink client_transport(server_wire);
    pausable_sink server_transport(client_wire);

    std::error_code ec;
    std::unique_ptr<tls_stream> client =
        tls_stream::create(client_transport, *client_context, "localhost", ec);
    CRPC_CHECK(!ec);
    if (!client) {
        return;
    }
    std::unique_ptr<tls_stream> server =
        tls_stream::create(server_transport, *server_context, "", ec);
    CRPC_CHECK(!ec);
    if (!server) {
        return;
    }

    recording_source server_application;
    server->bind_source(&server_application);
    CRPC_CHECK(complete_handshake(client_wire, server_wire, *client, *server));

    const char text[] = "borrowed plaintext";
    inline_completion written;
    client->push(span<const std::byte>(
                     reinterpret_cast<const std::byte*>(text), sizeof(text) - 1),
                 &written);
    CRPC_CHECK_EQ(written.calls, 1);
    CRPC_CHECK(written.result.ok());
    CRPC_CHECK_EQ(written.result.bytes, sizeof(text) - 1);

    for (int i = 0; i < 4000 && server_application.bytes.size() < sizeof(text) - 1;
         ++i) {
        if (!server_wire.inbox.empty()) {
            deliver(server_wire, *server);
        }
    }
    CRPC_CHECK_EQ(server_application.bytes.size(), sizeof(text) - 1);
    CRPC_CHECK(std::memcmp(server_application.bytes.data(), text,
                           sizeof(text) - 1) == 0);
}

// --- D5: a terminal notification cancels a parked borrowed write ------------

// close() must complete a parked write exactly once with operation_canceled.
CRPC_TEST(tls_write_borrow_cancel_on_close) {
    parked_client p;
    if (!p.client) {
        return;
    }
    CRPC_CHECK_EQ(p.written.calls, 0);
    p.client->close();
    CRPC_CHECK_EQ(p.written.calls, 1);
    CRPC_CHECK(p.written.result.ec ==
               std::make_error_code(std::errc::operation_canceled));
    // Exactly once: a later drive must not run it again.
    p.client->resume_read();
    CRPC_CHECK_EQ(p.written.calls, 1);
}

// on_eof() must complete a parked write exactly once with operation_canceled.
CRPC_TEST(tls_write_borrow_cancel_on_eof) {
    parked_client p;
    if (!p.client) {
        return;
    }
    CRPC_CHECK_EQ(p.written.calls, 0);
    // The transport reports EOF while the read BIO is empty: the parked write
    // must be canceled exactly once with operation_canceled.
    p.client->on_eof();
    CRPC_CHECK_EQ(p.written.calls, 1);
    CRPC_CHECK(p.written.result.ec ==
               std::make_error_code(std::errc::operation_canceled));
    p.client->resume_read();
    CRPC_CHECK_EQ(p.written.calls, 1);
}

// on_error() must complete a parked write exactly once with the injected error.
CRPC_TEST(tls_write_borrow_cancel_on_error) {
    parked_client p;
    if (!p.client) {
        return;
    }
    CRPC_CHECK_EQ(p.written.calls, 0);
    const std::error_code injected =
        std::make_error_code(std::errc::connection_reset);
    p.client->on_error(injected);
    CRPC_CHECK_EQ(p.written.calls, 1);
    CRPC_CHECK(p.written.result.ec == injected);
    p.client->resume_read();
    CRPC_CHECK_EQ(p.written.calls, 1);
}

// --- D6: the materialise OOM path completes the parked write exactly once ----

CRPC_TEST(tls_write_borrow_materialize_oom_completes_once) {
#if defined(CRPC_SHARED)
    // The executable's operator new replacement only intercepts allocations
    // made by the test image; a shared library resolves operator new inside its
    // own CRT, so the fault would never reach materialise_borrowed_locked().
    crpc_test::skip(
        "shared build: the library's allocations are not intercepted");
#else
    std::unique_ptr<tls_context> client_context =
        make_context(tls_role::client, tls_verify_mode::peer,
                     certificates().certificate);
    std::unique_ptr<tls_context> server_context =
        make_context(tls_role::server, tls_verify_mode::none, std::string());
    if (!client_context || !server_context) {
        return;
    }

    wire client_wire;
    wire server_wire;
    pausable_sink client_transport(server_wire);
    pausable_sink server_transport(client_wire);

    std::error_code ec;
    std::unique_ptr<tls_stream> client =
        tls_stream::create(client_transport, *client_context, "localhost", ec);
    CRPC_CHECK(!ec);
    std::unique_ptr<tls_stream> server =
        tls_stream::create(server_transport, *server_context, "", ec);
    CRPC_CHECK(!ec);
    if (!client || !server) {
        return;
    }

    recording_source server_application;
    server->bind_source(&server_application);
    CRPC_CHECK(complete_handshake(client_wire, server_wire, *client, *server));

    // Allocated before the fault is armed.
    std::vector<std::byte> payload(k_parked_payload);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>(i * 7u + 1u);
    }

    client_transport.set_paused(true);
    inline_completion written;

    // Only allocations at or above half the payload fail. The first ~16 KiB
    // ciphertext record and its tx_block are far below this, but the
    // materialise copy of the ~496 KiB remainder is not.
    g_fail_allocation_at.store(k_parked_payload / 2, std::memory_order_release);
    client->push(span<const std::byte>(payload.data(), payload.size()), &written);
    g_fail_allocation_at.store(0, std::memory_order_release);

    // The parked write was completed exactly once, with not_enough_memory.
    CRPC_CHECK_EQ(written.calls, 1);
    CRPC_CHECK(written.result.ec ==
               std::make_error_code(std::errc::not_enough_memory));

    // Exactly once: a later drive must not run it again.
    client->resume_read();
    CRPC_CHECK_EQ(written.calls, 1);
#endif
}

// --- D7: the encrypted-buffer backpressure bound ----------------------------

// With the transport stalled, repeated drive attempts fill the decorator's
// encrypted_buffer_ until it reaches k_max_encrypted (256 KiB) and
// try_write_locked() parks instead of encrypting more. The write must stay
// parked (the completion must not run) and the transport must have accepted at
// most the single in-flight record; releasing the transport then drains the
// whole backlog and completes the write exactly once.
CRPC_TEST(tls_write_borrow_encrypted_backlog_parks) {
    parked_client p;
    if (!p.client) {
        return;
    }
    CRPC_CHECK_EQ(p.written.calls, 0);

    // Drive the write until the encrypted backlog reaches its bound. Each
    // drive pass encrypts one 16 KiB record that the stalled transport cannot
    // drain, so the bound is reached after ~k_max_encrypted / k_write_chunk
    // passes and the following pass takes the park branch.
    for (int i = 0; i < 64; ++i) {
        p.client->resume_read();
    }

    // Backpressure held: the write did not complete and only one record ever
    // reached the transport.
    CRPC_CHECK_EQ(p.written.calls, 0);
    CRPC_CHECK(p.client_transport.accepted() <= 64 * 1024);

    // Release the transport: the backlog drains and the full plaintext arrives.
    p.client_transport.resume();
    for (int i = 0; i < 8000; ++i) {
        if (p.server_application.bytes.size() >= k_parked_payload &&
            p.written.calls == 1) {
            break;
        }
        if (!p.server_wire.inbox.empty()) {
            deliver(p.server_wire, *p.server);
        }
        if (!p.client_wire.inbox.empty()) {
            deliver(p.client_wire, *p.client);
        }
        p.client->resume_read();
    }

    CRPC_CHECK_EQ(p.written.calls, 1);
    CRPC_CHECK(p.written.result.ok());
    CRPC_CHECK_EQ(p.written.result.bytes, k_parked_payload);
    CRPC_CHECK_EQ(p.server_application.bytes.size(), k_parked_payload);
}

#endif // CRPC_HAVE_OPENSSL

CRPC_TEST_MAIN()
