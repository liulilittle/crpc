// CRPC Redis -- client-side caching over RESP3 invalidation tracking.
//
// This is the P6 plan item (`.opencode/sdd/redis-completeness-plan.md`):
// CLIENT TRACKING, a bounded invalidation map, monotonic versioning and
// reconnect resync. It is built on `message_channel` (a connection that can
// receive server pushes) and the existing RESP3 push decoder; this header adds
// the typed cache that consumes `invalidate` pushes and keeps keys coherent
// with the server.
//
// The caller owns the connection and the command path that reads values:
// `client_cache` only implements "which keys did I hold, and do I still have the
// latest copy". `get()` returns what was `put()`, a miss returns nullptr and
// the caller loads from the server and calls `put()`. An invalidation push
// erases the key and bumps a monotonic version, so a caller can cheaply decide
// "re-scan all cached values" after `version()` changed.
#ifndef CRPC_REDIS_CACHE_HPP
#define CRPC_REDIS_CACHE_HPP

#include <cstddef>
#include <cstdint>
#include <deque>
#include <list>
#include <string>
#include <unordered_map>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/redis/pipeline.hpp>
#include <crpc/redis/resp.hpp>

namespace crpc {
namespace redis {

// Bounded, client-side value cache fed by RESP3 CLIENT TRACKING invalidation.
class CRPC_API client_cache {
public:
    // `channel` must be a dedicated connection's command channel (the server
    // leaves request/response mode once CLIENT TRACKING is on, like Pub/Sub).
    // `max_entries` bounds the LRU; a put beyond that evicts the least recently
    // used key.
    client_cache(message_channel& channel, std::size_t max_entries) noexcept;
    ~client_cache();

    client_cache(const client_cache&) = delete;
    client_cache& operator=(const client_cache&) = delete;

    // Sends `CLIENT TRACKING on`. Failures (transport, or a server before
    // Redis 6 without the command) are returned as is.
    [[nodiscard]] status start();

    // Clears the whole table and re-arms tracking. This is the reconnect/resync
    // path: after the link is re-established, every cached value is stale, so
    // nothing survives and the caller re-reads through misses.
    [[nodiscard]] status resync();

    // Returns the cached value for `key`, or nullptr on a miss. The entry is
    // touched (LRU) on a hit. The pointer stays valid until the next
    // non-const call on this cache.
    [[nodiscard]] const resp_value* get(std::string_view key);

    // Stores `value` under `key`, touching it (LRU). Does not send anything:
    // the server learns the key the next time the caller reads it through
    // CLIENT TRACKING.
    void put(std::string key, resp_value value);

    // Removes `key` locally (the caller may have failed the read). Does not
    // send an unread: CLIENT TRACKING keys are erased on the server when they
    // are evicted from the client's read set, and an explicit read later
    // re-registers.
    void erase(std::string_view key);

    // Monotonic counter bumped on every invalidation and on resync. A caller
    // that caches derived values can compare it before/after work.
    [[nodiscard]] std::uint64_t version() const noexcept;

    // Number of cached entries.
    [[nodiscard]] std::size_t size() const noexcept;

    // Drains at most one pending push and applies it. Returns ok on "no push
    // pending yet, or one was applied"; a transport failure returns its error.
    // A push whose first element is `invalidate` erases every listed key and
    // bumps the version. Any other push shape is ignored (it belongs to a
    // different consumer on the same channel).
    [[nodiscard]] status poll_once();

private:
    void touch(std::list<std::string>::iterator where);
    void evict();

    message_channel* channel_;
    std::size_t max_entries_;

    std::list<std::string> lru_;
    using iterator_cache =
        std::unordered_map<std::string,
                           std::pair<resp_value, std::list<std::string>::iterator>>;
    iterator_cache entries_;
    std::uint64_t version_ = 0;
    bool tracking_on_ = false;
};

} // namespace redis
} // namespace crpc

#endif // CRPC_REDIS_CACHE_HPP