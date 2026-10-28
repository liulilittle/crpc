// CRPC -- pinned RSA-OAEP test material and an independent reference checker.
//
// The vector is the official RSA Labs "RSA-OAEP encryption with intermediate
// values" example (pkcs-1v2-1d2-vec/oaep-int.txt), the companion vector set to
// PKCS #1 v2.1/v2.2; the copy verified here is the verbatim mirror in
// pyca/cryptography (vectors/cryptography_vectors/asymmetric/RSA/
// pkcs-1v2-1d2-vec/oaep-int.txt). It was cross-checked before being committed
// here: p*q == n, EM^e mod n == C, C^d mod n == EM, the CRT components
// reproduce the published m1/m2, and OpenSSL 3.0.13 decrypts the pinned
// ciphertext (OAEP-SHA1) back to the pinned message. The key is a public test
// example from a 2001 RSA Labs document; nothing secret, nothing production.
//
// Everything in this header is test-side ONLY. The reference decryptor and the
// OAEP decoder are deliberately written against the raw RFC 8017 definitions
// (not against the library's own bignum), so the pinned KAT plus these checks
// cannot pass by two copies of one bug agreeing with each other.
#ifndef CRPC_TESTS_RSA_TEST_SUPPORT_HPP
#define CRPC_TESTS_RSA_TEST_SUPPORT_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <crpc/crypto/hash.hpp>
#include <crpc/span.hpp>

namespace rsa_test {

// --- the pinned vector -------------------------------------------------------

inline constexpr const char* vector_n =
    "bbf82f090682ce9c2338ac2b9da871f7368d07eed41043a440d6b6f07454f51fb8dfbaaf035c02ab61ea48ceeb6f"
    "cd4876ed520d60e1ec4619719d8a5b8b807fafb8e0a3dfc737723ee6b4b7d93a2584ee6a649d060953748834b245"
    "4598394ee0aab12d7b61a51f527a9a41f6c1687fe2537298ca2a8f5946f8e5fd091dbdcb";
inline constexpr const char* vector_e = "11"; // 17, an unusual but legal public exponent
inline constexpr const char* vector_d =
    "a5dafc5341faf289c4b988db30c1cdf83f31251e0668b42784813801579641b29410b3c7998d6bc465745e5c392669d6"
    "870da2c082a939e37fdcb82ec93edac97ff3ad5950accfbc111c76f1a9529444e56aaf68c56c092cd38dc3bef5d20a93"
    "9926ed4f74a13eddfbe1a1cecc4894af9428c2b7b8883fe4463a4bc85b1cb3c1";
inline constexpr const char* vector_p =
    "eecfae81b1b9b3c908810b10a1b5600199eb9f44aef4fda493b81a9e3d84f632124ef0236e5d1e3b7e28fae7aa040a2d"
    "5b252176459d1f397541ba2a58fb6599";
inline constexpr const char* vector_q =
    "c97fb1f027f453f6341233eaaad1d9353f6c42d08866b1d05a0f2035028b9d869840b41666b42e92ea0da3b43204b5cf"
    "ce3352524d0416a5a441e700af461503";
inline constexpr const char* vector_dp =
    "54494ca63eba0337e4e24023fcd69a5aeb07dddc0183a4d0ac9b54b051f2b13ed9490975eab77414ff59c1f7692e9a2e"
    "202b38fc910a474174adc93c1f67c981";
inline constexpr const char* vector_dq =
    "471e0290ff0af0750351b7f878864ca961adbd3a8a7e991c5c0556a94c3146a7f9803f8f6f8ae342e931fd8ae47a220d"
    "1b99a495849807fe39f9245a9836da3d";
inline constexpr const char* vector_qinv =
    "b06c4fdabb6301198d265bdbae9423b380f271f73453885093077fcd39e2119fc98632154f5883b167a967bf402b4e9e"
    "2e0f9656e698ea3666edfb25798039f7";
inline constexpr const char* vector_message = "d436e99569fd32a7c8a05bbc90d32c49";
inline constexpr const char* vector_seed = "aafd12f659cae63489b479e5076ddec2f06cb58f";
// EM as published omits the implied leading 0x00 octet (the document prints
// the 127 significant bytes); the "00" prefix here restores the full
// k-byte encoding that RSAEP consumes and RSADP produces.
inline constexpr const char* vector_em =
    "00eb7a19ace9e3006350e329504b45e2ca82310b26dcd87d5c68f1eea8f55267c31b2e8bb4251f84d7e0b2c04626f5a"
    "ff93edcfb25c9c2b3ff8ae10e839a2ddb4cdcfe4ff47728b4a1b7c1362baad29ab48d2869d5024121435811591be392"
    "f982fb3e87d095aeb40448db972f3ac14f7bc275195281ce32d2f1b76d4d353e2d";
inline constexpr const char* vector_ciphertext =
    "1253e04dc0a5397bb44a7ab87e9bf2a039a33d1e996fc82a94ccd30074c95df763722017069e5268da5d1c0b4f872cf6"
    "53c11df82314a67968dfeae28def04bb6d84b1c31d654a1970e5783bd6eb96a024c2ca2f4a90fe9f2ef5c9c140e5bb48"
    "da9536ad8700c84fc9130adea74e558d51a74ddf85d8b50de96838d6063e0955";

// The SubjectPublicKeyInfo PEM built from vector_n / vector_e (DER verified
// against OpenSSL 3.0.13: parses, modulus matches, 1024-bit).
inline constexpr const char* vector_pem =
    "-----BEGIN PUBLIC KEY-----\n"
    "MIGdMA0GCSqGSIb3DQEBAQUAA4GLADCBhwKBgQC7+C8JBoLOnCM4rCudqHH3No0H\n"
    "7tQQQ6RA1rbwdFT1H7jfuq8DXAKrYepIzutvzUh27VINYOHsRhlxnYpbi4B/r7jg\n"
    "o9/HN3I+5rS32TolhO5qZJ0GCVN0iDSyRUWYOU7gqrEte2GlH1J6mkH2wWh/4lNy\n"
    "mMoqj1lG+OX9CR29ywIBEQ==\n"
    "-----END PUBLIC KEY-----";

// --- hex helpers -------------------------------------------------------------

inline std::vector<std::byte> hex_bytes(const char* hex) {
    std::vector<std::byte> out;
    while (*hex != '\0') {
        if (*hex == ' ' || *hex == '\n') {
            ++hex;
            continue;
        }
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return c - 'A' + 10;
        };
        out.push_back(static_cast<std::byte>((nibble(hex[0]) << 4) | nibble(hex[1])));
        hex += 2;
    }
    return out;
}

