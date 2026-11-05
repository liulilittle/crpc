// CRPC Redis -- client-side cache (P6 plan item).
//
// Coverage targets: CLIENT TRACKING bootstrap, bounded LRU eviction, invalidation
// push application (keys erased, monotonic version bumped), cache hit/miss, and
// reconnect resync (clear + re-arm). The unit cases drive a scripted
// message_channel so no server is needed; the IT case (test_redis_it.cpp) proves
// the round trip against a live Redis 7 CLIENT TRACKING.
#include <cstddef>
#include <cstdint>
#include <string>

#include <crpc/redis/cache.hpp>
#include <crpc/redis/pipeline.hpp>
#include <crpc/redis/resp.hpp>

#include "redis_test_support.hpp"
#include "test_framework.hpp"

using namespace redis_test;
using crpc::redis::client_cache;
using crpc::redis::resp_value;

namespace {

resp_value invalidate_push(std::initializer_list<std::string> keys) {
    std::vector<resp_value> items{text("invalidate")};
    for (const std::string& key : keys) {
        items.push_back(text(key));
    }
    return push(std::move(items));
}

// A push that is not an invalidation (e.g. a Pub/Sub message on the same
// channel) must be ignored by the cache.
resp_value unrelated_push() {
    return push(std::vector<resp_value>{text("message"), text("ch"),
                                        text("payload")});
}

} // namespace

// start() sends the CLIENT TRACKING bootstrap command and consumes its +OK.
CRPC_TEST(client_cache_start_sends_tracking) {
    scripted_executor channel;
    channel.incoming.push_back(simple("OK")); // the +OK acknowledge
    client_cache cache(channel, 8);
    CRPC_CHECK(cache.start().ok());
    CRPC_CHECK_EQ(std::size_t{1}, channel.sent.size());
    const auto& cmd = channel.sent.front();
    CRPC_CHECK_EQ(std::string("CLIENT"), cmd.at(0));
    CRPC_CHECK_EQ(std::string("TRACKING"), cmd.at(1));
    CRPC_CHECK_EQ(std::string("on"), cmd.at(2));
}

// A value comes back from get() after put(), and a miss returns nullptr.
CRPC_TEST(client_cache_get_put_hit_miss) {
    scripted_executor channel;
    client_cache cache(channel, 8);
    CRPC_CHECK(cache.get("absent") == nullptr);
    cache.put("k", text("v"));
    const resp_value* got = cache.get("k");
    CRPC_CHECK(got != nullptr);
    CRPC_CHECK_EQ(std::string("v"), got->as_bulk_string());
    CRPC_CHECK_EQ(std::size_t{1}, cache.size());
}

// The LRU bound evicts the least recently used entry once max_entries is hit.
CRPC_TEST(client_cache_lru_evicts_oldest) {
    scripted_executor channel;
    client_cache cache(channel, 2);
    cache.put("a", text("1"));
    cache.put("b", text("2"));
    CRPC_CHECK(cache.get("a") != nullptr); // a is now most recent
    cache.put("c", text("3"));             // evicts b (least recent)
    CRPC_CHECK(cache.get("b") == nullptr);
    CRPC_CHECK(cache.get("a") != nullptr);
    CRPC_CHECK(cache.get("c") != nullptr);
    CRPC_CHECK_EQ(std::size_t{2}, cache.size());
}

// An invalidation push erases the listed keys and bumps the version.
CRPC_TEST(client_cache_invalidation_erases_and_bumps_version) {
    scripted_executor channel;
    client_cache cache(channel, 8);
    cache.put("k", text("v"));
    const std::uint64_t v0 = cache.version();
    channel.incoming.push_back(invalidate_push({"k"}));
    CRPC_CHECK(cache.poll_once().ok());
    CRPC_CHECK(cache.get("k") == nullptr);
    CRPC_CHECK(cache.version() > v0);
}

// The live Redis 7 server nests the keys: `> [invalidate, [key, ...]]`.
// The nested keys must be erased individually, not by a whole-table flush.
CRPC_TEST(client_cache_invalidation_nested_array_form) {
    scripted_executor channel;
    client_cache cache(channel, 8);
    cache.put("a", text("1"));
    cache.put("c", text("3"));
    const std::uint64_t v0 = cache.version();
    channel.incoming.push_back(push(std::vector<resp_value>{
        text("invalidate"),
        array(std::vector<resp_value>{text("a")})}));
    CRPC_CHECK(cache.poll_once().ok());
    CRPC_CHECK(cache.get("a") == nullptr);
    CRPC_CHECK(cache.get("c") != nullptr);
    CRPC_CHECK(cache.version() > v0);
}

// An empty `> [invalidate]` (a whole-table flush) clears every entry.
CRPC_TEST(client_cache_invalidation_empty_flushes_table) {
    scripted_executor channel;
    client_cache cache(channel, 8);
    cache.put("a", text("1"));
    cache.put("b", text("2"));
    channel.incoming.push_back(push(std::vector<resp_value>{text("invalidate")}));
    CRPC_CHECK(cache.poll_once().ok());
    CRPC_CHECK_EQ(std::size_t{0}, cache.size());
    CRPC_CHECK(cache.version() > 0u);
}

// A push without the invalidate kind is ignored: keys remain, version unchanged.
CRPC_TEST(client_cache_unrelated_push_is_ignored) {
    scripted_executor channel;
    client_cache cache(channel, 8);
    cache.put("k", text("v"));
    const std::uint64_t v0 = cache.version();
    channel.incoming.push_back(unrelated_push());
    CRPC_CHECK(cache.poll_once().ok());
    CRPC_CHECK(cache.get("k") != nullptr);
    CRPC_CHECK_EQ(v0, cache.version());
}

// resync() clears every entry, bumps the version, and re-sends the bootstrap.
CRPC_TEST(client_cache_resync_clears_and_rearms) {
    scripted_executor channel;
    channel.incoming.push_back(simple("OK")); // ack for start()
    client_cache cache(channel, 8);
    CRPC_CHECK(cache.start().ok());
    cache.put("k", text("v"));
    channel.incoming.push_back(simple("OK")); // ack for resync's re-send
    const std::size_t tracker_sent = channel.sent.size();
    const std::uint64_t v0 = cache.version();
    CRPC_CHECK(cache.resync().ok());
    CRPC_CHECK_EQ(std::size_t{0}, cache.size());
    CRPC_CHECK(cache.version() > v0);
    // start() + resync() = two TRACKING-on sends.
    CRPC_CHECK_EQ(tracker_sent + 1, channel.sent.size());
    const auto& cmd = channel.sent.back();
    CRPC_CHECK_EQ(std::string("TRACKING"), cmd.at(1));
    CRPC_CHECK_EQ(std::string("on"), cmd.at(2));
}

CRPC_TEST_MAIN()