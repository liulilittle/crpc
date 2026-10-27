// CRPC -- Redis SCAN cursor tests.
//
// A SCAN wrapper has three ways to be wrong: send the stored cursor on the first
// call (starting at the terminator), treat the cursor as a number, or stop before
// the server says "0". Each case pins one of them, plus the paired shapes
// (HSCAN/ZSCAN) and the cluster-wide deduplication.
#include "test_framework.hpp"

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/redis/pipeline.hpp>
#include <crpc/redis/resp.hpp>
#include <crpc/redis/scan.hpp>

#include "redis_test_support.hpp"

namespace {

using crpc::redis::command;
using crpc::redis::resp_value;
using crpc::redis::scan_batch;
using crpc::redis::scan_cursor;
using crpc::redis::scan_kind;
using crpc::redis::scan_options;
using redis_test::array;
using redis_test::text;

// Replays scripted replies and records the commands, so a case can assert both the
// cursor that was sent and how the reply was grouped.
class recording_executor final : public crpc::redis::command_executor {
public:
    std::deque<resp_value> replies;
    std::vector<command> executed;

    crpc::result<resp_value> execute(const command& cmd) override {
        executed.push_back(cmd);
        if (replies.empty()) {
            return crpc::rpc_error{crpc::rpc_code::internal, "no scripted reply"};
        }
        resp_value reply = std::move(replies.front());
        replies.pop_front();
        return reply;
    }

    crpc::result<std::vector<resp_value>> execute_batch(
        const std::vector<command>& commands) override {
        std::vector<resp_value> out;
        out.reserve(commands.size());
        for (const command& cmd : commands) {
            auto reply = execute(cmd);
            if (!reply) {
                return reply.error();
            }
            out.push_back(std::move(reply.value()));
        }
        return out;
    }
};

[[nodiscard]] resp_value batch_reply(std::string_view cursor,
                                     std::initializer_list<const char*> items) {
    std::vector<resp_value> values;
    for (const char* item : items) {
        values.push_back(text(item));
    }
    return array({text(std::string(cursor)), array(std::move(values))});
}

} // namespace

CRPC_TEST(scan_keys_walks_the_cursor) {
    recording_executor executor;
    executor.replies.push_back(batch_reply("17", {"a", "b"}));
    executor.replies.push_back(batch_reply("0", {"c"}));

    scan_cursor cursor(executor, scan_kind::keys, "", scan_options{});
    auto first = cursor.next();
    CRPC_CHECK(first.ok());
    if (!first.ok()) {
        return;
    }
    // The first call must send cursor "0"; the server's cursor is echoed back.
    CRPC_CHECK_EQ(std::size_t{1}, executor.executed.size());
    CRPC_CHECK_EQ(std::string("SCAN"), executor.executed[0][0]);
    CRPC_CHECK_EQ(std::string("0"), executor.executed[0][1]);
    CRPC_CHECK_FALSE(first.value().done);
    CRPC_CHECK_EQ(std::size_t{2}, first.value().entries.size());
    CRPC_CHECK_EQ(std::string("a"), first.value().entries[0].name);

    auto second = cursor.next();
    CRPC_CHECK(second.ok());
    if (!second.ok()) {
        return;
    }
    // The second call sends the cursor the server returned, not a counter.
    CRPC_CHECK_EQ(std::string("17"), executor.executed[1][1]);
    CRPC_CHECK(second.value().done);
    CRPC_CHECK_EQ(std::size_t{1}, second.value().entries.size());
    CRPC_CHECK(cursor.finished());
    CRPC_CHECK_EQ(std::size_t{2}, cursor.steps());

    // A call after the end is an empty done batch, not an error and not a loop.
    auto third = cursor.next();
    CRPC_CHECK(third.ok());
    if (third.ok()) {
        CRPC_CHECK(third.value().done);
        CRPC_CHECK(third.value().entries.empty());
    }
    CRPC_CHECK_EQ(std::size_t{2}, executor.executed.size());
}

CRPC_TEST(scan_passes_match_and_count) {
    recording_executor executor;
    executor.replies.push_back(batch_reply("0", {}));
    scan_options options;
    options.count = 500;
    options.match = "user:*";
    options.type = "string";

    scan_cursor cursor(executor, scan_kind::keys, "", options);
    auto batch = cursor.next();
    CRPC_CHECK(batch.ok());

    // SCAN 0 MATCH user:* COUNT 500 TYPE string
    const command& cmd = executor.executed[0];
    CRPC_CHECK_EQ(std::size_t{8}, cmd.size());
    CRPC_CHECK_EQ(std::string("SCAN"), cmd[0]);
    CRPC_CHECK_EQ(std::string("0"), cmd[1]);
    CRPC_CHECK_EQ(std::string("MATCH"), cmd[2]);
    CRPC_CHECK_EQ(std::string("user:*"), cmd[3]);
    CRPC_CHECK_EQ(std::string("COUNT"), cmd[4]);
    CRPC_CHECK_EQ(std::string("500"), cmd[5]);
    CRPC_CHECK_EQ(std::string("TYPE"), cmd[6]);
    CRPC_CHECK_EQ(std::string("string"), cmd[7]);
}

