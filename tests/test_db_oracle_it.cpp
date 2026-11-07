// CRPC -- Oracle TNS integration against a real Oracle listener.
//
// Skipped unless CRPC_ORACLE_IT=1. These cases exercise the connection layer
// only (CONNECT/ACCEPT/REFUSE); TTC authentication is deliberately absent from
// the codec and is not attempted here.
//
// Connection details come from the environment:
//   CRPC_ORACLE_IT=1
//   CRPC_ORACLE_HOST (default 127.0.0.1)
//   CRPC_ORACLE_PORT (default 1521)
//   CRPC_ORACLE_SERVICE (default FREE)
//   CRPC_ORACLE_USER / CRPC_ORACLE_PASSWORD (recorded only; no login is sent)
//
// The field assertions are pinned to the listener the codec was verified
// against: Oracle AI Database 26ai Free, image
// container-registry.oracle.com/database/free:latest (image label version
// 23.26.0), on 2026-11-07. A different server version may legitimately answer
// with different numbers; that is a version difference, not a codec bug, and
// the values are printed before they are asserted.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "test_framework.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <crpc/db/oracle/tns.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

using crpc::db::oracle::tns_accept;
using crpc::db::oracle::tns_assemble;
using crpc::db::oracle::tns_connect;
using crpc::db::oracle::tns_header;
using crpc::db::oracle::tns_packet;
using crpc::db::oracle::tns_packet_type;
using crpc::db::oracle::tns_refuse;

[[nodiscard]] std::string env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::string(value) : std::string(fallback);
}

[[nodiscard]] bool it_enabled() {
    const char* value = std::getenv("CRPC_ORACLE_IT");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

struct oracle_endpoint {
    std::string host;
    std::uint16_t port = 1521;
    std::string service;
    std::string user;
    std::string password;
};

[[nodiscard]] std::uint16_t parse_port(const std::string& text) {
    const unsigned long parsed = std::strtoul(text.c_str(), nullptr, 10);
    if (parsed == 0 || parsed > 65535) {
        return 1521;
    }
    return static_cast<std::uint16_t>(parsed);
}

[[nodiscard]] oracle_endpoint read_endpoint() {
    oracle_endpoint endpoint;
    endpoint.host = env_or("CRPC_ORACLE_HOST", "127.0.0.1");
    endpoint.port = parse_port(env_or("CRPC_ORACLE_PORT", "1521"));
    endpoint.service = env_or("CRPC_ORACLE_SERVICE", "FREE");
    endpoint.user = env_or("CRPC_ORACLE_USER", "system");
    endpoint.password = env_or("CRPC_ORACLE_PASSWORD", "");
    return endpoint;
}

[[nodiscard]] oracle_endpoint require_server() {
    if (!it_enabled()) {
        crpc_test::skip("CRPC_ORACLE_IT is not 1");
    }
    oracle_endpoint endpoint = read_endpoint();
    // The password is intentionally never printed or sent: the codec has no
    // authentication, so it is only kept here to document the environment.
    std::printf("  endpoint host=%s port=%u service=%s user=%s password_set=%d\n",
                endpoint.host.c_str(), static_cast<unsigned>(endpoint.port),
                endpoint.service.c_str(), endpoint.user.c_str(),
                endpoint.password.empty() ? 0 : 1);
    return endpoint;
}

[[nodiscard]] std::string connect_descriptor(const oracle_endpoint& endpoint,
                                             const std::string& service) {
    return "(DESCRIPTION=(ADDRESS=(PROTOCOL=tcp)(HOST=" + endpoint.host +
           ")(PORT=" + std::to_string(endpoint.port) +
           "))(CONNECT_DATA=(SERVICE_NAME=" + service + ")))";
}

[[nodiscard]] crpc::span<const std::byte> view(const std::vector<std::byte>& data) noexcept {
    return crpc::span<const std::byte>(data.data(), data.size());
}

[[nodiscard]] std::string hex(crpc::span<const std::byte> data) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (std::byte b : data) {
        const auto value = static_cast<std::uint8_t>(b);
        out.push_back(digits[value >> 4]);
        out.push_back(digits[value & 0x0f]);
    }
    return out;
}

[[nodiscard]] bool any_nonzero(crpc::span<const std::byte> data) noexcept {
    for (std::byte b : data) {
        if (b != std::byte{0}) {
            return true;
        }
    }
    return false;
}

#if defined(_WIN32)
using socket_handle = SOCKET;
constexpr socket_handle invalid_socket = INVALID_SOCKET;
#else
using socket_handle = int;
constexpr socket_handle invalid_socket = -1;
#endif

