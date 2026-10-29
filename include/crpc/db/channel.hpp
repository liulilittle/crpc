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

    // These four re-declare `net::channel`'s pure virtuals and say so with
    // `override`: the class already marks its destructor `override`, so clang
    // reports -Winconsistent-missing-override without it, and the keyword also
    // makes the compiler check that the signatures still match the base.
    // Reads exactly `count` bytes. A peer that closes first is `eof`, a reset is
    // `connection_reset`; both are terminal for this channel.
    [[nodiscard]] result<std::vector<std::byte>> read_exact(std::size_t count) override = 0;

    // Reads at least one byte and at most `buffer.size()`, blocking until some arrive.
    //
    // A TLS record layer needs this: it cannot know in advance how many bytes the peer
    // will send, so it must be able to consume whatever is there. Protocol layers that
    // always know their next length (MySQL, TDS framing) keep using `read_exact`.
    [[nodiscard]] result<std::size_t> read_some(span<std::byte> buffer) override {
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
    [[nodiscard]] status write(span<const std::byte> data) override = 0;

    // Terminal and idempotent.
    void close() noexcept override = 0;

    // True when the link protects bytes end to end from the wire: an
    // established TLS session qualifies; a plain TCP socket (loopback included)
    // does not. Protocol layers consult this to pick a credential form --
    // MySQL's caching_sha2_password full authentication may send the plaintext
    // password only over such a link, and must RSA-encrypt it anywhere else.
    // The default is `false` so every channel that does not prove protection
    // stays honest, and existing implementations need no change.
    [[nodiscard]] virtual bool is_secure() const noexcept { return false; }

    // True while the link can still carry bytes. A channel that observes its
    // transport (peer FIN, an abortive reset, a failed write, TLS close_notify)
    // reports those closures here, which is what makes a borrow-time health
    // check possible: the socket and TLS stream channels do exactly that, so a
    // connection the peer closed while idle reads as closed before the next
    // send. The default is `true` -- the honest answer for a channel that
    // cannot observe its peer at all -- and a consumer that needs the
    // observation must use a channel that implements this.
    [[nodiscard]] virtual bool is_open() const noexcept { return true; }

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

    [[nodiscard]] bool is_open() const noexcept override;

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