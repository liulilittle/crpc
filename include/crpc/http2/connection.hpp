// CRPC -- HTTP/2 connection state machine (RFC 7540 sections 5, 6.5, 6.9).
//
// This layer owns:
//
//   * the settings exchange (local/remote SETTINGS, ACK bookkeeping);
//   * connection- and stream-level flow-control windows;
//   * the stream state machine;
//   * CONTINUATION sequencing and header-block decoding;
//   * GOAWAY and RST_STREAM bookkeeping.
//
// It does not own the socket, TLS, ALPN or the CRPC call adapter. Inbound
// frames are handed in as (header, payload) after the 9-byte header has been
// parsed by frame.hpp; outbound frames are built by the caller using the
// helpers in frame.hpp and the accounting methods here.
//
// Flow control and R18 (no silent stall)
// --------------------------------------
// `send_data` never drops bytes. Bytes that fit in both windows (and in the
// peer's SETTINGS_MAX_FRAME_SIZE) are reported as `sent`; the rest is parked in
// the stream's send queue. `flush_parked` retries the queue after a
// WINDOW_UPDATE. Because a peer may never send a WINDOW_UPDATE (a zero-window
// stall is exactly what R18 forbids), the owner MUST also arm a periodic
// zero-window probe timer and, for every id returned by `parked_streams()`,
// call `flush_parked`; if the window is still zero the owner SHOULD emit a PING
// (frame.hpp) so that a dead peer is detected rather than waited on forever.
// The connection exposes `has_parked()`/`parked_streams()` for that timer.
//
// Receive-side flow control
// -------------------------
// DATA consumes the *whole* payload (including padding), per RFC 7540 6.9.1.
// The application calls `release_received` after consuming bytes; it restores
// both receive windows and returns the deltas to advertise. The caller is
// expected to emit the corresponding WINDOW_UPDATE frames immediately; the
// model restores the window at the same time (documented simplification).
//
// Error handling
// --------------
// `receive` returns an h2_error. `scope == none` means the frame was accepted.
// `scope == stream` means the caller must send RST_STREAM with `code` on
// `stream_id`; `scope == connection` means the caller must send GOAWAY with
// `code` and close. Illegal stream transitions map to STREAM_CLOSED or
// PROTOCOL_ERROR exactly as RFC 7540 section 5.1 requires.
//
// Thread safety: a connection is single-threaded. Every method must be called
// from the connection's owning shard/thread; no internal locking is performed.
// Encoder and decoder contexts are per-connection and per-direction; they are
// never shared between connections.
#ifndef CRPC_HTTP2_CONNECTION_HPP
#define CRPC_HTTP2_CONNECTION_HPP

#include <array>
#include <cstddef>
#include <unordered_map>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/http/message.hpp>
#include <crpc/http2/frame.hpp>
#include <crpc/http2/hpack.hpp>
#include <crpc/span.hpp>

namespace crpc::http2 {

enum class connection_role : u8 { client, server };

enum class stream_state : u8 {
    idle,
    reserved_local,
    reserved_remote,
    open,
    half_closed_local,
    half_closed_remote,
    closed
};

[[nodiscard]] CRPC_API const char* to_string(stream_state state) noexcept;

struct settings_values {
    u32 header_table_size = default_header_table_size;
    bool enable_push = true;
    u32 max_concurrent_streams = default_max_concurrent_streams;
    u32 initial_window_size = default_initial_window_size;
    u32 max_frame_size = default_max_frame_size;
    u32 max_header_list_size = default_max_header_list_size;
};

// Validates one setting value. Unknown setting identifiers are accepted (and
// ignored by the connection), as RFC 7540 6.5.2 requires. Failure is
// rpc_code::protocol_error for a value outside the RFC range.
[[nodiscard]] CRPC_API status validate_setting(u16 id, u32 value) noexcept;

enum class error_scope : u8 { none, stream, connection };

struct h2_error {
    error_code code = error_code::no_error;
    error_scope scope = error_scope::none;
    u32 stream_id = 0;

