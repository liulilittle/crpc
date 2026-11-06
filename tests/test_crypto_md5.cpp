// CRPC -- MD5, HMAC-MD5 vectors.
//
// Vectors are RFC 1321 appendix A.5, i.e. written by the specification, not by
// this implementation. The HMAC vectors are RFC 2202 §1.
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <crpc/crypto/md5.hpp>
#include <crpc/span.hpp>

namespace {

[[nodiscard]] crpc::span<const std::byte> as_bytes(std::string_view text) noexcept {
    return crpc::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                       text.size());
}

} // namespace

CRPC_TEST(md5_rfc1321_vectors) {
    CRPC_CHECK_EQ(std::string("d41d8cd98f00b204e9800998ecf8427e"),
                  crpc::crypto::to_hex(crpc::crypto::md5(as_bytes(""))));
    CRPC_CHECK_EQ(std::string("0cc175b9c0f1b6a831c399e269772661"),
                  crpc::crypto::to_hex(crpc::crypto::md5(as_bytes("a"))));
    CRPC_CHECK_EQ(std::string("900150983cd24fb0d6963f7d28e17f72"),
                  crpc::crypto::to_hex(crpc::crypto::md5(as_bytes("abc"))));
    CRPC_CHECK_EQ(std::string("f96b697d7cb7938d525a2f31aaf161d0"),
                  crpc::crypto::to_hex(crpc::crypto::md5(as_bytes("message digest"))));
    CRPC_CHECK_EQ(std::string("c3fcd3d76192e4007dfb496cca67e13b"),
                  crpc::crypto::to_hex(crpc::crypto::md5(
                      as_bytes("abcdefghijklmnopqrstuvwxyz"))));
    CRPC_CHECK_EQ(std::string("d174ab98d277d9f5a5611c2c9f419d9f"),
                  crpc::crypto::to_hex(crpc::crypto::md5(as_bytes(
                      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"))));
    CRPC_CHECK_EQ(std::string("57edf4a22be3c955ac49da2e2107b67a"),
                  crpc::crypto::to_hex(crpc::crypto::md5(as_bytes(
                      "12345678901234567890123456789012345678901234567890123456789012345678901234567890"))));
}

// 56 bytes is the first length whose padding does not fit in the same block:
// an implementation that forgets the extra block passes every vector above and
// fails here. This is the boundary case, not a redundancy.
// Expected values come from Python's hashlib (an independent implementation),
// not from this code:
//   printf 'a%.0s' {1..56} | md5sum
CRPC_TEST(md5_padding_crosses_the_block_boundary) {
    CRPC_CHECK_EQ(std::string("3b0c8ac703f828b04c6c197006d17218"),
                  crpc::crypto::to_hex(crpc::crypto::md5(as_bytes(std::string(56, 'a')))));
    CRPC_CHECK_EQ(std::string("014842d480b571495a4a0363793f7367"),
                  crpc::crypto::to_hex(crpc::crypto::md5(as_bytes(std::string(64, 'a')))));
    // One more than a block, and one less than the padding boundary.
    CRPC_CHECK_EQ(std::string("ef1772b6dff9a122358552954ad0df65"),
                  crpc::crypto::to_hex(crpc::crypto::md5(as_bytes(std::string(55, 'a')))));
    CRPC_CHECK_EQ(std::string("c743a45e0d2e6a95cb859adae0248435"),
                  crpc::crypto::to_hex(crpc::crypto::md5(as_bytes(std::string(65, 'a')))));
}

// The two-span overload must equal the concatenation: UUID v3 hashes the
// namespace and the name as one message, and copying them into a scratch buffer
// would allocate on every call.
CRPC_TEST(md5_two_spans_equal_concatenation) {
    const auto joined = crpc::crypto::md5(as_bytes("1234567890123456hello"));
    const auto split = crpc::crypto::md5(as_bytes("1234567890123456"), as_bytes("hello"));
    CRPC_CHECK_EQ(crpc::crypto::to_hex(joined), crpc::crypto::to_hex(split));
}

CRPC_TEST_MAIN()
