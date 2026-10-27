// CRPC -- TDS login-phase codec.
#include <crpc/db/tds/protocol.hpp>

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
    std::vector<std::uint8_t> out;
    out.reserve(password.size());
    for (const char c : password) {
        const auto value = static_cast<std::uint8_t>(c);
        // Nibble swap, then XOR. A password sent as plain bytes is rejected by every
        // SQL Server and looks like a wrong credential.
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
    const std::uint16_t characters = u16();
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
        const std::uint16_t length = u16();
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
        const std::uint16_t length = u16();
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
        const std::uint16_t length = u16();
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
            // An 8-byte transaction descriptor, then the old descriptor as a string.
            if (!need(8)) {
                fail(rpc_code::protocol_error);
                break;
            }
            offset_ += 8;
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
    default: {
        // A token this slice does not model still has a length; skipping it keeps the
        // stream aligned for the tokens after it.
        out.type = token::kind::unknown;
        const std::uint16_t length = u16();
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
