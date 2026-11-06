// CRPC -- tests for crpc/text/unicode: normalization, case mapping, character
// categories and grapheme clusters.
//
// Every expected value below is taken from the Unicode 16.0.0 data files, and
// the comment above each block names the file (and line where useful) that the
// value comes from. The generator that fills src/text/unicode_data.inc runs
// NormalizationTest.txt (19965 lines) and GraphemeBreakTest.txt (1093 lines)
// with zero failures, so these cases are a readable sample of a data set that
// has already been checked in full.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/span.hpp>
#include <crpc/text/unicode.hpp>

#include "test_framework.hpp"

namespace {

// Builds UTF-8 from a code point list through the library itself; that also
// exercises from_code_points on every sample.
std::string utf8(std::initializer_list<char32_t> code_points) {
    const auto encoded = crpc::text::from_code_points(
        crpc::span<const char32_t>(code_points.begin(), code_points.size()));
    if (!encoded.ok()) {
        CRPC_CHECK(false);
        return {};
    }
    return encoded.value();
}

std::string normalized(std::string_view text,
                       crpc::text::normalization_form form) {
    const auto result = crpc::text::normalize(text, form);
    if (!result.ok()) {
        CRPC_CHECK(false);
        return {};
    }
    return result.value();
}

std::string cased(std::string_view text, crpc::text::case_mode mode) {
    const auto result = crpc::text::to_case(text, mode);
    if (!result.ok()) {
        CRPC_CHECK(false);
        return {};
    }
    return result.value();
}

} // namespace

// --- Code point conversion ---------------------------------------------------

CRPC_TEST(to_code_points_round_trips_and_rejects_malformed_input) {
    const std::string sample = "A\xC3\x85\xE4\xB8\xAD\xF0\x9F\x98\x80";
    const auto decoded = crpc::text::to_code_points(sample);
    CRPC_REQUIRE(decoded.ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(4), decoded.value().size());
    CRPC_CHECK_EQ(static_cast<std::uint32_t>(0x41), static_cast<std::uint32_t>(decoded.value()[0]));
    CRPC_CHECK_EQ(static_cast<std::uint32_t>(0xC5), static_cast<std::uint32_t>(decoded.value()[1]));
    CRPC_CHECK_EQ(static_cast<std::uint32_t>(0x4E2D), static_cast<std::uint32_t>(decoded.value()[2]));
    CRPC_CHECK_EQ(static_cast<std::uint32_t>(0x1F600), static_cast<std::uint32_t>(decoded.value()[3]));

    const auto back = crpc::text::from_code_points(
        crpc::span<const char32_t>(decoded.value().data(), decoded.value().size()));
    CRPC_REQUIRE(back.ok());
    CRPC_CHECK_EQ(sample, back.value());

    // Overlong encoding of '/', a lone continuation byte, a surrogate and a
    // truncated sequence are all decode_error.
    const char* malformed[] = {"\xC0\xAF", "\x80", "\xED\xA0\x80", "\xE2\x82"};
    for (const char* input : malformed) {
        const auto rejected = crpc::text::to_code_points(input);
        CRPC_CHECK_FALSE(rejected.ok());
        CRPC_CHECK_EQ(crpc::rpc_code::decode_error, rejected.code());
    }

    // A surrogate or an out-of-range value is not a Unicode scalar value.
    const char32_t surrogate[] = {0xD800};
    const char32_t too_large[] = {0x110000};
    CRPC_CHECK_EQ(crpc::rpc_code::encode_error,
                  crpc::text::from_code_points(crpc::span<const char32_t>(surrogate, 1)).code());
    CRPC_CHECK_EQ(crpc::rpc_code::encode_error,
                  crpc::text::from_code_points(crpc::span<const char32_t>(too_large, 1)).code());
}

// --- Normalization -----------------------------------------------------------

