// CRPC -- the unified outbound connector.
//
// One non-blocking socket, one `select`-bounded wait, one error mapping. The
// code below is the single place a TCP or Unix-domain connection is
// established by this framework; every failure carries the classification
// documented in connector.hpp.
#include <crpc/net/connector.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "platform.hpp"

namespace crpc {
namespace net {
namespace {

using detail::native_socket;

// Gathers the known addresses of an established socket and builds the stream.
std::unique_ptr<tcp_stream> build_stream(native_socket s, const stream_options& so) {
    sockaddr_storage local_storage{};
    sockaddr_storage peer_storage{};
    int local_len = 0;
    int peer_len = 0;
    std::string local_host;
    std::string peer_host;
    u16 local_port = 0;
    u16 peer_port = 0;
    address local;
    address peer;
    if (detail::get_local_addr(s, local_storage, local_len) &&
        detail::sockaddr_to_host(local_storage, local_host, local_port)) {
        local = address::tcp(std::move(local_host), local_port);
    }
    if (detail::get_peer_addr(s, peer_storage, peer_len) &&
        detail::sockaddr_to_host(peer_storage, peer_host, peer_port)) {
        peer = address::tcp(std::move(peer_host), peer_port);
    }
    return detail::stream_factory::adopt(static_cast<std::uintptr_t>(s),
                                         std::move(local), std::move(peer), so);
}

// One non-blocking connect attempt against a single resolved literal.
result<std::unique_ptr<tcp_stream>> connect_one(
    const ip_address& target_ip, u16 target_port, const ip_address& local_ip,
    const connect_options& options, std::chrono::nanoseconds budget) {
    sockaddr_storage target_sa{};
    int target_len = 0;
    if (!detail::ip_to_sockaddr(target_ip, target_port, target_sa, target_len)) {
        return rpc_error{rpc_code::invalid_argument, "unsupported address family"};
    }

    const native_socket s =
        ::socket(target_ip.is_v4() ? AF_INET : AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (s == detail::invalid_socket) {
        return rpc_error{rpc_code::resource_exhausted,
                         detail::error_text("socket() failed", detail::last_error())};
    }

    if (local_ip.valid()) {
        sockaddr_storage local_sa{};
        int local_len = 0;
        if (detail::ip_to_sockaddr(local_ip, 0, local_sa, local_len)) {
            if (::bind(s, reinterpret_cast<sockaddr*>(&local_sa), local_len) != 0) {
                const int error = detail::last_error();
                detail::close_socket(s);
                return rpc_error{classify_connect_error(error),
                                 detail::error_text("local bind failed", error)};
            }
        }
    }

    (void)detail::apply_tcp_options(s, options.tcp_no_delay, options.stream,
                                    options.keepalive);
    (void)detail::set_nonblocking(s, true);

    const int attempt = ::connect(s, reinterpret_cast<sockaddr*>(&target_sa), target_len);
    if (attempt != 0) {
        const int error = detail::last_error();
        if (detail::connect_would_block(error)) {
            const int ready = detail::wait_ready(s, detail::wait_kind::write, budget);
            if (ready == 0) {
                detail::close_socket(s);
                return rpc_error{rpc_code::deadline_exceeded,
                                 "connect exceeded its deadline"};
            }
            if (ready < 0) {
                detail::close_socket(s);
                return rpc_error{rpc_code::internal,
                                 detail::error_text("waiting for connect failed",
                                                    detail::last_error())};
            }
            int so_error = 0;
            if (!detail::socket_error(s, so_error) || so_error != 0) {
                detail::close_socket(s);
                return rpc_error{classify_connect_error(so_error),
                                 detail::error_text("connection failed", so_error)};
            }
        } else {
            detail::close_socket(s);
            return rpc_error{classify_connect_error(error),
                             detail::error_text("connection failed", error)};
        }
    }

    return build_stream(s, options.stream);
}

result<std::unique_ptr<tcp_stream>> connect_unix(const address& target,
                                                 const connect_options& options) {
    const status validated = validate_unix_path(target.path());
    if (!validated) {
        return rpc_error{validated.code(), validated.message()};
    }
    sockaddr_un sun{};
    int sun_len = 0;
    if (!detail::unix_to_sockaddr(target.path(), sun, sun_len)) {
        return rpc_error{rpc_code::invalid_argument, "invalid unix path"};
    }

    const native_socket s =
        ::socket(detail::k_address_family_unix, SOCK_STREAM, 0);
    if (s == detail::invalid_socket) {
        const int error = detail::last_error();
        const rpc_code code = detail::missing_family(error)
                                  ? rpc_code::unavailable
                                  : rpc_code::resource_exhausted;
        return rpc_error{code, detail::error_text("unix socket() failed", error)};
    }
    (void)detail::apply_tcp_options(s, options.tcp_no_delay, options.stream,
                                    options.keepalive);
    (void)detail::set_nonblocking(s, true);

    const int attempt = ::connect(s, reinterpret_cast<sockaddr*>(&sun), sun_len);
    if (attempt != 0) {
        const int error = detail::last_error();
        if (detail::connect_would_block(error)) {
            const int ready = detail::wait_ready(s, detail::wait_kind::write,
                                                 options.timeout);
            if (ready == 0) {
                detail::close_socket(s);
                return rpc_error{rpc_code::deadline_exceeded,
                                 "unix connect exceeded its deadline"};
            }
            int so_error = 0;
            if (ready < 0 ||
                !detail::socket_error(s, so_error) || so_error != 0) {
                const int reported = so_error != 0 ? so_error : detail::last_error();
                detail::close_socket(s);
                return rpc_error{classify_connect_error(reported),
                                 detail::error_text("unix connection failed",
                                                    reported)};
            }
        } else {
            detail::close_socket(s);
            return rpc_error{classify_connect_error(error),
                             detail::error_text("unix connection failed", error)};
        }
    }

    auto stream = build_stream(s, options.stream);
    if (stream) {
        return stream;
    }
    detail::close_socket(s);
    return rpc_error{rpc_code::internal, "failed to adopt stream"};
}

} // namespace

rpc_code classify_connect_error(int error) noexcept {
#if defined(CRPC_OS_WINDOWS)
    switch (error) {
    case WSAECONNREFUSED:
    case WSAECONNRESET: // Windows reports some refused loopback connects this way
        return rpc_code::connection_refused;
    case WSAETIMEDOUT:
        return rpc_code::deadline_exceeded;
    case WSAEHOSTUNREACH:
    case WSAENETUNREACH:
    case WSAENETDOWN:
    case WSAEHOSTDOWN:
        return rpc_code::unavailable;
    case WSAEADDRNOTAVAIL:
    case WSAEADDRINUSE:
    case WSAEACCES:
        return rpc_code::invalid_argument;
    case WSAENOBUFS:
    case WSAENETRESET:
        return rpc_code::resource_exhausted;
    default:
        return rpc_code::internal;
    }
#else
    switch (error) {
    case ECONNREFUSED:
    case ECONNRESET:
        return rpc_code::connection_refused;
    case ETIMEDOUT:
        return rpc_code::deadline_exceeded;
    case EHOSTUNREACH:
    case ENETUNREACH:
    case ENETDOWN:
    case EHOSTDOWN:
        return rpc_code::unavailable;
    case EADDRNOTAVAIL:
    case EADDRINUSE:
    case EACCES:
        return rpc_code::invalid_argument;
    case EMFILE:
    case ENFILE:
    case ENOMEM:
    case ENOBUFS:
        return rpc_code::resource_exhausted;
    default:
        return rpc_code::internal;
    }
#endif
}

rpc_code classify_stream_error(int error) noexcept {
#if defined(CRPC_OS_WINDOWS)
    switch (error) {
    case WSAECONNABORTED:
    case WSAECONNRESET:
        return rpc_code::connection_reset;
    case WSAENOTCONN:
    case WSAESHUTDOWN:
        return rpc_code::connection_reset;
    case WSAETIMEDOUT:
        return rpc_code::deadline_exceeded;
    case WSAENOBUFS:
        return rpc_code::resource_exhausted;
    default:
        return rpc_code::internal;
    }
#else
    if (error == EPIPE) {
        return rpc_code::broken_pipe;
    }
    switch (error) {
    case ECONNRESET:
    case ECONNABORTED:
    case ENOTCONN:
        return rpc_code::connection_reset;
    case ETIMEDOUT:
        return rpc_code::deadline_exceeded;
    case ENOBUFS:
        return rpc_code::resource_exhausted;
    default:
        return rpc_code::internal;
    }
#endif
}

result<std::unique_ptr<tcp_stream>> connect(const address& target,
                                            const connect_options& options) {
    detail::ensure_sockets_initialized();
    if (!target.valid()) {
        return rpc_error{rpc_code::invalid_argument, "no target address"};
    }
    if (target.is_unix()) {
        return connect_unix(target, options);
    }
    if (target.host().empty()) {
        return rpc_error{rpc_code::invalid_argument, "empty target host"};
    }
    if (target.port() == 0) {
        return rpc_error{rpc_code::invalid_argument,
                         "port 0 is not a connection target"};
    }
    if (options.timeout < std::chrono::nanoseconds::zero()) {
        return rpc_error{rpc_code::invalid_argument, "negative connect timeout"};
    }

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + options.timeout;

    resolve_options resolver;
    resolver.family = ip_family::any;
    resolver.timeout = options.timeout;
    result<std::vector<ip_address>> targets = resolve_host(target.host(), resolver);
    if (!targets) {
        return targets.error();
    }

    std::vector<ip_address> locals;
    if (options.local_bind.valid() && options.local_bind.is_tcp()) {
        if (options.local_bind.host().empty()) {
            return rpc_error{rpc_code::invalid_argument, "empty local bind host"};
        }
        resolve_options local_resolver;
        local_resolver.family = ip_family::any;
        local_resolver.timeout = options.timeout;
        result<std::vector<ip_address>> resolved_local =
            resolve_host(options.local_bind.host(), local_resolver);
        if (!resolved_local) {
            return resolved_local.error();
        }
        locals = std::move(resolved_local.value());
    }

    rpc_error last{rpc_code::unavailable, "no candidate address established the link"};
    for (const ip_address& target_ip : targets.value()) {
        ip_address local_ip;
        for (const ip_address& candidate : locals) {
            if (candidate.is_v4() == target_ip.is_v4()) {
                local_ip = candidate;
                break;
            }
        }
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::nanoseconds::zero()) {
            return rpc_error{rpc_code::deadline_exceeded,
                             "connect exceeded its deadline across candidates"};
        }
        result<std::unique_ptr<tcp_stream>> linked =
            connect_one(target_ip, target.port(), local_ip, options, remaining);
        if (linked) {
            return linked;
        }
        last = linked.error();
    }
    return last;
}

} // namespace net
} // namespace crpc