// CRPC -- Redis data type wrappers.
//
// This header is the data-type layer of the Redis client: it models the value
// domain shared by every Redis type, builds the commands for each type, and
// turns replies back into typed C++ values. The value model is deliberately
// independent of the transport (connection.hpp) and of the wire codec's value
// type (resp.hpp's resp_value): the only wire-facing entry points are
// encode_command(), encode_value() and decode_value(), which are implemented in
// types.cpp. The command type is the wire codec's own `resp_command`, so the
// data-type layer and the protocol layer share one command representation.
//
// The value model mirrors RESP3 rather than RESP2: a reply can be null, a
// boolean, an integer, a double, a bulk/simple string, an array, a map, a set,
// a push or an error. RESP2 replies are a subset of that set, so a decoder for
// one is a decoder for the other.
#ifndef CRPC_REDIS_TYPES_HPP
#define CRPC_REDIS_TYPES_HPP

#include <crpc/config.hpp>
#include <crpc/redis/resp.hpp>

#include <chrono>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace crpc {
namespace redis {

// --- Value model -------------------------------------------------------------
//
// `value` is a recursive variant. The recursion goes through the container
// aliases below, which is why they are declared before `value` and why the
// variant is inherited from rather than held as a member: a member of type
// std::vector<value> would require `value` to be complete at the point the
// enclosing class is defined, and it never is.

struct value;

using array_value = std::vector<value>;
using map_value = std::vector<std::pair<value, value>>;

struct set_value {
    std::vector<value> items;
};

struct push_value {
    std::vector<value> items;
};

struct error_value {
    std::string message;
};

enum class value_kind {
    null = 0,
    boolean,
    integer,
    real,
    string,
    array,
    map,
    set,
    push,
    error
};

using value_base = std::variant<std::nullptr_t, bool, i64, double, std::string,
                                array_value, map_value, set_value, push_value,
                                error_value>;

#if defined(CRPC_COMPILER_MSVC)
// `value` derives from a std::variant, whose internal storage is an
// implementation detail of the standard library. C4251 would otherwise reject
// the exported class in a shared build; the exported ABI is the method set.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
struct CRPC_API value : value_base {
    using value_base::variant;
    using value_base::operator=;

    value_kind kind() const noexcept;
    bool is_null() const noexcept;
    bool is_bool() const noexcept;
    bool is_int() const noexcept;
    bool is_real() const noexcept;
    bool is_string() const noexcept;
    bool is_array() const noexcept;
    bool is_map() const noexcept;
    bool is_set() const noexcept;
    bool is_push() const noexcept;
    bool is_error() const noexcept;

    // Checked accessors. They throw std::bad_variant_access when the held
    // alternative does not match, which is what std::get does.
    bool as_bool() const;
    i64 as_int() const;
    double as_real() const;
    const std::string& as_string() const;
    const array_value& as_array() const;
    const map_value& as_map() const;
    const set_value& as_set() const;
    const push_value& as_push() const;
    const error_value& as_error() const;

