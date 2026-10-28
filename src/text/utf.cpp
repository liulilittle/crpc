// CRPC -- UTF-8/UTF-16 conversion (strict). See include/crpc/text/utf.hpp.
//
// Validation code shape: the SSE component validates UTF-8 for the same
// reasons (no overlong forms, no surrogates, nothing above U+10FFFF) and its
// checks are mirrored here, but it is text-event specific (it also rejects
// unescaped control bytes) and lives in an anonymous namespace, so the
// strict, reusable decoder lives here.
#include <crpc/text/utf.hpp>

namespace crpc {
namespace text {

namespace {

// The byte offsets in the messages make a rejected sequence easy to locate.
std::string message(const char* what, std::size_t offset) {
    std::string out = "invalid UTF-";
    out += what;
    out += " at offset ";
    out += std::to_string(offset);
    return out;
}

rpc_error decode_error(const char* what, std::size_t offset) {
    return rpc_error{rpc_code::decode_error, message(what, offset)};
}

// Appends `code_point` to `out` as UTF-16, splitting it into a surrogate pair
// when it does not fit in one code unit.
void append_utf16(std::u16string& out, std::uint32_t code_point) {
    if (code_point < 0x10000u) {
        out.push_back(static_cast<char16_t>(code_point));
        return;
    }
    const std::uint32_t adjusted = code_point - 0x10000u;
    out.push_back(static_cast<char16_t>(0xD800u + (adjusted >> 10)));
    out.push_back(static_cast<char16_t>(0xDC00u + (adjusted & 0x3FFu)));
}

void append_utf8(std::string& out, std::uint32_t code_point) {
    if (code_point < 0x80u) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    } else if (code_point < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80u | ((code_point >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    }
}

// Decodes one code point at `text[i]`; advances `i`, appends to `out` and
// returns true, or leaves both untouched and returns false on malformed UTF-8.
// Every disallowed form is checked: bad lead byte, truncated sequence, bad
// continuation, overlong encoding, a surrogate and a code point above
// U+10FFFF.
bool decode_utf8_unit(std::string_view text, std::size_t& i,
                      std::u16string& out) {
    const auto lead = static_cast<std::uint8_t>(text[i]);
    std::uint32_t code_point = 0;
    std::size_t extra = 0;
    if (lead < 0x80u) {
        out.push_back(static_cast<char16_t>(lead));
        ++i;
        return true;
    }
    if ((lead & 0xE0u) == 0xC0u) {
        code_point = lead & 0x1Fu;
        extra = 1;
    } else if ((lead & 0xF0u) == 0xE0u) {
        code_point = lead & 0x0Fu;
        extra = 2;
    } else if ((lead & 0xF8u) == 0xF0u) {
        code_point = lead & 0x07u;
        extra = 3;
    } else {
        return false; // C0/C1 or F8-FF lead byte
    }
    if (i + extra >= text.size()) {
        return false; // truncated
    }
    for (std::size_t k = 1; k <= extra; ++k) {
        const auto cont = static_cast<std::uint8_t>(text[i + k]);
        if ((cont & 0xC0u) != 0x80u) {
            return false; // invalid continuation
        }
        code_point = (code_point << 6) | (cont & 0x3Fu);
    }
    if ((extra == 1 && code_point < 0x80u) ||
        (extra == 2 && code_point < 0x800u) ||
        (extra == 3 && code_point < 0x10000u)) {
        return false; // overlong
    }
    if (code_point >= 0xD800u && code_point <= 0xDFFFu) {
        return false; // UTF-8 must not encode a surrogate
    }
    if (code_point > 0x10FFFFu) {
        return false;
    }
    append_utf16(out, code_point);
    i += extra + 1;
    return true;
}

// Encodes a UTF-16 stream into UTF-8, reporting the first unpaired surrogate.
bool utf16_to_utf8_impl(std::u16string_view text, std::string& out,
                        std::size_t& bad_offset) {
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const std::uint32_t unit = text[i];
        if (unit >= 0xD800u && unit <= 0xDBFFu) {
            if (i + 1 >= text.size()) {
                bad_offset = i;
                return false; // high surrogate at end of input
            }
            const std::uint32_t low = text[i + 1];
            if (low < 0xDC00u || low > 0xDFFFu) {
                bad_offset = i;
                return false; // high surrogate not followed by a low one
            }
            const std::uint32_t code_point =
                0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u);
            append_utf8(out, code_point);
            ++i;
        } else if (unit >= 0xDC00u && unit <= 0xDFFFu) {
            bad_offset = i;
            return false; // low surrogate without a high half
        } else {
            append_utf8(out, unit);
        }
    }
    return true;
}

std::uint16_t read_le(const std::byte* p) {
    const auto lo = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[0]));
    const auto hi = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[1]));
    return static_cast<std::uint16_t>(lo | (hi << 8));
}

