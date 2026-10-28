// CRPC -- MongoDB integration against a real server.
//
// Skipped unless CRPC_MONGO_IT=1. The cases that matter are the ones a scripted server
// cannot prove: a real SCRAM exchange (the proof has to be accepted by a real server that
// derives the same keys), a real cursor, and real BSON round trips through the server.
//
// Connection details come from the environment:
//   CRPC_MONGO_IT=1
//   CRPC_MONGO_HOST (default 127.0.0.1)
//   CRPC_MONGO_PORT (default 27017)
//   CRPC_MONGO_USER (default crpc)
//   CRPC_MONGO_PASSWORD (default crpc_pw)
//   CRPC_MONGO_DB (default crpc_it)
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "test_framework.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/db/mongo/auth.hpp>
#include <crpc/db/mongo/connection.hpp>
#include <crpc/db/mongo/protocol.hpp>
#include <crpc/error.hpp>
#include <crpc/io/iocp_backend.hpp>

namespace {

[[nodiscard]] std::string env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::string(value) : std::string(fallback);
}

[[nodiscard]] bool it_enabled() {
    const char* value = std::getenv("CRPC_MONGO_IT");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

[[nodiscard]] const char* require_server() {
    if (!it_enabled()) {
        crpc_test::skip("CRPC_MONGO_IT is not 1");
    }
    return "";
}

void report(const char* what, const crpc::rpc_error& error) {
    std::printf("  %s failed: %s (%d)\n", what, error.message.c_str(),
                static_cast<int>(error.code));
}

struct it_session {
    crpc::iocp_backend backend;
    std::unique_ptr<crpc::db::mongo::connection> session;

    explicit it_session(std::string_view user = {}, std::string_view password = {}) {
        crpc::db::mongo::connection_options options;
        options.user = user.empty() ? env_or("CRPC_MONGO_USER", "crpc") : std::string(user);
        options.password =
            password.empty() ? env_or("CRPC_MONGO_PASSWORD", "crpc_pw") : std::string(password);
        options.auth_database = "admin";
        options.database = env_or("CRPC_MONGO_DB", "crpc_it");

        const std::string host = env_or("CRPC_MONGO_HOST", "127.0.0.1");
        const std::string port = env_or("CRPC_MONGO_PORT", "27017");
        crpc::net::connection_config config;
        config.ep = crpc::endpoint::tcp(host, static_cast<std::uint16_t>(std::stoi(port)));
        config.operation_timeout = std::chrono::seconds(15);
        auto link = std::make_unique<crpc::db::socket_channel>(std::move(config), backend);
        session = std::unique_ptr<crpc::db::mongo::connection>(
            new crpc::db::mongo::connection(options, std::move(link)));
    }

    [[nodiscard]] bool ok() const { return session != nullptr && session->is_open(); }
};

// A tiny BSON builder for the test documents, so each case reads as data rather than as
// encoder calls.
[[nodiscard]] crpc::db::mongo::bson_document person(std::int32_t id, std::string name,
                                                    std::int32_t age) {
    crpc::db::mongo::bson_document doc;
    doc.append("_id", crpc::db::mongo::bson_value::make_int32(id));
    doc.append("name", crpc::db::mongo::bson_value::make_text(std::move(name)));
    doc.append("age", crpc::db::mongo::bson_value::make_int32(age));
    return doc;
}

// A sale document for the aggregation cases: a region to group by and an amount to sum
// and sort.
[[nodiscard]] crpc::db::mongo::bson_document sale(std::int32_t id, std::string region,
                                                  std::int32_t amount) {
    crpc::db::mongo::bson_document doc;
    doc.append("_id", crpc::db::mongo::bson_value::make_int32(id));
    doc.append("region", crpc::db::mongo::bson_value::make_text(std::move(region)));
    doc.append("amount", crpc::db::mongo::bson_value::make_int32(amount));
    return doc;
}

void drop_collection(it_session& it, std::string_view name) {
    crpc::db::mongo::bson_document drop;
    drop.append("drop", crpc::db::mongo::bson_value::make_text(std::string(name)));
    drop.append("$db", crpc::db::mongo::bson_value::make_text(
                           env_or("CRPC_MONGO_DB", "crpc_it")));
    CRPC_CHECK(static_cast<bool>(it.session->run_command(drop)));
}

// Five sales: east 10, west 20, east 30, west 40, east 50. Every aggregation case starts
// from the same data so the expected numbers are fixed.
void seed_sales(it_session& it) {
    drop_collection(it, "sales");
    std::vector<crpc::db::mongo::bson_document> documents;
    documents.push_back(sale(0, "east", 10));
    documents.push_back(sale(1, "west", 20));
    documents.push_back(sale(2, "east", 30));
    documents.push_back(sale(3, "west", 40));
    documents.push_back(sale(4, "east", 50));
    const auto inserted = it.session->insert("sales", std::move(documents));
    if (!inserted) {
        report("seed sales", inserted.error());
    }
    CRPC_CHECK(static_cast<bool>(inserted));
}

[[nodiscard]] crpc::db::mongo::aggregate_request sales_pipeline() {
    crpc::db::mongo::aggregate_request request;
    request.database = env_or("CRPC_MONGO_DB", "crpc_it");
    request.collection = "sales";
    return request;
}

} // namespace

