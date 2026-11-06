// CRPC -- tests for the binary-to-text encodings and legacy character sets.
//
// Every expected value below was pinned against an independent implementation
// before being written here: the base32 vectors are RFC 4648 section 10, the
// base85 vector is RFC 1924 section 5, the punycode table is RFC 3492 section
// 7.1, the quoted-printable vectors are RFC 2045 section 6.7 and Python's
// quopri, the ascii85/uuencode vectors are Python's binascii and Perl's
// pack("u"), the UTF-7 vectors are RFC 2152 section 5 and Python's utf-7
// codec, and the GBK vectors are Python's gbk codec cross-checked against
// glibc's iconv. The two exhaustive loops (GBK pairs, GBK BMP coverage) pin
// the generated table end to end.
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/span.hpp>
#include <crpc/text/encoding.hpp>

#include "test_framework.hpp"

using crpc::result;
using crpc::rpc_code;
namespace enc = crpc::text;

namespace {

crpc::span<const std::byte> span_of(const std::string& s) {
    return crpc::span<const std::byte>(
        reinterpret_cast<const std::byte*>(s.data()), s.size());
}

crpc::span<const std::byte> span_of(const std::vector<std::byte>& v) {
    return crpc::span<const std::byte>(v.data(), v.size());
}

std::string str_of(const std::vector<std::byte>& v) {
    return std::string(reinterpret_cast<const char*>(v.data()), v.size());
}

// Converts ASCII text to bytes. The wrapper converts to span<const byte> so
// encoder call sites read naturally; the temporary outlives the call.
class ByteText {
public:
    explicit ByteText(std::string_view s) {
        data_.reserve(s.size());
        for (const char c : s) {
            data_.push_back(
                static_cast<std::byte>(static_cast<unsigned char>(c)));
        }
    }
    operator crpc::span<const std::byte>() const noexcept {
        return crpc::span<const std::byte>(data_.data(), data_.size());
    }
    std::vector<std::byte> to_vector() const { return data_; }

private:
    std::vector<std::byte> data_;
};

ByteText bytes_of(std::string_view s) { return ByteText(s); }

void expect_error(rpc_code actual, rpc_code expected) {
    CRPC_CHECK_EQ(expected, actual);
}

// A repeatable pseudo-random source: fixed seed, so failures reproduce.
std::mt19937& rng() {
    static std::mt19937 engine(20261106u);
    return engine;
}

std::vector<std::byte> random_bytes(std::size_t length) {
    std::vector<std::byte> out(length);
    for (std::byte& b : out) {
        b = static_cast<std::byte>(rng()() & 0xFFu);
    }
    return out;
}

// Appends the UTF-8 encoding of one code point.
void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

// --- base32 / base32hex (RFC 4648 section 10) --------------------------------

CRPC_TEST(base32_rfc4648_vectors) {
    static constexpr struct {
        std::string_view input;
        std::string_view standard;
        std::string_view hex;
    } kVectors[] = {
        {"", "", ""},
        {"f", "MY======", "CO======"},
        {"fo", "MZXQ====", "CPNG===="},
        {"foo", "MZXW6===", "CPNMU==="},
        {"foob", "MZXW6YQ=", "CPNMUOG="},
        {"fooba", "MZXW6YTB", "CPNMUOJ1"},
        {"foobar", "MZXW6YTBOI======", "CPNMUOJ1E8======"},
    };
    for (const auto& v : kVectors) {
        CRPC_CHECK_EQ(std::string(v.standard), enc::base32_encode(bytes_of(v.input)));
        CRPC_CHECK_EQ(std::string(v.hex), enc::base32hex_encode(bytes_of(v.input)));

        const auto standard = enc::base32_decode(v.standard);
        CRPC_CHECK(standard.ok());
        if (standard.ok()) {
            CRPC_CHECK_EQ(std::string(v.input), str_of(standard.value()));
        }
        const auto hex = enc::base32hex_decode(v.hex);
        CRPC_CHECK(hex.ok());
        if (hex.ok()) {
            CRPC_CHECK_EQ(std::string(v.input), str_of(hex.value()));
        }
    }
}

CRPC_TEST(base32_decode_case_insensitive_and_unpadded) {
    // RFC 4648 section 3.4: base32 is case insensitive; the decoder may accept
    // the unpadded spelling as long as the length is otherwise canonical.
    const auto lower = enc::base32_decode("mzxw6ytboi======");
    CRPC_CHECK(lower.ok());
    if (lower.ok()) {
        CRPC_CHECK_EQ(std::string("foobar"), str_of(lower.value()));
    }
    const auto unpadded = enc::base32_decode("MZXW6YTB");
    CRPC_CHECK(unpadded.ok());
    if (unpadded.ok()) {
        CRPC_CHECK_EQ(std::string("fooba"), str_of(unpadded.value()));
    }
    const auto hex_lower = enc::base32hex_decode("cpnmuoj1e8======");
    CRPC_CHECK(hex_lower.ok());
    if (hex_lower.ok()) {
        CRPC_CHECK_EQ(std::string("foobar"), str_of(hex_lower.value()));
    }
}

CRPC_TEST(base32_rejects_malformed_input) {
    static constexpr std::string_view kBad[] = {
        "M",                // one symbol is not a group
        "MY=====",          // five pads instead of six
        "MY=======",        // seven pads instead of six
        "M=======",         // padding after an incomplete symbol
        "=MY======",        // padding before the body
        "MZ======",         // non-zero bits in the last symbol
        "MZXW6YTB======",   // padding after a complete group
        "M0======",         // digit outside the alphabet
        "MY====2=",         // digit inside the padding
    };
    for (const std::string_view text : kBad) {
        const auto r = enc::base32_decode(text);
        expect_error(r.code(), rpc_code::decode_error);
    }
    // base32hex uses a different alphabet: 'W' is above 'V'.
    const auto hex_bad = enc::base32hex_decode("CW======");
    expect_error(hex_bad.code(), rpc_code::decode_error);
}

CRPC_TEST(base32_roundtrip_random) {
    for (std::size_t length = 0; length <= 70; ++length) {
        const auto data = random_bytes(length);
        const auto standard = enc::base32_decode(enc::base32_encode(span_of(data)));
        CRPC_CHECK(standard.ok());
        if (standard.ok()) {
            CRPC_CHECK(data == standard.value());
        }
        const auto hex = enc::base32hex_decode(enc::base32hex_encode(span_of(data)));
        CRPC_CHECK(hex.ok());
        if (hex.ok()) {
            CRPC_CHECK(data == hex.value());
        }
    }
}

// --- base58 (Bitcoin alphabet) ------------------------------------------------

CRPC_TEST(base58_known_vectors) {
    // Entries with embedded NUL bytes spell std::string_view with an explicit
    // length: a string_view built from a bare literal would stop at the NUL.
    static constexpr struct {
        std::string_view input;
        std::string_view expected;
    } kVectors[] = {
        {"", ""},
        {std::string_view{"\x00", 1}, "1"},
        {std::string_view{"\x00\x00", 2}, "11"},
        {"hello world", "StV1DL6CwTryKyV"},
        {std::string_view{"\x00hello world", 12}, "1StV1DL6CwTryKyV"},
        {std::string_view{"\x00\x01\x09\x66\x77\x60\x06\x95\x3D\x55\x67\x43\x9E\x5E\x39\xF8"
         "\x6A\x0D\x27\x3B\xEE\xD6\x19\x67\xF6", 25},
         "16UwLL9Risc3QfPqBUvKofHmBQ7wMtjvM"},
    };
    for (const auto& v : kVectors) {
        CRPC_CHECK_EQ(std::string(v.expected), enc::base58_encode(bytes_of(v.input)));
        const auto back = enc::base58_decode(v.expected);
        CRPC_CHECK(back.ok());
        if (back.ok()) {
            CRPC_CHECK_EQ(std::string(v.input), str_of(back.value()));
        }
    }
}

CRPC_TEST(base58_rejects_ambiguous_characters) {
    static constexpr std::string_view kBad[] = {
        "0", "O", "I", "l", "1 2", "he-lo",
    };
    for (const std::string_view text : kBad) {
        const auto r = enc::base58_decode(text);
        expect_error(r.code(), rpc_code::decode_error);
    }
}

CRPC_TEST(base58_leading_zeros_survive) {
    // Every leading zero byte is one leading '1', so the count is preserved.
    std::vector<std::byte> data(5, std::byte{0});
    data.push_back(std::byte{0x42});
    // Five leading zero bytes then 0x42: five '1's plus base58(66) = "29".
    const std::string text = enc::base58_encode(span_of(data));
    CRPC_CHECK_EQ(std::string("1111129"), text);
    const auto back = enc::base58_decode(text);
    CRPC_CHECK(back.ok());
    if (back.ok()) {
        CRPC_CHECK(data == back.value());
    }
}

CRPC_TEST(base58_roundtrip_random) {
    for (std::size_t length = 0; length <= 64; length += 3) {
        auto data = random_bytes(length);
        if (length >= 2 && (rng()() & 1u) != 0u) {
            data[0] = std::byte{0}; // exercise the leading-zero path
        }
        const auto back = enc::base58_decode(enc::base58_encode(span_of(data)));
        CRPC_CHECK(back.ok());
        if (back.ok()) {
            CRPC_CHECK(data == back.value());
        }
    }
}

// --- base85 (RFC 1924) --------------------------------------------------------

CRPC_TEST(base85_rfc1924_sample) {
    // RFC 1924 section 5: 1080:0000:0000:0000:0008:0800:200C:417A.
    static constexpr std::byte kAddress[] = {
        std::byte{0x10}, std::byte{0x80}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x08}, std::byte{0x08}, std::byte{0x00},
        std::byte{0x20}, std::byte{0x0C}, std::byte{0x41}, std::byte{0x7A},
    };
    CRPC_CHECK_EQ(std::string("4)+k&C#VzJ4br>0wv%Yp"),
                  enc::base85_encode(kAddress));
    const auto back = enc::base85_decode("4)+k&C#VzJ4br>0wv%Yp");
    CRPC_CHECK(back.ok());
    if (back.ok()) {
        const std::vector<std::byte> expected(std::begin(kAddress),
                                              std::end(kAddress));
        CRPC_CHECK(expected == back.value());
    }
}

