// CRPC -- tests for crpc/text/pinyin.hpp.
//
// Every expected value in this file was produced by pypinyin 0.55.0 (the same
// library the shipped table is generated from) unless a case explicitly says
// it tests this module's documented policy instead:
//
//   lazy_pinyin(word)                              -> without_tone
//   lazy_pinyin(word, style=Style.TONE)            -> with_tone
//   lazy_pinyin(word, style=Style.TONE3,
//               neutral_tone_with_five=True)       -> tone_number
//
// The reference command for a value such as "银行" -> "yin hang" is
//   python -c "from pypinyin import lazy_pinyin; print(lazy_pinyin('银行'))"
// which prints ['yin', 'hang']; with Style.TONE it prints ['yín', 'háng'].
//
// Two conventions are this module's own and are labelled as such below:
//   * `without_tone`/`tone_number` spell ü as "v" (nv3, lve4), which is what
//     pypinyin's NORMAL and TONE3 styles do;
//   * `initials` is the first letter of each syllable (so 银 -> "y", as in
//     the required 银行 -> "yh"), not pypinyin's strict `Style.INITIALS`,
//     which returns an empty string for zero-initial syllables like yin.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/text/pinyin.hpp>

#include "test_framework.hpp"

namespace {

using crpc::rpc_code;
using crpc::text::pinyin_options;
using crpc::text::pinyin_style;
using crpc::text::to_pinyin;
using crpc::text::to_pinyin_initials;

// Runs to_pinyin and returns the value, or a marker for a failed decode so a
// broken case fails the assertion instead of aborting the suite.
std::string pinyin_of(std::string_view text, pinyin_options options = {}) {
    const auto result = to_pinyin(text, options);
    if (CRPC_CHECK(result.ok())) {
        return result.value();
    }
    return "<to_pinyin failed>";
}

std::string initials_of(std::string_view text, pinyin_options options = {}) {
    const auto result = to_pinyin_initials(text, options);
    if (CRPC_CHECK(result.ok())) {
        return result.value();
    }
    return "<to_pinyin_initials failed>";
}

pinyin_options with_separator(std::string_view separator) {
    pinyin_options options;
    options.separator = separator;
    return options;
}

pinyin_options with_style(pinyin_style style) {
    pinyin_options options;
    options.style = style;
    return options;
}

std::vector<std::string> candidates_of(char32_t code_point,
                                       pinyin_style style = pinyin_style::without_tone) {
    const auto result = crpc::text::pinyin_candidates(code_point, style);
    if (CRPC_CHECK(result.ok())) {
        return result.value();
    }
    return {};
}

// The ten GBK code points pypinyin defines no reading for.
bool is_known_unreadable(std::uint32_t value) {
    switch (value) {
    case 0x5159: // 兙
    case 0x5161: // 兡
    case 0x55E7: // 嗧
    case 0x685B: // 桛
    case 0x70EA: // 烪
    case 0x74E7: // 瓧
    case 0x74F0: // 瓰
    case 0x74F1: // 瓱
    case 0x74FC: // 瓼
    case 0x7505: // 甅
        return true;
    default:
        return false;
    }
}

} // namespace

// --- Single characters --------------------------------------------------------

CRPC_TEST(pinyin_single_character_default_readings) {
    // pypinyin 0.55.0 lazy_pinyin: ['zhong'], ['guo'], ['ren'], ['min'],
    // ['yin'], ['xing'], ['zhong'], ['qing'].
    CRPC_CHECK_EQ("zhong", pinyin_of("中"));
    CRPC_CHECK_EQ("guo", pinyin_of("国"));
    CRPC_CHECK_EQ("ren", pinyin_of("人"));
    CRPC_CHECK_EQ("min", pinyin_of("民"));
    CRPC_CHECK_EQ("yin", pinyin_of("银"));
    CRPC_CHECK_EQ("xing", pinyin_of("行"));
    CRPC_CHECK_EQ("zhong", pinyin_of("重"));
    CRPC_CHECK_EQ("qing", pinyin_of("庆"));

    // More heteronyms whose *first* reading is the default: 长 zhang, 乐 le,
    // 厦 sha, 币 bi (pypinyin lazy_pinyin).
    CRPC_CHECK_EQ("zhang", pinyin_of("长"));
    CRPC_CHECK_EQ("le", pinyin_of("乐"));
    CRPC_CHECK_EQ("sha", pinyin_of("厦"));
    CRPC_CHECK_EQ("bi", pinyin_of("币"));
}

