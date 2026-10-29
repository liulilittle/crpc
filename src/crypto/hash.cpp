// CRPC -- SHA-1 (RFC 3174) and SHA-256 (FIPS 180-4).
#include <crpc/crypto/hash.hpp>

#include <cstring>
#include <vector>

namespace crpc {
namespace crypto {
namespace {

// Incremental state for both functions: a message is hashed by pushing bytes into
// `pending`, flushing 64-byte blocks, then finishing with the length padding.
struct sha_state {
    std::uint32_t h[8] = {};
    std::uint8_t pending[64] = {};
    std::size_t used = 0;
    std::uint64_t total_bytes = 0;
};

constexpr std::uint32_t rotl32(std::uint32_t value, std::uint32_t count) noexcept {
    return (value << count) | (value >> (32u - count));
}

constexpr std::uint32_t rotr32(std::uint32_t value, std::uint32_t count) noexcept {
    return (value >> count) | (value << (32u - count));
}

// --- SHA-1 -------------------------------------------------------------------

constexpr std::uint32_t sha1_initial[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu,
                                           0x10325476u, 0xC3D2E1F0u};

void sha1_block(sha_state& state, const std::uint8_t* block) noexcept {
    std::uint32_t w[80];
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (std::size_t i = 16; i < 80; ++i) {
        w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    std::uint32_t a = state.h[0];
    std::uint32_t b = state.h[1];
    std::uint32_t c = state.h[2];
    std::uint32_t d = state.h[3];
    std::uint32_t e = state.h[4];

    for (std::size_t i = 0; i < 80; ++i) {
        std::uint32_t f = 0;
        std::uint32_t k = 0;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        const std::uint32_t temp = rotl32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl32(b, 30);
        b = a;
        a = temp;
    }

    state.h[0] += a;
    state.h[1] += b;
    state.h[2] += c;
    state.h[3] += d;
    state.h[4] += e;
}

void sha1_init(sha_state& state) noexcept {
    for (std::size_t i = 0; i < 5; ++i) {
        state.h[i] = sha1_initial[i];
    }
    state.used = 0;
    state.total_bytes = 0;
}

// --- SHA-256 -----------------------------------------------------------------

constexpr std::uint32_t sha256_initial[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
                                             0xa54ff53au, 0x510e527fu, 0x9b05688cu,
                                             0x1f83d9abu, 0x5be0cd19u};

constexpr std::uint32_t sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

void sha256_block(sha_state& state, const std::uint8_t* block) noexcept {
    std::uint32_t w[64];
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
               static_cast<std::uint32_t>(block[i * 4 + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    std::uint32_t a = state.h[0];
    std::uint32_t b = state.h[1];
    std::uint32_t c = state.h[2];
    std::uint32_t d = state.h[3];
    std::uint32_t e = state.h[4];
    std::uint32_t f = state.h[5];
    std::uint32_t g = state.h[6];
    std::uint32_t h = state.h[7];

    for (std::size_t i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t temp1 = h + s1 + ch + sha256_k[i] + w[i];
        const std::uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t temp2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state.h[0] += a;
    state.h[1] += b;
    state.h[2] += c;
    state.h[3] += d;
    state.h[4] += e;
    state.h[5] += f;
    state.h[6] += g;
    state.h[7] += h;
}

void sha256_init(sha_state& state) noexcept {
    for (std::size_t i = 0; i < 8; ++i) {
        state.h[i] = sha256_initial[i];
    }
    state.used = 0;
    state.total_bytes = 0;
}

// --- Shared plumbing ---------------------------------------------------------

template <typename BlockFn>
void absorb(sha_state& state, BlockFn block, const std::uint8_t* data, std::size_t size) noexcept {
    state.total_bytes += size;
    while (size != 0) {
        const std::size_t room = 64 - state.used;
        const std::size_t take = size < room ? size : room;
        std::memcpy(state.pending + state.used, data, take);
        state.used += take;
        data += take;
        size -= take;
        if (state.used == 64) {
            block(state, state.pending);
            state.used = 0;
        }
    }
}

// Appends the padding: 0x80, zeros, then the length in bits as a 64-bit big-endian
// value. `total_bytes` must be the length before padding.
template <typename BlockFn>
void finish(sha_state& state, BlockFn block) noexcept {
    const std::uint64_t bit_length = state.total_bytes * 8u;
    const std::uint8_t marker = 0x80;
    absorb(state, block, &marker, 1);
    const std::uint8_t zero = 0;
    while (state.used != 56) {
        absorb(state, block, &zero, 1);
    }
    std::uint8_t length_bytes[8];
    for (std::size_t i = 0; i < 8; ++i) {
        length_bytes[i] = static_cast<std::uint8_t>((bit_length >> (56 - 8 * i)) & 0xffu);
    }
    absorb(state, block, length_bytes, 8);
}

[[nodiscard]] std::string hex_of(const std::uint8_t* bytes, std::size_t size) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(digits[bytes[i] >> 4]);
        out.push_back(digits[bytes[i] & 0x0f]);
    }
    return out;
}

const auto sha1_block_fn = [](sha_state& state, const std::uint8_t* block) noexcept {
    sha1_block(state, block);
};
const auto sha256_block_fn = [](sha_state& state, const std::uint8_t* block) noexcept {
    sha256_block(state, block);
};

} // namespace

sha1_digest sha1(span<const std::byte> data) noexcept {
    sha_state state;
    sha1_init(state);
    absorb(state, sha1_block_fn, reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
    finish(state, sha1_block_fn);
    sha1_digest digest{};
    for (std::size_t i = 0; i < 5; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>((state.h[i] >> 24) & 0xffu);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((state.h[i] >> 16) & 0xffu);
        digest[i * 4 + 2] = static_cast<std::uint8_t>((state.h[i] >> 8) & 0xffu);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(state.h[i] & 0xffu);
    }
    return digest;
}

sha1_digest sha1(span<const std::byte> first, span<const std::byte> second) noexcept {
    sha_state state;
    sha1_init(state);
    absorb(state, sha1_block_fn, reinterpret_cast<const std::uint8_t*>(first.data()), first.size());
    absorb(state, sha1_block_fn, reinterpret_cast<const std::uint8_t*>(second.data()),
           second.size());
    finish(state, sha1_block_fn);
    sha1_digest digest{};
    for (std::size_t i = 0; i < 5; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>((state.h[i] >> 24) & 0xffu);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((state.h[i] >> 16) & 0xffu);
        digest[i * 4 + 2] = static_cast<std::uint8_t>((state.h[i] >> 8) & 0xffu);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(state.h[i] & 0xffu);
    }
    return digest;
}

sha256_digest sha256(span<const std::byte> data) noexcept {
    sha_state state;
    sha256_init(state);
    absorb(state, sha256_block_fn, reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
    finish(state, sha256_block_fn);
    sha256_digest digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>((state.h[i] >> 24) & 0xffu);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((state.h[i] >> 16) & 0xffu);
        digest[i * 4 + 2] = static_cast<std::uint8_t>((state.h[i] >> 8) & 0xffu);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(state.h[i] & 0xffu);
    }
    return digest;
}

namespace {

// HMAC needs the key XORed with a repeated byte, padded to the 64-byte block. Built as a
// small fixed buffer rather than a vector: every call would otherwise allocate twice.
using sha256_pad = std::array<std::uint8_t, 64>;

// Hashes two pieces without concatenating them into a buffer. HMAC always needs
// ipad||message and opad||inner, and copying the message for the sake of a single call is
// what makes an HMAC in a hot path slow.
[[nodiscard]] sha256_digest sha256_two(const std::uint8_t* first, std::size_t first_size,
                                       const std::uint8_t* second,
                                       std::size_t second_size) noexcept {
    sha_state state;
    sha256_init(state);
    absorb(state, sha256_block_fn, first, first_size);
    absorb(state, sha256_block_fn, second, second_size);
    finish(state, sha256_block_fn);
    sha256_digest digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>((state.h[i] >> 24) & 0xffu);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((state.h[i] >> 16) & 0xffu);
        digest[i * 4 + 2] = static_cast<std::uint8_t>((state.h[i] >> 8) & 0xffu);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(state.h[i] & 0xffu);
    }
    return digest;
}

} // namespace

sha256_digest hmac_sha256(span<const std::byte> key, span<const std::byte> message) {
    sha256_pad ipad{};
    sha256_pad opad{};
    if (key.size() > ipad.size()) {
        const sha256_digest hashed = sha256(key);
        for (std::size_t i = 0; i < hashed.size(); ++i) {
            ipad[i] = hashed[i];
            opad[i] = hashed[i];
        }
    } else {
        for (std::size_t i = 0; i < key.size(); ++i) {
            ipad[i] = static_cast<std::uint8_t>(key[i]);
            opad[i] = static_cast<std::uint8_t>(key[i]);
        }
    }
    for (std::size_t i = 0; i < ipad.size(); ++i) {
        ipad[i] = static_cast<std::uint8_t>(ipad[i] ^ 0x36u);
        opad[i] = static_cast<std::uint8_t>(opad[i] ^ 0x5cu);
    }

    const sha256_digest inner = sha256_two(ipad.data(), ipad.size(),
                                           reinterpret_cast<const std::uint8_t*>(message.data()),
                                           message.size());
    return sha256_two(opad.data(), opad.size(), inner.data(), inner.size());
}

sha256_digest pbkdf2_sha256(span<const std::byte> password, span<const std::byte> salt,
                            std::uint32_t iterations) {
    if (iterations == 0) {
        return sha256_digest{};
    }
    // U1 = HMAC(password, salt || INT(1)), then Ui = HMAC(password, U(i-1)) and
    // T = U1 xor U2 xor ... One block is enough because SHA-256's output is the whole
    // value SCRAM asks for.
    // U1 = HMAC(password, salt || INT(1)). The block index is appended to a copy of the
    // salt because HMAC takes one contiguous message; the salt is a few bytes, so this is
    // one small allocation per authentication, not per iteration.
    static const std::uint8_t block_index[4] = {0, 0, 0, 1};
    std::vector<std::uint8_t> joined;
    joined.reserve(salt.size() + 4);
    for (std::size_t i = 0; i < salt.size(); ++i) {
        joined.push_back(static_cast<std::uint8_t>(salt[i]));
    }
    for (const std::uint8_t byte : block_index) {
        joined.push_back(byte);
    }
    const sha256_digest first =
        hmac_sha256(password,
                    span<const std::byte>(reinterpret_cast<const std::byte*>(joined.data()),
                                          joined.size()));

    sha256_digest accumulated = first;
    sha256_digest previous = first;
    for (std::uint32_t round = 1; round < iterations; ++round) {
        previous = hmac_sha256(password,
                               span<const std::byte>(
                                   reinterpret_cast<const std::byte*>(previous.data()),
                                   previous.size()));
        for (std::size_t i = 0; i < accumulated.size(); ++i) {
            accumulated[i] = static_cast<std::uint8_t>(accumulated[i] ^ previous[i]);
        }
    }
    return accumulated;
}

sha256_digest sha256(span<const std::byte> first, span<const std::byte> second) noexcept {
    sha_state state;
    sha256_init(state);
    absorb(state, sha256_block_fn, reinterpret_cast<const std::uint8_t*>(first.data()),
           first.size());
    absorb(state, sha256_block_fn, reinterpret_cast<const std::uint8_t*>(second.data()),
           second.size());
    finish(state, sha256_block_fn);
    sha256_digest digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<std::uint8_t>((state.h[i] >> 24) & 0xffu);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((state.h[i] >> 16) & 0xffu);
        digest[i * 4 + 2] = static_cast<std::uint8_t>((state.h[i] >> 8) & 0xffu);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(state.h[i] & 0xffu);
    }
    return digest;
}

std::string to_hex(sha1_digest digest) { return hex_of(digest.data(), digest.size()); }

std::string to_hex(sha256_digest digest) { return hex_of(digest.data(), digest.size()); }

} // namespace crypto
} // namespace crpc