inline std::string bytes_hex(const std::vector<std::byte>& data) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (const std::byte b : data) {
        out.push_back(digits[static_cast<int>(b) >> 4]);
        out.push_back(digits[static_cast<int>(b) & 0xf]);
    }
    return out;
}

// --- test-side bignum (unsigned, little-endian uint32 limbs) -----------------

struct ref_number {
    std::vector<std::uint32_t> limb;

    static ref_number from_bytes(const std::vector<std::byte>& bytes) {
        ref_number out;
        out.limb.assign((bytes.size() + 3) / 4, 0);
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            const std::size_t rev = bytes.size() - 1 - i;
            out.limb[rev / 4] |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i]))
                                 << (8 * (rev % 4));
        }
        out.trim();
        return out;
    }

    static ref_number from_u32(std::uint32_t value) {
        ref_number out;
        if (value != 0) out.limb.push_back(value);
        return out;
    }

    void trim() {
        while (!limb.empty() && limb.back() == 0) limb.pop_back();
    }

    [[nodiscard]] bool is_zero() const { return limb.empty(); }

    [[nodiscard]] int compare(const ref_number& other) const {
        if (limb.size() != other.limb.size()) return limb.size() < other.limb.size() ? -1 : 1;
        for (std::size_t i = limb.size(); i-- > 0;) {
            if (limb[i] != other.limb[i]) return limb[i] < other.limb[i] ? -1 : 1;
        }
        return 0;
    }

    // *this -= other, requires *this >= other.
    void subtract(const ref_number& other) {
        std::uint64_t borrow = 0;
        for (std::size_t i = 0; i < limb.size(); ++i) {
            const std::uint64_t sub = i < other.limb.size() ? other.limb[i] : 0;
            const std::uint64_t value = limb[i];
            const std::uint64_t total = sub + borrow;
            if (value >= total) {
                limb[i] = static_cast<std::uint32_t>(value - total);
                borrow = 0;
            } else {
                limb[i] = static_cast<std::uint32_t>(value + 0x100000000ull - total);
                borrow = 1;
            }
        }
        trim();
    }

    void add(const ref_number& other) {
        limb.resize(std::max(limb.size(), other.limb.size()), 0);
        std::uint64_t carry = 0;
        for (std::size_t i = 0; i < limb.size(); ++i) {
            // Both operands must widen before the sum: uint32 + uint32 would
            // wrap mod 2^32 and lose the carry-out.
            const std::uint64_t sum = static_cast<std::uint64_t>(limb[i]) +
                                      (i < other.limb.size() ? other.limb[i] : 0u) + carry;
            limb[i] = static_cast<std::uint32_t>(sum & 0xffffffffu);
            carry = sum >> 32;
        }
        if (carry != 0) limb.push_back(static_cast<std::uint32_t>(carry));
        trim();
    }

    [[nodiscard]] static ref_number multiply(const ref_number& a, const ref_number& b) {
        ref_number out;
        out.limb.assign(a.limb.size() + b.limb.size(), 0);
        for (std::size_t i = 0; i < a.limb.size(); ++i) {
            std::uint64_t carry = 0;
            for (std::size_t j = 0; j < b.limb.size(); ++j) {
                const std::uint64_t product =
                    static_cast<std::uint64_t>(a.limb[i]) * b.limb[j] + out.limb[i + j] + carry;
                out.limb[i + j] = static_cast<std::uint32_t>(product & 0xffffffffu);
                carry = product >> 32;
            }
            std::size_t k = i + b.limb.size();
            while (carry != 0) {
                const std::uint64_t sum = out.limb[k] + carry;
                out.limb[k] = static_cast<std::uint32_t>(sum & 0xffffffffu);
                carry = sum >> 32;
                ++k;
            }
        }
        out.trim();
        return out;
    }

    // *this mod m by binary long division.
    [[nodiscard]] ref_number mod(const ref_number& m) const {
        ref_number remainder;
        std::size_t bits = 0;
        if (!limb.empty()) {
            bits = (limb.size() - 1) * 32;
            std::uint32_t top = limb.back();
            while (top != 0) { ++bits; top >>= 1; }
        }
        for (std::size_t i = bits; i-- > 0;) {
            std::uint32_t carry = 0;
            for (std::size_t j = 0; j < remainder.limb.size(); ++j) {
                const std::uint32_t next = remainder.limb[j] >> 31;
                remainder.limb[j] = (remainder.limb[j] << 1) | carry;
                carry = next;
            }
            if (carry != 0) remainder.limb.push_back(carry);
            if (remainder.limb.empty()) remainder.limb.push_back(0);
            remainder.limb[0] |= (limb[i / 32] >> (i % 32)) & 1u;
            remainder.trim();
            if (remainder.compare(m) >= 0) remainder.subtract(m);
        }
        return remainder;
    }

    [[nodiscard]] ref_number mod_pow(const ref_number& exponent, const ref_number& modulus) const {
        ref_number result = ref_number::from_u32(1);
        ref_number square = this->mod(modulus);
        for (std::size_t i = 0; i < exponent.limb.size(); ++i) {
            for (int bit = 0; bit < 32; ++bit) {
                if (((exponent.limb[i] >> bit) & 1u) != 0) {
                    result = ref_number::multiply(result, square).mod(modulus);
                }
                square = ref_number::multiply(square, square).mod(modulus);
            }
        }
        return result;
    }

    [[nodiscard]] std::vector<std::byte> to_bytes(std::size_t length) const {
        std::vector<std::byte> out(length, std::byte{0});
        for (std::size_t i = 0; i < length; ++i) {
            const std::size_t rev = length - 1 - i;
            const std::uint32_t word = rev / 4 < limb.size() ? limb[rev / 4] : 0;
            out[i] = static_cast<std::byte>((word >> (8 * (rev % 4))) & 0xffu);
        }
        return out;
    }
};

