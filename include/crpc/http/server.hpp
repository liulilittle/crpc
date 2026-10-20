// CRPC -- HTTP server facade: HTTP/1.1, HTTP/2 (h2c/h2) and HTTPS (ALPN).
//
// The facade turns the existing pieces -- the HTTP/1.1 message layer
// (http/message.hpp), the HTTP/2 frame/HPACK/connection state machine
// (http2/*) and the TLS decorator (io/tls.hpp) -- into a working server that
// accepts connections from any `backend_factory` listener (inproc for
// tests, epoll/IOCP for TCP) and dispatches requests through the MVC router.
//
// Threading
// ---------
// One `runtime` per shard, exactly as the RPC server. An accepted connection is
// bound to a shard by round-robin. The transport source callback never runs
// user code: it copies the bytes into the session's ingress buffer and posts a
// process task to the owning shard. All protocol state (parsers, the HTTP/2
// connection, the write queue) is therefore touched on one thread only. No
// per-connection thread is ever created.
//
// Protocols
// ---------
//   * cleartext "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n" preface -> h2c;
//   * otherwise cleartext -> HTTP/1.1;
//   * TLS -> the protocol negotiated by ALPN ("h2" or "http/1.1"). A client
//     that offers neither is rejected; the connection is never downgraded.
//
// Shutdown
// --------
// `stop()` stops accepting, sends GOAWAY on every HTTP/2 connection, drains
// in-flight requests up to `drain_timeout`, then closes. It is idempotent and
// never blocks longer than the drain budget.
#ifndef CRPC_HTTP_SERVER_HPP
#define CRPC_HTTP_SERVER_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/http/content_coding.hpp>
#include <crpc/http/message.hpp>
#include <crpc/io.hpp>
#include <crpc/io/tls.hpp>
#include <crpc/push/hub.hpp>
#include <crpc/runtime.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace mvc {
class router;
} // namespace mvc

namespace http {

// Server-Sent Events subset. SSE is available over HTTP/1.1 only, with a
// single shard, in process. A request is attached to the server's push hub
// under `?subscriber=<u64>` (optional; a fresh id is allocated when absent) and
// resumes the same subscription when it reconnects with `Last-Event-ID`.
struct sse_options {
    // Comment-only heartbeat period. Zero disables the heartbeat; the idle
    // timeout is always exempted for an open SSE response.
    std::chrono::milliseconds heartbeat{15000};

    // Optional `retry:` hint sent once at the start of the stream.
    std::chrono::milliseconds retry{0};

    // Per-session bound on staged SSE bytes (write queue plus the in-flight
    // write). Zero reuses `http_server_options::egress_cap_bytes`.
    std::size_t egress_cap_bytes = 0;

    // A peer that stops draining for this long is closed (0 disables the
    // check). This is the backstop for a half-open connection that never fails
    // a heartbeat write.
    std::chrono::milliseconds slow_timeout{30000};

    // Epoch stamped into every event id and into the hub. A reconnect whose
    // Last-Event-ID carries a different epoch is not acknowledged.
    u64 epoch = 1;

    // Hub subscription bounds and overflow policy. The hub makes every drop
    // decision; a `disconnect_subscriber` policy ends delivery to that
    // subscriber. The replay window bounds reconnect replay.
    push::subscribe_options subscribe{};
};

// HTTP/1.1 long polling. Each request waits up to `timeout` for one message
// and answers `200` with the raw payload plus `Content-Length`, or `204` when
// the wait expires. The subscription lives across polls: a normal response
// never detaches, and a client acknowledges cumulatively by sending the event
// id it received as `Last-Event-ID` (or `?cursor=`) on the next poll.
struct longpoll_options {
    // Default and maximum wait in milliseconds. A query `timeout` is clamped to
    // `[0, max_timeout]`; zero performs one non-blocking take and answers 204
    // when the queue is empty.
    std::chrono::milliseconds timeout{25000};
    std::chrono::milliseconds max_timeout{30000};

