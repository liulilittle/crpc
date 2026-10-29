// CRPC -- MySQL over TLS: a synchronous OpenSSL channel wrapped around the
// transport the handshake started on.
//
// MySQL upgrades the connection in the middle of the handshake phase: after
// the greeting the client sends the 32-byte SSLRequest packet (the response
// header with CLIENT_SSL set) and the TLS handshake then *replaces* the wire
// -- the server answers with TLS records, not with a MySQL packet. Only after
// the handshake does the ordinary packet framing resume, now encrypted, and
// the HandshakeResponse41 goes out at the next sequence id.
//
// This is the same shape the TDS adapter solves (`src/db/tds/tls_channel.cpp`),
// minus the PRELOGIN encapsulation: SQL Server carries its handshake records
// inside TDS packets, MySQL does not. The wrapper owns the SSL session over
// memory BIOs and forwards records straight to the inner channel, so any
// `db::channel` -- the real socket channel, or a scripted one in the tests --
// becomes a TLS channel without changing the channel interface.
//
// Without CRPC_HAVE_OPENSSL the entry points still exist and fail with an
// explicit error: a caller that asked for TLS must never silently get a
// cleartext session.
#ifndef CRPC_DB_MYSQL_TLS_CHANNEL_HPP
#define CRPC_DB_MYSQL_TLS_CHANNEL_HPP

#include <memory>
#include <string>

#include <crpc/db/channel.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace db {
namespace mysql {

// True when this build has the OpenSSL backend (CRPC_WITH_OPENSSL=ON).
[[nodiscard]] bool tls_backend_available() noexcept;

struct tls_client_options {
    // Sent as SNI and used for hostname verification. Empty omits both.
    std::string server_name;
    bool verify_server = true;
    // PEM trust anchors. Empty uses OpenSSL's default store.
    std::string ca_bundle;
};

// Wraps `inner` in TLS and runs the client handshake. On success the returned
// channel carries the encrypted stream and `inner` is consumed. On failure the
// error is returned and `inner` is handed back to the caller still owning it
// (the handshake closes it only where OpenSSL got far enough to fail cleanly;
// the caller decides what a dead session's transport is worth) -- a caller
// that keeps diagnostics around can still inspect what was written. The
// handshake is a plain TLS handshake -- MySQL framing is not involved, the
// caller sent the SSLRequest packet before calling this.
[[nodiscard]] result<std::unique_ptr<db::channel>> open_tls_channel(
    std::unique_ptr<db::channel>& inner, const tls_client_options& options);

} // namespace mysql
} // namespace db
} // namespace crpc

#endif // CRPC_DB_MYSQL_TLS_CHANNEL_HPP
