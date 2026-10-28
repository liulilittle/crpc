// CRPC -- Oracle TTC (Two-Task Common) message codec, minimal closed loop.
//
// END-TO-END UNVERIFIED. There is no Oracle server in this environment, so no
// message built here has ever been exchanged with a database. Unlike the TNS
// framing there is no captured server reply to compare against, so this module
// is backed by a **manual comparison** with Oracle's python-oracledb (thin
// mode): every field order and every constant below cites the reference file
// and line in docs/oracle-ttc-evidence.md sections 9-12. There is no
// differential byte vector for TTC -- saying otherwise would be false.
//
// Scope, deliberately minimal:
//
//   * universal integers (the length-prefixed big-endian form every TTC field
//     uses), length-prefixed bytes and the 254/255 markers;
//   * the 3-byte message header (type, function code, sequence number);
//   * the Execute and Fetch bodies in the reference's field order, for the case
//     this codec models: one statement, no binds, no defines, no arrays;
//   * column metadata, one row value, and the Oracle NUMBER and DATE decoders.
//
// Not here, on purpose: authentication (O5LOGON/OSESSKEY), LOBs, arrays,
// pipelining, DRCP, and the DataTypes capability exchange that a real server
// requires before it will accept an Execute. This is a codec, not a driver.
#ifndef CRPC_DB_ORACLE_TTC_HPP
#define CRPC_DB_ORACLE_TTC_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace db {
namespace oracle {

// --- Message types -----------------------------------------------------------
// impl/thin/constants.pxi:179-201.
inline constexpr std::uint8_t ttc_msg_protocol = 1;
inline constexpr std::uint8_t ttc_msg_data_types = 2;
inline constexpr std::uint8_t ttc_msg_function = 3;
inline constexpr std::uint8_t ttc_msg_error = 4;
inline constexpr std::uint8_t ttc_msg_row_header = 6;
inline constexpr std::uint8_t ttc_msg_row_data = 7;
inline constexpr std::uint8_t ttc_msg_parameter = 8;
inline constexpr std::uint8_t ttc_msg_status = 9;
inline constexpr std::uint8_t ttc_msg_io_vector = 11;
inline constexpr std::uint8_t ttc_msg_describe_info = 16;
inline constexpr std::uint8_t ttc_msg_piggyback = 17;
inline constexpr std::uint8_t ttc_msg_flush_out_binds = 19;
inline constexpr std::uint8_t ttc_msg_bit_vector = 21;
inline constexpr std::uint8_t ttc_msg_implicit_resultset = 27;
inline constexpr std::uint8_t ttc_msg_end_of_response = 29;

// --- Function codes ----------------------------------------------------------
// impl/thin/constants.pxi:418-450. The numbers the task brief assumed (Execute
// 0x03, Fetch 0x11, OALL8 0x06) do not exist in the reference: the thin client
// has no OALL8 path, and 3 is a data type number (BINARY_INTEGER).
enum class ttc_function : std::uint8_t {
    reexecute = 4,
    fetch = 5,
    logoff = 9,
    commit = 14,
    rollback = 15,
    reexecute_and_fetch = 78,
    execute = 94,
    close_cursors = 105,
    auth_phase_two = 115,
    auth_phase_one = 118,
    ping = 147,
    session_release = 163
};

// --- Execute options and flags -----------------------------------------------
// impl/thin/constants.pxi:275-293.
inline constexpr std::uint32_t ttc_exec_option_parse = 0x01;
inline constexpr std::uint32_t ttc_exec_option_bind = 0x08;
inline constexpr std::uint32_t ttc_exec_option_define = 0x10;
inline constexpr std::uint32_t ttc_exec_option_execute = 0x20;
inline constexpr std::uint32_t ttc_exec_option_fetch = 0x40;
inline constexpr std::uint32_t ttc_exec_option_commit = 0x100;
inline constexpr std::uint32_t ttc_exec_option_plsql_bind = 0x400;
inline constexpr std::uint32_t ttc_exec_option_not_plsql = 0x8000;
inline constexpr std::uint32_t ttc_exec_option_describe = 0x20000;
inline constexpr std::uint32_t ttc_exec_option_batch_errors = 0x80000;

inline constexpr std::uint32_t ttc_exec_flag_dml_rowcounts = 0x4000;
inline constexpr std::uint32_t ttc_exec_flag_implicit_resultset = 0x8000;
inline constexpr std::uint32_t ttc_exec_flag_no_cancel_on_eof = 0x80;
inline constexpr std::uint32_t ttc_exec_flag_scrollable = 0x02;

// The reference always declares an al8i4 array of 13 entries
// (impl/thin/messages/execute.pyx:131).
inline constexpr std::uint32_t ttc_al8i4_length = 13;
// `TNS_MAX_LONG_LENGTH` (impl/thin/constants.pxi:736).
inline constexpr std::uint32_t ttc_max_long_length = 0x7fffffffu;

// Length prefixes (impl/base/buffer.pyx:132-151, base_impl.pxd:150-151).
inline constexpr std::uint8_t ttc_long_length_indicator = 254;
inline constexpr std::uint8_t ttc_null_length_indicator = 255;
inline constexpr std::size_t ttc_max_short_length = 252;
inline constexpr std::size_t ttc_chunk_size = 32767;

// Connection field version at which Execute gains extra empty pointer fields
// (impl/thin/messages/execute.pyx:171-179).
inline constexpr std::uint8_t ttc_field_version_12_2 = 8;
inline constexpr std::uint8_t ttc_field_version_12_2_ext1 = 9;

// --- Data type numbers -------------------------------------------------------
// base_impl.pxd:118-142.
enum class ora_type : std::uint16_t {
    varchar = 1,
    number = 2,
    binary_integer = 3,
    long_text = 8,
    rowid = 11,
    date = 12,
    raw = 23,
    long_raw = 24,
    char_text = 96,
    binary_float = 100,
    binary_double = 101,
    cursor = 102,
    clob = 112,
    blob = 113,
    timestamp = 180,
    timestamp_tz = 181,
    urowid = 208,
    boolean = 252
};

// Character set form (base_impl.pxd:145-147).
inline constexpr std::uint8_t ttc_cs_form_implicit = 1;
inline constexpr std::uint8_t ttc_cs_form_nchar = 2;

// --- Universal integers ------------------------------------------------------

// One length byte, then the magnitude big-endian; zero is the single byte 0
// (impl/base/buffer.pyx:737-790). Signed values set bit 0x80 of the length byte
// (impl/base/buffer.pyx:666-684).
CRPC_API void ttc_write_ub2(std::vector<std::byte>& out, std::uint16_t value);
CRPC_API void ttc_write_ub4(std::vector<std::byte>& out, std::uint32_t value);
CRPC_API void ttc_write_ub8(std::vector<std::byte>& out, std::uint64_t value);
CRPC_API void ttc_write_sb4(std::vector<std::byte>& out, std::int32_t value);

// Length-prefixed bytes: one byte for 1..252; otherwise 254 followed by
// `ub4 chunk length` + chunk pairs and a terminating `ub4 0`
// (impl/base/buffer.pyx:132-151).
CRPC_API void ttc_write_bytes_with_length(std::vector<std::byte>& out,
                                          span<const std::byte> value);
CRPC_API void ttc_write_text_with_length(std::vector<std::byte>& out,
                                         std::string_view value);

// type, function code, sequence number (impl/thin/messages/base.pyx:786-793).
// The sequence number restarts at 1 and never takes 0
// (impl/thin/packet.pyx:903-907).
[[nodiscard]] CRPC_API status ttc_write_message_header(std::vector<std::byte>& out,
                                                       std::uint8_t message_type,
                                                       ttc_function function,
                                                       std::uint8_t sequence);

// --- Execute and Fetch -------------------------------------------------------

// One full Execute message. Only the first execution of a statement is modelled,
// so the cursor id is 0 and the SQL text travels inline
// (impl/thin/messages/execute.pyx:120-208).
struct ttc_execute {
    // Options bit mask; see ttc_execute_option_* above.
    std::uint32_t options = 0;
    // Zero means "first execution": the cursor-id pointer is set and the SQL
    // length follows it.
    std::uint32_t cursor_id = 0;
    std::string sql;
    // Prefetch rows; the reference sends the cursor's prefetchrows here.
    std::uint32_t prefetch_rows = 1;
    // al8i4[1], the execution count: 0 for a query's first execution.
    std::uint32_t num_iters = 1;
    // al8i4[9].
    std::uint32_t execute_flags = 0;
    // al8i4[7]: 1 for a query, 0 otherwise.
    bool is_query = true;
    // Number of defines (al8doac); 0 unless the caller asked to define.
    std::uint32_t num_defines = 0;
    // Negotiated field version; 8 = 12.2, the first version with the extra
    // empty pointer fields.
    std::uint8_t field_version = ttc_field_version_12_2;
};

// PARSE | EXECUTE | FETCH | NOT_PLSQL, the options the reference computes for a
// query's first execution (impl/thin/messages/execute.pyx:73-105).
[[nodiscard]] CRPC_API std::uint32_t ttc_execute_options_for_query() noexcept;

// Appends the Execute body (everything after the 3-byte message header).
[[nodiscard]] CRPC_API status ttc_write_execute_body(std::vector<std::byte>& out,
                                                    const ttc_execute& request);

// Header plus body, which is what goes inside a DATA packet.
[[nodiscard]] CRPC_API status ttc_write_execute(std::vector<std::byte>& out,
                                                const ttc_execute& request,
                                                std::uint8_t sequence);

struct ttc_fetch {
    std::uint32_t cursor_id = 0;
    std::uint32_t array_size = 1;
};

// `type=3, function_code=5, sequence, ub4 cursor id, ub4 array size`
// (impl/thin/messages/fetch.pyx:42-49).
[[nodiscard]] CRPC_API status ttc_write_fetch(std::vector<std::byte>& out,
                                              const ttc_fetch& request,
                                              std::uint8_t sequence);

// --- Reader ------------------------------------------------------------------

// A bounds-checked cursor over a TTC body. The first failure sticks and leaves
// `error()` set, so a caller may read a whole record and check once at the end.
// Every read past the end fails instead of returning a partial value.
class CRPC_API ttc_reader {
public:
    explicit ttc_reader(span<const std::byte> data) noexcept : data_(data) {}

