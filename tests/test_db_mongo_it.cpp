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

CRPC_TEST_MAIN()