// CRPC -- Server-Sent Events (text/event-stream) framing helpers.
//
// The HTTP/1.1 SSE subset carries one event stream per connection. This header
// owns the wire framing that is independent of the transport:
//
//   * the `Last-Event-ID: <epoch>:<seq>` cursor parser (strict: decimal u64,
//     no sign, no whitespace, one colon, bounded length);
//   * event encoding: `id: <epoch>:<seq>\ndata: ...\n\n`. Text payloads keep
//     their UTF-8 bytes (one `data:` line per line); binary payloads are
//     base64 encoded and marked with the `b64:` prefix so a reader can tell
//     them apart from text that happens to look like base64;
//   * heartbeat (`:hb\n\n`) and optional `retry:` hints.
//
// This layer allocates and never blocks. The HTTP server owns delivery,
// backpressure and the hub subscription lifecycle.
#ifndef CRPC_HTTP_SSE_HPP
#define CRPC_HTTP_SSE_HPP

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace http {
namespace sse {

// A parsed `Last-Event-ID`. `epoch` distinguishes publisher/server incarnations;
// `seq` is the last sequence the client accepted.
struct event_id {
    u64 epoch = 0;
    u64 seq = 0;
};

// Longest accepted textual cursor: two 20-digit u64 values and one colon.
inline constexpr std::size_t max_event_id_chars = 41;

// Parses `epoch:seq`. Rejects an empty value, a missing/extra colon, a sign or
// whitespace, a non-digit, a u64 overflow, and anything longer than
// `max_event_id_chars` (`rpc_code::invalid_argument`).
[[nodiscard]] CRPC_API status parse_last_event_id(std::string_view text,
                                                  event_id& out) noexcept;

// Appends one event frame. `data` is sent verbatim when it is valid UTF-8 text
// (allowing CR/LF/TAB); otherwise it is base64 encoded and prefixed with
// `b64:`. Every `data:` line is emitted with the SSE line prefix, and the frame
// ends with a blank line.
[[nodiscard]] CRPC_API status encode_event(event_id id,
                                           span<const std::byte> data,
                                           std::string& out);
[[nodiscard]] CRPC_API status encode_event(event_id id, std::string_view text,
                                           std::string& out);

// The comment-only heartbeat frame (`:hb\n\n`).
[[nodiscard]] CRPC_API std::string heartbeat();

// A `retry: <milliseconds>\n\n` hint. A non-positive value yields no bytes.
[[nodiscard]] CRPC_API std::string retry_hint(std::chrono::milliseconds value);

// Standard base64 with padding. Exposed for tests and for readers that decode
// binary events.
[[nodiscard]] CRPC_API std::string base64_encode(span<const std::byte> data);

// True when `data` is well-formed UTF-8 and contains no control character other
// than LF, CR or TAB.
[[nodiscard]] CRPC_API bool is_text(span<const std::byte> data) noexcept;

} // namespace sse
} // namespace http
} // namespace crpc

#endif // CRPC_HTTP_SSE_HPP
