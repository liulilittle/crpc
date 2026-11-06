// CRPC -- UUID tests (RFC 4122 / RFC 9562).
//
// The vectors come from the specifications, not from this implementation:
//   * RFC 9562 Appendix A.1 pins the version 1 field layout (timestamp,
//     clock sequence and node), Appendix A.2/A.4 pin version 3/5 and
//     Appendix A.6 pins the version 7 timestamp.
//   * The version 3 "www.widgets.com" value printed by RFC 4122 Appendix B,
//     e902893a-9d22-3c7e-a7b8-d6e313b71d9f, is a verified mistake (RFC 4122
//     errata 1352, https://www.rfc-editor.org/errata/eid1352): the namespace
//     and the digest were byte-swapped twice. The corrected value,
//     3d813cbb-47fb-32ba-91df-831e1593ac29, is what Python's uuid.uuid3()
//     computes and what this test pins.
//   * The version 5 "www.widgets.com" value was computed with Python's
//     uuid.uuid5(); the matching RFC 9562 vector ("www.example.com") is
//     also checked, so a wrong namespace byte order or a wrong hash cannot
//     pass by agreeing with itself.
#include "test_framework.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <crpc/crypto/uuid.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace {

using crpc::crypto::uuid;
using crpc::crypto::uuid_hash;
using crpc::crypto::uuid_variant;

[[nodiscard]] crpc::result<uuid> parse(std::string_view text) {
    return crpc::crypto::parse_uuid(text);
}

// The four fields the version 1 layout multiplexes into octets 8-15.
[[nodiscard]] std::uint16_t clock_sequence(const uuid& value) noexcept {
    return static_cast<std::uint16_t>(((value.bytes[8] & 0x3fu) << 8) | value.bytes[9]);
}

[[nodiscard]] std::uint64_t node(const uuid& value) noexcept {
    std::uint64_t out = 0;
    for (std::size_t i = 10; i < 16; ++i) {
        out = (out << 8) | value.bytes[i];
    }
    return out;
}

// The 48-bit big-endian millisecond timestamp of a version 7 value.
[[nodiscard]] std::uint64_t unix_ms(const uuid& value) noexcept {
    std::uint64_t out = 0;
    for (std::size_t i = 0; i < 6; ++i) {
        out = (out << 8) | value.bytes[i];
    }
    return out;
}

} // namespace

CRPC_TEST(uuid_v3_name_based_vectors) {
    // RFC 9562 Appendix A.2.
    const uuid example = crpc::crypto::uuid_v3(crpc::crypto::namespace_dns, "www.example.com");
    CRPC_CHECK_EQ(std::string("5df41881-3aed-3515-88a7-2f4a814cf09e"),
                  crpc::crypto::to_string(example));
    CRPC_CHECK_EQ(3, example.version());
    CRPC_CHECK_EQ(uuid_variant::rfc_4122, example.variant());

    // RFC 4122 errata 1352 corrects the Appendix B value; Python's uuid3()
    // computes the same result.
    const uuid widgets =
        crpc::crypto::uuid_v3(crpc::crypto::namespace_dns, "www.widgets.com");
    CRPC_CHECK_EQ(std::string("3d813cbb-47fb-32ba-91df-831e1593ac29"),
                  crpc::crypto::to_string(widgets));

    // The same namespace and name must always produce the same value.
    CRPC_CHECK(widgets ==
               crpc::crypto::uuid_v3(crpc::crypto::namespace_dns, "www.widgets.com"));
}

