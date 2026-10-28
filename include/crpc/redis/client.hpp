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
#include <crpc/redis/sentinel.hpp>

namespace crpc {
namespace redis {

// The server half of a successful `HELLO` reply, as reported by
// `client::hello()`. `proto` is the negotiated protocol version (3 on a RESP3
// handshake); `server`, `version`, `mode` and `role` are the values the server
// advertised. A RESP2 client never sends HELLO, so every field stays empty and
// `proto` stays 0.
struct hello_info {
    std::string server;
    std::string version;
    std::int64_t proto = 0;
    std::string mode;
    std::string role;
};

// A point-in-time view of the pool's master-tracking state: where the pool is
// pointed, whether a master switch is pending, and the counters a switch
// produces. `master` mirrors `sentinel_address` of the current endpoint (a TCP
// host/port, or the in-process transport name with port 0). Returned by value
// like `hello_info`, so the layout comes from this header on both sides of a
// shared boundary.
struct switch_stats {
    sentinel_address master;
    // True while the pool is mid-switch: `redirect()` has repointed the pool and
    // the new master has not yet answered a command. Requests that fail at the
    // transport level during this window return `unavailable` and bump
    // `rejected_during_switch`; they are never served from the old master.
    bool switching = false;
    // Successful redirects (an identical re-notification is not a switch).
    std::uint64_t switch_count = 0;
    // Requests rejected because they arrived during a switch window and the new
    // master did not answer.
    std::uint64_t rejected_during_switch = 0;
    // Time of the most recent redirect, or the default time point when no
    // redirect has happened yet.
    std::chrono::steady_clock::time_point last_switch{};
};

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
        //
        // RESP2 (the default) needs no handshake. RESP3 is negotiated on every
        // connection when it is first used: the client sends `HELLO 3` and,
        // when a server refuses it (a Redis before 6 has no HELLO command),
        // fails the call with a clear error instead of silently downgrading.
        resp_protocol protocol = resp_protocol::resp2;
    };

    client(options opts, crpc::backend_factory& backend, runtime* shard = nullptr);
    ~client();

    client(const client&) = delete;
    client& operator=(const client&) = delete;
    client(client&&) = delete;
    client& operator=(client&&) = delete;

    // --- Connection management -------------------------------------------------

    // Establishes a pooled connection, negotiates the requested protocol, and
    // verifies it with a PING. Returns the transport error when the endpoint
    // cannot be reached, or the negotiation error when a RESP3 client meets a
    // server without HELLO; the pool is left empty so a later retry can still
    // succeed.
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
    // exhausted; a RESP3 client that acquires a connection which has not
    // negotiated on this link yet sends `HELLO 3` before returning, so a raw
    // `command_channel` on the result runs on the requested protocol.
    // `release` returns the connection (closing it if it died).
    [[nodiscard]] result<std::shared_ptr<connection>> acquire();
    void release(std::shared_ptr<connection> conn);

    // The values advertised by the most recent successful `HELLO` handshake on
    // this endpoint's connections. Empty until a RESP3 client negotiates; see
    // `hello_info`.
    [[nodiscard]] hello_info hello() const;

    // --- Sentinel-aware master redirection -----------------------------------

    // Repoints the whole pool at `new_master`. This is the receiving half of
    // the Sentinel bridge: wire a `sentinel::master_switch` event handler (or
    // use `follow()`) to call it with the new master address.
    //
    // Semantics of a switch:
    //   * the generation of connections aimed at the old master is retired
    //     immediately -- its idle connections are closed and a connection a
    //     caller returns afterwards is closed, never re-lent, so no new request
    //     observes an old-master link;
    //   * commands already in flight on a connection the caller holds complete
    //     normally (they were accepted before the switch);
    //   * the next acquire builds a fresh connection to the new endpoint, and a
    //     RESP3 client negotiates `HELLO 3` on it before use;
    //   * until the new master answers one command, the pool is "switching": a
    //     request that fails at the transport level returns `unavailable` and
    //     is counted in `switch_stats::rejected_during_switch`. It is never
    //     silently served from the old master.
    // Idempotent: a redirect to the endpoint already in use is a no-op and does
    // not rebuild the pool. Thread-safe and non-blocking. An empty host/port is
    // `invalid_argument`; a redirect after `close()` is `shutting_down`.
    status redirect(const sentinel_address& new_master);

    // Same as above, speaking the transport's own endpoint type. The Sentinel
    // overload is sugar for `endpoint::tcp(host, port)`; a caller that manages
    // endpoints directly (for example the in-process test transport) uses this.
    status redirect(const endpoint& new_endpoint);

    // Registers this client as the Sentinel `master_switch` handler for
    // `master_name`: every such event emitted by `target` calls
    // `redirect(event.master)`. It takes over the sentinel's single handler
    // slot (see `sentinel::set_event_handler`), so a second `follow` on the
    // same sentinel replaces the first. The handler keeps a weak reference to
    // the client, so destroying the client before the sentinel is safe; the
    // sentinel must outlive the client's use of `follow` (a destroyed sentinel
    // simply stops emitting). Returns false when `target` cannot receive a
    // handler (null) or `master_name` is empty.
    bool follow(sentinel& target, const std::string& master_name = "mymaster");

    // Snapshot of the master-tracking state (address, switch counters).
    [[nodiscard]] switch_stats stats() const;

    // The endpoint the pool currently points at (`stats().master`).
    [[nodiscard]] sentinel_address master() const;

    // True while a redirect is pending confirmation (`stats().switching`).
    [[nodiscard]] bool switching() const noexcept;

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

    // Executes an arbitrary command: `args[0]` is the command name and the
    // rest are its arguments. This is the escape hatch for commands the typed
    // facade does not wrap. Each argument is encoded as a binary-safe bulk
    // string, so a value containing CR/LF, spaces or a fake `*N\n...` frame
    // cannot be split into extra arguments or spliced into a second command.
    // An empty `args` is rejected with `rpc_code::invalid_argument`.
    [[nodiscard]] result<resp_value> command(span<const std::string> args);

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
    // Shared, not unique: `follow()` hands the Sentinel a weak reference so an
    // event that fires after the client is destroyed is dropped instead of
    // dereferencing a dead object. The client is non-copyable/non-movable, so
    // the shared_ptr cannot be duplicated through the public interface.
    struct impl;
    std::shared_ptr<impl> impl_;
};

} // namespace redis
} // namespace crpc

#endif // CRPC_REDIS_CLIENT_HPP
