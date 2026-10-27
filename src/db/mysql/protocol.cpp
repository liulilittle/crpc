// CRPC -- MySQL/MariaDB wire protocol codec implementation.
#include <crpc/db/mysql/protocol.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace crpc {
namespace db {
namespace mysql {
namespace {

constexpr std::byte byte_of(std::uint8_t value) noexcept {
    return static_cast<std::byte>(value);
}

void append_u16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(byte_of(static_cast<std::uint8_t>(value & 0xffu)));
    out.push_back(byte_of(static_cast<std::uint8_t>((value >> 8) & 0xffu)));
}

void append_u24(std::vector<std::byte>& out, std::uint32_t value) {
    out.push_back(byte_of(static_cast<std::uint8_t>(value & 0xffu)));
    out.push_back(byte_of(static_cast<std::uint8_t>((value >> 8) & 0xffu)));
    out.push_back(byte_of(static_cast<std::uint8_t>((value >> 16) & 0xffu)));
}

void append_u32(std::vector<std::byte>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(byte_of(static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu)));
    }
}

// NUL-terminated C string, as the handshake uses for the server version and the
// user name.
void append_cstring(std::vector<std::byte>& out, std::string_view text) {
    out.insert(out.end(), reinterpret_cast<const std::byte*>(text.data()),
               reinterpret_cast<const std::byte*>(text.data()) + text.size());
    out.push_back(std::byte{0});
}

[[nodiscard]] bool read_cstring(span<const std::byte> in, std::size_t& offset,
                                std::string& out) {
    const std::size_t start = offset;
    while (offset < in.size() && in[offset] != std::byte{0}) {
        ++offset;
    }
    if (offset >= in.size()) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(in.data() + start), offset - start);
    ++offset; // the NUL
    return true;
}

[[nodiscard]] bool need(span<const std::byte> in, std::size_t offset, std::size_t count) noexcept {
    return offset <= in.size() && in.size() - offset >= count;
}

} // namespace

// --- Encoding ----------------------------------------------------------------

void append_lenenc_int(std::vector<std::byte>& out, std::uint64_t value) {
    if (value < 251) {
        out.push_back(byte_of(static_cast<std::uint8_t>(value)));
        return;
    }
    if (value <= 0xffffu) {
        out.push_back(byte_of(0xfc));
        append_u16(out, static_cast<std::uint16_t>(value));
        return;
    }
    if (value <= 0xffffffu) {
        out.push_back(byte_of(0xfd));
        append_u24(out, static_cast<std::uint32_t>(value));
        return;
    }
    out.push_back(byte_of(0xfe));
    for (int i = 0; i < 8; ++i) {
        out.push_back(byte_of(static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu)));
    }
}

void append_lenenc_string(std::vector<std::byte>& out, std::string_view text) {
    append_lenenc_int(out, text.size());
    out.insert(out.end(), reinterpret_cast<const std::byte*>(text.data()),
               reinterpret_cast<const std::byte*>(text.data()) + text.size());
}

void append_null(std::vector<std::byte>& out) { out.push_back(byte_of(0xfb)); }

status write_message(std::vector<std::byte>& out, span<const std::byte> payload,
                     std::uint8_t& sequence) {
    std::size_t offset = 0;
    for (;;) {
        const std::size_t remaining = payload.size() - offset;
        const std::size_t chunk = remaining > max_packet_payload ? max_packet_payload : remaining;

        out.push_back(byte_of(static_cast<std::uint8_t>(chunk & 0xffu)));
        out.push_back(byte_of(static_cast<std::uint8_t>((chunk >> 8) & 0xffu)));
        out.push_back(byte_of(static_cast<std::uint8_t>((chunk >> 16) & 0xffu)));
        out.push_back(byte_of(sequence));
        ++sequence;

        if (chunk > 0) {
            out.insert(out.end(), payload.begin() + static_cast<std::ptrdiff_t>(offset),
                       payload.begin() + static_cast<std::ptrdiff_t>(offset + chunk));
            offset += chunk;
        }

        // An exact multiple of the maximum payload needs a trailing empty packet:
        // otherwise the server waits for the rest of the message.
        if (chunk < max_packet_payload) {
            break;
        }
    }
    return status{};
}

std::size_t handshake_response_size(const handshake_response_options& options) noexcept {
    const bool lenenc = (options.capabilities & mask(capability::plugin_auth_lenenc_data)) != 0;
    const std::size_t auth = lenenc ? options.auth_response.size() + 9
                                    : options.auth_response.size() + 1;
    std::size_t size = 4 + 4 + 1 + 23 + options.username.size() + 1 + auth;
    if ((options.capabilities & mask(capability::connect_with_db)) != 0) {
        size += options.database.size() + 1;
    }
    if ((options.capabilities & mask(capability::plugin_auth)) != 0) {
        size += options.auth_plugin.size() + 1;
    }
    return size;
}

