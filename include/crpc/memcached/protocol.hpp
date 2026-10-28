// CRPC -- memcached wire protocol codec.
//
// memcached speaks three protocol families over the same connection:
//
//   * the text protocol: a command line, an optional data block, and a reply
//     made of ASCII tokens;
//   * the meta protocol (memcached 1.6+): `mg`/`ms`/`md`/`ma`/`mn`/`me`, whose
//     replies are one line of single-letter flags (optionally followed by a
//     value block);
//   * the binary protocol: a fixed 24-byte header with an opcode, a status and
//     extras, key and value sections.
//
// This header owns framing and parsing only. It never performs I/O: a caller
// feeds bytes with `feed()` and pulls decoded replies with `next()`. That split
// is what lets one connection serve all three families, and it lets the whole
// codec be tested without a socket.
//
// Fail-closed rules. Every length is validated against a caller-supplied limit
// before anything is allocated; a malformed token, a missing CRLF, a value block
// longer than the limit, an unknown reply line and a truncated stream all
// produce `rpc_code::protocol_error` (or `resource_exhausted` for a limit). A
// `SERVER_ERROR` reply is *data*, not a transport error: it is parsed and
// returned so the caller decides whether to surface or retry it.
#ifndef CRPC_MEMCACHED_PROTOCOL_HPP
#define CRPC_MEMCACHED_PROTOCOL_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace memcached {

// --- Shared limits -----------------------------------------------------------

// memcached's own hard limit on a key. A longer key is rejected by the server,
// so the encoder refuses it locally rather than emitting a frame the server
// would answer with CLIENT_ERROR.
inline constexpr std::size_t max_key_size = 250;

// Default ceiling for a single value block. 1 MiB matches the default
// `-I` item size of a stock memcached; a deployment with a different limit
// passes its own.
inline constexpr std::size_t default_max_value_size = 1u * 1024u * 1024u;

// Default ceiling for one reply line (a VALUE header, a STAT line, a meta flag
// line). The longest legitimate line is a STAT line; 64 KiB is far above it and
// still bounds a hostile peer.
inline constexpr std::size_t default_max_line_size = 64u * 1024u;

// --- Text protocol -----------------------------------------------------------

// A single key/value pair returned by `get`/`gets`/`gat`/`gats`. The data is
// owned: a reply outlives the buffer it was parsed from.
struct value_entry {
    std::string key;
    std::uint32_t flags = 0;
    // The CAS token is only non-zero for `gets`/`gats`; a plain `get` reply has
    // no CAS field, and the parser leaves this at zero for it.
    std::uint64_t cas = 0;
    std::string data;
};

// The outcome of a storage/removal command, i.e. the single-word reply.
enum class store_status : std::uint8_t {
    stored,      // STORED
    not_stored,  // NOT_STORED
    exists,      // EXISTS (cas mismatch)
    not_found,   // NOT_FOUND (cas/append/prepend/incr/decr on a missing key)
    deleted,     // DELETED
    touched,     // TOUCHED
    ok           // OK (meta set/delete success)
};

// Which kind of failure the server reported. `error` is the bare `ERROR` line
// (a malformed request); the other two carry a message that must be preserved.
enum class error_kind : std::uint8_t { error, client_error, server_error };

// One parsed reply. `kind` selects which payload field is meaningful, so the
// class is a closed union rather than a variant: a reply is a value that the
// caller reads once and discards, and this keeps the type copyable and cheap.
class CRPC_API text_reply {
public:
    enum class kind : std::uint8_t {
        values,       // get/gets/gat/gats: `entries` is filled (possibly empty)
        status,       // store/delete/touch: `store` names the outcome
        number,       // incr/decr: `number` holds the new value
        stats,        // stats: `stats_entries` is filled
        version,      // version: `text` holds the version string
        meta_hit,     // meta `mg`/`ms`/`md` with a value or explicit hit
        meta_miss,    // meta `mg` with EN: the key does not exist
        meta_status,  // meta single-word outcome (OK/NF/NS/EX/HD)
        error         // ERROR/CLIENT_ERROR/SERVER_ERROR: `error_kind` + `text`
    };

    kind type = kind::status;
    store_status store = store_status::ok;
    error_kind error = error_kind::error;
    std::uint64_t number = 0;
    std::vector<value_entry> entries;
    std::vector<std::pair<std::string, std::string>> stats_entries;
    // Meta flag tokens, in wire order (e.g. {"HD"} or {"VA","5","c12"}).
    std::vector<std::string> meta_tokens;
    // Version string, error message, or the STAT name for a single stat line.
    std::string text;
    // The value block of a meta `VA` reply, when one was present.
    std::string value;
    std::uint64_t meta_cas = 0;

    // True when the reply is one of the three error kinds. A caller that ignores
    // this can still obtain the message from `text`.
    [[nodiscard]] bool is_error() const noexcept { return type == kind::error; }
    // True when the reply carries a value block (text or meta). A meta hit with
    // a zero-length value still counts, which is why this tests the kind rather
    // than the payload size.
    [[nodiscard]] bool has_value() const noexcept {
        return type == kind::values || type == kind::meta_hit;
    }
};

