// CRPC -- streaming multipart/form-data parser (RFC 7578 / RFC 2046).
//
// The parser is incremental: `feed` may be called with any split of the byte
// stream, including one byte at a time. It never buffers the whole body; the
// only retained bytes are the current part header block (bounded by
// `max_part_header_bytes`) and a rollback tail of `boundary_len + 4` bytes so a
// boundary prefix split across two `feed` calls is not mis-split.
//
// Events are delivered through `multipart_handler`:
//
//   on_part_begin(info)  once, after the part headers have been parsed
//   on_part_data(bytes)  zero or more times, with a view into parser storage
//   on_part_end()        once, when the next delimiter has been validated
//
// The data view passed to `on_part_data` borrows from the parser's internal
// buffer and is only valid for the duration of the callback. A handler that
// keeps bytes must copy them.
//
// Failure model (fail closed):
//
//   * structurally malformed input (missing/mismatched boundary, unterminated
//     part or header block, illegal characters in a header) ->
//     `rpc_code::protocol_error` (HTTP 400);
//   * a configured limit exceeded -> `rpc_code::resource_exhausted`
//     (HTTP 413).
//
// Limit summary (all inclusive):
//
//   max_parts               32 parts
//   max_part_header_bytes   8 KiB per part header block
//   max_header_fields       32 fields per part
//   max_part_bytes          64 MiB of data per part
//   max_total_bytes         256 MiB of part data for the whole body
//   max_boundary_bytes      70 bytes (RFC 2046)
//   max_preamble_bytes      64 KiB of ignored preamble
//   max_header_extension    64 KiB for a folded/non-standard header value
//
// Filenames are sanitized before they are exposed: directory components are
// stripped, and a filename that is empty, "." or "..", or that contains a NUL
// or any control character is rejected. The raw client filename is never
// exposed. The sanitized name is metadata only; it must never be used as an
// on-disk path (see file_transfer.hpp for random temp names).
//
// Thread safety: a parser instance is single-threaded. Distinct instances are
// independent and share no mutable state.
//
// Allocation behaviour: `feed` appends to an internal buffer whose size is
// bounded by one input chunk plus `boundary_len + 4`; the buffer is compacted
// after every call. Part metadata allocates name/filename/content-type strings
// and a header_list per part. The data path performs no allocation beyond the
// internal input buffer growth.
#ifndef CRPC_HTTP_MULTIPART_HPP
#define CRPC_HTTP_MULTIPART_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/http/message.hpp>
#include <crpc/span.hpp>

namespace crpc::http {

// --- Limits -----------------------------------------------------------------

struct multipart_limits {
    std::size_t max_parts = 32;
    std::size_t max_part_header_bytes = 8u * 1024u;
    std::size_t max_header_fields = 32;
    std::size_t max_part_bytes = 64u * 1024u * 1024u;
    std::size_t max_total_bytes = 256u * 1024u * 1024u;
    std::size_t max_boundary_bytes = 70;
    std::size_t max_preamble_bytes = 64u * 1024u;
    // Upper bound on a single Content-Disposition parameter value (name or
    // filename). Bounds metadata allocation from a hostile client.
    std::size_t max_parameter_bytes = 4u * 1024u;
};

[[nodiscard]] const multipart_limits& default_multipart_limits() noexcept;

// --- Part metadata ----------------------------------------------------------

struct multipart_part_info {
    std::uint64_t index = 0;
    // Form field name from Content-Disposition; always present on success.
    std::string name;
    // Sanitized filename. Empty when the part has no filename (a form field).
    std::string filename;
    bool has_filename = false;
    // Raw Content-Type value for the part (trimmed), empty when absent.
    std::string content_type;
    header_list headers;
};

// Event sink. Every callback returns a status; a non-ok status aborts parsing
// and is surfaced by the next `feed`/`finish` call. A handler must not destroy
// the parser from inside a callback.
class multipart_handler {
public:
    virtual ~multipart_handler() = default;

    virtual status on_part_begin(const multipart_part_info& info) = 0;
    virtual status on_part_data(span<const std::byte> data) = 0;
    virtual status on_part_end() = 0;
};

// --- Filename sanitization --------------------------------------------------

// Returns the final path component of `raw`, or an error when the component is
// empty, "." or "..", or contains NUL/control characters. Both '/' and '\\'
// are treated as separators so a Windows-style path cannot smuggle a directory
// component through a server running on POSIX (and vice versa).
[[nodiscard]] result<std::string> sanitize_filename(std::string_view raw);

// --- Boundary extraction ----------------------------------------------------

// Extracts the boundary parameter from a multipart Content-Type value. The
// media type must be multipart/form-data (case-insensitive). The boundary is
// validated against the RFC 2046 character set and length limit.
[[nodiscard]] result<std::string> multipart_boundary(std::string_view content_type,
                                                     std::size_t max_bytes = 70);

// --- Parser -----------------------------------------------------------------

class multipart_parser {
public:
    // `boundary` must be a validated boundary without the leading "--".
    // Prefer `create`, which validates the Content-Type value.
    multipart_parser(std::string boundary, multipart_handler& handler,
                     multipart_limits limits = {});

    multipart_parser(const multipart_parser&) = delete;
    multipart_parser& operator=(const multipart_parser&) = delete;
    multipart_parser(multipart_parser&&) noexcept = default;
    multipart_parser& operator=(multipart_parser&&) noexcept = default;
    ~multipart_parser() = default;

    // Parses the Content-Type and constructs a parser in one step.
    [[nodiscard]] static result<multipart_parser> create(
        std::string_view content_type, multipart_handler& handler,
        multipart_limits limits = {});

    // Consumes as much of `data` as possible. Returns `ok` on progress (the
    // message may or may not be complete; see `complete()`), or the first
    // error. After an error every subsequent call returns the same error.
    [[nodiscard]] status feed(span<const std::byte> data);

    // Signals end of body. Returns `ok` when the closing delimiter was seen;
    // `protocol_error` when the body is truncated. `finish` is idempotent.
    [[nodiscard]] status finish();

    [[nodiscard]] bool complete() const noexcept { return complete_; }
    [[nodiscard]] std::size_t parts_seen() const noexcept { return part_count_; }
    [[nodiscard]] std::size_t total_data_bytes() const noexcept { return total_bytes_; }
    [[nodiscard]] std::size_t current_part_bytes() const noexcept {
        return current_part_bytes_;
    }
    [[nodiscard]] const rpc_error& error() const noexcept { return error_; }

private:
    enum class state : std::uint8_t {
        preamble,
        after_boundary,
        part_header,
        part_body,
        done,
        failed
    };

    status run();
    status fail(rpc_code code, std::string message);
    status emit_data(std::size_t begin, std::size_t end);
    status begin_part();
    status end_part();
    status parse_part_headers(std::string_view block);
    status after_boundary();
    void compact();

    multipart_limits limits_;
    std::string boundary_;
    // "\r\n--" + boundary, used to find the end of a part body.
    std::string body_delimiter_;
    // "--" + boundary, used to find the first boundary in the preamble.
    std::string start_delimiter_;
    multipart_handler* handler_;
    state state_ = state::preamble;
    std::string buffer_;
    std::size_t pos_ = 0;
    std::size_t preamble_bytes_ = 0;
    std::size_t total_bytes_ = 0;
    std::size_t current_part_bytes_ = 0;
    std::size_t part_count_ = 0;
    multipart_part_info current_;
    bool complete_ = false;
    rpc_error error_;
};

} // namespace crpc::http

#endif // CRPC_HTTP_MULTIPART_HPP