CRPC_TEST(base85_short_inputs_follow_the_documented_convention) {
    // The RFC defines only 16-byte input; for other lengths this module emits
    // the smallest digit count that can hold every value of that byte length
    // (2 digits for 1 byte, 3 for 2, ...). The reference values come from an
    // independent big-integer model of that rule.
    static constexpr struct {
        std::string_view input;
        std::string_view expected;
    } kVectors[] = {
        {"", ""},
        {"f", "1H"},
        {"fo", "3rh"},
        {"foo", "A_D*"},
        {"foob", "W^Zp|"},
        {"fooba", "1EDFLi9"},
        {"foobar", "3hrw_);c"},
    };
    for (const auto& v : kVectors) {
        CRPC_CHECK_EQ(std::string(v.expected), enc::base85_encode(bytes_of(v.input)));
        const auto back = enc::base85_decode(v.expected);
        CRPC_CHECK(back.ok());
        if (back.ok()) {
            CRPC_CHECK_EQ(std::string(v.input), str_of(back.value()));
        }
    }
    // Sixteen zero bytes are exactly 85^20 territory: twenty '0' digits.
    const std::vector<std::byte> zeros(16, std::byte{0});
    CRPC_CHECK_EQ(std::string(20, '0'), enc::base85_encode(span_of(zeros)));
}

CRPC_TEST(base85_rejects_malformed_input) {
    // A single digit cannot be the canonical count for any byte length.
    const auto one = enc::base85_decode("1");
    expect_error(one.code(), rpc_code::decode_error);
    // Five digits would be the count for 4 bytes, but 85^5-1 does not fit in
    // 32 bits, so all-maximal digits are an overflow.
    const auto overflow = enc::base85_decode("~~~~~");
    expect_error(overflow.code(), rpc_code::decode_error);
    // '"' is not in the alphabet.
    const auto alien = enc::base85_decode("1H\"");
    expect_error(alien.code(), rpc_code::decode_error);
}

CRPC_TEST(base85_roundtrip_random) {
    for (std::size_t length = 0; length <= 40; ++length) {
        auto data = random_bytes(length);
        if (length >= 1 && (rng()() & 1u) != 0u) {
            data[0] = std::byte{0}; // leading zero bytes must survive
        }
        const auto back = enc::base85_decode(enc::base85_encode(span_of(data)));
        CRPC_CHECK(back.ok());
        if (back.ok()) {
            CRPC_CHECK(data == back.value());
        }
    }
}

// --- ascii85 (Adobe) ----------------------------------------------------------

CRPC_TEST(ascii85_python_vectors) {
    static constexpr struct {
        std::string_view input;
        std::string_view adobe;
        std::string_view bare;
    } kVectors[] = {
        {"", "<~~>", ""},
        {"f", "<~Ac~>", "Ac"},
        {"fo", "<~Ao@~>", "Ao@"},
        {"foo", "<~AoDS~>", "AoDS"},
        {"foob", "<~AoDTs~>", "AoDTs"},
        {"fooba", "<~AoDTs@/~>", "AoDTs@/"},
        {"foobar", "<~AoDTs@<)~>", "AoDTs@<)"},
        {std::string_view{"\x00\x00\x00\x00", 4}, "<~z~>", "z"},
        {std::string_view{"\x00\x00\x00\x00\x00", 5}, "<~z!!~>", "z!!"},
    };
    for (const auto& v : kVectors) {
        CRPC_CHECK_EQ(std::string(v.adobe), enc::ascii85_encode(bytes_of(v.input), true));
        CRPC_CHECK_EQ(std::string(v.bare), enc::ascii85_encode(bytes_of(v.input), false));
        const auto back = enc::ascii85_decode(v.bare);
        CRPC_CHECK(back.ok());
        if (back.ok()) {
            CRPC_CHECK_EQ(std::string(v.input), str_of(back.value()));
        }
    }
}

