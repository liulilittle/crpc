// CRPC -- ICMP echo (ping) with round-trip statistics.
//
// The platform matrix, the DGRAM identifier semantics and the IPv4-header
// question are documented in the public header; this file implements them
// and keeps every OS difference behind one #if chain per decision.
#include <crpc/net/icmp.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "platform.hpp"

#if defined(CRPC_OS_WINDOWS)
#include <process.h>
#endif

namespace crpc {
namespace net {
namespace {

using detail::native_socket;

// --- Wire format (RFC 792) ---------------------------------------------------
//
// The 8-byte ICMP echo header this file builds and parses by hand: type,
// code, checksum, identifier, sequence number, then the caller's payload.
// Multi-byte fields are network byte order; the payload is opaque.
constexpr std::uint8_t k_icmp_echo_reply_type = 0;
constexpr std::uint8_t k_icmp_echo_request_type = 8;
constexpr std::uint8_t k_icmp_echo_code = 0;
constexpr std::size_t k_icmp_header_bytes = 8;
constexpr std::size_t k_max_ipv4_header_bytes = 60;
// 65535 total IPv4 length minus the 20-byte minimum IP header and the
// 8-byte ICMP header.
constexpr std::size_t k_max_echo_payload_bytes = 65507;

// One's-complement sum of big-endian 16-bit words, folding the trailing odd
// byte as a high-order word (RFC 1071). Generation complements this value;
// verification expects the sum over a complete frame to be 0xffff.
std::uint32_t checksum_sum(const std::byte* data, std::size_t length) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    std::uint32_t sum = 0;
    std::size_t i = 0;
    while (i + 1 < length) {
        sum += (static_cast<std::uint32_t>(bytes[i]) << 8) | bytes[i + 1];
        i += 2;
    }
    if (i < length) {
        sum += static_cast<std::uint32_t>(bytes[i]) << 8;
    }
    while (sum > 0xffffu) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return sum;
}

std::uint16_t checksum_word(const std::byte* data, std::size_t length) noexcept {
    return static_cast<std::uint16_t>(~checksum_sum(data, length) & 0xffffu);
}

std::uint16_t read_be16(const std::byte* data) noexcept {
    const auto* bytes = reinterpret_cast<const unsigned char*>(data);
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[0]) << 8) | bytes[1]);
}

std::uint16_t to_wire_u16(std::uint16_t value) noexcept {
    return static_cast<std::uint16_t>(((value & 0xffu) << 8) | (value >> 8));
}

std::vector<std::byte> build_request(bool kernel_identifier,
                                     std::uint16_t identifier,
                                     std::uint16_t sequence,
                                     std::size_t payload_bytes) {
    std::vector<std::byte> packet(k_icmp_header_bytes + payload_bytes,
                                  std::byte{0});
    packet[0] = std::byte{k_icmp_echo_request_type};
    packet[1] = std::byte{k_icmp_echo_code};
    // DGRAM ping socket: the kernel owns the identifier -- it replaces this
    // field with the socket's local port and recomputes the checksum, so 0
    // is written here deliberately. RAW: the caller's (or generated) id.
    const std::uint16_t id_wire =
        kernel_identifier ? std::uint16_t{0} : to_wire_u16(identifier);
    const std::uint16_t seq_wire = to_wire_u16(sequence);
    std::memcpy(packet.data() + 4, &id_wire, sizeof(id_wire));
    std::memcpy(packet.data() + 6, &seq_wire, sizeof(seq_wire));
    for (std::size_t i = 0; i < payload_bytes; ++i) {
        packet[k_icmp_header_bytes + i] = static_cast<std::byte>(i & 0xffu);
    }
    const std::uint16_t sum_wire =
        to_wire_u16(checksum_word(packet.data(), packet.size()));
    std::memcpy(packet.data() + 2, &sum_wire, sizeof(sum_wire));
    return packet;
}

struct parsed_reply {
    std::size_t icmp_offset = 0;
    std::size_t icmp_bytes = 0;
    std::uint8_t type = 0;
    std::uint8_t code = 0;
    std::uint16_t identifier = 0;
    std::uint16_t sequence = 0;
    std::uint8_t ttl = 0;
    bool checksum_ok = true;
};

