// CRPC -- message catalog with locale fallback, CLDR plural selection and
// placeholder interpolation.
//
// File format: INI, because it is the smallest format that still carries what a
// catalog needs (a locale name plus key/value pairs) and needs no dependency.
// The exact grammar is:
//
//     ; comment -- ';' or '#' at the start of a line
//     [en]                       ; locale section, required before any entry
//     greeting = Hello           ; key = value, both trimmed
//     items.one   = {0} item     ; a plural form is key.<category>
//     items.other = {0} items
//
//   * a locale name is matched case-insensitively and stored lower-cased;
//     "en-US" and "en_US" are equivalent;
//   * keys are case-sensitive and may contain dots;
//   * a blank line is ignored; a line without '=' in a section is an error;
//   * the escape sequences \n, \t and \\ expand inside values;
//   * a duplicate key inside one locale is an error (a silent overwrite hides
//     the typo that caused it);
//   * values are UTF-8 and never re-encoded.
//
// Plural categories are the CLDR cardinal set: zero, one, two, few, many,
// other. `plural` selects the category from the locale's language using the
// CLDR 49 rules restricted to the integer domain (count is std::int64_t, and
// CLDR evaluates the absolute value):
//
//     en, de   one  n = 1                      (i = 1 and v = 0)
//     zh       other always                    (no plural distinction)
//     fr       one  n = 0 or 1                 (i = 0..1)
//              many n != 0 and n % 1000000 = 0
//              other
//     ru       one  n % 10 = 1 and n % 100 != 11
//              few  n % 10 = 2..4 and n % 100 != 12..14
//              many n % 10 = 0 or n % 10 = 5..9 or n % 100 = 11..14
//              other
//
// Any other language uses the lossless fallback: it looks for the key's
// `.other` form only. The rules above are transcribed from CLDR 49
// supplemental/plurals.json (which itself says "Unicode 16.0.0, CLDR 49");
// the exact source lines are cited in tests/test_i18n_catalog.cpp.
//
// Fallback chain for `get`, `plural` and `format`:
//
//     requested locale ("fr-CA")
//       -> its language ("fr")
//         -> default_locale (options, "en" by default)
//           -> error `not_found`
//
// `fallback_to_default = false` drops the last step. A locale string of "C" or
// "" means "use the catalog's current locale".
//
// Thread safety: a catalog is immutable once loaded, so `get`, `plural`,
// `format` and every query may run concurrently. `set_locale` mutates the
// current-locale string and must not race with those calls.
#ifndef CRPC_I18N_CATALOG_HPP
#define CRPC_I18N_CATALOG_HPP

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace i18n {

enum class plural_category : std::uint8_t { zero, one, two, few, many, other };

// The lowercase name of a category ("one", "other", ...). Never nullptr.
[[nodiscard]] CRPC_API const char* to_string(plural_category category) noexcept;

struct message_catalog_options {
    std::string default_locale = "en";
    bool fallback_to_default = true;
};

class CRPC_API message_catalog {
public:
    explicit message_catalog(message_catalog_options options = {});

    // Parses `text`. `format` accepts "ini" (case-insensitive); every other
    // name is `invalid_argument` naming the supported set. A syntax error is
    // `invalid_argument` with the 1-based line number and, when a locale had
    // already been opened, its name.
    [[nodiscard]] static result<message_catalog>
    load(std::string_view text, std::string_view format,
         message_catalog_options options = {});

    // Selects the locale for calls that pass an empty locale. Returns false
    // and leaves the current locale untouched when neither `locale` nor its
    // language exists in the catalog.
    [[nodiscard]] bool set_locale(std::string_view locale);

    // The current locale, lower-cased. Defaults to options.default_locale
    // (also lower-cased), even when the catalog has no such section.
    [[nodiscard]] std::string_view locale() const noexcept;

    // True when `locale` or its language has a section.
    [[nodiscard]] bool has_locale(std::string_view locale) const;

    // Number of locales loaded.
    [[nodiscard]] std::size_t locale_count() const noexcept;

    // Raw lookup with fallback. The returned string is the catalog's own
    // storage and stays valid for the catalog's lifetime. A missing key is
    // `not_found` naming both the key and the locale tried last.
    [[nodiscard]] result<std::string> get(std::string_view key,
                                          std::string_view locale = {}) const;

    // Looks up `key.<category>` for the category CLDR selects for
    // (locale, count), then `key.other`, then repeats both on the next locale
    // in the fallback chain.
    [[nodiscard]] result<std::string> plural(std::string_view key,
                                             std::int64_t count,
                                             std::string_view locale = {}) const;

    // `get` followed by placeholder expansion. Placeholders are `{name}` and
    // `{0}`; names and indices live in one table, so `{"0", "x"}` wins over
    // the positional index 0. `{{` and `}}` produce literal braces.
    //
    // A placeholder with no matching argument, an index that is not a
    // non-negative decimal number, a lone brace and a nested/open-ended brace
    // are all `invalid_argument`.
    [[nodiscard]] result<std::string>
    format(std::string_view key,
           span<const std::pair<std::string_view, std::string_view>> args,
           std::string_view locale = {}) const;

    // Convenience overload for the common vector/initializer-list spelling.
    [[nodiscard]] result<std::string>
    format(std::string_view key,
           const std::vector<std::pair<std::string_view, std::string_view>>& args,
           std::string_view locale = {}) const {
        return format(key, span<const std::pair<std::string_view, std::string_view>>(
                               args.data(), args.size()),
                      locale);
    }

private:
    struct locale_block {
        std::string name;
        std::map<std::string, std::string, std::less<>> messages;
    };

    const locale_block* find_block(std::string_view locale) const;
    const std::string* find_message(const locale_block& block,
                                    std::string_view key) const;

    message_catalog_options options_;
    std::map<std::string, locale_block, std::less<>> locales_;
    std::string current_;
};

// Expands CLDR cardinal plural rules for `count` in `locale`'s language. An
// unknown language yields plural_category::other, which is the only category
// every catalog has to provide.
[[nodiscard]] CRPC_API plural_category
plural_category_for(std::string_view locale, std::int64_t count) noexcept;

} // namespace i18n
} // namespace crpc

#endif // CRPC_I18N_CATALOG_HPP
