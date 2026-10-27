// CRPC -- HBase REST integration against a real gateway.
//
// Skipped unless CRPC_HBASE_IT=1. The cases that matter need a real Stargate: the path/body
// encoding asymmetry, a scanner that is opened and drained, and a 404 for a row that does not
// exist.
//
// Connection details come from the environment:
//   CRPC_HBASE_IT=1
//   CRPC_HBASE_HOST (default 127.0.0.1)
//   CRPC_HBASE_PORT (default 18080)
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "test_framework.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/db/hbase/rest.hpp>
#include <crpc/error.hpp>
#include <crpc/http/client.hpp>
#include <crpc/io/iocp_backend.hpp>

namespace {

[[nodiscard]] std::string env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::string(value) : std::string(fallback);
}

[[nodiscard]] bool it_enabled() {
    const char* value = std::getenv("CRPC_HBASE_IT");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

[[nodiscard]] const char* require_gateway() {
    if (!it_enabled()) {
        crpc_test::skip("CRPC_HBASE_IT is not 1");
    }
    return "";
}

// stderr, not stdout: when a case hangs the process is killed and a buffered stdout loses
// everything, which is exactly when the message is needed.
void report(const char* what, const crpc::rpc_error& error) {
    std::fprintf(stderr, "  %s failed: %s (%d)\n", what, error.message.c_str(),
                 static_cast<int>(error.code));
}

[[nodiscard]] std::vector<std::byte> key_of(std::string_view text) {
    std::vector<std::byte> out(text.size());
    if (!text.empty()) {
        std::memcpy(out.data(), text.data(), text.size());
    }
    return out;
}

[[nodiscard]] std::vector<std::byte> value_of(std::string_view text) {
    return key_of(text);
}

// `span` is explicit here, so a vector has to be viewed rather than passed.
[[nodiscard]] crpc::span<const std::byte> view_of(const std::vector<std::byte>& data) noexcept {
    return crpc::span<const std::byte>(data.data(), data.size());
}

struct it_session {
    crpc::iocp_backend backend;
    std::unique_ptr<crpc::db::hbase::client> session;

    it_session() {
        crpc::db::hbase::client_options options;
        options.host = env_or("CRPC_HBASE_HOST", "127.0.0.1");
        options.port = static_cast<std::uint16_t>(std::stoi(env_or("CRPC_HBASE_PORT", "18080")));
        options.timeout_seconds = 20;

        crpc::http::client_options http_options;
        http_options.host = options.host;
        http_options.port = options.port;
        http_options.timeout_seconds = 20;

        const std::string host = options.host;
        const std::uint16_t port = options.port;
        // Named `owner`: `backend` is also the member name, and shadowing it is a warning
        // here and a confusing capture later.
        crpc::iocp_backend* owner = &this->backend;
        session = std::unique_ptr<crpc::db::hbase::client>(new crpc::db::hbase::client(
            std::move(options), [owner, host, port, http_options]() {
                crpc::http::client_options copy = http_options;
                return crpc::http::make_client(std::move(copy), *owner,
                                               crpc::endpoint::tcp(host, port));
            }));
    }
};

} // namespace

CRPC_TEST(hbase_it_version_and_table_list) {
    (void)require_gateway();
    it_session it;
    const auto version = it.session->version();
    if (!version) {
        report("version", version.error());
    }
    CRPC_CHECK(static_cast<bool>(version));
    if (version) {
        // The gateway reports its own build; anything non-empty means the REST path works.
        std::printf("  gateway: %s\n", version.value().substr(0, 60).c_str());
        CRPC_CHECK_FALSE(version.value().empty());
    }
    const auto tables = it.session->list_tables();
    if (!tables) {
        report("list tables", tables.error());
    }
    CRPC_CHECK(static_cast<bool>(tables));
}