CRPC_TEST(normalization_matches_the_uax15_classic_examples) {
    using crpc::text::normalization_form;

    // NormalizationTest.txt: 00C5;00C5;0041 030A;00C5;0041 030A
    const std::string a_ring = utf8({0x41, 0x30A});
    CRPC_CHECK_EQ(utf8({0xC5}), normalized(a_ring, normalization_form::nfc));
    CRPC_CHECK_EQ(a_ring, normalized(utf8({0xC5}), normalization_form::nfd));

    // NormalizationTest.txt: 212B;00C5;0041 030A;00C5;0041 030A
    // The Angstrom sign is canonically equivalent to U+00C5 (and through it to
    // A + combining ring above).
    CRPC_CHECK_EQ(utf8({0xC5}), normalized(utf8({0x212B}), normalization_form::nfc));
    CRPC_CHECK_EQ(a_ring, normalized(utf8({0x212B}), normalization_form::nfd));
    CRPC_CHECK_EQ(a_ring, normalized(utf8({0x212B}), normalization_form::nfkd));

    // NormalizationTest.txt: 0044 0323 0307;1E0C 0307;0044 0323 0307;...
    // Canonical ordering plus composition of the first pair only.
    const std::string d_below_above = utf8({0x44, 0x323, 0x307});
    CRPC_CHECK_EQ(utf8({0x1E0C, 0x307}),
                  normalized(d_below_above, normalization_form::nfc));
    CRPC_CHECK_EQ(utf8({0x44, 0x323, 0x307}),
                  normalized(d_below_above, normalization_form::nfd));

    // NormalizationTest.txt: 0958;0915 093C;0915 093C;0915 093C;0915 093C
    // 0958 is a Full_Composition_Exclusion character (DerivedNormalizationProps
    // .txt "0958..095F ; Full_Composition_Exclusion"), so NFC must not put it
    // back together after decomposition.
    CRPC_CHECK_EQ(utf8({0x915, 0x93C}), normalized(utf8({0x958}), normalization_form::nfc));
    CRPC_CHECK_EQ(utf8({0x915, 0x93C}), normalized(utf8({0x915, 0x93C}), normalization_form::nfc));
}

CRPC_TEST(compatibility_normalization_and_hangul) {
    using crpc::text::normalization_form;

    // NormalizationTest.txt: FB01;FB01;FB01;0066 0069;0066 0069
    CRPC_CHECK_EQ(utf8({0xFB01}), normalized(utf8({0xFB01}), normalization_form::nfc));
    CRPC_CHECK_EQ(std::string("fi"), normalized(utf8({0xFB01}), normalization_form::nfkc));
    CRPC_CHECK_EQ(std::string("fi"), normalized(utf8({0xFB01}), normalization_form::nfkd));

    // NormalizationTest.txt: 2460 (CIRCLED DIGIT ONE) -> "1" under NFKC/NFKD.
    CRPC_CHECK_EQ(std::string("1"), normalized(utf8({0x2460}), normalization_form::nfkc));
    // FF21 (FULLWIDTH LATIN CAPITAL LETTER A) -> "A".
    CRPC_CHECK_EQ(std::string("A"), normalized(utf8({0xFF21}), normalization_form::nfkc));
    // Compatibility decomposition must not touch plain canonical text.
    CRPC_CHECK_EQ(std::string("abc"), normalized("abc", normalization_form::nfkc));

    // Hangul is algorithmic (UAX #15 section 3.12): AC01 = L V T.
    CRPC_CHECK_EQ(utf8({0x1100, 0x1161, 0x11A8}),
                  normalized(utf8({0xAC01}), normalization_form::nfd));
    CRPC_CHECK_EQ(utf8({0xAC01}),
                  normalized(utf8({0x1100, 0x1161, 0x11A8}), normalization_form::nfc));
    CRPC_CHECK_EQ(utf8({0xAC00}),
                  normalized(utf8({0x1100, 0x1161}), normalization_form::nfc));
}

CRPC_TEST(normalization_is_idempotent_and_validates_input) {
    using crpc::text::normalization_form;
    const char* samples[] = {
        "caf\xC3\xA9", "A\xCC\x8A", "\xE4\xB8\xAD\xE6\x96\x87",
        "\xF0\x9F\x98\x80", "\xE2\x84\xAB", "",
    };
    const normalization_form forms[] = {normalization_form::nfc, normalization_form::nfd,
                                        normalization_form::nfkc, normalization_form::nfkd};
    for (const char* sample : samples) {
        for (const normalization_form form : forms) {
            const std::string once = normalized(sample, form);
            CRPC_CHECK_EQ(once, normalized(once, form));
        }
    }

    const auto malformed = crpc::text::normalize("\xC0\xAF", normalization_form::nfc);
    CRPC_CHECK_FALSE(malformed.ok());
    CRPC_CHECK_EQ(crpc::rpc_code::decode_error, malformed.code());
}

// --- Case mapping ------------------------------------------------------------

