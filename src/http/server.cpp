// CRPC -- HTTP server facade implementation.
//
// Layout of this translation unit:
//
//   1. small byte/string helpers;
//   2. `server_state` -- shards, listeners, the TLS context and the session
//      registry (shared by every session);
//   3. `session` -- one accepted connection: protocol sniffing, the
//      HTTP/1.1 engine, the HTTP/2 engine, the serialized write queue and the
//      shard hand-off;
//   4. the `http_server` facade.
//
// Everything a session owns that is not cross-thread state is touched on the
// owning shard only. The only cross-thread inputs are the source callbacks
// (ingress bytes, EOF/error) and write completions; both are published under
// `session::mu_` and processed by a task posted to the shard.
#include <crpc/http/server.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <crpc/http/content_coding.hpp>
#include <crpc/http/file_transfer.hpp>
#include <crpc/http/forwarded.hpp>
#include <crpc/http/sse.hpp>
#include <crpc/http2/connection.hpp>
#include <crpc/http2/frame.hpp>
#include <crpc/http2/hpack.hpp>
#include <crpc/mvc/router.hpp>
#include <crpc/net/ip.hpp>

#include "longpoll_sweep.hpp"

namespace crpc {
namespace http {
namespace {

namespace h2 = crpc::http2;

// The HTTP/2 connection preface sent by a client before its first SETTINGS
// frame. The same bytes are used for h2c and for h2 over TLS (RFC 9113 3.4).
constexpr std::string_view h2_connection_preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

[[nodiscard]] span<const std::byte> as_bytes(std::string_view text) noexcept {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                 text.size());
}

// The IP of a socket peer, or an invalid address when the transport is not a
// TCP socket (an in-process channel, for example). The host string on `remote()`
// is always an IP literal for the TCP backends, so this strict parse is the
// truth about whether the peer has an address at all.
[[nodiscard]] net::ip_address peer_ip(const endpoint& peer) noexcept {
    if (peer.type() != endpoint::kind::tcp) {
        return net::ip_address{};
    }
    result<net::ip_address> parsed = net::ip_address::parse(peer.host());
    return parsed.ok() ? std::move(parsed.value()) : net::ip_address{};
}

[[nodiscard]] char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool ci_equal(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) {
            return false;
        }
    }
    return true;
}

// True when a comma-separated header value contains `token` (case-insensitive).
// Used for `Connection: close` and `Connection: keep-alive`.
[[nodiscard]] bool header_has_token(const header_list& headers,
                                    std::string_view name,
                                    std::string_view token) noexcept {
    for (const header& field : headers) {
        if (!ci_equal(field.name, name)) {
            continue;
        }
        std::string_view value(field.value);
        std::size_t start = 0;
        for (;;) {
            const std::size_t comma = value.find(',', start);
            std::string_view item =
                comma == std::string_view::npos
                    ? value.substr(start)
                    : value.substr(start, comma - start);
            while (!item.empty() && (item.front() == ' ' || item.front() == '\t')) {
                item.remove_prefix(1);
            }
            while (!item.empty() && (item.back() == ' ' || item.back() == '\t')) {
                item.remove_suffix(1);
            }
            if (ci_equal(item, token)) {
                return true;
            }
            if (comma == std::string_view::npos) {
                return false;
            }
            start = comma + 1;
        }
    }
    return false;
}

[[nodiscard]] std::string lowercase(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(ascii_lower(c));
    }
    return out;
}

[[nodiscard]] std::string_view local_reason(int status_code) noexcept {
    switch (status_code) {
    case 411:
        return "Length Required";
    case 431:
        return "Request Header Fields Too Large";
    default:
        return mvc::reason_phrase(status_code);
    }
}

[[nodiscard]] http::response make_problem(int status_code, std::string_view detail,
                                          std::string_view instance) {
    http::response response;
    response.status_code = status_code;
    response.reason = std::string(local_reason(status_code));
    response.headers.add("Content-Type", "application/problem+json");
    std::string body = "{\"status\":";
    body += std::to_string(status_code);
    body += ",\"detail\":\"";
    for (const char c : detail) {
        if (c == '"' || c == '\\') {
            body.push_back('\\');
        }
        body.push_back(c);
    }
    body += "\",\"instance\":\"";
    for (const char c : instance) {
        if (c == '"' || c == '\\') {
            body.push_back('\\');
        }
        body.push_back(c);
    }
    body += "\"}";
    response.body = std::move(body);
    return response;
}

// SSE route lookup. The list is immutable once the server is built.
[[nodiscard]] const sse_route* find_sse_route(const std::vector<sse_route>& routes,
                                              std::string_view path) noexcept {
    for (const sse_route& route : routes) {
        if (route.path == path) {
            return &route;
        }
    }
    return nullptr;
}

// The outcome of resolving an SSE request against the push hub. `detail`
// carries the human-readable reason for a failure; it is empty on success.
enum class sse_open_error : u8 {
    ok,
    bad_request,
    conflict,
    unavailable,
};

// Long-poll route lookup. The list is immutable once the server is built.
[[nodiscard]] const longpoll_route* find_longpoll_route(
    const std::vector<longpoll_route>& routes, std::string_view path) noexcept {
    for (const longpoll_route& route : routes) {
        if (route.path == path) {
            return &route;
        }
    }
    return nullptr;
}

// Long-poll subscriber ids live in [1<<32, 1<<33); SSE allocates from 1. The
// namespace is a collision-avoidance convention, not a security boundary.
constexpr u64 k_longpoll_subscriber_base = 1ull << 32;
constexpr u64 k_longpoll_subscriber_limit = 1ull << 33;
constexpr u64 k_longpoll_subscriber_count =
    k_longpoll_subscriber_limit - k_longpoll_subscriber_base;

[[nodiscard]] bool is_longpoll_subscriber(u64 id) noexcept {
    return id >= k_longpoll_subscriber_base && id < k_longpoll_subscriber_limit;
}

// A download route match: the longest registered prefix that bounds the path at
// a '/' and the (still percent-encoded) remainder.
struct download_match {
    const download_route* route = nullptr;
    std::string relative;
};

[[nodiscard]] download_match find_download_route(
    const std::vector<download_route>& routes, std::string_view path) noexcept {
    download_match best;
    for (const download_route& route : routes) {
        if (route.path.empty()) {
            continue;
        }
        bool matched = false;
        std::string_view remainder;
        if (path.size() == route.path.size() &&
            path.compare(0, route.path.size(), route.path) == 0) {
            matched = true;
            remainder = std::string_view{};
        } else if (path.size() > route.path.size() &&
                   path.compare(0, route.path.size(), route.path) == 0 &&
                   path[route.path.size()] == '/') {
            matched = true;
            remainder = path.substr(route.path.size() + 1);
        }
        if (!matched) {
            continue;
        }
        if (best.route == nullptr || route.path.size() > best.route->path.size()) {
            best.route = &route;
            best.relative.assign(remainder.data(), remainder.size());
        }
    }
    return best;
}

// Resolves a download remainder under `root`, rejecting absolute paths,
// backslashes and any `..`/`.` segment, then verifying (after resolving
// symlinks) that the result stays inside `root` and is a regular file.
[[nodiscard]] bool resolve_download_path(const std::filesystem::path& root,
                                         std::string_view relative,
                                         std::filesystem::path& out) {
    std::string decoded;
    if (!http::percent_decode(relative, decoded).ok()) {
        return false;
    }
    if (decoded.empty() || decoded.front() == '/' ||
        decoded.find('\\') != std::string::npos) {
        return false;
    }
    std::filesystem::path candidate = root;
    std::size_t start = 0;
    while (start <= decoded.size()) {
        const std::size_t slash = decoded.find('/', start);
        const std::string_view segment =
            slash == std::string::npos
                ? std::string_view(decoded).substr(start)
                : std::string_view(decoded).substr(start, slash - start);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        candidate /= std::filesystem::path(segment);
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    std::error_code ec;
    const std::filesystem::path canonical_root =
        std::filesystem::weakly_canonical(root, ec);
    if (ec) {
        return false;
    }
    const std::filesystem::path canonical_file =
        std::filesystem::weakly_canonical(candidate, ec);
    if (ec) {
        return false;
    }
    if (std::filesystem::is_directory(canonical_file, ec) || ec) {
        return false;
    }
    // The file must be under the root: compare component-wise so `/var/wwwe`
    // cannot pass a naive string prefix of `/var/www`.
    auto root_it = canonical_root.begin();
    auto file_it = canonical_file.begin();
    for (; root_it != canonical_root.end(); ++root_it, ++file_it) {
        if (file_it == canonical_file.end() || *file_it != *root_it) {
            return false;
        }
    }
    out = canonical_file;
    return true;
}

// Media types that are already compressed (or binary); re-compressing them
// wastes CPU and can inflate the response.
[[nodiscard]] bool is_incompressible_type(std::string_view content_type) noexcept {
    const std::size_t semicolon = content_type.find(';');
    std::string_view type = content_type.substr(0, semicolon);
    while (!type.empty() && (type.back() == ' ' || type.back() == '\t')) {
        type.remove_suffix(1);
    }
    const std::size_t slash = type.find('/');
    if (slash == std::string_view::npos) {
        return false;
    }
    const std::string_view major = type.substr(0, slash);
    const std::string_view minor = type.substr(slash + 1);
    if (ci_equal(major, "image") || ci_equal(major, "video") ||
        ci_equal(major, "audio")) {
        return true;
    }
    if (!ci_equal(major, "application")) {
        return false;
    }
    return ci_equal(minor, "zip") || ci_equal(minor, "gzip") ||
           ci_equal(minor, "x-gzip") || ci_equal(minor, "zstd") ||
           ci_equal(minor, "x-zstd") || ci_equal(minor, "br") ||
           ci_equal(minor, "x-brotli") || ci_equal(minor, "octet-stream") ||
           ci_equal(minor, "x-7z-compressed") ||
           ci_equal(minor, "x-rar-compressed");
}

[[nodiscard]] bool vary_value_covers_accept_encoding(
    std::string_view value) noexcept {
    std::size_t pos = 0;
    while (pos <= value.size()) {
        while (pos < value.size() &&
               (value[pos] == ' ' || value[pos] == '\t' || value[pos] == ',')) {
            ++pos;
        }
        std::size_t end = pos;
        while (end < value.size() && value[end] != ',') {
            ++end;
        }
        std::string_view token = value.substr(pos, end - pos);
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
            token.remove_suffix(1);
        }
        if (ci_equal(token, "accept-encoding") || token == "*") {
            return true;
        }
        if (end >= value.size()) {
            break;
        }
        pos = end + 1;
    }
    return false;
}

// Adds `Accept-Encoding` to Vary without overwriting an existing Vary value:
// the token is appended to the first Vary field (and duplicates are not added).
void merge_vary_accept_encoding(http::header_list& headers) {
    bool have_vary = false;
    for (const header& field : headers) {
        if (!ci_equal(field.name, "vary")) {
            continue;
        }
        have_vary = true;
        if (vary_value_covers_accept_encoding(field.value)) {
            return;
        }
    }
    if (!have_vary) {
        headers.add("Vary", "Accept-Encoding");
        return;
    }
    http::header_list rebuilt;
    bool merged = false;
    for (const header& field : headers) {
        if (!merged && ci_equal(field.name, "vary")) {
            const std::string value =
                field.value.empty() ? std::string("Accept-Encoding")
                                    : field.value + ", Accept-Encoding";
            rebuilt.add("Vary", value);
            merged = true;
        } else {
            rebuilt.add(field.name, field.value);
        }
    }
    headers = std::move(rebuilt);
}

enum class query_result : u8 { missing, ok, invalid };

// Extracts one raw query parameter (percent decoding the value). The query is
// everything after the first '?', already split by the message layer.
[[nodiscard]] query_result query_param(std::string_view query,
                                       std::string_view name,
                                       std::string& out) {
    if (query.empty()) {
        return query_result::missing;
    }
    std::size_t pos = 0;
    for (;;) {
        const std::size_t amp = query.find('&', pos);
        const std::string_view item =
            amp == std::string_view::npos ? query.substr(pos)
                                          : query.substr(pos, amp - pos);
        const std::size_t eq = item.find('=');
        const std::string_view key = item.substr(0, eq);
        if (key == name) {
            const std::string_view raw =
                eq == std::string_view::npos ? std::string_view{} : item.substr(eq + 1);
            return http::percent_decode(raw, out).ok() ? query_result::ok
                                                       : query_result::invalid;
        }
        if (amp == std::string_view::npos) {
            return query_result::missing;
        }
        pos = amp + 1;
    }
}

[[nodiscard]] bool parse_u64_text(std::string_view text, u64& value) noexcept {
    if (text.empty()) {
        return false;
    }
    u64 result = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        const u64 digit = static_cast<u64>(c - '0');
        if (result > (~static_cast<u64>(0) - digit) / 10) {
            return false;
        }
        result = result * 10 + digit;
    }
    value = result;
    return true;
}

[[nodiscard]] bool parse_bool_text(std::string_view text, bool& value) noexcept {
    if (text == "1" || text == "true") {
        value = true;
        return true;
    }
    if (text == "0" || text == "false") {
        value = false;
        return true;
    }
    return false;
}

// PUT and PATCH carry a representation and must be framed. A POST without a
// body is meaningful (an empty trigger), so it is routed rather than rejected;
// the 411 path exists for the methods that cannot infer an empty body.
[[nodiscard]] bool method_requires_framing(http::method value) noexcept {
    return value == http::method::put || value == http::method::patch;
}

[[nodiscard]] bool response_forbids_body(int status_code) noexcept {
    return (status_code >= 100 && status_code < 200) || status_code == 204 ||
           status_code == 304;
}

struct server_state;

// --- Session -----------------------------------------------------------------

class session;

// One transport source. It holds only a weak reference: the session may be
// destroyed while a backend still has the adapter installed, and the adapter
// must never touch a dead session. While a callback runs it holds a strong
// reference, so the session cannot be destroyed underneath it.
class reader_source final : public io::source {
public:
    explicit reader_source(std::weak_ptr<session> owner) noexcept
        : owner_(std::move(owner)) {}

    std::size_t on_data(span<const std::byte> data) noexcept override;
    void on_eof() noexcept override;
    void on_error(std::error_code ec) noexcept override;

private:
    std::weak_ptr<session> owner_;
};

// One outstanding write. When it completes it publishes the result to the
// session (a lock-protected queue) and schedules shard work; it never starts
// another push directly (R1/I3).
struct write_completion final : io::completion {
    std::shared_ptr<session> owner;

    void complete(io::io_result result) noexcept override;
};

struct pending_h2_stream {
    http::request request;
    bool headers_ready = false;
    // Absolute monotonic deadline for the request body (0 = none). Set when
    // HEADERS arrive; an open stream that never reaches END_STREAM is reset at
    // this deadline instead of being bounded only by the connection idle timer,
    // which every trickled byte refreshes (No-stall rule).
    u64 deadline_ns = 0;
};

// The request-processing engine of one connection.
class session final : public std::enable_shared_from_this<session> {
public:
    session(server_state& state, std::size_t shard_index,
            std::unique_ptr<connection> transport, bool tls_mode,
            const endpoint& peer);
    ~session();

    session(const session&) = delete;
    session& operator=(const session&) = delete;

    // Installs the sources. Called once, after the owning shared_ptr exists.
    void start();

    // Called by `http_server::stop()` (any thread).
    void request_shutdown() noexcept;
    // Called by `http_server::stop()` after the drain deadline (any thread).
    void force_close() noexcept;

    // Source callbacks.
    std::size_t handle_data(span<const std::byte> data) noexcept;
    void handle_eof() noexcept;
    void handle_error(std::error_code ec) noexcept;

    // Write completion (any thread).
    void note_write_complete(bool ok) noexcept;

    // Wakes the SSE pump. Safe from any thread: it only posts a shard task, so
    // `http_server::publish()` never touches shard-only state directly.
    void wake_sse() noexcept;
    // Wakes a waiting long poll. Same contract as `wake_sse`.
    void wake_longpoll() noexcept;

    [[nodiscard]] runtime* shard() const noexcept { return shard_; }

private:
    enum class phase : u8 { detect, http1, h2_preface, h2 };

    // Shard-thread processing.
    void process();
    void consume(std::string input);
    void process_http1(std::string input);
    void handle_http1_request(http::request request);
    void process_h2(std::string input);
    void after_h2_receive(const h2::frame_header& frame, std::string data);
    void build_h2_request(u32 stream_id, const header_list& fields);
    void dispatch_h2(u32 stream_id);
    void send_h2_response(u32 stream_id, http::response& response, bool head_only);
    // Encodes and enqueues one HPACK header block as HEADERS + CONTINUATION,
    // with optional END_STREAM. Shared by `send_h2_response` and the SSE head.
    void h2_send_header_block(u32 stream_id, const std::string& block,
                              bool end_stream);
    // Sends the SSE response head (`:status 200`, `text/event-stream`, no
    // Content-Length, no END_STREAM). Returns false when HPACK encoding failed.
    [[nodiscard]] bool h2_send_sse_head(u32 stream_id);
    void h2_flush(u32 stream_id);
    void h2_resume_parked();
    void h2_send_initial_settings();
    void h2_send_settings_ack();
    void h2_send_ping_ack();
    void h2_send_ping();
    void h2_send_goaway(h2::error_code code);
    void h2_send_rst(u32 stream_id, h2::error_code code);
    void h2_send_window_update(u32 stream_id, u32 increment);
    void h2_connection_error(h2::error_code code);

    // Output.
    void enqueue(std::string bytes);
    void flush_writes();
    void maybe_finish();
    void finish_now();
    void schedule() noexcept;
    // Arms the single session timer for the nearest of: an HTTP/2 parked
    // response probe, the idle deadline, and the request deadline. One timer
    // per session because `runtime::arm_timer` is keyed by the session address.
    void arm_probe();
    void on_probe();
    void note_activity() noexcept;
    void note_request_progress() noexcept;

    // HTTP/1.1 helpers.
    [[nodiscard]] bool keep_alive(const http::request& request) const noexcept;
    void send_http1_error(int status_code, std::string_view detail,
                          const http::request& request, bool keep);
    // Long-poll error response; 503 may carry `Retry-After: 1`.
    void send_longpoll_error(int status_code, std::string_view detail,
                             const http::request& request, bool keep,
                             bool retry_after);
    // Request-side content coding: decodes a gzip/deflate body in place and
    // returns the HTTP status to answer with (200 when no change was needed).
    int decode_request_body(http::request& request);
    // Fills `request::peer_*` from the accepted connection and, when the peer
    // is a trusted proxy, resolves the forwarding chain into `request::client_*`
    // (safe defaults when no proxy is trusted). Returns the HTTP status to
    // answer with: 200 to serve the request, 400 when the chain is malformed
    // and the configured policy is `reject_400`. Shared by HTTP/1.1 and HTTP/2.
    int stamp_forwarded(http::request& request);
    // Response-side content coding: negotiates Accept-Encoding, encodes the
    // body, or replaces the response with 406.
    void apply_response_coding(const http::request& request,
                               http::response& response);

    // File download. `download_open` writes the head (and, for HEAD/416, the
    // whole response) and arms the streaming state used by `download_pump`.
    void download_open(http::request& request, const download_route& route,
                       const std::string& relative, bool keep);
    void download_pump();
    void download_complete() noexcept;
    void download_mark_output();

    // Server-Sent Events. `sse_open` runs on the shard after the request has
    // been parsed and takes ownership of the response (the connection is
    // dedicated to the event stream). `sse_open_h2` is the HTTP/2 variant: the
    // stream stays open and its events are DATA frames. Both share
    // `sse_resolve`, which validates the request and attaches the hub token.
    void sse_open(http::request request, const sse_route& route);
    void sse_open_h2(u32 stream_id, http::request request,
                     const sse_route& route);
    sse_open_error sse_resolve(const http::request& request,
                               const sse_route& route, std::string& detail);
    // The single event pump. Idempotent/re-entrancy safe; honors the per-session
    // staged-byte bound and parks one taken message instead of dropping it.
    void sse_pump();
    // The HTTP/2 pump: the same hub accounting, but each frame is a DATA frame
    // on `sse_stream_id_` and the flow-control window gates the loop.
    void sse_pump_h2();
    // Sends one event frame as a DATA frame without END_STREAM; parks the
    // remainder in the connection's send queue when the window is short.
    void sse_h2_send_frame(const std::string& frame);
    [[nodiscard]] bool sse_h2_window_exhausted() const noexcept;
    void sse_send_heartbeat();
    void sse_mark_output();
    void sse_clear_stall_if_drained();
    [[nodiscard]] std::size_t sse_staged_bytes() const noexcept;
    [[nodiscard]] std::size_t sse_egress_cap() const noexcept;
    // Detaches from the hub. Must run before the connection/token is destroyed;
    // idempotent.
    void sse_detach() noexcept;
    // Detaches and clears the SSE state without closing the connection. Used
    // when the peer resets an HTTP/2 SSE stream but the connection stays usable.
    void sse_reset() noexcept;
    // Graceful close: detach, then end the stream (HTTP/1.1 terminating chunk,
    // HTTP/2 empty DATA frame with END_STREAM) and let the write queue drain.
    // `finish_now()` is the hard close.
    void sse_close(bool graceful) noexcept;
    // Hard close after detach; used for stall detection and write failures.
    void sse_abort() noexcept;

    // HTTP/1.1 long polling. `longpoll_open` parses the request, acks the
    // cursor, attaches/retargets the hub token and either answers immediately
    // or arms one wait. `longpoll_pump` is the single take path, called from the
    // shard on a publish wake, on a probe, or after an open.
    void longpoll_open(http::request request, const longpoll_route& route);
    void longpoll_pump();
    void longpoll_send_message(const push::push_message& message, bool keep);
    void longpoll_send_timeout(bool keep);
    void longpoll_finish_wait() noexcept;
    // Session-end/release helper: keep-queue release, clear the owner and the
    // waiter counters. Idempotent and safe after a hub reset.
    void longpoll_release() noexcept;
    void longpoll_mark_output();
    [[nodiscard]] std::size_t longpoll_egress_cap() const noexcept;

    // Completion publishing helper (called under `mu_`).
    void queue_write_complete(bool ok) noexcept;

    static void process_trampoline(void* ctx) noexcept;
    static void drop_trampoline(void* ctx) noexcept;
    static void probe_trampoline(void* ctx) noexcept;
    static void arm_trampoline(void* ctx) noexcept;

