// CRPC -- RSA public-key encryption (RSAES-OAEP, SHA-1) for caching_sha2_password.
//
// Correctness bases, in order of authority:
//   * RSAES-OAEP is RFC 8017 section 7.1.1: EM = 0x00 || maskedSeed ||
//     maskedDB, DB = lHash || PS || 0x01 || M, masks from MGF1-SHA1. The label
//     is empty, so lHash is SHA1("") = da39a3ee5e6b4b0d3255bfef95601890afd80709.
//   * The primitive is RSAEP (RFC 8017 section 5.1.1): c = m^e mod n. Only a
//     small odd public exponent is supported, so square-and-multiply over a
//     32-bit exponent is the whole algorithm; there is no CRT and no private
//     key handling anywhere in this file.
//   * The DER layer reads exactly one shape: a PEM SubjectPublicKeyInfo
//     wrapping an RSAPublicKey (RFC 5280 appendix A / RFC 8017 appendix A.1),
//     which is what MySQL sends.
// Pinned against the official RSA-Labs PKCS#1 v2.1 OAEP vectors in
// tests/test_crypto_rsa.cpp.
#include <crpc/crypto/rsa.hpp>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <crpc/crypto/base64.hpp>
#include <crpc/crypto/hash.hpp>

#if defined(CRPC_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>
#endif

namespace crpc {
namespace crypto {
namespace {

// --- little-endian bignum (unsigned, base 2^32) ------------------------------
//
// Only what RSAEP needs: parse, multiply, reduce, exponentiate. Limbs are
// normalized (no trailing zeros) so comparisons and byte conversion are exact.

struct bignum {
    std::vector<std::uint32_t> limb; // limb[0] is least significant.

    void normalize() {
        while (!limb.empty() && limb.back() == 0) {
            limb.pop_back();
        }
    }

    static bignum from_bytes_be(span<const std::byte> bytes) {
        bignum out;
        out.limb.resize((bytes.size() + 3) / 4);
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            const std::size_t rev = bytes.size() - 1 - i; // byte index from the least significant end
            out.limb[rev / 4] |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i]))
                                 << (8 * (rev % 4));
        }
        out.normalize();
        return out;
    }

    // Fixed-length big-endian encoding. The caller picks the length (the RSA
    // ciphertext and modulus sizes agree with the key).
    [[nodiscard]] std::vector<std::byte> to_bytes_be(std::size_t length) const {
        std::vector<std::byte> out(length, std::byte{0});
        for (std::size_t i = 0; i < length; ++i) {
            const std::size_t rev = length - 1 - i; // byte index from the least significant end
            const std::uint32_t word = rev / 4 < limb.size() ? limb[rev / 4] : 0;
            out[i] = static_cast<std::byte>((word >> (8 * (rev % 4))) & 0xffu);
        }
        return out;
    }

    [[nodiscard]] std::size_t bit_length() const {
        if (limb.empty()) {
            return 0;
        }
        std::size_t bits = (limb.size() - 1) * 32;
        std::uint32_t top = limb.back();
        while (top != 0) {
            ++bits;
            top >>= 1;
        }
        return bits;
    }

    [[nodiscard]] bool is_odd() const noexcept {
        return !limb.empty() && (limb[0] & 1u) != 0;
    }

    [[nodiscard]] int compare(const bignum& other) const {
        if (limb.size() != other.limb.size()) {
            return limb.size() < other.limb.size() ? -1 : 1;
        }
        for (std::size_t i = limb.size(); i-- > 0;) {
            if (limb[i] != other.limb[i]) {
                return limb[i] < other.limb[i] ? -1 : 1;
            }
        }
        return 0;
    }

    // *this -= other; requires *this >= other.
    void subtract(const bignum& other) {
        std::uint64_t borrow = 0;
        for (std::size_t i = 0; i < limb.size(); ++i) {
            const std::uint64_t sub = i < other.limb.size() ? other.limb[i] : 0;
            const std::uint64_t value = static_cast<std::uint64_t>(limb[i]);
            const std::uint64_t total = sub + borrow; // sub <= 2^32-1, borrow <= 1: no overflow
            if (value >= total) {
                limb[i] = static_cast<std::uint32_t>(value - total);
                borrow = 0;
            } else {
                limb[i] = static_cast<std::uint32_t>(value + 0x100000000ull - total);
                borrow = 1;
            }
        }
        normalize();
    }

    [[nodiscard]] static bignum multiply(const bignum& a, const bignum& b) {
        bignum out;
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
        out.normalize();
        return out;
    }

    void shift_left_one() {
        std::uint32_t carry = 0;
        for (std::size_t i = 0; i < limb.size(); ++i) {
            const std::uint32_t next = limb[i] >> 31;
            limb[i] = (limb[i] << 1) | carry;
            carry = next;
        }
        if (carry != 0) {
            limb.push_back(carry);
        }
    }

    // *this mod n, by binary long division: feed the dividend bit by bit,
    // keeping the remainder below n. The dividend is at most twice the
    // modulus length, so this is at most 2*bits(n) shift/subtract rounds.
    [[nodiscard]] bignum mod(const bignum& n) const {
        bignum remainder;
        const std::size_t bits = bit_length();
        for (std::size_t i = bits; i-- > 0;) {
            remainder.shift_left_one();
            const std::size_t word = i / 32;
            const std::uint32_t bit = (limb[word] >> (i % 32)) & 1u;
            if (bit != 0) {
                if (remainder.limb.empty()) {
                    remainder.limb.push_back(1);
                } else {
                    remainder.limb[0] |= 1u;
                }
            }
            remainder.normalize();
            if (remainder.compare(n) >= 0) {
                remainder.subtract(n);
            }
        }
        return remainder;
    }

    [[nodiscard]] static bignum mod_multiply(const bignum& a, const bignum& b, const bignum& n) {
        return multiply(a, b).mod(n);
    }

    // RSAEP: base^exponent mod n with a 32-bit exponent. `base` must already
    // be reduced (the caller does `mod(n)` first); the result is reduced too.
    [[nodiscard]] static bignum mod_power(const bignum& base, std::uint32_t exponent,
                                          const bignum& n) {
        bignum result;
        result.limb.push_back(1); // 1
        bignum square = base;
        std::uint32_t remaining = exponent;
        while (remaining != 0) {
            if ((remaining & 1u) != 0) {
                result = mod_multiply(result, square, n);
            }
            remaining >>= 1;
            if (remaining != 0) {
                square = mod_multiply(square, square, n);
            }
        }
        return result;
    }
};