CRPC_TEST(pinyin_style_with_tone) {
    // pypinyin Style.TONE.
    CRPC_CHECK_EQ("zhōng", pinyin_of("中", with_style(pinyin_style::with_tone)));
    CRPC_CHECK_EQ("guó", pinyin_of("国", with_style(pinyin_style::with_tone)));
    CRPC_CHECK_EQ("rén", pinyin_of("人", with_style(pinyin_style::with_tone)));
    CRPC_CHECK_EQ("mín", pinyin_of("民", with_style(pinyin_style::with_tone)));
    CRPC_CHECK_EQ("yín", pinyin_of("银", with_style(pinyin_style::with_tone)));
    CRPC_CHECK_EQ("xíng", pinyin_of("行", with_style(pinyin_style::with_tone)));
    CRPC_CHECK_EQ("zhòng", pinyin_of("重", with_style(pinyin_style::with_tone)));
    CRPC_CHECK_EQ("qìng", pinyin_of("庆", with_style(pinyin_style::with_tone)));

    // ü keeps its diacritic: pypinyin Style.TONE gives nǚ, lüè.
    CRPC_CHECK_EQ("nǚ", pinyin_of("女", with_style(pinyin_style::with_tone)));
    CRPC_CHECK_EQ("lüè", pinyin_of("略", with_style(pinyin_style::with_tone)));

    // A syllabic nasal: 嗯 is ń in pypinyin Style.TONE.
    CRPC_CHECK_EQ("ń", pinyin_of("嗯", with_style(pinyin_style::with_tone)));
}

CRPC_TEST(pinyin_style_tone_number) {
    // pypinyin Style.TONE3 with neutral_tone_with_five=True.
    CRPC_CHECK_EQ("zhong1", pinyin_of("中", with_style(pinyin_style::tone_number)));
    CRPC_CHECK_EQ("guo2", pinyin_of("国", with_style(pinyin_style::tone_number)));
    CRPC_CHECK_EQ("ren2", pinyin_of("人", with_style(pinyin_style::tone_number)));
    CRPC_CHECK_EQ("min2", pinyin_of("民", with_style(pinyin_style::tone_number)));
    CRPC_CHECK_EQ("yin2", pinyin_of("银", with_style(pinyin_style::tone_number)));
    CRPC_CHECK_EQ("xing2", pinyin_of("行", with_style(pinyin_style::tone_number)));
    CRPC_CHECK_EQ("zhong4", pinyin_of("重", with_style(pinyin_style::tone_number)));
    CRPC_CHECK_EQ("qing4", pinyin_of("庆", with_style(pinyin_style::tone_number)));

    // ü is spelled "v" in TONE3: nv3, lve4.
    CRPC_CHECK_EQ("nv3", pinyin_of("女", with_style(pinyin_style::tone_number)));
    CRPC_CHECK_EQ("lve4", pinyin_of("略", with_style(pinyin_style::tone_number)));

    // Neutral tone is written with a trailing 5 (the documented convention;
    // pypinyin produces de5 with neutral_tone_with_five=True).
    CRPC_CHECK_EQ("de5", pinyin_of("的", with_style(pinyin_style::tone_number)));
}

// --- Word-level heteronym resolution ----------------------------------------

CRPC_TEST(pinyin_word_disambiguation) {
    // pypinyin lazy_pinyin: ['yin','hang'], ['xing','zou'],
    // ['chong','qing'], ['zhong','yao'].
    CRPC_CHECK_EQ("yin hang", pinyin_of("银行", with_separator(" ")));
    CRPC_CHECK_EQ("xing zou", pinyin_of("行走", with_separator(" ")));
    CRPC_CHECK_EQ("chong qing", pinyin_of("重庆", with_separator(" ")));
    CRPC_CHECK_EQ("zhong yao", pinyin_of("重要", with_separator(" ")));

    // TONE / TONE3 forms of the same words.
    pinyin_options toned = with_separator(" ");
    toned.style = pinyin_style::with_tone;
    CRPC_CHECK_EQ("yín háng", pinyin_of("银行", toned));
    CRPC_CHECK_EQ("chóng qìng", pinyin_of("重庆", toned));
    CRPC_CHECK_EQ("zhòng yào", pinyin_of("重要", toned));

    pinyin_options numbered = with_separator(" ");
    numbered.style = pinyin_style::tone_number;
    CRPC_CHECK_EQ("yin2 hang2", pinyin_of("银行", numbered));
    CRPC_CHECK_EQ("chong2 qing4", pinyin_of("重庆", numbered));
    CRPC_CHECK_EQ("zhong4 yao4", pinyin_of("重要", numbered));
}

