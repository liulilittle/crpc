// CRPC -- Redis RESP protocol codec.
//
// RESP2 and RESP3 share a small set of line-prefixed types. Every value starts
// with a one-byte marker:
//
//   marker  RESP2                RESP3
//   '+'     simple string
//   '-'     error
//   ':'     integer
//   '$'     bulk string          (also null when the length is -1)
//   '*'     array                (also null when the count is -1)
//   '_'                          null
//   '#'                          boolean
//   ','                          double
//   '('                          big number
//   '!'                          blob error
//   '='                          verbatim string
//   '%'                          map
//   '~'                          set
//   '>'                          push
//
// All line-oriented frames end in CRLF. Bulk-like frames (`$`, `!`, `=`) carry
// a byte length instead and are followed by exactly that many bytes plus CRLF,
// so binary payloads that contain CR or LF round-trip.
//
// The parser auto-detects the protocol version from the marker it reads, so a
// single stream can carry both RESP2 and RESP3 frames. The encoder is
// versioned explicitly: a value is serialised the way the negotiated protocol
// can express it, falling back to the RESP2 form (a null becomes `$-1`, a
// double becomes a bulk string, a map becomes a flat array, ...).
//
// Fail-closed rules: an unknown marker, a malformed length, a missing or
// misplaced CRLF, a negative count other than -1, a length longer than the
// parser's limit, and a nesting deeper than the parser's limit all produce
// rpc_code::protocol_error (or resource_exhausted for the two limits). No
// allocation happens before a length has been validated against the limit.
#ifndef CRPC_REDIS_RESP_HPP
#define CRPC_REDIS_RESP_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace redis {

// --- Protocol markers -------------------------------------------------------

enum class resp_type : u8 {
    simple_string = '+',
    error = '-',
    integer = ':',
    bulk_string = '$',
    array = '*',
    null_value = '_',
    boolean = '#',
    double_value = ',',
    big_number = '(',
    blob_error = '!',
    verbatim_string = '=',
    map_value = '%',
    set_value = '~',
    push = '>'
};

// The protocol a value is serialised for. Parsing is version-agnostic; only
// encoding needs to know.
enum class resp_protocol : u8 { resp2, resp3 };

// --- Value model ------------------------------------------------------------

// A decoded RESP value. It owns every byte it carries: bulk strings, maps and
// arrays are copied out of the wire, so a value outlives the parser and the
// buffer it came from.
class resp_value;

using resp_array = std::vector<resp_value>;
using resp_set = std::vector<resp_value>;
using resp_push = std::vector<resp_value>;
using resp_map = std::vector<std::pair<resp_value, resp_value>>;

struct resp_null {};
struct resp_simple_string {
    std::string value;
};
struct resp_error {
    std::string message;
};
struct resp_integer {
    std::int64_t value = 0;
};
struct resp_bulk_string {
    std::string value;
};
struct resp_array_value {
    resp_array items;
};
struct resp_boolean {
    bool value = false;
};
struct resp_double {
    double value = 0.0;
};
struct resp_big_number {
    std::string value;
};
struct resp_blob_error {
    std::string message;
};
// A verbatim string carries a short format label and the payload. The wire form
// is `=<len>\r\n<format>:<payload>\r\n`; the parser reads exactly three bytes of
// format, so only a three-byte format round-trips. The encoder accepts any
// format length (see resp_encode_verbatim_format_length_consistency), which is a
// documented asymmetry: a longer format produces a frame the parser rejects.
struct resp_verbatim_string {
    std::string value;
    std::string format;
};
struct resp_map_value {
    resp_map entries;
};
struct resp_set_value {
    resp_set items;
};
struct resp_push_value {
    resp_push items;
};

class CRPC_API resp_value {
public:
    using storage = std::variant<resp_null, resp_simple_string, resp_error, resp_integer,
                                 resp_bulk_string, resp_array_value, resp_boolean,
                                 resp_double, resp_big_number, resp_blob_error,
                                 resp_verbatim_string, resp_map_value, resp_set_value,
                                 resp_push_value>;

