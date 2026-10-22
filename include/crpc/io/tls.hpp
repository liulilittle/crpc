// CRPC -- TLS decorator for the two-interface transport contract (R1/R5).
//
// This header is deliberately free of OpenSSL types.  Everything that needs
// <openssl/ssl.h> lives in src/io/tls_openssl.cpp behind
// `#if defined(CRPC_HAVE_OPENSSL)`; when the macro is absent the entry points
// still exist but return "not available" so that a program can be written once
// and built in both configurations.
//
// Direction of the two halves
// ---------------------------
// A TLS connection is a byte transform between the wire and the application:
//
//   transport source --ciphertext--> tls_stream::on_data --> plaintext
//   plaintext --> tls_stream::push --> ciphertext --> transport sink
//
// `tls_stream` implements both io::source and io::sink.  The host wires it as:
//
//   auto stream = tls_stream::create(connection.sink(), context, "host", ec);
//   connection.bind_source(stream.get());   // backend delivers ciphertext here
//   stream->bind_source(&application_source); // plaintext leaves here
//   application pushes plaintext through stream->push(...)
//
// `make_tls_source` / `make_tls_sink` are the paired decorator entry points.
// Each call returns one half of the *same* session, keyed by the transport sink
// address and host name inside the context.  Call both with the same live
// transport (either order), install the source on the transport, bind the
// plaintext consumer with `bind_tls_source`, and use the sink for plaintext.
// `tls_stream` is the simpler one-object form and is what new code should use.
//
// Handshake state machine
// -----------------------
//   idle -> handshaking -> established -> closed
//                |                     \-> failed
//                +----------> failed
//
// `close()` moves any live state to closed; a transport or TLS failure moves
// the stream to failed, from which close() still releases resources.  A clean
// peer close_notify also ends in closed.  Every transition is driven by
// `SSL_do_handshake`; a return of
// SSL_ERROR_WANT_READ parks the state until the next `on_data`, and
// SSL_ERROR_WANT_WRITE parks it until the transport write completion.  There is
// no spin loop and no blocking call on any path.  A client starts its
// ClientHello when the stream is created; a server waits for the first record.
//
// Backpressure
// ------------
// The memory BIO is bounded (256 KiB of undecrypted input).  `on_data` returns
// a short count or 0 once that bound is reached, which is the R1 pause signal.
// Decrypted output is delivered to the bound application source; if that source
// returns a short count or 0, the remainder is retained and `resume_read()`
// retries it.  Plaintext that arrives before a consumer is bound is retained
// (bounded) rather than dropped.
//
// Resume wiring: after a 0 credit the host calls `tls_stream::resume_read()`
// (or `tls_resume_read()` for the paired source half) to drain retained
// plaintext, and the underlying `connection::resume_read()` to ask the backend
// to redeliver ciphertext retained by the transport.  Both are idempotent.
//
// Errors
// ------
//   SSL_ERROR_ZERO_RETURN              -> application source on_eof()
//   SSL_ERROR_SYSCALL with no error    -> application source on_eof()
//   certificate / hostname failure     -> on_error(tls_errc::ssl_verification_failed)
//   any other SSL failure              -> on_error(ssl_protocol_error /
//                                                    ssl_handshake_failed)
//   transport failure                  -> on_error(transport error code)
// The terminal source callback runs at most once.  After close(), no new
// source or completion callback starts.
//
// Session reuse and reload
// ------------------------
// The client caches at most `session_cache_entries` SSL_SESSION objects per
// context (FIFO eviction, default 1024, keyed by host name).  The server uses
// OpenSSL's session-id cache with the same bound.  `tls_context::reload()`
// builds a complete new SSL_CTX and swaps the shared pointer under a mutex;
// streams created before the swap keep the old context (OpenSSL reference
// counts it) and finish their handshakes undisturbed, while new streams see
// the new credentials.
//
// Thread safety
// -------------
// A context may be shared by any number of threads; `create`, `reload` and the
// session cache are internally synchronized.  A stream is one reader plus one
// writer: `on_data`/`on_eof`/`on_error` serialized on the reader side and
// `push` on the writer side may run concurrently.  State and every SSL_* call
// are protected by a mutex; application callbacks run outside it.  A stream is
// never used after both transport and application dropped it, and a transport
// completion that arrives late is harmless because it owns its ciphertext
// block.
#ifndef CRPC_IO_TLS_HPP
#define CRPC_IO_TLS_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

