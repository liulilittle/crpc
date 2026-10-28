// CRPC -- tests for crpc/text: UTF-8/UTF-16 conversion, BOM handling, case
// folding, string operations and strict number parsing.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/span.hpp>
#include <crpc/text/case.hpp>
#include <crpc/text/number.hpp>
#include <crpc/text/string_ops.hpp>
#include <crpc/text/utf.hpp>

#include "test_framework.hpp"

namespace {

crpc::span<const std::byte> byte_span(const std::string& s) {
    return crpc::span<const std::byte>(
        reinterpret_cast<const std::byte*>(s.data()), s.size());
}

crpc::span<const std::byte> byte_span(const char* data, std::size_t size) {
    return crpc::span<const std::byte>(
        reinterpret_cast<const std::byte*>(data), size);
}

// Builds a std::u16string from an explicit code-unit list.
std::u16string u16(const std::vector<std::uint16_t>& units) {
    std::u16string out;
    out.reserve(units.size());
    for (const std::uint16_t unit : units) {
        out.push_back(static_cast<char16_t>(unit));
    }
    return out;
}

// A string with explicit byte values, so malformed input is testable.
std::string bytes(const std::vector<unsigned char>& values) {
    std::string out;
    out.reserve(values.size());
    for (const unsigned char value : values) {
        out.push_back(static_cast<char>(value));
    }
    return out;
}

} // namespace

// --- UTF-8 <-> UTF-16 ---------------------------------------------------------

CRPC_TEST(utf8_to_utf16_round_trips_and_preserves_codepoints) {
    const char* samples[] = {
        "",
        "ascii",
        "caf\xC3\xA9",
        "\xE4\xB8\xAD\xE6\x96\x87",
        "a\xF0\x9F\x98\x80z",
        "\xF0\x90\x80\x80",
    };
    for (const char* sample : samples) {
        const std::string input(sample);
        const auto converted = crpc::text::utf8_to_utf16(input);
        CRPC_CHECK(converted.ok());
        if (converted.ok()) {
            const auto back = crpc::text::utf16_to_utf8(converted.value());
            CRPC_CHECK(back.ok());
            if (back.ok()) {
                CRPC_CHECK_EQ(input, back.value());
            }
        }
    }

    // Emoji U+1F600 encodes as a UTF-16 surrogate pair.
    const auto emoji = crpc::text::utf8_to_utf16("\xF0\x9F\x98\x80");
    CRPC_CHECK(emoji.ok());
    if (emoji.ok()) {
        CRPC_CHECK(emoji.value().size() == 2);
        CRPC_CHECK_EQ(static_cast<std::uint32_t>(0xD83D),
                      static_cast<std::uint32_t>(emoji.value()[0]));
        CRPC_CHECK_EQ(static_cast<std::uint32_t>(0xDE00),
                      static_cast<std::uint32_t>(emoji.value()[1]));
    }
}

CRPC_TEST(utf8_to_utf16_rejects_invalid_sequences) {
    const std::string invalid_cases[] = {
        bytes({0x80}),                                            // bare continuation
        bytes({0xC0, 0x80}),                                      // overlong NUL
        bytes({0xE2, 0x82}),                                      // truncated
        bytes({0xED, 0xA0, 0x80}),                                // UTF-8 encoded surrogate D800
        bytes({0xF4, 0x90, 0x80, 0x80}),                          // above U+10FFFF
        bytes({0xF5, 0x80, 0x80, 0x80}),                          // invalid lead
        bytes({0xE2, 0x28, 0xA1}),                                // bad continuation
        bytes({0xF0, 0x9F, 0x98}),                                // truncated 4-byte
        std::string("\xC3", 1),                                   // truncated 2-byte
    };
    for (const std::string& input : invalid_cases) {
        const auto converted = crpc::text::utf8_to_utf16(input);
        CRPC_CHECK_FALSE(converted.ok());
        if (!converted.ok()) {
            CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::decode_error),
                          static_cast<int>(converted.code()));
        }
    }
}

