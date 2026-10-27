// CRPC -- BSON codec tests.
//
// Every expected byte string is derived by hand from the specification, not produced by
// the encoder: a test that reuses the encoder's arithmetic cannot catch an encoder bug.
// The small documents below are short enough that the byte counts are checkable by eye.
#include "test_framework.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <crpc/db/mongo/bson.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace {

using crpc::db::mongo::bson_document;
using crpc::db::mongo::bson_type;
using crpc::db::mongo::bson_value;

using bytes = std::vector<std::byte>;

[[nodiscard]] crpc::span<const std::byte> view(const bytes& data) noexcept {
    return crpc::span<const std::byte>(data.data(), data.size());
}

// "01 00 00 00" + ... + "00"
[[nodiscard]] bytes document(std::initializer_list<int> body) {
    bytes out;
    const std::size_t total = 4 + body.size() + 1;
    out.push_back(static_cast<std::byte>(total & 0xffu));
    out.push_back(static_cast<std::byte>((total >> 8) & 0xffu));
    out.push_back(static_cast<std::byte>((total >> 16) & 0xffu));
    out.push_back(static_cast<std::byte>((total >> 24) & 0xffu));
    for (const int byte : body) {
        out.push_back(static_cast<std::byte>(byte));
    }
    out.push_back(std::byte{0});
    return out;
}

[[nodiscard]] std::string hex(const bytes& data) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (const std::byte byte : data) {
        const auto value = static_cast<unsigned>(byte);
        out.push_back(digits[value >> 4]);
        out.push_back(digits[value & 0x0fu]);
    }
    return out;
}

} // namespace

CRPC_TEST(bson_encodes_an_empty_document) {
    const bson_document empty;
    const bytes encoded = crpc::db::mongo::encode_bson(empty).value();
    // Five bytes: the int32 length and the terminator.
    CRPC_CHECK_EQ(std::string("0500000000"), hex(encoded));
    CRPC_CHECK_EQ(std::size_t{5}, empty.encoded_size());
}

CRPC_TEST(bson_encodes_a_single_string) {
    bson_document doc;
    doc.append("hello", bson_value::make_text("world"));
    const bytes encoded = crpc::db::mongo::encode_bson(doc).value();
    // 4 length + 1 type + 6 key ("hello\0") + 4 string length + 6 text ("world\0") + 1
    // terminator = 22 bytes, i.e. 0x16.
    CRPC_CHECK_EQ(std::string("16000000"
                              "02"
                              "68656c6c6f00"
                              "06000000"
                              "776f726c6400"
                              "00"),
                  hex(encoded));
    CRPC_CHECK_EQ(std::size_t{22}, doc.encoded_size());
}

CRPC_TEST(bson_encodes_int32_int64_and_boolean) {
    bson_document doc;
    doc.append("i", bson_value::make_int32(1));
    doc.append("l", bson_value::make_int64(1));
    doc.append("b", bson_value::make_boolean(true));
    const bytes encoded = crpc::db::mongo::encode_bson(doc).value();
    // 4 + (1+2+4) + (1+2+8) + (1+2+1) + 1 = 27 = 0x1b
    CRPC_CHECK_EQ(std::string("1b000000"
                              "10" "6900" "01000000"
                              "12" "6c00" "0100000000000000"
                              "08" "6200" "01"
                              "00"),
                  hex(encoded));
}

CRPC_TEST(bson_encodes_a_nested_document_and_an_array) {
    bson_document inner;
    inner.append("x", bson_value::make_int32(2));
    bson_document doc;
    doc.append("d", bson_value::make_document(inner));
    doc.append("a", bson_value::make_array({bson_value::make_int32(3),
                                             bson_value::make_int32(4)}));
    const bytes encoded = crpc::db::mongo::encode_bson(doc).value();
    // Nested: 4 + (1+2+4) + 1 = 12 = 0x0c
    // Array:  4 + (1+2+4) + (1+2+4) + 1 = 19 = 0x13
    // Outer:  4 + (1+2+12) + (1+2+19) + 1 = 42 = 0x2a
    CRPC_CHECK_EQ(std::string("2a000000"
                              "03" "6400" "0c000000" "10" "7800" "02000000" "00"
                              "04" "6100" "13000000"
                              "10" "3000" "03000000"
                              "10" "3100" "04000000"
                              "00"
                              "00"),
                  hex(encoded));
}