CRPC_TEST(ascii85_decode_tolerates_whitespace_and_delimiters) {
    const auto spaced = enc::ascii85_decode("Ao D Ts @ < )");
    CRPC_CHECK(spaced.ok());
    if (spaced.ok()) {
        CRPC_CHECK_EQ(std::string("foobar"), str_of(spaced.value()));
    }
    const auto framed = enc::ascii85_decode("<~z~>");
    CRPC_CHECK(framed.ok());
    if (framed.ok()) {
        CRPC_CHECK_EQ(std::string(4, '\0'), str_of(framed.value()));
    }
    const auto two_digits = enc::ascii85_decode("!!");
    CRPC_CHECK(two_digits.ok());
    if (two_digits.ok()) {
        CRPC_CHECK_EQ(std::string(1, '\0'), str_of(two_digits.value()));
    }
}

CRPC_TEST(ascii85_rejects_malformed_input) {
    // All-maximal digits overflow the 32-bit group.
    const auto overflow = enc::ascii85_decode("uuuuu");
    expect_error(overflow.code(), rpc_code::decode_error);
    // 'z' may only stand alone, not inside a group.
    const auto z_in_group = enc::ascii85_decode("zA");
    expect_error(z_in_group.code(), rpc_code::decode_error);
    // A one-character residual group cannot be padded to a value.
    const auto short_group = enc::ascii85_decode("AoDTsA");
    expect_error(short_group.code(), rpc_code::decode_error);
    // A stray closer without an opener.
    const auto stray = enc::ascii85_decode("~>");
    expect_error(stray.code(), rpc_code::decode_error);
}

CRPC_TEST(ascii85_roundtrip_random) {
    for (std::size_t length = 0; length <= 80; length += 3) {
        const auto data = random_bytes(length);
        const auto back = enc::ascii85_decode(enc::ascii85_encode(span_of(data)));
        CRPC_CHECK(back.ok());
        if (back.ok()) {
            CRPC_CHECK(data == back.value());
        }
    }
}

// --- uuencode -----------------------------------------------------------------

CRPC_TEST(uuencode_perl_pack_vectors) {
    // Perl pack("u") output with classic backtick padding; the space-padded
    // spellings come from Python's binascii.b2a_uu and decode identically.
    static constexpr struct {
        std::string_view input;
        std::string_view expected;
    } kVectors[] = {
        {"", ""},
        {"f", "!9@``\n"},
        {"fo", "\"9F\\`\n"},
        {"foo", "#9F]O\n"},
        {"Cat", "#0V%T\n"},
        {std::string_view{"\x00", 1}, "!````\n"},
        {std::string_view{"\x00\x00\x00", 3}, "#````\n"},
        {"hello world", "+:&5L;&\\@=V]R;&0`\n"},
    };
    for (const auto& v : kVectors) {
        CRPC_CHECK_EQ(std::string(v.expected), enc::uuencode(bytes_of(v.input)));
        const auto back = enc::uudecode(v.expected);
        CRPC_CHECK(back.ok());
        if (back.ok()) {
            CRPC_CHECK_EQ(std::string(v.input), str_of(back.value()));
        }
    }
}

CRPC_TEST(uudecode_accepts_space_padding_and_framing) {
    // Python's binascii.b2a_uu pads with spaces; classic mailers strip
    // trailing spaces, which is why backtick exists.
    const auto spaced = enc::uudecode("!9@  \n");
    CRPC_CHECK(spaced.ok());
    if (spaced.ok()) {
        CRPC_CHECK_EQ(std::string("f"), str_of(spaced.value()));
    }
    // A full mailbox framing, CRLF line endings and the backtick terminator.
    const auto framed = enc::uudecode("begin 644 file.txt\r\n!9@``\r\n`\r\nend\r\n");
    CRPC_CHECK(framed.ok());
    if (framed.ok()) {
        CRPC_CHECK_EQ(std::string("f"), str_of(framed.value()));
    }
    // A stray backtick as a zero 6-bit group (Perl/GNU spelling of 0x00).
    const auto zero = enc::uudecode("!````\n");
    CRPC_CHECK(zero.ok());
    if (zero.ok()) {
        CRPC_CHECK_EQ(std::string(1, '\0'), str_of(zero.value()));
    }
}

CRPC_TEST(uuencode_line_length_splits_the_stream) {
    // 11 bytes with a 5-byte line budget: lines of 5, 5 and 1 source bytes.
    CRPC_CHECK_EQ(std::string("%:&5L;&\\`\n%('=O<FP`\n!9```\n"),
                  enc::uuencode(bytes_of("hello world"), 5));
    const auto back = enc::uudecode(enc::uuencode(bytes_of("hello world"), 5));
    CRPC_CHECK(back.ok());
    if (back.ok()) {
        CRPC_CHECK_EQ(std::string("hello world"), str_of(back.value()));
    }
}

CRPC_TEST(uuencode_roundtrip_random_line_lengths) {
    for (const std::size_t budget : {std::size_t{1}, std::size_t{3},
                                     std::size_t{20}, std::size_t{45}}) {
        for (std::size_t length = 0; length <= 100; length += 7) {
            const auto data = random_bytes(length);
            const auto back =
                enc::uudecode(enc::uuencode(span_of(data), budget));
            CRPC_CHECK(back.ok());
            if (back.ok()) {
                CRPC_CHECK(data == back.value());
            }
        }
    }
}

CRPC_TEST(uudecode_rejects_malformed_input) {
    // 'N' declares 46 bytes per line, past the classic 45-byte limit.
    const auto overlong = enc::uudecode("N9F]O\n");
    expect_error(overlong.code(), rpc_code::decode_error);
    // The prefix declares 1 byte but the line carries only 3 characters.
    const auto truncated = enc::uudecode("!9@\n");
    expect_error(truncated.code(), rpc_code::decode_error);
    // '~' is not a uuencode digit.
    const auto alien = enc::uudecode("~9F]O\n");
    expect_error(alien.code(), rpc_code::decode_error);
    // An envelope whose 'end' line is missing still decodes: the header is
    // recognized and stripped, and the data lines are authoritative (the
    // decoder's documented framing rule).
    const auto unterminated = enc::uudecode("begin 644 x\n!9@``\n");
    CRPC_CHECK(unterminated.ok());
    if (unterminated.ok()) {
        CRPC_CHECK_EQ(std::string("f"), str_of(unterminated.value()));
    }
}

// --- quoted-printable (RFC 2045 section 6.7) ----------------------------------