CRPC_TEST(utf16_to_utf8_rejects_unpaired_surrogates) {
    struct case_type {
        std::vector<std::uint16_t> units;
    };
    const case_type cases[] = {
        {{0xD800}},                        // lone high surrogate
        {{0xDC00}},                        // lone low surrogate
        {{0x41, 0xD83D, 0x42}},            // high surrogate not followed by low
        {{0xDC00, 0xD83D}},                // low before high
    };
    for (const case_type& entry : cases) {
        const auto converted = crpc::text::utf16_to_utf8(u16(entry.units));
        CRPC_CHECK_FALSE(converted.ok());
        if (!converted.ok()) {
            CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::decode_error),
                          static_cast<int>(converted.code()));
        }
    }

    // A complete pair is accepted.
    const auto ok = crpc::text::utf16_to_utf8(u16({0xD83D, 0xDE00}));
    CRPC_CHECK(ok.ok());
    if (ok.ok()) {
        CRPC_CHECK_EQ(std::string("\xF0\x9F\x98\x80"), ok.value());
    }
}

CRPC_TEST(utf16le_to_utf8_is_strict) {
    const std::string le_ascii("\x41\x00\x42\x00", 4);
    const auto ascii = crpc::text::utf16le_to_utf8(byte_span(le_ascii));
    CRPC_CHECK(ascii.ok());
    if (ascii.ok()) {
        CRPC_CHECK_EQ(std::string("AB"), ascii.value());
    }

    const auto emoji =
        crpc::text::utf16le_to_utf8(byte_span(std::string("\x3D\xD8\x00\xDE", 4)));
    CRPC_CHECK(emoji.ok());
    if (emoji.ok()) {
        CRPC_CHECK_EQ(std::string("\xF0\x9F\x98\x80"), emoji.value());
    }

    // Odd byte count cannot be a well-formed UTF-16 stream.
    const auto odd =
        crpc::text::utf16le_to_utf8(byte_span(std::string("\x41\x00\x42", 3)));
    CRPC_CHECK_FALSE(odd.ok());

    // A lone surrogate unit must be an error, not U+FFFD.
    const auto lone =
        crpc::text::utf16le_to_utf8(byte_span(std::string("\x00\xD8", 2)));
    CRPC_CHECK_FALSE(lone.ok());
    if (!lone.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::decode_error),
                      static_cast<int>(lone.code()));
    }

    const auto empty = crpc::text::utf16le_to_utf8(byte_span(std::string("", 0)));
    CRPC_CHECK(empty.ok());
    if (empty.ok()) {
        CRPC_CHECK_EQ(std::string(""), empty.value());
    }
}

CRPC_TEST(utf8_to_utf16_le_and_be_round_trip) {
    const std::string text = "caf\xC3\xA9 \xE4\xB8\xAD \xF0\x9F\x98\x80";

    const auto le = crpc::text::utf8_to_utf16le(text);
    CRPC_CHECK(le.ok());
    if (le.ok()) {
        const auto back = crpc::text::utf16le_to_utf8(
            crpc::span<const std::byte>(le.value().data(), le.value().size()));
        CRPC_CHECK(back.ok());
        if (back.ok()) {
            CRPC_CHECK_EQ(text, back.value());
        }
    }

    const auto be = crpc::text::utf8_to_utf16be(text);
    CRPC_CHECK(be.ok());
    if (be.ok()) {
        const auto back = crpc::text::utf16be_to_utf8(
            crpc::span<const std::byte>(be.value().data(), be.value().size()));
        CRPC_CHECK(back.ok());
        if (back.ok()) {
            CRPC_CHECK_EQ(text, back.value());
        }
    }

    // The byte orders differ in the first code unit.
    CRPC_CHECK(le.ok() && be.ok());
    if (le.ok() && be.ok()) {
        CRPC_CHECK(le.value() != be.value());
    }
}