    [[nodiscard]] bool ok() const noexcept { return scope == error_scope::none; }
    [[nodiscard]] rpc_code rpc() const noexcept { return to_rpc_code(code); }
};

struct connection_options {
    connection_role role = connection_role::server;
    // Settings we advertise. `max_frame_size`, `initial_window_size`,
    // `max_concurrent_streams` and `max_header_list_size` are enforced on the
    // peer.
    settings_values local_settings{};
    // Our connection-level send and receive windows.
    u32 initial_connection_send_window = default_initial_window_size;
    u32 initial_connection_recv_window = default_initial_window_size;
    // Upper bound on bytes parked per stream while a window is zero. Exceeding
    // it fails the send with ENHANCE_YOUR_CALM rather than growing without
    // bound.
    std::size_t max_send_queue_bytes = 1u << 20u;
    // R9/R13 mandate fail-closed for the CRPC binary frame layer. HTTP/2 is a
    // different protocol: RFC 7540 4.1 says unknown types MUST be ignored.
    // The default is therefore RFC behaviour; set this to reject unknown types
    // with a connection PROTOCOL_ERROR when the deployment policy says so.
    bool reject_unknown_frame_types = false;
};

struct stream_snapshot {
    u32 id = 0;
    stream_state state = stream_state::idle;
    i64 send_window = 0;
    i64 recv_window = 0;
    bool local_end_stream = false;
    bool remote_end_stream = false;
    std::size_t queued_bytes = 0;
};

// Receive-window replenishment deltas to advertise. A zero member means "no
// update needed for that level".
struct window_credit {
    u32 connection = 0;
    u32 stream = 0;
};

class CRPC_API connection {
public:
    explicit connection(connection_options options = connection_options{});

    connection(const connection&) = delete;
    connection& operator=(const connection&) = delete;
    connection(connection&&) noexcept = default;
    connection& operator=(connection&&) noexcept = default;

    // --- Introspection -------------------------------------------------------

    [[nodiscard]] connection_role role() const noexcept { return role_; }
    [[nodiscard]] const settings_values& local_settings() const noexcept { return local_settings_; }
    [[nodiscard]] const settings_values& remote_settings() const noexcept { return remote_settings_; }
    [[nodiscard]] u64 received_frame_count() const noexcept { return received_frames_; }

    [[nodiscard]] hpack_decoder& decoder() noexcept { return decoder_; }
    [[nodiscard]] const hpack_decoder& decoder() const noexcept { return decoder_; }
    [[nodiscard]] hpack_encoder& encoder() noexcept { return encoder_; }
    [[nodiscard]] const hpack_encoder& encoder() const noexcept { return encoder_; }

    // --- Inbound -------------------------------------------------------------

    // Processes one frame. `header.length` must equal `payload.size()`. On a
    // connection-scope error the caller must send GOAWAY and close; on a
    // stream-scope error the caller must send RST_STREAM on `stream_id`.
    [[nodiscard]] h2_error receive(const frame_header& header,
                                   span<const std::byte> payload);

    // A non-ACK SETTINGS arrived and must be acknowledged.
    [[nodiscard]] bool pending_settings_ack() const noexcept { return pending_settings_ack_; }
    void settings_acknowledge() noexcept { pending_settings_ack_ = false; }

    // A non-ACK PING arrived; `last_ping()` is the opaque data to echo.
    [[nodiscard]] bool pending_ping_ack() const noexcept { return pending_ping_ack_; }
    [[nodiscard]] const ping_opaque& last_ping() const noexcept { return last_ping_; }
    void ping_acknowledge() noexcept { pending_ping_ack_ = false; }

    // Set when the peer acknowledges one of our PINGs (ACK flag).
    [[nodiscard]] bool ping_acknowledged() const noexcept { return ping_acknowledged_; }
    void clear_ping_acknowledged() noexcept { ping_acknowledged_ = false; }

    // Result of the last complete inbound header block (HEADERS or
    // PUSH_PROMISE). Valid until the next `receive` that completes a block.
    [[nodiscard]] const http::header_list& last_headers() const noexcept { return last_headers_; }
    [[nodiscard]] u32 last_headers_stream() const noexcept { return last_headers_stream_; }
    [[nodiscard]] bool last_headers_present() const noexcept { return last_headers_present_; }
    void clear_last_headers() noexcept;

