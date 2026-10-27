// CRPC -- MySQL authentication scrambles.
//
// Two plugins matter in practice:
//
//   * `mysql_native_password` (SHA-1): `token = SHA1(pw) XOR SHA1(nonce || SHA1(SHA1(pw)))`
//   * `caching_sha2_password` (SHA-256, MySQL 8 default):
//     `token = SHA256(pw) XOR SHA256(SHA256(SHA256(pw)) || nonce)`
//
// Both are challenge-response: the server sends a nonce in the handshake and the
// client proves knowledge of the password without sending it. The nonce is 20
// bytes in both cases (the handshake announces 21 because it counts a trailing
// NUL, which is not part of the challenge).
//
// An empty password produces an empty response for both plugins, which is what
// makes "no password" distinguishable from "password that hashes to zeros".
//
// What is NOT here: TLS, RSA password exchange (`caching_sha2_password` full
// authentication over a plaintext connection needs the server's public key), and
// the `sha256_password` plugin. Those belong to the connection layer, which knows
// whether the link is encrypted.
#ifndef CRPC_DB_MYSQL_AUTH_HPP
#define CRPC_DB_MYSQL_AUTH_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace db {
namespace mysql {

// Length of the challenge nonce both standard plugins use.
inline constexpr std::size_t auth_nonce_size = 20;

// `mysql_native_password`. Returns 20 bytes, or an empty vector for an empty
// password.
[[nodiscard]] CRPC_API result<std::vector<std::byte>> scramble_native_password(
    std::string_view password, span<const std::byte> nonce);

// `caching_sha2_password` fast path. Returns 32 bytes, or an empty vector for an
// empty password.
[[nodiscard]] CRPC_API result<std::vector<std::byte>> scramble_caching_sha2_password(
    std::string_view password, span<const std::byte> nonce);

// The plugin names this header understands, so a caller can decide whether it can
// authenticate at all instead of failing at the server's auth-switch.
[[nodiscard]] CRPC_API bool is_supported_plugin(std::string_view plugin) noexcept;

} // namespace mysql
} // namespace db
} // namespace crpc

#endif // CRPC_DB_MYSQL_AUTH_HPP