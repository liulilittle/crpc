// CRPC Redis -- Streams command construction and reply decoding.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include <crpc/redis/stream.hpp>

#include "redis_test_support.hpp"
#include "test_framework.hpp"

using namespace redis_test;

namespace {

resp_value make_entry(
    std::string id,
    std::initializer_list<std::pair<std::string, std::string>> fields) {
    std::vector<resp_value> flat;
    for (const auto& field : fields) {
        flat.push_back(text(field.first));
        flat.push_back(text(field.second));
    }
    return array({text(std::move(id)), array(std::move(flat))});
}

} // namespace

CRPC_TEST(stream_id_parses_full_and_short_forms) {
    crpc::redis::stream_id id;

    CRPC_CHECK(crpc::redis::stream_id::parse("123-45", id));
    CRPC_CHECK_EQ(std::uint64_t{123}, id.ms);
    CRPC_CHECK_EQ(std::uint64_t{45}, id.seq);

    CRPC_CHECK(crpc::redis::stream_id::parse("7", id));
    CRPC_CHECK_EQ(std::uint64_t{7}, id.ms);
    CRPC_CHECK_EQ(std::uint64_t{0}, id.seq);

    CRPC_CHECK(crpc::redis::stream_id::parse("0-0", id));
    CRPC_CHECK_EQ(std::uint64_t{0}, id.ms);
    CRPC_CHECK_EQ(std::uint64_t{0}, id.seq);
}

CRPC_TEST(stream_id_rejects_range_markers_and_malformed_text) {
    crpc::redis::stream_id id;
    CRPC_CHECK_FALSE(crpc::redis::stream_id::parse("", id));
    CRPC_CHECK_FALSE(crpc::redis::stream_id::parse("-", id));
    CRPC_CHECK_FALSE(crpc::redis::stream_id::parse("+", id));
    CRPC_CHECK_FALSE(crpc::redis::stream_id::parse("$", id));
    CRPC_CHECK_FALSE(crpc::redis::stream_id::parse("*", id));
    CRPC_CHECK_FALSE(crpc::redis::stream_id::parse("1-", id));
    CRPC_CHECK_FALSE(crpc::redis::stream_id::parse("-1", id));
    CRPC_CHECK_FALSE(crpc::redis::stream_id::parse("1-2-3", id));
    CRPC_CHECK_FALSE(crpc::redis::stream_id::parse("abc", id));
    CRPC_CHECK_FALSE(crpc::redis::stream_id::parse(
        "18446744073709551616", id));
}

CRPC_TEST(stream_id_orders_and_formats) {
    const crpc::redis::stream_id a{1, 9};
    const crpc::redis::stream_id b{2, 0};
    const crpc::redis::stream_id c{2, 1};

    CRPC_CHECK(a < b);
    CRPC_CHECK(b < c);
    CRPC_CHECK(a != b);
    CRPC_CHECK(b == crpc::redis::stream_id(2, 0));
    CRPC_CHECK_EQ(std::string("1-9"), a.to_string());
    CRPC_CHECK_EQ(std::string("2-0"), b.to_string());
}

CRPC_TEST(stream_add_builds_xadd_and_returns_id) {
    command last;
    resp_value reply = text("1-0");
    scripted_executor executor([&](const command& cmd) -> crpc::result<resp_value> {
        last = cmd;
        return reply;
    });

    crpc::redis::stream stream(executor);
    crpc::result<std::string> added = stream.add("s", {{"field", "value"}});
    CRPC_CHECK(added.ok());
    CRPC_CHECK_EQ(std::string("1-0"), added.value());
    CRPC_CHECK_EQ(std::size_t{5}, last.size());
    CRPC_CHECK_EQ(std::string("XADD"), last[0]);
    CRPC_CHECK_EQ(std::string("s"), last[1]);
    CRPC_CHECK_EQ(std::string("*"), last[2]);
    CRPC_CHECK_EQ(std::string("field"), last[3]);
    CRPC_CHECK_EQ(std::string("value"), last[4]);
}