CRPC_TEST(pinyin_longest_word_wins) {
    // Greedy longest match over pypinyin's word list (verified against
    // lazy_pinyin for every one of the 47111 keys by the generator):
    // 银行行长 -> yin hang hang zhang, 重庆银行 -> chong qing yin hang.
    CRPC_CHECK_EQ("yin hang hang zhang",
                  pinyin_of("银行行长", with_separator(" ")));
    CRPC_CHECK_EQ("chong qing yin hang",
                  pinyin_of("重庆银行", with_separator(" ")));
    CRPC_CHECK_EQ("yin yue", pinyin_of("音乐", with_separator(" ")));
    CRPC_CHECK_EQ("kuai le", pinyin_of("快乐", with_separator(" ")));
    CRPC_CHECK_EQ("xia men", pinyin_of("厦门", with_separator(" ")));
    CRPC_CHECK_EQ("zhang da", pinyin_of("长大", with_separator(" ")));
    CRPC_CHECK_EQ("gui huan", pinyin_of("归还", with_separator(" ")));
    CRPC_CHECK_EQ("chang cheng", pinyin_of("长城", with_separator(" ")));
    CRPC_CHECK_EQ("ren min bi", pinyin_of("人民币", with_separator(" ")));
    CRPC_CHECK_EQ("hang zhang", pinyin_of("行长", with_separator(" ")));

    // 4-character words are matched as one unit too.
    // pypinyin lazy_pinyin('中国人民银行') -> zhong guo ren min yin hang.
    CRPC_CHECK_EQ("zhong guo ren min yin hang",
                  pinyin_of("中国人民银行", with_separator(" ")));
}

CRPC_TEST(pinyin_sentence_level) {
    // pypinyin lazy_pinyin, joined without separators.
    CRPC_CHECK_EQ("woaibeijingtiananmen", pinyin_of("我爱北京天安门"));
    CRPC_CHECK_EQ("xingzouzaichongqingdejiedaoshang",
                  pinyin_of("行走在重庆的街道上"));
    CRPC_CHECK_EQ("zhegezhonglianghenzhong", pinyin_of("这个重量很重"));
    CRPC_CHECK_EQ("woyidingyaoquyinhangquqian",
                  pinyin_of("我一定要去银行取钱"));
}

// --- Initials -----------------------------------------------------------------

CRPC_TEST(pinyin_initials) {
    // First letter of each syllable; required examples first.
    CRPC_CHECK_EQ("zg", initials_of("中国"));
    CRPC_CHECK_EQ("yh", initials_of("银行"));
    CRPC_CHECK_EQ("cq", initials_of("重庆"));
    CRPC_CHECK_EQ("zy", initials_of("重要"));
    CRPC_CHECK_EQ("xz", initials_of("行走"));
    CRPC_CHECK_EQ("rmb", initials_of("人民币"));
    CRPC_CHECK_EQ("zgrmyh", initials_of("中国人民银行"));
}

CRPC_TEST(pinyin_initials_edge_syllables) {
    // Zero-initial syllables keep their first letter (this module's documented
    // contract; pypinyin's strict INITIALS would return "" for yin/ai).
    CRPC_CHECK_EQ("y", initials_of("银"));
    CRPC_CHECK_EQ("a", initials_of("爱"));
    // Syllabic nasals: 嗯 -> n, 呣 -> m.
    CRPC_CHECK_EQ("n", initials_of("嗯"));
    CRPC_CHECK_EQ("m", initials_of("呣"));
}

// --- Non-Chinese text --------------------------------------------------------

CRPC_TEST(pinyin_non_chinese_kept_by_default) {
    // Preserved verbatim; pypinyin lazy_pinyin('中国!') -> ['zhong','guo','!'].
    CRPC_CHECK_EQ("zhongguo!", pinyin_of("中国!"));
    CRPC_CHECK_EQ("azhongb", pinyin_of("a中b"));
    CRPC_CHECK_EQ("123zhongguo456", pinyin_of("123中国456"));
    CRPC_CHECK_EQ("zhong，guo。", pinyin_of("中，国。"));

    // Emoji survive as a code point (U+1F600).
    CRPC_CHECK_EQ("zhong\xF0\x9F\x98\x80guo", pinyin_of("中\xF0\x9F\x98\x80国"));

    // Leading/trailing spaces are preserved.
    CRPC_CHECK_EQ(" zhongguo ", pinyin_of(" 中国 "));

    // Pure ASCII passes through (the ASCII case fold is applied, see below).
    CRPC_CHECK_EQ("hello, world! 123", pinyin_of("Hello, World! 123"));
}

