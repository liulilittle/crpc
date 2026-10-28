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

} // namespace http
} // namespace crpc

#endif // CRPC_HTTP_CLIENT_HPP