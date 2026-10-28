// CRPC -- the unified inbound listener.
//
// Bind, listen, and a cancelable accept loop. Cancellation uses a loopback UDP
// "wake" pair: a blocked `select` also watches the wake socket, and `close()`
// sends one datagram, so a cancel never has to poke a socket that another
// thread may concurrently be selecting on.
#include <crpc/net/listener.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "platform.hpp"

namespace crpc {
namespace net {
namespace {

using detail::native_socket;

// Two readable fds in one select (the listen socket + the wake socket).
// Returns 1 when either is ready (flags tell which), 0 on timeout, -1 on error.
int wait_readable_two(native_socket first, native_socket second,
                      std::chrono::nanoseconds timeout, bool& first_ready,
                      bool& second_ready) noexcept {
    fd_set read_set;
    fd_set error_set;
    FD_ZERO(&read_set);
    FD_ZERO(&error_set);
    FD_SET(first, &read_set);
    FD_SET(first, &error_set);
    FD_SET(second, &read_set);
    FD_SET(second, &error_set);

    timeval tv{};
    timeval* tv_ptr = nullptr;
    if (timeout == std::chrono::nanoseconds::zero()) {
        tv.tv_sec = 0;
        tv.tv_usec = 0;
        tv_ptr = &tv;
    } else if (timeout != std::chrono::nanoseconds::max()) {
        detail::to_timeval(timeout, tv);
        tv_ptr = &tv;
    }

#if defined(CRPC_OS_WINDOWS)
    const int selected = ::select(0, &read_set, nullptr, &error_set, tv_ptr);
#else
    const int selected =
        ::select(static_cast<int>(std::max(first, second)) + 1, &read_set, nullptr,
                 &error_set, tv_ptr);
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
    first_ready = FD_ISSET(first, &read_set) != 0;
    second_ready = FD_ISSET(second, &read_set) != 0;
    return 1;
}

// Creates the loopback UDP wake pair. `in` is read from a select, `out` is
// written to by close().
bool make_wake_pair(native_socket& read_side, native_socket& write_side) noexcept {
    read_side = detail::invalid_socket;
    write_side = detail::invalid_socket;

    const native_socket receiver = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (receiver == detail::invalid_socket) {
        return false;
    }
    sockaddr_in bound{};
    bound.sin_family = AF_INET;
    bound.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bound.sin_port = 0;
    if (::bind(receiver, reinterpret_cast<sockaddr*>(&bound), sizeof(bound)) != 0) {
        detail::close_socket(receiver);
        return false;
    }
    sockaddr_storage local_storage{};
    int local_len = 0;
    if (!detail::get_local_addr(receiver, local_storage, local_len)) {
        detail::close_socket(receiver);
        return false;
    }
    const native_socket sender = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sender == detail::invalid_socket) {
        detail::close_socket(receiver);
        return false;
    }
    if (::connect(sender, reinterpret_cast<sockaddr*>(&local_storage), local_len) != 0) {
        detail::close_socket(sender);
        detail::close_socket(receiver);
        return false;
    }
    (void)detail::set_nonblocking(receiver, true);
    (void)detail::set_nonblocking(sender, true);
    read_side = receiver;
    write_side = sender;
    return true;
}

// Applies inbound TCP options to a freshly accepted socket.
void configure_accepted(native_socket s, const listen_options& options) noexcept {
    (void)detail::set_nonblocking(s, true);
    tcp_keepalive keepalive;
    keepalive.enabled = false;
    (void)detail::apply_tcp_options(s, options.tcp_no_delay, options.stream, keepalive);
}

std::unique_ptr<tcp_stream> accept_stream(native_socket s,
                                          const listen_options& options) {
    sockaddr_storage peer_storage{};
    int peer_len = 0;
    std::string peer_host;
    u16 peer_port = 0;
    address peer;
    if (detail::get_peer_addr(s, peer_storage, peer_len) &&
        detail::sockaddr_to_host(peer_storage, peer_host, peer_port)) {
        peer = address::tcp(std::move(peer_host), peer_port);
    }
    sockaddr_storage local_storage{};
    int local_len = 0;
    std::string local_host;
    u16 local_port = 0;
    address local;
    if (detail::get_local_addr(s, local_storage, local_len) &&
        detail::sockaddr_to_host(local_storage, local_host, local_port)) {
        local = address::tcp(std::move(local_host), local_port);
    }
    return detail::stream_factory::adopt(static_cast<std::uintptr_t>(s),
                                         std::move(local), std::move(peer),
                                         options.stream);
}

// Binds one literal. Returns the socket on success.
result<native_socket> bind_one(const ip_address& address_ip, u16 port,
                               const listen_options& options) {
    sockaddr_storage storage{};
    int length = 0;
    if (!detail::ip_to_sockaddr(address_ip, port, storage, length)) {
        return rpc_error{rpc_code::invalid_argument, "unsupported address family"};
    }
    const native_socket s =
        ::socket(address_ip.is_v4() ? AF_INET : AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (s == detail::invalid_socket) {
        return rpc_error{rpc_code::resource_exhausted,
                         detail::error_text("socket() failed", detail::last_error())};
    }
    if (options.reuse_address) {
#if defined(CRPC_OS_WINDOWS)
        // On Windows SO_REUSEADDR does NOT mean "rebind a TIME_WAIT port": it
        // actively lets a second process bind the same address, and the second
        // listener then never receives a connection (the first keeps them
        // all). A server must use SO_EXCLUSIVEADDRUSE instead, which makes any
        // conflicting bind fail cleanly with WSAEACCES/WSAEADDRINUSE.
        BOOL exclusive = TRUE;
        (void)::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                           reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
#else
        int one = 1;
        (void)::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
    }
    if (address_ip.is_v6()) {
        const int value = options.ipv6_only ? 1 : 0;
        (void)::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY,
                           reinterpret_cast<const char*>(&value), sizeof(value));
    }
    if (::bind(s, reinterpret_cast<sockaddr*>(&storage), length) != 0) {
        const int error = detail::last_error();
        detail::close_socket(s);
        return rpc_error{classify_connect_error(error),
                         detail::error_text("bind() failed", error)};
    }
    const std::size_t backlog_size = options.backlog == 0 ? 128 : options.backlog;
    const int backlog = backlog_size > 0x7fffffff
                            ? 0x7fffffff
                            : static_cast<int>(backlog_size);
    if (::listen(s, backlog) != 0) {
        const int error = detail::last_error();
        detail::close_socket(s);
        return rpc_error{rpc_code::internal,
                         detail::error_text("listen() failed", error)};
    }
    (void)detail::set_nonblocking(s, true);
    return s;
}

// The success path once bind+listen succeeded.
std::unique_ptr<listener> adopt_listener(native_socket s, address local,
                                         const listen_options& options) {
    return detail::listener_factory::adopt(static_cast<std::uintptr_t>(s),
                                           std::move(local), options);
}

} // namespace

// --- listener ----------------------------------------------------------------

struct listener::impl {
    mutable std::mutex mutex;
    native_socket sock = detail::invalid_socket;
    native_socket wake_read = detail::invalid_socket;
    native_socket wake_write = detail::invalid_socket;
    address local;
    listen_options options;
    bool closed = false;
};

listener::listener(std::uintptr_t native, address local, listen_options options)
    : impl_(new impl) {
    impl_->sock = static_cast<detail::native_socket>(native);
    impl_->local = std::move(local);
    impl_->options = std::move(options);
    if (!make_wake_pair(impl_->wake_read, impl_->wake_write)) {
        // No cancellation path: a blocked accept still honours its deadline.
        impl_->wake_read = detail::invalid_socket;
        impl_->wake_write = detail::invalid_socket;
    }
}

listener::~listener() {
    close();
    if (impl_->sock != detail::invalid_socket) {
        detail::close_socket(impl_->sock);
        impl_->sock = detail::invalid_socket;
    }
    if (impl_->wake_read != detail::invalid_socket) {
        detail::close_socket(impl_->wake_read);
        impl_->wake_read = detail::invalid_socket;
    }
    if (impl_->wake_write != detail::invalid_socket) {
        detail::close_socket(impl_->wake_write);
        impl_->wake_write = detail::invalid_socket;
    }
}

std::unique_ptr<listener> detail::listener_factory::adopt(std::uintptr_t native,
                                                          address local,
                                                          listen_options options) {
    return std::unique_ptr<listener>(
        new listener(native, std::move(local), std::move(options)));
}

result<std::unique_ptr<listener>> listener::bind(const address& bind_address,
                                                 const listen_options& options) {
    detail::ensure_sockets_initialized();
    if (!bind_address.valid()) {
        return rpc_error{rpc_code::invalid_argument, "no bind address"};
    }
    if (bind_address.is_unix()) {
        const status validated = validate_unix_path(bind_address.path());
        if (!validated) {
            return rpc_error{validated.code(), validated.message()};
        }
        sockaddr_un sun{};
        int sun_len = 0;
        if (!detail::unix_to_sockaddr(bind_address.path(), sun, sun_len)) {
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
        if (::bind(s, reinterpret_cast<sockaddr*>(&sun), sun_len) != 0) {
            const int error = detail::last_error();
            detail::close_socket(s);
            return rpc_error{classify_connect_error(error),
                             detail::error_text("unix bind() failed", error)};
        }
        const int backlog = static_cast<int>(options.backlog == 0 ? 128 : options.backlog);
        if (::listen(s, backlog) != 0) {
            const int error = detail::last_error();
            detail::close_socket(s);
            return rpc_error{rpc_code::internal,
                             detail::error_text("unix listen() failed", error)};
        }
        (void)detail::set_nonblocking(s, true);
        return adopt_listener(s, bind_address, options);
    }

    if (bind_address.host().empty()) {
        return rpc_error{rpc_code::invalid_argument, "empty bind host"};
    }
    resolve_options resolver;
    resolver.family = ip_family::any;
    resolver.timeout = std::chrono::seconds(5);
    result<std::vector<ip_address>> resolved = resolve_host(bind_address.host(), resolver);
    if (!resolved) {
        return resolved.error();
    }

    rpc_error last{rpc_code::unavailable, "no candidate address bound"};
    for (const ip_address& candidate : resolved.value()) {
        result<native_socket> bound = bind_one(candidate, bind_address.port(), options);
        if (bound) {
            // Report the real address (an ephemeral port is now known).
            sockaddr_storage local_storage{};
            int local_len = 0;
            std::string local_host;
            u16 local_port = 0;
            address local;
            if (detail::get_local_addr(bound.value(), local_storage, local_len) &&
                detail::sockaddr_to_host(local_storage, local_host, local_port)) {
                local = address::tcp(std::move(local_host), local_port);
            } else {
                local = address::tcp(bind_address.host(), bind_address.port());
            }
            return adopt_listener(bound.value(), std::move(local), options);
        }
        last = bound.error();
    }
    return last;
}

result<std::unique_ptr<tcp_stream>> listener::accept(std::chrono::nanoseconds timeout) {
    auto& self = *impl_;

    const bool have_wake = self.wake_read != detail::invalid_socket;
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(self.mutex);
            if (self.closed) {
                return rpc_error{rpc_code::shutting_down, "listener is closed"};
            }
            if (self.sock == detail::invalid_socket) {
                return rpc_error{rpc_code::shutting_down, "listener is closed"};
            }
        }

        bool listen_ready = false;
        bool wake_ready = false;
        // The listener contract: a zero timeout waits forever, so it is mapped
        // to the infinite wait (select with a null tv); only `close()` wakes it.
        const std::chrono::nanoseconds effective =
            timeout == std::chrono::nanoseconds::zero()
                ? std::chrono::nanoseconds::max()
                : timeout;
        const int ready =
            have_wake
                ? wait_readable_two(self.sock, self.wake_read, effective,
                                    listen_ready, wake_ready)
                : wait_readable_two(self.sock, self.sock, effective, listen_ready,
                                    wake_ready);
        if (ready == 0) {
            return rpc_error{rpc_code::deadline_exceeded, "accept timed out"};
        }
        if (ready < 0) {
            return rpc_error{rpc_code::internal,
                             detail::error_text("accept wait failed",
                                                detail::last_error())};
        }
        if (wake_ready) {
            // The wake datagram only ever arrives from close().
            std::lock_guard<std::mutex> lock(self.mutex);
            if (self.closed) {
                return rpc_error{rpc_code::shutting_down, "listener is closed"};
            }
            continue;
        }
        (void)listen_ready;

        native_socket accepted =
            ::accept(self.sock, nullptr, nullptr);
        if (accepted == detail::invalid_socket) {
            const int error = detail::last_error();
            if (detail::would_block(error)) {
                continue;
            }
#if defined(CRPC_OS_WINDOWS)
            if (error == WSAECONNABORTED) {
                continue;
            }
#else
            if (error == ECONNABORTED) {
                continue;
            }
#endif
            const rpc_code code = classify_connect_error(error);
            return rpc_error{code, detail::error_text("accept() failed", error)};
        }
        configure_accepted(accepted, self.options);
        return accept_stream(accepted, self.options);
    }
}

void listener::close() noexcept {
    auto& self = *impl_;
    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(self.mutex);
        if (!self.closed) {
            self.closed = true;
            notify = true;
        }
    }
    if (notify && self.wake_write != detail::invalid_socket) {
        const char wake = 'q';
        (void)::send(self.wake_write, &wake, static_cast<int>(sizeof(wake)), 0);
    }
}

bool listener::is_open() const noexcept {
    const auto& self = *impl_;
    std::lock_guard<std::mutex> lock(self.mutex);
    return !self.closed && self.sock != detail::invalid_socket;
}

address listener::local() const { return impl_->local; }
const listen_options& listener::options() const noexcept { return impl_->options; }

} // namespace net
} // namespace crpc