// CRPC -- RSA public-key encryption for MySQL caching_sha2_password.
//
// Scope is deliberately narrow: parse an RSA *public* key (PEM
// SubjectPublicKeyInfo) and encrypt with RSAES-OAEP (RFC 8017 section 7.1.1,
// SHA-1 label hash and MGF1-SHA1, empty label). That is exactly what MySQL 8
// `caching_sha2_password` full authentication needs over a plaintext link: the
// client asks for the server public key (one 0x02 byte), the server answers
// with the PEM key, and the client encrypts `(password || NUL) XOR scramble`.
//
// Why self-implemented instead of the optional OpenSSL backend: the default
// library build has no OpenSSL (`CRPC_WITH_OPENSSL` is OFF by default), and
// full authentication must work in that build -- an OpenSSL-only RSA path
// would leave the default build unable to connect to a stock MySQL 8 at all.
// Only the public-key operation is implemented (modular exponentiation with a
// small odd public exponent); there is no private-key operation and none is
// planned: a client never decrypts. Correctness is pinned to the official
// RSA Labs PKCS#1 v2.1 OAEP vectors (see tests/test_crypto_rsa.cpp) and
// exercised end to end against a real MySQL 8 server by the integration test.
#ifndef CRPC_CRYPTO_RSA_HPP
#define CRPC_CRYPTO_RSA_HPP

#include <cstddef>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace crypto {

// Encrypts `message` with RSAES-OAEP: SHA-1 for the label hash, MGF1-SHA1 for
// the masks, empty label (the exact parameter set MySQL uses). The key is a
// PEM "-----BEGIN PUBLIC KEY-----" SubjectPublicKeyInfo, which is what the
// MySQL server sends in answer to the caching_sha2_password public-key
// request. The modulus must be 1024..8192 bits and odd; the exponent must be
// odd and fit in 32 bits (65537 is what every server ships). The OAEP seed
// comes from the operating system's cryptographic random generator; a failing
// generator is an error, never a predictable fallback. The ciphertext length
// equals the modulus length.
[[nodiscard]] CRPC_API result<std::vector<std::byte>> rsa_oaep_sha1_encrypt_pem(
    std::string_view public_key_pem, span<const std::byte> message);

// Same, with a caller-chosen seed in place of the random one. Production
// callers must use the RNG overload: a fixed seed makes the encryption
// deterministic, which is only acceptable to pin the implementation against
// the published RSA-Labs vectors, which fix the seed. A seed whose size is not
// SHA-1's 20-byte output is refused.
[[nodiscard]] CRPC_API result<std::vector<std::byte>> rsa_oaep_sha1_encrypt_pem_with_seed(
    std::string_view public_key_pem, span<const std::byte> message,
    span<const std::byte> seed);

} // namespace crypto
} // namespace crpc

#endif // CRPC_CRYPTO_RSA_HPP