    // Nullable accessors. They return nullptr instead of throwing, which is
    // the right shape for "was this reply the shape I expected?".
    const bool* if_bool() const noexcept;
    const i64* if_int() const noexcept;
    const double* if_real() const noexcept;
    const std::string* if_string() const noexcept;
    const array_value* if_array() const noexcept;
    const map_value* if_map() const noexcept;
    const set_value* if_set() const noexcept;
    const push_value* if_push() const noexcept;
    const error_value* if_error() const noexcept;
};

#if defined(CRPC_COMPILER_MSVC)
#pragma warning(pop)
#endif

// --- Commands ----------------------------------------------------------------

// An ordered Redis command: a name plus its arguments. This is the wire
// codec's own `resp_command` (a vector of bulk strings), so a command built by
// any `*` builder can be handed straight to a `command_executor`.
using command = resp_command;

// --- Wire interop ------------------------------------------------------------

// Encodes `cmd` as a RESP2 request buffer (an array of bulk strings).
CRPC_API std::string encode_command(const command& cmd);

// Encodes `v` as a RESP3 reply buffer. An error value is written as a RESP
// error line, so its message must not carry CR or LF (there is no
// length-prefixed error form in RESP3); callers that build an error value from
// untrusted text should keep it single-line.
CRPC_API std::string encode_value(const value& v);

// Decodes exactly one RESP value from `bytes`. Returns true and sets `out` and
// `consumed` when a complete value is present; returns false when the buffer is
// incomplete or malformed, leaving `out` untouched and `consumed` at 0.
CRPC_API bool decode_value(const std::string& bytes, value& out,
                           std::size_t& consumed);

// --- Reply decoders ----------------------------------------------------------
//
// These are the typed projections of a reply. They throw std::bad_variant_access
// when the reply has the wrong shape, so a caller can rely on the return type.

CRPC_API bool decode_bool(const value& v);
CRPC_API i64 decode_int(const value& v);
CRPC_API double decode_real(const value& v);
CRPC_API std::string decode_string(const value& v);
CRPC_API std::vector<std::string> decode_string_array(const value& v);
CRPC_API std::vector<std::pair<std::string, std::string>> decode_pairs(
    const value& v);
CRPC_API std::vector<double> decode_real_array(const value& v);

struct scored_member {
    std::string member;
    double score;
};

CRPC_API std::vector<scored_member> decode_scored_members(const value& v);

struct redis_stream_entry {
    std::string id;
    std::vector<std::pair<std::string, std::string>> fields;
};

CRPC_API std::vector<redis_stream_entry> decode_stream_entries(const value& v);

// --- String ------------------------------------------------------------------

class CRPC_API string_type {
public:
    using ttl = std::chrono::milliseconds;

    static command set(std::string key, value val,
                       std::optional<ttl> expire = std::nullopt);
    static command setnx(std::string key, value val);
    static command setex(std::string key, std::chrono::seconds seconds, value val);
    static command psetex(std::string key, std::chrono::milliseconds millis,
                          value val);
    static command get(std::string key);
    static command getset(std::string key, value val);
    static command getdel(std::string key);
    static command getex(std::string key, std::optional<ttl> expire = std::nullopt);
    static command append(std::string key, std::string val);
    static command strlen(std::string key);
    static command incr(std::string key);
    static command decr(std::string key);
    static command incrby(std::string key, i64 amount);
    static command decrby(std::string key, i64 amount);
    static command incrbyfloat(std::string key, double amount);
    static command mget(std::vector<std::string> keys);
    static command mset(std::vector<std::pair<std::string, std::string>> pairs);
};

// --- List --------------------------------------------------------------------

class CRPC_API list_type {
public:
    static command lpush(std::string key, std::vector<value> values);
    static command rpush(std::string key, std::vector<value> values);
    static command lpushx(std::string key, std::vector<value> values);
    static command rpushx(std::string key, std::vector<value> values);
    static command lpop(std::string key, std::optional<i64> count = std::nullopt);
    static command rpop(std::string key, std::optional<i64> count = std::nullopt);
    static command lrange(std::string key, i64 start, i64 stop);
    static command llen(std::string key);
    static command lindex(std::string key, i64 index);
    static command lset(std::string key, i64 index, value val);
    static command lrem(std::string key, i64 count, value val);
    static command ltrim(std::string key, i64 start, i64 stop);
    static command lpos(std::string key, value val,
                        std::optional<i64> rank = std::nullopt,
                        std::optional<i64> count = std::nullopt,
                        std::optional<i64> maxlen = std::nullopt);
    static command rpoplpush(std::string source, std::string destination);
};

// --- Hash --------------------------------------------------------------------

class CRPC_API hash_type {
public:
    static command hset(std::string key, std::string field, value val);
    static command hset(std::string key,
                        std::vector<std::pair<std::string, std::string>> pairs);
    static command hsetnx(std::string key, std::string field, value val);
    static command hget(std::string key, std::string field);
    static command hdel(std::string key, std::vector<std::string> fields);
    static command hlen(std::string key);
    static command hexists(std::string key, std::string field);
    static command hkeys(std::string key);
    static command hvals(std::string key);
    static command hgetall(std::string key);
    static command hmget(std::string key, std::vector<std::string> fields);
    static command hincrby(std::string key, std::string field, i64 amount);
    static command hincrbyfloat(std::string key, std::string field, double amount);
    static command hstrlen(std::string key, std::string field);
    static command hrandfield(std::string key, std::optional<i64> count = std::nullopt);
};

// --- Set ---------------------------------------------------------------------

class CRPC_API set_type {
public:
    static command sadd(std::string key, std::vector<std::string> members);
    static command srem(std::string key, std::vector<std::string> members);
    static command smembers(std::string key);
    static command sismember(std::string key, std::string member);
    static command smismember(std::string key, std::vector<std::string> members);
    static command scard(std::string key);
    static command sinter(std::vector<std::string> keys);
    static command sunion(std::vector<std::string> keys);
    static command sdiff(std::vector<std::string> keys);
    static command sinterstore(std::string destination,
                              std::vector<std::string> keys);
    static command sunionstore(std::string destination,
                               std::vector<std::string> keys);
    static command sdiffstore(std::string destination,
                              std::vector<std::string> keys);
    static command spop(std::string key, std::optional<i64> count = std::nullopt);
    static command srandmember(std::string key, std::optional<i64> count = std::nullopt);
    static command smove(std::string source, std::string destination,
                         std::string member);
};

// --- Sorted set --------------------------------------------------------------

class CRPC_API zset_type {
public:
    struct member {
        double score;
        std::string value;
    };

