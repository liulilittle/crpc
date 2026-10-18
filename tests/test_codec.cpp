// CRPC -- codec layer tests.
//
// The suite covers the crpc-binary wire contract (including golden bytes),
// malformed input rejection, JSON through RTTI, the erased interface and the
// registry freeze semantics.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <crpc/codec.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <vector>

// test_framework.hpp (Debug) issues a file-scope
// `#pragma section(".CRT$XCU", read)`. Under MSVC that pragma makes every
// subsequent `static inline` data member's dynamic initializer inert, so RTTI's
// `fields_of<T>::members` tuple would stay zero-initialized (every member
// pointer reading as offset 0) in this translation unit. The codec's field walk
// depends on that table, so `_DEBUG` is undefined around the harness include to
// keep the pragma out of this TU. The CRT is still linked through /MDd, and
// `_ITERATOR_DEBUG_LEVEL` was already fixed by <crpc/codec.hpp>.
#undef _DEBUG
#include "test_framework.hpp"

// RTTI registration macros must be emitted at global namespace scope: they
// specialize rtti::fields_of / rtti::enum_table, and a specialization inside an
// anonymous namespace would target anonymous::rtti instead.
enum class color : std::uint8_t { red = 1, green = 2, blue = 255 };
RTTI_DEFINE_ENUM(color, red, green, blue)

struct all_scalars {
    bool flag;
    std::int8_t i8v;
    std::int16_t i16v;
    std::int32_t i32v;
    std::int64_t i64v;
    std::uint8_t u8v;
    std::uint16_t u16v;
    std::uint32_t u32v;
    std::uint64_t u64v;
    float f32;
    double f64;
    std::string text;
    std::vector<std::uint8_t> blob;
};
RTTI_DEFINE_TYPE(all_scalars, flag, i8v, i16v, i32v, i64v, u8v, u16v, u32v,
                 u64v, f32, f64, text, blob)

struct inner {
    std::int32_t a;
    std::string b;
};
RTTI_DEFINE_TYPE(inner, a, b)

struct outer {
    std::vector<std::int32_t> numbers;
    std::vector<std::string> names;
    std::map<std::string, std::int32_t> counts;
    std::optional<std::int32_t> maybe;
    std::optional<std::string> maybe_name;
    inner nested;
    color hue;
};
RTTI_DEFINE_TYPE(outer, numbers, names, counts, maybe, maybe_name, nested, hue)

struct golden_point {
    std::int32_t x;
    std::int32_t y;
    std::string label;
};
RTTI_DEFINE_TYPE(golden_point, x, y, label)

struct base_message {
    std::int32_t a;
};
RTTI_DEFINE_TYPE(base_message, a)

struct derived_message : base_message {
    std::int32_t b;
};
RTTI_DEFINE_TYPE_DERIVED(derived_message, base_message, b)

// Two-level inheritance: the flattened field table must contain the
// grandparent, parent, and own fields in declaration order.
struct grand_message {
    std::int32_t g;
};
RTTI_DEFINE_TYPE(grand_message, g)

struct mid_message : grand_message {
    std::int32_t m;
};
RTTI_DEFINE_TYPE_DERIVED(mid_message, grand_message, m)

struct leaf_message : mid_message {
    std::int32_t l;
};
RTTI_DEFINE_TYPE_DERIVED(leaf_message, mid_message, l)

struct tree {
    std::int32_t value;
    std::vector<tree> children;
};
RTTI_DEFINE_TYPE(tree, value, children)

struct keep_default {
    std::int32_t a;
    std::int32_t b;
};
RTTI_DEFINE_TYPE_WITH_DEFAULT(keep_default, a, b)

struct leave_missing {
    std::int32_t a;
    std::int32_t b;
};
RTTI_DEFINE_TYPE(leave_missing, a, b)

namespace codec_test {

using crpc::binary_codec;
using crpc::buffer;
using crpc::codec;
using crpc::codec_id;
using crpc::codec_registry;
using crpc::rpc_code;
using crpc::span;
using crpc::status;
using crpc::u16;
using crpc::u32;
using crpc::u64;
using crpc::u8;

const codec& binary() {
    const codec* found =
        crpc::default_codec_registry().find(codec_id::crpc_binary);
    CRPC_CHECK(found != nullptr);
    return *found;
}

std::string to_hex(span<const std::byte> bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::byte value : bytes) {
        const unsigned raw = static_cast<unsigned>(value);
        out.push_back(digits[raw >> 4]);
        out.push_back(digits[raw & 0x0Fu]);
    }
    return out;
}

