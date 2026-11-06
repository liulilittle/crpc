// CRPC -- tests for crpc/i18n number, currency and percentage formatting.
//
// Every expected string below is the CLDR 49 output for the cited pattern
// (cldr-numbers-full/main/<locale>/numbers.json; the patterns are quoted in
// include/crpc/i18n/format.hpp). The exact characters matter: de/fr use
// U+00A0 NO-BREAK SPACE before the currency symbol and the percent sign, and
// fr-FR groups digits with U+202F NARROW NO-BREAK SPACE.
#include <cmath>
#include <cstdint>
#include <string>

#include <crpc/i18n/format.hpp>

#include "test_framework.hpp"

namespace {

using crpc::i18n::locale_format_options;

constexpr const char* kNbsp = "\xC2\xA0";       // U+00A0
constexpr const char* kNnbsp = "\xE2\x80\xAF";  // U+202F

locale_format_options plain(const char* locale, int digits = 2) {
    locale_format_options options;
    options.locale = locale;
    options.decimal_digits = digits;
    options.currency = "";  // no currency symbol
    return options;
}

std::string text_of(const crpc::result<std::string>& value) {
    if (!value.ok()) {
        CRPC_CHECK(false);
        return {};
    }
    return value.value();
}

double number_of(const crpc::result<double>& value) {
    if (!value.ok()) {
        CRPC_CHECK(false);
        return 0.0;
    }
    return value.value();
}

// Formats `value` as a plain decimal and parses it back. Parsing recovers the
// rounded value: formatting keeps `digits` fraction digits, so the tolerance is
// half of the last digit plus a small epsilon for the binary conversion.
void round_trips(double value, const locale_format_options& options) {
    const auto formatted = crpc::i18n::format_number(value, options);
    CRPC_REQUIRE(formatted.ok());
    const auto parsed = crpc::i18n::parse_number(formatted.value(), options);
    if (!parsed.ok()) {
        CRPC_CHECK(false);
        return;
    }
    const double tolerance =
        0.5000001 * std::pow(10.0, -static_cast<double>(options.decimal_digits));
    if (std::fabs(parsed.value() - value) > tolerance) {
        CRPC_CHECK(false);
    }
}

} // namespace

// --- Numbers -----------------------------------------------------------------

CRPC_TEST(format_number_uses_cldr_separators_per_locale) {
    // 1234567.891 rounds to 2 fraction digits: ".89" / ",89".
    CRPC_CHECK_EQ(std::string("1,234,567.89"), text_of(crpc::i18n::format_number(
                                                   1234567.891, plain("en-US"))));
    // de-DE: decimal ",", group ".".
    CRPC_CHECK_EQ(std::string("1.234.567,89"), text_of(crpc::i18n::format_number(
                                                   1234567.891, plain("de-DE"))));
    // zh-CN: same separators as en-US.
    CRPC_CHECK_EQ(std::string("1,234,567.89"), text_of(crpc::i18n::format_number(
                                                   1234567.891, plain("zh-CN"))));
    // fr-FR: decimal ",", group U+202F NARROW NO-BREAK SPACE.
    CRPC_CHECK_EQ(std::string("1") + kNnbsp + "234" + kNnbsp + "567,89",
                  text_of(crpc::i18n::format_number(1234567.891, plain("fr-FR"))));

    // Language-only tags resolve to the same rule set.
    CRPC_CHECK_EQ(std::string("1.234.567,89"),
                  text_of(crpc::i18n::format_number(1234567.891, plain("de"))));
}

CRPC_TEST(format_number_honours_grouping_and_digit_options) {
    locale_format_options no_grouping = plain("en-US");
    no_grouping.use_grouping = false;
    CRPC_CHECK_EQ(std::string("1234567.89"),
                  text_of(crpc::i18n::format_number(1234567.891, no_grouping)));

    CRPC_CHECK_EQ(std::string("1,235"),
                  text_of(crpc::i18n::format_number(1234.6, plain("en-US", 0))));
    CRPC_CHECK_EQ(std::string("0.50"),
                  text_of(crpc::i18n::format_number(0.5, plain("en-US", 2))));
    CRPC_CHECK_EQ(std::string("42"),
                  text_of(crpc::i18n::format_number(42.0, plain("en-US", 0))));

    // Negative values keep the minus sign (CLDR minusSign is "-" everywhere).
    CRPC_CHECK_EQ(std::string("-1,234.50"),
                  text_of(crpc::i18n::format_number(-1234.5, plain("en-US"))));
    CRPC_CHECK_EQ(std::string("-1.234,50"),
                  text_of(crpc::i18n::format_number(-1234.5, plain("de-DE"))));
}

