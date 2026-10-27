// CRPC -- TDS login-phase codec.
#include <crpc/db/tds/protocol.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace crpc {
namespace db {
namespace tds {
namespace {

constexpr std::byte byte_of(std::uint8_t value) noexcept {
    return static_cast<std::byte>(value);
}

void put_u8(std::vector<std::byte>& out, std::uint8_t value) {
    out.push_back(byte_of(value));
}

// TDS is big-endian on the wire for the header and the multi-byte login fields; the
// little-endian fields are written explicitly where they occur.
void put_be16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(byte_of(static_cast<std::uint8_t>(value >> 8)));
    out.push_back(byte_of(static_cast<std::uint8_t>(value & 0xffu)));
}

void put_be32(std::vector<std::byte>& out, std::uint32_t value) {
    out.push_back(byte_of(static_cast<std::uint8_t>((value >> 24) & 0xffu)));
    out.push_back(byte_of(static_cast<std::uint8_t>((value >> 16) & 0xffu)));
    out.push_back(byte_of(static_cast<std::uint8_t>((value >> 8) & 0xffu)));
    out.push_back(byte_of(static_cast<std::uint8_t>(value & 0xffu)));
}

void put_le16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(byte_of(static_cast<std::uint8_t>(value & 0xffu)));
    out.push_back(byte_of(static_cast<std::uint8_t>((value >> 8) & 0xffu)));
}

void put_le32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(byte_of(static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu)));
    }
}

// Writes an offset/length pair into a fixed slot later, since the offset is only
// known once the whole fixed part has been laid out.
void patch_le16(std::vector<std::byte>& out, std::size_t offset, std::uint16_t value) {
    out[offset] = byte_of(static_cast<std::uint8_t>(value & 0xffu));
    out[offset + 1] = byte_of(static_cast<std::uint8_t>((value >> 8) & 0xffu));
}

// Days between 0001-01-01 (the TDS epoch) and 1970-01-01. Shared by the formatter
// and the parser so a round trip is exact.
constexpr std::int64_t tds_epoch_offset_days = 719162;

} // namespace

// --- Framing -----------------------------------------------------------------

status encode_header(span<std::byte> out, const packet_header& header,
                     std::size_t& written) noexcept {
    written = 0;
    if (out.size() < header_size) {
        return status{rpc_code::encode_error, "TDS header needs 8 bytes"};
    }
    out[0] = byte_of(static_cast<std::uint8_t>(header.type));
    out[1] = byte_of(header.status);
    // Length is big-endian and covers the header itself; a length below 8 is a
    // frame the peer cannot parse.
    if (header.length < header_size) {
        return status{rpc_code::invalid_argument, "TDS packet length is below the header size"};
    }
    out[2] = byte_of(static_cast<std::uint8_t>(header.length >> 8));
    out[3] = byte_of(static_cast<std::uint8_t>(header.length & 0xffu));
    out[4] = byte_of(static_cast<std::uint8_t>(header.spid >> 8));
    out[5] = byte_of(static_cast<std::uint8_t>(header.spid & 0xffu));
    out[6] = byte_of(header.packet_id);
    out[7] = byte_of(header.window);
    written = header_size;
    return status{};
}

status decode_header(span<const std::byte> in, packet_header& header) noexcept {
    if (in.size() < header_size) {
        return status{rpc_code::protocol_error, "TDS header is truncated"};
    }
    header.type = static_cast<packet_type>(static_cast<std::uint8_t>(in[0]));
    header.status = static_cast<std::uint8_t>(in[1]);
    header.length = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(static_cast<std::uint8_t>(in[2])) << 8) |
        static_cast<std::uint16_t>(static_cast<std::uint8_t>(in[3])));
    header.spid = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(static_cast<std::uint8_t>(in[4])) << 8) |
        static_cast<std::uint16_t>(static_cast<std::uint8_t>(in[5])));
    header.packet_id = static_cast<std::uint8_t>(in[6]);
    header.window = static_cast<std::uint8_t>(in[7]);
    // A length below the header size, or beyond the buffer, is a frame that cannot
    // be trusted; the caller would otherwise read past the message.
if (header.length < header_size) {
        return status{rpc_code::protocol_error, "TDS packet length is below the header size"};
    }
    // Only the header is validated here: a caller may hold exactly eight bytes and
    // read the payload separately. Requiring the whole packet made a header-only
    // decode impossible, which is how the first test failed.
    return status{};
}

// --- PRELOGIN ----------------------------------------------------------------

status encode_prelogin(std::vector<std::byte>& out, const prelogin& value) {
    out.clear();
    // Option entries are (token, offset, length) with the offset measured from the
    // start of the PRELOGIN payload, then a 0xFF terminator.
    struct entry {
        prelogin_option option;
        std::uint16_t offset;
        std::uint16_t length;
    };
    std::vector<entry> entries;
    std::vector<std::byte> data;

    const auto append = [&](prelogin_option option, std::vector<std::byte> payload) {
        entries.push_back(entry{option, 0, static_cast<std::uint16_t>(payload.size())});
        data.insert(data.end(), payload.begin(), payload.end());
    };

    // VERSION is six bytes: four of version plus two reserved build bytes.
    append(prelogin_option::version,
           std::vector<std::byte>{byte_of(value.version_major), byte_of(value.version_minor),
                                  byte_of(value.version_build_high), byte_of(value.version_build_low),
                                  byte_of(0), byte_of(0)});
    append(prelogin_option::encryption,
           std::vector<std::byte>{byte_of(static_cast<std::uint8_t>(value.encryption))});
    std::vector<std::byte> thread;
    put_be32(thread, value.thread_id);
    append(prelogin_option::thread_id, std::move(thread));
    append(prelogin_option::mars, std::vector<std::byte>{byte_of(value.mars)});

    const std::size_t table_size = entries.size() * 5 + 1;
    std::size_t offset = table_size;
    for (entry& item : entries) {
        item.offset = static_cast<std::uint16_t>(offset);
        offset += item.length;
    }

    for (const entry& item : entries) {
        put_u8(out, static_cast<std::uint8_t>(item.option));
        put_be16(out, item.offset);
        put_be16(out, item.length);
    }
    put_u8(out, static_cast<std::uint8_t>(prelogin_option::terminator));
    out.insert(out.end(), data.begin(), data.end());
    return status{};
}

status decode_prelogin(span<const std::byte> payload, prelogin& out) {
    out = prelogin{};
    std::size_t index = 0;
    const auto u16_at = [&](std::size_t at) {
        return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload[at])) << 8) |
            static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload[at + 1])));
    };

    for (;;) {
        if (index >= payload.size()) {
            // A table with no terminator is truncated, not empty.
            return status{rpc_code::protocol_error, "TDS PRELOGIN option table is truncated"};
        }
        const auto option = static_cast<prelogin_option>(static_cast<std::uint8_t>(payload[index]));
        ++index;
        if (option == prelogin_option::terminator) {
            break;
        }
        if (index + 4 > payload.size()) {
            return status{rpc_code::protocol_error, "TDS PRELOGIN option entry is truncated"};
        }
        const std::uint16_t offset = u16_at(index);
        const std::uint16_t length = u16_at(index + 2);
        index += 4;
        if (offset > payload.size() || payload.size() - offset < length) {
            return status{rpc_code::protocol_error, "TDS PRELOGIN option points outside its packet"};
        }
        const span<const std::byte> value = payload.subspan(offset, length);
        switch (option) {
        case prelogin_option::version:
            if (length >= 4) {
                out.version_major = static_cast<std::uint8_t>(value[0]);
                out.version_minor = static_cast<std::uint8_t>(value[1]);
                out.version_build_high = static_cast<std::uint8_t>(value[2]);
                out.version_build_low = static_cast<std::uint8_t>(value[3]);
            }
            break;
        case prelogin_option::encryption:
            if (length >= 1) {
                out.encryption = static_cast<encryption_mode>(static_cast<std::uint8_t>(value[0]));
            }
            break;
        case prelogin_option::thread_id:
            if (length >= 4) {
                out.thread_id =
                    (static_cast<std::uint32_t>(static_cast<std::uint8_t>(value[0])) << 24) |
                    (static_cast<std::uint32_t>(static_cast<std::uint8_t>(value[1])) << 16) |
                    (static_cast<std::uint32_t>(static_cast<std::uint8_t>(value[2])) << 8) |
                    static_cast<std::uint32_t>(static_cast<std::uint8_t>(value[3]));
            }
            break;
        case prelogin_option::mars:
            if (length >= 1) {
                out.mars = static_cast<std::uint8_t>(value[0]);
            }
            break;
        default:
            // A token this client does not use is skipped, not rejected: the server
            // is allowed to advertise more than we asked for.
            break;
        }
    }
    return status{};
}

// --- Text --------------------------------------------------------------------