    server_state& state_;
    runtime* shard_ = nullptr;
    bool tls_mode_ = false;
    // The socket peer captured at accept time (never header-derived).
    net::ip_address peer_address_;
    std::uint16_t peer_port_ = 0;

    std::unique_ptr<connection> transport_;
    std::unique_ptr<io::tls_stream> tls_;
    std::unique_ptr<reader_source> source_;
    io::sink* out_sink_ = nullptr;

    // Cross-thread state.
    std::mutex mu_;
    std::string ingress_;
    bool read_paused_ = false;
    bool input_closed_ = false;
    std::deque<bool> write_completions_;

    // Shard-only state.
    phase phase_ = phase::detect;
    std::string detect_buf_;
    http::http_parser parser_;
    h2::connection h2_;
    std::string h2_buf_;
    std::unordered_map<u32, pending_h2_stream> h2_streams_;
    std::unordered_set<u32> h2_end_pending_;
    u32 h2_last_peer_stream_ = 0;
    // Last RST_STREAM bookkeeping value we already accounted for. A locally
    // generated RST also updates the connection's `last_reset_stream`, so this
    // prevents the inbound path from re-processing our own reset.
    u32 h2_last_reset_seen_ = 0;
    bool h2_goaway_sent_ = false;
    bool h2_settings_sent_ = false;
    bool probe_armed_ = false;
    // The deadline the armed timer actually carries. A later arm request whose
    // nearest deadline is earlier must re-arm, otherwise an idle deadline armed
    // at accept time starves a request deadline that starts afterwards.
    u64 armed_deadline_ns_ = 0;
    // Absolute monotonic deadlines in nanoseconds (0 = not armed). The idle
    // deadline bounds a connection that sends nothing; the request deadline
    // bounds an incomplete request (slowloris, No-stall rule).
    u64 idle_deadline_ns_ = 0;
    u64 request_deadline_ns_ = 0;

    std::deque<std::string> out_;
    std::size_t out_bytes_ = 0;
    bool write_in_flight_ = false;
    std::string in_flight_;
    bool write_failed_ = false;
    bool sent_100_ = false;
    bool stop_reading_ = false;
    bool close_when_drained_ = false;
    std::atomic<bool> shutdown_requested_{false};
    bool finished_ = false;

    // SSE state. All of this is shard-only except `sse_token_`, which is only
    // touched on the shard and always under `server_state::hub_mutex`.
    // `sse_stream_id_` is the HTTP/2 stream carrying the event stream (0 when
    // the transport is HTTP/1.1). The rest is shared by both transports.
    u32 sse_stream_id_ = 0;
    http::chunked_encoder sse_encoder_{};
    std::atomic<bool> sse_active_{false};
    bool sse_pumping_ = false;
    push::subscriber_id sse_subscriber_ = 0;
    push::hub::connection_id sse_token_ = 0;
    std::string sse_topic_;
    // Highest sequence handed to this subscriber (across reconnects). A
    // Last-Event-ID cursor is clamped to it before `hub.ack`, so a client
    // cannot acknowledge an event the server never delivered.
    u64 sse_delivered_high_water_ = 0;
    // A message taken from the hub that did not fit under the egress bound.
    // It stays here (not in the hub, not dropped) until the queue drains.
    push::push_message sse_pending_{};
    std::size_t sse_pending_bytes_ = 0;
    bool sse_has_pending_ = false;
    u64 sse_heartbeat_ns_ = 0;
    // Non-zero while staged output has not drained; fires `slow_timeout`.
    u64 sse_stall_deadline_ns_ = 0;

    // Download state. `download_active_` is atomic because the transport source
    // callback (any thread) consults it to refuse further ingress; the rest is
    // shard-only. A download keeps the idle deadline armed (refreshed by write
    // progress) and uses its own output stall deadline, never the SSE one.
    std::unique_ptr<download_source> download_;
    std::atomic<bool> download_active_{false};
    bool download_keep_alive_ = true;
    std::string download_buffer_;
    u64 output_stall_deadline_ns_ = 0;

    // Long-poll state. `longpoll_active_` is the wait flag read by the publish
    // thread (it only posts a shard task), so it is atomic. Everything else is
    // shard-only. A normal 200/204 keeps the token: only session end calls
    // `longpoll_release()` (keep-queue).
    std::atomic<bool> longpoll_active_{false};
    bool longpoll_response_active_ = false;
    push::subscriber_id longpoll_subscriber_ = 0;
    push::hub::connection_id longpoll_token_ = 0;
    std::string longpoll_topic_;
    bool longpoll_reliable_ = false;
    bool longpoll_keep_ = true;
    u64 longpoll_epoch_ = 0;
    u64 longpoll_deadline_ns_ = 0;
};

// --- Server state ------------------------------------------------------------

struct server_state {
    http_server_options options;
    const mvc::router* router = nullptr;

    // HTTP push. One hub per shard. A topic is owned by exactly one hub
    // (`push::topic_shard`), so only that hub holds its subscriptions and queue
    // and every topic keeps a single writer. `hub_mutex` serializes every
    // access: a session on any shard pumps the hub of its topic (a request is
    // routed to the owning shard, not to the shard that accepted it) and a
    // publish is posted to the owning shard. No hub method blocks.
    std::vector<sse_route> sse_routes;
    // File download routes. Immutable once the server is built.
    std::vector<download_route> download_routes;
    std::recursive_mutex hub_mutex;
    std::vector<std::unique_ptr<push::hub>> hubs;
    // Per (subscriber, topic) high water: the largest seq ever handed to that
    // subscriber. Reconnect cursors are clamped to this value before acking.
    std::map<std::pair<u64, std::string>, u64> sse_delivered;
    std::atomic<u64> next_sse_subscriber{1};
    std::atomic<std::size_t> sse_connections{0};
    std::atomic<std::size_t> sse_events_delivered{0};
    std::atomic<std::size_t> sse_slow_closes{0};
    // SSE subscriber ids currently subscribed, so a long-poll request cannot
    // take over the same logical id. A long-poll owner is checked from the SSE
    // open path in the other direction.
    // Reference count: a reconnect with the same id can overlap the old
    // stream's detach. The long-poll conflict check only cares whether any SSE
    // transport still holds the id.
    std::unordered_map<u64, std::size_t> sse_subscribers;

    // HTTP long polling (single shard). Every field below is protected by
    // `hub_mutex` because publishes and opens run on different threads.
    std::vector<longpoll_route> longpoll_routes;
    struct longpoll_owner {
        std::weak_ptr<session> owner;
        push::hub::connection_id token = push::hub::invalid_connection;
    };
    std::unordered_map<u64, longpoll_owner> longpoll_owners;
    // A subscriber with an in-flight poll; a second concurrent poll is 409.
    std::unordered_set<u64> longpoll_busy;
    // Per (subscriber, topic) high water: the largest reliable seq handed out
    // by a long poll. A cursor is clamped to it before ack and a gap triggers
    // `retransmit_front`.
    std::map<std::pair<u64, std::string>, u64> longpoll_delivered;
    // Last poll timestamp (monotonic ns) per subscriber for the TTL sweep.
    std::unordered_map<u64, u64> longpoll_last_seen;
    // Tombstones for ids removed by the TTL sweep: a client that reuses the old
    // id after the subscription died must be handed a fresh server id (its old
    // sequence space is gone). Bounded to max(max_waiters, 1024) entries.
    std::unordered_map<u64, u64> longpoll_lost;
    u64 next_longpoll_sweep_ns = 0;
    std::atomic<u64> next_longpoll_subscriber{k_longpoll_subscriber_base};
    std::atomic<std::size_t> longpoll_replay_window{0};
    std::atomic<std::size_t> longpoll_waiters{0};
    std::atomic<std::size_t> longpoll_events{0};
    std::atomic<std::size_t> longpoll_timeouts{0};
    std::atomic<std::size_t> longpoll_ack_clamped{0};
    std::atomic<std::size_t> longpoll_conflicts{0};
    std::atomic<std::size_t> longpoll_restarts{0};
    std::atomic<std::size_t> longpoll_overloaded{0};

    void longpoll_forget(push::subscriber_id id) noexcept {
        longpoll_owners.erase(id);
        for (auto it = longpoll_delivered.begin();
             it != longpoll_delivered.end();) {
            if (it->first.first == id) {
                it = longpoll_delivered.erase(it);
            } else {
                ++it;
            }
        }
        longpoll_last_seen.erase(id);
        longpoll_lost.erase(id);
    }

    // Server-owned housekeeping timer. TTL cleanup must not depend on another
    // poll arriving: the last long-poll connection may already be gone. The
    // timer lives on the single shard runtime; `keepalive` is held by the
    // timer entry so a callback can never run against a destroyed context.
    struct longpoll_keepalive : std::enable_shared_from_this<longpoll_keepalive> {
        server_state* owner = nullptr;
        std::atomic<bool> stopped{false};
    };

    std::shared_ptr<longpoll_keepalive> longpoll_keepalive_timer;

    [[nodiscard]] static u64 longpoll_timer_period_ns(
        const http_server_options& opts) noexcept {
        const auto ttl_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                opts.longpoll.subscriber_ttl)
                .count();
        if (ttl_ns <= 0) {
            return 0;
        }
        u64 period = static_cast<u64>(ttl_ns);
        constexpr u64 k_max_period = 30ull * 1000ull * 1000ull * 1000ull;
        if (period > k_max_period) {
            period = k_max_period;
        }
        return period;
    }

    void arm_longpoll_timer() noexcept {
        if (shards.empty() || longpoll_routes.empty()) {
            return;
        }
        const u64 period = longpoll_timer_period_ns(options);
        if (period == 0) {
            return;
        }
        try {
            if (!longpoll_keepalive_timer) {
                longpoll_keepalive_timer =
                    std::make_shared<longpoll_keepalive>();
                longpoll_keepalive_timer->owner = this;
            } else {
                longpoll_keepalive_timer->stopped.store(
                    false, std::memory_order_release);
            }
            const u64 now = shards[0]->now_ns();
            (void)shards[0]->arm_timer(
                longpoll_keepalive_timer.get(), now + period,
                &server_state::longpoll_timer_trampoline,
                longpoll_keepalive_timer.get(),
                std::static_pointer_cast<void>(longpoll_keepalive_timer));
        } catch (...) {
        }
    }

    void stop_longpoll_timer() noexcept {
        if (!longpoll_keepalive_timer) {
            return;
        }
        longpoll_keepalive_timer->stopped.store(true,
                                                std::memory_order_release);
        if (!shards.empty()) {
            (void)shards[0]->cancel_timer(longpoll_keepalive_timer.get());
        }
    }

    static void longpoll_timer_trampoline(void* ctx) noexcept {
        auto* keeper = static_cast<longpoll_keepalive*>(ctx);
        if (keeper == nullptr || keeper->owner == nullptr ||
            keeper->stopped.load(std::memory_order_acquire)) {
            return;
        }
        server_state& st = *keeper->owner;
        if (st.shards.empty()) {
            return;
        }
        const u64 now = st.shards[0]->now_ns();
        try {
            std::lock_guard<std::recursive_mutex> lk(st.hub_mutex);
            st.longpoll_sweep(now);
        } catch (...) {
        }
        if (keeper->stopped.load(std::memory_order_acquire) ||
            st.shards.empty()) {
            return;
        }
        const u64 period = longpoll_timer_period_ns(st.options);
        if (period == 0) {
            return;
        }
        try {
            const std::shared_ptr<longpoll_keepalive> keep =
                keeper->shared_from_this();
            (void)st.shards[0]->arm_timer(
                keeper, now + period,
                &server_state::longpoll_timer_trampoline, keeper,
                std::static_pointer_cast<void>(keep));
        } catch (...) {
        }
    }

    void longpoll_sweep(u64 now_ns) noexcept {
        const std::int64_t ttl_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                options.longpoll.subscriber_ttl)
                .count();
        if (ttl_ns <= 0) {
            return;
        }
        if (now_ns < next_longpoll_sweep_ns) {
            return;
        }
        const u64 ttl = static_cast<u64>(ttl_ns);
        const u64 period = std::min<u64>(ttl, 30000000000ull); // 30 s
        next_longpoll_sweep_ns = now_ns + period;

        // UNION of owners and last_seen: an owner without last_seen is a
        // partial-request residue and is swept immediately; an ownerless
        // subscription (normal keep-queue session end) is swept after TTL.
        std::vector<push::subscriber_id> victims;
        try {
            detail::collect_expired_longpoll_ids(
                longpoll_owners, longpoll_last_seen, longpoll_busy, now_ns, ttl,
                victims);
        } catch (...) {
            return;
        }
        for (const push::subscriber_id id : victims) {
            if (longpoll_busy.find(id) != longpoll_busy.end()) {
                continue;
            }
            const auto seen = longpoll_last_seen.find(id);
            const bool owner = longpoll_owners.find(id) != longpoll_owners.end();
            const bool expired =
                owner ? (seen == longpoll_last_seen.end() ||
                         (now_ns >= seen->second &&
                          now_ns - seen->second >= ttl))
                      : (seen != longpoll_last_seen.end() &&
                         now_ns >= seen->second &&
                         now_ns - seen->second >= ttl);
            if (!expired) {
                continue;
            }
            for (auto& h : hubs) {
                if (h) {
                    (void)h->unsubscribe_all(id);
                }
            }
            longpoll_owners.erase(id);
            for (auto dit = longpoll_delivered.begin();
                 dit != longpoll_delivered.end();) {
                if (dit->first.first == id) {
                    dit = longpoll_delivered.erase(dit);
                } else {
                    ++dit;
                }
            }
            longpoll_last_seen.erase(id);
            try {
                longpoll_lost[id] = now_ns;
                const std::size_t tombstone_cap =
                    options.longpoll.max_waiters > 1024
                        ? options.longpoll.max_waiters
                        : 1024;
                while (longpoll_lost.size() > tombstone_cap) {
                    auto oldest = longpoll_lost.begin();
                    for (auto candidate = longpoll_lost.begin();
                         candidate != longpoll_lost.end(); ++candidate) {
                        if (candidate->second < oldest->second) {
                            oldest = candidate;
                        }
                    }
                    longpoll_lost.erase(oldest);
                }
            } catch (...) {
            }
        }
    }

    std::vector<std::unique_ptr<runtime>> shards;
    std::atomic<std::size_t> next_shard{0};

    std::atomic<bool> accepting{false};

    // Statistics.
    std::atomic<std::size_t> accepted{0};
    std::atomic<std::size_t> handled{0};
    std::atomic<std::size_t> responded{0};
    std::atomic<std::size_t> bad{0};
    std::atomic<std::size_t> h2_connections{0};
    std::atomic<std::size_t> tls_connections{0};
    std::atomic<std::size_t> rejected{0};
    std::atomic<std::size_t> timed_out{0};
    std::atomic<std::size_t> responses_compressed{0};
    std::atomic<std::size_t> requests_decoded{0};
    std::atomic<std::size_t> downloads_served{0};

    struct pending_listener {
        endpoint ep;
        backend_factory* backend = nullptr;
        bool tls = false;
    };
    std::vector<pending_listener> pending_listeners;
    std::vector<std::unique_ptr<listener>> listeners;

    bool has_tls = false;
    io::tls_config tls_config{};
    std::unique_ptr<io::tls_context> tls_context;

    std::mutex sessions_mutex;
    std::vector<std::shared_ptr<session>> sessions;
    std::condition_variable drain_cv;

    // One acceptor per listener so the session knows whether its transport
    // speaks TLS. A single acceptor cannot tell which listener accepted.
    struct acceptor final : accept_sink {
        acceptor(server_state& owner, bool tls) noexcept : owner_(owner), tls_(tls) {}
        void on_accept(std::unique_ptr<connection> c) noexcept override {
            owner_.on_accept(std::move(c), tls_);
        }
        server_state& owner_;
        bool tls_;
    };
    std::vector<std::unique_ptr<acceptor>> acceptors;

    explicit server_state(http_server_options opt) : options(std::move(opt)) {}

    // The hub that owns `topic`. Must be called under `hub_mutex`. The HTTP
    // server never declares a shard key, so the owner is the plain topic hash;
    // every shard computes the same owner from the same topic name, which is
    // what lets a request, a publish and a reconnect agree on one hub.
    [[nodiscard]] push::hub* hub_for(std::string_view topic) noexcept {
        if (hubs.empty()) {
            return nullptr;
        }
        const u32 owner =
            push::topic_shard(topic, 0, static_cast<u32>(hubs.size()));
        return hubs[owner].get();
    }

    void on_accept(std::unique_ptr<connection> transport, bool use_tls) noexcept {
        if (transport == nullptr) {
            return;
        }
        if (!accepting.load(std::memory_order_acquire) || shards.empty() ||
            (use_tls && tls_context == nullptr)) {
            transport->close();
            return;
        }
        try {
            const std::size_t index = static_cast<std::size_t>(
                next_shard.fetch_add(1, std::memory_order_relaxed)) %
                shards.size();
            // `remote()` is captured before the transport is moved into the
            // session: it is the one trustworthy answer to "who am I talking
            // to", and it must never come from request bytes.
            const endpoint peer = transport->remote();
            auto value = std::make_shared<session>(*this, index,
                                                   std::move(transport), use_tls,
                                                   peer);
            {
                std::lock_guard<std::mutex> lk(sessions_mutex);
                // The bound is checked and the session inserted under one lock,
                // so concurrent accepts cannot overshoot it.
                if (options.max_connections != 0 &&
                    sessions.size() >= options.max_connections) {
                    rejected.fetch_add(1, std::memory_order_relaxed);
                    value.reset();
                } else {
                    sessions.push_back(value);
                }
            }
            if (!value) {
                return;
            }
            accepted.fetch_add(1, std::memory_order_relaxed);
            if (use_tls) {
                tls_connections.fetch_add(1, std::memory_order_relaxed);
            }
            value->start();
        } catch (...) {
            // Allocation failure: the transport is closed by the destructor.
        }
    }

    void remove_session(const std::shared_ptr<session>& value) noexcept {
        std::lock_guard<std::mutex> lk(sessions_mutex);
        for (auto it = sessions.begin(); it != sessions.end(); ++it) {
            if (it->get() == value.get()) {
                sessions.erase(it);
                break;
            }
        }
        drain_cv.notify_all();
    }

    [[nodiscard]] std::vector<std::shared_ptr<session>> snapshot() {
        std::lock_guard<std::mutex> lk(sessions_mutex);
        return sessions;
    }

    [[nodiscard]] std::size_t active_sessions() {
        std::lock_guard<std::mutex> lk(sessions_mutex);
        return sessions.size();
    }
};

// --- Cross-shard publish -----------------------------------------------------

// A publish is routed to the hub of the shard that owns the topic. The
// application thread never touches a hub directly: it copies the payload and
// posts a task to the owning shard, which takes `hub_mutex`, publishes, then
// wakes every SSE / long-poll session. The task owns its copy, so the caller's
// buffers may go away immediately.
//
// `publish()` waits for the task to finish queueing the message. A poll that
// is sent after `publish()` returns must observe the message, and the shard's
// write-completion task may pick the poll's bytes out of the session ingress
// buffer before the publish task runs; waiting closes that window. The wait is
// on the shard's task queue, never on a subscriber: the hub's `publish` does
// not block.
struct publish_context {
    server_state* state = nullptr;
    std::string topic;
    std::vector<std::byte> body;
    push::delivery_mode mode = push::delivery_mode::reliable;
    // Shared so the caller can wait even if the shard stops and the drop hook
    // releases the task first.
    std::shared_ptr<std::mutex> mu = std::make_shared<std::mutex>();
    std::shared_ptr<std::condition_variable> cv =
        std::make_shared<std::condition_variable>();
    std::shared_ptr<bool> done = std::make_shared<bool>(false);
};

void publish_run(server_state& state, const std::string& topic,
                 const std::vector<std::byte>& body,
                 push::delivery_mode mode) noexcept {
    try {
        std::vector<std::shared_ptr<session>> snapshot;
        {
            std::lock_guard<std::mutex> lk(state.sessions_mutex);
            snapshot = state.sessions;
        }
        {
            std::lock_guard<std::recursive_mutex> lk(state.hub_mutex);
            push::hub* hub = state.hub_for(topic);
            if (hub != nullptr) {
                hub->publish(topic,
                             span<const std::byte>(body.data(), body.size()),
                             mode);
            }
        }
        // The publish completes before any wake, so a session that pumps on the
        // wake always sees the queued message. A wake only posts shard work.
        for (const std::shared_ptr<session>& value : snapshot) {
            value->wake_sse();
            value->wake_longpoll();
        }
    } catch (...) {
    }
}

void publish_trampoline(void* ctx) noexcept {
    auto* task = static_cast<publish_context*>(ctx);
    publish_run(*task->state, task->topic, task->body, task->mode);
    {
        std::lock_guard<std::mutex> lk(*task->mu);
        *task->done = true;
    }
    task->cv->notify_all();
    delete task;
}

void publish_drop(void* ctx) noexcept {
    auto* task = static_cast<publish_context*>(ctx);
    {
        std::lock_guard<std::mutex> lk(*task->mu);
        *task->done = true;
    }
    task->cv->notify_all();
    delete task;
}

// --- reader_source / write_completion ---------------------------------------

std::size_t reader_source::on_data(span<const std::byte> data) noexcept {
    std::shared_ptr<session> owner = owner_.lock();
    if (!owner) {
        return data.size();
    }
    return owner->handle_data(data);
}

void reader_source::on_eof() noexcept {
    std::shared_ptr<session> owner = owner_.lock();
    if (owner) {
        owner->handle_eof();
    }
}

void reader_source::on_error(std::error_code ec) noexcept {
    std::shared_ptr<session> owner = owner_.lock();
    if (owner) {
        owner->handle_error(ec);
    }
}

void write_completion::complete(io::io_result result) noexcept {
    if (owner) {
        owner->note_write_complete(result.ok());
    }
    delete this;
}

// --- session: construction and sources --------------------------------------

