// CRPC -- Chinese pinyin: transcription, initials and heteronym lookup.
//
// Data source
// -----------
// The mapping tables are generated (not hand-written) from `pypinyin`
// 0.55.0, whose character dictionary is derived from the Unicode Unihan
// `kMandarin` / `kHanyuPinlu` readings. The generator and its provenance
// header live in the generated file itself
// (`src/text/pinyin_table.inc`) and in `.opencode/tmp/pinyin/gen_pinyin.py`.
//
// Coverage
// --------
// Every GBK / GB2312 hanzi is covered: GBK maps exactly U+4E00..U+9FA5
// (20902 code points) and all of them carry at least one reading here, with
// the exception of ten code points pypinyin itself does not define
// (U+5159, U+5161, U+55E7, U+685B, U+70EA, U+74E7, U+74F0, U+74F1,
// U+74FC, U+7505). Those ten are preserved verbatim rather than given an
// invented reading: a wrong reading is worse than none.
//
// Words and heteronyms
// --------------------
// A heteronym reads differently by context. Inside a maximal run of Chinese
// characters the longest known word wins (47111 words from pypinyin, up to
// ten characters long), so "银行" is yin-hang and "行走" is xing-zou, while a
// lone character uses its most common reading (the first one listed). The
// lookup never spans a non-Chinese character, so "银A行" is yin + A + xing.
//
// Boundaries
// ----------
// "Chinese" means U+3400..U+4DBF (CJK Unified Ideographs Extension A) and
// U+4E00..U+9FFF (CJK Unified Ideographs). Everything else -- Latin, digits,
// punctuation, emoji, and Han outside those blocks -- is non-Chinese text and
// is copied or dropped according to `keep_non_chinese`.
//
// Strict input
// ------------
// Input must be well-formed UTF-8 (the same grammar as `text/utf.hpp`, which
// this module uses to validate). Malformed input is `decode_error`, never a
// silent replacement.
#ifndef CRPC_TEXT_PINYIN_HPP
#define CRPC_TEXT_PINYIN_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace text {

// How one syllable is written.
//
//   with_tone    zhōng  (diacritic on the syllable's main vowel)
//   without_tone zhong
//   tone_number  zhong1 (neutral tone is written 5: de5)
enum class pinyin_style : std::uint8_t {
    with_tone,
    without_tone,
    tone_number,
};

struct pinyin_options {
    pinyin_style style = pinyin_style::without_tone;

    // Written immediately before a syllable when an earlier syllable has
    // already been written, and nowhere else. So "中国" with "-" is
    // "zhong-guo", and preserved text is copied verbatim without separators
    // of its own: "中A国" is "zhongA-guo".
    std::string_view separator = "";

    // When false, non-Chinese code points are dropped from the output. It has
    // no effect on Chinese characters: one without a reading (see the header
    // comment) is always preserved.
    bool keep_non_chinese = true;

    // Folds ASCII A-Z in the output to lower case. Generated syllables are
    // already lower case; the flag therefore matters for preserved
    // non-Chinese text ("iPhone" -> "iphone" by default).
    bool lowercase = true;
};

// Transcribes `utf8_text` character by character. A Chinese character emits
// its syllable; a maximal known word emits the word's readings (which is what
// resolves heteronyms); anything else is copied or dropped as described
// above.
[[nodiscard]] CRPC_API result<std::string> to_pinyin(std::string_view utf8_text,
                                                     pinyin_options options = {});

// The pinyin initials of `utf8_text`: the first letter of each syllable, so
// "中国" is "zg" and "银行" is "yh". A syllable whose tone-less form does not
// start with an ASCII letter (the interjection reading "ê") contributes
// nothing. Non-Chinese text is handled exactly as in `to_pinyin` (kept or
// dropped per `keep_non_chinese`, folded per `lowercase`), and a Chinese
// character without a reading is preserved, so initials of "中国A" are "zga".
// `options.style` and `options.separator` are accepted for symmetry but have
// no effect: initials carry no tone and are concatenated.
[[nodiscard]] CRPC_API result<std::string> to_pinyin_initials(std::string_view utf8_text,
                                                              pinyin_options options = {});

// Every reading of one code point, most common first, de-duplicated *after*
// conversion to `style` (so "行" has "xing"/"hang"/"heng", not "hang" twice).
// A non-Chinese code point, or a Chinese one without a reading, yields an empty
// vector. A surrogate half or a value above U+10FFFF is `invalid_argument`:
// neither is a code point.
[[nodiscard]] CRPC_API result<std::vector<std::string>> pinyin_candidates(
    char32_t code_point, pinyin_style style = pinyin_style::without_tone);

// True for CJK Unified Ideographs Extension A (U+3400..U+4DBF) and CJK
// Unified Ideographs (U+4E00..U+9FFF).
[[nodiscard]] CRPC_API bool is_chinese(char32_t code_point) noexcept;

} // namespace text
} // namespace crpc

#endif // CRPC_TEXT_PINYIN_HPP
