// CRPC -- HTTP/2 client: one connection, many concurrent streams.
//
// The HTTP/1.1 client (`http/client.hpp`) sends one request per connection.
// Real HTTP/2 deployments get their value from multiplexing, so this header
// adds a client that keeps one connection open and runs many streams on it:
//
//   * `begin` opens a stream and puts the whole request on the wire
//     (HEADERS, then DATA with END_STREAM) without waiting for a response;
//   * `finish` pumps frames until that stream's response is complete. Frames
//     that belong to other streams are processed as they arrive: their
//     responses and failures are parked and returned by their own `finish`.
//     This is what makes the concurrent `begin`s observable: responses may be
//     collected in any order;
//   * `send` is `begin` + `finish` for the one-shot case.
//
// The session is single-threaded and blocking: `begin`, `finish` and `close`
// must be called from one thread (the HTTP/2 connection state machine is
// single-threaded by contract, and the channel is a blocking byte pipe).
// Flow control is honoured in both directions: a large request body parks in
// the stream's send queue and resumes as WINDOW_UPDATE frames are pumped; a
// large response body is consumed immediately, which replenishes the receive
// windows with WINDOW_UPDATE frames.
//
// Negotiation:
//   * cleartext (h2c): prior knowledge (RFC 9113 3.4). The client opens the
//     connection with the HTTP/2 preface instead of an HTTP/1.1 request line.
//     There is no fallback after the preface, which is why
//     `http::client_options::h2c` is opt-in; `make_h2_client` is the explicit
//     "this endpoint speaks h2c" entry point;
//   * TLS: the connection must negotiate "h2" by ALPN (RFC 9113 3.3).
//     `make_h2_client` waits for the handshake, refuses anything but h2 with
//     `failed_precondition`, and leaves the caller free to retry with
//     `http::make_client` (which falls back to HTTP/1.1).
//
// Not implemented (recorded, not silent): server push -- the client
// advertises SETTINGS_ENABLE_PUSH=0 and a server that pushes anyway fails the
// connection; WebSocket-style CONNECT -- rejected with `invalid_argument`.
#ifndef CRPC_HTTP_H2_CLIENT_HPP
#define CRPC_HTTP_H2_CLIENT_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/db/channel.hpp>
#include <crpc/error.hpp>
#include <crpc/http/message.hpp>
#include <crpc/io.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace http {

struct h2_client_options {
    // The value of the `:authority` pseudo-header (host, plus the port when
    // it is not the scheme's default). The h2 wire has no `Host` field.
    std::string host;
    std::uint16_t port = 80;

    // Extra fields sent with every request, before the per-request ones.
    // Connection-specific fields (`Connection`, `Keep-Alive`,
    // `Proxy-Connection`, `Transfer-Encoding`, `Upgrade`), `Host`, and any
    // `TE` whose value is not exactly `trailers` are dropped, because sending
    // them is a protocol error in HTTP/2.
    header_list default_headers;

    // Ceilings. `max_header_bytes` is both the largest response header list
    // this client accepts (advertised as SETTINGS_MAX_HEADER_LIST_SIZE) and
    // the request-side bound; exceeding either fails with
    // `resource_exhausted`. `max_body_bytes` bounds a response body.
    std::size_t max_header_bytes = 64u * 1024u;
    std::size_t max_body_bytes = 64u * 1024u * 1024u;

    // How long a request may take, in seconds: every channel read and write
    // is bounded by it. 0 leaves it to the transport.
    std::uint32_t timeout_seconds = 30;

    // Our advertised HTTP/2 settings. `h2_initial_window_size` is the
    // per-stream receive window we grant the server;
    // `h2_max_frame_size` is the largest frame we accept;
    // `h2_max_send_queue_bytes` bounds the bytes parked per stream while a
    // send window is zero (exceeding it fails with `resource_exhausted`
    // rather than growing without bound).
    std::uint32_t h2_initial_window_size = 65535;
    std::uint32_t h2_max_frame_size = 16384;
    std::size_t h2_max_send_queue_bytes = 1u * 1024u * 1024u;

    // --- TLS (https://) ------------------------------------------------------
    //
    // h2 over TLS requires ALPN: the factory waits for the handshake and
    // reports `failed_precondition` unless the server negotiated "h2".
    bool tls = false;
    bool tls_verify_peer = true;
    std::string tls_ca_bundle;
    std::string tls_server_name;

    // ALPN list offered by `make_h2_client`, in preference order. Only a
    // negotiated "h2" is usable here; any other outcome is reported to the
    // caller so it can fall back to `http::make_client`. Empty disables the
    // ALPN extension, which for this client means the handshake can never
    // produce a usable connection.
    std::vector<std::string> tls_alpn = {"h2", "http/1.1"};

    // The common TLS case: TLS on, port 443, verification on.
    [[nodiscard]] static h2_client_options https(std::string host,
                                                 std::uint16_t port = 443) {
        h2_client_options options;
        options.host = std::move(host);
        options.port = port;
        options.tls = true;
        return options;
    }
};

// A stream opened by `begin`: the 31-bit stream identifier (odd, client-initiated).
using h2_stream = std::uint32_t;

// One multiplexed HTTP/2 client connection.
class CRPC_API h2_client final {
public:
    // Takes ownership of an already-open channel. Separated from the
    // transport so a test can script a server without a socket.
    h2_client(h2_client_options options, std::unique_ptr<db::channel> link);
    ~h2_client();

    h2_client(const h2_client&) = delete;
    h2_client& operator=(const h2_client&) = delete;

    // Opens a stream and sends the whole request. Returns the stream
    // identifier to hand to `finish`. The request is on the wire when this
    // returns (a body that exceeds the send windows is parked and pushed out
    // as WINDOW_UPDATEs arrive, which is why `begin` may block on reads).
    [[nodiscard]] result<h2_stream> begin(method verb, std::string_view target,
                                          span<const std::byte> body,
                                          span<const header> extra_headers = {});

    // Waits for this stream's response and returns it. Responses that belong
    // to other streams are consumed on the way and parked for their own
    // `finish`. `response::version` is `http_version::http_2`; trailers, when
    // the server sent them, land in `response::trailers`.
    [[nodiscard]] result<response> finish(h2_stream stream);

    // `begin` + `finish`.
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
    struct impl;
    std::unique_ptr<impl> impl_;
};

// Builds an h2 client over TCP. Cleartext connections speak h2 with prior
// knowledge; TLS connections must negotiate "h2" by ALPN or the factory
// returns `failed_precondition` (fall back to `http::make_client`, which
// speaks h1.1).
[[nodiscard]] CRPC_API result<std::unique_ptr<h2_client>> make_h2_client(
    h2_client_options options, crpc::backend_factory& backend, crpc::endpoint endpoint,
    crpc::runtime* shard = nullptr);

} // namespace http
} // namespace crpc

#endif // CRPC_HTTP_H2_CLIENT_HPP