    resp_value() noexcept : storage_(resp_null{}) {}
    resp_value(resp_null value) : storage_(std::move(value)) {}
    resp_value(resp_simple_string value) : storage_(std::move(value)) {}
    resp_value(resp_error value) : storage_(std::move(value)) {}
    resp_value(resp_integer value) : storage_(std::move(value)) {}
    resp_value(resp_bulk_string value) : storage_(std::move(value)) {}
    resp_value(resp_array_value value) : storage_(std::move(value)) {}
    resp_value(resp_boolean value) : storage_(std::move(value)) {}
    resp_value(resp_double value) : storage_(std::move(value)) {}
    resp_value(resp_big_number value) : storage_(std::move(value)) {}
    resp_value(resp_blob_error value) : storage_(std::move(value)) {}
    resp_value(resp_verbatim_string value) : storage_(std::move(value)) {}
    resp_value(resp_map_value value) : storage_(std::move(value)) {}
    resp_value(resp_set_value value) : storage_(std::move(value)) {}
    resp_value(resp_push_value value) : storage_(std::move(value)) {}

    [[nodiscard]] resp_type type() const noexcept;

    [[nodiscard]] bool is_null() const noexcept;
    [[nodiscard]] bool is_simple_string() const noexcept;
    [[nodiscard]] bool is_error() const noexcept;
    [[nodiscard]] bool is_integer() const noexcept;
    [[nodiscard]] bool is_bulk_string() const noexcept;
    [[nodiscard]] bool is_array() const noexcept;
    [[nodiscard]] bool is_boolean() const noexcept;
    [[nodiscard]] bool is_double() const noexcept;
    [[nodiscard]] bool is_big_number() const noexcept;
    [[nodiscard]] bool is_blob_error() const noexcept;
    [[nodiscard]] bool is_verbatim_string() const noexcept;
    [[nodiscard]] bool is_map() const noexcept;
    [[nodiscard]] bool is_set() const noexcept;
    [[nodiscard]] bool is_push() const noexcept;

    // Precondition: the corresponding is_*() is true. Reading a member of the
    // wrong alternative is a programming error: std::get throws
    // std::bad_variant_access and, because the accessors are noexcept, that
    // terminates the process. Callers must check is_*() first.
    [[nodiscard]] std::int64_t as_integer() const noexcept;
    [[nodiscard]] bool as_boolean() const noexcept;
    [[nodiscard]] double as_double() const noexcept;
    [[nodiscard]] const std::string& as_simple_string() const noexcept;
    [[nodiscard]] const std::string& as_error() const noexcept;
    [[nodiscard]] const std::string& as_bulk_string() const noexcept;
    [[nodiscard]] const std::string& as_big_number() const noexcept;
    [[nodiscard]] const std::string& as_blob_error() const noexcept;
    [[nodiscard]] const std::string& as_verbatim_string() const noexcept;
    [[nodiscard]] const std::string& verbatim_format() const noexcept;
    [[nodiscard]] const resp_array& as_array() const noexcept;
    [[nodiscard]] const resp_set& as_set() const noexcept;
    [[nodiscard]] const resp_push& as_push() const noexcept;
    [[nodiscard]] const resp_map& as_map() const noexcept;

private:
    storage storage_;
};

[[nodiscard]] CRPC_API bool operator==(const resp_value& lhs,
                                        const resp_value& rhs) noexcept;
[[nodiscard]] CRPC_API bool operator!=(const resp_value& lhs,
                                        const resp_value& rhs) noexcept;

// Equality for the alternatives. These must exist: without them the variant
// comparison resolves `alt == alt` through the implicit `alt -> resp_value`
// converting constructor back into `operator==(resp_value, resp_value)`, which
// recurses until the stack is exhausted. Declared here so any translation unit
// that compares a variant directly finds them through ADL.
[[nodiscard]] CRPC_API bool operator==(const resp_null& lhs, const resp_null& rhs) noexcept;
[[nodiscard]] CRPC_API bool operator==(const resp_simple_string& lhs,
                                        const resp_simple_string& rhs) noexcept;