// --- BOM detection and general decode ----------------------------------------

CRPC_TEST(detect_bom_recognises_the_three_encodings) {
    const auto none = crpc::text::detect_bom(byte_span(std::string("abc", 3)));
    CRPC_CHECK(!none.has_value());

    const auto empty = crpc::text::detect_bom(byte_span(std::string("", 0)));
    CRPC_CHECK(!empty.has_value());

    const auto utf8 = crpc::text::detect_bom(
        byte_span(std::string("\xEF\xBB\xBF" "a", 4)));
    CRPC_CHECK(utf8.has_value());
    if (utf8.has_value()) {
        CRPC_CHECK_EQ(0, static_cast<int>(*utf8));
    }

    const auto le = crpc::text::detect_bom(byte_span(std::string("\xFF\xFE", 2)));
    CRPC_CHECK(le.has_value());
    if (le.has_value()) {
        CRPC_CHECK_EQ(1, static_cast<int>(*le));
    }

    const auto be = crpc::text::detect_bom(byte_span(std::string("\xFE\xFF", 2)));
    CRPC_CHECK(be.has_value());
    if (be.has_value()) {
        CRPC_CHECK_EQ(2, static_cast<int>(*be));
    }
}

CRPC_TEST(decode_strips_bom_and_detects_encoding) {
    // UTF-8 with BOM.
    const auto utf8 = crpc::text::decode(
        byte_span(std::string("\xEF\xBB\xBF" "A\xC3\xA9", 6)));
    CRPC_CHECK(utf8.ok());
    if (utf8.ok()) {
        CRPC_CHECK_EQ(std::string("A\xC3\xA9"), utf8.value());
    }

    // UTF-16LE with BOM.
    const auto le = crpc::text::decode(
        byte_span(std::string("\xFF\xFE\x3D\xD8\x00\xDE", 6)));
    CRPC_CHECK(le.ok());
    if (le.ok()) {
        CRPC_CHECK_EQ(std::string("\xF0\x9F\x98\x80"), le.value());
    }

    // UTF-16BE with BOM: U+0041 as FE? no, 0x0041 in BE is 00 41.
    const auto be = crpc::text::decode(
        byte_span(std::string("\xFE\xFF\x00\x41\x00\x42", 6)));
    CRPC_CHECK(be.ok());
    if (be.ok()) {
        CRPC_CHECK_EQ(std::string("AB"), be.value());
    }

    // No BOM: decoded as strict UTF-8.
    const auto bomless = crpc::text::decode(byte_span(std::string("\xE4\xB8\xAD", 3)));
    CRPC_CHECK(bomless.ok());
    if (bomless.ok()) {
        CRPC_CHECK_EQ(std::string("\xE4\xB8\xAD"), bomless.value());
    }

    // No BOM but invalid UTF-8: error rather than silent replacement.
    const auto invalid =
        crpc::text::decode(byte_span(std::string("\x80", 1)));
    CRPC_CHECK_FALSE(invalid.ok());

    // A lone surrogate with a UTF-16LE BOM is still an error.
    const auto lone = crpc::text::decode(byte_span(std::string("\xFF\xFE\x00\xD8", 4)));
    CRPC_CHECK_FALSE(lone.ok());

    // An explicit UTF-8 string_view decode validates strictly.
    const auto sv = crpc::text::decode(std::string_view("\xE2\x82", 2));
    CRPC_CHECK_FALSE(sv.ok());
}

// --- Case folding -------------------------------------------------------------