status encode_handshake_response(span<std::byte> out,
                                 const handshake_response_options& options,
                                 std::size_t& written) {
    written = 0;
    const std::size_t needed = handshake_response_size(options);
    if (out.size() < needed) {
        return status{rpc_code::encode_error, "mysql handshake response does not fit"};
    }
    if ((options.capabilities & mask(capability::protocol_41)) == 0) {
        return status{rpc_code::invalid_argument,
                      "mysql handshake response requires protocol 4.1"};
    }
    if (options.username.empty()) {
        return status{rpc_code::invalid_argument, "mysql handshake response needs a user name"};
    }

    std::vector<std::byte> body;
    body.reserve(needed);
    append_u32(body, options.capabilities);
    append_u32(body, options.max_packet_size);
    body.push_back(byte_of(options.charset));
    // 23 reserved zero bytes. A server may read them back, so they must be zeroed.
    body.insert(body.end(), 23, std::byte{0});
    append_cstring(body, options.username);

    const bool lenenc = (options.capabilities & mask(capability::plugin_auth_lenenc_data)) != 0;
    if (lenenc) {
        append_lenenc_int(body, options.auth_response.size());
        body.insert(body.end(), options.auth_response.begin(), options.auth_response.end());
    } else {
        // One-byte length. A response above 255 bytes cannot be expressed here,
        // which is why `plugin_auth_lenenc_data` exists.
        if (options.auth_response.size() > 255) {
            return status{rpc_code::invalid_argument,
                          "mysql auth response exceeds the one-byte length form"};
        }
        body.push_back(byte_of(static_cast<std::uint8_t>(options.auth_response.size())));
        body.insert(body.end(), options.auth_response.begin(), options.auth_response.end());
    }

    if ((options.capabilities & mask(capability::connect_with_db)) != 0) {
        append_cstring(body, options.database);
    }
    if ((options.capabilities & mask(capability::plugin_auth)) != 0) {
        append_cstring(body, options.auth_plugin);
    }
    if (body.size() != needed) {
        return status{rpc_code::internal, "mysql handshake response size estimate was wrong"};
    }

    std::memcpy(out.data(), body.data(), body.size());
    written = body.size();
    return status{};
}

void encode_auth_switch_response(std::vector<std::byte>& out, span<const std::byte> auth_response,
                                 bool lenenc) {
    if (lenenc) {
        append_lenenc_int(out, auth_response.size());
    } else {
        out.push_back(byte_of(static_cast<std::uint8_t>(auth_response.size())));
    }
    out.insert(out.end(), auth_response.begin(), auth_response.end());
}

std::vector<std::byte> encode_command(std::uint8_t command, std::string_view argument) {
    std::vector<std::byte> payload;
    payload.reserve(argument.size() + 1);
    payload.push_back(byte_of(command));
    payload.insert(payload.end(), reinterpret_cast<const std::byte*>(argument.data()),
                   reinterpret_cast<const std::byte*>(argument.data()) + argument.size());
    return payload;
}

// --- Decoding ----------------------------------------------------------------

std::uint16_t read_u16(span<const std::byte> in, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(static_cast<std::uint8_t>(in[offset]) |
                                      (static_cast<std::uint16_t>(
                                           static_cast<std::uint8_t>(in[offset + 1]))
                                       << 8));
}

std::uint32_t read_u24(span<const std::byte> in, std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[offset])) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[offset + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[offset + 2])) << 16);
}

std::uint32_t read_u32(span<const std::byte> in, std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[offset])) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[offset + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[offset + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[offset + 3])) << 24);
}

status read_lenenc_int(span<const std::byte> in, std::size_t& offset, bool null_ok,
                       std::uint64_t& value, bool& is_null) noexcept {
    value = 0;
    is_null = false;
    if (!need(in, offset, 1)) {
        return status{rpc_code::protocol_error, "mysql length-encoded integer is truncated"};
    }
    const std::uint8_t first = static_cast<std::uint8_t>(in[offset]);
    ++offset;
    if (first < 0xfb) {
        value = first;
        return status{};
    }
    if (first == 0xfb) {
        if (!null_ok) {
            return status{rpc_code::protocol_error,
                          "mysql NULL marker outside a text row"};
        }
        is_null = true;
        return status{};
    }
    if (first == 0xfc) {
        if (!need(in, offset, 2)) {
            return status{rpc_code::protocol_error, "mysql length-encoded integer is truncated"};
        }
        value = read_u16(in, offset);
        offset += 2;
        return status{};
    }
    if (first == 0xfd) {
        if (!need(in, offset, 3)) {
            return status{rpc_code::protocol_error, "mysql length-encoded integer is truncated"};
        }
        value = read_u24(in, offset);
        offset += 3;
        return status{};
    }
    if (first == 0xfe) {
        if (!need(in, offset, 8)) {
            return status{rpc_code::protocol_error, "mysql length-encoded integer is truncated"};
        }
        for (int i = 0; i < 8; ++i) {
            value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(in[offset + static_cast<std::size_t>(i)]))
                     << (8 * i);
        }
        offset += 8;
        return status{};
    }
    // 0xff is a protocol error as a length prefix.
    return status{rpc_code::protocol_error, "mysql length-encoded integer has an invalid prefix"};
}

