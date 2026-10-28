// CRPC -- internal socket platform layer for the net framework.
//
// Everything below this header is private to `src/net/`. It exists so a
// protocol-facing translation unit has a single spelling for the two platforms
// (Winsock and POSIX) instead of a scatter of `#ifdef`s: native handle,
// non-blocking mode, the wait set, fresh/deprecated address conversions and
// the error-to-`rpc_code` mapping that is part of the public failure contract.
#ifndef CRPC_SRC_NET_PLATFORM_HPP
#define CRPC_SRC_NET_PLATFORM_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/net/address.hpp>
#include <crpc/net/channel.hpp>
#include <crpc/net/connector.hpp>
#include <crpc/net/ip.hpp>
#include <crpc/net/udp.hpp>

#if defined(CRPC_OS_WINDOWS)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#include <mstcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace crpc {
namespace net {
namespace detail {

#if defined(CRPC_OS_WINDOWS)
using native_socket = SOCKET;
inline constexpr native_socket invalid_socket = INVALID_SOCKET;
inline constexpr int k_address_family_unix = AF_UNIX;
#else
using native_socket = int;
inline constexpr native_socket invalid_socket = -1;
inline constexpr int k_address_family_unix = AF_UNIX;
#endif

// Winsock needs one start per process; harmless when io::iocp_backend already
// started it (WSAStartup is ref-counted).
inline void ensure_sockets_initialized() noexcept {
#if defined(CRPC_OS_WINDOWS)
    static const bool started = []() {
        WSADATA data{};
        return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
#endif
}

inline int last_error() noexcept {
#if defined(CRPC_OS_WINDOWS)
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

inline bool connect_would_block(int error) noexcept {
#if defined(CRPC_OS_WINDOWS)
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
    return error == EINPROGRESS || error == EWOULDBLOCK;
#endif
}

inline bool missing_family(int error) noexcept {
#if defined(CRPC_OS_WINDOWS)
    return error == WSAEAFNOSUPPORT;
#else
    return error == EAFNOSUPPORT;
#endif
}

inline bool would_block(int error) noexcept {
#if defined(CRPC_OS_WINDOWS)
    return error == WSAEWOULDBLOCK;
#else
    return error == EWOULDBLOCK || error == EAGAIN;
#endif
}

inline void close_socket(native_socket s) noexcept {
    if (s != invalid_socket) {
#if defined(CRPC_OS_WINDOWS)
        (void)::closesocket(s);
#else
        (void)::close(s);
#endif
    }
}

inline bool set_nonblocking(native_socket s, bool on) noexcept {
#if defined(CRPC_OS_WINDOWS)
    u_long mode = on ? 1 : 0;
    return ::ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    const int flags = ::fcntl(s, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    const int next = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return ::fcntl(s, F_SETFL, next) == 0;
#endif
}

inline void to_timeval(std::chrono::nanoseconds ns, timeval& tv) noexcept {
    if (ns < std::chrono::nanoseconds::zero()) {
        ns = std::chrono::nanoseconds::zero();
    }
    const std::chrono::seconds secs = std::chrono::duration_cast<std::chrono::seconds>(ns);
    tv.tv_sec = static_cast<long>(secs.count());
    tv.tv_usec = static_cast<long>(
        std::chrono::duration_cast<std::chrono::microseconds>(ns - secs).count());
}

enum class wait_kind { read, write };

// Selects a socket for readiness. `timeout == 0` polls once; a `nanoseconds::max()`
// waits forever. Returns 1 when ready, 0 on timeout, -1 on a hard wait error.
inline int wait_ready(native_socket s, wait_kind kind,
                      std::chrono::nanoseconds timeout) noexcept {
    fd_set read_set;
    fd_set write_set;
    fd_set error_set;
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    FD_ZERO(&error_set);
    fd_set* read_ptr = nullptr;
    fd_set* write_ptr = nullptr;
    if (kind == wait_kind::read) {
        read_ptr = &read_set;
        FD_SET(s, &read_set);
    } else {
        write_ptr = &write_set;
        FD_SET(s, &write_set);
    }
    FD_SET(s, &error_set);
    fd_set* error_ptr = &error_set;

    timeval tv{};
    timeval* tv_ptr = nullptr;
    if (timeout == std::chrono::nanoseconds::zero()) {
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        tv_ptr = &tv;
    } else if (timeout != std::chrono::nanoseconds::max()) {
        to_timeval(timeout, tv);
        tv_ptr = &tv;
    }

#if defined(CRPC_OS_WINDOWS)
    const int selected = ::select(0, read_ptr, write_ptr, error_ptr, tv_ptr);
#else
    const int selected = ::select(static_cast<int>(s) + 1, read_ptr, write_ptr,
                                  error_ptr, tv_ptr);
#endif
    if (selected == 0) {
        return 0;
    }
    if (selected < 0) {
#if defined(CRPC_OS_WINDOWS)
        return -1;
#else
        return errno == EINTR ? 0 : -1;
#endif
    }
    return 1;
}

// Converts a resolved literal into a native sockaddr. Returns false when the
// literal is not an address of a supported family.
inline bool ip_to_sockaddr(const ip_address& ip, u16 port, sockaddr_storage& out,
                           int& out_len) noexcept {
    std::memset(&out, 0, sizeof(out));
    if (ip.is_v4()) {
        sockaddr_in* a = reinterpret_cast<sockaddr_in*>(&out);
        a->sin_family = AF_INET;
        a->sin_port = htons(port);
        std::memcpy(&a->sin_addr, ip.bytes().data(), 4);
        out_len = static_cast<int>(sizeof(sockaddr_in));
        return true;
    }
    if (ip.is_v6()) {
        sockaddr_in6* a = reinterpret_cast<sockaddr_in6*>(&out);
        a->sin6_family = AF_INET6;
        a->sin6_port = htons(port);
        std::memcpy(&a->sin6_addr, ip.bytes().data(), 16);
        out_len = static_cast<int>(sizeof(sockaddr_in6));
        return true;
    }
    return false;
}

inline int family_of(const sockaddr_storage& ss) noexcept {
    return static_cast<int>(ss.ss_family);
}

// The reverse: native sockaddr -> literal + port. Returns false on failure.
inline bool sockaddr_to_host(const sockaddr_storage& ss, std::string& host,
                             u16& port) noexcept {
    host.clear();
    port = 0;
    if (ss.ss_family == AF_INET) {
        const sockaddr_in* a = reinterpret_cast<const sockaddr_in*>(&ss);
        char buffer[INET_ADDRSTRLEN];
        if (::inet_ntop(AF_INET, &a->sin_addr, buffer, sizeof(buffer)) == nullptr) {
            return false;
        }
        host = buffer;
        port = ntohs(a->sin_port);
        return true;
    }
    if (ss.ss_family == AF_INET6) {
        const sockaddr_in6* a = reinterpret_cast<const sockaddr_in6*>(&ss);
        char buffer[INET6_ADDRSTRLEN];
        if (::inet_ntop(AF_INET6, &a->sin6_addr, buffer, sizeof(buffer)) == nullptr) {
            return false;
        }
        host = buffer;
        port = ntohs(a->sin6_port);
        return true;
    }
    return false;
}

// SO_ERROR for a socket, in the direction a generic portable caller needs.
inline bool socket_error(native_socket s, int& out) noexcept {
    int value = 0;
#if defined(CRPC_OS_WINDOWS)
    int length = static_cast<int>(sizeof(value));
    if (::getsockopt(s, SOL_SOCKET, SO_ERROR,
                     reinterpret_cast<char*>(&value), &length) != 0) {
        return false;
    }
#else
    socklen_t length = sizeof(value);
    if (::getsockopt(s, SOL_SOCKET, SO_ERROR, &value, &length) != 0) {
        return false;
    }
#endif
    out = value;
    return true;
}

inline bool get_local_addr(native_socket s, sockaddr_storage& ss, int& length) noexcept {
    std::memset(&ss, 0, sizeof(ss));
#if defined(CRPC_OS_WINDOWS)
    int len = static_cast<int>(sizeof(ss));
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
        return false;
    }
#else
    socklen_t len = sizeof(ss);
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
        return false;
    }
#endif
    length = static_cast<int>(len);
    return true;
}

inline bool get_peer_addr(native_socket s, sockaddr_storage& ss, int& length) noexcept {
    std::memset(&ss, 0, sizeof(ss));
#if defined(CRPC_OS_WINDOWS)
    int len = static_cast<int>(sizeof(ss));
    if (::getpeername(s, reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
        return false;
    }
#else
    socklen_t len = sizeof(ss);
    if (::getpeername(s, reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
        return false;
    }
#endif
    length = static_cast<int>(len);
    return true;
}

inline bool getsockopt_native(native_socket s, int level, int name, int& value) noexcept {
    int out = 0;
#if defined(CRPC_OS_WINDOWS)
    int length = static_cast<int>(sizeof(out));
    const bool ok = ::getsockopt(s, level, name, reinterpret_cast<char*>(&out),
                                 &length) == 0;
#else
    socklen_t length = sizeof(out);
    const bool ok = ::getsockopt(s, level, name, &out, &length) == 0;
#endif
    if (ok) {
        value = out;
    }
    return ok;
}

// Keepalive: enabled with idle/interval/probes. Windows uses SIO_KEEPALIVE_VALS
// (milliseconds), POSIX the TCP_KEEP* socket options.
//
// `keepalive_values` replicates the layout of winsock's `tcp_keepalive` without
// depending on its exact name: the field set is part of the WSAIoctl contract.
struct keepalive_values {
    unsigned long onoff;
    unsigned long keepalivetime;
    unsigned long keepaliveinterval;
};

inline bool apply_keepalive(native_socket s, const tcp_keepalive& ka) noexcept {
#if defined(CRPC_OS_WINDOWS)
    BOOL on = ka.enabled ? TRUE : FALSE;
    if (::setsockopt(s, SOL_SOCKET, SO_KEEPALIVE,
                     reinterpret_cast<const char*>(&on), sizeof(on)) != 0) {
        return false;
    }
    if (!ka.enabled) {
        return true;
    }
    keepalive_values values{};
    values.onoff = 1;
    values.keepalivetime = static_cast<unsigned long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(ka.idle).count());
    values.keepaliveinterval = static_cast<unsigned long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(ka.interval).count());
    DWORD returned = 0;
    return ::WSAIoctl(s, SIO_KEEPALIVE_VALS, &values, sizeof(values), nullptr, 0,
                      &returned, nullptr, nullptr) == 0;
#else
    int on = ka.enabled ? 1 : 0;
    if (::setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) != 0) {
        return false;
    }
    if (!ka.enabled) {
        return true;
    }
    const int idle = static_cast<int>(ka.idle.count());
    const int interval = static_cast<int>(ka.interval.count());
    const int probes = static_cast<int>(ka.probes);
    bool ok = true;
#ifdef TCP_KEEPIDLE
    ok = ok && ::setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) == 0;
#endif
#ifdef TCP_KEEPINTVL
    ok = ok && ::setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) == 0;
#endif
#ifdef TCP_KEEPCNT
    ok = ok && ::setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &probes, sizeof(probes)) == 0;
