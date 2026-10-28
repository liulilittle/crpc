// CRPC -- a WebSocket client (RFC 6455).
//
// `connect` opens the transport (TCP, or TLS for wss://), performs the opening
// handshake against the server, and returns a `client` over the resulting
// session. The client role fixes the mask direction: every frame this side
// sends is masked and every frame it accepts must be unmasked, which is the
// half of the RFC 6455 mask contract that a server-side stack is responsible
// for enforcing back.
//
// wss:// requires the TLS backend. When the library was built without OpenSSL
// (`CRPC_HAVE_OPENSSL` undefined) `connect` with `tls == true` fails with
// `failed_precondition` and names the missing backend; it is never silently
// downgraded to cleartext.
#ifndef CRPC_WS_CLIENT_HPP
#define CRPC_WS_CLIENT_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/io.hpp>
#include <crpc/io/tls.hpp>
#include <crpc/span.hpp>
#include <crpc/ws/frame.hpp>
#include <crpc/ws/stream.hpp>

namespace crpc {
namespace ws {

struct client_options {
    // Sent as the `Host` header (with `:port` appended when it differs from
    // the well-known default) and, for wss://, used as the TLS SNI name and the
    // certificate's host-name check target. The address actually dialed is the
    // `endpoint` passed to `connect`.
    std::string host;
    std::uint16_t port = 80;

    // Request target for the Upgrade ("" defaults to "/").
    std::string path;

    // Optional `Origin` header (RFC 6455 section 4.1).
    std::string origin;

    // wss:// — wrap the TCP link in TLS before the HTTP Upgrade. Requires
    // CRPC_HAVE_OPENSSL.
    bool tls = false;
    // Only consulted when `tls` is set; `role` and `hostname` are forced for
    // the client role, everything else (verification mode, CA bundle, ...) is
    // caller-controlled.
    io::tls_config tls_config{};

    // Transport and handshake time budgets.
    std::chrono::milliseconds connect_timeout{std::chrono::seconds(10)};
    std::chrono::milliseconds operation_timeout{std::chrono::seconds(15)};

    // Post-handshake session behaviour.
    stream_options stream{};
};

class CRPC_API client {
public:
    ~client();

    client(const client&) = delete;
    client& operator=(const client&) = delete;
    client(client&&) = delete;
    client& operator=(client&&) = delete;

    [[nodiscard]] result<event> recv();
    [[nodiscard]] status send_text(std::string_view text);
    [[nodiscard]] status send_binary(span<const std::byte> data);
    [[nodiscard]] status send_ping(span<const std::byte> payload = {});
    [[nodiscard]] status send_pong(span<const std::byte> payload = {});
    [[nodiscard]] status send_close(close_code code = close_code::normal,
                                    std::string_view reason = {});
    [[nodiscard]] status close();
    void abort() noexcept;
    [[nodiscard]] bool is_open() const noexcept;

private:
    friend CRPC_API result<std::unique_ptr<client>> connect(
        backend_factory& backend, endpoint ep, client_options options);

    explicit client(std::unique_ptr<stream> session) noexcept;

    std::unique_ptr<stream> session_;
};

// Connects, performs the opening handshake and returns the client. `ep` is
// the address to dial (TCP host/port or an in-process name for tests); on a
// successful handshake every byte left over after the 101 response (an early
// data frame from the server) is pipelined into the session.
[[nodiscard]] CRPC_API result<std::unique_ptr<client>> connect(
    backend_factory& backend, endpoint ep, client_options options);

} // namespace ws
} // namespace crpc

#endif // CRPC_WS_CLIENT_HPP