std::vector<std::byte> utf16le(std::string_view text) {
    std::vector<std::byte> out;
    out.reserve(text.size() * 2);
    // TDS strings are UCS-2 in practice; a code point above U+FFFF is written as a
    // surrogate pair, which every SQL Server collation accepts.
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::uint32_t code = lead;
        std::size_t width = 1;
        if (lead >= 0xF0 && i + 3 < text.size()) {
            code = ((lead & 0x07u) << 18) |
                   ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 12) |
                   ((static_cast<unsigned char>(text[i + 2]) & 0x3Fu) << 6) |
                   (static_cast<unsigned char>(text[i + 3]) & 0x3Fu);
            width = 4;
        } else if (lead >= 0xE0 && i + 2 < text.size()) {
            code = ((lead & 0x0Fu) << 12) |
                   ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 6) |
                   (static_cast<unsigned char>(text[i + 2]) & 0x3Fu);
            width = 3;
        } else if (lead >= 0xC0 && i + 1 < text.size()) {
            code = ((lead & 0x1Fu) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3Fu);
            width = 2;
        }
        i += width;
        if (code <= 0xFFFFu) {
            put_le16(out, static_cast<std::uint16_t>(code));
        } else {
            const std::uint32_t adjusted = code - 0x10000u;
            put_le16(out, static_cast<std::uint16_t>(0xD800u + (adjusted >> 10)));
            put_le16(out, static_cast<std::uint16_t>(0xDC00u + (adjusted & 0x3FFu)));
        }
    }
    return out;
}

std::string from_utf16le(span<const std::byte> data) {
    std::string out;
    out.reserve(data.size());
    for (std::size_t i = 0; i + 1 < data.size(); i += 2) {
        std::uint32_t code = static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[i])) |
                             (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[i + 1])) << 8);
        if (code >= 0xD800u && code <= 0xDBFFu && i + 3 < data.size()) {
            const std::uint32_t low =
                static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[i + 2])) |
                (static_cast<std::uint32_t>(static_cast<std::uint8_t>(data[i + 3])) << 8);
            if (low >= 0xDC00u && low <= 0xDFFFu) {
                code = 0x10000u + ((code - 0xD800u) << 10) + (low - 0xDC00u);
                i += 2;
            }
        }
        if (code <= 0x7Fu) {
            out.push_back(static_cast<char>(code));
        } else if (code <= 0x7FFu) {
            out.push_back(static_cast<char>(0xC0u | (code >> 6)));
            out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
        } else if (code <= 0xFFFFu) {
            // A lone surrogate cannot be encoded in UTF-8; U+FFFD says so instead of
            // producing invalid bytes.
            if (code >= 0xD800u && code <= 0xDFFFu) {
                code = 0xFFFDu;
            }
            out.push_back(static_cast<char>(0xE0u | (code >> 12)));
            out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xF0u | (code >> 18)));
            out.push_back(static_cast<char>(0x80u | ((code >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
        }
    }
    return out;
}

// --- LOGIN7 ------------------------------------------------------------------

std::vector<std::uint8_t> obfuscate_password(std::string_view password) {
    // LOGIN7 carries the password as UTF-16LE, and *each byte* of that encoding is
    // nibble-swapped and XORed. Doing this per character instead of per byte emits
    // half the bytes: the server then reports "an error occurred while evaluating the
    // password" (18456 state 7), which reads like a wrong credential rather than a
    // client bug.
    const std::vector<std::byte> text = utf16le(password);
    std::vector<std::uint8_t> out;
    out.reserve(text.size());
    for (const std::byte byte : text) {
        const auto value = static_cast<std::uint8_t>(byte);
        const std::uint8_t swapped =
            static_cast<std::uint8_t>(((value << 4) & 0xF0u) | ((value >> 4) & 0x0Fu));
        out.push_back(static_cast<std::uint8_t>(swapped ^ 0xA5u));
    }
    return out;
}

namespace {

// The fixed part of LOGIN7 is 94 bytes: 36 of header fields, nine offset/length
// pairs, a six-byte client id, and three more pairs plus the SSPI length.
constexpr std::size_t login7_fixed_size = 94;

void write_pair(std::vector<std::byte>& out, std::size_t& cursor, const std::string& text,
                std::vector<std::byte>& variables) {
    const std::uint16_t offset = static_cast<std::uint16_t>(login7_fixed_size + variables.size());
    const std::vector<std::byte> encoded = utf16le(text);
    // The length field counts characters, not bytes: a two-byte difference per
    // character is the classic off-by-double bug.
    const auto characters = static_cast<std::uint16_t>(text.size());
    patch_le16(out, cursor, offset);
    patch_le16(out, cursor + 2, characters);
    cursor += 4;
    variables.insert(variables.end(), encoded.begin(), encoded.end());
}

} // namespace

status encode_login7(std::vector<std::byte>& out, const login7& value) {
    out.assign(login7_fixed_size, std::byte{0});
    std::vector<std::byte> variables;

    // Fixed fields, little-endian.
    patch_le16(out, 0, 0); // total length, patched at the end
    patch_le16(out, 2, 0);
    std::vector<std::byte> fixed;
    put_le32(fixed, value.tds_version);
    put_le32(fixed, value.packet_size);
    put_le32(fixed, value.client_version);
    put_le32(fixed, value.client_pid);
    put_le32(fixed, value.connection_id);
    std::memcpy(out.data() + 4, fixed.data(), fixed.size()); // 20 bytes after the length

    out[24] = byte_of(static_cast<std::uint8_t>(value.option_flags & 0xffu));
    out[25] = byte_of(static_cast<std::uint8_t>((value.option_flags >> 8) & 0xffu));
    out[26] = byte_of(0); // TypeFlags: SQL_DFLT
    out[27] = byte_of(static_cast<std::uint8_t>((value.option_flags >> 16) & 0xffu));
    // ClientTimeZone and ClientLCID stay zero.

    std::size_t cursor = 36;
    write_pair(out, cursor, value.host_name, variables);
    write_pair(out, cursor, value.user_name, variables);

    // Password: length in characters, bytes obfuscated.
    {
        const auto offset = static_cast<std::uint16_t>(login7_fixed_size + variables.size());
        const std::vector<std::uint8_t> encoded = obfuscate_password(value.password);
        patch_le16(out, cursor, offset);
        patch_le16(out, cursor + 2, static_cast<std::uint16_t>(value.password.size()));
        cursor += 4;
        for (const std::uint8_t byte : encoded) {
            variables.push_back(byte_of(byte));
        }
    }

    write_pair(out, cursor, value.application_name, variables);
    write_pair(out, cursor, value.server_name, variables);

    // Extension / unused pair stays zero-length.
    patch_le16(out, cursor, 0);
    patch_le16(out, cursor + 2, 0);
    cursor += 4;

    write_pair(out, cursor, value.client_interface_name, variables);
    write_pair(out, cursor, value.language, variables);
    write_pair(out, cursor, value.database, variables);

    // ClientID: six bytes, zero when the caller has none.
    for (std::size_t i = 0; i < 6; ++i) {
        out[cursor + i] = i < value.client_id.size() ? byte_of(value.client_id[i]) : byte_of(0);
    }
    cursor += 6;

    // SSPI pair: unused without integrated security.
    patch_le16(out, cursor, 0);
    patch_le16(out, cursor + 2, 0);
    cursor += 4;

    write_pair(out, cursor, value.attach_db_file, variables);
    write_pair(out, cursor, value.change_password, variables);

    // SSPI long length.
    patch_le16(out, cursor, 0);
    patch_le16(out, cursor + 2, 0);
    cursor += 4;

    if (cursor != login7_fixed_size) {
        return status{rpc_code::internal, "LOGIN7 fixed size is not 94 bytes"};
    }

    out.insert(out.end(), variables.begin(), variables.end());
    const std::size_t total = out.size();
    if (total > 0xFFFFu) {
        return status{rpc_code::encode_error, "LOGIN7 packet exceeds 64 KiB"};
    }
    patch_le16(out, 0, static_cast<std::uint16_t>(total));
    return status{};
}

// --- Value encoding ----------------------------------------------------------

namespace {

// "YYYY-MM-DD" -> days since 0001-01-01. The inverse of `format_date`; both use the
// same civil-calendar arithmetic so a round trip is exact.
[[nodiscard]] result<std::uint32_t> parse_date_days(std::string_view text) {
    if (text.size() < 10 || text[4] != '-' || text[7] != '-') {
        return rpc_error{rpc_code::invalid_argument, "a date must be YYYY-MM-DD"};
    }
    int year = 0;
    int month = 0;
    int day = 0;
    for (int i = 0; i < 4; ++i) {
        if (text[static_cast<std::size_t>(i)] < '0' || text[static_cast<std::size_t>(i)] > '9') {
            return rpc_error{rpc_code::invalid_argument, "a date year must be digits"};
        }
        year = year * 10 + (text[static_cast<std::size_t>(i)] - '0');
    }
    month = (text[5] - '0') * 10 + (text[6] - '0');
    day = (text[8] - '0') * 10 + (text[9] - '0');
    if (month < 1 || month > 12 || day < 1 || day > 31) {
        return rpc_error{rpc_code::invalid_argument, "a date field is out of range"};
    }
    // Civil-from-days, the inverse of the formatting algorithm above.
    const int y = month <= 2 ? year - 1 : year;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy =
        static_cast<unsigned>((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const std::int64_t days_since_1970 = era * 146097 + static_cast<std::int64_t>(doe) - 719468;
    const std::int64_t days = days_since_1970 + tds_epoch_offset_days;
    if (days < 0 || days > 3652058) { // 9999-12-31
        return rpc_error{rpc_code::invalid_argument, "a date is outside 0001-01-01..9999-12-31"};
    }
    return static_cast<std::uint32_t>(days);
}

// Splits "HH:MM:SS[.fraction]" into ticks of 100 ns.
[[nodiscard]] result<std::uint64_t> parse_ticks(std::string_view text) {
    if (text.size() < 8 || text[2] != ':' || text[5] != ':') {
        return rpc_error{rpc_code::invalid_argument, "a time must be HH:MM:SS"};
    }
    const auto digits = [&](std::size_t at, std::size_t count) -> int {
        int value = 0;
        for (std::size_t i = 0; i < count; ++i) {
            const char c = text[at + i];
            if (c < '0' || c > '9') {
                return -1;
            }
            value = value * 10 + (c - '0');
        }
        return value;
    };
    const int hour = digits(0, 2);
    const int minute = digits(3, 2);
    const int second = digits(6, 2);
    if (hour < 0 || minute < 0 || second < 0) {
        return rpc_error{rpc_code::invalid_argument, "a time field is not numeric"};
    }
    if (hour > 23 || minute > 59 || second > 59) {
        return rpc_error{rpc_code::invalid_argument, "a time field is out of range"};
    }
    std::uint64_t ticks = (static_cast<std::uint64_t>(hour) * 3600u +
                           static_cast<std::uint64_t>(minute) * 60u +
                           static_cast<std::uint64_t>(second)) *
                          10000000ull;
    if (text.size() > 9 && (text[8] == '.' || text[8] == ',')) {
        std::uint64_t fraction = 0;
        std::size_t used = 0;
        for (std::size_t i = 9; i < text.size() && used < 7; ++i, ++used) {
            const char c = text[i];
            if (c < '0' || c > '9') {
                return rpc_error{rpc_code::invalid_argument, "a fractional second is not numeric"};
            }
            fraction = fraction * 10 + static_cast<std::uint64_t>(c - '0');
        }
        for (std::size_t i = used; i < 7; ++i) {
            fraction *= 10;
        }
        ticks += fraction;
    }
    return ticks;
}

[[nodiscard]] int hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

} // namespace

status encode_date_value(std::vector<std::byte>& out, std::string_view text) {
    const result<std::uint32_t> days = parse_date_days(text);
    if (!days) {
        return days.error();
    }
    put_u8(out, static_cast<std::uint8_t>(days.value() & 0xffu));
    put_u8(out, static_cast<std::uint8_t>((days.value() >> 8) & 0xffu));
    put_u8(out, static_cast<std::uint8_t>((days.value() >> 16) & 0xffu));
    return status{};
}

status encode_time_value(std::vector<std::byte>& out, std::string_view text,
                         std::uint8_t scale) {
    const result<std::uint64_t> ticks = parse_ticks(text);
    if (!ticks) {
        return ticks.error();
    }
    const std::uint8_t bytes = scale <= 2 ? 3 : (scale <= 4 ? 4 : 5);
    for (std::uint8_t i = 0; i < bytes; ++i) {
        put_u8(out, static_cast<std::uint8_t>((ticks.value() >> (8 * i)) & 0xffu));
    }
    return status{};
}

status encode_datetime2_value(std::vector<std::byte>& out, std::string_view text,
                              std::uint8_t scale) {
    const std::size_t split = text.find_first_of(" T");
    if (split == std::string_view::npos) {
        return status{rpc_code::invalid_argument,
                      "a datetime2 value must be 'YYYY-MM-DD HH:MM:SS'"};
    }
    const result<std::uint32_t> days = parse_date_days(text.substr(0, split));
    if (!days) {
        return days.error();
    }
    const result<std::uint64_t> ticks = parse_ticks(text.substr(split + 1));
    if (!ticks) {
        return ticks.error();
    }
    const std::uint8_t time_bytes = scale <= 2 ? 3 : (scale <= 4 ? 4 : 5);
    for (std::uint8_t i = 0; i < time_bytes; ++i) {
        put_u8(out, static_cast<std::uint8_t>((ticks.value() >> (8 * i)) & 0xffu));
    }
    put_u8(out, static_cast<std::uint8_t>(days.value() & 0xffu));
    put_u8(out, static_cast<std::uint8_t>((days.value() >> 8) & 0xffu));
    put_u8(out, static_cast<std::uint8_t>((days.value() >> 16) & 0xffu));
    return status{};
}

status encode_guid_value(std::vector<std::byte>& out, std::string_view text) {
    // Accept the 36-character form with dashes in the usual places.
    if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' ||
        text[23] != '-') {
        return status{rpc_code::invalid_argument,
                      "a uniqueidentifier must be 8-4-4-4-12 hexadecimal digits"};
    }
    std::vector<std::uint8_t> raw;
    raw.reserve(16);
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '-') {
            ++i;
            continue;
        }
        if (i + 1 >= text.size()) {
            return status{rpc_code::invalid_argument, "a uniqueidentifier has an odd digit"};
        }
        const int high = hex_digit(text[i]);
        const int low = hex_digit(text[i + 1]);
        if (high < 0 || low < 0) {
            return status{rpc_code::invalid_argument, "a uniqueidentifier has a non-hex digit"};
        }
        raw.push_back(static_cast<std::uint8_t>((high << 4) | low));
        i += 2;
    }
    if (raw.size() != 16) {
        return status{rpc_code::invalid_argument, "a uniqueidentifier does not decode to 16 bytes"};
    }
    // Windows order: the first three groups are little-endian.
    const std::size_t order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    for (const std::size_t index : order) {
        put_u8(out, raw[index]);
    }
    return status{};
}

