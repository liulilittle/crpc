// CRPC -- HMAC-SHA256, PBKDF2 and base64 tests.
//
// The vectors come from the standards, not from this implementation: RFC 4231 for HMAC,
// RFC 6070's PBKDF2 structure with SHA-256 substituted (published in RFC 7914's test
// notes), and RFC 4648 for base64. A hash that agrees with itself proves nothing.
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/crypto/base64.hpp>
#include <crpc/crypto/hash.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace {

using crpc::crypto::sha256_digest;

[[nodiscard]] crpc::span<const std::byte> bytes_of(std::string_view text) noexcept {
    return crpc::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                       text.size());
}

[[nodiscard]] std::string hex_of(const sha256_digest& digest) {
    return crpc::crypto::to_hex(digest);
}

// 0x0b repeated `count` times, which is what RFC 4231 uses for its key vectors.
[[nodiscard]] std::string repeated(unsigned char value, std::size_t count) {
    return std::string(count, static_cast<char>(value));
}

} // namespace

CRPC_TEST(crypto_hmac_sha256_rfc4231_case_1) {
    // Key "0b"x20, data "Hi There"
    const sha256_digest digest =
        crpc::crypto::hmac_sha256(bytes_of(repeated(0x0b, 20)), bytes_of("Hi There"));
    CRPC_CHECK_EQ(std::string("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"),
                  hex_of(digest));
}

CRPC_TEST(crypto_hmac_sha256_rfc4231_case_2) {
    // Key "Jefe", data "what do ya want for nothing?"
    const sha256_digest digest =
        crpc::crypto::hmac_sha256(bytes_of("Jefe"), bytes_of("what do ya want for nothing?"));
    CRPC_CHECK_EQ(std::string("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"),
                  hex_of(digest));
}

CRPC_TEST(crypto_hmac_sha256_rfc4231_case_3) {
    // Key "aa"x20, data "dd"x50
    const sha256_digest digest =
        crpc::crypto::hmac_sha256(bytes_of(repeated(0xaa, 20)), bytes_of(repeated(0xdd, 50)));
    CRPC_CHECK_EQ(std::string("773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"),
                  hex_of(digest));
}

CRPC_TEST(crypto_hmac_sha256_handles_a_key_longer_than_the_block) {
    // RFC 4231 case 6: a 131-byte key must be hashed down to 32 bytes first. A naive
    // implementation that truncates instead produces a different digest.
    const sha256_digest digest = crpc::crypto::hmac_sha256(
        bytes_of(repeated(0xaa, 131)),
        bytes_of("Test Using Larger Than Block-Size Key - Hash Key First"));
    CRPC_CHECK_EQ(std::string("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"),
                  hex_of(digest));
}

CRPC_TEST(crypto_pbkdf2_sha256_matches_the_published_vectors) {
    // PBKDF2-HMAC-SHA256, password "password", salt "salt", 1 iteration.
    const sha256_digest one =
        crpc::crypto::pbkdf2_sha256(bytes_of("password"), bytes_of("salt"), 1);
    CRPC_CHECK_EQ(std::string("120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"),
                  hex_of(one));

    // 2 iterations.
    const sha256_digest two =
        crpc::crypto::pbkdf2_sha256(bytes_of("password"), bytes_of("salt"), 2);
    CRPC_CHECK_EQ(std::string("ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"),
                  hex_of(two));

    // 4096 iterations, the vector that catches an implementation which forgets to XOR
    // the rounds together.
    const sha256_digest many =
        crpc::crypto::pbkdf2_sha256(bytes_of("password"), bytes_of("salt"), 4096);
    CRPC_CHECK_EQ(std::string("c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"),
                  hex_of(many));
}

CRPC_TEST(crypto_pbkdf2_refuses_zero_iterations) {
    // Zero iterations has no defined result; returning a zero digest makes the caller's
    // authentication fail loudly instead of producing a value that looks like a password
    // hash derived with one round.
    const sha256_digest digest =
        crpc::crypto::pbkdf2_sha256(bytes_of("password"), bytes_of("salt"), 0);
    CRPC_CHECK_EQ(std::string(64, '0'), hex_of(digest));
}

CRPC_TEST(crypto_base64_matches_rfc4648) {
    // RFC 4648 section 10.
    CRPC_CHECK_EQ(std::string(""), crpc::crypto::base64_encode(std::string_view{}));
    CRPC_CHECK_EQ(std::string("Zg=="), crpc::crypto::base64_encode("f"));
    CRPC_CHECK_EQ(std::string("Zm8="), crpc::crypto::base64_encode("fo"));
    CRPC_CHECK_EQ(std::string("Zm9v"), crpc::crypto::base64_encode("foo"));
    CRPC_CHECK_EQ(std::string("Zm9vYg=="), crpc::crypto::base64_encode("foob"));
    CRPC_CHECK_EQ(std::string("Zm9vYmE="), crpc::crypto::base64_encode("fooba"));
    CRPC_CHECK_EQ(std::string("Zm9vYmFy"), crpc::crypto::base64_encode("foobar"));

    CRPC_CHECK_EQ(std::string("f"), crpc::crypto::base64_decode("Zg==").value());
    CRPC_CHECK_EQ(std::string("fo"), crpc::crypto::base64_decode("Zm8=").value());
    CRPC_CHECK_EQ(std::string("foo"), crpc::crypto::base64_decode("Zm9v").value());
    CRPC_CHECK_EQ(std::string("foobar"), crpc::crypto::base64_decode("Zm9vYmFy").value());
}

CRPC_TEST(crypto_base64_skips_whitespace_and_refuses_garbage) {
    // Line wrapping is normal in PEM and in long SASL payloads.
    CRPC_CHECK_EQ(std::string("foobar"), crpc::crypto::base64_decode("Zm9v\r\nYmFy").value());

    // A character outside the alphabet is an error, not something to skip: a signature
    // decoded from a corrupted payload would fail verification with no clue why.
    CRPC_CHECK_FALSE(static_cast<bool>(crpc::crypto::base64_decode("Zm9v!mFy")));
    // Data after the padding cannot be a valid encoding.
    CRPC_CHECK_FALSE(static_cast<bool>(crpc::crypto::base64_decode("Zg==Zg==")));
    // A lone sextet is not a whole byte.
    CRPC_CHECK_FALSE(static_cast<bool>(crpc::crypto::base64_decode("Z")));
}

CRPC_TEST_MAIN()