status read_lenenc_string(span<const std::byte> in, std::size_t& offset, bool null_ok,
                          std::string_view& value, bool& is_null) {
    value = {};
    std::uint64_t length = 0;
    const status read = read_lenenc_int(in, offset, null_ok, length, is_null);
    if (!read) {
        return read;
    }
    if (is_null) {
        return status{};
    }
    if (!need(in, offset, static_cast<std::size_t>(length))) {
        return status{rpc_code::protocol_error, "mysql length-encoded string is truncated"};
    }
    value = std::string_view(reinterpret_cast<const char*>(in.data() + offset),
                             static_cast<std::size_t>(length));
    offset += static_cast<std::size_t>(length);
    return status{};
}

status decode_handshake(span<const std::byte> payload, handshake& out) {
    out = handshake{};
    std::size_t offset = 0;
    if (!need(payload, offset, 1)) {
        return status{rpc_code::protocol_error, "mysql handshake is empty"};
    }
    out.protocol_version = static_cast<std::uint8_t>(payload[offset]);
    ++offset;
    if (out.protocol_version != 10) {
        return status{rpc_code::protocol_error, "unsupported mysql protocol version"};
    }
    if (!read_cstring(payload, offset, out.server_version)) {
        return status{rpc_code::protocol_error, "mysql handshake has no server version"};
    }
    if (!need(payload, offset, 4 + 8 + 1 + 2)) {
        return status{rpc_code::protocol_error, "mysql handshake is truncated"};
    }
    out.connection_id = read_u32(payload, offset);
    offset += 4;

    // auth-plugin-data-part-1 is eight bytes.
    out.auth_plugin_data.assign(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                                payload.begin() + static_cast<std::ptrdiff_t>(offset + 8));
    offset += 8;
    ++offset; // filler 0x00

    std::uint32_t capabilities = read_u16(payload, offset);
    offset += 2;
    if (!need(payload, offset, 1 + 2 + 2 + 1 + 10)) {
        return status{rpc_code::protocol_error, "mysql handshake is truncated"};
    }
    out.charset = static_cast<std::uint8_t>(payload[offset]);
    ++offset;
    out.status_flags = read_u16(payload, offset);
    offset += 2;
    capabilities |= static_cast<std::uint32_t>(read_u16(payload, offset)) << 16;
    offset += 2;
    out.capabilities = capabilities;

    const std::uint8_t auth_data_length = static_cast<std::uint8_t>(payload[offset]);
    ++offset;
    offset += 10; // reserved

    if ((capabilities & mask(capability::secure_connection)) != 0) {
        // The rest of the nonce is `auth_data_length - 8` bytes; servers send a
        // trailing NUL, so the usable length is one less than announced. Both
        // conventions exist in the wild, so read what is actually there.
        const std::size_t declared =
            auth_data_length > 8 ? static_cast<std::size_t>(auth_data_length) - 8 : 0;
        std::size_t available = 0;
        while (available < declared && need(payload, offset + available, 1) &&
               payload[offset + available] != std::byte{0}) {
            ++available;
        }
        if (available == 0 && declared != 0) {
            return status{rpc_code::protocol_error, "mysql handshake nonce is truncated"};
        }
        out.auth_plugin_data.insert(
            out.auth_plugin_data.end(),
            payload.begin() + static_cast<std::ptrdiff_t>(offset),
            payload.begin() + static_cast<std::ptrdiff_t>(offset + available));
        offset += available;
        // Skip the NUL terminator if present.
        if (need(payload, offset, 1) && payload[offset] == std::byte{0}) {
            ++offset;
        }
    }

    if ((capabilities & mask(capability::plugin_auth)) != 0 && offset < payload.size()) {
        if (!read_cstring(payload, offset, out.auth_plugin_name)) {
            return status{rpc_code::protocol_error, "mysql handshake plugin name is truncated"};
        }
    }
    if (out.auth_plugin_data.empty()) {
        return status{rpc_code::protocol_error, "mysql handshake carries no auth nonce"};
    }
    return status{};
}

packet_kind classify(span<const std::byte> payload, bool expect_column) noexcept {
    if (payload.empty()) {
        return packet_kind::unknown;
    }
    const std::uint8_t first = static_cast<std::uint8_t>(payload[0]);
    if (first == 0x00) {
        return packet_kind::ok;
    }
    if (first == 0xff) {
        return packet_kind::err;
    }
    if (first == 0xfe) {
        // A row whose first value is length 0xfe cannot occur: 0xfe introduces an
        // eight-byte length, so a genuine EOF is the short packet.
        if (payload.size() < 9) {
            return packet_kind::eof;
        }
        return packet_kind::row;
    }
    if (first == 0xfb) {
        return packet_kind::local_infile;
    }
    if (expect_column) {
        return packet_kind::column;
    }
    return packet_kind::row;
}

