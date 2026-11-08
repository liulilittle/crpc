// CRPC -- protobuf and FlatBuffers codec tests.
//
// The shared record below is reflected once and encoded by every format the
// build offers (crpc-binary, JSON, protobuf, FlatBuffers), which is the only
// way to prove the codec registry maps one object to four independent wire
// walks with identical values on the way back. protobuf and FlatBuffers need
// explicit rtti::proto::schema<T> / rtti::flat::schema<T> specializations, so
// those are declared right where the types are.
//
// Error-path cases pin the contract the codec layer promises in
// <crpc/codec.hpp>: malformed, truncated or length-overrunning input returns
// rpc_code::decode_error through decode_from; it never crashes and never
// reports success.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <crpc/codec.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <vector>

#include "test_framework.hpp"

#if !CRPC_HAVE_RTTI

CRPC_TEST(codec_protobuf_flatbuffers_requires_rtti) {
    ::crpc_test::skip("RTTI reflection dependency absent");
}

CRPC_TEST_MAIN()

#else

enum class trip_kind : std::int32_t { none = 0, alpha = 1, omega = -3 };
RTTI_DEFINE_ENUM(trip_kind, none, alpha, omega)

struct trip_point {
    std::int32_t x;
    std::int32_t y;
};
RTTI_DEFINE_TYPE(trip_point, x, y)

struct trip_record {
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
    trip_point origin;
    std::vector<std::int32_t> samples;
    std::vector<std::string> tags;
};
RTTI_DEFINE_TYPE(trip_record, flag, i8v, i16v, i32v, i64v, u8v, u16v, u32v,
                 u64v, f32, f64, text, origin, samples, tags)

struct trip_blank {
    std::int32_t count;
    std::string label;
    std::vector<std::int32_t> values;
};
RTTI_DEFINE_TYPE(trip_blank, count, label, values)

struct trip_empty {};
RTTI_DEFINE_TYPE(trip_empty, /* no fields */)

struct pb_enum_record {
    trip_kind kind;
    std::optional<std::int32_t> maybe;
};
RTTI_DEFINE_TYPE(pb_enum_record, kind, maybe)

struct fb_text_holder {
    std::string text;
};
RTTI_DEFINE_TYPE(fb_text_holder, text)