CRPC_TEST(stream_add_builds_trim_and_nomkstream_options) {
    command last;
    scripted_executor executor([&](const command& cmd) -> crpc::result<resp_value> {
        last = cmd;
        return text("5-0");
    });

    crpc::redis::stream_add_options options;
    options.nomkstream = true;
    options.trim = crpc::redis::stream_trim_kind::maxlen;
    options.approximate = true;
    options.maxlen = 100;
    options.limit = 10;

    crpc::redis::stream stream(executor);
    crpc::result<std::string> added =
        stream.add("s", {{"f", "v"}}, "5-0", options);
    CRPC_CHECK(added.ok());
    CRPC_CHECK_EQ(std::size_t{11}, last.size());
    CRPC_CHECK_EQ(std::string("XADD"), last[0]);
    CRPC_CHECK_EQ(std::string("NOMKSTREAM"), last[2]);
    CRPC_CHECK_EQ(std::string("MAXLEN"), last[3]);
    CRPC_CHECK_EQ(std::string("~"), last[4]);
    CRPC_CHECK_EQ(std::string("100"), last[5]);
    CRPC_CHECK_EQ(std::string("LIMIT"), last[6]);
    CRPC_CHECK_EQ(std::string("10"), last[7]);
    CRPC_CHECK_EQ(std::string("5-0"), last[8]);
}

CRPC_TEST(stream_add_builds_minid_trim) {
    command last;
    scripted_executor executor([&](const command& cmd) -> crpc::result<resp_value> {
        last = cmd;
        return text("9-0");
    });

    crpc::redis::stream_add_options options;
    options.trim = crpc::redis::stream_trim_kind::minid;
    options.approximate = false;
    options.minid = "8-0";

    crpc::redis::stream stream(executor);
    CRPC_CHECK(stream.add("s", {{"f", "v"}}, options).ok());
    CRPC_CHECK_EQ(std::string("MINID"), last[2]);
    CRPC_CHECK_EQ(std::string("="), last[3]);
    CRPC_CHECK_EQ(std::string("8-0"), last[4]);
}

CRPC_TEST(stream_add_null_reply_is_not_found) {
    scripted_executor executor([](const command&) -> crpc::result<resp_value> {
        return null_value();
    });
    crpc::redis::stream stream(executor);
    crpc::result<std::string> added = stream.add("s", {{"f", "v"}});
    CRPC_CHECK_FALSE(added.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                  static_cast<int>(added.code()));
}

CRPC_TEST(stream_add_error_reply_is_application_error) {
    scripted_executor executor([](const command&) -> crpc::result<resp_value> {
        return error("WRONGTYPE Operation against a key");
    });
    crpc::redis::stream stream(executor);
    crpc::result<std::string> added = stream.add("s", {{"f", "v"}});
    CRPC_CHECK_FALSE(added.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::application),
                  static_cast<int>(added.code()));
}

CRPC_TEST(stream_add_rejects_invalid_arguments_locally) {
    scripted_executor executor([](const command&) -> crpc::result<resp_value> {
        return text("1-0");
    });
    crpc::redis::stream stream(executor);

    crpc::result<std::string> empty_fields = stream.add("s", {});
    CRPC_CHECK_FALSE(empty_fields.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(empty_fields.code()));

    crpc::redis::stream_add_options bad_maxlen;
    bad_maxlen.trim = crpc::redis::stream_trim_kind::maxlen;
    bad_maxlen.maxlen = 0;
    crpc::result<std::string> trimmed = stream.add("s", {{"f", "v"}}, bad_maxlen);
    CRPC_CHECK_FALSE(trimmed.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(trimmed.code()));

    crpc::redis::stream_add_options bad_minid;
    bad_minid.trim = crpc::redis::stream_trim_kind::minid;
    crpc::result<std::string> minid = stream.add("s", {{"f", "v"}}, bad_minid);
    CRPC_CHECK_FALSE(minid.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(minid.code()));

    crpc::redis::stream_add_options bad_limit;
    bad_limit.trim = crpc::redis::stream_trim_kind::maxlen;
    bad_limit.approximate = false;
    bad_limit.maxlen = 5;
    bad_limit.limit = 1;
    crpc::result<std::string> limited =
        stream.add("s", {{"f", "v"}}, bad_limit);
    CRPC_CHECK_FALSE(limited.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(limited.code()));

    crpc::redis::stream_add_options orphan_limit;
    orphan_limit.limit = 1;
    crpc::result<std::string> orphan =
        stream.add("s", {{"f", "v"}}, orphan_limit);
    CRPC_CHECK_FALSE(orphan.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(orphan.code()));

    CRPC_CHECK(executor.executed.empty());
}