CRPC_TEST(mongo_it_handshake_and_authentication) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        report("login", it.session->connect_error());
        CRPC_CHECK(it.ok());
        return;
    }
    // The SCRAM proof was accepted, which means the client derived the same keys the
    // server did from the stored credentials.
    CRPC_CHECK(it.session->authenticated());
    CRPC_CHECK_EQ(std::string("SCRAM-SHA-256"), it.session->auth_mechanism_used());
    // hello reported the wire version; OP_MSG needs at least 6.
    CRPC_CHECK(it.session->max_wire_version() >= 6);
    CRPC_CHECK(it.session->max_bson_size() > 0);
    // `hello` on MongoDB 7 does not report a version string; that comes from `buildInfo`,
    // which this client does not call. Asserting it here would encode a false expectation.
    CRPC_CHECK(static_cast<bool>(it.session->ping()));
}

CRPC_TEST(mongo_it_wrong_password_is_rejected) {
    (void)require_server();
    it_session it(env_or("CRPC_MONGO_USER", "crpc"), "definitely-not-the-password");
    CRPC_CHECK_FALSE(it.ok());
    if (!it.ok()) {
        // The server's own message, not a generic "login failed": it is what tells a
        // caller whether the user exists or the password is wrong.
        report("wrong password", it.session->connect_error());
        CRPC_CHECK_FALSE(it.session->connect_error().message.empty());
    }
}

CRPC_TEST(mongo_it_insert_and_find) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        report("login", it.session->connect_error());
        CRPC_CHECK(it.ok());
        return;
    }
    // A clean collection per run: the server keeps data between runs, and a duplicate _id
    // would make the insert fail for a reason unrelated to the case.
    {
        crpc::db::mongo::bson_document drop;
        drop.append("drop", crpc::db::mongo::bson_value::make_text("people"));
        drop.append("$db", crpc::db::mongo::bson_value::make_text(
                               env_or("CRPC_MONGO_DB", "crpc_it")));
        const auto dropped = it.session->run_command(drop);
        CRPC_CHECK(static_cast<bool>(dropped));
    }

    std::vector<crpc::db::mongo::bson_document> documents;
    documents.push_back(person(1, "ada", 36));
    documents.push_back(person(2, "grace", 45));
    documents.push_back(person(3, "alan", 41));
    const auto inserted = it.session->insert("people", std::move(documents));
    if (!inserted) {
        report("insert", inserted.error());
    }
    CRPC_CHECK(static_cast<bool>(inserted));
    if (inserted) {
        CRPC_CHECK_EQ(std::int64_t{3}, inserted.value());
    }

    // A filter, a projection and a limit in one command, which is what exercises the
    // document order and the nested sub-documents.
    crpc::db::mongo::find_request request;
    request.database = env_or("CRPC_MONGO_DB", "crpc_it");
    request.collection = "people";
    request.filter.append("age", crpc::db::mongo::bson_value::make_int32(41));
    request.projection.append("name", crpc::db::mongo::bson_value::make_int32(1));
    request.projection.append("_id", crpc::db::mongo::bson_value::make_int32(0));
    request.limit = 10;

    const auto found = it.session->find(request);
    if (!found) {
        report("find", found.error());
    }
    CRPC_CHECK(static_cast<bool>(found));
    if (found) {
        CRPC_CHECK_EQ(std::size_t{1}, found.value().batch.size());
        if (!found.value().batch.empty()) {
            std::string name;
            CRPC_CHECK(found.value().batch[0].get_text("name", name));
            CRPC_CHECK_EQ(std::string("alan"), name);
            // The projection excluded _id, so it must be absent rather than null.
            CRPC_CHECK(found.value().batch[0].find("_id") == nullptr);
        }
    }
}

