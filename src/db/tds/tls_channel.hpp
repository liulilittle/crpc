// CRPC -- TDS over TLS: a synchronous OpenSSL channel.
//
// TDS frames TLS differently in its two phases, and getting this backwards is
// what made the first two attempts fail:
//
//   * during the handshake every TLS record produced by the client is carried as
//     the body of a TDS PRELOGIN packet (type 0x12, status EOM). The server's
//     handshake records arrive the same way, so the body is what feeds the TLS
//     state machine;
//   * once the handshake completes, TLS records go straight to the transport --
//     there is no TDS header around them. 0x17 is the *TLS* content type for
//     application data, not a TDS packet type, and SQL Server's stream parser
//     rejects a session whose first post-handshake bytes carry an extra header.
//
// Inside that encrypted stream the ordinary TDS packet framing resumes, so the
// connection layer keeps writing one packet per call; one write becomes one TLS
// record, which is the boundary SQL Server's reader expects.
#ifndef CRPC_DB_TDS_TLS_CHANNEL_HPP
#define CRPC_DB_TDS_TLS_CHANNEL_HPP

#include <cstdint>
#include <memory>
#include <string>

#include <crpc/db/channel.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace db {
namespace tds {

// True when this build has the OpenSSL backend (CRPC_WITH_OPENSSL=ON).
[[nodiscard]] bool tls_backend_available() noexcept;

struct tls_client_options {
    // Sent as SNI and used for hostname verification. Empty omits both.
    std::string server_name;
    bool verify_server = true;
    std::string ca_bundle;
    // TDS packet size used for the PRELOGIN packets that carry the handshake.
    std::uint16_t packet_size = 4096;
};

// Runs the TDS-encapsulated TLS handshake over `inner` and returns a channel that
// carries the encrypted TDS stream. `packet_id` is the last TDS packet id used by
// the session; the handshake consumes the following ids and advances it so the
// LOGIN7 that follows continues the same sequence.
[[nodiscard]] result<std::unique_ptr<db::channel>> open_tls_channel(
    std::unique_ptr<db::channel> inner, const tls_client_options& options,
    std::uint8_t& packet_id);

} // namespace tds
} // namespace db
} // namespace crpc

#endif // CRPC_DB_TDS_TLS_CHANNEL_HPP