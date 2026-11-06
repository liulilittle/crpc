// CRPC -- UUIDs (RFC 4122, obsoleted by RFC 9562): versions 1, 3, 4, 5 and 7.
//
// The value is the 16-octet big-endian layout of RFC 4122 section 4.1.2, which
// is also the field order of the textual form. Every generator writes the
// version and variant bits itself: a v4 value carrying a v1 nibble is not a v4,
// and a consumer that dispatches on the version would silently misread it.
//
// Scope: this is an identifier generator, not a security primitive. RFC 9562
// section 8 repeats RFC 4122's warning that a UUID must not be used as a
// capability -- the value is often guessable, and v1 exposes its timestamp.
// Version 3 is here only because the version is defined in terms of MD5 (see
// md5.hpp); it is not a general-purpose hash.
//
// Randomness: v4 and the random fields of v1/v7 are filled from the operating
// system's cryptographic generator (BCryptGenRandom, getrandom(2) or
// /dev/urandom), the same source rsa.cpp uses. The degraded path taken only if
// that generator fails is documented at its definition in uuid.cpp.
#ifndef CRPC_CRYPTO_UUID_HPP
#define CRPC_CRYPTO_UUID_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace crypto {

using uuid_bytes = std::array<std::uint8_t, 16>;

// RFC 4122 section 4.1.1: the two most significant bits of octet 8 select how
// the rest of the value is interpreted. Only rfc_4122 is produced here.
enum class uuid_variant : std::uint8_t {
    ncs = 0x00,       // 0xx -- reserved for NCS backward compatibility
    rfc_4122 = 0x80,  // 10x -- the variant specified by RFC 4122 and RFC 9562
    microsoft = 0xc0, // 110 -- reserved for Microsoft backward compatibility
    future = 0xe0     // 111 -- reserved for future definition
};

struct uuid {
    uuid_bytes bytes{};

    // The 4-bit version: the most significant nibble of octet 6. Meaningless
    // when variant() is not rfc_4122.
    [[nodiscard]] constexpr std::uint8_t version() const noexcept {
        return static_cast<std::uint8_t>((bytes[6] >> 4) & 0x0f);
    }

    [[nodiscard]] constexpr uuid_variant variant() const noexcept {
        switch (static_cast<std::uint8_t>(bytes[8] & 0xc0u)) {
        case 0x00:
            return uuid_variant::ncs;
        case 0x80:
            return uuid_variant::rfc_4122;
        case 0xc0:
            return uuid_variant::microsoft;
        default:
            return uuid_variant::future;
        }
    }

    // The 60-bit timestamp field of RFC 4122 section 4.1.4, as a count of
    // 100-nanosecond intervals since 00:00:00.00 UTC on 15 October 1582. This
    // is a time only for version 1: for v3/v5 the same octets are hash
    // material, and v7 uses a different layout (Unix milliseconds in octets
    // 0-5), so the bits are returned reinterpreted rather than converted.
    [[nodiscard]] constexpr std::uint64_t timestamp_100ns() const noexcept {
        return (static_cast<std::uint64_t>(bytes[6] & 0x0fu) << 56) |
               (static_cast<std::uint64_t>(bytes[7]) << 48) |
               (static_cast<std::uint64_t>(bytes[4]) << 40) |
               (static_cast<std::uint64_t>(bytes[5]) << 32) |
               (static_cast<std::uint64_t>(bytes[0]) << 24) |
               (static_cast<std::uint64_t>(bytes[1]) << 16) |
               (static_cast<std::uint64_t>(bytes[2]) << 8) |
               static_cast<std::uint64_t>(bytes[3]);
    }
};