session::session(server_state& state, std::size_t shard_index,
                 std::unique_ptr<connection> transport, bool tls_mode,
                 const endpoint& peer)
    : state_(state),
      shard_(shard_index < state.shards.size() ? state.shards[shard_index].get()
                                               : nullptr),
      tls_mode_(tls_mode),
      peer_address_(peer_ip(peer)),
      peer_port_(peer.type() == endpoint::kind::tcp ? peer.port() : 0),
      transport_(std::move(transport)),
      parser_(http::http_parser::kind::request, state.options.limits),
      h2_([&state] {
          h2::connection_options options;
          options.role = h2::connection_role::server;
          options.local_settings.max_concurrent_streams =
              state.options.h2_max_concurrent_streams;
          options.local_settings.initial_window_size =
              state.options.h2_initial_window_size;
          options.local_settings.max_frame_size = state.options.h2_max_frame_size;
          options.local_settings.max_header_list_size =
              state.options.h2_max_header_list_size;
          options.max_send_queue_bytes = state.options.h2_max_send_queue_bytes;
          return options;
      }()) {}

session::~session() {
    if (tls_) {
        tls_->close();
    }
    if (transport_) {
        transport_->bind_source(nullptr);
        transport_->close();
    }
}

void session::start() {
    source_ = std::make_unique<reader_source>(weak_from_this());
    if (shard_ == nullptr) {
        return;
    }
    if (tls_mode_) {
        if (state_.tls_context == nullptr) {
            return;
        }
        std::error_code ec;
        tls_ = io::tls_stream::create(transport_->sink(), *state_.tls_context,
                                      std::string(), ec);
        if (!tls_) {
            transport_->close();
            return;
        }
        out_sink_ = tls_.get();
        tls_->bind_source(source_.get());
        transport_->bind_source(tls_.get());
    } else {
        out_sink_ = &transport_->sink();
        transport_->bind_source(source_.get());
    }
    // The idle bound is armed on the owning shard: session deadlines are
    // shard-only state, and start() runs on the accept thread.
    std::shared_ptr<session> self = weak_from_this().lock();
    if (self) {
        auto* holder = new (std::nothrow) std::shared_ptr<session>(std::move(self));
        if (holder != nullptr &&
            !shard_->post(&session::arm_trampoline, holder,
                          &session::drop_trampoline)) {
            delete holder;
        }
    }
}

void session::request_shutdown() noexcept {
    shutdown_requested_.store(true, std::memory_order_release);
    schedule();
}

void session::force_close() noexcept {
    // Thread-safe hard stop: close cancels delivery and any in-flight write.
    // Any armed probe timer is invalidated by the caller retaining the session
    // until the shards are joined (see http_server::stop).
    if (shard_ != nullptr) {
        (void)shard_->cancel_timer(this);
    }
    if (transport_) {
        transport_->close();
    }
}

std::size_t session::handle_data(span<const std::byte> data) noexcept {
    std::size_t accepted = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (input_closed_) {
            return 0;
        }
        if (read_paused_ || download_active_.load(std::memory_order_acquire)) {
            return 0;
        }
        const std::size_t cap = state_.options.ingress_cap_bytes;
        const std::size_t room = cap > ingress_.size() ? cap - ingress_.size() : 0;
        accepted = std::min(room, data.size());
        ingress_.append(reinterpret_cast<const char*>(data.data()), accepted);
        if (accepted < data.size()) {
            read_paused_ = true;
        }
    }
    if (accepted != 0) {
        schedule();
    }
    return accepted;
}

void session::handle_eof() noexcept {
    {
        std::lock_guard<std::mutex> lk(mu_);
        input_closed_ = true;
    }
    schedule();
}

void session::handle_error(std::error_code ec) noexcept {
    (void)ec;
    {
        std::lock_guard<std::mutex> lk(mu_);
        input_closed_ = true;
    }
    schedule();
}

void session::note_write_complete(bool ok) noexcept {
    queue_write_complete(ok);
    schedule();
}

void session::queue_write_complete(bool ok) noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    write_completions_.push_back(ok);
}

// --- session: scheduling -----------------------------------------------------

void session::schedule() noexcept {
    std::shared_ptr<session> self = weak_from_this().lock();
    if (!self) {
        return;
    }
    if (shard_ == nullptr) {
        return;
    }
    auto* holder = new (std::nothrow) std::shared_ptr<session>(std::move(self));
    if (holder == nullptr) {
        return;
    }
    // If the shard stops before running the task, the runtime calls the drop
    // hook instead: without it the holder (and the session it keeps alive)
    // leaked whenever a wakeup was queued during shutdown.
    if (!shard_->post(&session::process_trampoline, holder,
                      &session::drop_trampoline)) {
        delete holder;
    }
}

void session::process_trampoline(void* ctx) noexcept {
    std::unique_ptr<std::shared_ptr<session>> holder(
        static_cast<std::shared_ptr<session>*>(ctx));
    (*holder)->process();
}

void session::drop_trampoline(void* ctx) noexcept {
    // The task will never run: release the holder (and the session reference it
    // owns) without touching the session itself.
    delete static_cast<std::shared_ptr<session>*>(ctx);
}

void session::probe_trampoline(void* ctx) noexcept {
    auto* self = static_cast<session*>(ctx);
    self->on_probe();
}

void session::arm_trampoline(void* ctx) noexcept {
    std::unique_ptr<std::shared_ptr<session>> holder(
        static_cast<std::shared_ptr<session>*>(ctx));
    session& self = **holder;
    self.note_activity();
    self.arm_probe();
}

void session::process() {
    if (finished_) {
        return;
    }
    for (;;) {
        std::string input;
        std::deque<bool> completions;
        bool peer_closed = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            input.swap(ingress_);
            completions.swap(write_completions_);
            peer_closed = input_closed_;
        }
        bool sse_write_failed = false;
        for (const bool ok : completions) {
            write_in_flight_ = false;
            if (!ok) {
                write_failed_ = true;
                close_when_drained_ = true;
                stop_reading_ = true;
                if (sse_active_.load(std::memory_order_acquire)) {
                    sse_write_failed = true;
                }
            } else if (download_active_.load(std::memory_order_acquire)) {
                // A completed write proves the downloading peer is alive: both
                // the idle deadline and the download output stall deadline are
                // refreshed from that progress, so a slow-but-draining peer is
                // never cut off at a fixed offset from the first chunk.
                note_activity();
                download_mark_output();
            }
        }
        if (sse_write_failed) {
            // A failed heartbeat/event write is the primary dead-peer signal
            // for an idle SSE stream. Detach before the transport goes away.
            sse_abort();
            return;
        }
        // Once the response is an event stream the connection is dedicated to
        // it: later bytes are not a pipelined request (HTTP/1.1 cannot answer
        // them in order) and must not be parsed. A streamed download likewise
        // holds the connection until its body is written, and a poll in flight
        // keeps the response order intact. HTTP/2 is different: its streams
        // multiplex on one connection, so inbound frames (WINDOW_UPDATE,
        // RST_STREAM, PING) must keep being processed while an SSE stream is
        // open.
        const bool http1_dedicated =
            sse_active_.load(std::memory_order_acquire) && sse_stream_id_ == 0;
        if (!input.empty() && !http1_dedicated &&
            !download_active_.load(std::memory_order_acquire) &&
            !longpoll_active_.load(std::memory_order_acquire)) {
            note_activity();
            consume(std::move(input));
            arm_probe();
        }
        if (peer_closed && sse_active_.load(std::memory_order_acquire)) {
            sse_close(false);
        }
        if (sse_active_.load(std::memory_order_acquire)) {
            sse_pump();
        }
        if (longpoll_active_.load(std::memory_order_acquire)) {
            longpoll_pump();
        }
        const bool shutdown =
            shutdown_requested_.load(std::memory_order_acquire);
        if (shutdown) {
            stop_reading_ = true;
            if (sse_active_.load(std::memory_order_acquire)) {
                sse_close(true);
            }
            if (phase_ == phase::h2) {
                h2_send_goaway(h2::error_code::no_error);
            }
            close_when_drained_ = true;
        }
        if (download_active_.load(std::memory_order_acquire)) {
            download_pump();
            if (out_.empty() && !write_in_flight_) {
                output_stall_deadline_ns_ = 0;
            }
        }
        flush_writes();
        maybe_finish();
        if (finished_) {
            return;
        }
        if (sse_active_.load(std::memory_order_acquire)) {
            sse_clear_stall_if_drained();
            arm_probe();
        }
        if (longpoll_response_active_ && out_.empty() && !write_in_flight_) {
            longpoll_response_active_ = false;
            output_stall_deadline_ns_ = 0;
        }
        if (longpoll_response_active_ || longpoll_active_.load()) {
            arm_probe();
        }
        bool resume = false;
        bool over_egress = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            // A long-poll response may have a span-local bound below the
            // generic session cap; honor the tighter one while it is staged.
            const std::size_t route_cap =
                longpoll_response_active_ ? longpoll_egress_cap() : 0;
            const std::size_t generic_cap = state_.options.egress_cap_bytes;
            const bool under_caps =
                (generic_cap == 0 || out_bytes_ <= generic_cap) &&
                (route_cap == 0 || out_bytes_ <= route_cap);
            if (read_paused_ && ingress_.empty() && !close_when_drained_ &&
                !stop_reading_ && !download_active_.load(std::memory_order_acquire) &&
                !longpoll_active_.load(std::memory_order_acquire) && under_caps) {
                read_paused_ = false;
                resume = true;
            }
            // Bytes were put back into the ingress buffer because the write
            // queue crossed its cap. Keep them there until a completion drains
            // the queue: looping now would re-parse the whole batch and rebuild
            // the unbounded queue the cap exists to prevent.
            over_egress =
                (generic_cap != 0 && out_bytes_ > generic_cap) ||
                (route_cap != 0 && out_bytes_ > route_cap);
            if (ingress_.empty() || over_egress ||
                download_active_.load(std::memory_order_acquire) ||
                longpoll_active_.load(std::memory_order_acquire)) {
                // A resumed pause must still reach the backend even when the
                // ingress buffer is empty; otherwise a peer that was paused by
                // a full buffer never delivers again.
                if (resume) {
                    if (tls_) {
                        tls_->resume_read();
                    }
                    if (transport_) {
                        transport_->resume_read();
                    }
                }
                break;
            }
        }
        if (resume) {
            if (tls_) {
                tls_->resume_read();
            }
            if (transport_) {
                transport_->resume_read();
            }
        }
    }
}

// --- session: protocol sniffing ---------------------------------------------

void session::consume(std::string input) {
    if (phase_ == phase::detect) {
        if (tls_mode_) {
            if (tls_ == nullptr) {
                close_when_drained_ = true;
                stop_reading_ = true;
                return;
            }
            if (!tls_->handshake_complete()) {
                // Plaintext cannot arrive before the handshake completes; keep
                // the bytes only to be safe against a decorator bug.
                detect_buf_.append(input);
                return;
            }
            const std::string alpn = tls_->alpn_protocol();
            if (alpn == "h2") {
                phase_ = phase::h2_preface;
            } else if (alpn.empty() || alpn == "http/1.1") {
                phase_ = phase::http1;
            } else {
                close_when_drained_ = true;
                stop_reading_ = true;
                return;
            }
            detect_buf_.append(input);
            input = std::move(detect_buf_);
            detect_buf_.clear();
        } else {
            detect_buf_.append(input);
            const std::size_t compare =
                std::min(detect_buf_.size(), h2_connection_preface.size());
            const bool prefix_ok =
                std::memcmp(detect_buf_.data(), h2_connection_preface.data(),
                            compare) == 0;
            if (!prefix_ok) {
                phase_ = phase::http1;
                input = std::move(detect_buf_);
                detect_buf_.clear();
            } else if (detect_buf_.size() >= h2_connection_preface.size()) {
                phase_ = phase::h2_preface;
                input = std::move(detect_buf_);
                detect_buf_.clear();
            } else {
                return;
            }
        }
    }

    if (phase_ == phase::http1) {
        process_http1(std::move(input));
        return;
    }

    if (phase_ == phase::h2_preface) {
        h2_buf_.append(input);
        if (h2_buf_.size() < h2_connection_preface.size()) {
            return;
        }
        if (std::memcmp(h2_buf_.data(), h2_connection_preface.data(),
                        h2_connection_preface.size()) != 0) {
            h2_connection_error(h2::error_code::protocol_error);
            return;
        }
        h2_buf_.erase(0, h2_connection_preface.size());
        phase_ = phase::h2;
        state_.h2_connections.fetch_add(1, std::memory_order_relaxed);
        std::string rest = std::move(h2_buf_);
        h2_buf_.clear();
        h2_send_initial_settings();
        if (!rest.empty()) {
            process_h2(std::move(rest));
        }
        return;
    }

    if (phase_ == phase::h2) {
        process_h2(std::move(input));
    }
}

// --- session: HTTP/1.1 -------------------------------------------------------

bool session::keep_alive(const http::request& request) const noexcept {
    const bool wants_close =
        header_has_token(request.headers, "connection", "close");
    if (request.version == http_version::http_1_0) {
        return !wants_close &&
               header_has_token(request.headers, "connection", "keep-alive");
    }
    return !wants_close;
}

void session::send_http1_error(int status_code, std::string_view detail,
                               const http::request& request, bool keep) {
    http::response response =
        make_problem(status_code, detail, request.path);
    response.version = request.version;
    if (!keep) {
        response.headers.add("Connection", "close");
    }
    std::string wire;
    if (!http::serialize(response, wire).ok()) {
        wire = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n"
               "Connection: close\r\n\r\n";
        keep = false;
    }
    enqueue(std::move(wire));
    state_.responded.fetch_add(1, std::memory_order_relaxed);
    if (!keep) {
        close_when_drained_ = true;
        stop_reading_ = true;
    }
    flush_writes();
}

void session::send_longpoll_error(int status_code, std::string_view detail,
                                  const http::request& request, bool keep,
                                  bool retry_after) {
    http::response response = make_problem(status_code, detail, request.path);
    response.version = request.version;
    if (retry_after) {
        response.headers.add("Retry-After", "1");
    }
    if (!keep) {
        response.headers.add("Connection", "close");
    }
    http::serialize_options options;
    options.include_body = request.method != http::method::head;
    std::string wire;
    if (!http::serialize(response, wire, options).ok()) {
        wire = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n"
               "Connection: close\r\n\r\n";
        keep = false;
    }
    enqueue(std::move(wire));
    state_.responded.fetch_add(1, std::memory_order_relaxed);
    if (!keep) {
        close_when_drained_ = true;
        stop_reading_ = true;
    }
    flush_writes();
}

int session::decode_request_body(http::request& request) {
    const std::string* encoding = request.headers.find("content-encoding");
    if (encoding == nullptr) {
        return 200;
    }
    const request_encoding_plan plan = plan_request_decoding(*encoding);
    if (!plan.ok) {
        return plan.http_status;
    }
    if (!plan.encoded) {
        return 200;
    }

    coding_limits limits = state_.options.coding;
    if (state_.options.max_decoded_body != 0) {
        limits.max_decompressed_bytes =
            std::min(limits.max_decompressed_bytes,
                     state_.options.max_decoded_body);
    }
    limits.max_compression_ratio = state_.options.max_decoded_ratio;

    content_decoder decoder(plan.coding, limits);
    std::string decoded;
    status result = decoder.feed(as_bytes(request.body), decoded);
    if (result.ok()) {
        result = decoder.finish(decoded);
    }
    if (!result.ok()) {
        return result.code() == rpc_code::resource_exhausted ? 413 : 400;
    }

    request.body = std::move(decoded);
    state_.requests_decoded.fetch_add(1, std::memory_order_relaxed);
    // The representation is now decoded: drop Content-Encoding and describe the
    // decoded length if the request carried one.
    http::header_list filtered;
    for (const header& field : request.headers) {
        if (ci_equal(field.name, "content-encoding")) {
            continue;
        }
        if (ci_equal(field.name, "content-length")) {
            filtered.add(field.name, std::to_string(request.body.size()));
            continue;
        }
        filtered.add(field.name, field.value);
    }
    request.headers = std::move(filtered);
    return 200;
}

int session::stamp_forwarded(http::request& request) {
    // The peer is always the truth; the defaults for a resolution failure are
    // the peer itself, so a request is never served from header content.
    request.peer_address = peer_address_;
    request.peer_port = peer_port_;
    request.client_address = peer_address_;
    request.client_port = peer_port_;
    request.client_is_https = false;
    request.used_forwarded_header = false;
    request.trusted_hops = 0;
    // No IP peer (an in-process or Unix-domain channel): nothing can resolve
    // against an address, and there is nothing a header could claim.
    if (!peer_address_.valid()) {
        return 200;
    }
    const result<forwarded_client> resolved =
        http::resolve_forwarded(request, peer_address_,
                                state_.options.trusted_proxies);
    if (!resolved.ok()) {
        if (state_.options.forwarded_invalid ==
            forwarded_invalid_policy::fallback_to_peer) {
            return 200; // the client stays the socket peer
        }
        return 400;
    }
    const forwarded_client& client = resolved.value();
    request.client_address = client.address;
    request.client_port = client.port;
    request.client_is_https = client.is_https();
    request.used_forwarded_header = client.used_forwarded_header;
    request.trusted_hops = client.trusted_hops;
    return 200;
}

void session::apply_response_coding(const http::request& request,
                                    http::response& response) {
    const std::string* accept = request.headers.find("accept-encoding");
    const std::string value = accept != nullptr ? *accept : std::string{};
    const negotiation neg =
        negotiate_response(value, state_.options.coding);
    if (!neg.acceptable) {
        response = make_problem(406, "no acceptable content coding", request.path);
        response.version = request.version;
        merge_vary_accept_encoding(response.headers);
        return;
    }
    const int status_code = response.status_code;
    if (response_forbids_body(status_code) || status_code == 206 ||
        response.headers.contains("content-encoding")) {
        return;
    }
    // A negotiated response varies on Accept-Encoding even when the body stays
    // identity (a cache must not reuse it for a different client). An existing
    // Vary value (e.g. Origin) is merged, never overwritten.
    merge_vary_accept_encoding(response.headers);
    if (!neg.compressed || !should_compress(neg, response.body.size(),
                                            state_.options.coding) ||
        response.body.empty()) {
        return;
    }
    if (const std::string* type = response.headers.find("content-type")) {
        if (is_incompressible_type(*type)) {
            return;
        }
    }
    std::string encoded;
    const crpc::status result = compress_all(neg.coding, as_bytes(response.body),
                                             encoded, state_.options.coding);
    if (!result.ok() || encoded.size() >= response.body.size()) {
        // Never inflate a response: fall back to identity and send no
        // Content-Encoding.
        return;
    }
    http::header_list filtered;
    for (const header& field : response.headers) {
        if (ci_equal(field.name, "content-length") ||
            ci_equal(field.name, "transfer-encoding")) {
            continue;
        }
        filtered.add(field.name, field.value);
    }
    filtered.add("Content-Encoding", to_string(neg.coding));
    response.headers = std::move(filtered);
    response.body = std::move(encoded);
    state_.responses_compressed.fetch_add(1, std::memory_order_relaxed);
}

void session::process_http1(std::string input) {
    http::parse_status status =
        parser_.feed(as_bytes(input));
    for (;;) {
        if (status == http::parse_status::need_more) {
            // A request that has started but not completed is bounded by
            // `request_timeout` (slowloris, No-stall rule).
            note_request_progress();
            return;
        }
        if (status == http::parse_status::headers_complete) {
            note_request_progress();
            if (parser_.expect_100_continue() && !sent_100_) {
                sent_100_ = true;
                enqueue("HTTP/1.1 100 Continue\r\n\r\n");
                flush_writes();
            }
            return;
        }
        if (status == http::parse_status::error) {
            const rpc_error& error = parser_.error();
            int code = 400;
            if (error.code == rpc_code::resource_exhausted) {
                // The message layer names the limit in the error text; over a
                // header limit is 431, over the body limit is 413, and an over
                // long request line is a plain 400 (no 414 in this facade).
                if (error.message.find("header") != std::string::npos) {
                    code = 431;
                } else if (error.message.find("body") != std::string::npos) {
                    code = 413;
                }
            }
            state_.bad.fetch_add(1, std::memory_order_relaxed);
            send_http1_error(code, error.message, parser_.get_request(), false);
            return;
        }

        // message_complete
        http::request request = parser_.get_request();
        const span<const std::byte> rest = parser_.pending();
        std::string pending(reinterpret_cast<const char*>(rest.data()),
                            rest.size());
        parser_.reset();
        sent_100_ = false;
        request_deadline_ns_ = 0;

        handle_http1_request(std::move(request));
        if (download_active_.load(std::memory_order_acquire)) {
            // The connection is dedicated to the download body until it is
            // fully written; keep any pipelined bytes for after that.
            if (!pending.empty()) {
                std::lock_guard<std::mutex> lk(mu_);
                ingress_.insert(0, pending);
                read_paused_ = true;
            }
            return;
        }
        if (longpoll_active_.load(std::memory_order_acquire)) {
            // An in-flight poll owns the response order: parse pipelined bytes
            // only after its 200/204 has been queued.
            if (!pending.empty()) {
                std::lock_guard<std::mutex> lk(mu_);
                ingress_.insert(0, pending);
                read_paused_ = true;
            }
            return;
        }
        if (stop_reading_ || pending.empty()) {
            return;
        }
        // Hard egress bound (H-4): a batch of pipelined requests must not be
        // answered into an unbounded write queue. Once the queue is over the
        // cap, the unparsed bytes go back to the ingress buffer and the reader
        // stays paused until the queue drains. Nothing is dropped: the bytes
        // are bounded by `ingress_cap_bytes` and are parsed on a later turn.
        if (state_.options.egress_cap_bytes != 0 &&
            out_bytes_ > state_.options.egress_cap_bytes) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                ingress_.insert(0, pending);
                read_paused_ = true;
            }
            return;
        }
        status = parser_.feed(as_bytes(pending));
    }
}

