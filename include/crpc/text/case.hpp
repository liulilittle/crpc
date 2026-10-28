// CRPC -- Unicode-aware case mapping and case-insensitive comparison.
//
// Scope: ASCII plus the Latin-1 supplement. Full Unicode case folding needs
// the complete CaseFolding table (hundreds of multi-code-point entries); that
// is intentionally out of scope here. Specifically:
//
//   * ASCII (U+0000..U+007F) maps with the standard one-to-one rule;
//   * Latin-1 letters with a single-code-point counterpart inside
//     U+00C0..U+00FF map both ways (A<->a, À<->à, Ö<->ö, ...);
//   * U+00DF (ß) and U+00FF (ÿ) have no single-code-point Latin-1
//     counterpart and are left unchanged, as is everything above U+00FF
//     (Greek, Cyrillic, CJK, ...).
//
// Anything outside that boundary compares strictly. The functions never fail
// and never crash on malformed UTF-8: input that is not well-formed passes
// through byte-for-byte unchanged.
#ifndef CRPC_TEXT_CASE_HPP
#define CRPC_TEXT_CASE_HPP

#include <string>
#include <string_view>

#include <crpc/config.hpp>

namespace crpc {
namespace text {

// True when every byte of `text` is in U+0000..U+007F. The fast path used by
// `to_lower`/`to_upper`; it lets ASCII-heavy workloads avoid the slow loop.
[[nodiscard]] CRPC_API bool is_ascii(std::string_view text) noexcept;

// Lower/upper-cases ASCII + Latin-1 (see the header comment for the exact
// boundary). Returns a new string; length and byte layout of non-ASCII
// multi-byte sequences are preserved.
[[nodiscard]] CRPC_API std::string to_lower(std::string_view text);
[[nodiscard]] CRPC_API std::string to_upper(std::string_view text);

// ASCII + Latin-1 case-insensitive comparisons. Case folding never changes the
// length of a string, so byte-length prefixes compare directly.
[[nodiscard]] CRPC_API bool iequals(std::string_view a, std::string_view b) noexcept;
[[nodiscard]] CRPC_API bool istarts_with(std::string_view text,
                                         std::string_view prefix) noexcept;
[[nodiscard]] CRPC_API bool iends_with(std::string_view text,
                                       std::string_view suffix) noexcept;

} // namespace text
} // namespace crpc

#endif // CRPC_TEXT_CASE_HPP