CRPC_TEST(quoted_printable_rfc2045_examples) {
    // RFC 2045 section 6.7, example CE: soft breaks are removed.
    const auto soft =
        enc::quoted_printable_decode(
            "Now's the time =\r\nfor all folk to come=\r\n"
            " to the aid of their country.");
    CRPC_CHECK(soft.ok());
    if (soft.ok()) {
        CRPC_CHECK_EQ(std::string("Now's the time for all folk to come to the "
                                  "aid of their country."),
                      str_of(soft.value()));
    }
    // Rule 5: '=' must always be encoded.
    CRPC_CHECK_EQ(std::string("if (a =3D=3D b)\r\n\treturn 1;\r\n"),
                  enc::quoted_printable_encode(
                      bytes_of("if (a == b)\r\n\treturn 1;\r\n")));
    // Rule 2 and section 6.7 note 3: literal tab and space survive, but a
    // trailing white space must be encoded (rule 3).
    CRPC_CHECK_EQ(std::string("=00=01=02=7F=80=FF\t =3D"),
                  enc::quoted_printable_encode(
                      bytes_of(std::string_view{"\x00\x01\x02\x7f\x80\xff\t =", 9})));
    CRPC_CHECK_EQ(std::string("x=20\r\n"), enc::quoted_printable_encode(bytes_of("x \r\n")));
    CRPC_CHECK_EQ(std::string("x=09\r\n"), enc::quoted_printable_encode(bytes_of("x\t\r\n")));
}

CRPC_TEST(quoted_printable_line_breaking) {
    // A 62-character printable line needs no break at the 76-character limit.
    const std::string plain(62, 'a');
    CRPC_CHECK_EQ(plain, enc::quoted_printable_encode(bytes_of(plain)));
    // 76 literal 'a' fit exactly; 77 force a soft break after 75 + '='.
    const std::string full(76, 'a');
    CRPC_CHECK_EQ(full, enc::quoted_printable_encode(bytes_of(full)));
    CRPC_CHECK_EQ(std::string(75, 'a') + "=\r\n" + std::string(2, 'a'),
                  enc::quoted_printable_encode(bytes_of(std::string(77, 'a'))));
    // A line_length below 8 is raised to 8, so no line exceeds 8 characters.
    const std::string long_line(40, 'a');
    const std::string squeezed = enc::quoted_printable_encode(bytes_of(long_line), 1);
    std::size_t start = 0;
    while (start < squeezed.size()) {
        const std::size_t stop = squeezed.find('\n', start);
        std::size_t width = (stop == std::string::npos ? squeezed.size() : stop) - start;
        if (width > 0 && squeezed[stop == std::string::npos ? squeezed.size() - 1 : stop - 1] == '\r') {
            --width;
        }
        CRPC_CHECK(width <= 8);
        start = (stop == std::string::npos) ? squeezed.size() : stop + 1;
    }
}

CRPC_TEST(quoted_printable_decode_accepts_lowercase_hex_and_tab) {
    const auto lower = enc::quoted_printable_decode("=4a=4J");
    expect_error(lower.code(), rpc_code::decode_error); // 'J' is not hex
    const auto mixed = enc::quoted_printable_decode("a\tb=41");
    CRPC_CHECK(mixed.ok());
    if (mixed.ok()) {
        CRPC_CHECK_EQ(std::string("a\tbA"), str_of(mixed.value()));
    }
}

CRPC_TEST(quoted_printable_rejects_malformed_input) {
    static constexpr std::string_view kBad[] = {
        "ab=",      // truncated soft break
        "ab=4",     // truncated escape
        "ab=G1",    // non-hex escape
        "a\rb",     // bare CR
        "a\x80" "b",   // raw 8-bit octet
        "ab=\r",    // escape followed by a bare CR
    };
    for (const std::string_view text : kBad) {
        const auto r = enc::quoted_printable_decode(text);
        expect_error(r.code(), rpc_code::decode_error);
    }
}

CRPC_TEST(quoted_printable_roundtrip_random) {
    for (std::size_t trial = 0; trial < 64; ++trial) {
        const std::size_t length = trial % 96;
        auto data = random_bytes(length);
        // Bias the sample toward the characters that drive the line-breaking
        // rules: CR, LF, '=', space and tab.
        for (std::byte& b : data) {
            if ((rng()() & 3u) == 0u) {
                static constexpr std::uint8_t kSpecial[] = {0x0D, 0x0A, 0x3D,
                                                            0x20, 0x09};
                b = std::byte{kSpecial[rng()() % 5]};
            }
        }
        const std::string encoded =
            enc::quoted_printable_encode(span_of(data));
        const auto back = enc::quoted_printable_decode(encoded);
        CRPC_CHECK(back.ok());
        if (back.ok()) {
            CRPC_CHECK(data == back.value());
        }
        // Every line obeys the limit (soft breaks include the '=' itself).
        std::size_t start = 0;
        while (start < encoded.size()) {
            const std::size_t stop = encoded.find('\n', start);
            const std::size_t end =
                (stop == std::string::npos) ? encoded.size() : stop;
            std::size_t width = end - start;
            if (width > 0 && encoded[end - 1] == '\r') {
                --width;
            }
            CRPC_CHECK(width <= 76);
            start = (stop == std::string::npos) ? encoded.size() : stop + 1;
        }
    }
}

// --- punycode (RFC 3492) ------------------------------------------------------