// --- cryptographic randomness (no fallback) ----------------------------------

// Fills `out` from the operating system's cryptographic random generator.
// Every failure mode is an error; there is no predictable fallback, because a
// predictable OAEP seed would let an attacker recover the password offline.
[[nodiscard]] bool fill_random_bytes(span<std::byte> out) {
    if (out.empty()) {
        return true;
    }
#if defined(CRPC_OS_WINDOWS)
    // BCRYPT_USE_SYSTEM_PREFERRED_RNG lets the OS pick its approved generator;
    // a negative NTSTATUS (all failures) is an error.
    const LONG status =
        ::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(out.data()),
                          static_cast<ULONG>(out.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status >= 0;
#else
    // getrandom(2) may return short reads on very large requests and is
    // interrupted by signals; loop until the buffer is full.
    std::size_t done = 0;
    while (done < out.size()) {
        const ssize_t got = ::getrandom(out.data() + done, out.size() - done, 0);
        if (got > 0) {
            done += static_cast<std::size_t>(got);
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        // getrandom(2) unavailable (old kernel): read /dev/urandom once.
        const int fd = ::open("/dev/urandom", O_RDONLY);
        if (fd < 0) {
            return false;
        }
        bool ok = true;
        std::size_t filled = 0;
        while (filled < out.size()) {
            const ssize_t read_bytes = ::read(fd, out.data() + filled, out.size() - filled);
            if (read_bytes <= 0) {
                ok = false;
                break;
            }
            filled += static_cast<std::size_t>(read_bytes);
        }
        ::close(fd);
        return ok;
    }
    return true;
#endif
}

// --- PEM / DER ---------------------------------------------------------------

constexpr std::string_view pem_begin = "-----BEGIN PUBLIC KEY-----";
constexpr std::string_view pem_end = "-----END PUBLIC KEY-----";

[[nodiscard]] std::string pem_base64_body(std::string_view pem) {
    const std::size_t begin = pem.find(pem_begin);
    if (begin == std::string_view::npos) {
        return {};
    }
    const std::size_t body_start = begin + pem_begin.size();
    const std::size_t end = pem.find(pem_end, body_start);
    if (end == std::string_view::npos) {
        return {};
    }
    std::string body(pem.substr(body_start, end - body_start));
    // base64_decode skips whitespace, but a stray NUL would not be whitespace;
    // drop anything that is not base64 alphabet or whitespace.
    std::string cleaned;
    cleaned.reserve(body.size());
    for (const char c : body) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '+' || c == '/' || c == '=' || c == ' ' || c == '\t' || c == '\r' ||
            c == '\n' || c == '\f' || c == '\v') {
            cleaned.push_back(c);
        }
    }
    return cleaned;
}

