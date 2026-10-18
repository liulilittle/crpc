// CRPC -- HTTP/1.1 message model, incremental parser and serializer.
//
// The parser is deliberately strict where strictness prevents request
// smuggling and deliberately lenient where RFC 9112 tells recipients to be
// tolerant:
//
//   * the start line and header block accept CRLF and bare LF, but a bare CR
//     inside a line is rejected;
//   * obs-fold (a header line starting with SP or HTAB) is rejected outright;
//   * chunk framing is strict CRLF only, because chunk boundaries are the one
//     place where a lenient line ending changes the message length;
//   * Content-Length and Transfer-Encoding are mutually exclusive, a
//     Transfer-Encoding chain must end in "chunked", and repeated
//     Content-Length fields are rejected.
//
// Parsing is incremental: `feed` may be called with any split of the byte
// stream, including one byte at a time. The parser owns a small pending buffer
// so callers never have to preserve the input span after `feed` returns.
#ifndef CRPC_HTTP_MESSAGE_HPP
#define CRPC_HTTP_MESSAGE_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc::http {

// --- Methods -----------------------------------------------------------------

// The nine methods RFC 9110 defines. Any other token is accepted as an
// extension method and surfaced as `extension`; the original token is kept in
// `request::method_token`. Method comparison is case-sensitive: "get" is not
// GET.
enum class method : std::uint8_t {
    get,
    head,
    post,
    put,
    del,
    patch,
    options,
    trace,
    connect,
    extension
};

[[nodiscard]] method method_from_token(std::string_view token) noexcept;

// Returns the canonical upper-case token, or "" for `extension`.
[[nodiscard]] const char* to_token(method value) noexcept;

// True when the string is a non-empty RFC 9110 token.
[[nodiscard]] bool is_token(std::string_view text) noexcept;

// --- Versions and target forms -----------------------------------------------

enum class http_version : std::uint8_t { http_1_0, http_1_1 };

[[nodiscard]] const char* to_string(http_version version) noexcept;

enum class target_form : std::uint8_t {
    origin,    // /path?query
    absolute,  // scheme://authority/path?query
    authority, // host:port (CONNECT)
    asterisk   // *
};

// Splits a request target into its raw (not percent-decoded) path and query.
// `path` excludes the query, `query` is everything after the first '?'.
// A fragment ('#') is rejected: request targets must not carry one.
[[nodiscard]] status split_target(std::string_view target, target_form& form,
                                  std::string& path, std::string& query);

// Strict percent decoding. Invalid escapes ("%zz", a trailing "%"), a raw NUL
// and a decoded NUL are rejected with `rpc_code::invalid_argument`. '+' is
// passed through unchanged; form decoding is an MVC concern, not an HTTP
// message concern.
[[nodiscard]] status percent_decode(std::string_view input, std::string& output);

// --- Header container --------------------------------------------------------

struct header {
    std::string name;
    std::string value;
};

// Ordered, case-insensitive header list. Insertion order is preserved on the
// wire. Duplicate fields are preserved (HTTP permits them, e.g. Set-Cookie);
// `find` returns the first match and `get_all` returns every value.
class header_list {
public:
    using container_type = std::vector<header>;
    using const_iterator = container_type::const_iterator;

    header_list() = default;

    void add(std::string_view name, std::string_view value);

    [[nodiscard]] const std::string* find(std::string_view name) const noexcept;
    [[nodiscard]] bool contains(std::string_view name) const noexcept;
    [[nodiscard]] std::size_t count(std::string_view name) const noexcept;

    // The returned views borrow from the stored strings and stay valid until
    // the next mutation of this list.
    [[nodiscard]] std::vector<std::string_view> get_all(std::string_view name) const;

    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    void clear() noexcept { entries_.clear(); }

    [[nodiscard]] const_iterator begin() const noexcept { return entries_.begin(); }
    [[nodiscard]] const_iterator end() const noexcept { return entries_.end(); }

private:
    container_type entries_;
};

// --- Messages ----------------------------------------------------------------

struct request {
    crpc::http::method method = crpc::http::method::get;
    // Canonical token for known methods, the original token for extensions.
    std::string method_token;
    target_form form = target_form::origin;
    // The raw request target exactly as received.
    std::string target;
    // Raw path and query components (percent escapes not decoded).
    std::string path;
    std::string query;
    http_version version = http_version::http_1_1;
    header_list headers;
    header_list trailers;
    std::string body;
    // True when an `Expect: 100-continue` field was present. The server decides
    // whether to send the interim response; the parser never does.
    bool expect_continue = false;
};

struct response {
    http_version version = http_version::http_1_1;
    int status_code = 200;
    std::string reason = "OK";
    header_list headers;
    header_list trailers;
    std::string body;
};

// --- Limits ------------------------------------------------------------------

// Every limit is inclusive. Exceeding one yields
// `rpc_code::resource_exhausted` and the error message names the limit.
struct parser_limits {
    std::size_t request_line_bytes = 8u * 1024u;
    std::size_t header_block_bytes = 64u * 1024u;
    std::size_t header_count = 100;
    std::size_t body_bytes = 8u * 1024u * 1024u;
    // Maximum length of a chunk-size line and of one trailer line.
    std::size_t chunk_line_bytes = 8u * 1024u;
    std::size_t trailer_block_bytes = 64u * 1024u;
    std::size_t trailer_count = 100;
};

// --- Chunked codec -----------------------------------------------------------

enum class chunk_status : std::uint8_t { need_more, complete, error };

// Incremental chunked transfer decoder.
//
// `feed` consumes bytes from `data` (it reports the number consumed through
// `consumed`), appends decoded body bytes to `out`, and keeps any partial
// chunk-size or trailer line in an internal buffer. Bytes after the final
// empty line are not consumed, so a caller can feed a pipelined message next.
class chunked_decoder {
public:
    explicit chunked_decoder(parser_limits limits = parser_limits{});