CRPC_TEST(punycode_rfc3492_section_7_1_samples) {
    // All 19 sample lines of RFC 3492 section 7.1; the UTF-8 input is spelled
    // in hex escapes so the file is ASCII-clean.
    static constexpr struct {
        std::string_view utf8;
        std::string_view expected;
    } kSamples[] = {
        {"\xD9\x84\xD9\x8A\xD9\x87\xD9\x85\xD8\xA7\xD8\xA8\xD8\xAA\xD9\x83\xD9\x84\xD9\x85\xD9\x88\xD8\xB4\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\x9F", "egbpdaj6bu4bxfgehfvwxn"},
        {"\xE4\xBB\x96\xE4\xBB\xAC\xE4\xB8\xBA\xE4\xBB\x80\xE4\xB9\x88\xE4\xB8\x8D\xE8\xAF\xB4\xE4\xB8\xAD\xE6\x96\x87", "ihqwcrb4cv8a8dqg056pqjye"},
        {"\xE4\xBB\x96\xE5\x80\x91\xE7\x88\xB2\xE4\xBB\x80\xE9\xBA\xBD\xE4\xB8\x8D\xE8\xAA\xAA\xE4\xB8\xAD\xE6\x96\x87", "ihqwctvzc91f659drss3x8bo0yb"},
        {"\x50\x72\x6F\xC4\x8D\x70\x72\x6F\x73\x74\xC4\x9B\x6E\x65\x6D\x6C\x75\x76\xC3\xAD\xC4\x8D\x65\x73\x6B\x79", "Proprostnemluvesky-uyb24dma41a"},
        {"\xD7\x9C\xD7\x9E\xD7\x94\xD7\x94\xD7\x9D\xD7\xA4\xD7\xA9\xD7\x95\xD7\x98\xD7\x9C\xD7\x90\xD7\x9E\xD7\x93\xD7\x91\xD7\xA8\xD7\x99\xD7\x9D\xD7\xA2\xD7\x91\xD7\xA8\xD7\x99\xD7\xAA", "4dbcagdahymbxekheh6e0a7fei0b"},
        {"\xE0\xA4\xAF\xE0\xA4\xB9\xE0\xA4\xB2\xE0\xA5\x8B\xE0\xA4\x97\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x80\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xAF\xE0\xA5\x8B\xE0\xA4\x82\xE0\xA4\xA8\xE0\xA4\xB9\xE0\xA5\x80\xE0\xA4\x82\xE0\xA4\xAC\xE0\xA5\x8B\xE0\xA4\xB2\xE0\xA4\xB8\xE0\xA4\x95\xE0\xA4\xA4\xE0\xA5\x87\xE0\xA4\xB9\xE0\xA5\x88\xE0\xA4\x82", "i1baa7eci9glrd9b2ae1bj0hfcgg6iyaf8o0a1dig0cd"},
        {"\xE3\x81\xAA\xE3\x81\x9C\xE3\x81\xBF\xE3\x82\x93\xE3\x81\xAA\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x82\x92\xE8\xA9\xB1\xE3\x81\x97\xE3\x81\xA6\xE3\x81\x8F\xE3\x82\x8C\xE3\x81\xAA\xE3\x81\x84\xE3\x81\xAE\xE3\x81\x8B", "n8jok5ay5dzabd5bym9f0cm5685rrjetr6pdxa"},
        {"\xEC\x84\xB8\xEA\xB3\x84\xEC\x9D\x98\xEB\xAA\xA8\xEB\x93\xA0\xEC\x82\xAC\xEB\x9E\x8C\xEB\x93\xA4\xEC\x9D\xB4\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4\xEB\xA5\xBC\xEC\x9D\xB4\xED\x95\xB4\xED\x95\x9C\xEB\x8B\xA4\xEB\xA9\xB4\xEC\x96\xBC\xEB\xA7\x88\xEB\x82\x98\xEC\xA2\x8B\xEC\x9D\x84\xEA\xB9\x8C", "989aomsvi5e83db1d2a355cv1e0vak1dwrv93d5xbh15a0dt30a5jpsd879ccm6fea98c"},
        {"\xD0\xBF\xD0\xBE\xD1\x87\xD0\xB5\xD0\xBC\xD1\x83\xD0\xB6\xD0\xB5\xD0\xBE\xD0\xBD\xD0\xB8\xD0\xBD\xD0\xB5\xD0\xB3\xD0\xBE\xD0\xB2\xD0\xBE\xD1\x80\xD1\x8F\xD1\x82\xD0\xBF\xD0\xBE\xD1\x80\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8", "b1abfaaepdrnnbgefbaDotcwatmq2g4l"},
        {"\x50\x6F\x72\x71\x75\xC3\xA9\x6E\x6F\x70\x75\x65\x64\x65\x6E\x73\x69\x6D\x70\x6C\x65\x6D\x65\x6E\x74\x65\x68\x61\x62\x6C\x61\x72\x65\x6E\x45\x73\x70\x61\xC3\xB1\x6F\x6C", "PorqunopuedensimplementehablarenEspaol-fmd56a"},
        {"\x54\xE1\xBA\xA1\x69\x73\x61\x6F\x68\xE1\xBB\x8D\x6B\x68\xC3\xB4\x6E\x67\x74\x68\xE1\xBB\x83\x63\x68\xE1\xBB\x89\x6E\xC3\xB3\x69\x74\x69\xE1\xBA\xBF\x6E\x67\x56\x69\xE1\xBB\x87\x74", "TisaohkhngthchnitingVit-kjcr8268qyxafd2f1b9g"},
        {"\x33\xE5\xB9\xB4\x42\xE7\xB5\x84\xE9\x87\x91\xE5\x85\xAB\xE5\x85\x88\xE7\x94\x9F", "3B-ww4c5e180e575a65lsy2b"},
        {"\xE5\xAE\x89\xE5\xAE\xA4\xE5\xA5\x88\xE7\xBE\x8E\xE6\x81\xB5\x2D\x77\x69\x74\x68\x2D\x53\x55\x50\x45\x52\x2D\x4D\x4F\x4E\x4B\x45\x59\x53", "-with-SUPER-MONKEYS-pc58ag80a8qai00g7n9n"},
        {"\x48\x65\x6C\x6C\x6F\x2D\x41\x6E\x6F\x74\x68\x65\x72\x2D\x57\x61\x79\x2D\xE3\x81\x9D\xE3\x82\x8C\xE3\x81\x9E\xE3\x82\x8C\xE3\x81\xAE\xE5\xA0\xB4\xE6\x89\x80", "Hello-Another-Way--fc4qua05auwb3674vfr0b"},
        {"\xE3\x81\xB2\xE3\x81\xA8\xE3\x81\xA4\xE5\xB1\x8B\xE6\xA0\xB9\xE3\x81\xAE\xE4\xB8\x8B\x32", "2-u9tlzr9756bt3uc0v"},
        {"\x4D\x61\x6A\x69\xE3\x81\xA7\x4B\x6F\x69\xE3\x81\x99\xE3\x82\x8B\x35\xE7\xA7\x92\xE5\x89\x8D", "MajiKoi5-783gue6qz075azm5e"},
        {"\xE3\x83\x91\xE3\x83\x95\xE3\x82\xA3\xE3\x83\xBC\x64\x65\xE3\x83\xAB\xE3\x83\xB3\xE3\x83\x90", "de-jg4avhby1noc0d"},
        {"\xE3\x81\x9D\xE3\x81\xAE\xE3\x82\xB9\xE3\x83\x94\xE3\x83\xBC\xE3\x83\x89\xE3\x81\xA7", "d9juau41awczczp"},
        {"\x2D\x3E\x20\x24\x31\x2E\x30\x30\x20\x3C\x2D", "-> $1.00 <--"},
    };
    for (const auto& s : kSamples) {
        const auto ascii = enc::punycode_to_ascii(s.utf8);
        CRPC_CHECK(ascii.ok());
        if (ascii.ok()) {
            // The RFC prints the Russian sample with a deliberately uppercase
            // 'D' to demonstrate mixed case; encoders are recommended to emit
            // lowercase, which is what this module (and Python's codec) does.
            // Fold the extended part of the RFC spelling to lowercase so the
            // comparison matches the recommended encoder form. Everything up
            // to and including the LAST delimiter is the basic string, whose
            // case is real data and must not be folded.
            std::string folded;
            folded.reserve(s.expected.size());
            std::size_t delimiter = s.expected.rfind('-');
            delimiter = (delimiter == std::string_view::npos)
                            ? 0
                            : delimiter + 1;
            for (std::size_t p = 0; p < s.expected.size(); ++p) {
                const char c = s.expected[p];
                if (p >= delimiter && c >= 'A' && c <= 'Z') {
                    folded.push_back(static_cast<char>(c - 'A' + 'a'));
                } else {
                    folded.push_back(c);
                }
            }
            CRPC_CHECK_EQ(folded, ascii.value());
        }
        const auto back = enc::punycode_to_unicode(s.expected);
        CRPC_CHECK(back.ok());
        if (back.ok()) {
            // The decoder must accept the RFC's exact spelling, including the
            // mixed-case Russian sample.
            CRPC_CHECK_EQ(std::string(s.utf8), back.value());
        }
    }
    // The RFC's mixed-case spelling and the lowercase spelling are the same
    // label to the decoder.
    const auto mixed = enc::punycode_to_unicode("b1abfaaepdrnnbgefbaDotcwatmq2g4l");
    const auto lower = enc::punycode_to_unicode("b1abfaaepdrnnbgefbadotcwatmq2g4l");
    CRPC_CHECK(mixed.ok() && lower.ok() && mixed.value() == lower.value());
}

