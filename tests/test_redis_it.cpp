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
#include <cctype>
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
#include "test_tcp_backend.hpp"
#include <crpc/redis/client.hpp>
#include <crpc/redis/command_table.hpp>
#include <crpc/redis/connection.hpp>
#include <crpc/redis/pipeline.hpp>
#include <crpc/redis/resp.hpp>
#include <crpc/redis/sentinel.hpp>

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
    tcp_backend backend;
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
    tcp_backend backend;
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
    tcp_backend backend;
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
    tcp_backend backend;
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
    tcp_backend backend;
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
    tcp_backend backend;
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
    tcp_backend backend;
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
    tcp_backend backend;
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

// A Redis error reply is a *successful* `result`: the bytes were understood and
// the server answered. Every assertion that means "the command worked" must
// therefore exclude it explicitly, or a READONLY/NOAUTH reply passes silently.
bool command_succeeded(const crpc::result<crpc::redis::resp_value>& reply) {
    return reply.ok() && !reply.value().is_error();
}

// Reads a string-valued reply with the type checked first. `as_bulk_string()`
// on any other alternative throws std::bad_variant_access, which reaches
// std::terminate -- it must never be called on an unchecked reply.
//
// Both the bulk and the verbatim alternatives count as text: on a RESP3 link
// CLIENT INFO answers with a verbatim string ('='), so a bulk-only helper
// rejects a perfectly good reply. A simple string is accepted too (SET -> +OK).
bool read_text(const crpc::result<crpc::redis::resp_value>& reply,
               const char* label, std::string& out) {
    if (!reply.ok()) {
        std::printf("  %s: no reply (%s)\n", label, reply.error().message.c_str());
        std::fflush(stdout);
        return false;
    }
    const crpc::redis::resp_value& value = reply.value();
    if (value.is_bulk_string()) {
        out = value.as_bulk_string();
        return true;
    }
    if (value.is_verbatim_string()) {
        out = value.as_verbatim_string();
        return true;
    }
    if (value.is_simple_string()) {
        out = value.as_simple_string();
        return true;
    }
    std::printf("  %s: unexpected reply type (bulk=%d verbatim=%d simple=%d "
                "error=%d int=%d null=%d)\n",
                label, value.is_bulk_string() ? 1 : 0,
                value.is_verbatim_string() ? 1 : 0,
                value.is_simple_string() ? 1 : 0, value.is_error() ? 1 : 0,
                value.is_integer() ? 1 : 0, value.is_null() ? 1 : 0);
    if (value.is_error()) {
        std::printf("  %s: server said: %s\n", label, value.as_error().c_str());
    }
    std::fflush(stdout);
    return false;
}

// A direct, unpooled command against one port, for putting the topology back
// into a known shape before the failover case runs.
crpc::result<crpc::redis::resp_value> admin_command(
    tcp_backend& backend, const std::string& host, std::uint16_t port,
    crpc::redis::command args) {
    crpc::net::pool_config options;
    options.connection.ep = crpc::endpoint::tcp(host, port);
    options.connection.operation_timeout = std::chrono::seconds(5);
    crpc::redis::connection_pool pool(options, backend);
    auto acquired = pool.acquire();
    if (!acquired) {
        return acquired.error();
    }
    std::shared_ptr<crpc::redis::connection> conn = std::move(acquired).value();
    {
        crpc::redis::command_channel channel(*conn);
        channel.set_protocol(crpc::redis::resp_protocol::resp2);
        crpc::result<crpc::redis::resp_value> reply = channel.execute(args);
        pool.release(std::move(conn));
        return reply;
    }
}

// Reads INFO replication from one node and reports whether it says `role:master`.
// Used to wait for a promotion to actually finish: Sentinel can publish the new
// address before the promoted node has dropped its replica state, and a write
// in that window is rejected with READONLY.
bool node_is_master(tcp_backend& backend, const std::string& host,
                    std::uint16_t port) {
    crpc::result<crpc::redis::resp_value> info = admin_command(
        backend, host, port, crpc::redis::command{"INFO", "replication"});
    if (!info.ok()) {
        return false;
    }
    std::string text;
    return read_text(info, "INFO replication", text) &&
           text.find("role:master") != std::string::npos;
}

