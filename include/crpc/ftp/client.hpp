// CRPC -- FTP/FTPS client (RFC 959, RFC 2228 authentication commands,
// RFC 4217 TLS, RFC 2389 feature negotiation).
//
// One client owns one authenticated control connection.  Commands are issued
// synchronously: `command` writes one line, reads exactly one reply, and returns
// it.  Transfer methods open a second (data) connection through PASV/EPSV (the
// default) or PORT/EPRT, move the bytes, close that connection, and only then
// read the server's final completion reply.
//
// Reply handling
// --------------
// Methods returning `ftp_reply` return the server's reply verbatim, including
// 4xx and 5xx completions: a negative FTP reply is a protocol-level answer, not
// a transport failure, and the caller often wants to branch on the exact code
// (550 missing vs 553 name rejected).  Methods returning a typed value
// (`pwd`, `size`, `mdtm`, `list`, `retr`) cannot do that, so they fail with an
// `rpc_error` carrying `rpc_code::application` and `metadata["ftp.code"]` when
// the reply is not 2xx.
//
// Data connections
// ----------------
// Passive mode is the default: EPSV is tried first (RFC 2428, required for
// IPv6) and PASV is the fallback for older servers.  The advertised PASV host
// is ignored by default because a malicious server can use it to make the
// client connect to a third party (FTP bounce); the control connection's peer
// address is used instead.  Set `client_options::use_pasv_address` to honour
// the advertised address behind a NAT.
//
// When the control connection is protected (`use_tls` or `implicit_tls`) the
// client negotiates PBSZ 0 + PROT P and every data connection is protected with
// a fresh TLS session in the same context.  Active-mode FTPS is not supported:
// the data-connection TLS role would require the client to present a server
// certificate, which this API does not take.
//
// Thread safety
// -------------
// A client is a session, not a pool: the public methods serialise on one mutex,
// but FTP state (current directory, transfer type, protection level) is shared
// by the whole session, so concurrent callers must not rely on command ordering
// across threads.  Use one client per thread for concurrent work.
#ifndef CRPC_FTP_CLIENT_HPP
#define CRPC_FTP_CLIENT_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/ftp/command.hpp>
#include <crpc/ftp/reply.hpp>
#include <crpc/io/tls.hpp>
#include <crpc/net/channel.hpp>

namespace crpc {
namespace ftp {

struct CRPC_API client_options {
    std::string host = "127.0.0.1";
    std::uint16_t port = 21;
    std::string user = "anonymous";
    std::string password = "anonymous@";
    // Explicit FTPS: connect in clear, send AUTH TLS, then upgrade.
    bool use_tls = false;
    // Implicit FTPS: TLS starts before the greeting.  Normally port 990.
    bool implicit_tls = false;
    // Verify the server certificate and host name (client role only).
    bool verify_peer = true;
    // PEM trust-anchor file path, or PEM text (materialised transiently).
    // Empty uses OpenSSL's default trust store.
    std::string ca_bundle;
    // Connect and per-I/O deadline for the control and data connections.
    std::chrono::milliseconds timeout{30000};
    // Passive (EPSV/PASV) or active (EPRT/PORT) data connections.
    bool passive = true;
    // Log in automatically with `user` / `password` after the transport is
    // established.  Set false to drive USER/PASS yourself with `login()`.
    bool auto_login = true;
    // Use the address a PASV reply advertises instead of the control
    // connection's peer address.  Only enable this behind a NAT that rewrites
    // the advertised address.
    bool use_pasv_address = false;
    // Ceiling for one control line; longer replies fail with protocol_error.
    std::size_t max_line_bytes = 64 * 1024;
};

namespace detail {

// Blocking byte transport shared by the client and the server: one
// `net::tcp_stream`, optionally wrapped by `io::tls` in either role.  It exists
// as a public detail type because the component ships four headers and both
// peers must use exactly one TLS pump implementation.
class CRPC_API transport final : public net::channel {
public:
    // Plain transport over an established stream.
    explicit transport(std::unique_ptr<net::tcp_stream> stream);
    ~transport() override;

    transport(const transport&) = delete;
    transport& operator=(const transport&) = delete;
    transport(transport&&) = delete;
    transport& operator=(transport&&) = delete;

    // Wraps `plain` in TLS and runs the handshake.  `plain` must be a
    // plain transport with no read-ahead buffering (the line reader must have
    // yielded no leftover bytes).  `server_name` is the SNI / verification
    // target on a client and empty on a server.  On failure `plain` is closed.
    [[nodiscard]] static result<std::unique_ptr<transport>> upgrade(
        std::unique_ptr<transport> plain, io::tls_context& context,
        std::string server_name, std::chrono::milliseconds handshake_timeout);

