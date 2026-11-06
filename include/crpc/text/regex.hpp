// CRPC -- a small backtracking regular expression engine with a step budget.
//
// std::regex is usable but awkward for this library's callers, in three
// specific ways, and this header fixes exactly those:
//
//   1. std::regex drags in `std::regex_constants` (`syntax_option_type`,
//      `match_flag_type`, ...) and a second type (`std::smatch`) that exists
//      only to name the result. Here there is one class, `regex`, and one
//      result value, `regex_match`.
//   2. std::regex only accepts `std::basic_string`, so every search copies or
//      materialises a buffer. Every function here takes `std::string_view`, so
//      a `const char*` buffer, a `std::string` and a substring view are all
//      matched in place with no allocation.
//   3. std::regex reports a bad pattern by throwing `std::regex_error`. This
//      library never throws across a boundary, so `compile` returns
//      `result<regex>` and pattern errors are ordinary values.
//
// The engine is a backtracking one: alternation, greedy and lazy quantifiers
// and back references all behave the way PCRE/Python users expect, which is
// what a Thompson NFA (the linear-time alternative) cannot give without
// changing match results. The price of backtracking is worst-case exponential
// time, so every match attempt carries a hard step budget: a pathological
// pattern such as `(a+)+b` is stopped with `rpc_code::resource_exhausted`
// instead of running forever. That trade-off is deliberate and is the reason
// the budget is part of the public options instead of an internal constant.
//
// Scope: the engine is byte-oriented and ASCII-only. `icase` folds A-Z only,
// `.` and the character classes work on bytes, and no Unicode property is
// interpreted. Input is not required to be valid UTF-8; it is matched as the
// bytes it is.
//
// Supported syntax:
//   * literals and `.`;
//   * character classes `[abc]`, `[^abc]`, ranges `[a-z]`, and the escapes
//     `\d \w \s \D \W \S \n \t \r \f \v` inside and outside a class;
//   * an escaped punctuation character is that literal (`\.`, `\*`, `\[`,
//     `\\`, ...);
//   * quantifiers `*`, `+`, `?`, `{n}`, `{n,}`, `{n,m}`, each optionally
//     followed by `?` for the lazy form;
//   * groups `(...)`, non-capturing groups `(?:...)`, alternation `|`;
//   * anchors `^`, `$` and word boundaries `\b`, `\B`;
//   * back references `\1`..`\9`.
//
// Deliberately unsupported (rejected with `rpc_code::decode_error`):
//   * `(?=...)`, `(?!...)`, `(?<=...)`, `(?<!...)` lookaround, and any other
//     `(?` form except `(?:`;
//   * named groups, `\p{...}`, `\xHH`, `\uHHHH`, octal escapes and `\0`;
//   * `{,m}` (a quantifier with no lower bound) -- the lower bound is
//     mandatory, and a `{` that does not start a valid quantifier is an
//     error, so a literal `{` must be written `\{`;
//   * a second quantifier applied to a quantified atom (`a*+`) and any
//     quantifier applied to an anchor or a word boundary: both are errors.
//
// Both a character class and a quantifier are strict on purpose: silently
// treating a malformed `{` or `[` as a literal is how a typo becomes a
// production bug, and a compile call that cannot fail is worth more than one
// that guesses.
//
// One behavioral deviation from Python's `re` remains: when a counted lazy
// quantifier such as `(...){1,2}?` wraps a body that can match empty through a
// nested quantifier (for example `((.)*?){1,2}?`), Python's REPEAT frame is
// updated in place, so after a failed tail attempt Python resumes the inner
// quantifier of the *earlier* iteration before trying the next outer
// iteration. This engine keeps one LIFO backtrack frame per outer iteration,
// so the next outer iteration is resumed first. Match spans agree; only the
// last-participation span of a capture group inside the wrapped body can
// differ in that nested case. Every other quantifier interaction (greedy vs
// lazy ordering, empty-iteration cutoff, bounded repeats) is validated
// differentially against CPython's `re` module.
#ifndef CRPC_TEXT_REGEX_HPP
#define CRPC_TEXT_REGEX_HPP

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace text {

// Match behaviour. The defaults match the common case: case-sensitive,
// `^`/`$` anchor the whole text and `.` does not match a newline.
struct regex_options {
    // ASCII case-insensitive matching: literals, character classes and back
    // references all fold A-Z. Non-ASCII bytes are never folded.
    bool icase = false;
    // `^` and `$` also match at the start and the end of each line.
    bool multiline = false;
    // `.` also matches '\n'.
    bool dotall = false;
    // Upper bound on the number of engine steps one match attempt may run.
    // The budget is per starting position, not per call: a scan that tries
    // n+1 start positions runs at most (n+1) * max_steps steps, but the first
    // attempt that exhausts its own budget aborts the whole call with
    // `rpc_code::resource_exhausted`. That is what bounds a catastrophic
    // backtracking pattern, because such a pattern explodes inside a single
    // starting position.
    std::size_t max_steps = 1'000'000;
};

// Hard limits. Exceeding any of them during compile is
// `rpc_code::resource_exhausted` (they are bounds, not bugs in the pattern).
// They exist so that a hostile or generated pattern cannot make `compile`
// allocate without bound. They are part of the interface so a caller can check
// a pattern against the same numbers the engine uses.
inline constexpr std::size_t regex_max_pattern_length = 8192;
inline constexpr std::size_t regex_max_groups = 10;
inline constexpr std::size_t regex_max_program_size = 65536;
inline constexpr std::size_t regex_max_repeat = 65535;

// One match. `begin`/`end` are byte offsets into the searched text and
// `matched` says whether this value describes a match at all: `search` returns
// an ok result with `matched == false` when the pattern is simply not present.
//
// `groups` has exactly one entry per capture group in the pattern, in group
// order, so `groups.size()` is the group count. A group that did not take part
// in the match is `{npos, npos}`; `groups` is empty when `matched` is false.
struct regex_match {
    // The value used for a capture group that did not participate.
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    std::size_t begin = 0;
    std::size_t end = 0;
    bool matched = false;
    std::vector<std::pair<std::size_t, std::size_t>> groups;
};

// A compiled pattern. Immutable and safe to share between threads: matching
// keeps all of its state on the stack, so concurrent `search`/`find_all`/
// `replace_*` calls on one object are safe.
class CRPC_API regex {
public:
    // Compiles `pattern`. The pattern is copied; the view need not outlive the
    // call. Malformed patterns are `rpc_code::decode_error` with the byte
    // offset in the message; hitting one of the limits above is
    // `rpc_code::resource_exhausted`.
    static result<regex> compile(std::string_view pattern,
                                 regex_options options = {});

    regex(const regex&) = default;
    regex(regex&&) noexcept = default;
    regex& operator=(const regex&) = default;
    regex& operator=(regex&&) noexcept = default;
    ~regex() = default;

    // True when `text` contains a match. This signature has no error channel,
    // so an exhausted step budget (see `regex_options::max_steps`) reads as
    // "no match"; call `search` when the distinction matters.
    bool is_match(std::string_view text) const;

    // First match in `text`, scanning start positions left to right. Returns
    // ok with `matched == false` when there is none, and
    // `rpc_code::resource_exhausted` when an attempt exceeds its step budget.
    result<regex_match> search(std::string_view text) const;

    // Every non-overlapping match, left to right. An empty match advances the
    // scan by one byte (a match at [p,p) is followed by a search starting at
    // p+1), which is what stops a zero-width pattern from looping.
    result<std::vector<regex_match>> find_all(std::string_view text) const;

    // Replaces the first match / every match with `replacement`, in which `$0`
    // through `$9` expand to the corresponding capture (with `$0` the whole
    // match, and an unset group expanding to nothing) and `\\` is a literal
    // backslash. `$` not followed by a digit is a literal `$`; a backslash not
    // followed by a backslash, and a group reference past the pattern's group
    // count, are `rpc_code::invalid_argument`.
    result<std::string> replace_first(std::string_view text,
                                      std::string_view replacement) const;
    result<std::string> replace_all(std::string_view text,
                                    std::string_view replacement) const;

    // The source text that was compiled. The view is valid as long as this
    // object (or any copy of it) is alive; a moved-from object returns an
    // empty view.
    std::string_view pattern() const noexcept;

private:
    struct impl;

    explicit regex(std::shared_ptr<const impl> compiled) noexcept;

    std::shared_ptr<const impl> impl_;
};

} // namespace text
} // namespace crpc

#endif // CRPC_TEXT_REGEX_HPP