    static command zadd(std::string key, std::vector<member> members);
    static command zadd_incr(std::string key, double score, std::string member);
    static command zrem(std::string key, std::vector<std::string> members);
    static command zscore(std::string key, std::string member);
    static command zmscore(std::string key, std::vector<std::string> members);
    static command zcard(std::string key);
    static command zcount(std::string key, std::string min, std::string max);
    static command zincrby(std::string key, double amount, std::string member);
    static command zrange(std::string key, i64 start, i64 stop,
                          bool with_scores = false);
    static command zrangebyscore(std::string key, std::string min, std::string max,
                                 bool with_scores = false,
                                 std::optional<i64> offset = std::nullopt,
                                 std::optional<i64> count = std::nullopt);
    static command zrevrange(std::string key, i64 start, i64 stop,
                             bool with_scores = false);
    static command zrank(std::string key, std::string member);
    static command zrevrank(std::string key, std::string member);
    static command zremrangebyrank(std::string key, i64 start, i64 stop);
    static command zremrangebyscore(std::string key, std::string min,
                                    std::string max);
    static command zrandmember(std::string key, std::optional<i64> count = std::nullopt);
};

// --- Stream ------------------------------------------------------------------

class CRPC_API stream_type {
public:
    using fields = std::vector<std::pair<std::string, std::string>>;
    struct stream_spec {
        std::string key;
        std::string last_id;
    };