CRPC_TEST(bson_encodes_binary_and_object_id) {
    bson_document doc;
    doc.append("bin", bson_value::make_binary({std::byte{0xAB}, std::byte{0xCD}}));
    doc.append("id", bson_value::make_object_id(bytes(12, std::byte{0x11})));
    const bytes encoded = crpc::db::mongo::encode_bson(doc).value();
    // binary: 1+4+4+1+2 = 12; objectid: 1+3+12 = 16; total 4+12+16+1 = 33 = 0x21
    CRPC_CHECK_EQ(std::string("21000000"
                              "05" "62696e00" "02000000" "00" "abcd"
                              "07" "696400" "111111111111111111111111"
                              "00"),
                  hex(encoded));
}

CRPC_TEST(bson_round_trips_every_supported_type) {
    bson_document nested;
    nested.append("n", bson_value::make_null());
    bson_document doc;
    doc.append("r", bson_value::make_real(2.5));
    doc.append("t", bson_value::make_text("text"));
    doc.append("i32", bson_value::make_int32(-7));
    doc.append("i64", bson_value::make_int64(-9000000000LL));
    doc.append("b", bson_value::make_boolean(false));
    doc.append("dt", bson_value::make_datetime(1700000000123LL));
    doc.append("bin", bson_value::make_binary({std::byte{1}, std::byte{2}, std::byte{3}}, 4));
    doc.append("oid", bson_value::make_object_id(bytes(12, std::byte{0x2A})));
    doc.append("doc", bson_value::make_document(nested));
    doc.append("arr", bson_value::make_array({bson_value::make_text("a"),
                                               bson_value::make_int32(1)}));
    doc.append("nul", bson_value::make_null());

    const bytes encoded = crpc::db::mongo::encode_bson(doc).value();
    CRPC_CHECK_EQ(doc.encoded_size(), encoded.size());

    bson_document decoded;
    std::size_t consumed = 0;
    const crpc::status read = crpc::db::mongo::decode_bson(view(encoded), decoded, consumed);
    CRPC_CHECK(static_cast<bool>(read));
    CRPC_CHECK_EQ(encoded.size(), consumed);
    CRPC_CHECK_EQ(std::size_t{11}, decoded.size());

    double real = 0.0;
    CRPC_CHECK(decoded.get_real("r", real));
    CRPC_CHECK_EQ(2.5, real);
    std::string text;
    CRPC_CHECK(decoded.get_text("t", text));
    CRPC_CHECK_EQ(std::string("text"), text);
    std::int64_t integer = 0;
    CRPC_CHECK(decoded.get_int64("i32", integer));
    CRPC_CHECK_EQ(std::int64_t{-7}, integer);
    CRPC_CHECK(decoded.get_int64("i64", integer));
    CRPC_CHECK_EQ(std::int64_t{-9000000000LL}, integer);
    bool flag = true;
    CRPC_CHECK(decoded.get_boolean("b", flag));
    CRPC_CHECK_FALSE(flag);
    // A datetime is its own type, not an integer: get_int64 refuses it on purpose, so the
    // assertion reads the field and checks the tag as well as the value.
    const bson_value* stamp = decoded.find("dt");
    CRPC_CHECK(stamp != nullptr);
    if (stamp != nullptr) {
        CRPC_CHECK_EQ(static_cast<int>(bson_type::datetime), static_cast<int>(stamp->type));
        CRPC_CHECK_EQ(std::int64_t{1700000000123LL}, stamp->integer);
    }
    CRPC_CHECK_FALSE(decoded.get_int64("dt", integer));

    const bson_value* binary = decoded.find("bin");
    CRPC_CHECK(binary != nullptr);
    if (binary != nullptr) {
        CRPC_CHECK_EQ(static_cast<int>(bson_type::binary), static_cast<int>(binary->type));
        CRPC_CHECK_EQ(std::size_t{3}, binary->bytes.size());
        CRPC_CHECK_EQ(4, static_cast<int>(binary->subtype));
    }
    const bson_value* oid = decoded.find("oid");
    CRPC_CHECK(oid != nullptr);
    if (oid != nullptr) {
        CRPC_CHECK_EQ(std::size_t{12}, oid->bytes.size());
    }
    const bson_document* inner = decoded.get_document("doc");
    CRPC_CHECK(inner != nullptr);
    if (inner != nullptr) {
        const bson_value* nul = inner->find("n");
        CRPC_CHECK(nul != nullptr);
        if (nul != nullptr) {
            CRPC_CHECK_EQ(static_cast<int>(bson_type::null), static_cast<int>(nul->type));
        }
    }
    const bson_value* array = decoded.find("arr");
    CRPC_CHECK(array != nullptr);
    if (array != nullptr) {
        CRPC_CHECK_EQ(std::size_t{2}, array->array.size());
        CRPC_CHECK_EQ(std::string("a"), array->array[0].text);
        CRPC_CHECK_EQ(std::int64_t{1}, array->array[1].integer);
    }
    // A field order change would change the encoded bytes, so the round trip must keep
    // the order the caller used.
    CRPC_CHECK_EQ(std::string("r"), decoded.fields()[0].first);
    CRPC_CHECK_EQ(std::string("nul"), decoded.fields()[10].first);
}