// Decodes one received datagram. `strip_ip_header` says whether the buffer
// starts with an IPv4 header (RAW everywhere, DGRAM on macOS) or with the
// ICMP frame itself (DGRAM on Linux/Android). IPv4 version and IHL are
// validated; anything the parser cannot decode is not an echo reply.
bool parse_reply(const std::byte* data, std::size_t bytes, bool strip_ip_header,
                 parsed_reply& out) noexcept {
    std::size_t offset = 0;
    std::uint8_t ttl = 0;
    if (strip_ip_header) {
        if (bytes < 20) {
            return false;
        }
        const auto* raw = reinterpret_cast<const unsigned char*>(data);
        if ((raw[0] >> 4) != 4) {
            return false;
        }
        const std::size_t ihl = static_cast<std::size_t>(raw[0] & 0x0fu) * 4;
        if (ihl < 20 || ihl > k_max_ipv4_header_bytes ||
            bytes < ihl + k_icmp_header_bytes) {
            return false;
        }
        offset = ihl;
        ttl = raw[8];
    }
    if (bytes < offset + k_icmp_header_bytes) {
        return false;
    }
    out.icmp_offset = offset;
    out.icmp_bytes = bytes - offset;
    out.type = static_cast<std::uint8_t>(data[offset]);
    out.code = static_cast<std::uint8_t>(data[offset + 1]);
    out.identifier = read_be16(data + offset + 4);
    out.sequence = read_be16(data + offset + 6);
    out.ttl = ttl;
    // The kernel validates the checksum before feeding a DGRAM ping socket
    // (Linux icmp_rcv -> ping_rcv), so only frames that still carry the
    // IPv4 header are re-verified here.
    out.checksum_ok =
        !strip_ip_header || checksum_sum(data + offset, out.icmp_bytes) == 0xffffu;
    return true;
}

// Which receive path must skip an IPv4 header before the ICMP frame:
//   * RAW, on every platform: the kernel hands the socket the complete IPv4
//     packet (Linux raw(7); 4.4BSD/macOS raw sockets; Windows raw sockets).
//   * DGRAM ping socket on Linux/Android: the kernel strips the IPv4 header
//     and delivers the ICMP frame first (net/ipv4/ping.c ping_rcv).
//   * DGRAM on macOS: the 4.4BSD DGRAM ICMP socket delivers the IPv4 header
//     in front of the ICMP frame -- the opposite of Android.
// The decision is a compile-time platform check combined with the runtime
// mode on purpose: sniffing "is the first byte 0x45" is not a parser (an
// IHL of 69 is representable, and so is any payload byte), and a guessed
// offset would mis-parse attacker-crafted frames.
bool strips_ip_header(icmp_mode mode) noexcept {
#if defined(CRPC_OS_MACOS)
    (void)mode;
    return true;
#else
    return mode == icmp_mode::raw;
#endif
}

rpc_code classify_socket_creation(int error) noexcept {
#if defined(CRPC_OS_WINDOWS)
    switch (error) {
    case WSAEACCES:
        return rpc_code::permission_denied;
    case WSAEAFNOSUPPORT:
    case WSAEPROTONOSUPPORT:
        return rpc_code::unavailable;
    case WSAEMFILE:
    case WSAENOBUFS:
        return rpc_code::resource_exhausted;
    default:
        return rpc_code::internal;
    }
#else
    switch (error) {
    case EACCES:
    case EPERM:
        return rpc_code::permission_denied;
    case EAFNOSUPPORT:
    case EPFNOSUPPORT:
    case EPROTONOSUPPORT:
        return rpc_code::unavailable;
    case EMFILE:
    case ENFILE:
    case ENOBUFS:
    case ENOMEM:
        return rpc_code::resource_exhausted;
    default:
        return rpc_code::internal;
    }
#endif
}

