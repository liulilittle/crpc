// CRPC -- a small HTTP/1.1 client.
//
// The library had a server and a message parser but no client, and the REST adapters (HBase
// first) need one. This is deliberately the smallest client that is still correct:
//
//   * one request per connection (`Connection: close`), so there is no keep-alive state
//     machine to get wrong and no risk of reading a previous response's tail;
//   * `Content-Length` and `Transfer-Encoding: chunked` are both handled, because a server
//     may answer either way and a client that handles only one truncates or hangs;
//   * the response body is bounded, so a hostile or broken server cannot exhaust memory.
//
// It does not follow redirects: an adapter that silently followed one could send a request
// (and its credentials) to a host the caller never named.
#ifndef CRPC_HTTP_CLIENT_HPP
#define CRPC_HTTP_CLIENT_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/db/channel.hpp>
#include <crpc/error.hpp>
#include <crpc/http/message.hpp>
#include <crpc/io.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace http {

struct client_options {
    // Sent as `Host`. The port is appended when it is not 80, which is what makes a
    // virtual host work behind a non-default port.
    std::string host;
    std::uint16_t port = 80;

    // Extra fields sent with every request (an authorization header, for instance).
    header_list default_headers;

    // Ceilings. Exceeding either fails with `resource_exhausted` rather than growing until
    // the process dies.
    std::size_t max_header_bytes = 64u * 1024u;
    std::size_t max_body_bytes = 64u * 1024u * 1024u;

    // How long a whole request may take, in seconds. 0 leaves it to the transport.
    std::uint32_t timeout_seconds = 30;

    // --- TLS (https://) ------------------------------------------------------
    //
    // When `tls` is true the client completes a TLS handshake before the first
    // request and sends every byte through it. The caller still supplies the
    // address to dial as `endpoint`; `host` is the logical host (the `Host`
    // header, the SNI name and the certificate's host-name check target).
    bool tls = false;

    // Verify the server certificate and host name. This is on by default and
    // cannot be disabled implicitly: it takes an explicit `false` here. A
    // verification failure is reported as `failed_precondition`; the request is
    // never downgraded to cleartext.
    bool tls_verify_peer = true;

    // PEM trust anchors. Empty means OpenSSL's default trust store.
    std::string tls_ca_bundle;

    // SNI name. Empty uses `host`. An IP address sends no SNI (TLS has no SNI
    // for IP literals) and is verified as an IP address instead.
    std::string tls_server_name;

    // ALPN protocols advertised in preference order. The default offers h2
    // first and http/1.1 second, which is what a modern origin expects: the
    // negotiated value decides which engine speaks on the connection, "h2"
    // selecting the HTTP/2 client and anything else (http/1.1, or no ALPN at
    // all, which many origins without TLS-ALPN still do) selecting
    // HTTP/1.1. An empty list disables the ALPN extension entirely.
    std::vector<std::string> tls_alpn = {"h2", "http/1.1"};

    // Cleartext HTTP/2 (h2c) with prior knowledge (RFC 9113 3.4): the client
    // opens the connection with the HTTP/2 connection preface instead of an
    // HTTP/1.1 request line. Off by default because there is no fallback
    // after the preface -- an HTTP/1.1-only server simply fails the
    // connection -- so a caller must choose it for endpoints known to speak
    // h2c. Under TLS this flag has no effect; the negotiated ALPN decides.
    bool h2c = false;

    // HTTP/1.1 keep-alive (RFC 9112 9.3). When true (the default) the client
    // does not ask for a close, keeps the connection open after a response,
    // and further `send` calls reuse it -- strictly one request in flight at a
    // time, and each response is read to the end before the next request goes
    // out. The server retiring the connection (`Connection: close`, an
    // HTTP/1.0 reply without a keep-alive token, a body delimited by close)
    // closes it; the next `send` then fails rather than writing into a dead
    // socket -- a caller that wants replacement borrows from a
    // `client_pool`, which retries once for idempotent verbs. When false the
    // client sends `Connection: close` and closes after each response: the
    // historical one-request-per-connection behaviour.
    bool keep_alive = true;

    // The common HTTPS case: TLS on, port 443, verification on.
    [[nodiscard]] static client_options https(std::string host,
                                              std::uint16_t port = 443) {
        client_options options;
        options.host = std::move(host);
        options.port = port;
        options.tls = true;
        return options;
    }
};