[[nodiscard]] CRPC_API bool operator==(const resp_error& lhs,
                                        const resp_error& rhs) noexcept;
[[nodiscard]] CRPC_API bool operator==(const resp_integer& lhs,
                                        const resp_integer& rhs) noexcept;
[[nodiscard]] CRPC_API bool operator==(const resp_bulk_string& lhs,
                                        const resp_bulk_string& rhs) noexcept;
[[nodiscard]] CRPC_API bool operator==(const resp_array_value& lhs,
                                        const resp_array_value& rhs) noexcept;
[[nodiscard]] CRPC_API bool operator==(const resp_boolean& lhs,
                                        const resp_boolean& rhs) noexcept;
[[nodiscard]] CRPC_API bool operator==(const resp_double& lhs,
                                        const resp_double& rhs) noexcept;
[[nodiscard]] CRPC_API bool operator==(const resp_big_number& lhs,
                                        const resp_big_number& rhs) noexcept;
[[nodiscard]] CRPC_API bool operator==(const resp_blob_error& lhs,
                                        const resp_blob_error& rhs) noexcept;
[[nodiscard]] CRPC_API bool operator==(const resp_verbatim_string& lhs,
                                        const resp_verbatim_string& rhs) noexcept;
[[nodiscard]] CRPC_API bool operator==(const resp_map_value& lhs,
                                        const resp_map_value& rhs) noexcept;
[[nodiscard]] CRPC_API bool operator==(const resp_set_value& lhs,
                                        const resp_set_value& rhs) noexcept;
[[nodiscard]] CRPC_API bool operator==(const resp_push_value& lhs,
                                        const resp_push_value& rhs) noexcept;

// --- Parser -----------------------------------------------------------------

// Default ceiling for a single bulk string, blob error or verbatim string, and
// for the element count of an array, set or push. 512 MiB matches Redis'
// proto-max-bulk-len; a caller with a different policy passes its own limit.
inline constexpr std::size_t resp_default_max_value_size = 512u * 1024u * 1024u;

// Default nesting limit for recursive containers. A hostile stream of `*1`
// frames must not be able to exhaust the stack.
inline constexpr std::size_t resp_default_max_depth = 128;

// Streaming RESP decoder. Bytes are copied into an internal buffer; a complete
// value is only produced by next(). feed() accepts arbitrarily split chunks,
// including one byte at a time, and several values in one chunk.
//
//   resp_parser parser;
//   parser.feed(chunk);
//   for (;;) {
//       resp_value value;
//       switch (parser.next(value)) {
//       case resp_parser::result::value_ready: handle(value); break;
//       case resp_parser::result::need_more: goto next_chunk;
//       case resp_parser::result::error: fail(parser.error()); break;
//       }
//   }
//   next_chunk:
//   ...
//   if (!parser.finish().ok()) { /* a value was truncated */ }
class CRPC_API resp_parser {
public:
    enum class result : u8 { need_more, value_ready, error };

    resp_parser() noexcept;
    explicit resp_parser(std::size_t max_value_size) noexcept;
    resp_parser(std::size_t max_value_size, std::size_t max_depth) noexcept;

    // Appends `data` to the internal buffer. Input need not outlive the call.
    // A no-op after an error; use reset() to clear the error state.
    void feed(span<const std::byte> data) noexcept;

    // Attempts to decode one value from the buffered bytes.
    //   * need_more     -- the value is incomplete; `out` is unchanged;
    //   * value_ready  -- `out` is filled and the consumed bytes are released;
    //   * error         -- a protocol violation; error() names it and the parser
    //                      stays in the error state until reset().
    [[nodiscard]] result next(resp_value& out) noexcept;

    // End-of-stream check. ok() when the buffer holds no partial value; a
    // truncated value is rpc_code::protocol_error. Does not clear the error
    // state.
    [[nodiscard]] status finish() noexcept;

