// CRPC -- string ordering modelled on .NET's StringComparer / StringComparison.
//
// What is exact and what is approximate, stated up front (this is the whole
// point of the file; do not read the culture modes as real collation):
//
//   ordinal                    EXACT. Compares the UTF-8 bytes, exactly
//                              std::string_view's operator<. For ASCII this is
//                              identical to .NET's Ordinal. For non-ASCII it
//                              differs from .NET in one narrow way: .NET
//                              compares UTF-16 *code units*, so a supplementary
//                              character (UTF-16 surrogate pair, U+D800..U+DFFF)
//                              sorts before U+E000..U+FFFF, while UTF-8 byte
//                              order sorts it after (UTF-8 order is Unicode
//                              code point order). Everything in the BMP
//                              compares identically, because there the code
//                              unit equals the code point.
//   ordinal_ignore_case        ASCII case folding only: 'A'..'Z' and 'a'..'z'
//                              are folded, every other byte is compared
//                              exactly. This matches .NET OrdinalIgnoreCase for
//                              ASCII. It diverges for non-ASCII: .NET folds
//                              with the invariant Unicode casing table (so it
//                              treats U+00C4 A-umlaut as equal to U+00E4),
//                              while this implementation compares those bytes
//                              exactly. The narrower behaviour is deliberate:
//                              it needs no casing table and cannot silently
//                              get a table entry wrong.
//   invariant_culture          APPROXIMATE, NOT linguistic collation. Compares
//   current_culture            Unicode code points in ascending order. For
//                              valid UTF-8 this is the same order as ordinal
//                              byte comparison; what is missing is all of the
//                              culture behaviour: no collation weights, no
//                              treatment of hyphens/apostrophes/whitespace as
//                              ignorable, no accent or case-level comparison,
//                              no contractions. Current culture has no locale
//                              parameter here at all, so it behaves exactly like
//                              invariant. Do not use these modes for
//                              user-visible sorting where a real collator
//                              (ICU, or the platform's CompareStringEx) is
//                              required.
//   invariant_culture_ignore_case   APPROXIMATE, as above plus ASCII case
//   current_culture_ignore_case     folding.
//
// All comparisons are total: invalid UTF-8 never throws, never reads past the
// end, and never produces an inconsistent result. An invalid byte is compared
// as a token above every valid code point (0x110000 + byte), so a malformed
// string still has a stable place in the order.
//
// Hash contract: `equals(a, b)` implies `hash(a) == hash(b)` for the same
// string_order instance. The hash is FNV-1a over the (case-folded, for the
// ignore-case modes) bytes; it is stable within a program, not across builds,
// and is not a security primitive (no randomised seed -- use a keyed hash for
// untrusted keys, e.g. the hashtable's own seed).
#ifndef CRPC_TEXT_STRING_ORDER_HPP
#define CRPC_TEXT_STRING_ORDER_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace crpc {
namespace text {

// Mirrors .NET's System.StringComparison. `current_culture` has no locale
// argument: the process culture is not modelled here (see the file comment).
enum class string_comparison : std::uint8_t {
    ordinal,
    ordinal_ignore_case,
    invariant_culture,
    invariant_culture_ignore_case,
    current_culture,
    current_culture_ignore_case
};

namespace detail {

constexpr bool order_ignores_case(string_comparison comparison) noexcept {
    return comparison == string_comparison::ordinal_ignore_case ||
           comparison == string_comparison::invariant_culture_ignore_case ||
           comparison == string_comparison::current_culture_ignore_case;
}

constexpr bool order_is_linguistic(string_comparison comparison) noexcept {
    return comparison == string_comparison::invariant_culture ||
           comparison == string_comparison::invariant_culture_ignore_case ||
           comparison == string_comparison::current_culture ||
           comparison == string_comparison::current_culture_ignore_case;
}

constexpr std::uint8_t fold_ascii(std::uint8_t byte) noexcept {
    return (byte >= 'a' && byte <= 'z')
               ? static_cast<std::uint8_t>(byte - 'a' + 'A')
               : byte;
}

// Decodes one strict UTF-8 sequence starting at `position`. On success advances
// `position` and returns the code point. On an invalid byte (bad lead byte,
// truncated sequence, bad continuation, overlong form, surrogate, > U+10FFFF)
// advances by exactly one byte and returns 0x110000 + byte, a value above every
// code point, so invalid bytes order after valid text but keep a total order.
inline std::uint32_t next_order_token(std::string_view text,
                                      std::size_t& position) noexcept {
    const auto byte_at = [&text](std::size_t index) noexcept {
        return static_cast<std::uint8_t>(text[index]);
    };
    const std::uint8_t lead = byte_at(position);
    std::size_t length = 0;
    std::uint32_t code_point = 0;
    std::uint32_t minimum = 0;
    if (lead < 0x80) {
        ++position;
        return lead;
    }
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
        code_point = lead & 0x1F;
        minimum = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        code_point = lead & 0x0F;
        minimum = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        code_point = lead & 0x07;
        minimum = 0x10000;
    } else {
        ++position;
        return 0x110000u + lead;
    }
    if (position + length > text.size()) {
        ++position;
        return 0x110000u + lead;
    }
    for (std::size_t i = 1; i < length; ++i) {
        const std::uint8_t continuation = byte_at(position + i);
        if ((continuation & 0xC0) != 0x80) {
            ++position;
            return 0x110000u + lead;
        }
        code_point = (code_point << 6) | (continuation & 0x3F);
    }
    if (code_point < minimum || code_point > 0x10FFFFu ||
        (code_point >= 0xD800u && code_point <= 0xDFFFu)) {
        ++position;
        return 0x110000u + lead;
    }
    position += length;
    return code_point;
}

// Compares the code points (or the folded bytes for the ordinal ignore-case
// mode). Returns -1, 0 or 1. `ascii_fold` is true for the ignore-case modes.
inline int compare_tokens(std::string_view a, std::string_view b,
                          bool ascii_fold) noexcept {
    std::size_t left = 0;
    std::size_t right = 0;
    while (left < a.size() && right < b.size()) {
        const std::uint32_t lhs = next_order_token(a, left);
        const std::uint32_t rhs = next_order_token(b, right);
        const std::uint32_t lhs_key =
            ascii_fold && lhs < 0x80 ? fold_ascii(static_cast<std::uint8_t>(lhs))
                                     : lhs;
        const std::uint32_t rhs_key =
            ascii_fold && rhs < 0x80 ? fold_ascii(static_cast<std::uint8_t>(rhs))
                                     : rhs;
        if (lhs_key != rhs_key) {
            return lhs_key < rhs_key ? -1 : 1;
        }
    }
    if (left < a.size()) {
        return 1;
    }
    if (right < b.size()) {
        return -1;
    }
    return 0;
}

inline int compare_bytes(std::string_view a, std::string_view b,
                        bool ascii_fold) noexcept {
    const std::size_t common = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < common; ++i) {
        std::uint8_t lhs = static_cast<std::uint8_t>(a[i]);
        std::uint8_t rhs = static_cast<std::uint8_t>(b[i]);
        if (ascii_fold) {
            lhs = fold_ascii(lhs);
            rhs = fold_ascii(rhs);
        }
        if (lhs != rhs) {
            return lhs < rhs ? -1 : 1;
        }
    }
    if (a.size() == b.size()) {
        return 0;
    }
    return a.size() < b.size() ? -1 : 1;
}