// Encodes one request. A memcached text command is line-oriented, so the
// encoder writes directly into a caller buffer; the returned `written` is the
// exact byte count and the caller is responsible for writing it in one piece
// (a memcached text command is not self-delimiting on the wire).
class CRPC_API text_encoder {
public:
    explicit text_encoder() noexcept = default;

    // `set`, `add`, `replace`, `append`, `prepend`. `cas` is only emitted for
    // `cas`. Returns `invalid_argument` when the key exceeds max_key_size, and
    // `encode_error` when `out` is too small.
    [[nodiscard]] status storage(std::string_view command, std::string_view key,
                                 std::uint32_t flags, std::uint64_t exptime,
                                 std::string_view data, std::uint64_t cas,
                                 span<std::byte> out, std::size_t& written) const;

    // `get`/`gets`/`gat`/`gats` with one or more keys. `exptime` is only
    // emitted for the `gat`/`gats` form.
    [[nodiscard]] status retrieval(std::string_view command,
                                   const std::vector<std::string_view>& keys,
                                   std::uint64_t exptime, span<std::byte> out,
                                   std::size_t& written) const;

    // `delete <key> [<cas>]`.
    [[nodiscard]] status remove(std::string_view key, std::uint64_t cas,
                                span<std::byte> out, std::size_t& written) const;

    // `incr`/`decr <key> <delta>`.
    [[nodiscard]] status arithmetic(std::string_view command, std::string_view key,
                                    std::uint64_t delta, span<std::byte> out,
                                    std::size_t& written) const;

    // `touch <key> <exptime>`.
    [[nodiscard]] status touch(std::string_view key, std::uint64_t exptime,
                               span<std::byte> out, std::size_t& written) const;

    // `stats [<subcommand>]`, `version`, `flush_all [<delay>]`, `quit`,
    // `verbosity <level>`. A null subcommand omits it.
    [[nodiscard]] status simple(std::string_view command, std::string_view argument,
                                span<std::byte> out, std::size_t& written) const;

    // `mg <key> <flags...>`, `ms <key> <size> <flags...> [data]`, `md <key>`,
    // `ma <key>`, `mn`, `me <key>`. The flags are appended verbatim in order.
    [[nodiscard]] status meta(std::string_view command, std::string_view key,
                              std::uint64_t size, const std::vector<std::string_view>& flags,
                              std::string_view data, span<std::byte> out,
                              std::size_t& written) const;

    // Byte size of a command produced by the matching function, so a caller can
    // reserve exactly once.
    [[nodiscard]] static std::size_t storage_size(std::string_view command,
                                                  std::string_view key,
                                                  std::string_view data,
                                                  std::uint64_t cas) noexcept;
    [[nodiscard]] static std::size_t retrieval_size(
        std::string_view command, const std::vector<std::string_view>& keys,
        std::uint64_t exptime) noexcept;
    [[nodiscard]] static std::size_t meta_size(
        std::string_view command, std::string_view key, const std::vector<std::string_view>& flags,
        std::string_view data) noexcept;
};

// Streaming reply parser. It is stateful because a value block spans the CRLF
// that ends its header line, and because a multi-key `get` produces a sequence
// of VALUE blocks terminated by `END`.
//
//   parser.feed(chunk);
//   for (;;) {
//       text_reply reply;
//       switch (parser.next(reply)) {
//       case text_parser::result::reply_ready: handle(reply); break;
//       case text_parser::result::need_more:   goto next_chunk;
//       case text_parser::result::error:       fail(parser.error()); break;
//       }
//   }
//
// One `next()` call returns one whole reply: for `get` it returns a single
// reply whose `entries` holds every VALUE block up to `END`, so the caller does
// not have to reassemble a multi-line response.
class CRPC_API text_parser {
public:
    enum class result : std::uint8_t { need_more, reply_ready, error };

    text_parser() noexcept;
    explicit text_parser(std::size_t max_value_size) noexcept;
    text_parser(std::size_t max_value_size, std::size_t max_line_size) noexcept;

    // Appends `data`; the input need not outlive the call. A no-op after an
    // error until `reset()`.
    void feed(span<const std::byte> data) noexcept;

    // Decodes one reply. `next()` must not be called again until the previous
    // reply was fully consumed, which the parser guarantees by only reporting
    // `reply_ready` after the terminating line was seen.
    [[nodiscard]] result next(text_reply& out) noexcept;

    // A bare `END` is ambiguous on its own: it is the normal answer to a
    // retrieval that found nothing, but also a stray line in any other reply.
    // The parser defaults to the fail-closed reading (protocol_error). A caller
    // that knows it sent a retrieval command (`get`/`gets`/`gat`/`gats`) enables
    // this so an all-miss reply decodes as an empty value list. The transport
    // does that automatically from the request bytes.
    void set_allow_bare_end(bool allowed) noexcept;