#include <crpc/io.hpp>

namespace crpc::io {

class tls_stream;

// Internal accessor used by the paired decorator entry points.  It is declared
// here only so that it can be a friend of tls_context; no user names it.
struct tls_context_access;

// Which side of the handshake this context creates.
enum class tls_role : std::uint8_t { client, server };

// `peer` verifies the peer certificate and, on a client, the host name.
// A server never requests a client certificate in v1 (mTLS is out of scope);
// the mode is therefore only meaningful for clients.
enum class tls_verify_mode : std::uint8_t { none, peer };

// TLS 1.2 and 1.3 are the only versions in scope (specification section 12.3).
enum class tls_version : std::uint8_t { tls_1_2, tls_1_3 };

enum class tls_errc : int {
    ok = 0,
    ssl_configuration_error = 1,
    ssl_handshake_failed = 2,
    ssl_verification_failed = 3,
    ssl_protocol_error = 4,
    ssl_alpn_mismatch = 5,
    ssl_closed = 6,
    ssl_not_available = 7
};

[[nodiscard]] CRPC_API const std::error_category& tls_error_category() noexcept;
[[nodiscard]] CRPC_API std::error_code make_error_code(tls_errc code) noexcept;

// Configuration snapshot.  Paths are read when the context is created or
// reloaded; the strings themselves are not consulted again, so a reload that
// points at a new file is the supported certificate hot-swap path.
struct tls_config {
    tls_role role = tls_role::client;
    tls_verify_mode verify = tls_verify_mode::peer;

    // Server credential files.  A server requires both.
    std::string certificate_chain;
    std::string private_key;

    // Trust anchors.  When empty and verification is enabled, OpenSSL's
    // default trust store is used.
    std::string ca_bundle;

    // ALPN preference list.  The server selects the first entry it offers that
    // the client also offers; the client asserts that the negotiated value is
    // one of the protocols it sent.  The specification requires h2 and
    // http/1.1, which is the default.
    std::vector<std::string> alpn_protocols{"h2", "http/1.1"};
    bool require_alpn = true;

    bool session_reuse = true;
    std::size_t session_cache_entries = 1024;

    tls_version min_version = tls_version::tls_1_2;
    tls_version max_version = tls_version::tls_1_3;
};

// An immutable, thread-safe credential set.  Creation and reload are the only
// operations that touch files; connection setup only reads shared state.
class CRPC_API tls_context {
public:
    // The definition lives in the translation unit.  It is declared here so
    // that the out-of-line code can name it; no OpenSSL type crosses this
    // boundary.
    struct impl;

    [[nodiscard]] static std::unique_ptr<tls_context> create(
        const tls_config& config, std::error_code& ec) noexcept;

    ~tls_context();

    tls_context(const tls_context&) = delete;
    tls_context& operator=(const tls_context&) = delete;
    tls_context(tls_context&&) = delete;
    tls_context& operator=(tls_context&&) = delete;

    // Builds a complete replacement context (including certificate files) and
    // swaps it in.  On failure the old context stays active and `ec` is set.
    // Live streams are not disturbed.
    [[nodiscard]] bool reload(const tls_config& config, std::error_code& ec) noexcept;

    [[nodiscard]] tls_config config() const;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool server_side() const noexcept;

private:
    explicit tls_context(std::shared_ptr<impl> impl) noexcept;

    std::shared_ptr<impl> impl_;