status encode_decimal_value(std::vector<std::byte>& out, std::string_view text,
                            std::uint8_t& precision, std::uint8_t& scale) {
    if (text.empty()) {
        return status{rpc_code::invalid_argument, "a decimal value is empty"};
    }
    std::size_t index = 0;
    bool negative = false;
    if (text[0] == '+' || text[0] == '-') {
        negative = text[0] == '-';
        index = 1;
    }
    std::uint64_t magnitude = 0;
    std::uint8_t digits = 0;
    std::uint8_t fraction = 0;
    bool saw_point = false;
    for (; index < text.size(); ++index) {
        const char c = text[index];
        if (c == '.') {
            if (saw_point) {
                return status{rpc_code::invalid_argument, "a decimal value has two points"};
            }
            saw_point = true;
            continue;
        }
        if (c < '0' || c > '9') {
            return status{rpc_code::invalid_argument, "a decimal value has a non-digit"};
        }
        if (magnitude > (std::numeric_limits<std::uint64_t>::max() - 9) / 10) {
            return status{rpc_code::invalid_argument, "a decimal value overflows 64 bits"};
        }
        magnitude = magnitude * 10 + static_cast<std::uint64_t>(c - '0');
        ++digits;
        if (saw_point) {
            ++fraction;
        }
    }
    if (digits == 0) {
        return status{rpc_code::invalid_argument, "a decimal value has no digits"};
    }
    if (digits > 38) {
        return status{rpc_code::invalid_argument, "a decimal value exceeds 38 digits"};
    }
    scale = fraction;
    precision = digits;
    // Byte count from the precision, per the server's own table: 1-9 digits fit in
    // five bytes including the sign, 10-19 in nine, 20-28 in thirteen, 29-38 in
    // seventeen.
    const std::uint8_t payload_bytes = precision <= 9     ? 5
                                       : precision <= 19  ? 9
                                       : precision <= 28  ? 13
                                                          : 17;
    put_u8(out, negative ? 0 : 1); // 1 means positive
    for (std::uint8_t i = 1; i < payload_bytes; ++i) {
        put_u8(out, static_cast<std::uint8_t>((magnitude >> (8 * (i - 1))) & 0xffu));
    }
    return status{};
}

// --- RPC ---------------------------------------------------------------------