constexpr std::uint64_t fnv_offset_basis = 1469598103934665603ull;
constexpr std::uint64_t fnv_prime = 1099511628211ull;

inline std::size_t hash_bytes(std::string_view text, bool ascii_fold) noexcept {
    std::uint64_t state = fnv_offset_basis;
    for (const char raw : text) {
        std::uint8_t byte = static_cast<std::uint8_t>(raw);
        if (ascii_fold) {
            byte = fold_ascii(byte);
        }
        state ^= byte;
        state *= fnv_prime;
    }
    return static_cast<std::size_t>(state);
}

} // namespace detail

// A comparison policy object: construct once per mode and pass it to the
// container or algorithm. Cheap to copy; holds no state beyond the mode.
class string_order {
public:
    explicit string_order(string_comparison comparison =
                              string_comparison::ordinal) noexcept
        : comparison_(comparison) {}

    // Convenience constructors mirroring the StringComparer statics.
    [[nodiscard]] static string_order ordinal() noexcept {
        return string_order(string_comparison::ordinal);
    }
    [[nodiscard]] static string_order ordinal_ignore_case() noexcept {
        return string_order(string_comparison::ordinal_ignore_case);
    }
    [[nodiscard]] static string_order invariant_culture() noexcept {
        return string_order(string_comparison::invariant_culture);
    }
    [[nodiscard]] static string_order invariant_culture_ignore_case() noexcept {
        return string_order(string_comparison::invariant_culture_ignore_case);
    }
    [[nodiscard]] static string_order current_culture() noexcept {
        return string_order(string_comparison::current_culture);
    }
    [[nodiscard]] static string_order current_culture_ignore_case() noexcept {
        return string_order(string_comparison::current_culture_ignore_case);
    }

    [[nodiscard]] string_comparison comparison() const noexcept {
        return comparison_;
    }

    // Negative if a sorts before b, zero if they compare equal, positive
    // otherwise. Never throws.
    [[nodiscard]] int compare(std::string_view a,
                              std::string_view b) const noexcept {
        const bool fold = detail::order_ignores_case(comparison_);
        if (detail::order_is_linguistic(comparison_)) {
            return detail::compare_tokens(a, b, fold);
        }
        return detail::compare_bytes(a, b, fold);
    }

    [[nodiscard]] bool equals(std::string_view a,
                              std::string_view b) const noexcept {
        return compare(a, b) == 0;
    }

    // Byte-prefix/suffix test with the same ASCII folding as compare. For the
    // culture modes this is still exact in the sense that it agrees with
    // `compare` on our code-point approximation -- but a real collator's
    // StartsWith can ignore leading characters (soft hyphens, zero-width
    // joiners), which this does not. See the file comment.
    [[nodiscard]] bool starts_with(std::string_view a,
                                   std::string_view b) const noexcept {
        if (a.size() < b.size()) {
            return false;
        }
        return equals(a.substr(0, b.size()), b);
    }

    [[nodiscard]] bool ends_with(std::string_view a,
                                 std::string_view b) const noexcept {
        if (a.size() < b.size()) {
            return false;
        }
        return equals(a.substr(a.size() - b.size()), b);
    }

    // True when `a` sorts before `b` under this mode.
    [[nodiscard]] bool operator()(std::string_view a,
                                  std::string_view b) const noexcept {
        return compare(a, b) < 0;
    }

    // Consistent with equals(): equal strings hash equally. See the file
    // comment for the seeding contract.
    [[nodiscard]] std::size_t hash(std::string_view text) const noexcept {
        return detail::hash_bytes(text,
                                  detail::order_ignores_case(comparison_));
    }

private:
    string_comparison comparison_;
};

} // namespace text
} // namespace crpc

#endif // CRPC_TEXT_STRING_ORDER_HPP
