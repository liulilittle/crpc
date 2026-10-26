// CRPC Redis -- asynchronous client facade.
#include <crpc/redis/client.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

result<resp_value> execute_over_pool(connection_pool& pool, resp_protocol protocol,
                                     const command& cmd) {
    result<std::shared_ptr<connection>> acquired = pool.acquire();
    if (!acquired) {
        return acquired.error();
    }
    pooled_connection guard{pool, std::move(acquired).value()};
    command_channel channel(*guard.conn);
    channel.set_protocol(protocol);
    return channel.execute(cmd);
}

result<std::vector<resp_value>> execute_batch_over_pool(
    connection_pool& pool, resp_protocol protocol,
    const std::vector<command>& commands) {
    if (commands.empty()) {
        return std::vector<resp_value>{};
    }
    result<std::shared_ptr<connection>> acquired = pool.acquire();
    if (!acquired) {
        return acquired.error();
    }
    pooled_connection guard{pool, std::move(acquired).value()};
    command_channel channel(*guard.conn);
    channel.set_protocol(protocol);
    return channel.execute_batch(commands);
}

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

} // namespace

struct client::impl {
    options opts;
    crpc::backend_factory& backend;
    runtime* shard;
    connection_pool pool;

    impl(options o, crpc::backend_factory& b, runtime* rt)
        : opts(std::move(o)), backend(b), shard(rt),
          pool(opts.pool, backend, shard) {}
};

client::client(options opts, crpc::backend_factory& backend, runtime* shard)
    : impl_(std::make_unique<impl>(std::move(opts), backend, shard)) {}

client::~client() = default;

status client::connect() {
    result<std::shared_ptr<connection>> acquired = impl_->pool.acquire();
    if (!acquired) {
        return status{acquired.code()};
    }
    std::shared_ptr<connection> conn = std::move(acquired).value();
    command_channel channel(*conn);
    channel.set_protocol(impl_->opts.protocol);
    result<resp_value> reply = channel.execute(command{"PING"});
    impl_->pool.release(std::move(conn));
    return classify(reply);
}

void client::close() noexcept { impl_->pool.close(); }

bool client::is_open() const noexcept { return impl_->pool.total() > 0; }

status client::health_check() { return classify(ping()); }

result<std::shared_ptr<connection>> client::acquire() { return impl_->pool.acquire(); }

void client::release(std::shared_ptr<connection> conn) {
    impl_->pool.release(std::move(conn));
}

std::size_t client::idle() const noexcept { return impl_->pool.idle(); }

std::size_t client::in_use() const noexcept { return impl_->pool.in_use(); }

std::size_t client::total() const noexcept { return impl_->pool.total(); }

result<resp_value> client::execute(const command& cmd) {
    return execute_over_pool(impl_->pool, impl_->opts.protocol, cmd);
}

result<std::vector<resp_value>> client::execute_batch(
    const std::vector<command>& commands) {
    return execute_batch_over_pool(impl_->pool, impl_->opts.protocol, commands);
}

// --- typed convenience ---------------------------------------------------------

result<resp_value> client::ping() { return execute(command{"PING"}); }

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
