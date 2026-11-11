// CRPC -- G19 step4 PR-10: the registryd composition test.
//
// `registryd` wires three things that were built and tested separately:
//
//   * a `registry_service` on the binary CRPC channel;
//   * a `consul_http` facade over that same service on an HTTP listener;
//   * a `kv_store`, so the KV endpoints have a data plane.
//
// Testing the pieces does not test the composition: a route table can be correct
// and still never be registered, and a service can be correct and still be given
// a null KV store. This drives a REAL HTTP listener over a real socket, because
// the failure this guards against (the facade not wired to the server) is
// invisible to an in-process call.
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "test_framework.hpp"

#include <crpc/cluster/consul_http.hpp>
#include <crpc/cluster/kv_store.hpp>
#include <crpc/cluster/raft_node.hpp>
#include <crpc/cluster/raft_state.hpp>
#include <crpc/cluster/registry_log.hpp>
#include <crpc/cluster/registry_service.hpp>
#include <crpc/fs/directory.hpp>
#include <crpc/fs/temp.hpp>
#include <crpc/http/client.hpp>
#include <crpc/http/server.hpp>
#include <crpc/io/iocp_backend.hpp>
#include <crpc/mvc/router.hpp>

namespace cl = crpc::cluster;
namespace fs = crpc::fs;
using namespace std::chrono_literals;

