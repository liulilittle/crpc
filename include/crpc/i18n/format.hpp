// CRPC -- locale-aware number, currency and percentage formatting.
//
// The rules are transcribed from CLDR 49 (cldr-json, Unicode 16.0.0) and cover
// en-US, de-DE, zh-CN and fr-FR. The exact source values are:
//
//   locale   decimal  group   currency pattern    percent pattern
//   en-US    '.'      ','     ¤#,##0.00          #,##0%
//   de-DE    ','      '.'     #,##0.00<NBSP>¤    #,##0<NBSP>%
//   zh-CN    '.'      ','     ¤#,##0.00          #,##0%
//   fr-FR    ','      <NNBSP> #,##0.00<NBSP>¤    #,##0<NBSP>%
//
//   <NBSP>  = U+00A0 NO-BREAK SPACE
//   <NNBSP> = U+202F NARROW NO-BREAK SPACE
//
// (cldr-numbers-full/main/<locale>/numbers.json, keys
// "symbols-numberSystem-latn", "currencyFormats-numberSystem-latn"."standard"
// and "percentFormats-numberSystem-latn"."standard"; minimumGroupingDigits is
// 1 for all four, so grouping starts at four integer digits.)
//
// Currency symbols come from the same CLDR data (currencies.json "symbol"):
//
//   code   en-US   de-DE   zh-CN   fr-FR
//   USD    $       $       US$     $US
//   EUR    €       €       €       €
//   CNY    CN¥     CN¥     ¥       CNY
//
// A code the table does not carry is rendered as the code itself, which is
// what ICU does and keeps an unknown code visible instead of dropping it.
//
// Rounding uses the host's correctly rounded fixed-point conversion
// (`std::to_chars` with chars_format::fixed), so the digits are independent of
// the process locale. NaN and infinity are rejected: a catalog or wire value
// must not print as "nan".
//
// Parsing is the inverse of formatting for the same option set: it accepts the
// locale's own separators (plus any of the space characters CLDR uses for
// grouping in that locale), an optional sign, an optional exponent, and the
// currency symbol or percent sign the matching formatter would emit. It
// rejects ASCII '.', ',' and whitespace combinations that the locale does not
// use, so "1.234" parses as 1234 in de-DE (grouping) and as 1.234 in en-US.
#ifndef CRPC_I18N_FORMAT_HPP
#define CRPC_I18N_FORMAT_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace i18n {

struct locale_format_options {
    // BCP 47 tag. The language selects the rule set and the region selects the
    // currency symbol variant; "en", "de", "zh" and "fr" are understood, as
    // are their "-US"/"-DE"/"-CN"/"-FR" forms. Any other tag is
    // `invalid_argument` rather than a silent fallback to en-US.
    std::string locale = "en-US";
    // Fraction digits, 0..15. Grouping is unaffected: it applies to the
    // integer part.
    int decimal_digits = 2;
    bool use_grouping = true;
    // ISO 4217 code used when `use_currency` is true.
    std::string currency = "USD";
    // Multiply by 100 and append the locale's percent sign. Takes precedence
    // over `use_currency`.
    bool percent = false;
    // Emit the currency symbol in the locale's position. Off by default so
    // `format_number(value)` is a plain decimal number.
    bool use_currency = false;
};

// Formats with the locale's separators, optional grouping, optional currency
// symbol and optional percent scaling. Never emits an exponent; a value with a
// huge magnitude expands to all its digits.
[[nodiscard]] CRPC_API result<std::string>
format_number(double value, locale_format_options options = {});

// Integer convenience: same pipeline without a fractional part. The
// `decimal_digits` field is ignored.
[[nodiscard]] CRPC_API result<std::string>
format_integer(std::int64_t value, locale_format_options options = {});

// Parses the format produced by the functions above (with the same options),
// plus plain numbers without currency or percent decoration. Returns
// `invalid_argument` for anything the locale grammar does not allow.
[[nodiscard]] CRPC_API result<double>
parse_number(std::string_view text, locale_format_options options = {});

} // namespace i18n
} // namespace crpc

#endif // CRPC_I18N_FORMAT_HPP
