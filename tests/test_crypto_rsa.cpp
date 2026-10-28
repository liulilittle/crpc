// CRPC -- RSA-OAEP-SHA1 public-key encryption tests.
//
// The implementation is pinned to the official RSA Labs PKCS#1 v2.1 OAEP
// vector set (see rsa_test_support.hpp for provenance and the pre-commit
// cross-checks). The reference decryptor/OAEP-decoder in that header is
// written against the raw RFC 8017 definitions, independent of the library's
// bignum, so a shared bug cannot validate itself.
#include "rsa_test_support.hpp"
#include "test_framework.hpp"

#include <cstddef>
#include <string>
#include <vector>

#include <crpc/crypto/base64.hpp>
#include <crpc/crypto/rsa.hpp>
#include <crpc/error.hpp>

namespace {

using crpc::crypto::rsa_oaep_sha1_encrypt_pem;
using crpc::crypto::rsa_oaep_sha1_encrypt_pem_with_seed;

// Wraps the given raw INTEGER bytes (modulus, exponent) into a
// SubjectPublicKeyInfo PEM, so the negative cases can present hand-built
// keys to the parser.
[[nodiscard]] std::string spki_pem(const std::vector<std::byte>& modulus,
                                   const std::vector<std::byte>& exponent) {
    auto sized_integer = [](const std::vector<std::byte>& body) {
        std::vector<std::byte> content;
        if (!body.empty() && (static_cast<int>(body[0]) & 0x80) != 0) {
            content.push_back(std::byte{0}); // DER: positive INTEGER keeps a 0x00 before a high bit
        }
        content.insert(content.end(), body.begin(), body.end());
        std::vector<std::byte> out;
        out.push_back(std::byte{0x02});
        if (content.size() >= 0x80) {
            out.push_back(std::byte{0x81});
            out.push_back(std::byte{static_cast<unsigned char>(content.size())});
        } else {
            out.push_back(std::byte{static_cast<unsigned char>(content.size())});
        }
        out.insert(out.end(), content.begin(), content.end());
        return out;
    };
    auto with_length = [](std::byte tag, const std::vector<std::byte>& body) {
        std::vector<std::byte> out;
        out.push_back(tag);
        if (body.size() >= 0x80) {
            out.push_back(std::byte{0x81});
            out.push_back(std::byte{static_cast<unsigned char>(body.size())});
        } else {
            out.push_back(std::byte{static_cast<unsigned char>(body.size())});
        }
        out.insert(out.end(), body.begin(), body.end());
        return out;
    };

    std::vector<std::byte> key;
    const std::vector<std::byte> modulus_int = sized_integer(modulus);
    const std::vector<std::byte> exponent_int = sized_integer(exponent);
    key.insert(key.end(), modulus_int.begin(), modulus_int.end());
    key.insert(key.end(), exponent_int.begin(), exponent_int.end());
    const std::vector<std::byte> key_sequence = with_length(std::byte{0x30}, key);

    std::vector<std::byte> algorithm = {
        std::byte{0x30}, std::byte{0x0d}, std::byte{0x06}, std::byte{0x09},
        std::byte{0x2a}, std::byte{0x86}, std::byte{0x48}, std::byte{0x86},
        std::byte{0xf7}, std::byte{0x0d}, std::byte{0x01}, std::byte{0x01},
        std::byte{0x01}, std::byte{0x05}, std::byte{0x00}};

    std::vector<std::byte> bits_body;
    bits_body.push_back(std::byte{0});
    bits_body.insert(bits_body.end(), key_sequence.begin(), key_sequence.end());
    const std::vector<std::byte> bits = with_length(std::byte{0x03}, bits_body);

    std::vector<std::byte> body = algorithm;
    body.insert(body.end(), bits.begin(), bits.end());
    const std::vector<std::byte> der = with_length(std::byte{0x30}, body);

    const std::string encoded = crpc::crypto::base64_encode(
        crpc::span<const std::byte>(der.data(), der.size()));
    return "-----BEGIN PUBLIC KEY-----\n" + encoded + "\n-----END PUBLIC KEY-----";
}

[[nodiscard]] crpc::result<std::vector<std::byte>> encrypt_with(
    const std::string& pem, const std::vector<std::byte>& message,
    const std::vector<std::byte>& seed) {
    return rsa_oaep_sha1_encrypt_pem_with_seed(
        pem, crpc::span<const std::byte>(message.data(), message.size()),
        crpc::span<const std::byte>(seed.data(), seed.size()));
}

} // namespace