namespace {

crpc::u64 fnv1a(const std::string& value) {
    crpc::u64 hash = 1469598103934665603ull;
    for (const char c : value) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

// A port in the 40000-48999 band: below the OS ephemeral range (49152+ on
// Windows), so an outgoing connection cannot claim it between the bind attempt
// and the listen, and ABOVE the band `test_http_keepalive` / `test_http_client_*`
// use (12000+pid%15000), so a parallel CTest process cannot collide with us.
// The formula is deliberately not copied from those files: sharing one allocator
// across processes is what made the first parallel run fail.
crpc::u16 next_port() {
    static std::atomic<unsigned> counter{0};
    const unsigned sequence = counter.fetch_add(1, std::memory_order_relaxed);
    return static_cast<crpc::u16>(40000u + (sequence % 8000u));
}

crpc::span<const std::byte> as_bytes(std::string_view text) noexcept {
    return crpc::span<const std::byte>(
        reinterpret_cast<const std::byte*>(text.data()), text.size());
}

// The composition `registryd` builds: one service, one facade, one HTTP server.
struct daemon {
    std::unique_ptr<crpc::iocp_backend> backend;
    std::unique_ptr<cl::kv_store> store;
    std::unique_ptr<cl::registry_service> service;
    std::unique_ptr<cl::consul_http> facade;
    std::unique_ptr<crpc::mvc::router> routes;
    std::unique_ptr<cl::raft_node> raft;
    std::unique_ptr<cl::file_registry_log> raft_log;
    std::unique_ptr<crpc::http::http_server> http;
    fs::path dir;
    crpc::u16 port = 0;

    // `preserve` keeps an existing state directory: that is what a restart is.
    // The default wipes it, so each case starts from a known-empty tree.
    bool start(const std::string& name, bool preserve = false) {
        // A plain directory, not `temp_dir`: that type is move-only and its
        // `result` hands out a const reference, so it cannot be moved into a
        // member. Cleanup is explicit in the destructor instead.
        dir = fs::temporary_directory().value() / name;
        if (!preserve) {
            (void)fs::remove_all(dir);
        }
        if (!fs::create_directories(dir).ok()) {
            return false;
        }

        backend = std::make_unique<crpc::iocp_backend>();
        store = std::make_unique<cl::kv_store>();

        // The fixture mirrors `registryd`: a single-node raft node over a file
        // log, not a null raft. A null raft applies KV writes to memory and
        // never logs them, so a restart silently loses the data -- the exact
        // defect an earlier version of `registryd` shipped with. Wiring raft here
        // is what makes `daemon_kv_survives_a_restart` a real test of the
        // composition rather than of one in-memory map.
        const crpc::status dir_made = fs::create_directories(dir / "state");
        if (!dir_made.ok()) {
            return false;
        }
        auto opened = cl::file_registry_log::open((dir / "state" / "raft.log").str());
        if (!opened.ok()) {
            std::printf("raft log: %s\n", opened.error().message.c_str());
            return false;
        }
        raft_log = std::move(opened).value();
        cl::raft_config raft_config;
        raft_config.node_id = "node-A";
        raft_config.election_timeout_min = std::chrono::milliseconds(100);
        raft_config.election_timeout_max = std::chrono::milliseconds(200);
        raft_config.heartbeat_interval = std::chrono::milliseconds(50);
        raft = std::make_unique<cl::raft_node>(raft_config, *raft_log,
                                               dir / "state",
                                               cl::system_monotonic_clock());
        if (!raft->start().ok()) {
            return false;
        }
        // A single node elects itself once its election deadline passes, so
        // drive the timer until it is leader. Without this the first write is
        // refused with "only the leader can propose".
        for (int i = 0; i < 200 && raft->role() != cl::raft_role::leader; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            raft->tick();
        }
        if (raft->role() != cl::raft_role::leader) {
            std::printf("raft: not leader\n");
            return false;
        }

        cl::registry_service_config config;
        config.state_dir = dir / "state";
        // No binary listener: this case is about the HTTP channel. `prepare`
        // still registers every handler, which `dispatch` uses.
        config.listen = crpc::endpoint{};
        config.backend = backend.get();
        config.kv = store.get();
        config.raft = raft.get();
        config.registry.admin_token = "admin-secret";

        // `node_id` is left empty: an empty one means "any node". A non-empty
        // one binds writes to that node, and a Consul registration carries its
        // own node identity in `ID`.
        cl::acl_rule writer;
        writer.token_hash = fnv1a("token-A");
        writer.principal_id = "node-A";
        writer.read_prefixes.push_back("");
        writer.write_prefixes.push_back("");
        config.acl.push_back(writer);

        service = std::make_unique<cl::registry_service>(std::move(config));
        crpc::server::builder binary_builder;
        const crpc::status prepared = service->prepare(binary_builder);
        if (!prepared.ok()) {
            std::printf("prepare: %s\n", prepared.message().c_str());
            return false;
        }

        facade = std::make_unique<cl::consul_http>(*service);
        routes = std::make_unique<crpc::mvc::router>();
        const crpc::status added = facade->add_routes(*routes);
        if (!added.ok()) {
            std::printf("add_routes: %s\n", added.message().c_str());
            return false;
        }
        // The server refuses to start on an unfrozen router: freeze compiles the
        // patterns and is the last legal moment to add routes.
        const crpc::status frozen = routes->freeze();
        if (!frozen.ok()) {
            std::printf("freeze: %s\n", frozen.message().c_str());
            return false;
        }

        port = next_port();
        crpc::http::http_server::builder http_builder;
        http_builder.add_listener(crpc::endpoint::tcp("127.0.0.1", port),
                                  *backend);
        http_builder.router(*routes);
        http_builder.shards(1);
        http = http_builder.build();
        if (!http) {
            std::printf("http build: null\n");
            return false;
        }
        // A bind can lose a race with another CTest process that picked the same
        // port. Rebuilding with the next candidate is the honest retry; a
        // silently skipped case would report a failure as a pass.
        crpc::status started = http->start();
        for (int attempt = 0; !started.ok() && attempt < 8; ++attempt) {
            http.reset();
            port = next_port();
            crpc::http::http_server::builder retry_builder;
            retry_builder.add_listener(crpc::endpoint::tcp("127.0.0.1", port),
                                       *backend);
            retry_builder.router(*routes);
            retry_builder.shards(1);
            http = retry_builder.build();
            if (!http) {
                return false;
            }
            started = http->start();
        }
        if (!started.ok()) {
            std::printf("http start: %s\n", started.message().c_str());
            return false;
        }
        return true;
    }

    ~daemon() {
        if (http) {
            http->stop();
        }
        http.reset();
        facade.reset();
        routes.reset();
        service.reset();
        store.reset();
        backend.reset();
        if (!dir.empty()) {
            (void)fs::remove_all(dir);
        }
    }

    struct reply {
        int status = 0;
        std::string body;
    };

    // One request over a fresh connection. `http_server` exposes no bound
    // endpoint for port 0, which is why `port` is chosen up front.
    reply call(crpc::http::method verb, const std::string& target,
               const std::string& token, const std::string& body) {
        reply out;
        crpc::http::client_options options;
        options.host = "127.0.0.1";
        options.port = port;
        options.timeout_seconds = 5;
        std::unique_ptr<crpc::http::client> client = crpc::http::make_client(
            options, *backend, crpc::endpoint::tcp("127.0.0.1", port));
        if (!client) {
            std::printf("client: null\n");
            return out;
        }
        std::vector<crpc::http::header> headers;
        headers.push_back({"X-Consul-Token", token});
        auto response = client->send(
            verb, target, as_bytes(body),
            crpc::span<const crpc::http::header>(headers.data(),
                                                 headers.size()));
        if (!response.ok()) {
            std::printf("send: %s\n", response.error().message.c_str());
            return out;
        }
        out.status = response.value().status_code;
        out.body.assign(
            reinterpret_cast<const char*>(response.value().body.data()),
            response.value().body.size());
        return out;
    }
};

} // namespace

// --- The KV endpoint answers on a real socket --------------------------------

CRPC_TEST(daemon_kv_put_then_get_over_http) {
    daemon d;
    CRPC_REQUIRE(d.start("crpc_g19p4_daemon_kv"));

    const daemon::reply put =
        d.call(crpc::http::method::put, "/v1/kv/composed", "token-A", "hello");
    CRPC_CHECK_EQ(200, put.status);

    const daemon::reply got =
        d.call(crpc::http::method::get, "/v1/kv/composed", "token-A", "");
    CRPC_CHECK_EQ(200, got.status);
    CRPC_CHECK(got.body.find("\"Value\":") != std::string::npos);

    // The value the facade returned must be the one the store holds, not a copy
    // the facade kept: the two channels must share one data plane.
    const auto entry = d.store->get("composed");
    CRPC_REQUIRE(entry.ok());
    CRPC_CHECK_EQ(std::string("hello"),
                  std::string(reinterpret_cast<const char*>(
                                  entry.value().value.data()),
                              entry.value().value.size()));
}

// --- The catalog endpoint answers on a real socket ---------------------------

CRPC_TEST(daemon_register_then_catalog_over_http) {
    daemon d;
    CRPC_REQUIRE(d.start("crpc_g19p4_daemon_catalog"));

    // `ID` must match the ACL principal: the registry injects the principal as
    // the write context's node id and refuses a write whose node does not match
    // the instance's. A mismatch is a 409 carrying
    // "write authority does not belong to this node".
    const std::string registration =
        "{\"Name\":\"api\",\"ID\":\"node-A\",\"Address\":\"10.0.0.1\","
        "\"Port\":8080}";
    const daemon::reply put =
        d.call(crpc::http::method::put, "/v1/agent/service/register",
               "token-A", registration);
    if (put.status != 200) {
        std::printf("register body: %s\n", put.body.c_str());
    }
    CRPC_CHECK_EQ(200, put.status);

    const daemon::reply catalog =
        d.call(crpc::http::method::get, "/v1/catalog/services", "token-A", "");
    CRPC_CHECK_EQ(200, catalog.status);
    CRPC_CHECK(catalog.body.find("api") != std::string::npos);
}

// --- The refusal paths survive the composition -------------------------------

CRPC_TEST(daemon_routes_are_reachable_and_refuse_unknown_paths) {
    daemon d;
    CRPC_REQUIRE(d.start("crpc_g19p4_daemon_routes"));

    // A known route with no token is 401, not 404: the route exists and the
    // facade decides the status before dispatch.
    const daemon::reply anonymous =
        d.call(crpc::http::method::get, "/v1/catalog/services", "", "");
    CRPC_CHECK_EQ(401, anonymous.status);

    // A path the facade does not register must not be answered by a default
    // handler that pretends success.
    const daemon::reply unknown =
        d.call(crpc::http::method::get, "/v1/nothing-here", "token-A", "");
    CRPC_CHECK(unknown.status == 404 || unknown.status == 405);
}

// --- A KV write survives a restart -------------------------------------------

CRPC_TEST(daemon_kv_survives_a_restart) {
    // The composition's durability claim: a value written through the HTTP
    // facade is still there after the process is torn down and rebuilt over the
    // same state directory. This is the case that fails when the service is
    // given a null raft (the write lands in memory and is never logged) or when
    // `prepare` does not replay the committed log.
    const std::string name = "crpc_g19p4_daemon_restart";
    {
        daemon first;
        CRPC_REQUIRE(first.start(name));
        const daemon::reply put =
            first.call(crpc::http::method::put, "/v1/kv/durable", "token-A",
                       "survives");
        CRPC_CHECK_EQ(200, put.status);
        const auto entry = first.store->get("durable");
        CRPC_REQUIRE(entry.ok());
    }

    // The first daemon is destroyed here: its HTTP server, service, raft node
    // and log are all gone. Only the files under the state directory remain.
    daemon second;
    CRPC_REQUIRE(second.start(name, /*preserve=*/true));
    const daemon::reply got =
        second.call(crpc::http::method::get, "/v1/kv/durable", "token-A", "");
    CRPC_CHECK_EQ(200, got.status);
    CRPC_CHECK(got.body.find("\"Value\":") != std::string::npos);

    const auto entry = second.store->get("durable");
    CRPC_REQUIRE(entry.ok());
    CRPC_CHECK_EQ(std::string("survives"),
                  std::string(reinterpret_cast<const char*>(
                                  entry.value().value.data()),
                              entry.value().value.size()));
}

CRPC_TEST_MAIN()