    // Borrows the receive() payload; valid until the next call to receive().
    [[nodiscard]] span<const std::byte> last_data() const noexcept { return last_data_; }
    [[nodiscard]] u32 last_data_stream() const noexcept { return last_data_stream_; }

    // Result of the last RST_STREAM we accepted.
    [[nodiscard]] u32 last_reset_stream() const noexcept { return last_reset_stream_; }
    [[nodiscard]] error_code last_reset_code() const noexcept { return last_reset_code_; }

    // --- Streams -------------------------------------------------------------

    [[nodiscard]] stream_state state_of(u32 stream_id) const noexcept;
    [[nodiscard]] bool stream_exists(u32 stream_id) const noexcept;
    [[nodiscard]] stream_snapshot snapshot(u32 stream_id) const noexcept;
    [[nodiscard]] std::size_t active_stream_count() const noexcept;
    [[nodiscard]] std::size_t peer_open_stream_count() const noexcept;

    [[nodiscard]] bool goaway_received() const noexcept { return goaway_received_; }
    [[nodiscard]] u32 goaway_last_stream_id() const noexcept { return goaway_last_stream_id_; }
    [[nodiscard]] error_code goaway_error() const noexcept { return goaway_error_; }

    // True when `id` may open a locally initiated stream now (parity, ordering,
    // GOAWAY). Does not check the concurrent-stream limit.
    [[nodiscard]] bool can_open_stream(u32 stream_id) const noexcept;

    // Opens a locally initiated stream. `stream_id` must have the role's local
    // parity (odd for a client, even for a server), be greater than any id used
    // so far, and be allowed by GOAWAY.
    [[nodiscard]] h2_error open_stream(u32 stream_id, bool end_stream = false);

    // Sends END_STREAM locally (without a DATA frame). Same state rules as
    // sending DATA.
    [[nodiscard]] h2_error local_end_stream(u32 stream_id);

    // Locally resets a stream. Stream must exist and not be idle.
    [[nodiscard]] h2_error reset_stream(u32 stream_id,
                                        error_code code = error_code::cancel);

    // Releases closed stream records. Until called, `state_of` reports the
    // factual closed state. Callers should prune periodically to bound memory.
    void prune_closed_streams();

    // --- Send-side flow control ---------------------------------------------

    [[nodiscard]] i64 connection_send_window() const noexcept { return connection_send_window_; }
    [[nodiscard]] i64 stream_send_window(u32 stream_id) const noexcept;

    // min(connection window, stream window, peer max frame size), clamped to 0.
    [[nodiscard]] std::size_t available_send_window(u32 stream_id) const noexcept;

    // Sends as much of `data` as the windows and frame-size limit allow.
    //   * `sent`   -- bytes the caller must write as the payload of one DATA
    //                 frame (already deducted from both windows);
    //   * `parked` -- true when the remainder was queued (never dropped).
    // Sending on a half-closed-local or closed stream fails.
    [[nodiscard]] h2_error send_data(u32 stream_id, span<const std::byte> data,
                                     std::size_t& sent, bool& parked);

    // Retries parked bytes. Copies up to `max_bytes` (and up to the window) into
    // `out` (cleared first), removes them from the queue and deducts the
    // windows. `flushed` is false when nothing could be sent (tree still
    // parked). `max_bytes` == 0 means "up to the peer's max frame size".
    [[nodiscard]] h2_error flush_parked(u32 stream_id, std::size_t max_bytes,
                                        std::vector<std::byte>& out, bool& flushed);

    [[nodiscard]] std::size_t parked_bytes(u32 stream_id) const noexcept;
    [[nodiscard]] std::size_t total_parked_bytes() const noexcept;
    [[nodiscard]] bool has_parked() const noexcept;
    [[nodiscard]] std::vector<u32> parked_streams() const;

    // --- Receive-side flow control ------------------------------------------

    [[nodiscard]] i64 connection_recv_window() const noexcept { return connection_recv_window_; }
    [[nodiscard]] i64 stream_recv_window(u32 stream_id) const noexcept;

