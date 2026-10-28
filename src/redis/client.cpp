// CRPC Redis -- asynchronous client facade.
#include <crpc/redis/client.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "reply_util.hpp"

namespace crpc {
namespace redis {
namespace {

// Builds a RESP command: a name followed by bulk-string arguments. It is a
// tiny local helper so the typed methods read like the Redis command they send.
class command_builder {
public:
    explicit command_builder(std::string name) {
        command_.push_back(std::move(name));
    }

    command_builder& arg(std::string value) {
        command_.push_back(std::move(value));
        return *this;
    }

    command_builder& arg(std::int64_t value) { return arg(std::to_string(value)); }

    command_builder& arg(double value) { return arg(std::to_string(value)); }

    command_builder& arg(std::chrono::seconds value) {
        return arg(static_cast<std::int64_t>(value.count()));
    }

    resp_command build() { return std::move(command_); }

private:
    resp_command command_;
};

// Returns a pooled connection to its pool when the enclosing scope ends, so an
// early return cannot leak a checked-out connection.
struct pooled_connection {
    connection_pool& pool;
    std::shared_ptr<connection> conn;
    ~pooled_connection() { pool.release(std::move(conn)); }
};

// Turns a command reply into a health status: a transport failure or a Redis
// error reply is a failure, a well-formed reply is success.
status classify(const result<resp_value>& reply) {
    if (!reply) {
        return status{reply.code()};
    }
    if (reply.value().is_error()) {
        return status{rpc_error{rpc_code::application, reply.value().as_error()}};
    }
    return status{};
}

// Reads a text field out of a HELLO map entry, folding the string
// alternatives. Returns nullptr for a non-text value (the integer `proto` or
// the array `modules`).
const std::string* map_text_value(const resp_value& value) {
    return detail::is_text(value) ? &detail::as_text(value) : nullptr;
}

// Sends `HELLO 3` and turns the reply into `hello_info`. A transport or
// framing failure fails the result; a server error reply (Redis < 6 has no
// HELLO command) fails with a clear "RESP3 not supported" error; a reply that
// is not a map, or that negotiates a proto other than 3, is a protocol error.
// A failure never downgrades the session silently.
result<hello_info> negotiate_resp3(connection& conn) {
    command_channel channel(conn);
    channel.set_protocol(resp_protocol::resp3);
    result<resp_value> reply = channel.execute(resp_command{"HELLO", "3"});
    if (!reply) {
        return reply.error();
    }
    const resp_value& value = reply.value();
    if (detail::is_error_reply(value)) {
        return rpc_error{
            rpc_code::failed_precondition,
            "RESP3 negotiation failed: server answered HELLO 3 with an error (\""
                + detail::error_message(value)
                + "\"); RESP3 requires Redis 6 or newer"};
    }
    if (!value.is_map()) {
        return rpc_error{rpc_code::protocol_error,
                         "RESP3 negotiation failed: HELLO 3 did not return a map"};
    }
    hello_info info;
    bool saw_proto = false;
    for (const auto& entry : value.as_map()) {
        const std::string* key = map_text_value(entry.first);
        if (key == nullptr) {
            continue;
        }
        const std::string* field = map_text_value(entry.second);
        if (*key == "server") {
            if (field != nullptr) {
                info.server = *field;
            }
        } else if (*key == "version") {
            if (field != nullptr) {
                info.version = *field;
            }
        } else if (*key == "proto") {
            if (entry.second.is_integer()) {
                info.proto = entry.second.as_integer();
                saw_proto = true;
            }
        } else if (*key == "mode") {
            if (field != nullptr) {
                info.mode = *field;
            }
        } else if (*key == "role") {
            if (field != nullptr) {
                info.role = *field;
            }
        }
    }
    if (!saw_proto || info.proto != 3) {
        return rpc_error{rpc_code::protocol_error,
                         "RESP3 negotiation failed: server advertised proto "
                             + std::to_string(info.proto) + " instead of 3"};
    }
    return info;
}

} // namespace

struct client::impl {
    options opts;
    crpc::backend_factory& backend;
    runtime* shard;
    connection_pool pool;

