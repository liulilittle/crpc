// CRPC -- a WebSocket server (RFC 6455).
//
// `server` binds one listener on a `backend_factory` (in-process for tests, a
// TCP backend for the network) and serves one WebSocket session at a time
// through `accept()`: it reads the client's Upgrade request, validates it
// against RFC 6455 section 4.2.1, answers 101 with the SHA-1 accept value, and
// returns a `stream`. A request that fails validation is answered 400 and
// dropped; `accept()` then serves the next client, so a single misbehaving
// peer cannot wedge the accept loop.
//
// wss:// needs the TLS backend; without it (`CRPC_HAVE_OPENSSL` undefined) a
// server configured with TLS credentials fails its finite handshake and every
// `accept()` returns `failed_precondition`.
#ifndef CRPC_WS_SERVER_HPP
#define CRPC_WS_SERVER_HPP

#include <chrono>
#include <cstddef>
#include <memory>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/io.hpp>
#include <crpc/io/tls.hpp>
#include <crpc/ws/stream.hpp>

namespace crpc {
namespace ws {

struct server_options {
    // Bound on connections whose Accept was received but not yet consumed by
    // `accept()`. Beyond it a new connection is closed immediately instead of
    // growing the queue.
    std::size_t max_pending_accepts = 64;

    // Transport and handshake time budget for a freshly accepted connection.
    std::chrono::milliseconds operation_timeout{std::chrono::seconds(15)};

    // Post-handshake session behaviour.
    stream_options stream{};

    // TLS for wss. Leave at defaults for cleartext; for wss set `role` to
    // server and provide the certificate/private-key files.
    io::tls_config tls{};
};

class CRPC_API server {
public:
    // Binds the listener. A bind failure is not fatal to construction: it is
    // surfaced as `failed_precondition` by every `accept()` call.
    server(backend_factory& backend, endpoint ep, const server_options& options);
    ~server();

    server(const server&) = delete;
    server& operator=(const server&) = delete;
    server(server&&) = delete;
    server& operator=(server&&) = delete;

    // Blocks until a client completes the opening handshake, then returns the
    // live session. An Upgrade request that fails validation is answered 400
    // and the accept loop moves on. Returns `unavailable` after `stop()`.
    [[nodiscard]] result<std::unique_ptr<stream>> accept();

    // Closes the listener, drops unconsumed accepted connections and unblocks
    // any waiting `accept()` (which then returns `unavailable`). Idempotent.
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept;

    // The endpoint the listener is bound to; useful when the caller let the
    // backend choose an ephemeral port.
    [[nodiscard]] endpoint bound_endpoint() const noexcept;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace ws
} // namespace crpc

#endif // CRPC_WS_SERVER_HPP