status decode_ok(span<const std::byte> payload, std::uint32_t capabilities, ok_packet& out) {
    out = ok_packet{};
    std::size_t offset = 0;
    if (!need(payload, offset, 1) || payload[offset] != std::byte{0}) {
        return status{rpc_code::protocol_error, "mysql OK packet has no 0x00 header"};
    }
    ++offset;
    bool is_null = false;
    status read = read_lenenc_int(payload, offset, false, out.affected_rows, is_null);
    if (!read) {
        return read;
    }
    read = read_lenenc_int(payload, offset, false, out.last_insert_id, is_null);
    if (!read) {
        return read;
    }
    if ((capabilities & mask(capability::protocol_41)) != 0) {
        if (!need(payload, offset, 4)) {
            return status{rpc_code::protocol_error, "mysql OK packet is truncated"};
        }
        out.status_flags = read_u16(payload, offset);
        out.warnings = read_u16(payload, offset + 2);
        offset += 4;
    }
    if (offset < payload.size()) {
        if ((capabilities & mask(capability::session_track)) != 0) {
            std::string_view info;
            const status info_read = read_lenenc_string(payload, offset, false, info, is_null);
            if (!info_read) {
                return info_read;
            }
            out.info.assign(info.data(), info.size());
        } else {
            out.info.assign(reinterpret_cast<const char*>(payload.data() + offset),
                            payload.size() - offset);
        }
    }
    return status{};
}

status decode_err(span<const std::byte> payload, err_packet& out) {
    out = err_packet{};
    std::size_t offset = 0;
    if (!need(payload, offset, 3) || payload[offset] != byte_of(0xff)) {
        return status{rpc_code::protocol_error, "mysql ERR packet has no 0xff header"};
    }
    ++offset;
    out.code = read_u16(payload, offset);
    offset += 2;
    if (need(payload, offset, 1) && payload[offset] == byte_of('#')) {
        if (!need(payload, offset, 6)) {
            return status{rpc_code::protocol_error, "mysql ERR packet has a truncated SQL state"};
        }
        out.sql_state.assign(reinterpret_cast<const char*>(payload.data() + offset + 1), 5);
        offset += 6;
    }
    out.message.assign(reinterpret_cast<const char*>(payload.data() + offset),
                       payload.size() - offset);
    return status{};
}

status decode_eof(span<const std::byte> payload, eof_packet& out) {
    out = eof_packet{};
    std::size_t offset = 0;
    if (!need(payload, offset, 1) || payload[offset] != byte_of(0xfe)) {
        return status{rpc_code::protocol_error, "mysql EOF packet has no 0xfe header"};
    }
    ++offset;
    if (!need(payload, offset, 4)) {
        // A short EOF is legal for very old servers; it carries no flags.
        return status{};
    }
    out.warnings = read_u16(payload, offset);
    out.status_flags = read_u16(payload, offset + 2);
    return status{};
}

status decode_column(span<const std::byte> payload, column_definition& out) {
    out = column_definition{};
    std::size_t offset = 0;
    bool is_null = false;
    std::string_view field;
    const char* const fields[] = {nullptr};
    (void)fields;

    struct target {
        std::string* text;
    };
    const target targets[] = {{&out.catalog}, {&out.schema}, {&out.table},
                              {&out.org_table}, {&out.name},   {&out.org_name}};
    for (const target& item : targets) {
        status read = read_lenenc_string(payload, offset, false, field, is_null);
        if (!read) {
            return read;
        }
        item.text->assign(field.data(), field.size());
    }

    // fixed-length fields: 0x0c, charset(2), length(4), type(1), flags(2),
    // decimals(1), filler(2)
    std::uint64_t fixed_length = 0;
    status read = read_lenenc_int(payload, offset, false, fixed_length, is_null);
    if (!read) {
        return read;
    }
    if (!need(payload, offset, 10)) {
        return status{rpc_code::protocol_error, "mysql column definition is truncated"};
    }
    out.charset = read_u16(payload, offset);
    out.length = read_u32(payload, offset + 2);
    out.type = static_cast<column_type>(static_cast<std::uint8_t>(payload[offset + 6]));
    out.flags = read_u16(payload, offset + 7);
    out.decimals = static_cast<std::uint8_t>(payload[offset + 9]);
    return status{};
}

status decode_text_row(span<const std::byte> payload, text_row& out) {
    out.values.clear();
    out.nulls.clear();
    std::size_t offset = 0;
    while (offset < payload.size()) {
        std::string_view value;
        bool is_null = false;
        const status read = read_lenenc_string(payload, offset, true, value, is_null);
        if (!read) {
            return read;
        }
        out.values.push_back(value);
        out.nulls.push_back(is_null);
    }
    return status{};
}

// --- Prepared statements -----------------------------------------------------