void session::handle_http1_request(http::request request) {
    const bool head_only = request.method == http::method::head;
    const bool keep = keep_alive(request);

    if (request.version == http_version::http_1_1 &&
        !request.headers.contains("host")) {
        state_.bad.fetch_add(1, std::memory_order_relaxed);
        send_http1_error(400, "HTTP/1.1 requires a Host header", request, false);
        return;
    }

    if (const int forwarded = stamp_forwarded(request); forwarded != 200) {
        state_.bad.fetch_add(1, std::memory_order_relaxed);
        send_http1_error(forwarded, "the forwarding header is malformed",
                         request, keep);
        return;
    }

    if (state_.options.enable_request_decoding) {
        const int decode_status = decode_request_body(request);
        if (decode_status != 200) {
            state_.bad.fetch_add(1, std::memory_order_relaxed);
            send_http1_error(decode_status, "request content decoding failed",
                             request, decode_status != 413 && keep);
            return;
        }
    }

    // A long-poll path is a special route, not a router action: a non-GET
    // method is 405 even when it is a PUT/PATCH that would otherwise trip the
    // framing rule below.
    const longpoll_route* early_longpoll =
        find_longpoll_route(state_.longpoll_routes, request.path);
    if (early_longpoll != nullptr && request.method != http::method::get) {
        longpoll_open(std::move(request), *early_longpoll);
        return;
    }

    const bool framed = request.headers.contains("content-length") ||
                        request.headers.contains("transfer-encoding");
    if (method_requires_framing(request.method) && !framed &&
        request.body.empty()) {
        state_.bad.fetch_add(1, std::memory_order_relaxed);
        send_http1_error(411, "Length Required: send Content-Length or a chunked body",
                         request, keep);
        if (!keep) {
            return;
        }
    } else {
        const sse_route* sse = find_sse_route(state_.sse_routes, request.path);
        if (sse != nullptr) {
            // SSE is a long-lived, unbounded response. It is only defined for
            // HTTP/1.1 over a single shard; anything else fails closed rather
            // than producing a half-stream.
            if (request.method != http::method::get) {
                state_.bad.fetch_add(1, std::memory_order_relaxed);
                send_http1_error(405, "SSE requires GET", request, keep);
                return;
            }
            if (request.version != http_version::http_1_1) {
                state_.bad.fetch_add(1, std::memory_order_relaxed);
                send_http1_error(400, "SSE requires HTTP/1.1", request, false);
                return;
            }
            sse_open(std::move(request), *sse);
            return;
        }
        const longpoll_route* longpoll =
            find_longpoll_route(state_.longpoll_routes, request.path);
        if (longpoll != nullptr) {
            longpoll_open(std::move(request), *longpoll);
            return;
        }
        const download_match download =
            find_download_route(state_.download_routes, request.path);
        if (download.route != nullptr) {
            download_open(request, *download.route, download.relative, keep);
            return;
        }
        http::request routed = std::move(request);
        if (head_only) {
            routed.method = http::method::get;
            routed.method_token = "GET";
        }
        http::response response;
        if (state_.router != nullptr) {
            (void)state_.router->handle(routed, response);
        } else {
            response = make_problem(404, "no router configured", routed.path);
        }
        response.version = routed.version;
        if (state_.options.enable_response_coding) {
            apply_response_coding(routed, response);
        }
        if (!keep) {
            response.headers.add("Connection", "close");
        } else if (routed.version == http_version::http_1_0) {
            response.headers.add("Connection", "keep-alive");
        }
        http::serialize_options options;
        options.include_body = !head_only;
        std::string wire;
        if (!http::serialize(response, wire, options).ok()) {
            wire = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n"
                   "Connection: close\r\n\r\n";
            close_when_drained_ = true;
            stop_reading_ = true;
        }
        enqueue(std::move(wire));
        state_.handled.fetch_add(1, std::memory_order_relaxed);
        state_.responded.fetch_add(1, std::memory_order_relaxed);
        if (!keep) {
            close_when_drained_ = true;
            stop_reading_ = true;
        }
        flush_writes();
        if (stop_reading_) {
            return;
        }
    }
}

// --- session: file download --------------------------------------------------

void session::download_mark_output() {
    if (shard_ == nullptr ||
        state_.options.download_stall_timeout.count() <= 0) {
        return;
    }
    // Refreshed on every progress event (a queued chunk or a completed write),
    // so a slow-but-progressing peer is never mistaken for a stalled one.
    output_stall_deadline_ns_ =
        shard_->now_ns() +
        static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             state_.options.download_stall_timeout)
                             .count());
}

void session::download_complete() noexcept {
    output_stall_deadline_ns_ = 0;
    download_.reset();
    download_buffer_.clear();
    download_active_.store(false, std::memory_order_release);
    if (!download_keep_alive_) {
        close_when_drained_ = true;
        stop_reading_ = true;
    }
}

void session::download_pump() {
    if (!download_active_.load(std::memory_order_acquire) || finished_ ||
        write_failed_ || download_ == nullptr) {
        return;
    }
    const std::size_t cap = state_.options.egress_cap_bytes;
    std::size_t chunk = state_.options.download_chunk_bytes;
    if (chunk == 0) {
        chunk = 64u * 1024u;
    }
    while (download_active_.load(std::memory_order_acquire) && !write_failed_) {
        if (!download_keep_alive_ && close_when_drained_) {
            break;
        }
        const std::uint64_t remaining = download_->remaining();
        if (remaining == 0) {
            download_complete();
            return;
        }
        std::size_t wanted = chunk;
        if (static_cast<std::uint64_t>(wanted) > remaining) {
            wanted = static_cast<std::size_t>(remaining);
        }
        if (cap != 0) {
            if (out_bytes_ >= cap) {
                break;
            }
            if (wanted > cap - out_bytes_) {
                wanted = cap - out_bytes_;
            }
        }
        if (wanted == 0) {
            break;
        }
        download_buffer_.resize(wanted);
        std::size_t count = 0;
        const status result = download_->read(
            span<std::byte>(reinterpret_cast<std::byte*>(download_buffer_.data()),
                            download_buffer_.size()),
            count);
        if (!result.ok()) {
            download_.reset();
            download_active_.store(false, std::memory_order_release);
            write_failed_ = true;
            close_when_drained_ = true;
            stop_reading_ = true;
            break;
        }
        if (count == 0) {
            download_complete();
            return;
        }
        enqueue(std::string(download_buffer_.data(), count));
        download_mark_output();
        note_activity();
    }
}

void session::download_open(http::request& request, const download_route& route,
                            const std::string& relative, bool keep) {
    const bool head_only = request.method == http::method::head;
    if (request.method != http::method::get &&
        request.method != http::method::head) {
        state_.bad.fetch_add(1, std::memory_order_relaxed);
        http::response response =
            make_problem(405, "download requires GET or HEAD", request.path);
        response.version = request.version;
        response.headers.add("Allow", "GET, HEAD");
        if (!keep) {
            response.headers.add("Connection", "close");
        }
        http::serialize_options options;
        options.include_body = !head_only;
        std::string wire;
        if (!http::serialize(response, wire, options).ok()) {
            wire = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n"
                   "Connection: close\r\n\r\n";
            keep = false;
        }
        enqueue(std::move(wire));
        state_.responded.fetch_add(1, std::memory_order_relaxed);
        if (!keep) {
            close_when_drained_ = true;
            stop_reading_ = true;
        }
        flush_writes();
        return;
    }

    // A HEAD error must carry the headers a GET would carry but no body;
    // `send_http1_error` always serializes the problem body, so handle HEAD
    // locally to keep the connection in sync.
    const auto head_error = [&](int status, std::string_view detail) {
        state_.bad.fetch_add(1, std::memory_order_relaxed);
        http::response response = make_problem(status, detail, request.path);
        response.version = request.version;
        if (!keep) {
            response.headers.add("Connection", "close");
        }
        http::serialize_options options;
        options.include_body = !head_only;
        std::string wire;
        if (!http::serialize(response, wire, options).ok()) {
            wire = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n"
                   "Connection: close\r\n\r\n";
            keep = false;
        }
        enqueue(std::move(wire));
        state_.responded.fetch_add(1, std::memory_order_relaxed);
        if (!keep) {
            close_when_drained_ = true;
            stop_reading_ = true;
        }
        flush_writes();
    };

    std::filesystem::path file;
    if (!resolve_download_path(route.root, relative, file)) {
        head_error(404, "download file not found");
        return;
    }
    result<download_source> opened = download_source::open_file(file);
    if (!opened.ok()) {
        head_error(404, "download file not found");
        return;
    }
    download_ =
        std::make_unique<download_source>(std::move(opened.value()));
    // `If-Range` can only be honored against a validator (ETag or Last-Modified)
    // that this slice does not emit; without one the Range must be ignored and
    // the full entity served (RFC 9110 13.1.5).
    if (!request.headers.contains("if-range")) {
        if (const std::string* range = request.headers.find("range")) {
            (void)download_->apply_range(*range);
        }
    }

    const int code = download_->status_code();
    std::string head;
    head.reserve(256);
    head += "HTTP/1.1 ";
    head += std::to_string(code);
    head += " ";
    head += local_reason(code);
    head += "\r\n";
    head += "Content-Type: ";
    head += route.content_type.empty() ? "application/octet-stream"
                                       : route.content_type;
    head += "\r\n";
    if (code == 416) {
        head += "Content-Range: ";
        head += download_->content_range();
        head += "\r\n";
        head += "Accept-Ranges: bytes\r\n";
        head += "Content-Length: 0\r\n";
    } else {
        if (code == 206) {
            head += "Content-Range: ";
            head += download_->content_range();
            head += "\r\n";
        } else if (state_.options.enable_response_coding) {
            // The full-body form could be encoded; advertise that it varies.
            head += "Vary: Accept-Encoding\r\n";
        }
        head += "Accept-Ranges: bytes\r\n";
        head += "Content-Length: ";
        head += std::to_string(download_->content_length());
        head += "\r\n";
    }
    if (keep) {
        head += "Connection: keep-alive\r\n";
    } else {
        head += "Connection: close\r\n";
    }
    head += "\r\n";
    enqueue(std::move(head));
    state_.handled.fetch_add(1, std::memory_order_relaxed);
    state_.responded.fetch_add(1, std::memory_order_relaxed);
    state_.downloads_served.fetch_add(1, std::memory_order_relaxed);

    if (code == 416 || head_only) {
        download_.reset();
        if (!keep) {
            close_when_drained_ = true;
            stop_reading_ = true;
        }
        flush_writes();
        return;
    }

    download_keep_alive_ = keep;
    download_active_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(mu_);
        read_paused_ = true;
    }
    // A download has no inbound liveness signal; the idle deadline stays armed
    // and is refreshed by write progress, and the output stall deadline bounds
    // a peer that stops draining.
    note_activity();
    download_pump();
    flush_writes();
    arm_probe();
}

// --- session: Server-Sent Events --------------------------------------------

std::size_t session::sse_egress_cap() const noexcept {
    const std::size_t configured = state_.options.sse.egress_cap_bytes;
    return configured != 0 ? configured : state_.options.egress_cap_bytes;
}

std::size_t session::sse_staged_bytes() const noexcept {
    std::size_t staged = out_bytes_ + (write_in_flight_ ? in_flight_.size() : 0);
    if (sse_stream_id_ != 0) {
        // HTTP/2 bytes parked behind a zero window are staged output too: they
        // must count toward the bound so a slow peer cannot grow memory.
        staged += h2_.parked_bytes(sse_stream_id_);
    }
    return staged;
}

void session::wake_sse() noexcept {
    if (!sse_active_.load(std::memory_order_acquire)) {
        return;
    }
    schedule();
}

void session::wake_longpoll() noexcept {
    if (!longpoll_active_.load(std::memory_order_acquire)) {
        return;
    }
    schedule();
}

void session::sse_mark_output() {
    if (sse_stall_deadline_ns_ != 0 || shard_ == nullptr ||
        state_.options.sse.slow_timeout.count() <= 0) {
        return;
    }
    sse_stall_deadline_ns_ =
        shard_->now_ns() +
        static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             state_.options.sse.slow_timeout)
                             .count());
}

void session::sse_clear_stall_if_drained() {
    if (sse_staged_bytes() == 0) {
        sse_stall_deadline_ns_ = 0;
    }
}

void session::sse_detach() noexcept {
    sse_active_.store(false, std::memory_order_release);
    sse_has_pending_ = false;
    sse_pending_bytes_ = 0;
    sse_pending_ = push::push_message{};
    sse_heartbeat_ns_ = 0;
    sse_stall_deadline_ns_ = 0;
    const push::hub::connection_id token = sse_token_;
    sse_token_ = 0;
    if (token == 0) {
        return;
    }
    // MUST happen before the session/transport (and this token) is destroyed:
    // a detached token can no longer receive a delivery or acknowledge a late
    // write. The subscription itself survives so a reconnect can replay.
    std::lock_guard<std::recursive_mutex> lk(state_.hub_mutex);
    if (sse_subscriber_ != 0) {
        auto it = state_.sse_subscribers.find(sse_subscriber_);
        if (it != state_.sse_subscribers.end()) {
            if (it->second <= 1) {
                state_.sse_subscribers.erase(it);
            } else {
                --it->second;
            }
        }
    }
    push::hub* hub = state_.hub_for(sse_topic_);
    if (hub != nullptr) {
        (void)hub->detach(token);
    }
    state_.sse_connections.fetch_sub(1, std::memory_order_relaxed);
}

void session::sse_abort() noexcept {
    const bool was_active = sse_active_.exchange(false);
    if (!was_active) {
        sse_detach();
        return;
    }
    sse_detach();
    sse_pending_ = push::push_message{};
    sse_has_pending_ = false;
    sse_stream_id_ = 0;
    close_when_drained_ = true;
    stop_reading_ = true;
    // The peer is gone or wedged: do not wait for a queued write to complete.
    finish_now();
}

void session::sse_reset() noexcept {
    if (!sse_active_.exchange(false)) {
        sse_stream_id_ = 0;
        return;
    }
    sse_detach();
    sse_pending_ = push::push_message{};
    sse_has_pending_ = false;
    sse_stream_id_ = 0;
    // The connection stays usable: other HTTP/2 streams may still be active.
}

void session::sse_close(bool graceful) noexcept {
    if (!sse_active_.exchange(false)) {
        sse_detach();
        return;
    }
    sse_detach();
    if (sse_stream_id_ != 0) {
        const u32 stream_id = sse_stream_id_;
        sse_stream_id_ = 0;
        if (graceful && !write_failed_ &&
            h2_.state_of(stream_id) != h2::stream_state::closed) {
            // End the stream with an empty DATA frame carrying END_STREAM. A
            // non-graceful close (peer reset/EOF) has already ended it.
            if (h2_.local_end_stream(stream_id).ok()) {
                h2::frame_header header;
                header.type = h2::frame_type::data;
                header.flags = h2::flags::end_stream;
                header.stream_id = stream_id;
                header.length = 0;
                std::string frame(h2::frame_header_size, '\0');
                std::size_t count = 0;
                (void)h2::encode_frame(
                    span<std::byte>(reinterpret_cast<std::byte*>(frame.data()),
                                    frame.size()),
                    header, span<const std::byte>(), count);
                enqueue(std::move(frame));
            }
        }
        close_when_drained_ = true;
        stop_reading_ = true;
        flush_writes();
        return;
    }
    if (graceful && !write_failed_) {
        // The terminating chunk is bound to the response close, never emitted
        // between events (mirrors maybe_finish's drain rule).
        std::string terminator;
        if (sse_encoder_.finish(terminator).ok()) {
            enqueue(std::move(terminator));
        }
    }
    close_when_drained_ = true;
    stop_reading_ = true;
    flush_writes();
}

sse_open_error session::sse_resolve(const http::request& request,
                                     const sse_route& route,
                                     std::string& detail) {
    std::string topic = route.topic;
    std::string value;
    switch (query_param(request.query, "topic", value)) {
    case query_result::invalid:
        detail = "invalid topic parameter";
        return sse_open_error::bad_request;
    case query_result::ok:
        topic = std::move(value);
        break;
    case query_result::missing:
        break;
    }
    if (topic.empty()) {
        detail = "SSE route requires a topic";
        return sse_open_error::bad_request;
    }

    push::delivery_mode mode = route.mode;
    switch (query_param(request.query, "mode", value)) {
    case query_result::invalid:
        detail = "invalid mode parameter";
        return sse_open_error::bad_request;
    case query_result::ok:
        if (value == "reliable") {
            mode = push::delivery_mode::reliable;
        } else if (value == "unreliable") {
            mode = push::delivery_mode::unreliable;
        } else {
            detail = "mode must be reliable or unreliable";
            return sse_open_error::bad_request;
        }
        break;
    case query_result::missing:
        break;
    }

    bool ordered = route.ordered;
    switch (query_param(request.query, "ordered", value)) {
    case query_result::invalid:
        detail = "invalid ordered parameter";
        return sse_open_error::bad_request;
    case query_result::ok:
        if (!parse_bool_text(value, ordered)) {
            detail = "ordered must be 0/1 or false/true";
            return sse_open_error::bad_request;
        }
        break;
    case query_result::missing:
        break;
    }

    u64 subscriber = 0;
    switch (query_param(request.query, "subscriber", value)) {
    case query_result::invalid:
        detail = "invalid subscriber parameter";
        return sse_open_error::bad_request;
    case query_result::ok:
        if (!parse_u64_text(value, subscriber) || subscriber == 0) {
            detail = "subscriber must be a non-zero u64";
            return sse_open_error::bad_request;
        }
        break;
    case query_result::missing:
        subscriber =
            state_.next_sse_subscriber.fetch_add(1, std::memory_order_relaxed);
        break;
    }

    bool have_cursor = false;
    sse::event_id cursor{};
    if (const std::string* field = request.headers.find("last-event-id")) {
        if (!sse::parse_last_event_id(*field, cursor).ok()) {
            detail = "invalid Last-Event-ID";
            return sse_open_error::bad_request;
        }
        have_cursor = true;
    }

    {
        std::lock_guard<std::recursive_mutex> lk(state_.hub_mutex);
        if (state_.longpoll_owners.find(subscriber) !=
            state_.longpoll_owners.end()) {
            detail = "subscriber is used by long polling";
            return sse_open_error::conflict;
        }
    }

    std::lock_guard<std::recursive_mutex> lk(state_.hub_mutex);
    push::hub* hub = state_.hub_for(topic);
    if (hub == nullptr) {
        detail = "push hub is not running";
        return sse_open_error::unavailable;
    }
    const status subscribed = hub->subscribe(topic, subscriber, mode, ordered,
                                            state_.options.sse.subscribe);
    if (!subscribed.ok() && subscribed.code() != rpc_code::failed_precondition) {
        detail = subscribed.error().message;
        return sse_open_error::unavailable;
    }
    // Reconnect order: clamp the client cursor to the highest seq this server
    // ever delivered for (subscriber, topic), then ack, then attach. Ack-only:
    // a re-subscribe would fail with failed_precondition and must never create
    // a new subscriber.
    u64 accepted = 0;
    if (have_cursor && mode == push::delivery_mode::reliable &&
        cursor.epoch == state_.options.sse.epoch) {
        const auto high = state_.sse_delivered.find({subscriber, topic});
        const u64 delivered = high == state_.sse_delivered.end() ? 0 : high->second;
        accepted = std::min(cursor.seq, delivered);
    }
    if (accepted != 0) {
        (void)hub->ack(subscriber, topic, accepted);
    }
    const push::hub::connection_id token = hub->attach(subscriber);
    if (token == push::hub::invalid_connection) {
        detail = "SSE attach failed";
        return sse_open_error::unavailable;
    }
    sse_subscriber_ = subscriber;
    sse_token_ = token;
    sse_topic_ = topic;
    const auto high = state_.sse_delivered.find({subscriber, topic});
    sse_delivered_high_water_ = high == state_.sse_delivered.end() ? 0 : high->second;
    ++state_.sse_subscribers[subscriber];
    sse_active_.store(true, std::memory_order_release);
    state_.sse_connections.fetch_add(1, std::memory_order_relaxed);
    return sse_open_error::ok;
}

void session::sse_open(http::request request, const sse_route& route) {
    std::string detail;
    const sse_open_error err = sse_resolve(request, route, detail);
    if (err != sse_open_error::ok) {
        if (err == sse_open_error::bad_request) {
            state_.bad.fetch_add(1, std::memory_order_relaxed);
            send_http1_error(400, detail, request, false);
        } else if (err == sse_open_error::conflict) {
            state_.longpoll_conflicts.fetch_add(1, std::memory_order_relaxed);
            send_longpoll_error(409, detail, request, keep_alive(request), false);
        } else {
            send_http1_error(503, detail, request, false);
        }
        return;
    }

    // Hand-written streaming head: no Content-Length and no terminal chunk.
    // `http::serialize` always appends the zero chunk, so it cannot be used
    // for this response (see message.cpp serialize()).
    std::string head;
    head.reserve(192);
    head += "HTTP/1.1 200 OK\r\n";
    head += "Content-Type: text/event-stream; charset=utf-8\r\n";
    head += "Cache-Control: no-cache\r\n";
    head += "Connection: keep-alive\r\n";
    head += "X-Accel-Buffering: no\r\n";
    head += "Transfer-Encoding: chunked\r\n\r\n";
    enqueue(std::move(head));

    if (state_.options.sse.retry.count() > 0) {
        const std::string hint =
            sse::retry_hint(state_.options.sse.retry);
        std::string chunk;
        if (!hint.empty()) {
            (void)sse_encoder_.append(as_bytes(hint), chunk);
            enqueue(std::move(chunk));
            sse_mark_output();
        }
    }

    state_.handled.fetch_add(1, std::memory_order_relaxed);
    state_.responded.fetch_add(1, std::memory_order_relaxed);

    // An SSE response has no inbound liveness signal. The idle and request
    // deadlines are therefore disabled; heartbeats and the slow-read stall
    // bound are the liveness checks instead.
    idle_deadline_ns_ = 0;
    request_deadline_ns_ = 0;
    stop_reading_ = true;
    if (state_.options.sse.heartbeat.count() > 0 && shard_ != nullptr) {
        sse_heartbeat_ns_ =
            shard_->now_ns() +
            static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 state_.options.sse.heartbeat)
                                 .count());
    }

    sse_pump();
    flush_writes();
    arm_probe();
}

