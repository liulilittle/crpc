// CRPC -- memcached routing hash tests.
//
// MD5 is pinned by the RFC's own vectors: these are not our expectations, they
// are the published answers, so a wrong implementation cannot pass by agreeing
// with itself. The ring is pinned by the properties libketama guarantees, not by
// a recorded table: adding a server may only move keys to the new server, a
// stable ring reproduces exactly, and a key's placement depends on the key and
// the server names and on nothing else.
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/memcached/hash.hpp>

namespace {

using crpc::memcached::digest_u32;
using crpc::memcached::ketama_ring;
using crpc::memcached::md5;
using crpc::memcached::md5_digest;
using crpc::memcached::ring_node;

[[nodiscard]] std::string to_hex(const md5_digest& digest) {
    static const char* digits = "0123456789abcdef";
    std::string text;
    text.reserve(32);
    for (const std::uint8_t byte : digest) {
        text.push_back(digits[byte >> 4]);
        text.push_back(digits[byte & 0x0f]);
    }
    return text;
}

} // namespace

CRPC_TEST(md5_rfc1321_vectors) {
    // RFC 1321, appendix A.5.
    CRPC_CHECK_EQ(std::string("d41d8cd98f00b204e9800998ecf8427e"), to_hex(md5("")));
    CRPC_CHECK_EQ(std::string("900150983cd24fb0d6963f7d28e17f72"), to_hex(md5("abc")));
    CRPC_CHECK_EQ(std::string("f96b697d7cb7938d525a2f31aaf161d0"),
                  to_hex(md5("message digest")));
    CRPC_CHECK_EQ(std::string("c3fcd3d76192e4007dfb496cca67e13b"),
                  to_hex(md5("abcdefghijklmnopqrstuvwxyz")));
    CRPC_CHECK_EQ(std::string("d174ab98d277d9f5a5611c2c9f419d9f"),
                  to_hex(md5("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789")));
    CRPC_CHECK_EQ(std::string("57edf4a22be3c955ac49da2e2107b67a"),
                  to_hex(md5("1234567890123456789012345678901234567890"
                             "1234567890123456789012345678901234567890")));
}

CRPC_TEST(md5_block_boundaries) {
    // The padding rules differ at 55/56/57 bytes and at 63/64/65 bytes, which is
    // exactly where a hand-written MD5 breaks. Values computed with the
    // reference implementation in Python's hashlib (OpenSSL-backed).
    CRPC_CHECK_EQ(std::string("ef1772b6dff9a122358552954ad0df65"),
                  to_hex(md5(std::string(55, 'a'))));
    CRPC_CHECK_EQ(std::string("3b0c8ac703f828b04c6c197006d17218"),
                  to_hex(md5(std::string(56, 'a'))));
    CRPC_CHECK_EQ(std::string("014842d480b571495a4a0363793f7367"),
                  to_hex(md5(std::string(64, 'a'))));
    // 1000 bytes crosses sixteen block boundaries.
    CRPC_CHECK_EQ(std::string("cabe45dcc9ae5b66ba86600cca6b8ba8"),
                  to_hex(md5(std::string(1000, 'a'))));
    // 80 bytes is one full block plus 16: the padding path that a naive
    // "pad to 56 in one block" implementation gets wrong.
    CRPC_CHECK_EQ(std::string("b15af9cdabbaea0516866a33d8fd0f98"),
                  to_hex(md5(std::string(80, 'a'))));
}

CRPC_TEST(md5_span_overload_matches_text) {
    const std::string text = "message digest";
    const md5_digest from_text = md5(text);
    const md5_digest from_bytes = md5(crpc::span<const std::byte>(
        reinterpret_cast<const std::byte*>(text.data()), text.size()));
    CRPC_CHECK(from_text == from_bytes);
}

CRPC_TEST(digest_u32_is_little_endian) {
    md5_digest digest{};
    digest[0] = 0x01;
    digest[1] = 0x02;
    digest[2] = 0x03;
    digest[3] = 0x04;
    CRPC_CHECK_EQ(std::uint32_t{0x04030201}, digest_u32(digest, 0));

    digest[4] = 0xff;
    CRPC_CHECK_EQ(std::uint32_t{0x000000ff}, digest_u32(digest, 4));
}

CRPC_TEST(ketama_point_count_matches_weight) {
    // libketama: 40 md5 seeds * 4 points = 160 points per unit of weight.
    const ketama_ring single({ring_node{"10.0.0.1:11211", 1}});
    CRPC_CHECK_EQ(std::size_t{1}, single.size());
    CRPC_CHECK_EQ(std::size_t{160}, single.point_count());

    const ketama_ring weighted({ring_node{"a:11211", 2}});
    CRPC_CHECK_EQ(std::size_t{320}, weighted.point_count());

    const ketama_ring zero({ring_node{"a:11211", 0}});
    CRPC_CHECK(zero.empty());
    CRPC_CHECK_EQ(std::size_t{0}, zero.size());

    const ketama_ring none(std::vector<ring_node>{});
    CRPC_CHECK(none.empty());
    CRPC_CHECK_EQ(std::ptrdiff_t{-1}, none.locate("key"));
    CRPC_CHECK(none.node_for("key") == nullptr);
}

