// CRPC -- MySQL authentication scrambles.
#include <crpc/db/mysql/auth.hpp>

#include <crpc/crypto/hash.hpp>

namespace crpc {
namespace db {
namespace mysql {
namespace {

[[nodiscard]] span<const std::byte> as_bytes(std::string_view text) noexcept {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size());
}

// Both scrambles are "hash(pw) XOR hash( ...  || nonce)" with a different hash and
// a different inner construction, so the nonce is validated once here.
[[nodiscard]] status check_nonce(span<const std::byte> nonce) {
    if (nonce.size() != auth_nonce_size) {
        // A shorter nonce would silently produce a token the server rejects, with
        // no hint as to why. 20 bytes is what both plugins send.
        return status{rpc_code::invalid_argument, "mysql auth nonce must be 20 bytes"};
    }
    return status{};
}

} // namespace

result<std::vector<std::byte>> scramble_native_password(std::string_view password,
                                                        span<const std::byte> nonce) {
    const status nonce_ok = check_nonce(nonce);
    if (!nonce_ok) {
        return nonce_ok.error();
    }
    // An empty password is expressed as an empty auth response, not as a scramble
    // of the empty string.
    if (password.empty()) {
        return std::vector<std::byte>{};
    }

    const crypto::sha1_digest stage1 = crypto::sha1(as_bytes(password));
    const crypto::sha1_digest stage2 =
        crypto::sha1(span<const std::byte>(reinterpret_cast<const std::byte*>(stage1.data()),
                                           stage1.size()));
    const crypto::sha1_digest stage3 = crypto::sha1(
        nonce, span<const std::byte>(reinterpret_cast<const std::byte*>(stage2.data()),
                                     stage2.size()));

    std::vector<std::byte> token(stage1.size());
    for (std::size_t i = 0; i < token.size(); ++i) {
        token[i] = static_cast<std::byte>(stage1[i] ^ stage3[i]);
    }
    return token;
}

result<std::vector<std::byte>> scramble_caching_sha2_password(std::string_view password,
                                                              span<const std::byte> nonce) {
    const status nonce_ok = check_nonce(nonce);
    if (!nonce_ok) {
        return nonce_ok.error();
    }
    if (password.empty()) {
        return std::vector<std::byte>{};
    }

    const crypto::sha256_digest stage1 = crypto::sha256(as_bytes(password));
    const crypto::sha256_digest stage2 =
        crypto::sha256(span<const std::byte>(reinterpret_cast<const std::byte*>(stage1.data()),
                                             stage1.size()));
    // Note the order: the *nonce comes first* here, unlike the SHA-1 plugin. A
    // client that reuses one construction for both authenticates against neither.
    const crypto::sha256_digest stage3 = crypto::sha256(
        span<const std::byte>(reinterpret_cast<const std::byte*>(stage2.data()), stage2.size()),
        nonce);

    std::vector<std::byte> token(stage1.size());
    for (std::size_t i = 0; i < token.size(); ++i) {
        token[i] = static_cast<std::byte>(stage1[i] ^ stage3[i]);
    }
    return token;
}

bool is_supported_plugin(std::string_view plugin) noexcept {
    return plugin == "mysql_native_password" || plugin == "caching_sha2_password" ||
           plugin.empty();
}

} // namespace mysql
} // namespace db
} // namespace crpc