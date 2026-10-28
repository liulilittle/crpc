// CRPC -- the unified UDP socket.
#include <crpc/net/udp.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "platform.hpp"

#if defined(CRPC_OS_WINDOWS)
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#endif

namespace crpc {
namespace net {
namespace {

using detail::native_socket;

#if defined(CRPC_OS_WINDOWS)
// The IPv4 address of the adapter whose interface index matches, or 0 when the
// index does not exist.
std::uint32_t interface_ipv4(std::uint32_t index) noexcept {
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer;
    buffer.resize(size);
    const ULONG result = ::GetAdaptersAddresses(AF_INET, 0, nullptr,
                                                reinterpret_cast<IP_ADAPTER_ADDRESSES*>(
                                                    buffer.data()),
                                                &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
    }
    auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    for (IP_ADAPTER_ADDRESSES* it = adapter; it != nullptr; it = it->Next) {
        if (it->IfIndex == index || it->Ipv6IfIndex == index) {
            for (const auto* unicast = it->FirstUnicastAddress; unicast != nullptr;
                 unicast = unicast->Next) {
                if (unicast->Address.lpSockaddr != nullptr &&
                    unicast->Address.lpSockaddr->sa_family == AF_INET) {
                    const auto* addr =
                        reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
                    return addr->sin_addr.s_addr;
                }
            }
        }
    }
    return 0;
}
#endif

} // namespace

// --- udp_socket ---------------------------------------------------------------

struct udp_socket::impl {
    native_socket sock = detail::invalid_socket;
    address local;
    udp_options options;
    int family = AF_INET;
    bool open = true;
    rpc_error terminal;