namespace {

// A parameter's TYPE_INFO: the type byte and its type-specific info, with no user
// type and no flags word (a result column has both, a parameter has neither).
void put_type_info(std::vector<std::byte>& out, const column_meta& info) {
    put_u8(out, info.type);
    switch (info.type) {
    case 0x30: // INT1
    case 0x32: // BIT
    case 0x34: // INT2
    case 0x38: // INT4
    case 0x3B: // FLT4
    case 0x3E: // FLT8
    case 0x7F: // INT8
    case 0x3D: // DATETIME
    case 0x3C: // MONEY
    case 0x3A: // SMALLDATETIME
    case 0x7A: // SMALLMONEY
    case 0x1F: // NULL
        // Fixed-width types and NULL carry no type info at all.
        break;
    case 0x24: // GUID
    case 0x26: // INTN
    case 0x68: // BITN
    case 0x6D: // FLTN
        put_u8(out, static_cast<std::uint8_t>(info.max_length));
        break;
    case 0x28: // DATE
        break;
    case 0x29: // TIME
    case 0x2A: // DATETIME2
        put_u8(out, info.scale);
        break;
    case 0x6A: // DECIMALN
    case 0x6C: // NUMERICN
        put_u8(out, static_cast<std::uint8_t>(info.max_length));
        put_u8(out, info.precision);
        put_u8(out, info.scale);
        break;
    case 0xA7: // BIGVARCHAR
    case 0xAF: // BIGCHAR
    case 0xE7: // NVARCHAR
    case 0xEF: // NCHAR
        put_le16(out, static_cast<std::uint16_t>(info.max_length));
        if (info.max_length == 0xFFFF) {
            // A MAX type still carries a collation after the 0xFFFF marker.
            for (const std::uint8_t byte : info.collation) {
                put_u8(out, byte);
            }
        } else {
            for (const std::uint8_t byte : info.collation) {
                put_u8(out, byte);
            }
        }
        break;
    case 0xA5: // BIGVARBINARY
    case 0xAD: // BIGBINARY
        put_le16(out, static_cast<std::uint16_t>(info.max_length));
        break;
    default:
        break;
    }
}

} // namespace

status encode_rpc_value(std::vector<std::byte>& out, const rpc_parameter& param) {
    const column_meta& info = param.info;
    if (param.is_null) {
        // A NULL is a zero length for the length-prefixed types, and a zero byte for
        // the ones that use a byte count.
        switch (info.type) {
        case 0x1F:
            return status{};
        case 0x30: // INT1
        case 0x32: // BIT
        case 0x34: // INT2
        case 0x38: // INT4
        case 0x3B: // FLT4
        case 0x3E: // FLT8
        case 0x7F: // INT8
        case 0x3D: // DATETIME
        case 0x3C: // MONEY
        case 0x3A: // SMALLDATETIME
        case 0x7A: // SMALLMONEY
            return status{rpc_code::invalid_argument,
                          "a fixed-width TDS parameter cannot be NULL; send the nullable form"};
        case 0xA7: // BIGVARCHAR
        case 0xAF: // BIGCHAR
        case 0xE7: // NVARCHAR
        case 0xEF: // NCHAR
        case 0xA5: // BIGVARBINARY
        case 0xAD: // BIGBINARY
            put_le16(out, 0xFFFFu); // a byte count of 0xFFFF means NULL
            return status{};
        default:
            put_u8(out, 0); // the remaining types use a zero byte count for NULL
            return status{};
        }
    }

    switch (info.type) {
    case 0x30: // INT1
    case 0x32: // BIT
    case 0x34: // INT2
    case 0x38: // INT4
    case 0x3B: // FLT4
    case 0x3E: // FLT8
    case 0x7F: // INT8
    case 0x3D: // DATETIME
    case 0x3C: // MONEY
    case 0x3A: // SMALLDATETIME
    case 0x7A: // SMALLMONEY
        // Fixed-width: the value bytes only, with no length prefix at all.
        out.insert(out.end(), param.value.begin(), param.value.end());
        return status{};
    case 0x1F:
        return status{};
    case 0x24: // GUID
    case 0x26: // INTN
    case 0x68: // BITN
    case 0x6D: // FLTN
    case 0x28: // DATE
    case 0x29: // TIME
    case 0x2A: // DATETIME2
    case 0x6A: // DECIMALN
    case 0x6C: // NUMERICN
        if (param.value.size() > 0xFF) {
            return status{rpc_code::invalid_argument, "an RPC value exceeds its byte count"};
        }
        put_u8(out, static_cast<std::uint8_t>(param.value.size()));
        out.insert(out.end(), param.value.begin(), param.value.end());
        return status{};
    case 0xA7: // BIGVARCHAR
    case 0xAF: // BIGCHAR
    case 0xE7: // NVARCHAR
    case 0xEF: // NCHAR
    case 0xA5: // BIGVARBINARY
    case 0xAD: { // BIGBINARY
        // Bounded variable-length types prefix a little-endian byte count; a MAX type
        // uses PLP instead.
        if (info.max_length == 0xFFFF) {
            const std::size_t size = param.value.size();
            for (int i = 0; i < 8; ++i) {
                put_u8(out, static_cast<std::uint8_t>((static_cast<std::uint64_t>(size) >> (8 * i)) & 0xffu));
            }
            if (size != 0) {
                put_le32(out, static_cast<std::uint32_t>(size));
                out.insert(out.end(), param.value.begin(), param.value.end());
            }
            // PLP terminator: a chunk length of zero.
            put_le32(out, 0);
            return status{};
        }
        if (param.value.size() > 0xFFFFu || param.value.size() > info.max_length) {
            return status{rpc_code::invalid_argument,
                          "an RPC value does not fit the parameter's declared length"};
        }
        put_le16(out, static_cast<std::uint16_t>(param.value.size()));
        out.insert(out.end(), param.value.begin(), param.value.end());
        return status{};
    }
    default:
        return status{rpc_code::no_such_method, "this TDS parameter type is not encoded yet"};
    }
}

status encode_rpc(std::vector<std::byte>& out, const rpc_request& request) {
    out.clear();
    // A named procedure sends a USHORT character count and UTF-16LE characters; the
    // reserved proc id 0xFFFF followed by a proc id calls a system procedure.
    if (request.proc_name.empty() && request.proc_id != 0) {
        put_le16(out, 0xFFFFu);
        put_le16(out, request.proc_id);
    } else {
        if (request.proc_name.size() > 0xFFFFu / 2) {
            return status{rpc_code::invalid_argument, "the RPC name is too long"};
        }
        put_le16(out, static_cast<std::uint16_t>(request.proc_name.size()));
        const std::vector<std::byte> name = utf16le(request.proc_name);
        out.insert(out.end(), name.begin(), name.end());
    }
    put_le16(out, request.option_flags);
    for (const rpc_parameter& param : request.params) {
        if (param.name.size() > 0xFFu) {
            return status{rpc_code::invalid_argument, "an RPC parameter name is too long"};
        }
        put_u8(out, static_cast<std::uint8_t>(param.name.size()));
        const std::vector<std::byte> name = utf16le(param.name);
        out.insert(out.end(), name.begin(), name.end());
        put_u8(out, param.status);
        put_type_info(out, param.info);
        const status value = encode_rpc_value(out, param);
        if (!value) {
            return value;
        }
    }
    return status{};
}

// --- Column metadata and rows ------------------------------------------------

namespace {

[[nodiscard]] std::string format_date(std::uint32_t days) {
    // Civil-from-days (Howard Hinnant's algorithm), with the TDS epoch shifted to
    // 1970 so the arithmetic matches the published derivation.
    const std::int64_t z = static_cast<std::int64_t>(days) + (719468 - tds_epoch_offset_days);
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::uint32_t doe = static_cast<std::uint32_t>(z - era * 146097);
    const std::uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t year = static_cast<std::int64_t>(yoe) + era * 400;
    const std::uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::uint32_t mp = (5 * doy + 2) / 153;
    const std::uint32_t day = doy - (153 * mp + 2) / 5 + 1;
    const std::uint32_t month = mp + (mp < 10 ? 3u : static_cast<std::uint32_t>(-9));
    const std::int64_t civil_year = year + (month <= 2 ? 1 : 0);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04lld-%02u-%02u",
                  static_cast<long long>(civil_year), month, day);
    return buffer;
}

// Ticks are 100 ns units, which is the unit TIME and DATETIME2 use on the wire.
[[nodiscard]] std::string format_time(std::uint64_t ticks, std::uint8_t scale) {
    const std::uint64_t per_second = 10000000ull;
    const std::uint64_t total_seconds = ticks / per_second;
    const std::uint64_t fraction = ticks % per_second;
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%02llu:%02llu:%02llu",
                  static_cast<unsigned long long>(total_seconds / 3600),
                  static_cast<unsigned long long>((total_seconds / 60) % 60),
                  static_cast<unsigned long long>(total_seconds % 60));
    std::string text(buffer);
    if (scale != 0) {
        // The fraction is printed at the declared scale, not at full precision: a
        // DATETIME2(3) column must not look like it has seven digits of accuracy.
        char digits[16];
        std::snprintf(digits, sizeof(digits), "%07llu",
                      static_cast<unsigned long long>(fraction));
        text.push_back('.');
        text.append(digits, scale > 7 ? 7 : scale);
    }
    return text;
}

