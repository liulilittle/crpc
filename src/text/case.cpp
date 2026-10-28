// CRPC -- case mapping and case-insensitive comparison (ASCII + Latin-1).
// See include/crpc/text/case.hpp for the exact scope.
#include <crpc/text/case.hpp>

namespace crpc {
namespace text {

namespace {

inline char ascii_to_lower_impl(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

inline char ascii_to_upper_impl(char c) noexcept {
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - ('a' - 'A')) : c;
}

// Decodes a two-byte UTF-8 sequence into its code point, or 0xFFFFFFFF when
// the bytes are not a well-formed two-byte sequence.
std::uint32_t decode_latin1_sequence(const char* bytes) noexcept {
    const auto b0 = static_cast<std::uint8_t>(bytes[0]);
    const auto b1 = static_cast<std::uint8_t>(bytes[1]);
    if ((b0 & 0xE0u) != 0xC0u || (b1 & 0xC0u) != 0x80u) {
        return 0xFFFFFFFFu;
    }
    return static_cast<std::uint32_t>(((b0 & 0x1Fu) << 6) | (b1 & 0x3Fu));
}

void encode_latin1_sequence(std::string& out, std::uint32_t code_point) {
    out.push_back(static_cast<char>(0xC0u | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
}

// The Latin-1 letters that have a single-code-point case counterpart:
// U+00C0..U+00D6 and U+00D8..U+00DE (upper), U+00E0..U+00F6 and
// U+00F8..U+00FE (lower). U+00D7 (x), U+00DF (ss), U+00F7 (/) and U+00FF (y
// diaeresis) have no Latin-1 counterpart and pass through.
bool latin1_is_uppercase(std::uint32_t cp) noexcept {
    return (cp >= 0xC0u && cp <= 0xD6u) || (cp >= 0xD8u && cp <= 0xDEu);
}

bool latin1_is_lowercase(std::uint32_t cp) noexcept {
    return (cp >= 0xE0u && cp <= 0xF6u) || (cp >= 0xF8u && cp <= 0xFEu);
}

void case_impl(const char* bytes, std::size_t size, bool to_upper,
               std::string& out) {
    out.reserve(size);
    for (std::size_t i = 0; i < size;) {
        const auto b0 = static_cast<std::uint8_t>(bytes[i]);
        if (b0 < 0x80u) {
            out.push_back(to_upper ? ascii_to_upper_impl(bytes[i])
                                   : ascii_to_lower_impl(bytes[i]));
            ++i;
            continue;
        }
        if ((b0 & 0xE0u) == 0xC0u && i + 1 < size) {
            const std::uint32_t cp = decode_latin1_sequence(bytes + i);
            if (cp != 0xFFFFFFFFu &&
                (to_upper ? latin1_is_lowercase(cp) : latin1_is_uppercase(cp))) {
                // Lowercase is uppercase + 0x20 in Latin-1.
                encode_latin1_sequence(out, to_upper ? cp - 0x20u : cp + 0x20u);
            } else {
                out.push_back(bytes[i]);
                out.push_back(bytes[i + 1]);
            }
            i += 2;
            continue;
        }
        // A three/four-byte sequence (or a malformed two-byte tail) passes
        // through byte-for-byte.
        out.push_back(bytes[i]);
        ++i;
    }
}

} // namespace

bool is_ascii(std::string_view text) noexcept {
    for (const unsigned char c : text) {
        if (c >= 0x80u) {
            return false;
        }
    }
    return true;
}

std::string to_lower(std::string_view text) {
    // ASCII fast path.
    if (is_ascii(text)) {
        std::string out(text);
        for (char& c : out) {
            c = ascii_to_lower_impl(c);
        }
        return out;
    }
    std::string out;
    case_impl(text.data(), text.size(), /*to_upper=*/false, out);
    return out;
}

std::string to_upper(std::string_view text) {
    if (is_ascii(text)) {
        std::string out(text);
        for (char& c : out) {
            c = ascii_to_upper_impl(c);
        }
        return out;
    }
    std::string out;
    case_impl(text.data(), text.size(), /*to_upper=*/true, out);
    return out;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    if (is_ascii(a) && is_ascii(b)) {
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (ascii_to_lower_impl(a[i]) != ascii_to_lower_impl(b[i])) {
                return false;
            }
        }
        return true;
    }
    // Non-ASCII comparison folds Latin-1 and falls back to strict bytes for
    // everything else. Case mapping never changes the number of bytes, so the
    // length check above is already sufficient.
    std::string lower_a = to_lower(a);
    std::string lower_b = to_lower(b);
    return lower_a == lower_b;
}

bool istarts_with(std::string_view text, std::string_view prefix) noexcept {
    return prefix.size() <= text.size() &&
           iequals(text.substr(0, prefix.size()), prefix);
}

bool iends_with(std::string_view text, std::string_view suffix) noexcept {
    return suffix.size() <= text.size() &&
           iequals(text.substr(text.size() - suffix.size()), suffix);
}

} // namespace text
} // namespace crpc