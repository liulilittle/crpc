// CRPC -- the WebSocket opening handshake (RFC 6455 section 4).
//
// Both roles share this module: the client builds a GET request carrying a
// random Sec-WebSocket-Key, and the server validates that request and answers
// 101 with the accept value base64(SHA-1(key + GUID)) -- the one piece of
// protocol algebra that is pinned by the RFC's own worked example. The HTTP
// request/response model and the incremental parser from crpc/http/message.hpp
// are reused so the Upgrade exchange rides the same battle-tested message
// layer as the rest of the library.
#ifndef CRPC_WS_HANDSHAKE_HPP
#define CRPC_WS_HANDSHAKE_HPP

#include <string>
#include <string_view>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/http/message.hpp>

namespace crpc {
namespace ws {

// Generates a Sec-WebSocket-Key: 16 random bytes, base64-encoded (RFC 6455
// section 4.1). The randomness comes from the operating system's entropy
// source via std::random_device, exactly as the SCRAM nonce is generated.
[[nodiscard]] CRPC_API std::string websocket_key();

// Computes the Sec-WebSocket-Accept value for a client key:
// base64(SHA-1(key + GUID)). This is the value pinned by the RFC's test case.
[[nodiscard]] CRPC_API std::string compute_accept(std::string_view key);

// Builds the client's opening handshake request (RFC 6455 section 4.1): a GET
// on `path` advertising `Upgrade: websocket`, `Connection: Upgrade`,
// `Sec-WebSocket-Key`, `Sec-WebSocket-Version: 13` and (when non-empty) an
// `Origin`. `key` has already been produced by `websocket_key()`; the caller
// keeps it for the response check.
[[nodiscard]] CRPC_API http::request make_upgrade_request(std::string_view host,
                                                          std::string_view path,
                                                          std::string_view origin,
                                                          std::string_view key);

// Validates a client's opening handshake (RFC 6455 section 4.2.1). On success
// returns the client's key, which the caller feeds to `make_upgrade_response`.
// Rejects a wrong method, a missing / wrongly-valued `Upgrade` or
// `Connection`, a `Sec-WebSocket-Version` other than 13, and a key that is
// absent or is not exactly 16 bytes of base64.
[[nodiscard]] CRPC_API result<std::string> validate_upgrade_request(
    const http::request& req);

// Builds the server's 101 response for a validated client key.
[[nodiscard]] CRPC_API http::response make_upgrade_response(std::string_view key);

// Validates the server's 101 answer to our own handshake (RFC 6455 sections
// 4.1 and 4.2.2): status must be 101, both `Upgrade` and `Connection` must
// echo the values we sent, and `Sec-WebSocket-Accept` must equal
// `compute_accept(key)` for the key we sent.
[[nodiscard]] CRPC_API status validate_upgrade_response(
    const http::response& resp, std::string_view key);

} // namespace ws
} // namespace crpc

#endif // CRPC_WS_HANDSHAKE_HPP