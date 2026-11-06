// CRPC -- tests for crpc/i18n message catalogs: INI loading, locale fallback,
// CLDR plural selection and placeholder interpolation.
//
// The plural expectations are transcribed from CLDR 49
// supplemental/plurals.json ("_unicodeVersion": "16.0.0"); the exact rule
// strings appear next to the checks that exercise them.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <crpc/i18n/catalog.hpp>

#include "test_framework.hpp"

namespace {

constexpr std::string_view kSample =
    "; a small catalog in the documented INI dialect\n"
    "\n"
    "[en]\n"
    "greeting = Hello, {name}!\n"
    "files.one = {count} file\n"
    "files.other = {count} files\n"
    "only_en = English only\n"
    "braces = {{literal}} and {{0}}\n"
    "positional = {0} + {1}\n"
    "newline = first\\nsecond\n"
    "[de]\n"
    "greeting = Hallo, {name}!\n"
    "files.one = {count} Datei\n"
    "files.other = {count} Dateien\n"
    "[zh]\n"
    "greeting = \xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C{name}\xEF\xBC\x81\n"
    "files.other = {count} \xE4\xB8\xAA\xE6\x96\x87\xE4\xBB\xB6\n"
    "[ru]\n"
    "files.one = {count} \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB\n"
    "files.few = {count} \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB\xD0\xB0\n"
    "files.many = {count} \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB\xD0\xBE\xD0\xB2\n"
    "files.other = {count} \xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB\xD0\xB0\n";

crpc::i18n::message_catalog sample_catalog(
    crpc::i18n::message_catalog_options options = {}) {
    const auto loaded = crpc::i18n::message_catalog::load(kSample, "ini", options);
    if (!loaded.ok()) {
        CRPC_CHECK(false);
        return crpc::i18n::message_catalog{};
    }
    return loaded.value();
}

std::string text_of(const crpc::result<std::string>& value) {
    if (!value.ok()) {
        CRPC_CHECK(false);
        return {};
    }
    return value.value();
}

} // namespace

CRPC_TEST(catalog_loads_and_reports_its_locales) {
    const auto catalog = sample_catalog();
    CRPC_CHECK_EQ(static_cast<std::size_t>(4), catalog.locale_count());
    CRPC_CHECK_EQ(std::string("en"), std::string(catalog.locale()));
    CRPC_CHECK(catalog.has_locale("en"));
    CRPC_CHECK(catalog.has_locale("DE")); // case-insensitive
    CRPC_CHECK_FALSE(catalog.has_locale("fr"));
    CRPC_CHECK_EQ(std::string("Hello, {name}!"), text_of(catalog.get("greeting")));
}

CRPC_TEST(catalog_set_locale_requires_a_known_locale) {
    auto catalog = sample_catalog();
    CRPC_CHECK(catalog.set_locale("de"));
    CRPC_CHECK_EQ(std::string("de"), std::string(catalog.locale()));
    CRPC_CHECK_EQ(std::string("Hallo, {name}!"), text_of(catalog.get("greeting")));

    // "de-AT" has no section but its language does; the documented behaviour
    // is to accept it and to keep the canonical name of the block.
    CRPC_CHECK(catalog.set_locale("de_AT"));
    CRPC_CHECK_EQ(std::string("de"), std::string(catalog.locale()));

    CRPC_CHECK_FALSE(catalog.set_locale("xx"));
    CRPC_CHECK_EQ(std::string("de"), std::string(catalog.locale()));
}

CRPC_TEST(catalog_get_falls_back_through_language_then_default) {
    // Requested locale -> language -> default_locale (options.fallback_to_default).
    const auto catalog = sample_catalog();
    CRPC_CHECK_EQ(std::string("Hallo, {name}!"), text_of(catalog.get("greeting", "de-DE")));
    CRPC_CHECK_EQ(std::string("English only"), text_of(catalog.get("only_en", "de")));

    crpc::i18n::message_catalog_options strict;
    strict.fallback_to_default = false;
    const auto catalog_strict = sample_catalog(strict);
    const auto missing = catalog_strict.get("only_en", "de");
    CRPC_CHECK_FALSE(missing.ok());
    CRPC_CHECK_EQ(crpc::rpc_code::not_found, missing.code());

    const auto unknown_key = catalog.get("missing_key");
    CRPC_CHECK_FALSE(unknown_key.ok());
    CRPC_CHECK_EQ(crpc::rpc_code::not_found, unknown_key.code());
}

