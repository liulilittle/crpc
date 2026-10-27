// CRPC -- MySQL/MariaDB wire protocol codec.
//
// Framing and the packets a server sends, plus the two packets a client sends
// during the handshake. Authentication *math* (SHA1/SHA256 scrambling) is not
// here: `encode_handshake_response` takes the auth response bytes the caller
// already computed, so this file stays a pure codec and the crypto can be added
// and tested on its own.
//
// Everything is length-prefixed and little-endian. Two rules matter:
//
//   * a packet is 3-byte length + 1-byte sequence id, and a payload of 2^24-1
//     bytes is *continued* by the next packet. A reader that treats the length as
//     a complete message silently truncates a large value;
//   * strings use a length-encoded integer, whose 0xfb byte means SQL NULL in a
//     row but is a protocol error elsewhere, so the decoder always states which
//     context it is in.
#ifndef CRPC_DB_MYSQL_PROTOCOL_HPP
#define CRPC_DB_MYSQL_PROTOCOL_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/db/types.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace db {
namespace mysql {

// Capability flags. Only the ones this codec interprets are named; the rest are
// passed through.
enum class capability : std::uint32_t {
    long_password = 0x00000001,
    found_rows = 0x00000002,
    long_flag = 0x00000004,
    connect_with_db = 0x00000008,
    protocol_41 = 0x00000200,
    ssl = 0x00000800,
    transactions = 0x00002000,
    secure_connection = 0x00008000,
    multi_statements = 0x00010000,
    multi_results = 0x00020000,
    ps_multi_results = 0x00040000,
    plugin_auth = 0x00080000,
    connect_attrs = 0x00100000,
    plugin_auth_lenenc_data = 0x00200000,
    session_track = 0x00800000,
    deprecate_eof = 0x01000000
};

[[nodiscard]] constexpr std::uint32_t mask(capability flag) noexcept {
    return static_cast<std::uint32_t>(flag);
}

// Server status flags carried by OK and EOF packets.
enum class server_status : std::uint16_t {
    in_transaction = 0x0001,
    autocommit = 0x0002,
    more_results = 0x0008,
    no_good_index_used = 0x0010,
    no_index_used = 0x0020,
    cursor_exists = 0x0040,
    last_row_sent = 0x0080,
    session_state_changed = 0x4000
};

// Column types. The numeric values are the wire values.
enum class column_type : std::uint8_t {
    decimal = 0,
    tiny = 1,
    short_ = 2,
    long_ = 3,
    float_ = 4,
    double_ = 5,
    null = 6,
    timestamp = 7,
    longlong = 8,
    int24 = 9,
    date = 10,
    time = 11,
    datetime = 12,
    year = 13,
    newdate = 14,
    varchar = 15,
    bit = 16,
    json = 245,
    newdecimal = 246,
    enum_ = 247,
    set = 248,
    tiny_blob = 249,
    medium_blob = 250,
    long_blob = 251,
    blob = 252,
    var_string = 253,
    string_ = 254,
    geometry = 255
};

// Column flags (a subset).
enum class column_flag : std::uint16_t {
    not_null = 0x0001,
    primary_key = 0x0002,
    unique_key = 0x0004,
    multiple_key = 0x0008,
    blob = 0x0010,
    unsigned_ = 0x0020,
    zerofill = 0x0040,
    binary = 0x0080,
    enum_ = 0x0100,
    auto_increment = 0x0200,
    timestamp = 0x0400,
    set = 0x0800
};

// Largest payload one packet can carry; a longer message is split.
inline constexpr std::size_t max_packet_payload = 0xFFFFFF;

// Default ceiling for a reassembled message.
inline constexpr std::size_t default_max_message_size = 64u * 1024u * 1024u;

// --- Encoding ----------------------------------------------------------------

CRPC_API void append_lenenc_int(std::vector<std::byte>& out, std::uint64_t value);
CRPC_API void append_lenenc_string(std::vector<std::byte>& out, std::string_view text);
CRPC_API void append_null(std::vector<std::byte>& out);

// Wraps one message in as many packets as its length requires, starting at
// `sequence` and updating it. A message that is an exact multiple of the maximum
// payload ends with an empty packet, which is what tells the server the message
// is complete.
CRPC_API status write_message(std::vector<std::byte>& out, span<const std::byte> payload,
                              std::uint8_t& sequence);

// Encodes a handshake response for protocol 4.1.
struct handshake_response_options {
    std::uint32_t capabilities = 0;
    std::uint32_t max_packet_size = 16u * 1024u * 1024u;
    std::uint8_t charset = 45; // utf8mb4_general_ci
    std::string_view username;
    // The auth response bytes (for `caching_sha2_password` this is the 32-byte
    // scramble; for `mysql_native_password`, 20 bytes). The length-encoded form is
    // selected by `plugin_auth_lenenc_data`.
    span<const std::byte> auth_response;
    // Only emitted when `connect_with_db` is set.
    std::string_view database;
    // Only emitted when `plugin_auth` is set. It must echo the plugin the server
    // offered, because a server rejects a response that names a different one.
    std::string_view auth_plugin;
};

CRPC_API status encode_handshake_response(span<std::byte> out,
                                          const handshake_response_options& options,
                                          std::size_t& written);

[[nodiscard]] CRPC_API std::size_t handshake_response_size(
    const handshake_response_options& options) noexcept;

// Encodes the response to an auth-switch request: just the auth response bytes,
// length-prefixed.
CRPC_API void encode_auth_switch_response(std::vector<std::byte>& out,
                                          span<const std::byte> auth_response, bool lenenc);

// Encodes a command payload: the command byte followed by its argument.
[[nodiscard]] CRPC_API std::vector<std::byte> encode_command(std::uint8_t command,
                                                            std::string_view argument);

// --- Decoding ----------------------------------------------------------------

// Reads a length-encoded integer. `null_ok` selects the row context, where 0xfb
// means SQL NULL; outside a row it is a protocol error.
[[nodiscard]] CRPC_API status read_lenenc_int(span<const std::byte> in, std::size_t& offset,
                                              bool null_ok, std::uint64_t& value,
                                              bool& is_null) noexcept;

[[nodiscard]] CRPC_API status read_lenenc_string(span<const std::byte> in, std::size_t& offset,
                                                 bool null_ok, std::string_view& value,
                                                 bool& is_null);

[[nodiscard]] CRPC_API std::uint16_t read_u16(span<const std::byte> in,
                                              std::size_t offset) noexcept;
[[nodiscard]] CRPC_API std::uint32_t read_u24(span<const std::byte> in,
                                              std::size_t offset) noexcept;
[[nodiscard]] CRPC_API std::uint32_t read_u32(span<const std::byte> in,
                                              std::size_t offset) noexcept;

// The initial handshake (protocol version 10).
struct handshake {
    std::uint8_t protocol_version = 0;
    std::string server_version;
    std::uint32_t connection_id = 0;
    std::vector<std::byte> auth_plugin_data;
    std::uint32_t capabilities = 0;
    std::uint8_t charset = 0;
    std::uint16_t status_flags = 0;
    std::string auth_plugin_name;
};

[[nodiscard]] CRPC_API status decode_handshake(span<const std::byte> payload,
                                               handshake& out);

struct ok_packet {
    std::uint64_t affected_rows = 0;
    std::uint64_t last_insert_id = 0;
    std::uint16_t status_flags = 0;
    std::uint16_t warnings = 0;
    std::string info;
};

struct err_packet {
    std::uint16_t code = 0;
    std::string sql_state;
    std::string message;
};

struct eof_packet {
    std::uint16_t warnings = 0;
    std::uint16_t status_flags = 0;
};

struct column_definition {
    std::string catalog;
    std::string schema;
    std::string table;
    std::string org_table;
    std::string name;
    std::string org_name;
    std::uint16_t charset = 0;
    std::uint32_t length = 0;
    column_type type = column_type::var_string;
    std::uint16_t flags = 0;
    std::uint8_t decimals = 0;
};

// The kind of packet a payload is. OK and EOF are distinguished by their first
// byte *and* their length: an EOF packet is shorter than 9 bytes, which is how a
// row whose first column starts with 0xFE is not mistaken for one.
enum class packet_kind : std::uint8_t { ok, err, eof, local_infile, column, row, unknown };

[[nodiscard]] CRPC_API packet_kind classify(span<const std::byte> payload,
                                           bool expect_column) noexcept;

[[nodiscard]] CRPC_API status decode_ok(span<const std::byte> payload, std::uint32_t capabilities,
                                        ok_packet& out);
[[nodiscard]] CRPC_API status decode_err(span<const std::byte> payload, err_packet& out);
[[nodiscard]] CRPC_API status decode_eof(span<const std::byte> payload, eof_packet& out);
[[nodiscard]] CRPC_API status decode_column(span<const std::byte> payload,
                                            column_definition& out);

// One text-protocol row. Values are views into the packet payload, so the caller
// consumes the row before reading the next packet.
struct text_row {
    std::vector<std::string_view> values;
    std::vector<bool> nulls;
};

[[nodiscard]] CRPC_API status decode_text_row(span<const std::byte> payload, text_row& out);

// --- Prepared statements -----------------------------------------------------

// Command bytes for the prepared-statement protocol.
inline constexpr std::uint8_t com_stmt_prepare = 0x16;
inline constexpr std::uint8_t com_stmt_execute = 0x17;
inline constexpr std::uint8_t com_stmt_close = 0x19;
inline constexpr std::uint8_t com_stmt_reset = 0x1a;

// The reply to COM_STMT_PREPARE. It is followed by `param_count` parameter
// definitions (and an EOF) and `column_count` column definitions (and an EOF), so a
// caller that ignores those counts leaves the stream desynchronised.
struct prepare_ok {
    std::uint32_t statement_id = 0;
    std::uint16_t column_count = 0;
    std::uint16_t param_count = 0;
    std::uint16_t warnings = 0;
};

[[nodiscard]] CRPC_API status decode_prepare_ok(span<const std::byte> payload,
                                               prepare_ok& out);

// Encodes COM_STMT_EXECUTE for `params`.
//
// The type array is always sent (the "new parameters bound" flag is 1): a prepared
// statement is often executed more than once with different types, and relying on
// the server's cached types turns a type change into a silent misread of the bytes.
[[nodiscard]] CRPC_API status encode_stmt_execute(std::vector<std::byte>& out,
                                                  std::uint32_t statement_id,
                                                  span<const parameter> params);

// COM_STMT_CLOSE takes the id and nothing else; the server sends no reply.
[[nodiscard]] CRPC_API std::vector<std::byte> encode_stmt_close(std::uint32_t statement_id);

// One binary-protocol row, already converted to the text form the cursor
// accessors parse. A binary row carries integers as fixed-width little-endian
// fields and temporals as length-prefixed tuples, so it cannot share the text
// decoder; converting here keeps one result-set implementation for both protocols.
struct binary_row {
    std::vector<std::string> values;
    std::vector<bool> nulls;
};

// Decodes one binary row. `types` is the column types from the result-set
// definitions, in column order.
[[nodiscard]] CRPC_API status decode_binary_row(span<const std::byte> payload,
                                               span<const column_type> types,
                                               binary_row& out);

// Maps a wire column definition to the abstract metadata a caller (and an ORM) uses
// to bind a struct field to a column: alias, original name, owning schema/table,
// abstract type, precision/scale, nullability and the raw provider flags.
[[nodiscard]] CRPC_API column_info to_column_info(const column_definition& definition);

// Maps a wire error code to the library code. The server's own code is never lost:
// it stays in `err_packet::code`, and this only classifies it.
[[nodiscard]] CRPC_API rpc_code map_error_code(std::uint16_t code) noexcept;

[[nodiscard]] CRPC_API db_type map_column_type(column_type type, std::uint16_t flags) noexcept;

} // namespace mysql
} // namespace db
} // namespace crpc

#endif // CRPC_DB_MYSQL_PROTOCOL_HPP