// CRPC -- a full-duplex WebSocket session (RFC 6455 sections 5.4-7).
//
// `stream` sits on top of an already-handshaken byte channel (crpc/ws/handshake
// performs the Upgrade) and provides the data-message / control-frame state
// machine the RFC calls "the WebSocket connection":
//
//   * fragmented messages are reassembled in order, with a hard cap so a peer
//     cannot exhaust memory by never finishing a message;
//   * text messages are validated as UTF-8 and rejected with close 1007;
//   * every Ping is answered with a Pong carrying the identical payload;
//   * the two-way Close handshake is completed automatically: the first Close
//     received triggers the matching reply and no data frames are either sent
//     or accepted once either side has closed;
//   * frame-level check is delegated to ws::frame_decoder, whose mask-direction
//     policy is fixed by the session role.
//
// Everything returns `result<T>`/`status`; nothing throws. `stream` owns the
// channel, so its destructor and `abort()` tear the link down even when the
// peer vanishes without a Close frame.
#ifndef CRPC_WS_STREAM_HPP
#define CRPC_WS_STREAM_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <crpc/config.hpp>
#include <crpc/db/channel.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>
#include <crpc/ws/frame.hpp>

namespace crpc {
namespace ws {

struct stream_options {
    // Hard ceiling on one reassembled message, in bytes. A frame that claims
    // more, or a fragmented message that grows past it, is answered with a
    // Close carrying 1009 (Message Too Big) and the connection is dropped.
    std::size_t max_message_bytes = 64u * 1024u * 1024u;

    // Outgoing data messages larger than this are split into fragments
    // (RFC 6455 section 5.4). Zero disables outgoing fragmentation and sends
    // the whole message in one frame.
    std::size_t max_fragment_bytes = 0;

    // The role fixes the mask direction (RFC 6455 section 5.1): a client masks
    // its output and requires unmasked input, a server sends unmasked and
    // requires masked input. It is never guessed from a URL.
    bool is_client = false;
};

// One event sent up from `stream::recv()`. Text and binary frames (including
// reassembled fragments) arrive as `message`; control frames carry their own
// kind. `closed` carries the peer's Close status code and reason; `had_code` is
// false when the peer closed without a status payload (code 1005).
struct event {
    enum class kind : std::uint8_t { message, ping, pong, closed };

    kind type = kind::message;
    opcode op = opcode::binary; // message kind for type == message
    std::string data;           // payload / ping or pong payload / close reason
    std::uint16_t code = 0;     // type == closed: the peer's close status
    bool had_code = false;      // type == closed: a status payload was present
};

// A WebSocket session over one `db::channel`. Not thread-safe: one receiver
// and one sender (or a single thread doing both) is the supported shape; the
// channel underneath obeys the same contract.
class CRPC_API stream {
public:
    stream(std::unique_ptr<db::channel> link, const stream_options& options);
    // Additionally seeds the frame decoder with bytes that arrived immediately
    // after the HTTP Upgrade response (an early server frame), so nothing that
    // the handshake read-ahead caught off the wire is lost.
    stream(std::unique_ptr<db::channel> link, const stream_options& options,
           span<const std::byte> leftover);
    ~stream();

    stream(const stream&) = delete;
    stream& operator=(const stream&) = delete;
    stream(stream&&) = delete;
    stream& operator=(stream&&) = delete;

    // Blocks until the next event (`message`, `ping`, `pong` or `closed`) or
    // fails with a transport error / `eof` when the connection is gone. A
    // protocol violation returns `protocol_error` after the matching Close has
    // been sent; a message that exceeds `max_message_bytes` returns
    // `resource_exhausted` after a Close 1009. After `closed` has been
    // returned, further calls return `eof`.
    [[nodiscard]] result<event> recv();

    // Outgoing data messages. Both fail with `failed_precondition` once a Close
    // has been sent or received (RFC 6455 section 5.5.1: no data frames after
    // a Close). `send_text` validates the payload as UTF-8.
    [[nodiscard]] status send_text(std::string_view text);
    [[nodiscard]] status send_binary(span<const std::byte> data);

    // Explicit control frames. `send_close` is idempotent; after it has been
    // called the session stops accepting data frames and a later `recv()`
    // reports the peer's echo (or `eof`). `send_ping` / `send_pong` reject a
    // payload over 125 bytes.
    [[nodiscard]] status send_ping(span<const std::byte> payload = {});
    [[nodiscard]] status send_pong(span<const std::byte> payload = {});
    [[nodiscard]] status send_close(close_code code = close_code::normal,
                                    std::string_view reason = {});

    // Sends Close(normal, "") — the same frame `send_close` produces. Bounded
    // by the channel; use `abort()` when no reply is wanted.
    [[nodiscard]] status close();

    // Hard teardown: no Close frame is written, buffered bytes are dropped and
    // the underlying channel is closed. Idempotent and safe from any thread.
    void abort() noexcept;

    [[nodiscard]] bool is_open() const noexcept;

private:
    struct impl;

    // Pulls the next complete frame from the channel, answering decoder errors
    // with the matching Close on the wire.
    [[nodiscard]] result<decoded_frame> next_frame();
    // Consumes one frame; true when an event has been staged for `recv()`.
    [[nodiscard]] result<bool> handle_frame(const decoded_frame& frame);
    [[nodiscard]] status send_data(opcode op, span<const std::byte> data);
    [[nodiscard]] status emit_frame(opcode op, span<const std::byte> payload,
                                    bool fin = true);

    std::unique_ptr<impl> impl_;
};

} // namespace ws
} // namespace crpc

#endif // CRPC_WS_STREAM_HPP