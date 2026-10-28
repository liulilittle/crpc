// CRPC -- UTF-8 / UTF-16 conversion, BOM handling and strict decoding.
//
// The library already contains UTF-16 codecs, but each one is coupled to its
// transport: the TDS codecs (`db/tds/protocol.hpp`) deliberately decode a lone
// surrogate to U+FFFD and never validate their UTF-8 input, and the ODBC
// helpers are Windows API wrappers. This module is the strict, platform
// independent one:
//
//   * every invalid sequence is an error (`rpc_code::decode_error`), never a
//     silent replacement;
//   * UTF-16 surrogate pairs are handled and lone surrogates rejected;
//   * the three standard BOMs are recognised and stripped.
//
// `std::u16string` holds UTF-16 code units, which is the encoding Windows
// applications speak natively; the byte-serialised forms (UTF-16LE/BE) are
// what files and wire protocols carry.
#ifndef CRPC_TEXT_UTF_HPP
#define CRPC_TEXT_UTF_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace text {

// The Unicode encodings a BOM can identify.
enum class encoding : std::uint8_t { utf8, utf16le, utf16be };

// BOM length: three bytes for UTF-8, two for UTF-16.
constexpr std::size_t bom_size(encoding value) noexcept {
    return value == encoding::utf8 ? 3U : 2U;
}

// Recognises a UTF-8 / UTF-16LE / UTF-16BE byte order mark at the start of
// `data`. Returns std::nullopt when `data` is empty or does not start with a
// BOM; such input is treated as BOM-less UTF-8 by `decode`.
[[nodiscard]] CRPC_API std::optional<encoding>
detect_bom(span<const std::byte> data) noexcept;

// --- Strict UTF-8 <-> UTF-16 -------------------------------------------------

// Converts UTF-8 text to UTF-16 code units. Overlong forms, encoded
// surrogates, code points above U+10FFFF, truncated sequences and invalid
// continuation bytes are all `decode_error`.
[[nodiscard]] CRPC_API result<std::u16string> utf8_to_utf16(std::string_view text);

// Converts UTF-16 code units to UTF-8. A high surrogate not followed by a low
// one, and any low surrogate without its high half, are `decode_error`.
[[nodiscard]] CRPC_API result<std::string> utf16_to_utf8(std::u16string_view text);

// --- Byte-serialised UTF-16 (no BOM handling) ---------------------------------

[[nodiscard]] CRPC_API result<std::vector<std::byte>>
utf8_to_utf16le(std::string_view text);
[[nodiscard]] CRPC_API result<std::vector<std::byte>>
utf8_to_utf16be(std::string_view text);

// Decodes raw UTF-16LE/BE bytes (an odd byte count is `decode_error`, as is
// any unpaired surrogate half). The caller handles BOM detection separately
// when needed; these functions never look at a BOM.
[[nodiscard]] CRPC_API result<std::string>
utf16le_to_utf8(span<const std::byte> data);
[[nodiscard]] CRPC_API result<std::string>
utf16be_to_utf8(span<const std::byte> data);

// --- BOM-aware decode ---------------------------------------------------------

// Detects and strips a BOM, then decodes to UTF-8: UTF-16LE/BE with a BOM, or
// strict UTF-8 when there is none. Invalid content is `decode_error`.
[[nodiscard]] CRPC_API result<std::string> decode(span<const std::byte> data);

// Decodes a string_view as strict UTF-8, returning it unchanged when valid
// (there is no BOM concept in a char buffer).
[[nodiscard]] CRPC_API result<std::string> decode(std::string_view text);

} // namespace text
} // namespace crpc

#endif // CRPC_TEXT_UTF_HPP