CRPC_TEST(stream_range_parses_entries) {
    command last;
    scripted_executor executor([&](const command& cmd) -> crpc::result<resp_value> {
        last = cmd;
        return array({make_entry("1-0", {{"a", "1"}, {"b", "2"}}),
                      make_entry("2-0", {{"c", "3"}})});
    });

    crpc::redis::stream stream(executor);
    crpc::result<std::vector<crpc::redis::stream_entry>> entries =
        stream.range("s", "-", "+", 10);
    CRPC_CHECK(entries.ok());
    CRPC_CHECK_EQ(std::size_t{2}, entries.value().size());
    CRPC_CHECK_EQ(std::string("1-0"), entries.value()[0].id);
    CRPC_CHECK_EQ(std::uint64_t{1}, entries.value()[0].parsed_id.ms);
    CRPC_CHECK_EQ(std::size_t{2}, entries.value()[0].fields.size());
    CRPC_CHECK_EQ(std::string("a"), entries.value()[0].fields[0].first);
    CRPC_CHECK_EQ(std::string("1"), entries.value()[0].fields[0].second);
    CRPC_CHECK_EQ(std::string("2-0"), entries.value()[1].id);

    CRPC_CHECK_EQ(std::size_t{6}, last.size());
    CRPC_CHECK_EQ(std::string("XRANGE"), last[0]);
    CRPC_CHECK_EQ(std::string("COUNT"), last[4]);
    CRPC_CHECK_EQ(std::string("10"), last[5]);
}

CRPC_TEST(stream_reverse_range_swaps_bounds) {
    command last;
    scripted_executor executor([&](const command& cmd) -> crpc::result<resp_value> {
        last = cmd;
        return array({});
    });

    crpc::redis::stream stream(executor);
    CRPC_CHECK(stream.reverse_range("s").ok());
    CRPC_CHECK_EQ(std::string("XREVRANGE"), last[0]);
    CRPC_CHECK_EQ(std::string("+"), last[1 + 1]);
    CRPC_CHECK_EQ(std::string("-"), last[1 + 2]);
}

CRPC_TEST(stream_read_parses_streams_and_builds_options) {
    command last;
    scripted_executor executor([&](const command& cmd) -> crpc::result<resp_value> {
        last = cmd;
        return array({array({text("s1"), array({make_entry("1-0", {{"f", "v"}})})}),
                      array({text("s2"), array({})})});
    });

    crpc::redis::stream_read_options options;
    options.count = 5;
    options.block = std::chrono::milliseconds(100);

    crpc::redis::stream stream(executor);
    crpc::result<std::vector<crpc::redis::stream_read>> reads =
        stream.read({"s1", "s2"}, {"0-0", "0-0"}, options);
    CRPC_CHECK(reads.ok());
    CRPC_CHECK_EQ(std::size_t{2}, reads.value().size());
    CRPC_CHECK_EQ(std::string("s1"), reads.value()[0].key);
    CRPC_CHECK_EQ(std::size_t{1}, reads.value()[0].entries.size());
    CRPC_CHECK_EQ(std::string("s2"), reads.value()[1].key);
    CRPC_CHECK(reads.value()[1].entries.empty());

    CRPC_CHECK_EQ(std::size_t{10}, last.size());
    CRPC_CHECK_EQ(std::string("XREAD"), last[0]);
    CRPC_CHECK_EQ(std::string("COUNT"), last[1]);
    CRPC_CHECK_EQ(std::string("5"), last[2]);
    CRPC_CHECK_EQ(std::string("BLOCK"), last[3]);
    CRPC_CHECK_EQ(std::string("100"), last[4]);
    CRPC_CHECK_EQ(std::string("STREAMS"), last[5]);
}

