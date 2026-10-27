// CRPC -- SHA-1 and SHA-256.
//
// Both are here for one reason: MySQL's authentication plugins hash the password
// with them. SHA-1 and SHA-256 are needed even though neither is a good password
// hash — the protocol dictates the construction, so a client has no choice.
//
// This is not a hardened cryptographic library. It is correct (pinned against the
// published test vectors), constant-time only in the sense that it has no data
// dependent branches, and it does not use hardware acceleration. It must not be
// used for password storage, signatures or anything where an attacker's timing
// matters.
#ifndef CRPC_CRYPTO_HASH_HPP
#define CRPC_CRYPTO_HASH_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include <crpc/config.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace crypto {

using sha1_digest = std::array<std::uint8_t, 20>;
using sha256_digest = std::array<std::uint8_t, 32>;

[[nodiscard]] CRPC_API sha1_digest sha1(span<const std::byte> data) noexcept;
// Hashes the concatenation of two spans without copying them into one buffer,
// which is what the MySQL scramble needs.
[[nodiscard]] CRPC_API sha1_digest sha1(span<const std::byte> first,
                                        span<const std::byte> second) noexcept;

[[nodiscard]] CRPC_API sha256_digest sha256(span<const std::byte> data) noexcept;
[[nodiscard]] CRPC_API sha256_digest sha256(span<const std::byte> first,
                                            span<const std::byte> second) noexcept;

// HMAC-SHA256 (RFC 2104). SCRAM authentication is defined in terms of it, so a client
// that substitutes a plain hash cannot authenticate at all.
[[nodiscard]] CRPC_API sha256_digest hmac_sha256(span<const std::byte> key,
                                                 span<const std::byte> message);

// PBKDF2-HMAC-SHA256 with one output block, which is what SCRAM's Hi() needs: the output
// is exactly one SHA-256 digest, so a general multi-block implementation would be dead
// code. iterations of 0 is refused by returning a zero digest rather than silently
// producing the wrong value.
[[nodiscard]] CRPC_API sha256_digest pbkdf2_sha256(span<const std::byte> password,
                                                   span<const std::byte> salt,
                                                   std::uint32_t iterations);

// Hex encoding, lowercase. For tests and diagnostics.
[[nodiscard]] CRPC_API std::string to_hex(sha1_digest digest);
[[nodiscard]] CRPC_API std::string to_hex(sha256_digest digest);

} // namespace crypto
} // namespace crpc

#endif // CRPC_CRYPTO_HASH_HPP