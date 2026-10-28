// CRPC -- Redis integration against a real server.
//
// Skipped unless CRPC_REDIS_IT=1: a suite that needs a live Redis on every
// developer machine is a suite people disable. The unit suite proves the codec
// on constructed bytes; these cases prove the same machinery against a live
// RESP stream and an arbitrary command path.
//
// Configuration comes from the environment:
//   CRPC_REDIS_IT=1
//   CRPC_REDIS_HOST (default 127.0.0.1)
//   CRPC_REDIS_PORT (default 6379)
//
// What is and is not verifiable against a real server is part of the contract
// this file documents:
//   * arbitrary commands via `command()` (INFO, COMMAND COUNT, OBJECT ENCODING);
//   * the facade negotiates RESP3 when `options.protocol == resp3`: a pooled
//     connection sends `HELLO 3` once per link before its first command, so
//     maps (HGETALL), doubles (ZSCORE) and pushes (CLIENT TRACKING
//     invalidations) are real wire frames the decoder must turn into values;
//   * RESP3 attribute (`|`) and streamed (`$?`/`*?`/`~?`/`%?`) frames are
//     part of the spec but no Redis 7 core command emits them (the RESP3 spec
//     states the server implements neither), so those remain constructed-byte
//     unit tests; a failed trigger attempt is reported rather than faked.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/io/iocp_backend.hpp>
#include <crpc/redis/client.hpp>
#include <crpc/redis/connection.hpp>
#include <crpc/redis/pipeline.hpp>
#include <crpc/redis/resp.hpp>

using namespace std::chrono_literals;

namespace {

[[nodiscard]] std::string env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::string(value)
                                                : std::string(fallback);
}

[[nodiscard]] const char* require_redis() {
    const char* value = std::getenv("CRPC_REDIS_IT");
    if (value == nullptr || std::strcmp(value, "1") != 0) {
        crpc_test::skip("CRPC_REDIS_IT is not 1");
    }
    return "";
}

crpc::endpoint redis_endpoint() {
    return crpc::endpoint::tcp(
        env_or("CRPC_REDIS_HOST", "127.0.0.1"),
        static_cast<std::uint16_t>(std::stoi(env_or("CRPC_REDIS_PORT", "6379"))));
}

crpc::redis::client::options client_options() {
    crpc::redis::client::options opts;
    opts.pool.connection.ep = redis_endpoint();
    opts.pool.connection.operation_timeout = std::chrono::seconds(5);
    return opts;
}

crpc::result<crpc::redis::resp_value> call(crpc::redis::client& c,
                                           crpc::redis::command args) {
    return c.command(crpc::span<const std::string>(args.data(), args.size()));
}

// One pooled raw connection plus a request/reply channel, for protocol-level
// cases (HELLO 3, CLIENT TRACKING) that the facade cannot express. The channel
// is heap-owned because `command_channel` neither default-constructs nor
// reassigns; it is created right after the connection is acquired.
struct raw_session {
    crpc::iocp_backend backend;
    crpc::redis::connection_pool pool;
    std::shared_ptr<crpc::redis::connection> conn;
    std::unique_ptr<crpc::redis::command_channel> channel;

    raw_session() : pool(client_options().pool, backend) {}

    crpc::status connect() {
        auto acquired = pool.acquire();
        if (!acquired) {
            return acquired.error();
        }
        conn = std::move(acquired).value();
        channel = std::make_unique<crpc::redis::command_channel>(*conn);
        channel->set_protocol(crpc::redis::resp_protocol::resp3);
        return crpc::status{};
    }

    [[nodiscard]] crpc::result<crpc::redis::resp_value> exec(
        const crpc::redis::command& cmd) {
        return channel->execute(cmd);
    }

    ~raw_session() {
        channel.reset();
        if (conn) {
            pool.release(std::move(conn));
        }
    }
};