CRPC_TEST(stream_read_parses_resp3_map_reply) {
    scripted_executor executor([](const command&) -> crpc::result<resp_value> {
        return map({{text("s1"), array({make_entry("1-0", {{"f", "v"}})})}});
    });
    crpc::redis::stream stream(executor);
    crpc::result<std::vector<crpc::redis::stream_read>> reads =
        stream.read({"s1"}, {"0-0"});
    CRPC_CHECK(reads.ok());
    CRPC_CHECK_EQ(std::size_t{1}, reads.value().size());
    CRPC_CHECK_EQ(std::string("s1"), reads.value()[0].key);
    CRPC_CHECK_EQ(std::size_t{1}, reads.value()[0].entries.size());
}

CRPC_TEST(stream_range_parses_resp3_field_map) {
    scripted_executor executor([](const command&) -> crpc::result<resp_value> {
        return array({array({text("1-0"),
                             map({{text("f"), text("v")}, {text("g"), text("w")}})})});
    });
    crpc::redis::stream stream(executor);
    crpc::result<std::vector<crpc::redis::stream_entry>> entries =
        stream.range("s");
    CRPC_CHECK(entries.ok());
    CRPC_CHECK_EQ(std::size_t{1}, entries.value().size());
    CRPC_CHECK_EQ(std::size_t{2}, entries.value()[0].fields.size());
    CRPC_CHECK_EQ(std::string("f"), entries.value()[0].fields[0].first);
    CRPC_CHECK_EQ(std::string("v"), entries.value()[0].fields[0].second);
}

CRPC_TEST(stream_read_null_reply_is_empty) {
    scripted_executor executor([](const command&) -> crpc::result<resp_value> {
        return null_value();
    });
    crpc::redis::stream stream(executor);
    crpc::result<std::vector<crpc::redis::stream_read>> reads =
        stream.read({"s"}, {"$"});
    CRPC_CHECK(reads.ok());
    CRPC_CHECK(reads.value().empty());
}

CRPC_TEST(stream_read_validates_key_and_id_counts) {
    scripted_executor executor([](const command&) -> crpc::result<resp_value> {
        return array({});
    });
    crpc::redis::stream stream(executor);

    crpc::result<std::vector<crpc::redis::stream_read>> empty =
        stream.read({}, {});
    CRPC_CHECK_FALSE(empty.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(empty.code()));

    crpc::result<std::vector<crpc::redis::stream_read>> mismatch =
        stream.read({"a", "b"}, {"0-0"});
    CRPC_CHECK_FALSE(mismatch.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                  static_cast<int>(mismatch.code()));
}

CRPC_TEST(stream_read_group_builds_group_clause) {
    command last;
    scripted_executor executor([&](const command& cmd) -> crpc::result<resp_value> {
        last = cmd;
        return array({});
    });

    crpc::redis::stream_read_options options;
    options.count = 2;
    options.block = std::chrono::milliseconds(50);
    options.noack = true;

    crpc::redis::stream stream(executor);
    CRPC_CHECK(stream.read_group("g", "c", {"s"}, {"0"}, options).ok());
    CRPC_CHECK_EQ(std::size_t{12}, last.size());
    CRPC_CHECK_EQ(std::string("XREADGROUP"), last[0]);
    CRPC_CHECK_EQ(std::string("GROUP"), last[1]);
    CRPC_CHECK_EQ(std::string("g"), last[2]);
    CRPC_CHECK_EQ(std::string("c"), last[3]);
    CRPC_CHECK_EQ(std::string("COUNT"), last[4]);
    CRPC_CHECK_EQ(std::string("BLOCK"), last[6]);
    CRPC_CHECK_EQ(std::string("NOACK"), last[8]);
    CRPC_CHECK_EQ(std::string("STREAMS"), last[9]);
}