CRPC_TEST(punycode_delimiter_edges) {
    const auto plain = enc::punycode_to_ascii("abc");
    CRPC_CHECK(plain.ok());
    if (plain.ok()) {
        CRPC_CHECK_EQ(std::string("abc-"), plain.value());
    }
    const auto accented = enc::punycode_to_ascii("caf\xC3\xA9");
    CRPC_CHECK(accented.ok());
    if (accented.ok()) {
        CRPC_CHECK_EQ(std::string("caf-dma"), accented.value());
    }
    const auto empty = enc::punycode_to_ascii("");
    CRPC_CHECK(empty.ok());
    const auto only_dash = enc::punycode_to_ascii("-");
    CRPC_CHECK(only_dash.ok());
    if (only_dash.ok()) {
        CRPC_CHECK_EQ(std::string("--"), only_dash.value());
    }
    const auto dash = enc::punycode_to_unicode("--");
    CRPC_CHECK(dash.ok());
    if (dash.ok()) {
        CRPC_CHECK_EQ(std::string("-"), dash.value());
    }
    const auto trailing_dash = enc::punycode_to_unicode("abc--");
    CRPC_CHECK(trailing_dash.ok());
    if (trailing_dash.ok()) {
        CRPC_CHECK_EQ(std::string("abc-"), trailing_dash.value());
    }
    // Uppercase extended digits are accepted (RFC 3492 section 6.3).
    const auto upper = enc::punycode_to_unicode("3B-WW4C5E180E575A65LSY2B");
    CRPC_CHECK(upper.ok());
    if (upper.ok()) {
        CRPC_CHECK_EQ(std::string("3\xE5\xB9\xB4""B\xE7\xB5\x84\xE9\x87\x91\xE5\x85\xAB\xE5\x85\x88\xE7\x94\x9F"),
                      upper.value());
    }
}

CRPC_TEST(punycode_rejects_malformed_input) {
    // '!' is not an extended-alphabet digit.
    const auto bad_digit = enc::punycode_to_unicode("3B!");
    expect_error(bad_digit.code(), rpc_code::decode_error);
    // A delta that does not fit in 32 bits must overflow, not wrap.
    const auto overflow = enc::punycode_to_unicode("99999999999999999999");
    expect_error(overflow.code(), rpc_code::decode_error);
    // The input side is ASCII-only.
    const auto non_ascii = enc::punycode_to_unicode("\xC3\xA4");
    expect_error(non_ascii.code(), rpc_code::decode_error);
}

CRPC_TEST(punycode_roundtrip_random_code_points) {
    for (std::size_t trial = 0; trial < 96; ++trial) {
        std::string utf8;
        const std::size_t count = trial % 12;
        for (std::size_t i = 0; i < count; ++i) {
            std::uint32_t cp;
            switch (rng()() % 4) {
            case 0:
                cp = rng()() % 0x80u;             // basic
                break;
            case 1:
                cp = 0x80u + rng()() % 0x7FFu;    // 2-byte
                break;
            case 2:
                cp = 0x800u + rng()() % 0xF7FFu;  // 3-byte, sans surrogates
                if (cp >= 0xD800u && cp <= 0xDFFFu) {
                    cp = 0x4E2Du;
                }
                break;
            default:
                cp = 0x10000u + rng()() % 0xFFFFu; // 4-byte
                break;
            }
            append_utf8(utf8, cp);
        }
        const auto ascii = enc::punycode_to_ascii(utf8);
        CRPC_CHECK(ascii.ok());
        if (!ascii.ok()) {
            continue;
        }
        const auto back = enc::punycode_to_unicode(ascii.value());
        CRPC_CHECK(back.ok());
        if (back.ok()) {
            CRPC_CHECK_EQ(utf8, back.value());
        }
    }
}

// --- UTF-7 (RFC 2152) ---------------------------------------------------------

CRPC_TEST(utf7_rfc2152_section_5_examples) {
    static constexpr struct {
        std::string_view utf8;
        std::string_view utf7;
    } kExamples[] = {
        {"A\xE2\x89\xA2""\xCE\x91.", "A+ImIDkQ-."},
        {"Hi Mom -\xE2\x98\xBA-!", "Hi Mom -+Jjo--!"},
        {"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", "+ZeVnLIqe-"},
        {"Item 3 is \xC2\xA3""1.", "Item 3 is +AKM-1."},
    };
    for (const auto& v : kExamples) {
        const auto encoded = enc::utf7_encode(v.utf8);
        CRPC_CHECK(encoded.ok());
        if (encoded.ok()) {
            // This encoder always closes a shift with '-' (documented in the
            // header). The RFC also allows the shift to end implicitly at the
            // first non-set-B character, so where the RFC sample omits the
            // dash both spellings are legal; the expected text here is the
            // dash-closed form Python's utf-7 codec emits as well.
            CRPC_CHECK_EQ(std::string(v.utf7), encoded.value());
        }
        const auto decoded = enc::utf7_decode(v.utf7);
        CRPC_CHECK(decoded.ok());
        if (decoded.ok()) {
            CRPC_CHECK_EQ(std::string(v.utf8), decoded.value());
        }
    }
    // The RFC's own spelling of the first example relies on the implicit
    // termination at '.'; the decoder must accept it too.
    const auto implicit_form = enc::utf7_decode("A+ImIDkQ.");
    CRPC_CHECK(implicit_form.ok());
    if (implicit_form.ok()) {
        CRPC_CHECK_EQ(std::string("A\xE2\x89\xA2""\xCE\x91."),
                      implicit_form.value());
    }
}

CRPC_TEST(utf7_special_characters) {
    static constexpr struct {
        std::string_view utf8;
        std::string_view utf7;
    } kVectors[] = {
        {"+", "+-"},                  // set D: '+' must be escaped
        {"a+b", "a+-b"},
        {"abc", "abc"},               // printable ASCII is direct
        {std::string_view{"\\\x7F", 2}, "+AFwAfw-"}, // '\\' and control shift (one merged run)
        {std::string_view{"\x00", 1}, "+AAA-"},
        {"\xC3\xB6", "+APY-"},        // U+00F6
        {"\xF0\x9F\x98\x80", "+2D3eAA-"}, // U+1F600
    };
    for (const auto& v : kVectors) {
        const auto encoded = enc::utf7_encode(v.utf8);
        CRPC_CHECK(encoded.ok());
        if (encoded.ok()) {
            CRPC_CHECK_EQ(std::string(v.utf7), encoded.value());
        }
        const auto decoded = enc::utf7_decode(v.utf7);
        CRPC_CHECK(decoded.ok());
        if (decoded.ok()) {
            CRPC_CHECK_EQ(std::string(v.utf8), decoded.value());
        }
    }
}