    // A stream that ends in the middle of a reply is `protocol_error`.
    [[nodiscard]] status finish() noexcept;

    [[nodiscard]] rpc_code error() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] std::size_t buffered() const noexcept;
    [[nodiscard]] std::size_t max_value_size() const noexcept;
    [[nodiscard]] std::size_t max_line_size() const noexcept;

    void reset() noexcept;

private:
    enum class parse_result : std::uint8_t { need_more, ok, error };

    // Reads one CRLF-terminated line into `line_`. Returns need_more when no
    // complete line is buffered yet.
    [[nodiscard]] parse_result read_line() noexcept;
    // Dispatches a complete line. `line_` holds the line without CRLF.
    [[nodiscard]] parse_result dispatch(text_reply& out) noexcept;
    // Consumes a value block of exactly `size` bytes plus its trailing CRLF.
    [[nodiscard]] parse_result take_value(std::size_t size, std::string& out) noexcept;
    void fail(rpc_code code) noexcept;
    void consume(std::size_t bytes) noexcept;
    void maybe_compact() noexcept;

    std::vector<std::byte> buffer_;
    std::size_t offset_ = 0;
    std::size_t max_value_size_ = default_max_value_size;
    std::size_t max_line_size_ = default_max_line_size;
    bool allow_bare_end_ = false;
    rpc_code error_ = rpc_code::ok;
    // The most recently read line, without its CRLF.
    std::string line_;

    // Parser state: a multi-key `get` accumulates entries across VALUE lines
    // until END; a pending value block is read before the next line.
    enum class state : std::uint8_t { line, value_block } state_ = state::line;
    std::vector<value_entry> pending_entries_;
    std::size_t pending_value_size_ = 0;
    value_entry pending_entry_;
    // Set when the current reply is a meta hit: the value line was recognised
    // and only its data block is pending.
    text_reply pending_reply_;
};

// --- Binary protocol ---------------------------------------------------------

// The binary protocol is legacy but still required. Only the header layout and
// the opcode/status enumerations are modelled here; the full opcode set lives in
// the client, which knows which extras layout each opcode uses.
enum class binary_opcode : std::uint8_t {
    get = 0x00,
    set = 0x01,
    add = 0x02,
    replace = 0x03,
    delete_op = 0x04,
    increment = 0x05,
    decrement = 0x06,
    quit = 0x07,
    flush = 0x08,
    get_q = 0x09,
    noop = 0x0a,
    version_op = 0x0b,
    getk = 0x0c,
    getkq = 0x0d,
    append = 0x0e,
    prepend = 0x0f,
    stat = 0x10,
    touch_op = 0x1c
};

enum class binary_status : std::uint16_t {
    ok = 0x0000,
    key_not_found = 0x0001,
    key_exists = 0x0002,
    value_too_large = 0x0003,
    invalid_arguments = 0x0004,
    item_not_stored = 0x0005,
    unknown_command = 0x0081,
    out_of_memory = 0x0082
};

// A decoded binary request or response header. The caller then reads exactly
// `extras_length`, `key_length` and `body_length` bytes from the stream.
struct binary_header {
    std::uint8_t opcode = 0;
    std::uint16_t key_length = 0;
    std::uint8_t extras_length = 0;
    std::uint8_t data_type = 0;
    std::uint16_t status = 0;
    std::uint32_t body_length = 0;
    std::uint32_t opaque = 0;
    std::uint64_t cas = 0;
};

// Size of the fixed binary header.
inline constexpr std::size_t binary_header_size = 24;

// Encodes the 24-byte header. `out` must hold at least binary_header_size bytes.
[[nodiscard]] CRPC_API status encode_binary_header(span<std::byte> out,
                                                   const binary_header& header,
                                                   std::size_t& written) noexcept;

// Decodes a 24-byte header. Returns `protocol_error` when the magic does not
// match the requested direction.
[[nodiscard]] CRPC_API status decode_binary_header(span<const std::byte> in,
                                                   bool response,
                                                   binary_header& header) noexcept;

// Maps a binary status word to the library error code. An unknown status maps to
// `internal`; the caller keeps the raw word for diagnostics.
[[nodiscard]] CRPC_API rpc_code map_binary_status(std::uint16_t status) noexcept;

// Maps a binary status word to a stable, human-readable name. Unknown values
// return the empty string so a caller can fall back to the numeric word.
[[nodiscard]] CRPC_API const char* binary_status_name(std::uint16_t status) noexcept;

// --- Text helpers ------------------------------------------------------------

// Parses a non-negative decimal integer from `text`. Returns false on an empty
// string, a sign, a non-digit, or an overflow. memcached never sends a signed
// value in a counter reply, so a sign is a protocol violation.
[[nodiscard]] CRPC_API bool parse_u64(std::string_view text, std::uint64_t& out) noexcept;

// Splits one meta reply line into its space-separated tokens.
[[nodiscard]] CRPC_API std::vector<std::string> split_tokens(std::string_view line);

} // namespace memcached
} // namespace crpc

#endif // CRPC_MEMCACHED_PROTOCOL_HPP