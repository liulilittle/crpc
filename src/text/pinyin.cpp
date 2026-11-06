// CRPC -- Chinese pinyin transcription. See include/crpc/text/pinyin.hpp.
//
// The generated table (src/text/pinyin_table.inc) is laid out so that a
// transcript is one greedy left-to-right scan:
//
//   * every code point owns a contiguous run of reading ids, most common
//     first (kReadingStart / kReadingIds);
//   * words are bucketed by their first character, and inside a bucket they
//     are sorted longest-first, so the first key that matches the input at the
//     current position is also the longest one (kPhraseBucketStart,
//     kPhraseStart, kPhraseChars);
//   * a word whose readings equal the per-character defaults stores the
//     kNoPhraseReading sentinel instead of a reading list, because the only
//     thing it contributes is its length (it still has to be matched, or a
//     longer unrelated key could win across the boundary).
//
// A character that matches no word emits its default (first) reading. This is
// exactly how pypinyin behaves, and the generator verifies it: on every one of
// the 47111 word keys and on thousands of random concatenations the greedy
// scan reproduces pypinyin's lazy_pinyin output.
#include <crpc/text/pinyin.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/text/utf.hpp>

// The generated data. It defines crpc::text::pinyin_table itself, so it must
// be included at namespace scope, not from inside crpc::text. The quoted form
// resolves next to this source file, like unicode_data.inc does.
#include "pinyin_table.inc"