class socket_runtime {
public:
    socket_runtime() {
#if defined(_WIN32)
        WSADATA data{};
        ok_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
        ok_ = true;
#endif
    }

    ~socket_runtime() {
#if defined(_WIN32)
        if (ok_) {
            (void)WSACleanup();
        }
#endif
    }

    socket_runtime(const socket_runtime&) = delete;
    socket_runtime& operator=(const socket_runtime&) = delete;

    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    bool ok_ = false;
};

enum class io_result { data, closed, timeout, failed };

class tcp_connection {
public:
    tcp_connection() = default;

    ~tcp_connection() { close(); }

    tcp_connection(const tcp_connection&) = delete;
    tcp_connection& operator=(const tcp_connection&) = delete;

    [[nodiscard]] bool open(const std::string& host, std::uint16_t port,
                            std::string& why) {
        static const socket_runtime runtime;
        if (!runtime.ok()) {
            why = "WSAStartup failed";
            return false;
        }

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        addrinfo* addresses = nullptr;
        const std::string port_text = std::to_string(port);
        const int resolved =
            ::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &addresses);
        if (resolved != 0) {
            why = "getaddrinfo failed (" + std::to_string(resolved) + ")";
            return false;
        }

        for (addrinfo* address = addresses; address != nullptr;
             address = address->ai_next) {
            const socket_handle candidate =
                ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
            if (candidate == invalid_socket) {
                continue;
            }
#if defined(_WIN32)
            const int connected =
                ::connect(candidate, address->ai_addr,
                          static_cast<int>(address->ai_addrlen));
#else
            const int connected =
                ::connect(candidate, address->ai_addr, address->ai_addrlen);
#endif
            if (connected == 0) {
                socket_ = candidate;
                ::freeaddrinfo(addresses);
                return true;
            }
            close_handle(candidate);
        }

        ::freeaddrinfo(addresses);
        why = "connect failed to " + host + ":" + port_text;
        return false;
    }

    [[nodiscard]] bool set_receive_timeout(unsigned milliseconds) {
        if (socket_ == invalid_socket) {
            return false;
        }
#if defined(_WIN32)
        const DWORD timeout = static_cast<DWORD>(milliseconds);
        return ::setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO,
                            reinterpret_cast<const char*>(&timeout),
                            static_cast<int>(sizeof(timeout))) == 0;
#else
        timeval timeout{};
        timeout.tv_sec = static_cast<time_t>(milliseconds / 1000u);
        timeout.tv_usec = static_cast<suseconds_t>((milliseconds % 1000u) * 1000u);
        return ::setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                            sizeof(timeout)) == 0;
#endif
    }

    [[nodiscard]] bool send_all(crpc::span<const std::byte> data, std::string& why) {
        std::size_t offset = 0;
        while (offset < data.size()) {
            const std::size_t remaining = data.size() - offset;
            const std::size_t chunk = std::min<std::size_t>(remaining, 1u << 20);
            const auto sent =
                ::send(socket_, reinterpret_cast<const char*>(data.data() + offset),
                       static_cast<int>(chunk), 0);
            if (sent <= 0) {
                why = "send failed (" + last_error_text() + ")";
                return false;
            }
            offset += static_cast<std::size_t>(sent);
        }
        return true;
    }

    [[nodiscard]] io_result receive(std::byte* out, std::size_t capacity,
                                    std::size_t& received, std::string& why) {
        received = 0;
        if (capacity == 0) {
            return io_result::data;
        }
        const auto result =
            ::recv(socket_, reinterpret_cast<char*>(out), static_cast<int>(capacity), 0);
        if (result > 0) {
            received = static_cast<std::size_t>(result);
            return io_result::data;
        }
        if (result == 0) {
            return io_result::closed;
        }
        const int error = last_error();
        if (is_timeout(error) || is_would_block(error)) {
            return io_result::timeout;
        }
        if (is_reset(error)) {
            return io_result::closed;
        }
        why = "recv failed (" + std::to_string(error) + ")";
        return io_result::failed;
    }

    void close() noexcept {
        if (socket_ != invalid_socket) {
            close_handle(socket_);
            socket_ = invalid_socket;
        }
    }