CRPC_TEST(crypto_rsa_oaep_sha1_matches_the_official_rsa_labs_vector) {
    // The full encryption KAT: same key, message and seed must produce the
    // ciphertext the RSA Labs document published in 2001.
    const crpc::result<std::vector<std::byte>> cipher = encrypt_with(
        rsa_test::vector_pem, rsa_test::hex_bytes(rsa_test::vector_message),
        rsa_test::hex_bytes(rsa_test::vector_seed));
    CRPC_CHECK(cipher.ok());
    if (cipher.ok()) {
        CRPC_CHECK_EQ(std::string(rsa_test::vector_ciphertext), rsa_test::bytes_hex(cipher.value()));
        CRPC_CHECK_EQ(std::size_t{128}, cipher.value().size()); // 1024-bit modulus
    }
}

CRPC_TEST(crypto_rsa_primitive_matches_the_published_vector_in_reverse) {
    // Decrypt-side anchor for the test-only reference implementation: the
    // pinned ciphertext run through the CRT decryptor must reproduce the
    // pinned EM byte for byte. Together with the KAT above this pins both
    // directions of the primitive.
    const std::vector<std::byte> em =
        rsa_test::reference_decrypt(rsa_test::hex_bytes(rsa_test::vector_ciphertext));
    CRPC_CHECK_EQ(std::string(rsa_test::vector_em), rsa_test::bytes_hex(em));
}

CRPC_TEST(crypto_rsa_oaep_sha1_random_seed_round_trips) {
    // The RNG overload must produce a ciphertext whose reference decryption
    // and OAEP decode recover the message -- this exercises the random-seed
    // path, which the fixed-seed KAT cannot.
    const std::vector<std::byte> message = rsa_test::hex_bytes(rsa_test::vector_message);
    const crpc::result<std::vector<std::byte>> cipher = rsa_oaep_sha1_encrypt_pem(
        rsa_test::vector_pem, crpc::span<const std::byte>(message.data(), message.size()));
    CRPC_CHECK(cipher.ok());
    if (!cipher.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::size_t{128}, cipher.value().size());
    const std::vector<std::byte> em = rsa_test::reference_decrypt(cipher.value());
    std::vector<std::byte> recovered;
    CRPC_CHECK(rsa_test::reference_oaep_decode(em, recovered));
    CRPC_CHECK(message == recovered);
}

CRPC_TEST(crypto_rsa_oaep_sha1_is_randomized) {
    // Two encryptions of the same message with fresh seeds must differ; a
    // deterministic output would mean the RNG was bypassed or ignored.
    const std::vector<std::byte> message = rsa_test::hex_bytes(rsa_test::vector_message);
    const crpc::result<std::vector<std::byte>> first = rsa_oaep_sha1_encrypt_pem(
        rsa_test::vector_pem, crpc::span<const std::byte>(message.data(), message.size()));
    const crpc::result<std::vector<std::byte>> second = rsa_oaep_sha1_encrypt_pem(
        rsa_test::vector_pem, crpc::span<const std::byte>(message.data(), message.size()));
    CRPC_CHECK(first.ok());
    CRPC_CHECK(second.ok());
    if (first.ok() && second.ok()) {
        CRPC_CHECK(first.value() != second.value());
    }
}

CRPC_TEST(crypto_rsa_refuses_oversized_messages) {
    // RSAES-OAEP accepts at most k - 2*20 - 2 bytes; for the 1024-bit vector
    // key that is 86. MySQL applies the same bound with its "+41" check.
    const std::vector<std::byte> seed = rsa_test::hex_bytes(rsa_test::vector_seed);
    const std::vector<std::byte> ok_message(86, std::byte{0x42});
    CRPC_CHECK(encrypt_with(rsa_test::vector_pem, ok_message, seed).ok());

    const std::vector<std::byte> too_long(87, std::byte{0x42});
    const crpc::result<std::vector<std::byte>> refused = encrypt_with(rsa_test::vector_pem, too_long, seed);
    CRPC_CHECK_FALSE(refused.ok());
    if (!refused.ok()) {
        CRPC_CHECK(refused.error().message.find("too long") != std::string::npos);
    }
}