    [[nodiscard]] std::uint8_t u8() noexcept;
    [[nodiscard]] std::uint16_t ub2() noexcept;
    [[nodiscard]] std::uint32_t ub4() noexcept;
    [[nodiscard]] std::uint64_t ub8() noexcept;
    [[nodiscard]] std::int32_t sb4() noexcept;
    [[nodiscard]] std::uint16_t u16be() noexcept;
    [[nodiscard]] std::uint32_t u32be() noexcept;

    // Reads the length byte and then the bytes. A 255 length byte (or 0) means
    // "no value" and is reported through `last_was_null()`.
    [[nodiscard]] std::vector<std::byte> bytes_with_length();
    [[nodiscard]] std::string text_with_length();

    // True when the most recent bytes_with_length()/text_with_length() saw the
    // null indicator.
    [[nodiscard]] bool last_was_null() const noexcept { return null_; }
    [[nodiscard]] bool ok() const noexcept { return error_.ok(); }
    [[nodiscard]] const status& error() const noexcept { return error_; }
    [[nodiscard]] std::size_t position() const noexcept { return offset_; }
    [[nodiscard]] std::size_t remaining() const noexcept;

    void skip(std::size_t count) noexcept;
    void fail(rpc_code code, const char* message) noexcept;

private:
    [[nodiscard]] bool need(std::size_t count) noexcept;
    [[nodiscard]] std::uint8_t length_byte(bool& negative) noexcept;