CRPC_TEST(format_currency_places_the_symbol_per_cldr_pattern) {
    locale_format_options money = plain("en-US");
    money.use_currency = true;
    money.currency = "USD";
    // en-US pattern "¤#,##0.00": symbol first, no space.
    CRPC_CHECK_EQ(std::string("$1,234.50"),
                  text_of(crpc::i18n::format_number(1234.5, money)));
    // CNY in en-US is "CN¥".
    money.currency = "CNY";
    CRPC_CHECK_EQ(std::string("CN\xC2\xA5" "1,234.50"),
                  text_of(crpc::i18n::format_number(1234.5, money)));

    locale_format_options de = plain("de-DE");
    de.use_currency = true;
    de.currency = "EUR";
    // de-DE pattern "#,##0.00<NBSP>¤": symbol after a no-break space.
    CRPC_CHECK_EQ(std::string("1.234,50") + kNbsp + "\xE2\x82\xAC",
                  text_of(crpc::i18n::format_number(1234.5, de)));

    locale_format_options zh = plain("zh-CN");
    zh.use_currency = true;
    zh.currency = "USD";
    CRPC_CHECK_EQ(std::string("US$1,234.50"),
                  text_of(crpc::i18n::format_number(1234.5, zh)));
    zh.currency = "CNY";
    CRPC_CHECK_EQ(std::string("\xC2\xA5" "1,234.50"),
                  text_of(crpc::i18n::format_number(1234.5, zh)));

    locale_format_options fr = plain("fr-FR");
    fr.use_currency = true;
    fr.currency = "USD";
    // fr-FR: narrow no-break space between groups, NBSP before "$US".
    CRPC_CHECK_EQ(std::string("1") + kNnbsp + "234,50" + kNbsp + "$US",
                  text_of(crpc::i18n::format_number(1234.5, fr)));

    // Negative amounts: the sign precedes the whole decoration.
    fr.currency = "EUR";
    CRPC_CHECK_EQ(std::string("-1") + kNnbsp + "234,50" + kNbsp + "\xE2\x82\xAC",
                  text_of(crpc::i18n::format_number(-1234.5, fr)));

    // An unknown code renders as the code itself (ICU behaviour).
    locale_format_options unknown = plain("en-US");
    unknown.use_currency = true;
    unknown.currency = "XYZ";
    CRPC_CHECK_EQ(std::string("XYZ1,234.50"),
                  text_of(crpc::i18n::format_number(1234.5, unknown)));
}

CRPC_TEST(format_percent_scales_by_100_and_appends_the_sign) {
    locale_format_options pct = plain("en-US");
    pct.percent = true;
    CRPC_CHECK_EQ(std::string("12.34%"),
                  text_of(crpc::i18n::format_number(0.1234, pct)));

    locale_format_options de = plain("de-DE");
    de.percent = true;
    // de-DE percent pattern "#,##0<NBSP>%".
    CRPC_CHECK_EQ(std::string("12,34") + kNbsp + "%",
                  text_of(crpc::i18n::format_number(0.1234, de)));

    locale_format_options fr = plain("fr-FR");
    fr.percent = true;
    CRPC_CHECK_EQ(std::string("12,34") + kNbsp + "%",
                  text_of(crpc::i18n::format_number(0.1234, fr)));

    locale_format_options zh = plain("zh-CN");
    zh.percent = true;
    CRPC_CHECK_EQ(std::string("12.34%"),
                  text_of(crpc::i18n::format_number(0.1234, zh)));

    // Percent wins over currency: both set is a percent format.
    pct.use_currency = true;
    CRPC_CHECK_EQ(std::string("12.34%"),
                  text_of(crpc::i18n::format_number(0.1234, pct)));
}

CRPC_TEST(format_integer_groups_without_a_fraction) {
    CRPC_CHECK_EQ(std::string("1,234,567"),
                  text_of(crpc::i18n::format_integer(1234567, plain("en-US", 0))));
    CRPC_CHECK_EQ(std::string("1.234.567"),
                  text_of(crpc::i18n::format_integer(1234567, plain("de-DE", 0))));
    CRPC_CHECK_EQ(std::string("1") + kNnbsp + "234" + kNnbsp + "567",
                  text_of(crpc::i18n::format_integer(1234567, plain("fr-FR", 0))));
    CRPC_CHECK_EQ(std::string("0"), text_of(crpc::i18n::format_integer(0, plain("en-US"))));
    CRPC_CHECK_EQ(std::string("-9,223,372,036,854,775,808"),
                  text_of(crpc::i18n::format_integer(INT64_MIN, plain("en-US"))));
    CRPC_CHECK_EQ(std::string("-9.223.372.036.854.775.808"),
                  text_of(crpc::i18n::format_integer(INT64_MIN, plain("de-DE"))));

    locale_format_options money = plain("en-US", 0);
    money.use_currency = true;
    money.currency = "USD";
    CRPC_CHECK_EQ(std::string("$1,234,567"),
                  text_of(crpc::i18n::format_integer(1234567, money)));
}

// --- Parsing -----------------------------------------------------------------

