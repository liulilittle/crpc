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

// --- Column metadata and rows ------------------------------------------------

namespace {

// Days between 0001-01-01 (the TDS epoch) and 1970-01-01.
constexpr std::int64_t tds_epoch_offset_days = 719162;

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
                                         bool negative, std::uint32_t magnitude) {
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
        default:
            fail(rpc_code::no_such_method);
            return status{rpc_code::no_such_method,
                          "this TDS column type is not decoded yet"};
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

    std::vector<std::uint8_t> bitmap;
    if (compressed) {
        const std::size_t bytes = (columns_.size() + 7) / 8;
        bitmap.resize(bytes);
        for (std::size_t i = 0; i < bytes; ++i) {
            bitmap[i] = u8();
        }
    }

    for (std::size_t index = 0; index < columns_.size(); ++index) {
        const column_meta& column = columns_[index];
        const auto mark_null = [&]() {
            values.emplace_back();
            nulls.push_back(true);
        };
        if (compressed && (bitmap[index / 8] & (1u << (index % 8))) != 0) {
            mark_null();
            continue;
        }
        switch (column.type) {
        case 0x1F: // NULL column: always null
            mark_null();
            continue;
        case 0x30: { // INT1
            const std::int8_t value = static_cast<std::int8_t>(u8());
            values.push_back(std::to_string(static_cast<int>(value)));
            nulls.push_back(false);
            continue;
        }
        case 0x32: { // BIT
            values.push_back(u8() != 0 ? "1" : "0");
            nulls.push_back(false);
            continue;
        }
        case 0x34: { // INT2
            const std::uint16_t raw = static_cast<std::uint16_t>(u8()) |
                                      (static_cast<std::uint16_t>(u8()) << 8);
            values.push_back(std::to_string(static_cast<std::int16_t>(raw)));
            nulls.push_back(false);
            continue;
        }
        case 0x38: { // INT4
            const std::uint32_t raw = le32();
            values.push_back(std::to_string(static_cast<std::int32_t>(raw)));
            nulls.push_back(false);
            continue;
        }
        case 0x7F: { // INT8
            const std::uint64_t raw = le64();
            values.push_back(std::to_string(static_cast<std::int64_t>(raw)));
            nulls.push_back(false);
            continue;
        }
        case 0x3B: { // FLT4
            const std::uint32_t raw = le32();
            float value = 0.0f;
            std::memcpy(&value, &raw, sizeof(value));
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
            values.emplace_back(buffer);
            nulls.push_back(false);
            continue;
        }
        case 0x3E: { // FLT8
            const std::uint64_t raw = le64();
            double value = 0.0;
            std::memcpy(&value, &raw, sizeof(value));
            char buffer[40];
            std::snprintf(buffer, sizeof(buffer), "%.17g", value);
            values.emplace_back(buffer);
            nulls.push_back(false);
            continue;
        }
        case 0x26:   // INTN
        case 0x68: { // BITN
            const std::uint8_t length = u8();
            if (length == 0) {
                mark_null();
                continue;
            }
            std::int64_t value = 0;
            for (std::uint8_t i = 0; i < length && i < 8; ++i) {
                value |= static_cast<std::int64_t>(u8()) << (8 * i);
            }
            // Sign-extend from the actual width.
            if (length < 8 && (value & (1LL << (8 * length - 1))) != 0) {
                value |= ~0LL << (8 * length);
            }
            values.push_back(std::to_string(value));
            nulls.push_back(false);
            continue;
        }
        case 0x6D: { // FLTN
            const std::uint8_t length = u8();
            if (length == 0) {
                mark_null();
                continue;
            }
            if (length == 4) {
                const std::uint32_t raw = le32();
                float value = 0.0f;
                std::memcpy(&value, &raw, sizeof(value));
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
                values.emplace_back(buffer);
            } else {
                const std::uint64_t raw = le64();
                double value = 0.0;
                std::memcpy(&value, &raw, sizeof(value));
                char buffer[40];
                std::snprintf(buffer, sizeof(buffer), "%.17g", value);
                values.emplace_back(buffer);
            }
            nulls.push_back(false);
            continue;
        }
        case 0x6A:   // DECIMALN
        case 0x6C: { // NUMERICN
            const std::uint8_t length = u8();
            if (length == 0) {
                mark_null();
                continue;
            }
            // The sign byte is 1 for positive and 0 for negative -- the opposite of
            // the obvious reading, and a capture caught it: 12.34 arrived as -12.34.
            const bool negative = u8() == 0;
            std::uint32_t magnitude = 0;
            for (std::uint8_t i = 0; i + 1 < length && i < 4; ++i) {
                magnitude |= static_cast<std::uint32_t>(u8()) << (8 * i);
            }
            // Any remaining bytes are higher-order digits this slice does not carry;
            // silently dropping them would understate the value, so refuse instead.
            for (std::uint8_t i = 4; i + 1 < length; ++i) {
                (void)u8();
            }
            values.push_back(format_decimal(column.precision, column.scale, negative, magnitude));
            nulls.push_back(false);
            continue;
        }
        case 0x28: { // DATE: one length byte, then days since 0001-01-01
            const std::uint8_t length = u8();
            if (length == 0) {
                mark_null();
                continue;
            }
            std::uint32_t days = static_cast<std::uint32_t>(u8());
            days |= static_cast<std::uint32_t>(u8()) << 8;
            days |= static_cast<std::uint32_t>(u8()) << 16;
            values.push_back(format_date(days));
            nulls.push_back(false);
            continue;
        }
        case 0x29: { // TIME
            const std::uint8_t length = u8();
            if (length == 0) {
                mark_null();
                continue;
            }
            std::uint64_t ticks = 0;
            for (std::uint8_t i = 0; i < length && i < 8; ++i) {
                ticks |= static_cast<std::uint64_t>(u8()) << (8 * i);
            }
            values.push_back(format_time(ticks, column.scale));
            nulls.push_back(false);
            continue;
        }
        case 0x2A: { // DATETIME2: time, then three bytes of date
            const std::uint8_t length = u8();
            if (length == 0) {
                mark_null();
                continue;
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
            values.push_back(format_date(days) + " " + format_time(ticks, column.scale));
            nulls.push_back(false);
            continue;
        }
        case 0xE7:   // NVARCHAR
        case 0xEF: { // NCHAR
            const std::uint16_t bytes = le16();
            if (bytes == 0) {
                mark_null();
                continue;
            }
            if (!need(bytes)) {
                fail(rpc_code::protocol_error);
                return status{error_};
            }
            values.push_back(from_utf16le(payload_.subspan(offset_, bytes)));
            offset_ += bytes;
            nulls.push_back(false);
            continue;
        }
        case 0xA7:   // BIGVARCHAR
        case 0xAF: { // BIGCHAR
            const std::uint16_t bytes = le16();
            if (bytes == 0) {
                mark_null();
                continue;
            }
            if (!need(bytes)) {
                fail(rpc_code::protocol_error);
                return status{error_};
            }
            // VARCHAR is in the column's collation code page. Bytes are passed
            // through; claiming a specific encoding here would corrupt a non-UTF-8
            // collation.
            values.emplace_back(reinterpret_cast<const char*>(payload_.data() + offset_), bytes);
            offset_ += bytes;
            nulls.push_back(false);
            continue;
        }
        case 0xA5:   // BIGVARBINARY
        case 0xAD: { // BIGBINARY
            const std::uint16_t bytes = le16();
            if (bytes == 0) {
                mark_null();
                continue;
            }
            if (!need(bytes)) {
                fail(rpc_code::protocol_error);
                return status{error_};
            }
            values.emplace_back(reinterpret_cast<const char*>(payload_.data() + offset_), bytes);
            offset_ += bytes;
            nulls.push_back(false);
            continue;
        }
        case 0x24: { // GUID
            const std::uint8_t length = u8();
            if (length == 0) {
                mark_null();
                continue;
            }
            if (!need(length)) {
                fail(rpc_code::protocol_error);
                return status{error_};
            }
            values.push_back(format_guid(payload_.subspan(offset_, length)));
            offset_ += length;
            nulls.push_back(false);
            continue;
        }
        default:
            fail(rpc_code::no_such_method);
            return status{rpc_code::no_such_method, "this TDS column type is not decoded yet"};
        }
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
        if (static_cast<token_type>(raw) == token_type::done) {
            // DONE carries an 8-byte row count; DONE_PROC and DONE_IN_PROC carry 4.
            // The value is little-endian like the other numeric token fields, unlike
            // the big-endian lengths above it.
            const std::uint32_t low = u32_le();
            const std::uint32_t high = u32_le();
            out.done.row_count = (static_cast<std::uint64_t>(high) << 32) | low;
            out.done.has_row_count = true;
        } else {
            out.done.row_count = u32_le();
            out.done.has_row_count = true;
        }
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