namespace crpc {
namespace text {

namespace {

namespace table = pinyin_table;

constexpr std::uint32_t kExtensionAFirst = 0x3400U;
constexpr std::uint32_t kExtensionALast = 0x4DBFU;
constexpr std::uint32_t kUnifiedFirst = 0x4E00U;
constexpr std::uint32_t kUnifiedLast = 0x9FFFU;
constexpr std::uint32_t kMaxCodePoint = 0x10FFFFU;

// Only used as "no match"; kPhraseCount is far below it.
constexpr std::uint16_t kNoPhrase = 0xFFFFU;
static_assert(table::kPhraseCount < kNoPhrase,
              "phrase count must leave 0xFFFF free as the no-match sentinel");
static_assert(table::kNoPhraseReading == kNoPhrase,
              "generated sentinel and the local no-match value must agree");

struct reading_range {
    const std::uint16_t* ids = nullptr;
    std::size_t count = 0;
};

reading_range readings_of(std::uint32_t code_point) noexcept {
    const std::size_t index = code_point - table::kCodePointBase;
    const std::uint16_t begin = table::kReadingStart[index];
    const std::uint16_t end = table::kReadingStart[index + 1];
    return reading_range{table::kReadingIds + begin,
                         static_cast<std::size_t>(end - begin)};
}

// One syllable in the requested style. The generated strings are static, so
// the view remains valid for the life of the process.
std::string_view form_of(std::uint16_t id, pinyin_style style) noexcept {
    const table::syllable_forms& forms = table::kSyllables[id];
    switch (style) {
    case pinyin_style::with_tone:
        return forms.toned;
    case pinyin_style::tone_number:
        return forms.numbered;
    case pinyin_style::without_tone:
        break;
    }
    return forms.plain;
}

void append_utf8(std::string& out, std::uint32_t code_point) {
    if (code_point < 0x80U) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else if (code_point < 0x10000U) {
        out.push_back(static_cast<char>(0xE0U | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80U | ((code_point >> 6) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80U | ((code_point >> 12) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((code_point >> 6) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    }
}

// Decodes strict UTF-8 into code points. The heavy lifting (overlong forms,
// surrogates, truncation, > U+10FFFF) is `utf8_to_utf16`, the module that owns
// that grammar; this only folds surrogate pairs back into code points.
result<std::vector<char32_t>> decode_code_points(std::string_view text) {
    const auto units = utf8_to_utf16(text);
    if (!units.ok()) {
        return units.error();
    }
    const std::u16string& value = units.value();
    std::vector<char32_t> out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const std::uint32_t unit = value[i];
        if (unit >= 0xD800U && unit <= 0xDBFFU) {
            // utf8_to_utf16 never emits an unpaired surrogate, so the next
            // unit exists and is a low surrogate.
            const std::uint32_t low = value[i + 1];
            out.push_back(static_cast<char32_t>(0x10000U + ((unit - 0xD800U) << 10) +
                                               (low - 0xDC00U)));
            ++i;
        } else {
            out.push_back(static_cast<char32_t>(unit));
        }
    }
    return out;
}

// Longest word starting at `position`, or kNoPhrase. Buckets are sorted
// longest-first, so the first match is the longest.
std::uint16_t match_phrase(const std::vector<char32_t>& code_points,
                           std::size_t position) noexcept {
    const std::size_t bucket =
        static_cast<std::uint32_t>(code_points[position]) - table::kCodePointBase;
    const std::uint16_t last = table::kPhraseBucketStart[bucket + 1];
    for (std::uint16_t index = table::kPhraseBucketStart[bucket]; index < last;
         ++index) {
        const std::uint32_t tail_begin = table::kPhraseStart[index];
        const std::uint32_t tail_end = table::kPhraseStart[index + 1];
        const std::size_t length =
            1 + static_cast<std::size_t>(tail_end - tail_begin);
        if (length > code_points.size() - position) {
            continue;
        }
        bool matched = true;
        for (std::uint32_t k = tail_begin; k < tail_end; ++k) {
            const std::uint32_t expected = table::kPhraseChars[k];
            const std::uint32_t actual = static_cast<std::uint32_t>(
                code_points[position + 1 + (k - tail_begin)]);
            if (actual != expected) {
                matched = false;
                break;
            }
        }
        if (matched) {
            return index;
        }
    }
    return kNoPhrase;
}

// Collects the output, applying the separator and the ASCII case fold.
class writer {
public:
    writer(std::string_view separator, bool lowercase) noexcept
        : separator_(separator), lowercase_(lowercase) {}

    // A syllable separator goes immediately before every syllable except the
    // first one written, so "中A国" with "-" is "zhongA-guo" (the preserved A
    // is copied verbatim, and the separator still marks the syllable after
    // it).
    void syllable(std::string_view text) {
        if (!out_.empty()) {
            out_.append(separator_);
        }
        out_.append(text);
    }

    // A preserved code point, re-encoded as UTF-8.
    void literal(std::uint32_t code_point) {
        if (lowercase_ && code_point >= 'A' && code_point <= 'Z') {
            code_point += 'a' - 'A';
        }
        append_utf8(out_, code_point);
    }

    void ascii_letter(char value) { out_.push_back(value); }

    std::string take() { return std::move(out_); }

private:
    std::string out_;
    std::string_view separator_;
    bool lowercase_;
};

std::string transcribe(const std::vector<char32_t>& code_points,
                       pinyin_options options, bool initials) {
    writer out(initials ? std::string_view{} : options.separator, options.lowercase);
    const pinyin_style style =
        initials ? pinyin_style::without_tone : options.style;

    // Emits one syllable, or -- for initials -- its first letter when that
    // letter is ASCII. "ê" (the interjection reading) starts with a byte
    // outside ASCII and therefore contributes nothing, matching pypinyin's
    // strict initials.
    const auto emit = [&](std::uint16_t id) {
        const std::string_view form = form_of(id, style);
        if (!initials) {
            out.syllable(form);
            return;
        }
        if (!form.empty() && form[0] >= 'a' && form[0] <= 'z') {
            out.ascii_letter(form[0]);
        }
    };

    std::size_t position = 0;
    while (position < code_points.size()) {
        const std::uint32_t code_point =
            static_cast<std::uint32_t>(code_points[position]);
        if (!is_chinese(static_cast<char32_t>(code_point))) {
            if (options.keep_non_chinese) {
                out.literal(code_point);
            }
            ++position;
            continue;
        }

        const std::uint16_t phrase = match_phrase(code_points, position);
        if (phrase != kNoPhrase) {
            const std::size_t length = 1 + static_cast<std::size_t>(
                                               table::kPhraseStart[phrase + 1] -
                                               table::kPhraseStart[phrase]);
            const std::uint16_t start = table::kPhraseReadingStart[phrase];
            if (start == table::kNoPhraseReading) {
                for (std::size_t k = 0; k < length; ++k) {
                    const reading_range single = readings_of(
                        static_cast<std::uint32_t>(code_points[position + k]));
                    emit(single.ids[0]);
                }
            } else {
                for (std::size_t k = 0; k < length; ++k) {
                    emit(table::kPhraseReadingIds[start + k]);
                }
            }
            position += length;
            continue;
        }

        const reading_range single = readings_of(code_point);
        if (single.count != 0) {
            emit(single.ids[0]);
        } else {
            // A Chinese character this table has no reading for (ten GBK code
            // points). Copy it rather than invent a reading; keep_non_chinese
            // deliberately does not drop it, because it *is* Chinese text.
            out.literal(code_point);
        }
        ++position;
    }
    return out.take();
}

std::string invalid_code_point(std::uint32_t value) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string out = "not a Unicode scalar value: U+";
    char buffer[8] = {};
    int used = 0;
    do {
        buffer[used++] = digits[value & 0xFU];
        value >>= 4;
    } while (value != 0 && used < static_cast<int>(sizeof(buffer)));
    while (used > 0) {
        out.push_back(buffer[--used]);
    }
    return out;
}

} // namespace

bool is_chinese(char32_t code_point) noexcept {
    const std::uint32_t value = code_point;
    return (value >= kExtensionAFirst && value <= kExtensionALast) ||
           (value >= kUnifiedFirst && value <= kUnifiedLast);
}

result<std::string> to_pinyin(std::string_view utf8_text, pinyin_options options) {
    const auto code_points = decode_code_points(utf8_text);
    if (!code_points.ok()) {
        return code_points.error();
    }
    return transcribe(code_points.value(), options, /*initials=*/false);
}

result<std::string> to_pinyin_initials(std::string_view utf8_text,
                                       pinyin_options options) {
    const auto code_points = decode_code_points(utf8_text);
    if (!code_points.ok()) {
        return code_points.error();
    }
    return transcribe(code_points.value(), options, /*initials=*/true);
}

result<std::vector<std::string>> pinyin_candidates(char32_t code_point,
                                                   pinyin_style style) {
    const std::uint32_t value = code_point;
    if (value > kMaxCodePoint || (value >= 0xD800U && value <= 0xDFFFU)) {
        return rpc_error{rpc_code::invalid_argument, invalid_code_point(value)};
    }

    std::vector<std::string> out;
    if (!is_chinese(code_point)) {
        return out;
    }
    const reading_range range = readings_of(value);
    out.reserve(range.count);
    for (std::size_t i = 0; i < range.count; ++i) {
        std::string text(form_of(range.ids[i], style));
        bool seen = false;
        for (const std::string& existing : out) {
            if (existing == text) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            out.push_back(std::move(text));
        }
    }
    return out;
}

} // namespace text
} // namespace crpc