CRPC_TEST(mongo_it_cursor_batches_and_release) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }
    {
        crpc::db::mongo::bson_document drop;
        drop.append("drop", crpc::db::mongo::bson_value::make_text("numbers"));
        drop.append("$db", crpc::db::mongo::bson_value::make_text(
                               env_or("CRPC_MONGO_DB", "crpc_it")));
        CRPC_CHECK(static_cast<bool>(it.session->run_command(drop)));
    }
    std::vector<crpc::db::mongo::bson_document> documents;
    for (std::int32_t i = 0; i < 25; ++i) {
        crpc::db::mongo::bson_document doc;
        doc.append("_id", crpc::db::mongo::bson_value::make_int32(i));
        documents.push_back(std::move(doc));
    }
    const auto inserted = it.session->insert("numbers", std::move(documents));
    CRPC_CHECK(static_cast<bool>(inserted));

    // A small batch size forces the server to leave a cursor open, which is the only way
    // to exercise getMore and the release path.
    crpc::db::mongo::find_request request;
    request.database = env_or("CRPC_MONGO_DB", "crpc_it");
    request.collection = "numbers";
    request.batch_size = 5;

    auto found = it.session->find(request);
    if (!found) {
        report("find", found.error());
    }
    CRPC_CHECK(static_cast<bool>(found));
    if (!found) {
        return;
    }
    crpc::db::mongo::cursor state = std::move(found.value());
    CRPC_CHECK_EQ(std::size_t{5}, state.batch.size());
    CRPC_CHECK(state.id != 0);

    std::size_t total = state.batch.size();
    while (state.id != 0) {
        const auto more = it.session->next_batch(state);
        if (!more) {
            report("getMore", more.error());
            CRPC_CHECK(static_cast<bool>(more));
            break;
        }
        if (!more.value()) {
            break;
        }
        total += state.batch.size();
    }
    CRPC_CHECK_EQ(std::size_t{25}, total);
    // The server closes the cursor when it runs out, so nothing is left to release.
    CRPC_CHECK_EQ(std::int64_t{0}, state.id);

    // A cursor abandoned mid-way must be released explicitly; the release is what the
    // server needs to reclaim the resources.
    auto second = it.session->find(request);
    CRPC_CHECK(static_cast<bool>(second));
    if (second) {
        crpc::db::mongo::cursor abandoned = std::move(second.value());
        CRPC_CHECK(abandoned.id != 0);
        const crpc::status released = it.session->close_cursor(abandoned);
        if (!released) {
            report("killCursors", released.error());
        }
        CRPC_CHECK(static_cast<bool>(released));
        CRPC_CHECK_EQ(std::int64_t{0}, abandoned.id);
        // Releasing twice must be a no-op, not a second command.
        CRPC_CHECK(static_cast<bool>(it.session->close_cursor(abandoned)));
    }
}

CRPC_TEST(mongo_it_duplicate_key_reports_the_server_code) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }
    {
        crpc::db::mongo::bson_document drop;
        drop.append("drop", crpc::db::mongo::bson_value::make_text("unique"));
        drop.append("$db", crpc::db::mongo::bson_value::make_text(
                               env_or("CRPC_MONGO_DB", "crpc_it")));
        CRPC_CHECK(static_cast<bool>(it.session->run_command(drop)));
    }
    std::vector<crpc::db::mongo::bson_document> first;
    first.push_back(person(1, "first", 20));
    CRPC_CHECK(static_cast<bool>(it.session->insert("unique", std::move(first))));

    std::vector<crpc::db::mongo::bson_document> duplicate;
    duplicate.push_back(person(1, "second", 30));
    const auto inserted = it.session->insert("unique", std::move(duplicate));
    CRPC_CHECK_FALSE(static_cast<bool>(inserted));
    if (!inserted) {
        report("duplicate", inserted.error());
        // 11000 is "duplicate key"; the message must be the server's own.
        CRPC_CHECK(inserted.error().message.find("E11000") != std::string::npos ||
                   inserted.error().message.find("duplicate") != std::string::npos);
    }
    // The session survives its own error.
    CRPC_CHECK(it.session->is_open());
    CRPC_CHECK(static_cast<bool>(it.session->ping()));
}