// RAW identifiers are the caller's problem (the kernel does not filter raw
// replies), so two sockets in one process must not collide: mix the process
// id with a process-wide counter.
std::uint16_t generate_raw_identifier() noexcept {
    static std::atomic<std::uint32_t> counter{0};
#if defined(CRPC_OS_WINDOWS)
    const std::uint32_t pid = static_cast<std::uint32_t>(::_getpid());
#else
    const std::uint32_t pid = static_cast<std::uint32_t>(::getpid());
#endif
    std::uint32_t mixed =
        pid * 0x9E3779B9u +
        counter.fetch_add(1, std::memory_order_relaxed) * 0x85EBCA6Bu;
    mixed ^= mixed >> 15;
    mixed *= 0x2545F491u;
    mixed ^= mixed >> 13;
    const std::uint16_t identifier = static_cast<std::uint16_t>(mixed);
    return identifier == 0 ? std::uint16_t{1} : identifier;
}

std::uint16_t effective_raw_identifier(const icmp_options& options) noexcept {
    return options.identifier != 0 ? options.identifier
                                   : generate_raw_identifier();
}

bool set_ttl(native_socket s, std::uint8_t ttl) noexcept {
    const int value = static_cast<int>(ttl);
#if defined(CRPC_OS_WINDOWS)
    return ::setsockopt(s, IPPROTO_IP, IP_TTL,
                        reinterpret_cast<const char*>(&value),
                        sizeof(value)) == 0;
#else
    return ::setsockopt(s, IPPROTO_IP, IP_TTL, &value, sizeof(value)) == 0;
#endif
}

ip_address source_address(const sockaddr_storage& ss) noexcept {
    std::string host;
    u16 port = 0;
    if (detail::sockaddr_to_host(ss, host, port)) {
        result<ip_address> parsed = ip_address::parse(host);
        if (parsed.ok()) {
            return parsed.value();
        }
    }
    return ip_address{};
}

// A raw socket sees the host's whole ICMP traffic; a DGRAM socket is
// kernel-filtered by identifier. Either way the reply must come from the
// address that was probed.
bool source_matches(const sockaddr_storage& ss, const ip_address& target) noexcept {
    if (ss.ss_family != AF_INET) {
        return false;
    }
    const auto* a = reinterpret_cast<const sockaddr_in*>(&ss);
    std::array<unsigned char, 4> addr{};
    std::memcpy(addr.data(), &a->sin_addr, addr.size());
    const std::array<std::uint8_t, 16>& raw = target.bytes();
    return std::equal(addr.begin(), addr.end(), raw.begin());
}

// Windows surfaces the async ICMP failures of earlier sends as error codes
// on a later receive; the ICMP error itself also arrives as a plain packet
// with type != 0. Neither is a receive failure, so these codes drop the
// call and keep waiting instead of aborting the echo. POSIX (with an
// unconnected socket) never reports these, so the helper exists only there.
#if defined(CRPC_OS_WINDOWS)
bool recv_error_is_transient(int error) noexcept {
    return error == WSAEHOSTUNREACH || error == WSAENETUNREACH ||
           error == WSAECONNRESET || error == WSAENETRESET;
}
#endif

constexpr std::size_t recv_capacity(std::size_t payload_bytes) noexcept {
    return k_icmp_header_bytes + payload_bytes + k_max_ipv4_header_bytes;
}

// A matched reply, ready to become the public icmp_echo_reply.
struct matched_reply {
    ip_address source;
    std::uint16_t identifier = 0;
    std::uint16_t sequence = 0;
    std::uint8_t ttl = 0;
    std::size_t payload_bytes = 0;
    std::chrono::nanoseconds rtt{0};
};

// Bounded select wait: the deadline is re-checked at least every 250 ms,
// which also keeps every individual wait far inside timeval's range.
constexpr std::chrono::nanoseconds k_wait_slice{250000000};

} // namespace

// --- icmp_socket -------------------------------------------------------------