status decode_prepare_ok(span<const std::byte> payload, prepare_ok& out) {
    out = prepare_ok{};
    std::size_t offset = 0;
    if (!need(payload, offset, 1) || payload[offset] != std::byte{0}) {
        // A prepare failure arrives as an ERR packet; the caller classifies it. This
        // function only accepts the success shape.
        return status{rpc_code::protocol_error, "mysql prepare reply has no 0x00 header"};
    }
    ++offset;
    if (!need(payload, offset, 4 + 2 + 2 + 1 + 2)) {
        return status{rpc_code::protocol_error, "mysql prepare reply is truncated"};
    }
    out.statement_id = read_u32(payload, offset);
    offset += 4;
    out.column_count = read_u16(payload, offset);
    offset += 2;
    out.param_count = read_u16(payload, offset);
    offset += 2;
    ++offset; // filler
    out.warnings = read_u16(payload, offset);
    if (out.statement_id == 0) {
        return status{rpc_code::protocol_error, "mysql prepare returned statement id 0"};
    }
    return status{};
}

status encode_stmt_execute(std::vector<std::byte>& out, std::uint32_t statement_id,
                           span<const parameter> params) {
    // Validate everything before writing a byte: a rejected call must leave `out`
    // empty, or a caller that ignores the status would send a half-built command.
    if (statement_id == 0) {
        return status{rpc_code::invalid_argument, "mysql statement id 0 is not valid"};
    }
    // A parameter that only receives a value has no bytes on the wire. MySQL binds
    // OUT/INOUT through session variables referenced in the SQL text, so the caller
    // rewrites those placeholders and must not include them here. Rejecting it keeps
    // a mismatch between placeholder count and parameter count from becoming a
    // silent value shift.
    for (const parameter& param : params) {
        if (!param.sends_value()) {
            return status{rpc_code::invalid_argument,
                          "mysql execute cannot bind an output-only parameter; rewrite it to a "
                          "session variable"};
        }
    }

    out.clear();
    out.reserve(16 + params.size() * 12);
    out.push_back(byte_of(com_stmt_execute));
    append_u32(out, statement_id);
    out.push_back(byte_of(0)); // flags: CURSOR_TYPE_NO_CURSOR
    append_u32(out, 1);        // iteration count, always 1

    if (params.empty()) {
        return status{};
    }

    // The null bitmap covers parameters (not columns) and starts at bit 0.
    const std::size_t bitmap_bytes = (params.size() + 7) / 8;
    const std::size_t bitmap_offset = out.size();
    out.insert(out.end(), bitmap_bytes, std::byte{0});
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (params[i].is_null) {
            out[bitmap_offset + i / 8] |= static_cast<std::byte>(1u << (i % 8));
        }
    }

    // New-parameters-bound flag: always 1, so the type array below is authoritative.
    out.push_back(byte_of(1));
    for (const parameter& param : params) {
        if (param.is_null) {
            // A null parameter still needs a type entry; the server skips its value.
            append_u16(out, static_cast<std::uint16_t>(column_type::null));
            continue;
        }
        std::uint16_t wire_type = 0;
        bool is_unsigned = false;
        switch (param.type) {
        case db_type::boolean:
        case db_type::int64:
            wire_type = static_cast<std::uint16_t>(column_type::longlong);
            break;
        case db_type::uint64:
            wire_type = static_cast<std::uint16_t>(column_type::longlong);
            is_unsigned = true;
            break;
        case db_type::real:
            wire_type = static_cast<std::uint16_t>(column_type::double_);
            break;
        case db_type::blob:
            wire_type = static_cast<std::uint16_t>(column_type::blob);
            break;
        case db_type::text:
        case db_type::decimal:
        case db_type::date:
        case db_type::time:
        case db_type::timestamp:
        case db_type::uuid:
        case db_type::json:
        case db_type::xml:
        case db_type::unknown:
            wire_type = static_cast<std::uint16_t>(column_type::var_string);
            break;
        }
        if (is_unsigned) {
            wire_type |= 0x8000u;
        }
        append_u16(out, wire_type);
    }

    for (const parameter& param : params) {
        if (param.is_null) {
            continue;
        }
        switch (param.type) {
        case db_type::boolean:
        case db_type::int64: {
            const auto value = static_cast<std::uint64_t>(param.int_value);
            for (int i = 0; i < 8; ++i) {
                out.push_back(byte_of(static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu)));
            }
            break;
        }
        case db_type::uint64: {
            for (int i = 0; i < 8; ++i) {
                out.push_back(
                    byte_of(static_cast<std::uint8_t>((param.uint_value >> (8 * i)) & 0xffu)));
            }
            break;
        }
        case db_type::real: {
            // The wire form is the IEEE-754 bit pattern, little-endian.
            std::uint64_t bits = 0;
            static_assert(sizeof(bits) == sizeof(param.real_value), "double is 8 bytes");
            std::memcpy(&bits, &param.real_value, sizeof(bits));
            for (int i = 0; i < 8; ++i) {
                out.push_back(byte_of(static_cast<std::uint8_t>((bits >> (8 * i)) & 0xffu)));
            }
            break;
        }
        case db_type::blob:
            append_lenenc_string(
                out, std::string_view(reinterpret_cast<const char*>(param.blob.data()),
                                      param.blob.size()));
            break;
        default:
            append_lenenc_string(out, param.text);
            break;
        }
    }
    return status{};
}