    [[nodiscard]] result<std::vector<std::byte>> read_exact(
        std::size_t count) override;
    [[nodiscard]] result<std::size_t> read_some(span<std::byte> buffer) override;
    [[nodiscard]] status write(span<const std::byte> data) override;
    void close() noexcept override;

    [[nodiscard]] bool is_secure() const noexcept;
    [[nodiscard]] net::address local() const;
    [[nodiscard]] net::address peer() const;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

// Buffered line reader over a transport.  Each call returns one line *with* its
// terminator as it appeared on the wire (CRLF, or a bare LF that the reply /
// command parsers then judge).  A read deadline leaves the partial line in the
// buffer, so an idle control connection can be polled without losing a command
// that arrives in pieces.  Exceeding `max_line_bytes` fails with
// `protocol_error`.
class CRPC_API line_reader {
public:
    explicit line_reader(net::channel& channel, std::size_t max_line_bytes);

    [[nodiscard]] result<std::string> read_line();
    [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size() - offset_; }

    // Rebinds to a new channel after a TLS upgrade.  Precondition: buffered()
    // is zero, because bytes read in clear cannot be replayed through TLS.
    void rebind(net::channel& channel);

private:
    net::channel* channel_;
    std::size_t max_line_bytes_;
    std::string buffer_;
    std::size_t offset_ = 0;
};

// Extracts the port from an EPSV reply body (`... (|||port|)`).  Returns 0 for
// every non-conforming shape, which the caller treats as "fall back to PASV".
// Exposed because the passive-mode fallback can mask a parser regression in
// behaviour tests: these cases are unit-tested directly.
[[nodiscard]] CRPC_API int parse_epsv_port(std::string_view text) noexcept;

} // namespace detail

class CRPC_API client {
public:
    // Establishes the control connection, reads the greeting, negotiates
    // AUTH TLS when requested, and (with `auto_login`) logs in.  Failure to
    // authenticate fails the connect rather than returning a half-usable
    // session.
    [[nodiscard]] static result<std::unique_ptr<client>> connect(
        const client_options& options);

    ~client();

    client(const client&) = delete;
    client& operator=(const client&) = delete;
    client(client&&) = delete;
    client& operator=(client&&) = delete;

    // Sends a raw command line (without CRLF) and returns the reply.  CR/LF/NUL
    // in `cmd` is rejected with `invalid_argument`.
    [[nodiscard]] result<ftp_reply> command(std::string_view cmd);

    [[nodiscard]] result<ftp_reply> login(std::string_view user,
                                          std::string_view password);
    [[nodiscard]] result<std::string> pwd();
    [[nodiscard]] result<ftp_reply> cwd(std::string_view path);
    [[nodiscard]] result<ftp_reply> cdup();
    [[nodiscard]] result<ftp_reply> mkdir(std::string_view path);
    [[nodiscard]] result<ftp_reply> rmdir(std::string_view path);
    [[nodiscard]] result<ftp_reply> remove(std::string_view path);
    [[nodiscard]] result<ftp_reply> rename(std::string_view from,
                                           std::string_view to);
    [[nodiscard]] result<std::uint64_t> size(std::string_view path);
    [[nodiscard]] result<std::string> mdtm(std::string_view path);
    [[nodiscard]] result<std::vector<std::string>> list(std::string_view path = {});
    [[nodiscard]] result<std::vector<std::string>> nlst(std::string_view path = {});
    [[nodiscard]] result<ftp_reply> type_binary();
    [[nodiscard]] result<ftp_reply> type_ascii();
    [[nodiscard]] result<std::string> retr(std::string_view path,
                                           std::uint64_t offset = 0);
    [[nodiscard]] result<ftp_reply> stor(std::string_view path, std::string_view data);
    [[nodiscard]] result<ftp_reply> appe(std::string_view path, std::string_view data);
    // Streams the remote file to `local`; returns `local.string()` on success.
    // With a non-zero `offset` the local file is opened for append, so an
    // interrupted download resumes at the server's offset.
    [[nodiscard]] result<std::string> retr_to_file(
        std::string_view remote, const std::filesystem::path& local,
        std::uint64_t offset = 0);
    [[nodiscard]] result<ftp_reply> stor_from_file(
        std::string_view remote, const std::filesystem::path& local,
        bool append = false);
    [[nodiscard]] result<ftp_reply> quit();

    // --- Diagnostics -------------------------------------------------------
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] bool is_secure() const noexcept;   // control connection under TLS
    [[nodiscard]] bool data_is_secure() const noexcept; // PROT P negotiated
    [[nodiscard]] const ftp_reply& greeting() const noexcept;
    [[nodiscard]] const client_options& options() const noexcept;

private:
    explicit client(const client_options& options);
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace ftp
} // namespace crpc

#endif // CRPC_FTP_CLIENT_HPP