CRPC_TEST(uuid_v5_name_based_vectors) {
    // RFC 9562 Appendix A.4.
    const uuid example = crpc::crypto::uuid_v5(crpc::crypto::namespace_dns, "www.example.com");
    CRPC_CHECK_EQ(std::string("2ed6657d-e927-568b-95e1-2665a8aea6a2"),
                  crpc::crypto::to_string(example));
    CRPC_CHECK_EQ(5, example.version());
    CRPC_CHECK_EQ(uuid_variant::rfc_4122, example.variant());

    // Python: uuid.uuid5(uuid.NAMESPACE_DNS, "www.widgets.com").
    CRPC_CHECK_EQ(std::string("21f7f8de-8051-5b89-8680-0195ef798b6a"),
                  crpc::crypto::to_string(
                      crpc::crypto::uuid_v5(crpc::crypto::namespace_dns, "www.widgets.com")));

    // A different namespace must not collide with the DNS one.
    CRPC_CHECK(crpc::crypto::uuid_v5(crpc::crypto::namespace_dns, "www.example.com") !=
               crpc::crypto::uuid_v5(crpc::crypto::namespace_url, "www.example.com"));
}

CRPC_TEST(uuid_v1_rfc9562_field_layout) {
    // RFC 9562 Appendix A.1: timestamp 0x01EC9414C232AB00, clock sequence
    // 0x33C8 and node 0x9F6BDECED846.
    crpc::crypto::uuid_v1_context context;
    context.node = {0x9f, 0x6b, 0xde, 0xce, 0xd8, 0x46};
    context.clock_sequence = 0x33c8;
    const uuid value = crpc::crypto::uuid_v1(0x01ec9414c232ab00ULL, context);

    CRPC_CHECK_EQ(std::string("c232ab00-9414-11ec-b3c8-9f6bdeced846"),
                  crpc::crypto::to_string(value));
    CRPC_CHECK_EQ(1, value.version());
    CRPC_CHECK_EQ(uuid_variant::rfc_4122, value.variant());
    CRPC_CHECK_EQ(std::uint64_t{0x01ec9414c232ab00ULL}, value.timestamp_100ns());
    CRPC_CHECK_EQ(std::uint16_t{0x33c8}, clock_sequence(value));
    CRPC_CHECK_EQ(std::uint64_t{0x9f6bdeced846ULL}, node(value));
}

CRPC_TEST(uuid_v1_system_clock_round_trip) {
    const uuid first = crpc::crypto::uuid_v1();
    const uuid second = crpc::crypto::uuid_v1();

    CRPC_CHECK_EQ(1, first.version());
    CRPC_CHECK_EQ(uuid_variant::rfc_4122, first.variant());
    // The node is fixed for the process, and two calls one after the other must
    // not repeat a timestamp even when the clock granularity is coarser than
    // the 100 ns UUID tick.
    CRPC_CHECK_EQ(node(first), node(second));
    CRPC_CHECK(first.timestamp_100ns() < second.timestamp_100ns());

    // Convert the Gregorian 100 ns count back to Unix time and compare with the
    // clock the value was generated from. A generous second of slack keeps the
    // check meaningful without depending on scheduler latency.
    constexpr std::uint64_t gregorian_to_unix_100ns = 122192928000000000ULL;
    CRPC_REQUIRE(first.timestamp_100ns() >= gregorian_to_unix_100ns);
    const std::int64_t generated =
        static_cast<std::int64_t>(first.timestamp_100ns() - gregorian_to_unix_100ns);
    const std::int64_t now =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count() /
        100;
    const std::int64_t difference = now - generated;
    CRPC_CHECK(difference >= -10000000);
    CRPC_CHECK(difference <= 10000000);
}

CRPC_TEST(uuid_v4_version_variant_and_uniqueness) {
    constexpr int count = 4096;
    int wrong_version = 0;
    int wrong_variant = 0;
    std::unordered_set<uuid, uuid_hash> seen;
    for (int i = 0; i < count; ++i) {
        const uuid value = crpc::crypto::uuid_v4();
        if (value.version() != 4) {
            ++wrong_version;
        }
        if (value.variant() != uuid_variant::rfc_4122 ||
            (value.bytes[8] & 0xc0u) != 0x80u) {
            ++wrong_variant;
        }
        seen.insert(value);
    }
    CRPC_CHECK_EQ(0, wrong_version);
    CRPC_CHECK_EQ(0, wrong_variant);
    CRPC_CHECK_EQ(static_cast<std::size_t>(count), seen.size());
}