CRPC_TEST(ketama_ring_is_sorted_and_deterministic) {
    const std::vector<ring_node> nodes{{"10.0.0.1:11211", 1},
                                       {"10.0.0.2:11211", 1},
                                       {"10.0.0.3:11211", 1}};
    const ketama_ring first(nodes);
    const ketama_ring second(nodes);
    CRPC_CHECK_EQ(first.point_count(), second.point_count());
    CRPC_CHECK_EQ(first.point_count(), std::size_t{480});

    for (std::size_t i = 0; i < first.points().size(); ++i) {
        CRPC_CHECK_EQ(first.points()[i].hash, second.points()[i].hash);
        CRPC_CHECK_EQ(first.points()[i].node, second.points()[i].node);
    }
    for (std::size_t i = 1; i < first.points().size(); ++i) {
        CRPC_CHECK(first.points()[i - 1].hash <= first.points()[i].hash);
    }
}

CRPC_TEST(ketama_duplicate_names_collapse) {
    const ketama_ring ring(
        {ring_node{"a:11211", 1}, ring_node{"a:11211", 3}, ring_node{"b:11211", 1}});
    CRPC_CHECK_EQ(std::size_t{2}, ring.size());
    // The first weight wins, so the duplicate cannot silently buy more points.
    CRPC_CHECK_EQ(std::size_t{320}, ring.point_count());
}

CRPC_TEST(ketama_locate_is_stable_and_wraps) {
    const ketama_ring ring({ring_node{"a:11211", 1}, ring_node{"b:11211", 1}});
    // Same key, same answer, every time.
    for (int i = 0; i < 32; ++i) {
        const std::ptrdiff_t index = ring.locate("user:42");
        CRPC_CHECK(index == 0 || index == 1);
        CRPC_CHECK_EQ(ring.locate("user:42"), index);
    }
    // Every key that hashes past the last point wraps to the first node.
    for (int i = 0; i < 2000; ++i) {
        const std::string key = "k" + std::to_string(i);
        const std::ptrdiff_t index = ring.locate(key);
        CRPC_CHECK(index == 0 || index == 1);
        const ring_node* node = ring.node_for(key);
        CRPC_CHECK(node != nullptr);
        if (node != nullptr) {
            CRPC_CHECK(node->name == "a:11211" || node->name == "b:11211");
        }
    }
}

CRPC_TEST(ketama_distribution_is_roughly_even) {
    // A ring that sends everything to one node would pass the stability checks
    // above, so this pins the actual balance within a generous band.
    const ketama_ring ring({ring_node{"a:11211", 1}, ring_node{"b:11211", 1},
                            ring_node{"c:11211", 1}});
    std::map<std::ptrdiff_t, int> counts;
    constexpr int keys = 6000;
    for (int i = 0; i < keys; ++i) {
        ++counts[ring.locate("session:" + std::to_string(i))];
    }
    CRPC_CHECK_EQ(std::size_t{3}, counts.size());
    for (const auto& entry : counts) {
        const double share = static_cast<double>(entry.second) / keys;
        CRPC_CHECK(share > 0.20);
        CRPC_CHECK(share < 0.47);
    }
}

CRPC_TEST(ketama_remap_is_minimal) {
    // The defining property of consistent hashing: adding a server may only move
    // keys onto the new server, and only about 1/N of them.
    const std::vector<ring_node> three{{"a:11211", 1}, {"b:11211", 1}, {"c:11211", 1}};
    const std::vector<ring_node> four{{"a:11211", 1},
                                      {"b:11211", 1},
                                      {"c:11211", 1},
                                      {"d:11211", 1}};
    const ketama_ring before(three);
    const ketama_ring after(four);

    constexpr int keys = 5000;
    int moved = 0;
    for (int i = 0; i < keys; ++i) {
        const std::string key = "obj:" + std::to_string(i);
        const std::ptrdiff_t old_node = before.locate(key);
        const std::ptrdiff_t new_node = after.locate(key);
        if (old_node != new_node) {
            // A move is legal only onto the newly added node (index 3).
            CRPC_CHECK_EQ(std::ptrdiff_t{3}, new_node);
            ++moved;
        }
    }
    // Ideal is 25%; the band is wide enough for hash variance but tight enough
    // to fail a modulo-style ring, which would move ~75%.
    CRPC_CHECK(moved > keys / 8);
    CRPC_CHECK(moved < keys / 2);
}

CRPC_TEST(ketama_removal_is_minimal) {
    const std::vector<ring_node> four{{"a:11211", 1},
                                      {"b:11211", 1},
                                      {"c:11211", 1},
                                      {"d:11211", 1}};
    const std::vector<ring_node> three{{"a:11211", 1}, {"b:11211", 1}, {"c:11211", 1}};
    const ketama_ring before(four);
    const ketama_ring after(three);

    constexpr int keys = 5000;
    int moved = 0;
    for (int i = 0; i < keys; ++i) {
        const std::string key = "obj:" + std::to_string(i);
        const std::ptrdiff_t old_node = before.locate(key);
        const std::ptrdiff_t new_node = after.locate(key);
        if (old_node != new_node) {
            // Only keys that lived on the removed node may move.
            CRPC_CHECK_EQ(std::ptrdiff_t{3}, old_node);
            ++moved;
        }
    }
    CRPC_CHECK(moved > keys / 8);
    CRPC_CHECK(moved < keys / 2);
}

CRPC_TEST_MAIN()