private:
    static void close_handle(socket_handle handle) noexcept {
#if defined(_WIN32)
        (void)::closesocket(handle);
#else
        (void)::close(handle);
#endif
    }

    static int last_error() noexcept {
#if defined(_WIN32)
        return WSAGetLastError();
#else
        return errno;
#endif
    }

    static std::string last_error_text() {
#if defined(_WIN32)
        return std::to_string(WSAGetLastError());
#else
        return std::string(std::strerror(errno));
#endif
    }

    static bool is_timeout(int error) noexcept {
#if defined(_WIN32)
        return error == WSAETIMEDOUT;
#else
        return error == ETIMEDOUT;
#endif
    }

    static bool is_would_block(int error) noexcept {
#if defined(_WIN32)
        return error == WSAEWOULDBLOCK;
#else
        return error == EAGAIN || error == EWOULDBLOCK;
#endif
    }

    static bool is_reset(int error) noexcept {
#if defined(_WIN32)
        return error == WSAECONNRESET || error == WSAECONNABORTED;
#else
        return error == ECONNRESET || error == ECONNABORTED || error == EPIPE;
#endif
    }

    socket_handle socket_ = invalid_socket;
};

enum class read_outcome { ok, closed, timeout, failed };

[[nodiscard]] read_outcome read_exact(tcp_connection& link, std::byte* out,
                                      std::size_t size, std::string& why) {
    std::size_t offset = 0;
    while (offset < size) {
        std::size_t received = 0;
        const io_result result = link.receive(out + offset, size - offset, received, why);
        if (result == io_result::data) {
            offset += received;
            continue;
        }
        if (result == io_result::closed) {
            why = "connection closed after " + std::to_string(offset) + " of " +
                  std::to_string(size) + " bytes";
            return read_outcome::closed;
        }
        if (result == io_result::timeout) {
            why = "timed out after " + std::to_string(offset) + " of " +
                  std::to_string(size) + " bytes";
            return read_outcome::timeout;
        }
        return read_outcome::failed;
    }
    return read_outcome::ok;
}

struct packet_reply {
    tns_header header;
    std::vector<std::byte> body;
    read_outcome outcome = read_outcome::failed;
    std::string why;

    [[nodiscard]] bool ok() const noexcept { return outcome == read_outcome::ok; }
};

// The listener answers CONNECT before any version has been negotiated, so its
// first packet uses the 16-bit length layout (the same rule the encoder follows
// for the outbound CONNECT).
[[nodiscard]] packet_reply read_packet(tcp_connection& link, unsigned timeout_ms) {
    packet_reply reply;
    if (!link.set_receive_timeout(timeout_ms)) {
        reply.why = "could not set the receive timeout";
        return reply;
    }

    std::array<std::byte, crpc::db::oracle::tns_header_size> head{};
    reply.outcome = read_exact(link, head.data(), head.size(), reply.why);
    if (reply.outcome != read_outcome::ok) {
        return reply;
    }

    const crpc::status decoded = crpc::db::oracle::decode_tns_header(
        crpc::span<const std::byte>(head.data(), head.size()), false, reply.header);
    if (!decoded.ok()) {
        reply.outcome = read_outcome::failed;
        reply.why = decoded.message();
        return reply;
    }
    if (reply.header.length < crpc::db::oracle::tns_header_size ||
        reply.header.length > crpc::db::oracle::tns_max_packet_length) {
        reply.outcome = read_outcome::failed;
        reply.why = "server declared an out-of-range packet length";
        return reply;
    }

    reply.body.resize(reply.header.length - crpc::db::oracle::tns_header_size);
    if (!reply.body.empty()) {
        reply.outcome = read_exact(link, reply.body.data(), reply.body.size(), reply.why);
    }
    return reply;
}

[[nodiscard]] std::vector<std::byte> encode_request(const oracle_endpoint& endpoint,
                                                    const std::string& service,
                                                    std::string& descriptor_out) {
    descriptor_out = connect_descriptor(endpoint, service);
    tns_connect request;
    request.connect_data = descriptor_out;
    std::vector<std::byte> encoded;
    const crpc::status encoded_status =
        crpc::db::oracle::encode_connect(encoded, request);
    if (!encoded_status.ok()) {
        std::printf("  encode_connect failed: %s\n", encoded_status.message().c_str());
    }
    return encoded;
}

[[nodiscard]] bool send_request(tcp_connection& link,
                                crpc::span<const std::byte> request,
                                const char* what) {
    std::string why;
    if (!link.send_all(request, why)) {
        std::printf("  %s send failed: %s\n", what, why.c_str());
        return false;
    }
    return true;
}