CRPC_TEST(stream_read_group_accepts_deleted_entry) {
    scripted_executor executor([](const command&) -> crpc::result<resp_value> {
        return array({array({text("s"), array({array({text("1-0"), null_value()})})})});
    });
    crpc::redis::stream stream(executor);
    crpc::result<std::vector<crpc::redis::stream_read>> reads =
        stream.read_group("g", "c", {"s"}, {">"});
    CRPC_CHECK(reads.ok());
    CRPC_CHECK_EQ(std::size_t{1}, reads.value()[0].entries.size());
    CRPC_CHECK_EQ(std::string("1-0"), reads.value()[0].entries[0].id);
    CRPC_CHECK(reads.value()[0].entries[0].fields.empty());
}

CRPC_TEST(stream_scalar_commands) {
    command last;
    resp_value reply = integer(3);
    scripted_executor executor([&](const command& cmd) -> crpc::result<resp_value> {
        last = cmd;
        return reply;
    });
    crpc::redis::stream stream(executor);

    CRPC_CHECK(stream.length("s").ok());
    CRPC_CHECK_EQ(std::string("XLEN"), last[0]);

    reply = integer(2);
    CRPC_CHECK_EQ(std::int64_t{2}, stream.del("s", {"1-0", "2-0"}).value());
    CRPC_CHECK_EQ(std::string("XDEL"), last[0]);
    CRPC_CHECK_EQ(std::size_t{4}, last.size());

    reply = integer(1);
    crpc::redis::stream_trim_options trim;
    trim.trim = crpc::redis::stream_trim_kind::maxlen;
    trim.approximate = true;
    trim.maxlen = 5;
    trim.limit = 3;
    CRPC_CHECK_EQ(std::int64_t{1}, stream.trim("s", trim).value());
    CRPC_CHECK_EQ(std::string("XTRIM"), last[0]);
    CRPC_CHECK_EQ(std::string("MAXLEN"), last[2]);
    CRPC_CHECK_EQ(std::string("~"), last[3]);
    CRPC_CHECK_EQ(std::string("LIMIT"), last[5]);

    reply = integer(0);
    crpc::redis::stream_trim_options exact;
    exact.trim = crpc::redis::stream_trim_kind::maxlen;
    exact.approximate = false;
    exact.maxlen = 5;
    CRPC_CHECK_EQ(std::int64_t{0}, stream.trim("s", exact).value());
    CRPC_CHECK_EQ(std::string("="), last[3]);

    reply = integer(1);
    CRPC_CHECK_EQ(std::int64_t{1}, stream.ack("s", "g", {"1-0"}).value());
    CRPC_CHECK_EQ(std::string("XACK"), last[0]);
}

CRPC_TEST(stream_group_commands) {
    command last;
    resp_value reply = simple("OK");
    scripted_executor executor([&](const command& cmd) -> crpc::result<resp_value> {
        last = cmd;
        return reply;
    });
    crpc::redis::stream stream(executor);

    CRPC_CHECK(stream.group_create("s", "g", "$", true).ok());
    CRPC_CHECK_EQ(std::string("XGROUP"), last[0]);
    CRPC_CHECK_EQ(std::string("CREATE"), last[1]);
    CRPC_CHECK_EQ(std::string("MKSTREAM"), last[5]);

    reply = integer(1);
    CRPC_CHECK_EQ(std::int64_t{1}, stream.group_destroy("s", "g").value());
    CRPC_CHECK_EQ(std::string("DESTROY"), last[1]);

    reply = integer(1);
    CRPC_CHECK_EQ(std::int64_t{1},
                  stream.group_create_consumer("s", "g", "c").value());
    CRPC_CHECK_EQ(std::string("CREATECONSUMER"), last[1]);

    reply = integer(0);
    CRPC_CHECK_EQ(std::int64_t{0},
                  stream.group_del_consumer("s", "g", "c").value());
    CRPC_CHECK_EQ(std::string("DELCONSUMER"), last[1]);
}