struct der_cursor {
    span<const std::byte> data;
    std::size_t pos = 0;

    [[nodiscard]] bool at_end() const noexcept { return pos >= data.size(); }

    [[nodiscard]] bool read_byte(std::uint8_t& out) {
        if (pos >= data.size()) {
            return false;
        }
        out = static_cast<std::uint8_t>(data[pos]);
        ++pos;
        return true;
    }

    // Reads one TLV header; `out` becomes the content span.
    [[nodiscard]] bool read_tlv(std::uint8_t& tag, span<const std::byte>& out) {
        if (!read_byte(tag)) {
            return false;
        }
        std::uint8_t first = 0;
        if (!read_byte(first)) {
            return false;
        }
        std::size_t length = 0;
        if (first < 0x80u) {
            length = first;
        } else {
            const std::size_t extra = first & 0x7fu;
            if (extra == 0 || extra > 4) { // indefinite and absurd lengths are refused
                return false;
            }
            for (std::size_t i = 0; i < extra; ++i) {
                std::uint8_t byte = 0;
                if (!read_byte(byte)) {
                    return false;
                }
                length = (length << 8) | byte;
            }
            if (length < 0x80u) { // non-minimal long form
                return false;
            }
        }
        if (length > data.size() - pos) {
            return false;
        }
        out = span<const std::byte>(data.data() + pos, length);
        pos += length;
        return true;
    }

    // Reads an INTEGER content and strips leading zeros (DER allows one 0x00
    // before a positive number's high bit).
    [[nodiscard]] bool read_integer(span<const std::byte>& out) {
        std::uint8_t tag = 0;
        span<const std::byte> content;
        if (!read_tlv(tag, content) || tag != 0x02u || content.empty()) {
            return false;
        }
        std::size_t offset = 0;
        while (offset + 1 < content.size() && content[offset] == std::byte{0}) {
            ++offset;
        }
        out = span<const std::byte>(content.data() + offset, content.size() - offset);
        return !out.empty();
    }
};

[[nodiscard]] bool is_oid_rsa_encryption(span<const std::byte> oid) {
    static const std::byte rsa_oid[] = {std::byte{0x2a}, std::byte{0x86}, std::byte{0x48},
                                        std::byte{0x86}, std::byte{0xf7}, std::byte{0x0d},
                                        std::byte{0x01}, std::byte{0x01}, std::byte{0x01}};
    if (oid.size() != sizeof rsa_oid) {
        return false;
    }
    return std::memcmp(oid.data(), rsa_oid, sizeof rsa_oid) == 0;
}

struct rsa_public_key {
    std::vector<std::byte> modulus;   // big-endian, no leading zero
    std::vector<std::byte> exponent;  // big-endian, no leading zero
};