void print_packet(const char* label, const packet_reply& reply) {
    std::printf("  %s type=%d flags=%u length=%u body=%s\n", label,
                static_cast<int>(reply.header.type),
                static_cast<unsigned>(reply.header.flags),
                static_cast<unsigned>(reply.header.length),
                hex(view(reply.body)).c_str());
}

} // namespace

CRPC_TEST(oracle_it_accept_from_real_listener) {
    const oracle_endpoint endpoint = require_server();

    tcp_connection link;
    std::string why;
    CRPC_REQUIRE(link.open(endpoint.host, endpoint.port, why));

    std::string descriptor;
    const std::vector<std::byte> request =
        encode_request(endpoint, endpoint.service, descriptor);
    CRPC_REQUIRE(!request.empty());
    std::printf("  CONNECT %zu bytes descriptor=%s\n", request.size(),
                descriptor.c_str());
    CRPC_CHECK(send_request(link, view(request), "CONNECT"));

    const packet_reply reply = read_packet(link, 10000);
    if (!reply.ok()) {
        std::printf("  ACCEPT read failed: %s\n", reply.why.c_str());
    }
    CRPC_REQUIRE(reply.ok());
    print_packet("ACCEPT", reply);

    CRPC_CHECK_EQ(static_cast<int>(tns_packet_type::accept),
                  static_cast<int>(reply.header.type));
    // Observed from the 26ai Free listener: 8-byte small-layout header plus a
    // 53-byte body. The 37-byte documented prefix is followed by a 16-byte
    // server session value that the reference client ignores.
    CRPC_CHECK_EQ(std::uint32_t{61}, reply.header.length);
    CRPC_CHECK_EQ(std::size_t{53}, reply.body.size());
    CRPC_REQUIRE(reply.body.size() >= 37);

    tns_accept accept;
    const crpc::status decoded = crpc::db::oracle::decode_accept(view(reply.body), accept);
    if (!decoded.ok()) {
        std::printf("  decode_accept failed: %s\n", decoded.message().c_str());
    }
    CRPC_CHECK(decoded.ok());

    std::printf("  decoded version=%u options=0x%04x nsi=0x%02x sdu=%u flags2=0x%08x\n",
                static_cast<unsigned>(accept.version),
                static_cast<unsigned>(accept.options),
                static_cast<unsigned>(accept.nsi_flags), accept.sdu, accept.flags2);
    // These are the listener's own values, not copies of the request: the codec
    // reads them from the bytes the server sent.
    CRPC_CHECK_EQ(std::uint16_t{319}, accept.version);
    CRPC_CHECK_EQ(std::uint16_t{0x0001}, accept.options);
    CRPC_CHECK_EQ(std::uint8_t{0xc5}, accept.nsi_flags);
    CRPC_CHECK_EQ(std::uint32_t{8192}, accept.sdu);
    CRPC_CHECK_EQ(std::uint32_t{0x1a000000}, accept.flags2);
    CRPC_CHECK((accept.flags2 & crpc::db::oracle::tns_accept_flag_end_of_response) != 0);
    CRPC_CHECK((accept.flags2 & crpc::db::oracle::tns_accept_flag_fast_auth) != 0);

    // TNS ACCEPT carries no character-set field; the character set is negotiated
    // later in the TTC DataTypes message, which this codec deliberately does not
    // implement. What ACCEPT does carry is the NSI negotiation byte asserted
    // above.
    CRPC_CHECK_EQ(std::uint32_t{8192}, accept.sdu);

    // The listener appended a non-zero 16-byte session value after the
    // documented fields. verify it is present and not all zero.
    const crpc::span<const std::byte> session(reply.body.data() + 37, 16);
    std::printf("  server session=%s\n", hex(session).c_str());
    CRPC_CHECK(any_nonzero(session));
}

