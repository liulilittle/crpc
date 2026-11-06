// CRPC -- locale-aware number formatting. See include/crpc/i18n/format.hpp.
//
// Every rule below is a transcription of one CLDR 49 value; the header names
// the exact JSON keys. The table is kept literal (instead of interpreting the
// CLDR pattern language) because four locales are all the API promises, and a
// literal table cannot quietly mis-handle a pattern construct nobody tested.
#include <crpc/i18n/format.hpp>

#include <charconv>
#include <cmath>
#include <cstddef>
#include <system_error>

namespace crpc {
namespace i18n {

namespace {

constexpr std::string_view nbsp = "\xC2\xA0";             // U+00A0
constexpr std::string_view narrow_nbsp = "\xE2\x80\xAF";  // U+202F

struct locale_rules {
    const char* language;
    const char* decimal_separator;
    const char* group_separator;
    // True when the currency symbol precedes the amount ("$1,234.50"); false
    // when it follows it after a no-break space ("1.234,50 €").
    bool currency_before;
    // The separator inserted before a percent sign ("" for en/zh, NBSP for
    // de/fr, per the CLDR percent pattern "#,##0 %").
    const char* percent_space;
    int group_size;
};

constexpr locale_rules kLocales[] = {
    // en-US: decimal ".", group ",", currency "¤#,##0.00", percent "#,##0%"
    {"en", ".", ",", true, "", 3},
    // de-DE: decimal ",", group ".", currency "#,##0.00<NBSP>¤",
    //        percent "#,##0<NBSP>%"
    {"de", ",", ".", false, "\xC2\xA0", 3},
    // zh-CN: decimal ".", group ",", currency "¤#,##0.00", percent "#,##0%"
    {"zh", ".", ",", true, "", 3},
    // fr-FR: decimal ",", group U+202F, currency "#,##0.00<NBSP>¤",
    //        percent "#,##0<NBSP>%"
    {"fr", ",", "\xE2\x80\xAF", false, "\xC2\xA0", 3},
};

constexpr std::size_t kLocaleCount = sizeof(kLocales) / sizeof(kLocales[0]);

struct currency_symbol {
    const char* language;
    const char* code;
    const char* symbol;
};

constexpr currency_symbol kCurrencySymbols[] = {
    // CLDR currencies.json "symbol" for the four locales. A missing pair
    // renders the ISO code itself (ICU behaves the same way).
    {"en", "USD", "$"},          {"en", "EUR", "\xE2\x82\xAC"},
    {"en", "CNY", "CN\xC2\xA5"}, {"en", "GBP", "\xC2\xA3"},
    {"en", "JPY", "\xC2\xA5"},   {"en", "RUB", "RUB"},
    {"de", "USD", "$"},          {"de", "EUR", "\xE2\x82\xAC"},
    {"de", "CNY", "CN\xC2\xA5"}, {"de", "GBP", "\xC2\xA3"},
    {"de", "JPY", "\xC2\xA5"},   {"de", "RUB", "RUB"},
    {"zh", "USD", "US$"},        {"zh", "EUR", "\xE2\x82\xAC"},
    {"zh", "CNY", "\xC2\xA5"},   {"zh", "GBP", "\xC2\xA3"},
    {"zh", "JPY", "JP\xC2\xA5"}, {"zh", "RUB", "RUB"},
    {"fr", "USD", "$US"},        {"fr", "EUR", "\xE2\x82\xAC"},
    {"fr", "CNY", "CNY"},        {"fr", "GBP", "\xC2\xA3GB"},
    {"fr", "JPY", "JPY"},        {"fr", "RUB", "RUB"},
};

bool ascii_iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char left = a[i];
        char right = b[i];
        if (left >= 'A' && left <= 'Z') {
            left = static_cast<char>(left - 'A' + 'a');
        }
        if (right >= 'A' && right <= 'Z') {
            right = static_cast<char>(right - 'A' + 'a');
        }
        if (left != right) {
            return false;
        }
    }
    return true;
}

rpc_error invalid(std::string message) {
    return rpc_error{rpc_code::invalid_argument, std::move(message)};
}

const locale_rules* find_locale(std::string_view locale) noexcept {
    const std::size_t separator = locale.find_first_of("-_");
    const std::string_view language = locale.substr(0, separator);
    if (language.empty()) {
        return nullptr;
    }
    for (const locale_rules& rule : kLocales) {
        if (ascii_iequals(language, rule.language)) {
            return &rule;
        }
    }
    return nullptr;
}

std::string_view currency_symbol_of(std::string_view language,
                                    std::string_view code,
                                    std::string& storage) {
    for (const auto& entry : kCurrencySymbols) {
        if (ascii_iequals(language, entry.language) &&
            ascii_iequals(code, entry.code)) {
            return entry.symbol;
        }
    }
    // Unknown code: echo the upper-case spelling so the output still names the
    // currency.
    storage.clear();
    for (const char ch : code) {
        storage.push_back(ch >= 'a' && ch <= 'z'
                              ? static_cast<char>(ch - 'a' + 'A')
                              : ch);
    }
    return storage;
}

void append_grouped(std::string& out, std::string_view digits,
                    std::string_view separator, int group_size) {
    if (group_size <= 0 ||
        digits.size() <= static_cast<std::size_t>(group_size)) {
        out.append(digits.data(), digits.size());
        return;
    }
    const std::size_t first_group =
        digits.size() % static_cast<std::size_t>(group_size);
    std::size_t position = 0;
    if (first_group != 0) {
        out.append(digits.data(), first_group);
        position = first_group;
    }
    while (position < digits.size()) {
        if (position != 0) {
            out.append(separator.data(), separator.size());
        }
        out.append(digits.data() + position,
                   static_cast<std::size_t>(group_size));
        position += static_cast<std::size_t>(group_size);
    }
}

// Turns a finite double into fixed-point digits with exactly `digits` fraction
// digits. `std::to_chars` is locale-independent and correctly rounded, which
// sprintf("%f") cannot promise on every CRT.
bool split_fixed(double value, int digits, bool& negative, std::string& integer,
                 std::string& fraction) {
    negative = std::signbit(value);
    const double magnitude = std::fabs(value);

    char buffer[512];
    const auto outcome = std::to_chars(buffer, buffer + sizeof(buffer), magnitude,
                                       std::chars_format::fixed, digits);
    if (outcome.ec != std::errc()) {
        return false;
    }
    const std::string_view text(buffer,
                                static_cast<std::size_t>(outcome.ptr - buffer));
    const std::size_t point = text.find('.');
    if (point == std::string_view::npos) {
        integer.assign(text.data(), text.size());
        fraction.clear();
    } else {
        integer.assign(text.data(), point);
        fraction.assign(text.data() + point + 1, text.size() - point - 1);
    }
    return true;
}

// Formats the magnitude body (grouped integer part + optional fraction).
std::string format_body(double value, const locale_rules& rules,
                        const locale_format_options& options, bool& negative) {
    std::string integer;
    std::string fraction;
    const int digits = options.decimal_digits;
    if (!split_fixed(value, digits, negative, integer, fraction)) {
        return {};
    }
    std::string body;
    if (options.use_grouping) {
        append_grouped(body, integer, rules.group_separator, rules.group_size);
    } else {
        body = integer;
    }
    if (digits > 0) {
        body.append(rules.decimal_separator);
        body.append(fraction);
    }
    return body;
}

// Applies the currency or percent decoration that CLDR's pattern describes.
// `body` already carries the grouped digits without a sign.
std::string decorate(std::string body, bool negative, const locale_rules& rules,
                     const locale_format_options& options,
                     std::string_view symbol) {
    std::string out;
    if (negative) {
        out.push_back('-');
    }
    if (options.percent) {
        out.append(body);
        out.append(rules.percent_space);
        out.push_back('%');
        return out;
    }
    if (options.use_currency) {
        if (rules.currency_before) {
            out.append(symbol.data(), symbol.size());
            out.append(body);
        } else {
            out.append(body);
            out.append(nbsp.data(), nbsp.size());
            out.append(symbol.data(), symbol.size());
        }
        return out;
    }
    out.append(body);
    return out;
}

// --- Parsing -----------------------------------------------------------------

bool starts_with(std::string_view text, std::string_view prefix) noexcept {
    return text.size() >= prefix.size() &&
           text.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

void trim_ascii_space(std::string_view& text) noexcept {
    auto is_space = [](char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    };
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
}

void trim_trailing_space_like(std::string_view& text) noexcept {
    // Sprinkles of typography: strip the no-break spaces a CLDR pattern uses
    // between the number and its currency/percent sign.
    while (!text.empty()) {
        if (ends_with(text, nbsp)) {
            text.remove_suffix(nbsp.size());
        } else if (ends_with(text, narrow_nbsp)) {
            text.remove_suffix(narrow_nbsp.size());
        } else if (text.back() == ' ') {
            text.remove_suffix(1);
        } else {
            break;
        }
    }
}

bool strip_currency(std::string_view& text, std::string_view symbol) noexcept {
    if (symbol.empty()) {
        return false;
    }
    if (starts_with(text, symbol)) {
        text.remove_prefix(symbol.size());
        if (starts_with(text, nbsp)) {
            text.remove_prefix(nbsp.size());
        }
        return true;
    }
    if (ends_with(text, symbol)) {
        text.remove_suffix(symbol.size());
        trim_trailing_space_like(text);
        return true;
    }
    return false;
}

bool is_group_space(std::string_view text, std::size_t position) noexcept {
    return position + narrow_nbsp.size() <= text.size() &&
           text.compare(position, narrow_nbsp.size(), narrow_nbsp) == 0;
}

} // namespace

result<std::string> format_number(double value, locale_format_options options) {
    if (std::isnan(value) || std::isinf(value)) {
        return invalid("cannot format a non-finite number");
    }
    if (options.decimal_digits < 0 || options.decimal_digits > 15) {
        return invalid("decimal_digits must be between 0 and 15");
    }
    const locale_rules* rules = find_locale(options.locale);
    if (rules == nullptr) {
        return invalid("unsupported locale \"" + options.locale +
                       "\": en, de, zh and fr are implemented");
    }
    // In percent mode the currency field is ignored, so its length is not
    // validated there.
    if (options.use_currency && !options.percent && options.currency.size() != 3) {
        return invalid("currency must be a three-letter ISO 4217 code, got \"" +
                       options.currency + "\"");
    }

    std::string storage;
    const std::string_view symbol =
        currency_symbol_of(rules->language, options.currency, storage);

    // Percent scales the value before the pattern's digits are produced.
    if (options.percent && std::isinf(value * 100.0)) {
        return invalid("percentage of this value overflows a double");
    }
    const double scaled = options.percent ? value * 100.0 : value;
    bool negative = false;
    std::string body = format_body(scaled, *rules, options, negative);
    if (body.empty()) {
        return invalid("number is too large to format as fixed point");
    }
    return decorate(std::move(body), negative, *rules, options, symbol);
}

result<std::string> format_integer(std::int64_t value,
                                   locale_format_options options) {
    const locale_rules* rules = find_locale(options.locale);
    if (rules == nullptr) {
        return invalid("unsupported locale \"" + options.locale +
                       "\": en, de, zh and fr are implemented");
    }
    if (options.use_currency && options.currency.size() != 3) {
        return invalid("currency must be a three-letter ISO 4217 code, got \"" +
                       options.currency + "\"");
    }

    bool negative = value < 0;
    std::uint64_t magnitude =
        negative ? static_cast<std::uint64_t>(-(value + 1)) + 1
                 : static_cast<std::uint64_t>(value);
    // A percentage of an integer is still a percentage: scale by 100 with an
    // explicit overflow check instead of wrapping.
    if (options.percent) {
        if (magnitude > UINT64_MAX / 100u) {
            return invalid("percentage of this integer overflows 64 bits");
        }
        magnitude *= 100u;
    }

    char buffer[24];
    const auto outcome =
        std::to_chars(buffer, buffer + sizeof(buffer), magnitude);
    const std::string_view digits(buffer,
                                  static_cast<std::size_t>(outcome.ptr - buffer));

    std::string body;
    if (options.use_grouping) {
        append_grouped(body, digits, rules->group_separator, rules->group_size);
    } else {
        body.assign(digits.data(), digits.size());
    }

    std::string storage;
    const std::string_view symbol =
        currency_symbol_of(rules->language, options.currency, storage);
    // `format_integer` deliberately carries no fraction digits, so the
    // currency pattern's ".00" is not reproduced; the symbol position is.
    if (options.percent) {
        std::string out;
        if (negative) {
            out.push_back('-');
        }
        out.append(body);
        out.append(rules->percent_space);
        out.push_back('%');
        return out;
    }
    if (options.use_currency) {
        std::string out;
        if (negative) {
            out.push_back('-');
        }
        if (rules->currency_before) {
            out.append(symbol.data(), symbol.size());
            out.append(body);
        } else {
            out.append(body);
            out.append(nbsp.data(), nbsp.size());
            out.append(symbol.data(), symbol.size());
        }
        return out;
    }
    if (negative) {
        return "-" + body;
    }
    return body;
}

result<double> parse_number(std::string_view text, locale_format_options options) {
    const locale_rules* rules = find_locale(options.locale);
    if (rules == nullptr) {
        return invalid("unsupported locale \"" + options.locale +
                       "\": en, de, zh and fr are implemented");
    }

    trim_ascii_space(text);
    if (text.empty()) {
        return invalid("cannot parse an empty number");
    }

    bool negative = false;
    if (text.front() == '-') {
        negative = true;
        text.remove_prefix(1);
    } else if (text.front() == '+') {
        text.remove_prefix(1);
    }

    // A percent sign scales the result by 1/100. Its presence in the text is
    // what decides this, so parsing "50%" and "0.5" both give 0.5 with
    // options.percent = true.
    bool percent_seen = false;
    if (!text.empty() && text.back() == '%') {
        text.remove_suffix(1);
        trim_trailing_space_like(text);
        percent_seen = true;
    }

    std::string storage;
    std::string_view symbol =
        currency_symbol_of(rules->language, options.currency, storage);
    strip_currency(text, symbol);
    // Also accept the bare ISO code, which is what an unknown currency
    // renders as.
    if (options.currency.size() == 3) {
        strip_currency(text, options.currency);
    }

    if (text.empty()) {
        return invalid("cannot parse a number without digits");
    }

    const std::string_view decimal(rules->decimal_separator);
    const std::string_view group(rules->group_separator);

    std::string normalized;
    normalized.reserve(text.size());
    bool point_seen = false;
    bool digit_seen = false;
    bool exponent_seen = false;
    // CLDR grouping: the first group carries 1..3 digits and every following
    // group exactly 3. "1,234" parses; "1,5", "1,2345" and "1,,234" do not.
    // Once a separator has been seen, the group that just closed must have
    // been exactly 3 digits and the trailing group must be too; with no
    // separator at all any digit count is a valid integer.
    std::size_t run = 0;
    bool separator_seen = false;
    auto close_group = [&](bool at_separator) -> bool {
        if (run == 0) {
            return false; // empty group ("1,,234", ",123")
        }
        if (!separator_seen) {
            return at_separator ? run <= 3u : true;
        }
        return run == 3u;
    };

    for (std::size_t i = 0; i < text.size();) {
        const char ch = text[i];
        if (ch >= '0' && ch <= '9') {
            normalized.push_back(ch);
            digit_seen = true;
            ++run;
            ++i;
            continue;
        }
        if (!point_seen && !exponent_seen &&
            text.compare(i, decimal.size(), decimal) == 0) {
            if (!close_group(false)) {
                return invalid("misplaced group separator in \"" + std::string(text) + "\"");
            }
            run = 0;
            normalized.push_back('.');
            point_seen = true;
            i += decimal.size();
            continue;
        }
        if (!point_seen && !exponent_seen && options.use_grouping &&
            (text.compare(i, group.size(), group) == 0 ||
             (group == narrow_nbsp && is_group_space(text, i)))) {
            if (!close_group(true)) {
                return invalid("misplaced group separator in \"" +
                               std::string(text) + "\"");
            }
            separator_seen = true;
            run = 0;
            const std::size_t width =
                text.compare(i, group.size(), group) == 0 ? group.size()
                                                          : narrow_nbsp.size();
            i += width;
            continue;
        }
        if (!exponent_seen && (ch == 'e' || ch == 'E')) {
            normalized.push_back(ch);
            exponent_seen = true;
            ++i;
            if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
                normalized.push_back(text[i]);
                ++i;
            }
            continue;
        }
        return invalid("unexpected character in \"" + std::string(text) + "\"");
    }
    // The group that closed the integer part must satisfy the same rule (the
    // exponent digits after 'e' are not grouped and carry no constraint).
    if (!exponent_seen && !point_seen && !close_group(false)) {
        return invalid("misplaced group separator in \"" + std::string(text) + "\"");
    }

    if (!digit_seen) {
        return invalid("no digits in \"" + std::string(text) + "\"");
    }

    double value = 0.0;
    const auto outcome =
        std::from_chars(normalized.data(), normalized.data() + normalized.size(),
                        value);
    if (outcome.ec == std::errc::result_out_of_range) {
        return invalid("number is out of range");
    }
    if (outcome.ec != std::errc() ||
        outcome.ptr != normalized.data() + normalized.size()) {
        return invalid("not a complete decimal literal");
    }
    if (percent_seen) {
        value /= 100.0;
    }
    return negative ? -value : value;
}

} // namespace i18n
} // namespace crpc