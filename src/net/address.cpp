// CRPC -- network addresses, parsing, and name resolution with a deadline.
#include <crpc/net/address.hpp>

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "platform.hpp"

namespace crpc {
namespace net {
namespace {

// Every address is validated textually before a resolver, an OS call or a
// socket is touched. Control characters and NULs are always rejected: they can
// neither be a host name nor a filesystem path, and their presence means the
// caller fed in an untrusted blob instead of an address.
inline bool contains_forbidden(std::string_view text) noexcept {
    for (const char c : text) {
        if (static_cast<unsigned char>(c) < 0x20u || c == '\x7f') {
            return true;
        }
    }
    return false;
}

// Maps a `getaddrinfo` return into the failure vocabulary. `host` is the name
// that failed, for the message.
inline rpc_error classify_resolution(int code, const std::string& host) noexcept {
    const std::string message = "name resolution failed for " + host;
#if defined(CRPC_OS_WINDOWS)
    switch (code) {
    case WSAHOST_NOT_FOUND:
    case WSATRY_AGAIN:
    case WSANO_RECOVERY:
    case WSANO_DATA:
        return rpc_error(rpc_code::unavailable, message);
    case WSAEAFNOSUPPORT:
        return rpc_error(rpc_code::invalid_argument, message);
    case WSA_NOT_ENOUGH_MEMORY:
        return rpc_error(rpc_code::resource_exhausted, message);
    default:
        return rpc_error(rpc_code::internal, message);
    }
#else
    switch (code) {
    case EAI_NONAME:
    case EAI_NODATA:
    case EAI_FAIL:
        return rpc_error(rpc_code::unavailable, message);
    case EAI_AGAIN:
        return rpc_error(rpc_code::unavailable, message);
    case EAI_MEMORY:
        return rpc_error(rpc_code::resource_exhausted, message);
    case EAI_SYSTEM:
        return rpc_error(rpc_code::internal, message);
    default:
        return rpc_error(rpc_code::internal, message);
    }
#endif
}

} // namespace

// --- address ------------------------------------------------------------------

address address::tcp(std::string host, u16 port) {
    address result;
    result.kind_ = address_kind::tcp;
    result.host_ = std::move(host);
    result.port_ = port;
    return result;
}

address address::unix_path(std::string path) {
    address result;
    result.kind_ = address_kind::unix;
    result.path_ = std::move(path);
    return result;
}

std::string address::to_string() const {
    switch (kind_) {
    case address_kind::tcp:
        if (host_.find(':') != std::string::npos) {
            // An IPv6 literal needs brackets to be unambiguous next to a port.
            return "tcp:[" + host_ + "]:" + std::to_string(port_);
        }
        return "tcp:" + host_ + ":" + std::to_string(port_);
    case address_kind::unix:
        return "unix:" + path_;
    case address_kind::none:
        break;
    }
    return "none";
}

result<address> address::parse_tcp(std::string_view text) {
    if (text.empty()) {
        return rpc_error{rpc_code::invalid_argument, "empty TCP endpoint"};
    }
    if (contains_forbidden(text)) {
        return rpc_error{rpc_code::invalid_argument, "TCP endpoint contains a control character"};
    }
    std::string_view host;
    std::string_view port_text;
    if (text.front() == '[') {
        const std::size_t close = text.find(']');
        if (close == std::string_view::npos) {
            return rpc_error{rpc_code::invalid_argument, "unterminated '[' in TCP endpoint"};
        }
        if (close + 1 >= text.size() || text[close + 1] != ':') {
            return rpc_error{rpc_code::invalid_argument,
                             "expected ':port' after ']' in TCP endpoint"};
        }
        host = text.substr(1, close - 1);
        port_text = text.substr(close + 2);
    } else {
        const std::size_t colon = text.rfind(':');
        if (colon == std::string_view::npos) {
            return rpc_error{rpc_code::invalid_argument, "TCP endpoint has no port"};
        }
        host = text.substr(0, colon);
        port_text = text.substr(colon + 1);
        if (host.find(':') != std::string_view::npos) {
            return rpc_error{rpc_code::invalid_argument,
                             "bracket an IPv6 literal in a TCP endpoint"};
        }
    }
    if (host.empty()) {
        return rpc_error{rpc_code::invalid_argument, "TCP endpoint has no host"};
    }
    result<u16> port = parse_port(port_text);
    if (!port) {
        return port.error();
    }
    return address::tcp(std::string(host), port.value());
}

result<address> address::parse_unix(std::string_view text) {
    const status validated = validate_unix_path(text);
    if (!validated) {
        return rpc_error{validated.code(), validated.message()};
    }
    return address::unix_path(std::string(text));
}

// --- ports -------------------------------------------------------------------

result<u16> parse_port(std::string_view text) {
    if (text.empty()) {
        return rpc_error{rpc_code::invalid_argument, "empty port"};
    }
    unsigned value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return rpc_error{rpc_code::invalid_argument, "port is not a decimal number"};
        }
        value = value * 10u + static_cast<unsigned>(c - '0');
        if (value > 65535u) {
            return rpc_error{rpc_code::invalid_argument, "port out of range 1..65535"};
        }
    }
    if (value == 0) {
        // "0" requests an ephemeral port and is only meaningful for binding.
        return rpc_error{rpc_code::invalid_argument, "port 0 is not a connection target"};
    }
    return static_cast<u16>(value);
}

