// CRPC -- common string operations.
//
// All operations are byte-oriented (there is no full Unicode decomposition
// here), but they are safe on UTF-8: trimming and splitting operate on byte
// boundaries and a multi-byte sequence is never split mid-sequence unless the
// caller's delimiter literally falls inside one.
#ifndef CRPC_TEXT_STRING_OPS_HPP
#define CRPC_TEXT_STRING_OPS_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace text {

// Removes leading/trailing (or both) characters that appear in `charset`.
// `charset` is a byte set; to trim a multi-byte character pass its full UTF-8
// sequence(s). The default set is the ASCII whitespace. Returns a view into
// the input; the input must outlive the result.
[[nodiscard]] CRPC_API std::string_view
trim_left(std::string_view text, std::string_view charset = " \t\r\n\v\f") noexcept;
[[nodiscard]] CRPC_API std::string_view
trim_right(std::string_view text, std::string_view charset = " \t\r\n\v\f") noexcept;
[[nodiscard]] CRPC_API std::string_view
trim(std::string_view text, std::string_view charset = " \t\r\n\v\f") noexcept;

// C++17 prefix/suffix checks (std::string_view gets these natively in C++20;
// until then the library provides its own to keep the C++17 default).
[[nodiscard]] inline bool starts_with(std::string_view text,
                                      std::string_view prefix) noexcept {
    return text.size() >= prefix.size() &&
           text.compare(0, prefix.size(), prefix) == 0;
}

[[nodiscard]] inline bool ends_with(std::string_view text,
                                    std::string_view suffix) noexcept {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Splits `text` at every occurrence of `delimiter`. Empty tokens between two
// consecutive delimiters are kept; a single trailing delimiter does not
// produce a trailing empty token, and an empty input yields an empty result.
// The returned views reference `text`; it must outlive them.
[[nodiscard]] CRPC_API std::vector<std::string_view>
split(std::string_view text, char delimiter);

// Same contract with a multi-character delimiter. An empty delimiter returns a
// single view of the whole input.
[[nodiscard]] CRPC_API std::vector<std::string_view>
split(std::string_view text, std::string_view delimiter);

// Joins a range of string-like elements with `separator` between them.
// Elements may be std::string, std::string_view or anything convertible to
// std::string_view.
template <typename Range>
[[nodiscard]] std::string join(const Range& parts, std::string_view separator) {
    std::string out;
    bool first = true;
    for (const auto& part : parts) {
        if (!first) {
            out.append(separator.data(), separator.size());
        }
        first = false;
        const std::string_view view(part);
        out.append(view.data(), view.size());
    }
    return out;
}

// Replaces every non-overlapping occurrence of `from` in `text` with `to`.
// An empty `from` is `invalid_argument` (replacement would be ambiguous).
[[nodiscard]] CRPC_API result<std::string>
replace_all(std::string_view text, std::string_view from, std::string_view to);

} // namespace text
} // namespace crpc

#endif // CRPC_TEXT_STRING_OPS_HPP