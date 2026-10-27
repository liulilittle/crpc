// CRPC -- TDS (SQL Server) wire codec, login phase.
//
// SQL Server speaks TDS: an 8-byte header in front of every packet, and a stream of
// "tokens" inside the payload once the session is up. This header covers the login
// phase — framing, PRELOGIN, LOGIN7 and the tokens a server sends back while
// authenticating. Query results (COLMETADATA/ROW) are a separate slice.
//
// Three details cost a day each if guessed:
//
//   * every length in the header is **big-endian** except the ones that are not;
//     TDS mixes conventions by field, so each is named explicitly here;
//   * a LOGIN7 packet carries the password obfuscated by nibble-swap and XOR 0xA5.
//     Sending it in the clear is accepted by nothing and looks like an auth failure;
//   * every string in the login phase is UCS-2 (UTF-16LE) with a character count,
//     not a byte count, so a length in bytes is wrong by a factor of two.
#ifndef CRPC_DB_TDS_PROTOCOL_HPP
#define CRPC_DB_TDS_PROTOCOL_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace db {
namespace tds {

// --- Framing -----------------------------------------------------------------

// Packet types this codec produces or consumes.
enum class packet_type : std::uint8_t {
    sql_batch = 0x01,
    rpc = 0x03,
    tabular_result = 0x04,
    attention = 0x06,
    bulk_load = 0x07,
    transaction_manager = 0x0e,
    login7 = 0x10,
    prelogin = 0x12,
    sspi = 0x11
};

// Status bits. `eom` marks the last packet of a message; a message split across
// packets is reassembled until it is set.
enum class packet_status : std::uint8_t {
    normal = 0x00,
    eom = 0x01,
    ignore = 0x02,
    reset_connection = 0x08,
    reset_connection_skip_trans = 0x10
};

struct packet_header {
    packet_type type = packet_type::prelogin;
    // Raw status byte; `is_eom()` is the question callers actually ask.
    std::uint8_t status = 0;
    // Total length of header plus payload, in bytes, big-endian.
    std::uint16_t length = 8;
    std::uint16_t spid = 0;
    std::uint8_t packet_id = 0;
    std::uint8_t window = 0;

    [[nodiscard]] bool is_eom() const noexcept {
        return (status & static_cast<std::uint8_t>(packet_status::eom)) != 0;
    }
};

inline constexpr std::size_t header_size = 8;

// Default packet size a client asks for. SQL Server's minimum is 512 and its
// maximum is 32767; asking for more is silently clamped, so this stays inside.
inline constexpr std::uint16_t default_packet_size = 4096;

// Upper bound on one reassembled message. The wire length field is 16 bits per
// packet, so a message longer than the packet size arrives as several packets; this
// is the point where the client refuses to keep buffering.
inline constexpr std::size_t default_max_message_size = 64u * 1024u * 1024u;

[[nodiscard]] CRPC_API status encode_header(span<std::byte> out, const packet_header& header,
                                            std::size_t& written) noexcept;
[[nodiscard]] CRPC_API status decode_header(span<const std::byte> in,
                                            packet_header& header) noexcept;

// --- PRELOGIN ----------------------------------------------------------------

enum class prelogin_option : std::uint8_t {
    version = 0x00,
    encryption = 0x01,
    instance = 0x02,
    thread_id = 0x03,
    mars = 0x04,
    trace_id = 0x05,
    fedauth_required = 0x06,
    nonce = 0x07,
    terminator = 0xff
};

// What the client wants from TLS.
enum class encryption_mode : std::uint8_t {
    off = 0x00,
    on = 0x01,
    not_supported = 0x02,
    required = 0x03
};

struct prelogin {
    // Server version the client claims to speak. Encoded as four bytes.
    std::uint8_t version_major = 0x0f;
    std::uint8_t version_minor = 0x00;
    std::uint8_t version_build_high = 0x00;
    std::uint8_t version_build_low = 0x00;

    encryption_mode encryption = encryption_mode::not_supported;

    // Thread id, sent as four bytes.
    std::uint32_t thread_id = 0;

