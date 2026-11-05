// CRPC -- Redis command metadata table.
//
// One checked-in table of the commands this library emits, holding what the
// server itself reports for each command. The numbers below were read from a
// live Redis 7.0.15 `COMMAND` reply, not from memory; see
// `tests/test_redis_command_table.cpp` for the rule that they stay in sync with
// a real server.
//
// Each entry carries the five `COMMAND` fields that matter to a client:
//   * arity       -- count of arguments including the command name; negative
//                    means "at least |arity| arguments".
//   * first_key, last_key, key_step -- which arguments are key names, using the
//                    server's convention (0 = no key; negative last_key = "all
//                    remaining" with step).
//
// The set of names is the emitted surface: `src/redis/*.cpp` and the typed
// builders in `include/crpc/redis/*.hpp`. A guard test rescans those sources
// and fails when a command is emitted without a table row, so the table cannot
// drift away from what the library actually sends.

#ifndef CRPC_REDIS_COMMAND_TABLE_HPP
#define CRPC_REDIS_COMMAND_TABLE_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace crpc {
namespace redis {

struct command_spec {
    std::string_view name;
    // Total argument count including the command name. Negative when Redis
    // accepts any count >= |arity| (the server's own convention).
    std::int16_t arity;
    // Key positions in the server's convention: 0 means the command takes no
    // key; `last_key < 0` means "through the last argument"; both use 1-based
    // indexing as the server reports them.
    std::int16_t first_key;
    std::int16_t last_key;
    std::int16_t key_step;
};

// The complete emitted surface as of this revision. Kept sorted by name.
inline constexpr command_spec k_command_table[] = {
    {"APPEND", 3, 1, 1, 1},
    {"ASKING", 1, 0, 0, 0},
    {"CLUSTER", -2, 0, 0, 0},
    {"DECR", 2, 1, 1, 1},
    {"DECRBY", 3, 1, 1, 1},
    {"DEL", -2, 1, -1, 1},
    {"DISCARD", 1, 0, 0, 0},
    {"EVAL", -3, 0, 0, 0},
    {"EXEC", 1, 0, 0, 0},
    {"EXISTS", -2, 1, -1, 1},
    {"EXPIRE", -3, 1, 1, 1},
    {"GET", 2, 1, 1, 1},
    {"GETBIT", 3, 1, 1, 1},
    {"GETDEL", 2, 1, 1, 1},
    {"GETSET", 3, 1, 1, 1},
    {"HDEL", -3, 1, 1, 1},
    {"HELLO", -1, 0, 0, 0},
    {"HEXISTS", 3, 1, 1, 1},
    {"HGET", 3, 1, 1, 1},
    {"HGETALL", 2, 1, 1, 1},
    {"HINCRBY", 4, 1, 1, 1},
    {"HINCRBYFLOAT", 4, 1, 1, 1},
    {"HKEYS", 2, 1, 1, 1},
    {"HLEN", 2, 1, 1, 1},
    {"HSET", -4, 1, 1, 1},
    {"HSETNX", 4, 1, 1, 1},
    {"HSTRLEN", 3, 1, 1, 1},
    {"HVALS", 2, 1, 1, 1},
    {"INCR", 2, 1, 1, 1},
    {"INCRBY", 3, 1, 1, 1},
    {"INCRBYFLOAT", 3, 1, 1, 1},
    {"KEYS", 2, 0, 0, 0},
    {"LINDEX", 3, 1, 1, 1},
    {"LLEN", 2, 1, 1, 1},
    {"LPOP", -2, 1, 1, 1},
    {"LPUSH", -3, 1, 1, 1},
    {"LRANGE", 4, 1, 1, 1},
    {"LREM", 4, 1, 1, 1},
    {"LSET", 4, 1, 1, 1},
    {"LTRIM", 4, 1, 1, 1},
    {"MGET", -2, 1, -1, 1},
    {"MSET", -3, 1, -1, 2},
    {"MULTI", 1, 0, 0, 0},
    {"PING", -1, 0, 0, 0},
    {"PSETEX", 4, 1, 1, 1},
    {"PUBLISH", 3, 0, 0, 0},
    {"PUBSUB", -2, 0, 0, 0},
    {"RPOP", -2, 1, 1, 1},
    {"RPOPLPUSH", 3, 1, 2, 1},
    {"RPUSH", -3, 1, 1, 1},
    {"SADD", -3, 1, 1, 1},
    {"SCARD", 2, 1, 1, 1},
    {"SCRIPT", -2, 0, 0, 0},
    {"SET", -3, 1, 1, 1},
    {"SETBIT", 4, 1, 1, 1},
    {"SETEX", 4, 1, 1, 1},
    {"SETNX", 3, 1, 1, 1},
    {"SISMEMBER", 3, 1, 1, 1},
    {"SMEMBERS", 2, 1, 1, 1},
    {"SMOVE", 4, 1, 2, 1},
    {"SREM", -3, 1, 1, 1},
    {"STRLEN", 2, 1, 1, 1},
    {"TTL", 2, 1, 1, 1},
    {"UNWATCH", 1, 0, 0, 0},
    {"XGROUP", -2, 0, 0, 0},
    {"XLEN", 2, 1, 1, 1},
    {"XPENDING", -3, 1, 1, 1},
    {"ZADD", -4, 1, 1, 1},
    {"ZCARD", 2, 1, 1, 1},
    {"ZCOUNT", 4, 1, 1, 1},
    {"ZINCRBY", 4, 1, 1, 1},
    {"ZRANGE", -4, 1, 1, 1},
    {"ZRANK", 3, 1, 1, 1},
    {"ZREMRANGEBYRANK", 4, 1, 1, 1},
    {"ZREMRANGEBYSCORE", 4, 1, 1, 1},
    {"ZREVRANK", 3, 1, 1, 1},
    {"ZSCORE", 3, 1, 1, 1},
};

inline constexpr std::size_t k_command_table_size =
    sizeof(k_command_table) / sizeof(k_command_table[0]);

[[nodiscard]] inline const command_spec* lookup_command(
    std::string_view name) noexcept {
    std::size_t lo = 0, hi = k_command_table_size;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const int cmp = static_cast<int>(
            k_command_table[mid].name.compare(name));
        if (cmp == 0) {
            return &k_command_table[mid];
        }
        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return nullptr;
}

} // namespace redis
} // namespace crpc

#endif // CRPC_REDIS_COMMAND_TABLE_HPP