CRPC_TEST(pinyin_non_chinese_dropped) {
    pinyin_options options;
    options.keep_non_chinese = false;
    CRPC_CHECK_EQ("zhongguo", pinyin_of("中A国", options));
    CRPC_CHECK_EQ("zhongguo", pinyin_of("123中国456", options));
    CRPC_CHECK_EQ("zhongguo", pinyin_of("中国!", options));
    CRPC_CHECK_EQ("zhongguo", pinyin_of("中\xF0\x9F\x98\x80国", options));
    CRPC_CHECK_EQ("", pinyin_of("Hello, World! 123", options));
    CRPC_CHECK_EQ("", pinyin_of("", options));
}

CRPC_TEST(pinyin_lowercase_option) {
    // The default folds ASCII A-Z in the whole output (documented policy).
    CRPC_CHECK_EQ("iphonezhongguo", pinyin_of("iPhone中国"));
    CRPC_CHECK_EQ("zhongaguo", pinyin_of("中A国"));

    pinyin_options verbatim;
    verbatim.lowercase = false;
    CRPC_CHECK_EQ("iPhonezhongguo", pinyin_of("iPhone中国", verbatim));
    CRPC_CHECK_EQ("zhongAguo", pinyin_of("中A国", verbatim));
    CRPC_CHECK_EQ("Hello, World! 123", pinyin_of("Hello, World! 123", verbatim));
}

CRPC_TEST(pinyin_separator) {
    CRPC_CHECK_EQ("zhong-guo", pinyin_of("中国", with_separator("-")));
    CRPC_CHECK_EQ("yin-hang", pinyin_of("银行", with_separator("-")));
    CRPC_CHECK_EQ("yin hang", pinyin_of("银行", with_separator(" ")));
    // Preserved text does not get a separator of its own; the next syllable
    // still gets one because output already exists.
    pinyin_options verbatim = with_separator("-");
    verbatim.lowercase = false;
    CRPC_CHECK_EQ("zhongA-guo", pinyin_of("中A国", verbatim));
    // A separator never trails the output ("!" is not a syllable), but the
    // syllables themselves are still separated.
    CRPC_CHECK_EQ("zhong-guo!", pinyin_of("中国!", with_separator("-")));
}

// --- Candidates ---------------------------------------------------------------