std::string printable(const std::vector<std::byte>& bytes) {
    std::string out;
    for (std::byte b : bytes) {
        const char c = static_cast<char>(b);
        if (c == '\r') {
            out += "\\r";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c >= 0x20 && c <= 0x7e) {
            out += c;
        } else {
            out += "\\x";
            char hex[3];
            std::snprintf(hex, sizeof(hex), "%02x",
                          static_cast<unsigned>(static_cast<unsigned char>(c)));
            out += hex;
        }
    }
    return out;
}

bool first_push_kind_is(const crpc::redis::resp_value& value,
                        const char* kind) {
    if (!value.is_push() || value.as_push().empty()) {
        return false;
    }
    const crpc::redis::resp_value& head = value.as_push().front();
    if (head.is_simple_string()) {
        return head.as_simple_string() == kind;
    }
    if (head.is_bulk_string()) {
        return head.as_bulk_string() == kind;
    }
    return false;
}

// Redis spells some fields as simple strings, others as bulk strings, and
// INFO/CLIENT INFO as a RESP3 verbatim string (`=`); fold the three like the
// feature layer's reply helpers do, so an assertion does not care which wire
// form arrived.
const std::string* text_of(const crpc::redis::resp_value& value) {
    if (value.is_simple_string()) {
        return &value.as_simple_string();
    }
    if (value.is_bulk_string()) {
        return &value.as_bulk_string();
    }
    if (value.is_verbatim_string()) {
        return &value.as_verbatim_string();
    }
    return nullptr;
}

std::string unique_key(const char* prefix) {
    return std::string(prefix) +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

// Finds the protocol number in a `CLIENT INFO` reply. Redis reports it as a
// `resp=` field (that is wire text, not the same `proto` field as HELLO);
// returns -1 when the reply is not text or the field is absent.
std::int64_t client_info_resp(const crpc::redis::resp_value& reply) {
    const std::string* body = text_of(reply);
    if (body == nullptr) {
        return -1;
    }
    const std::string_view needle = "resp=";
    const std::size_t at = body->find(needle);
    if (at == std::string::npos) {
        return -1;
    }
    std::string_view rest = std::string_view(*body).substr(at + needle.size());
    const std::size_t nl = rest.find_first_of("\r\n ");
    if (nl != std::string::npos) {
        rest = rest.substr(0, nl);
    }
    const std::string value(rest);
    return value.empty() ? -1 : static_cast<std::int64_t>(std::stoll(value));
}

} // namespace

CRPC_TEST(it_reads_redis_version) {
    (void)require_redis();
    crpc::iocp_backend backend;
    crpc::redis::client c(client_options(), backend);
    crpc::result<crpc::redis::resp_value> info = call(c, {"INFO", "server"});
    CRPC_CHECK(info.ok());
    CRPC_CHECK(info.value().is_bulk_string() || info.value().is_simple_string());

    const std::string& body = info.value().is_bulk_string()
                                  ? info.value().as_bulk_string()
                                  : info.value().as_simple_string();
    const std::string_view needle = "redis_version:";
    const std::size_t at = body.find(needle);
    CRPC_CHECK(at != std::string::npos);
    const std::string_view version =
        std::string_view(body).substr(at + needle.size());
    const std::size_t nl = version.find('\r');
    const std::string actual(version.substr(0, nl));
    std::printf("  redis_version=%s\n", actual.c_str());

    // RESP3 attribute/streamed frames require Redis 6+ (HELLO 3).
    const int major = actual.size() > 0 ? actual[0] - '0' : 0;
    CRPC_CHECK(major >= 6);
}

