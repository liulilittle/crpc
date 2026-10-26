// CRPC Redis -- Redis Streams.
//
// Streams are an append-only log with consumer groups. This wrapper builds the
// XADD/XREAD/XRANGE/XGROUP family and decodes the nested array replies into
// ordinary C++ values. It uses the same `command_executor` as the rest of the
// feature layer, so it works over any connection (including a pooled one) and
// inherits the coroutine-aware blocking behaviour of `connection`.
//
// IDs
// ---
// A stream ID is `<milliseconds>-<sequence>`. `stream_id::parse` accepts the
// short form `<milliseconds>` (sequence 0) and rejects the special range
// bounds `-`, `+` and `$`; those are range markers, not entries. Entry IDs are
// also kept as the exact server string in `stream_entry::id`, so a caller can
// round-trip an ID without formatting it again.
//
// Replies
// -------
// XREAD returns one group per requested stream and XRANGE returns one group of
// entries. Both are decoded into `stream_read` / `stream_entry`. A RESP error
// reply becomes an `rpc_code::application` failure; a malformed shape becomes
// `rpc_code::protocol_error`.
#ifndef CRPC_REDIS_STREAM_HPP
#define CRPC_REDIS_STREAM_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/redis/pipeline.hpp>

namespace crpc {
namespace redis {

// A parsed Redis stream ID.
struct CRPC_API stream_id {
    std::uint64_t ms = 0;
    std::uint64_t seq = 0;

    stream_id() noexcept = default;
    stream_id(std::uint64_t milliseconds, std::uint64_t sequence) noexcept
        : ms(milliseconds), seq(sequence) {}

    // Parses `<ms>-<seq>` or `<ms>`. Returns false for an empty string, a
    // non-digit, a leading sign, an overflow, or the range markers `-`, `+`,
    // `$`, `*`.
    [[nodiscard]] static bool parse(std::string_view text,
                                    stream_id& out) noexcept;

    [[nodiscard]] std::string to_string() const;

    friend bool operator==(const stream_id& a, const stream_id& b) noexcept {
        return a.ms == b.ms && a.seq == b.seq;
    }
    friend bool operator!=(const stream_id& a, const stream_id& b) noexcept {
        return !(a == b);
    }
    friend bool operator<(const stream_id& a, const stream_id& b) noexcept {
        return a.ms < b.ms || (a.ms == b.ms && a.seq < b.seq);
    }
    friend bool operator<=(const stream_id& a, const stream_id& b) noexcept {
        return !(b < a);
    }
    friend bool operator>(const stream_id& a, const stream_id& b) noexcept {
        return b < a;
    }
    friend bool operator>=(const stream_id& a, const stream_id& b) noexcept {
        return !(a < b);
    }
};

// One stream entry: its ID and its field/value pairs. Redis preserves the
// field order, so the vector is the right container.
struct stream_entry {
    std::string id;
    stream_id parsed_id;
    std::vector<std::pair<std::string, std::string>> fields;
};

// One stream's slice of an XREAD/XREADGROUP reply.
struct stream_read {
    std::string key;
    std::vector<stream_entry> entries;
};

enum class stream_trim_kind : std::uint8_t { none, maxlen, minid };

// Options for XADD.
struct stream_add_options {
    // Do not create the stream when it does not exist (NOMKSTREAM). The reply is
    // null in that case and `add()` reports `rpc_code::not_found`.
    bool nomkstream = false;

    // Optional trimming before appending.
    stream_trim_kind trim = stream_trim_kind::none;
    bool approximate = true; // `~` when true, `=` when false
    std::uint64_t maxlen = 0;
    std::string minid;
    std::size_t limit = 0; // 0 means no LIMIT clause
};

// Options for XTRIM.
struct stream_trim_options {
    stream_trim_kind trim = stream_trim_kind::maxlen;
    bool approximate = true;
    std::uint64_t maxlen = 0;
    std::string minid;
    std::size_t limit = 0;
};

// Options shared by XREAD and XREADGROUP.
struct stream_read_options {
    std::size_t count = 0;                        // 0 means no COUNT clause
    std::chrono::milliseconds block{0};           // 0 means no BLOCK clause
    bool noack = false;                           // XREADGROUP only
};

// One entry in the detailed XPENDING reply.
struct stream_pending_entry {
    std::string id;
    std::string consumer;
    std::chrono::milliseconds idle{0};
    std::int64_t deliveries = 0;
};

// The summary XPENDING reply: total count, the smallest/largest pending ID and
// the per-consumer pending counts.
struct stream_pending_summary {
    std::int64_t total = 0;
    std::string smallest_id;
    std::string greatest_id;
    std::vector<std::pair<std::string, std::int64_t>> consumers;
};

// Options for XCLAIM.
struct stream_claim_options {
    std::chrono::milliseconds idle{0};       // 0 means no IDLE clause
    // Absolute Unix timestamp in milliseconds, not a duration. 0 means no TIME
    // clause.
    std::chrono::milliseconds time{0};
    std::int64_t retry_count = -1;           // negative means no RETRYCOUNT
    bool force = false;
    bool justid = false;
    std::string last_id;                     // empty means no LASTID
};

// The XAUTOCLAIM reply.
struct stream_autoclaim_result {
    std::string next_id;
    std::vector<stream_entry> entries;
    // Entries that were deleted from the stream and removed from the PEL. The
    // field is empty on Redis versions that do not report them (6.2).
    std::vector<std::string> deleted_ids;
};

class CRPC_API stream {
public:
    explicit stream(command_executor& executor) noexcept;
    ~stream();

