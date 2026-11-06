// CRPC -- Unicode normalization, case mapping, categories and grapheme
// clusters. See include/crpc/text/unicode.hpp.
//
// The data tables are generated (src/text/unicode_data.inc) rather than
// hand-written: every classification, mapping and composition pair below is
// straight out of the Unicode 16.0.0 Character Database, and the generator
// proves the tables against NormalizationTest.txt and GraphemeBreakTest.txt
// before writing them.
//
// Layout notes:
//   * ranges are sorted, non-overlapping and inclusive; lookups are a binary
//     search, which keeps the tables compact (a couple of range arrays instead
//     of 1.1M-entry lookup tables) and fast enough for text processing;
//   * mappings use an index table plus one flat data pool, so a multi-code-
//     point mapping ("ß" -> "SS") costs three index fields and two pool slots
//     and no per-entry vector;
//   * the ASCII fast path in `to_case` avoids touching the tables at all for
//     text that has no cased non-ASCII character.
#include <crpc/text/unicode.hpp>

#include <algorithm>
#include <cstddef>

namespace crpc {
namespace text {

namespace {

// --- Generated table shapes --------------------------------------------------
//
// `range0` is a predicate range (presence only), `range_byte` carries a small
// enum value, `map_entry` indexes into a flat `char32_t` pool,
// `composition_entry` is a (first, second) -> composite pair and
// `decomposition_entry` indexes the decomposition pool.
struct range0 {
    char32_t first;
    char32_t last;
};

struct range_byte {
    char32_t first;
    char32_t last;
    std::uint8_t value;
};

struct map_entry {
    char32_t code_point;
    std::uint32_t offset;
    std::uint16_t length;
};

struct composition_entry {
    char32_t first;
    char32_t second;
    char32_t composite;
};

struct decomposition_entry {
    char32_t code_point;
    std::uint32_t offset;
    std::uint16_t length;
    bool compatibility;
};

// The general-category values are indices into this list, which the generator
// writes in the same order. Keeping the names here (instead of in the .inc) is
// what makes a table dump readable.
enum class category : std::uint8_t {
    Cc, Cf, Cn, Co, Cs,
    Ll, Lm, Lo, Lt, Lu,
    Mc, Me, Mn,
    Nd, Nl, No,
    Pc, Pd, Pe, Pf, Pi, Po, Ps,
    Sc, Sk, Sm, So,
    Zl, Zp, Zs
};

enum class gcb : std::uint8_t {
    other, cr, lf, control, extend, zwj,
    regional_indicator, prepend, spacing_mark,
    l, v, t, lv, lvt
};

enum class incb : std::uint8_t {
    none, linker, consonant, extend
};

#include "unicode_data.inc"

constexpr std::size_t kCategoryCount = 30;

// `kCategories` stores a category index and the enum order above matches the
// generator's code list, so a static cast is enough. This assertion turns a
// silent reorder on either side into a compile error.
static_assert(static_cast<std::size_t>(category::Zs) == kCategoryCount - 1,
              "category enum and generator code list disagree");

template <typename Range>
bool in_ranges(const Range* ranges, std::size_t count, char32_t value) noexcept {
    std::size_t lo = 0;
    std::size_t hi = count;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (value < ranges[mid].first) {
            hi = mid;
        } else if (value > ranges[mid].last) {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

// Returns the value of the range containing `value`, or `fallback`.
template <typename Range>
std::uint8_t value_in_ranges(const Range* ranges, std::size_t count,
                             char32_t value, std::uint8_t fallback) noexcept {
    std::size_t lo = 0;
    std::size_t hi = count;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (value < ranges[mid].first) {
            hi = mid;
        } else if (value > ranges[mid].last) {
            lo = mid + 1;
        } else {
            return ranges[mid].value;
        }
    }
    return fallback;
}

template <typename Entry>
const Entry* find_entry(const Entry* entries, std::size_t count,
                        char32_t value) noexcept {
    std::size_t lo = 0;
    std::size_t hi = count;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (value < entries[mid].code_point) {
            hi = mid;
        } else if (value > entries[mid].code_point) {
            lo = mid + 1;
        } else {
            return &entries[mid];
        }
    }
    return nullptr;
}

template <typename T, std::size_t N>
constexpr std::size_t count_of(T (&)[N]) noexcept {
    return N;
}

category category_of(char32_t code_point) noexcept {
    const std::uint8_t value =
        value_in_ranges(kCategories, count_of(kCategories), code_point,
                        static_cast<std::uint8_t>(category::Cn));
    if (value >= kCategoryCount) {
        return category::Cn;
    }
    return static_cast<category>(value);
}

std::uint8_t canonical_combining_class(char32_t code_point) noexcept {
    return value_in_ranges(kCcc, count_of(kCcc), code_point, 0);
}

const decomposition_entry* decomposition_of(char32_t code_point) noexcept {
    return find_entry(kDecompositions, count_of(kDecompositions), code_point);
}

const map_entry* case_entry(const map_entry* entries, std::size_t count,
                            char32_t code_point) noexcept {
    return find_entry(entries, count, code_point);
}

// --- Errors ------------------------------------------------------------------

rpc_error decode_error(std::size_t offset) {
    return rpc_error{rpc_code::decode_error,
                     "invalid UTF-8 at byte offset " + std::to_string(offset)};
}

rpc_error encode_error(char32_t code_point) {
    return rpc_error{rpc_code::encode_error,
                     "code point U+" + std::to_string(static_cast<std::uint32_t>(code_point)) +
                         " is not a Unicode scalar value"};
}

// --- Strict UTF-8 ------------------------------------------------------------
//
// The public utf.hpp decoder targets UTF-16 and allocates a u16string; this one
// appends code points to a vector and reports the same byte-level failures.
bool decode_next(std::string_view text, std::size_t& offset,
                 char32_t& code_point) noexcept {
    const auto lead = static_cast<std::uint8_t>(text[offset]);
    std::uint32_t value = 0;
    std::size_t extra = 0;
    if (lead < 0x80u) {
        code_point = lead;
        ++offset;
        return true;
    }
    if ((lead & 0xE0u) == 0xC0u) {
        value = lead & 0x1Fu;
        extra = 1;
    } else if ((lead & 0xF0u) == 0xE0u) {
        value = lead & 0x0Fu;
        extra = 2;
    } else if ((lead & 0xF8u) == 0xF0u) {
        value = lead & 0x07u;
        extra = 3;
    } else {
        return false;
    }
    if (offset + extra >= text.size()) {
        return false;
    }
    for (std::size_t k = 1; k <= extra; ++k) {
        const auto cont = static_cast<std::uint8_t>(text[offset + k]);
        if ((cont & 0xC0u) != 0x80u) {
            return false;
        }
        value = (value << 6) | (cont & 0x3Fu);
    }
    if ((extra == 1 && value < 0x80u) || (extra == 2 && value < 0x800u) ||
        (extra == 3 && value < 0x10000u)) {
        return false;
    }
    if (value >= 0xD800u && value <= 0xDFFFu) {
        return false;
    }
    if (value > 0x10FFFFu) {
        return false;
    }
    code_point = static_cast<char32_t>(value);
    offset += extra + 1;
    return true;
}

void append_utf8(std::string& out, char32_t code_point) {
    const auto value = static_cast<std::uint32_t>(code_point);
    if (value < 0x80u) {
        out.push_back(static_cast<char>(value));
    } else if (value < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (value >> 6)));
        out.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
    } else if (value < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (value >> 12)));
        out.push_back(static_cast<char>(0x80u | ((value >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (value >> 18)));
        out.push_back(static_cast<char>(0x80u | ((value >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((value >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
    }
}

result<std::vector<char32_t>> decode_all(std::string_view text) {
    std::vector<char32_t> out;
    out.reserve(text.size());
    for (std::size_t offset = 0; offset < text.size();) {
        char32_t code_point = 0;
        if (!decode_next(text, offset, code_point)) {
            return decode_error(offset);
        }
        out.push_back(code_point);
    }
    return out;
}

// --- Case mapping ------------------------------------------------------------

void append_mapping(std::string& out, const char32_t* pool,
                    const map_entry* entry) {
    for (std::uint16_t i = 0; i < entry->length; ++i) {
        append_utf8(out, pool[entry->offset + i]);
    }
}

// --- Composition -------------------------------------------------------------

char32_t compose_pair(char32_t first, char32_t second) noexcept {
    // Hangul composition is arithmetic (UAX #15, section 3.12); the tables
    // only carry the non-Hangul pairs.
    constexpr char32_t s_base = 0xAC00;
    constexpr char32_t l_base = 0x1100;
    constexpr char32_t v_base = 0x1161;
    constexpr char32_t t_base = 0x11A7;
    constexpr char32_t l_count = 19;
    constexpr char32_t v_count = 21;
    constexpr char32_t t_count = 28;
    constexpr char32_t n_count = v_count * t_count;
    constexpr char32_t s_count = l_count * n_count;

    if (first >= l_base && first < l_base + l_count && second >= v_base &&
        second < v_base + v_count) {
        return static_cast<char32_t>(
            s_base + ((first - l_base) * v_count + (second - v_base)) * t_count);
    }
    if (first >= s_base && first < s_base + s_count &&
        (first - s_base) % t_count == 0 && second > t_base &&
        second < t_base + t_count) {
        return static_cast<char32_t>(first + (second - t_base));
    }

    std::size_t lo = 0;
    std::size_t hi = count_of(kCompositions);
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (first < kCompositions[mid].first ||
            (first == kCompositions[mid].first && second < kCompositions[mid].second)) {
            hi = mid;
        } else if (first > kCompositions[mid].first ||
                   (first == kCompositions[mid].first && second > kCompositions[mid].second)) {
            lo = mid + 1;
        } else {
            return kCompositions[mid].composite;
        }
    }
    return 0; // 0 is not a scalar value, so it doubles as "no composition"
}

void decompose_into(std::vector<char32_t>& out, char32_t code_point,
                    bool compatibility) {
    constexpr char32_t s_base = 0xAC00;
    constexpr char32_t l_base = 0x1100;
    constexpr char32_t v_base = 0x1161;
    constexpr char32_t t_base = 0x11A7;
    constexpr char32_t l_count = 19;
    constexpr char32_t v_count = 21;
    constexpr char32_t t_count = 28;
    constexpr char32_t n_count = v_count * t_count;
    constexpr char32_t s_count = l_count * n_count;

    if (code_point >= s_base && code_point < s_base + s_count) {
        const char32_t index = code_point - s_base;
        out.push_back(static_cast<char32_t>(l_base + index / n_count));
        out.push_back(static_cast<char32_t>(v_base + (index % n_count) / t_count));
        if (index % t_count != 0) {
            out.push_back(static_cast<char32_t>(t_base + index % t_count));
        }
        return;
    }

    const decomposition_entry* entry = decomposition_of(code_point);
    if (entry == nullptr || (entry->compatibility && !compatibility)) {
        out.push_back(code_point);
        return;
    }
    for (std::uint16_t i = 0; i < entry->length; ++i) {
        decompose_into(out, kDecompositionData[entry->offset + i], compatibility);
    }
}

// Canonical ordering (UAX #15, D109): stable-sort every maximal run of
// non-starters by combining class. Runs are almost always one or two
// characters, so a small insertion sort beats a general sort here.
void canonical_order(std::vector<char32_t>& text) {
    std::size_t i = 0;
    while (i < text.size()) {
        if (canonical_combining_class(text[i]) == 0) {
            ++i;
            continue;
        }
        const std::size_t start = i;
        while (i < text.size() && canonical_combining_class(text[i]) != 0) {
            ++i;
        }
        for (std::size_t k = start + 1; k < i; ++k) {
            const char32_t value = text[k];
            const std::uint8_t value_class = canonical_combining_class(value);
            std::size_t position = k;
            while (position > start &&
                   canonical_combining_class(text[position - 1]) > value_class) {
                text[position] = text[position - 1];
                --position;
            }
            text[position] = value;
        }
    }
}

// The compose pass of UAX #15 (definition of NFC/NFKC) lives inside
// normalize(); there is no separate helper because the starter/combining-class
// bookkeeping is only meaningful with the surrounding decomposition pass.

} // namespace

const char* unicode_version() noexcept {
    return "16.0.0";
}

result<std::vector<char32_t>> to_code_points(std::string_view utf8) {
    return decode_all(utf8);
}

result<std::string> from_code_points(span<const char32_t> code_points) {
    std::string out;
    out.reserve(code_points.size());
    for (const char32_t code_point : code_points) {
        const auto value = static_cast<std::uint32_t>(code_point);
        if (value > 0x10FFFFu || (value >= 0xD800u && value <= 0xDFFFu)) {
            return encode_error(code_point);
        }
        append_utf8(out, code_point);
    }
    return out;
}

result<std::string> normalize(std::string_view utf8, normalization_form form) {
    const auto decoded = decode_all(utf8);
    if (!decoded.ok()) {
        return decoded.error();
    }

    const bool compatibility =
        form == normalization_form::nfkc || form == normalization_form::nfkd;

    std::vector<char32_t> decomposed;
    decomposed.reserve(decoded.value().size());
    for (const char32_t code_point : decoded.value()) {
        decompose_into(decomposed, code_point, compatibility);
    }
    canonical_order(decomposed);

    if (form == normalization_form::nfd || form == normalization_form::nfkd) {
        std::string out;
        out.reserve(utf8.size());
        for (const char32_t code_point : decomposed) {
            append_utf8(out, code_point);
        }
        return out;
    }

    // Canonical composition (UAX #15, section 1.3): a starter absorbs the
    // following marks while their combining class keeps increasing, and a
    // starter with class 0 never blocks a later composition.
    std::vector<char32_t> composed;
    composed.reserve(decomposed.size());
    std::size_t starter = static_cast<std::size_t>(-1);
    std::int32_t last_class = -1;
    for (const char32_t code_point : decomposed) {
        const std::int32_t current =
            static_cast<std::int32_t>(canonical_combining_class(code_point));
        if (starter != static_cast<std::size_t>(-1) &&
            (last_class < current || last_class == -1)) {
            const char32_t composite = compose_pair(composed[starter], code_point);
            if (composite != 0) {
                composed[starter] = composite;
                continue;
            }
        }
        if (current == 0) {
            starter = composed.size();
            last_class = -1;
        } else {
            last_class = current;
        }
        composed.push_back(code_point);
    }

    std::string out;
    out.reserve(utf8.size());
    for (const char32_t code_point : composed) {
        append_utf8(out, code_point);
    }
    return out;
}

result<std::string> to_case(std::string_view utf8, case_mode mode) {
    const auto decoded = decode_all(utf8);
    if (!decoded.ok()) {
        return decoded.error();
    }

    const map_entry* entries = nullptr;
    std::size_t entry_count = 0;
    const char32_t* pool = nullptr;
    switch (mode) {
    case case_mode::lower:
        entries = kLowercaseMap;
        entry_count = count_of(kLowercaseMap);
        pool = kLowercaseMap_data;
        break;
    case case_mode::upper:
        entries = kUppercaseMap;
        entry_count = count_of(kUppercaseMap);
        pool = kUppercaseMap_data;
        break;
    case case_mode::title:
        entries = kTitlecaseMap;
        entry_count = count_of(kTitlecaseMap);
        pool = kTitlecaseMap_data;
        break;
    case case_mode::fold:
        entries = kCaseFoldMap;
        entry_count = count_of(kCaseFoldMap);
        pool = kCaseFoldMap_data;
        break;
    }

    std::string out;
    out.reserve(utf8.size());
    bool cased_seen = false;
    for (const char32_t code_point : decoded.value()) {
        if (mode == case_mode::title) {
            const category cat = category_of(code_point);
            const bool is_letter = cat == category::Lu || cat == category::Ll ||
                                   cat == category::Lt || cat == category::Lm ||
                                   cat == category::Lo;
            const bool is_mark = cat == category::Mn || cat == category::Mc ||
                                 cat == category::Me;
            // A word is a maximal run of letters and combining marks: only a
            // character that is neither starts a new word. Case-less letters
            // (Lm/Lo) belong to the word but have no case, so they must not
            // consume the "first cased letter" slot.
            if (!is_letter && !is_mark) {
                cased_seen = false;
            }
            if (cat == category::Lu || cat == category::Ll || cat == category::Lt) {
                const map_entry* entry =
                    cased_seen ? case_entry(kLowercaseMap, count_of(kLowercaseMap),
                                            code_point)
                               : case_entry(kTitlecaseMap, count_of(kTitlecaseMap),
                                            code_point);
                if (entry != nullptr) {
                    append_mapping(out, cased_seen ? kLowercaseMap_data
                                                   : kTitlecaseMap_data,
                                   entry);
                } else {
                    append_utf8(out, code_point);
                }
                cased_seen = true;
                continue;
            }
            append_utf8(out, code_point);
            continue;
        }

        const map_entry* entry = case_entry(entries, entry_count, code_point);
        if (entry != nullptr) {
            append_mapping(out, pool, entry);
        } else {
            append_utf8(out, code_point);
        }
    }
    return out;
}

bool is_letter(char32_t code_point) noexcept {
    const category cat = category_of(code_point);
    return cat == category::Lu || cat == category::Ll || cat == category::Lt ||
           cat == category::Lm || cat == category::Lo;
}

bool is_digit(char32_t code_point) noexcept {
    return category_of(code_point) == category::Nd;
}

bool is_space(char32_t code_point) noexcept {
    return in_ranges(kWhiteSpace, count_of(kWhiteSpace), code_point);
}

bool is_upper(char32_t code_point) noexcept {
    return category_of(code_point) == category::Lu;
}

bool is_lower(char32_t code_point) noexcept {
    return category_of(code_point) == category::Ll;
}

bool is_punctuation(char32_t code_point) noexcept {
    const category cat = category_of(code_point);
    return cat == category::Pc || cat == category::Pd || cat == category::Pe ||
           cat == category::Pf || cat == category::Pi || cat == category::Po ||
           cat == category::Ps;
}

result<std::vector<std::string_view>> graphemes(std::string_view utf8) {
    std::vector<std::string_view> out;
    if (utf8.empty()) {
        return out;
    }

    // Decode once, remembering the byte offset of every code point so the
    // returned views can point into the caller's buffer.
    std::vector<char32_t> points;
    std::vector<std::size_t> offsets;
    points.reserve(utf8.size());
    offsets.reserve(utf8.size());
    for (std::size_t offset = 0; offset < utf8.size();) {
        const std::size_t start = offset;
        char32_t code_point = 0;
        if (!decode_next(utf8, offset, code_point)) {
            return decode_error(start);
        }
        points.push_back(code_point);
        offsets.push_back(start);
    }
    offsets.push_back(utf8.size());

    auto break_before = [&points](std::size_t i) -> bool {
        const gcb previous = static_cast<gcb>(value_in_ranges(
            kGraphemeBreak, count_of(kGraphemeBreak), points[i - 1],
            static_cast<std::uint8_t>(gcb::other)));
        const gcb current = static_cast<gcb>(value_in_ranges(
            kGraphemeBreak, count_of(kGraphemeBreak), points[i],
            static_cast<std::uint8_t>(gcb::other)));

        // UAX #29 rules GB3..GB13, in the order the specification applies
        // them. Rules phrased as "do not break" are checked in that order and
        // the first match wins; anything left breaks (GB999).
        if (previous == gcb::cr && current == gcb::lf) {
            return false; // GB3
        }
        if (previous == gcb::control || previous == gcb::cr || previous == gcb::lf) {
            return true; // GB4
        }
        if (current == gcb::control || current == gcb::cr || current == gcb::lf) {
            return true; // GB5
        }
        if (previous == gcb::l &&
            (current == gcb::l || current == gcb::v || current == gcb::lv ||
             current == gcb::lvt)) {
            return false; // GB6
        }
        if ((previous == gcb::lv || previous == gcb::v) &&
            (current == gcb::v || current == gcb::t)) {
            return false; // GB7
        }
        if ((previous == gcb::lvt || previous == gcb::t) && current == gcb::t) {
            return false; // GB8
        }
        if (current == gcb::extend || current == gcb::zwj) {
            return false; // GB9
        }
        if (current == gcb::spacing_mark) {
            return false; // GB9a
        }
        if (previous == gcb::prepend) {
            return false; // GB9b
        }
        // GB9c: Consonant [Extend Linker]* Linker [Extend Linker]* x
        // Consonant. Only a consonant can start the right-hand side, so the
        // backward scan runs only for one. (GB9/GB9a/GB9b above already claim
        // Extend, ZWJ, SpacingMark and after-Prepend, none of which can be
        // InCB=Consonant.)
        if (static_cast<incb>(value_in_ranges(
                kIndicConjunctBreak, count_of(kIndicConjunctBreak), points[i],
                static_cast<std::uint8_t>(incb::none))) == incb::consonant) {
            std::size_t j = i;
            bool linker_seen = false;
            while (j > 0) {
                const incb previous_incb = static_cast<incb>(value_in_ranges(
                    kIndicConjunctBreak, count_of(kIndicConjunctBreak),
                    points[j - 1], static_cast<std::uint8_t>(incb::none)));
                if (previous_incb != incb::extend && previous_incb != incb::linker) {
                    break;
                }
                if (previous_incb == incb::linker) {
                    linker_seen = true;
                }
                --j;
            }
            if (linker_seen && j > 0 &&
                static_cast<incb>(value_in_ranges(
                    kIndicConjunctBreak, count_of(kIndicConjunctBreak),
                    points[j - 1], static_cast<std::uint8_t>(incb::none))) ==
                    incb::consonant) {
                return false;
            }
        }
        if (previous == gcb::zwj &&
            in_ranges(kExtendedPictographic, count_of(kExtendedPictographic),
                      points[i])) {
            // GB11: Extend* ZWJ x Extended_Pictographic.
            std::size_t j = i - 1; // the ZWJ itself
            while (j > 0 &&
                   static_cast<gcb>(value_in_ranges(
                       kGraphemeBreak, count_of(kGraphemeBreak), points[j - 1],
                       static_cast<std::uint8_t>(gcb::other))) == gcb::extend) {
                --j;
            }
            if (j > 0 &&
                in_ranges(kExtendedPictographic, count_of(kExtendedPictographic),
                          points[j - 1])) {
                return false;
            }
        }
        if (previous == gcb::regional_indicator &&
            current == gcb::regional_indicator) {
            // GB12/GB13: pair region indicators from the start of the run.
            std::size_t count = 0;
            std::size_t j = i;
            while (j > 0 &&
                   static_cast<gcb>(value_in_ranges(
                       kGraphemeBreak, count_of(kGraphemeBreak), points[j - 1],
                       static_cast<std::uint8_t>(gcb::other))) ==
                       gcb::regional_indicator) {
                ++count;
                --j;
            }
            if (count % 2 == 1) {
                return false;
            }
        }
        return true;
    };

    std::size_t start = 0;
    for (std::size_t i = 1; i < points.size(); ++i) {
        if (break_before(i)) {
            out.push_back(utf8.substr(offsets[start], offsets[i] - offsets[start]));
            start = i;
        }
    }
    out.push_back(utf8.substr(offsets[start]));
    return out;
}

} // namespace text
} // namespace crpc