#if defined(CRPC_HAVE_RTTI_PROTOBUF)
namespace rtti {
namespace proto {

template <>
struct schema<trip_point> {
    static constexpr std::size_t count = 2;
    static constexpr syntax syn = syntax::proto3;
    static inline const field_base* const array[count] = {
        make_field<trip_point, &trip_point::x>(
            make_field_desc("x", 1, wire_type::varint)),
        make_field<trip_point, &trip_point::y>(
            make_field_desc("y", 2, wire_type::varint)),
    };
};

template <>
struct schema<trip_record> {
    static constexpr std::size_t count = 15;
    static constexpr syntax syn = syntax::proto3;
    static inline const field_base* const array[count] = {
        make_field<trip_record, &trip_record::flag>(
            make_field_desc("flag", 1, wire_type::varint)),
        make_field<trip_record, &trip_record::i8v>(
            make_field_desc("i8v", 2, wire_type::varint)),
        make_field<trip_record, &trip_record::i16v>(
            make_field_desc("i16v", 3, wire_type::varint)),
        make_field<trip_record, &trip_record::i32v>(
            make_field_desc("i32v", 4, wire_type::varint)),
        make_field<trip_record, &trip_record::i64v>(
            make_field_desc("i64v", 5, wire_type::varint)),
        make_field<trip_record, &trip_record::u8v>(
            make_field_desc("u8v", 6, wire_type::varint)),
        make_field<trip_record, &trip_record::u16v>(
            make_field_desc("u16v", 7, wire_type::varint)),
        make_field<trip_record, &trip_record::u32v>(
            make_field_desc("u32v", 8, wire_type::varint)),
        make_field<trip_record, &trip_record::u64v>(
            make_field_desc("u64v", 9, wire_type::varint)),
        make_field<trip_record, &trip_record::f32>(
            make_field_desc("f32", 10, wire_type::fixed32)),
        make_field<trip_record, &trip_record::f64>(
            make_field_desc("f64", 11, wire_type::fixed64)),
        make_field<trip_record, &trip_record::text>(
            make_field_desc("text", 12, wire_type::length)),
        make_field<trip_record, &trip_record::origin>(
            make_field_desc("origin", 13, wire_type::length)),
        make_field<trip_record, &trip_record::samples>(
            make_field_desc("samples", 14, wire_type::varint,
                            field_mode::repeated, true)),
        // Field number 16 keeps the tag above 127 so the decoder must read a
        // two-byte tag varint, not just a single byte.
        make_field<trip_record, &trip_record::tags>(
            make_field_desc("tags", 16, wire_type::length,
                            field_mode::repeated)),
    };
};

template <>
struct schema<trip_blank> {
    static constexpr std::size_t count = 3;
    static constexpr syntax syn = syntax::proto3;
    static inline const field_base* const array[count] = {
        make_field<trip_blank, &trip_blank::count>(
            make_field_desc("count", 1, wire_type::varint)),
        make_field<trip_blank, &trip_blank::label>(
            make_field_desc("label", 2, wire_type::length)),
        make_field<trip_blank, &trip_blank::values>(
            make_field_desc("values", 3, wire_type::varint,
                            field_mode::repeated, true)),
    };
};

template <>
struct schema<pb_enum_record> {
    static constexpr std::size_t count = 2;
    static constexpr syntax syn = syntax::proto3;
    static inline const field_base* const array[count] = {
        make_field<pb_enum_record, &pb_enum_record::kind>(
            make_field_desc("kind", 1, wire_type::varint)),
        make_field<pb_enum_record, &pb_enum_record::maybe>(
            make_field_desc("maybe", 2, wire_type::varint)),
    };
};

} // namespace proto
} // namespace rtti
#endif

#if defined(CRPC_HAVE_RTTI_FLATBUFFERS)
namespace rtti {
namespace flat {

template <>
struct schema<trip_point> {
    static constexpr std::size_t count = 2;
    static inline const field_desc fields[count] = {
        make_field_desc("x", 0, fkind::i32),
        make_field_desc("y", 1, fkind::i32),
    };
};

template <>
struct schema<trip_record> {
    static constexpr std::size_t count = 15;
    static inline const field_desc fields[count] = {
        make_field_desc("flag", 0, fkind::bool_),
        make_field_desc("i8v", 1, fkind::i8),
        make_field_desc("i16v", 2, fkind::i16),
        make_field_desc("i32v", 3, fkind::i32),
        make_field_desc("i64v", 4, fkind::i64),
        make_field_desc("u8v", 5, fkind::u8),
        make_field_desc("u16v", 6, fkind::u16),
        make_field_desc("u32v", 7, fkind::u32),
        make_field_desc("u64v", 8, fkind::u64),
        make_field_desc("f32", 9, fkind::f32),
        make_field_desc("f64", 10, fkind::f64),
        make_field_desc("text", 11, fkind::string),
        make_field_desc("origin", 12, fkind::table, fkind::none,
                        schema_of<trip_point>()),
        make_field_desc("samples", 13, fkind::vector, fkind::i32),
        make_field_desc("tags", 14, fkind::vector, fkind::string),
    };
};

template <>
struct schema<trip_blank> {
    static constexpr std::size_t count = 3;
    static inline const field_desc fields[count] = {
        make_field_desc("count", 0, fkind::i32),
        make_field_desc("label", 1, fkind::string),
        make_field_desc("values", 2, fkind::vector, fkind::i32),
    };
};

template <>
struct schema<fb_text_holder> {
    static constexpr std::size_t count = 1;
    static inline const field_desc fields[count] = {
        make_field_desc("text", 0, fkind::string),
    };
};

} // namespace flat
} // namespace rtti
#endif