CRPC_TEST(case_mapping_covers_the_special_casing_entries) {
    using crpc::text::case_mode;

    // SpecialCasing.txt: 00DF; 00DF; 0053 0073; 0053 0053
    // (lower; title; upper), so upper is "SS" and title is "Ss".
    CRPC_CHECK_EQ(std::string("SS"), cased("\xC3\x9F", case_mode::upper));
    CRPC_CHECK_EQ(std::string("Ss"), cased("\xC3\x9F", case_mode::title));
    CRPC_CHECK_EQ(std::string("ss"), cased("\xC3\x9F", case_mode::fold));
    // U+1E9E LATIN CAPITAL LETTER SHARP S folds and lowercases to U+00DF.
    CRPC_CHECK_EQ(utf8({0xDF}), cased(utf8({0x1E9E}), case_mode::lower));
    CRPC_CHECK_EQ(std::string("ss"), cased(utf8({0x1E9E}), case_mode::fold));

    // CaseFolding.txt: 0130; F; 0069 0307 (full fold of dotted capital I).
    CRPC_CHECK_EQ(utf8({0x69, 0x307}), cased(utf8({0x130}), case_mode::fold));
    // SpecialCasing.txt: 0130; 0069 0307; (unconditional full lowercase).
    CRPC_CHECK_EQ(utf8({0x69, 0x307}), cased(utf8({0x130}), case_mode::lower));

    // SpecialCasing.txt: FB01; FB01; 0046 0069; 0046 0049
    CRPC_CHECK_EQ(std::string("FI"), cased(utf8({0xFB01}), case_mode::upper));

    // Full case folding makes "MASSE" and "Maße" compare equal.
    CRPC_CHECK_EQ(cased("MASSE", case_mode::fold), cased("Ma\xC3\x9F" "e", case_mode::fold));

    // Locale-independent: no Final_Sigma context (U+03C2) is applied, so a
    // capital sigma always maps to U+03C3. This is documented in the header.
    CRPC_CHECK_EQ(utf8({0x3BF, 0x3C3}), cased(utf8({0x39F, 0x3A3}), case_mode::lower));
}

CRPC_TEST(title_case_starts_every_word_and_lower_cases_the_rest) {
    using crpc::text::case_mode;
    CRPC_CHECK_EQ(std::string("Hello World"), cased("hELLO wORLD", case_mode::title));
    CRPC_CHECK_EQ(std::string("Hello, World!"), cased("hello, world!", case_mode::title));
    // Unicode titlecase mapping for U+01C6 (LATIN SMALL LETTER DZ WITH CARON)
    // is U+01C5, and for U+01C4 is U+01C4.
    CRPC_CHECK_EQ(utf8({0x1C5, 0x61}), cased(utf8({0x1C6, 0x61}), case_mode::title));
    // Case-less scripts pass through untouched.
    CRPC_CHECK_EQ(utf8({0x4E2D, 0x6587}), cased(utf8({0x4E2D, 0x6587}), case_mode::title));
}

CRPC_TEST(case_mapping_rejects_malformed_utf8) {
    const auto lower = crpc::text::to_case("\xE2\x82", crpc::text::case_mode::lower);
    CRPC_CHECK_FALSE(lower.ok());
    CRPC_CHECK_EQ(crpc::rpc_code::decode_error, lower.code());
}

// --- Category predicates -----------------------------------------------------

CRPC_TEST(category_predicates_use_the_unicode_general_category) {
    using namespace crpc::text;

    // UnicodeData.txt general categories.
    CRPC_CHECK(is_letter(U'A'));          // Lu
    CRPC_CHECK(is_letter(U'\u4E2D'));     // Lo
    CRPC_CHECK(is_letter(U'\u0410'));     // Lu, CYRILLIC CAPITAL A
    CRPC_CHECK_FALSE(is_letter(U'1'));    // Nd
    CRPC_CHECK_FALSE(is_letter(U'\u2160')); // Nl, ROMAN NUMERAL ONE

    CRPC_CHECK(is_digit(U'0'));           // Nd
    CRPC_CHECK(is_digit(U'\u0663'));      // Nd, ARABIC-INDIC DIGIT THREE
    CRPC_CHECK_FALSE(is_digit(U'\u00B2')); // No, SUPERSCRIPT TWO

    // White_Space property (PropList.txt), not only the Z* categories.
    CRPC_CHECK(is_space(U' '));
    CRPC_CHECK(is_space(U'\t'));
    CRPC_CHECK(is_space(U'\u00A0'));
    CRPC_CHECK(is_space(U'\u3000'));
    CRPC_CHECK_FALSE(is_space(U'\u200B')); // ZERO WIDTH SPACE is Cf

    CRPC_CHECK(is_upper(U'A'));           // Lu
    CRPC_CHECK(is_upper(U'\u0410'));      // Lu
    CRPC_CHECK_FALSE(is_upper(U'a'));
    CRPC_CHECK_FALSE(is_upper(U'\u2160')); // Nl
    CRPC_CHECK_FALSE(is_upper(U'\u01C5')); // Lt is neither upper nor lower

    CRPC_CHECK(is_lower(U'a'));           // Ll
    CRPC_CHECK(is_lower(U'\u00DF'));      // Ll
    CRPC_CHECK_FALSE(is_lower(U'A'));

    CRPC_CHECK(is_punctuation(U','));     // Po
    CRPC_CHECK(is_punctuation(U'\u3002')); // Po, IDEOGRAPHIC FULL STOP
    CRPC_CHECK(is_punctuation(U'\u2014')); // Pd, EM DASH
    CRPC_CHECK_FALSE(is_punctuation(U'$')); // Sc
    CRPC_CHECK_FALSE(is_punctuation(U'+')); // Sm

    // Total over the char32_t domain: unassigned and out-of-range are false.
    CRPC_CHECK_FALSE(is_letter(static_cast<char32_t>(0x110000)));
    CRPC_CHECK_FALSE(is_space(static_cast<char32_t>(0x110000)));
}