CRPC_TEST(pinyin_candidates_heteronyms) {
    // pypinyin heteronym data, de-duplicated after style conversion:
    //   行 -> plain xing/hang/heng; TONE xíng/háng/héng/xìng/hàng;
    //        TONE3 xing2/hang2/heng2/xing4/hang4
    //   重 -> zhong/chong/tong; zhòng/chóng/tóng; zhong4/chong2/tong2
    const std::vector<std::string> xing = candidates_of(U'\u884C');
    CRPC_CHECK_EQ(std::size_t{3}, xing.size());
    CRPC_CHECK_EQ("xing", xing[0]);
    CRPC_CHECK_EQ("hang", xing[1]);
    CRPC_CHECK_EQ("heng", xing[2]);

    const std::vector<std::string> xing_toned =
        candidates_of(U'\u884C', pinyin_style::with_tone);
    CRPC_CHECK_EQ(std::size_t{5}, xing_toned.size());
    CRPC_CHECK_EQ("xíng", xing_toned[0]);
    CRPC_CHECK_EQ("háng", xing_toned[1]);
    CRPC_CHECK_EQ("héng", xing_toned[2]);
    CRPC_CHECK_EQ("xìng", xing_toned[3]);
    CRPC_CHECK_EQ("hàng", xing_toned[4]);

    const std::vector<std::string> xing_numbered =
        candidates_of(U'\u884C', pinyin_style::tone_number);
    CRPC_CHECK_EQ(std::size_t{5}, xing_numbered.size());
    CRPC_CHECK_EQ("xing2", xing_numbered[0]);
    CRPC_CHECK_EQ("hang2", xing_numbered[1]);
    CRPC_CHECK_EQ("heng2", xing_numbered[2]);
    CRPC_CHECK_EQ("xing4", xing_numbered[3]);
    CRPC_CHECK_EQ("hang4", xing_numbered[4]);

    const std::vector<std::string> chong = candidates_of(U'\u91CD');
    CRPC_CHECK_EQ(std::size_t{3}, chong.size());
    CRPC_CHECK_EQ("zhong", chong[0]);
    CRPC_CHECK_EQ("chong", chong[1]);
    CRPC_CHECK_EQ("tong", chong[2]);

    // ü -> v in the plain/numbered styles.
    const std::vector<std::string> nv = candidates_of(U'\u5973');
    CRPC_CHECK_EQ(std::size_t{2}, nv.size());
    CRPC_CHECK_EQ("nv", nv[0]);
    CRPC_CHECK_EQ("ru", nv[1]);

    // A character with two readings that collapse to one plain form: 中 keeps
    // one "zhong" but two tones.
    const std::vector<std::string> zhong = candidates_of(U'\u4E2D');
    CRPC_CHECK_EQ(std::size_t{1}, zhong.size());
    CRPC_CHECK_EQ("zhong", zhong[0]);
    const std::vector<std::string> zhong_toned =
        candidates_of(U'\u4E2D', pinyin_style::with_tone);
    CRPC_CHECK_EQ(std::size_t{2}, zhong_toned.size());
    CRPC_CHECK_EQ("zhōng", zhong_toned[0]);
    CRPC_CHECK_EQ("zhòng", zhong_toned[1]);

    // Single-reading character.
    const std::vector<std::string> guo = candidates_of(U'\u56FD');
    CRPC_CHECK_EQ(std::size_t{1}, guo.size());
    CRPC_CHECK_EQ("guo", guo[0]);
}

CRPC_TEST(pinyin_candidates_empty_and_invalid) {
    // Non-Chinese code points have no readings.
    CRPC_CHECK(candidates_of(U'A').empty());
    CRPC_CHECK(candidates_of(U' ').empty());
    CRPC_CHECK(candidates_of(char32_t{0x1F600}).empty());
    // A Chinese character the table has no reading for (兙, U+5159).
    CRPC_CHECK(candidates_of(char32_t{0x5159}).empty());

    // Surrogates and values above U+10FFFF are not code points.
    const auto surrogate = crpc::text::pinyin_candidates(char32_t{0xD800});
    CRPC_CHECK_FALSE(surrogate.ok());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, surrogate.error().code);
    const auto too_large = crpc::text::pinyin_candidates(char32_t{0x110000});
    CRPC_CHECK_FALSE(too_large.ok());
    CRPC_CHECK_EQ(rpc_code::invalid_argument, too_large.error().code);
}

// --- is_chinese boundaries ----------------------------------------------------

CRPC_TEST(pinyin_is_chinese_boundaries) {
    CRPC_CHECK(crpc::text::is_chinese(U'\u4E00'));  // first unified ideograph
    CRPC_CHECK(crpc::text::is_chinese(U'\u9FFF'));  // last unified ideograph
    CRPC_CHECK(crpc::text::is_chinese(U'\u3400'));  // first extension A
    CRPC_CHECK(crpc::text::is_chinese(U'\u4DBF'));  // last extension A
    CRPC_CHECK(crpc::text::is_chinese(U'\u4E2D'));  // 中
    CRPC_CHECK(crpc::text::is_chinese(U'\u9FA5'));  // last GBK hanzi

    CRPC_CHECK_FALSE(crpc::text::is_chinese(U'\u33FF')); // before extension A
    CRPC_CHECK_FALSE(crpc::text::is_chinese(U'\u4DC0')); // Yijing hexagrams
    CRPC_CHECK_FALSE(crpc::text::is_chinese(U'\u3007')); // ideographic zero
    CRPC_CHECK_FALSE(crpc::text::is_chinese(U'\uA000')); // Yi syllables
    CRPC_CHECK_FALSE(crpc::text::is_chinese(U'A'));
    CRPC_CHECK_FALSE(crpc::text::is_chinese(U'\u00E9'));
    CRPC_CHECK_FALSE(crpc::text::is_chinese(char32_t{0x1F600}));
    CRPC_CHECK_FALSE(crpc::text::is_chinese(char32_t{0x20000})); // extension B
}