    stream(const stream&) = delete;
    stream& operator=(const stream&) = delete;

    // XADD key [NOMKSTREAM] [MAXLEN|MINID ...] *|id field value ...
    //
    // `fields` must be non-empty and must have an even size. The default ID is
    // `*` (server-generated). A null reply (NOMKSTREAM on a missing stream) is
    // reported as `rpc_code::not_found`.
    [[nodiscard]] result<std::string> add(
        const std::string& key,
        const std::vector<std::pair<std::string, std::string>>& fields,
        const stream_add_options& options = {});

    [[nodiscard]] result<std::string> add(
        const std::string& key,
        const std::vector<std::pair<std::string, std::string>>& fields,
        std::string id, const stream_add_options& options = {});

    // XRANGE key start end [COUNT n]
    [[nodiscard]] result<std::vector<stream_entry>> range(
        const std::string& key, const std::string& start = "-",
        const std::string& end = "+", std::size_t count = 0);

    // XREVRANGE key end start [COUNT n]
    [[nodiscard]] result<std::vector<stream_entry>> reverse_range(
        const std::string& key, const std::string& end = "+",
        const std::string& start = "-", std::size_t count = 0);

    // XREAD [COUNT n] [BLOCK ms] STREAMS key ... id ...
    //
    // `keys` and `ids` must be non-empty and the same size. A null reply (no
    // entries available and no BLOCK, or the BLOCK timeout elapsed) decodes to
    // an empty vector.
    [[nodiscard]] result<std::vector<stream_read>> read(
        const std::vector<std::string>& keys,
        const std::vector<std::string>& ids,
        const stream_read_options& options = {});

    // XREADGROUP GROUP group consumer [COUNT n] [BLOCK ms] [NOACK]
    //           STREAMS key ... id ...
    [[nodiscard]] result<std::vector<stream_read>> read_group(
        const std::string& group, const std::string& consumer,
        const std::vector<std::string>& keys,
        const std::vector<std::string>& ids,
        const stream_read_options& options = {});

    // XLEN key
    [[nodiscard]] result<std::int64_t> length(const std::string& key);

    // XDEL key id ...
    [[nodiscard]] result<std::int64_t> del(const std::string& key,
                                           const std::vector<std::string>& ids);

    // XTRIM key MAXLEN|MINID [=|~] threshold [LIMIT n]
    [[nodiscard]] result<std::int64_t> trim(const std::string& key,
                                            const stream_trim_options& options);

    // XGROUP CREATE key group id [MKSTREAM]
    [[nodiscard]] status group_create(const std::string& key,
                                      const std::string& group,
                                      const std::string& id = "$",
                                      bool mkstream = false);

    // XGROUP DESTROY key group. The reply is 1 when the group existed and 0
    // when it did not.
    [[nodiscard]] result<std::int64_t> group_destroy(const std::string& key,
                                                     const std::string& group);

    // XGROUP CREATECONSUMER key group consumer. The reply is 1 when the
    // consumer was created and 0 when it already existed.
    [[nodiscard]] result<std::int64_t> group_create_consumer(
        const std::string& key, const std::string& group,
        const std::string& consumer);

    // XGROUP DELCONSUMER key group consumer. The reply is the number of
    // pending entries the consumer still owned.
    [[nodiscard]] result<std::int64_t> group_del_consumer(
        const std::string& key, const std::string& group,
        const std::string& consumer);

    // XACK key group id ...
    [[nodiscard]] result<std::int64_t> ack(const std::string& key,
                                           const std::string& group,
                                           const std::vector<std::string>& ids);

    // XPENDING key group -- summary form.
    [[nodiscard]] result<stream_pending_summary> pending(
        const std::string& key, const std::string& group);

    // XPENDING key group [IDLE min-idle] start end count [consumer] --
    // detailed form. `consumer` empty means all consumers; `idle` zero means
    // no IDLE clause.
    [[nodiscard]] result<std::vector<stream_pending_entry>> pending_range(
        const std::string& key, const std::string& group,
        const std::string& start, const std::string& end, std::size_t count,
        const std::string& consumer = "",
        std::chrono::milliseconds idle = std::chrono::milliseconds{0});

    // XCLAIM key group consumer min-idle-time id ... [options]
    [[nodiscard]] result<std::vector<stream_entry>> claim(
        const std::string& key, const std::string& group,
        const std::string& consumer, std::chrono::milliseconds min_idle,
        const std::vector<std::string>& ids,
        const stream_claim_options& options = {});

    // XAUTOCLAIM key group consumer min-idle-time start [COUNT n] [JUSTID]
    [[nodiscard]] result<stream_autoclaim_result> auto_claim(
        const std::string& key, const std::string& group,
        const std::string& consumer, std::chrono::milliseconds min_idle,
        const std::string& start = "0-0", std::size_t count = 0,
        bool justid = false);

private:
    command_executor* executor_;
};

} // namespace redis
} // namespace crpc

#endif // CRPC_REDIS_STREAM_HPP
