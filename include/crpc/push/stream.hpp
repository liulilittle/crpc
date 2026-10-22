// CRPC -- server/client stream reader and writer.
//
// A stream is one RPC or push instance carried by DATA frames plus END_STREAM
// on a connection's demultiplexer. `stream_transport` is the demultiplexer
// boundary: it owns the frame buffers, the send ring and the flow-control
// windows, so the reader and writer stay allocation-free and never block the
// shard thread.
//
// Shapes
// ------
//   * `client_streaming`  -- this endpoint sends a stream and receives one
//                            response; reading is legal only after `close()`.
//   * `server_streaming`  -- this endpoint sends one request and receives a
//                            stream.
//   * `bidirectional`     -- both directions are open independently.
//
// State machine and errors
// ------------------------
//   * Writing after `close()` or in a forbidden direction returns
//     `failed_precondition`.
//   * After `reset`, reads and writes return `canceled` (or the reset code),
//     and `close()` / `reset()` are idempotent no-ops.
//   * `read_some` reports `eof` when the peer ended the stream and `canceled`
//     when it was reset; the two are always distinct. A non-blocking read with
//     no frame buffered reports `unavailable` (retry when readable).
//
// Thread safety
// -------------
//   A reader, its writer and their shared `stream_state` belong to one shard
//   thread; they are not internally synchronised.
//
// Backpressure
// ------------
//   `write` accepts at most the transport's current send credit and returns the
//   number of bytes accepted; the caller retries the tail. A return of 0 is
//   backpressure, not an error.
#ifndef CRPC_PUSH_STREAM_HPP
#define CRPC_PUSH_STREAM_HPP

#include <cstddef>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc::push {

enum class stream_shape : u8 {
    client_streaming = 0,
    server_streaming = 1,
    bidirectional = 2
};

// Shared lifecycle of one stream. Owned by the endpoint; referenced (not owned)
// by its reader and writer.
class CRPC_API stream_state {
public:
    stream_state(u32 stream_id, stream_shape shape) noexcept;

    [[nodiscard]] u32 id() const noexcept { return id_; }
    [[nodiscard]] stream_shape shape() const noexcept { return shape_; }
    [[nodiscard]] bool local_closed() const noexcept { return local_closed_; }
    [[nodiscard]] bool remote_closed() const noexcept { return remote_closed_; }
    [[nodiscard]] bool canceled() const noexcept { return canceled_; }
    [[nodiscard]] rpc_code terminal_code() const noexcept { return terminal_; }

    // Reading is allowed unless the stream was reset, or the shape is
    // client_streaming and the local writer has not closed yet.
    [[nodiscard]] bool can_read() const noexcept;
    // Writing is allowed until the local side is closed or the stream is reset.
    [[nodiscard]] bool can_write() const noexcept;

    void mark_local_close() noexcept;
    void mark_remote_eof() noexcept;
    void mark_reset(rpc_code code) noexcept;

private:
    friend class stream_reader;
    friend class stream_writer;

    u32 id_ = 0;
    stream_shape shape_ = stream_shape::bidirectional;
    bool local_closed_ = false;
    bool remote_closed_ = false;
    bool canceled_ = false;
    rpc_code terminal_ = rpc_code::canceled;
};

// The connection demultiplexer. `pull` borrows the payload of the next DATA
// frame for the stream; the span stays valid until the next `pull` on the same
// stream. All byte accounting and ring storage live in the implementation.
class stream_transport {
public:
    enum class pull_result : u8 {
        data = 0,
        would_block = 1,
        eof = 2,
        canceled = 3,
        error = 4
    };

    virtual ~stream_transport() = default;

    [[nodiscard]] virtual pull_result pull(u32 stream_id,
                                           span<const std::byte>& payload,
                                           rpc_code& error) noexcept = 0;

    // Bytes the peer's flow-control window currently allows us to send.
    [[nodiscard]] virtual std::size_t send_credit(u32 stream_id) const noexcept = 0;

    // Accepts up to `data.size()` bytes into the send ring and returns how many
    // were accepted. A short count (including 0) is backpressure.
    [[nodiscard]] virtual std::size_t send(u32 stream_id,
                                           span<const std::byte> data) noexcept = 0;

    // Enqueues END_STREAM. False means the ring is full; the caller retries.
    [[nodiscard]] virtual bool send_end(u32 stream_id) noexcept = 0;

    // Releases inbound flow-control credit after the application consumed
    // bytes; the transport emits WINDOW_UPDATE as appropriate.
    virtual void release_read_credit(u32 stream_id, std::size_t bytes) noexcept = 0;

    // Sends RST_STREAM/CANCEL exactly once. Idempotent at this layer.
    virtual void send_reset(u32 stream_id, rpc_code code) noexcept = 0;
};

class CRPC_API stream_reader {
public:
    stream_reader(stream_transport& transport, stream_state& state) noexcept;

    // Copies up to `dst.size()` bytes. Returns the count, or an error:
    //   eof / canceled / failed_precondition / unavailable / transport code.
    [[nodiscard]] result<std::size_t> read_some(span<std::byte> dst) noexcept;

    // Fills `dst` completely or returns the first error.
    [[nodiscard]] status read_exact(span<std::byte> dst) noexcept;

    [[nodiscard]] bool eof() const noexcept { return state_->remote_closed(); }
    [[nodiscard]] bool canceled() const noexcept { return state_->canceled(); }
    [[nodiscard]] u32 stream_id() const noexcept { return state_->id(); }

private:
    stream_transport* transport_;
    stream_state* state_;
    span<const std::byte> pending_;
    std::size_t offset_ = 0;
};

class CRPC_API stream_writer {
public:
    stream_writer(stream_transport& transport, stream_state& state) noexcept;

    // Returns the number of bytes accepted, or an error:
    //   failed_precondition (shape/closed) / canceled (reset) / transport code.
    [[nodiscard]] result<std::size_t> write(span<const std::byte> src) noexcept;

    // Ends the local side. Idempotent; reports `resource_exhausted` when the
    // transport cannot enqueue END_STREAM yet (the stream stays open).
    [[nodiscard]] status close() noexcept;

    // Cancels the stream, releases the transport buffers and is idempotent.
    [[nodiscard]] status reset(rpc_code code) noexcept;

    [[nodiscard]] bool closed() const noexcept { return state_->local_closed(); }
    [[nodiscard]] u32 stream_id() const noexcept { return state_->id(); }

private:
    stream_transport* transport_;
    stream_state* state_;
};

} // namespace crpc::push

#endif // CRPC_PUSH_STREAM_HPP