void put_varint(std::vector<std::byte>& out, u64 value) {
    while (value >= 0x80u) {
        out.push_back(static_cast<std::byte>((value & 0x7Fu) | 0x80u));
        value >>= 7;
    }
    out.push_back(static_cast<std::byte>(value));
}

std::vector<std::byte> bytes_of(std::initializer_list<unsigned> values) {
    std::vector<std::byte> out;
    out.reserve(values.size());
    for (const unsigned value : values) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

void append_field(std::vector<std::byte>& out, u64 id,
                  const std::vector<std::byte>& payload) {
    put_varint(out, id);
    put_varint(out, static_cast<u64>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
}

span<const std::byte> as_span(const std::vector<std::byte>& value) {
    return span<const std::byte>(value.data(), value.size());
}

} // namespace codec_test

using namespace codec_test;

CRPC_TEST(binary_scalars_roundtrip) {
    all_scalars input{};
    input.flag = true;
    input.i8v = std::numeric_limits<std::int8_t>::min();
    input.i16v = std::numeric_limits<std::int16_t>::max();
    input.i32v = std::numeric_limits<std::int32_t>::min();
    input.i64v = std::numeric_limits<std::int64_t>::min();
    input.u8v = std::numeric_limits<std::uint8_t>::max();
    input.u16v = std::numeric_limits<std::uint16_t>::max();
    input.u32v = std::numeric_limits<std::uint32_t>::max();
    input.u64v = std::numeric_limits<std::uint64_t>::max();
    input.f32 = -0.0f;
    input.f64 = std::numeric_limits<double>::lowest();
    input.text = "h\xc3\xa9llo";
    input.blob = {0x00, 0x7F, 0x80, 0xFF};

    buffer encoded;
    const status encoded_status = crpc::encode_into(binary(), input, encoded);
    CRPC_CHECK(encoded_status.ok());
    CRPC_CHECK(!encoded.empty());

    all_scalars output{};
    const status decoded_status =
        crpc::decode_from(binary(), encoded.readable(), output);
    CRPC_CHECK(decoded_status.ok());
    CRPC_CHECK_EQ(input.flag, output.flag);
    CRPC_CHECK_EQ(input.i8v, output.i8v);
    CRPC_CHECK_EQ(input.i16v, output.i16v);
    CRPC_CHECK_EQ(input.i32v, output.i32v);
    CRPC_CHECK_EQ(input.i64v, output.i64v);
    CRPC_CHECK_EQ(input.u8v, output.u8v);
    CRPC_CHECK_EQ(input.u16v, output.u16v);
    CRPC_CHECK_EQ(input.u32v, output.u32v);
    CRPC_CHECK_EQ(input.u64v, output.u64v);
    CRPC_CHECK_EQ(input.f32, output.f32);
    CRPC_CHECK(std::signbit(output.f32));
    CRPC_CHECK_EQ(input.f64, output.f64);
    CRPC_CHECK_EQ(input.text, output.text);
    CRPC_CHECK_EQ(input.blob.size(), output.blob.size());
    CRPC_CHECK(std::equal(input.blob.begin(), input.blob.end(),
                          output.blob.begin()));
}

CRPC_TEST(binary_containers_nested_optional_enum) {
    outer input{};
    input.numbers = {0, -1, 1, std::numeric_limits<std::int32_t>::max(),
                     std::numeric_limits<std::int32_t>::min()};
    input.names = {"", "alpha", "beta"};
    input.counts = {{"one", 1}, {"two", 2}, {"zero", 0}};
    input.maybe = -7;
    input.maybe_name = std::nullopt;
    input.nested.a = 42;
    input.nested.b = "nested";
    input.hue = color::blue;

    buffer encoded;
    CRPC_CHECK(crpc::encode_into(binary(), input, encoded).ok());

    outer output{};
    CRPC_CHECK(crpc::decode_from(binary(), encoded.readable(), output).ok());
    CRPC_CHECK_EQ(input.numbers.size(), output.numbers.size());
    CRPC_CHECK(std::equal(input.numbers.begin(), input.numbers.end(),
                          output.numbers.begin()));
    CRPC_CHECK_EQ(input.names.size(), output.names.size());
    CRPC_CHECK(std::equal(input.names.begin(), input.names.end(),
                          output.names.begin()));
    CRPC_CHECK_EQ(input.counts.size(), output.counts.size());
    CRPC_CHECK_EQ(input.counts.at("one"), output.counts.at("one"));
    CRPC_CHECK_EQ(input.counts.at("two"), output.counts.at("two"));
    CRPC_CHECK_EQ(input.counts.at("zero"), output.counts.at("zero"));
    CRPC_CHECK(output.maybe.has_value());
    CRPC_CHECK_EQ(*input.maybe, *output.maybe);
    CRPC_CHECK(!output.maybe_name.has_value());
    CRPC_CHECK_EQ(input.nested.a, output.nested.a);
    CRPC_CHECK_EQ(input.nested.b, output.nested.b);
    CRPC_CHECK_EQ(static_cast<unsigned>(input.hue),
                  static_cast<unsigned>(output.hue));
}

CRPC_TEST(binary_golden_bytes) {
    golden_point input{};
    input.x = -1;
    input.y = 2;
    input.label = "hi";

    buffer encoded;
    CRPC_CHECK(crpc::encode_into(binary(), input, encoded).ok());
    CRPC_CHECK_EQ(std::string("01010102010403026869"),
                  to_hex(encoded.readable()));

    const std::vector<std::byte> expected =
        bytes_of({0x01, 0x01, 0x01, 0x02, 0x01, 0x04, 0x03, 0x02, 0x68, 0x69});
    golden_point output{};
    CRPC_CHECK(crpc::decode_from(binary(), as_span(expected), output).ok());
    CRPC_CHECK_EQ(input.x, output.x);
    CRPC_CHECK_EQ(input.y, output.y);
    CRPC_CHECK_EQ(input.label, output.label);
}

CRPC_TEST(binary_field_order_and_unknown_ids) {
    // label, then x, then y; plus an unknown id 9 that must be skipped.
    std::vector<std::byte> wire =
        bytes_of({0x03, 0x02, 0x68, 0x69, 0x09, 0x03, 0x61, 0x62, 0x63,
                  0x01, 0x01, 0x01, 0x02, 0x01, 0x04});
    golden_point output{};
    CRPC_CHECK(crpc::decode_from(binary(), as_span(wire), output).ok());
    CRPC_CHECK_EQ(-1, output.x);
    CRPC_CHECK_EQ(2, output.y);
    CRPC_CHECK_EQ(std::string("hi"), output.label);
}

CRPC_TEST(binary_rejects_duplicate_id) {
    std::vector<std::byte> wire =
        bytes_of({0x01, 0x01, 0x01, 0x01, 0x01, 0x01});
    golden_point output{};
    const status decoded = crpc::decode_from(binary(), as_span(wire), output);
    CRPC_CHECK(!decoded.ok());
    CRPC_CHECK(decoded.code() == rpc_code::decode_error);
}

CRPC_TEST(binary_rejects_id_zero) {
    std::vector<std::byte> wire = bytes_of({0x00, 0x00});
    golden_point output{};
    const status decoded = crpc::decode_from(binary(), as_span(wire), output);
    CRPC_CHECK(!decoded.ok());
    CRPC_CHECK(decoded.code() == rpc_code::decode_error);
}

CRPC_TEST(binary_rejects_truncated_payload) {
    std::vector<std::byte> wire =
        bytes_of({0x01, 0x01, 0x01, 0x02, 0x01, 0x04, 0x03, 0x02, 0x68});
    golden_point output{};
    const status decoded = crpc::decode_from(binary(), as_span(wire), output);
    CRPC_CHECK(!decoded.ok());
    CRPC_CHECK(decoded.code() == rpc_code::decode_error);
}

CRPC_TEST(binary_strict_and_lenient_trailing_bytes) {
    // Field 1 payload is two bytes but the value is a one-byte varint.
    std::vector<std::byte> wire = bytes_of({0x01, 0x02, 0x01, 0x00});
    golden_point strict_output{};
    CRPC_CHECK(!crpc::decode_from(binary(), as_span(wire), strict_output).ok());

    binary_codec lenient(binary_codec::default_depth_limit, false);
    golden_point lenient_output{};
    CRPC_CHECK(crpc::decode_from(lenient, as_span(wire), lenient_output).ok());
    // Zigzag: raw varint 0x01 decodes to -1 (the golden test pins the same\n    // mapping). The point of this case is that the lenient codec ignores the\n    // trailing byte that the strict codec rejects.\n    CRPC_CHECK_EQ(-1, lenient_output.x);
}

CRPC_TEST(binary_depth_limit) {
    tree root{};
    root.value = 0;
    tree* cursor = &root;
    for (std::int32_t i = 1; i <= 20; ++i) {
        cursor->children.emplace_back();
        cursor = &cursor->children.back();
        cursor->value = i;
    }

    buffer encoded;
    const status encoded_status = crpc::encode_into(binary(), root, encoded);
    CRPC_CHECK(!encoded_status.ok());
    CRPC_CHECK(encoded_status.code() == rpc_code::encode_error);

    binary_codec deep(128, true);
    buffer deep_encoded;
    CRPC_CHECK(crpc::encode_into(deep, root, deep_encoded).ok());

    tree output{};
    const status decoded_status =
        crpc::decode_from(binary(), deep_encoded.readable(), output);
    CRPC_CHECK(!decoded_status.ok());
    CRPC_CHECK(decoded_status.code() == rpc_code::decode_error);

    tree deep_output{};
    CRPC_CHECK(crpc::decode_from(deep, deep_encoded.readable(), deep_output).ok());
    // The fixture is a chain: every node has exactly one child. Walking it is
    // what proves the whole nesting survived, not the size of the root's
    // children vector.
    int levels = 0;
    const tree* cursor_out = &deep_output;
    while (!cursor_out->children.empty()) {
        cursor_out = &cursor_out->children.front();
        ++levels;
    }
    CRPC_CHECK_EQ(20, levels);
}

CRPC_TEST(binary_derived_types_base_first) {
    derived_message input{};
    input.a = -1;
    input.b = 2;

    buffer encoded;
    CRPC_CHECK(crpc::encode_into(binary(), input, encoded).ok());
    CRPC_CHECK_EQ(std::string("010101020104"), to_hex(encoded.readable()));

    derived_message output{};
    CRPC_CHECK(crpc::decode_from(binary(), encoded.readable(), output).ok());
    CRPC_CHECK_EQ(-1, output.a);
    CRPC_CHECK_EQ(2, output.b);
}

CRPC_TEST(binary_multilevel_derived_fields) {
    leaf_message input{};
    input.g = -1;
    input.m = 2;
    input.l = 3;

    buffer encoded;
    CRPC_CHECK(crpc::encode_into(binary(), input, encoded).ok());
    CRPC_CHECK_EQ(std::string("010101020104030106"), to_hex(encoded.readable()));

    leaf_message output{};
    CRPC_CHECK(crpc::decode_from(binary(), encoded.readable(), output).ok());
    CRPC_CHECK_EQ(-1, output.g);
    CRPC_CHECK_EQ(2, output.m);
    CRPC_CHECK_EQ(3, output.l);
}

CRPC_TEST(binary_missing_field_policy) {
    // Only field 2 is present.
    std::vector<std::byte> wire = bytes_of({0x02, 0x01, 0x04});

    keep_default with_default{};
    with_default.a = 99;
    with_default.b = 99;
    CRPC_CHECK(crpc::decode_from(binary(), as_span(wire), with_default).ok());
    CRPC_CHECK_EQ(0, with_default.a);
    CRPC_CHECK_EQ(2, with_default.b);

    leave_missing without_default{};
    without_default.a = 99;
    without_default.b = 99;
    CRPC_CHECK(
        crpc::decode_from(binary(), as_span(wire), without_default).ok());
    CRPC_CHECK_EQ(99, without_default.a);
    CRPC_CHECK_EQ(2, without_default.b);
}

CRPC_TEST(binary_erased_interface) {
    binary_codec codec_instance;
    codec_instance.register_type<golden_point>();

    golden_point input{};
    input.x = 7;
    input.y = -9;
    input.label = "erased";

    buffer encoded;
    CRPC_CHECK(
        codec_instance.encode(&input, typeid(golden_point), encoded).ok());

    golden_point output{};
    CRPC_CHECK(
        codec_instance.decode(encoded.readable(), &output, typeid(golden_point))
            .ok());
    CRPC_CHECK_EQ(input.x, output.x);
    CRPC_CHECK_EQ(input.y, output.y);
    CRPC_CHECK_EQ(input.label, output.label);

    // Unregistered types are reported, never silently encoded.
    struct not_registered {
        std::int32_t value;
    };
    buffer scratch;
    const not_registered other{1};
    const status missing =
        codec_instance.encode(&other, typeid(not_registered), scratch);
    CRPC_CHECK(!missing.ok());
    CRPC_CHECK(missing.code() == rpc_code::encode_error);
}

#if defined(CRPC_HAVE_RTTI_JSON)
CRPC_TEST(json_roundtrip) {
    const codec* json = crpc::default_codec_registry().find(codec_id::json);
    CRPC_CHECK(json != nullptr);
    if (json == nullptr) {
        return;
    }

    inner input{};
    input.a = -1234;
    input.b = "json text";

    buffer encoded;
    CRPC_CHECK(crpc::encode_into(*json, input, encoded).ok());
    CRPC_CHECK(!encoded.empty());

    inner output{};
    CRPC_CHECK(crpc::decode_from(*json, encoded.readable(), output).ok());
    CRPC_CHECK_EQ(input.a, output.a);
    CRPC_CHECK_EQ(input.b, output.b);
}

CRPC_TEST(json_malformed_maps_to_decode_error) {
    const codec* json = crpc::default_codec_registry().find(codec_id::json);
    CRPC_CHECK(json != nullptr);
    if (json == nullptr) {
        return;
    }

    const std::string malformed = "{\"a\": [1, 2";
    inner output{};
    const status decoded = crpc::decode_from(
        *json,
        span<const std::byte>(
            reinterpret_cast<const std::byte*>(malformed.data()),
            malformed.size()),
        output);
    CRPC_CHECK(!decoded.ok());
    CRPC_CHECK(decoded.code() == rpc_code::decode_error);
    CRPC_CHECK(decoded.message().find("offset") != std::string::npos);
}
#endif

CRPC_TEST(registry_lookup_duplicate_and_freeze) {
    codec_registry registry;
    CRPC_CHECK(!registry.frozen());
    CRPC_CHECK_EQ(std::size_t{0}, registry.size());

    CRPC_CHECK(registry.add(std::make_unique<binary_codec>()).ok());
    CRPC_CHECK_EQ(std::size_t{1}, registry.size());

    CRPC_CHECK(registry.find(codec_id::crpc_binary) != nullptr);
    CRPC_CHECK(registry.find("application/crpc") != nullptr);
    CRPC_CHECK(registry.find("Application/CRPC; charset=utf-8") != nullptr);
    CRPC_CHECK(registry.find("application/unknown") == nullptr);

    // Duplicate id.
    CRPC_CHECK(registry.add(std::make_unique<binary_codec>()).code() ==
               rpc_code::failed_precondition);

    // Duplicate content type under a different id.
    class duplicate_content_type final : public codec {
    public:
        [[nodiscard]] codec_id id() const noexcept override {
            return static_cast<codec_id>(99);
        }
        [[nodiscard]] std::string_view content_type() const noexcept override {
            return "application/crpc";
        }
        [[nodiscard]] status encode(const void*, const std::type_info&,
                                    buffer&) const override {
            return status{};
        }
        [[nodiscard]] status decode(span<const std::byte>, void*,
                                    const std::type_info&) const override {
            return status{};
        }
    };
    CRPC_CHECK(registry.add(std::make_unique<duplicate_content_type>()).code() ==
               rpc_code::failed_precondition);

    CRPC_CHECK(registry.freeze().ok());
    CRPC_CHECK(registry.frozen());
    CRPC_CHECK(registry.find(codec_id::crpc_binary) != nullptr);
    CRPC_CHECK(registry.add(std::make_unique<binary_codec>()).code() ==
               rpc_code::failed_precondition);
    CRPC_CHECK(registry.freeze().ok());
}

CRPC_TEST(default_registry_contains_builtins) {
    codec_registry& registry = crpc::default_codec_registry();
    CRPC_CHECK(registry.frozen());
    CRPC_CHECK(registry.find(codec_id::crpc_binary) != nullptr);
    CRPC_CHECK(registry.find("application/crpc") != nullptr);
#if defined(CRPC_HAVE_RTTI_JSON)
    CRPC_CHECK(registry.find(codec_id::json) != nullptr);
    CRPC_CHECK(registry.find("application/json") != nullptr);
#endif
}

CRPC_TEST_MAIN()