CRPC_TEST(it_command_runs_arbitrary_commands) {
    (void)require_redis();
    crpc::iocp_backend backend;
    crpc::redis::client c(client_options(), backend);
    CRPC_CHECK(c.connect().ok());

    crpc::result<crpc::redis::resp_value> count = call(c, {"COMMAND", "COUNT"});
    CRPC_CHECK(count.ok());
    CRPC_CHECK(count.value().is_integer());
    CRPC_CHECK(count.value().as_integer() > 0);

    const std::string key = "crpc:it:command:" +
                            std::to_string(std::chrono::steady_clock::now()
                                               .time_since_epoch()
                                               .count());
    crpc::result<crpc::redis::resp_value> set = call(c, {"SET", key, "v1"});
    CRPC_CHECK(set.ok());

    crpc::result<crpc::redis::resp_value> encoding =
        call(c, {"OBJECT", "ENCODING", key});
    CRPC_CHECK(encoding.ok());
    CRPC_CHECK(encoding.value().is_bulk_string() || encoding.value().is_null());
    if (encoding.value().is_bulk_string()) {
        CRPC_CHECK_FALSE(encoding.value().as_bulk_string().empty());
    }

    crpc::result<crpc::redis::resp_value> get = call(c, {"GET", key});
    CRPC_CHECK(get.ok());
    CRPC_CHECK(get.value().is_bulk_string());
    CRPC_CHECK_EQ(std::string("v1"), get.value().as_bulk_string());

    CRPC_CHECK(call(c, {"DEL", key}).ok());
}

CRPC_TEST(it_command_encoding_is_injection_safe) {
    (void)require_redis();
    crpc::iocp_backend backend;
    crpc::redis::client c(client_options(), backend);
    CRPC_CHECK(c.connect().ok());

    const std::string key = "crpc:it:sneaky:" +
                            std::to_string(std::chrono::steady_clock::now()
                                               .time_since_epoch()
                                               .count());
    // An artificial "second command" embedded in a payload value. If the
    // encoder split or mis-sized arguments, GET would observe garbage.
    const std::string payload = "abc\r\n*2\r\n$3\r\nGET\r\n$1\r\nx\r\n zz";
    const std::vector<std::string> args{"SET", key, payload};
    crpc::result<crpc::redis::resp_value> set =
        call(c, args);
    CRPC_CHECK(set.ok());

    crpc::result<crpc::redis::resp_value> get = call(c, {"GET", key});
    CRPC_CHECK(get.ok());
    CRPC_CHECK_EQ(payload, get.value().as_bulk_string());

    CRPC_CHECK(call(c, {"DEL", key}).ok());
}

CRPC_TEST(it_resp3_client_negotiates_and_serves_resp3_shapes) {
    (void)require_redis();
    crpc::iocp_backend backend;
    crpc::redis::client::options opts = client_options();
    opts.protocol = crpc::redis::resp_protocol::resp3;
    crpc::redis::client c(opts, backend);
    CRPC_CHECK(c.connect().ok());

    // connect() negotiated: HELLO 3 reported the server identity, version and
    // role, and a RESP3 protocol number.
    CRPC_CHECK_EQ(static_cast<std::int64_t>(3), c.hello().proto);
    CRPC_CHECK_EQ(std::string("redis"), c.hello().server);
    CRPC_CHECK_FALSE(c.hello().version.empty());
    CRPC_CHECK_FALSE(c.hello().role.empty());

    // Cross-check the advertised version against INFO server.
    crpc::result<crpc::redis::resp_value> info = call(c, {"INFO", "server"});
    CRPC_CHECK(info.ok());
    const std::string* body = text_of(info.value());
    CRPC_CHECK(body != nullptr);
    if (body == nullptr) {
        return;
    }
    const std::string_view needle = "redis_version:";
    const std::size_t at = body->find(needle);
    CRPC_CHECK(at != std::string::npos);
    const std::string_view version =
        std::string_view(*body).substr(at + needle.size());
    const std::size_t nl = version.find('\r');
    CRPC_CHECK_EQ(std::string(version.substr(0, nl)), c.hello().version);

    // HGETALL is `%` (map) only under RESP3; under RESP2 it would be a flat
    // array. The facade path proves the negotiated session actually talks RESP3.
    const std::string hkey = unique_key("crpc:it:resp3:h:");
    CRPC_CHECK(c.hset(hkey, "f", "v").ok());
    crpc::result<crpc::redis::resp_value> hgetall = c.hgetall(hkey);
    CRPC_CHECK(hgetall.ok());
    CRPC_CHECK(hgetall.value().is_map());
    if (hgetall.value().is_map()) {
        const std::string* field_value =
            text_of(hgetall.value().as_map()[0].second);
        CRPC_CHECK(field_value != nullptr && *field_value == "v");
    }

    // ZSCORE is `,` (double) only under RESP3.
    const std::string zkey = unique_key("crpc:it:resp3:z:");
    CRPC_CHECK(c.zadd(zkey, 1.5, "m").ok());
    crpc::result<crpc::redis::resp_value> zscore = c.zscore(zkey, "m");
    CRPC_CHECK(zscore.ok());
    CRPC_CHECK(zscore.value().is_double());
    if (zscore.value().is_double()) {
        CRPC_CHECK_EQ(zscore.value().as_double(), 1.5);
    }

    // CLIENT INFO on the live session proves the wire protocol really is 3.
    crpc::result<crpc::redis::resp_value> client_info = c.execute({"CLIENT", "INFO"});
    CRPC_CHECK(client_info.ok());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(3), client_info_resp(client_info.value()));

    CRPC_CHECK(c.del({hkey}).ok());
    CRPC_CHECK(c.del({zkey}).ok());
}