void session::sse_open_h2(u32 stream_id, http::request request,
                          const sse_route& route) {
    if (h2_.state_of(stream_id) == h2::stream_state::closed) {
        return;
    }
    // A session owns one SSE subscription. A second SSE request on the same
    // connection replaces the first: detach it before re-attaching so the hub
    // token cannot be leaked.
    if (sse_active_.load(std::memory_order_acquire)) {
        const u32 replaced = sse_stream_id_;
        sse_reset();
        // The replaced stream must actually end: leaving it open would keep its
        // record (and any parked bytes) alive until the connection closes.
        if (replaced != 0 && h2_.stream_exists(replaced) &&
            h2_.state_of(replaced) != h2::stream_state::closed) {
            h2_send_rst(replaced, h2::error_code::cancel);
        }
    }

    std::string detail;
    const sse_open_error err = sse_resolve(request, route, detail);
    if (err != sse_open_error::ok) {
        int code = 503;
        if (err == sse_open_error::bad_request) {
            code = 400;
            state_.bad.fetch_add(1, std::memory_order_relaxed);
        } else if (err == sse_open_error::conflict) {
            code = 409;
            state_.longpoll_conflicts.fetch_add(1, std::memory_order_relaxed);
        }
        http::response response;
        response.status_code = code;
        response.reason = local_reason(code);
        response.headers.add("Content-Type", "text/plain; charset=utf-8");
        response.body = detail + "\n";
        state_.handled.fetch_add(1, std::memory_order_relaxed);
        send_h2_response(stream_id, response, false);
        return;
    }

    sse_stream_id_ = stream_id;
    if (!h2_send_sse_head(stream_id)) {
        sse_reset();
        return;
    }

    if (state_.options.sse.retry.count() > 0) {
        const std::string hint =
            sse::retry_hint(state_.options.sse.retry);
        if (!hint.empty()) {
            sse_h2_send_frame(hint);
        }
    }
    // The head and retry hint are already output: arm the slow-read stall
    // deadline so a peer that never drains them is closed instead of waiting
    // for the first event (D4).
    sse_mark_output();

    state_.handled.fetch_add(1, std::memory_order_relaxed);
    state_.responded.fetch_add(1, std::memory_order_relaxed);

    // The SSE response is long-lived: no idle or request deadline. HTTP/2 keeps
    // reading because flow-control frames (WINDOW_UPDATE/RST_STREAM/PING) must
    // still be processed while the stream is open.
    idle_deadline_ns_ = 0;
    request_deadline_ns_ = 0;
    if (state_.options.sse.heartbeat.count() > 0 && shard_ != nullptr) {
        sse_heartbeat_ns_ =
            shard_->now_ns() +
            static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 state_.options.sse.heartbeat)
                                 .count());
    }

    sse_pump();
    flush_writes();
    arm_probe();
}

void session::sse_pump() {
    if (!sse_active_.load(std::memory_order_acquire) || finished_ ||
        sse_pumping_) {
        return;
    }
    if (sse_stream_id_ != 0) {
        sse_pump_h2();
        return;
    }
    sse_pumping_ = true;
    {
        std::lock_guard<std::recursive_mutex> lk(state_.hub_mutex);
        push::hub* hub = state_.hub_for(sse_topic_);
        if (hub == nullptr || sse_token_ == 0) {
            sse_pumping_ = false;
            return;
        }
        const std::size_t cap = sse_egress_cap();
        for (;;) {
            if (finished_ || write_failed_ || !sse_active_.load()) {
                break;
            }
            const std::size_t staged = sse_staged_bytes();
            if (sse_has_pending_) {
                // A single message larger than the bound still makes progress:
                // the bound pauses the pump, it never refuses a message.
                if (cap != 0 && staged != 0 &&
                    staged + sse_pending_bytes_ > cap) {
                    break;
                }
                std::string frame;
                const bool reliable =
                    sse_pending_.mode == push::delivery_mode::reliable;
                const sse::event_id id{
                    sse_pending_.epoch, reliable ? sse_pending_.seq
                                                 : sse_pending_.topic_seq};
                if (!sse::encode_event(id, sse_pending_.data(), frame).ok()) {
                    sse_has_pending_ = false;
                    sse_pending_bytes_ = 0;
                    sse_pending_ = push::push_message{};
                    break;
                }
                sse_pending_ = push::push_message{};
                sse_has_pending_ = false;
                sse_pending_bytes_ = 0;
                std::string chunk;
                if (!sse_encoder_.append(as_bytes(frame), chunk).ok()) {
                    break;
                }
                enqueue(std::move(chunk));
                sse_mark_output();
                if (reliable && id.seq > sse_delivered_high_water_) {
                    sse_delivered_high_water_ = id.seq;
                    state_.sse_delivered[{sse_subscriber_, sse_topic_}] = id.seq;
                }
                state_.sse_events_delivered.fetch_add(1,
                                                      std::memory_order_relaxed);
                continue;
            }
            if (cap != 0 && staged >= cap) {
                break;
            }
            push::push_message message;
            if (!hub->try_take(sse_token_, sse_topic_, message)) {
                break;
            }
            const sse::event_id id{
                message.epoch,
                message.mode == push::delivery_mode::reliable ? message.seq
                                                              : message.topic_seq};
            std::string frame;
            if (!sse::encode_event(id, message.data(), frame).ok()) {
                continue;
            }
            if (cap != 0 && staged != 0 && staged + frame.size() > cap) {
                sse_pending_ = std::move(message);
                sse_pending_bytes_ = frame.size();
                sse_has_pending_ = true;
                break;
            }
            std::string chunk;
            if (!sse_encoder_.append(as_bytes(frame), chunk).ok()) {
                break;
            }
            enqueue(std::move(chunk));
            sse_mark_output();
            if (message.mode == push::delivery_mode::reliable &&
                id.seq > sse_delivered_high_water_) {
                sse_delivered_high_water_ = id.seq;
                state_.sse_delivered[{sse_subscriber_, sse_topic_}] = id.seq;
            }
            state_.sse_events_delivered.fetch_add(1, std::memory_order_relaxed);
        }
    }
    sse_pumping_ = false;
}

bool session::sse_h2_window_exhausted() const noexcept {
    return sse_stream_id_ != 0 &&
           h2_.available_send_window(sse_stream_id_) == 0;
}

void session::sse_pump_h2() {
    if (sse_pumping_) {
        return;
    }
    sse_pumping_ = true;
    {
        std::lock_guard<std::recursive_mutex> lk(state_.hub_mutex);
        push::hub* hub = state_.hub_for(sse_topic_);
        if (hub == nullptr || sse_token_ == 0) {
            sse_pumping_ = false;
            return;
        }
        const std::size_t cap = sse_egress_cap();
        for (;;) {
            if (finished_ || write_failed_ || !sse_active_.load()) {
                break;
            }
            // A zero send window means every further frame would park: stop and
            // let WINDOW_UPDATE (or the probe PING) reopen the stream instead of
            // growing the send queue toward its cap.
            if (sse_h2_window_exhausted()) {
                break;
            }
            const std::size_t staged = sse_staged_bytes();
            if (sse_has_pending_) {
                if (cap != 0 && staged != 0 &&
                    staged + sse_pending_bytes_ > cap) {
                    break;
                }
                std::string frame;
                const bool reliable =
                    sse_pending_.mode == push::delivery_mode::reliable;
                const sse::event_id id{
                    sse_pending_.epoch, reliable ? sse_pending_.seq
                                                 : sse_pending_.topic_seq};
                if (!sse::encode_event(id, sse_pending_.data(), frame).ok()) {
                    sse_has_pending_ = false;
                    sse_pending_bytes_ = 0;
                    sse_pending_ = push::push_message{};
                    break;
                }
                sse_pending_ = push::push_message{};
                sse_has_pending_ = false;
                sse_pending_bytes_ = 0;
                sse_h2_send_frame(frame);
                if (write_failed_ || finished_ || sse_stream_id_ == 0) {
                    break;
                }
                if (reliable && id.seq > sse_delivered_high_water_) {
                    sse_delivered_high_water_ = id.seq;
                    state_.sse_delivered[{sse_subscriber_, sse_topic_}] = id.seq;
                }
                state_.sse_events_delivered.fetch_add(1,
                                                      std::memory_order_relaxed);
                continue;
            }
            if (cap != 0 && staged >= cap) {
                break;
            }
            push::push_message message;
            if (!hub->try_take(sse_token_, sse_topic_, message)) {
                break;
            }
            const sse::event_id id{
                message.epoch,
                message.mode == push::delivery_mode::reliable ? message.seq
                                                              : message.topic_seq};
            std::string frame;
            if (!sse::encode_event(id, message.data(), frame).ok()) {
                continue;
            }
            if (cap != 0 && staged != 0 && staged + frame.size() > cap) {
                sse_pending_ = std::move(message);
                sse_pending_bytes_ = frame.size();
                sse_has_pending_ = true;
                break;
            }
            sse_h2_send_frame(frame);
            if (write_failed_ || finished_ || sse_stream_id_ == 0) {
                break;
            }
            if (message.mode == push::delivery_mode::reliable &&
                id.seq > sse_delivered_high_water_) {
                sse_delivered_high_water_ = id.seq;
                state_.sse_delivered[{sse_subscriber_, sse_topic_}] = id.seq;
            }
            state_.sse_events_delivered.fetch_add(1, std::memory_order_relaxed);
        }
    }
    sse_pumping_ = false;
}

void session::sse_h2_send_frame(const std::string& frame) {
    const u32 stream_id = sse_stream_id_;
    if (stream_id == 0 || finished_ || write_failed_ || frame.empty()) {
        return;
    }
    std::size_t sent = 0;
    bool parked = false;
    const h2::h2_error error =
        h2_.send_data(stream_id, as_bytes(frame), sent, parked);
    if (!error.ok()) {
        // A stream-scope error (closed or flow-control overflow) ends this
        // stream; the connection stays usable for its other streams.
        if (error.scope == h2::error_scope::stream) {
            h2_send_rst(stream_id, error.code);
        }
        sse_reset();
        return;
    }
    if (sent != 0) {
        h2::frame_header data_frame;
        data_frame.type = h2::frame_type::data;
        data_frame.flags = 0; // END_STREAM is sent only when the stream ends.
        data_frame.stream_id = stream_id;
        data_frame.length = static_cast<u32>(sent);
        std::string out(h2::frame_header_size + sent, '\0');
        std::size_t count = 0;
        (void)h2::encode_frame(
            span<std::byte>(reinterpret_cast<std::byte*>(out.data()), out.size()),
            data_frame,
            span<const std::byte>(
                reinterpret_cast<const std::byte*>(frame.data()), sent),
            count);
        enqueue(std::move(out));
        sse_mark_output();
    }
    if (parked) {
        // Retry the remainder now; the probe PING covers a peer that stays at a
        // zero window (R18).
        h2_flush(stream_id);
    }
}

void session::sse_send_heartbeat() {
    if (!sse_active_.load(std::memory_order_acquire) || finished_) {
        return;
    }
    if (sse_staged_bytes() != 0) {
        // The peer is still draining an earlier write. Skip this tick instead
        // of growing the queue; the stall deadline closes a peer that never
        // drains.
        return;
    }
    if (sse_stream_id_ != 0) {
        sse_h2_send_frame(sse::heartbeat());
        return;
    }
    std::string chunk;
    if (!sse_encoder_.append(as_bytes(sse::heartbeat()), chunk).ok()) {
        return;
    }
    enqueue(std::move(chunk));
    sse_mark_output();
}

// --- session: HTTP/1.1 long polling -----------------------------------------

std::size_t session::longpoll_egress_cap() const noexcept {
    const std::size_t configured = state_.options.longpoll.egress_cap_bytes;
    return configured != 0 ? configured : state_.options.egress_cap_bytes;
}

void session::longpoll_mark_output() {
    longpoll_response_active_ = true;
    if (shard_ == nullptr || state_.options.longpoll.slow_timeout.count() <= 0) {
        output_stall_deadline_ns_ = 0;
        return;
    }
    output_stall_deadline_ns_ =
        shard_->now_ns() +
        static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             state_.options.longpoll.slow_timeout)
                             .count());
}

void session::longpoll_finish_wait() noexcept {
    if (!longpoll_active_.exchange(false)) {
        return;
    }
    longpoll_deadline_ns_ = 0;
    try {
        std::lock_guard<std::recursive_mutex> lk(state_.hub_mutex);
        if (longpoll_subscriber_ != 0 &&
            state_.longpoll_busy.erase(longpoll_subscriber_) != 0) {
            state_.longpoll_waiters.fetch_sub(1, std::memory_order_relaxed);
        }
        if (longpoll_subscriber_ != 0) {
            state_.longpoll_last_seen[longpoll_subscriber_] =
                shard_ != nullptr ? shard_->now_ns() : 0;
        }
    } catch (...) {
    }
}

void session::longpoll_release() noexcept {
    const bool was_waiting = longpoll_active_.exchange(false);
    const push::hub::connection_id token = longpoll_token_;
    const push::subscriber_id subscriber = longpoll_subscriber_;
    longpoll_token_ = 0;
    longpoll_subscriber_ = 0;
    longpoll_deadline_ns_ = 0;
    longpoll_response_active_ = false;
    output_stall_deadline_ns_ = 0;
    try {
        std::lock_guard<std::recursive_mutex> lk(state_.hub_mutex);
        if (was_waiting && subscriber != 0 &&
            state_.longpoll_busy.erase(subscriber) != 0) {
            state_.longpoll_waiters.fetch_sub(1, std::memory_order_relaxed);
        }
        if (subscriber != 0) {
            auto it = state_.longpoll_owners.find(subscriber);
            if (it != state_.longpoll_owners.end()) {
                const std::shared_ptr<session> owner = it->second.owner.lock();
                if (!owner || owner.get() == this ||
                    it->second.token == token) {
                    state_.longpoll_owners.erase(it);
                }
            }
            state_.longpoll_last_seen[subscriber] =
                shard_ != nullptr ? shard_->now_ns() : 0;
        }
        push::hub* hub = state_.hub_for(longpoll_topic_);
        if (token != 0 && hub != nullptr) {
            // Keep-queue: queued messages stay in_queue, an already-taken
            // message stays in the replay window, and nothing is counted as a
            // retransmission. The subscription is intentionally kept so a
            // reconnect inside `subscriber_ttl` still resumes it; the periodic
            // long-poll housekeeping timer performs `unsubscribe_all` when the
            // TTL expires (no later poll is required).
            (void)hub->release(token);
        }
    } catch (...) {
    }
    longpoll_topic_.clear();
}

void session::longpoll_send_message(const push::push_message& message,
                                    bool keep) {
    const bool reliable = message.mode == push::delivery_mode::reliable;
    const u64 event_seq = reliable ? message.seq : message.topic_seq;
    const span<const std::byte> body = message.data();
    std::string head;
    try {
        head.reserve(256 + body.size());
        head += "HTTP/1.1 200 OK\r\n";
        head += "Content-Type: ";
        head += state_.options.longpoll.content_type.empty()
                    ? "application/octet-stream"
                    : state_.options.longpoll.content_type;
        head += "\r\n";
        head += "Content-Length: ";
        head += std::to_string(body.size());
        head += "\r\n";
        head += "Cache-Control: no-store, no-cache\r\n";
        head += "X-CRPC-Event-ID: ";
        head += std::to_string(message.epoch);
        head.push_back(':');
        head += std::to_string(event_seq);
        head += "\r\n";
        head += "X-CRPC-Subscriber: ";
        head += std::to_string(longpoll_subscriber_);
        head += "\r\n";
        head += keep ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
        head += "\r\n";
        if (!body.empty()) {
            head.append(reinterpret_cast<const char*>(body.data()), body.size());
        }
    } catch (...) {
        // The message was taken and stays unacknowledged; the next poll's
        // cursor gap replays it. Close instead of emitting a truncated head.
        write_failed_ = true;
        close_when_drained_ = true;
        stop_reading_ = true;
        return;
    }
    enqueue(std::move(head));
    const std::size_t cap = longpoll_egress_cap();
    if (cap != 0 && out_bytes_ > cap) {
        // The long-poll bound is lower than the generic session bound: one
        // message still makes progress, but no further ingress is read until
        // the queue drains.
        std::lock_guard<std::mutex> lk(mu_);
        read_paused_ = true;
    }
    longpoll_mark_output();
    state_.longpoll_events.fetch_add(1, std::memory_order_relaxed);
    if (!keep) {
        close_when_drained_ = true;
        stop_reading_ = true;
    }
    // The wait disabled the idle deadline; a completed poll is inbound activity
    // again. A peer that stops draining is still bounded by the output stall.
    note_activity();
    flush_writes();
    arm_probe();
}

void session::longpoll_send_timeout(bool keep) {
    std::string head;
    try {
        head += "HTTP/1.1 204 No Content\r\n";
        head += "Cache-Control: no-store, no-cache\r\n";
        head += "X-CRPC-Subscriber: ";
        head += std::to_string(longpoll_subscriber_);
        head += "\r\n";
        head += keep ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
        head += "\r\n";
    } catch (...) {
        write_failed_ = true;
        close_when_drained_ = true;
        stop_reading_ = true;
        return;
    }
    enqueue(std::move(head));
    state_.longpoll_timeouts.fetch_add(1, std::memory_order_relaxed);
    if (!keep) {
        close_when_drained_ = true;
        stop_reading_ = true;
    }
    // A poll is inbound activity for the connection: re-arm the idle bound
    // after the wait that had disabled it.
    note_activity();
    flush_writes();
    arm_probe();
}

void session::longpoll_pump() {
    if (!longpoll_active_.load(std::memory_order_acquire) || finished_ ||
        write_failed_) {
        return;
    }
    push::push_message message;
    bool taken = false;
    try {
        std::lock_guard<std::recursive_mutex> lk(state_.hub_mutex);
        push::hub* hub = state_.hub_for(longpoll_topic_);
        if (hub != nullptr && longpoll_token_ != 0) {
            taken = hub->try_take(longpoll_token_, longpoll_topic_, message);
            if (taken) {
                state_.longpoll_last_seen[longpoll_subscriber_] =
                    shard_ != nullptr ? shard_->now_ns() : 0;
                if (longpoll_reliable_ &&
                    message.mode == push::delivery_mode::reliable) {
                    u64& high = state_.longpoll_delivered[
                        {longpoll_subscriber_, longpoll_topic_}];
                    if (message.seq > high) {
                        high = message.seq;
                    }
                }
            }
        }
    } catch (...) {
        return;
    }
    if (!taken) {
        arm_probe();
        return;
    }
    longpoll_finish_wait();
    longpoll_send_message(message, longpoll_keep_);
    flush_writes();
}