CRPC_TEST(stream_malformed_replies_are_protocol_errors) {
    resp_value reply = array({array({text("1-0")})});
    scripted_executor executor([&](const command&) -> crpc::result<resp_value> {
        return reply;
    });
    crpc::redis::stream stream(executor);

    crpc::result<std::vector<crpc::redis::stream_entry>> short_entry =
        stream.range("s");
    CRPC_CHECK_FALSE(short_entry.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(short_entry.code()));

    reply = array({array({text("1-0"), array({text("f")})})});
    crpc::result<std::vector<crpc::redis::stream_entry>> odd_fields =
        stream.range("s");
    CRPC_CHECK_FALSE(odd_fields.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(odd_fields.code()));

    reply = array({array({text("not-an-id"), array({})})});
    crpc::result<std::vector<crpc::redis::stream_entry>> bad_id =
        stream.range("s");
    CRPC_CHECK_FALSE(bad_id.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(bad_id.code()));

    reply = text("not an integer");
    crpc::result<std::int64_t> length = stream.length("s");
    CRPC_CHECK_FALSE(length.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(length.code()));
}

CRPC_TEST(stream_pending_summary_and_range) {
    command last;
    resp_value reply = array(
        {integer(2), text("1-0"), text("2-0"),
         array({array({text("c1"), integer(1)}),
                array({text("c2"), integer(1)})})});
    scripted_executor executor([&](const command& cmd) -> crpc::result<resp_value> {
        last = cmd;
        return reply;
    });
    crpc::redis::stream stream(executor);

    crpc::result<crpc::redis::stream_pending_summary> summary =
        stream.pending("s", "g");
    CRPC_CHECK(summary.ok());
    CRPC_CHECK_EQ(std::int64_t{2}, summary.value().total);
    CRPC_CHECK_EQ(std::string("1-0"), summary.value().smallest_id);
    CRPC_CHECK_EQ(std::string("2-0"), summary.value().greatest_id);
    CRPC_CHECK_EQ(std::size_t{2}, summary.value().consumers.size());
    CRPC_CHECK_EQ(std::string("c2"), summary.value().consumers[1].first);
    CRPC_CHECK_EQ(std::int64_t{1}, summary.value().consumers[1].second);
    CRPC_CHECK_EQ(std::string("XPENDING"), last[0]);
    CRPC_CHECK_EQ(std::size_t{3}, last.size());

    reply = array({array({text("1-0"), text("c1"), integer(100), integer(2)})});
    crpc::result<std::vector<crpc::redis::stream_pending_entry>> pending =
        stream.pending_range("s", "g", "-", "+", 10, "c1",
                             std::chrono::milliseconds(50));
    CRPC_CHECK(pending.ok());
    CRPC_CHECK_EQ(std::size_t{1}, pending.value().size());
    CRPC_CHECK_EQ(std::string("1-0"), pending.value()[0].id);
    CRPC_CHECK_EQ(std::string("c1"), pending.value()[0].consumer);
    CRPC_CHECK_EQ(std::int64_t{100}, pending.value()[0].idle.count());
    CRPC_CHECK_EQ(std::int64_t{2}, pending.value()[0].deliveries);
    CRPC_CHECK_EQ(std::size_t{9}, last.size());
    CRPC_CHECK_EQ(std::string("IDLE"), last[3]);
    CRPC_CHECK_EQ(std::string("50"), last[4]);
    CRPC_CHECK_EQ(std::string("c1"), last[8]);
}

