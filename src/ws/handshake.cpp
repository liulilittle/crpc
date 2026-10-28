// CRPC -- WebSocket opening handshake implementation (RFC 6455 section 4).
#include <crpc/ws/handshake.hpp>

#include <array>
#include <cstddef>
#include <random>

#include <crpc/crypto/base64.hpp>
#include <crpc/crypto/hash.hpp>
#include <crpc/ws/frame.hpp>

namespace crpc {
namespace ws {
namespace {

[[nodiscard]] span<const std::byte> as_bytes(std::string_view text) noexcept {
    return span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                 text.size());
}

[[nodiscard]] std::string_view trim_ows(std::string_view value) noexcept {
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] std::string_view lower(std::string_view text, std::string& scratch) {
    scratch.clear();
    scratch.reserve(text.size());
    for (const char c : text) {
        scratch.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return scratch;
}

// A comma-separated list (Connection can carry several tokens); RFC 6455 does
// the Upgrade and Connection matching token-wise.
[[nodiscard]] bool list_has(const http::header_list& headers, std::string_view name,
                            std::string_view wanted) {
    std::string scratch;
    const std::vector<std::string_view> values = headers.get_all(name);
    for (const std::string_view value : values) {
        std::size_t start = 0;
        while (start < value.size()) {
            const std::size_t comma = value.find(',', start);
            const std::size_t end = comma == std::string_view::npos ? value.size() : comma;
            const std::string_view token =
                lower(trim_ows(value.substr(start, end - start)), scratch);
            if (token == wanted) {
                return true;
            }
            if (comma == std::string_view::npos) {
                break;
            }
            start = comma + 1;
        }
    }
    return false;
}

void random_bytes(span<std::byte> out) {
    std::random_device device;
    for (auto& byte : out) {
        byte = static_cast<std::byte>(device() & 0xffu);
    }
}

} // namespace

std::string websocket_key() {
    std::array<std::byte, 16> raw{};
    random_bytes(span<std::byte>(raw.data(), raw.size()));
    return crypto::base64_encode(span<const std::byte>(raw.data(), raw.size()));
}

std::string compute_accept(std::string_view key) {
    const crypto::sha1_digest digest =
        crypto::sha1(as_bytes(key), as_bytes(websocket_guid));
    return crypto::base64_encode(span<const std::byte>(
        reinterpret_cast<const std::byte*>(digest.data()), digest.size()));
}

http::request make_upgrade_request(std::string_view host, std::string_view path,
                                   std::string_view origin, std::string_view key) {
    http::request req;
    req.method = http::method::get;
    req.target = path.empty() ? std::string("/") : std::string(path);
    req.version = http::http_version::http_1_1;
    req.headers.add("Host", host);
    req.headers.add("Upgrade", "websocket");
    req.headers.add("Connection", "Upgrade");
    req.headers.add("Sec-WebSocket-Key", key);
    req.headers.add("Sec-WebSocket-Version", "13");
    if (!origin.empty()) {
        req.headers.add("Origin", origin);
    }
    return req;
}

result<std::string> validate_upgrade_request(const http::request& req) {
    if (req.method != http::method::get) {
        return rpc_error{rpc_code::invalid_argument,
                         "a WebSocket upgrade must be a GET request"};
    }
    if (!list_has(req.headers, "upgrade", "websocket")) {
        return rpc_error{rpc_code::invalid_argument,
                         "the Upgrade header must contain 'websocket'"};
    }
    if (!list_has(req.headers, "connection", "upgrade")) {
        return rpc_error{rpc_code::invalid_argument,
                         "the Connection header must contain 'upgrade'"};
    }
    const std::string* version = req.headers.find("sec-websocket-version");
    if (version == nullptr || trim_ows(*version) != "13") {
        return rpc_error{rpc_code::invalid_argument,
                         "Sec-WebSocket-Version must be 13"};
    }
    const std::string* key = req.headers.find("sec-websocket-key");
    if (key == nullptr || key->empty()) {
        return rpc_error{rpc_code::invalid_argument,
                         "a Sec-WebSocket-Key header is required"};
    }
    result<std::string> decoded = crypto::base64_decode(*key);
    if (!decoded || decoded.value().size() != 16) {
        return rpc_error{rpc_code::invalid_argument,
                         "Sec-WebSocket-Key must decode to exactly 16 bytes"};
    }
    return *key;
}

http::response make_upgrade_response(std::string_view key) {
    http::response resp;
    resp.status_code = 101;
    resp.reason = "Switching Protocols";
    resp.headers.add("Upgrade", "websocket");
    resp.headers.add("Connection", "Upgrade");
    resp.headers.add("Sec-WebSocket-Accept", compute_accept(key));
    return resp;
}

status validate_upgrade_response(const http::response& resp, std::string_view key) {
    if (resp.status_code != 101) {
        return status{rpc_code::failed_precondition,
                      "the server refused the WebSocket upgrade"};
    }
    if (!list_has(resp.headers, "upgrade", "websocket")) {
        return status{rpc_code::protocol_error,
                      "a 101 response must carry Upgrade: websocket"};
    }
    if (!list_has(resp.headers, "connection", "upgrade")) {
        return status{rpc_code::protocol_error,
                      "a 101 response must carry Connection: upgrade"};
    }
    const std::string* accept = resp.headers.find("sec-websocket-accept");
    if (accept == nullptr || trim_ows(*accept) != compute_accept(key)) {
        return status{rpc_code::protocol_error,
                      "Sec-WebSocket-Accept does not match the request key"};
    }
    return status{};
}

} // namespace ws
} // namespace crpc