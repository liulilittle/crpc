// CRPC -- Unicode normalization, case mapping, character categories and
// grapheme cluster segmentation.
//
// Backing data: the Unicode 16.0.0 Character Database (UnicodeData.txt,
// SpecialCasing.txt, CaseFolding.txt, PropList.txt, DerivedCoreProperties.txt,
// DerivedNormalizationProps.txt, auxiliary/GraphemeBreakProperty.txt) plus the
// UTS #51 emoji data for Extended_Pictographic. The tables live in
// `src/text/unicode_data.inc` and are produced by
// `.opencode/tmp/unicode/gen_unicode.py`; that generator runs the official
// NormalizationTest.txt (19965 lines) and GraphemeBreakTest.txt (1093 lines)
// conformance files against the parsed data and refuses to emit anything when
// a single line disagrees.
//
// All string entry points take and return UTF-8. Malformed input is always
// `rpc_code::decode_error` with the byte offset in the message; nothing is
// silently replaced with U+FFFD.
//
// Thread safety: every function in this module is a pure function of its
// arguments and the immutable tables, so they may be called concurrently.
#ifndef CRPC_TEXT_UNICODE_HPP
#define CRPC_TEXT_UNICODE_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace text {

// The four forms of UAX #15: canonical and compatibility decomposition, plus
// canonical and compatibility recomposition.
enum class normalization_form : std::uint8_t { nfc, nfd, nfkc, nfkd };

// `lower`/`upper` apply the full (multi-code-point) unconditional mappings
// from SpecialCasing.txt, so "ß" upper-cases to "SS", not to a single
// character. `title` upper-cases the first cased letter of every word and
// lower-cases the rest; a word is a maximal run of letters and combining
// marks, so punctuation and digits end it. `fold` is full case folding
// (CaseFolding.txt statuses C and F): it maps "ß" to "ss" and "ẞ" to "ss", and
// unlike `lower` it never maps two strings that compare equal to two different
// results.
//
// Locale- and context-sensitive mappings (SpecialCasing.txt conditions such as
// Final_Sigma and the tr/az/lt variants) are deliberately not applied: every
// function here is locale-independent. Greek capital sigma therefore always
// lower-cases to U+03C3, never to the word-final U+03C2.
enum class case_mode : std::uint8_t { lower, upper, title, fold };

// UAX #15 normalization. The result is UTF-8, fully normalized for `form`.
[[nodiscard]] CRPC_API result<std::string> normalize(std::string_view utf8,
                                                     normalization_form form);

// Strict UTF-8 <-> code point conversion. Surrogates, overlong sequences and
// values above U+10FFFF are rejected in both directions (the UTF-8 side with
// `decode_error`, the code point side with `encode_error`).
[[nodiscard]] CRPC_API result<std::vector<char32_t>>
to_code_points(std::string_view utf8);
[[nodiscard]] CRPC_API result<std::string>
from_code_points(span<const char32_t> code_points);

// Unicode-aware case conversion; see `case_mode` for the exact semantics.
[[nodiscard]] CRPC_API result<std::string> to_case(std::string_view utf8,
                                                   case_mode mode);

// --- General-category predicates --------------------------------------------
//
// `is_letter`   Lu, Ll, Lt, Lm, Lo
// `is_digit`    Nd (the decimal digits of every script, not just ASCII)
// `is_space`    the Unicode White_Space property (U+0009..U+000D, U+0020,
//               U+0085, U+00A0, U+1680, U+2000..U+200A, U+2028, U+2029,
//               U+202F, U+205F, U+3000)
// `is_upper`    Lu only; the titlecase category Lt answers false for both
//               predicates, and so do Other_Uppercase characters such as the
//               Roman numerals (Nl)
// `is_lower`    Ll only
// `is_punctuation` Pc, Pd, Pe, Pf, Pi, Po, Ps
//
// Unassigned code points (Cn) answer false to all six. Values above U+10FFFF
// are treated as unassigned rather than rejecting them, so the predicates are
// total over the char32_t domain.
[[nodiscard]] CRPC_API bool is_letter(char32_t code_point) noexcept;
[[nodiscard]] CRPC_API bool is_digit(char32_t code_point) noexcept;
[[nodiscard]] CRPC_API bool is_space(char32_t code_point) noexcept;
[[nodiscard]] CRPC_API bool is_upper(char32_t code_point) noexcept;
[[nodiscard]] CRPC_API bool is_lower(char32_t code_point) noexcept;
[[nodiscard]] CRPC_API bool is_punctuation(char32_t code_point) noexcept;

// --- Grapheme clusters -------------------------------------------------------

// Splits UTF-8 into extended grapheme clusters (UAX #29). Each returned view
// aliases `utf8`; the input must outlive the result. Concatenating the views in
// order reproduces the input exactly, so no byte is dropped or duplicated.
// Empty input yields an empty vector.
[[nodiscard]] CRPC_API result<std::vector<std::string_view>>
graphemes(std::string_view utf8);

// The Unicode version of the compiled tables, e.g. "16.0.0". Never nullptr.
[[nodiscard]] CRPC_API const char* unicode_version() noexcept;

} // namespace text
} // namespace crpc

#endif // CRPC_TEXT_UNICODE_HPP