CRPC_TEST(it_resp2_client_keeps_resp2_shapes) {
    (void)require_redis();
    crpc::iocp_backend backend;
    crpc::redis::client c(client_options(), backend);
    CRPC_CHECK(c.connect().ok());

    // A RESP2 client never sends HELLO, so no server information is known.
    CRPC_CHECK(c.hello().version.empty());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(0), c.hello().proto);

    const std::string hkey = unique_key("crpc:it:resp2:h:");
    CRPC_CHECK(c.hset(hkey, "f", "v").ok());
    crpc::result<crpc::redis::resp_value> hgetall = c.hgetall(hkey);
    CRPC_CHECK(hgetall.ok());
    CRPC_CHECK_FALSE(hgetall.value().is_map());
    CRPC_CHECK(hgetall.value().is_array());
    if (hgetall.value().is_array()) {
        CRPC_CHECK_EQ(std::size_t{2}, hgetall.value().as_array().size());
    }

    const std::string zkey = unique_key("crpc:it:resp2:z:");
    CRPC_CHECK(c.zadd(zkey, 1.5, "m").ok());
    crpc::result<crpc::redis::resp_value> zscore = c.zscore(zkey, "m");
    CRPC_CHECK(zscore.ok());
    CRPC_CHECK_FALSE(zscore.value().is_double());
    if (zscore.value().is_bulk_string()) {
        CRPC_CHECK_EQ(std::string("1.5"), zscore.value().as_bulk_string());
    }

    crpc::result<crpc::redis::resp_value> client_info = c.execute({"CLIENT", "INFO"});
    CRPC_CHECK(client_info.ok());
    CRPC_CHECK_EQ(static_cast<std::int64_t>(2), client_info_resp(client_info.value()));

    CRPC_CHECK(c.del({hkey}).ok());
    CRPC_CHECK(c.del({zkey}).ok());
}