CRPC_TEST(mongo_it_bson_types_survive_the_server) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        CRPC_CHECK(it.ok());
        return;
    }
    {
        crpc::db::mongo::bson_document drop;
        drop.append("drop", crpc::db::mongo::bson_value::make_text("types"));
        drop.append("$db", crpc::db::mongo::bson_value::make_text(
                               env_or("CRPC_MONGO_DB", "crpc_it")));
        CRPC_CHECK(static_cast<bool>(it.session->run_command(drop)));
    }

    crpc::db::mongo::bson_document doc;
    doc.append("_id", crpc::db::mongo::bson_value::make_int32(1));
    doc.append("real", crpc::db::mongo::bson_value::make_real(2.5));
    doc.append("big", crpc::db::mongo::bson_value::make_int64(9000000000LL));
    doc.append("text", crpc::db::mongo::bson_value::make_text("héllo"));
    doc.append("flag", crpc::db::mongo::bson_value::make_boolean(true));
    doc.append("nothing", crpc::db::mongo::bson_value::make_null());
    doc.append("bin", crpc::db::mongo::bson_value::make_binary(
                           {std::byte{0x00}, std::byte{0xFF}, std::byte{0x10}}));
    // Named `inner`: `nested` is also the name of the pointer used below, and shadowing it
    // makes the second read of the field a compile error.
    crpc::db::mongo::bson_document inner;
    inner.append("deep", crpc::db::mongo::bson_value::make_int32(7));
    doc.append("nested", crpc::db::mongo::bson_value::make_document(std::move(inner)));
    doc.append("list", crpc::db::mongo::bson_value::make_array(
                           {crpc::db::mongo::bson_value::make_int32(1),
                            crpc::db::mongo::bson_value::make_text("two")}));

    std::vector<crpc::db::mongo::bson_document> documents;
    documents.push_back(std::move(doc));
    const auto inserted = it.session->insert("types", std::move(documents));
    if (!inserted) {
        report("insert types", inserted.error());
    }
    CRPC_CHECK(static_cast<bool>(inserted));

    crpc::db::mongo::find_request request;
    request.database = env_or("CRPC_MONGO_DB", "crpc_it");
    request.collection = "types";
    request.filter.append("_id", crpc::db::mongo::bson_value::make_int32(1));
    const auto found = it.session->find(request);
    if (!found) {
        report("find types", found.error());
    }
    CRPC_CHECK(static_cast<bool>(found));
    if (!found || found.value().batch.empty()) {
        return;
    }
    const crpc::db::mongo::bson_document& back = found.value().batch[0];
    double real = 0.0;
    CRPC_CHECK(back.get_real("real", real));
    CRPC_CHECK_EQ(2.5, real);
    std::int64_t big = 0;
    CRPC_CHECK(back.get_int64("big", big));
    CRPC_CHECK_EQ(std::int64_t{9000000000LL}, big);
    std::string text;
    CRPC_CHECK(back.get_text("text", text));
    CRPC_CHECK_EQ(std::string("héllo"), text);
    bool flag = false;
    CRPC_CHECK(back.get_boolean("flag", flag));
    CRPC_CHECK(flag);
    const crpc::db::mongo::bson_value* nothing = back.find("nothing");
    CRPC_CHECK(nothing != nullptr);
    if (nothing != nullptr) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::db::mongo::bson_type::null),
                      static_cast<int>(nothing->type));
    }
    const crpc::db::mongo::bson_value* binary = back.find("bin");
    CRPC_CHECK(binary != nullptr);
    if (binary != nullptr) {
        CRPC_CHECK_EQ(std::size_t{3}, binary->bytes.size());
    }
    const crpc::db::mongo::bson_document* nested = back.get_document("nested");
    CRPC_CHECK(nested != nullptr);
    if (nested != nullptr) {
        std::int64_t deep = 0;
        CRPC_CHECK(nested->get_int64("deep", deep));
        CRPC_CHECK_EQ(std::int64_t{7}, deep);
    }
    const crpc::db::mongo::bson_value* list = back.find("list");
    CRPC_CHECK(list != nullptr);
    if (list != nullptr) {
        CRPC_CHECK_EQ(std::size_t{2}, list->array.size());
    }
}