    // A subscriber that sends no poll for this long is unsubscribed and its
    // bookkeeping removed (zero disables the sweep).
    std::chrono::milliseconds subscriber_ttl{std::chrono::minutes(5)};

    // Bound on concurrently waiting polls. A poll beyond it is answered 503
    // with `Retry-After: 1`.
    std::size_t max_waiters = 4096;

    // Per-session bound on staged response bytes. Zero reuses
    // `http_server_options::egress_cap_bytes`.
    std::size_t egress_cap_bytes = 0;

    // A peer that stops draining a polled response for this long is closed
    // (zero disables the check). The message stays unacknowledged and is
    // replayed by the next poll's cursor gap.
    std::chrono::milliseconds slow_timeout{30000};

    // `Content-Type` of a 200 response.
    std::string content_type = "application/octet-stream";

    // Epoch stamped into event ids and compared with a client cursor.
    u64 epoch = 1;

    // Hub subscription bounds for front replay. start() rejects a replay bound
    // larger than the queue (`max_replay > max_messages`) with
    // `invalid_argument`; it never silently enlarges the queue's memory bound.
    // A smaller replay bound is raised to the queue bound. Tail replay (SSE)
    // has no such cross-bound requirement.
    push::subscribe_options subscribe{};
};

// One long-poll endpoint. The request may override `topic`, `mode` and
// `ordered` with the query parameters of the same names.
struct longpoll_route {
    std::string path;
    std::string topic;
    push::delivery_mode mode = push::delivery_mode::reliable;
    bool ordered = false;
};

// One file download endpoint. `path` is the request path prefix (e.g.
// `/files`); the remainder is resolved under `root` after percent-decoding and
// a path-traversal check. `GET` and `HEAD` are served, `Range` is honored with
// `bytes` units only.
struct download_route {
    std::string path;
    std::filesystem::path root;
    // Sent as `Content-Type` on 200/206/416. Empty falls back to
    // `application/octet-stream`.
    std::string content_type = "application/octet-stream";
};

// One SSE endpoint. The request may override `topic`, `mode` and `ordered`
// with the query parameters of the same names.
struct sse_route {
    std::string path;
    std::string topic;
    push::delivery_mode mode = push::delivery_mode::reliable;
    bool ordered = false;
};

// Tunables. Every field is a plain value: the facade has no process-global
// configuration, and a caller can change any limit per server instance.
struct http_server_options {
    // Shards == runtimes == OS threads. Default 1.
    std::size_t shards = 1;
    runtime_options runtime{};

    // HTTP/1.1 message-layer limits (request line, header block, body, ...).
    parser_limits limits{};

    // Bound on bytes buffered per connection before reading is paused (the R1
    // credit signal). Reading resumes automatically once the buffer drains.
    std::size_t ingress_cap_bytes = 256u * 1024u;

    // Bound on bytes queued for writing per connection. Reaching it pauses
    // reading, so a slow peer backpressures instead of growing memory.
    std::size_t egress_cap_bytes = 1u * 1024u * 1024u;

    // HTTP/2 local settings advertised in the initial SETTINGS frame.
    u32 h2_max_concurrent_streams = 100;
    u32 h2_initial_window_size = 65535;
    u32 h2_max_frame_size = 16384;
    u32 h2_max_header_list_size = 16384;
    // Upper bound on bytes parked per HTTP/2 stream while a window is zero.
    std::size_t h2_max_send_queue_bytes = 1u * 1024u * 1024u;

    // Drain budget for `stop()`.
    std::chrono::milliseconds drain_timeout{5000};

    // HTTP/2 zero-window probe period (R18). While a response is parked the
    // shard retries the flush and, if still blocked, emits a PING so a dead
    // peer is detected instead of waited on forever.
    std::chrono::milliseconds h2_probe_interval{1000};

