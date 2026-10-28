// CRPC -- tests for the WebSocket opening handshake (crpc/ws/handshake.hpp).
//
// The accept computation is pinned with the RFC 6455 section 1.3 worked
// example (key dGhlIHNhbXBsZSBub25jZQ== -> s3pPLMBiTxaQ9kYGzzhZRbK+xOo=).
// The client-side and server-side validation covers every field RFC 6455
// section 4 requires both roles to check.
#include <cstddef>
#include <string>
#include <string_view>

#include <crpc/crypto/base64.hpp>
#include <crpc/ws/handshake.hpp>

#include "test_framework.hpp"
#include <ostream>
#include <type_traits>

// check_equal() streams both sides of a failed comparison; this library's
// enum-classes have no operator<<, so the integer form is provided for ADL.
namespace crpc {
template <typename E,
          typename std::enable_if<std::is_enum<E>::value, int>::type = 0>
std::ostream& operator<<(std::ostream& os, E value) {
    return os << static_cast<long long>(value);
}
} // namespace crpc

using namespace crpc;
using namespace crpc::ws;

namespace {

[[nodiscard]] http::request make_valid_client_request(std::string_view key) {
    http::request req;
    req.method = http::method::get;
    req.target = "/chat";
    req.headers.add("Host", "example.com");
    req.headers.add("Upgrade", "websocket");
    req.headers.add("Connection", "Upgrade");
    req.headers.add("Sec-WebSocket-Key", key);
    req.headers.add("Sec-WebSocket-Version", "13");
    return req;
}

[[nodiscard]] std::string sixteen_byte_key() {
    // Any 16 random bytes base64-encoded.
    return crypto::base64_encode(std::string_view("0123456789abcdef"));
}

CRPC_TEST(ws_handshake_rfc_accept_vector) {
    const std::string accept =
        compute_accept("dGhlIHNhbXBsZSBub25jZQ==");
    CRPC_CHECK_EQ("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", accept);
}

CRPC_TEST(ws_handshake_key_is_sixteen_bytes_base64) {
    const std::string key = websocket_key();
    CRPC_CHECK_EQ(24u, key.size()); // 16 bytes -> 24 base64 chars
    result<std::string> decoded = crypto::base64_decode(key);
    CRPC_CHECK(decoded.ok());
    CRPC_CHECK_EQ(16u, decoded.value().size());
    CRPC_CHECK(websocket_key() != key);
}

CRPC_TEST(ws_handshake_client_request_shape) {
    const http::request req =
        make_upgrade_request("example.com", "/chat", "https://example.com", "k");
    CRPC_CHECK_EQ(http::method::get, req.method);
    CRPC_CHECK_EQ("/chat", req.target);
    CRPC_CHECK(req.headers.contains("Host"));
    CRPC_CHECK(req.headers.contains("Upgrade"));
    CRPC_CHECK(req.headers.contains("Connection"));
    CRPC_CHECK(req.headers.contains("Sec-WebSocket-Key"));
    CRPC_CHECK(req.headers.contains("Sec-WebSocket-Version"));
    const std::string* version = req.headers.find("Sec-WebSocket-Version");
    CRPC_CHECK(version != nullptr);
    CRPC_CHECK_EQ("13", *version);
    const std::string* origin = req.headers.find("Origin");
    CRPC_CHECK(origin != nullptr);
    CRPC_CHECK_EQ("https://example.com", *origin);
}

CRPC_TEST(ws_handshake_server_validates_good_request) {
    const std::string key = sixteen_byte_key();
    result<std::string> out = validate_upgrade_request(make_valid_client_request(key));
    CRPC_CHECK(out.ok());
    CRPC_CHECK_EQ(key, out.value());
}

CRPC_TEST(ws_handshake_server_rejects_wrong_method) {
    http::request req = make_valid_client_request(sixteen_byte_key());
    req.method = http::method::post;
    CRPC_CHECK_FALSE(validate_upgrade_request(req).ok());
}

CRPC_TEST(ws_handshake_server_rejects_missing_upgrade) {
    http::request req;
    req.method = http::method::get;
    req.target = "/chat";
    req.headers.add("Host", "example.com");
    req.headers.add("Connection", "Upgrade");
    req.headers.add("Sec-WebSocket-Key", sixteen_byte_key());
    req.headers.add("Sec-WebSocket-Version", "13");
    CRPC_CHECK_FALSE(validate_upgrade_request(req).ok());
}

CRPC_TEST(ws_handshake_server_rejects_wrong_upgrade_value) {
    http::request req;
    req.method = http::method::get;
    req.target = "/chat";
    req.headers.add("Host", "example.com");
    req.headers.add("Upgrade", "h2c");
    req.headers.add("Connection", "Upgrade");
    req.headers.add("Sec-WebSocket-Key", sixteen_byte_key());
    req.headers.add("Sec-WebSocket-Version", "13");
    CRPC_CHECK_FALSE(validate_upgrade_request(req).ok());
}

CRPC_TEST(ws_handshake_server_rejects_missing_connection_upgrade) {
    http::request req;
    req.method = http::method::get;
    req.target = "/chat";
    req.headers.add("Host", "example.com");
    req.headers.add("Upgrade", "websocket");
    req.headers.add("Connection", "keep-alive");
    req.headers.add("Sec-WebSocket-Key", sixteen_byte_key());
    req.headers.add("Sec-WebSocket-Version", "13");
    CRPC_CHECK_FALSE(validate_upgrade_request(req).ok());
}

CRPC_TEST(ws_handshake_server_rejects_wrong_version) {
    http::request req;
    req.method = http::method::get;
    req.target = "/chat";
    req.headers.add("Host", "example.com");
    req.headers.add("Upgrade", "websocket");
    req.headers.add("Connection", "Upgrade");
    req.headers.add("Sec-WebSocket-Key", sixteen_byte_key());
    req.headers.add("Sec-WebSocket-Version", "12");
    CRPC_CHECK_FALSE(validate_upgrade_request(req).ok());
}

CRPC_TEST(ws_handshake_server_rejects_bad_key) {
    {
        http::request req;
        req.method = http::method::get;
        req.target = "/chat";
        req.headers.add("Host", "example.com");
        req.headers.add("Upgrade", "websocket");
        req.headers.add("Connection", "Upgrade");
        req.headers.add("Sec-WebSocket-Version", "13");
        CRPC_CHECK_FALSE(validate_upgrade_request(req).ok());
    }
    {
        http::request req;
        req.method = http::method::get;
        req.target = "/chat";
        req.headers.add("Host", "example.com");
        req.headers.add("Upgrade", "websocket");
        req.headers.add("Connection", "Upgrade");
        req.headers.add("Sec-WebSocket-Key",
                        crypto::base64_encode(std::string_view("abcdefghijkl")));
        req.headers.add("Sec-WebSocket-Version", "13");
        CRPC_CHECK_FALSE(validate_upgrade_request(req).ok());
    }
    {
        http::request req;
        req.method = http::method::get;
        req.target = "/chat";
        req.headers.add("Host", "example.com");
        req.headers.add("Upgrade", "websocket");
        req.headers.add("Connection", "Upgrade");
        req.headers.add("Sec-WebSocket-Key", std::string(24, '!'));
        req.headers.add("Sec-WebSocket-Version", "13");
        CRPC_CHECK_FALSE(validate_upgrade_request(req).ok());
    }
}

CRPC_TEST(ws_handshake_server_response_matches_accept) {
    const http::response resp = make_upgrade_response("dGhlIHNhbXBsZSBub25jZQ==");
    CRPC_CHECK_EQ(101, resp.status_code);
    const std::string* accept = resp.headers.find("Sec-WebSocket-Accept");
    CRPC_CHECK(accept != nullptr);
    CRPC_CHECK_EQ("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", *accept);
    const std::string* upgrade = resp.headers.find("Upgrade");
    CRPC_CHECK(upgrade != nullptr);
    CRPC_CHECK_EQ("websocket", *upgrade);
}

CRPC_TEST(ws_handshake_client_validates_response) {
    const std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
    const http::response good = make_upgrade_response(key);
    CRPC_CHECK(validate_upgrade_response(good, key).ok());

    http::response wrong_status = good;
    wrong_status.status_code = 200;
    CRPC_CHECK_FALSE(validate_upgrade_response(wrong_status, key).ok());

    // A 101 without Upgrade: websocket is a protocol error.
    http::response no_upgrade;
    no_upgrade.status_code = 101;
    no_upgrade.reason = "Switching Protocols";
    no_upgrade.headers.add("Connection", "Upgrade");
    no_upgrade.headers.add("Sec-WebSocket-Accept", "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    CRPC_CHECK_FALSE(validate_upgrade_response(no_upgrade, key).ok());

    // A 101 whose accept value does not match the key we sent.
    http::response bad_accept;
    bad_accept.status_code = 101;
    bad_accept.reason = "Switching Protocols";
    bad_accept.headers.add("Upgrade", "websocket");
    bad_accept.headers.add("Connection", "Upgrade");
    bad_accept.headers.add("Sec-WebSocket-Accept", "AAAAAAAAAAAAAAAAAAAAAAAAABg=");
    CRPC_CHECK_FALSE(validate_upgrade_response(bad_accept, key).ok());
}

CRPC_TEST(ws_handshake_validates_missing_version_field) {
    http::request req = make_valid_client_request(sixteen_byte_key());
    // A request without Sec-WebSocket-Version is rejected as well; build one by
    // keeping only the required headers.
    http::request bare;
    bare.method = http::method::get;
    bare.target = "/";
    bare.headers.add("Host", "example.com");
    bare.headers.add("Upgrade", "websocket");
    bare.headers.add("Connection", "Upgrade");
    bare.headers.add("Sec-WebSocket-Key", sixteen_byte_key());
    CRPC_CHECK_FALSE(validate_upgrade_request(bare).ok());
}

} // namespace

CRPC_TEST_MAIN()