CRPC_TEST(case_folding_covers_ascii_and_latin1) {
    CRPC_CHECK(crpc::text::is_ascii(std::string_view("plain text")));
    CRPC_CHECK_FALSE(crpc::text::is_ascii(std::string_view("caf\xC3\xA9")));
    CRPC_CHECK(crpc::text::is_ascii(std::string_view("")));

    CRPC_CHECK_EQ(std::string("hello, world!"),
                  crpc::text::to_lower(std::string_view("Hello, World!")));
    CRPC_CHECK_EQ(std::string("HELLO, WORLD!"),
                  crpc::text::to_upper(std::string_view("Hello, World!")));

    // Latin-1 supplement: U+00C0 (À) <-> U+00E0 (à).
    CRPC_CHECK_EQ(std::string("\xC3\xA9"), crpc::text::to_lower(std::string_view("\xC3\x89")));
    CRPC_CHECK_EQ(std::string("\xC3\x89"), crpc::text::to_upper(std::string_view("\xC3\xA9")));

    // ASCII fast path never touches non-ASCII bytes; Latin-1 handles only
    // U+00C0..U+00FF, so Greek/Cyrillic pass through byte-for-byte unchanged.
    const std::string greek = "\xCE\xA9"; // U+03A9 capital omega
    CRPC_CHECK_EQ(greek, crpc::text::to_lower(std::string_view(greek)));
    CRPC_CHECK_EQ(greek, crpc::text::to_upper(std::string_view(greek)));

    // Latin-1 characters without case counterparts stay unchanged.
    const std::string yen = "\xC2\xA5"; // U+00A5
    CRPC_CHECK_EQ(yen, crpc::text::to_lower(std::string_view(yen)));
    CRPC_CHECK_EQ(yen, crpc::text::to_upper(std::string_view(yen)));
}

CRPC_TEST(case_insensitive_comparisons) {
    CRPC_CHECK(crpc::text::iequals(std::string_view("Hello"), std::string_view("hELLO")));
    CRPC_CHECK_FALSE(crpc::text::iequals(std::string_view("Hello"), std::string_view("Hell")));
    CRPC_CHECK(crpc::text::iequals(std::string_view("\xC3\x89"), std::string_view("\xC3\xA9")));
    CRPC_CHECK(crpc::text::iequals(std::string_view(""), std::string_view("")));

    CRPC_CHECK(crpc::text::istarts_with(std::string_view("Hello World"), std::string_view("he")));
    CRPC_CHECK_FALSE(crpc::text::istarts_with(std::string_view("Hello World"), std::string_view("wo")));
    CRPC_CHECK(crpc::text::iends_with(std::string_view("Hello World"), std::string_view("RLD")));
    CRPC_CHECK_FALSE(crpc::text::iends_with(std::string_view("Hello World"), std::string_view("HE")));
}

// --- trim / split / join / replace ---------------------------------------------

CRPC_TEST(trim_handles_sides_and_custom_charsets) {
    CRPC_CHECK_EQ(std::string_view("x"), crpc::text::trim("  x  "));
    CRPC_CHECK_EQ(std::string_view("x  "), crpc::text::trim_left("  x  "));
    CRPC_CHECK_EQ(std::string_view("  x"), crpc::text::trim_right("  x  "));
    CRPC_CHECK_EQ(std::string_view(""), crpc::text::trim("   "));
    CRPC_CHECK_EQ(std::string_view("x"), crpc::text::trim("x\r\n"));

    CRPC_CHECK_EQ(std::string_view("a/b///"), crpc::text::trim_left("//a/b///", "/"));
    CRPC_CHECK_EQ(std::string_view("//a/b"), crpc::text::trim_right("//a/b///", "/"));
    CRPC_CHECK_EQ(std::string_view("a/b"), crpc::text::trim("//a/b///", "/"));
    CRPC_CHECK_EQ(std::string_view("x"), crpc::text::trim("\t\r\nx ", " \t\r\n"));
    CRPC_CHECK_EQ(std::string_view("ab"), crpc::text::trim("xxabxx", "x"));
}

