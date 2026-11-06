// CRPC -- tests for crpc/text/string_order.hpp.
//
// Expected values are derived from the .NET StringComparer contract where the
// implementation claims to match it (Ordinal: byte/code-unit order; Ordinal
// IgnoreCase: ASCII case folding), and from the Unicode code point order where
// the implementation documents an approximation (the culture modes). Each case
// names its source in a comment.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <crpc/text/string_order.hpp>

#include "test_framework.hpp"

namespace {

using crpc::text::string_comparison;
using crpc::text::string_order;

// Sign of an int, to compare against the documented -1/0/1 contract without
// depending on the exact magnitude.
int sign(int value) { return (value > 0) - (value < 0); }

} // namespace

CRPC_TEST(string_order_ordinal_byte_order) {
    // .NET Ordinal compares code units; for ASCII this is byte order:
    // "Z" (0x5A) < "a" (0x61), "a" < "aa", "abc" < "abd".
    const string_order order = string_order::ordinal();
    CRPC_CHECK_EQ(-1, sign(order.compare("Z", "a")));
    CRPC_CHECK_EQ(1, sign(order.compare("a", "Z")));
    CRPC_CHECK_EQ(-1, sign(order.compare("a", "aa")));
    CRPC_CHECK_EQ(1, sign(order.compare("aa", "a")));
    CRPC_CHECK_EQ(-1, sign(order.compare("abc", "abd")));
    CRPC_CHECK_EQ(0, sign(order.compare("abc", "abc")));
    CRPC_CHECK_EQ(0, sign(order.compare("", "")));
    CRPC_CHECK_EQ(-1, sign(order.compare("", "a")));

    // Case matters in ordinal compare (M4 gate: making ordinal case-insensitive
    // flips this expectation).
    CRPC_CHECK_EQ(-1, sign(order.compare("ABC", "abc")));
    CRPC_CHECK_FALSE(order.equals("ABC", "abc"));
    CRPC_CHECK(order.equals("ABC", "ABC"));

    // Digits sort before letters (ASCII).
    CRPC_CHECK_EQ(-1, sign(order.compare("9", "A")));
}

CRPC_TEST(string_order_ordinal_utf8_order) {
    // UTF-8 preserves code point order, so this is also Unicode code point
    // order for the BMP: U+00E9 (茅, C3 A9) < U+4E2D (涓? E4 B8 AD) < U+1F600
    // (emoji, F0 9F 98 80).
    const string_order order = string_order::ordinal();
    CRPC_CHECK_EQ(-1, sign(order.compare("\xC3\xA9", "\xE4\xB8\xAD")));
    CRPC_CHECK_EQ(-1, sign(order.compare("\xE4\xB8\xAD", "\xF0\x9F\x98\x80")));
    // ASCII < any non-ASCII byte.
    CRPC_CHECK_EQ(-1, sign(order.compare("z", "\xC3\xA9")));
}

CRPC_TEST(string_order_ordinal_ignore_case_ascii) {
    // .NET OrdinalIgnoreCase: ASCII letters fold, so "HELLO" equals "hello".
    const string_order order = string_order::ordinal_ignore_case();
    CRPC_CHECK(order.equals("HELLO", "hello"));
    CRPC_CHECK(order.equals("Hello", "hELLo"));
    CRPC_CHECK_EQ(0, sign(order.compare("ABC", "abc")));
    CRPC_CHECK_FALSE(order.equals("ABC", "abd"));
    CRPC_CHECK(order.starts_with("Hello World", "hello "));
    CRPC_CHECK(order.ends_with("Hello World", "WORLD"));

    // Non-ASCII bytes are NOT folded: the mode is ASCII-only (see the file
    // comment; a real OrdinalIgnoreCase would also fold U+00C4/U+00E4).
    // "脛" = C3 84, "盲" = C3 A4; the second byte differs (0x84 vs 0xA4).
    CRPC_CHECK_FALSE(order.equals("\xC3\x84", "\xC3\xA4"));
    CRPC_CHECK_EQ(-1, sign(order.compare("\xC3\x84", "\xC3\xA4")));

    // Folding is per byte and does not hide a real difference elsewhere.
    CRPC_CHECK_FALSE(order.equals("ABC", "AB"));
    CRPC_CHECK_EQ(1, sign(order.compare("ABC", "AB")));
}