struct icmp_socket::impl {
    native_socket sock = detail::invalid_socket;
    bool open = true;
    icmp_mode effective_mode = icmp_mode::datagram;
    std::uint16_t identifier = 0;
    std::uint16_t next_sequence = 1;
    ip_address target;
    icmp_options options;
    sockaddr_storage destination{};
    int destination_length = 0;
    bool strip_header = false;
    std::vector<std::byte> recv_buffer;

    [[nodiscard]] rpc_error fail() const noexcept {
        if (sock == detail::invalid_socket || !open) {
            return rpc_error{rpc_code::shutting_down, "socket is closed"};
        }
        return rpc_error{rpc_code::internal, "socket is not ready"};
    }

    // Waits for the echo reply matching `sequence` (and, in RAW mode, the
    // socket's identifier) until `deadline`. Mismatched frames -- late
    // replies of earlier probes, other people's ICMP traffic a raw socket
    // also receives -- are discarded and the wait continues.
    [[nodiscard]] result<matched_reply> wait_reply(
        std::uint16_t sequence, std::chrono::steady_clock::time_point sent_at,
        std::chrono::steady_clock::time_point deadline);
};

result<matched_reply> icmp_socket::impl::wait_reply(
    std::uint16_t sequence, std::chrono::steady_clock::time_point sent_at,
    std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        const std::chrono::steady_clock::time_point now =
            std::chrono::steady_clock::now();
        if (now >= deadline) {
            return rpc_error{rpc_code::deadline_exceeded,
                             "no matching echo reply arrived in time"};
        }
        const std::chrono::nanoseconds remaining = deadline - now;
        const int ready =
            detail::wait_ready(sock, detail::wait_kind::read,
                               std::min(remaining, k_wait_slice));
        if (ready == 0) {
            continue;
        }
        if (ready < 0) {
            return rpc_error{rpc_code::internal, detail::error_text(
                                                     "waiting for echo reply failed",
                                                     detail::last_error())};
        }

        sockaddr_storage source{};
        std::uint8_t ancillary_ttl = 0;
        std::size_t received = 0;
#if defined(CRPC_OS_WINDOWS)
        int source_length = static_cast<int>(sizeof(source));
        const int capacity = static_cast<int>(
            std::min<std::size_t>(recv_buffer.size(),
                                  static_cast<std::size_t>(INT_MAX)));
        const int got =
            ::recvfrom(sock, reinterpret_cast<char*>(recv_buffer.data()),
                       capacity, 0, reinterpret_cast<sockaddr*>(&source),
                       &source_length);
        if (got < 0) {
            const int error = detail::last_error();
            if (detail::would_block(error) || recv_error_is_transient(error)) {
                continue;
            }
            return rpc_error{classify_stream_error(error),
                             detail::error_text("icmp recvfrom failed", error)};
        }
        received = static_cast<std::size_t>(got);
#else
        iovec iov{};
        iov.iov_base = recv_buffer.data();
        iov.iov_len = recv_buffer.size();
        msghdr message{};
        message.msg_name = &source;
        message.msg_namelen = sizeof(source);
        unsigned char control[64];
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        const ssize_t got = ::recvmsg(sock, &message, 0);
        if (got < 0) {
            const int error = detail::last_error();
            if (detail::would_block(error)) {
                continue;
            }
            return rpc_error{classify_stream_error(error),
                             detail::error_text("icmp recvmsg failed", error)};
        }
        received = static_cast<std::size_t>(got);
        // IP_RECVTTL delivers the reply's TTL as an ancillary int; the DGRAM
        // ping socket carries no IPv4 header to read it from. Absent support
        // leaves `ttl` at 0 (documented in the public header).
        for (cmsghdr* entry = CMSG_FIRSTHDR(&message); entry != nullptr;
             entry = CMSG_NXTHDR(&message, entry)) {
            if (entry->cmsg_level == IPPROTO_IP && entry->cmsg_type == IP_TTL) {
                int value = 0;
                std::memcpy(&value, CMSG_DATA(entry), sizeof(value));
                ancillary_ttl = static_cast<std::uint8_t>(value);
            }
        }
#endif

        parsed_reply reply{};
        if (!parse_reply(recv_buffer.data(), received, strip_header, reply)) {
            continue;
        }
        if (!reply.checksum_ok) {
            continue;
        }
        if (reply.type != k_icmp_echo_reply_type ||
            reply.code != k_icmp_echo_code) {
            continue;  // another ICMP packet (error, request, ...) -- drop it
        }
        // DGRAM: the kernel only delivers replies carrying this socket's
        // identifier, so the sequence number is what must match here. RAW:
        // the socket sees every ICMP packet the host receives, so both the
        // identifier and the sequence number must match.
        if (effective_mode == icmp_mode::raw && reply.identifier != identifier) {
            continue;
        }
        if (reply.sequence != sequence) {
            continue;
        }
        if (!source_matches(source, target)) {
            continue;
        }

        matched_reply out;
        out.source = source_address(source);
        out.identifier = reply.identifier;
        out.sequence = reply.sequence;
        out.ttl = strip_header ? reply.ttl : ancillary_ttl;
        out.payload_bytes = reply.icmp_bytes - k_icmp_header_bytes;
        out.rtt = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - sent_at);
        return out;
    }
}