    // MARS: 0 = off. A connection with MARS off is the simple case and the one this
    // codec supports; turning it on changes the session semantics.
    std::uint8_t mars = 0;
};

[[nodiscard]] CRPC_API status encode_prelogin(std::vector<std::byte>& out,
                                              const prelogin& value);
[[nodiscard]] CRPC_API status decode_prelogin(span<const std::byte> payload, prelogin& out);

// --- LOGIN7 ------------------------------------------------------------------

// The option flags a LOGIN7 header carries. Only the ones this client sets are
// named; the rest stay zero.
enum class login_option : std::uint32_t {
    use_db_notify = 0x00000001,
    set_language_on = 0x00000002,
    init_db_fatal = 0x00000004,
    language_fatal = 0x00000010,
    set_language_warn = 0x00000020,
    integrated_security = 0x00000080
};

struct login7 {
    // TDS version the client speaks. 0x74000004 is SQL Server 2012 and later;
    // sending an older one changes the result format the server chooses.
    std::uint32_t tds_version = 0x74000004u;
    std::uint32_t packet_size = default_packet_size;
    // Client program version, informational.
    std::uint32_t client_version = 0x00000000u;
    std::uint32_t client_pid = 0;
    std::uint32_t connection_id = 0;
    std::uint32_t option_flags = 0;

    std::string host_name;
    std::string user_name;
    // Plain password. The encoder obfuscates it; the caller never does.
    std::string password;
    std::string application_name;
    std::string server_name;
    // 0 means "use the driver default", which is what a login packet 0-length
    // library name means.
    std::string client_interface_name;
    std::string language;
    std::string database;
    std::vector<std::uint8_t> client_id; // 6 bytes: MAC address
    std::string attach_db_file;
    std::string change_password;
    std::uint32_t sspi_length = 0;
};

// Obfuscates a password for LOGIN7: swap the nibbles of each byte, then XOR 0xA5.
// Exposed because it is the single most common reason a hand-written login fails.
[[nodiscard]] CRPC_API std::vector<std::uint8_t> obfuscate_password(std::string_view password);

[[nodiscard]] CRPC_API status encode_login7(std::vector<std::byte>& out, const login7& value);

// Encodes the UTF-16LE form TDS uses for every string in the login phase.
[[nodiscard]] CRPC_API std::vector<std::byte> utf16le(std::string_view text);
// Decodes UTF-16LE; a lone surrogate becomes U+FFFD rather than invalid UTF-8.
[[nodiscard]] CRPC_API std::string from_utf16le(span<const std::byte> data);

// --- Tokens ------------------------------------------------------------------

enum class token_type : std::uint8_t {
    colmetadata = 0x81,
    row = 0xd1,
    nbc_row = 0xd2,
    error = 0xaa,
    info = 0xab,
    loginack = 0xad,
    envchange = 0xe3,
    done = 0xfd,
    done_proc = 0xfe,
    done_in_proc = 0xff,
    return_status = 0x79,
    return_value = 0xac
};

struct error_token {
    std::uint32_t number = 0;
    std::uint8_t state = 0;
    std::uint8_t severity = 0;
    std::string message;
    std::string server_name;
    std::string procedure_name;
    std::uint32_t line = 0;
};

struct login_ack {
    std::uint8_t interface = 0;
    // The server's TDS version, e.g. 0x74000004.
    std::uint32_t tds_version = 0;
    std::string program_name;
    // 4- or 5-byte server version.
    std::vector<std::uint8_t> server_version;
};

// The few ENVCHANGE types that matter for a session: the database actually
// selected, the packet size the server settled on, and the transaction descriptor.
enum class env_change_type : std::uint8_t {
    database = 1,
    language = 2,
    charset = 3,
    packet_size = 4,
    begin_transaction = 8,
    commit_transaction = 9,
    rollback_transaction = 10
};

struct env_change {
    env_change_type type = env_change_type::database;
    // Raw payload: text values are UCS-2, the numeric ones are raw bytes.
    std::string old_value;
    std::string new_value;
    std::uint32_t new_packet_size = 0;
};

struct done_token {
    std::uint16_t status = 0;
    std::uint16_t current_command = 0;
    std::uint64_t row_count = 0;
    bool has_row_count = false;