    [[nodiscard]] chunk_status feed(span<const std::byte> data, std::string& out,
                                    std::size_t& consumed);

    [[nodiscard]] bool complete() const noexcept { return state_ == state::done; }
    [[nodiscard]] const header_list& trailers() const noexcept { return trailers_; }
    [[nodiscard]] const rpc_error& error() const noexcept { return error_; }
    [[nodiscard]] std::size_t body_size() const noexcept { return body_size_; }

    void reset();

private:
    enum class state : std::uint8_t { size, data, data_crlf, trailers, done, failed };

    bool fail(rpc_code code, std::string message);
    bool finish_size_line();
    bool finish_trailer_line();

    parser_limits limits_;
    state state_ = state::size;
    std::string line_;
    std::size_t chunk_remaining_ = 0;
    std::size_t body_size_ = 0;
    std::size_t trailer_bytes_ = 0;
    bool saw_cr_ = false;
    int crlf_state_ = 0;
    header_list trailers_;
    rpc_error error_;
};

// Chunked transfer encoder. `append` emits zero or more complete chunks;
// `finish` emits the terminating zero chunk and optional trailers. All output
// uses CRLF, regardless of how lenient the input side is.
class chunked_encoder {
public:
    explicit chunked_encoder(std::size_t max_chunk_bytes = 16u * 1024u) noexcept
        : max_chunk_bytes_(max_chunk_bytes) {}

    [[nodiscard]] status append(span<const std::byte> data, std::string& out) const;
    [[nodiscard]] status finish(std::string& out) const;
    [[nodiscard]] status finish(const header_list& trailers, std::string& out) const;

private:
    std::size_t max_chunk_bytes_;
};

// --- Incremental parser ------------------------------------------------------

enum class parse_status : std::uint8_t {
    need_more,
    headers_complete,
    message_complete,
    error
};

class http_parser {
public:
    enum class kind : std::uint8_t { request, response };

    explicit http_parser(kind type = kind::request, parser_limits limits = parser_limits{});

    // Appends `data` to the pending buffer and advances the state machine.
    // Returns `headers_complete` once the header block is parsed but the body
    // is not yet complete, and `message_complete` exactly when the last body
    // byte (or the final chunk terminator) has arrived. After an error every
    // subsequent call returns `error`.
    [[nodiscard]] parse_status feed(span<const std::byte> data);

    [[nodiscard]] parse_status current_status() const noexcept;
    [[nodiscard]] bool headers_complete() const noexcept { return headers_done_; }
    [[nodiscard]] bool complete() const noexcept { return complete_; }
    [[nodiscard]] const rpc_error& error() const noexcept { return error_; }
    [[nodiscard]] bool expect_100_continue() const noexcept {
        return request_.expect_continue;
    }

    [[nodiscard]] const request& get_request() const noexcept { return request_; }
    [[nodiscard]] const response& get_response() const noexcept { return response_; }

    // Bytes still buffered after `message_complete`, e.g. a pipelined request.
    // The view is valid until the next call to `feed` or `reset`.
    [[nodiscard]] span<const std::byte> pending() const noexcept;

    void reset();

private:
    enum class state : std::uint8_t {
        start_line,
        header_lines,
        body_fixed,
        body_chunked,
        done,
        failed
    };

    enum class line_result : std::uint8_t { found, need_more, error };

    bool fail(rpc_code code, std::string message);
    [[nodiscard]] line_result read_line(std::size_t limit, const char* what,
                                        std::string& line, std::size_t& consumed);
    [[nodiscard]] bool parse_start_line(std::string_view line);
    [[nodiscard]] bool parse_request_line(std::string_view line);
    [[nodiscard]] bool parse_status_line(std::string_view line);
    [[nodiscard]] bool parse_header_line(std::string_view line);
    [[nodiscard]] parse_status finish_headers();

    [[nodiscard]] std::string& body() noexcept {
        return kind_ == kind::request ? request_.body : response_.body;
    }
    [[nodiscard]] header_list& headers() noexcept {
        return kind_ == kind::request ? request_.headers : response_.headers;
    }
    [[nodiscard]] header_list& trailers() noexcept {
        return kind_ == kind::request ? request_.trailers : response_.trailers;
    }

    kind kind_;
    parser_limits limits_;
    state state_ = state::start_line;
    std::string buf_;
    request request_;
    response response_;
    std::size_t header_bytes_ = 0;
    std::size_t fixed_remaining_ = 0;
    bool headers_done_ = false;
    bool complete_ = false;
    bool chunked_ = false;
    chunked_decoder decoder_;
    rpc_error error_;
};

// --- Serializer --------------------------------------------------------------

struct serialize_options {
    // When false the body bytes (and the chunk terminator) are omitted. The
    // framing headers still describe `body.size()`, which is what a HEAD
    // response needs.
    bool include_body = true;
    // Emit `Transfer-Encoding: chunked` even when the message has no such
    // header. Ignored (and rejected) when a Content-Length is present.
    bool force_chunked = false;
};

// Serializes a complete message. Header order is preserved; a missing
// Content-Length is added when the body is not chunked. Header names and values
// are validated and CR/LF injection is rejected with `rpc_code::encode_error`.
// On failure `out` is left unchanged.
[[nodiscard]] status serialize(const request& message, std::string& out,
                               serialize_options options = serialize_options{});
[[nodiscard]] status serialize(const response& message, std::string& out,
                               serialize_options options = serialize_options{});

} // namespace crpc::http

#endif // CRPC_HTTP_MESSAGE_HPP