// --- resolution --------------------------------------------------------------

namespace {

struct resolve_job {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool cancelled = false;
    int gai_code = 0;
    std::vector<ip_address> addresses;
};

void run_resolve(std::shared_ptr<resolve_job> job, std::string host) noexcept {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result_list = nullptr;
    const int code = ::getaddrinfo(host.c_str(), nullptr, &hints, &result_list);
    std::vector<ip_address> addresses;
    if (code == 0 && result_list != nullptr) {
        for (const addrinfo* it = result_list; it != nullptr; it = it->ai_next) {
            if (it->ai_addrlen != 0 && it->ai_addr != nullptr) {
                sockaddr_storage storage{};
                const std::size_t copy = it->ai_addrlen < sizeof(storage)
                                             ? static_cast<std::size_t>(it->ai_addrlen)
                                             : sizeof(storage);
                std::memcpy(&storage, it->ai_addr, copy);
                std::string literal;
                u16 port_unused = 0;
                if (detail::sockaddr_to_host(storage, literal, port_unused)) {
                    result<ip_address> parsed = ip_address::parse(literal);
                    if (parsed) {
                        addresses.push_back(std::move(parsed.value()));
                    }
                }
            }
        }
        ::freeaddrinfo(result_list);
    }

    {
        std::lock_guard<std::mutex> lock(job->mutex);
        if (!job->cancelled) {
            job->gai_code = code;
            job->addresses = std::move(addresses);
        }
        job->done = true;
    }
    job->cv.notify_all();
}

} // namespace

result<std::vector<ip_address>> resolve_host(std::string_view host,
                                             const resolve_options& options) {
    if (host.empty()) {
        return rpc_error{rpc_code::invalid_argument, "empty host name"};
    }
    if (contains_forbidden(host)) {
        return rpc_error{rpc_code::invalid_argument, "host name contains a control character"};
    }

    // A numeric literal is answered without the resolver, so a connector never
    // waits on DNS for an address it already has.
    result<ip_address> literal = ip_address::parse(host);
    if (literal) {
        if (options.family == ip_family::ipv4 && !literal.value().is_v4()) {
            return rpc_error{rpc_code::unavailable,
                             "no address of the requested family"};
        }
        if (options.family == ip_family::ipv6 && !literal.value().is_v6()) {
            return rpc_error{rpc_code::unavailable,
                             "no address of the requested family"};
        }
        std::vector<ip_address> out;
        out.push_back(std::move(literal.value()));
        return out;
    }

    detail::ensure_sockets_initialized();

    std::shared_ptr<resolve_job> job = std::make_shared<resolve_job>();
    std::thread worker(run_resolve, job, std::string(host));
    worker.detach();

    std::unique_lock<std::mutex> lock(job->mutex);
    const bool finished = job->cv.wait_for(lock, options.timeout,
                                           [&job] { return job->done; });
    if (!finished) {
        job->cancelled = true;
        return rpc_error{rpc_code::deadline_exceeded,
                         "name resolution exceeded its deadline"};
    }
    if (job->gai_code != 0) {
        return classify_resolution(job->gai_code, std::string(host));
    }

    std::vector<ip_address> out;
    for (const ip_address& address_result : job->addresses) {
        if (options.family == ip_family::ipv4 && !address_result.is_v4()) {
            continue;
        }
        if (options.family == ip_family::ipv6 && !address_result.is_v6()) {
            continue;
        }
        out.push_back(address_result);
    }
    if (out.empty()) {
        return rpc_error{rpc_code::unavailable, "no address of the requested family"};
    }
    return out;
}

// --- Unix domain sockets ------------------------------------------------------

bool unix_socket_supported() noexcept {
    detail::ensure_sockets_initialized();
    static const bool supported = []() {
        const detail::native_socket probe =
            ::socket(detail::k_address_family_unix, SOCK_STREAM, 0);
        if (probe == detail::invalid_socket) {
            return false;
        }
        detail::close_socket(probe);
        return true;
    }();
    return supported;
}

status validate_unix_path(std::string_view path) {
    if (path.empty()) {
        return status{rpc_code::invalid_argument, "unix path is empty"};
    }
    if (path.find('\0') != std::string_view::npos) {
        return status{rpc_code::invalid_argument, "unix path contains a NUL"};
    }
    // `sockaddr_un::sun_path` holds 108 bytes on Windows and POSIX.
    if (path.size() >= 108) {
        return status{rpc_code::invalid_argument, "unix path is longer than 107 bytes"};
    }
    return status{};
}

} // namespace net
} // namespace crpc