CRPC_TEST(split_and_join_are_inverse) {
    const auto parts = crpc::text::split("a,b,,c", ',');
    CRPC_CHECK_EQ(std::size_t(4), parts.size());
    if (parts.size() == 4) {
        CRPC_CHECK_EQ(std::string_view("a"), parts[0]);
        CRPC_CHECK_EQ(std::string_view("b"), parts[1]);
        CRPC_CHECK_EQ(std::string_view(""), parts[2]);
        CRPC_CHECK_EQ(std::string_view("c"), parts[3]);
    }

    // A trailing separator does not produce a trailing empty token.
    const auto trailing = crpc::text::split("a,b,", ',');
    CRPC_CHECK_EQ(std::size_t(2), trailing.size());

    const auto single = crpc::text::split("abc", ',');
    CRPC_CHECK_EQ(std::size_t(1), single.size());
    if (single.size() == 1) {
        CRPC_CHECK_EQ(std::string_view("abc"), single[0]);
    }

    const auto empty = crpc::text::split("", ',');
    CRPC_CHECK_EQ(std::size_t(0), empty.size());

    // Multi-character delimiter.
    const auto multi = crpc::text::split("a::b::c", "::");
    CRPC_CHECK_EQ(std::size_t(3), multi.size());

    const auto joined = crpc::text::join(parts, std::string_view(","));
    CRPC_CHECK_EQ(std::string("a,b,,c"), joined);

    const std::vector<std::string> owned{"x", "y"};
    CRPC_CHECK_EQ(std::string("x;y"), crpc::text::join(owned, std::string_view(";")));

    CRPC_CHECK_EQ(std::string(""), crpc::text::join(
        std::vector<std::string_view>{}, std::string_view(",")));
}

CRPC_TEST(starts_with_ends_with_and_replace_all) {
    CRPC_CHECK(crpc::text::starts_with("Hello", "He"));
    CRPC_CHECK_FALSE(crpc::text::starts_with("Hello", "el"));
    CRPC_CHECK(crpc::text::ends_with("Hello", "llo"));
    CRPC_CHECK_FALSE(crpc::text::ends_with("Hello", "He"));
    CRPC_CHECK(crpc::text::starts_with("", ""));
    CRPC_CHECK(crpc::text::ends_with("", ""));

    CRPC_CHECK_EQ(std::string("acacac"),
                  crpc::text::replace_all("ababab", "b", "c").value_or(std::string("fail")));
    CRPC_CHECK_EQ(std::string("a_b_c"),
                  crpc::text::replace_all("a-b-c", "-", "_").value_or(std::string("fail")));
    CRPC_CHECK_EQ(std::string("no match here"),
                  crpc::text::replace_all("no match here", "zz", "x").value_or(std::string("fail")));

    const auto empty_from = crpc::text::replace_all("abc", "", "x");
    CRPC_CHECK_FALSE(empty_from.ok());
}

// --- Strict number parsing and formatting ---------------------------------------