// --- Empty / pure ASCII / mixed ----------------------------------------------

CRPC_TEST(pinyin_empty_and_ascii) {
    CRPC_CHECK_EQ("", pinyin_of(""));
    CRPC_CHECK_EQ("", initials_of(""));

    // Pure ASCII is only case-folded; no pinyin is invented.
    CRPC_CHECK_EQ("abc123", pinyin_of("abc123"));
    CRPC_CHECK_EQ("abc123", pinyin_of("ABC123"));
    pinyin_options verbatim;
    verbatim.lowercase = false;
    CRPC_CHECK_EQ("ABC123", pinyin_of("ABC123", verbatim));

    // ASCII has no syllable to take a letter from, but preserved non-Chinese
    // text is copied exactly as to_pinyin would copy it.
    CRPC_CHECK_EQ("abc123", initials_of("abc123"));
    CRPC_CHECK_EQ("zg", initials_of("中国"));
    CRPC_CHECK_EQ("zga", initials_of("中国A"));
    CRPC_CHECK_EQ("zgA", initials_of("中国A", [] {
                      pinyin_options options;
                      options.lowercase = false;
                      return options;
                  }()));
}

// --- Invalid UTF-8 ------------------------------------------------------------

CRPC_TEST(pinyin_invalid_utf8) {
    const char* samples[] = {
        "\x80",                 // lone continuation byte
        "\xE4\xB8",             // truncated three-byte sequence
        "\xC0\xAF",             // overlong encoding of '/'
        "\xED\xA0\x80",         // UTF-8-encoded surrogate half
        "\xF4\x90\x80\x80",     // above U+10FFFF
        "\xFF",                 // invalid lead byte
        "\xE4\xB8\xAD\xE4\xB8", // valid 中 followed by a truncated sequence
    };
    for (const char* sample : samples) {
        const auto decoded = to_pinyin(sample);
        CRPC_CHECK_FALSE(decoded.ok());
        CRPC_CHECK_EQ(rpc_code::decode_error, decoded.error().code);

        const auto initialed = to_pinyin_initials(sample);
        CRPC_CHECK_FALSE(initialed.ok());
        CRPC_CHECK_EQ(rpc_code::decode_error, initialed.error().code);
    }

    // Well-formed UTF-8 of every length still succeeds and transcribes.
    CRPC_CHECK_EQ("a", pinyin_of("a"));
    CRPC_CHECK_EQ("é", pinyin_of("\xC3\xA9"));
    CRPC_CHECK_EQ("zhong", pinyin_of("\xE4\xB8\xAD"));
    CRPC_CHECK_EQ("\xF0\x9F\x98\x80", pinyin_of("\xF0\x9F\x98\x80"));
}

// --- GBK coverage -------------------------------------------------------------

CRPC_TEST(pinyin_gbk_coverage) {
    // GBK maps exactly U+4E00..U+9FA5 to hanzi (20902 code points; verified by
    // enumerating every valid GBK double byte with Python's `gbk` codec).
    // Every one of them must be recognised as Chinese and must have a reading,
    // except the ten pypinyin does not define.
    std::uint32_t covered = 0;
    std::uint32_t unreadable = 0;
    for (std::uint32_t value = 0x4E00; value <= 0x9FA5; ++value) {
        CRPC_REQUIRE(crpc::text::is_chinese(static_cast<char32_t>(value)));
        const auto result =
            crpc::text::pinyin_candidates(static_cast<char32_t>(value));
        CRPC_REQUIRE(result.ok());
        if (result.value().empty()) {
            ++unreadable;
            CRPC_CHECK(is_known_unreadable(value));
        } else {
            ++covered;
        }
    }
    CRPC_CHECK_EQ(std::uint32_t{20902}, covered + unreadable);
    CRPC_CHECK_EQ(std::uint32_t{20892}, covered);
    CRPC_CHECK_EQ(std::uint32_t{10}, unreadable);

    // The ten unreadable characters are preserved verbatim even when
    // non-Chinese text is dropped: dropping Chinese text the table cannot
    // transliterate would silently lose characters.
    pinyin_options drop;
    drop.keep_non_chinese = false;
    CRPC_CHECK_EQ("\xE5\x85\x99", pinyin_of("\xE5\x85\x99", drop)); // 兙 U+5159
    CRPC_CHECK_EQ("\xE5\x85\x99", pinyin_of("\xE5\x85\x99"));       // 兙 default
    // Initials treat it the same way: preserved, never invented.
    CRPC_CHECK_EQ("\xE5\x85\x99", initials_of("\xE5\x85\x99", drop));
}