// Byte-wise lexicographic order. For v7 values this is time order; for v1 it is
// time order within one clock sequence. Comparing the fields as integers, which
// RFC 4122 section 3 defines as lexical equivalence, yields the same result
// because every field is stored big-endian.
[[nodiscard]] constexpr bool operator==(const uuid& left, const uuid& right) noexcept {
    for (std::size_t i = 0; i < left.bytes.size(); ++i) {
        if (left.bytes[i] != right.bytes[i]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool operator!=(const uuid& left, const uuid& right) noexcept {
    return !(left == right);
}

[[nodiscard]] constexpr bool operator<(const uuid& left, const uuid& right) noexcept {
    for (std::size_t i = 0; i < left.bytes.size(); ++i) {
        if (left.bytes[i] != right.bytes[i]) {
            return left.bytes[i] < right.bytes[i];
        }
    }
    return false;
}

// Hash for unordered containers. FNV-1a over the 16 octets: a bucket index is
// all the container needs, and a cryptographic hash would cost more than the
// container saves. Not stable across processes and not a security primitive.
struct uuid_hash {
    [[nodiscard]] std::size_t operator()(const uuid& value) const noexcept {
        std::uint64_t hash = 14695981039346656037ULL;
        for (const std::uint8_t byte : value.bytes) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
        return static_cast<std::size_t>(hash);
    }
};

// RFC 4122 Appendix C name spaces, in network byte order.
inline constexpr uuid namespace_dns = {
    {0x6b, 0xa7, 0xb8, 0x10, 0x9d, 0xad, 0x11, 0xd1, 0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8}};
inline constexpr uuid namespace_url = {
    {0x6b, 0xa7, 0xb8, 0x11, 0x9d, 0xad, 0x11, 0xd1, 0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8}};
inline constexpr uuid namespace_oid = {
    {0x6b, 0xa7, 0xb8, 0x12, 0x9d, 0xad, 0x11, 0xd1, 0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8}};
inline constexpr uuid namespace_x500 = {
    {0x6b, 0xa7, 0xb8, 0x14, 0x9d, 0xad, 0x11, 0xd1, 0x80, 0xb4, 0x00, 0xc0, 0x4f, 0xd4, 0x30, 0xc8}};

// Version 1 (time-based, RFC 4122 section 4.2). The context carries the 48-bit
// node and the clock sequence; only the low 14 bits of clock_sequence are
// written, and the node is copied verbatim, so a caller that supplies a random
// node must set its multicast bit (uuid.cpp does for the generated one).
struct uuid_v1_context {
    std::array<std::uint8_t, 6> node{};
    std::uint16_t clock_sequence = 0;
};

// Version 1 with an explicit 60-bit Gregorian timestamp. Higher bits of
// `timestamp_100ns` are ignored. The clock sequence and node are written
// exactly as given, which is what makes the RFC 9562 Appendix A.1 vector
// reproducible.
[[nodiscard]] CRPC_API uuid uuid_v1(std::uint64_t timestamp_100ns,
                                   const uuid_v1_context& context) noexcept;

// Version 1 from the system clock. The node and clock sequence are generated
// once per process from the OS random source (node: RFC 4122 section 4.5,
// random with the multicast bit set) and the timestamp is forced strictly
// upwards, because the system clock's resolution is coarser than the UUID
// tick and two calls in a row must not collide.
[[nodiscard]] CRPC_API uuid uuid_v1();

// Version 3/5 (name-based, RFC 4122 section 4.3): MD5/SHA-1 over the name
// space in network byte order followed by the name octets. The name is opaque
// octets, not text, and the same pair always yields the same value.
[[nodiscard]] CRPC_API uuid uuid_v3(const uuid& name_space, std::string_view name);
[[nodiscard]] CRPC_API uuid uuid_v5(const uuid& name_space, std::string_view name);

// Version 4 (random, RFC 4122 section 4.4): 16 octets from the OS generator
// with the version and variant bits pinned.
[[nodiscard]] CRPC_API uuid uuid_v4();

// Version 7 (Unix-epoch time-ordered, RFC 9562 section 5.7). Octets 0-5 are
// `unix_ms`; the 12 rand_a bits are a per-millisecond counter so values created
// within one millisecond are strictly increasing in byte order, and rand_b is
// fresh random data. A clock that steps backwards is clamped to the last
// millisecond, which keeps `operator<` a time order. The no-argument form reads
// the system clock.
[[nodiscard]] CRPC_API uuid uuid_v7(std::uint64_t unix_ms);
[[nodiscard]] CRPC_API uuid uuid_v7();

// Parses the canonical 8-4-4-4-12 form or 32 bare hex digits, in either case.
// Braces and a urn:uuid: prefix are refused: a peer using them should be
// normalised explicitly, because guessing which notation an identifier came in
// is how two systems end up disagreeing about the same value. A wrong length,
// a hyphen anywhere but the four canonical positions, or a non-hex character
// is decode_error.
[[nodiscard]] CRPC_API result<uuid> parse_uuid(std::string_view text);

// Lowercase 8-4-4-4-12, the form RFC 9562 section 4 recommends for output.
[[nodiscard]] CRPC_API std::string to_string(const uuid& value);

// Exactly 16 octets are required; any other length is decode_error, because a
// short slice is a truncated identifier and zero-filling it would silently
// invent a different one.
[[nodiscard]] CRPC_API result<uuid> uuid_from_bytes(span<const std::uint8_t> bytes);

[[nodiscard]] CRPC_API uuid_bytes to_bytes(const uuid& value) noexcept;

} // namespace crypto
} // namespace crpc

#endif // CRPC_CRYPTO_UUID_HPP