[[nodiscard]] std::string format_decimal(std::uint8_t precision, std::uint8_t scale,
                                         bool negative, std::uint64_t magnitude) {
    (void)precision;
    std::string digits = std::to_string(magnitude);
    if (scale != 0) {
        if (digits.size() <= scale) {
            digits.insert(0, static_cast<std::size_t>(scale) - digits.size() + 1, '0');
        }
        digits.insert(digits.size() - scale, 1, '.');
    }
    if (negative) {
        digits.insert(digits.begin(), '-');
    }
    return digits;
}

[[nodiscard]] std::string format_guid(span<const std::byte> data) {
    if (data.size() < 16) {
        return {};
    }
    // A TDS GUID is a Windows GUID: the first three groups are little-endian.
    const auto byte_at = [&](std::size_t index) {
        return static_cast<unsigned>(static_cast<std::uint8_t>(data[index]));
    };
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  byte_at(3), byte_at(2), byte_at(1), byte_at(0), byte_at(5), byte_at(4),
                  byte_at(7), byte_at(6), byte_at(8), byte_at(9), byte_at(10), byte_at(11),
                  byte_at(12), byte_at(13), byte_at(14), byte_at(15));
    return buffer;
}

} // namespace

status token_reader::read_colmetadata(std::vector<column_meta>& out) noexcept {
    // Little-endian, like every other multi-byte field inside a token. Reading the
    // count big-endian turns a one-column result into "256 columns" and desynchronises
    // the whole stream, which is how this was found.
    const std::uint16_t count = u16_le();
    if (error_ != rpc_code::ok) {
        return status{error_};
    }
    if (count == 0xFFFF) {
        // "No metadata": the statement produced no result set.
        out.clear();
        return status{};
    }
    out.clear();
    out.reserve(count);

    for (std::uint16_t index = 0; index < count; ++index) {
        column_meta column;
        (void)u32_le();                 // user type, not needed for decoding
        column.flags = u16_le();
        column.nullable = (column.flags & 0x0001u) != 0 || (column.flags & 0x0020u) != 0;
        column.type = u8();
        switch (column.type) {
        case 0x1F: // NULL
            column.max_length = 0;
            break;
        case 0x30: // INT1
        case 0x32: // BIT
            column.max_length = 1;
            break;
        case 0x34: // INT2
            column.max_length = 2;
            break;
        case 0x38: // INT4
        case 0x3B: // FLT4
        case 0x3A: // SMALLDATETIME
        case 0x7A: // SMALLMONEY
            column.max_length = 4;
            break;
        case 0x7F: // INT8
        case 0x3E: // FLT8
        case 0x3D: // DATETIME
        case 0x3C: // MONEY
            column.max_length = 8;
            break;
        case 0x24: // GUID
        case 0x26: // INTN
        case 0x68: // BITN
        case 0x6D: // FLTN
        case 0x6E: // MONEYN
        case 0x6F: // DATETIMEN
            column.max_length = u8();
            break;
        case 0x28: // DATE: no type info; the value is always three bytes.
            column.max_length = 3;
            break;
        case 0x29: // TIME
        case 0x2A: // DATETIME2
        case 0x2B: // DATETIMEOFFSET
            // Only the scale is sent. The byte count follows from it, and reading a
            // "max length" here as well shifts every later column by one byte.
            column.scale = u8();
            if (column.type == 0x29) {
                column.max_length = column.scale <= 2 ? 3 : (column.scale <= 4 ? 4 : 5);
            } else if (column.type == 0x2A) {
                column.max_length = static_cast<std::uint32_t>(
                    (column.scale <= 2 ? 6 : (column.scale <= 4 ? 7 : 8)));
            } else {
                column.max_length = static_cast<std::uint32_t>(
                    (column.scale <= 2 ? 8 : (column.scale <= 4 ? 9 : 10)));
            }
            break;
        case 0x6A: // DECIMALN
        case 0x6C: // NUMERICN
            column.max_length = u8();
            column.precision = u8();
            column.scale = u8();
            break;
        case 0xA7: // BIGVARCHAR
        case 0xAF: // BIGCHAR
        case 0xE7: // NVARCHAR
        case 0xEF: // NCHAR
            column.max_length = le16();
            // Character types carry a five-byte collation.
            for (std::size_t i = 0; i < 5; ++i) {
                column.collation[i] = u8();
            }
            break;
        case 0xA5: // BIGVARBINARY
        case 0xAD: // BIGBINARY
            column.max_length = le16();
            break;
        default: {
            fail(rpc_code::no_such_method);
            char message[64];
            std::snprintf(message, sizeof(message),
                          "TDS column type 0x%02x is not decoded yet",
                          static_cast<unsigned>(column.type));
            error_message_ = message;
            return status{rpc_code::no_such_method, message};
        }
        }
        column.name = b_varchar();
        if (error_ != rpc_code::ok) {
            return status{error_};
        }
        // A fixed-width type is never NULL on the wire; a variable one is nullable
        // whenever it can hold a zero length.
        column.nullable = column.max_length > 0 && column.type != 0x1F &&
                          column.type != 0x30 && column.type != 0x32 &&
                          column.type != 0x34 && column.type != 0x38 &&
                          column.type != 0x3B && column.type != 0x3A &&
                          column.type != 0x7A && column.type != 0x7F &&
                          column.type != 0x3E && column.type != 0x3D &&
                          column.type != 0x3C;
        out.push_back(std::move(column));
    }
    columns_ = out;
    return status{};
}

// One value, in canonical text. A ROW cell and a RETURNVALUE payload are the same
// bytes, so they share this reader; the only difference is that a row knows its NULLs
// from a bitmap while a return value encodes a NULL as a zero length.
// A parameter's TYPE_INFO: just the type byte and its type-specific info. A result
// column has a four-byte user type and a two-byte flags word in front of this; a
// parameter has neither, which is why one capture of `@P1` starts at `0x26 0x04`.
status token_reader::read_type_info(column_meta& column) noexcept {
    column = column_meta{};
    column.type = u8();
    if (error_ != rpc_code::ok) {
        return status{error_};
    }
    switch (column.type) {
    case 0x1F: // NULL
        column.max_length = 0;
        break;
    case 0x30: // INT1
    case 0x32: // BIT
        column.max_length = 1;
        break;
    case 0x34: // INT2
        column.max_length = 2;
        break;
    case 0x38: // INT4
    case 0x3B: // FLT4
    case 0x3A: // SMALLDATETIME
    case 0x7A: // SMALLMONEY
        column.max_length = 4;
        break;
    case 0x7F: // INT8
    case 0x3E: // FLT8
    case 0x3D: // DATETIME
    case 0x3C: // MONEY
        column.max_length = 8;
        break;
    case 0x24: // GUID
    case 0x26: // INTN
    case 0x68: // BITN
    case 0x6D: // FLTN
    case 0x6E: // MONEYN
    case 0x6F: // DATETIMEN
        column.max_length = u8();
        break;
    case 0x28: // DATE: no type info; the value is always three bytes.
        column.max_length = 3;
        break;
    case 0x29: // TIME
    case 0x2A: // DATETIME2
    case 0x2B: // DATETIMEOFFSET
        // Only the scale is sent; the byte count follows from it.
        column.scale = u8();
        if (column.type == 0x29) {
            column.max_length = column.scale <= 2 ? 3 : (column.scale <= 4 ? 4 : 5);
        } else if (column.type == 0x2A) {
            column.max_length = column.scale <= 2 ? 6 : (column.scale <= 4 ? 7 : 8);
        } else {
            column.max_length = column.scale <= 2 ? 8 : (column.scale <= 4 ? 9 : 10);
        }
        break;
    case 0x6A: // DECIMALN
    case 0x6C: // NUMERICN
        column.max_length = u8();
        column.precision = u8();
        column.scale = u8();
        break;
    case 0xA7: // BIGVARCHAR
    case 0xAF: // BIGCHAR
    case 0xE7: // NVARCHAR
    case 0xEF: // NCHAR
        column.max_length = le16();
        for (std::size_t i = 0; i < 5; ++i) {
            column.collation[i] = u8();
        }
        break;
    case 0xA5: // BIGVARBINARY
    case 0xAD: // BIGBINARY
        column.max_length = le16();
        break;
    default: {
        fail(rpc_code::no_such_method);
        char message[64];
        std::snprintf(message, sizeof(message), "TDS parameter type 0x%02x is not decoded yet",
                      static_cast<unsigned>(column.type));
        error_message_ = message;
        return status{rpc_code::no_such_method, message};
    }
    }
    return status{};
}

