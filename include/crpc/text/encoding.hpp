// CRPC -- binary-to-text transfer encodings and legacy character sets.
//
// base64, hex, percent-encoding and UTF-8/UTF-16 conversion already live in
// other modules (`crpc/crypto/base64.hpp`, `crpc/crypto/hash.hpp`,
// `crpc/text/utf.hpp`); this header adds the remaining encodings the library
// has to speak: the RFC 4648 base32 family, Bitcoin's base58, RFC 1924 base85,
// Adobe ascii85, uuencode, quoted-printable, punycode, UTF-7, and the two
// legacy character sets that predate Unicode in their respective ecosystems
// (Latin-1 and GBK).
//
// The shared contract, and the reason these live together:
//
//   * text is a byte range, never a NUL-terminated string, and never a
//     character count: an encoded payload may legitimately contain NUL bytes
//     and multi-byte sequences;
//   * every decoder is strict. Input outside the alphabet, a truncated group,
//     a missing or misplaced pad character, or non-zero trailing pad bits is
//     an `rpc_code::decode_error` naming the offset, never a silently dropped
//     character and never a replacement code point. A payload that "decoded"
//     to almost the right bytes is worse than one that failed;
//   * every encoder is the exact inverse of its decoder, so encode -> decode
//     reproduces the input byte for byte. Where an encoding has choices that
//     are not observable after decoding (line length, soft break placement,
//     the case of base32 output), the encoder picks one deterministic form
//     rather than mirroring whatever the input happened to look like.
//
// All functions return `crpc::result<T>`; nothing here throws.
#ifndef CRPC_TEXT_ENCODING_HPP
#define CRPC_TEXT_ENCODING_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace text {

// --- base32 / base32hex (RFC 4648) -------------------------------------------
//
// 5 bits per character, 8 characters per 5-byte group, "=" padding. The
// extended-hex alphabet (section 7) keeps the sort order of the input, which
// is why DNSSEC uses it.
//
// The decoders accept both padded and unpadded input, and both letter cases
// (RFC 4648 section 3.4 notes base32 is meant to be case insensitive). The
// amount of padding, when present, must be exactly what the body length
// implies, and the unused bits of the last character must be zero: an
// encoder that leaves garbage there produces a second spelling of the same
// bytes, which is a signature-bypass primitive.

[[nodiscard]] CRPC_API std::string base32_encode(span<const std::byte> data);
[[nodiscard]] CRPC_API result<std::vector<std::byte>>
base32_decode(std::string_view text);

[[nodiscard]] CRPC_API std::string base32hex_encode(span<const std::byte> data);
[[nodiscard]] CRPC_API result<std::vector<std::byte>>
base32hex_decode(std::string_view text);

// --- base58 (Bitcoin alphabet) -----------------------------------------------
//
// Big-integer base conversion with the alphabet
// "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz" (no 0, O, I or
// l). Every leading zero byte is one leading '1', which is what makes a
// Bitcoin address checksum survive the conversion. Empty input encodes to the
// empty string; a string of only '1's decodes to that many zero bytes.

[[nodiscard]] CRPC_API std::string base58_encode(span<const std::byte> data);
[[nodiscard]] CRPC_API result<std::vector<std::byte>>
base58_decode(std::string_view text);

// --- base85 (RFC 1924) -------------------------------------------------------
//
// RFC 1924 treats the whole input as one big-endian integer and writes it in
// base 85 with the character set from section 4.2, most significant digit
// first. A 16-byte address therefore becomes exactly 20 characters, because
// 85^19 < 2^128 <= 85^20; the RFC's section 5 sample is reproduced exactly.
//
// The RFC does not define other lengths, so the digit count here is the
// smallest d with 85^d >= 256^n: the count needed for *every* n-byte value.
// It is strictly increasing, which is what lets the decoder recover n from the
// number of digits; a value that does not fit in n bytes, or a digit count no
// byte length can produce (1, 6, 11, ...), is a decode_error. Leading zero
// bytes survive because the byte count is encoded in the length.

[[nodiscard]] CRPC_API std::string base85_encode(span<const std::byte> data);
[[nodiscard]] CRPC_API result<std::vector<std::byte>>
base85_decode(std::string_view text);

// --- ascii85 (Adobe) ---------------------------------------------------------
//
// The Adobe variant of the 4 -> 5 transform. `z` abbreviates a group of four
// zero bytes (four bytes only; `z` is an error inside a short final group),
// and the encoder can wrap the text in the `<~` ... `~>` delimiters. The
// decoder accepts input with or without the delimiters and ignores ASCII
// whitespace, as Adobe's own filters do.

[[nodiscard]] CRPC_API std::string ascii85_encode(span<const std::byte> data,
                                                  bool adobe_delimiters = true);
[[nodiscard]] CRPC_API result<std::vector<std::byte>>
ascii85_decode(std::string_view text);

// --- uuencode ----------------------------------------------------------------
//
// The classic Unix 6-bit form: one length prefix character (' ' + n, so the
// byte 0x20 + n) per line, four characters per three input bytes, and '`'
// padding for the unused characters of the last group. `line_length` is the
// number of source bytes per line; values outside 1..45 are clamped to 45
// (the classic limit, where 1 + 60 + newline fits the 63-byte mail limit).
//
// The decoder accepts the `begin <mode> <name>` / `end` framing when it is
// present, CRLF or LF line endings, and both '`' and ' ' as the zero-length
// marker (some mailers strip trailing spaces, so classic encoders emit '`').
// '`' is also accepted as a zero 6-bit group, which is how Perl and GNU
// uuencode spell a zero byte.