// Parses a SubjectPublicKeyInfo containing rsaEncryption. Everything else is
// refused with a precise message so an operator can tell a wrong key file
// from a corrupt one.
[[nodiscard]] bool parse_spki(span<const std::byte> der, rsa_public_key& out) {
    der_cursor cursor{der, 0};
    std::uint8_t tag = 0;
    span<const std::byte> spki;
    if (!cursor.read_tlv(tag, spki) || tag != 0x30u || !cursor.at_end()) {
        return false;
    }

    der_cursor outer{spki, 0};
    span<const std::byte> algorithm;
    span<const std::byte> subject_key;
    if (!outer.read_tlv(tag, algorithm) || tag != 0x30u) {
        return false;
    }
    if (!outer.read_tlv(tag, subject_key) || tag != 0x03u) {
        return false;
    }
    if (!outer.at_end()) {
        return false;
    }
    if (subject_key.empty() || subject_key[0] != std::byte{0}) { // unused-bits count must be 0
        return false;
    }
    subject_key = span<const std::byte>(subject_key.data() + 1, subject_key.size() - 1);

    // AlgorithmIdentifier: SEQUENCE { OID rsaEncryption, NULL (optional) }.
    der_cursor algorithm_cursor{algorithm, 0};
    span<const std::byte> oid;
    if (!algorithm_cursor.read_tlv(tag, oid) || tag != 0x06u || !is_oid_rsa_encryption(oid)) {
        return false;
    }
    if (!algorithm_cursor.at_end()) { // optional NULL parameters, nothing else
        span<const std::byte> parameters;
        if (!algorithm_cursor.read_tlv(tag, parameters) || tag != 0x05u) {
            return false;
        }
        if (!algorithm_cursor.at_end()) {
            return false;
        }
    }

    der_cursor key_cursor{subject_key, 0};
    span<const std::byte> key_sequence;
    if (!key_cursor.read_tlv(tag, key_sequence) || tag != 0x30u || !key_cursor.at_end()) {
        return false;
    }
    der_cursor integers{key_sequence, 0};
    span<const std::byte> modulus;
    span<const std::byte> exponent;
    if (!integers.read_integer(modulus) || !integers.read_integer(exponent) ||
        !integers.at_end()) {
        return false;
    }
    out.modulus.assign(modulus.begin(), modulus.end());
    out.exponent.assign(exponent.begin(), exponent.end());
    return true;
}

// --- MGF1 / OAEP (RFC 8017) --------------------------------------------------

constexpr std::size_t sha1_size = 20;

// MGF1 with SHA-1 (RFC 8017 appendix B.2.1): T = SHA1(seed || C0) || SHA1(seed
// || C1) || ... truncated to `length`, with C a 4-byte big-endian counter.
[[nodiscard]] std::vector<std::byte> mgf1_sha1(span<const std::byte> seed, std::size_t length) {
    std::vector<std::byte> out;
    out.reserve(length + sha1_size);
    std::vector<std::byte> input(seed.size() + 4);
    std::memcpy(input.data(), seed.data(), seed.size());
    for (std::uint32_t counter = 0; out.size() < length; ++counter) {
        input[seed.size()] = static_cast<std::byte>((counter >> 24) & 0xffu);
        input[seed.size() + 1] = static_cast<std::byte>((counter >> 16) & 0xffu);
        input[seed.size() + 2] = static_cast<std::byte>((counter >> 8) & 0xffu);
        input[seed.size() + 3] = static_cast<std::byte>(counter & 0xffu);
        const sha1_digest block = sha1(span<const std::byte>(input.data(), input.size()));
        for (const std::uint8_t value : block) {
            out.push_back(static_cast<std::byte>(value));
        }
    }
    out.resize(length);
    return out;
}

