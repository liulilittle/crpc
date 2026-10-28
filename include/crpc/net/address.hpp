// CRPC -- network addresses and name resolution.
//
// This is the address layer of the unified socket framework in `crpc::net`.
// Upper protocol layers (HTTP, the database adapters, the message queues, a
// future WebSocket layer) should never build a `sockaddr` themselves; the
// concrete forms they need -- a TCP host/port pair and a Unix-domain-socket
// path -- are modelled here, together with the two control-plane operations
// that socket users otherwise share a copy of: TCP endpoint parsing with port
// range validation, and hostname resolution with a deadline.
//
// Windows specifics
// -----------------
// Winsock has supported `AF_UNIX` since Windows 10 1803. The runtime probe
// `unix_socket_supported()` reflects the *actual* host (measured, not assumed):
// a fresh copy of the tool or OS drops the capability to null instead of
// guessing. `sun_path` holds at most 108 bytes on both platforms.
//
// Resolution failure model (see `resolve_host`)
// ---------------------------------------------
//   * invalid_argument  -- the host string is syntactically rejected before any
//                          resolver is consulted (empty, contains a NUL or a
//                          control character). This is the one error that can
//                          never be fixed by retrying.
//   * unavailable       -- the resolver answered: the name does not exist, or
//                          DNS service is currently failing (e.g. `EAI_AGAIN`).
//                          This is the "DNS failure" class a connector reports.
//   * deadline_exceeded -- the resolver did not answer within
//                          `resolve_options::timeout`.
//   * resource_exhausted -- an out-of-memory condition inside resolution.
#ifndef CRPC_NET_ADDRESS_HPP
#define CRPC_NET_ADDRESS_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/net/ip.hpp>

namespace crpc {
namespace net {

// Which of an address's two concrete forms it holds. Distinct from `io::endpoint`
// because that type deliberately knows nothing about Unix-domain sockets.
enum class address_kind : std::uint8_t {
    none,
    tcp,  // a host (literal or name) plus a port
    unix  // a Unix-domain-socket path
};

// Family preference for resolution, mirroring the `ai_family` hint.
enum class ip_family : std::uint8_t {
    any,
    ipv4,
    ipv6
};

// A validated network address.
//
// Construction validates eagerly: `tcp()` builds a concrete value, `parse_*`
// returns a `result` describing the first problem. A default-constructed
// address is invalid, which is how "absent" is distinguished from "wildcard".
class CRPC_API address {
public:
    address() noexcept = default;

    // A TCP host/port pair ready to resolve. `port` is a `u16` so the range is
    // already enforced by the type; use `parse_port()`/`parse_tcp()` to accept
    // trusted textual input.
    [[nodiscard]] static address tcp(std::string host, u16 port);

    // A Unix-domain-socket path (not yet validated; see `validate_unix_path`).
    [[nodiscard]] static address unix_path(std::string path);

    // Parses "host:port" or "[v6-literal]:port", with the port range checked.
    // The bracket form is the only one that may contain a ':' in the host.
    [[nodiscard]] static result<address> parse_tcp(std::string_view text);

    // Parses a bare Unix path (the same validation as `validate_unix_path`).
    [[nodiscard]] static result<address> parse_unix(std::string_view text);

    [[nodiscard]] address_kind type() const noexcept { return kind_; }
    [[nodiscard]] bool valid() const noexcept { return kind_ != address_kind::none; }
    [[nodiscard]] bool is_tcp() const noexcept { return kind_ == address_kind::tcp; }
    [[nodiscard]] bool is_unix() const noexcept { return kind_ == address_kind::unix; }

    // Host part (host name or IP literal) and port for a TCP address. Empty/0
    // when the address is not in TCP form.
    [[nodiscard]] const std::string& host() const noexcept { return host_; }
    [[nodiscard]] u16 port() const noexcept { return port_; }

    // Filesystem path for a Unix-domain address.
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] friend bool operator==(const address& a, const address& b) noexcept {
        return a.kind_ == b.kind_ && a.host_ == b.host_ && a.port_ == b.port_ &&
               a.path_ == b.path_;
    }
    [[nodiscard]] friend bool operator!=(const address& a, const address& b) noexcept {
        return !(a == b);
    }

private:
    address_kind kind_ = address_kind::none;
    std::string host_;
    u16 port_ = 0;
    std::string path_;
};

// Parses a decimal port with the range checked (1..65535). "0" is rejected: it
// means "ephemeral" and is only meaningful for binding, never for a target.
[[nodiscard]] CRPC_API result<u16> parse_port(std::string_view text);

// Configures one `resolve_host()` call.
struct resolve_options {
    // Restrict the family of the returned addresses.
    ip_family family = ip_family::any;

    // Bound on the resolution, including the resolver's own retries. An answer
    // after this deadline is discarded and `deadline_exceeded` is returned.
    std::chrono::nanoseconds timeout{std::chrono::seconds(5)};
};

// Resolves a host to one address per family. A numeric literal is answered
// without touching the resolver, so a connector need not wait on DNS for an
// address it already has. Error classification follows the table in the file
// comment. The returned literals are canonical (see `ip_address`).
//
// The resolver call itself is executed on a helper thread so that a stuck DNS
// lookup cannot exceed `resolve_options::timeout`; the helper finishes in the
// background and is harmless after the caller has returned.
[[nodiscard]] CRPC_API result<std::vector<ip_address>> resolve_host(
    std::string_view host, const resolve_options& options = resolve_options{});

// True when the *running* host can create `AF_UNIX` stream sockets. Measured
// once per process with a real `socket()` call.
[[nodiscard]] CRPC_API bool unix_socket_supported() noexcept;

// Validates a Unix-domain-socket path: non-empty, no embedded NUL, and short
// enough for `sockaddr_un::sun_path` (108 bytes on Windows and POSIX).
[[nodiscard]] CRPC_API status validate_unix_path(std::string_view path);

} // namespace net
} // namespace crpc

#endif // CRPC_NET_ADDRESS_HPP