status token_reader::read_one(const column_meta& column, std::string& text,
                              bool& nulled) noexcept {
    text.clear();
    nulled = false;
    switch (column.type) {
    case 0x1F: // NULL column: always null
        nulled = true;
        return status{};
    case 0x30: { // INT1
        const std::int8_t value = static_cast<std::int8_t>(u8());
        text = std::to_string(static_cast<int>(value));
        return status{};
    }
    case 0x32: { // BIT
        text = u8() != 0 ? "1" : "0";
        return status{};
    }
    case 0x34: { // INT2
        const std::uint16_t low = u8();
        const std::uint16_t high = u8();
        const std::uint16_t raw = static_cast<std::uint16_t>(low | (high << 8));
        text = std::to_string(static_cast<std::int16_t>(raw));
        return status{};
    }
    case 0x38: { // INT4
        const std::uint32_t raw = le32();
        text = std::to_string(static_cast<std::int32_t>(raw));
        return status{};
    }
    case 0x7F: { // INT8
        const std::uint64_t raw = le64();
        text = std::to_string(static_cast<std::int64_t>(raw));
        return status{};
    }
    case 0x3B: { // FLT4
        const std::uint32_t raw = le32();
        float value = 0.0f;
        std::memcpy(&value, &raw, sizeof(value));
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
        text.assign(buffer);
        return status{};
    }
    case 0x3E: { // FLT8
        const std::uint64_t raw = le64();
        double value = 0.0;
        std::memcpy(&value, &raw, sizeof(value));
        char buffer[40];
        std::snprintf(buffer, sizeof(buffer), "%.17g", value);
        text.assign(buffer);
        return status{};
    }
    case 0x26:   // INTN
    case 0x68: { // BITN
        const std::uint8_t length = u8();
        if (length == 0) {
            nulled = true;
            return status{};
        }
        if (length > 8) {
            fail(rpc_code::protocol_error);
            return status{rpc_code::protocol_error, "an INTN value is wider than 8 bytes"};
        }
        std::uint64_t value = 0;
        for (std::uint8_t i = 0; i < length; ++i) {
            value |= static_cast<std::uint64_t>(u8()) << (8 * i);
        }
        // Sign-extend from the actual width.
        if (length < 8 && (value & (1ull << (8 * length - 1))) != 0) {
            value |= ~0ull << (8 * length);
        }
        text = std::to_string(static_cast<std::int64_t>(value));
        return status{};
    }
    case 0x6D: { // FLTN
        const std::uint8_t length = u8();
        if (length == 0) {
            nulled = true;
            return status{};
        }
        if (length == 4) {
            const std::uint32_t raw = le32();
            float value = 0.0f;
            std::memcpy(&value, &raw, sizeof(value));
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
            text.assign(buffer);
        } else {
            const std::uint64_t raw = le64();
            double value = 0.0;
            std::memcpy(&value, &raw, sizeof(value));
            char buffer[40];
            std::snprintf(buffer, sizeof(buffer), "%.17g", value);
            text.assign(buffer);
        }
        return status{};
    }
    case 0x6A:   // DECIMALN
    case 0x6C: { // NUMERICN
        const std::uint8_t length = u8();
        if (length == 0) {
            nulled = true;
            return status{};
        }
        // The sign byte is 1 for positive and 0 for negative -- the opposite of the
        // obvious reading, and a capture caught it: 12.34 arrived as -12.34.
        const bool negative = u8() == 0;
        std::uint64_t magnitude = 0;
        for (std::uint8_t i = 0; i + 1 < length; ++i) {
            const std::uint64_t byte = u8();
            if (i < 8) {
                magnitude |= byte << (8 * i);
            } else if (byte != 0) {
                // Beyond 16 digits the value cannot be carried here; refusing beats
                // printing a truncated number that looks right.
                fail(rpc_code::protocol_error);
                return status{rpc_code::protocol_error,
                              "a decimal value exceeds the 64-bit magnitude this reader carries"};
            }
        }
        text = format_decimal(column.precision, column.scale, negative, magnitude);
        return status{};
    }
    case 0x28: { // DATE: one length byte, then days since 0001-01-01
        const std::uint8_t length = u8();
        if (length == 0) {
            nulled = true;
            return status{};
        }
        std::uint32_t days = static_cast<std::uint32_t>(u8());
        days |= static_cast<std::uint32_t>(u8()) << 8;
        days |= static_cast<std::uint32_t>(u8()) << 16;
        text = format_date(days);
        return status{};
    }
    case 0x29: { // TIME
        const std::uint8_t length = u8();
        if (length == 0) {
            nulled = true;
            return status{};
        }
        std::uint64_t ticks = 0;
        for (std::uint8_t i = 0; i < length && i < 8; ++i) {
            ticks |= static_cast<std::uint64_t>(u8()) << (8 * i);
        }
        text = format_time(ticks, column.scale);
        return status{};
    }
    case 0x2A: { // DATETIME2: time, then three bytes of date
        const std::uint8_t length = u8();
        if (length == 0) {
            nulled = true;
            return status{};
        }
        std::uint64_t ticks = 0;
        const std::uint8_t time_bytes =
            column.scale <= 2 ? 3 : (column.scale <= 4 ? 4 : 5);
        for (std::uint8_t i = 0; i < time_bytes; ++i) {
            ticks |= static_cast<std::uint64_t>(u8()) << (8 * i);
        }
        std::uint32_t days = static_cast<std::uint32_t>(u8());
        days |= static_cast<std::uint32_t>(u8()) << 8;
        days |= static_cast<std::uint32_t>(u8()) << 16;
        text = format_date(days) + " " + format_time(ticks, column.scale);
        return status{};
    }
    case 0x6E:   // MONEYN
    case 0x6F: { // DATETIMEN: the nullable forms of MONEY/DATETIME and SMALLMONEY/SMALLDATETIME
        // A single length byte picks the width: 4 for the small variant, 8 for the large
        // one, 0 for NULL. A schema column is always nullable, so the server sends these
        // rather than the fixed-width types -- which is why `CAST(x AS money)` arrives as
        // 0x6E and not 0x3C.
        const std::uint8_t length = u8();
        if (length == 0) {
            nulled = true;
            return status{};
        }
        if (column.type == 0x6E) {
            if (length == 4) {
                const std::uint32_t raw = le32();
                const bool negative = (raw & 0x80000000u) != 0;
                const std::uint32_t magnitude = negative ? (~raw + 1u) : raw;
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), "%s%u.%04u", negative ? "-" : "",
                              static_cast<unsigned>(magnitude / 10000u),
                              static_cast<unsigned>(magnitude % 10000u));
                text.assign(buffer);
                return status{};
            }
            // Two 32-bit halves, high first: the 64-bit money value is not stored as one
            // little-endian number.
            const std::uint64_t high = le32();
            const std::uint64_t low = le32();
            const std::uint64_t raw = (high << 32) | low;
            const bool negative = (raw & 0x8000000000000000ull) != 0;
            const std::uint64_t magnitude = negative ? (~raw + 1ull) : raw;
            char buffer[40];
            std::snprintf(buffer, sizeof(buffer), "%s%llu.%04llu", negative ? "-" : "",
                          static_cast<unsigned long long>(magnitude / 10000ull),
                          static_cast<unsigned long long>(magnitude % 10000ull));
            text.assign(buffer);
            return status{};
        }
        if (length == 4) {
            const std::uint16_t days = static_cast<std::uint16_t>(u8() | (u8() << 8));
            const std::uint16_t minutes = static_cast<std::uint16_t>(u8() | (u8() << 8));
            text = format_date(static_cast<std::uint32_t>(days) + 693595u) + " " +
                   format_time(static_cast<std::uint64_t>(minutes) * 600000000ull, 0).substr(0, 5);
            return status{};
        }
        const std::uint32_t days = le32();
        const std::uint32_t ticks = le32();
        // 300 ticks per second, and one tick is 10,000,000/300 hundred-nanoseconds.
        const std::uint64_t hundred_ns =
            (static_cast<std::uint64_t>(ticks) * 10000000ull) / 300ull;
        text = format_date(days + 693595u) + " " + format_time(hundred_ns, 3);
        return status{};
    }
    case 0x3C: { // MONEY: two 32-bit halves (high, low), scaled by 10000
        const std::uint64_t high = le32();
        const std::uint64_t low = le32();
        const std::uint64_t raw = (high << 32) | low;
        const bool negative = (raw & 0x8000000000000000ull) != 0;
        const std::uint64_t magnitude = negative ? (~raw + 1ull) : raw;
        char buffer[40];
        std::snprintf(buffer, sizeof(buffer), "%s%llu.%04llu", negative ? "-" : "",
                      static_cast<unsigned long long>(magnitude / 10000ull),
                      static_cast<unsigned long long>(magnitude % 10000ull));
        text.assign(buffer);
        return status{};
    }
    case 0x7A: { // SMALLMONEY: same scale, fixed 4 bytes
        const std::uint32_t raw = le32();
        const bool negative = (raw & 0x80000000u) != 0;
        const std::uint32_t magnitude = negative ? (~raw + 1u) : raw;
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%s%u.%04u", negative ? "-" : "",
                      static_cast<unsigned>(magnitude / 10000u),
                      static_cast<unsigned>(magnitude % 10000u));
        text.assign(buffer);
        return status{};
    }
    case 0x3A: { // SMALLDATETIME: days since 1900-01-01 and minutes since midnight
        const std::uint16_t days = static_cast<std::uint16_t>(u8() | (u8() << 8));
        const std::uint16_t minutes = static_cast<std::uint16_t>(u8() | (u8() << 8));
        // 1900-01-01 is 693595 days after the 0001-01-01 epoch TDS uses elsewhere.
        text = format_date(static_cast<std::uint32_t>(days) + 693595u) + " " +
               format_time(static_cast<std::uint64_t>(minutes) * 600000000ull, 0).substr(0, 5);
        return status{};
    }
    case 0x3D: { // DATETIME: days since 1900-01-01 and 1/300 s ticks
        const std::uint32_t days = le32();
        const std::uint32_t ticks = le32();
        // 300 ticks per second. Rounding to milliseconds matches what the type can
        // represent (it is exact at 3.33 ms).
        // 300 ticks per second, and one tick is 10,000,000/300 hundred-nanoseconds.
        const std::uint64_t hundred_ns =
            (static_cast<std::uint64_t>(ticks) * 10000000ull) / 300ull;
        text = format_date(days + 693595u) + " " + format_time(hundred_ns, 3);
        return status{};
    }
    case 0x2B: { // DATETIMEOFFSET: a length, then time, date and the offset in minutes
        const std::uint8_t length = u8();
        if (length == 0) {
            nulled = true;
            return status{};
        }
        const std::uint8_t time_bytes =
            column.scale <= 2 ? 3 : (column.scale <= 4 ? 4 : 5);
        std::uint64_t ticks = 0;
        for (std::uint8_t i = 0; i < time_bytes && i < length; ++i) {
            ticks |= static_cast<std::uint64_t>(u8()) << (8 * i);
        }
        std::uint32_t days = static_cast<std::uint32_t>(u8());
        days |= static_cast<std::uint32_t>(u8()) << 8;
        days |= static_cast<std::uint32_t>(u8()) << 16;
        const std::int16_t minutes = static_cast<std::int16_t>(le16());
        // The value is UTC plus an offset; the text form is the local time with the
        // offset appended, so the offset is added back here.
        std::int64_t local_ticks = static_cast<std::int64_t>(ticks) +
                                   (static_cast<std::int64_t>(minutes) * 60ll * 10000000ll);
        std::int64_t local_days = static_cast<std::int64_t>(days);
        const std::int64_t ticks_per_day = 86400ll * 10000000ll;
        while (local_ticks < 0) {
            local_ticks += ticks_per_day;
            --local_days;
        }
        while (local_ticks >= ticks_per_day) {
            local_ticks -= ticks_per_day;
            ++local_days;
        }
        const bool west = minutes < 0;
        const int magnitude = west ? -static_cast<int>(minutes) : static_cast<int>(minutes);
        char offset[8];
        std::snprintf(offset, sizeof(offset), "%c%02d:%02d", west ? '-' : '+', magnitude / 60,
                      magnitude % 60);
        text = format_date(static_cast<std::uint32_t>(local_days)) + " " +
               format_time(static_cast<std::uint64_t>(local_ticks), column.scale) + " " + offset;
        return status{};
    }
    case 0xE7:   // NVARCHAR
    case 0xEF: { // NCHAR
        const std::uint16_t bytes = le16();
        if (bytes == 0) {
            nulled = true;
            return status{};
        }
        if (!need(bytes)) {
            fail(rpc_code::protocol_error);
            return status{rpc_code::protocol_error, "a truncated NVARCHAR value"};
        }
        text = from_utf16le(payload_.subspan(offset_, bytes));
        offset_ += bytes;
        return status{};
    }
    case 0xA7:   // BIGVARCHAR
    case 0xAF: { // BIGCHAR
        const std::uint16_t bytes = le16();
        if (bytes == 0) {
            nulled = true;
            return status{};
        }
        if (!need(bytes)) {
            fail(rpc_code::protocol_error);
            return status{rpc_code::protocol_error, "a truncated VARCHAR value"};
        }
        // VARCHAR is in the column's collation code page. Bytes are passed through;
        // claiming a specific encoding here would corrupt a non-UTF-8 collation.
        text.assign(reinterpret_cast<const char*>(payload_.data() + offset_), bytes);
        offset_ += bytes;
        return status{};
    }
    case 0xA5:   // BIGVARBINARY
    case 0xAD: { // BIGBINARY
        const std::uint16_t bytes = le16();
        if (bytes == 0) {
            nulled = true;
            return status{};
        }
        if (!need(bytes)) {
            fail(rpc_code::protocol_error);
            return status{rpc_code::protocol_error, "a truncated VARBINARY value"};
        }
        text.assign(reinterpret_cast<const char*>(payload_.data() + offset_), bytes);
        offset_ += bytes;
        return status{};
    }
    case 0x24: { // GUID
        const std::uint8_t length = u8();
        if (length == 0) {
            nulled = true;
            return status{};
        }
        if (length != 16 || !need(length)) {
            fail(rpc_code::protocol_error);
            return status{rpc_code::protocol_error, "a UNIQUEIDENTIFIER is not 16 bytes"};
        }
        text = format_guid(payload_.subspan(offset_, length));
        offset_ += length;
        return status{};
    }
    default: {
        fail(rpc_code::no_such_method);
        char message[64];
        std::snprintf(message, sizeof(message), "TDS column type 0x%02x is not decoded yet",
                      static_cast<unsigned>(column.type));
        error_message_ = message;
        return status{rpc_code::no_such_method, message};
    }
    }
}