void session::longpoll_open(http::request request, const longpoll_route& route) {
    const bool keep = keep_alive(request);
    if (request.method != http::method::get) {
        state_.bad.fetch_add(1, std::memory_order_relaxed);
        send_longpoll_error(405, "long polling requires GET", request, keep,
                            false);
        return;
    }
    if (request.version != http_version::http_1_1) {
        state_.bad.fetch_add(1, std::memory_order_relaxed);
        send_longpoll_error(400, "long polling requires HTTP/1.1", request,
                            false, false);
        return;
    }

    std::string topic = route.topic;
    std::string value;
    switch (query_param(request.query, "topic", value)) {
    case query_result::invalid:
        state_.bad.fetch_add(1, std::memory_order_relaxed);
        send_longpoll_error(400, "invalid topic parameter", request, keep, false);
        return;
    case query_result::ok:
        topic = std::move(value);
        break;
    case query_result::missing:
        break;
    }
    if (topic.empty()) {
        state_.bad.fetch_add(1, std::memory_order_relaxed);
        send_longpoll_error(400, "long-poll route requires a topic", request,
                            keep, false);
        return;
    }

    push::delivery_mode mode = route.mode;
    switch (query_param(request.query, "mode", value)) {
    case query_result::invalid:
        state_.bad.fetch_add(1, std::memory_order_relaxed);
        send_longpoll_error(400, "invalid mode parameter", request, keep, false);
        return;
    case query_result::ok:
        if (value == "reliable") {
            mode = push::delivery_mode::reliable;
        } else if (value == "unreliable") {
            mode = push::delivery_mode::unreliable;
        } else {
            state_.bad.fetch_add(1, std::memory_order_relaxed);
            send_longpoll_error(400, "mode must be reliable or unreliable",
                                request, keep, false);
            return;
        }
        break;
    case query_result::missing:
        break;
    }

    bool ordered = route.ordered;
    switch (query_param(request.query, "ordered", value)) {
    case query_result::invalid:
        state_.bad.fetch_add(1, std::memory_order_relaxed);
        send_longpoll_error(400, "invalid ordered parameter", request, keep,
                            false);
        return;
    case query_result::ok:
        if (!parse_bool_text(value, ordered)) {
            state_.bad.fetch_add(1, std::memory_order_relaxed);
            send_longpoll_error(400, "ordered must be 0/1 or false/true",
                                request, keep, false);
            return;
        }
        break;
    case query_result::missing:
        break;
    }

    const auto allocate_subscriber = [this]() -> push::subscriber_id {
        const u64 slot =
            state_.next_longpoll_subscriber.fetch_add(
                1, std::memory_order_relaxed) %
            k_longpoll_subscriber_count;
        return k_longpoll_subscriber_base + slot;
    };

    bool have_client_subscriber = false;
    push::subscriber_id subscriber = 0;
    switch (query_param(request.query, "subscriber", value)) {
    case query_result::invalid:
        state_.bad.fetch_add(1, std::memory_order_relaxed);
        send_longpoll_error(400, "invalid subscriber parameter", request, keep,
                            false);
        return;
    case query_result::ok:
        if (!parse_u64_text(value, subscriber) ||
            !is_longpoll_subscriber(subscriber)) {
            state_.bad.fetch_add(1, std::memory_order_relaxed);
            send_longpoll_error(
                400, "subscriber must be in [2^32, 2^33)", request, keep, false);
            return;
        }
        have_client_subscriber = true;
        break;
    case query_result::missing:
        subscriber = allocate_subscriber();
        break;
    }

    const u64 max_timeout_ms =
        state_.options.longpoll.max_timeout.count() > 0
            ? static_cast<u64>(state_.options.longpoll.max_timeout.count())
            : 0;
    u64 timeout_ms =
        state_.options.longpoll.timeout.count() > 0
            ? static_cast<u64>(state_.options.longpoll.timeout.count())
            : 0;
    switch (query_param(request.query, "timeout", value)) {
    case query_result::invalid:
        state_.bad.fetch_add(1, std::memory_order_relaxed);
        send_longpoll_error(400, "invalid timeout parameter", request, keep,
                            false);
        return;
    case query_result::ok: {
        u64 parsed = 0;
        if (!parse_u64_text(value, parsed)) {
            state_.bad.fetch_add(1, std::memory_order_relaxed);
            send_longpoll_error(400, "timeout must be a non-negative u64",
                                request, keep, false);
            return;
        }
        timeout_ms = parsed;
        break;
    }
    case query_result::missing:
        break;
    }
    if (timeout_ms > max_timeout_ms) {
        timeout_ms = max_timeout_ms;
    }
    const std::chrono::milliseconds timeout(
        static_cast<std::chrono::milliseconds::rep>(timeout_ms));

    bool have_cursor = false;
    sse::event_id cursor{};
    if (const std::string* field = request.headers.find("last-event-id")) {
        if (!sse::parse_last_event_id(*field, cursor).ok()) {
            state_.bad.fetch_add(1, std::memory_order_relaxed);
            send_longpoll_error(400, "invalid Last-Event-ID", request, keep,
                                false);
            return;
        }
        have_cursor = true;
    }
    switch (query_param(request.query, "cursor", value)) {
    case query_result::invalid:
        state_.bad.fetch_add(1, std::memory_order_relaxed);
        send_longpoll_error(400, "invalid cursor parameter", request, keep,
                            false);
        return;
    case query_result::ok: {
        sse::event_id parsed{};
        if (!sse::parse_last_event_id(value, parsed).ok()) {
            state_.bad.fetch_add(1, std::memory_order_relaxed);
            send_longpoll_error(400, "cursor must be <epoch>:<seq>", request,
                                keep, false);
            return;
        }
        if (have_cursor &&
            (parsed.epoch != cursor.epoch || parsed.seq != cursor.seq)) {
            state_.bad.fetch_add(1, std::memory_order_relaxed);
            send_longpoll_error(400, "cursor and Last-Event-ID disagree",
                                request, keep, false);
            return;
        }
        cursor = parsed;
        have_cursor = true;
        break;
    }
    case query_result::missing:
        break;
    }

    const u64 now = shard_ != nullptr ? shard_->now_ns() : 0;
    const u64 epoch = state_.options.longpoll.epoch;
    push::push_message message;
    bool taken = false;
    bool conflict = false;
    bool restart = false;
    bool topic_missing = false;
    bool unavailable = false;
    bool overloaded = false;

    {
        std::lock_guard<std::recursive_mutex> lk(state_.hub_mutex);
        push::hub* hub = state_.hub_for(topic);
        if (hub == nullptr) {
            unavailable = true;
        } else if (!hub->owns_topic(topic)) {
            topic_missing = true;
        } else {
            state_.longpoll_sweep(now);

            if (state_.sse_subscribers.find(subscriber) !=
                state_.sse_subscribers.end()) {
                conflict = true;
            }
            if (!conflict) {
                auto owner_it = state_.longpoll_owners.find(subscriber);
                if (owner_it != state_.longpoll_owners.end()) {
                    const std::shared_ptr<session> owner =
                        owner_it->second.owner.lock();
                    if (owner && owner.get() != this) {
                        conflict = true;
                    } else if (!owner) {
                        state_.longpoll_owners.erase(owner_it);
                    }
                }
            }
            if (!conflict &&
                state_.longpoll_busy.find(subscriber) !=
                    state_.longpoll_busy.end()) {
                conflict = true;
            }

            if (!conflict) {
                const bool known =
                    state_.longpoll_last_seen.find(subscriber) !=
                        state_.longpoll_last_seen.end() ||
                    state_.longpoll_delivered.find({subscriber, topic}) !=
                        state_.longpoll_delivered.end() ||
                    state_.longpoll_lost.find(subscriber) !=
                        state_.longpoll_lost.end();
                const status subscribed = hub->subscribe(
                    topic, subscriber, mode, ordered,
                    state_.options.longpoll.subscribe);
                const bool already =
                    subscribed.code() == rpc_code::failed_precondition;
                if (!subscribed.ok() && !already) {
                    unavailable = true;
                } else if (!already && known && have_client_subscriber) {
                    // The subscription was lost (overflow/TTL/disconnect) and
                    // the client reused the old id. Hand out a fresh id and a
                    // fresh subscription so the dedup key cannot collide with
                    // the old sequence space.
                    const push::subscriber_id fresh = allocate_subscriber();
                    (void)hub->unsubscribe_all(subscriber);
                    state_.longpoll_forget(subscriber);
                    subscriber = fresh;
                    have_cursor = false;
                    cursor = sse::event_id{};
                    const status again = hub->subscribe(
                        topic, subscriber, mode, ordered,
                        state_.options.longpoll.subscribe);
                    if (!again.ok()) {
                        unavailable = true;
                    } else {
                        state_.longpoll_delivered.erase({subscriber, topic});
                        // A subscription now exists even if the following
                        // attach fails; record activity so the TTL sweep can
                        // find and remove this ownerless residue.
                        state_.longpoll_last_seen[subscriber] = now;
                    }
                } else if (!already) {
                    state_.longpoll_delivered.erase({subscriber, topic});
                    state_.longpoll_last_seen[subscriber] = now;
                }
            }

            if (!conflict && !unavailable) {
                auto high_it =
                    state_.longpoll_delivered.find({subscriber, topic});
                const u64 high =
                    high_it == state_.longpoll_delivered.end() ? 0
                                                               : high_it->second;
                u64 accepted = 0;
                if (have_cursor && mode == push::delivery_mode::reliable &&
                    cursor.epoch == epoch) {
                    accepted = std::min(cursor.seq, high);
                    if (cursor.seq > high) {
                        state_.longpoll_ack_clamped.fetch_add(
                            1, std::memory_order_relaxed);
                    }
                    if (accepted != 0) {
                        (void)hub->ack(subscriber, topic, accepted);
                    }
                }

                bool reuse = false;
                auto owner_it = state_.longpoll_owners.find(subscriber);
                if (owner_it != state_.longpoll_owners.end() &&
                    owner_it->second.token != push::hub::invalid_connection) {
                    const std::shared_ptr<session> owner =
                        owner_it->second.owner.lock();
                    reuse = owner && owner.get() == this &&
                            owner_it->second.token == longpoll_token_;
                }
                push::hub::connection_id token = longpoll_token_;
                if (reuse) {
                    token = owner_it->second.token;
                } else {
                    token = hub->attach(subscriber, push::attach_replay::none);
                    if (token == push::hub::invalid_connection) {
                        unavailable = true;
                    } else {
                        longpoll_token_ = token;
                        state_.longpoll_owners[subscriber] =
                            server_state::longpoll_owner{weak_from_this(),
                                                         token};
                        // The owner is visible before the response/wait path
                        // can return early (409/503): record activity now so
                        // the TTL sweep can never see an owner without a
                        // last_seen entry.
                        state_.longpoll_last_seen[subscriber] = now;
                    }
                }

                if (!unavailable) {
                    longpoll_subscriber_ = subscriber;
                    longpoll_topic_ = topic;
                    longpoll_reliable_ =
                        mode == push::delivery_mode::reliable;
                    longpoll_epoch_ = epoch;
                    longpoll_keep_ = keep;

                    if (have_cursor &&
                        mode == push::delivery_mode::reliable &&
                        cursor.epoch == epoch && accepted < high) {
                        const status front =
                            hub->retransmit_front(subscriber, topic);
                        if (!front.ok()) {
                            if (front.code() == rpc_code::not_found ||
                                front.code() == rpc_code::not_acknowledged ||
                                front.code() == rpc_code::resource_exhausted) {
                                restart = true;
                            } else {
                                unavailable = true;
                            }
                        }
                    }
                    if (restart) {
                        // Drop the old subscription and its sequence space. The
                        // last_seen tombstone is kept so a retry with the same
                        // client-supplied id is handed a fresh server id
                        // (D13), and the next poll neither acks the old cursor
                        // nor replays the old sequence.
                        (void)hub->unsubscribe_all(subscriber);
                        state_.longpoll_owners.erase(subscriber);
                        for (auto it = state_.longpoll_delivered.begin();
                             it != state_.longpoll_delivered.end();) {
                            if (it->first.first == subscriber) {
                                it = state_.longpoll_delivered.erase(it);
                            } else {
                                ++it;
                            }
                        }
                        state_.longpoll_last_seen[subscriber] = now;
                        longpoll_token_ = 0;
                        longpoll_subscriber_ = 0;
                        longpoll_topic_.clear();
                    } else if (!unavailable) {
                        state_.longpoll_last_seen[subscriber] = now;
                        taken = hub->try_take(token, topic, message);
                        if (taken &&
                            longpoll_reliable_ &&
                            message.mode == push::delivery_mode::reliable) {
                            u64& delivered = state_.longpoll_delivered[
                                {subscriber, topic}];
                            if (message.seq > delivered) {
                                delivered = message.seq;
                            }
                        }
                    }
                }
            }

            if (!conflict && !restart && !unavailable && !topic_missing &&
                !taken && timeout.count() > 0) {
                const std::size_t waiter_cap =
                    state_.options.longpoll.max_waiters;
                if (waiter_cap != 0 &&
                    state_.longpoll_waiters.load(std::memory_order_relaxed) >=
                        waiter_cap) {
                    overloaded = true;
                } else {
                    state_.longpoll_busy.insert(subscriber);
                    state_.longpoll_waiters.fetch_add(1,
                                                      std::memory_order_relaxed);
                    state_.longpoll_last_seen[subscriber] = now;
                    longpoll_deadline_ns_ =
                        now + static_cast<u64>(
                                  std::chrono::duration_cast<
                                      std::chrono::nanoseconds>(timeout)
                                      .count());
                    idle_deadline_ns_ = 0;
                    request_deadline_ns_ = 0;
                    longpoll_active_.store(true, std::memory_order_release);
                }
            }
        }
    }

    if (conflict) {
        state_.longpoll_conflicts.fetch_add(1, std::memory_order_relaxed);
        send_longpoll_error(409, "subscriber already has an active transport",
                            request, keep, false);
        return;
    }
    if (restart) {
        state_.longpoll_restarts.fetch_add(1, std::memory_order_relaxed);
        send_longpoll_error(409, "restart required: replay window or budget",
                            request, keep, false);
        return;
    }
    if (topic_missing) {
        send_longpoll_error(404, "topic is not owned by this shard", request,
                            keep, false);
        return;
    }
    if (overloaded) {
        state_.longpoll_overloaded.fetch_add(1, std::memory_order_relaxed);
        send_longpoll_error(503, "too many waiting polls", request, keep, true);
        return;
    }
    if (unavailable) {
        send_longpoll_error(503, "long polling is unavailable", request, keep,
                            true);
        return;
    }
    if (taken) {
        note_activity();
        longpoll_send_message(message, keep);
        return;
    }
    if (timeout.count() <= 0) {
        longpoll_send_timeout(keep);
        return;
    }
    // The wait is armed; a publish wake or the probe deadline drives it.
    arm_probe();
}

// --- session: HTTP/2 ---------------------------------------------------------

void session::process_h2(std::string input) {
    h2_buf_.append(input);
    for (;;) {
        if (h2_buf_.size() < h2::frame_header_size) {
            return;
        }
        const span<const std::byte> head(
            reinterpret_cast<const std::byte*>(h2_buf_.data()),
            h2::frame_header_size);
        h2::frame_header frame;
        const status decoded = h2::decode_frame_header(
            head, frame, state_.options.h2_max_frame_size);
        if (!decoded.ok()) {
            h2_connection_error(h2::error_code::frame_size_error);
            return;
        }
        const std::size_t total = h2::frame_header_size + frame.length;
        if (h2_buf_.size() < total) {
            return;
        }
        const std::byte* payload =
            reinterpret_cast<const std::byte*>(h2_buf_.data()) + h2::frame_header_size;
        const h2::h2_error error = h2_.receive(
            frame, span<const std::byte>(payload, frame.length));

        // `last_data()` borrows the receive payload, which points into
        // `h2_buf_`. Copy it before erasing the buffer.
        std::string data;
        if (frame.type == h2::frame_type::data) {
            const span<const std::byte> body = h2_.last_data();
            data.assign(reinterpret_cast<const char*>(body.data()), body.size());
        }
        h2_buf_.erase(0, total);

        if (!error.ok()) {
            if (error.scope == h2::error_scope::connection) {
                h2_connection_error(error.code);
                return;
            }
            h2_send_rst(error.stream_id, error.code);
            h2_streams_.erase(error.stream_id);
            h2_end_pending_.erase(error.stream_id);
            continue;
        }

        if (frame.type == h2::frame_type::headers ||
            frame.type == h2::frame_type::continuation) {
            h2_last_peer_stream_ = frame.stream_id;
        }
        after_h2_receive(frame, std::move(data));
        if (stop_reading_) {
            return;
        }
    }
}

void session::after_h2_receive(const h2::frame_header& frame, std::string data) {
    if (h2_.pending_settings_ack()) {
        h2_send_settings_ack();
        h2_.settings_acknowledge();
    }
    if (h2_.pending_ping_ack()) {
        h2_send_ping_ack();
        h2_.ping_acknowledge();
    }

    if (h2_.last_headers_present()) {
        const u32 stream_id = h2_.last_headers_stream();
        const http::header_list fields = h2_.last_headers();
        h2_.clear_last_headers();
        build_h2_request(stream_id, fields);
        if (h2_.snapshot(stream_id).remote_end_stream) {
            dispatch_h2(stream_id);
        }
    }

    if (frame.type == h2::frame_type::data) {
        const u32 stream_id = frame.stream_id;
        auto it = h2_streams_.find(stream_id);
        if (it != h2_streams_.end() && it->second.headers_ready) {
            if (it->second.request.body.size() + data.size() >
                state_.options.limits.body_bytes) {
                h2_send_rst(stream_id, h2::error_code::enhance_your_calm);
                h2_streams_.erase(it);
                h2_end_pending_.erase(stream_id);
            } else if (!data.empty()) {
                it->second.request.body.append(data);
            }
        }
        // We consumed the data immediately; advertise the restored windows.
        const h2::window_credit credit =
            h2_.release_received(stream_id, frame.length);
        if (credit.connection != 0) {
            h2_send_window_update(0, credit.connection);
        }
        if (credit.stream != 0) {
            h2_send_window_update(stream_id, credit.stream);
        }
        if (h2_.snapshot(stream_id).remote_end_stream) {
            dispatch_h2(stream_id);
        }
    }

    if (h2_.last_reset_stream() != 0 &&
        h2_.last_reset_stream() != h2_last_reset_seen_) {
        const u32 stream_id = h2_.last_reset_stream();
        h2_last_reset_seen_ = stream_id;
        h2_streams_.erase(stream_id);
        h2_end_pending_.erase(stream_id);
        // The peer reset the SSE stream. Detach the hub subscription; the
        // connection itself stays usable for its remaining streams.
        if (sse_active_.load(std::memory_order_acquire) &&
            stream_id == sse_stream_id_) {
            sse_reset();
        }
    }

    if (h2_.goaway_received()) {
        // The peer is shutting the connection down: end every SSE stream.
        if (sse_active_.load(std::memory_order_acquire)) {
            sse_close(false);
        }
        if (!h2_goaway_sent_) {
            h2_send_goaway(h2::error_code::no_error);
        }
        close_when_drained_ = true;
    }

    if (h2_.has_parked()) {
        h2_resume_parked();
    }
}

void session::build_h2_request(u32 stream_id, const header_list& fields) {
    http::request request;
    request.version = http_version::http_1_1;
    bool have_method = false;
    bool have_path = false;
    bool have_scheme = false;
    bool bad_pseudo = false;

    for (const header& field : fields) {
        if (!field.name.empty() && field.name.front() == ':') {
            if (field.name == ":method") {
                request.method_token = field.value;
                request.method = http::method_from_token(field.value);
                have_method = true;
            } else if (field.name == ":path") {
                request.target = field.value;
                have_path = true;
            } else if (field.name == ":scheme") {
                have_scheme = true;
            } else if (field.name == ":authority") {
                request.headers.add("host", field.value);
            } else {
                bad_pseudo = true;
            }
        } else {
            request.headers.add(lowercase(field.name), field.value);
        }
    }

    http::target_form form = http::target_form::origin;
    const status split =
        have_path ? http::split_target(request.target, form, request.path,
                                       request.query)
                  : status{rpc_error(rpc_code::protocol_error)};
    if (bad_pseudo || !have_method || !have_path || !have_scheme || !split.ok()) {
        h2_send_rst(stream_id, h2::error_code::protocol_error);
        h2_streams_.erase(stream_id);
        return;
    }

    pending_h2_stream stream;
    stream.headers_ready = true;
    stream.request = std::move(request);
    if (shard_ != nullptr && state_.options.request_timeout.count() > 0) {
        stream.deadline_ns =
            shard_->now_ns() +
            static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 state_.options.request_timeout)
                                 .count());
    }
    h2_streams_[stream_id] = std::move(stream);
}

void session::dispatch_h2(u32 stream_id) {
    auto it = h2_streams_.find(stream_id);
    if (it == h2_streams_.end() || !it->second.headers_ready) {
        return;
    }
    const bool head_only = it->second.request.method == http::method::head;
    http::request routed = std::move(it->second.request);
    h2_streams_.erase(it);
    if (head_only) {
        routed.method = http::method::get;
        routed.method_token = "GET";
    }
    if (const int forwarded = stamp_forwarded(routed); forwarded != 200) {
        http::response response =
            make_problem(forwarded, "the forwarding header is malformed",
                         routed.path);
        response.version = routed.version;
        state_.handled.fetch_add(1, std::memory_order_relaxed);
        send_h2_response(stream_id, response, head_only);
        return;
    }
    // Request content coding, file download and response content coding are
    // HTTP/1.1-only in this slice: HTTP/2 requests that would need them fail
    // closed with a complete response instead of a silent identity downgrade.
    const auto h2_not_implemented = [&](std::string_view detail) {
        http::response response;
        response.status_code = 501;
        response.reason = "Not Implemented";
        response.headers.add("Content-Type", "text/plain; charset=utf-8");
        response.body = detail;
        state_.handled.fetch_add(1, std::memory_order_relaxed);
        send_h2_response(stream_id, response, head_only);
    };
    if (const std::string* encoding = routed.headers.find("content-encoding")) {
        const request_encoding_plan plan = plan_request_decoding(*encoding);
        if (!plan.ok || plan.encoded) {
            h2_not_implemented("request content coding is not supported over HTTP/2\n");
            return;
        }
    }
    if (find_download_route(state_.download_routes, routed.path).route != nullptr) {
        h2_not_implemented("file download is not supported over HTTP/2\n");
        return;
    }
    if (state_.options.enable_response_coding) {
        const std::string* accept = routed.headers.find("accept-encoding");
        const negotiation neg = negotiate_response(
            accept != nullptr ? *accept : std::string{}, state_.options.coding);
        if (!neg.acceptable) {
            http::response response =
                make_problem(406, "no acceptable content coding", routed.path);
            response.version = routed.version;
            merge_vary_accept_encoding(response.headers);
            state_.handled.fetch_add(1, std::memory_order_relaxed);
            send_h2_response(stream_id, response, head_only);
            return;
        }
        // HTTP/2 does not encode responses in this slice, but an acceptable
        // gzip/deflate offer must NOT fail the request: serve identity and
        // advertise the variance (a normal h2 client sends gzip by default).
    }
    // SSE is served over HTTP/2 when the server is single-shard: the request
    // opens one stream and the events are DATA frames (see `sse_open_h2`).
    if (const sse_route* sse = find_sse_route(state_.sse_routes, routed.path)) {
        if (head_only || routed.method != http::method::get) {
            http::response response =
                make_problem(405, "SSE requires GET", routed.path);
            response.version = routed.version;
            state_.handled.fetch_add(1, std::memory_order_relaxed);
            send_h2_response(stream_id, response, head_only);
            return;
        }
        sse_open_h2(stream_id, std::move(routed), *sse);
        return;
    }
    if (find_longpoll_route(state_.longpoll_routes, routed.path) != nullptr) {
        http::response response;
        response.status_code = 501;
        response.reason = "Not Implemented";
        response.headers.add("Content-Type", "text/plain; charset=utf-8");
        response.body = "long polling is not supported over HTTP/2\n";
        state_.handled.fetch_add(1, std::memory_order_relaxed);
        send_h2_response(stream_id, response, head_only);
        return;
    }
    http::response response;
    if (state_.router != nullptr) {
        (void)state_.router->handle(routed, response);
    } else {
        response = make_problem(404, "no router configured", routed.path);
    }
    if (state_.options.enable_response_coding &&
        !response_forbids_body(response.status_code)) {
        // Identity is served, but the negotiation is still advertised.
        merge_vary_accept_encoding(response.headers);
    }
    state_.handled.fetch_add(1, std::memory_order_relaxed);
    send_h2_response(stream_id, response, head_only);
}

void session::h2_send_header_block(u32 stream_id, const std::string& block,
                                   bool end_stream) {
    const std::size_t max_frame = h2_.remote_settings().max_frame_size;
    const std::size_t first = std::min(block.size(), max_frame);
    // END_HEADERS belongs on the frame that completes the block. When the block
    // spills into CONTINUATION frames the HEADERS frame must not carry it
    // (RFC 7540 6.2); END_STREAM is independent and still applies to HEADERS.
    const bool whole = first == block.size();
    h2::frame_header header;
    header.type = h2::frame_type::headers;
    header.flags = static_cast<u8>(
        (whole ? h2::flags::end_headers : static_cast<u8>(0)) |
        (end_stream ? h2::flags::end_stream : static_cast<u8>(0)));
    header.stream_id = stream_id;
    header.length = static_cast<u32>(first);
    std::string header_frame(h2::frame_header_size + first, '\0');
    std::size_t written = 0;
    (void)h2::encode_frame(
        span<std::byte>(reinterpret_cast<std::byte*>(header_frame.data()),
                        header_frame.size()),
        header,
        span<const std::byte>(reinterpret_cast<const std::byte*>(block.data()),
                             first),
        written);
    enqueue(std::move(header_frame));
    std::size_t offset = first;
    while (offset < block.size()) {
        const std::size_t chunk = std::min(block.size() - offset, max_frame);
        const bool last = offset + chunk == block.size();
        h2::frame_header continuation;
        continuation.type = h2::frame_type::continuation;
        continuation.flags = last ? h2::flags::end_headers : static_cast<u8>(0);
        continuation.stream_id = stream_id;
        continuation.length = static_cast<u32>(chunk);
        std::string frame(h2::frame_header_size + chunk, '\0');
        std::size_t count = 0;
        (void)h2::encode_frame(
            span<std::byte>(reinterpret_cast<std::byte*>(frame.data()),
                            frame.size()),
            continuation,
            span<const std::byte>(
                reinterpret_cast<const std::byte*>(block.data() + offset), chunk),
            count);
        enqueue(std::move(frame));
        offset += chunk;
    }
}