std::vector<std::byte> encode_stmt_close(std::uint32_t statement_id) {
    std::vector<std::byte> out;
    out.reserve(5);
    out.push_back(byte_of(com_stmt_close));
    append_u32(out, statement_id);
    return out;
}

namespace {

// Reads a little-endian signed integer of `bytes` width.
[[nodiscard]] std::int64_t read_signed(span<const std::byte> in, std::size_t offset,
                                       std::size_t bytes) noexcept {
    std::uint64_t raw = 0;
    for (std::size_t i = 0; i < bytes; ++i) {
        raw |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(in[offset + i]))
               << (8 * i);
    }
    // Sign-extend from the field width.
    const std::size_t bits = bytes * 8;
    if (bits < 64 && (raw & (1ull << (bits - 1))) != 0) {
        raw |= ~0ull << bits;
    }
    return static_cast<std::int64_t>(raw);
}

[[nodiscard]] std::string two_digit(unsigned value) {
    std::string text = std::to_string(value);
    if (text.size() < 2) {
        text.insert(text.begin(), '0');
    }
    return text;
}

} // namespace

status decode_binary_row(span<const std::byte> payload, span<const column_type> types,
                         binary_row& out) {
    out.values.clear();
    out.nulls.clear();
    std::size_t offset = 0;
    if (!need(payload, offset, 1) || payload[offset] != std::byte{0}) {
        return status{rpc_code::protocol_error, "mysql binary row has no 0x00 header"};
    }
    ++offset;

    // The null bitmap has two reserved bits at the front, so bit i of the row maps
    // to bit (i + 2) of the bitmap. Getting this off by two shifts every null.
const std::size_t bitmap_bytes = (types.size() + 7 + 2) / 8;
    if (!need(payload, offset, bitmap_bytes)) {
        return status{rpc_code::protocol_error, "mysql binary row null bitmap is truncated"};
    }
    // The bitmap start must be captured before `offset` advances: a lambda that
    // captured `offset` by reference would read the first value byte instead of the
    // bitmap and report the wrong columns as null.
    const std::size_t bitmap_offset = offset;
    const auto is_null = [&](std::size_t index) {
        const std::size_t bit = index + 2;
        const std::size_t byte = bit / 8;
        const unsigned shift = static_cast<unsigned>(bit % 8);
        return (static_cast<std::uint8_t>(payload[bitmap_offset + byte]) & (1u << shift)) != 0;
    };
    offset += bitmap_bytes;

    out.values.reserve(types.size());
    out.nulls.reserve(types.size());
    for (std::size_t index = 0; index < types.size(); ++index) {
        if (is_null(index)) {
            out.values.emplace_back();
            out.nulls.push_back(true);
            continue;
        }
        out.nulls.push_back(false);
        char buffer[64];
        switch (types[index]) {
        case column_type::tiny:
            if (!need(payload, offset, 1)) {
                return status{rpc_code::protocol_error, "mysql binary row is truncated"};
            }
            out.values.push_back(std::to_string(read_signed(payload, offset, 1)));
            offset += 1;
            break;
        case column_type::short_:
        case column_type::year:
            if (!need(payload, offset, 2)) {
                return status{rpc_code::protocol_error, "mysql binary row is truncated"};
            }
            out.values.push_back(std::to_string(read_signed(payload, offset, 2)));
            offset += 2;
            break;
        case column_type::int24:
        case column_type::long_:
            if (!need(payload, offset, 4)) {
                return status{rpc_code::protocol_error, "mysql binary row is truncated"};
            }
            out.values.push_back(std::to_string(read_signed(payload, offset, 4)));
            offset += 4;
            break;
        case column_type::longlong:
            if (!need(payload, offset, 8)) {
                return status{rpc_code::protocol_error, "mysql binary row is truncated"};
            }
            out.values.push_back(std::to_string(read_signed(payload, offset, 8)));
            offset += 8;
            break;
        case column_type::float_: {
            if (!need(payload, offset, 4)) {
                return status{rpc_code::protocol_error, "mysql binary row is truncated"};
            }
            std::uint32_t bits = 0;
            for (int i = 0; i < 4; ++i) {
                bits |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(
                            payload[offset + static_cast<std::size_t>(i)]))
                        << (8 * i);
            }
            float value = 0.0f;
            std::memcpy(&value, &bits, sizeof(value));
            std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
            out.values.emplace_back(buffer);
            offset += 4;
            break;
        }
        case column_type::double_: {
            if (!need(payload, offset, 8)) {
                return status{rpc_code::protocol_error, "mysql binary row is truncated"};
            }
            std::uint64_t bits = 0;
            for (int i = 0; i < 8; ++i) {
                bits |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(
                            payload[offset + static_cast<std::size_t>(i)]))
                        << (8 * i);
            }
            double value = 0.0;
            std::memcpy(&value, &bits, sizeof(value));
            std::snprintf(buffer, sizeof(buffer), "%.17g", value);
            out.values.emplace_back(buffer);
            offset += 8;
            break;
        }
        case column_type::date:
        case column_type::newdate:
        case column_type::datetime:
        case column_type::timestamp: {
            if (!need(payload, offset, 1)) {
                return status{rpc_code::protocol_error, "mysql binary row is truncated"};
            }
            const std::uint8_t length = static_cast<std::uint8_t>(payload[offset]);
            ++offset;
            if (length == 0) {
                // The zero date, which MySQL permits. It is text, not NULL.
                out.values.emplace_back("0000-00-00");
                break;
            }
            if (length != 4 && length != 7 && length != 11) {
                return status{rpc_code::protocol_error, "mysql temporal length is invalid"};
            }
            if (!need(payload, offset, length)) {
                return status{rpc_code::protocol_error, "mysql binary row is truncated"};
            }
            const unsigned year = read_u16(payload, offset);
            const unsigned month = static_cast<std::uint8_t>(payload[offset + 2]);
            const unsigned day = static_cast<std::uint8_t>(payload[offset + 3]);
// The year is a 2-byte value, so it prints as four digits; month and day as two.
            std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02u", year, month, day);
            std::string text(buffer);
            if (length >= 7) {
                const unsigned hour = static_cast<std::uint8_t>(payload[offset + 4]);
                const unsigned minute = static_cast<std::uint8_t>(payload[offset + 5]);
                const unsigned second = static_cast<std::uint8_t>(payload[offset + 6]);
                text += " " + two_digit(hour) + ":" + two_digit(minute) + ":" +
                        two_digit(second);
                if (length == 11) {
                    std::uint32_t micro = 0;
                    for (int i = 0; i < 4; ++i) {
                        micro |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(
                                     payload[offset + 7 + static_cast<std::size_t>(i)]))
                                 << (8 * i);
                    }
                    std::snprintf(buffer, sizeof(buffer), ".%06u", micro);
                    text += buffer;
                }
            }
            out.values.push_back(std::move(text));
            offset += length;
            break;
        }
        case column_type::time: {
            if (!need(payload, offset, 1)) {
                return status{rpc_code::protocol_error, "mysql binary row is truncated"};
            }
            const std::uint8_t length = static_cast<std::uint8_t>(payload[offset]);
            ++offset;
            if (length == 0) {
                out.values.emplace_back("00:00:00");
                break;
            }
            if (length != 8 && length != 12) {
                return status{rpc_code::protocol_error, "mysql time length is invalid"};
            }
            if (!need(payload, offset, length)) {
                return status{rpc_code::protocol_error, "mysql binary row is truncated"};
            }
            const bool negative = payload[offset] != std::byte{0};
            std::uint32_t days = 0;
            for (int i = 0; i < 4; ++i) {
                days |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(
                            payload[offset + 1 + static_cast<std::size_t>(i)]))
                        << (8 * i);
            }
            const unsigned hour = static_cast<std::uint8_t>(payload[offset + 5]);
            const unsigned minute = static_cast<std::uint8_t>(payload[offset + 6]);
            const unsigned second = static_cast<std::uint8_t>(payload[offset + 7]);
            std::string text;
            if (negative) {
                text.push_back('-');
            }
            // MySQL reports a duration as days + h:m:s; a TIME can exceed 24 hours.
            std::snprintf(buffer, sizeof(buffer), "%u:", days * 24u + hour);
            text += buffer;
            text += two_digit(minute) + ":" + two_digit(second);
            if (length == 12) {
                std::uint32_t micro = 0;
                for (int i = 0; i < 4; ++i) {
                    micro |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(
                                 payload[offset + 8 + static_cast<std::size_t>(i)]))
                             << (8 * i);
                }
                std::snprintf(buffer, sizeof(buffer), ".%06u", micro);
                text += buffer;
            }
            out.values.push_back(std::move(text));
            offset += length;
            break;
        }
        default: {
            // Everything else on the binary protocol is a length-encoded byte
            // string: decimal, varchar, blob, json, geometry, bit.
            std::string_view text;
            bool ignored_null = false;
            const status read = read_lenenc_string(payload, offset, false, text, ignored_null);
            if (!read) {
                return read;
            }
            out.values.emplace_back(text.data(), text.size());
            break;
        }
        }
    }
    return status{};
}