CRPC_TEST(scan_hash_returns_field_value_pairs) {
    recording_executor executor;
    executor.replies.push_back(batch_reply("0", {"f1", "v1", "f2", "v2"}));

    scan_cursor cursor(executor, scan_kind::hash, "h", scan_options{});
    auto batch = cursor.next();
    CRPC_CHECK(batch.ok());
    if (!batch.ok()) {
        return;
    }
    // HSCAN takes the key before the cursor.
    CRPC_CHECK_EQ(std::string("HSCAN"), executor.executed[0][0]);
    CRPC_CHECK_EQ(std::string("h"), executor.executed[0][1]);
    CRPC_CHECK_EQ(std::string("0"), executor.executed[0][2]);
    CRPC_CHECK_EQ(std::size_t{2}, batch.value().entries.size());
    CRPC_CHECK_EQ(std::string("f1"), batch.value().entries[0].name);
    CRPC_CHECK_EQ(std::string("v1"), batch.value().entries[0].value);
    CRPC_CHECK_EQ(std::string("f2"), batch.value().entries[1].name);
}

CRPC_TEST(scan_sorted_set_returns_member_score_pairs) {
    recording_executor executor;
    executor.replies.push_back(batch_reply("0", {"alice", "1.5", "bob", "2"}));

    scan_cursor cursor(executor, scan_kind::sorted_set, "z", scan_options{});
    auto batch = cursor.next();
    CRPC_CHECK(batch.ok());
    if (!batch.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::string("ZSCAN"), executor.executed[0][0]);
    CRPC_CHECK_EQ(std::size_t{2}, batch.value().entries.size());
    CRPC_CHECK_EQ(std::string("alice"), batch.value().entries[0].name);
    CRPC_CHECK_EQ(std::string("1.5"), batch.value().entries[0].value);
}

CRPC_TEST(scan_rejects_a_malformed_reply) {
    {
        // Not an array of two.
        recording_executor executor;
        executor.replies.push_back(text("0"));
        scan_cursor cursor(executor, scan_kind::keys, "", scan_options{});
        auto batch = cursor.next();
        CRPC_CHECK_FALSE(batch.ok());
        if (!batch.ok()) {
            CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                          static_cast<int>(batch.error().code));
        }
    }
    {
        // A paired scan with an odd item count would silently drop the last field.
        recording_executor executor;
        executor.replies.push_back(batch_reply("0", {"f1", "v1", "f2"}));
        scan_cursor cursor(executor, scan_kind::hash, "h", scan_options{});
        auto batch = cursor.next();
        CRPC_CHECK_FALSE(batch.ok());
    }
}

CRPC_TEST(scan_reset_restarts_at_zero) {
    recording_executor executor;
    executor.replies.push_back(batch_reply("0", {"a"}));
    executor.replies.push_back(batch_reply("0", {"a"}));

    scan_cursor cursor(executor, scan_kind::keys, "", scan_options{});
    CRPC_CHECK(cursor.next().ok());
    CRPC_CHECK(cursor.finished());

    cursor.reset();
    CRPC_CHECK_FALSE(cursor.finished());
    CRPC_CHECK(cursor.next().ok());
    // Both first calls sent "0".
    CRPC_CHECK_EQ(std::string("0"), executor.executed[0][1]);
    CRPC_CHECK_EQ(std::string("0"), executor.executed[1][1]);
}

CRPC_TEST(scan_each_visits_every_entry_and_can_stop) {
    recording_executor executor;
    executor.replies.push_back(batch_reply("9", {"a", "b"}));
    executor.replies.push_back(batch_reply("0", {"c"}));

    std::vector<std::string> visited;
    const std::size_t total = crpc::redis::scan_each(
        executor, scan_kind::keys, "", scan_options{}, [&](const scan_batch::entry& entry) {
            visited.push_back(entry.name);
            return true;
        });
    CRPC_CHECK_EQ(std::size_t{3}, total);
    CRPC_CHECK_EQ(std::size_t{3}, visited.size());
    CRPC_CHECK_EQ(std::string("c"), visited[2]);

    // A visitor that returns false stops the scan without draining it.
    recording_executor early;
    early.replies.push_back(batch_reply("9", {"a", "b", "c"}));
    std::size_t seen = 0;
    const std::size_t stopped = crpc::redis::scan_each(
        early, scan_kind::keys, "", scan_options{}, [&](const scan_batch::entry&) {
            ++seen;
            return seen < 2;
        });
    CRPC_CHECK_EQ(std::size_t{2}, stopped);
    CRPC_CHECK_EQ(std::size_t{2}, seen);
}

CRPC_TEST(cluster_scan_deduplicates_across_nodes) {
    recording_executor first;
    recording_executor second;
    // Both nodes hand back "shared": a resharding can do that, and the caller must
    // not see the key twice.
    first.replies.push_back(batch_reply("0", {"a", "shared"}));
    second.replies.push_back(batch_reply("0", {"shared", "b"}));

    crpc::redis::cluster_scan scan({&first, &second}, scan_kind::keys, scan_options{});
    std::vector<std::string> names;
    for (;;) {
        auto batch = scan.next();
        CRPC_CHECK(batch.ok());
        if (!batch.ok()) {
            return;
        }
        for (const scan_batch::entry& entry : batch.value().entries) {
            names.push_back(entry.name);
        }
        if (batch.value().done) {
            break;
        }
    }
    CRPC_CHECK_EQ(std::size_t{3}, names.size());
    CRPC_CHECK_EQ(std::string("a"), names[0]);
    CRPC_CHECK_EQ(std::string("shared"), names[1]);
    CRPC_CHECK_EQ(std::string("b"), names[2]);
    CRPC_CHECK(scan.finished());
}

CRPC_TEST(cluster_scan_rejects_keyed_collections) {
    recording_executor first;
    crpc::redis::cluster_scan scan({&first}, scan_kind::hash, scan_options{});
    auto batch = scan.next();
    CRPC_CHECK_FALSE(batch.ok());
    if (!batch.ok()) {
        // A hash lives on one node, so a cluster-wide scan of it has no meaning.
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(batch.error().code));
    }
    CRPC_CHECK(first.executed.empty());
}

CRPC_TEST_MAIN()