CRPC_TEST(parse_number_accepts_the_locale_grammar) {
    // Each locale accepts its own separators.
    CRPC_CHECK_EQ(1234.56, number_of(crpc::i18n::parse_number("1,234.56", plain("en-US"))));
    CRPC_CHECK_EQ(1234.56, number_of(crpc::i18n::parse_number("1.234,56", plain("de-DE"))));
    CRPC_CHECK_EQ(1234.56, number_of(crpc::i18n::parse_number("1,234.56", plain("zh-CN"))));
    CRPC_CHECK_EQ(1234.56,
                  number_of(crpc::i18n::parse_number(std::string("1") + kNnbsp + "234,56",
                                                     plain("fr-FR"))));
    // Ungrouped numbers parse too.
    CRPC_CHECK_EQ(1234.56, number_of(crpc::i18n::parse_number("1234.56", plain("en-US"))));
    // "1.234" is 1234 in de-DE (grouping) and 1.234 in en-US (decimal).
    CRPC_CHECK_EQ(1234.0, number_of(crpc::i18n::parse_number("1.234", plain("de-DE"))));
    CRPC_CHECK_EQ(1.234, number_of(crpc::i18n::parse_number("1.234", plain("en-US"))));
    // Signs and a leading zero fraction.
    CRPC_CHECK_EQ(-0.5, number_of(crpc::i18n::parse_number("-0,5", plain("de-DE"))));
}

CRPC_TEST(parse_number_strips_currency_and_percent_decoration) {
    locale_format_options money = plain("en-US");
    money.use_currency = true;
    money.currency = "USD";
    CRPC_CHECK_EQ(1234.5, number_of(crpc::i18n::parse_number("$1,234.50", money)));

    locale_format_options de = plain("de-DE");
    de.use_currency = true;
    de.currency = "EUR";
    CRPC_CHECK_EQ(1234.5,
                  number_of(crpc::i18n::parse_number(std::string("1.234,50") + kNbsp +
                                                         "\xE2\x82\xAC",
                                                     de)));

    locale_format_options pct = plain("en-US");
    pct.percent = true;
    CRPC_CHECK_EQ(0.1234, number_of(crpc::i18n::parse_number("12.34%", pct)));

    locale_format_options de_pct = plain("de-DE");
    de_pct.percent = true;
    CRPC_CHECK_EQ(0.1234,
                  number_of(crpc::i18n::parse_number(std::string("12,34") + kNbsp + "%",
                                                     de_pct)));

    // A percent sign in the text divides by 100 even without options.percent.
    CRPC_CHECK_EQ(0.5, number_of(crpc::i18n::parse_number("50%", plain("en-US"))));
}

CRPC_TEST(format_parse_round_trips_every_locale) {
    for (const char* locale : {"en-US", "de-DE", "zh-CN", "fr-FR"}) {
        round_trips(0.0, plain(locale));
        round_trips(1.0, plain(locale));
        round_trips(1234.5, plain(locale));
        round_trips(1234567.891, plain(locale));
        round_trips(-987654.321, plain(locale));

        locale_format_options money = plain(locale);
        money.use_currency = true;
        money.currency = "USD";
        round_trips(1234.5, money);
        round_trips(-1234.5, money);

        locale_format_options pct = plain(locale);
        pct.percent = true;
        round_trips(0.125, pct);
        round_trips(-0.5, pct);
    }
}

CRPC_TEST(parse_number_rejects_input_the_locale_does_not_use) {
    // Empty, garbage and truncated input.
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  crpc::i18n::parse_number("", plain("en-US")).code());
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  crpc::i18n::parse_number("abc", plain("en-US")).code());
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  crpc::i18n::parse_number("12.3.4", plain("en-US")).code());
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  crpc::i18n::parse_number("1,,234", plain("en-US")).code());
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  crpc::i18n::parse_number(",123", plain("en-US")).code());
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  crpc::i18n::parse_number("1,234,", plain("en-US")).code());
    // A comma is never a decimal point in en-US.
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  crpc::i18n::parse_number("1,5", plain("en-US")).code());
    // ... and a period is never a decimal point in de-DE (it groups).
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  crpc::i18n::parse_number("1.5", plain("de-DE")).code());
}

CRPC_TEST(format_rejects_invalid_options_and_values) {
    locale_format_options bad_locale = plain("xx-YY");
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  crpc::i18n::format_number(1.0, bad_locale).code());
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  crpc::i18n::parse_number("1", bad_locale).code());
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  crpc::i18n::format_integer(1, bad_locale).code());

    locale_format_options bad_digits = plain("en-US");
    bad_digits.decimal_digits = -1;
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  crpc::i18n::format_number(1.0, bad_digits).code());
    bad_digits.decimal_digits = 16;
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  crpc::i18n::format_number(1.0, bad_digits).code());

    locale_format_options bad_currency = plain("en-US");
    bad_currency.use_currency = true;
    bad_currency.currency = "US";
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  crpc::i18n::format_number(1.0, bad_currency).code());

    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  crpc::i18n::format_number(std::nan(""), plain("en-US")).code());
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  crpc::i18n::format_number(HUGE_VAL, plain("en-US")).code());
}

CRPC_TEST_MAIN()