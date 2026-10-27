// CRPC -- cursor-based iteration for Redis collections.
//
// `KEYS` is not an option in a server: it blocks the single-threaded event loop
// while it walks the whole keyspace, and on a large database that is an outage
// caused by a monitoring script. `SCAN` is the alternative, but it is a cursor
// API with three properties a naive wrapper gets wrong:
//
//   * the cursor is opaque and must be echoed back verbatim; the reply's own
//     cursor is the only correct next value, not a counter;
//   * a key may be returned more than once, and a key present for the whole scan
//     is guaranteed to be returned at least once. Iteration is therefore
//     at-least-once, and a caller that needs exactly-once must deduplicate;
//   * `COUNT` is a hint. The server may return more or fewer entries, so memory
//     per step is bounded by the server's answer, not by `COUNT`.
//
// This type makes the first two explicit and keeps the third honest: `next()`
// returns one server step at a time and the caller drives the loop, so nothing is
// buffered beyond a single reply.
#ifndef CRPC_REDIS_SCAN_HPP
#define CRPC_REDIS_SCAN_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/redis/pipeline.hpp>

namespace crpc {
namespace redis {

// Which collection is being walked. Each variant has its own command name and its
// own reply shape:
//
//   keys        SCAN  -> [cursor, [key ...]]
//   hash        HSCAN -> [cursor, [field, value, ...]]
//   set         SSCAN -> [cursor, [member ...]]
//   sorted_set  ZSCAN -> [cursor, [member, score, ...]]
enum class scan_kind : std::uint8_t { keys, hash, set, sorted_set };

struct scan_options {
    // Hint for how much work the server does per call. Bigger means fewer round
    // trips and a longer pause per call; 10-1000 is the useful range.
    std::size_t count = 100;

    // Optional glob pattern. A pattern that matches little still costs the server
    // a scan, so it narrows the result, not the work.
    std::string match;

    // ZSCAN/SCAN can filter by type on Redis 6+; empty leaves it unset.
    std::string type;
};

// One server reply.
struct scan_batch {
    // An entry is a name plus an optional second component: empty for keys and
    // sets, the field for a hash, the score for a sorted set.
    struct entry {
        std::string name;
        std::string value;
    };

    // True when the server says the scan is complete. The cursor itself is opaque
    // and must not be compared to anything but "0".
    bool done = false;
    std::vector<entry> entries;
};

// A single-collection cursor over one executor.
//
// The cursor is copied, not owned: two cursors over one executor remember their
// own position. `reset()` starts over.
class CRPC_API scan_cursor {
public:
    scan_cursor(command_executor& executor, scan_kind kind, std::string key,
                scan_options options = {});

    // Fetches the next batch. `done` is set on the batch that closes the scan; a
    // call after that returns an empty, done batch rather than an error, so a
    // `while (!batch.done)` loop cannot spin forever.
    [[nodiscard]] result<scan_batch> next();

    // Restarts from the beginning.
    void reset() noexcept;

    [[nodiscard]] bool finished() const noexcept { return finished_; }
    [[nodiscard]] std::size_t steps() const noexcept { return steps_; }
    [[nodiscard]] const std::string& cursor() const noexcept { return cursor_; }

private:
    command_executor* executor_ = nullptr;
    scan_kind kind_;
    std::string key_;
    scan_options options_;
    std::string cursor_ = "0";
    bool started_ = false;
    bool finished_ = false;
    std::size_t steps_ = 0;
};

// Walks one collection and hands every entry to `visitor`, returning the number of
// entries seen. This is the shape most callers want, and it exists so a caller
// does not have to write the cursor loop (and get the `started` flag wrong: the
// first call must send cursor "0", and "0" is also the terminator).
//
// `visitor` is called once per entry and may return false to stop early. The
// return value counts entries actually visited.
template <typename Visitor>
std::size_t scan_each(command_executor& executor, scan_kind kind, std::string key,
                      scan_options options, Visitor&& visitor) {
    scan_cursor cursor(executor, kind, std::move(key), std::move(options));
    std::size_t visited = 0;
    for (;;) {
        result<scan_batch> batch = cursor.next();
        if (!batch) {
            return visited;
        }
        for (const scan_batch::entry& entry : batch.value().entries) {
            // Count the entry that stopped the scan: the visitor was called with
            // it, so returning `visited` before the ++ would undercount by one.
            ++visited;
            if (!visitor(entry)) {
                return visited;
            }
        }
        if (batch.value().done) {
            return visited;
        }
    }
}

// Scans several independent servers (a Redis Cluster's masters, for example) as
// one stream.
//
// Cluster-wide SCAN is per node: there is no single cursor for the whole keyspace.
// Because a key can move between nodes during a resharding, and because each node
// has its own at-least-once guarantee, the combined result can repeat a key across
// nodes as well as within one — so this type deduplicates by name, which is the
// only way a caller can iterate without seeing the same key twice per pass.
class CRPC_API cluster_scan {
public:
    cluster_scan(std::vector<command_executor*> executors, scan_kind kind,
                 scan_options options = {});

    [[nodiscard]] result<scan_batch> next();

    void reset() noexcept;
    [[nodiscard]] bool finished() const noexcept { return done_; }

private:
    std::vector<command_executor*> executors_;
    scan_kind kind_;
    scan_options options_;
    // One cursor per node; the cluster scan advances them in turn.
    std::vector<std::string> cursors_;
    std::vector<bool> node_done_;
    // Keys already emitted. A resharding can return one key from two nodes, so
    // per-node at-least-once is not enough for the combined stream.
    std::unordered_set<std::string> seen_;
    std::size_t active_ = 0;
    bool done_ = false;
};

} // namespace redis
} // namespace crpc

#endif // CRPC_REDIS_SCAN_HPP