[[nodiscard]] CRPC_API std::string uuencode(span<const std::byte> data,
                                           std::size_t line_length = 45);
[[nodiscard]] CRPC_API result<std::vector<std::byte>>
uudecode(std::string_view text);

// --- quoted-printable (RFC 2045 section 6.7) ---------------------------------
//
// Printable ASCII (33..60, 62..126) stays literal, '=' always becomes =3D, a
// space or tab is escaped when it would end a line, and any other octet
// becomes =XX with uppercase hex digits. Lines are at most `line_length`
// characters (default 76, the RFC limit) and are continued with a soft break
// ("=" at end of line). CRLF and LF already present in the input are kept as
// hard line breaks and reset the line counter; CR alone is escaped (=0D). The
// encoder emits CRLF after a soft break regardless of the input's local
// newline convention, because that is what the encoding defines.
//
// The decoder removes soft breaks, expands =XX, and rejects a truncated or
// non-hex escape. It accepts lowercase hex, which is common in the wild.
// Bytes are otherwise preserved exactly: in particular trailing whitespace on
// a line is not stripped, so decode(encode(x)) == x for every x. A
// `line_length` below 8 is raised to 8 (a line must hold a "=XX" escape and
// the soft break that may follow it).

[[nodiscard]] CRPC_API std::string
quoted_printable_encode(span<const std::byte> data,
                        std::size_t line_length = 76);
[[nodiscard]] CRPC_API result<std::vector<std::byte>>
quoted_printable_decode(std::string_view text);

// --- punycode (RFC 3492) -----------------------------------------------------
//
// Converts between UTF-8 text and the ASCII basic string used by IDN labels.
// The API works on code points, not UTF-16 units: `to_ascii` takes UTF-8 and
// returns the raw punycode label (without the "xn--" prefix, which is an IDNA
// detail, not a punycode one); `to_unicode` performs the reverse. Encoded
// digits use lowercase, as the RFC recommends; the decoder accepts either
// case. Output code points above U+10FFFF or in the surrogate range are
// rejected, because the result has to be valid UTF-8.

[[nodiscard]] CRPC_API result<std::string>
punycode_to_ascii(std::string_view utf8_text);
[[nodiscard]] CRPC_API result<std::string>
punycode_to_unicode(std::string_view ascii_text);

// --- UTF-7 (RFC 2152) --------------------------------------------------------
//
// `utf7_encode` takes UTF-8 and returns UTF-7; `utf7_decode` takes UTF-7 and
// returns UTF-8. Directly encoded characters are the RFC 2152 set D, the
// optional set O and the whitespace of rule 3: printable ASCII except '+',
// '\\' and '~', plus tab, CR and LF. '+' becomes "+-". Runs of other
// characters are shifted: UTF-16BE octets (surrogate pairs for non-BMP input)
// in modified base64 without '=', opened by '+' and closed by '-'.
//
// The decoder rejects a shift that starts with neither '-' nor a base64
// character, a run whose octet count is odd, non-zero discarded bits, and
// unpaired surrogates.

[[nodiscard]] CRPC_API result<std::string> utf7_encode(std::string_view utf8_text);
[[nodiscard]] CRPC_API result<std::string> utf7_decode(std::string_view utf7_text);

// --- Latin-1 (ISO-8859-1) ----------------------------------------------------
//
// The identity mapping between the 256 single bytes and U+0000..U+00FF.
// `latin1_to_utf8` cannot fail for any byte string; the result wrapper keeps
// the module's uniform signature. `utf8_to_latin1` rejects (encode_error) any
// code point above U+00FF instead of dropping it.

[[nodiscard]] CRPC_API result<std::string>
latin1_to_utf8(span<const std::byte> data);
[[nodiscard]] CRPC_API result<std::string>
utf8_to_latin1(std::string_view utf8_text);

// --- GBK ---------------------------------------------------------------------
//
// The GBK double-byte set: ASCII is single-byte, 0x80 is the euro sign (as in
// CP936), and 0x81..0xFE followed by 0x40..0xFE (except 0x7F) is one Han or
// symbol code point. The mapping table is generated (see
// `.opencode/tmp/encoding/gen_gbk.py` and `src/text/gbk_table.inc`) from the
// GBK mapping shared by glibc's iconv and Python's `gbk` codec; the two agree
// on all 21791 double-byte mappings. Microsoft's CP936 private-use
// extensions (U+E000..U+E864 and the 81 code points it maps to private-use
// instead of their assigned Unicode values) are deliberately not included:
// they are not part of GBK proper, and including them would make the same
// bytes decode differently depending on the platform.
//
// A lone lead byte, a byte outside 0x00..0x80, 0x81..0xFE, a trail byte
// outside the valid range, or a character with no GBK form is an error; the
// conversion never substitutes '?'.

[[nodiscard]] CRPC_API result<std::string>
gbk_to_utf8(span<const std::byte> data);
[[nodiscard]] CRPC_API result<std::vector<std::byte>>
utf8_to_gbk(std::string_view utf8_text);

} // namespace text
} // namespace crpc

#endif // CRPC_TEXT_ENCODING_HPP