// Flattens a RESP value into a searchable string. Sentinel's introspection
// replies are arrays of alternating field/value entries, and walking them by
// hand for every poll would bury the actual condition in type checks.
void flatten_text(const crpc::redis::resp_value& value, std::string& out) {
    if (value.is_bulk_string()) {
        out += value.as_bulk_string();
    } else if (value.is_verbatim_string()) {
        out += value.as_verbatim_string();
    } else if (value.is_simple_string()) {
        out += value.as_simple_string();
    } else if (value.is_integer()) {
        out += std::to_string(value.as_integer());
    } else if (value.is_array()) {
        for (const auto& item : value.as_array()) {
            out += ' ';
            flatten_text(item, out);
        }
    } else if (value.is_map()) {
        for (const auto& entry : value.as_map()) {
            out += ' ';
            flatten_text(entry.first, out);
            out += ' ';
            flatten_text(entry.second, out);
        }
    }
}

// Waits until Sentinel reports `replica_port` as a healthy replica of the
// current master. After SENTINEL RESET the daemon starts from nothing, so
// FAILOVER is answered with NOGOODSLAVE until discovery has run.
bool wait_until_replica_known(tcp_backend& backend, const std::string& host,
                              std::uint16_t sentinel_port, std::uint16_t replica_port,
                              std::chrono::seconds budget) {
    const std::string wanted = host + ":" + std::to_string(replica_port);
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        crpc::result<crpc::redis::resp_value> replicas = admin_command(
            backend, host, sentinel_port,
            crpc::redis::command{"SENTINEL", "REPLICAS", "mymaster"});
        if (replicas.ok() && !replicas.value().is_error()) {
            std::string text;
            flatten_text(replicas.value(), text);
            if (text.find(wanted) != std::string::npos &&
                text.find("s_down") == std::string::npos &&
                text.find("disconnected") == std::string::npos) {
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

bool wait_until_master(tcp_backend& backend, const std::string& host,
                       std::uint16_t port, std::chrono::seconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (node_is_master(backend, host, port)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

// --- Sentinel-driven failover ------------------------------------------------
//
// End-to-end proof that the bridge works against a real Sentinel: a live
// mymaster on 6380 with a replica on 6381, monitored by a real Sentinel daemon
// on 26380 (the smallest `sentinel monitor mymaster 127.0.0.1 6380 1`
// deployment). This case drives the library's own `sentinel` monitor against
// that daemon, wires `follow()` so a `master_switch` event repoints the pool,
// then makes the daemon fail the master over and asserts the pool follows.
//
// It is skipped unless CRPC_REDIS_SENTINEL_IT=1 (it needs the live cluster,
// like every IT case). The trigger is controlled by
// CRPC_REDIS_SENTINEL_TRIGGER:
//   * "failover" (default) -- issue `SENTINEL FAILOVER mymaster`;
//   * "kill" -- do nothing and wait (up to the deadline) for the operator to
//     kill the 6380 process, proving the pool follows a real master death.
CRPC_TEST(it_sentinel_failover_redirects_the_pool) {
    const char* gate = std::getenv("CRPC_REDIS_SENTINEL_IT");
    if (gate == nullptr || std::strcmp(gate, "1") != 0) {
        crpc_test::skip("CRPC_REDIS_SENTINEL_IT is not 1");
    }

    const std::string host = env_or("CRPC_REDIS_SENTINEL_HOST", "127.0.0.1");
    const std::uint16_t master_port = static_cast<std::uint16_t>(
        std::stoi(env_or("CRPC_REDIS_MASTER_PORT", "6380")));
    const std::uint16_t replica_port = static_cast<std::uint16_t>(
        std::stoi(env_or("CRPC_REDIS_REPLICA_PORT", "6381")));
    const std::uint16_t sentinel_port = static_cast<std::uint16_t>(
        std::stoi(env_or("CRPC_REDIS_SENTINEL_PORT", "26380")));
    const std::string trigger = env_or("CRPC_REDIS_SENTINEL_TRIGGER", "failover");

    tcp_backend backend;

    // The case ends by failing the master over, so it cannot assume which node
    // is the master when it starts: a second run begins with the roles swapped,
    // and a client pointed at a demoted node gets READONLY for every write.
    //
    // Resetting the daemon with SENTINEL RESET is not an option: that rolls its
    // config epoch back, and the library monitor correctly treats a lower epoch
    // as stale, so it would ignore the daemon for the rest of the run. Instead
    // adopt whatever the daemon currently believes and fail over from there.
    auto sentinel_master = [&]() -> crpc::result<crpc::redis::resp_value> {
        return admin_command(backend, host, sentinel_port,
                             crpc::redis::command{"SENTINEL", "GET-MASTER-ADDR-BY-NAME",
                                                  "mymaster"});
    };

    std::uint16_t initial_port = master_port;
    {
        crpc::result<crpc::redis::resp_value> view = sentinel_master();
        if (!view.ok() || !view.value().is_array() || view.value().as_array().size() != 2) {
            crpc_test::skip("the sentinel daemon did not report a master");
        }
        const auto& parts = view.value().as_array();
        if (!parts[1].is_bulk_string()) {
            crpc_test::skip("the sentinel daemon reported a master without a port");
        }
        const auto reported = static_cast<std::uint16_t>(std::stoi(parts[1].as_bulk_string()));
        if (reported != master_port && reported != replica_port) {
            crpc_test::skip("the sentinel daemon monitors an unexpected master");
        }
        initial_port = reported;
    }
    const std::uint16_t next_port = initial_port == master_port ? replica_port : master_port;

    // Force the pair into "initial_port master, next_port replica" so the case
    // always starts from the same shape, whichever node the daemon names.
    {
        const crpc::result<crpc::redis::resp_value> promote = admin_command(
            backend, host, initial_port, crpc::redis::command{"REPLICAOF", "NO", "ONE"});
        if (!promote.ok() || promote.value().is_error()) {
            const std::string reason =
                std::string("could not promote ") + std::to_string(initial_port) +
                " to master: " +
                (promote.ok() ? promote.value().as_error() : promote.error().message);
            crpc_test::skip(reason.c_str());
        }
        const crpc::result<crpc::redis::resp_value> follow = admin_command(
            backend, host, next_port,
            crpc::redis::command{"REPLICAOF", host, std::to_string(initial_port)});
        if (!follow.ok() || follow.value().is_error()) {
            const std::string reason =
                std::string("could not make ") + std::to_string(next_port) +
                " follow " + std::to_string(initial_port) + ": " +
                (follow.ok() ? follow.value().as_error() : follow.error().message);
            crpc_test::skip(reason.c_str());
        }
        if (!wait_until_master(backend, host, initial_port, std::chrono::seconds(10))) {
            crpc_test::skip("the initial master did not come up in time");
        }
        if (!wait_until_replica_known(backend, host, sentinel_port, next_port,
                                      std::chrono::seconds(30))) {
            crpc_test::skip("the sentinel daemon did not see the replica");
        }
        std::printf("  sentinel IT: topology %u master / %u replica\n",
                    static_cast<unsigned>(initial_port),
                    static_cast<unsigned>(next_port));
        std::fflush(stdout);
    }

    const crpc::redis::sentinel_address initial{host, initial_port};
    const crpc::redis::sentinel_address expected_new{host, next_port};

    // Library monitor: observers only (auto_failover=false). The real daemon
    // performs the promotion; this monitor just has to notice the new
    // configuration and emit master_switch, which follow() turns into a pool
    // redirect.
    crpc::redis::sentinel_connection_executor executor(backend);
    crpc::redis::sentinel_options monitor_opts;
    monitor_opts.master_name = "mymaster";
    monitor_opts.master = initial;
    monitor_opts.sentinels = {{host, sentinel_port}};
    // `self` identifies *this* monitor, so the peer list can drop it when a
    // deployment lists the local node among the sentinels. Setting it to the
    // daemon's own address therefore filtered the only peer out and left the
    // monitor with nobody to ask -- it never saw the switch. The monitor here is
    // a client of that daemon, so it must present a different identity.
    monitor_opts.self = {host, static_cast<std::uint16_t>(sentinel_port + 1)};
    monitor_opts.down_after = std::chrono::milliseconds(1000);
    monitor_opts.failover_timeout = std::chrono::milliseconds(15000);
    monitor_opts.poll_interval = std::chrono::milliseconds(500);
    monitor_opts.quorum = 1;
    monitor_opts.auto_failover = false;
    monitor_opts.run_id = "crpc-failover-it";
    crpc::redis::sentinel monitor(monitor_opts, executor);

    crpc::redis::client::options client_opts;
    client_opts.pool.connection.ep =
        crpc::endpoint::tcp(host, initial_port);
    client_opts.pool.connection.operation_timeout = std::chrono::seconds(5);
    client_opts.protocol = crpc::redis::resp_protocol::resp3;
    crpc::redis::client client(client_opts, backend);
    CRPC_CHECK(client.follow(monitor, "mymaster"));
    std::printf("  sentinel IT: wired, initial master %s:%u\n", host.c_str(),
                static_cast<unsigned>(initial_port));

    const std::string key =
        "crpc_failover_probe_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const crpc::result<crpc::redis::resp_value> set_reply =
        client.set(key, "before");
    CRPC_CHECK(command_succeeded(set_reply));
    if (!command_succeeded(set_reply)) {
        std::printf("  sentinel IT: SET failed: %s\n",
                    set_reply.ok() ? set_reply.value().as_error().c_str()
                                   : set_reply.error().message.c_str());
        std::fflush(stdout);
        return;
    }
    std::string before;
    CRPC_CHECK(read_text(client.get(key), "initial GET", before));
    CRPC_CHECK_EQ(std::string("before"), before);
    std::printf("  sentinel IT: write/read on the initial master OK\n");
    std::fflush(stdout);

    // Prove the pool currently talks to the master on 6380 via CLIENT INFO's
    // laddr, and hold one connection across the failover: it is the "old"
    // connection that must stop being writable afterwards.
    crpc::result<std::shared_ptr<crpc::redis::connection>> old = client.acquire();
    CRPC_CHECK(old.ok());
    std::printf("  sentinel IT: held old link open=%d\n",
                old.ok() ? (old.value()->is_open() ? 1 : 0) : -1);
    std::fflush(stdout);
    if (old.ok()) {
        crpc::redis::command_channel channel(*old.value());
        channel.set_protocol(client_opts.protocol);
        crpc::result<crpc::redis::resp_value> info =
            channel.execute({"CLIENT", "INFO"});
        std::printf("  sentinel IT: CLIENT INFO result ok=%d bulk=%d\n",
                    info.ok() ? 1 : 0,
                    info.ok() ? (info.value().is_bulk_string() ? 1 : 0) : -1);
        std::fflush(stdout);
        CRPC_CHECK(info.ok());
        std::string old_info;
        if (read_text(info, "CLIENT INFO before switch", old_info)) {
            const std::string expected_laddr =
                "laddr=" + host + ":" + std::to_string(initial_port);
            // Print what the server actually reported, so a mismatch names the
            // endpoint the pool is really on instead of only the expected one.
            const std::size_t at = old_info.find("laddr=");
            const std::string actual_laddr =
                at == std::string::npos ? std::string("<no laddr>")
                                        : old_info.substr(at, old_info.find('\n', at) - at);
            std::printf("  sentinel IT: old link expected=%s actual=%s\n",
                        expected_laddr.c_str(), actual_laddr.c_str());
            std::fflush(stdout);
            CRPC_CHECK(old_info.find(expected_laddr) != std::string::npos);
        }
    }

    if (trigger == "kill") {
        std::printf("  sentinel IT: kill the master on port %u now (trigger=kill)\n",
                    static_cast<unsigned>(initial_port));
        std::fflush(stdout);
    } else {
        std::printf("  sentinel IT: issuing SENTINEL FAILOVER...\n");
        std::fflush(stdout);
        // A dedicated RESP2 connection to the Sentinel daemon issues the
        // FAILOVER; the daemon then promotes the replica and bumps its epoch.
        crpc::redis::client::options sentinel_opts = client_opts;
        sentinel_opts.pool.connection.ep =
            crpc::endpoint::tcp(host, sentinel_port);
        sentinel_opts.protocol = crpc::redis::resp_protocol::resp2;
        sentinel_opts.pool.connection.operation_timeout = std::chrono::seconds(10);
        crpc::redis::client sentinel_cmd(sentinel_opts, backend);
        crpc::result<crpc::redis::resp_value> failover =
            sentinel_cmd.execute({"SENTINEL", "FAILOVER", "mymaster"});
        if (!failover.ok()) {
            const std::string reason =
                "SENTINEL FAILOVER did not reach the daemon: "
                + failover.error().message;
            crpc_test::skip(reason.c_str());
        }
        if (failover.value().is_error()) {
            const std::string reason =
                "SENTINEL FAILOVER refused by the daemon: "
                + failover.value().as_error();
            crpc_test::skip(reason.c_str());
        }
        std::printf("  sentinel IT: SENTINEL FAILOVER issued\n");
    }

    // Drive the library monitor until it observes the master switch (or the
    // deadline elapses, in which case the assertion below reports it).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(40);
    std::size_t polls = 0;
    while (client.stats().switch_count == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        monitor.poll_once();
        ++polls;
        std::this_thread::sleep_for(200ms);
    }
    std::printf("  sentinel IT: master switch observed after %zu poll(s)\n",
                polls);
    CRPC_CHECK(client.stats().switch_count >= 1);
    CRPC_CHECK_EQ(expected_new.host, client.master().host);
    CRPC_CHECK_EQ(expected_new.port, client.master().port);

    // Following the address is the client's job; finishing the promotion is the
    // daemon's. Sentinel can publish the new master while that node still
    // answers READONLY, so wait for the node itself before judging the pool.
    CRPC_CHECK(wait_until_master(backend, host, next_port, std::chrono::seconds(30)));

    // The pool now serves the new master: a write lands there and reads back.
    const crpc::result<crpc::redis::resp_value> after_set = client.set(key, "after");
    CRPC_CHECK(command_succeeded(after_set));
    if (!command_succeeded(after_set)) {
        std::printf("  sentinel IT: post-switch SET failed: %s\n",
                    after_set.ok() ? after_set.value().as_error().c_str()
                                   : after_set.error().message.c_str());
        std::fflush(stdout);
    }
    std::string after;
    CRPC_CHECK(read_text(client.get(key), "post-switch GET", after));
    CRPC_CHECK_EQ(std::string("after"), after);

    // The new connection is a RESP3 connection and really sits on 6381
    // (CLIENT INFO shows the laddr of the endpoint the pool talks to).
    crpc::result<std::shared_ptr<crpc::redis::connection>> fresh =
        client.acquire();
    CRPC_CHECK(fresh.ok());
    if (fresh.ok()) {
        crpc::redis::command_channel channel(*fresh.value());
        channel.set_protocol(crpc::redis::resp_protocol::resp3);
        // An empty hash replies as a RESP3 map on a negotiated connection; a
        // RESP2 link would answer a flat empty array. Proves the post-switch
        // connection still speaks RESP3 after its HELLO handshake.
        crpc::result<crpc::redis::resp_value> empty_hash =
            channel.execute({"HGETALL", "crpc_failover_nosuch"});
        CRPC_CHECK(empty_hash.ok());
        if (empty_hash.ok()) {
            CRPC_CHECK(empty_hash.value().is_map());
        }
        crpc::result<crpc::redis::resp_value> info =
            channel.execute({"CLIENT", "INFO"});
        CRPC_CHECK(info.ok());
        std::string new_info;
        if (read_text(info, "CLIENT INFO after switch", new_info)) {
            const std::string new_laddr =
                "laddr=" + host + ":" + std::to_string(next_port);
            CRPC_CHECK(new_info.find(new_laddr) != std::string::npos);
        }
        client.release(std::move(fresh).value());
    }

    // The old connection was stopped: writing through it must fail. After a
    // graceful failover the old master is a read-only replica, so the write is
    // a Redis READONLY error; after a kill it is a transport failure. Both are
    // explicit; neither is a silent write to a demoted node.
    if (old.ok()) {
        crpc::redis::command_channel channel(*old.value());
        channel.set_protocol(client_opts.protocol);
        crpc::result<crpc::redis::resp_value> old_write =
            channel.execute({"SET", key, "should-not-stick"});
        CRPC_CHECK_FALSE(old_write.ok() && !old_write.value().is_error());
        if (old_write.ok()) {
            CRPC_CHECK(old_write.value().as_error() != std::string{});
        }
        const std::string old_reason =
            old_write.ok() ? ("error value: " + old_write.value().as_error())
                           : old_write.error().message;
        std::printf("  old-master write after switch: %s\n", old_reason.c_str());
        client.release(std::move(old).value());
    }

    // The value on the new master is intact (the old write did not stick).
    std::string intact;
    CRPC_CHECK(read_text(client.get(key), "final GET", intact));
    CRPC_CHECK_EQ(std::string("after"), intact);
    CRPC_CHECK(command_succeeded(client.del({key})));

    const crpc::redis::switch_stats stats = client.stats();
    std::printf(
        "  sentinel failover: switches=%llu rejected_during_switch=%llu "
        "master=%s:%u\n",
        static_cast<unsigned long long>(stats.switch_count),
        static_cast<unsigned long long>(stats.rejected_during_switch),
        stats.master.host.c_str(), stats.master.port);
}

CRPC_TEST(it_command_table_matches_the_real_server) {
    using crpc::redis::command_spec;
    using crpc::redis::k_command_table;
    using crpc::redis::k_command_table_size;
    using crpc::redis::lookup_command;
    using crpc::redis::resp_value;
    (void)require_redis();
    tcp_backend backend;
    crpc::redis::client::options opts = client_options();
    crpc::redis::client client(opts, backend);
    CRPC_CHECK(client.connect().ok());

    // Ask the live server for its own metadata on every command the library
    // can emit, and compare the table row by row. `COMMAND INFO <name>`
    // replies with a single-element array holding [name, arity, flags...,
    // first_key, last_key, step], exactly the fields the table stores.
    std::vector<std::string> args{"COMMAND", "INFO"};
    for (std::size_t i = 0; i < k_command_table_size; ++i) {
        args.push_back(std::string(k_command_table[i].name));
    }
    crpc::result<resp_value> reply =
        client.command(crpc::span<const std::string>(args.data(), args.size()));
    CRPC_CHECK(reply.ok());
    CRPC_CHECK(!reply.value().is_error());

    // The server reply has one array per requested command. Unsupported
    // commands come back shortened; only a full-length row is comparable.
    // `COMMAND` reports names lowercased ("get"); the table stores the canonical
    // uppercase, so fold before the lookup.
    std::size_t checked = 0;
    std::vector<std::string> mismatches;
    const auto walk = [&](const crpc::redis::resp_value& array) {
        using crpc::redis::resp_value;
        const std::vector<resp_value>& items = array.as_array();
        if (items.size() < 6) {
            return; // the server does not know this command
        }
        const std::string raw = items[0].as_bulk_string();
        std::string name = raw;
        for (char& c : name) {
            c = static_cast<char>(
                std::toupper(static_cast<unsigned char>(c)));
        }
        const command_spec* spec = lookup_command(name);
        CRPC_CHECK(spec != nullptr);
        if (spec == nullptr) {
            return;
        }

        std::int64_t arity = items[1].as_integer();
        std::int64_t first_key = items[3].as_integer();
        std::int64_t last_key = items[4].as_integer();
        std::int64_t key_step = items[5].as_integer();

        if (arity != spec->arity || first_key != spec->first_key ||
            last_key != spec->last_key || key_step != spec->key_step) {
            mismatches.push_back(
                name + ": table(" + std::to_string(spec->arity) + "," +
                std::to_string(spec->first_key) + "," +
                std::to_string(spec->last_key) + "," +
                std::to_string(spec->key_step) + ") server(" +
                std::to_string(arity) + "," + std::to_string(first_key) + "," +
                std::to_string(last_key) + "," + std::to_string(key_step) + ")");
            return;
        }
        ++checked;
    };

    if (reply.value().is_array()) {
        const std::vector<crpc::redis::resp_value>& outer = reply.value().as_array();
        for (const crpc::redis::resp_value& row : outer) {
            walk(row);
        }
    }
    for (const std::string& m : mismatches) {
        std::printf("  command table vs server: %s\n", m.c_str());
    }
    CRPC_CHECK(mismatches.empty());
    // Every row must have been compared against a full server reply. A server
    // too old to know a command is a real signal, not a skip.
    CRPC_CHECK_EQ(checked, k_command_table_size);
    std::printf("  command table: %zu rows verified against live server\n",
                checked);
}

CRPC_TEST_MAIN()