    friend class tls_stream;
    friend struct tls_context_access;
    friend CRPC_API std::unique_ptr<source> make_tls_source(io::sink& transport,
                                                           tls_context& context,
                                                           std::string hostname);
    friend CRPC_API std::unique_ptr<sink> make_tls_sink(io::sink& transport,
                                                       tls_context& context,
                                                       std::string hostname);
};

// A TLS endpoint implementing both halves of the R1 contract.  It owns the SSL
// object, both memory BIOs and the bounded encrypt/decrypt queues.
class CRPC_API tls_stream final : public source, public sink {
public:
    [[nodiscard]] static std::unique_ptr<tls_stream> create(
        io::sink& transport, tls_context& context, std::string hostname,
        std::error_code& ec) noexcept;

    ~tls_stream() override;

    tls_stream(const tls_stream&) = delete;
    tls_stream& operator=(const tls_stream&) = delete;
    tls_stream(tls_stream&&) = delete;
    tls_stream& operator=(tls_stream&&) = delete;

    // Backend -> framework.  `data` is ciphertext.  Returns the number of
    // ciphertext bytes accepted (0 pauses when the input bound is reached).
    std::size_t on_data(span<const std::byte> data) noexcept override;
    void on_eof() noexcept override;
    void on_error(std::error_code ec) noexcept override;

    // Framework -> backend.  `data` is plaintext; the completion runs exactly
    // once, after the encrypted bytes have been accepted by the transport.
    void push(span<const std::byte> data, completion* c) noexcept override;

    // Installs the plaintext consumer.  While it returns 0, decrypted bytes are
    // retained (bounded) and no further SSL_read is issued.
    void bind_source(source* plaintext) noexcept;

    // Retries the retained plaintext and resumes reading.  The host calls this
    // when its consumer can accept more; a new on_data also retries.
    void resume_read() noexcept;

    // Sends close_notify (best effort) and rejects all later pushes with
    // operation_canceled.  Idempotent.
    void close() noexcept;

    [[nodiscard]] bool handshake_complete() const noexcept;
    [[nodiscard]] bool handshake_failed() const noexcept;
    [[nodiscard]] bool session_reused() const noexcept;
    [[nodiscard]] std::string alpn_protocol() const;
    [[nodiscard]] std::error_code error() const noexcept;

private:
    class impl;
    explicit tls_stream(std::shared_ptr<impl> impl) noexcept;

    std::shared_ptr<impl> impl_;
};

// --- Paired decorator entry points ------------------------------------------
//
// These create (or find) one shared session per (transport sink, host name).
// Both calls must be made for the same live transport, with exactly the same
// host name string, and the returned halves must be kept for the connection's
// lifetime (do not reuse the transport sink address while a half is alive).
// `make_tls_source` is the
// transport-facing source (install it with connection::bind_source);
// `make_tls_sink` is the application-facing sink.  `bind_tls_source` attaches
// the plaintext consumer to the source half.  The functions return nullptr when
// the context is not usable or allocation fails.
//
// `make_tls_source` and `make_tls_sink` are also friend declarations inside
// `tls_context`. A friend declaration introduces the name into the enclosing
// namespace, so the decoration must agree with these declarations: marking
// only one side of the pair produces C2375 (redefinition; different linkage)
// on MSVC. Both sides therefore carry CRPC_API.
[[nodiscard]] CRPC_API std::unique_ptr<source> make_tls_source(io::sink& transport,
                                                              tls_context& context,
                                                              std::string hostname);
[[nodiscard]] CRPC_API std::unique_ptr<sink> make_tls_sink(io::sink& transport,
                                                           tls_context& context,
                                                           std::string hostname);

// No-op when the source was not produced by `make_tls_source`.
CRPC_API void bind_tls_source(source& tls_source, source* plaintext) noexcept;
CRPC_API void tls_resume_read(source& tls_source) noexcept;

} // namespace crpc::io

namespace std {
template <>
struct is_error_code_enum<crpc::io::tls_errc> : true_type {};
} // namespace std

#endif // CRPC_IO_TLS_HPP