CRPC_TEST(uuid_v7_timestamp_and_same_millisecond_order) {
    // RFC 9562 Appendix A.6 fixes the first 48 bits to the Unix millisecond.
    const std::uint64_t millisecond = 1700000000123ULL;
    const uuid first = crpc::crypto::uuid_v7(millisecond);
    CRPC_CHECK_EQ(7, first.version());
    CRPC_CHECK_EQ(uuid_variant::rfc_4122, first.variant());
    CRPC_CHECK_EQ(millisecond, unix_ms(first));

    // One thousand values in the same millisecond must come out strictly
    // increasing in byte order, which is what makes `operator<` a time order.
    uuid previous = first;
    int order_failures = 0;
    for (int i = 0; i < 1000; ++i) {
        const uuid next = crpc::crypto::uuid_v7(millisecond);
        if (!(previous < next)) {
            ++order_failures;
        }
        previous = next;
    }
    CRPC_CHECK_EQ(0, order_failures);

    // A clock that steps backwards is clamped to the last millisecond instead
    // of producing a value that sorts before its predecessor.
    const uuid backwards = crpc::crypto::uuid_v7(millisecond - 1000);
    CRPC_CHECK(previous < backwards);
}

CRPC_TEST(uuid_parse_two_forms_and_case) {
    const auto canonical = parse("c232ab00-9414-11ec-b3c8-9f6bdeced846");
    CRPC_REQUIRE(canonical.ok());
    CRPC_CHECK_EQ(std::string("c232ab00-9414-11ec-b3c8-9f6bdeced846"),
                  crpc::crypto::to_string(canonical.value()));

    const auto upper = parse("C232AB00-9414-11EC-B3C8-9F6BDECED846");
    CRPC_REQUIRE(upper.ok());
    CRPC_CHECK(upper.value() == canonical.value());
    // Formatting is always lowercase, regardless of the input case.
    CRPC_CHECK_EQ(std::string("c232ab00-9414-11ec-b3c8-9f6bdeced846"),
                  crpc::crypto::to_string(upper.value()));

    const auto compact = parse("c232ab00941411ecb3c89f6bdeced846");
    CRPC_REQUIRE(compact.ok());
    CRPC_CHECK(compact.value() == canonical.value());

    const auto compact_upper = parse("C232AB00941411ECB3C89F6BDECED846");
    CRPC_REQUIRE(compact_upper.ok());
    CRPC_CHECK(compact_upper.value() == canonical.value());

    // Round trip through both textual forms.
    CRPC_CHECK_EQ(std::string("c232ab00-9414-11ec-b3c8-9f6bdeced846"),
                  crpc::crypto::to_string(parse(crpc::crypto::to_string(canonical.value()))
                                              .value()));
}

CRPC_TEST(uuid_parse_rejects_malformed_text) {
    const char* const invalid[] = {
        "",
        "c232ab00-9414-11ec-b3c8-9f6bdeced84",       // 35 characters
        "c232ab00-9414-11ec-b3c8-9f6bdeced8460",     // 37 characters
        "c232ab00941411ecb3c89f6bdeced84",           // 31 characters
        "c232ab00941411ecb3c89f6bdeced8460",         // 33 characters
        "c232ab0g-9414-11ec-b3c8-9f6bdeced846",      // non-hex digit
        "c232ab00-9414-11ec-b3c8-9f6bdeced84g",      // non-hex digit
        "c232ab00-9414-11ec-b3c8-9f6bdeced8 6",      // space
        "c232ab00_9414_11ec_b3c8_9f6bdeced846",      // wrong separators
        "c232ab0094-1411ec-b3c8-9f6bdeced846",       // hyphen moved
        "c232ab00-941411ec-b3c8-9f6bdeced846",       // hyphen moved
        "c232ab00-9414-11ecb3c8-9f6bdeced846",       // hyphen moved
        "c232ab00-9414-11ec-b3c89f6bdeced846",       // final hyphen missing
        "c232ab00-9414-11ec-b3c8-9f6bdeced84-6",     // trailing hyphen
        "{c232ab00-9414-11ec-b3c8-9f6bdeced846}",    // braces are not accepted
        "urn:uuid:c232ab00-9414-11ec-b3c8-9f6bdeced846",
        " c232ab00-9414-11ec-b3c8-9f6bdeced846",
    };
    for (const char* const text : invalid) {
        const auto parsed = parse(text);
        CRPC_CHECK(!parsed.ok());
        CRPC_CHECK_EQ(crpc::rpc_code::decode_error, parsed.code());
    }
}

