// CRPC -- tests for crpc/text/regex.hpp: the backtracking regular expression
// engine.
//
// Every expectation in the generated tables below was produced by running the
// pattern and text through Python 3.14.7's `re` module; the originating call is
// recorded in the comment above each row. Nothing here is hand-computed, so a
// wrong expectation cannot be mistaken for a wrong engine (the engine is
// ASCII-only and every vector is pure ASCII, where the two agree).
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/text/regex.hpp>

#include "test_framework.hpp"

namespace {

using crpc::text::regex;
using crpc::text::regex_match;
using crpc::text::regex_options;

regex_options with_flags(unsigned flags) {
    regex_options options;
    options.icase = (flags & 1u) != 0;
    options.multiline = (flags & 2u) != 0;
    options.dotall = (flags & 4u) != 0;
    return options;
}

struct group_expectation {
    bool set;
    std::size_t begin;
    std::size_t end;
};

struct search_vector {
    const char* pattern;
    const char* text;
    unsigned flags;
    std::size_t group_count;
    bool matched;
    std::size_t begin;
    std::size_t end;
    group_expectation groups[4];
};

struct find_all_vector {
    const char* pattern;
    const char* text;
    unsigned flags;
    std::vector<std::pair<std::size_t, std::size_t>> spans;
};

struct replace_vector {
    const char* pattern;
    const char* text;
    const char* replacement;
    unsigned flags;
    const char* first;
    const char* all;
};

// --- Generated with Python 3.14.7 re module: search --------------------------
// Each row's comment is the exact call that produced it.
const search_vector search_vectors[] = {
    {"abc", "xxabcyy", 0, 0, true, 2, 5, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('abc', 'xxabcyy') -> (2, 5), groups=()
    {"abc", "xxabXyy", 0, 0, false, 0, 0, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    {"a.c", "xxabcxx", 0, 0, true, 2, 5, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('a.c', 'xxabcxx') -> (2, 5)
    {"a.c", "a\nc", 0, 0, false, 0, 0, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    {"a.c", "a\nc", 4, 0, true, 0, 3, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('a.c', 'a\nc', re.DOTALL) -> (0, 3)
    {"[abc]+", "xxabcab", 0, 0, true, 2, 7, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('[abc]+', 'xxabcab') -> (2, 7)
    {"[^abc]+", "abcxyz", 0, 0, true, 3, 6, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('[^abc]+', 'abcxyz') -> (3, 6)
    {"[a-f0-9]+", "zz1a2fz", 0, 0, true, 2, 6, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('[a-f0-9]+', 'zz1a2fz') -> (2, 6)
    {"a+", "baaaac", 0, 0, true, 1, 5, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('a+', 'baaaac') -> (1, 5)
    {"a+?", "baaaac", 0, 0, true, 1, 2, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('a+?', 'baaaac') -> (1, 2)
    {"ab?c", "ac", 0, 0, true, 0, 2, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('ab?c', 'ac') -> (0, 2)
    {"a{3}", "aaaa", 0, 0, true, 0, 3, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('a{3}', 'aaaa') -> (0, 3)
    {"a{2,3}", "aaaa", 0, 0, true, 0, 3, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('a{2,3}', 'aaaa') -> (0, 3)
    {"a{2,3}?", "aaaa", 0, 0, true, 0, 2, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('a{2,3}?', 'aaaa') -> (0, 2)
    {"a{2,}", "baaaa", 0, 0, true, 1, 5, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('a{2,}', 'baaaa') -> (1, 5)
    {"(ab)+", "xababy", 0, 1, true, 1, 5, {{true, 3, 5}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('(ab)+', 'xababy') -> (1, 5), groups=('ab',)
    {"((a)(b))c", "abc", 0, 3, true, 0, 3, {{true, 0, 2}, {true, 0, 1}, {true, 1, 2}, {false, 0, 0}}},
    // re.search('((a)(b))c', 'abc') -> (0, 3), groups=('ab', 'a', 'b')
    {"(?:ab)+", "abab", 0, 0, true, 0, 4, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('(?:ab)+', 'abab') -> (0, 4)
    {"(a|b)+", "xabba", 0, 1, true, 1, 5, {{true, 4, 5}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('(a|b)+', 'xabba') -> (1, 5), groups=('a',)
    {"cat|category", "category", 0, 0, true, 0, 3, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('cat|category', 'category') -> (0, 3)  (leftmost alternative wins)
    {"cat|dog", "a dog", 0, 0, true, 2, 5, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('cat|dog', 'a dog') -> (2, 5)
    {"a|", "b", 0, 0, true, 0, 0, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('a|', 'b') -> (0, 0)  (the empty branch matches)
    {"^abc", "abcdef", 0, 0, true, 0, 3, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('^abc', 'abcdef') -> (0, 3)
    {"^abc", "xabc", 0, 0, false, 0, 0, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    {"abc$", "xxabc", 0, 0, true, 2, 5, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('abc$', 'xxabc') -> (2, 5)
    {"abc$", "xxabcx", 0, 0, false, 0, 0, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    {"abc$", "abc\n", 0, 0, true, 0, 3, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('abc$', 'abc\n') -> (0, 3)  ('$' matches before a trailing newline)
    {"^b", "a\nb", 2, 0, true, 2, 3, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('^b', 'a\nb', re.MULTILINE) -> (2, 3)
    {"a$", "a\nb", 2, 0, true, 0, 1, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('a$', 'a\nb', re.MULTILINE) -> (0, 1)
    {"\\bcat\\b", "a cat!", 0, 0, true, 2, 5, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search(r'\bcat\b', 'a cat!') -> (2, 5)
    {"\\Bcat\\B", "scatter", 0, 0, true, 1, 4, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search(r'\Bcat\B', 'scatter') -> (1, 4)
    {"(ab)\\1", "xabab", 0, 1, true, 1, 5, {{true, 1, 3}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search(r'(ab)\1', 'xabab') -> (1, 5), groups=('ab',)
    {"(ab)\\1", "xabac", 0, 1, false, 0, 0, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    {"(a)\\1", "aA", 1, 1, true, 0, 2, {{true, 0, 1}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search(r'(a)\1', 'aA', re.IGNORECASE) -> (0, 2)
    {"abc", "xAbC", 1, 0, true, 1, 4, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('abc', 'xAbC', re.IGNORECASE) -> (1, 4)
    {"[a-z]+", "ABC", 1, 0, true, 0, 3, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('[a-z]+', 'ABC', re.IGNORECASE) -> (0, 3)
    {"\\d+", "ab123x", 0, 0, true, 2, 5, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search(r'\d+', 'ab123x') -> (2, 5)
    {"\\w+", " ab_12-x", 0, 0, true, 1, 6, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search(r'\w+', ' ab_12-x') -> (1, 6)
    {"\\s+", "ab \t\ncd", 0, 0, true, 2, 5, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search(r'\s+', 'ab \t\ncd') -> (2, 5)
    {"\\D+", "12ab34", 0, 0, true, 2, 4, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search(r'\D+', '12ab34') -> (2, 4)
    {"a\\.b", "a.b", 0, 0, true, 0, 3, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search(r'a\.b', 'a.b') -> (0, 3)
    {"a\\.b", "axb", 0, 0, false, 0, 0, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    {"a\\nb", "a\nb", 0, 0, true, 0, 3, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search(r'a\nb', 'a\nb') -> (0, 3)
    {"", "abc", 0, 0, true, 0, 0, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('', 'abc') -> (0, 0)
    {"a*", "", 0, 0, true, 0, 0, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('a*', '') -> (0, 0)
    {"abc", "abc", 0, 0, true, 0, 3, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('abc', 'abc') -> (0, 3)
    {"(a?)*", "aa", 0, 1, true, 0, 2, {{true, 2, 2}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('(a?)*', 'aa') -> (0, 2), groups=('',)
    {"(a?)*", "b", 0, 1, true, 0, 0, {{true, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('(a?)*', 'b') -> (0, 0), groups=('',)
    {"(?:a*)*", "aaa", 0, 0, true, 0, 3, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('(?:a*)*', 'aaa') -> (0, 3)
    {"x*", "yyy", 0, 0, true, 0, 0, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search('x*', 'yyy') -> (0, 0)
    {"(a)?b\\1", "b", 0, 1, false, 0, 0, {{false, 0, 0}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search(r'(a)?b\1', 'b') -> None  (an unset group never matches)
    {"(a)?b\\1", "aba", 0, 1, true, 0, 3, {{true, 0, 1}, {false, 0, 0}, {false, 0, 0}, {false, 0, 0}}},
    // re.search(r'(a)?b\1', 'aba') -> (0, 3), groups=('a',)
};

// --- Generated with Python 3.14.7 re module: find_all ------------------------
const find_all_vector find_all_vectors[] = {
    {"\\d+", "a1b22c333", 0, {{1u, 2u}, {3u, 5u}, {6u, 9u}}},
    // [m.span() for m in re.finditer(r'\d+', 'a1b22c333')] -> [(1,2), (3,5), (6,9)]
    {"a*", "baa", 0, {{0u, 0u}, {1u, 3u}, {3u, 3u}}},
    // [m.span() for m in re.finditer('a*', 'baa')] -> [(0,0), (1,3), (3,3)]
    {"a*", "aab", 0, {{0u, 2u}, {2u, 2u}, {3u, 3u}}},
    // [m.span() for m in re.finditer('a*', 'aab')] -> [(0,2), (2,2), (3,3)]
    {"a*", "aaa", 0, {{0u, 3u}, {3u, 3u}}},
    // [m.span() for m in re.finditer('a*', 'aaa')] -> [(0,3), (3,3)]
    {"a*", "bab", 0, {{0u, 0u}, {1u, 2u}, {2u, 2u}, {3u, 3u}}},
    // [m.span() for m in re.finditer('a*', 'bab')] -> [(0,0), (1,2), (2,2), (3,3)]
    {"", "ab", 0, {{0u, 0u}, {1u, 1u}, {2u, 2u}}},
    // [m.span() for m in re.finditer('', 'ab')] -> [(0,0), (1,1), (2,2)]
    {"..", "abcde", 0, {{0u, 2u}, {2u, 4u}}},
    // [m.span() for m in re.finditer('..', 'abcde')] -> [(0,2), (2,4)]
    {"\\b\\w+\\b", "one two", 0, {{0u, 3u}, {4u, 7u}}},
    // [m.span() for m in re.finditer(r'\b\w+\b', 'one two')] -> [(0,3), (4,7)]
    {"a?", "aaa", 0, {{0u, 1u}, {1u, 2u}, {2u, 3u}, {3u, 3u}}},
    // [m.span() for m in re.finditer('a?', 'aaa')] -> [(0,1), (1,2), (2,3), (3,3)]
    {"a?", "", 0, {{0u, 0u}}},
    // [m.span() for m in re.finditer('a?', '')] -> [(0,0)]
    {"x*", "abxd", 0, {{0u, 0u}, {1u, 1u}, {2u, 3u}, {3u, 3u}, {4u, 4u}}},
    // [m.span() for m in re.finditer('x*', 'abxd')] -> [(0,0), (1,1), (2,3), (3,3), (4,4)]
    {"[0-9]+", "no digits", 0, {}},
    // [m.span() for m in re.finditer('[0-9]+', 'no digits')] -> []
};

// --- Generated with Python 3.14.7 re module: replace -------------------------
const replace_vector replace_vectors[] = {
    {"(\\w+)@(\\w+)", "user@host", "$2:$1", 0, "host:user", "host:user"},
    // re.sub(r'(\w+)@(\w+)', r'\g<2>:\g<1>', 'user@host') -> 'host:user'
    {"a", "banana", "b", 0, "bbnana", "bbnbnb"},
    // re.sub('a', 'b', 'banana') -> first 'bbnana', all 'bbnbnb'
    {"", "abc", "-", 0, "-abc", "-a-b-c-"},
    // re.sub('', '-', 'abc') -> first '-abc', all '-a-b-c-'
    {"", "", "-", 0, "-", "-"},
    // re.sub('', '-', '') -> '-'
    {"\\d+", "a12b3", "<$0>", 0, "a<12>b3", "a<12>b<3>"},
    // re.sub(r'\d+', r'<\g<0>>', 'a12b3') -> first 'a<12>b3', all 'a<12>b<3>'
    {"(a)(b)", "ab", "$2$1", 0, "ba", "ba"},
    // re.sub('(a)(b)', r'\g<2>\g<1>', 'ab') -> 'ba'
    {"a", "a", "\\\\", 0, "\\", "\\"},
    // re.sub('a', '\\\\', 'a') -> '\\'
    {"x", "abc", "y", 0, "abc", "abc"},
    // re.sub('x', 'y', 'abc') -> 'abc'
    {"(a)?b", "b", "<$1>", 0, "<>", "<>"},
    // re.sub('(a)?b', r'<\g<1>>', 'b') -> '<>'
    {"a*", "aab", "-", 0, "-b", "--b-"},
    // re.sub('a*', '-', 'aab') -> first '-b', all '--b-'
    {"x*", "abxd", "-", 0, "-abxd", "-a-b--d-"},
    // re.sub('x*', '-', 'abxd') -> first '-abxd', all '-a-b--d-'
    {"(a)|(b)", "ab", "<$1$2>", 0, "<a>b", "<a><b>"},
    // re.sub('(a)|(b)', r'<\g<1>\g<2>>', 'ab') -> first '<a>b', all '<a><b>'
};

} // namespace

CRPC_TEST(regex_search_matches_python_vectors) {
    for (const search_vector& v : search_vectors) {
        const auto compiled = regex::compile(v.pattern, with_flags(v.flags));
        CRPC_REQUIRE(compiled.ok());
        const auto found = compiled.value().search(v.text);
        CRPC_REQUIRE(found.ok());
        const regex_match& m = found.value();
        if (v.matched != m.matched) {
            CRPC_CHECK_EQ(v.matched, m.matched);
            continue;
        }
        if (!v.matched) {
            continue;
        }
        CRPC_CHECK_EQ(v.begin, m.begin);
        CRPC_CHECK_EQ(v.end, m.end);
        CRPC_REQUIRE(m.groups.size() == v.group_count);
        for (std::size_t i = 0; i < v.group_count; ++i) {
            const group_expectation& expected = v.groups[i];
            if (expected.set) {
                CRPC_CHECK(m.groups[i].first == expected.begin &&
                           m.groups[i].second == expected.end);
            } else {
                CRPC_CHECK(m.groups[i].first == regex_match::npos);
            }
        }
    }
}

CRPC_TEST(regex_find_all_matches_python_vectors) {
    for (const find_all_vector& v : find_all_vectors) {
        const auto compiled = regex::compile(v.pattern, with_flags(v.flags));
        CRPC_REQUIRE(compiled.ok());
        const auto found = compiled.value().find_all(v.text);
        CRPC_REQUIRE(found.ok());
        const std::vector<regex_match>& matches = found.value();
        CRPC_REQUIRE(matches.size() == v.spans.size());
        for (std::size_t i = 0; i < matches.size(); ++i) {
            CRPC_CHECK_EQ(v.spans[i].first, matches[i].begin);
            CRPC_CHECK_EQ(v.spans[i].second, matches[i].end);
        }
    }
}

CRPC_TEST(regex_replace_matches_python_vectors) {
    for (const replace_vector& v : replace_vectors) {
        const auto compiled = regex::compile(v.pattern, with_flags(v.flags));
        CRPC_REQUIRE(compiled.ok());
        const auto first = compiled.value().replace_first(v.text, v.replacement);
        const auto all = compiled.value().replace_all(v.text, v.replacement);
        CRPC_REQUIRE(first.ok());
        CRPC_REQUIRE(all.ok());
        CRPC_CHECK_EQ(std::string(v.first), first.value());
        CRPC_CHECK_EQ(std::string(v.all), all.value());
    }
}

CRPC_TEST(regex_is_match_agrees_with_search) {
    const auto compiled = regex::compile("a+b");
    CRPC_REQUIRE(compiled.ok());
    CRPC_CHECK(compiled.value().is_match("xxaaabyy"));
    CRPC_CHECK_FALSE(compiled.value().is_match("xxaaacyy"));
    CRPC_CHECK(compiled.value().is_match("ab"));
    CRPC_CHECK_FALSE(compiled.value().is_match(""));
}

CRPC_TEST(regex_empty_pattern_and_empty_text) {
    const auto empty = regex::compile("");
    CRPC_REQUIRE(empty.ok());
    const auto on_text = empty.value().search("abc");
    CRPC_REQUIRE(on_text.ok());
    CRPC_CHECK(on_text.value().matched);
    CRPC_CHECK_EQ(std::size_t{0}, on_text.value().begin);
    CRPC_CHECK_EQ(std::size_t{0}, on_text.value().end);

    const auto on_empty = empty.value().search("");
    CRPC_REQUIRE(on_empty.ok());
    CRPC_CHECK(on_empty.value().matched);

    // An empty pattern replaces at every position; the zero-width advance is
    // what makes that terminate.
    const auto replaced = empty.value().replace_all("abc", "-");
    CRPC_REQUIRE(replaced.ok());
    CRPC_CHECK_EQ(std::string("-a-b-c-"), replaced.value());
}

CRPC_TEST(regex_whole_string_match_through_anchors) {
    const auto compiled = regex::compile("^[a-z]+$");
    CRPC_REQUIRE(compiled.ok());
    const auto hit = compiled.value().search("abcdef");
    CRPC_REQUIRE(hit.ok());
    CRPC_CHECK(hit.value().matched);
    CRPC_CHECK_EQ(std::size_t{0}, hit.value().begin);
    CRPC_CHECK_EQ(std::size_t{6}, hit.value().end);
    const auto partial = compiled.value().search("abc1");
    CRPC_REQUIRE(partial.ok());
    CRPC_CHECK_FALSE(partial.value().matched);
}

CRPC_TEST(regex_quantifier_greedy_versus_lazy) {
    const auto greedy = regex::compile("a+");
    const auto lazy = regex::compile("a+?");
    CRPC_REQUIRE(greedy.ok());
    CRPC_REQUIRE(lazy.ok());
    const auto greedy_match = greedy.value().search("baaaac");
    const auto lazy_match = lazy.value().search("baaaac");
    CRPC_REQUIRE(greedy_match.ok());
    CRPC_REQUIRE(lazy_match.ok());
    CRPC_CHECK_EQ(std::size_t{1}, greedy_match.value().begin);
    CRPC_CHECK_EQ(std::size_t{5}, greedy_match.value().end);
    CRPC_CHECK_EQ(std::size_t{1}, lazy_match.value().begin);
    CRPC_CHECK_EQ(std::size_t{2}, lazy_match.value().end);

    const auto bounded = regex::compile("a{2,3}");
    const auto bounded_lazy = regex::compile("a{2,3}?");
    CRPC_REQUIRE(bounded.ok());
    CRPC_REQUIRE(bounded_lazy.ok());
    const auto bounded_match = bounded.value().search("aaaa");
    const auto bounded_lazy_match = bounded_lazy.value().search("aaaa");
    CRPC_REQUIRE(bounded_match.ok());
    CRPC_REQUIRE(bounded_lazy_match.ok());
    CRPC_CHECK_EQ(std::size_t{3}, bounded_match.value().end);
    CRPC_CHECK_EQ(std::size_t{2}, bounded_lazy_match.value().end);
}

CRPC_TEST(regex_multiline_and_dotall_are_independent) {
    const auto plain = regex::compile("a.b");
    const auto dotall = regex::compile("a.b", [] {
        regex_options options;
        options.dotall = true;
        return options;
    }());
    CRPC_REQUIRE(plain.ok());
    CRPC_REQUIRE(dotall.ok());
    const auto plain_match = plain.value().search("a\nb");
    const auto dotall_match = dotall.value().search("a\nb");
    CRPC_REQUIRE(plain_match.ok());
    CRPC_REQUIRE(dotall_match.ok());
    CRPC_CHECK_FALSE(plain_match.value().matched);
    CRPC_CHECK(dotall_match.value().matched);

    regex_options multiline_options;
    multiline_options.multiline = true;
    const auto multiline = regex::compile("^b", multiline_options);
    CRPC_REQUIRE(multiline.ok());
    const auto off = regex::compile("^b");
    CRPC_REQUIRE(off.ok());
    const auto on_match = multiline.value().search("a\nb");
    const auto off_match = off.value().search("a\nb");
    CRPC_REQUIRE(on_match.ok());
    CRPC_REQUIRE(off_match.ok());
    CRPC_CHECK(on_match.value().matched);
    CRPC_CHECK_FALSE(off_match.value().matched);
}

CRPC_TEST(regex_icase_applies_to_literals_classes_and_backrefs) {
    regex_options options;
    options.icase = true;
    const auto literal = regex::compile("abc", options);
    const auto klass = regex::compile("[a-c]+", options);
    const auto backref = regex::compile("(a)\\1", options);
    CRPC_REQUIRE(literal.ok());
    CRPC_REQUIRE(klass.ok());
    CRPC_REQUIRE(backref.ok());
    CRPC_CHECK(literal.value().is_match("xAbC"));
    CRPC_CHECK(klass.value().is_match("ABC"));
    CRPC_CHECK(backref.value().is_match("aA"));

    // Case folding is ASCII-only: a high byte is never folded.
    CRPC_CHECK_FALSE(regex::compile("\xC3\x89", options)
                         .value()
                         .is_match("\xC3\xA9"));
}

CRPC_TEST(regex_pattern_is_preserved) {
    const std::string source = "(a)[b-d]+\\1(?:x)";
    const auto compiled = regex::compile(source);
    CRPC_REQUIRE(compiled.ok());
    CRPC_CHECK_EQ(source, std::string(compiled.value().pattern()));
    CRPC_CHECK_EQ(source.size(), compiled.value().pattern().size());
}

CRPC_TEST(regex_compile_rejects_malformed_patterns) {
    // Syntax errors are decode_error, with the offending offset in the message.
    const char* malformed[] = {
        "[abc",      // unterminated class
        "[]",        // an empty class is not a class
        "[^]",       // nor is a negated empty one
        "[z-a]",     // reversed range
        "(abc",      // unterminated group
        "abc)",      // unmatched ')'
        "(?=a)",     // lookahead is not supported
        "(?P<n>a)",  // named groups are not supported
        "*a",        // a quantifier with no atom
        "a**",       // stacked quantifiers
        "a{2}{3}",   // ditto
        "a{2,1}",    // inverted bounds
        "a{",        // a '{' that is not a quantifier
        "a{2",       // unterminated
        "a{,3}",     // no lower bound
        "\\",        // trailing backslash
        "\\q",       // not an escape
        "\\1",       // no such group
        "\\1(a)",    // referenced before it is closed
        "(a\\1)",    // referenced inside itself
        "^*",        // quantified anchor
        "a\\b*",     // quantified word boundary
    };
    for (const char* pattern : malformed) {
        const auto compiled = regex::compile(pattern);
        if (compiled.ok()) {
            CRPC_CHECK_FALSE(compiled.ok());
            continue;
        }
        CRPC_CHECK(compiled.error().code == crpc::rpc_code::decode_error);
        CRPC_CHECK(!compiled.error().message.empty());
    }
}

CRPC_TEST(regex_compile_enforces_limits) {
    // A pattern longer than the length cap is a resource limit.
    std::string long_pattern(crpc::text::regex_max_pattern_length + 1, 'a');
    const auto too_long = regex::compile(long_pattern);
    CRPC_CHECK_FALSE(too_long.ok());
    CRPC_CHECK(too_long.error().code == crpc::rpc_code::resource_exhausted);

    // Ten groups are allowed, eleven are not.
    std::string ten;
    for (int i = 1; i <= 10; ++i) {
        ten += "(a)";
    }
    const auto ten_groups = regex::compile(ten);
    CRPC_CHECK(ten_groups.ok());
    if (ten_groups.ok()) {
        const auto found = ten_groups.value().search("aaaaaaaaaa");
        CRPC_REQUIRE(found.ok());
        CRPC_CHECK(found.value().matched);
        CRPC_CHECK_EQ(std::size_t{10}, found.value().groups.size());
    }
    const auto eleven_groups = regex::compile(ten + "(a)");
    CRPC_CHECK_FALSE(eleven_groups.ok());
    if (!eleven_groups.ok()) {
        CRPC_CHECK(eleven_groups.error().code ==
                   crpc::rpc_code::resource_exhausted);
    }

    // A repeat count beyond the limit is a resource error, not a syntax error.
    const auto huge_repeat = regex::compile("a{99999}");
    CRPC_CHECK_FALSE(huge_repeat.ok());
    if (!huge_repeat.ok()) {
        CRPC_CHECK(huge_repeat.error().code ==
                   crpc::rpc_code::resource_exhausted);
    }

    // Deep nesting is bounded too.
    std::string nested;
    for (int i = 0; i < 100; ++i) {
        nested += "(?:";
    }
    nested += "a";
    for (int i = 0; i < 100; ++i) {
        nested += ")";
    }
    const auto deep = regex::compile(nested);
    CRPC_CHECK_FALSE(deep.ok());
    if (!deep.ok()) {
        CRPC_CHECK(deep.error().code == crpc::rpc_code::resource_exhausted);
    }
}

CRPC_TEST(regex_quantified_anchor_is_rejected) {
    const auto bad = regex::compile("^*");
    CRPC_CHECK_FALSE(bad.ok());
    if (!bad.ok()) {
        CRPC_CHECK(bad.error().code == crpc::rpc_code::decode_error);
    }
}

CRPC_TEST(regex_replace_template_errors) {
    const auto compiled = regex::compile("(a)");
    CRPC_REQUIRE(compiled.ok());
    // A group reference past the pattern's groups is invalid_argument.
    const auto too_high = compiled.value().replace_first("a", "$2");
    CRPC_CHECK_FALSE(too_high.ok());
    if (!too_high.ok()) {
        CRPC_CHECK(too_high.error().code == crpc::rpc_code::invalid_argument);
    }
    // A lone backslash is invalid_argument too.
    const auto lone_backslash = compiled.value().replace_all("a", "\\");
    CRPC_CHECK_FALSE(lone_backslash.ok());
    if (!lone_backslash.ok()) {
        CRPC_CHECK(lone_backslash.error().code ==
                   crpc::rpc_code::invalid_argument);
    }
    // A literal '$' not followed by a digit is not an error.
    const auto literal = compiled.value().replace_first("a", "$x");
    CRPC_REQUIRE(literal.ok());
    CRPC_CHECK_EQ(std::string("$x"), literal.value());
}

CRPC_TEST(regex_catastrophic_backtracking_is_bounded) {
    // `(a+)+b` over a long run of 'a' with no 'b' is the textbook exponential
    // backtracking case. Without the step budget this call would not return.
    regex_options options;
    options.max_steps = 100'000;
    const auto compiled = regex::compile("(a+)+b", options);
    CRPC_REQUIRE(compiled.ok());

    const std::string text(4'000, 'a');
    const auto started = std::chrono::steady_clock::now();
    const auto found = compiled.value().search(text);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    CRPC_REQUIRE(!found.ok());
    CRPC_CHECK(found.error().code == crpc::rpc_code::resource_exhausted);
    // The message reports the budget it stopped at, and the number of steps
    // actually executed is the budget (the guard tests before every step).
    // The full "steps=N, max_steps=M" fragment is required: matching a bare
    // "steps=100000" would also hit inside "max_steps=100000" and could hide
    // an off-by-one in the executed-step count.
    CRPC_CHECK(found.error().message.find("steps=100000, max_steps=100000") !=
               std::string::npos);
    // A 100k-step budget is milliseconds of work; ten seconds is an
    // extraordinarily generous ceiling that still fails a real hang.
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            elapsed)
                            .count();
    CRPC_CHECK(millis < 10'000);
}

CRPC_TEST(regex_catastrophic_pattern_still_matches_when_it_should) {
    // The same pattern must still work when the budget is ample and the text
    // does match: the guard cannot be a blanket rejection.
    regex_options options;
    options.max_steps = 1'000'000;
    const auto compiled = regex::compile("(a+)+b", options);
    CRPC_REQUIRE(compiled.ok());
    const auto found = compiled.value().search("aaaaab");
    CRPC_REQUIRE(found.ok());
    CRPC_CHECK(found.value().matched);
    CRPC_CHECK_EQ(std::size_t{0}, found.value().begin);
    CRPC_CHECK_EQ(std::size_t{6}, found.value().end);
}

CRPC_TEST(regex_step_budget_does_not_break_linear_patterns) {
    // A long text through a simple pattern stays far below a small budget.
    regex_options options;
    options.max_steps = 100'000;
    const auto compiled = regex::compile("[a-z]+", options);
    CRPC_REQUIRE(compiled.ok());
    const std::string text(3'000, 'a');
    const auto found = compiled.value().search(text);
    CRPC_REQUIRE(found.ok());
    CRPC_CHECK(found.value().matched);
    CRPC_CHECK_EQ(std::size_t{3'000}, found.value().end);
}

CRPC_TEST(regex_find_all_advances_past_empty_matches) {
    // A zero-width match must advance the scan or this never returns.
    const auto compiled = regex::compile("(a?)*");
    CRPC_REQUIRE(compiled.ok());
    const auto found = compiled.value().find_all("aa");
    CRPC_REQUIRE(found.ok());
    CRPC_REQUIRE(found.value().size() == 2);
    CRPC_CHECK_EQ(std::size_t{0}, found.value()[0].begin);
    CRPC_CHECK_EQ(std::size_t{2}, found.value()[0].end);
    CRPC_CHECK_EQ(std::size_t{2}, found.value()[1].begin);
    CRPC_CHECK_EQ(std::size_t{2}, found.value()[1].end);
}

CRPC_TEST(regex_search_reports_absent_match_without_error) {
    const auto compiled = regex::compile("needle");
    CRPC_REQUIRE(compiled.ok());
    const auto found = compiled.value().search("haystack");
    CRPC_REQUIRE(found.ok());
    CRPC_CHECK_FALSE(found.value().matched);
    CRPC_CHECK(found.value().groups.empty());
}

CRPC_TEST(regex_matches_std_string_view_without_copying) {
    // The whole API eats string_view: a substring of a buffer matches in
    // place, with the reported offsets relative to the view.
    const std::string buffer = "--start-of-text--";
    const std::string_view view(buffer.data() + 2, 5);  // "start"
    const auto compiled = regex::compile("tart");
    CRPC_REQUIRE(compiled.ok());
    const auto found = compiled.value().search(view);
    CRPC_REQUIRE(found.ok());
    CRPC_CHECK(found.value().matched);
    CRPC_CHECK_EQ(std::size_t{1}, found.value().begin);
}

CRPC_TEST(regex_returns_crpc_result_and_never_throws) {
    // Compiling a bad pattern on a non-throwing code path: the outcome is a
    // value, there is no try/catch anywhere in this case.
    const auto bad = regex::compile("(");
    CRPC_CHECK_FALSE(bad.ok());
    CRPC_CHECK(bad.code() == crpc::rpc_code::decode_error);

    // A moved-from object still answers safely (it holds no pattern).
    auto source = regex::compile("a");
    CRPC_REQUIRE(source.ok());
    regex live = std::move(source.value());
    CRPC_CHECK(live.is_match("a"));
    const regex empty = std::move(live);
    CRPC_CHECK_FALSE(live.is_match("anything"));
    CRPC_CHECK_EQ(std::string(), std::string(live.pattern()));
    const auto searched = live.search("x");
    CRPC_CHECK_FALSE(searched.ok());
    if (!searched.ok()) {
        CRPC_CHECK(searched.error().code == crpc::rpc_code::failed_precondition);
    }
    CRPC_CHECK(empty.is_match("a"));
}

CRPC_TEST(regex_copies_and_moves_are_independent_and_shareable) {
    const auto compiled = regex::compile("(\\d+)");
    CRPC_REQUIRE(compiled.ok());
    regex copy = compiled.value();
    regex moved = std::move(copy);
    const auto found = moved.search("abc123");
    CRPC_REQUIRE(found.ok());
    CRPC_CHECK(found.value().matched);
    CRPC_CHECK_EQ(std::size_t{1}, found.value().groups.size());
    CRPC_CHECK_EQ(std::size_t{3}, found.value().groups[0].first);
    CRPC_CHECK_EQ(std::size_t{6}, found.value().groups[0].second);
}

CRPC_TEST(regex_arithmetic_alternation_prefers_left) {
    const auto compiled = regex::compile("a|ab");
    CRPC_REQUIRE(compiled.ok());
    const auto found = compiled.value().search("ab");
    CRPC_REQUIRE(found.ok());
    CRPC_CHECK_EQ(std::size_t{1}, found.value().end);
}

CRPC_TEST(regex_backreference_to_unset_group_never_matches) {
    const auto compiled = regex::compile("(a)?b\\1");
    CRPC_REQUIRE(compiled.ok());
    const auto found = compiled.value().search("b");
    CRPC_REQUIRE(found.ok());
    CRPC_CHECK_FALSE(found.value().matched);
}

CRPC_TEST(regex_multiple_of_same_group_keeps_last_iteration) {
    // Python keeps the last iteration's capture, not the first.
    const auto compiled = regex::compile("(a|b)*");
    CRPC_REQUIRE(compiled.ok());
    const auto found = compiled.value().search("ab");
    CRPC_REQUIRE(found.ok());
    CRPC_REQUIRE(found.value().matched);
    CRPC_REQUIRE(found.value().groups.size() == 1);
    CRPC_CHECK_EQ(std::size_t{1}, found.value().groups[0].first);
    CRPC_CHECK_EQ(std::size_t{2}, found.value().groups[0].second);
}

CRPC_TEST(regex_character_class_escapes_inside_brackets) {
    const auto compiled = regex::compile("[\\d\\s]+");
    CRPC_REQUIRE(compiled.ok());
    const auto found = compiled.value().search("x1 2y");
    CRPC_REQUIRE(found.ok());
    CRPC_CHECK(found.value().matched);
    CRPC_CHECK_EQ(std::size_t{1}, found.value().begin);
    CRPC_CHECK_EQ(std::size_t{4}, found.value().end);

    // A ']' first in the class is a literal (Python: '[]]' is a valid class).
    const auto bracket = regex::compile("[]]+");
    CRPC_REQUIRE(bracket.ok());
    CRPC_CHECK(bracket.value().is_match("]]"));
}

CRPC_TEST(regex_escaped_punctuation_is_literal) {
    const char* pairs[][2] = {
        {"\\.", "."},
        {"\\*", "*"},
        {"\\+", "+"},
        {"\\?", "?"},
        {"\\(", "("},
        {"\\)", ")"},
        {"\\[", "["},
        {"\\]", "]"},
        {"\\{", "{"},
        {"\\}", "}"},
        {"\\|", "|"},
        {"\\\\", "\\"},
        {"\\^", "^"},
        {"\\$", "$"},
    };
    for (const auto& pair : pairs) {
        const auto compiled = regex::compile(pair[0]);
        if (!CRPC_CHECK(compiled.ok())) {
            continue;
        }
        const auto found = compiled.value().search(pair[1]);
        CRPC_CHECK(found.ok() && found.value().matched);
    }
}

CRPC_TEST(regex_step_budget_is_per_start_position) {
    // The budget bounds one attempt, so a scan over many start positions is
    // not rejected just because the attempts add up to more than the budget.
    // Each attempt here is a couple of steps; the whole scan is far longer.
    regex_options options;
    options.max_steps = 1'000;
    const auto compiled = regex::compile("nice", options);
    CRPC_REQUIRE(compiled.ok());
    std::string text(20'000, 'x');
    text += "nice";
    const auto found = compiled.value().search(text);
    CRPC_REQUIRE(found.ok());
    CRPC_CHECK(found.value().matched);
    CRPC_CHECK_EQ(std::size_t{20'000}, found.value().begin);
}

CRPC_TEST_MAIN()