    // Error code of the last failure. rpc_code::ok when no error occurred.
    [[nodiscard]] rpc_code error() const noexcept;

    // True when the parser is in the error state.
    [[nodiscard]] bool failed() const noexcept;

    // Number of bytes buffered but not yet consumed.
    [[nodiscard]] std::size_t buffered() const noexcept;

    [[nodiscard]] std::size_t max_value_size() const noexcept;
    [[nodiscard]] std::size_t max_depth() const noexcept;

    // Clears buffered bytes and the error state. Retains allocated capacity.
    void reset() noexcept;

private:
    enum class parse_result : u8 { need_more, ok, error };

    parse_result parse_value(std::size_t& pos, std::size_t depth,
                             resp_value& out);
    parse_result read_line(std::size_t& pos, char& marker,
                           std::string& content);
    void maybe_compact() noexcept;

    std::vector<std::byte> buffer_;
    std::size_t offset_ = 0;
    std::size_t max_value_size_ = resp_default_max_value_size;
    std::size_t max_depth_ = resp_default_max_depth;
    rpc_code error_ = rpc_code::ok;
};

// --- Encoder ----------------------------------------------------------------

// A Redis command: the command name followed by its arguments, all encoded as
// bulk strings inside one array.
using resp_command = std::vector<std::string>;

class CRPC_API resp_encoder {
public:
    explicit resp_encoder(resp_protocol protocol = resp_protocol::resp2) noexcept;

    void protocol(resp_protocol value) noexcept;
    [[nodiscard]] resp_protocol protocol() const noexcept;

    [[nodiscard]] std::size_t encoded_size(const resp_value& value) const noexcept;
    [[nodiscard]] std::size_t encoded_size(const resp_command& command) const noexcept;

    // Encodes into `out` and reports the number of bytes written. Failure
    // (out too small, or a simple string/error payload carrying CR/LF, or a
    // blob error carrying CR/LF under RESP2, or a big number carrying CR/LF
    // under RESP3) is rpc_code::encode_error and leaves `written` zero.
    [[nodiscard]] status encode(span<std::byte> out, const resp_value& value,
                                std::size_t& written) const;
    [[nodiscard]] status encode(span<std::byte> out, const resp_command& command,
                                std::size_t& written) const;

    // Allocating convenience forms. `encode_to_string` throws
    // std::invalid_argument when a simple string or error payload carries CR or
    // LF, when a blob error carries CR/LF under RESP2, or when a big number
    // carries CR/LF under RESP3, because the wire form would be a frame the
    // parser rejects; it returns no status, so the throw is the only fail-closed
    // signal it can give.
    [[nodiscard]] std::string encode_to_string(const resp_value& value) const;
    [[nodiscard]] std::string encode_command_to_string(
        const resp_command& command) const;

private:
    std::size_t value_size(const resp_value& value) const noexcept;

    resp_protocol protocol_ = resp_protocol::resp2;
};

// --- Free functions ---------------------------------------------------------

[[nodiscard]] CRPC_API std::size_t encoded_value_size(
    const resp_value& value, resp_protocol protocol = resp_protocol::resp2) noexcept;
[[nodiscard]] CRPC_API std::size_t encoded_command_size(
    const resp_command& command) noexcept;

[[nodiscard]] CRPC_API status encode_value(span<std::byte> out, const resp_value& value,
                                           std::size_t& written,
                                           resp_protocol protocol = resp_protocol::resp2);
[[nodiscard]] CRPC_API status encode_command(span<std::byte> out,
                                             const resp_command& command,
                                             std::size_t& written);

[[nodiscard]] CRPC_API std::string encode_value_to_string(
    const resp_value& value, resp_protocol protocol = resp_protocol::resp2);
[[nodiscard]] CRPC_API std::string encode_command_to_string(
    const resp_command& command);

} // namespace redis
} // namespace crpc

#endif // CRPC_REDIS_RESP_HPP