// RSADP with CRT (RFC 8017 section 5.1.2), the test-side counterpart of the
// library's encrypt-only primitive.
inline std::vector<std::byte> reference_decrypt(const std::vector<std::byte>& ciphertext) {
    const ref_number c = ref_number::from_bytes(ciphertext);
    const ref_number p = ref_number::from_bytes(hex_bytes(vector_p));
    const ref_number q = ref_number::from_bytes(hex_bytes(vector_q));
    const ref_number dp = ref_number::from_bytes(hex_bytes(vector_dp));
    const ref_number dq = ref_number::from_bytes(hex_bytes(vector_dq));
    const ref_number qinv = ref_number::from_bytes(hex_bytes(vector_qinv));

    ref_number m1 = c.mod(p).mod_pow(dp, p);
    ref_number m2 = c.mod(q).mod_pow(dq, q);
    // h = qinv * (m1 - m2) mod p; m = m2 + q*h
    if (m1.compare(m2) < 0) {
        m1.add(p);
    }
    m1.subtract(m2);
    ref_number h = ref_number::multiply(qinv, m1).mod(p);
    ref_number m = ref_number::multiply(q, h);
    m.add(m2);
    return m.to_bytes(ciphertext.size());
}

// --- test-side OAEP decode (RFC 8017 section 7.1.2, SHA-1, empty label) ------