icmp_socket::icmp_socket() : impl_(new impl) {}

icmp_socket::~icmp_socket() { (void)close(); }

result<std::unique_ptr<icmp_socket>> icmp_socket::open(const ip_address& target,
                                                       const icmp_options& options) {
    detail::ensure_sockets_initialized();
    if (!target.valid()) {
        return rpc_error{rpc_code::invalid_argument,
                         "ICMP target is not a valid address"};
    }
    if (!target.is_v4()) {
        return rpc_error{rpc_code::invalid_argument,
                         "ICMP echo supports IPv4 targets only (ICMPv6 is "
                         "not implemented)"};
    }
    if (options.payload_bytes > k_max_echo_payload_bytes) {
        return rpc_error{rpc_code::invalid_argument,
                         "echo payload exceeds the IPv4 datagram limit"};
    }
#if defined(__ANDROID__)
    if (options.mode == icmp_mode::raw) {
        return rpc_error{
            rpc_code::failed_precondition,
            "Android does not allow unprivileged applications to create raw "
            "ICMP sockets (SOCK_RAW IPPROTO_ICMP); use icmp_mode::datagram "
            "(SOCK_DGRAM IPPROTO_ICMP) or icmp_mode::automatic"};
    }
#endif

    sockaddr_storage destination{};
    int destination_length = 0;
    (void)detail::ip_to_sockaddr(target, 0, destination, destination_length);

    struct attempt {
        bool tried = false;
        rpc_code code = rpc_code::ok;
        std::string message;
    };
    attempt dgram_outcome;
    attempt raw_outcome;

    auto make_datagram = [&target, &options, &destination, &destination_length](
                             attempt& outcome) -> std::unique_ptr<icmp_socket> {
        const native_socket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
        if (s == detail::invalid_socket) {
            const int error = detail::last_error();
            std::string message =
                "SOCK_DGRAM IPPROTO_ICMP socket() failed: " +
                detail::error_text("ping socket", error);
#if defined(CRPC_OS_WINDOWS)
            // Unreachable here (Windows rejects the mode up front); kept for
            // completeness of the shared error path.
            (void)message;
#elif defined(__ANDROID__)
            if (error == EACCES || error == EPERM) {
                message +=
                    "; Android provides ICMP only through the unprivileged "
                    "DGRAM ping socket, so this failure is a kernel policy "
                    "problem rather than a missing privilege";
            }
#else
            if (error == EACCES || error == EPERM) {
                message +=
                    "; the unprivileged DGRAM ping socket requires the "
                    "caller's group to be inside net.ipv4.ping_group_range "
                    "(check: cat /proc/sys/net/ipv4/ping_group_range; open: "
                    "echo 0 2147483647 > /proc/sys/net/ipv4/ping_group_range)";
            }
#endif
            outcome = attempt{true, classify_socket_creation(error),
                              std::move(message)};
            return nullptr;
        }
#if !defined(CRPC_OS_WINDOWS)
        // Deliver the reply's TTL as an ancillary message; the DGRAM ping
        // socket carries no IPv4 header to read it from. Best effort: a
        // kernel without IP_RECVTTL support reports ttl 0 (documented).
#ifdef IP_RECVTTL
        {
            const int on = 1;
            (void)::setsockopt(s, IPPROTO_IP, IP_RECVTTL, &on, sizeof(on));
        }
#endif
#endif
        // Best effort as well: some kernels reject IP_TTL on ping sockets.
        (void)set_ttl(s, options.ttl);

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = 0;  // the kernel picks the identifier...
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) !=
            0) {
            const int error = detail::last_error();
            detail::close_socket(s);
            outcome = attempt{true, classify_socket_creation(error),
                              "binding the ping socket failed: " +
                                  detail::error_text("bind", error)};
            return nullptr;
        }
        // ...and reports it as this socket's local port: on a DGRAM ping
        // socket the ICMP identifier IS the local port number.
        sockaddr_storage bound{};
        int bound_length = 0;
        if (!detail::get_local_addr(s, bound, bound_length) ||
            bound.ss_family != AF_INET) {
            detail::close_socket(s);
            outcome = attempt{true, rpc_code::internal,
                              "reading the kernel-assigned ping identifier "
                              "back failed (getsockname)"};
            return nullptr;
        }
        const std::uint16_t assigned =
            ntohs(reinterpret_cast<const sockaddr_in*>(&bound)->sin_port);
        if (assigned == 0) {
            detail::close_socket(s);
            outcome = attempt{true, rpc_code::internal,
                              "the kernel assigned ping identifier 0"};
            return nullptr;
        }
        (void)detail::set_nonblocking(s, true);

        std::unique_ptr<icmp_socket> created(new icmp_socket());
        impl& state = *created->impl_;
        state.sock = s;
        state.effective_mode = icmp_mode::datagram;
        state.identifier = assigned;
        state.target = target;
        state.options = options;
        state.destination = destination;
        state.destination_length = destination_length;
        state.strip_header = strips_ip_header(icmp_mode::datagram);
        state.recv_buffer.assign(recv_capacity(options.payload_bytes),
                                 std::byte{0});
        return created;
    };

    auto make_raw = [&target, &options, &destination, &destination_length](
                        attempt& outcome) -> std::unique_ptr<icmp_socket> {
        const native_socket s = ::socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
        if (s == detail::invalid_socket) {
            const int error = detail::last_error();
            std::string message = "SOCK_RAW IPPROTO_ICMP socket() failed: " +
                                  detail::error_text("raw socket", error);
#if defined(CRPC_OS_WINDOWS)
            if (error == WSAEACCES) {
                message =
                    "creating a raw ICMP socket requires an administrator "
                    "token (WSAEACCES); run elevated, or use a platform with "
                    "unprivileged DGRAM ping sockets";
            }
#elif defined(__ANDROID__)
            if (error == EACCES || error == EPERM) {
                message =
                    "Android refused the raw ICMP socket (unprivileged "
                    "applications may not create SOCK_RAW IPPROTO_ICMP); use "
                    "icmp_mode::datagram";
            }
#else
            if (error == EACCES || error == EPERM) {
                message +=
                    "; a raw ICMP socket needs root or CAP_NET_RAW (the "
                    "unprivileged DGRAM ping socket is governed by "
                    "net.ipv4.ping_group_range)";
            }
#endif
            outcome = attempt{true, classify_socket_creation(error),
                              std::move(message)};
            return nullptr;
        }
        if (!set_ttl(s, options.ttl)) {
            const int error = detail::last_error();
            detail::close_socket(s);
            outcome = attempt{true, rpc_code::internal,
                              detail::error_text("setting IP_TTL failed", error)};
            return nullptr;
        }
        (void)detail::set_nonblocking(s, true);

        std::unique_ptr<icmp_socket> created(new icmp_socket());
        impl& state = *created->impl_;
        state.sock = s;
        state.effective_mode = icmp_mode::raw;
        state.identifier = effective_raw_identifier(options);
        state.target = target;
        state.options = options;
        state.destination = destination;
        state.destination_length = destination_length;
        state.strip_header = strips_ip_header(icmp_mode::raw);
        state.recv_buffer.assign(recv_capacity(options.payload_bytes),
                                 std::byte{0});
        return created;
    };

    bool try_dgram = false;
    bool try_raw = false;
    switch (options.mode) {
    case icmp_mode::automatic:
#if defined(CRPC_OS_WINDOWS)
        try_raw = true;
        break;
#elif defined(__ANDROID__)
        try_dgram = true;
        break;
#else
        try_dgram = true;
        try_raw = true;
        break;
#endif
    case icmp_mode::datagram:
#if defined(CRPC_OS_WINDOWS)
        return rpc_error{rpc_code::failed_precondition,
                         "Windows provides ICMP only through raw sockets; "
                         "icmp_mode::datagram is not available"};
#else
        try_dgram = true;
        break;
#endif
    case icmp_mode::raw:
        try_raw = true;
        break;
    }

    std::unique_ptr<icmp_socket> socket;
    if (try_dgram) {
        socket = make_datagram(dgram_outcome);
    }
    if (!socket && try_raw) {
        socket = make_raw(raw_outcome);
    }
    if (socket) {
        return socket;
    }

    if (try_dgram && try_raw) {
        std::string message =
            "creating an ICMP socket failed in both modes; DGRAM (SOCK_DGRAM "
            "IPPROTO_ICMP): " +
            dgram_outcome.message + "; RAW (SOCK_RAW IPPROTO_ICMP): " +
            raw_outcome.message +
            "; on Linux the unprivileged DGRAM ping socket needs the "
            "caller's group inside net.ipv4.ping_group_range (check: cat "
            "/proc/sys/net/ipv4/ping_group_range; open: echo 0 2147483647 > "
            "/proc/sys/net/ipv4/ping_group_range) and RAW needs root or "
            "CAP_NET_RAW; on macOS DGRAM ICMP normally needs no privileges";
        return rpc_error{rpc_code::permission_denied, std::move(message)};
    }
    if (dgram_outcome.tried) {
        return rpc_error{dgram_outcome.code, std::move(dgram_outcome.message)};
    }
    return rpc_error{raw_outcome.code, std::move(raw_outcome.message)};
}