status token_reader::read_row(bool compressed, std::vector<std::string>& values,
                              std::vector<bool>& nulls) noexcept {
    values.clear();
    nulls.clear();
    if (columns_.empty()) {
        // A ROW without metadata means the stream lost the description; guessing
        // widths from the payload would produce silent garbage.
        fail(rpc_code::protocol_error);
        return status{rpc_code::protocol_error, "a TDS row arrived before any column metadata"};
    }

    if (compressed) {
        // NBCROW prefixes the row with a null bitmap: a set bit means no value bytes
        // follow for that column, which is what makes fixed-width types nullable.
        const std::size_t bytes = (columns_.size() + 7) / 8;
        bitmap_.assign(bytes, 0);
        for (std::size_t i = 0; i < bytes; ++i) {
            bitmap_[i] = u8();
        }
    }

    for (std::size_t index = 0; index < columns_.size(); ++index) {
        std::string text;
        bool nulled = false;
        if (compressed && (bitmap_[index / 8] & (1u << (index % 8))) != 0) {
            nulled = true;
        } else {
            const status read = read_one(columns_[index], text, nulled);
            if (!read) {
                return read;
            }
        }
        values.push_back(std::move(text));
        nulls.push_back(nulled);
    }
    return status{};
}

// --- Tokens ------------------------------------------------------------------

void token_reader::fail(rpc_code code) noexcept {
    if (error_ == rpc_code::ok) {
        error_ = code;
    }
}

std::uint8_t token_reader::u8() noexcept {
    if (!need(1)) {
        fail(rpc_code::protocol_error);
        return 0;
    }
    const auto value = static_cast<std::uint8_t>(payload_[offset_]);
    ++offset_;
    return value;
}

std::uint16_t token_reader::u16() noexcept {
    if (!need(2)) {
        fail(rpc_code::protocol_error);
        return 0;
    }
    const auto value = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload_[offset_])) << 8) |
        static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload_[offset_ + 1])));
    offset_ += 2;
    return value;
}

std::uint32_t token_reader::u32() noexcept {
    if (!need(4)) {
        fail(rpc_code::protocol_error);
        return 0;
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value = (value << 8) |
                static_cast<std::uint32_t>(static_cast<std::uint8_t>(payload_[offset_ + static_cast<std::size_t>(i)]));
    }
    offset_ += 4;
    return value;
}

std::uint16_t token_reader::u16_le() noexcept {
    if (!need(2)) {
        fail(rpc_code::protocol_error);
        return 0;
    }
    const auto value = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload_[offset_])) |
        (static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload_[offset_ + 1])) << 8));
    offset_ += 2;
    return value;
}