[[nodiscard]] result<std::vector<std::byte>> oaep_sha1_encrypt(
    span<const std::byte> modulus_be, span<const std::byte> exponent_be,
    span<const std::byte> message, span<const std::byte> seed) {
    const bignum n = bignum::from_bytes_be(modulus_be);
    if (!n.is_odd()) {
        return rpc_error{rpc_code::invalid_argument, "the RSA public key modulus must be odd"};
    }
    const std::size_t bits = n.bit_length();
    if (bits < 1024 || bits > 8192) {
        return rpc_error{rpc_code::invalid_argument,
                      "the RSA public key modulus must be 1024..8192 bits"};
    }
    if (exponent_be.empty() || exponent_be.size() > 4) {
        return rpc_error{rpc_code::invalid_argument,
                      "the RSA public key exponent must fit in 32 bits"};
    }
    std::uint32_t exponent = 0;
    for (const std::byte byte : exponent_be) {
        exponent = (exponent << 8) | static_cast<std::uint32_t>(static_cast<unsigned char>(byte));
    }
    if (exponent < 3 || (exponent & 1u) == 0) {
        return rpc_error{rpc_code::invalid_argument, "the RSA public key exponent must be odd and >= 3"};
    }

    const std::size_t k = modulus_be.size();
    if (message.size() > k - 2 * sha1_size - 2) {
        return rpc_error{rpc_code::invalid_argument,
                      "the message is too long for this RSA-OAEP key (max " +
                          std::to_string(k - 2 * sha1_size - 2) + " bytes)"};
    }
    if (seed.size() != sha1_size) {
        return rpc_error{rpc_code::invalid_argument, "the OAEP seed must be 20 bytes"};
    }

    // Step 2: the encoded message. lHash is SHA1 of the empty label -- the
    // real SHA-1 value, not zeros.
    static const std::byte empty_label_digest[sha1_size] = {
        std::byte{0xda}, std::byte{0x39}, std::byte{0xa3}, std::byte{0xee}, std::byte{0x5e},
        std::byte{0x6b}, std::byte{0x4b}, std::byte{0x0d}, std::byte{0x32}, std::byte{0x55},
        std::byte{0xbf}, std::byte{0xef}, std::byte{0x95}, std::byte{0x60}, std::byte{0x18},
        std::byte{0x90}, std::byte{0xaf}, std::byte{0xd8}, std::byte{0x07}, std::byte{0x09}};
    const sha1_digest computed = sha1(span<const std::byte>());
    (void)computed; // cross-check the pinned constant against the real SHA-1
    if (std::memcmp(computed.data(), empty_label_digest, sha1_size) != 0) {
        return rpc_error{rpc_code::internal, "SHA-1 of the empty string does not match RFC 8017"};
    }

    std::vector<std::byte> db(k - sha1_size - 1, std::byte{0});
    std::memcpy(db.data(), empty_label_digest, sha1_size);
    const std::size_t ps_length = k - message.size() - 2 * sha1_size - 2;
    db[sha1_size + ps_length] = std::byte{0x01};
    std::memcpy(db.data() + sha1_size + ps_length + 1, message.data(), message.size());

    std::vector<std::byte> masked_db = mgf1_sha1(seed, db.size());
    for (std::size_t i = 0; i < db.size(); ++i) {
        masked_db[i] = db[i] ^ masked_db[i];
    }
    std::vector<std::byte> seed_mask = mgf1_sha1(span<const std::byte>(masked_db.data(), masked_db.size()), sha1_size);
    std::vector<std::byte> masked_seed(sha1_size, std::byte{0});
    for (std::size_t i = 0; i < sha1_size; ++i) {
        masked_seed[i] = seed[i] ^ seed_mask[i];
    }

    std::vector<std::byte> em;
    em.reserve(k);
    em.push_back(std::byte{0});
    em.insert(em.end(), masked_seed.begin(), masked_seed.end());
    em.insert(em.end(), masked_db.begin(), masked_db.end());

    const bignum m = bignum::from_bytes_be(span<const std::byte>(em.data(), em.size()));
    const bignum cipher = bignum::mod_power(m, exponent, n);
    return cipher.to_bytes_be(k);
}

} // namespace

result<std::vector<std::byte>> rsa_oaep_sha1_encrypt_pem(std::string_view public_key_pem,
                                                         span<const std::byte> message) {
    std::vector<std::byte> seed(sha1_size, std::byte{0});
    if (!fill_random_bytes(span<std::byte>(seed.data(), seed.size()))) {
        return rpc_error{rpc_code::internal,
                      "the system cryptographic random generator failed; refusing to "
                      "encrypt with a predictable OAEP seed"};
    }
    return rsa_oaep_sha1_encrypt_pem_with_seed(public_key_pem, message,
                                               span<const std::byte>(seed.data(), seed.size()));
}

result<std::vector<std::byte>> rsa_oaep_sha1_encrypt_pem_with_seed(std::string_view public_key_pem,
                                                                   span<const std::byte> message,
                                                                   span<const std::byte> seed) {
    const std::string body = pem_base64_body(public_key_pem);
    if (body.empty()) {
        return rpc_error{rpc_code::invalid_argument,
                      "the public key must be a PEM \"-----BEGIN PUBLIC KEY-----\" "
                      "SubjectPublicKeyInfo block"};
    }
    const result<std::string> der_text = base64_decode(body);
    if (!der_text) {
        return rpc_error{rpc_code::invalid_argument, "the PEM public key body is not valid base64"};
    }
    const span<const std::byte> der(reinterpret_cast<const std::byte*>(der_text.value().data()),
                                    der_text.value().size());
    rsa_public_key key;
    if (!parse_spki(der, key)) {
        return rpc_error{rpc_code::invalid_argument,
                      "the PEM public key is not an RSA SubjectPublicKeyInfo"};
    }
    return oaep_sha1_encrypt(span<const std::byte>(key.modulus.data(), key.modulus.size()),
                             span<const std::byte>(key.exponent.data(), key.exponent.size()),
                             message, seed);
}

} // namespace crypto
} // namespace crpc
