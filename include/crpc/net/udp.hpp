// CRPC -- the unified UDP socket.
//
// UDP is a message transport, not a stream, so it deliberately does not share
// `net::channel`: a datagram arrives whole or it is truncated, and there is no
// EOF. `udp_socket` gives the protocol layers one message primitive -- send to
// an address, receive from an address -- with the same `result`/`status` and
// timeout discipline as the stream half of the framework.
//
// Datagram semantics
// ------------------
//   * `send_to` is all-or-nothing for the datagram (a single `sendto`).
//   * `recv_from` fills `buffer` with one datagram. The remote address is
//     always reported next to the bytes. On Windows a datagram larger than the
//     buffer is truncated in place and the excess discarded (the API reports
//     `invalid_argument`); size the buffer to the protocol's maximum message.
//   * A timeout of 0 on either operation means "no wait": `recv_from` with no
//     pending datagram fails `deadline_exceeded` immediately, `send_to` fails
//     the same way if the send buffer is full.
//
// Broadcast and multicast are supported (`set_broadcast`, `join_multicast` /
// `leave_multicast`); neither is enabled implicitly.
#ifndef CRPC_NET_UDP_HPP
#define CRPC_NET_UDP_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/net/address.hpp>
#include <crpc/net/ip.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace net {

// Tunables for one UDP socket.
struct udp_options {
    // Whether `SO_BROADCAST` is enabled at bind time.
    bool enable_broadcast = false;
    std::size_t recv_buffer_bytes = 64 * 1024;
    std::size_t send_buffer_bytes = 64 * 1024;
};

// One received datagram plus where it came from.
struct udp_datagram {
    std::size_t bytes = 0;
    address source;  // a TCP-form address holding the sender's literal + port
};

// The socket the factory hands out.
class udp_socket;

namespace detail {
// Internal bridge; see the channel header for the rationale.
struct udp_factory {
    [[nodiscard]] static std::unique_ptr<udp_socket> adopt(std::uintptr_t native,
                                                           address local,
                                                           udp_options options);
};
} // namespace detail

class CRPC_API udp_socket {
public:
    // Binds `bind_address` (a TCP-form address; a host literal and port, port 0
    // = ephemeral). Unix paths are not valid for UDP.
    [[nodiscard]] static result<std::unique_ptr<udp_socket>> bind(
        const address& bind_address, const udp_options& options = udp_options{});

    ~udp_socket();

    udp_socket(const udp_socket&) = delete;
    udp_socket& operator=(const udp_socket&) = delete;
    udp_socket(udp_socket&&) = delete;
    udp_socket& operator=(udp_socket&&) = delete;

    // Sends one datagram. `dest` is a TCP-form address (literal + port).
    // `timeout` only bounds the wait for buffer room, not delivery.
    [[nodiscard]] status send_to(span<const std::byte> data, const address& dest,
                                 std::chrono::nanoseconds timeout);

    // Receives one datagram. Fails `invalid_argument` when the buffer cannot
    // hold it (see the file comment), `deadline_exceeded` on timeout.
    [[nodiscard]] result<udp_datagram> recv_from(span<std::byte> buffer,
                                                 std::chrono::nanoseconds timeout);

    [[nodiscard]] status set_broadcast(bool enable);

    // Joins/leaves a multicast group (IPv4 or IPv6 literal) on the given local
    // interface. `interface_index` 0 selects the OS default.
    [[nodiscard]] status join_multicast(const ip_address& group,
                                        std::uint32_t interface_index = 0);
    [[nodiscard]] status leave_multicast(const ip_address& group,
                                         std::uint32_t interface_index = 0);

    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] address local() const;
    [[nodiscard]] const udp_options& options() const noexcept;

private:
    explicit udp_socket(std::uintptr_t native, address local, udp_options options);
    friend struct detail::udp_factory;

    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace net
} // namespace crpc

#endif // CRPC_NET_UDP_HPP