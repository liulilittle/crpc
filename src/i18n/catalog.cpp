// CRPC -- message catalog. See include/crpc/i18n/catalog.hpp.
//
// The plural rules are not hand-written: src/i18n/plural_data.inc is generated
// from CLDR 49 supplemental/plurals.json and the generator validates every
// rule against the @integer samples CLDR publishes with it before writing.
#include <crpc/i18n/catalog.hpp>

#include <cctype>
#include <cstddef>

namespace crpc {
namespace i18n {

namespace {

// --- Generated CLDR plural tables -------------------------------------------
//
// plural_range  one inclusive [first, last] value interval
// plural_term   one relation: operand (mod) operation any-of-ranges
// plural_group  an AND of consecutive terms
// plural_rule   an OR of consecutive groups for one category
// plural_locale the rule list of one CLDR locale (binary-searched by name)
struct plural_range {
    std::uint32_t first;
    std::uint32_t last;
};

struct plural_term {
    std::uint8_t operand; // 0 n, 1 i, 2 v, 3 w, 4 f, 5 t, 6 e, 7 c
    std::uint8_t operation; // 0 '=', 1 '!='
    std::uint32_t mod;      // 0 = no modulo (CLDR uses up to n % 1000000)
    std::uint16_t first_range;
    std::uint16_t range_count;
};

struct plural_group {
    std::uint16_t first_term;
    std::uint16_t term_count;
};

struct plural_rule {
    std::uint8_t category; // index into plural_category
    std::uint16_t first_group;
    std::uint16_t group_count;
};

struct plural_locale {
    const char* name;
    std::uint16_t first_rule;
    std::uint16_t rule_count;
};

#include "plural_data.inc"

template <typename T, std::size_t N>
constexpr std::size_t count_of(T (&)[N]) noexcept {
    return N;
}

bool ascii_equal_ignore_case(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const unsigned char left = static_cast<unsigned char>(a[i]);
        const unsigned char right = static_cast<unsigned char>(b[i]);
        if (left == right) {
            continue;
        }
        if (std::tolower(left) != std::tolower(right)) {
            return false;
        }
    }
    return true;
}

rpc_error invalid(std::string message) {
    return rpc_error{rpc_code::invalid_argument, std::move(message)};
}

std::string_view trim(std::string_view text) noexcept {
    auto is_space = [](char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' ||
               ch == '\f' || ch == '\v';
    };
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && is_space(text[begin])) {
        ++begin;
    }
    while (end > begin && is_space(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// Lower-cases and maps '_' to '-'. Only the language and region matter for
// catalog lookup; script and variant subtags are preserved in the name.
std::string normalize_locale(std::string_view locale) {
    std::string out;
    out.reserve(locale.size());
    for (const char ch : locale) {
        if (ch == '_') {
            out.push_back('-');
        } else {
            out.push_back(
                static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    return out;
}

std::string language_of(std::string_view locale) {
    const std::size_t dash = locale.find('-');
    return std::string(locale.substr(0, dash));
}

// Expands \n, \t and \\ ; a backslash before any other character is kept
// verbatim so a Windows path in a message stays readable.
std::string unescape_value(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 >= text.size()) {
            out.push_back(text[i]);
            continue;
        }
        switch (text[i + 1]) {
        case 'n':
            out.push_back('\n');
            ++i;
            break;
        case 't':
            out.push_back('\t');
            ++i;
            break;
        case '\\':
            out.push_back('\\');
            ++i;
            break;
        default:
            out.push_back('\\');
            break;
        }
    }
    return out;
}

bool is_digits(std::string_view text) noexcept {
    if (text.empty()) {
        return false;
    }
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    return true;
}

// Integer operands: only n and i are non-zero, every other CLDR operand
// (v, w, f, t, e, c) describes the fractional/compact part of a number, which
// a std::int64_t count never has.
std::uint64_t plural_operand_value(std::uint8_t operand,
                                   std::uint64_t magnitude) noexcept {
    return operand <= 1 ? magnitude : 0;
}

bool plural_group_matches(const plural_group& group,
                          std::uint64_t magnitude) noexcept {
    for (std::uint16_t i = 0; i < group.term_count; ++i) {
        const plural_term& term = kPluralTerms[group.first_term + i];
        std::uint64_t value = plural_operand_value(term.operand, magnitude);
        if (term.mod != 0) {
            value %= term.mod;
        }
        bool within = false;
        for (std::uint16_t r = 0; r < term.range_count; ++r) {
            const plural_range& range = kPluralRanges[term.first_range + r];
            if (value >= range.first && value <= range.last) {
                within = true;
                break;
            }
        }
        if ((term.operation == 0) != within) {
            return false;
        }
    }
    return true;
}

const plural_locale* find_plural_locale(std::string_view locale) noexcept {
    std::size_t lo = 0;
    std::size_t hi = count_of(kPluralLocales);
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const int order = locale.compare(kPluralLocales[mid].name);
        if (order == 0) {
            return &kPluralLocales[mid];
        }
        if (order < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return nullptr;
}

} // namespace

const char* to_string(plural_category category) noexcept {
    switch (category) {
    case plural_category::zero:
        return "zero";
    case plural_category::one:
        return "one";
    case plural_category::two:
        return "two";
    case plural_category::few:
        return "few";
    case plural_category::many:
        return "many";
    case plural_category::other:
        return "other";
    }
    return "other";
}

plural_category plural_category_for(std::string_view locale,
                                    std::int64_t count) noexcept {
    // CLDR evaluates the absolute value, so -1 is in the "one" set of English
    // just like 1. INT64_MIN has no positive counterpart, hence the unsigned
    // magnitude.
    std::uint64_t magnitude = 0;
    if (count < 0) {
        magnitude = static_cast<std::uint64_t>(-(count + 1)) + 1;
    } else {
        magnitude = static_cast<std::uint64_t>(count);
    }

    const std::string normalized = normalize_locale(locale);
    const plural_locale* entry = find_plural_locale(normalized);
    if (entry == nullptr) {
        const std::string language = language_of(normalized);
        entry = find_plural_locale(language);
    }
    if (entry == nullptr) {
        return plural_category::other;
    }

    // The generator preserves CLDR evaluation order (zero, one, two, few,
    // many, other), so the first matching rule is the answer.
    for (std::uint16_t i = 0; i < entry->rule_count; ++i) {
        const plural_rule& rule = kPluralRules[entry->first_rule + i];
        for (std::uint16_t g = 0; g < rule.group_count; ++g) {
            if (plural_group_matches(kPluralGroups[rule.first_group + g],
                                     magnitude)) {
                return static_cast<plural_category>(rule.category);
            }
        }
    }
    return plural_category::other;
}

message_catalog::message_catalog(message_catalog_options options)
    : options_(std::move(options)) {
    current_ = normalize_locale(options_.default_locale);
}

result<message_catalog> message_catalog::load(std::string_view text,
                                              std::string_view format,
                                              message_catalog_options options) {
    if (!ascii_equal_ignore_case(format, "ini")) {
        return invalid("unsupported catalog format \"" + std::string(format) +
                       "\": only \"ini\" is implemented");
    }

    message_catalog catalog(std::move(options));
    locale_block* current = nullptr;

    std::size_t line_number = 0;
    std::size_t position = 0;
    while (position <= text.size()) {
        const std::size_t newline = text.find('\n', position);
        const std::size_t line_end =
            newline == std::string_view::npos ? text.size() : newline;
        std::string_view line = text.substr(position, line_end - position);
        position = line_end + 1;
        ++line_number;

        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        const std::string_view content = trim(line);
        if (content.empty() || content.front() == ';' || content.front() == '#') {
            continue;
        }

        if (content.front() == '[') {
            if (content.size() < 3 || content.back() != ']') {
                return invalid("line " + std::to_string(line_number) +
                               ": malformed locale header");
            }
            std::string name =
                normalize_locale(trim(content.substr(1, content.size() - 2)));
            if (name.empty()) {
                return invalid("line " + std::to_string(line_number) +
                               ": empty locale name");
            }
            locale_block& block = catalog.locales_[name];
            block.name = name;
            current = &block;
            continue;
        }

        const std::size_t equals = content.find('=');
        if (equals == std::string_view::npos) {
            return invalid("line " + std::to_string(line_number) +
                           ": expected 'key = value'");
        }
        if (current == nullptr) {
            return invalid("line " + std::to_string(line_number) +
                           ": entry before the first [locale] header");
        }
        const std::string key(trim(content.substr(0, equals)));
        if (key.empty()) {
            return invalid("line " + std::to_string(line_number) + " in [" +
                           current->name + "]: empty key");
        }
        const std::string value =
            unescape_value(trim(content.substr(equals + 1)));
        if (!current->messages.emplace(key, value).second) {
            return invalid("line " + std::to_string(line_number) + " in [" +
                           current->name + "]: duplicate key \"" + key + "\"");
        }
    }

    return catalog;
}

bool message_catalog::set_locale(std::string_view locale) {
    const std::string normalized = normalize_locale(locale);
    const locale_block* block = find_block(normalized);
    if (block == nullptr) {
        block = find_block(language_of(normalized));
    }
    if (block == nullptr) {
        return false;
    }
    current_ = block->name;
    return true;
}

std::string_view message_catalog::locale() const noexcept { return current_; }

bool message_catalog::has_locale(std::string_view locale) const {
    const std::string normalized = normalize_locale(locale);
    return find_block(normalized) != nullptr ||
           find_block(language_of(normalized)) != nullptr;
}

std::size_t message_catalog::locale_count() const noexcept {
    return locales_.size();
}

const message_catalog::locale_block* message_catalog::find_block(
    std::string_view locale) const {
    if (locale.empty()) {
        return nullptr;
    }
    const auto it = locales_.find(locale);
    if (it == locales_.end()) {
        return nullptr;
    }
    return &it->second;
}

const std::string* message_catalog::find_message(const locale_block& block,
                                                 std::string_view key) const {
    const auto it = block.messages.find(key);
    if (it == block.messages.end()) {
        return nullptr;
    }
    return &it->second;
}

result<std::string> message_catalog::get(std::string_view key,
                                         std::string_view locale) const {
    std::string requested = locale.empty() ? current_ : normalize_locale(locale);
    std::string last = requested;

    if (const locale_block* block = find_block(requested)) {
        if (const std::string* message = find_message(*block, key)) {
            return *message;
        }
    } else {
        const std::string language = language_of(requested);
        if (const locale_block* language_block = find_block(language)) {
            last = language;
            if (const std::string* message = find_message(*language_block, key)) {
                return *message;
            }
        } else {
            last = language;
        }
    }

    if (options_.fallback_to_default) {
        last = normalize_locale(options_.default_locale);
        if (const locale_block* default_block = find_block(last)) {
            if (const std::string* message = find_message(*default_block, key)) {
                return *message;
            }
        }
    }

    return rpc_error{rpc_code::not_found, "message \"" + std::string(key) +
                                             "\" not found for locale \"" +
                                             last + "\""};
}

result<std::string> message_catalog::plural(std::string_view key,
                                            std::int64_t count,
                                            std::string_view locale) const {
    const std::string requested =
        locale.empty() ? current_ : normalize_locale(locale);
    const plural_category selected = plural_category_for(requested, count);
    const std::string suffix = "." + std::string(to_string(selected));

    // At every level the selected category is tried first and "other" second:
    // a language without a "many" form must still find its default wording.
    auto lookup = [&](const locale_block& block) -> const std::string* {
        if (const std::string* message =
                find_message(block, std::string(key) + suffix)) {
            return message;
        }
        return find_message(block, std::string(key) + ".other");
    };

    if (const locale_block* block = find_block(requested)) {
        if (const std::string* message = lookup(*block)) {
            return *message;
        }
    } else {
        const std::string language = language_of(requested);
        if (const locale_block* language_block = find_block(language)) {
            if (const std::string* message = lookup(*language_block)) {
                return *message;
            }
        }
    }

    if (options_.fallback_to_default) {
        const std::string fallback = normalize_locale(options_.default_locale);
        if (const locale_block* fallback_block = find_block(fallback)) {
            if (const std::string* message = lookup(*fallback_block)) {
                return *message;
            }
        }
    }

    return rpc_error{rpc_code::not_found,
                     "plural message \"" + std::string(key) +
                         "\" not found for locale \"" + requested + "\""};
}

result<std::string> message_catalog::format(
    std::string_view key,
    span<const std::pair<std::string_view, std::string_view>> args,
    std::string_view locale) const {
    const auto message = get(key, locale);
    if (!message.ok()) {
        return message.error();
    }

    const std::string& text = message.value();
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '}') {
            if (i + 1 < text.size() && text[i + 1] == '}') {
                out.push_back('}');
                ++i;
                continue;
            }
            return invalid("unbalanced '}' in message \"" + std::string(key) +
                           "\"");
        }
        if (ch != '{') {
            out.push_back(ch);
            continue;
        }
        if (i + 1 < text.size() && text[i + 1] == '{') {
            out.push_back('{');
            ++i;
            continue;
        }
        const std::size_t close = text.find('}', i + 1);
        if (close == std::string::npos) {
            return invalid("unclosed '{' in message \"" + std::string(key) +
                           "\"");
        }
        const std::string_view name(text.data() + i + 1, close - i - 1);
        if (name.empty() || name.find('{') != std::string_view::npos) {
            return invalid("malformed placeholder in message \"" +
                           std::string(key) + "\"");
        }

        const std::pair<std::string_view, std::string_view>* match = nullptr;
        for (const auto& arg : args) {
            if (arg.first == name) {
                match = &arg;
                break;
            }
        }
        if (match == nullptr && is_digits(name)) {
            // A numeric placeholder falls back to the positional argument at
            // that index unless a named argument shadowed it above.
            std::size_t index = 0;
            for (const char digit : name) {
                index = index * 10 + static_cast<std::size_t>(digit - '0');
            }
            if (index < args.size()) {
                match = &args[index];
            }
        }
        if (match == nullptr) {
            return invalid("no argument for placeholder \"" + std::string(name) +
                           "\" in message \"" + std::string(key) + "\"");
        }
        out.append(match->second.data(), match->second.size());
        i = close;
    }
    return out;
}

} // namespace i18n
} // namespace crpc