CRPC_TEST(stream_claim_builds_options_and_parses_entries) {
    command last;
    resp_value reply = array({make_entry("1-0", {{"f", "v"}}), text("2-0")});
    scripted_executor executor([&](const command& cmd) -> crpc::result<resp_value> {
        last = cmd;
        return reply;
    });
    crpc::redis::stream stream(executor);

    crpc::redis::stream_claim_options options;
    options.idle = std::chrono::milliseconds(100);
    options.time = std::chrono::milliseconds(200);
    options.retry_count = 3;
    options.force = true;
    options.last_id = "9-0";

    crpc::result<std::vector<crpc::redis::stream_entry>> claimed =
        stream.claim("s", "g", "c", std::chrono::milliseconds(50),
                     {"1-0", "2-0"}, options);
    CRPC_CHECK(claimed.ok());
    CRPC_CHECK_EQ(std::size_t{2}, claimed.value().size());
    CRPC_CHECK_EQ(std::string("1-0"), claimed.value()[0].id);
    CRPC_CHECK_EQ(std::size_t{1}, claimed.value()[0].fields.size());
    CRPC_CHECK_EQ(std::string("2-0"), claimed.value()[1].id);
    CRPC_CHECK(claimed.value()[1].fields.empty());

    CRPC_CHECK_EQ(std::size_t{16}, last.size());
    CRPC_CHECK_EQ(std::string("XCLAIM"), last[0]);
    CRPC_CHECK_EQ(std::string("50"), last[4]);
    CRPC_CHECK_EQ(std::string("IDLE"), last[7]);
    CRPC_CHECK_EQ(std::string("TIME"), last[9]);
    CRPC_CHECK_EQ(std::string("RETRYCOUNT"), last[11]);
    CRPC_CHECK_EQ(std::string("FORCE"), last[13]);
    CRPC_CHECK_EQ(std::string("LASTID"), last[14]);
    CRPC_CHECK_EQ(std::string("9-0"), last[15]);
}

CRPC_TEST(stream_auto_claim_parses_next_id_entries_and_deleted_ids) {
    command last;
    resp_value reply = array(
        {text("3-0"), array({make_entry("1-0", {{"f", "v"}})}),
         array({text("2-0")})});
    scripted_executor executor([&](const command& cmd) -> crpc::result<resp_value> {
        last = cmd;
        return reply;
    });
    crpc::redis::stream stream(executor);

    crpc::result<crpc::redis::stream_autoclaim_result> claimed =
        stream.auto_claim("s", "g", "c", std::chrono::milliseconds(10), "0-0",
                          5);
    CRPC_CHECK(claimed.ok());
    CRPC_CHECK_EQ(std::string("3-0"), claimed.value().next_id);
    CRPC_CHECK_EQ(std::size_t{1}, claimed.value().entries.size());
    CRPC_CHECK_EQ(std::string("1-0"), claimed.value().entries[0].id);
    CRPC_CHECK_EQ(std::size_t{1}, claimed.value().deleted_ids.size());
    CRPC_CHECK_EQ(std::string("2-0"), claimed.value().deleted_ids[0]);
    CRPC_CHECK_EQ(std::string("XAUTOCLAIM"), last[0]);
    CRPC_CHECK_EQ(std::string("COUNT"), last[6]);
    CRPC_CHECK_EQ(std::string("5"), last[7]);
}

CRPC_TEST(stream_pending_rejects_malformed_summary) {
    scripted_executor executor([](const command&) -> crpc::result<resp_value> {
        return array({integer(1), text("1-0")});
    });
    crpc::redis::stream stream(executor);
    crpc::result<crpc::redis::stream_pending_summary> summary =
        stream.pending("s", "g");
    CRPC_CHECK_FALSE(summary.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::protocol_error),
                  static_cast<int>(summary.code()));
}

CRPC_TEST(stream_transport_error_is_propagated) {
    scripted_executor executor([](const command&) -> crpc::result<resp_value> {
        return crpc::rpc_error{crpc::rpc_code::eof, "connection closed"};
    });
    crpc::redis::stream stream(executor);
    crpc::result<std::int64_t> length = stream.length("s");
    CRPC_CHECK_FALSE(length.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::eof),
                  static_cast<int>(length.code()));
}

CRPC_TEST_MAIN()
