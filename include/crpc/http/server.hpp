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
#include <memory>
#include <string>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/http/message.hpp>
#include <crpc/io.hpp>
#include <crpc/io/tls.hpp>
#include <crpc/runtime.hpp>

namespace crpc {
namespace mvc {
class router;
} // namespace mvc

namespace http {

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
};

struct http_statistics {
    std::size_t active_connections = 0;
    std::size_t accepted_connections = 0;
    std::size_t requests_handled = 0;
    std::size_t responses_written = 0;
    std::size_t bad_requests = 0;
    std::size_t h2_connections = 0;
    std::size_t tls_connections = 0;
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

        [[nodiscard]] std::unique_ptr<http_server> build();

    private:
        http_server_options options_{};
        std::vector<listener_config> listeners_;
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

    struct impl;

private:
    explicit http_server(http_server_options opt);

    std::unique_ptr<impl> impl_;
};

} // namespace http
} // namespace crpc

#endif // CRPC_HTTP_SERVER_HPP