CRPC_TEST(hbase_it_create_put_get_delete) {
    (void)require_gateway();
    it_session it;
    // A fresh table per run: HBase keeps data between runs, and a leftover row would make the
    // assertions depend on history.
    (void)it.session->delete_table("crpc_it");
    const std::vector<std::string> families{"cf"};
    const crpc::status created =
        it.session->create_table("crpc_it", crpc::span<const std::string>(families.data(), 1));
    if (!created) {
        report("create table", created.error());
    }
    CRPC_CHECK(static_cast<bool>(created));
    const auto exists = it.session->table_exists("crpc_it");
    CRPC_CHECK(static_cast<bool>(exists));
    if (exists) {
        CRPC_CHECK(exists.value());
    }

    crpc::db::hbase::row value;
    value.key = key_of("row1");
    crpc::db::hbase::cell name;
    name.family = "cf";
    name.qualifier = "name";
    name.value = value_of("ada");
    value.cells.push_back(name);
    crpc::db::hbase::cell age;
    age.family = "cf";
    age.qualifier = "age";
    age.value = value_of("36");
    value.cells.push_back(age);

    const crpc::status wrote = it.session->put_row("crpc_it", value);
    if (!wrote) {
        report("put row", wrote.error());
    }
    CRPC_CHECK(static_cast<bool>(wrote));

    // The read path is the one that took several probes: the row key goes in the URL raw, so
    // a client that base64-encodes it gets a 404 for a row that exists.
    const auto read = it.session->get_row("crpc_it", view_of(key_of("row1")));
    if (!read) {
        report("get row", read.error());
    }
    CRPC_CHECK(static_cast<bool>(read));
    if (read) {
        CRPC_CHECK_EQ(std::size_t{2}, read.value().cells.size());
        CRPC_CHECK_EQ(std::string("row1"), std::string(
            reinterpret_cast<const char*>(read.value().key.data()), read.value().key.size()));
        bool saw_name = false;
        bool saw_age = false;
        for (const crpc::db::hbase::cell& item : read.value().cells) {
            CRPC_CHECK_EQ(std::string("cf"), item.family);
            const std::string text(reinterpret_cast<const char*>(item.value.data()),
                                   item.value.size());
            if (item.qualifier == "name") {
                CRPC_CHECK_EQ(std::string("ada"), text);
                saw_name = true;
            } else if (item.qualifier == "age") {
                CRPC_CHECK_EQ(std::string("36"), text);
                saw_age = true;
            }
            // The server stamps every cell, and a zero timestamp would mean the reply was
            // parsed without it.
            CRPC_CHECK(item.timestamp > 0);
        }
        CRPC_CHECK(saw_name);
        CRPC_CHECK(saw_age);
    }

    // A missing row is `not_found`, which is how a caller distinguishes it from an empty row.
    const auto missing = it.session->get_row("crpc_it", view_of(key_of("no-such-row")));
    CRPC_CHECK_FALSE(static_cast<bool>(missing));
    if (!missing) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                      static_cast<int>(missing.error().code));
    }

    const crpc::status removed = it.session->delete_row("crpc_it", view_of(key_of("row1")));
    if (!removed) {
        report("delete row", removed.error());
    }
    CRPC_CHECK(static_cast<bool>(removed));
    const auto gone = it.session->get_row("crpc_it", view_of(key_of("row1")));
    CRPC_CHECK_FALSE(static_cast<bool>(gone));
}

CRPC_TEST(hbase_it_scan_reads_a_range) {
    (void)require_gateway();
    it_session it;
    (void)it.session->delete_table("crpc_scan");
    const std::vector<std::string> families{"cf"};
    const crpc::status created =
        it.session->create_table("crpc_scan", crpc::span<const std::string>(families.data(), 1));
    CRPC_CHECK(static_cast<bool>(created));

    for (int i = 0; i < 5; ++i) {
        crpc::db::hbase::row value;
        // Zero-padded so the lexicographic order is the numeric order, which is what HBase
        // sorts by.
        char key[16];
        std::snprintf(key, sizeof(key), "k%02d", i);
        value.key = key_of(key);
        crpc::db::hbase::cell cell;
        cell.family = "cf";
        cell.qualifier = "n";
        cell.value = value_of(std::to_string(i));
        value.cells.push_back(cell);
        const crpc::status wrote = it.session->put_row("crpc_scan", value);
        CRPC_CHECK(static_cast<bool>(wrote));
    }

    const auto rows = it.session->scan("crpc_scan", {}, 0);
    if (!rows) {
        report("scan", rows.error());
    }
    CRPC_CHECK(static_cast<bool>(rows));
    if (rows) {
        CRPC_CHECK_EQ(std::size_t{5}, rows.value().size());
        if (rows.value().size() == 5) {
            // The scanner returns rows in key order, and the first one is k00.
            CRPC_CHECK_EQ(std::string("k00"),
                          std::string(reinterpret_cast<const char*>(rows.value()[0].key.data()),
                                      rows.value()[0].key.size()));
            CRPC_CHECK_EQ(std::string("k04"),
                          std::string(reinterpret_cast<const char*>(rows.value()[4].key.data()),
                                      rows.value()[4].key.size()));
        }
    }

    // A start row is exclusive of nothing before it: k02 and k03 come back.
    const auto tail = it.session->scan("crpc_scan", view_of(key_of("k02")), 0);
    CRPC_CHECK(static_cast<bool>(tail));
    if (tail) {
        CRPC_CHECK_EQ(std::size_t{3}, tail.value().size());
    }

    // A limit stops the scan early, and the scanner must still be released; the call does that
    // itself, so the next call must work.
    const auto limited = it.session->scan("crpc_scan", {}, 2);
    CRPC_CHECK(static_cast<bool>(limited));
    if (limited) {
        CRPC_CHECK_EQ(std::size_t{2}, limited.value().size());
    }
    const auto again = it.session->scan("crpc_scan", {}, 1);
    CRPC_CHECK(static_cast<bool>(again));
}

CRPC_TEST(hbase_it_missing_table_is_not_found) {
    (void)require_gateway();
    it_session it;
    const auto read = it.session->get_row("no_such_table_at_all", view_of(key_of("x")));
    CRPC_CHECK_FALSE(static_cast<bool>(read));
    if (!read) {
        report("missing table", read.error());
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                      static_cast<int>(read.error().code));
    }
}

CRPC_TEST_MAIN()