CRPC_TEST(it_resp3_pool_negotiates_every_connection) {
    (void)require_redis();
    crpc::iocp_backend backend;
    crpc::redis::client::options opts = client_options();
    opts.protocol = crpc::redis::resp_protocol::resp3;
    opts.pool.max_size = 8;
    crpc::redis::client c(opts, backend);

    // Check out several pooled connections and prove each live session runs
    // RESP3. acquire() is exactly the path a caller that drives a raw
    // command_channel over a pooled connection uses.
    std::vector<std::shared_ptr<crpc::redis::connection>> held;
    constexpr int connections = 4;
    for (int i = 0; i < connections; ++i) {
        crpc::result<std::shared_ptr<crpc::redis::connection>> conn = c.acquire();
        CRPC_CHECK(conn.ok());
        held.push_back(std::move(conn).value());
    }
    CRPC_CHECK_EQ(static_cast<std::size_t>(connections), c.total());

    for (const std::shared_ptr<crpc::redis::connection>& conn : held) {
        crpc::redis::command_channel channel(*conn);
        channel.set_protocol(crpc::redis::resp_protocol::resp3);
        crpc::result<crpc::redis::resp_value> info =
            channel.execute({"CLIENT", "INFO"});
        CRPC_CHECK(info.ok());
        CRPC_CHECK_EQ(static_cast<std::int64_t>(3), client_info_resp(info.value()));
    }

    for (std::shared_ptr<crpc::redis::connection>& conn : held) {
        c.release(std::move(conn));
    }
    CRPC_CHECK_EQ(std::size_t{0}, c.in_use());
}

CRPC_TEST(it_client_tracking_sends_push_invalidations) {
    (void)require_redis();
    raw_session session;
    CRPC_CHECK(session.connect().ok());

    // A second, independent connection mutates the key. On either protocol the
    // mutation reply shares no ordering with the invalidation push.
    crpc::iocp_backend backend;
    crpc::redis::client writer(client_options(), backend);
    CRPC_CHECK(writer.connect().ok());

    const std::string key = "crpc:it:track:" +
                            std::to_string(std::chrono::steady_clock::now()
                                               .time_since_epoch()
                                               .count());

    CRPC_CHECK(session.exec({"HELLO", "3"}).ok());
    CRPC_CHECK(session.exec({"CLIENT", "TRACKING", "on"}).ok());
    // Reading the key registers it for invalidation tracking.
    CRPC_CHECK(session.exec({"GET", key}).ok());

    CRPC_CHECK(call(writer, {"SET", key, "changed"}).ok());

    // Drain whatever (if anything) the server pushed: read one byte to block
    // for the push, then drain, and feed the bytes into the decoder.
    std::vector<std::byte> raw;
    const auto deadline = std::chrono::steady_clock::now() + 3000ms;
    crpc::redis::resp_value push;
    bool got_push = false;
    while (std::chrono::steady_clock::now() < deadline) {
        crpc::result<std::vector<std::byte>> first = session.conn->read_exact(1);
        if (!first) {
            break;
        }
        raw.push_back(first.value().front());
        std::vector<std::byte> rest = session.conn->read_available();
        raw.insert(raw.end(), rest.begin(), rest.end());

        crpc::redis::resp_parser parser;
        parser.feed(crpc::span<const std::byte>(raw.data(), raw.size()));
        while (parser.next(push) == crpc::redis::resp_parser::result::value_ready) {
            if (first_push_kind_is(push, "invalidate")) {
                got_push = true;
                break;
            }
        }
        if (got_push || parser.failed()) {
            break;
        }
        std::this_thread::sleep_for(50ms);
    }

    // RESP3 invalidation arrives as a `>` push frame, never as a `|` attribute
    // frame on Redis 7 -- that is exactly the evidence this case documents.
    std::printf("  tracking push bytes: %s\n", printable(raw).c_str());
    if (!got_push) {
        crpc_test::skip(
            "no CLIENT TRACKING invalidation arrived (server did not push); "
            "RESP3 attribute frames stay constructed-byte cases");
    }
    CRPC_CHECK(first_push_kind_is(push, "invalidate"));

    CRPC_CHECK(call(writer, {"DEL", key}).ok());
    CRPC_CHECK(session.exec({"CLIENT", "TRACKING", "off"}).ok());
}

CRPC_TEST_MAIN()