// One request/response exchange. Constructing it opens the link; `send` may be called once.
//
// The protocol spoken on the link is negotiated, not fixed:
//
//   * TLS: the ALPN offer defaults to {h2, http/1.1}; when the server selects
//     "h2" the request runs over HTTP/2 (one stream, `response::version` is
//     `http_version::http_2`), otherwise it falls back to HTTP/1.1. The
//     fallback is not a failure: a server without ALPN support is a valid
//     HTTP/1.1 server.
//   * cleartext: HTTP/1.1 unless `client_options::h2c` opts into h2c prior
//     knowledge (no fallback is possible after the preface, hence opt-in).
//
// For many requests on one connection, prefer `http/h2_client.hpp`, which
// multiplexes streams.
class CRPC_API client final {
public:
    // Takes ownership of an already-open channel. Separated from the transport so a test
    // can script a server without a socket.
    client(client_options options, std::unique_ptr<db::channel> link);
    ~client();

    client(const client&) = delete;
    client& operator=(const client&) = delete;

    // Sends one request and reads the whole response. The connection is closed afterwards,
    // because the request asked for `Connection: close`.
    [[nodiscard]] result<response> send(method verb, std::string_view target,
                                        span<const std::byte> body,
                                        span<const header> extra_headers = {});

    // Convenience for the common case: a text body with a content type.
    [[nodiscard]] result<response> send_text(method verb, std::string_view target,
                                             std::string_view body,
                                             std::string_view content_type);

    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;

private:
    // Used by `make_client` for a client that could not be configured (for
    // example HTTPS requested in a build without the TLS backend). The error is
    // retained and returned by the first `send`, so a caller that ignores the
    // factory's return value still gets a clear failure instead of a crash.
    client(client_options options, std::unique_ptr<db::channel> link,
           rpc_error setup_error);

    friend CRPC_API std::unique_ptr<client> make_client(client_options options,
                                                        crpc::backend_factory& backend,
                                                        crpc::endpoint endpoint,
                                                        crpc::runtime* shard);

    struct impl;
    std::unique_ptr<impl> impl_;
};

// Builds a client over TCP.
[[nodiscard]] CRPC_API std::unique_ptr<client> make_client(client_options options,
                                                           crpc::backend_factory& backend,
                                                           crpc::endpoint endpoint,
                                                           crpc::runtime* shard = nullptr);

// --- Connection pool ---------------------------------------------------------
//
// HTTP/1.1 keep-alive makes a connection reusable *within one client*. A pool
// makes connections reusable *across clients*: a caller borrows one, runs an
// exchange, and returns it for the next caller, so N callers share a bounded
// set of live sockets instead of each opening their own. The contract mirrors
// `db::pool` (include/crpc/db/connection.hpp, rules P1-P15) rule for rule; the
// HTTP-specific differences are what the borrow-time health check and the
// retry policy are.

struct client_pool_options {
    // Hard ceiling on live connections (idle + in use). An acquire beyond it
    // waits up to `acquire_timeout` and then fails with `resource_exhausted`,
    // which is retryable (P5).
    std::size_t max_size = 8;

    // Ceiling on idle connections; a release beyond it closes the connection
    // (P8).
    std::size_t max_idle = 8;

    // How long an acquire waits for a free connection before
    // `resource_exhausted` (P5).
    std::chrono::nanoseconds acquire_timeout{std::chrono::seconds(5)};

    // Close an idle connection that has been idle longer. Zero disables it.
    std::chrono::nanoseconds idle_timeout{std::chrono::seconds(60)};

    // Close a connection older than this, measured from the moment it was
    // opened, so long-lived sockets are recycled even when busy callers keep
    // them alive. Zero disables it (P7).
    std::chrono::nanoseconds max_lifetime{std::chrono::minutes(30)};

    // Check an idle connection (`client::is_open`, which consults the
    // channel's liveness -- a peer FIN or TLS close_notify observed by the
    // transport marks it closed) before handing it out; a dead connection is
    // discarded and the next idle one is tried (P9). A death that lands after
    // the check but before the request is covered by the lease's one
    // transparent retry for idempotent verbs.
    bool validate_on_borrow = true;
};

// Opens pooled connections. A factory is stateless dialing logic: the pool
// owns everything it returns (P10/P11).
class CRPC_API client_factory {
public:
    virtual ~client_factory();

    client_factory(const client_factory&) = delete;
    client_factory& operator=(const client_factory&) = delete;