    [[nodiscard]] bool more_results() const noexcept { return (status & 0x0001) != 0; }
    [[nodiscard]] bool error() const noexcept { return (status & 0x0002) != 0; }
};

// A column as COLMETADATA describes it. A ROW carries values only, so every row is
// decoded against this description; a reader that forgot it would have to guess the
// width of each value.
struct column_meta {
    // The wire type byte, kept raw so an unmodelled type is reported rather than
    // guessed from its size.
    std::uint8_t type = 0;
    // Maximum length in bytes for a variable-length type; the fixed width otherwise.
    std::uint32_t max_length = 0;
    std::uint8_t precision = 0;
    std::uint8_t scale = 0;
    // Only meaningful for character types; five raw bytes.
    std::array<std::uint8_t, 5> collation{};
    std::string name;
    // A fixed-width column is never NULL; a variable one is nullable unless its
    // maximum length is zero.
    bool nullable = false;
};

// One decoded token. `kind` selects the meaningful member.
struct token {
    enum class kind : std::uint8_t { error, info, login_ack, env_change, done, colmetadata, row, unknown };
    kind type = kind::unknown;
    error_token error;
    error_token info;
    login_ack ack;
    env_change env;
    done_token done;
    // Set for a COLMETADATA token.
    std::vector<column_meta> columns;
    // Set for a ROW/NBCROW token: canonical text values and which were NULL.
    std::vector<std::string> values;
    std::vector<bool> nulls;
    // The raw token byte, so an unhandled token can be reported with its type.
    std::uint8_t raw_type = 0;
};

// Reads tokens from one message payload, in order.
class CRPC_API token_reader {
public:
    explicit token_reader(span<const std::byte> payload) noexcept : payload_(payload) {}

    // Decodes the next token. `need_more` means the payload ended before a token
    // did, which for a complete message is a protocol error rather than a pause.
    enum class result : std::uint8_t { token_ready, end, error };
    [[nodiscard]] result next(token& out) noexcept;

    [[nodiscard]] rpc_code error() const noexcept { return error_; }

private:
    [[nodiscard]] bool need(std::size_t count) const noexcept {
        return offset_ <= payload_.size() && payload_.size() - offset_ >= count;
    }
    [[nodiscard]] std::uint8_t u8() noexcept;
    [[nodiscard]] std::uint16_t u16() noexcept;
    [[nodiscard]] std::uint32_t u32() noexcept;
    // Token lengths and character counts are big-endian, but the numeric fields
    // inside a token are little-endian. Both live in one struct, so both are named.
    [[nodiscard]] std::uint16_t u16_le() noexcept;
    [[nodiscard]] std::uint32_t u32_le() noexcept;
    // Explicit byte-order readers. `(u8() << 8) | u8()` is undefined: the two calls
    // are unsequenced, so the bytes can be read in either order.
    [[nodiscard]] std::uint16_t le16() noexcept;
    [[nodiscard]] std::uint32_t le32() noexcept;
    [[nodiscard]] std::uint64_t le64() noexcept;
    [[nodiscard]] std::uint16_t be16() noexcept;
    [[nodiscard]] std::string b_varchar() noexcept;   // 1-byte length in chars
    [[nodiscard]] std::string us_varchar() noexcept;  // 2-byte length in chars
    [[nodiscard]] status read_colmetadata(std::vector<column_meta>& out) noexcept;
    [[nodiscard]] status read_row(bool compressed, std::vector<std::string>& values,
                                  std::vector<bool>& nulls) noexcept;
    void fail(rpc_code code) noexcept;

    span<const std::byte> payload_;
    std::size_t offset_ = 0;
    rpc_code error_ = rpc_code::ok;
    // The most recent COLMETADATA. A ROW carries values only, so this is the only
    // way to know how wide each one is.
    std::vector<column_meta> columns_;
};

// Maps a TDS error number to the library code. SQL Server numbers are grouped:
// 2xx/5xx are permission and object errors, 1205 deadlock, 2601/2627 duplicate key.
[[nodiscard]] CRPC_API rpc_code map_error_number(std::uint32_t number) noexcept;

} // namespace tds
} // namespace db
} // namespace crpc

#endif // CRPC_DB_TDS_PROTOCOL_HPP