#endif
    return ok;
#endif
}

inline bool apply_tcp_options(native_socket s, bool no_delay,
                              const stream_options& so, const tcp_keepalive& ka) noexcept {
    if (so.recv_buffer_bytes > 0) {
        const int rcv = static_cast<int>(so.recv_buffer_bytes);
        (void)::setsockopt(s, SOL_SOCKET, SO_RCVBUF,
                           reinterpret_cast<const char*>(&rcv), sizeof(rcv));
    }
    if (so.send_buffer_bytes > 0) {
        const int snd = static_cast<int>(so.send_buffer_bytes);
        (void)::setsockopt(s, SOL_SOCKET, SO_SNDBUF,
                           reinterpret_cast<const char*>(&snd), sizeof(snd));
    }
    if (no_delay) {
#if defined(CRPC_OS_WINDOWS)
        BOOL one = TRUE;
        (void)::setsockopt(s, IPPROTO_TCP, TCP_NODELAY,
                           reinterpret_cast<const char*>(&one), sizeof(one));
#else
        int one = 1;
        (void)::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
    }
    return apply_keepalive(s, ka);
}

inline bool apply_udp_options(native_socket s, const udp_options& uo) noexcept {
    if (uo.recv_buffer_bytes > 0) {
        const int rcv = static_cast<int>(uo.recv_buffer_bytes);
        (void)::setsockopt(s, SOL_SOCKET, SO_RCVBUF,
                           reinterpret_cast<const char*>(&rcv), sizeof(rcv));
    }
    if (uo.send_buffer_bytes > 0) {
        const int snd = static_cast<int>(uo.send_buffer_bytes);
        (void)::setsockopt(s, SOL_SOCKET, SO_SNDBUF,
                           reinterpret_cast<const char*>(&snd), sizeof(snd));
    }
#if defined(CRPC_OS_WINDOWS)
    BOOL b = uo.enable_broadcast ? TRUE : FALSE;
    (void)::setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&b),
                       sizeof(b));