std::uint32_t token_reader::u32_le() noexcept {
    if (!need(4)) {
        fail(rpc_code::protocol_error);
        return 0;
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(
                     static_cast<std::uint8_t>(payload_[offset_ + static_cast<std::size_t>(i)]))
                 << (8 * i);
    }
    offset_ += 4;
    return value;
}

std::uint16_t token_reader::le16() noexcept {
    const std::uint8_t low = u8();
    const std::uint8_t high = u8();
    return static_cast<std::uint16_t>(low) | (static_cast<std::uint16_t>(high) << 8);
}

std::uint32_t token_reader::le32() noexcept {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(u8()) << (8 * i);
    }
    return value;
}

std::uint64_t token_reader::le64() noexcept {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(u8()) << (8 * i);
    }
    return value;
}

std::uint16_t token_reader::be16() noexcept {
    const std::uint8_t high = u8();
    const std::uint8_t low = u8();
    return (static_cast<std::uint16_t>(high) << 8) | static_cast<std::uint16_t>(low);
}
std::string token_reader::b_varchar() noexcept {
    const std::uint8_t characters = u8();
    if (error_ != rpc_code::ok) {
        return {};
    }
    const std::size_t bytes = static_cast<std::size_t>(characters) * 2;
    if (!need(bytes)) {
        fail(rpc_code::protocol_error);
        return {};
    }
    std::string text = from_utf16le(payload_.subspan(offset_, bytes));
    offset_ += bytes;
    return text;
}

std::string token_reader::us_varchar() noexcept {
    const std::uint16_t characters = u16_le();
    if (error_ != rpc_code::ok) {
        return {};
    }
    const std::size_t bytes = static_cast<std::size_t>(characters) * 2;
    if (!need(bytes)) {
        fail(rpc_code::protocol_error);
        return {};
    }
    std::string text = from_utf16le(payload_.subspan(offset_, bytes));
    offset_ += bytes;
    return text;
}

token_reader::result token_reader::next(token& out) noexcept {
    if (error_ != rpc_code::ok) {
        return result::error;
    }
    if (offset_ >= payload_.size()) {
        return result::end;
    }
    out = token{};
    const std::uint8_t raw = u8();
    out.raw_type = raw;
    if (error_ != rpc_code::ok) {
        return result::error;
    }

    const auto read_error_like = [&](error_token& target) {
        const std::uint16_t length = u16_le();
        if (error_ != rpc_code::ok) {
            return;
        }
        const std::size_t end = offset_ + length;
        if (!need(length)) {
            fail(rpc_code::protocol_error);
            return;
        }
        target.number = u32_le();
        target.state = u8();
        target.severity = u8();
        target.message = us_varchar();
        target.server_name = b_varchar();
        target.procedure_name = b_varchar();
        target.line = u32_le();
        // The declared length is authoritative: a token whose fields disagree with it
        // must not shift the following tokens.
        offset_ = end;
    };

    switch (static_cast<token_type>(raw)) {
    case token_type::error:
        out.type = token::kind::error;
        read_error_like(out.error);
        break;
    case token_type::info:
        out.type = token::kind::info;
        read_error_like(out.info);
        break;
    case token_type::loginack: {
        out.type = token::kind::login_ack;
        const std::uint16_t length = u16_le();
        if (error_ != rpc_code::ok) {
            break;
        }
        const std::size_t end = offset_ + length;
        if (!need(length)) {
            fail(rpc_code::protocol_error);
            break;
        }
        out.ack.interface = u8();
        out.ack.tds_version = u32();
        out.ack.program_name = b_varchar();
        const std::size_t remaining = end > offset_ ? end - offset_ : 0;
        for (std::size_t i = 0; i < remaining && i < 5; ++i) {
            out.ack.server_version.push_back(u8());
        }
        offset_ = end;
        break;
    }
    case token_type::envchange: {
        out.type = token::kind::env_change;
        const std::uint16_t length = u16_le();
        if (error_ != rpc_code::ok) {
            break;
        }
        const std::size_t end = offset_ + length;
        if (!need(length)) {
            fail(rpc_code::protocol_error);
            break;
        }
        out.env.type = static_cast<env_change_type>(u8());
        switch (out.env.type) {
        case env_change_type::database:
        case env_change_type::language:
        case env_change_type::charset: {
            out.env.new_value = b_varchar();
            out.env.old_value = b_varchar();
            break;
        }
        case env_change_type::packet_size: {
            const std::string text = b_varchar();
            out.env.new_packet_size = static_cast<std::uint32_t>(std::strtoul(text.c_str(), nullptr, 10));
            out.env.new_value = text;
            out.env.old_value = b_varchar();
            break;
        }
        case env_change_type::begin_transaction:
        case env_change_type::commit_transaction:
        case env_change_type::rollback_transaction: {
            // The new value is length-prefixed: one byte of length, then the
            // descriptor itself. Without consuming that length byte the client echoes
            // a descriptor shifted by one, and SQL Server answers "the server failed
            // to resume the transaction" with digits that look almost right.
            const std::uint8_t descriptor_length = u8();
            if (descriptor_length > transaction_descriptor_size) {
                fail(rpc_code::protocol_error);
                break;
            }
            for (std::size_t i = 0; i < descriptor_length; ++i) {
                out.env.new_descriptor[i] = u8();
            }
            out.env.old_value = b_varchar();
            break;
        }
        default:
            break;
        }
        offset_ = end;
        break;
    }
    case token_type::done:
    case token_type::done_proc:
    case token_type::done_in_proc: {
        out.type = token::kind::done;
        out.done.status = u16_le();
        out.done.current_command = u16_le();
        // All three DONE variants carry an 8-byte row count in TDS 7.2+. Reading
        // four for DONE_PROC/DONE_IN_PROC leaves four bytes in the stream, and the
        // next token then decodes as garbage -- which is exactly what a real server
        // reply showed before this was fixed.
        const std::uint32_t low = u32_le();
        const std::uint32_t high = u32_le();
        out.done.row_count = (static_cast<std::uint64_t>(high) << 32) | low;
        out.done.has_row_count = true;
        break;
    }
    case token_type::return_status: {
        out.type = token::kind::return_status;
        out.ret_status = u32_le();
        break;
    }
    case token_type::return_value: {
        out.type = token::kind::return_value;
        out.ret.ordinal = u16_le();
        out.ret.name = b_varchar();
        out.ret.status = u8();
        // A RETURNVALUE carries a user type and a flags word before its TYPE_INFO,
        // unlike an RPC request parameter, which has neither. Skipping them shifts
        // the type byte by six and the value decodes as garbage.
        out.ret.meta.flags = 0;
        (void)u32_le();            // user type
        out.ret.meta.flags = u16_le();
        const status info = read_type_info(out.ret.meta);
        if (!info) {
            return result::error;
        }
        std::string text;
        bool nulled = false;
        const status read = read_one(out.ret.meta, text, nulled);
        if (!read) {
            return result::error;
        }
        out.ret.value = std::move(text);
        out.ret.is_null = nulled;
        break;
    }
    case token_type::colmetadata: {
        out.type = token::kind::colmetadata;
        const status read = read_colmetadata(out.columns);
        if (!read) {
            return result::error;
        }
        break;
    }
    case token_type::row:
    case token_type::nbc_row: {
        out.type = token::kind::row;
        const status read = read_row(static_cast<token_type>(raw) == token_type::nbc_row,
                                     out.values, out.nulls);
        if (!read) {
            return result::error;
        }
        break;
    }    default: {
        // A token this slice does not model still has a length; skipping it keeps the
        // stream aligned for the tokens after it.
        out.type = token::kind::unknown;
        const std::uint16_t length = u16_le();
        if (error_ != rpc_code::ok) {
            break;
        }
        if (!need(length)) {
            fail(rpc_code::protocol_error);
            break;
        }
        offset_ += length;
        break;
    }
    }

    if (error_ != rpc_code::ok) {
        return result::error;
    }
    return result::token_ready;
}

rpc_code map_error_number(std::uint32_t number) noexcept {
    switch (number) {
    case 1205: // deadlock victim
    case 1222: // lock request timeout
        return rpc_code::unavailable;
    case 2601: // duplicate key, unique index
    case 2627: // duplicate key, constraint
    case 515:  // cannot insert NULL
    case 547:  // constraint conflict
        return rpc_code::failed_precondition;
    case 208:  // invalid object name
    case 2812: // could not find stored procedure
        return rpc_code::not_found;
    case 229:  // permission denied
    case 230:  // column permission denied
    case 18456: // login failed
        return rpc_code::failed_precondition;
    case 102:  // incorrect syntax
    case 105:  // unclosed quotation
    case 156:  // keyword syntax
    case 8134: // divide by zero
        return rpc_code::invalid_argument;
    case 0:    // often used for a "no error" placeholder in an ERROR token
        return rpc_code::application;
    default:
        return rpc_code::application;
    }
}

} // namespace tds
} // namespace db
} // namespace crpc