CRPC_TEST(oracle_it_refuse_unregistered_service) {
    const oracle_endpoint endpoint = require_server();
    const std::string missing = "CRPC_IT_NO_SUCH_SERVICE_7F3A";

    tcp_connection link;
    std::string why;
    CRPC_REQUIRE(link.open(endpoint.host, endpoint.port, why));

    std::string descriptor;
    const std::vector<std::byte> request = encode_request(endpoint, missing, descriptor);
    CRPC_REQUIRE(!request.empty());
    std::printf("  CONNECT %zu bytes service=%s\n", request.size(), missing.c_str());
    CRPC_CHECK(send_request(link, view(request), "CONNECT"));

    const packet_reply reply = read_packet(link, 10000);
    if (!reply.ok()) {
        std::printf("  REFUSE read failed: %s\n", reply.why.c_str());
    }
    CRPC_REQUIRE(reply.ok());
    print_packet("REFUSE", reply);

    CRPC_CHECK_EQ(static_cast<int>(tns_packet_type::refuse),
                  static_cast<int>(reply.header.type));
    CRPC_REQUIRE(reply.body.size() >= 4);

    tns_refuse refuse;
    const crpc::status decoded = crpc::db::oracle::decode_refuse(view(reply.body), refuse);
    if (!decoded.ok()) {
        std::printf("  decode_refuse failed: %s\n", decoded.message().c_str());
    }
    CRPC_CHECK(decoded.ok());
    std::printf("  decoded code=%u message=%s\n", refuse.code, refuse.message.c_str());

    // The 26ai listener answers with
    // (DESCRIPTION=(TMP=)(VSNNUM=0)(ERR=12514)(ERROR_STACK=...)) and does not
    // echo the requested service name. Asserting the name would be inventing a
    // behaviour this listener does not have; the numeric code is the server's
    // actual refusal signal.
    CRPC_CHECK_EQ(std::uint32_t{12514}, refuse.code);
    CRPC_CHECK(refuse.message.find("(ERR=12514)") != std::string::npos);
    CRPC_CHECK(refuse.message.find("(CODE=12514)") != std::string::npos);
    CRPC_CHECK_FALSE(refuse.message.empty());
    // The requested name really was on the wire, but this listener's REFUSE is
    // the DESCRIPTION/ERROR_STACK form and does not echo it. Assert both facts
    // so the shape of the rejection is evidence rather than an assumption.
    CRPC_CHECK(descriptor.find(missing) != std::string::npos);
    CRPC_CHECK(refuse.message.find(missing) == std::string::npos);
}

CRPC_TEST(oracle_it_two_connections_report_the_same_session_field) {
    const oracle_endpoint endpoint = require_server();

    tcp_connection first;
    std::string why;
    CRPC_REQUIRE(first.open(endpoint.host, endpoint.port, why));

    std::string descriptor;
    const std::vector<std::byte> request =
        encode_request(endpoint, endpoint.service, descriptor);
    CRPC_REQUIRE(!request.empty());
    CRPC_CHECK(send_request(first, view(request), "first CONNECT"));
    const packet_reply first_reply = read_packet(first, 10000);
    if (!first_reply.ok()) {
        std::printf("  first ACCEPT read failed: %s\n", first_reply.why.c_str());
    }
    CRPC_REQUIRE(first_reply.ok());
    print_packet("first ACCEPT", first_reply);

    // The first socket stays open while the second connection is established:
    // both listeners' replies are consumed by their own socket, so a byte leak
    // between connections would desynchronise one of them.
    tcp_connection second;
    CRPC_REQUIRE(second.open(endpoint.host, endpoint.port, why));
    CRPC_CHECK(send_request(second, view(request), "second CONNECT"));
    const packet_reply second_reply = read_packet(second, 10000);
    if (!second_reply.ok()) {
        std::printf("  second ACCEPT read failed: %s\n", second_reply.why.c_str());
    }
    CRPC_REQUIRE(second_reply.ok());
    print_packet("second ACCEPT", second_reply);

    CRPC_CHECK_EQ(static_cast<int>(tns_packet_type::accept),
                  static_cast<int>(first_reply.header.type));
    CRPC_CHECK_EQ(static_cast<int>(tns_packet_type::accept),
                  static_cast<int>(second_reply.header.type));
    CRPC_REQUIRE(first_reply.body.size() >= 37);
    CRPC_REQUIRE(second_reply.body.size() >= 37);

    tns_accept first_accept;
    tns_accept second_accept;
    CRPC_CHECK(crpc::db::oracle::decode_accept(view(first_reply.body), first_accept).ok());
    CRPC_CHECK(crpc::db::oracle::decode_accept(view(second_reply.body), second_accept).ok());
    CRPC_CHECK_EQ(first_accept.version, second_accept.version);
    CRPC_CHECK_EQ(first_accept.sdu, second_accept.sdu);
    CRPC_CHECK_EQ(first_accept.options, second_accept.options);
    CRPC_CHECK_EQ(first_accept.nsi_flags, second_accept.nsi_flags);

    // Both replies must be complete to their declared length and carry the
    // same non-zero 16-byte session value; a short read or a packet from the
    // other connection would break one of these.
    CRPC_CHECK_EQ(first_reply.body.size(), second_reply.body.size());
    if (first_reply.body.size() >= 53 && second_reply.body.size() >= 53) {
        const crpc::span<const std::byte> first_session(first_reply.body.data() + 37, 16);
        const crpc::span<const std::byte> second_session(second_reply.body.data() + 37, 16);
        std::printf("  first session=%s\n  second session=%s\n",
                    hex(first_session).c_str(), hex(second_session).c_str());
        CRPC_CHECK(any_nonzero(first_session));
        CRPC_CHECK(any_nonzero(second_session));
        CRPC_CHECK(std::memcmp(first_session.data(), second_session.data(), 16) == 0);
    } else {
        CRPC_CHECK(false);
    }
}