    // Hard bound on concurrently served connections. An accepted connection
    // beyond this is closed immediately instead of growing the session table;
    // a server must never be pushed into unbounded memory by opening sockets.
    std::size_t max_connections = 10000;

    // A connection that sends no bytes for this long is closed. Zero disables
    // the idle bound.
    std::chrono::milliseconds idle_timeout{60000};

    // An incomplete request (headers or body) older than this is answered with
    // 408 and the connection is closed. Zero disables the request bound.
    std::chrono::milliseconds request_timeout{30000};

    // Server-Sent Events (HTTP/1.1, single shard) options. SSE routes are
    // registered on the builder with `sse_route()`.
    sse_options sse{};

    // HTTP/1.1 long polling (single shard). Routes are registered on the
    // builder with `add_longpoll_route()`.
    longpoll_options longpoll{};

    // --- HTTP content coding -------------------------------------------------
    // Negotiate a response Content-Encoding from the request's Accept-Encoding
    // and encode the body. When false, responses are always identity (the
    // request side may still decode).
    bool enable_response_coding = true;
    // Decode a request body that carries Content-Encoding: gzip|deflate. When
    // false the encoding is left for the application (fail-closed 415 is still
    // not produced; the header reaches the route unchanged).
    bool enable_request_decoding = true;
    // Compression thresholds and bomb limits shared with the content_coding
    // module. `min_response_bytes` gates response compression.
    coding_limits coding{};
    // Hard ceiling on the decoded request body. A body that would exceed it is
    // answered with 413 and the connection is closed. Zero disables the check
    // (the module's own `max_decompressed_bytes` still applies).
    std::size_t max_decoded_body = 8u * 1024u * 1024u;
    // Decoded/encoded ratio ceiling applied on top of `coding`. Zero disables
    // the check (the default: requests may be highly compressible).
    std::size_t max_decoded_ratio = 0;

    // --- File download -------------------------------------------------------
    // A download response keeps the connection alive: the idle deadline stays
    // armed, refreshed by write progress. This bounds a peer that stops
    // draining for this long after staged output is non-empty (0 disables).
    std::chrono::milliseconds download_stall_timeout{30000};
    // Read chunk size for a streamed download body.
    std::size_t download_chunk_bytes = 64u * 1024u;
};

struct http_statistics {
    std::size_t active_connections = 0;
    std::size_t accepted_connections = 0;
    std::size_t requests_handled = 0;
    std::size_t responses_written = 0;
    std::size_t bad_requests = 0;
    std::size_t h2_connections = 0;
    std::size_t tls_connections = 0;
    // Connections closed because `max_connections` was reached.
    std::size_t rejected_connections = 0;
    // Connections closed by the idle or request deadline.
    std::size_t timed_out_connections = 0;
    // Live HTTP/1.1 SSE responses.
    std::size_t sse_connections = 0;
    // Event frames emitted onto SSE connections since start().
    std::size_t sse_events_delivered = 0;
    // SSE sessions closed because the peer stopped draining.
    std::size_t sse_slow_closes = 0;
    // Hub counters, so a test or operator can verify the drop accounting:
    // published == delivered + queued + drops (one subscriber).
    std::size_t sse_hub_published = 0;
    std::size_t sse_hub_delivered = 0;
    std::size_t sse_hub_queued = 0;
    std::size_t sse_hub_dropped = 0;
    // Responses whose body was actually encoded (the encoded form was smaller
    // than the identity body). A rejected compression (identity fallback or a
    // non-compressible response) is not counted.
    std::size_t responses_compressed = 0;
    // Request bodies decoded from a Content-Encoding.
    std::size_t requests_decoded = 0;
    // Download responses (200/206/416) served from a download route.
    std::size_t downloads_served = 0;
    // Long-poll state: currently waiting polls, live subscribers tracked for
    // TTL, 200 responses, 204 timeouts, cursors clamped to the delivered high
    // water, 409 conflicts/restarts, and 503 capacity rejections.
    std::size_t longpoll_waiters = 0;
    std::size_t longpoll_subscribers = 0;
    std::size_t longpoll_events = 0;
    std::size_t longpoll_timeouts = 0;
    std::size_t longpoll_ack_clamped = 0;
    std::size_t longpoll_conflicts = 0;
    std::size_t longpoll_restarts = 0;
    std::size_t longpoll_overloaded = 0;
    // Effective replay bound after start() clamps it to `max_messages`; zero
    // when the server is not running. Appended at the end so the field order
    // of every pre-existing member is unchanged (docs/abi.md §4).
    std::size_t longpoll_replay_window = 0;
};

// The server facade. Build it with `builder`, add TLS listeners with
// `listen_tls`, then `start()`.
class CRPC_API http_server {
public:
    struct listener_config {
        endpoint ep;
        backend_factory* backend = nullptr;
    };

