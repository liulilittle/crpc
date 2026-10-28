// CRPC -- the byte channel a database connection runs on.
//
// The protocol layers (MySQL, TDS, and later others) need exactly three
// operations: read N bytes, write all bytes, close. Putting them behind an
// interface is what makes the handshake, authentication and result-set decoding
// testable with a scripted server and no socket, which is the only way to exercise
// the error paths deterministically.
//
// This lives in `crpc::db` rather than in one adapter because the adapters share
// it: a second copy of the same socket wrapper would be duplicate knowledge, and
// the TLS and Unix-socket extensions have to be written once.
#ifndef CRPC_DB_CHANNEL_HPP
#define CRPC_DB_CHANNEL_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/net/channel.hpp>
#include <crpc/net/connection.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace db {

// The database channel is the byte-channel contract from the unified socket
// framework (`crpc::net::channel`); this type keeps the `crpc::db` name the
// adapters already use and pins the semantics that a database connection needs
// (no silent reconnect, terminal on any error).
class CRPC_API channel : public net::channel {
public:
    ~channel() override;

    channel(const channel&) = delete;
    channel& operator=(const channel&) = delete;

    // Reads exactly `count` bytes. A peer that closes first is `eof`, a reset is
    // `connection_reset`; both are terminal for this channel.
    [[nodiscard]] virtual result<std::vector<std::byte>> read_exact(std::size_t count) = 0;

    // Reads at least one byte and at most `buffer.size()`, blocking until some arrive.
    //
    // A TLS record layer needs this: it cannot know in advance how many bytes the peer
    // will send, so it must be able to consume whatever is there. Protocol layers that
    // always know their next length (MySQL, TDS framing) keep using `read_exact`.
    [[nodiscard]] virtual result<std::size_t> read_some(span<std::byte> buffer) {
        // A default in terms of read_exact keeps every existing channel usable; a
        // channel that can do better overrides it.
        if (buffer.empty()) {
            return std::size_t{0};
        }
        result<std::vector<std::byte>> one = read_exact(1);
        if (!one) {
            return one.error();
        }
        buffer[0] = one.value()[0];
        return std::size_t{1};
    }

    // Writes every byte. `ok` means the kernel accepted them, not that the server
    // processed them.
    [[nodiscard]] virtual status write(span<const std::byte> data) = 0;

    // Terminal and idempotent.
    virtual void close() noexcept = 0;

protected:
    channel() = default;
};

// The real channel: one TCP link over `crpc::net::connection`.
//
// It deliberately does not reuse the reconnect behaviour of that type: a database
// connection carries session state (transaction, charset, temporary tables,
// prepared statements), so reconnecting behind the caller's back would silently
// change semantics. The connection layer calls `close()` on any error, which is
// terminal for the underlying link.
class CRPC_API socket_channel final : public channel {
public:
    socket_channel(crpc::net::connection_config config, crpc::backend_factory& backend,
                   crpc::runtime* shard = nullptr);
    ~socket_channel() override;

    socket_channel(const socket_channel&) = delete;
    socket_channel& operator=(const socket_channel&) = delete;

    [[nodiscard]] result<std::vector<std::byte>> read_exact(std::size_t count) override;
    [[nodiscard]] result<std::size_t> read_some(span<std::byte> buffer) override;
    [[nodiscard]] status write(span<const std::byte> data) override;
    void close() noexcept override;

    [[nodiscard]] bool is_open() const noexcept;

private:
    std::unique_ptr<crpc::net::connection> link_;
    // Bytes taken from the transport but not handed to the caller yet.
    //
    // `read_some` asks the transport for everything it has buffered and then copies out what
    // fits; whatever did not fit is kept here. Dropping it would silently truncate a
    // response, which is how an HTTP client ends up reporting eof in the middle of a body.
    std::vector<std::byte> pending_;
    std::size_t pending_offset_ = 0;
};

} // namespace db
} // namespace crpc

#endif // CRPC_DB_CHANNEL_HPP