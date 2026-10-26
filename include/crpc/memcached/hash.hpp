// CRPC -- MD5 and a libketama-compatible consistent-hash ring.
//
// Two multi-server memcached clients that route the same key to different
// servers are not *incorrect*, but they destroy the cache: one client's write is
// invisible to the other, so the hit rate collapses and the backend sees the
// full load. memcached itself has no routing, so the ring is the de-facto
// interoperability contract between clients.
//
// The ring implemented here is libketama's:
//
//   * for each server, 40 hashes are produced from `"<name>-<i>"`, i = 0..39;
//   * each MD5 digest yields four little-endian u32 points, so one server owns
//     160 points (the value libketama uses for a server of weight 1);
//   * a key is placed on the first point clockwise from `md5(key)` read as a
//     little-endian u32;
//   * a server with a weight of w contributes w * 160 points.
//
// MD5 is required because that is what libketama hashes with, not because it is
// a good hash. It is used only for placement, never for integrity or secrecy.
#ifndef CRPC_MEMCACHED_HASH_HPP
#define CRPC_MEMCACHED_HASH_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace memcached {

// 16-byte MD5 digest.
using md5_digest = std::array<std::uint8_t, 16>;

// RFC 1321 MD5 of `data`. Correctness is pinned by the RFC's own test vectors in
// the test suite; this is a plain implementation, not a hardened one.
[[nodiscard]] CRPC_API md5_digest md5(span<const std::byte> data) noexcept;

// Convenience overload for the textual inputs the ring uses.
[[nodiscard]] CRPC_API md5_digest md5(std::string_view text) noexcept;

// Reads the first four bytes of a digest as a little-endian u32, which is how
// libketama derives a ring point from a digest.
[[nodiscard]] constexpr std::uint32_t digest_u32(const md5_digest& digest,
                                                 std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(digest[offset]) |
           (static_cast<std::uint32_t>(digest[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(digest[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(digest[offset + 3]) << 24);
}

// One server in the ring. `name` is the `host:port` string libketama hashes, so
// it must not contain the replica suffix.
struct ring_node {
    std::string name;
    // Relative weight; 0 removes the node from the ring. Each unit contributes
    // 160 points.
    std::uint32_t weight = 1;
};

// An immutable, sorted ketama ring. Construction is the only expensive step;
// lookup is one binary search. The ring is immutable after construction, so any
// number of threads may query it concurrently.
class CRPC_API ketama_ring {
public:
    // Builds the ring. Duplicate names are collapsed (the first weight wins) so a
    // configuration mistake cannot silently give one server twice the share.
    // A node with weight 0 contributes nothing; an empty node list produces an
    // empty ring whose `size()` is 0 and whose lookups return -1.
    explicit ketama_ring(std::vector<ring_node> nodes);

    ketama_ring() = default;

    // Index into the node list passed to the constructor, or -1 for an empty
    // ring. `key` is hashed exactly as the wire command will carry it.
    [[nodiscard]] std::ptrdiff_t locate(std::string_view key) const noexcept;

    // The node a key maps to, or nullptr for an empty ring.
    [[nodiscard]] const ring_node* node_for(std::string_view key) const noexcept;

    [[nodiscard]] bool empty() const noexcept { return points_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }
    [[nodiscard]] std::size_t point_count() const noexcept { return points_.size(); }
    [[nodiscard]] const std::vector<ring_node>& nodes() const noexcept { return nodes_; }

    // The raw points, ascending. Exposed for tests and for diagnostics that must
    // prove two builds produce the same ring.
    struct point {
        std::uint32_t hash;
        std::uint32_t node;
    };
    [[nodiscard]] const std::vector<point>& points() const noexcept { return points_; }

private:
    std::vector<ring_node> nodes_;
    std::vector<point> points_;
};

} // namespace memcached
} // namespace crpc

#endif // CRPC_MEMCACHED_HASH_HPP