CRPC_TEST(string_order_ordinal_ignore_case_boundary_bytes) {
    // '@' (0x40) is just below 'A', '{' (0x7B) just above 'Z'. Folding must not
    // map them into the alphabet: '@' != 'a', '{' != 'Z'.
    const string_order order = string_order::ordinal_ignore_case();
    CRPC_CHECK_FALSE(order.equals("@", "a"));
    CRPC_CHECK_FALSE(order.equals("{", "Z"));
    CRPC_CHECK_EQ(-1, sign(order.compare("@", "a")));
    CRPC_CHECK_EQ(1, sign(order.compare("{", "Z")));
    // '[' (0x5B) sits between 'Z' (0x5A) and 'a' (0x61); it is not a letter.
    CRPC_CHECK(order.equals("[", "["));
}

CRPC_TEST(string_order_culture_modes_approximate_code_point_order) {
    // The header states that invariant/current culture are approximated by
    // Unicode code point order. Verify exactly that, and verify the case
    // folding is ASCII-only -- do NOT assert linguistic-collation behaviour
    // that is not implemented.
    const string_order invariant = string_order::invariant_culture();
    const string_order invariant_fold =
        string_order::invariant_culture_ignore_case();
    const string_order current = string_order::current_culture();
    const string_order current_fold =
        string_order::current_culture_ignore_case();

    for (const string_order* order :
         {&invariant, &invariant_fold, &current, &current_fold}) {
        CRPC_CHECK_EQ(-1, sign(order->compare("a", "b")));
        CRPC_CHECK_EQ(1, sign(order->compare("b", "a")));
        CRPC_CHECK_EQ(0, sign(order->compare("abc", "abc")));
        CRPC_CHECK_EQ(-1, sign(order->compare("a", "aa")));
        // BMP code points: CJK after Latin.
        CRPC_CHECK_EQ(-1, sign(order->compare("z", "\xE4\xB8\xAD")));
    }

    // Code point order: 'Z' (U+005A) < 'a' (U+0061), same as ordinal -- but
    // only in the case-sensitive modes. The ignore-case variants fold ASCII
    // first, so there 'Z' and 'a' become 'Z' vs 'A' and 'Z' sorts after.
    CRPC_CHECK_EQ(-1, sign(invariant.compare("Z", "a")));
    CRPC_CHECK_EQ(-1, sign(current.compare("Z", "a")));
    CRPC_CHECK_EQ(1, sign(invariant_fold.compare("Z", "a")));
    CRPC_CHECK_EQ(1, sign(current_fold.compare("Z", "a")));

    // The ignore-case variants fold ASCII only, exactly like the ordinal one.
    CRPC_CHECK(invariant_fold.equals("ABC", "abc"));
    CRPC_CHECK(current_fold.equals("ABC", "abc"));
    CRPC_CHECK_FALSE(invariant_fold.equals("\xC3\x84", "\xC3\xA4"));
    CRPC_CHECK_FALSE(current_fold.equals("\xC3\x84", "\xC3\xa4"));

    // Non-ignore-case culture modes are case sensitive.
    CRPC_CHECK_FALSE(invariant.equals("ABC", "abc"));
    CRPC_CHECK_FALSE(current.equals("ABC", "abc"));
}

CRPC_TEST(string_order_starts_and_ends_with) {
    const string_order ordinal = string_order::ordinal();
    CRPC_CHECK(ordinal.starts_with("hello world", "hello"));
    CRPC_CHECK_FALSE(ordinal.starts_with("hello world", "Hello"));
    CRPC_CHECK(ordinal.ends_with("hello world", "world"));
    CRPC_CHECK_FALSE(ordinal.ends_with("hello world", "World"));
    CRPC_CHECK(ordinal.starts_with("abc", ""));
    CRPC_CHECK(ordinal.ends_with("abc", ""));
    CRPC_CHECK_FALSE(ordinal.starts_with("ab", "abc"));
    CRPC_CHECK_FALSE(ordinal.ends_with("ab", "abc"));
    CRPC_CHECK(ordinal.starts_with("abc", "abc"));
    CRPC_CHECK(ordinal.ends_with("abc", "abc"));

    const string_order folded = string_order::ordinal_ignore_case();
    CRPC_CHECK(folded.starts_with("Hello", "hE"));
    CRPC_CHECK(folded.ends_with("Hello", "LO"));
    CRPC_CHECK_FALSE(folded.ends_with("Hell", "Hello"));
}