bool session::h2_send_sse_head(u32 stream_id) {
    std::vector<h2::header_field> fields;
    fields.push_back({":status", "200"});
    fields.push_back({"content-type", "text/event-stream; charset=utf-8"});
    fields.push_back({"cache-control", "no-cache"});
    fields.push_back({"x-accel-buffering", "no"});
    std::string block;
    const status encoded = h2_.encoder().encode(
        span<const h2::header_field>(fields.data(), fields.size()), block);
    if (!encoded.ok()) {
        h2_send_rst(stream_id, h2::error_code::internal_error);
        return false;
    }
    // No Content-Length (a stream has no fixed length) and no END_STREAM (the
    // events follow on the same stream).
    h2_send_header_block(stream_id, block, false);
    return true;
}

void session::send_h2_response(u32 stream_id, http::response& response,
                               bool head_only) {
    if (h2_.state_of(stream_id) == h2::stream_state::closed) {
        return;
    }
    std::vector<h2::header_field> fields;
    fields.push_back({":status", std::to_string(response.status_code)});
    bool have_content_length = false;
    for (const header& field : response.headers) {
        const std::string name = lowercase(field.name);
        // Connection-specific fields are forbidden in HTTP/2 (RFC 9113 8.2.2).
        if (name == "connection" || name == "keep-alive" ||
            name == "transfer-encoding" || name == "upgrade" ||
            name == "proxy-connection") {
            continue;
        }
        if (name == "content-length") {
            have_content_length = true;
        }
        fields.push_back({name, field.value});
    }
    const bool body_forbidden = response_forbids_body(response.status_code);
    const bool send_body = !head_only && !body_forbidden && !response.body.empty();
    if (!body_forbidden && !have_content_length) {
        fields.push_back({"content-length",
                          std::to_string(response.body.size())});
    }

    std::string block;
    const status encoded = h2_.encoder().encode(
        span<const h2::header_field>(fields.data(), fields.size()), block);
    if (!encoded.ok()) {
        h2_send_rst(stream_id, h2::error_code::internal_error);
        return;
    }

    const bool end_stream = !send_body;
    h2_send_header_block(stream_id, block, end_stream);

    if (!send_body) {
        (void)h2_.local_end_stream(stream_id);
        state_.responded.fetch_add(1, std::memory_order_relaxed);
        flush_writes();
        return;
    }

    std::size_t sent = 0;
    bool parked = false;
    const h2::h2_error error = h2_.send_data(
        stream_id, as_bytes(response.body), sent, parked);
    if (!error.ok()) {
        h2_send_rst(stream_id, error.code);
        return;
    }
    if (sent != 0) {
        h2::frame_header data_frame;
        data_frame.type = h2::frame_type::data;
        data_frame.flags = 0;
        data_frame.stream_id = stream_id;
        data_frame.length = static_cast<u32>(sent);
        std::string frame(h2::frame_header_size + sent, '\0');
        std::size_t count = 0;
        (void)h2::encode_frame(
            span<std::byte>(reinterpret_cast<std::byte*>(frame.data()), frame.size()),
            data_frame,
            span<const std::byte>(
                reinterpret_cast<const std::byte*>(response.body.data()), sent),
            count);
        enqueue(std::move(frame));
    }
    h2_end_pending_.insert(stream_id);
    state_.responded.fetch_add(1, std::memory_order_relaxed);
    h2_flush(stream_id);
    flush_writes();
}

void session::h2_flush(u32 stream_id) {
    for (;;) {
        std::vector<std::byte> out;
        bool flushed = false;
        const h2::h2_error error = h2_.flush_parked(stream_id, 0, out, flushed);
        if (!error.ok()) {
            h2_end_pending_.erase(stream_id);
            return;
        }
        if (!flushed || out.empty()) {
            break;
        }
        h2::frame_header header;
        header.type = h2::frame_type::data;
        header.flags = 0;
        header.stream_id = stream_id;
        header.length = static_cast<u32>(out.size());
        std::string frame(h2::frame_header_size + out.size(), '\0');
        std::size_t count = 0;
        (void)h2::encode_frame(
            span<std::byte>(reinterpret_cast<std::byte*>(frame.data()), frame.size()),
            header, span<const std::byte>(out.data(), out.size()), count);
        enqueue(std::move(frame));
    }

    if (h2_.parked_bytes(stream_id) != 0) {
        arm_probe();
        return;
    }
    if (h2_end_pending_.erase(stream_id) != 0) {
        const h2::h2_error ending = h2_.local_end_stream(stream_id);
        if (ending.ok()) {
            h2::frame_header header;
            header.type = h2::frame_type::data;
            header.flags = h2::flags::end_stream;
            header.stream_id = stream_id;
            header.length = 0;
            std::string frame(h2::frame_header_size, '\0');
            std::size_t count = 0;
            (void)h2::encode_frame(
                span<std::byte>(reinterpret_cast<std::byte*>(frame.data()),
                                frame.size()),
                header, span<const std::byte>(), count);
            enqueue(std::move(frame));
        }
    }
}

void session::h2_resume_parked() {
    const std::vector<u32> parked = h2_.parked_streams();
    for (const u32 stream_id : parked) {
        h2_flush(stream_id);
    }
    if (h2_.has_parked()) {
        arm_probe();
    }
    flush_writes();
}

void session::h2_send_initial_settings() {
    if (h2_settings_sent_) {
        return;
    }
    h2_settings_sent_ = true;
    h2::setting entries[5];
    entries[0].id = static_cast<u16>(h2::setting_id::max_concurrent_streams);
    entries[0].value = state_.options.h2_max_concurrent_streams;
    entries[1].id = static_cast<u16>(h2::setting_id::initial_window_size);
    entries[1].value = state_.options.h2_initial_window_size;
    entries[2].id = static_cast<u16>(h2::setting_id::max_frame_size);
    entries[2].value = state_.options.h2_max_frame_size;
    entries[3].id = static_cast<u16>(h2::setting_id::max_header_list_size);
    entries[3].value = state_.options.h2_max_header_list_size;
    entries[4].id = static_cast<u16>(h2::setting_id::enable_push);
    entries[4].value = 0;

    std::string payload(5 * 6, '\0');
    std::size_t written = 0;
    (void)h2::encode_settings(
        span<std::byte>(reinterpret_cast<std::byte*>(payload.data()), payload.size()),
        span<const h2::setting>(entries, 5), written);
    payload.resize(written);

    h2::frame_header header;
    header.type = h2::frame_type::settings;
    header.flags = 0;
    header.stream_id = 0;
    header.length = static_cast<u32>(payload.size());
    std::string frame(h2::frame_header_size + payload.size(), '\0');
    std::size_t count = 0;
    (void)h2::encode_frame(
        span<std::byte>(reinterpret_cast<std::byte*>(frame.data()), frame.size()),
        header, as_bytes(payload), count);
    enqueue(std::move(frame));
    flush_writes();
}

void session::h2_send_settings_ack() {
    h2::frame_header header;
    header.type = h2::frame_type::settings;
    header.flags = h2::flags::ack;
    header.stream_id = 0;
    header.length = 0;
    std::string frame(h2::frame_header_size, '\0');
    std::size_t count = 0;
    (void)h2::encode_frame(
        span<std::byte>(reinterpret_cast<std::byte*>(frame.data()), frame.size()),
        header, span<const std::byte>(), count);
    enqueue(std::move(frame));
    flush_writes();
}

void session::h2_send_ping_ack() {
    const h2::ping_opaque opaque = h2_.last_ping();
    std::string payload(8, '\0');
    std::size_t written = 0;
    (void)h2::encode_ping(
        span<std::byte>(reinterpret_cast<std::byte*>(payload.data()), payload.size()),
        opaque, written);
    h2::frame_header header;
    header.type = h2::frame_type::ping;
    header.flags = h2::flags::ack;
    header.stream_id = 0;
    header.length = 8;
    std::string frame(h2::frame_header_size + 8, '\0');
    std::size_t count = 0;
    (void)h2::encode_frame(
        span<std::byte>(reinterpret_cast<std::byte*>(frame.data()), frame.size()),
        header,
        span<const std::byte>(reinterpret_cast<const std::byte*>(payload.data()), 8),
        count);
    enqueue(std::move(frame));
    flush_writes();
}

void session::h2_send_ping() {
    h2::ping_opaque opaque{};
    const u64 now = shard_ != nullptr ? shard_->now_ns() : 0;
    for (std::size_t i = 0; i < 8; ++i) {
        opaque[i] = static_cast<std::byte>((now >> (i * 8)) & 0xFFu);
    }
    std::string payload(8, '\0');
    std::size_t written = 0;
    (void)h2::encode_ping(
        span<std::byte>(reinterpret_cast<std::byte*>(payload.data()), payload.size()),
        opaque, written);
    h2::frame_header header;
    header.type = h2::frame_type::ping;
    header.flags = 0;
    header.stream_id = 0;
    header.length = 8;
    std::string frame(h2::frame_header_size + 8, '\0');
    std::size_t count = 0;
    (void)h2::encode_frame(
        span<std::byte>(reinterpret_cast<std::byte*>(frame.data()), frame.size()),
        header,
        span<const std::byte>(reinterpret_cast<const std::byte*>(payload.data()), 8),
        count);
    enqueue(std::move(frame));
    flush_writes();
}

void session::h2_send_goaway(h2::error_code code) {
    if (h2_goaway_sent_) {
        return;
    }
    h2_goaway_sent_ = true;
    h2::goaway_payload value;
    value.last_stream_id = h2_last_peer_stream_;
    value.code = code;
    std::string payload(8, '\0');
    std::size_t written = 0;
    (void)h2::encode_goaway(
        span<std::byte>(reinterpret_cast<std::byte*>(payload.data()), payload.size()),
        value, written);
    h2::frame_header header;
    header.type = h2::frame_type::goaway;
    header.flags = 0;
    header.stream_id = 0;
    header.length = static_cast<u32>(payload.size());
    std::string frame(h2::frame_header_size + payload.size(), '\0');
    std::size_t count = 0;
    (void)h2::encode_frame(
        span<std::byte>(reinterpret_cast<std::byte*>(frame.data()), frame.size()),
        header, as_bytes(payload), count);
    enqueue(std::move(frame));
    flush_writes();
}

void session::h2_send_rst(u32 stream_id, h2::error_code code) {
    if (h2_.stream_exists(stream_id) &&
        h2_.state_of(stream_id) != h2::stream_state::closed) {
        (void)h2_.reset_stream(stream_id, code);
        // Remember our own reset so the inbound bookkeeping path does not treat
        // it as a peer RST_STREAM on the next frame.
        h2_last_reset_seen_ = h2_.last_reset_stream();
    }
    std::string payload(4, '\0');
    std::size_t written = 0;
    (void)h2::encode_rst_stream(
        span<std::byte>(reinterpret_cast<std::byte*>(payload.data()), payload.size()),
        code, written);
    h2::frame_header header;
    header.type = h2::frame_type::rst_stream;
    header.flags = 0;
    header.stream_id = stream_id;
    header.length = 4;
    std::string frame(h2::frame_header_size + 4, '\0');
    std::size_t count = 0;
    (void)h2::encode_frame(
        span<std::byte>(reinterpret_cast<std::byte*>(frame.data()), frame.size()),
        header,
        span<const std::byte>(reinterpret_cast<const std::byte*>(payload.data()), 4),
        count);
    enqueue(std::move(frame));
    flush_writes();
}

void session::h2_send_window_update(u32 stream_id, u32 increment) {
    if (increment == 0) {
        return;
    }
    std::string payload(4, '\0');
    std::size_t written = 0;
    (void)h2::encode_window_update(
        span<std::byte>(reinterpret_cast<std::byte*>(payload.data()), payload.size()),
        increment, written);
    h2::frame_header header;
    header.type = h2::frame_type::window_update;
    header.flags = 0;
    header.stream_id = stream_id;
    header.length = 4;
    std::string frame(h2::frame_header_size + 4, '\0');
    std::size_t count = 0;
    (void)h2::encode_frame(
        span<std::byte>(reinterpret_cast<std::byte*>(frame.data()), frame.size()),
        header,
        span<const std::byte>(reinterpret_cast<const std::byte*>(payload.data()), 4),
        count);
    enqueue(std::move(frame));
    flush_writes();
}

void session::h2_connection_error(h2::error_code code) {
    h2_send_goaway(code);
    close_when_drained_ = true;
    stop_reading_ = true;
}

// --- session: output ---------------------------------------------------------

void session::enqueue(std::string bytes) {
    if (finished_) {
        return;
    }
    out_bytes_ += bytes.size();
    out_.push_back(std::move(bytes));
    if (out_bytes_ > state_.options.egress_cap_bytes) {
        // Backpressure: stop reading until the queue drains. The bytes already
        // parsed keep their response, so nothing is dropped (R18).
        std::lock_guard<std::mutex> lk(mu_);
        read_paused_ = true;
    }
}

void session::flush_writes() {
    if (finished_ || write_failed_ || write_in_flight_ || out_.empty()) {
        return;
    }
    in_flight_ = std::move(out_.front());
    out_.pop_front();
    out_bytes_ -= in_flight_.size();
    write_in_flight_ = true;

    auto* op = new (std::nothrow) write_completion();
    if (op == nullptr) {
        write_in_flight_ = false;
        write_failed_ = true;
        close_when_drained_ = true;
        return;
    }
    op->owner = shared_from_this();
    const span<const std::byte> bytes = as_bytes(in_flight_);
    if (out_sink_ == nullptr) {
        delete op;
        write_in_flight_ = false;
        write_failed_ = true;
        close_when_drained_ = true;
        return;
    }
    out_sink_->push(bytes, op);
}

void session::maybe_finish() {
    if (finished_) {
        return;
    }
    const bool drained = out_.empty() && !write_in_flight_;
    if (!drained) {
        return;
    }
    // `input_closed_` is written by whichever thread observes EOF or an error
    // (reader_source::on_eof/on_error) under `mu_`; read it the same way. Reading
    // the plain bool here raced with those writers (TSan: maybe_finish vs
    // handle_eof), and a torn/stale read would leave a drained session open.
    bool input_closed = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        input_closed = input_closed_;
    }
    if (close_when_drained_ || input_closed) {
        finish_now();
    }
}

void session::finish_now() {
    if (finished_) {
        return;
    }
    finished_ = true;
    // Detach BEFORE the transport and the hub token are destroyed. This is the
    // same ordering the push control path requires; a token that outlives its
    // connection would receive deliveries into a dead session. The long-poll
    // path uses release() (keep-queue), never detach().
    sse_detach();
    longpoll_release();
    download_active_.store(false, std::memory_order_release);
    download_.reset();
    output_stall_deadline_ns_ = 0;
    close_when_drained_ = true;
    stop_reading_ = true;
    if (shard_ != nullptr) {
        (void)shard_->cancel_timer(this);
    }
    probe_armed_ = false;
    armed_deadline_ns_ = 0;
    out_.clear();
    out_bytes_ = 0;
    in_flight_.clear();
    if (tls_) {
        tls_->close();
    }
    if (transport_) {
        transport_->bind_source(nullptr);
        transport_->close();
    }
    state_.remove_session(shared_from_this());
}

void session::note_activity() noexcept {
    if (sse_active_.load(std::memory_order_relaxed)) {
        // An SSE response is intentionally long-lived and has no inbound
        // traffic of its own; the idle deadline must not be re-armed.
        return;
    }
    if (shard_ == nullptr || state_.options.idle_timeout.count() <= 0) {
        return;
    }
    idle_deadline_ns_ =
        shard_->now_ns() +
        static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             state_.options.idle_timeout)
                             .count());
}

void session::note_request_progress() noexcept {
    if (shard_ == nullptr || state_.options.request_timeout.count() <= 0 ||
        request_deadline_ns_ != 0) {
        return;
    }
    request_deadline_ns_ =
        shard_->now_ns() +
        static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             state_.options.request_timeout)
                             .count());
}

void session::arm_probe() {
    if (finished_ || shard_ == nullptr) {
        return;
    }
    u64 deadline = 0;
    if (phase_ == phase::h2 && h2_.has_parked()) {
        deadline = shard_->now_ns() +
                   static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        state_.options.h2_probe_interval)
                                        .count());
    }
    if (idle_deadline_ns_ != 0 && (deadline == 0 || idle_deadline_ns_ < deadline)) {
        deadline = idle_deadline_ns_;
    }
    if (request_deadline_ns_ != 0 &&
        (deadline == 0 || request_deadline_ns_ < deadline)) {
        deadline = request_deadline_ns_;
    }
    // An open SSE response replaces the idle/request timers with a heartbeat
    // and a slow-read stall deadline (idle_deadline_ns_ is zeroed on open).
    if (sse_active_.load(std::memory_order_relaxed)) {
        if (sse_heartbeat_ns_ != 0 &&
            (deadline == 0 || sse_heartbeat_ns_ < deadline)) {
            deadline = sse_heartbeat_ns_;
        }
        if (sse_stall_deadline_ns_ != 0 &&
            (deadline == 0 || sse_stall_deadline_ns_ < deadline)) {
            deadline = sse_stall_deadline_ns_;
        }
    }
    // HTTP/2 bounds each open stream separately: one stream trickling bytes must
    // not keep another stream's request alive forever.
    for (const auto& item : h2_streams_) {
        const u64 stream_deadline = item.second.deadline_ns;
        if (stream_deadline != 0 && (deadline == 0 || stream_deadline < deadline)) {
            deadline = stream_deadline;
        }
    }
    // A streamed download or a long-poll response keeps the idle deadline and
    // adds its own output stall deadline (the earlier of the two fires first;
    // the deadline list above already includes the idle deadline).
    if ((download_active_.load(std::memory_order_relaxed) ||
         longpoll_response_active_) &&
        output_stall_deadline_ns_ != 0 &&
        (deadline == 0 || output_stall_deadline_ns_ < deadline)) {
        deadline = output_stall_deadline_ns_;
    }
    // A waiting poll has no inbound liveness signal; its own deadline is the
    // only timer besides a publish wake.
    if (longpoll_active_.load(std::memory_order_acquire) &&
        longpoll_deadline_ns_ != 0 &&
        (deadline == 0 || longpoll_deadline_ns_ < deadline)) {
        deadline = longpoll_deadline_ns_;
    }
    if (deadline == 0) {
        return;
    }
    if (probe_armed_ && armed_deadline_ns_ != 0 && armed_deadline_ns_ <= deadline) {
        // The armed timer already fires no later than this deadline; let it.
        return;
    }
    // `runtime::arm_timer` is keyed by the session address, so re-arming
    // replaces the pending entry rather than adding a second one. The entry
    // also keeps the session alive across the wheel-pop/callback window: the
    // runtime pops under its lock and calls the trampoline after releasing it,
    // so a bare `this` could dangle if the last owner dropped its reference.
    probe_armed_ = true;
    armed_deadline_ns_ = deadline;
    std::shared_ptr<session> keep = shared_from_this();
    if (!shard_->arm_timer(this, deadline, &session::probe_trampoline, this,
                           keep)) {
        probe_armed_ = false;
        armed_deadline_ns_ = 0;
    }
}

void session::on_probe() {
    probe_armed_ = false;
    armed_deadline_ns_ = 0;
    if (finished_ || shutdown_requested_.load(std::memory_order_acquire) ||
        shard_ == nullptr) {
        return;
    }
    const u64 now = shard_->now_ns();
    if (sse_active_.load(std::memory_order_acquire)) {
        if (sse_stall_deadline_ns_ != 0 && now >= sse_stall_deadline_ns_) {
            // The peer is connected but stopped draining; staying open would
            // accumulate an unbounded queue behind TCP. This is the backstop
            // for a half-open connection whose writes never fail.
            sse_stall_deadline_ns_ = 0;
            state_.sse_slow_closes.fetch_add(1, std::memory_order_relaxed);
            sse_abort();
            return;
        }
        if (sse_heartbeat_ns_ != 0 && now >= sse_heartbeat_ns_) {
            sse_heartbeat_ns_ =
                now +
                static_cast<u64>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        state_.options.sse.heartbeat)
                        .count());
            sse_send_heartbeat();
        }
        if (sse_stream_id_ != 0) {
            // HTTP/2: retry bytes parked behind a zero window and, if the peer
            // still sends no WINDOW_UPDATE, probe it so a dead peer is detected
            // instead of waited on forever (R18).
            h2_resume_parked();
            if (h2_.has_parked()) {
                h2_send_ping();
            }
        } else {
            flush_writes();
        }
        arm_probe();
        return;
    }
    if (download_active_.load(std::memory_order_acquire)) {
        if (output_stall_deadline_ns_ != 0 && now >= output_stall_deadline_ns_) {
            // The peer is connected but stopped draining the download body.
            output_stall_deadline_ns_ = 0;
            state_.timed_out.fetch_add(1, std::memory_order_relaxed);
            download_active_.store(false, std::memory_order_release);
            download_.reset();
            close_when_drained_ = true;
            stop_reading_ = true;
            finish_now();
            return;
        }
        arm_probe();
        return;
    }
    if (longpoll_active_.load(std::memory_order_acquire)) {
        if (longpoll_deadline_ns_ != 0 && now >= longpoll_deadline_ns_) {
            // A publish wake can be dropped when the shard inbox is full (the
            // SSE path has a heartbeat as backstop; a poll does not), so take
            // one more time before deciding the wait expired.
            longpoll_pump();
            if (longpoll_active_.load(std::memory_order_acquire)) {
                longpoll_finish_wait();
                longpoll_send_timeout(longpoll_keep_);
                flush_writes();
                maybe_finish();
            }
        }
        arm_probe();
        return;
    }
    if (longpoll_response_active_) {
        if (output_stall_deadline_ns_ != 0 && now >= output_stall_deadline_ns_) {
            // The peer stopped draining the polled body. The message stays
            // unacknowledged in the replay window and is replayed by the next
            // poll's cursor gap.
            output_stall_deadline_ns_ = 0;
            longpoll_response_active_ = false;
            state_.timed_out.fetch_add(1, std::memory_order_relaxed);
            close_when_drained_ = true;
            stop_reading_ = true;
            finish_now();
            return;
        }
        arm_probe();
        return;
    }
    if (request_deadline_ns_ != 0 && now >= request_deadline_ns_) {
        // A request that never completes must fail explicitly instead of
        // occupying the connection forever (No-stall rule).
        request_deadline_ns_ = 0;
        state_.timed_out.fetch_add(1, std::memory_order_relaxed);
        if (phase_ == phase::h2) {
            h2_send_goaway(h2::error_code::no_error);
        } else {
            http::request partial = parser_.get_request();
            send_http1_error(408, "request timeout", partial, false);
        }
        close_when_drained_ = true;
        stop_reading_ = true;
        flush_writes();
        return;
    }
    if (idle_deadline_ns_ != 0 && now >= idle_deadline_ns_) {
        idle_deadline_ns_ = 0;
        state_.timed_out.fetch_add(1, std::memory_order_relaxed);
        close_when_drained_ = true;
        stop_reading_ = true;
        flush_writes();
        maybe_finish();
        return;
    }
    if (phase_ == phase::h2 && h2_.has_parked()) {
        h2_resume_parked();
        if (h2_.has_parked()) {
            // A peer that never sends WINDOW_UPDATE must not stall the
            // connection silently: probe it so a dead peer is observed (R18).
            h2_send_ping();
        }
    }
    if (phase_ == phase::h2) {
        // Reset every stream whose request never completed. A stream-level
        // error is enough: the connection and the other streams stay usable.
        for (auto it = h2_streams_.begin(); it != h2_streams_.end();) {
            if (it->second.deadline_ns != 0 && now >= it->second.deadline_ns) {
                const u32 stream_id = it->first;
                it = h2_streams_.erase(it);
                h2_end_pending_.erase(stream_id);
                state_.timed_out.fetch_add(1, std::memory_order_relaxed);
                // h2_send_rst also closes the connection-level record and
                // records our own reset so it is not re-processed.
                h2_send_rst(stream_id, h2::error_code::cancel);
            } else {
                ++it;
            }
        }
    }
    arm_probe();
}

} // namespace