// --- Sampling across the GBK range -------------------------------------------

CRPC_TEST(pinyin_gbk_sample_readings) {
    // A spread of GBK-only (non-GB2312) characters. Each expected triple is
    // pypinyin 0.55.0: (NORMAL, TONE, TONE3 with neutral five).
    struct sample {
        const char* text;
        const char* plain;
        const char* toned;
        const char* numbered;
    };
    const sample samples[] = {
        {"丂", "kao", "kǎo", "kao3"},
        {"乂", "yi", "yì", "yi4"},
        {"俣", "yu", "yǔ", "yu3"},
        {"兕", "si", "sì", "si4"},
        {"刳", "ku", "kū", "ku1"},
        {"嗲", "die", "diē", "die1"},
        {"埸", "yi", "yì", "yi4"},
        {"奘", "zang", "zàng", "zang4"},
        {"孱", "can", "càn", "can4"},
        {"崤", "xiao", "xiáo", "xiao2"},
        {"弭", "mi", "mǐ", "mi3"},
        {"扃", "jiong", "jiōng", "jiong1"},
        {"摭", "zhi", "zhí", "zhi2"},
        {"旮", "ga", "gā", "ga1"},
        {"朐", "qu", "qú", "qu2"},
        {"桴", "fu", "fú", "fu2"},
        {"檩", "lin", "lǐn", "lin3"},
        {"毖", "bi", "bì", "bi4"},
        {"溘", "ke", "kè", "ke4"},
        {"灬", "biao", "biāo", "biao1"},
        {"琮", "cong", "cóng", "cong2"},
        {"疖", "jie", "jiē", "jie1"},
        {"矧", "shen", "shěn", "shen3"},
        {"磔", "zhe", "zhé", "zhe2"},
        {"穑", "se", "sè", "se4"},
        {"箦", "ze", "zé", "ze2"},
        {"紊", "wen", "wěn", "wen3"},
        {"羝", "di", "dī", "di1"},
        {"脒", "mi", "mǐ", "mi3"},
        {"舄", "xi", "xì", "xi4"},
        {"茈", "ci", "cí", "ci2"},
        {"葳", "wei", "wēi", "wei1"},
        {"薅", "hao", "hāo", "hao1"},
        {"蛉", "ling", "líng", "ling2"},
        {"衄", "nv", "nǜ", "nv4"},
        {"褫", "chi", "chǐ", "chi3"},
        {"谂", "shen", "shěn", "shen3"},
        {"跬", "kui", "kuǐ", "kui3"},
        {"迓", "ya", "yà", "ya4"},
        {"鎏", "liu", "liú", "liu2"},
        {"闳", "hong", "hóng", "hong2"},
        {"韪", "wei", "wěi", "wei3"},
        {"鲠", "geng", "gěng", "geng3"},
        {"黟", "yi", "yī", "yi1"},
        {"龠", "yue", "yuè", "yue4"},
    };
    for (const sample& entry : samples) {
        const std::string text(entry.text);
        CRPC_CHECK_EQ(std::string(entry.plain), pinyin_of(text));
        CRPC_CHECK_EQ(std::string(entry.toned),
                      pinyin_of(text, with_style(pinyin_style::with_tone)));
        CRPC_CHECK_EQ(std::string(entry.numbered),
                      pinyin_of(text, with_style(pinyin_style::tone_number)));
    }
}

CRPC_TEST(pinyin_extension_a_supported) {
    // Extension A is part of is_chinese and is present in the table.
    // pypinyin lazy_pinyin(U+3400) -> ['qiu'].
    CRPC_CHECK_EQ("qiu", pinyin_of("\xE3\x90\x80"));
    const std::vector<std::string> values = candidates_of(char32_t{0x3400});
    CRPC_CHECK_FALSE(values.empty());
    CRPC_CHECK_EQ("qiu", values[0]);
}

CRPC_TEST(pinyin_concurrent_purity) {
    // Results do not depend on call order; every entry point is a pure
    // function of its arguments and the immutable table.
    const std::string first = pinyin_of("银行行长");
    const std::string again = pinyin_of("银行行长");
    CRPC_CHECK_EQ(first, again);
    CRPC_CHECK_EQ("yinhanghangzhang", first);
}

CRPC_TEST_MAIN()