#else
    int b = uo.enable_broadcast ? 1 : 0;
    (void)::setsockopt(s, SOL_SOCKET, SO_BROADCAST, &b, sizeof(b));
#endif
    return true;
}

// A Unix path into sockaddr_un. `out_len` receives the addrlen to pass.
inline bool unix_to_sockaddr(const std::string& path, sockaddr_un& out,
                             int& out_len) noexcept {
    if (path.empty() || path.size() >= sizeof(out.sun_path) ||
        path.find('\0') != std::string::npos) {
        return false;
    }
    std::memset(&out, 0, sizeof(out));
    out.sun_family = AF_UNIX;
    std::memcpy(out.sun_path, path.data(), path.size());
    out.sun_path[path.size()] = '\0';
#if defined(CRPC_OS_WINDOWS)
    out_len = static_cast<int>(sizeof(out));
#else
    out_len = static_cast<int>(offsetof(struct sockaddr_un, sun_path) + path.size() + 1);
#endif
    return true;
}

// Error classification lives in the public `crpc::net::classify_connect_error`
// / `crpc::net::classify_stream_error` (see connector.cpp); it is a documented
// part of the connector contract so upper layers can map raw OS errors too.

// The error message shown next to an rpc_code, with the OS error number.
inline std::string error_text(const char* context, int error) noexcept {
    std::string out = context;
    out += " (os error ";
    out += std::to_string(error);
    out += ")";
    return out;
}

} // namespace detail
} // namespace net
} // namespace crpc

#endif // CRPC_SRC_NET_PLATFORM_HPP