column_info to_column_info(const column_definition& definition) {
    column_info info;
    // `name` is what the query produced (possibly an alias); `original_name` is the
    // column as declared. An ORM must map on the original name, or an alias silently
    // changes which field binds.
    info.name = definition.name;
    info.original_name = definition.org_name;
    info.schema = definition.schema;
    info.table = definition.org_table.empty() ? definition.table : definition.org_table;
    info.type = map_column_type(definition.type, definition.flags);
    info.precision = definition.length;
    info.scale = definition.decimals;
    info.nullable =
        (definition.flags & static_cast<std::uint16_t>(column_flag::not_null)) == 0;
    info.flags = definition.flags;
    return info;
}

rpc_code map_error_code(std::uint16_t code) noexcept {
    switch (code) {
    // Duplicate entry, constraint failures: the request conflicts with existing
    // state, which may be temporary, so it is a precondition failure rather than a
    // malformed request.
    case 1022: // ER_DUP_KEY
    case 1048: // ER_BAD_NULL_ERROR
    case 1062: // ER_DUP_ENTRY
    case 1216: // ER_NO_REFERENCED_ROW
    case 1217: // ER_ROW_IS_REFERENCED
    case 1451: // ER_ROW_IS_REFERENCED_2
    case 1452: // ER_NO_REFERENCED_ROW_2
    case 3819: // ER_CHECK_CONSTRAINT_VIOLATED
        return rpc_code::failed_precondition;

    // Missing object or schema.
    case 1049: // ER_BAD_DB_ERROR
    case 1146: // ER_NO_SUCH_TABLE
    case 1051: // ER_BAD_TABLE_ERROR
    case 1054: // ER_BAD_FIELD_ERROR
    case 1305: // ER_SP_DOES_NOT_EXIST
        return rpc_code::not_found;

    // Deadlock and lock wait: the call did not complete but may succeed on retry.
    case 1205: // ER_LOCK_WAIT_TIMEOUT
    case 1213: // ER_LOCK_DEADLOCK
        return rpc_code::unavailable;

    // Authentication and permission: the request is well formed but not allowed,
    // and retrying the same request cannot help.
    case 1044: // ER_DBACCESS_DENIED_ERROR
    case 1045: // ER_ACCESS_DENIED_ERROR
    case 1142: // ER_TABLEACCESS_DENIED_ERROR
    case 1143: // ER_COLUMNACCESS_DENIED_ERROR
        return rpc_code::failed_precondition;

    // Out of resources on the server.
    case 1040: // ER_CON_COUNT_ERROR
    case 1203: // ER_TOO_MANY_USER_CONNECTIONS
    case 1226: // ER_USER_LIMIT_REACHED
        return rpc_code::resource_exhausted;

    // The server is shutting down.
    case 1053: // ER_SERVER_SHUTDOWN
        return rpc_code::unavailable;

    // Syntax and semantic errors are the caller's bug.
    case 1064: // ER_PARSE_ERROR
    case 1052: // ER_NON_UNIQ_ERROR
    case 1055: // ER_WRONG_GROUP_FIELD
    case 1056: // ER_WRONG_VALUE_COUNT
    case 1059: // ER_TOO_LONG_IDENT
    case 1136: // ER_WRONG_VALUE_COUNT_ON_ROW
        return rpc_code::invalid_argument;

    // A query that ran too long was killed.
    case 1317: // ER_QUERY_INTERRUPTED
    case 3024: // ER_QUERY_TIMEOUT
        return rpc_code::deadline_exceeded;

    default:
        // Everything else keeps the server's own code in the message; the abstract
        // code says only that the server rejected the call.
        return rpc_code::application;
    }
}