result<icmp_echo_reply> icmp_socket::echo(std::uint16_t sequence,
                                          std::chrono::nanoseconds timeout) {
    auto& self = *impl_;
    if (self.sock == detail::invalid_socket || !self.open) {
        return self.fail();
    }
    if (timeout < std::chrono::nanoseconds::zero()) {
        return rpc_error{rpc_code::invalid_argument, "negative echo timeout"};
    }
    const bool kernel_identifier = self.effective_mode == icmp_mode::datagram;
    const std::vector<std::byte> request =
        build_request(kernel_identifier, self.identifier, sequence,
                      self.options.payload_bytes);

    const std::chrono::steady_clock::time_point sent_at =
        std::chrono::steady_clock::now();
    for (;;) {
        const int ready =
            detail::wait_ready(self.sock, detail::wait_kind::write, timeout);
        if (ready == 0) {
            return rpc_error{rpc_code::deadline_exceeded, "echo send timed out"};
        }
        if (ready < 0) {
            return rpc_error{rpc_code::internal,
                             detail::error_text("waiting to send echo failed",
                                                detail::last_error())};
        }
#if defined(CRPC_OS_WINDOWS)
        const int count =
            request.size() > static_cast<std::size_t>(INT_MAX)
                ? INT_MAX
                : static_cast<int>(request.size());
        const int pushed = ::sendto(
            self.sock, reinterpret_cast<const char*>(request.data()), count, 0,
            reinterpret_cast<sockaddr*>(&self.destination),
            self.destination_length);
#else
        const ssize_t pushed =
            ::sendto(self.sock, request.data(), request.size(), 0,
                     reinterpret_cast<sockaddr*>(&self.destination),
                     static_cast<socklen_t>(self.destination_length));
#endif
        if (pushed >= 0) {
            break;
        }
        const int error = detail::last_error();
        if (detail::would_block(error)) {
            continue;
        }
        return rpc_error{classify_stream_error(error),
                         detail::error_text("icmp sendto failed", error)};
    }

    result<matched_reply> matched =
        self.wait_reply(sequence, sent_at, sent_at + timeout);
    if (!matched) {
        return matched.error();
    }
    icmp_echo_reply reply;
    reply.source = matched.value().source;
    reply.sequence = matched.value().sequence;
    reply.identifier = matched.value().identifier;
    reply.ttl = matched.value().ttl;
    reply.rtt = matched.value().rtt;
    reply.payload_bytes = matched.value().payload_bytes;
    return reply;
}

