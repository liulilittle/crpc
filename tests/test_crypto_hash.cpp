// CRPC -- SHA-1, SHA-256 and MySQL authentication scramble tests.
//
// The hash vectors are the published ones (FIPS 180-4 and the RFC 3174 examples).
// The scramble vectors were computed with Python's hashlib from the construction
// documented in the MySQL protocol, i.e. by an independent implementation, so a
// wrong XOR order or a wrong nonce position cannot pass by agreeing with itself.
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/crypto/hash.hpp>
#include <crpc/db/mysql/auth.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace {

[[nodiscard]] crpc::span<const std::byte> as_bytes(std::string_view text) noexcept {
    return crpc::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                       text.size());
}

[[nodiscard]] std::string hex(const std::vector<std::byte>& data) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (const std::byte value : data) {
        const auto byte = static_cast<std::uint8_t>(value);
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0f]);
    }
    return out;
}

// The 20-byte challenge used by the scramble vectors: 0x01..0x14.
[[nodiscard]] std::vector<std::byte> challenge() {
    std::vector<std::byte> nonce(20);
    for (std::size_t i = 0; i < nonce.size(); ++i) {
        nonce[i] = static_cast<std::byte>(i + 1);
    }
    return nonce;
}

} // namespace

CRPC_TEST(sha1_published_vectors) {
    CRPC_CHECK_EQ(std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"),
                  crpc::crypto::to_hex(crpc::crypto::sha1(as_bytes(""))));
    CRPC_CHECK_EQ(std::string("a9993e364706816aba3e25717850c26c9cd0d89d"),
                  crpc::crypto::to_hex(crpc::crypto::sha1(as_bytes("abc"))));
    // RFC 3174's two-block example.
    CRPC_CHECK_EQ(std::string("84983e441c3bd26ebaae4aa1f95129e5e54670f1"),
                  crpc::crypto::to_hex(crpc::crypto::sha1(
                      as_bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))));
    // 56 bytes is the first length where the padding does not fit in the block.
    CRPC_CHECK_EQ(std::string("c2db330f6083854c99d4b5bfb6e8f29f201be699"),
                  crpc::crypto::to_hex(crpc::crypto::sha1(as_bytes(std::string(56, 'a')))));
}

CRPC_TEST(sha256_published_vectors) {
    CRPC_CHECK_EQ(
        std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
        crpc::crypto::to_hex(crpc::crypto::sha256(as_bytes(""))));
    CRPC_CHECK_EQ(
        std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
        crpc::crypto::to_hex(crpc::crypto::sha256(as_bytes("abc"))));
    CRPC_CHECK_EQ(
        std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
        crpc::crypto::to_hex(crpc::crypto::sha256(
            as_bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))));
    CRPC_CHECK_EQ(
        std::string("b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"),
        crpc::crypto::to_hex(crpc::crypto::sha256(as_bytes(std::string(56, 'a')))));
    // Sixteen blocks, to catch a block-counter bug.
    CRPC_CHECK_EQ(
        std::string("41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"),
        crpc::crypto::to_hex(crpc::crypto::sha256(as_bytes(std::string(1000, 'a')))));
}

CRPC_TEST(hash_two_span_form_equals_concatenation) {
    const std::string first = "abcdbcde";
    const std::string second = "cdefdefg";
    const std::string joined = first + second;

    CRPC_CHECK(crpc::crypto::sha1(as_bytes(first), as_bytes(second)) ==
               crpc::crypto::sha1(as_bytes(joined)));
    CRPC_CHECK(crpc::crypto::sha256(as_bytes(first), as_bytes(second)) ==
               crpc::crypto::sha256(as_bytes(joined)));

    // An empty first span must behave like a plain hash.
    CRPC_CHECK(crpc::crypto::sha256(as_bytes(""), as_bytes("abc")) ==
               crpc::crypto::sha256(as_bytes("abc")));
}

CRPC_TEST(mysql_native_scramble_vectors) {
    struct sample {
        const char* password;
        const char* expected;
    };
    const sample samples[] = {
        {"secret", "b32bb3a583e1340c0a1108d58b1be49781ad8c2f"},
        {"p@ss w0rd!", "117c496a9bb825fb081006b9b1d65da9bf455dc5"},
        {"\xe5\xaf\x86\xe7\xa0\x81", "3be2253e0b6c2857aae861435061368c1c6d1c70"},
    };
    const std::vector<std::byte> nonce = challenge();
    for (const sample& item : samples) {
        const auto token = crpc::db::mysql::scramble_native_password(
            item.password, crpc::span<const std::byte>(nonce.data(), nonce.size()));
        CRPC_CHECK(token.ok());
        if (!token.ok()) {
            continue;
        }
        // 20 bytes, the length of SHA-1.
        CRPC_CHECK_EQ(std::size_t{20}, token.value().size());
        CRPC_CHECK_EQ(std::string(item.expected), hex(token.value()));
    }
}