db_type map_column_type(column_type type, std::uint16_t flags) noexcept {
    const bool is_unsigned = (flags & static_cast<std::uint16_t>(column_flag::unsigned_)) != 0;
    switch (type) {
    case column_type::tiny:
    case column_type::short_:
    case column_type::long_:
    case column_type::int24:
    case column_type::year:
        return is_unsigned ? db_type::uint64 : db_type::int64;
    case column_type::longlong:
        return is_unsigned ? db_type::uint64 : db_type::int64;
    case column_type::float_:
    case column_type::double_:
        return db_type::real;
    case column_type::decimal:
    case column_type::newdecimal:
        // A DECIMAL is carried as text: converting it to a binary float would lose
        // digits the server took care to preserve.
        return db_type::decimal;
    case column_type::date:
    case column_type::newdate:
        return db_type::date;
    case column_type::time:
        return db_type::time;
    case column_type::timestamp:
    case column_type::datetime:
        return db_type::timestamp;
    case column_type::json:
        return db_type::json;
    case column_type::bit:
        // A BIT(n) is a bit string, not a number.
        return db_type::blob;
    case column_type::blob:
    case column_type::tiny_blob:
    case column_type::medium_blob:
    case column_type::long_blob:
    case column_type::geometry:
        // The server reports BLOB for any binary payload; a column with the binary
        // charset flag is bytes, one without is text.
        return (flags & static_cast<std::uint16_t>(column_flag::binary)) != 0 ? db_type::blob
                                                                             : db_type::text;
    case column_type::varchar:
    case column_type::var_string:
    case column_type::string_:
    case column_type::enum_:
    case column_type::set:
        return db_type::text;
    case column_type::null:
        return db_type::unknown;
    }
    return db_type::unknown;
}

} // namespace mysql
} // namespace db
} // namespace crpc