CRPC_TEST(mongo_it_aggregate_match_and_group) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        report("login", it.session->connect_error());
        CRPC_CHECK(it.ok());
        return;
    }
    seed_sales(it);

    crpc::db::mongo::aggregate_request request = sales_pipeline();
    crpc::db::mongo::bson_document match_body;
    match_body.append("region", crpc::db::mongo::bson_value::make_text("east"));
    crpc::db::mongo::bson_document match_stage;
    match_stage.append("$match", crpc::db::mongo::bson_value::make_document(
                                     std::move(match_body)));
    request.pipeline.push_back(std::move(match_stage));

    crpc::db::mongo::bson_document group_body;
    group_body.append("_id", crpc::db::mongo::bson_value::make_text("$region"));
    crpc::db::mongo::bson_document count_body;
    count_body.append("$sum", crpc::db::mongo::bson_value::make_int32(1));
    group_body.append("n", crpc::db::mongo::bson_value::make_document(std::move(count_body)));
    crpc::db::mongo::bson_document total_body;
    total_body.append("$sum", crpc::db::mongo::bson_value::make_text("$amount"));
    group_body.append("total",
                      crpc::db::mongo::bson_value::make_document(std::move(total_body)));
    crpc::db::mongo::bson_document group_stage;
    group_stage.append("$group", crpc::db::mongo::bson_value::make_document(
                                     std::move(group_body)));
    request.pipeline.push_back(std::move(group_stage));

    const auto aggregated = it.session->aggregate(request);
    if (!aggregated) {
        report("aggregate $match/$group", aggregated.error());
    }
    CRPC_CHECK(static_cast<bool>(aggregated));
    if (!aggregated) {
        return;
    }
    // One group, small enough to come back whole: a group over a collection is the
    // ordinary aggregation result and must not need a second round trip.
    CRPC_CHECK_EQ(std::int64_t{0}, aggregated.value().id);
    CRPC_CHECK_EQ(std::size_t{1}, aggregated.value().batch.size());
    if (aggregated.value().batch.empty()) {
        return;
    }
    const crpc::db::mongo::bson_document& row = aggregated.value().batch[0];
    std::string region;
    CRPC_CHECK(row.get_text("_id", region));
    CRPC_CHECK_EQ(std::string("east"), region);
    std::int64_t count = 0;
    CRPC_CHECK(row.get_int64("n", count));
    CRPC_CHECK_EQ(std::int64_t{3}, count);
    std::int64_t total = 0;
    CRPC_CHECK(row.get_int64("total", total));
    CRPC_CHECK_EQ(std::int64_t{90}, total);
}

CRPC_TEST(mongo_it_aggregate_sort_order) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        report("login", it.session->connect_error());
        CRPC_CHECK(it.ok());
        return;
    }
    seed_sales(it);

    crpc::db::mongo::aggregate_request request = sales_pipeline();
    crpc::db::mongo::bson_document direction;
    direction.append("amount", crpc::db::mongo::bson_value::make_int32(-1));
    crpc::db::mongo::bson_document sort_stage;
    sort_stage.append("$sort", crpc::db::mongo::bson_value::make_document(
                                   std::move(direction)));
    request.pipeline.push_back(std::move(sort_stage));

    const auto aggregated = it.session->aggregate(request);
    if (!aggregated) {
        report("aggregate $sort", aggregated.error());
    }
    CRPC_CHECK(static_cast<bool>(aggregated));
    if (!aggregated) {
        return;
    }
    CRPC_CHECK_EQ(std::int64_t{0}, aggregated.value().id);
    CRPC_CHECK_EQ(std::size_t{5}, aggregated.value().batch.size());
    // Descending by amount: 50, 40, 30, 20, 10.
    const std::int64_t expected[] = {50, 40, 30, 20, 10};
    for (std::size_t i = 0; i < aggregated.value().batch.size() && i < 5; ++i) {
        std::int64_t amount = 0;
        CRPC_CHECK(aggregated.value().batch[i].get_int64("amount", amount));
        CRPC_CHECK_EQ(expected[i], amount);
    }
}

