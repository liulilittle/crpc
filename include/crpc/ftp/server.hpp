// CRPC -- FTP/FTPS server (RFC 959, RFC 2228, RFC 4217, RFC 2389).
//
// The server binds one control listener and serves each accepted connection on
// its own thread.  Every data connection is paired one-to-one with one transfer
// command: PASV/EPSV arms a one-shot listener, the next list/retrieve/store
// command consumes it (and only that command), and the listener is closed after
// the accept so a stale port can never serve a later transfer.
//
// Filesystem confinement
// ----------------------
// `root_directory` is the only tree the server can touch.  A request path is
// rejected outright when it is absolute (`/x`, `\x`, `C:x`, `\\host\share`),
// contains a `..` component, a drive/ADS colon, a NUL or a control byte, or a
// name whose trailing spaces/dots make it a Windows alias.  The accepted path is
// then resolved with `weakly_canonical`, which follows symlinks and junctions for
// the existing prefix, and the result must remain under the canonical root.
// That second step is what stops `root/link -> C:\Windows` from being a way out
// even though no `..` appears in the request.
//
// TLS
// ---
// `enable_tls` offers AUTH TLS on the control connection; every subsequent
// PBSZ/PROT negotiation and PROT P data connection uses the same certificate.
// `allow_implicit_tls` additionally accepts a TLS ClientHello as the first bytes
// of a connection (the implicit-FTPS convention, normally port 990).  TLS needs
// a build with CRPC_WITH_OPENSSL=ON; without it `start` fails with
// `failed_precondition` instead of silently serving cleartext.
#ifndef CRPC_FTP_SERVER_HPP
#define CRPC_FTP_SERVER_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace ftp {

struct CRPC_API server_options {
    std::string address = "127.0.0.1";
    std::uint16_t port = 21;
    // The only tree the server exposes.  Must name an existing directory.
    std::string root_directory;
    bool require_auth = true;
    std::string user = "anonymous";
    // Empty means "any password".
    std::string password;
    // Explicit AUTH TLS on the control connection.
    bool enable_tls = false;
    // Accept a TLS ClientHello as the first bytes of a connection (implicit
    // FTPS).  Works together with `enable_tls`: a TLS record is served with
    // implicit TLS, cleartext gets the normal greeting and may still AUTH TLS.
    bool allow_implicit_tls = false;
    // PEM certificate chain and private key.  Either PEM text or a file path.
    // Materialised PEM text is written to a private temporary file and removed
    // as soon as the TLS context has loaded it.
    std::string cert_chain;
    std::string private_key;
    std::size_t max_connections = 64;
    std::size_t max_line_bytes = 4096;
    bool read_only = false;
    // Address advertised by PASV (an IPv4 literal or a host name that resolves
    // to one).  Empty advertises the control connection's local address.
    std::filesystem::path passive_address;
    // Bound per data read/write and for the passive accept.
    std::chrono::milliseconds io_timeout{30000};
    // How often an idle control wait wakes to notice `stop()`.
    std::chrono::milliseconds poll_interval{250};
};

class CRPC_API server {
public:
    // Binds the listener, loads TLS credentials (when requested) and starts the
    // accept thread.  A port of 0 asks the OS for an ephemeral port, reported
    // by `port()`.
    [[nodiscard]] static result<std::unique_ptr<server>> start(
        const server_options& options);

    ~server();

    server(const server&) = delete;
    server& operator=(const server&) = delete;
    server(server&&) = delete;
    server& operator=(server&&) = delete;

    // Closes the listener and every session, then joins the worker threads.
    // Idempotent and safe from any thread except a session thread.
    void stop();

    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] std::size_t active_sessions() const noexcept;

private:
    explicit server(const server_options& options);
    struct impl;
    std::shared_ptr<impl> impl_;
};

} // namespace ftp
} // namespace crpc

#endif // CRPC_FTP_SERVER_HPP