inline std::vector<std::byte> mgf1_sha1(const std::vector<std::byte>& seed, std::size_t length) {
    std::vector<std::byte> out;
    std::vector<std::byte> input(seed.size() + 4);
    std::memcpy(input.data(), seed.data(), seed.size());
    for (std::uint32_t counter = 0; out.size() < length; ++counter) {
        input[seed.size()] = static_cast<std::byte>((counter >> 24) & 0xffu);
        input[seed.size() + 1] = static_cast<std::byte>((counter >> 16) & 0xffu);
        input[seed.size() + 2] = static_cast<std::byte>((counter >> 8) & 0xffu);
        input[seed.size() + 3] = static_cast<std::byte>(counter & 0xffu);
        const crpc::crypto::sha1_digest block = crpc::crypto::sha1(
            crpc::span<const std::byte>(input.data(), input.size()));
        for (const std::uint8_t value : block) {
            out.push_back(static_cast<std::byte>(value));
        }
    }
    out.resize(length);
    return out;
}

// Returns false when the encoded message is not valid (wrong label hash, no
// 0x01 separator, empty message).
inline bool reference_oaep_decode(const std::vector<std::byte>& em, std::vector<std::byte>& out) {
    const std::size_t k = em.size();
    const std::size_t hash_size = 20;
    if (k < 2 * hash_size + 2 || em[0] != std::byte{0}) {
        return false;
    }
    std::vector<std::byte> masked_seed(em.begin() + 1, em.begin() + 1 + hash_size);
    std::vector<std::byte> masked_db(em.begin() + 1 + hash_size, em.end());
    const std::vector<std::byte> seed_mask =
        mgf1_sha1(masked_db, hash_size);
    std::vector<std::byte> seed(hash_size);
    for (std::size_t i = 0; i < hash_size; ++i) seed[i] = masked_seed[i] ^ seed_mask[i];
    const std::vector<std::byte> db_mask = mgf1_sha1(seed, masked_db.size());
    std::vector<std::byte> db(masked_db.size());
    for (std::size_t i = 0; i < db.size(); ++i) db[i] = masked_db[i] ^ db_mask[i];

    // lHash must be SHA-1 of the empty label.
    const crpc::crypto::sha1_digest lhash = crpc::crypto::sha1(crpc::span<const std::byte>());
    if (std::memcmp(db.data(), lhash.data(), hash_size) != 0) {
        return false;
    }
    std::size_t separator = hash_size;
    while (separator < db.size() && db[separator] == std::byte{0}) {
        ++separator;
    }
    if (separator >= db.size() || db[separator] != std::byte{0x01}) {
        return false;
    }
    out.assign(db.begin() + separator + 1, db.end());
    return !out.empty();
}

} // namespace rsa_test

#endif // CRPC_TESTS_RSA_TEST_SUPPORT_HPP