CRPC_TEST(bson_refuses_a_truncated_document) {
    bson_document doc;
    doc.append("k", bson_value::make_text("value"));
    const bytes encoded = crpc::db::mongo::encode_bson(doc).value();
    // Every prefix that cuts into the document must fail rather than decode partially.
    for (std::size_t cut = 5; cut < encoded.size(); ++cut) {
        const bytes partial(encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(cut));
        bson_document decoded;
        std::size_t consumed = 0;
        const crpc::status read = crpc::db::mongo::decode_bson(view(partial), decoded, consumed);
        CRPC_CHECK_FALSE(static_cast<bool>(read));
        CRPC_CHECK_EQ(std::size_t{0}, consumed);
    }
}

CRPC_TEST(bson_refuses_an_impossible_length) {
    // A length smaller than the header is not a document.
    const bytes tiny{std::byte{4}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
    bson_document decoded;
    std::size_t consumed = 0;
    CRPC_CHECK_FALSE(static_cast<bool>(
        crpc::db::mongo::decode_bson(view(tiny), decoded, consumed)));

    // A length larger than the buffer must not be trusted.
    const bytes lying{std::byte{0xff}, std::byte{0xff}, std::byte{0}, std::byte{0}, std::byte{0}};
    CRPC_CHECK_FALSE(static_cast<bool>(
        crpc::db::mongo::decode_bson(view(lying), decoded, consumed)));

    // peek_bson_size reports the claim so a framer can wait for more bytes.
    const auto claimed = crpc::db::mongo::peek_bson_size(view(lying));
    CRPC_CHECK(static_cast<bool>(claimed));
    if (claimed) {
        CRPC_CHECK_EQ(std::size_t{0xffff}, claimed.value());
    }
}

CRPC_TEST(bson_refuses_bytes_after_the_terminator) {
    // A document whose length covers an extra byte after the terminator: the length and
    // the contents disagree, which is a malformed document, not a longer one.
    const bytes bad = document({});
    bytes extended = bad;
    extended[0] = std::byte{6};
    extended.push_back(std::byte{0});
    bson_document decoded;
    std::size_t consumed = 0;
    CRPC_CHECK_FALSE(static_cast<bool>(
        crpc::db::mongo::decode_bson(view(extended), decoded, consumed)));
}

CRPC_TEST(bson_decodes_a_document_with_a_leading_and_trailing_field) {
    // Field order is preserved both ways, including the first field, because some
    // commands give the first field a meaning of its own.
    bson_document doc;
    doc.append("$db", bson_value::make_text("admin"));
    doc.append("ok", bson_value::make_real(1.0));
    const bytes encoded = crpc::db::mongo::encode_bson(doc).value();
    bson_document decoded;
    std::size_t consumed = 0;
    CRPC_CHECK(static_cast<bool>(
        crpc::db::mongo::decode_bson(view(encoded), decoded, consumed)));
    CRPC_CHECK_EQ(std::string("$db"), decoded.fields().front().first);
    CRPC_CHECK_EQ(std::string("ok"), decoded.fields().back().first);
}

CRPC_TEST_MAIN()