namespace codec_pf_test {

using crpc::buffer;
using crpc::codec;
using crpc::codec_id;
using crpc::rpc_code;
using crpc::span;
using crpc::status;
using crpc::u32;
using crpc::u64;

const codec* lookup(codec_id id) {
    const codec* found = crpc::default_codec_registry().find(id);
    CRPC_CHECK(found != nullptr);
    return found;
}

span<const std::byte> as_span(const std::vector<std::byte>& value) {
    return span<const std::byte>(value.data(), value.size());
}

span<const std::byte> as_span(const std::string& value) {
    return span<const std::byte>(
        reinterpret_cast<const std::byte*>(value.data()), value.size());
}

std::vector<std::byte> bytes_of(std::initializer_list<unsigned> values) {
    std::vector<std::byte> out;
    out.reserve(values.size());
    for (const unsigned value : values) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

trip_record make_record() {
    trip_record value{};
    value.flag = true;
    value.i8v = std::numeric_limits<std::int8_t>::min();
    value.i16v = std::numeric_limits<std::int16_t>::max();
    value.i32v = std::numeric_limits<std::int32_t>::min();
    value.i64v = std::numeric_limits<std::int64_t>::min();
    value.u8v = std::numeric_limits<std::uint8_t>::max();
    value.u16v = std::numeric_limits<std::uint16_t>::max();
    value.u32v = std::numeric_limits<std::uint32_t>::max();
    // FlatBuffers carries u64 through an int64 value_io node, so the portable
    // boundary across all four formats is INT64_MAX, not UINT64_MAX. The
    // protobuf-only case below still pins UINT64_MAX on the typed path.
    value.u64v = static_cast<std::uint64_t>(
        std::numeric_limits<std::int64_t>::max());
    value.f32 = 1.25f;
    value.f64 = -1234.5;
    value.text = "h\xc3\xa9llo";
    value.origin.x = std::numeric_limits<std::int32_t>::min();
    value.origin.y = std::numeric_limits<std::int32_t>::max();
    value.samples = {std::numeric_limits<std::int32_t>::min(), -1, 0, 1,
                     std::numeric_limits<std::int32_t>::max()};
    value.tags = {"", "alpha", "beta"};
    return value;
}

void check_record_eq(const trip_record& expected, const trip_record& actual) {
    CRPC_CHECK_EQ(expected.flag, actual.flag);
    CRPC_CHECK_EQ(expected.i8v, actual.i8v);
    CRPC_CHECK_EQ(expected.i16v, actual.i16v);
    CRPC_CHECK_EQ(expected.i32v, actual.i32v);
    CRPC_CHECK_EQ(expected.i64v, actual.i64v);
    CRPC_CHECK_EQ(expected.u8v, actual.u8v);
    CRPC_CHECK_EQ(expected.u16v, actual.u16v);
    CRPC_CHECK_EQ(expected.u32v, actual.u32v);
    CRPC_CHECK_EQ(expected.u64v, actual.u64v);
    CRPC_CHECK_EQ(expected.f32, actual.f32);
    CRPC_CHECK_EQ(expected.f64, actual.f64);
    CRPC_CHECK_EQ(std::signbit(expected.f32), std::signbit(actual.f32));
    CRPC_CHECK_EQ(expected.text, actual.text);
    CRPC_CHECK_EQ(expected.origin.x, actual.origin.x);
    CRPC_CHECK_EQ(expected.origin.y, actual.origin.y);
    CRPC_CHECK_EQ(expected.samples.size(), actual.samples.size());
    CRPC_CHECK(expected.samples == actual.samples);
    CRPC_CHECK_EQ(expected.tags.size(), actual.tags.size());
    CRPC_CHECK(expected.tags == actual.tags);
}

void check_blank_eq(const trip_blank& expected, const trip_blank& actual) {
    CRPC_CHECK_EQ(expected.count, actual.count);
    CRPC_CHECK_EQ(expected.label, actual.label);
    CRPC_CHECK(expected.values == actual.values);
}

void decode_error_of(const codec& target, span<const std::byte> wire,
                     const char* format_name, trip_record& out) {
    const status decoded = crpc::decode_from(target, wire, out);
    CRPC_CHECK(!decoded.ok());
    CRPC_CHECK_EQ(rpc_code::decode_error, decoded.code());
    CRPC_CHECK(decoded.message().find(format_name) != std::string::npos);
    CRPC_CHECK(decoded.message().find("offset") != std::string::npos);
}

} // namespace codec_pf_test

using namespace codec_pf_test;

CRPC_TEST(default_registry_contains_protobuf_and_flatbuffers) {
    crpc::codec_registry& registry = crpc::default_codec_registry();
    CRPC_CHECK(registry.frozen());
#if defined(CRPC_HAVE_RTTI_PROTOBUF)
    CRPC_CHECK(registry.find(codec_id::protobuf) != nullptr);
    CRPC_CHECK(registry.find("application/x-protobuf") != nullptr);
    CRPC_CHECK(registry.find("Application/X-Protobuf; charset=binary") !=
               nullptr);
#endif
#if defined(CRPC_HAVE_RTTI_FLATBUFFERS)
    CRPC_CHECK(registry.find(codec_id::flatbuffers) != nullptr);
    CRPC_CHECK(registry.find("application/x-flatbuffers") != nullptr);
#endif
}

#if defined(CRPC_HAVE_RTTI_PROTOBUF)
CRPC_TEST(protobuf_roundtrip_all_fields) {
    const codec* protobuf = lookup(codec_id::protobuf);
    CRPC_REQUIRE(protobuf != nullptr);

    const trip_record input = make_record();
    buffer encoded;
    CRPC_CHECK(crpc::encode_into(*protobuf, input, encoded).ok());
    CRPC_CHECK(!encoded.empty());

    trip_record output{};
    CRPC_CHECK(crpc::decode_from(*protobuf, encoded.readable(), output).ok());
    check_record_eq(input, output);
}

CRPC_TEST(protobuf_integer_and_float_boundaries) {
    const codec* protobuf = lookup(codec_id::protobuf);
    CRPC_REQUIRE(protobuf != nullptr);

    trip_record input = make_record();
    input.u64v = std::numeric_limits<std::uint64_t>::max();
    // -0.0f is not a usable boundary here: protoc (like this adapter) treats it
    // as the proto3 default 0 and omits it. lowest() stays non-zero and keeps
    // the sign bit meaningful.
    input.f32 = std::numeric_limits<float>::lowest();
    input.f64 = std::numeric_limits<double>::lowest();

    buffer encoded;
    CRPC_CHECK(crpc::encode_into(*protobuf, input, encoded).ok());

    trip_record output{};
    CRPC_CHECK(crpc::decode_from(*protobuf, encoded.readable(), output).ok());
    check_record_eq(input, output);
    CRPC_CHECK(std::signbit(output.f32));
}

CRPC_TEST(protobuf_long_string_uses_multi_byte_length) {
    const codec* protobuf = lookup(codec_id::protobuf);
    CRPC_REQUIRE(protobuf != nullptr);

    trip_record input = make_record();
    input.text.assign(200, 'x');

    buffer encoded;
    CRPC_CHECK(crpc::encode_into(*protobuf, input, encoded).ok());
    CRPC_CHECK(encoded.size() > input.text.size());

    trip_record output{};
    CRPC_CHECK(crpc::decode_from(*protobuf, encoded.readable(), output).ok());
    CRPC_CHECK_EQ(std::size_t{200}, output.text.size());
    CRPC_CHECK_EQ(input.text, output.text);
}

CRPC_TEST(protobuf_enum_and_optional) {
    const codec* protobuf = lookup(codec_id::protobuf);
    CRPC_REQUIRE(protobuf != nullptr);

    pb_enum_record input{};
    input.kind = trip_kind::omega;
    input.maybe = -7;

    buffer encoded;
    CRPC_CHECK(crpc::encode_into(*protobuf, input, encoded).ok());

    pb_enum_record output{};
    CRPC_CHECK(crpc::decode_from(*protobuf, encoded.readable(), output).ok());
    CRPC_CHECK_EQ(static_cast<std::int32_t>(input.kind),
                  static_cast<std::int32_t>(output.kind));
    CRPC_REQUIRE(output.maybe.has_value());
    CRPC_CHECK_EQ(*input.maybe, *output.maybe);

    input.maybe = std::nullopt;
    buffer encoded_absent;
    CRPC_CHECK(crpc::encode_into(*protobuf, input, encoded_absent).ok());
    pb_enum_record output_absent{};
    output_absent.maybe = 99;
    CRPC_CHECK(crpc::decode_from(*protobuf, encoded_absent.readable(),
                                 output_absent)
                   .ok());
    CRPC_CHECK(!output_absent.maybe.has_value());
}

CRPC_TEST(protobuf_empty_message_roundtrip) {
    const codec* protobuf = lookup(codec_id::protobuf);
    CRPC_REQUIRE(protobuf != nullptr);

    // proto3 omits default-valued scalars, so an all-default message has a
    // legal zero-byte encoding. The codec layer must accept it instead of
    // treating "no bytes" as a conversion failure.
    const trip_blank input{};
    buffer encoded;
    CRPC_CHECK(crpc::encode_into(*protobuf, input, encoded).ok());
    CRPC_CHECK(encoded.empty());

    trip_blank output{};
    output.count = 99;
    output.label = "stale";
    output.values = {1, 2, 3};
    CRPC_CHECK(crpc::decode_from(*protobuf, encoded.readable(), output).ok());
    check_blank_eq(input, output);
}

CRPC_TEST(protobuf_unregistered_type_is_a_clear_error) {
    const codec* protobuf = lookup(codec_id::protobuf);
    CRPC_REQUIRE(protobuf != nullptr);

    const trip_empty input{};
    buffer encoded;
    const status encoded_status = crpc::encode_into(*protobuf, input, encoded);
    CRPC_CHECK(!encoded_status.ok());
    CRPC_CHECK_EQ(rpc_code::encode_error, encoded_status.code());
    CRPC_CHECK(encoded_status.message().find("specialize") !=
               std::string::npos);

    trip_empty output{};
    const status decoded_status =
        crpc::decode_from(*protobuf, encoded.readable(), output);
    CRPC_CHECK(!decoded_status.ok());
    CRPC_CHECK_EQ(rpc_code::decode_error, decoded_status.code());
    CRPC_CHECK(decoded_status.message().find("specialize") !=
               std::string::npos);
}

CRPC_TEST(protobuf_rejects_truncated_varint) {
    const codec* protobuf = lookup(codec_id::protobuf);
    CRPC_REQUIRE(protobuf != nullptr);

    trip_record out{};
    // Field 1 tag with no value byte.
    decode_error_of(*protobuf, as_span(bytes_of({0x08})), "protobuf", out);
    // Field 1 value varint with the continuation bit at EOF.
    decode_error_of(*protobuf, as_span(bytes_of({0x08, 0x80})), "protobuf",
                    out);
    // A real encoding cut in the middle of the next field's value.
    const trip_record input = make_record();
    buffer encoded;
    CRPC_CHECK(crpc::encode_into(*protobuf, input, encoded).ok());
    CRPC_REQUIRE(encoded.size() > 3);
    trip_record truncated_out{};
    decode_error_of(*protobuf, encoded.readable().first(3), "protobuf",
                    truncated_out);
}

CRPC_TEST(protobuf_rejects_truncated_length_payload) {
    const codec* protobuf = lookup(codec_id::protobuf);
    CRPC_REQUIRE(protobuf != nullptr);

    trip_record out{};
    // Field 12 (text), length 5, only one payload byte present.
    decode_error_of(*protobuf,
                    as_span(bytes_of({0x62, 0x05, 'h'})), "protobuf", out);
}

CRPC_TEST(protobuf_rejects_length_overrun) {
    const codec* protobuf = lookup(codec_id::protobuf);
    CRPC_REQUIRE(protobuf != nullptr);

    trip_record out{};
    // Field 13 (origin), length 16383, no payload.
    decode_error_of(*protobuf, as_span(bytes_of({0x6A, 0xFF, 0x7F})),
                    "protobuf", out);
    // Field 16 (tags, two-byte tag) declares a 127-byte string with none.
    decode_error_of(*protobuf, as_span(bytes_of({0x82, 0x01, 0x7F})),
                    "protobuf", out);
}

CRPC_TEST(protobuf_rejects_bad_wire_type_and_field_zero) {
    const codec* protobuf = lookup(codec_id::protobuf);
    CRPC_REQUIRE(protobuf != nullptr);

    trip_record out{};
    // Field 1, wire type 3 (legacy group) is not a supported protobuf type.
    decode_error_of(*protobuf, as_span(bytes_of({0x0B})), "protobuf", out);
    // Field number 0 is invalid.
    decode_error_of(*protobuf, as_span(bytes_of({0x00, 0x00})), "protobuf",
                    out);
}

CRPC_TEST(protobuf_erased_interface_roundtrip) {
    crpc::protobuf_codec protobuf;
    protobuf.register_type<trip_point>();

    trip_point input{};
    input.x = -7;
    input.y = 42;

    buffer encoded;
    CRPC_CHECK(
        protobuf.encode(&input, typeid(trip_point), encoded).ok());
    CRPC_CHECK(!encoded.empty());

    trip_point output{};
    CRPC_CHECK(
        protobuf.decode(encoded.readable(), &output, typeid(trip_point))
            .ok());
    CRPC_CHECK_EQ(input.x, output.x);
    CRPC_CHECK_EQ(input.y, output.y);

    const trip_blank blank{};
    buffer blank_encoded;
    crpc::protobuf_codec measurable;
    measurable.register_type<trip_blank>();
    CRPC_CHECK(
        measurable.encode(&blank, typeid(trip_blank), blank_encoded).ok());
    CRPC_CHECK(blank_encoded.empty());
    trip_blank blank_output{};
    CRPC_CHECK(
        measurable.decode(blank_encoded.readable(), &blank_output,
                          typeid(trip_blank))
            .ok());
    check_blank_eq(blank, blank_output);
}
#endif

#if defined(CRPC_HAVE_RTTI_FLATBUFFERS)
CRPC_TEST(flatbuffers_roundtrip_all_fields) {
    const codec* flatbuffers = lookup(codec_id::flatbuffers);
    CRPC_REQUIRE(flatbuffers != nullptr);

    const trip_record input = make_record();
    buffer encoded;
    CRPC_CHECK(crpc::encode_into(*flatbuffers, input, encoded).ok());
    CRPC_CHECK(!encoded.empty());

    trip_record output{};
    CRPC_CHECK(
        crpc::decode_from(*flatbuffers, encoded.readable(), output).ok());
    check_record_eq(input, output);
}

CRPC_TEST(flatbuffers_empty_table_roundtrip) {
    const codec* flatbuffers = lookup(codec_id::flatbuffers);
    CRPC_REQUIRE(flatbuffers != nullptr);

    const trip_blank input{};
    buffer encoded;
    CRPC_CHECK(crpc::encode_into(*flatbuffers, input, encoded).ok());
    // A FlatBuffers root always carries the root offset (and a minimal
    // vtable), so an all-default table is not zero bytes.
    CRPC_CHECK(!encoded.empty());

    trip_blank output{};
    output.count = 99;
    output.label = "stale";
    output.values = {1, 2, 3};
    CRPC_CHECK(
        crpc::decode_from(*flatbuffers, encoded.readable(), output).ok());
    check_blank_eq(input, output);
}

CRPC_TEST(flatbuffers_unregistered_type_is_a_clear_error) {
    const codec* flatbuffers = lookup(codec_id::flatbuffers);
    CRPC_REQUIRE(flatbuffers != nullptr);

    const trip_empty input{};
    buffer encoded;
    const status encoded_status =
        crpc::encode_into(*flatbuffers, input, encoded);
    CRPC_CHECK(!encoded_status.ok());
    CRPC_CHECK_EQ(rpc_code::encode_error, encoded_status.code());
    CRPC_CHECK(encoded_status.message().find("specialize") !=
               std::string::npos);

    trip_empty output{};
    const status decoded_status =
        crpc::decode_from(*flatbuffers, encoded.readable(), output);
    CRPC_CHECK(!decoded_status.ok());
    CRPC_CHECK_EQ(rpc_code::decode_error, decoded_status.code());
    CRPC_CHECK(decoded_status.message().find("specialize") !=
               std::string::npos);
}

CRPC_TEST(flatbuffers_rejects_truncated_buffer) {
    const codec* flatbuffers = lookup(codec_id::flatbuffers);
    CRPC_REQUIRE(flatbuffers != nullptr);

    const trip_record input = make_record();
    buffer encoded;
    CRPC_CHECK(crpc::encode_into(*flatbuffers, input, encoded).ok());
    CRPC_REQUIRE(encoded.size() > 8);

    trip_record out{};
    // Only the root offset fits; the root table does not.
    decode_error_of(*flatbuffers, encoded.readable().first(4), "flatbuffers",
                    out);
    // Half the buffer: the root table is there, referenced data is not.
    decode_error_of(*flatbuffers,
                    encoded.readable().first(encoded.size() / 2),
                    "flatbuffers", out);
    // Empty input cannot hold a root offset.
    decode_error_of(*flatbuffers, span<const std::byte>(), "flatbuffers", out);
}

CRPC_TEST(flatbuffers_rejects_garbage_and_bad_root_offset) {
    const codec* flatbuffers = lookup(codec_id::flatbuffers);
    CRPC_REQUIRE(flatbuffers != nullptr);

    trip_record out{};
    const std::string garbage = "not a flatbuffer";
    decode_error_of(*flatbuffers, as_span(garbage), "flatbuffers", out);

    // Root table offset far beyond the 8-byte buffer.
    decode_error_of(*flatbuffers,
                    as_span(bytes_of({0x10, 0x00, 0x00, 0x00, 0x00, 0x00,
                                      0x00, 0x00})),
                    "flatbuffers", out);

    // Root table present, but its vtable soffset points before the buffer.
    decode_error_of(*flatbuffers,
                    as_span(bytes_of({0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
                                      0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x7F})),
                    "flatbuffers", out);

    // A protobuf wire stream cannot be a FlatBuffers root (offset overflows).
    const codec* protobuf = crpc::default_codec_registry().find(
        codec_id::protobuf);
    if (protobuf != nullptr) {
        const trip_record input = make_record();
        buffer protobuf_wire;
        if (crpc::encode_into(*protobuf, input, protobuf_wire).ok()) {
            decode_error_of(*flatbuffers, protobuf_wire.readable(),
                            "flatbuffers", out);
        }
    }
}

CRPC_TEST(flatbuffers_rejects_string_length_overrun) {
    const codec* flatbuffers = lookup(codec_id::flatbuffers);
    CRPC_REQUIRE(flatbuffers != nullptr);

    // Hand-built table for fb_text_holder { text: string }:
    //   [0,4)  root table offset 8
    //   [0,8)  vtable: size 8, table size 8, slot0 offset 4, slot1 absent
    //   [8,12) table soffset 8 -> vtable at 0
    //   [12,16) slot0 uoffset 4 -> string at 16
    //   [16,20) string length 0x7FFFFFFF, no payload
    const std::vector<std::byte> wire =
        bytes_of({0x08, 0x00, 0x00, 0x00, 0x08, 0x00, 0x08, 0x00, 0x04, 0x00,
                  0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
                  0xFF, 0xFF, 0xFF, 0x7F});
    fb_text_holder out{};
    const status decoded = crpc::decode_from(*flatbuffers, as_span(wire), out);
    CRPC_CHECK(!decoded.ok());
    CRPC_CHECK_EQ(rpc_code::decode_error, decoded.code());
    CRPC_CHECK(decoded.message().find("flatbuffers") != std::string::npos);
}

CRPC_TEST(flatbuffers_erased_interface_roundtrip) {
    crpc::flatbuffers_codec flatbuffers;
    flatbuffers.register_type<trip_point>();

    trip_point input{};
    input.x = 11;
    input.y = -13;

    buffer encoded;
    CRPC_CHECK(
        flatbuffers.encode(&input, typeid(trip_point), encoded).ok());
    CRPC_CHECK(!encoded.empty());

    trip_point output{};
    CRPC_CHECK(
        flatbuffers.decode(encoded.readable(), &output, typeid(trip_point))
            .ok());
    CRPC_CHECK_EQ(input.x, output.x);
    CRPC_CHECK_EQ(input.y, output.y);
}
#endif

CRPC_TEST(empty_struct_binary_and_json_roundtrip) {
    const codec* binary = lookup(codec_id::crpc_binary);
    CRPC_REQUIRE(binary != nullptr);

    const trip_empty input{};
    buffer binary_encoded;
    CRPC_CHECK(crpc::encode_into(*binary, input, binary_encoded).ok());
    trip_empty binary_output{};
    CRPC_CHECK(
        crpc::decode_from(*binary, binary_encoded.readable(), binary_output)
            .ok());

#if defined(CRPC_HAVE_RTTI_JSON)
    const codec* json = lookup(codec_id::json);
    CRPC_REQUIRE(json != nullptr);
    buffer json_encoded;
    CRPC_CHECK(crpc::encode_into(*json, input, json_encoded).ok());
    CRPC_CHECK(!json_encoded.empty());
    trip_empty json_output{};
    CRPC_CHECK(
        crpc::decode_from(*json, json_encoded.readable(), json_output).ok());
#endif
}

CRPC_TEST(codec_cross_format_roundtrip) {
    const trip_record input = make_record();

    const codec* binary = lookup(codec_id::crpc_binary);
    CRPC_REQUIRE(binary != nullptr);
    buffer binary_wire;
    CRPC_CHECK(crpc::encode_into(*binary, input, binary_wire).ok());
    trip_record binary_out{};
    CRPC_CHECK(
        crpc::decode_from(*binary, binary_wire.readable(), binary_out).ok());
    check_record_eq(input, binary_out);

#if defined(CRPC_HAVE_RTTI_JSON)
    const codec* json = lookup(codec_id::json);
    CRPC_REQUIRE(json != nullptr);
    buffer json_wire;
    CRPC_CHECK(crpc::encode_into(*json, input, json_wire).ok());
    trip_record json_out{};
    CRPC_CHECK(
        crpc::decode_from(*json, json_wire.readable(), json_out).ok());
    check_record_eq(input, json_out);
#endif

#if defined(CRPC_HAVE_RTTI_PROTOBUF)
    const codec* protobuf = lookup(codec_id::protobuf);
    CRPC_REQUIRE(protobuf != nullptr);
    buffer protobuf_wire;
    CRPC_CHECK(crpc::encode_into(*protobuf, input, protobuf_wire).ok());
    trip_record protobuf_out{};
    CRPC_CHECK(crpc::decode_from(*protobuf, protobuf_wire.readable(),
                                 protobuf_out)
                   .ok());
    check_record_eq(input, protobuf_out);
#endif

#if defined(CRPC_HAVE_RTTI_FLATBUFFERS)
    const codec* flatbuffers = lookup(codec_id::flatbuffers);
    CRPC_REQUIRE(flatbuffers != nullptr);
    buffer flatbuffers_wire;
    CRPC_CHECK(crpc::encode_into(*flatbuffers, input, flatbuffers_wire).ok());
    trip_record flatbuffers_out{};
    CRPC_CHECK(crpc::decode_from(*flatbuffers, flatbuffers_wire.readable(),
                                 flatbuffers_out)
                   .ok());
    check_record_eq(input, flatbuffers_out);
#endif
}

CRPC_TEST_MAIN()

#endif // CRPC_HAVE_RTTI