CRPC_TEST(parse_integers_is_strict) {
    const auto i1 = crpc::text::parse_i64("-12345");
    CRPC_CHECK(i1.ok());
    if (i1.ok()) {
        CRPC_CHECK_EQ(static_cast<std::int64_t>(-12345), i1.value());
    }

    const auto u1 = crpc::text::parse_u64("0123");
    CRPC_CHECK(u1.ok());
    if (u1.ok()) {
        CRPC_CHECK_EQ(std::uint64_t(123), u1.value());
    }

    const auto iv = crpc::text::parse_i64("0");
    CRPC_CHECK(iv.ok());
    if (iv.ok()) {
        CRPC_CHECK_EQ(std::int64_t(0), iv.value());
    }

    // Trailing text, empty, whitespace and signs-with-no-digits are junk.
    const std::string_view junk[] = {
        "", " 42", "42 ", "+", "-", "--1", "4x", "x4", "4 2", "\t4",
    };
    for (const std::string_view sample : junk) {
        CRPC_CHECK_FALSE(crpc::text::parse_i64(sample).ok());
        CRPC_CHECK_FALSE(crpc::text::parse_u64(sample).ok());
    }

    // A negative value is not a valid u64.
    CRPC_CHECK_FALSE(crpc::text::parse_u64("-1").ok());

    // Overflow must be reported, not wrapped.
    const auto too_big = crpc::text::parse_i64("9223372036854775808");
    CRPC_CHECK_FALSE(too_big.ok());
    if (!too_big.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(too_big.code()));
    }
    CRPC_CHECK_FALSE(crpc::text::parse_i64("-9223372036854775809").ok());
    CRPC_CHECK_FALSE(crpc::text::parse_u64("18446744073709551616").ok());

    // Boundary values parse exactly.
    const auto max64 = crpc::text::parse_i64("9223372036854775807");
    CRPC_CHECK(max64.ok());
    if (max64.ok()) {
        CRPC_CHECK_EQ(std::numeric_limits<std::int64_t>::max(), max64.value());
    }
    const auto min64 = crpc::text::parse_i64("-9223372036854775808");
    CRPC_CHECK(min64.ok());
    if (min64.ok()) {
        CRPC_CHECK_EQ(std::numeric_limits<std::int64_t>::min(), min64.value());
    }

    const auto i32 = crpc::text::parse_int("-2147483648");
    CRPC_CHECK(i32.ok());
    if (i32.ok()) {
        CRPC_CHECK_EQ(std::numeric_limits<std::int32_t>::min(), i32.value());
    }
    CRPC_CHECK_FALSE(crpc::text::parse_int("2147483648").ok());
    CRPC_CHECK_FALSE(crpc::text::parse_uint("4294967296").ok());
}

CRPC_TEST(parse_double_is_strict) {
    const auto d1 = crpc::text::parse_double("3.25");
    CRPC_CHECK(d1.ok());
    if (d1.ok()) {
        CRPC_CHECK_EQ(3.25, d1.value());
    }
    const auto d2 = crpc::text::parse_double("-1e3");
    CRPC_CHECK(d2.ok());
    if (d2.ok()) {
        CRPC_CHECK_EQ(-1000.0, d2.value());
    }
    const auto d3 = crpc::text::parse_double("0");
    CRPC_CHECK(d3.ok());
    if (d3.ok()) {
        CRPC_CHECK_EQ(0.0, d3.value());
    }
    const auto d4 = crpc::text::parse_double("1.5e-2");
    CRPC_CHECK(d4.ok());
    if (d4.ok()) {
        CRPC_CHECK(std::fabs(d4.value() - 0.015) < 1e-12);
    }

    // Infinities and NaN are not produced by strict decimal parsing.
    CRPC_CHECK_FALSE(crpc::text::parse_double("inf").ok());
    CRPC_CHECK_FALSE(crpc::text::parse_double("nan").ok());
    CRPC_CHECK_FALSE(crpc::text::parse_double("").ok());
    CRPC_CHECK_FALSE(crpc::text::parse_double("1.5 ").ok());
    CRPC_CHECK_FALSE(crpc::text::parse_double(" 1.5").ok());
    CRPC_CHECK_FALSE(crpc::text::parse_double("1e9999").ok());    // overflow
    CRPC_CHECK_FALSE(crpc::text::parse_double("0x1.2p3").ok());   // no hex floats
}

CRPC_TEST(format_round_trips) {
    CRPC_CHECK_EQ(std::string("0"), crpc::text::format(std::int64_t(0)));
    CRPC_CHECK_EQ(std::string("42"), crpc::text::format(std::int64_t(42)));
    CRPC_CHECK_EQ(std::string("-42"), crpc::text::format(std::int64_t(-42)));
    CRPC_CHECK_EQ(std::string("18446744073709551615"),
                  crpc::text::format(std::numeric_limits<std::uint64_t>::max()));
    CRPC_CHECK_EQ(std::string("2.5"), crpc::text::format(2.5));

    const double value = 0.1 + 0.2;
    CRPC_CHECK_EQ(value, crpc::text::parse_double(crpc::text::format(value)).value_or(-1.0));
}

CRPC_TEST_MAIN()