    [[nodiscard]] rpc_error fail() const noexcept {
        if (terminal.code != rpc_code::ok) {
            return terminal;
        }
        if (sock == detail::invalid_socket || !open) {
            return rpc_error{rpc_code::shutting_down, "socket is closed"};
        }
        return rpc_error{rpc_code::internal, "socket is not ready"};
    }
};

udp_socket::udp_socket(std::uintptr_t native, address local, udp_options options)
    : impl_(new impl) {
    impl_->sock = static_cast<detail::native_socket>(native);
    impl_->local = std::move(local);
    impl_->options = std::move(options);
    impl_->family = AF_INET;
#if !defined(CRPC_OS_WINDOWS)
    (void)impl_->family;
#endif
}

udp_socket::~udp_socket() { close(); }

std::unique_ptr<udp_socket> detail::udp_factory::adopt(std::uintptr_t native,
                                                       address local,
                                                       udp_options options) {
    return std::unique_ptr<udp_socket>(
        new udp_socket(native, std::move(local), std::move(options)));
}

result<std::unique_ptr<udp_socket>> udp_socket::bind(const address& bind_address,
                                                     const udp_options& options) {
    detail::ensure_sockets_initialized();
    if (!bind_address.valid() || !bind_address.is_tcp()) {
        return rpc_error{rpc_code::invalid_argument,
                         "UDP bind needs a TCP-form host/port address"};
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
        sockaddr_storage storage{};
        int length = 0;
        if (!detail::ip_to_sockaddr(candidate, bind_address.port(), storage, length)) {
            continue;
        }
        const native_socket s =
            ::socket(candidate.is_v4() ? AF_INET : AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
        if (s == detail::invalid_socket) {
            last = rpc_error{rpc_code::resource_exhausted,
                             detail::error_text("socket() failed",
                                                detail::last_error())};
            continue;
        }
        (void)detail::apply_udp_options(s, options);
        if (::bind(s, reinterpret_cast<sockaddr*>(&storage), length) != 0) {
            const int error = detail::last_error();
            detail::close_socket(s);
            last = rpc_error{classify_connect_error(error),
                             detail::error_text("udp bind() failed", error)};
            continue;
        }
        (void)detail::set_nonblocking(s, true);

        sockaddr_storage local_storage{};
        int local_len = 0;
        std::string local_host;
        u16 local_port = 0;
        address local;
        if (detail::get_local_addr(s, local_storage, local_len) &&
            detail::sockaddr_to_host(local_storage, local_host, local_port)) {
            local = address::tcp(std::move(local_host), local_port);
        } else {
            local = address::tcp(bind_address.host(), bind_address.port());
        }
        std::unique_ptr<udp_socket> created =
            detail::udp_factory::adopt(static_cast<std::uintptr_t>(s),
                                       std::move(local), options);
        impl& state = *created->impl_;
        state.family = candidate.is_v4() ? AF_INET : AF_INET6;
        return created;
    }
    return last;
}

status udp_socket::send_to(span<const std::byte> data, const address& dest,
                           std::chrono::nanoseconds timeout) {
    auto& self = *impl_;
    if (self.sock == detail::invalid_socket || !self.open) {
        return self.fail();
    }
    if (!dest.valid() || !dest.is_tcp()) {
        return rpc_error{rpc_code::invalid_argument,
                         "UDP destination needs a TCP-form host/port address"};
    }
    if (dest.port() == 0) {
        return rpc_error{rpc_code::invalid_argument, "UDP destination port is 0"};
    }
    resolve_options resolver;
    resolver.family = ip_family::any;
    resolver.timeout = std::chrono::seconds(5);
    result<std::vector<ip_address>> resolved = resolve_host(dest.host(), resolver);
    if (!resolved) {
        return resolved.error();
    }
    ip_address chosen;
    bool matched = false;
    for (const ip_address& candidate : resolved.value()) {
        const int family = candidate.is_v4() ? AF_INET : AF_INET6;
        if (family == self.family) {
            chosen = candidate;
            matched = true;
            break;
        }
    }
    if (!matched) {
        return rpc_error{rpc_code::invalid_argument,
                         "destination family does not match the socket"};
    }
    sockaddr_storage storage{};
    int length = 0;
    if (!detail::ip_to_sockaddr(chosen, dest.port(), storage, length)) {
        return rpc_error{rpc_code::invalid_argument, "unsupported destination family"};
    }

    for (;;) {
        const int ready = detail::wait_ready(self.sock, detail::wait_kind::write,
                                             timeout);
        if (ready == 0) {
            return rpc_error{rpc_code::deadline_exceeded, "send timed out"};
        }
        if (ready < 0) {
            return rpc_error{rpc_code::internal,
                             detail::error_text("waiting to send failed",
                                                detail::last_error())};
        }
#if defined(CRPC_OS_WINDOWS)
        const int count = data.size() > static_cast<std::size_t>(INT_MAX)
                              ? INT_MAX
                              : static_cast<int>(data.size());
        const int pushed = ::sendto(self.sock,
                                    reinterpret_cast<const char*>(data.data()), count,
                                    0, reinterpret_cast<sockaddr*>(&storage), length);
#else
        const ssize_t pushed = ::sendto(self.sock, data.data(), data.size(), 0,
                                        reinterpret_cast<sockaddr*>(&storage),
                                        static_cast<socklen_t>(length));
#endif
        if (pushed >= 0) {
            return status{};
        }
        const int error = detail::last_error();
        if (detail::would_block(error)) {
            continue;
        }
        const rpc_code code = classify_stream_error(error);
        return status{code, detail::error_text("sendto failed", error)};
    }
}

result<udp_datagram> udp_socket::recv_from(span<std::byte> buffer,
                                           std::chrono::nanoseconds timeout) {
    auto& self = *impl_;
    if (self.sock == detail::invalid_socket || !self.open) {
        return self.fail();
    }
    if (buffer.empty()) {
        return rpc_error{rpc_code::invalid_argument, "empty receive buffer"};
    }
    for (;;) {
        const int ready = detail::wait_ready(self.sock, detail::wait_kind::read,
                                             timeout);
        if (ready == 0) {
            return rpc_error{rpc_code::deadline_exceeded, "receive timed out"};
        }
        if (ready < 0) {
            return rpc_error{rpc_code::internal,
                             detail::error_text("waiting to receive failed",
                                                detail::last_error())};
        }
        sockaddr_storage source_storage{};
#if defined(CRPC_OS_WINDOWS)
        int source_len = static_cast<int>(sizeof(source_storage));
        const int count = buffer.size() > static_cast<std::size_t>(INT_MAX)
                              ? INT_MAX
                              : static_cast<int>(buffer.size());
        const int got = ::recvfrom(self.sock, reinterpret_cast<char*>(buffer.data()),
                                   count, 0, reinterpret_cast<sockaddr*>(&source_storage),
                                   &source_len);
#else
        socklen_t source_len = sizeof(source_storage);
        const ssize_t got = ::recvfrom(self.sock, buffer.data(), buffer.size(), 0,
                                       reinterpret_cast<sockaddr*>(&source_storage),
                                       &source_len);
#endif
        if (got >= 0) {
            udp_datagram datagram;
            datagram.bytes = static_cast<std::size_t>(got);
            std::string source_host;
            u16 source_port = 0;
            if (detail::sockaddr_to_host(source_storage, source_host, source_port)) {
                datagram.source = address::tcp(std::move(source_host), source_port);
            }
            return datagram;
        }
        const int error = detail::last_error();
        if (detail::would_block(error)) {
            continue;
        }
#if defined(CRPC_OS_WINDOWS)
        if (error == WSAEMSGSIZE) {
            return rpc_error{rpc_code::invalid_argument,
                             "datagram is larger than the receive buffer"};
        }
#else
        if (error == EMSGSIZE) {
            return rpc_error{rpc_code::invalid_argument,
                             "datagram is larger than the receive buffer"};
        }
#endif
        const rpc_code code = classify_stream_error(error);
        return rpc_error{code, detail::error_text("recvfrom failed", error)};
    }
}

status udp_socket::set_broadcast(bool enable) {
    auto& self = *impl_;
    if (self.sock == detail::invalid_socket || !self.open) {
        return self.fail();
    }
#if defined(CRPC_OS_WINDOWS)
    BOOL value = enable ? TRUE : FALSE;
    if (::setsockopt(self.sock, SOL_SOCKET, SO_BROADCAST,
                     reinterpret_cast<const char*>(&value), sizeof(value)) != 0) {
        return status{rpc_code::internal,
                      detail::error_text("SO_BROADCAST failed", detail::last_error())};
    }
#else
    int value = enable ? 1 : 0;
    if (::setsockopt(self.sock, SOL_SOCKET, SO_BROADCAST, &value, sizeof(value)) != 0) {
        return status{rpc_code::internal,
                      detail::error_text("SO_BROADCAST failed", detail::last_error())};
    }
#endif
    return status{};
}

status udp_socket::join_multicast(const ip_address& group,
                                  std::uint32_t interface_index) {
    auto& self = *impl_;
    if (self.sock == detail::invalid_socket || !self.open) {
        return self.fail();
    }
    if (!group.valid()) {
        return rpc_error{rpc_code::invalid_argument, "empty multicast group"};
    }
    if (group.is_v4()) {
        ip_mreq membership{};
#if defined(CRPC_OS_WINDOWS)
        std::memcpy(&membership.imr_multiaddr, group.bytes().data(), 4);
        if (interface_index == 0) {
            membership.imr_interface.s_addr = htonl(INADDR_ANY);
        } else {
            membership.imr_interface.s_addr = interface_ipv4(interface_index);
            if (membership.imr_interface.s_addr == 0) {
                return rpc_error{rpc_code::invalid_argument,
                                 "multicast interface index is not an IPv4 adapter"};
            }
        }
        if (::setsockopt(self.sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                         reinterpret_cast<const char*>(&membership),
                         sizeof(membership)) != 0) {
            return status{rpc_code::internal,
                          detail::error_text("IP_ADD_MEMBERSHIP failed",
                                             detail::last_error())};
        }
#else
        std::memcpy(&membership.imr_multiaddr, group.bytes().data(), 4);
        if (interface_index == 0) {
            membership.imr_interface.s_addr = htonl(INADDR_ANY);
        } else {
            // POSIX has no portable index->address helper here; use the index
            // directly via the IPv4-mapped support on platforms that offer it.
            return rpc_error{rpc_code::invalid_argument,
                             "non-default IPv4 multicast interface not supported here"};
        }
        if (::setsockopt(self.sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership,
                         sizeof(membership)) != 0) {
            return status{rpc_code::internal,
                          detail::error_text("IP_ADD_MEMBERSHIP failed",
                                             detail::last_error())};
        }
#endif
        return status{};
    }
    if (group.is_v6()) {
#if defined(CRPC_OS_WINDOWS)
        ipv6_mreq membership{};
        std::memcpy(&membership.ipv6mr_multiaddr, group.bytes().data(), 16);
        membership.ipv6mr_interface = static_cast<unsigned>(interface_index);
        if (::setsockopt(self.sock, IPPROTO_IPV6, IPV6_JOIN_GROUP,
                         reinterpret_cast<const char*>(&membership),
                         sizeof(membership)) != 0) {
            return status{rpc_code::internal,
                          detail::error_text("IPV6_JOIN_GROUP failed",
                                             detail::last_error())};
        }
#else
        ipv6_mreq membership{};
        std::memcpy(&membership.ipv6mr_multiaddr, group.bytes().data(), 16);
        membership.ipv6mr_interface = static_cast<unsigned>(interface_index);
        if (::setsockopt(self.sock, IPPROTO_IPV6, IPV6_JOIN_GROUP, &membership,
                         sizeof(membership)) != 0) {
            return status{rpc_code::internal,
                          detail::error_text("IPV6_JOIN_GROUP failed",
                                             detail::last_error())};
        }
#endif
        return status{};
    }
    return rpc_error{rpc_code::invalid_argument, "unsupported multicast address"};
}

status udp_socket::leave_multicast(const ip_address& group,
                                   std::uint32_t interface_index) {
    auto& self = *impl_;
    if (self.sock == detail::invalid_socket || !self.open) {
        return self.fail();
    }
    if (!group.valid()) {
        return rpc_error{rpc_code::invalid_argument, "empty multicast group"};
    }
    if (group.is_v4()) {
        ip_mreq membership{};
        std::memcpy(&membership.imr_multiaddr, group.bytes().data(), 4);
#if defined(CRPC_OS_WINDOWS)
        if (interface_index == 0) {
            membership.imr_interface.s_addr = htonl(INADDR_ANY);
        } else {
            membership.imr_interface.s_addr = interface_ipv4(interface_index);
            if (membership.imr_interface.s_addr == 0) {
                return rpc_error{rpc_code::invalid_argument,
                                 "multicast interface index is not an IPv4 adapter"};
            }
        }
        if (::setsockopt(self.sock, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                         reinterpret_cast<const char*>(&membership),
                         sizeof(membership)) != 0) {
            return status{rpc_code::internal,
                          detail::error_text("IP_DROP_MEMBERSHIP failed",
                                             detail::last_error())};
        }
#else
        membership.imr_interface.s_addr = htonl(INADDR_ANY);
        if (::setsockopt(self.sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &membership,
                         sizeof(membership)) != 0) {
            return status{rpc_code::internal,
                          detail::error_text("IP_DROP_MEMBERSHIP failed",
                                             detail::last_error())};
        }
#endif
        return status{};
    }
    if (group.is_v6()) {
#if defined(CRPC_OS_WINDOWS)
        ipv6_mreq membership{};
        std::memcpy(&membership.ipv6mr_multiaddr, group.bytes().data(), 16);
        membership.ipv6mr_interface = static_cast<unsigned>(interface_index);
        if (::setsockopt(self.sock, IPPROTO_IPV6, IPV6_LEAVE_GROUP,
                         reinterpret_cast<const char*>(&membership),
                         sizeof(membership)) != 0) {
            return status{rpc_code::internal,
                          detail::error_text("IPV6_LEAVE_GROUP failed",
                                             detail::last_error())};
        }
#else
        ipv6_mreq membership{};
        std::memcpy(&membership.ipv6mr_multiaddr, group.bytes().data(), 16);
        membership.ipv6mr_interface = static_cast<unsigned>(interface_index);
        if (::setsockopt(self.sock, IPPROTO_IPV6, IPV6_LEAVE_GROUP, &membership,
                         sizeof(membership)) != 0) {
            return status{rpc_code::internal,
                          detail::error_text("IPV6_LEAVE_GROUP failed",
                                             detail::last_error())};
        }
#endif
        return status{};
    }
    return rpc_error{rpc_code::invalid_argument, "unsupported multicast address"};
}

void udp_socket::close() noexcept {
    auto& self = *impl_;
    if (self.sock != detail::invalid_socket) {
        detail::close_socket(self.sock);
        self.sock = detail::invalid_socket;
    }
    self.open = false;
    if (self.terminal.code == rpc_code::ok) {
        self.terminal = rpc_error{rpc_code::shutting_down, "socket is closed"};
    }
}

bool udp_socket::is_open() const noexcept {
    const auto& self = *impl_;
    return self.sock != detail::invalid_socket && self.open;
}

address udp_socket::local() const { return impl_->local; }
const udp_options& udp_socket::options() const noexcept { return impl_->options; }

} // namespace net
} // namespace crpc