// CRPC -- the unified byte-channel contract and one TCP implementation.
//
// Every protocol in the library (HTTP, the database adapters, the message
// queues, a future WebSocket layer) needs the same four operations on a
// transport: read N bytes, read whatever is available, write all bytes, close.
// `net::channel` is the single spelling of that contract. The database layer
// historically owned its own copy (`crpc::db::channel`); it now derives from
// this type, and a concrete socket transport (`net::tcp_stream`) implements the
// contract directly, so the knowledge of how a stream behaves is defined once.
//
// Read contract
// -------------
// A channel has no streaming semantics of its own -- that choice is left to the
// protocol on top, which is precisely why a shared line-oriented or length-
// framed layer can sit on top of any channel:
//
//   * `read_exact(n)` returns exactly `n` bytes or fails. 0 is answered
//     immediately with an empty vector.
//   * `read_some(buffer)` blocks until at least one byte arrives, then returns
//     however many bytes are available up to `buffer.size()`. It never waits
//     for the buffer to be full (that is what makes a TLS/record/body reader
//     with unknown framing possible). A caller that needs all of a known length
//     must loop -- `read_exact` is that loop.
//   * Partial reads are a fact of life, not an error. Nothing in this contract
//     is "at most one syscall that may come back short"; every reader handles
//     short reads by construction.
//
// EOF versus error
// ----------------
// A clean close of the peer's write side is *EOF* (`rpc_code::eof`) and is only
// reported on the read path, after every byte the peer sent has been delivered.
// A close that left unsent data in a kernel buffer, or a failure on the write
// path, is an *error* (`connection_reset`, `broken_pipe`). The two are never
// merged: a retry policy that must not reconnect past a clean EOF (an HTTP
// response body delimited by close) can tell them apart. Codes reported by an
// operation in flight:
//
//   * `eof`               - clean end of stream, everything before was delivered
//   * `connection_reset`  - abortive close by peer / network
//   * `broken_pipe`       - write into a closed read side
//   * `deadline_exceeded` - the operation's timeout elapsed
//
// After EOF or an error the channel is closed; a further operation fails the
// same terminal way.
#ifndef CRPC_NET_CHANNEL_HPP
#define CRPC_NET_CHANNEL_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/net/address.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace net {

class CRPC_API channel {
public:
    virtual ~channel();

    channel(const channel&) = delete;
    channel& operator=(const channel&) = delete;
    channel(channel&&) = delete;
    channel& operator=(channel&&) = delete;

    // Reads exactly `count` bytes. `count == 0` returns an empty vector. A peer
    // that closes first reports `eof` (or `connection_reset`); the message is
    // terminal for the channel.
    [[nodiscard]] virtual result<std::vector<std::byte>> read_exact(std::size_t count) = 0;

    // Reads at least one byte and at most `buffer.size()`, blocking until some
    // arrive. An empty buffer returns 0 without blocking. EOF surfaces as `eof`
    // once buffered bytes (if any) are exhausted.
    [[nodiscard]] virtual result<std::size_t> read_some(span<std::byte> buffer) = 0;

    // Writes every byte. `ok` means the kernel accepted them all, not that the
    // peer processed them.
    [[nodiscard]] virtual status write(span<const std::byte> data) = 0;

    // Terminal and idempotent.
    virtual void close() noexcept = 0;

protected:
    channel() = default;
};

// Transport tunables shared by the TCP connector, listener and stream.
struct stream_options {
    // Per-operation read/write deadline on the calling thread. A read that
    // times out fails with `deadline_exceeded`; a timed-out write closes the
    // stream (a partial frame must not precede the next message).
    std::chrono::nanoseconds rw_timeout{std::chrono::seconds(5)};

    // Socket buffer hints. Zero leaves the kernel default.
    std::size_t recv_buffer_bytes = 64 * 1024;
    std::size_t send_buffer_bytes = 64 * 1024;
};

// The byte-transport contract the connector and listener hand out.
class tcp_stream;

namespace detail {
// Internal bridge: the connector and the listener hold the only valid native
// socket, and use this factory to build a stream without exposing the platform
// handle type in a public header.
struct stream_factory {
    [[nodiscard]] static std::unique_ptr<tcp_stream> adopt(std::uintptr_t native,
                                                           address local, address peer,
                                                           stream_options options);
};
} // namespace detail

// One established TCP (or Unix-domain) stream implementing the `channel`
// contract. Streams are only obtained from `net::connect`, from `net::listener`
// or from the internal factory; the constructor is private precisely so a bogus
// handle cannot be invented.
class CRPC_API tcp_stream final : public channel {
public:
    ~tcp_stream() override;

    tcp_stream(const tcp_stream&) = delete;
    tcp_stream& operator=(const tcp_stream&) = delete;
    tcp_stream(tcp_stream&&) = delete;
    tcp_stream& operator=(tcp_stream&&) = delete;

    [[nodiscard]] result<std::vector<std::byte>> read_exact(std::size_t count) override;
    [[nodiscard]] result<std::size_t> read_some(span<std::byte> buffer) override;

    // Non-blocking drain of whatever is immediately available (0 when nothing).
    [[nodiscard]] result<std::size_t> read_available(span<std::byte> buffer);

    [[nodiscard]] status write(span<const std::byte> data) override;
    void close() noexcept override;

    // Half-close: sends FIN on the write side while the read side stays open.
    // A peer that read to the end observes EOF, then may still send a reply.
    [[nodiscard]] status shutdown_send();

    // Abortive close: sets `SO_LINGER{1, 0}` and closes, so the peer observes a
    // reset rather than a clean EOF.
    void abort() noexcept;

    [[nodiscard]] bool is_open() const noexcept;

    // The addresses known to the stream.
    [[nodiscard]] address local() const;
    [[nodiscard]] address peer() const;

    [[nodiscard]] const stream_options& options() const noexcept;

private:
    explicit tcp_stream(std::uintptr_t native, address local, address peer,
                        stream_options options);
    friend struct detail::stream_factory;

    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace net
} // namespace crpc

#endif // CRPC_NET_CHANNEL_HPP