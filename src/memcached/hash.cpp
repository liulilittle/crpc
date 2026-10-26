// CRPC -- MD5 (RFC 1321) and the libketama ring.
#include <crpc/memcached/hash.hpp>

#include <algorithm>
#include <cstring>

namespace crpc {
namespace memcached {
namespace {

// --- MD5 ---------------------------------------------------------------------

struct md5_context {
    std::uint32_t state[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    std::uint64_t total_bytes = 0;
    std::uint8_t block[64] = {};
    std::size_t used = 0;
};

constexpr std::uint32_t round_constant[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au,
    0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u,
    0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
    0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
    0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u,
    0xffeff47du, 0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u};

constexpr std::uint32_t shift_amount[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

constexpr std::uint32_t rotate_left(std::uint32_t value, std::uint32_t count) noexcept {
    return (value << count) | (value >> (32u - count));
}

void md5_transform(md5_context& context, const std::uint8_t* block) noexcept {
    std::uint32_t words[16];
    for (std::size_t i = 0; i < 16; ++i) {
        words[i] = static_cast<std::uint32_t>(block[i * 4]) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 8) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 16) |
                   (static_cast<std::uint32_t>(block[i * 4 + 3]) << 24);
    }

    std::uint32_t a = context.state[0];
    std::uint32_t b = context.state[1];
    std::uint32_t c = context.state[2];
    std::uint32_t d = context.state[3];

    for (std::uint32_t i = 0; i < 64; ++i) {
        std::uint32_t f = 0;
        std::uint32_t g = 0;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        const std::uint32_t temp = d;
        d = c;
        c = b;
        b = b + rotate_left(a + f + round_constant[i] + words[g], shift_amount[i]);
        a = temp;
    }

    context.state[0] += a;
    context.state[1] += b;
    context.state[2] += c;
    context.state[3] += d;
}

void md5_update(md5_context& context, const std::uint8_t* data, std::size_t size) noexcept {
    context.total_bytes += size;
    while (size != 0) {
        const std::size_t room = 64 - context.used;
        const std::size_t take = size < room ? size : room;
        std::memcpy(context.block + context.used, data, take);
        context.used += take;
        data += take;
        size -= take;
        if (context.used == 64) {
            md5_transform(context, context.block);
            context.used = 0;
        }
    }
}

void md5_finish(md5_context& context, md5_digest& out) noexcept {
    const std::uint64_t bit_length = context.total_bytes * 8u;

    // Padding: 0x80, then zeros until 56 bytes into the block, then the 64-bit
    // length in bits, little-endian. `total_bytes` is the length before padding.
    const std::uint8_t pad = 0x80;
    md5_update(context, &pad, 1);
    const std::uint8_t zero = 0;
    while (context.used != 56) {
        md5_update(context, &zero, 1);
    }
    std::uint8_t length_bytes[8];
    for (std::size_t i = 0; i < 8; ++i) {
        length_bytes[i] = static_cast<std::uint8_t>((bit_length >> (8 * i)) & 0xffu);
    }
    md5_update(context, length_bytes, 8);

    for (std::size_t i = 0; i < 4; ++i) {
        out[i * 4] = static_cast<std::uint8_t>(context.state[i] & 0xffu);
        out[i * 4 + 1] = static_cast<std::uint8_t>((context.state[i] >> 8) & 0xffu);
        out[i * 4 + 2] = static_cast<std::uint8_t>((context.state[i] >> 16) & 0xffu);
        out[i * 4 + 3] = static_cast<std::uint8_t>((context.state[i] >> 24) & 0xffu);
    }
}

// libketama places 40 hashes per unit of weight; each digest yields four points.
constexpr std::uint32_t replicas_per_weight = 40;
constexpr std::uint32_t points_per_replica = 4;
constexpr std::uint32_t points_per_weight = replicas_per_weight * points_per_replica;

} // namespace

md5_digest md5(span<const std::byte> data) noexcept {
    md5_context context;
    md5_update(context, reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
    md5_digest digest{};
    md5_finish(context, digest);
    return digest;
}

md5_digest md5(std::string_view text) noexcept {
    return md5(span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                     text.size()));
}

// --- Ketama ring -------------------------------------------------------------

ketama_ring::ketama_ring(std::vector<ring_node> nodes) {
    nodes_.reserve(nodes.size());
    for (ring_node& node : nodes) {
        if (node.weight == 0 || node.name.empty()) {
            continue;
        }
        bool duplicate = false;
        for (const ring_node& kept : nodes_) {
            if (kept.name == node.name) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            nodes_.push_back(std::move(node));
        }
    }

    points_.reserve(nodes_.size() * points_per_weight);
    for (std::uint32_t index = 0; index < nodes_.size(); ++index) {
        const std::string& name = nodes_[index].name;
        const std::uint32_t weight = nodes_[index].weight;
        const std::uint32_t replicas = replicas_per_weight * weight;
        for (std::uint32_t replica = 0; replica < replicas; ++replica) {
            const std::string seed = name + "-" + std::to_string(replica);
            const md5_digest digest = md5(seed);
            for (std::uint32_t slot = 0; slot < points_per_replica; ++slot) {
                points_.push_back(point{digest_u32(digest, slot * 4), index});
            }
        }
    }

    // Ascending by hash; the node index breaks ties so the ring is identical on
    // every platform and in every build (std::sort is not stable, so the
    // comparator must be a total order).
    std::sort(points_.begin(), points_.end(), [](const point& lhs, const point& rhs) {
        if (lhs.hash != rhs.hash) {
            return lhs.hash < rhs.hash;
        }
        return lhs.node < rhs.node;
    });
}

std::ptrdiff_t ketama_ring::locate(std::string_view key) const noexcept {
    if (points_.empty()) {
        return -1;
    }
    const std::uint32_t hash = digest_u32(md5(key), 0);
    const auto found = std::lower_bound(
        points_.begin(), points_.end(), hash,
        [](const point& candidate, std::uint32_t value) { return candidate.hash < value; });
    if (found == points_.end()) {
        return static_cast<std::ptrdiff_t>(points_.front().node);
    }
    return static_cast<std::ptrdiff_t>(found->node);
}

const ring_node* ketama_ring::node_for(std::string_view key) const noexcept {
    const std::ptrdiff_t index = locate(key);
    if (index < 0) {
        return nullptr;
    }
    return &nodes_[static_cast<std::size_t>(index)];
}

} // namespace memcached
} // namespace crpc