// --- http_server -------------------------------------------------------------

struct http_server::impl {
    server_state state;

    explicit impl(http_server_options options) : state(std::move(options)) {}
};

http_server::builder::builder() = default;
http_server::builder::~builder() = default;

http_server::builder& http_server::builder::add_listener(endpoint ep,
                                                         backend_factory& backend) {
    listener_config config;
    config.ep = std::move(ep);
    config.backend = &backend;
    listeners_.push_back(std::move(config));
    return *this;
}

http_server::builder& http_server::builder::router(const mvc::router& routes) {
    router_ = &routes;
    return *this;
}

http_server::builder& http_server::builder::shards(std::size_t count) {
    options_.shards = count == 0 ? 1 : count;
    return *this;
}

http_server::builder& http_server::builder::options(
    const http_server_options& value) {
    options_ = value;
    return *this;
}

http_server::builder& http_server::builder::set_parser_limits(
    const parser_limits& limits) {
    options_.limits = limits;
    return *this;
}

http_server::builder& http_server::builder::drain_timeout(
    std::chrono::milliseconds value) {
    options_.drain_timeout = value;
    return *this;
}

http_server::builder& http_server::builder::add_sse_route(sse_route route) {
    sse_routes_.push_back(std::move(route));
    return *this;
}

http_server::builder& http_server::builder::add_longpoll_route(
    longpoll_route route) {
    longpoll_routes_.push_back(std::move(route));
    return *this;
}

http_server::builder& http_server::builder::add_download_route(
    download_route route) {
    download_routes_.push_back(std::move(route));
    return *this;
}

std::unique_ptr<http_server> http_server::builder::build() {
    auto result = std::unique_ptr<http_server>(new http_server(options_));
    result->impl_->state.router = router_;
    result->impl_->state.sse_routes = sse_routes_;
    result->impl_->state.longpoll_routes = longpoll_routes_;
    result->impl_->state.download_routes = download_routes_;
    for (const listener_config& config : listeners_) {
        server_state::pending_listener pending;
        pending.ep = config.ep;
        pending.backend = config.backend;
        pending.tls = false;
        result->impl_->state.pending_listeners.push_back(std::move(pending));
    }
    return result;
}

http_server::http_server(http_server_options opt)
    : impl_(std::make_unique<impl>(std::move(opt))) {}

http_server::~http_server() {
    stop();
}

status http_server::listen_tls(const endpoint& ep, backend_factory& backend,
                               io::tls_config config) {
    if (impl_->state.accepting.load(std::memory_order_acquire)) {
        return status{rpc_error(rpc_code::failed_precondition,
                                "listen_tls must be called before start()")};
    }
#if defined(CRPC_HAVE_OPENSSL)
    config.role = io::tls_role::server;
    config.require_alpn = true;
    if (config.alpn_protocols.empty()) {
        config.alpn_protocols = {"h2", "http/1.1"};
    } else {
        bool has_h2 = false;
        bool has_http1 = false;
        for (const std::string& item : config.alpn_protocols) {
            has_h2 = has_h2 || item == "h2";
            has_http1 = has_http1 || item == "http/1.1";
        }
        if (!has_h2) {
            config.alpn_protocols.insert(config.alpn_protocols.begin(), "h2");
        }
        if (!has_http1) {
            config.alpn_protocols.push_back("http/1.1");
        }
    }
    server_state::pending_listener pending;
    pending.ep = ep;
    pending.backend = &backend;
    pending.tls = true;
    impl_->state.pending_listeners.push_back(std::move(pending));
    impl_->state.has_tls = true;
    impl_->state.tls_config = std::move(config);
    return status{};
#else
    (void)ep;
    (void)backend;
    (void)config;
    return status{rpc_error(
        rpc_code::failed_precondition,
        "HTTPS/TLS is not available: the library was built without CRPC_HAVE_OPENSSL")};
#endif
}

status http_server::start() {
    server_state& state = impl_->state;
    if (state.accepting.load(std::memory_order_acquire)) {
        return status{};
    }
    if (state.router == nullptr) {
        state.router = &mvc::default_router();
    }
    if (!state.router->frozen()) {
        return status{rpc_error(rpc_code::failed_precondition,
                                "the MVC router must be frozen before start()")};
    }

    // One hub per shard. A topic is owned by exactly one hub, so a request is
    // routed to the owning shard's hub instead of being refused when the server
    // runs more than one shard.
    const std::size_t shard_count =
        state.options.shards == 0 ? 1 : state.options.shards;
    // Published only after every validation below succeeds, so a failed start()
    // keeps reporting zero (the documented "not running" value).
    std::size_t effective_replay_window = 0;
    for (std::size_t i = 0; i < state.sse_routes.size(); ++i) {
        if (state.sse_routes[i].path.empty()) {
            return status{rpc_error(rpc_code::invalid_argument,
                                    "an SSE route requires a path")};
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (state.sse_routes[j].path == state.sse_routes[i].path) {
                return status{rpc_error(rpc_code::invalid_argument,
                                        "duplicate SSE route path")};
            }
        }
    }

    if (!state.longpoll_routes.empty()) {
        // The hub stamps one epoch into every message, and both SSE and long
        // polling compare cursors against it. Normalize the two routing stubs
        // to a single epoch instead of letting event ids and cursor checks
        // disagree.
        if (state.sse_routes.empty()) {
            state.options.sse.epoch = state.options.longpoll.epoch;
        } else if (state.options.sse.epoch != state.options.longpoll.epoch) {
            return status{rpc_error(
                rpc_code::invalid_argument,
                "SSE and long polling must use the same epoch")};
        } else {
            state.options.longpoll.epoch = state.options.sse.epoch;
        }
        {
            // retransmit_front moves the whole unacknowledged replay window
            // back into the queue, so the queue must be able to hold it
            // (`max_messages >= max_replay`). A replay bound larger than the
            // queue is rejected instead of silently growing the queue's
            // memory bound; a smaller replay bound is raised to the queue bound
            // (the existing equalization). Zero has always meant "one".
            push::subscribe_options& sub =
                state.options.longpoll.subscribe;
            if (sub.max_messages == 0) {
                sub.max_messages = 1;
            }
            if (sub.max_replay == 0) {
                sub.max_replay = 1;
            }
            if (sub.max_replay > sub.max_messages) {
                return status{rpc_error(
                    rpc_code::invalid_argument,
                    "long-poll subscribe.max_replay must not exceed max_messages")};
            }
            if (sub.max_replay < sub.max_messages) {
                sub.max_replay = sub.max_messages;
            }
            effective_replay_window = sub.max_replay;
        }
        for (std::size_t i = 0; i < state.longpoll_routes.size(); ++i) {
            const std::string& path = state.longpoll_routes[i].path;
            if (path.empty() || path.front() != '/') {
                return status{rpc_error(
                    rpc_code::invalid_argument,
                    "a long-poll route requires an absolute path")};
            }
            for (std::size_t j = 0; j < i; ++j) {
                if (state.longpoll_routes[j].path == path) {
                    return status{rpc_error(rpc_code::invalid_argument,
                                            "duplicate long-poll route path")};
                }
            }
            for (const sse_route& sse : state.sse_routes) {
                if (sse.path == path) {
                    return status{rpc_error(
                        rpc_code::invalid_argument,
                        "a long-poll path conflicts with an SSE path")};
                }
            }
            for (const download_route& download : state.download_routes) {
                const bool same = download.path == path;
                const bool prefix =
                    path.size() > download.path.size() &&
                    path.compare(0, download.path.size(), download.path) == 0 &&
                    path[download.path.size()] == '/';
                if (same || prefix) {
                    return status{rpc_error(
                        rpc_code::invalid_argument,
                        "a long-poll path conflicts with a download path")};
                }
            }
        }
    }

    for (std::size_t i = 0; i < state.download_routes.size(); ++i) {
        const std::string& path = state.download_routes[i].path;
        if (path.empty() || path.front() != '/') {
            return status{rpc_error(rpc_code::invalid_argument,
                                    "a download route requires an absolute path")};
        }
        if (path.size() > 1 && path.back() == '/') {
            return status{rpc_error(rpc_code::invalid_argument,
                                    "a download route path must not end with '/'")};
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (state.download_routes[j].path == path) {
                return status{rpc_error(rpc_code::invalid_argument,
                                        "duplicate download route path")};
            }
        }
    }

    if (state.has_tls) {
#if defined(CRPC_HAVE_OPENSSL)
        std::error_code ec;
        state.tls_context = io::tls_context::create(state.tls_config, ec);
        if (!state.tls_context) {
            return status{rpc_error(rpc_code::failed_precondition,
                                    "TLS context creation failed: " + ec.message())};
        }
#else
        return status{rpc_error(
            rpc_code::failed_precondition,
            "HTTPS/TLS is not available: the library was built without CRPC_HAVE_OPENSSL")};
#endif
    }

    if (!state.longpoll_routes.empty()) {
        state.longpoll_replay_window.store(effective_replay_window,
                                           std::memory_order_relaxed);
    }

    try {
        const std::size_t count = shard_count;
        {
            std::lock_guard<std::recursive_mutex> lk(state.hub_mutex);
            push::hub_options hub_options;
            hub_options.shard_count = static_cast<u32>(count);
            hub_options.epoch = state.options.sse.epoch;
            state.hubs.clear();
            for (std::size_t i = 0; i < count; ++i) {
                hub_options.shard_index = static_cast<u32>(i);
                state.hubs.push_back(std::make_unique<push::hub>(hub_options));
            }
            state.sse_delivered.clear();
            state.longpoll_owners.clear();
            state.longpoll_busy.clear();
            state.longpoll_delivered.clear();
            state.longpoll_last_seen.clear();
            state.longpoll_lost.clear();
            state.next_longpoll_sweep_ns = 0;
        }
        for (std::size_t i = 0; i < count; ++i) {
            runtime_options options = state.options.runtime;
            options.start_handler_pool = true;
            auto shard = std::make_unique<runtime>(options);
            shard->start_thread();
            state.shards.push_back(std::move(shard));
        }
        for (const server_state::pending_listener& config :
             state.pending_listeners) {
            if (config.backend == nullptr) {
                stop();
                return status{rpc_error(rpc_code::invalid_argument,
                                        "listener has no backend")};
            }
            std::unique_ptr<listener> bound =
                config.backend->listen(config.ep, listen_options{});
            if (!bound) {
                stop();
                return status{rpc_error(rpc_code::invalid_argument,
                                        "listener could not bind endpoint")};
            }
            state.listeners.push_back(std::move(bound));
            state.acceptors.push_back(
                std::make_unique<server_state::acceptor>(state, config.tls));
        }
        state.accepting.store(true, std::memory_order_release);
        for (std::size_t i = 0; i < state.listeners.size(); ++i) {
            state.listeners[i]->start(state.acceptors[i].get());
        }
        // Periodic long-poll TTL housekeeping. Armed after the shard is
        // running; cancelled by stop() before the hub is torn down.
        state.arm_longpoll_timer();
    } catch (...) {
        stop();
        return status{rpc_code::resource_exhausted};
    }
    return status{};
}

void http_server::stop() noexcept {
    server_state& state = impl_->state;
    const bool was_accepting = state.accepting.exchange(false, std::memory_order_acq_rel);
    (void)was_accepting;
    // Stop the periodic TTL sweep before any hub/session teardown. A callback
    // already running on the shard finishes under `hub_mutex`.
    state.stop_longpoll_timer();

    for (std::unique_ptr<listener>& listener : state.listeners) {
        if (listener) {
            listener->stop();
        }
    }
    state.listeners.clear();

    if (state.shards.empty()) {
        // Nothing was started (or start() failed early). Close anything the
        // backend already handed over.
        const std::vector<std::shared_ptr<session>> sessions = state.snapshot();
        for (const std::shared_ptr<session>& value : sessions) {
            value->force_close();
        }
        {
            std::lock_guard<std::mutex> lk(state.sessions_mutex);
            state.sessions.clear();
        }
        {
            std::lock_guard<std::recursive_mutex> lk(state.hub_mutex);
            state.hubs.clear();
            state.sse_delivered.clear();
            state.sse_subscribers.clear();
            state.longpoll_owners.clear();
            state.longpoll_busy.clear();
            state.longpoll_delivered.clear();
            state.longpoll_last_seen.clear();
            state.longpoll_lost.clear();
            state.longpoll_waiters.store(0, std::memory_order_relaxed);
            state.sse_connections.store(0, std::memory_order_relaxed);
            // Same lifecycle as waiters/sse_connections: a stopped server
            // reports no effective replay bound until the next start().
            state.longpoll_replay_window.store(0, std::memory_order_relaxed);
        }
        state.longpoll_keepalive_timer.reset();
        state.acceptors.clear();
        return;
    }

    std::vector<std::shared_ptr<session>> sessions = state.snapshot();
    for (const std::shared_ptr<session>& value : sessions) {
        value->request_shutdown();
    }

    {
        std::unique_lock<std::mutex> lk(state.sessions_mutex);
        state.drain_cv.wait_for(lk, state.options.drain_timeout, [&state] {
            return state.sessions.empty();
        });
    }

    sessions = state.snapshot();
    for (const std::shared_ptr<session>& value : sessions) {
        value->force_close();
    }
    // Sessions are retained until the shards are joined. A probe timer entry
    // now holds a keep-alive reference to the session, so the callback cannot
    // outlive it; the entry is released when the runtime (and its timer wheel)
    // is destroyed. The session destructor therefore must not touch shard_.
    for (std::unique_ptr<runtime>& shard : state.shards) {
        shard->stop();
    }
    for (std::unique_ptr<runtime>& shard : state.shards) {
        shard->join();
    }
    {
        std::lock_guard<std::mutex> lk(state.sessions_mutex);
        state.sessions.clear();
    }
    {
        std::lock_guard<std::recursive_mutex> lk(state.hub_mutex);
        state.hubs.clear();
        state.sse_delivered.clear();
        state.sse_subscribers.clear();
        state.longpoll_owners.clear();
        state.longpoll_busy.clear();
        state.longpoll_delivered.clear();
        state.longpoll_last_seen.clear();
        state.longpoll_lost.clear();
        state.longpoll_waiters.store(0, std::memory_order_relaxed);
        state.sse_connections.store(0, std::memory_order_relaxed);
        // Same lifecycle as waiters/sse_connections: a stopped server reports
        // no effective replay bound until the next start().
        state.longpoll_replay_window.store(0, std::memory_order_relaxed);
    }
    state.shards.clear();
    state.longpoll_keepalive_timer.reset();
    // `pending_listeners` is builder configuration, not live state: keep it so
    // a later start() can bind the same listeners again. Clearing it made
    // start() return ok() after stop() while binding nothing, which left every
    // client without a connection. The bound `listener` objects themselves are
    // released above. The stored backend pointers are borrowed: the caller
    // must keep every backend alive for the whole server lifetime, including
    // any stop()/start() cycle (see http_server::builder::add_listener()).
    state.tls_context.reset();
    state.acceptors.clear();
}

bool http_server::running() const noexcept {
    return impl_->state.accepting.load(std::memory_order_acquire);
}

http_statistics http_server::stats() const noexcept {
    http_statistics out;
    {
        std::lock_guard<std::mutex> lk(impl_->state.sessions_mutex);
        out.active_connections = impl_->state.sessions.size();
    }
    out.accepted_connections =
        impl_->state.accepted.load(std::memory_order_relaxed);
    out.requests_handled = impl_->state.handled.load(std::memory_order_relaxed);
    out.responses_written = impl_->state.responded.load(std::memory_order_relaxed);
    out.bad_requests = impl_->state.bad.load(std::memory_order_relaxed);
    out.h2_connections = impl_->state.h2_connections.load(std::memory_order_relaxed);
    out.tls_connections = impl_->state.tls_connections.load(std::memory_order_relaxed);
    out.rejected_connections = impl_->state.rejected.load(std::memory_order_relaxed);
    out.timed_out_connections = impl_->state.timed_out.load(std::memory_order_relaxed);
    out.responses_compressed =
        impl_->state.responses_compressed.load(std::memory_order_relaxed);
    out.requests_decoded =
        impl_->state.requests_decoded.load(std::memory_order_relaxed);
    out.downloads_served =
        impl_->state.downloads_served.load(std::memory_order_relaxed);
    out.sse_connections =
        impl_->state.sse_connections.load(std::memory_order_relaxed);
    out.sse_events_delivered =
        impl_->state.sse_events_delivered.load(std::memory_order_relaxed);
    out.sse_slow_closes =
        impl_->state.sse_slow_closes.load(std::memory_order_relaxed);
    out.longpoll_waiters =
        impl_->state.longpoll_waiters.load(std::memory_order_relaxed);
    out.longpoll_events =
        impl_->state.longpoll_events.load(std::memory_order_relaxed);
    out.longpoll_timeouts =
        impl_->state.longpoll_timeouts.load(std::memory_order_relaxed);
    out.longpoll_ack_clamped =
        impl_->state.longpoll_ack_clamped.load(std::memory_order_relaxed);
    out.longpoll_conflicts =
        impl_->state.longpoll_conflicts.load(std::memory_order_relaxed);
    out.longpoll_restarts =
        impl_->state.longpoll_restarts.load(std::memory_order_relaxed);
    out.longpoll_overloaded =
        impl_->state.longpoll_overloaded.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::recursive_mutex> lk(impl_->state.hub_mutex);
        out.longpoll_subscribers = impl_->state.longpoll_owners.size();
        out.longpoll_replay_window =
            impl_->state.longpoll_replay_window.load(
                std::memory_order_relaxed);
        push::hub_stats counters{};
        for (const auto& h : impl_->state.hubs) {
            if (h) {
                const push::hub_stats partial = h->stats();
                counters.published += partial.published;
                counters.delivered += partial.delivered;
                counters.queued_messages += partial.queued_messages;
                counters.dropped_overflow += partial.dropped_overflow;
                counters.dropped_unreliable += partial.dropped_unreliable;
                counters.dropped_disconnected += partial.dropped_disconnected;
                counters.dropped_allocation += partial.dropped_allocation;
            }
        }
        out.sse_hub_published = static_cast<std::size_t>(counters.published);
        out.sse_hub_delivered = static_cast<std::size_t>(counters.delivered);
        out.sse_hub_queued = static_cast<std::size_t>(counters.queued_messages);
        out.sse_hub_dropped = static_cast<std::size_t>(
            counters.dropped_overflow + counters.dropped_unreliable +
            counters.dropped_disconnected + counters.dropped_allocation);
    }
    return out;
}

bool http_server::publish(std::string_view topic, span<const std::byte> body,
                          push::delivery_mode mode) noexcept {
    server_state& state = impl_->state;
    if (topic.empty() || state.hubs.empty() ||
        !state.accepting.load(std::memory_order_acquire)) {
        return false;
    }
    // Route to the hub of the shard that owns the topic.
    const u32 owner =
        push::topic_shard(topic, 0, static_cast<u32>(state.hubs.size()));
    if (owner >= state.shards.size()) {
        return false;
    }
    runtime* shard = state.shards[owner].get();
    if (shard == nullptr) {
        return false;
    }
    std::string topic_copy;
    std::vector<std::byte> body_copy;
    try {
        topic_copy.assign(topic.data(), topic.size());
        body_copy.assign(body.begin(), body.end());
    } catch (...) {
        return false;
    }
    // Fast path: called on the owning shard's own thread (an RPC handler on the
    // same shard, for example). Run inline instead of waiting on a task that
    // cannot be drained without deadlocking the caller.
    if (shard->is_owner_thread()) {
        publish_run(state, topic_copy, body_copy, mode);
        return true;
    }
    // Cross-shard: post a task to the owning shard and wait until it has queued
    // the message. The wait is on the shard's task queue, never on a subscriber.
    auto* task = new (std::nothrow) publish_context();
    if (task == nullptr) {
        return false;
    }
    task->state = &state;
    task->mode = mode;
    try {
        task->topic = std::move(topic_copy);
        task->body = std::move(body_copy);
    } catch (...) {
        delete task;
        return false;
    }
    // Capture the wait state before posting: the trampoline (or the shutdown
    // drop hook) may release the task while the caller is still here.
    std::shared_ptr<std::mutex> mu = task->mu;
    std::shared_ptr<std::condition_variable> cv = task->cv;
    std::shared_ptr<bool> done = task->done;
    if (!shard->post(&publish_trampoline, task, &publish_drop)) {
        delete task;
        return false;
    }
    try {
        std::unique_lock<std::mutex> lk(*mu);
        cv->wait(lk, [&done] { return *done; });
    } catch (...) {
        return false;
    }
    return true;
}

} // namespace http
} // namespace crpc