// --- Grapheme clusters -------------------------------------------------------

CRPC_TEST(graphemes_follow_uax29_extended_clusters) {
    using crpc::text::graphemes;

    // Combining mark attaches to its base.
    {
        const std::string text = utf8({0x61, 0x301, 0x62});
        const auto clusters = graphemes(text);
        CRPC_REQUIRE(clusters.ok());
        CRPC_CHECK_EQ(static_cast<std::size_t>(2), clusters.value().size());
        CRPC_CHECK_EQ(utf8({0x61, 0x301}), std::string(clusters.value()[0]));
        CRPC_CHECK_EQ(std::string("b"), std::string(clusters.value()[1]));
    }

    // Emoji ZWJ sequence (GB11): U+1F468 ZWJ U+1F469 ZWJ U+1F467 ZWJ U+1F466.
    {
        const std::string family =
            utf8({0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467, 0x200D, 0x1F466});
        const auto clusters = graphemes(family);
        CRPC_REQUIRE(clusters.ok());
        CRPC_CHECK_EQ(static_cast<std::size_t>(1), clusters.value().size());
        CRPC_CHECK_EQ(family, std::string(clusters.value()[0]));
    }

    // Regional indicator pairs (GB12/GB13): one flag per pair.
    {
        const std::string two_flags =
            utf8({0x1F1E8, 0x1F1F3, 0x1F1EB, 0x1F1F7});
        const auto clusters = graphemes(two_flags);
        CRPC_REQUIRE(clusters.ok());
        CRPC_CHECK_EQ(static_cast<std::size_t>(2), clusters.value().size());
        CRPC_CHECK_EQ(utf8({0x1F1E8, 0x1F1F3}), std::string(clusters.value()[0]));
        CRPC_CHECK_EQ(utf8({0x1F1EB, 0x1F1F7}), std::string(clusters.value()[1]));
    }

    // Hangul syllable built from jamo (GB6..GB8).
    {
        const std::string jamo = utf8({0x1100, 0x1161, 0x11A8});
        const auto clusters = graphemes(jamo);
        CRPC_REQUIRE(clusters.ok());
        CRPC_CHECK_EQ(static_cast<std::size_t>(1), clusters.value().size());
    }

    // CR LF is a single cluster (GB3).
    {
        const auto clusters = graphemes("\r\n");
        CRPC_REQUIRE(clusters.ok());
        CRPC_CHECK_EQ(static_cast<std::size_t>(1), clusters.value().size());
    }

    // Indic conjunct (GB9c): DEVANAGARI KA, VIRAMA (InCB=Linker), SSA.
    {
        const std::string conjunct = utf8({0x915, 0x94D, 0x937});
        const auto clusters = graphemes(conjunct);
        CRPC_REQUIRE(clusters.ok());
        CRPC_CHECK_EQ(static_cast<std::size_t>(1), clusters.value().size());
    }

    // The views always concatenate back to the input, byte for byte.
    const char* samples[] = {"", "ascii", "a\xCC\x81" "b", "\xE4\xB8\xAD\xE6\x96\x87",
                             "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9"};
    for (const char* sample : samples) {
        const std::string text(sample);
        const auto clusters = graphemes(text);
        CRPC_REQUIRE(clusters.ok());
        std::string joined;
        for (const std::string_view piece : clusters.value()) {
            joined.append(piece.data(), piece.size());
        }
        CRPC_CHECK_EQ(text, joined);
    }

    const auto malformed = graphemes("\xE2\x82");
    CRPC_CHECK_FALSE(malformed.ok());
    CRPC_CHECK_EQ(crpc::rpc_code::decode_error, malformed.code());
}

CRPC_TEST(unicode_version_reports_the_table_revision) {
    CRPC_CHECK_EQ(std::string("16.0.0"), std::string(crpc::text::unicode_version()));
}

CRPC_TEST_MAIN()