CRPC_TEST(oracle_it_malformed_connect_is_closed_or_refused) {
    const oracle_endpoint endpoint = require_server();
    // A service name of 5000 bytes makes the descriptor longer than
    // tns_max_connect_data (230), so encode_connect emits the CONNECT packet
    // followed by a DATA packet. The listener must reject it without trusting
    // the declared sizes.
    const std::string oversized_service(5000, 'X');

    tcp_connection link;
    std::string why;
    CRPC_REQUIRE(link.open(endpoint.host, endpoint.port, why));

    std::string descriptor;
    const std::vector<std::byte> request =
        encode_request(endpoint, oversized_service, descriptor);
    CRPC_REQUIRE(!request.empty());
    std::printf("  malformed CONNECT %zu bytes\n", request.size());
    CRPC_CHECK(send_request(link, view(request), "malformed CONNECT"));

    const packet_reply reply = read_packet(link, 5000);
    if (reply.outcome == read_outcome::closed) {
        std::printf("  listener closed the connection: %s\n", reply.why.c_str());
    } else if (reply.ok()) {
        print_packet("malformed reply", reply);
    } else {
        std::printf("  malformed read outcome=%d: %s\n",
                    static_cast<int>(reply.outcome), reply.why.c_str());
    }

    // A rejection is either a clean close or a REFUSE packet. A timeout would
    // mean the listener neither rejected nor answered, and a parse failure would
    // mean our decoder choked on what it did send.
    CRPC_CHECK(reply.outcome == read_outcome::closed || reply.ok());
    if (reply.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(tns_packet_type::refuse),
                      static_cast<int>(reply.header.type));
        tns_refuse refuse;
        const crpc::status decoded =
            crpc::db::oracle::decode_refuse(view(reply.body), refuse);
        if (!decoded.ok()) {
            std::printf("  decode_refuse failed: %s\n", decoded.message().c_str());
        }
        CRPC_CHECK(decoded.ok());
        std::printf("  malformed decoded code=%u message=%s\n", refuse.code,
                    refuse.message.c_str());
    }

    // The same bytes fed to the assembler must not crash it: a complete REFUSE
    // is ready, a short/closed stream is need_more until more arrives.
    {
        crpc::db::oracle::tns_assembler assembler(false);
        if (reply.ok()) {
            std::array<std::byte, crpc::db::oracle::tns_header_size> head{};
            // Re-encode the header exactly as read: declared length fills the
            // 16-bit layout and the checksum is zero.
            head[0] = static_cast<std::byte>(reply.header.length >> 8);
            head[1] = static_cast<std::byte>(reply.header.length & 0xffu);
            head[2] = std::byte{0};
            head[3] = std::byte{0};
            head[4] = static_cast<std::byte>(
                static_cast<std::uint8_t>(reply.header.type));
            head[5] = static_cast<std::byte>(reply.header.flags);
            head[6] = std::byte{0};
            head[7] = std::byte{0};
            std::vector<std::byte> raw(head.begin(), head.end());
            raw.insert(raw.end(), reply.body.begin(), reply.body.end());
            CRPC_CHECK(assembler.feed(view(raw)).ok());
            tns_packet packet;
            const tns_assemble state = assembler.next_message(packet);
            CRPC_CHECK(state == tns_assemble::ready);
            CRPC_CHECK_EQ(static_cast<int>(tns_packet_type::refuse),
                          static_cast<int>(packet.type));
        } else {
            const std::array<std::byte, 3> truncated{std::byte{0}, std::byte{0},
                                                     std::byte{0}};
            CRPC_CHECK(assembler.feed(crpc::span<const std::byte>(
                           truncated.data(), truncated.size()))
                           .ok());
            tns_packet packet;
            CRPC_CHECK(assembler.next_message(packet) == tns_assemble::need_more);
        }
    }
}

CRPC_TEST_MAIN()