CRPC_TEST(catalog_plural_uses_cldr_cardinal_categories) {
    // CLDR 49 plurals.json:
    //   en: "one" i = 1 and v = 0; "other" otherwise
    //   zh: "other" only
    //   ru: "one" v = 0 and i % 10 = 1 and i % 100 != 11
    //       "few" v = 0 and i % 10 = 2..4 and i % 100 != 12..14
    //       "many" v = 0 and i % 10 = 0 or i % 10 = 5..9 or i % 100 = 11..14
    const auto catalog = sample_catalog();
    using crpc::i18n::plural_category;

    CRPC_CHECK_EQ(std::string("{count} file"), text_of(catalog.plural("files", 1, "en")));
    CRPC_CHECK_EQ(std::string("{count} files"), text_of(catalog.plural("files", 2, "en")));
    CRPC_CHECK_EQ(std::string("{count} files"), text_of(catalog.plural("files", 0, "en")));
    // CLDR evaluates the absolute value, so -1 is "one".
    CRPC_CHECK_EQ(std::string("{count} file"), text_of(catalog.plural("files", -1, "en")));

    CRPC_CHECK_EQ(std::string("{count} \xE4\xB8\xAA\xE6\x96\x87\xE4\xBB\xB6"),
                  text_of(catalog.plural("files", 1, "zh")));

    using crpc::i18n::plural_category_for;
    CRPC_CHECK_EQ(std::string("one"), crpc::i18n::to_string(plural_category_for("ru", 1)));
    CRPC_CHECK_EQ(std::string("few"), crpc::i18n::to_string(plural_category_for("ru", 2)));
    CRPC_CHECK_EQ(std::string("many"), crpc::i18n::to_string(plural_category_for("ru", 5)));
    CRPC_CHECK_EQ(std::string("many"), crpc::i18n::to_string(plural_category_for("ru", 11)));
    CRPC_CHECK_EQ(std::string("one"), crpc::i18n::to_string(plural_category_for("ru", 21)));
    CRPC_CHECK_EQ(std::string("few"), crpc::i18n::to_string(plural_category_for("ru", 22)));
}

CRPC_TEST(plural_category_for_follows_cldr_rules) {
    using crpc::i18n::plural_category;
    using crpc::i18n::plural_category_for;

    CRPC_CHECK_EQ(plural_category::one, plural_category_for("en", 1));
    CRPC_CHECK_EQ(plural_category::other, plural_category_for("en", 2));
    CRPC_CHECK_EQ(plural_category::other, plural_category_for("en", 0));
    CRPC_CHECK_EQ(plural_category::other, plural_category_for("zh", 1));
    // fr: "one" i = 0..1; "many" e = 0 and i != 0 and i % 1000000 = 0 and v = 0
    CRPC_CHECK_EQ(plural_category::one, plural_category_for("fr", 0));
    CRPC_CHECK_EQ(plural_category::one, plural_category_for("fr", 1));
    CRPC_CHECK_EQ(plural_category::other, plural_category_for("fr", 2));
    CRPC_CHECK_EQ(plural_category::many, plural_category_for("fr", 1000000));
    CRPC_CHECK_EQ(plural_category::many, plural_category_for("fr", 2000000));
    CRPC_CHECK_EQ(plural_category::other, plural_category_for("fr", 1234567));
    CRPC_CHECK_EQ(plural_category::one, plural_category_for("de-DE", 1));
    CRPC_CHECK_EQ(plural_category::other, plural_category_for("de-DE", 3));
    // Unknown language: CLDR has no rule, so the lossless fallback is "other".
    CRPC_CHECK_EQ(plural_category::other, plural_category_for("xx", 1));
}

CRPC_TEST(catalog_format_interpolates_named_and_positional_placeholders) {
    const auto catalog = sample_catalog();

    const std::vector<std::pair<std::string_view, std::string_view>> named = {
        {"name", "Ada"}};
    CRPC_CHECK_EQ(std::string("Hello, Ada!"), text_of(catalog.format("greeting", named)));

    // "{0}" resolves by name first, then positionally; "{1}" with no second
    // argument is an error, not a leftover placeholder.
    const std::vector<std::pair<std::string_view, std::string_view>> positional = {
        {"0", "three"}};
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  catalog.format("positional", positional).code());

    const std::vector<std::pair<std::string_view, std::string_view>> two = {
        {"0", "one"}, {"1", "two"}};
    CRPC_CHECK_EQ(std::string("one + two"), text_of(catalog.format("positional", two)));

    // "{{" and "}}" expand to literal braces (the .NET spelling), so the
    // message renders as "{literal} and {0}".
    const std::vector<std::pair<std::string_view, std::string_view>> none = {};
    CRPC_CHECK_EQ(std::string("{literal} and {0}"),
                  text_of(catalog.format("braces", none)));

    // The escaped newline survives the round trip.
    CRPC_CHECK_EQ(std::string("first\nsecond"), text_of(catalog.get("newline")));

    // Missing arguments and broken braces are errors, not silent output.
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  catalog.format("greeting", none).code());
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  catalog.format("positional", positional).code());

    // Positional {1} without a second argument is an error as well.
    const std::vector<std::pair<std::string_view, std::string_view>> one = {{"0", "a"}};
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  catalog.format("positional", one).code());
}

CRPC_TEST(catalog_load_rejects_broken_input) {
    // Unsupported format name.
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  crpc::i18n::message_catalog::load(kSample, "json").code());

    // Entry outside any locale section.
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  crpc::i18n::message_catalog::load("key = value\n", "ini").code());

    // Line without '='.
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  crpc::i18n::message_catalog::load("[en]\ngreeting\n", "ini").code());

    // Duplicate key inside one locale.
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  crpc::i18n::message_catalog::load(
                      "[en]\na = 1\na = 2\n", "ini").code());

    // Malformed header.
    CRPC_CHECK_EQ(crpc::rpc_code::invalid_argument,
                  crpc::i18n::message_catalog::load("[en\n", "ini").code());
}

CRPC_TEST_MAIN()