CRPC_TEST(mysql_caching_sha2_scramble_vectors) {
    struct sample {
        const char* password;
        const char* expected;
    };
    const sample samples[] = {
        {"secret",
         "746ebe205d56a0707acb3e796e834e0dd7b1d61743b26bd5202c7a623230c7c9"},
        {"p@ss w0rd!",
         "5088b1baa885895a8dbacc713c10ff7167d6b37d7a712874207e25abb36b4fa1"},
        {"\xe5\xaf\x86\xe7\xa0\x81",
         "c10842cd9fe9cf7ab2fbb9bf316eb0751743258c3b3f717f8bc592a6ff323969"},
    };
    const std::vector<std::byte> nonce = challenge();
    for (const sample& item : samples) {
        const auto token = crpc::db::mysql::scramble_caching_sha2_password(
            item.password, crpc::span<const std::byte>(nonce.data(), nonce.size()));
        CRPC_CHECK(token.ok());
        if (!token.ok()) {
            continue;
        }
        // 32 bytes, the length of SHA-256.
        CRPC_CHECK_EQ(std::size_t{32}, token.value().size());
        CRPC_CHECK_EQ(std::string(item.expected), hex(token.value()));
    }
}

CRPC_TEST(mysql_empty_password_yields_empty_response) {
    const std::vector<std::byte> nonce = challenge();
    // An empty password is not scrambled: the response is empty, which is how the
    // server tells "no password" from "some password".
    const auto native = crpc::db::mysql::scramble_native_password(
        "", crpc::span<const std::byte>(nonce.data(), nonce.size()));
    CRPC_CHECK(native.ok());
    if (native.ok()) {
        CRPC_CHECK(native.value().empty());
    }
    const auto sha2 = crpc::db::mysql::scramble_caching_sha2_password(
        "", crpc::span<const std::byte>(nonce.data(), nonce.size()));
    CRPC_CHECK(sha2.ok());
    if (sha2.ok()) {
        CRPC_CHECK(sha2.value().empty());
    }
}

CRPC_TEST(mysql_scramble_rejects_wrong_nonce_length) {
    // A 19-byte nonce would produce a token the server silently rejects; refusing
    // it locally is the only way the caller learns what went wrong.
    const std::vector<std::byte> short_nonce(19, std::byte{7});
    const auto native = crpc::db::mysql::scramble_native_password(
        "secret", crpc::span<const std::byte>(short_nonce.data(), short_nonce.size()));
    CRPC_CHECK_FALSE(native.ok());
    if (!native.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(native.error().code));
    }
    const auto sha2 = crpc::db::mysql::scramble_caching_sha2_password(
        "secret", crpc::span<const std::byte>(short_nonce.data(), short_nonce.size()));
    CRPC_CHECK_FALSE(sha2.ok());
}

CRPC_TEST(mysql_scramble_depends_on_the_nonce) {
    // The same password with a different challenge must produce a different token;
    // otherwise the handshake nonce is being ignored and the client is replayable.
    std::vector<std::byte> first(20, std::byte{1});
    std::vector<std::byte> second(20, std::byte{2});
    const auto a = crpc::db::mysql::scramble_native_password(
        "secret", crpc::span<const std::byte>(first.data(), first.size()));
    const auto b = crpc::db::mysql::scramble_native_password(
        "secret", crpc::span<const std::byte>(second.data(), second.size()));
    CRPC_CHECK(a.ok());
    CRPC_CHECK(b.ok());
    if (a.ok() && b.ok()) {
        CRPC_CHECK(a.value() != b.value());
    }
}

CRPC_TEST(mysql_supported_plugins) {
    CRPC_CHECK(crpc::db::mysql::is_supported_plugin("mysql_native_password"));
    CRPC_CHECK(crpc::db::mysql::is_supported_plugin("caching_sha2_password"));
    // An empty name means the server sent no plugin and expects the default.
    CRPC_CHECK(crpc::db::mysql::is_supported_plugin(""));
    CRPC_CHECK_FALSE(crpc::db::mysql::is_supported_plugin("sha256_password"));
    CRPC_CHECK_FALSE(crpc::db::mysql::is_supported_plugin("auth_socket"));
}

CRPC_TEST_MAIN()