    class builder {
    public:
        builder();
        ~builder();
        builder(const builder&) = delete;
        builder& operator=(const builder&) = delete;

        // Binds a cleartext listener (HTTP/1.1 or h2c by preface sniffing).
        builder& add_listener(endpoint ep, backend_factory& backend);
        // Selects the MVC router. Must outlive the server. When unset the
        // process-wide `mvc::default_router()` is used.
        builder& router(const mvc::router& routes);
        builder& shards(std::size_t count);
        builder& options(const http_server_options& value);
        builder& set_parser_limits(const parser_limits& limits);
        builder& drain_timeout(std::chrono::milliseconds value);
        // Registers an SSE endpoint. SSE requires `shards == 1`; `start()`
        // fails with `failed_precondition` otherwise. An HTTP/2 request that
        // targets an SSE path is answered with 501 (fail-closed, no stream).
        builder& add_sse_route(sse_route route);
        // Registers a long-poll endpoint (HTTP/1.1 only; HTTP/2 requests that
        // match are answered with 501).
        builder& add_longpoll_route(longpoll_route route);
        // Registers a file download endpoint (HTTP/1.1 only; HTTP/2 requests
        // that match are answered with 501).
        builder& add_download_route(download_route route);

        [[nodiscard]] std::unique_ptr<http_server> build();

    private:
        http_server_options options_{};
        std::vector<listener_config> listeners_;
        std::vector<sse_route> sse_routes_;
        std::vector<longpoll_route> longpoll_routes_;
        std::vector<download_route> download_routes_;
        const mvc::router* router_ = nullptr;
    };

    ~http_server();
    http_server(const http_server&) = delete;
    http_server& operator=(const http_server&) = delete;
    http_server(http_server&&) = delete;
    http_server& operator=(http_server&&) = delete;

    // Registers a TLS listener. Call before `start()`. When the library is
    // built without `CRPC_HAVE_OPENSSL` this returns a clear
    // `failed_precondition` error and nothing is registered. A TLS listener
    // requires ALPN: the negotiated "h2" or "http/1.1" selects the engine.
    [[nodiscard]] status listen_tls(const endpoint& ep, backend_factory& backend,
                                    io::tls_config config);

    // Binds every listener, starts the shards and returns once listening.
    [[nodiscard]] status start();

    // Graceful shutdown. Idempotent. Bounded by `drain_timeout`.
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] http_statistics stats() const noexcept;

    // Publishes one payload to every SSE subscriber of `topic`. Single shard
    // only. The call never blocks on a slow subscriber: the hub's bounds and
    // overflow policy make every drop decision, and each attached SSE pump is
    // woken after the publish. Returns false when the server is not running,
    // has more than one shard, or `topic` is empty.
    [[nodiscard]] bool publish(std::string_view topic, span<const std::byte> body,
                               push::delivery_mode mode =
                                   push::delivery_mode::reliable) noexcept;

    struct impl;

private:
    explicit http_server(http_server_options opt);

    std::unique_ptr<impl> impl_;
};

} // namespace http
} // namespace crpc

#endif // CRPC_HTTP_SERVER_HPP