result<std::vector<icmp_ping_result>> icmp_socket::ping_sequence(
    std::uint16_t count, std::chrono::nanoseconds interval) {
    auto& self = *impl_;
    if (self.sock == detail::invalid_socket || !self.open) {
        return self.fail();
    }
    std::vector<icmp_ping_result> attempts;
    attempts.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::uint16_t sequence = self.next_sequence++;
        icmp_ping_result attempt;
        attempt.sequence = sequence;
        result<icmp_echo_reply> reply = echo(sequence, self.options.timeout);
        if (reply.ok()) {
            attempt.replied = true;
            attempt.rtt = reply.value().rtt;
        } else {
            attempt.error = reply.code();
            attempt.error_message = reply.error().message;
        }
        attempts.push_back(std::move(attempt));
        if (i + 1 < count && interval > std::chrono::nanoseconds::zero()) {
            std::this_thread::sleep_for(interval);
        }
    }
    return attempts;
}

result<icmp_ping_statistics> icmp_socket::ping(
    std::uint16_t count, std::chrono::nanoseconds interval) {
    result<std::vector<icmp_ping_result>> attempts =
        ping_sequence(count, interval);
    if (!attempts) {
        return attempts.error();
    }
    icmp_ping_statistics stats;
    stats.sent = count;
    std::chrono::nanoseconds total{0};
    std::chrono::nanoseconds previous{0};
    std::chrono::nanoseconds variation_total{0};
    std::uint32_t variation_samples = 0;
    for (const icmp_ping_result& attempt : attempts.value()) {
        if (!attempt.replied) {
            continue;
        }
        if (stats.received == 0 || attempt.rtt < stats.min_rtt) {
            stats.min_rtt = attempt.rtt;
        }
        if (attempt.rtt > stats.max_rtt) {
            stats.max_rtt = attempt.rtt;
        }
        if (stats.received > 0) {
            // Jitter input: the mean absolute difference of consecutive
            // successfully-measured RTTs (see the header's comment).
            const std::chrono::nanoseconds delta =
                attempt.rtt >= previous ? attempt.rtt - previous
                                        : previous - attempt.rtt;
            variation_total += delta;
            ++variation_samples;
        }
        previous = attempt.rtt;
        total += attempt.rtt;
        ++stats.received;
    }
    stats.lost = stats.sent - stats.received;
    if (stats.received > 0) {
        stats.avg_rtt = total / static_cast<std::int64_t>(stats.received);
    }
    if (variation_samples > 0) {
        stats.jitter =
            variation_total / static_cast<std::int64_t>(variation_samples);
    }
    return stats;
}

status icmp_socket::close() {
    auto& self = *impl_;
    if (self.sock != detail::invalid_socket) {
        detail::close_socket(self.sock);
        self.sock = detail::invalid_socket;
    }
    self.open = false;
    return status{};
}

bool icmp_socket::is_open() const noexcept {
    const auto& self = *impl_;
    return self.sock != detail::invalid_socket && self.open;
}

icmp_mode icmp_socket::mode() const noexcept { return impl_->effective_mode; }

std::uint16_t icmp_socket::identifier() const noexcept { return impl_->identifier; }

const ip_address& icmp_socket::target() const noexcept { return impl_->target; }

const icmp_options& icmp_socket::options() const noexcept { return impl_->options; }

} // namespace net
} // namespace crpc