CRPC_TEST(string_order_hash_consistency) {
    // Equals => same hash, for every mode. This is the only hash property the
    // header promises; distinct strings may collide, so the converse is only
    // spot-checked for obvious non-collisions.
    const std::vector<string_order> modes{
        string_order::ordinal(),
        string_order::ordinal_ignore_case(),
        string_order::invariant_culture(),
        string_order::invariant_culture_ignore_case(),
        string_order::current_culture(),
        string_order::current_culture_ignore_case()};

    for (const string_order& order : modes) {
        CRPC_CHECK_EQ(order.hash("abc"), order.hash("abc"));
        CRPC_CHECK_EQ(order.hash(""), order.hash(std::string_view()));
        CRPC_CHECK_FALSE(order.hash("abc") == order.hash("abd"));
        if (order.equals("ABC", "abc")) {
            CRPC_CHECK_EQ(order.hash("ABC"), order.hash("abc"));
        }
        if (order.equals("Hello", "HELLO")) {
            CRPC_CHECK_EQ(order.hash("Hello"), order.hash("HELLO"));
        }
    }

    // The hash of ordinal is not case folded: "ABC" and "abc" hash differently.
    const string_order ordinal = string_order::ordinal();
    CRPC_CHECK_FALSE(ordinal.hash("ABC") == ordinal.hash("abc"));
}

CRPC_TEST(string_order_usable_as_container_policy) {
    // The policy object is the third template argument of the standard
    // hashtable; equality and hashing must agree, or the container misbehaves.
    struct folded_hash {
        string_order order = string_order::ordinal_ignore_case();
        std::size_t operator()(std::string_view text) const {
            return order.hash(text);
        }
    };
    struct folded_equal {
        string_order order = string_order::ordinal_ignore_case();
        bool operator()(std::string_view a, std::string_view b) const {
            return order.equals(a, b);
        }
    };
    using set_type =
        std::unordered_set<std::string, folded_hash, folded_equal>;

    set_type seen;
    seen.insert(std::string("Hello"));
    seen.insert(std::string("HELLO")); // equal under the policy
    seen.insert(std::string("world"));
    CRPC_CHECK_EQ(2u, seen.size());

    // And the comparison functor sorts.
    std::vector<std::string> values{"banana", "Apple", "cherry", "apple"};
    std::sort(values.begin(), values.end(),
              string_order::ordinal_ignore_case());
    // Ordering under ASCII fold: Apple/apple (equal) then banana, cherry.
    CRPC_CHECK_EQ(std::string("Apple"), values[0]);
    CRPC_CHECK_EQ(std::string("banana"), values[2]);
    CRPC_CHECK_EQ(std::string("cherry"), values[3]);
}

CRPC_TEST(string_order_malformed_utf8_is_total) {
    // Invalid UTF-8 never throws and still produces a stable total order. A
    // lone continuation byte (0x80) and a truncated 2-byte lead (0xC3) are the
    // cases that a strict decoder would reject; here they are ordered after
    // valid text and compare consistently with themselves.
    CRPC_CHECK_EQ(0, sign(string_order::ordinal().compare("\x80", "\x80")));
    CRPC_CHECK_EQ(0, sign(string_order::ordinal().compare("\xC3", "\xC3")));
    CRPC_CHECK_EQ(-1, sign(string_order::ordinal().compare("a", "\x80")));
    CRPC_CHECK_EQ(-1, sign(string_order::ordinal().compare("a", "\xC3")));

    // The culture modes decode: a truncated lead becomes one invalid token,
    // which the implementation orders above every valid code point. That keeps
    // the order total and deterministic, and is the documented fallback (a real
    // collator would reject or replace the sequence instead).
    const string_order culture = string_order::invariant_culture();
    CRPC_CHECK_EQ(-1, sign(culture.compare("a", "\xC3")));
    CRPC_CHECK_EQ(0, sign(culture.compare("\xC3", "\xC3")));
    // "\xC3" (invalid token) sorts after "\xC3\xA9" (valid U+00E9).
    CRPC_CHECK_EQ(1, sign(culture.compare("\xC3", "\xC3\xA9")));
    // Prefix rule still applies when the tokens match: "" < anything.
    CRPC_CHECK_EQ(-1, sign(culture.compare("", "\xC3")));
}

CRPC_TEST(string_order_static_factories) {
    CRPC_CHECK(string_order::ordinal().comparison() ==
               string_comparison::ordinal);
    CRPC_CHECK(string_order::ordinal_ignore_case().comparison() ==
               string_comparison::ordinal_ignore_case);
    CRPC_CHECK(string_order::invariant_culture().comparison() ==
               string_comparison::invariant_culture);
    CRPC_CHECK(string_order::invariant_culture_ignore_case().comparison() ==
               string_comparison::invariant_culture_ignore_case);
    CRPC_CHECK(string_order::current_culture().comparison() ==
               string_comparison::current_culture);
    CRPC_CHECK(string_order::current_culture_ignore_case().comparison() ==
               string_comparison::current_culture_ignore_case);

    // The default constructor is ordinal.
    const string_order defaulted;
    CRPC_CHECK(defaulted.comparison() == string_comparison::ordinal);
    CRPC_CHECK_FALSE(defaulted.equals("A", "a"));
}

CRPC_TEST_MAIN()