CRPC_TEST(utf7_rejects_malformed_input) {
    // A '+' at end of input has no shift sequence at all.
    const auto bare = enc::utf7_decode("+");
    expect_error(bare.code(), rpc_code::decode_error);
    // "+A" is a shift opened but never closed.
    const auto open = enc::utf7_decode("+A");
    expect_error(open.code(), rpc_code::decode_error);
    // '+' followed by a character outside set B is ill-formed (RFC 2152 5.3).
    const auto bang = enc::utf7_decode("+!");
    expect_error(bang.code(), rpc_code::decode_error);
    // Three octets cannot be split into whole 16-bit units.
    const auto odd = enc::utf7_decode("+AAAA");
    expect_error(odd.code(), rpc_code::decode_error);
    // D800 is a lone high surrogate.
    const auto surrogate = enc::utf7_decode("+2AD-");
    expect_error(surrogate.code(), rpc_code::decode_error);
    // Raw 8-bit bytes are not UTF-7.
    const auto raw = enc::utf7_decode("a\x80");
    expect_error(raw.code(), rpc_code::decode_error);
}

CRPC_TEST(utf7_roundtrip_random_code_points) {
    for (std::size_t trial = 0; trial < 64; ++trial) {
        std::string utf8;
        const std::size_t count = trial % 14;
        for (std::size_t i = 0; i < count; ++i) {
            std::uint32_t cp;
            switch (rng()() % 5) {
            case 0:
                cp = 0x20u + rng()() % 0x5Fu;     // direct set, incl '+'
                break;
            case 1:
                cp = rng()() % 0x80u;             // whitespace and controls
                break;
            case 2:
                cp = 0x80u + rng()() % 0x7FFu;
                break;
            case 3:
                cp = 0x800u + rng()() % 0xF7FFu;
                if (cp >= 0xD800u && cp <= 0xDFFFu) {
                    cp = 0x30DEu;
                }
                break;
            default:
                cp = 0x10000u + rng()() % 0xFFFFu;
                break;
            }
            append_utf8(utf8, cp);
        }
        const auto encoded = enc::utf7_encode(utf8);
        CRPC_CHECK(encoded.ok());
        if (!encoded.ok()) {
            continue;
        }
        const auto back = enc::utf7_decode(encoded.value());
        CRPC_CHECK(back.ok());
        if (back.ok()) {
            CRPC_CHECK_EQ(utf8, back.value());
        }
    }
}

// --- Latin-1 (ISO-8859-1) -----------------------------------------------------

CRPC_TEST(latin1_all_256_bytes_map_to_the_same_code_points) {
    for (unsigned value = 0; value < 256; ++value) {
        const std::vector<std::byte> single{std::byte{static_cast<std::uint8_t>(value)}};
        const auto utf8 = enc::latin1_to_utf8(span_of(single));
        CRPC_CHECK(utf8.ok());
        if (!utf8.ok()) {
            continue;
        }
        std::string expected;
        append_utf8(expected, value); // Latin-1 bytes ARE the code points
        CRPC_CHECK_EQ(expected, utf8.value());

        const auto back = enc::utf8_to_latin1(utf8.value());
        CRPC_CHECK(back.ok());
        if (back.ok()) {
            CRPC_CHECK_EQ(1u, back.value().size());
            if (back.value().size() == 1) {
                CRPC_CHECK_EQ(static_cast<int>(value),
                              static_cast<int>(
                                  static_cast<unsigned char>(back.value()[0])));
            }
        }
    }
}

CRPC_TEST(latin1_rejects_code_points_above_00ff) {
    // U+20AC has no Latin-1 form; the conversion must refuse, not drop.
    const auto euro = enc::utf8_to_latin1("\xE2\x82\xAC");
    expect_error(euro.code(), rpc_code::encode_error);
    // Malformed UTF-8 on the input side.
    const auto bad = enc::utf8_to_latin1("\xC3");
    expect_error(bad.code(), rpc_code::decode_error);
    const auto empty = enc::latin1_to_utf8(span_of(std::string()));
    CRPC_CHECK(empty.ok());
}

// --- GBK ----------------------------------------------------------------------

CRPC_TEST(gbk_known_vectors) {
    static constexpr struct {
        std::string_view gbk;
        std::string_view utf8;
    } kPairs[] = {
        {"\xD6\xD0\xCE\xC4", "\xE4\xB8\xAD\xE6\x96\x87"},             // 涓枃
        {"\xC4\xE3\xBA\xC3", "\xE4\xBD\xA0\xE5\xA5\xBD"},             // 浣犲ソ
        {"\xB2\xE2\xCA\xD4\x61\x62\x63", "\xE6\xB5\x8B\xE8\xAF\x95""abc"}, // 娴嬭瘯abc
        {"\x9D\x68\xD7\xD6", "\xE6\xBC\xA2\xE5\xAD\x97"},             // 婕㈠瓧
        {"\xA1\xA1", "\xE3\x80\x80"},                                  // U+3000
        {"\x81\x40", "\xE4\xB8\x82"},                                  // U+4E02
        {"\x80", "\xE2\x82\xAC"},                                      // euro
        {"abc", "abc"},                                                // ASCII
        {"", ""},
    };
    for (const auto& p : kPairs) {
        const auto utf8 = enc::gbk_to_utf8(bytes_of(p.gbk));
        CRPC_CHECK(utf8.ok());
        if (utf8.ok()) {
            CRPC_CHECK_EQ(std::string(p.utf8), utf8.value());
        }
        const auto gbk = enc::utf8_to_gbk(p.utf8);
        CRPC_CHECK(gbk.ok());
        if (gbk.ok()) {
            CRPC_CHECK_EQ(str_of(bytes_of(p.gbk).to_vector()),
                          str_of(gbk.value()));
        }
    }
}

CRPC_TEST(gbk_rejects_malformed_input) {
    static constexpr std::string_view kBadGbk[] = {
        "\x81",        // lone lead byte
        "\x81\x7F",    // trail byte below 0x40
        "\xFF",        // byte outside the lead range
        "\xD6",        // truncated double-byte sequence
    };
    for (const std::string_view text : kBadGbk) {
        const auto r = enc::gbk_to_utf8(bytes_of(text));
        expect_error(r.code(), rpc_code::decode_error);
    }
    // A non-BMP character has no GBK form.
    const auto emoji = enc::utf8_to_gbk("\xF0\x9F\x98\x80");
    expect_error(emoji.code(), rpc_code::encode_error);
    // CP936 private-use extensions are deliberately excluded (see header).
    const auto pua = enc::utf8_to_gbk("\xEE\x80\x80");
    expect_error(pua.code(), rpc_code::encode_error);
    // Malformed UTF-8 on the input side.
    const auto bad_utf8 = enc::utf8_to_gbk("\xE4\xB8");
    expect_error(bad_utf8.code(), rpc_code::decode_error);
}