    static command xadd(std::string key, std::string id, fields values);
    static command xlen(std::string key);
    static command xdel(std::string key, std::vector<std::string> ids);
    static command xrange(std::string key, std::string start, std::string end,
                          std::optional<i64> count = std::nullopt);
    static command xrevrange(std::string key, std::string end, std::string start,
                             std::optional<i64> count = std::nullopt);
    static command xread(std::optional<i64> count, std::optional<i64> block,
                         std::vector<stream_spec> streams);
    static command xgroup_create(std::string key, std::string group, std::string id,
                                 bool mkstream = false);
    static command xgroup_destroy(std::string key, std::string group);
    static command xgroup_setid(std::string key, std::string group, std::string id);
    static command xack(std::string key, std::string group,
                        std::vector<std::string> ids);
    static command xpending(std::string key, std::string group);
};

// --- Bitmap ------------------------------------------------------------------

class CRPC_API bitmap_type {
public:
    static command setbit(std::string key, i64 offset, bool bit);
    static command getbit(std::string key, i64 offset);
    static command bitcount(std::string key, std::optional<i64> start = std::nullopt,
                            std::optional<i64> end = std::nullopt,
                            bool bitmap_semantics = false);
    static command bitpos(std::string key, bool bit,
                          std::optional<i64> start = std::nullopt,
                          std::optional<i64> end = std::nullopt);
    static command bitop(std::string op, std::string destination,
                         std::vector<std::string> keys);
};

// --- HyperLogLog -------------------------------------------------------------

class CRPC_API hyperloglog_type {
public:
    static command pfadd(std::string key,
                        std::optional<std::vector<std::string>> elements = std::nullopt);
    static command pfcount(std::vector<std::string> keys);
    static command pfmerge(std::string destination,
                           std::vector<std::string> sources);
};

// --- Geo ---------------------------------------------------------------------

class CRPC_API geo_type {
public:
    struct point {
        double longitude;
        double latitude;
        std::string member;
    };

    static command geoadd(std::string key, std::vector<point> points);
    static command geopos(std::string key, std::vector<std::string> members);
    static command geodist(std::string key, std::string member_a,
                           std::string member_b, std::optional<std::string> unit = std::nullopt);
    static command geohash(std::string key, std::vector<std::string> members);
    static command georadius(std::string key, double longitude, double latitude,
                             double radius, std::string unit, bool with_dist = false);
    static command georadiusbymember(std::string key, std::string member,
                                     double radius, std::string unit,
                                     bool with_dist = false);
};

// --- Iterators ---------------------------------------------------------------
//
// The iterators adapt a decoded array into typed elements. list/set iterate
// over strings, zset over (member, score) pairs and stream over entries. They
// are input iterators: they hold a pointer to the decoded array and an index,
// so the caller keeps the reply alive for as long as it iterates.

class CRPC_API string_iterator {
public:
    using iterator_category = std::input_iterator_tag;
    using value_type = std::string;
    using difference_type = std::ptrdiff_t;
    using pointer = const std::string*;
    using reference = const std::string&;

    string_iterator() = default;
    explicit string_iterator(const array_value& items);

    reference operator*() const;
    pointer operator->() const;
    string_iterator& operator++();
    string_iterator operator++(int);
    bool operator==(const string_iterator& other) const noexcept;
    bool operator!=(const string_iterator& other) const noexcept;

private:
    const array_value* items_ = nullptr;
    std::size_t index_ = 0;
};

using list_iterator = string_iterator;
using set_iterator = string_iterator;

class CRPC_API zset_iterator {
public:
    using iterator_category = std::input_iterator_tag;
    using value_type = scored_member;
    using difference_type = std::ptrdiff_t;
    using pointer = void;
    using reference = scored_member;

    zset_iterator() = default;
    explicit zset_iterator(const array_value& items);

    reference operator*() const;
    zset_iterator& operator++();
    zset_iterator operator++(int);
    bool operator==(const zset_iterator& other) const noexcept;
    bool operator!=(const zset_iterator& other) const noexcept;

private:
    const array_value* items_ = nullptr;
    std::size_t index_ = 0;
};

class CRPC_API stream_iterator {
public:
    using iterator_category = std::input_iterator_tag;
    using value_type = redis_stream_entry;
    using difference_type = std::ptrdiff_t;
    using pointer = void;
    using reference = redis_stream_entry;

    stream_iterator() = default;
    explicit stream_iterator(const array_value& items);

    reference operator*() const;
    stream_iterator& operator++();
    stream_iterator operator++(int);
    bool operator==(const stream_iterator& other) const noexcept;
    bool operator!=(const stream_iterator& other) const noexcept;

private:
    const array_value* items_ = nullptr;
    std::size_t index_ = 0;
};

} // namespace redis
} // namespace crpc

#endif // CRPC_REDIS_TYPES_HPP
