// CRPC Redis -- asynchronous client facade.
//
// `connection` moves bytes, `command_channel` turns bytes into replies, and
// `connection_pool` keeps a set of connections to one endpoint warm. This
// header is the layer a caller actually talks to: it composes the pool and the
// command executor behind one asynchronous, type-aware API.
//
// Execution model
// ---------------
// Nothing here starts a thread. Every method acquires a pooled connection and
// wraps it in a `command_channel`; the channel's read/write calls park the
// calling coroutine on the bound shard and block a plain thread, exactly like
// `command_channel` itself. A `client` is therefore safe to use from a
// coroutine and from a plain thread with the same call shape.
//
// Errors
// ------
// A Redis error reply is a value (`resp_value` of type `resp_type::error`);
// only a transport, framing or local failure makes the `result` fail. The
// facade never throws.
#ifndef CRPC_REDIS_CLIENT_HPP
#define CRPC_REDIS_CLIENT_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/redis/connection.hpp>
#include <crpc/redis/pipeline.hpp>
#include <crpc/redis/resp.hpp>

namespace crpc {
namespace redis {

// A high-level asynchronous Redis client: a connection pool plus a typed
// command API. It owns the pool; the backend and shard are borrowed and must
// outlive the client.
class CRPC_API client {
public:
    // Every field is a plain value, so a client's behaviour can be tuned at run
    // time without recompiling the caller.
    struct options {
        // Pool policy (size, acquire timeout) and the configuration cloned into
        // every connection the pool creates.
        pool_config pool{};

        // RESP dialect used for replies. Commands are always arrays of bulk
        // strings, which are identical in RESP2 and RESP3.
        resp_protocol protocol = resp_protocol::resp2;
    };

    client(options opts, crpc::backend_factory& backend, runtime* shard = nullptr);
    ~client();

    client(const client&) = delete;
    client& operator=(const client&) = delete;
    client(client&&) = delete;
    client& operator=(client&&) = delete;

    // --- Connection management -------------------------------------------------

    // Establishes a pooled connection and verifies it with a PING. Returns the
    // transport error when the endpoint cannot be reached; the pool is left
    // empty so a later retry can still succeed.
    status connect();

    // Closes every idle connection and rejects further acquires with
    // `shutting_down`. Idempotent.
    void close() noexcept;

    // True while the pool holds at least one connection.
    [[nodiscard]] bool is_open() const noexcept;

    // Sends PING and reports `ok` only when the round trip succeeded and the
    // reply was not a Redis error. This is the health probe.
    [[nodiscard]] status health_check();

    // Direct pool access for callers that need to hold a connection across
    // several commands. `acquire` parks a coroutine when the pool is
    // exhausted; `release` returns the connection (closing it if it died).
    [[nodiscard]] result<std::shared_ptr<connection>> acquire();
    void release(std::shared_ptr<connection> conn);

    // Pool observability.
    [[nodiscard]] std::size_t idle() const noexcept;
    [[nodiscard]] std::size_t in_use() const noexcept;
    [[nodiscard]] std::size_t total() const noexcept;

    // --- Command execution -----------------------------------------------------

    // Sends one command and returns its reply. A Redis error reply is returned
    // as a `resp_value` of type `resp_type::error`.
    [[nodiscard]] result<resp_value> execute(const command& cmd);

    // Sends every command in one batch and returns replies in command order.
    // An empty batch returns an empty vector without touching the transport.
    [[nodiscard]] result<std::vector<resp_value>> execute_batch(
        const std::vector<command>& commands);

    // --- Typed convenience -----------------------------------------------------
    //
    // Each method builds the command from its arguments, so a caller never has
    // to spell out the RESP array. The return value is the raw reply; callers
    // that want a typed projection inspect the `resp_value` themselves.

    [[nodiscard]] result<resp_value> ping();

    // Strings.
    [[nodiscard]] result<resp_value> get(std::string key);
    [[nodiscard]] result<resp_value> set(std::string key, std::string value);
    [[nodiscard]] result<resp_value> setex(std::string key,
                                          std::chrono::seconds seconds,
                                          std::string value);
    [[nodiscard]] result<resp_value> del(std::vector<std::string> keys);
    [[nodiscard]] result<resp_value> exists(std::vector<std::string> keys);
    [[nodiscard]] result<resp_value> expire(std::string key,
                                            std::chrono::seconds seconds);
    [[nodiscard]] result<resp_value> ttl(std::string key);
    [[nodiscard]] result<resp_value> incr(std::string key);
    [[nodiscard]] result<resp_value> decr(std::string key);
    [[nodiscard]] result<resp_value> incrby(std::string key, std::int64_t amount);
    [[nodiscard]] result<resp_value> append(std::string key, std::string value);
    [[nodiscard]] result<resp_value> strlen(std::string key);
    [[nodiscard]] result<resp_value> keys(std::string pattern);
    [[nodiscard]] result<resp_value> mget(std::vector<std::string> keys);
    [[nodiscard]] result<resp_value> mset(
        std::vector<std::pair<std::string, std::string>> pairs);

    // Hashes.
    [[nodiscard]] result<resp_value> hset(std::string key, std::string field,
                                          std::string value);
    [[nodiscard]] result<resp_value> hget(std::string key, std::string field);
    [[nodiscard]] result<resp_value> hdel(std::string key,
                                          std::vector<std::string> fields);
    [[nodiscard]] result<resp_value> hexists(std::string key, std::string field);
    [[nodiscard]] result<resp_value> hgetall(std::string key);

    // Lists.
    [[nodiscard]] result<resp_value> lpush(std::string key,
                                           std::vector<std::string> values);
    [[nodiscard]] result<resp_value> rpush(std::string key,
                                           std::vector<std::string> values);
    [[nodiscard]] result<resp_value> lpop(std::string key);
    [[nodiscard]] result<resp_value> rpop(std::string key);
    [[nodiscard]] result<resp_value> llen(std::string key);

    // Sets.
    [[nodiscard]] result<resp_value> sadd(std::string key,
                                          std::vector<std::string> members);
    [[nodiscard]] result<resp_value> srem(std::string key,
                                          std::vector<std::string> members);
    [[nodiscard]] result<resp_value> smembers(std::string key);
    [[nodiscard]] result<resp_value> sismember(std::string key, std::string member);

    // Sorted sets.
    [[nodiscard]] result<resp_value> zadd(std::string key, double score,
                                          std::string member);
    [[nodiscard]] result<resp_value> zrange(std::string key, std::int64_t start,
                                            std::int64_t stop,
                                            bool with_scores = false);
    [[nodiscard]] result<resp_value> zscore(std::string key, std::string member);

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace redis
} // namespace crpc

#endif // CRPC_REDIS_CLIENT_HPP