CRPC_TEST(uuid_bytes_round_trip) {
    const uuid value = crpc::crypto::uuid_v4();
    const std::array<std::uint8_t, 16> bytes = crpc::crypto::to_bytes(value);
    const auto restored =
        crpc::crypto::uuid_from_bytes(crpc::span<const std::uint8_t>(bytes.data(), bytes.size()));
    CRPC_REQUIRE(restored.ok());
    CRPC_CHECK(restored.value() == value);

    const std::uint8_t short_bytes[15] = {};
    const auto too_short =
        crpc::crypto::uuid_from_bytes(crpc::span<const std::uint8_t>(short_bytes, 15));
    CRPC_CHECK(!too_short.ok());
    CRPC_CHECK_EQ(crpc::rpc_code::decode_error, too_short.code());
}

CRPC_TEST(uuid_ordering_and_hash) {
    const uuid low = parse("00000000-0000-0000-0000-000000000001").value();
    const uuid middle = parse("00000000-0000-0000-0001-000000000000").value();
    const uuid high = parse("01000000-0000-0000-0000-000000000000").value();

    CRPC_CHECK(low < middle);
    CRPC_CHECK(middle < high);
    CRPC_CHECK(low < high);
    CRPC_CHECK_FALSE(high < low);
    CRPC_CHECK(low == low);
    CRPC_CHECK(low != middle);

    // Operator< must agree with a byte-by-byte lexicographic comparison.
    CRPC_CHECK_EQ(low.bytes < middle.bytes, low < middle);
    CRPC_CHECK_EQ(middle.bytes < high.bytes, middle < high);

    std::unordered_map<uuid, int, uuid_hash> map;
    map[low] = 1;
    map[middle] = 2;
    map[high] = 3;
    CRPC_CHECK_EQ(static_cast<std::size_t>(3), map.size());
    CRPC_CHECK_EQ(1, map[low]);
    CRPC_CHECK_EQ(2, map[middle]);
    CRPC_CHECK_EQ(3, map[high]);
    CRPC_CHECK_EQ(0, static_cast<int>(map.count(parse("ffffffff-ffff-ffff-ffff-ffffffffffff").value())));
}

CRPC_TEST(uuid_namespace_constants) {
    CRPC_CHECK_EQ(std::string("6ba7b810-9dad-11d1-80b4-00c04fd430c8"),
                  crpc::crypto::to_string(crpc::crypto::namespace_dns));
    CRPC_CHECK_EQ(std::string("6ba7b811-9dad-11d1-80b4-00c04fd430c8"),
                  crpc::crypto::to_string(crpc::crypto::namespace_url));
    CRPC_CHECK_EQ(std::string("6ba7b812-9dad-11d1-80b4-00c04fd430c8"),
                  crpc::crypto::to_string(crpc::crypto::namespace_oid));
    CRPC_CHECK_EQ(std::string("6ba7b814-9dad-11d1-80b4-00c04fd430c8"),
                  crpc::crypto::to_string(crpc::crypto::namespace_x500));
}

CRPC_TEST_MAIN()
