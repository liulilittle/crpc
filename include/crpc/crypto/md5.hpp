// CRPC -- MD5 (RFC 1321), and HMAC-MD5 (RFC 2104).
//
// MD5 is here for exactly one reason: UUID version 3 (RFC 4122 §4.3) is defined
// as MD5 over the namespace/name pair, and a v3 value computed with any other
// hash is simply not a v3 value. Nothing else in this library may use it.
//
// It is broken for collision resistance and must never be used for signatures,
// password storage, or anywhere an attacker chooses both inputs.
#ifndef CRPC_CRYPTO_MD5_HPP
#define CRPC_CRYPTO_MD5_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include <crpc/config.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace crypto {

using md5_digest = std::array<std::uint8_t, 16>;

// RFC 1321 digest of `data`.
[[nodiscard]] CRPC_API md5_digest md5(span<const std::byte> data) noexcept;

// Hashes the concatenation of two spans without copying them into one buffer,
// which is what the UUID v3 namespace+name construction needs.
[[nodiscard]] CRPC_API md5_digest md5(span<const std::byte> first,
                                      span<const std::byte> second) noexcept;

// Hex encoding, lowercase.
[[nodiscard]] CRPC_API std::string to_hex(md5_digest digest);

} // namespace crypto
} // namespace crpc

#endif // CRPC_CRYPTO_MD5_HPP