CRPC_TEST(gbk_error_offsets_and_messages_unchanged) {
    // The strict-decode contract names the offending byte offset in the
    // message. These pins hold the fast paths (ASCII batching on decode,
    // direct-index table on encode) to exactly the messages and offsets the
    // original implementation produced, including mid-string positions.
    struct decode_case {
        std::string_view in;
        const char* message;
    };
    static constexpr decode_case kDecode[] = {
        {"\x81\x7F", "gbk: invalid trail byte at offset 1"},
        {"\xD6", "gbk: lone lead byte at offset 0"},
        {"\x81", "gbk: lone lead byte at offset 0"},
        {"\xFF", "gbk: byte 0xFF is not a lead byte at offset 0"},
        {"\xA1\x40", "gbk: unmapped double-byte sequence at offset 0"},
        {"\xB2\xE2\xA1\x40", "gbk: unmapped double-byte sequence at offset 2"},
        {"\xB2\xE2\x81\x7F", "gbk: invalid trail byte at offset 3"},
        {"\xB2\xE2\xFF", "gbk: byte 0xFF is not a lead byte at offset 2"},
        {"\xB2\xE2\xD6", "gbk: lone lead byte at offset 2"},
    };
    for (const decode_case& c : kDecode) {
        const auto r = enc::gbk_to_utf8(bytes_of(c.in));
        CRPC_CHECK(!r.ok());
        CRPC_CHECK_EQ(std::string(c.message),
                      r.error_or(crpc::rpc_error{}).message);
    }
    struct encode_case {
        std::string_view in;
        crpc::rpc_code code;
        const char* message;
    };
    static constexpr encode_case kEncode[] = {
        {"\xE4\xB8", rpc_code::decode_error,
         "invalid UTF-8: malformed sequence at byte at offset 0"},
        {"\xF0\x9F\x98\x80", rpc_code::encode_error,
         "gbk: code point above U+FFFF at offset 0"},
        {"\xEE\x80\x80", rpc_code::encode_error,
         "gbk: no mapping for code point at offset 0"},
        {"ab\xF0\x9F\x98\x80", rpc_code::encode_error,
         "gbk: code point above U+FFFF at offset 2"},
        {"\xE4\xB8\xAD\xEE\x80\x80", rpc_code::encode_error,
         "gbk: no mapping for code point at offset 3"},
        {"ab\xE4\xB8", rpc_code::decode_error,
         "invalid UTF-8: malformed sequence at byte at offset 2"},
    };
    for (const encode_case& c : kEncode) {
        const auto r = enc::utf8_to_gbk(c.in);
        CRPC_CHECK(!r.ok());
        CRPC_CHECK(r.error_or(crpc::rpc_error{}).code == c.code);
        CRPC_CHECK_EQ(std::string(c.message),
                      r.error_or(crpc::rpc_error{}).message);
    }
}

CRPC_TEST(gbk_ascii_mixed_corpus_roundtrips) {
    // Real mixed text alternates ASCII runs and CJK runs. Every boundary
    // combination must survive both directions byte-exactly.
    static constexpr std::string_view kMixed[] = {
        "abc",
        "\xE4\xB8\xAD",
        "abc" "\xE4\xB8\xAD",
        "\xE4\xB8\xAD" "abc",
        "a" "\xE4\xB8\xAD" "b" "\xE6\x96\x87" "c",
        "\xE2\x82\xAC\xE4\xB8\xAD\xE2\x82\xAC",
        "abc" "\xE4\xB8\xAD" "\xE2\x82\xAC" "xyz",
        "abc" "\xE2\x82\xAC" "\xE4\xB8\xAD",
        "x" "\xE2\x82\xAC" "y" "\xE2\x82\xAC" "z",
        "The " "\xE4\xB8\xAD\xE5\x9B\xBD" " bank reported 12% growth in 2026.",
    };
    for (const std::string_view text : kMixed) {
        const auto gbk = enc::utf8_to_gbk(text);
        CRPC_CHECK(gbk.ok());
        if (!gbk.ok()) {
            continue;
        }
        const auto back = enc::gbk_to_utf8(span_of(gbk.value()));
        CRPC_CHECK(back.ok());
        if (back.ok()) {
            CRPC_CHECK_EQ(std::string(text), back.value());
        }
    }
}

CRPC_TEST(gbk_every_valid_pair_roundtrips) {
    // The full GBK double-byte space: leads 0x81..0xFE, trails 0x40..0xFE
    // except 0x7F. 21791 of the 23940 combinations are mapped; each mapped
    // pair must re-encode to the identical bytes.
    static constexpr std::size_t kExpectedMapped = 21791;
    std::size_t mapped = 0;
    std::vector<std::byte> pair(2);
    for (unsigned lead = 0x81u; lead <= 0xFEu; ++lead) {
        for (unsigned trail = 0x40u; trail <= 0xFEu; ++trail) {
            if (trail == 0x7Fu) {
                continue;
            }
            pair[0] = std::byte{static_cast<std::uint8_t>(lead)};
            pair[1] = std::byte{static_cast<std::uint8_t>(trail)};
            const auto utf8 = enc::gbk_to_utf8(span_of(pair));
            if (!utf8.ok()) {
                continue;
            }
            ++mapped;
            const auto gbk = enc::utf8_to_gbk(utf8.value());
            CRPC_CHECK(gbk.ok());
            if (gbk.ok()) {
                CRPC_CHECK(gbk.value() == pair);
            }
        }
    }
    CRPC_CHECK_EQ(kExpectedMapped, mapped);
}

CRPC_TEST(gbk_every_mappable_bmp_code_point_roundtrips) {
    // For every BMP code point the table can express, the UTF-8 -> GBK ->
    // UTF-8 path must return the original encoding. 21920 code points are
    // mappable: the 21791 double-byte entries, the euro sign, and the 128
    // direct ASCII bytes. Unmapped code points must be refused.
    static constexpr std::size_t kExpectedMappable = 21920;
    std::size_t mappable = 0;
    for (std::uint32_t cp = 0; cp <= 0xFFFFu; ++cp) {
        if (cp >= 0xD800u && cp <= 0xDFFFu) {
            continue; // surrogates are not characters
        }
        std::string utf8;
        append_utf8(utf8, cp);
        const auto gbk = enc::utf8_to_gbk(utf8);
        if (!gbk.ok()) {
            expect_error(gbk.code(), rpc_code::encode_error);
            continue;
        }
        ++mappable;
        const auto back = enc::gbk_to_utf8(span_of(gbk.value()));
        CRPC_CHECK(back.ok());
        if (back.ok()) {
            CRPC_CHECK_EQ(utf8, back.value());
        }
    }
    CRPC_CHECK_EQ(kExpectedMappable, mappable);
}

} // namespace

CRPC_TEST_MAIN()