    // RESP3 negotiation state. `negotiated` maps a live connection to the link
    // generation at which `HELLO 3` was last sent; the weak_ptr guard tells a
    // recycled heap address from the same connection object, so a stale entry
    // never lets a fresh connection skip its handshake. `last_hello` is the
    // most recent successful negotiation. Both are guarded by `negotiate_mutex`.
    mutable std::mutex negotiate_mutex;
    std::unordered_map<const connection*,
                       std::pair<std::weak_ptr<connection>, std::uint64_t>>
        negotiated;
    hello_info last_hello;

    impl(options o, crpc::backend_factory& b, runtime* rt)
        : opts(std::move(o)), backend(b), shard(rt),
          pool(opts.pool, backend, shard) {}

    // Ensures `conn` runs the requested RESP dialect. RESP2 needs no handshake
    // and is a no-op. RESP3 sends `HELLO 3` once per connection link
    // generation; a later acquire of the same live link skips the round trip.
    result<hello_info> ensure_negotiated(const std::shared_ptr<connection>& conn,
                                         resp_protocol protocol);
    // Acquires a pooled connection and, for RESP3, negotiates it before handing
    // it out. A negotiation failure closes the acquired connection and returns
    // it to the pool (which drops a closed link), so a refused session cannot
    // linger as a silent RESP2 fallback.
    result<std::shared_ptr<connection>> acquire_negotiated(resp_protocol protocol);
    result<resp_value> execute_over_pool(resp_protocol protocol,
                                         const resp_command& cmd);
    result<std::vector<resp_value>> execute_batch_over_pool(
        resp_protocol protocol, const std::vector<resp_command>& commands);
};

result<hello_info> client::impl::ensure_negotiated(
    const std::shared_ptr<connection>& conn, resp_protocol protocol) {
    if (protocol != resp_protocol::resp3) {
        return hello_info{};
    }
    const connection* raw = conn.get();
    const std::uint64_t generation = conn->reconnect_count();
    {
        std::lock_guard<std::mutex> lock(negotiate_mutex);
        const auto entry = negotiated.find(raw);
        if (entry != negotiated.end()) {
            const std::shared_ptr<connection> alive = entry->second.first.lock();
            if (alive != nullptr && entry->second.second == generation) {
                return last_hello;
            }
            // A recycled heap address or a newer link generation: renegotiate.
            negotiated.erase(entry);
        }
    }

    result<hello_info> info = negotiate_resp3(*conn);
    if (!info) {
        return info.error();
    }

    std::lock_guard<std::mutex> lock(negotiate_mutex);
    negotiated[raw] = {conn, generation};
    last_hello = info.value();
    return info;
}

result<std::shared_ptr<connection>> client::impl::acquire_negotiated(
    resp_protocol protocol) {
    result<std::shared_ptr<connection>> acquired = pool.acquire();
    if (!acquired) {
        return acquired.error();
    }
    result<hello_info> handshake = ensure_negotiated(acquired.value(), protocol);
    if (!handshake) {
        acquired.value()->close();
        pool.release(acquired.value());
        return handshake.error();
    }
    return acquired;
}

result<resp_value> client::impl::execute_over_pool(resp_protocol protocol,
                                                   const resp_command& cmd) {
    result<std::shared_ptr<connection>> acquired = acquire_negotiated(protocol);
    if (!acquired) {
        return acquired.error();
    }
    pooled_connection guard{pool, std::move(acquired).value()};
    command_channel channel(*guard.conn);
    channel.set_protocol(protocol);
    return channel.execute(cmd);
}

result<std::vector<resp_value>> client::impl::execute_batch_over_pool(
    resp_protocol protocol, const std::vector<resp_command>& commands) {
    if (commands.empty()) {
        return std::vector<resp_value>{};
    }
    result<std::shared_ptr<connection>> acquired = acquire_negotiated(protocol);
    if (!acquired) {
        return acquired.error();
    }
    pooled_connection guard{pool, std::move(acquired).value()};
    command_channel channel(*guard.conn);
    channel.set_protocol(protocol);
    return channel.execute_batch(commands);
}

client::client(options opts, crpc::backend_factory& backend, runtime* shard)
    : impl_(std::make_unique<impl>(std::move(opts), backend, shard)) {}

client::~client() = default;

status client::connect() {
    result<std::shared_ptr<connection>> acquired =
        impl_->acquire_negotiated(impl_->opts.protocol);
    if (!acquired) {
        return status{acquired.error()};
    }
    std::shared_ptr<connection> conn = std::move(acquired).value();
    command_channel channel(*conn);
    channel.set_protocol(impl_->opts.protocol);
    result<resp_value> reply = channel.execute(resp_command{"PING"});
    impl_->pool.release(std::move(conn));
    return classify(reply);
}

void client::close() noexcept {
    impl_->pool.close();
    try {
        std::lock_guard<std::mutex> lock(impl_->negotiate_mutex);
        impl_->negotiated.clear();
    } catch (...) {
        // close() is noexcept; a mutex failure cannot break the shutdown
        // contract, and the stale entries self-heal through their weak_ptr.
    }
}

bool client::is_open() const noexcept { return impl_->pool.total() > 0; }

status client::health_check() { return classify(ping()); }

result<std::shared_ptr<connection>> client::acquire() {
    return impl_->acquire_negotiated(impl_->opts.protocol);
}

void client::release(std::shared_ptr<connection> conn) {
    impl_->pool.release(std::move(conn));
}

hello_info client::hello() const {
    std::lock_guard<std::mutex> lock(impl_->negotiate_mutex);
    return impl_->last_hello;
}

std::size_t client::idle() const noexcept { return impl_->pool.idle(); }

std::size_t client::in_use() const noexcept { return impl_->pool.in_use(); }

std::size_t client::total() const noexcept { return impl_->pool.total(); }

result<resp_value> client::execute(const resp_command& cmd) {
    return impl_->execute_over_pool(impl_->opts.protocol, cmd);
}

result<std::vector<resp_value>> client::execute_batch(
    const std::vector<resp_command>& commands) {
    return impl_->execute_batch_over_pool(impl_->opts.protocol, commands);
}

result<resp_value> client::command(span<const std::string> args) {
    if (args.empty()) {
        return rpc_error{rpc_code::invalid_argument,
                         "an empty Redis command cannot be sent"};
    }
    resp_command cmd;
    cmd.reserve(args.size());
    for (const std::string& arg : args) {
        cmd.push_back(arg);
    }
    return execute(cmd);
}

// --- typed convenience ---------------------------------------------------------

result<resp_value> client::ping() { return execute(resp_command{"PING"}); }

result<resp_value> client::get(std::string key) {
    return execute(command_builder("GET").arg(std::move(key)).build());
}

result<resp_value> client::set(std::string key, std::string value) {
    return execute(
        command_builder("SET").arg(std::move(key)).arg(std::move(value)).build());
}

result<resp_value> client::setex(std::string key, std::chrono::seconds seconds,
                                std::string value) {
    return execute(command_builder("SETEX")
                       .arg(std::move(key))
                       .arg(seconds)
                       .arg(std::move(value))
                       .build());
}

result<resp_value> client::del(std::vector<std::string> keys) {
    command_builder builder("DEL");
    for (std::string& key : keys) {
        builder.arg(std::move(key));
    }
    return execute(builder.build());
}

result<resp_value> client::exists(std::vector<std::string> keys) {
    command_builder builder("EXISTS");
    for (std::string& key : keys) {
        builder.arg(std::move(key));
    }
    return execute(builder.build());
}

result<resp_value> client::expire(std::string key, std::chrono::seconds seconds) {
    return execute(
        command_builder("EXPIRE").arg(std::move(key)).arg(seconds).build());
}

result<resp_value> client::ttl(std::string key) {
    return execute(command_builder("TTL").arg(std::move(key)).build());
}

result<resp_value> client::incr(std::string key) {
    return execute(command_builder("INCR").arg(std::move(key)).build());
}

result<resp_value> client::decr(std::string key) {
    return execute(command_builder("DECR").arg(std::move(key)).build());
}

result<resp_value> client::incrby(std::string key, std::int64_t amount) {
    return execute(
        command_builder("INCRBY").arg(std::move(key)).arg(amount).build());
}

result<resp_value> client::append(std::string key, std::string value) {
    return execute(
        command_builder("APPEND").arg(std::move(key)).arg(std::move(value)).build());
}

result<resp_value> client::strlen(std::string key) {
    return execute(command_builder("STRLEN").arg(std::move(key)).build());
}

result<resp_value> client::keys(std::string pattern) {
    return execute(command_builder("KEYS").arg(std::move(pattern)).build());
}

result<resp_value> client::mget(std::vector<std::string> keys) {
    command_builder builder("MGET");
    for (std::string& key : keys) {
        builder.arg(std::move(key));
    }
    return execute(builder.build());
}

result<resp_value> client::mset(
    std::vector<std::pair<std::string, std::string>> pairs) {
    command_builder builder("MSET");
    for (auto& pair : pairs) {
        builder.arg(std::move(pair.first)).arg(std::move(pair.second));
    }
    return execute(builder.build());
}

result<resp_value> client::hset(std::string key, std::string field,
                                std::string value) {
    return execute(command_builder("HSET")
                       .arg(std::move(key))
                       .arg(std::move(field))
                       .arg(std::move(value))
                       .build());
}

result<resp_value> client::hget(std::string key, std::string field) {
    return execute(
        command_builder("HGET").arg(std::move(key)).arg(std::move(field)).build());
}

result<resp_value> client::hdel(std::string key, std::vector<std::string> fields) {
    command_builder builder("HDEL");
    builder.arg(std::move(key));
    for (std::string& field : fields) {
        builder.arg(std::move(field));
    }
    return execute(builder.build());
}

result<resp_value> client::hexists(std::string key, std::string field) {
    return execute(command_builder("HEXISTS")
                       .arg(std::move(key))
                       .arg(std::move(field))
                       .build());
}

result<resp_value> client::hgetall(std::string key) {
    return execute(command_builder("HGETALL").arg(std::move(key)).build());
}

result<resp_value> client::lpush(std::string key, std::vector<std::string> values) {
    command_builder builder("LPUSH");
    builder.arg(std::move(key));
    for (std::string& value : values) {
        builder.arg(std::move(value));
    }
    return execute(builder.build());
}

result<resp_value> client::rpush(std::string key, std::vector<std::string> values) {
    command_builder builder("RPUSH");
    builder.arg(std::move(key));
    for (std::string& value : values) {
        builder.arg(std::move(value));
    }
    return execute(builder.build());
}

result<resp_value> client::lpop(std::string key) {
    return execute(command_builder("LPOP").arg(std::move(key)).build());
}

result<resp_value> client::rpop(std::string key) {
    return execute(command_builder("RPOP").arg(std::move(key)).build());
}

result<resp_value> client::llen(std::string key) {
    return execute(command_builder("LLEN").arg(std::move(key)).build());
}

result<resp_value> client::sadd(std::string key, std::vector<std::string> members) {
    command_builder builder("SADD");
    builder.arg(std::move(key));
    for (std::string& member : members) {
        builder.arg(std::move(member));
    }
    return execute(builder.build());
}

result<resp_value> client::srem(std::string key, std::vector<std::string> members) {
    command_builder builder("SREM");
    builder.arg(std::move(key));
    for (std::string& member : members) {
        builder.arg(std::move(member));
    }
    return execute(builder.build());
}

result<resp_value> client::smembers(std::string key) {
    return execute(command_builder("SMEMBERS").arg(std::move(key)).build());
}

result<resp_value> client::sismember(std::string key, std::string member) {
    return execute(command_builder("SISMEMBER")
                       .arg(std::move(key))
                       .arg(std::move(member))
                       .build());
}

result<resp_value> client::zadd(std::string key, double score, std::string member) {
    return execute(command_builder("ZADD")
                       .arg(std::move(key))
                       .arg(score)
                       .arg(std::move(member))
                       .build());
}

result<resp_value> client::zrange(std::string key, std::int64_t start,
                                  std::int64_t stop, bool with_scores) {
    command_builder builder("ZRANGE");
    builder.arg(std::move(key)).arg(start).arg(stop);
    if (with_scores) {
        builder.arg("WITHSCORES");
    }
    return execute(builder.build());
}

result<resp_value> client::zscore(std::string key, std::string member) {
    return execute(command_builder("ZSCORE")
                       .arg(std::move(key))
                       .arg(std::move(member))
                       .build());
}

} // namespace redis
} // namespace crpc
