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
#include <crpc/net/connection.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace db {

class CRPC_API channel {
public:
    virtual ~channel();

    channel(const channel&) = delete;
    channel& operator=(const channel&) = delete;

    // Reads exactly `count` bytes. A peer that closes first is `eof`, a reset is
    // `connection_reset`; both are terminal for this channel.
    [[nodiscard]] virtual result<std::vector<std::byte>> read_exact(std::size_t count) = 0;

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
    [[nodiscard]] status write(span<const std::byte> data) override;
    void close() noexcept override;

    [[nodiscard]] bool is_open() const noexcept;

private:
    std::unique_ptr<crpc::net::connection> link_;
};

} // namespace db
} // namespace crpc

#endif // CRPC_DB_CHANNEL_HPP