    // Opens one connection. A failure is reported as a value; the pool counts
    // it and does not retry beyond the caller's own retry.
    [[nodiscard]] virtual result<std::unique_ptr<client>> open() = 0;

protected:
    client_factory() = default;
};

// The built-in factory: dials one endpoint per `open()`, cleartext or TLS,
// exactly like `make_client`. All pooled connections share the options, so a
// pool is a pool for one origin.
class CRPC_API dial_factory final : public client_factory {
public:
    dial_factory(client_options options, crpc::backend_factory& backend,
                 crpc::endpoint endpoint, crpc::runtime* shard = nullptr);

    [[nodiscard]] result<std::unique_ptr<client>> open() override;

private:
    client_options options_;
    crpc::backend_factory* backend_;
    crpc::endpoint endpoint_;
    crpc::runtime* shard_;
};

// A fixed-capacity pool over one factory.
class CRPC_API client_pool {
public:
    // RAII handle over one borrowed connection. The destructor returns the
    // connection to the pool when it is still healthy and closes it when it
    // is not, so an early return cannot leak or poison the idle set (P1/P2).
    class CRPC_API lease {
    public:
        lease() noexcept = default;
        ~lease();

        lease(const lease&) = delete;
        lease& operator=(const lease&) = delete;
        lease(lease&& other) noexcept;
        lease& operator=(lease&& other) noexcept;

        [[nodiscard]] bool valid() const noexcept { return held_ != nullptr; }
        // Explicit raw access for a caller that wants to drive the connection
        // itself and take over its failure handling. Note there is no
        // `operator->`: the one-method-apart spelling would make it far too
        // easy to bypass the lease's replacement semantics by accident.
        [[nodiscard]] client* get() const noexcept { return held_; }

        // Sends one request/response exchange on the leased connection --
        // `client::send`, plus one replacement of a connection that died
        // before a complete response arrived. The replacement happens only
        // for idempotent verbs (GET/HEAD/OPTIONS): the dead connection is
        // closed, a new one is opened through the factory, and the same
        // request is sent once on it. A non-idempotent request is never
        // replayed automatically: when the request may have been executed
        // (`client::send` reports `unknown_side_effect`) the failure is
        // returned as it is, and when nothing could have been sent the
        // transport's own failure is returned.
        [[nodiscard]] result<response> send(method verb, std::string_view target,
                                            span<const std::byte> body,
                                            span<const header> extra_headers = {});

        [[nodiscard]] result<response> send_text(method verb, std::string_view target,
                                                 std::string_view body,
                                                 std::string_view content_type);

        // Returns the connection to the pool now -- closed, never pooled,
        // when it is unhealthy (P2). Idempotent.
        [[nodiscard]] status release() noexcept;

        // Closes the connection instead of returning it, for a connection
        // whose protocol state the caller knows to be broken (P14). Idempotent.
        void discard() noexcept;

    private:
        friend class client_pool;
        lease(std::shared_ptr<void> state, client* held,
              std::chrono::steady_clock::time_point created) noexcept;

        std::shared_ptr<void> state_;
        client* held_ = nullptr;
        // Carried across borrows so `max_lifetime` is measured from open, not
        // from the last release.
        std::chrono::steady_clock::time_point created_{};
    };

    client_pool(client_pool_options options, client_factory& factory);
    ~client_pool();

    client_pool(const client_pool&) = delete;
    client_pool& operator=(const client_pool&) = delete;
    client_pool(client_pool&&) = delete;
    client_pool& operator=(client_pool&&) = delete;

    [[nodiscard]] result<lease> acquire();

    // Rejects further acquires with `shutting_down` and closes idle
    // connections; a lease released afterwards is closed, never returned
    // (P4/P15).
    void close() noexcept;

    [[nodiscard]] std::size_t idle() const noexcept;
    [[nodiscard]] std::size_t in_use() const noexcept;
    [[nodiscard]] std::size_t total() const noexcept;
    // Connections opened since construction, including closed ones. Together
    // with the server's own accept count this is how reuse is proven: N
    // exchanges with `opened() == 1` ran over one connection.
    [[nodiscard]] std::uint64_t opened() const noexcept;
    [[nodiscard]] bool is_closed() const noexcept;

private:
    struct impl;
    std::shared_ptr<impl> impl_;
};

} // namespace http
} // namespace crpc

#endif // CRPC_HTTP_CLIENT_HPP