    // Application consumed `consumed` DATA bytes. Restores both receive windows
    // and returns the deltas to advertise via WINDOW_UPDATE. See the file
    // comment for the immediate-advertisement simplification.
    [[nodiscard]] window_credit release_received(u32 stream_id,
                                                 std::size_t consumed) noexcept;

private:
    struct stream_entry {
        u32 id = 0;
        stream_state state = stream_state::idle;
        i64 send_window = 0;
        i64 recv_window = 0;
        bool local_end = false;
        bool remote_end = false;
        bool peer_initiated = false;
        std::vector<std::byte> send_queue;
    };

    [[nodiscard]] stream_entry* find_stream(u32 stream_id) noexcept;
    [[nodiscard]] const stream_entry* find_stream(u32 stream_id) const noexcept;

    [[nodiscard]] h2_error connection_error(error_code code) const noexcept;
    [[nodiscard]] h2_error stream_error(u32 stream_id, error_code code) const noexcept;

    [[nodiscard]] bool is_local_id(u32 stream_id) const noexcept;
    [[nodiscard]] bool is_peer_id(u32 stream_id) const noexcept;

    [[nodiscard]] h2_error receive_data(const frame_header& header,
                                        span<const std::byte> payload);
    [[nodiscard]] h2_error receive_headers(const frame_header& header,
                                           span<const std::byte> payload);
    [[nodiscard]] h2_error receive_push_promise(const frame_header& header,
                                                span<const std::byte> payload);
    [[nodiscard]] h2_error receive_continuation(const frame_header& header,
                                                span<const std::byte> payload);
    [[nodiscard]] h2_error receive_settings(const frame_header& header,
                                            span<const std::byte> payload);
    [[nodiscard]] h2_error receive_ping(const frame_header& header,
                                        span<const std::byte> payload);
    [[nodiscard]] h2_error receive_goaway(const frame_header& header,
                                          span<const std::byte> payload);
    [[nodiscard]] h2_error receive_window_update(const frame_header& header,
                                                 span<const std::byte> payload);
    [[nodiscard]] h2_error receive_rst_stream(const frame_header& header,
                                              span<const std::byte> payload);
    [[nodiscard]] h2_error receive_priority(const frame_header& header,
                                            span<const std::byte> payload);

    // Creates a peer stream record after validating ordering/parity/limits.
    [[nodiscard]] h2_error open_peer_stream(u32 stream_id);
    [[nodiscard]] h2_error finish_header_block(u32 stream_id,
                                               span<const std::byte> fragment);
    void close_stream(stream_entry& entry) noexcept;
    void transition_remote_end(stream_entry& entry) noexcept;
    void transition_local_end(stream_entry& entry) noexcept;

    connection_role role_;
    connection_options options_;
    settings_values local_settings_;
    settings_values remote_settings_;

    i64 connection_send_window_;
    i64 connection_recv_window_;

    std::unordered_map<u32, stream_entry> streams_;
    u32 last_peer_stream_id_ = 0;
    u32 last_local_stream_id_ = 0;

    // CONTINUATION sequencing. While `continuation_pending_` is true no frame
    // other than a CONTINUATION on `continuation_stream_` is legal.
    bool continuation_pending_ = false;
    u32 continuation_stream_ = 0;
    std::vector<std::byte> header_block_;

    bool pending_settings_ack_ = false;
    bool pending_ping_ack_ = false;
    bool ping_acknowledged_ = false;
    ping_opaque last_ping_{};

    bool goaway_received_ = false;
    u32 goaway_last_stream_id_ = 0;
    error_code goaway_error_ = error_code::no_error;

    http::header_list last_headers_;
    u32 last_headers_stream_ = 0;
    bool last_headers_present_ = false;

    span<const std::byte> last_data_;
    u32 last_data_stream_ = 0;

    u32 last_reset_stream_ = 0;
    error_code last_reset_code_ = error_code::no_error;

    u64 received_frames_ = 0;

    hpack_decoder decoder_;
    hpack_encoder encoder_;
};

} // namespace crpc::http2

#endif // CRPC_HTTP2_CONNECTION_HPP