CRPC_TEST(mongo_it_aggregate_crosses_batches_and_releases_the_cursor) {
    (void)require_server();
    it_session it;
    if (!it.ok()) {
        report("login", it.session->connect_error());
        CRPC_CHECK(it.ok());
        return;
    }
    seed_sales(it);

    crpc::db::mongo::aggregate_request request = sales_pipeline();
    crpc::db::mongo::bson_document direction;
    direction.append("amount", crpc::db::mongo::bson_value::make_int32(1));
    crpc::db::mongo::bson_document sort_stage;
    sort_stage.append("$sort", crpc::db::mongo::bson_value::make_document(
                                   std::move(direction)));
    request.pipeline.push_back(std::move(sort_stage));
    // Five documents with a batch of two: the first batch cannot hold them all, so the
    // rest has to come through getMore on the same cursor.
    request.batch_size = 2;

    auto first = it.session->aggregate(request);
    if (!first) {
        report("aggregate batches", first.error());
    }
    CRPC_CHECK(static_cast<bool>(first));
    if (!first) {
        return;
    }
    crpc::db::mongo::cursor state = std::move(first.value());
    CRPC_CHECK_EQ(std::size_t{2}, state.batch.size());
    CRPC_CHECK(state.id != 0);
    // The id the server handed out, kept so the release can be proven after the cursor is
    // exhausted and its reply id has become 0.
    const std::int64_t first_cursor_id = state.id;

    std::vector<std::int64_t> amounts;
    std::vector<std::int64_t> ids;
    auto collect = [&amounts, &ids](const crpc::db::mongo::cursor& current) {
        for (const crpc::db::mongo::bson_document& document : current.batch) {
            std::int64_t amount = 0;
            CRPC_CHECK(document.get_int64("amount", amount));
            amounts.push_back(amount);
            std::int64_t id = 0;
            CRPC_CHECK(document.get_int64("_id", id));
            ids.push_back(id);
        }
    };
    collect(state);
    while (state.id != 0) {
        const auto more = it.session->next_batch(state);
        if (!more) {
            report("aggregate getMore", more.error());
            CRPC_CHECK(static_cast<bool>(more));
            break;
        }
        if (!more.value()) {
            break;
        }
        collect(state);
    }
    CRPC_CHECK_EQ(std::size_t{5}, amounts.size());
    CRPC_CHECK_EQ(std::size_t{5}, ids.size());
    // Ascending by amount, every document exactly once: a batch boundary that repeats or
    // drops a document shows up here.
    const std::int64_t expected[] = {10, 20, 30, 40, 50};
    for (std::size_t i = 0; i < amounts.size() && i < 5; ++i) {
        CRPC_CHECK_EQ(expected[i], amounts[i]);
    }
    std::vector<std::int64_t> sorted_ids = ids;
    std::sort(sorted_ids.begin(), sorted_ids.end());
    const auto distinct_end = std::unique(sorted_ids.begin(), sorted_ids.end());
    CRPC_CHECK_EQ(std::size_t{5},
                  static_cast<std::size_t>(std::distance(sorted_ids.begin(), distinct_end)));
    // An exhausted cursor is closed by the server, which reports id 0.
    CRPC_CHECK_EQ(std::int64_t{0}, state.id);
    // The id previously handed out must now be unknown to the server. Proving that needs
    // a getMore: a client that only trusted its own id 0 could have leaked the cursor.
    const auto after_exhaustion =
        it.session->run_command(crpc::db::mongo::make_get_more(
            env_or("CRPC_MONGO_DB", "crpc_it"), first_cursor_id, "sales"));
    CRPC_CHECK(static_cast<bool>(after_exhaustion));
    if (after_exhaustion) {
        CRPC_CHECK_FALSE(after_exhaustion.value().ok);
        std::int64_t code = 0;
        CRPC_CHECK(after_exhaustion.value().reply.get_int64("code", code));
        // 43 is CursorNotFound: the server no longer has this cursor.
        CRPC_CHECK_EQ(std::int64_t{43}, code);
    }

    // The same cursor path must release a cursor abandoned before it is read out; that is
    // the killCursors call `find` already makes.
    auto second = it.session->aggregate(request);
    CRPC_CHECK(static_cast<bool>(second));
    if (second) {
        crpc::db::mongo::cursor abandoned = std::move(second.value());
        CRPC_CHECK(abandoned.id != 0);
        const std::int64_t abandoned_id = abandoned.id;
        const crpc::status released = it.session->close_cursor(abandoned);
        if (!released) {
            report("aggregate killCursors", released.error());
        }
        CRPC_CHECK(static_cast<bool>(released));
        CRPC_CHECK_EQ(std::int64_t{0}, abandoned.id);
        CRPC_CHECK(static_cast<bool>(it.session->close_cursor(abandoned)));
        const auto killed = it.session->run_command(crpc::db::mongo::make_get_more(
            env_or("CRPC_MONGO_DB", "crpc_it"), abandoned_id, "sales"));
        CRPC_CHECK(static_cast<bool>(killed));
        if (killed) {
            CRPC_CHECK_FALSE(killed.value().ok);
            std::int64_t code = 0;
            CRPC_CHECK(killed.value().reply.get_int64("code", code));
            CRPC_CHECK_EQ(std::int64_t{43}, code);
        }
    }
}

CRPC_TEST_MAIN()