CRPC_TEST(crypto_rsa_refuses_bad_seeds) {
    const std::vector<std::byte> message = rsa_test::hex_bytes(rsa_test::vector_message);
    std::vector<std::byte> short_seed = rsa_test::hex_bytes(rsa_test::vector_seed);
    short_seed.pop_back();
    CRPC_CHECK_FALSE(encrypt_with(rsa_test::vector_pem, message, short_seed).ok());
}

CRPC_TEST(crypto_rsa_refuses_malformed_pem) {
    const std::vector<std::byte> message = rsa_test::hex_bytes(rsa_test::vector_message);
    const std::vector<std::byte> seed = rsa_test::hex_bytes(rsa_test::vector_seed);

    // No PEM markers at all.
    CRPC_CHECK_FALSE(encrypt_with("not a pem", message, seed).ok());

    // BEGIN without END.
    CRPC_CHECK_FALSE(encrypt_with("-----BEGIN PUBLIC KEY-----\nAAAA", message, seed).ok());

    // Valid base64 that is not a key structure.
    CRPC_CHECK_FALSE(encrypt_with("-----BEGIN PUBLIC KEY-----\nZm9vYmFy\n-----END PUBLIC KEY-----",
                                  message, seed)
                         .ok());

    // The PKCS#1 wrapping ("BEGIN RSA PUBLIC KEY") is not the
    // SubjectPublicKeyInfo MySQL sends; refuse rather than guess.
    CRPC_CHECK_FALSE(encrypt_with("-----BEGIN RSA PUBLIC KEY-----\nZm9vYmFy\n-----END RSA PUBLIC KEY-----",
                                  message, seed)
                         .ok());
}

CRPC_TEST(crypto_rsa_rejects_out_of_range_keys) {
    const std::vector<std::byte> message = rsa_test::hex_bytes(rsa_test::vector_message);
    const std::vector<std::byte> seed = rsa_test::hex_bytes(rsa_test::vector_seed);
    const std::vector<std::byte> modulus = rsa_test::hex_bytes(rsa_test::vector_n);

    // An exponent that does not fit in 32 bits (five significant bytes).
    {
        std::vector<std::byte> big_exponent = {std::byte{0x01}, std::byte{0x00}, std::byte{0x00},
                                               std::byte{0x00}, std::byte{0x11}};
        const crpc::result<std::vector<std::byte>> refused =
            encrypt_with(spki_pem(modulus, big_exponent), message, seed);
        CRPC_CHECK_FALSE(refused.ok());
        if (!refused.ok()) {
            CRPC_CHECK(refused.error().message.find("32 bits") != std::string::npos);
        }
    }

    // An even exponent.
    {
        const std::vector<std::byte> even_exponent = {std::byte{0x10}};
        const crpc::result<std::vector<std::byte>> refused =
            encrypt_with(spki_pem(modulus, even_exponent), message, seed);
        CRPC_CHECK_FALSE(refused.ok());
        if (!refused.ok()) {
            CRPC_CHECK(refused.error().message.find("odd") != std::string::npos);
        }
    }

    // A modulus below 1024 bits (512 bits here), even though odd.
    {
        std::vector<std::byte> small_modulus(64, std::byte{0xff});
        small_modulus.back() = std::byte{0xfd};
        std::vector<std::byte> exponent = {std::byte{0x01}, std::byte{0x00}, std::byte{0x01}};
        const crpc::result<std::vector<std::byte>> refused =
            encrypt_with(spki_pem(small_modulus, exponent), message, seed);
        CRPC_CHECK_FALSE(refused.ok());
        if (!refused.ok()) {
            CRPC_CHECK(refused.error().message.find("1024") != std::string::npos);
        }
    }

    // An even modulus.
    {
        std::vector<std::byte> even_modulus = modulus;
        even_modulus.back() = std::byte{0xca};
        std::vector<std::byte> exponent = {std::byte{0x01}, std::byte{0x00}, std::byte{0x01}};
        const crpc::result<std::vector<std::byte>> refused =
            encrypt_with(spki_pem(even_modulus, exponent), message, seed);
        CRPC_CHECK_FALSE(refused.ok());
        if (!refused.ok()) {
            CRPC_CHECK(refused.error().message.find("odd") != std::string::npos);
        }
    }
}

CRPC_TEST_MAIN()