    span<const std::byte> data_;
    std::size_t offset_ = 0;
    bool null_ = false;
    status error_;
};

// --- Column metadata and row values -----------------------------------------

// The subset of the reference's OracleMetadata this codec models
// (impl/thin/messages/base.pyx:315-396). The OID and the annotation blocks are
// skipped on read and not modelled.
struct ttc_column {
    ora_type type = ora_type::varchar;
    std::int8_t precision = 0;
    std::int8_t scale = 0;
    std::uint32_t buffer_size = 0;
    std::uint32_t max_size = 0;
    std::uint8_t csfrm = 0;
    std::uint8_t nulls_allowed = 0;
    std::string name;
    std::string schema;
    std::string type_name;
};

// Decodes one metadata block. `field_version` selects the 12.2+ oaccolid field;
// the caller passes what DataTypes negotiated.
[[nodiscard]] CRPC_API status ttc_read_metadata(ttc_reader& reader,
                                               std::uint8_t field_version,
                                               ttc_column& out);

// A decoded cell. `text` carries canonical text for NUMBER/DATE/character data;
// `bytes` carries RAW data and is empty otherwise.
struct ttc_value {
    bool is_null = false;
    std::string text;
    std::vector<std::byte> bytes;
};

// Decodes one value using the column's type. Types outside the modelled set
// report `protocol_error` rather than guessing a width
// (impl/base/buffer.pyx:159-236).
[[nodiscard]] CRPC_API status ttc_read_value(ttc_reader& reader,
                                             const ttc_column& column,
                                             ttc_value& out);

// Oracle NUMBER to canonical decimal text, as impl/base/decoders.pyx:157-273
// builds it. Note the exponent arithmetic is done in 8 bits and wraps: that is
// how 0xc1 becomes exponent 0.
[[nodiscard]] CRPC_API status ttc_decode_number(span<const std::byte> value,
                                                std::string& text);

// Oracle DATE: century, year-of-century, month, day, then hour, minute and
// second each biased by 1 (impl/base/decoders.pyx:87-109). The result is
// `YYYY-MM-DD HH:MM:SS`.
[[nodiscard]] CRPC_API status ttc_decode_date(span<const std::byte> value,
                                              std::string& text);

} // namespace oracle
} // namespace db
} // namespace crpc

#endif // CRPC_DB_ORACLE_TTC_HPP