std::uint16_t read_be(const std::byte* p) {
    const auto lo = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[0]));
    const auto hi = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[1]));
    return static_cast<std::uint16_t>((lo << 8) | hi);
}

// Decodes byte-serialised UTF-16 in the given byte order. An odd byte count is
// malformed; the caller checks that before calling us (a pair of bytes is
// consumed per loop iteration).
result<std::string> utf16_bytes_to_utf8(span<const std::byte> data,
                                        bool big_endian) {
    std::u16string units;
    units.reserve(data.size() / 2);
    for (std::size_t i = 0; i + 1 < data.size(); i += 2) {
        units.push_back(static_cast<char16_t>(
            big_endian ? read_be(data.data() + i) : read_le(data.data() + i)));
    }
    std::string out;
    std::size_t bad_offset = 0;
    if (!utf16_to_utf8_impl(units, out, bad_offset)) {
        return decode_error("16: unpaired surrogate at code unit", bad_offset);
    }
    return out;
}

} // namespace

std::optional<encoding> detect_bom(span<const std::byte> data) noexcept {
    if (data.size() >= 3 && data[0] == std::byte{0xEF} &&
        data[1] == std::byte{0xBB} && data[2] == std::byte{0xBF}) {
        return encoding::utf8;
    }
    if (data.size() >= 2 && data[0] == std::byte{0xFF} &&
        data[1] == std::byte{0xFE}) {
        return encoding::utf16le;
    }
    if (data.size() >= 2 && data[0] == std::byte{0xFE} &&
        data[1] == std::byte{0xFF}) {
        return encoding::utf16be;
    }
    return std::nullopt;
}

result<std::u16string> utf8_to_utf16(std::string_view text) {
    std::u16string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (!decode_utf8_unit(text, i, out)) {
            return decode_error("8: malformed sequence at byte", i);
        }
    }
    return out;
}

result<std::string> utf16_to_utf8(std::u16string_view text) {
    std::string out;
    std::size_t bad_offset = 0;
    if (!utf16_to_utf8_impl(text, out, bad_offset)) {
        return decode_error("16: unpaired surrogate at code unit", bad_offset);
    }
    return out;
}

result<std::vector<std::byte>> utf8_to_utf16le(std::string_view text) {
    const auto units = utf8_to_utf16(text);
    if (!units.ok()) {
        return units.error();
    }
    std::vector<std::byte> out;
    out.reserve(units.value().size() * 2);
    for (const char16_t unit : units.value()) {
        out.push_back(std::byte{static_cast<std::uint8_t>(unit & 0xFFu)});
        out.push_back(std::byte{static_cast<std::uint8_t>((unit >> 8) & 0xFFu)});
    }
    return out;
}

result<std::vector<std::byte>> utf8_to_utf16be(std::string_view text) {
    const auto units = utf8_to_utf16(text);
    if (!units.ok()) {
        return units.error();
    }
    std::vector<std::byte> out;
    out.reserve(units.value().size() * 2);
    for (const char16_t unit : units.value()) {
        out.push_back(std::byte{static_cast<std::uint8_t>((unit >> 8) & 0xFFu)});
        out.push_back(std::byte{static_cast<std::uint8_t>(unit & 0xFFu)});
    }
    return out;
}

result<std::string> utf16le_to_utf8(span<const std::byte> data) {
    if (data.size() % 2 != 0) {
        return decode_error("16LE: odd byte count", data.size());
    }
    return utf16_bytes_to_utf8(data, /*big_endian=*/false);
}

result<std::string> utf16be_to_utf8(span<const std::byte> data) {
    if (data.size() % 2 != 0) {
        return decode_error("16BE: odd byte count", data.size());
    }
    return utf16_bytes_to_utf8(data, /*big_endian=*/true);
}

result<std::string> decode(span<const std::byte> data) {
    const auto bom = detect_bom(data);
    if (!bom.has_value()) {
        return decode(std::string_view(
            reinterpret_cast<const char*>(data.data()), data.size()));
    }
    const span<const std::byte> body = data.subspan(bom_size(*bom));
    switch (*bom) {
    case encoding::utf8:
        return decode(std::string_view(
            reinterpret_cast<const char*>(body.data()), body.size()));
    case encoding::utf16le:
        return utf16le_to_utf8(body);
    case encoding::utf16be:
        return utf16be_to_utf8(body);
    }
    return rpc_error{rpc_code::internal, "unreachable encoding"};
}

result<std::string> decode(std::string_view text) {
    const auto units = utf8_to_utf16(text);
    if (!units.ok()) {
        return units.error();
    }
    // The input is validated; UTF-8 round-trips byte-for-byte, so return it
    // unchanged rather than re-encoding.
    return std::string(text);
}

} // namespace text
} // namespace crpc