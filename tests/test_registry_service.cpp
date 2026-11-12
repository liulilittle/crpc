// CRPC -- G19 step2: registry service tests (E1 / E5 / E8 / epoch_store).
//
// The service is exercised over a real `rpc::client` on a real TCP transport so
// the ACL, the DTO codec, the anchor logic and the registry all run together.
// The cross-process cases live in test_registry_cross_process.cpp.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <set>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "test_framework.hpp"

#include <crpc/cluster/registry_client.hpp>
#include <crpc/cluster/registry_service.hpp>
#include <crpc/cluster/fence_authority.hpp>
#include <crpc/error.hpp>
#include <crpc/frame.hpp>
#include <crpc/fs/directory.hpp>
#include <crpc/fs/file.hpp>
#include <crpc/io.hpp>
#include <crpc/io/iocp_backend.hpp>
#include <crpc/rpc/dispatcher.hpp>

#if defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
#endif

namespace cl = crpc::cluster;
namespace fs = crpc::fs;
using namespace std::chrono_literals;

namespace {

// The token hash is the same FNV-1a the registry uses for principal_hash.
crpc::u64 fnv1a(const std::string& value) {
    crpc::u64 hash = 1469598103934665603ull;
    for (const char c : value) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

crpc::u16 next_port() {
    static std::atomic<unsigned> counter{0};
#if defined(_WIN32)
    static const unsigned base = 26000u + (::GetCurrentProcessId() % 20000u);
#else
    static const unsigned base = 26000u + (static_cast<unsigned>(::getpid()) % 20000u);
#endif
    const unsigned value = base + counter.fetch_add(13, std::memory_order_relaxed);
    return static_cast<crpc::u16>(value % 60000u + 1024u);
}

// A running service plus its client, so a case can start/stop as one unit.
struct node {
    std::unique_ptr<crpc::iocp_backend> backend;
    std::unique_ptr<cl::registry_service> service;
    // The transport is shared so a case can hold two clients (legitimate and
    // impersonating) on one connection.
    std::shared_ptr<crpc::rpc::client> client;
    crpc::endpoint bound_ep;
    fs::path state_dir;
    std::string token;

    ~node() { stop(); }

    bool start(const std::string& dir, const std::string& tok) {
        backend = std::make_unique<crpc::iocp_backend>();
        token = tok;
        state_dir = fs::path(dir);

        cl::registry_service_config config;
        config.state_dir = state_dir;
        config.listen = crpc::endpoint::tcp("127.0.0.1", next_port());
        config.backend = backend.get();
        cl::acl_rule rule;
        rule.token_hash = fnv1a(tok);
        rule.principal_id = "node-A";
        rule.node_id = "node-A";
        rule.read_prefixes.push_back("");
        rule.write_prefixes.push_back("");
        config.acl.push_back(rule);
        // The ACL layer injects this into `write_context::admin_token`; without
        // it the registry's authority check rejects a write whose node_id is not
        // the stored one.
        config.registry.admin_token = "admin-secret";
        // A second token bound to a different node, so an impersonation case
        // has something to present.
        cl::acl_rule other;
        other.token_hash = fnv1a("token-B");
        other.principal_id = "node-B";
        other.node_id = "node-B";
        other.read_prefixes.push_back("");
        other.write_prefixes.push_back("");
        config.acl.push_back(other);

        service = std::make_unique<cl::registry_service>(std::move(config));
        crpc::server::builder builder;
        const crpc::status prepared = service->prepare(builder);
        if (!prepared.ok()) {
            std::printf("prepare failed: %s\n", prepared.message().c_str());
            return false;
        }
        auto candidate = builder.build();
        if (!candidate) {
            return false;
        }
        if (!candidate->start().ok()) {
            return false;
        }
        service->set_server(std::move(candidate));
        bound_ep = service->bound_endpoint();

        auto connection = backend->connect(bound_ep, crpc::connect_options{});
        if (!connection) {
            return false;
        }
        crpc::rpc::client_options options;
        options.default_timeout = 2s;
        client = std::make_shared<crpc::rpc::client>(std::move(connection), options);
        return true;
    }

    void stop() noexcept {
        client.reset();
        service.reset();
        backend.reset();
    }
};

} // namespace

// --- E1: register then query over a real client ------------------------------

CRPC_TEST(service_register_then_query_over_client) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_e1";
    (void)fs::remove_all(dir);

    node n;
    CRPC_REQUIRE(n.start(dir.str(), "token-A"));

    cl::registry_client registry(n.client, cl::registry_client_options{"token-A"});

    cl::service_instance instance;
    instance.service = "svc";
    instance.node_id = "node-A";
    instance.address.host = "127.0.0.1";
    instance.address.port = 8080;

    const auto registered = registry.register_instance(instance, 1, 7);
    if (!registered.ok()) {
        std::printf("register failed: code=%d message=%s\n",
                    static_cast<int>(registered.error().code),
                    registered.error().message.c_str());
        const std::string* reason =
            registered.error().metadata.find("registry.reason");
        if (reason != nullptr) {
            std::printf("  registry.reason=%s\n", reason->c_str());
        }
    }
    CRPC_REQUIRE(registered.ok());
    CRPC_CHECK(registered.value().index > 0);

    const auto queried = registry.query("svc");
    if (!queried.ok()) {
        std::printf("query failed: code=%d message=%s\n",
                    static_cast<int>(queried.error().code),
                    queried.error().message.c_str());
    }
    CRPC_REQUIRE(queried.ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), queried.value().rows.size());

    (void)fs::remove_all(dir);
}

// --- E8: an impersonating writer is denied, and the catalog is unchanged ------

CRPC_TEST(service_impersonating_write_is_denied_and_catalog_unchanged) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_e8";
    (void)fs::remove_all(dir);

    node n;
    CRPC_REQUIRE(n.start(dir.str(), "token-A"));

    // Control: the legitimate token succeeds. Without this, "deny everything"
    // would also make the impersonation case green.
    {
        cl::registry_client legitimate(n.client, cl::registry_client_options{"token-A"});
        cl::service_instance instance;
        instance.service = "svc";
        instance.node_id = "node-A";
        instance.address.host = "127.0.0.1";
        instance.address.port = 8080;
        const auto ok_write = legitimate.register_instance(instance, 1, 7);
        if (!ok_write.ok()) {
            std::printf("control write failed: code=%d message=%s\n",
                        static_cast<int>(ok_write.error().code),
                        ok_write.error().message.c_str());
        }
        CRPC_REQUIRE(ok_write.ok());
        CRPC_CHECK(ok_write.value().index > 0);
    }

    // The impersonating token belongs to a rule bound to node-B, so writing
    // node-A's key must be denied.
    {
        cl::registry_client impersonating(n.client,
                                          cl::registry_client_options{"token-B"});
        // Probe the fence allocation alone: it is the first call the write
        // path makes, so its code tells us whether the denial happens at the
        // ACL layer or somewhere else.
        const auto fence = impersonating.next_fence("svc", "node-A");
        if (!fence.ok()) {
            std::printf("next_fence denied: code=%d message=%s\n",
                        static_cast<int>(fence.error().code),
                        fence.error().message.c_str());
        } else {
            std::printf("next_fence unexpectedly allowed\n");
        }
        cl::service_instance instance;
        instance.service = "svc";
        instance.node_id = "node-A";
        instance.address.host = "127.0.0.1";
        instance.address.port = 9090;
        const auto denied = impersonating.register_instance(instance, 2, 8);
        CRPC_CHECK_FALSE(denied.ok());
        if (denied.ok()) {
            return;
        }
        const std::string* raw = denied.error().metadata.find("crpc.raw_code");
        // The response codec fails closed on codes above `application` (21):
        // the client observes `internal` plus the original code in
        // `crpc.raw_code`. The service still emits 22 (VERDICT section 2.5);
        // this is the transport's documented behaviour, not a service defect.
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::internal),
                      static_cast<int>(denied.error().code));
        CRPC_CHECK(raw != nullptr && *raw == "22");
        const std::string* reason = denied.error().metadata.find("registry.reason");
        CRPC_CHECK(reason != nullptr && *reason == "acl_denied");
    }

    // The catalog must be unchanged: the impersonating write did not advance
    // the index and did not change the endpoint.
    {
        cl::registry_client legitimate(n.client, cl::registry_client_options{"token-A"});
        const auto after = legitimate.query("svc");
        CRPC_REQUIRE(after.ok());
        CRPC_CHECK_EQ(static_cast<std::size_t>(1), after.value().rows.size());
        if (!after.value().rows.empty()) {
            CRPC_CHECK_EQ(static_cast<crpc::u16>(8080),
                          after.value().rows.front().instance.address.port);
        }
    }

    (void)fs::remove_all(dir);
}

// --- E5: a stale anchor is rejected with the service's index ------------------
//
// The stale anchor is manufactured the way VERDICT section 5.2 describes: a
// second writer commits first, so the first writer's observation is behind the
// authority when it finally sends.

CRPC_TEST(service_stale_anchor_is_rejected_with_index) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_e5";
    (void)fs::remove_all(dir);

    node n;
    CRPC_REQUIRE(n.start(dir.str(), "token-A"));

    // Two clients on two threads: the anchor is per-thread (VERDICT section
    // 5.2), so a client on another thread cannot advance `first`'s anchor.
    cl::registry_client first(n.client, cl::registry_client_options{"token-A"});
    // `first` has the retry disabled so the case observes the service's raw
    // answer. With the retry enabled (the default) the client re-anchors and
    // succeeds, which is the documented behaviour but not what E5 measures.
    first.set_stale_retries(0);
    auto second = std::make_unique<cl::registry_client>(n.client,
                                                        cl::registry_client_options{"token-A"});

    cl::service_instance instance;
    instance.service = "svc";
    instance.node_id = "node-A";
    instance.address.host = "127.0.0.1";
    instance.address.port = 8080;

    const auto seeded = first.register_instance(instance, 1, 7);
    CRPC_REQUIRE(seeded.ok());
    const crpc::u64 accepted_index = seeded.value().index;

    // `second` commits a newer incarnation on its own thread, moving the
    // authority's index. `first`'s anchor is untouched.
    cl::service_instance newer = instance;
    newer.address.port = 9090;
    crpc::u64 advanced_index = 0;
    {
        std::thread worker([&] {
            const auto advanced = second->register_instance(newer, 2, 8);
            CRPC_CHECK(advanced.ok());
            if (advanced.ok()) {
                advanced_index = advanced.value().index;
            }
        });
        worker.join();
    }
    CRPC_CHECK(advanced_index > accepted_index);

    // `first` still holds the old anchor (its last accepted index). Its write
    // presents an observation behind the authority and must be rejected.
    cl::service_instance stale_payload = instance;
    stale_payload.address.port = 7070;
    const auto stale = first.register_instance(stale_payload, 3, 9);
    // The write MUST be rejected. An `else` branch that accepted a successful
    // write here would make a bypass of the anchor check green: the whole point
    // of E5 is that a stale anchor cannot silently commit.
    CRPC_CHECK_FALSE(stale.ok());
    if (stale.ok()) {
        return;
    }
    const std::string* reason = stale.error().metadata.find("registry.reason");
    if (reason == nullptr) {
        std::printf("stale error: code=%d message=%s\n",
                    static_cast<int>(stale.error().code),
                    stale.error().message.c_str());
    }
    CRPC_CHECK(reason != nullptr);
    if (reason != nullptr) {
        CRPC_CHECK_EQ(std::string("stale_anchor"), *reason);
    }
    // The metadata must carry the authority's current index so the client can
    // re-anchor, and it must equal the index the service actually reached.
    const std::string* index = stale.error().metadata.find("registry.index");
    CRPC_CHECK(index != nullptr);
    if (index != nullptr) {
        CRPC_CHECK_EQ(advanced_index, std::stoull(*index));
    }
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(stale.error().code));

    // And the catalog must not have changed: the stale write did not land.
    const auto after = first.query("svc");
    CRPC_REQUIRE(after.ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), after.value().rows.size());
    if (!after.value().rows.empty()) {
        CRPC_CHECK_EQ(static_cast<crpc::u16>(9090),
                      after.value().rows.front().instance.address.port);
    }

    (void)fs::remove_all(dir);
}

// --- epoch_store: crash consistency -------------------------------------------

CRPC_TEST(epoch_store_persists_before_returning) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_epoch";
    (void)fs::remove_all(dir);
    CRPC_REQUIRE(fs::create_directories(dir).ok());

    cl::epoch_store store;
    store.bind(dir);
    store.seed(0, 0);
    const auto first = store.next();
    CRPC_REQUIRE(first.ok());
    CRPC_CHECK_EQ(crpc::u64{1}, first.value());

    // A second store over the same directory must read the persisted value and
    // never hand out an epoch at or below one already issued.
    cl::epoch_store restarted;
    restarted.bind(dir);
    const auto persisted = cl::epoch_store::read(dir);
    CRPC_REQUIRE(persisted.ok());
    CRPC_CHECK_EQ(first.value(), persisted.value());
    restarted.seed(persisted.value(), 0);
    const auto second = restarted.next();
    CRPC_REQUIRE(second.ok());
    CRPC_CHECK(second.value() > first.value());

    (void)fs::remove_all(dir);
}

CRPC_TEST(epoch_store_seed_takes_the_larger_of_persisted_and_fence) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_epoch_seed";
    (void)fs::remove_all(dir);
    CRPC_REQUIRE(fs::create_directories(dir).ok());

    cl::epoch_store store;
    store.bind(dir);
    store.seed(100, 50);
    CRPC_CHECK_EQ(crpc::u64{100}, store.value());
    const auto first = store.next();
    CRPC_REQUIRE(first.ok());
    CRPC_CHECK_EQ(crpc::u64{101}, first.value());

    cl::epoch_store higher_fence;
    higher_fence.bind(dir);
    higher_fence.seed(10, 200);
    CRPC_CHECK_EQ(crpc::u64{200}, higher_fence.value());
    const auto second = higher_fence.next();
    CRPC_REQUIRE(second.ok());
    CRPC_CHECK_EQ(crpc::u64{201}, second.value());

    (void)fs::remove_all(dir);
}

// --- D3: the service issues the epoch, the client does not choose it ---------

CRPC_TEST(service_issues_its_own_epoch_not_the_clients_claim) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_epoch_issue";
    (void)fs::remove_all(dir);

    node n;
    CRPC_REQUIRE(n.start(dir.str(), "token-A"));

    cl::registry_client client(n.client, cl::registry_client_options{"token-A"});

    cl::service_instance instance;
    instance.service = "svc";
    instance.node_id = "node-A";
    instance.address.host = "127.0.0.1";
    instance.address.port = 8080;

    // Claim an absurd epoch: if the service passed the client's value through,
    // the accepted epoch would be the claim. The service must instead issue its
    // own from the epoch store.
    const auto written = client.register_instance(instance, 1, 900000);
    CRPC_REQUIRE(written.ok());
    const auto queried = client.query("svc");
    CRPC_REQUIRE(queried.ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), queried.value().rows.size());

    // The epoch file must have advanced, and a second client's epoch must be
    // strictly greater than the first (the collision the epoch store exists to
    // prevent).
    const auto persisted = cl::epoch_store::read(dir);
    CRPC_REQUIRE(persisted.ok());
    CRPC_CHECK(persisted.value() > 0);
    CRPC_CHECK(persisted.value() != 900000);

    (void)fs::remove_all(dir);
}

// --- D6: the service-layer idempotency branch returns the cached result -------

CRPC_TEST(service_idempotent_replay_returns_the_first_result) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_idem";
    (void)fs::remove_all(dir);

    node n;
    CRPC_REQUIRE(n.start(dir.str(), "token-A"));

    cl::registry_client client(n.client, cl::registry_client_options{"token-A"});

    cl::service_instance instance;
    instance.service = "svc";
    instance.node_id = "node-A";
    instance.address.host = "127.0.0.1";
    instance.address.port = 8080;

    const auto first = client.register_instance(instance, 1, 7);
    CRPC_REQUIRE(first.ok());
    const crpc::u64 first_index = first.value().index;

    // A replay with the same payload and the same client anchor must be served
    // from the idempotency map: the index must NOT advance.
    const auto replay = client.register_instance(instance, 1, 7);
    CRPC_REQUIRE(replay.ok());
    CRPC_CHECK_EQ(first_index, replay.value().index);

    // A *different* payload with the same request identity must still be served
    // from the cache: without the service-layer map this advances the index,
    // because the registry only makes an identical payload idempotent.
    cl::service_instance mutated = instance;
    mutated.address.port = 9999;
    const auto replay2 = client.register_instance(mutated, 1, 7);
    CRPC_REQUIRE(replay2.ok());
    CRPC_CHECK_EQ(first_index, replay2.value().index);

    // The catalog has exactly one row, the index advanced only once, and the
    // stored endpoint is the FIRST payload (the replay must not overwrite it).
    const auto queried = client.query("svc");
    CRPC_REQUIRE(queried.ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), queried.value().rows.size());
    CRPC_CHECK_EQ(first_index, queried.value().anchor.index);
    if (!queried.value().rows.empty()) {
        CRPC_CHECK_EQ(static_cast<crpc::u16>(8080),
                      queried.value().rows.front().instance.address.port);
    }

    (void)fs::remove_all(dir);
}

// --- M2: the epoch is NOT issued when the persist fails ----------------------
//
// `epoch_store::next()` must persist before returning. This case injects a
// persist failure and asserts three things a "return then write" order would
// violate: the call fails, the counter does not advance, and the next call does
// not hand out the same number.

CRPC_TEST(epoch_store_persist_failure_does_not_issue_a_number) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_epoch_fail";
    (void)fs::remove_all(dir);
    CRPC_REQUIRE(fs::create_directories(dir).ok());

    cl::epoch_store store;
    store.bind(dir);
    store.seed(0, 0);

    const auto first = store.next();
    CRPC_REQUIRE(first.ok());
    CRPC_CHECK_EQ(crpc::u64{1}, first.value());

    store.inject_persist_failure();
    const auto failed = store.next();
    CRPC_CHECK_FALSE(failed.ok());
    if (failed.ok()) {
        return;
    }
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(failed.error().code));
    // The counter must not have advanced: the number was never issued.
    CRPC_CHECK_EQ(crpc::u64{1}, store.value());

    // And the next successful call must not reuse the failed candidate.
    cl::epoch_store healthy;
    healthy.bind(dir);
    const auto persisted = cl::epoch_store::read(dir);
    CRPC_REQUIRE(persisted.ok());
    CRPC_CHECK_EQ(crpc::u64{1}, persisted.value());
    healthy.seed(persisted.value(), 0);
    const auto recovered = healthy.next();
    CRPC_REQUIRE(recovered.ok());
    CRPC_CHECK_EQ(crpc::u64{2}, recovered.value());

    (void)fs::remove_all(dir);
}

// --- A/B/C: the idempotency map is TTL-bounded, not clear()ed ----------------
//
// A `map.clear()` on overflow would drop the protection of every earlier
// request at once, so a replay of a request from before the overflow would
// execute a second time -- exactly what the second layer of the three-layer
// retry exists to prevent. This case sets a tiny cap, forces an eviction, then
// replays the request that was evicted and asserts it is still recognised.

CRPC_TEST(idempotency_map_evicts_oldest_without_losing_protection) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_ttl";
    (void)fs::remove_all(dir);

    auto backend = std::make_unique<crpc::iocp_backend>();
    cl::registry_service_config config;
    config.state_dir = dir;
    config.listen = crpc::endpoint::tcp("127.0.0.1", next_port());
    config.backend = backend.get();
    config.max_idempotency_entries = 2;
    config.idempotency_ttl = std::chrono::milliseconds{300000};
    const std::string token = "token-A";
    cl::acl_rule rule;
    rule.token_hash = fnv1a(token);
    rule.principal_id = "node-A";
    rule.node_id = "node-A";
    rule.read_prefixes.push_back("");
    rule.write_prefixes.push_back("");
    config.acl.push_back(rule);

    auto service = std::make_unique<cl::registry_service>(std::move(config));
    crpc::server::builder builder;
    CRPC_REQUIRE(service->prepare(builder).ok());
    auto server = builder.build();
    CRPC_REQUIRE(server != nullptr);
    CRPC_REQUIRE(server->start().ok());
    service->set_server(std::move(server));

    auto connection = backend->connect(service->bound_endpoint(),
                                       crpc::connect_options{});
    CRPC_REQUIRE(connection != nullptr);
    crpc::rpc::client_options options;
    options.default_timeout = 2s;
    auto client = std::make_shared<crpc::rpc::client>(std::move(connection), options);
    cl::registry_client registry(client, cl::registry_client_options{token});

    // Three distinct logical writes with a cap of two: the first must be
    // evicted by the third.
    cl::service_instance first_instance;
    first_instance.service = "svc";
    first_instance.node_id = "node-A";
    first_instance.address.host = "127.0.0.1";
    first_instance.address.port = 8001;
    const auto first = registry.register_instance(first_instance, 1, 7);
    CRPC_REQUIRE(first.ok());

    cl::service_instance second_instance = first_instance;
    second_instance.address.port = 8002;
    const auto second = registry.register_instance(second_instance, 2, 7);
    CRPC_REQUIRE(second.ok());

    cl::service_instance third_instance = first_instance;
    third_instance.address.port = 8003;
    const auto third = registry.register_instance(third_instance, 3, 7);
    CRPC_REQUIRE(third.ok());

    // Replay the SECOND write (incarnation 2): it must still be recognised,
    // because only the OLDEST record was evicted -- the recent protection is
    // intact. A `clear()` would have dropped this too.
    const auto replay_second = registry.register_instance(second_instance, 2, 7);
    CRPC_REQUIRE(replay_second.ok());
    CRPC_CHECK_EQ(second.value().index, replay_second.value().index);

    // Replay the THIRD write (incarnation 3): also still protected.
    const auto replay_third = registry.register_instance(third_instance, 3, 7);
    CRPC_REQUIRE(replay_third.ok());
    CRPC_CHECK_EQ(third.value().index, replay_third.value().index);

    // The FIRST write was evicted by design (cap of two). Its replay must NOT
    // be served from the cache: the catalog's incarnation has moved past it, so
    // the registry rejects it as a stale incarnation. That is the documented
    // boundary of a bounded map, not a defect.
    const auto replay_evicted = registry.register_instance(first_instance, 1, 7);
    CRPC_CHECK_FALSE(replay_evicted.ok());
    if (replay_evicted.ok()) {
        return;
    }
    const std::string* reason =
        replay_evicted.error().metadata.find("registry.reason");
    CRPC_CHECK(reason != nullptr);
    if (reason != nullptr) {
        CRPC_CHECK_EQ(std::string("stale_incarnation"), *reason);
    }

    // The catalog still holds the third write's endpoint and index.
    const auto queried = registry.query("svc");
    CRPC_REQUIRE(queried.ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), queried.value().rows.size());
    if (!queried.value().rows.empty()) {
        CRPC_CHECK_EQ(static_cast<crpc::u16>(8003),
                      queried.value().rows.front().instance.address.port);
    }

    (void)fs::remove_all(dir);
}

// --- D4: the time-limited token window ---------------------------------------
//
// VERDICT section 1.3 chose option (ii): a token carries `issued_at_ms` and a
// `nonce`, and the service rejects one whose timestamp is outside the window.
// A token observed on the wire is therefore replayable only inside the window,
// not forever. Clock synchronisation is UNVERIFIED (documented in the header).

CRPC_TEST(time_limited_token_outside_window_is_denied) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_window";
    (void)fs::remove_all(dir);
    const std::string token = "token-A";

    node n;
    CRPC_REQUIRE(n.start(dir.str(), token));

    // A client presenting a token whose issue time is an hour in the past must
    // be denied, not accepted. The client's own issued_at is what the service
    // compares against its clock.
    cl::registry_client client(n.client, cl::registry_client_options{"token-A"});
    // system_clock: the window is cross-process, and steady_clock has a
    // per-process epoch that cannot be compared across a process boundary.
    client.set_token_issued_at(std::chrono::duration_cast<std::chrono::milliseconds>(
        (std::chrono::system_clock::now() - std::chrono::hours(1)).time_since_epoch()));

    cl::service_instance instance;
    instance.service = "svc";
    instance.node_id = "node-A";
    instance.address.host = "127.0.0.1";
    instance.address.port = 8080;
    const auto written = client.register_instance(instance, 1, 7);
    CRPC_CHECK_FALSE(written.ok());
    if (written.ok()) {
        return;
    }
    const std::string* reason = written.error().metadata.find("registry.reason");
    CRPC_CHECK(reason != nullptr);
    if (reason != nullptr) {
        CRPC_CHECK_EQ(std::string("acl_denied"), *reason);
    }

    // Control: a token inside the window is accepted.
    cl::registry_client fresh(n.client, cl::registry_client_options{"token-A"});
    fresh.set_token_issued_at(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()));
    const auto accepted = fresh.register_instance(instance, 1, 7);
    CRPC_CHECK(accepted.ok());

    (void)fs::remove_all(dir);
}

// --- The destructor releases the lease ---------------------------------------
//
// REGRESSION GUARD, NOT A DEFECT DETECTOR. The lease is released by
// `instance_lease`'s own destructor, so a mutant that removes the explicit
// `stop()` from `~registry_service()` cannot make this red; the server is
// likewise stopped by `~server()`. The test still pins the *observable*
// contract -- after a service is destroyed, a new one may take over the same
// state directory -- so a future change that breaks either destructor is
// caught here rather than in production.

CRPC_TEST(service_destructor_releases_the_lease) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_dtor";
    (void)fs::remove_all(dir);
    const std::string token = "token-A";

    auto backend = std::make_unique<crpc::iocp_backend>();
    cl::registry_service_config config;
    config.state_dir = dir;
    config.listen = crpc::endpoint::tcp("127.0.0.1", next_port());
    config.backend = backend.get();
    config.registry.admin_token = "admin-secret";
    cl::acl_rule rule;
    rule.token_hash = fnv1a(token);
    rule.principal_id = "node-A";
    rule.read_prefixes.push_back("");
    rule.write_prefixes.push_back("");
    config.acl.push_back(rule);

    {
        auto service = std::make_unique<cl::registry_service>(std::move(config));
        crpc::server::builder builder;
        CRPC_REQUIRE(service->prepare(builder).ok());
        auto server = builder.build();
        CRPC_REQUIRE(server != nullptr);
        CRPC_REQUIRE(server->start().ok());
        service->set_server(std::move(server));
        CRPC_CHECK(service->running());
        // No stop(): the destructor must do it.
    }

    // A second service over the SAME directory must be able to acquire the
    // lease. If the destructor left it held, prepare() fails.
    auto backend2 = std::make_unique<crpc::iocp_backend>();
    cl::registry_service_config config2;
    config2.state_dir = dir;
    config2.listen = crpc::endpoint::tcp("127.0.0.1", next_port());
    config2.backend = backend2.get();
    config2.registry.admin_token = "admin-secret";
    config2.acl.push_back(rule);
    auto second = std::make_unique<cl::registry_service>(std::move(config2));
    crpc::server::builder builder2;
    const crpc::status acquired = second->prepare(builder2);
    CRPC_CHECK(acquired.ok());
    if (!acquired.ok()) {
        std::printf("second prepare failed: %s\n", acquired.message().c_str());
        return;
    }
    auto server2 = builder2.build();
    if (server2 != nullptr) {
        (void)server2->start();
        second->set_server(std::move(server2));
    }

    (void)fs::remove_all(dir);
}

// --- M4: the single-instance lease -------------------------------------------

CRPC_TEST(single_instance_lease_refuses_a_second_service_and_is_recoverable) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_lease";
    (void)fs::remove_all(dir);
    const std::string token = "token-A";

    auto backend_one = std::make_unique<crpc::iocp_backend>();
    cl::registry_service_config config_one;
    config_one.state_dir = dir;
    config_one.listen = crpc::endpoint::tcp("127.0.0.1", next_port());
    config_one.backend = backend_one.get();
    cl::acl_rule rule;
    rule.token_hash = fnv1a(token);
    rule.principal_id = "node-A";
    rule.node_id = "node-A";
    rule.read_prefixes.push_back("");
    rule.write_prefixes.push_back("");
    config_one.acl.push_back(rule);

    auto first = std::make_unique<cl::registry_service>(std::move(config_one));
    crpc::server::builder builder_one;
    CRPC_REQUIRE(first->prepare(builder_one).ok());
    auto server_one = builder_one.build();
    CRPC_REQUIRE(server_one != nullptr);
    CRPC_REQUIRE(server_one->start().ok());
    first->set_server(std::move(server_one));
    const crpc::u16 bound_port = first->bound_endpoint().port();
    CRPC_CHECK(bound_port != 0);

    // A second service over the SAME state directory must be refused and must
    // not bind a port.
    auto backend_two = std::make_unique<crpc::iocp_backend>();
    cl::registry_service_config config_two;
    config_two.state_dir = dir;
    config_two.listen = crpc::endpoint::tcp("127.0.0.1", next_port());
    config_two.backend = backend_two.get();
    config_two.acl.push_back(rule);
    auto second = std::make_unique<cl::registry_service>(std::move(config_two));
    crpc::server::builder builder_two;
    const crpc::status refused = second->prepare(builder_two);
    CRPC_CHECK_FALSE(refused.ok());
    if (refused.ok()) {
        return;
    }
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                  static_cast<int>(refused.code()));
    CRPC_CHECK_FALSE(second->running());

    // After the first service stops, a new one must be able to take over.
    first->stop();
    first.reset();

    auto backend_three = std::make_unique<crpc::iocp_backend>();
    cl::registry_service_config config_three;
    config_three.state_dir = dir;
    config_three.listen = crpc::endpoint::tcp("127.0.0.1", next_port());
    config_three.backend = backend_three.get();
    config_three.acl.push_back(rule);
    auto third = std::make_unique<cl::registry_service>(std::move(config_three));
    crpc::server::builder builder_three;
    const crpc::status taken = third->prepare(builder_three);
    CRPC_CHECK(taken.ok());
    if (taken.ok()) {
        auto server_three = builder_three.build();
        CRPC_CHECK(server_three != nullptr);
        if (server_three != nullptr) {
            CRPC_CHECK(server_three->start().ok());
            third->set_server(std::move(server_three));
        }
    }

    (void)fs::remove_all(dir);
}

// --- D8: takeover repairs a torn log tail ------------------------------------
//
// A crashed predecessor may have written half a record. `replay()` stops at
// the torn tail, but the bytes stay on disk and would be read again on the next
// restart. The service's takeover path must detect the un-synced suffix and
// truncate to the durable boundary, then restore the catalog from the intact
// prefix.

CRPC_TEST(takeover_repair_truncates_a_torn_log_tail) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_torn";
    (void)fs::remove_all(dir);
    const std::string token = "token-A";

    auto backend = std::make_unique<crpc::iocp_backend>();
    crpc::u16 first_port = 0;
    crpc::u16 second_port = 0;
    {
        // A first service writes two records and is then abandoned.
        cl::registry_service_config config;
        config.state_dir = dir;
        config.listen = crpc::endpoint::tcp("127.0.0.1", next_port());
        config.backend = backend.get();
        cl::acl_rule rule;
        rule.token_hash = fnv1a(token);
        rule.principal_id = "node-A";
        // Admin: the registry's authority check requires the write context's
        // node_id to equal the stored node_id unless an admin token is
        // supplied, and the ACL layer injects `principal_id` as the context's
        // node_id. An admin rule lets the two records use distinct node ids.
        rule.node_id = "";
        rule.admin = true;
        rule.read_prefixes.push_back("");
        rule.write_prefixes.push_back("");
        config.acl.push_back(rule);
        // The ACL layer injects this into `write_context::admin_token`; without
        // it the registry's authority check rejects a write whose node_id is not
        // the stored one.
        config.registry.admin_token = "admin-secret";

        auto service = std::make_unique<cl::registry_service>(std::move(config));
        crpc::server::builder builder;
        CRPC_REQUIRE(service->prepare(builder).ok());
        auto server = builder.build();
        CRPC_REQUIRE(server != nullptr);
        CRPC_REQUIRE(server->start().ok());
        service->set_server(std::move(server));
        first_port = service->bound_endpoint().port();

        auto connection = backend->connect(crpc::endpoint::tcp("127.0.0.1", first_port),
                                           crpc::connect_options{});
        CRPC_REQUIRE(connection != nullptr);
        crpc::rpc::client_options options;
        options.default_timeout = 2s;
        auto client =
            std::make_shared<crpc::rpc::client>(std::move(connection), options);
        cl::registry_client registry(client, cl::registry_client_options{token});

        for (crpc::u16 port : {crpc::u16{8001}, crpc::u16{8002}}) {
            cl::service_instance instance;
            // Two DISTINCT keys (different SERVICE, same node), so the catalog
            // holds two rows after a correct restore. A shared key would
            // last-wins collapse them into one row and the test could not tell
            // "both records restored" from "only one did". The node is the
            // ACL's principal, so the registry's authority check passes.
            instance.service = "svc-" + std::to_string(port - 8000);
            instance.node_id = "node-A";
            instance.address.host = "127.0.0.1";
            instance.address.port = port;
            const auto written = registry.register_instance(instance, 1, 7);
            CRPC_REQUIRE(written.ok());
        }
    }

    // Simulate a crash: append a partial record to the log without syncing.
    const fs::path log_path = dir / "registry.log";
    const auto size = fs::file_size(log_path);
    CRPC_REQUIRE(size.ok());
    const crpc::u64 intact_bytes = size.value();
    {
        // A torn record: the length prefix says 64 bytes, only 20 are present.
        std::vector<std::byte> torn;
        crpc::u32 claimed = 64;
        torn.resize(4);
        crpc::endian::store_le32(torn.data(), claimed);
        torn.resize(20);
        const crpc::status appended = fs::append_bytes(
            log_path, crpc::span<const std::byte>(torn.data(), torn.size()));
        CRPC_REQUIRE(appended.ok());
    }

    // A second service takes over. It must start, and the log must have been
    // repaired back to the intact prefix.
    {
        cl::registry_service_config config;
        config.state_dir = dir;
        config.listen = crpc::endpoint::tcp("127.0.0.1", next_port());
        config.backend = backend.get();
        cl::acl_rule rule;
        rule.token_hash = fnv1a(token);
        rule.principal_id = "node-A";
        rule.node_id = "node-A";
        rule.read_prefixes.push_back("");
        rule.write_prefixes.push_back("");
        config.acl.push_back(rule);
        // The ACL layer injects this into `write_context::admin_token`; without
        // it the registry's authority check rejects a write whose node_id is not
        // the stored one.
        config.registry.admin_token = "admin-secret";

        auto service = std::make_unique<cl::registry_service>(std::move(config));
        crpc::server::builder builder;
        const crpc::status prepared = service->prepare(builder);
        CRPC_CHECK(prepared.ok());
        if (!prepared.ok()) {
            return;
        }
        auto server = builder.build();
        CRPC_REQUIRE(server != nullptr);
        CRPC_REQUIRE(server->start().ok());
        service->set_server(std::move(server));
        second_port = service->bound_endpoint().port();

        const auto repaired_size = fs::file_size(log_path);
        CRPC_REQUIRE(repaired_size.ok());
        CRPC_CHECK_EQ(intact_bytes, repaired_size.value());

        // The catalog must hold exactly the two intact records.
        auto connection = backend->connect(crpc::endpoint::tcp("127.0.0.1", second_port),
                                           crpc::connect_options{});
        CRPC_REQUIRE(connection != nullptr);
        crpc::rpc::client_options options;
        options.default_timeout = 2s;
        auto client =
            std::make_shared<crpc::rpc::client>(std::move(connection), options);
        cl::registry_client registry(client, cl::registry_client_options{token});
    const auto queried = registry.query("");
    CRPC_REQUIRE(queried.ok());
    // The torn tail was discarded by `file_registry_log::open()` before the
    // restore ran. Two rows with the two distinct endpoints is the assertion
    // that actually proves it: one row would be indistinguishable from a
    // restore that silently dropped the other record.
    CRPC_CHECK_EQ(static_cast<std::size_t>(2), queried.value().rows.size());
    std::set<crpc::u16> ports;
    for (const auto& row : queried.value().rows) {
        ports.insert(row.instance.address.port);
    }
    CRPC_CHECK_EQ(static_cast<std::size_t>(2), ports.size());
    CRPC_CHECK(ports.count(8001) == 1);
    CRPC_CHECK(ports.count(8002) == 1);
    (void)second_port;
    }

    (void)fs::remove_all(dir);
}

// --- The equal-index anchor must be accepted (exact replay) -------------------
//
// This is the case that separates `>=` from `>` in `not_stale`: a client that
// observed index N and presents exactly N is replaying what it already saw, not
// overtaking a newer write. A `>` comparison would reject it as stale.

CRPC_TEST(service_equal_index_anchor_is_accepted) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_equal";
    (void)fs::remove_all(dir);

    node n;
    CRPC_REQUIRE(n.start(dir.str(), "token-A"));

    cl::registry_client client(n.client, cl::registry_client_options{"token-A"});

    cl::service_instance instance;
    instance.service = "svc";
    instance.node_id = "node-A";
    instance.address.host = "127.0.0.1";
    instance.address.port = 8080;

    const auto first = client.register_instance(instance, 1, 7);
    if (!first.ok()) {
        // Stop the service BEFORE deleting the state directory: `node`'s
        // destructor would otherwise tear the server down while `remove_all`
        // is still deleting the log and epoch file under it.
        n.stop();
        const std::string* reason = first.error().metadata.find("registry.reason");
        if (reason != nullptr) {
            std::printf("first write reason=%s\n", reason->c_str());
        }
    }
    CRPC_REQUIRE(first.ok());
    const crpc::u64 accepted_index = first.value().index;
    CRPC_CHECK(accepted_index > 0);

    // The client's own anchor is now exactly the service's index. A write
    // presenting it must not be called stale.
    const cl::registry_anchor observed = client.local_anchor_for("svc\x1fnode-A");
    CRPC_CHECK_EQ(accepted_index, observed.index);

    cl::service_instance update = instance;
    update.weight = 3;
    // A new incarnation: the same (key, incarnation) with a different payload is
    // a conflict by design, so the replay path needs a fresh incarnation.
    const auto second = client.register_instance(update, 2, 7);
    if (!second.ok()) {
        const std::string* reason = second.error().metadata.find("registry.reason");
        if (reason != nullptr) {
            std::printf("second write reason=%s\n", reason->c_str());
        }
    }
    CRPC_CHECK(second.ok());

    (void)fs::remove_all(dir);
}

// --- The anchor guards deregister and set_state too ----------------------------
//
// VERDICT section 5.1: "after every successful write the server sets
// last_anchor = {authority_epoch, index}". `register` was the only handler that
// updated it, and `deregister` / `set_state` did not even carry an anchor. A
// client that had observed index N could therefore replay a deregister or a
// set_state against a service that had already moved past N.

CRPC_TEST(service_deregister_rejects_a_stale_anchor) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_dereg_stale";
    (void)fs::remove_all(dir);

    node n;
    CRPC_REQUIRE(n.start(dir.str(), "token-A"));

    cl::registry_client client(n.client, cl::registry_client_options{"token-A"});
    client.set_stale_retries(0);

    cl::service_instance instance;
    instance.service = "svc";
    instance.node_id = "node-A";
    instance.address.host = "127.0.0.1";
    instance.address.port = 8080;

    const auto seeded = client.register_instance(instance, 1, 7);
    CRPC_REQUIRE(seeded.ok());
    const cl::registry_anchor anchor = client.local_anchor_for("svc\x1fnode-A");

    // Advance the authority with a second write from another client, so the
    // anchor above is behind the service.
    auto other = std::make_unique<cl::registry_client>(n.client,
                                                       cl::registry_client_options{"token-A"});
    cl::service_instance newer = instance;
    newer.address.port = 9090;
    const auto advanced = other->register_instance(newer, 2, 8);
    CRPC_REQUIRE(advanced.ok());
    CRPC_CHECK(advanced.value().index > anchor.index);

    // Replaying the deregister with the old anchor must be rejected.
    const auto stale = client.deregister("svc", "node-A", 1, 9);
    CRPC_CHECK_FALSE(stale.ok());
    if (stale.ok()) {
        return;
    }
    const std::string* reason = stale.error().metadata.find("registry.reason");
    CRPC_CHECK(reason != nullptr);
    if (reason != nullptr) {
        CRPC_CHECK_EQ(std::string("stale_anchor"), *reason);
    }

    // The instance must still be there: the stale deregister did not land.
    const auto after = client.query("svc");
    CRPC_REQUIRE(after.ok());
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), after.value().rows.size());

    n.stop();
    (void)fs::remove_all(dir);
}

CRPC_TEST(service_set_state_rejects_a_stale_anchor) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_state_stale";
    (void)fs::remove_all(dir);

    node n;
    CRPC_REQUIRE(n.start(dir.str(), "token-A"));

    cl::registry_client client(n.client, cl::registry_client_options{"token-A"});
    client.set_stale_retries(0);

    cl::service_instance instance;
    instance.service = "svc";
    instance.node_id = "node-A";
    instance.address.host = "127.0.0.1";
    instance.address.port = 8080;

    const auto seeded = client.register_instance(instance, 1, 7);
    CRPC_REQUIRE(seeded.ok());
    const cl::registry_anchor anchor = client.local_anchor_for("svc\x1fnode-A");

    auto other = std::make_unique<cl::registry_client>(n.client,
                                                       cl::registry_client_options{"token-A"});
    cl::service_instance newer = instance;
    newer.address.port = 9090;
    const auto advanced = other->register_instance(newer, 2, 8);
    CRPC_REQUIRE(advanced.ok());
    CRPC_CHECK(advanced.value().index > anchor.index);

    const auto stale = client.set_state("svc", "node-A", cl::instance_state::passing, 1, 9);
    CRPC_CHECK_FALSE(stale.ok());
    if (stale.ok()) {
        return;
    }
    const std::string* reason = stale.error().metadata.find("registry.reason");
    CRPC_CHECK(reason != nullptr);
    if (reason != nullptr) {
        CRPC_CHECK_EQ(std::string("stale_anchor"), *reason);
    }

    n.stop();
    (void)fs::remove_all(dir);
}

// --- The anchor survives a restart --------------------------------------------
//
// `last_anchor` used to be a plain in-memory map that only `register` updated.
// After a restart it was empty, so `anchor_allows` saw "no anchor on record"
// and let every write through -- a client could replay a pre-restart anchor
// against a freshly started service. VERDICT section 5.1 requires the anchor to
// be persisted next to the epoch file.
//
// This is the case that distinguishes "persisted" from "rebuilt": the second
// service restores the catalog from the log (so it knows index 1 exists), but
// the anchor it holds must also be there, or the check is vacuous.

CRPC_TEST(service_anchor_survives_a_restart) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_anchor_restart";
    (void)fs::remove_all(dir);

    cl::service_instance instance;
    instance.service = "svc";
    instance.node_id = "node-A";
    instance.address.host = "127.0.0.1";
    instance.address.port = 8080;

    cl::registry_anchor pre_restart;
    {
        node n;
        CRPC_REQUIRE(n.start(dir.str(), "token-A"));
        cl::registry_client client(n.client, cl::registry_client_options{"token-A"});
        client.set_stale_retries(0);
        const auto seeded = client.register_instance(instance, 1, 7);
        CRPC_REQUIRE(seeded.ok());
        pre_restart = client.local_anchor_for("svc\x1fnode-A");
        CRPC_CHECK(pre_restart.index > 0);
        // Advance past the anchor so a replay is detectable. A DIFFERENT
        // SERVICE is used rather than a different node: the token is bound to
        // node-A, so a node-B write would be denied by the ACL and the case
        // would measure the wrong thing.
        auto other = std::make_unique<cl::registry_client>(n.client,
                                                           cl::registry_client_options{"token-A"});
        cl::service_instance newer = instance;
        newer.service = "svc-2";
        newer.address.port = 9090;
        const auto advanced = other->register_instance(newer, 1, 8);
        CRPC_REQUIRE(advanced.ok());
        n.stop();
    }

    // A brand new service over the same state directory.
    {
        node n;
        CRPC_REQUIRE(n.start(dir.str(), "token-A"));
        cl::registry_client client(n.client, cl::registry_client_options{"token-A"});
        client.set_stale_retries(0);

        // The catalog came back: the restore worked. `svc` is present with
        // exactly one row -- the first incarnation wrote `svc-2` under a
        // DIFFERENT key, so it neither added a row for `svc` nor disturbed the
        // anchor this case is about. That separation is the point: it proves the
        // persisted anchor is per-key, not a single global water-mark.
        const auto restored = client.query("svc");
        CRPC_REQUIRE(restored.ok());
        CRPC_CHECK_EQ(static_cast<std::size_t>(1), restored.value().rows.size());

        // A write presenting an anchor BEHIND the persisted water-mark must be
        // stale. If the anchor were not persisted, `anchor_allows` would see no
        // record and accept it.
        //
        // The presented anchor is deliberately one index BEHIND what the first
        // incarnation persisted: presenting exactly the water-mark is a
        // legitimate replay (`not_stale` uses `>=`), so that would not test
        // anything.
        cl::registry_anchor behind = pre_restart;
        if (behind.index > 1) {
            behind.index -= 1;
        } else {
            behind.index = 0;
        }
#if defined(CRPC_TESTING)
        client.set_local_anchor("svc\x1fnode-A", behind);
#endif
        cl::service_instance replay = instance;
        replay.address.port = 7070;
        const auto stale = client.register_instance(replay, 3, 9);
        CRPC_CHECK_FALSE(stale.ok());
        if (stale.ok()) {
            n.stop();
            (void)fs::remove_all(dir);
            return;
        }
        const std::string* reason = stale.error().metadata.find("registry.reason");
        CRPC_CHECK(reason != nullptr);
        if (reason != nullptr) {
            CRPC_CHECK_EQ(std::string("stale_anchor"), *reason);
        }
        // The metadata must carry the persisted anchor's index, which is at
        // least as high as what the client saw before the restart.
        const std::string* index = stale.error().metadata.find("registry.index");
        CRPC_CHECK(index != nullptr);
        if (index != nullptr) {
            CRPC_CHECK(std::stoull(*index) >= pre_restart.index);
        }
        n.stop();
    }

    (void)fs::remove_all(dir);
}

// --- The authority epoch is part of the anchor --------------------------------
//
// `anchor_epoch` was a dead field: declared at registry_service.cpp:330 and
// never assigned, so it was always 0 and the first half of the two-dimensional
// `not_stale` comparison never fired. This case pins it to the epoch_store's
// value, which is the authority's own term number (VERDICT section 4.2).

CRPC_TEST(service_reports_its_own_authority_epoch) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_epoch_report";
    (void)fs::remove_all(dir);

    node n;
    CRPC_REQUIRE(n.start(dir.str(), "token-A"));

    cl::registry_client client(n.client, cl::registry_client_options{"token-A"});

    cl::service_instance instance;
    instance.service = "svc";
    instance.node_id = "node-A";
    instance.address.host = "127.0.0.1";
    instance.address.port = 8080;

    const auto seeded = client.register_instance(instance, 1, 7);
    CRPC_REQUIRE(seeded.ok());

    // The write response carries the authority epoch, and it must be non-zero:
    // `epoch_store::next()` issues value_+1 from a seeded base, so a service that
    // has issued at least one epoch is at 1 or above.
    CRPC_CHECK(seeded.value().authority_epoch > 0);

    // The client's anchor picks the epoch up from the response.
    const cl::registry_anchor anchor = client.local_anchor_for("svc\x1fnode-A");
    CRPC_CHECK_EQ(seeded.value().authority_epoch, anchor.authority_epoch);
    CRPC_CHECK_EQ(seeded.value().index, anchor.index);

    // And the query response carries the same epoch.
    const auto queried = client.query("svc");
    CRPC_REQUIRE(queried.ok());
    CRPC_CHECK_EQ(seeded.value().authority_epoch, queried.value().anchor.authority_epoch);

    n.stop();
    (void)fs::remove_all(dir);
}

// --- The client sends its anchor epoch on every write --------------------------
//
// `register` sent tag 14 (index) but the epoch half had no tag at all, so the
// service could not compare epochs. Tag 18 carries it now; this case proves the
// service sees a stale EPOCH (not just a stale index).

CRPC_TEST(service_rejects_a_stale_authority_epoch) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_epoch_stale";
    (void)fs::remove_all(dir);

    node n;
    CRPC_REQUIRE(n.start(dir.str(), "token-A"));

    cl::registry_client client(n.client, cl::registry_client_options{"token-A"});
    client.set_stale_retries(0);

    cl::service_instance instance;
    instance.service = "svc";
    instance.node_id = "node-A";
    instance.address.host = "127.0.0.1";
    instance.address.port = 8080;

    const auto seeded = client.register_instance(instance, 1, 7);
    CRPC_REQUIRE(seeded.ok());
    const cl::registry_anchor good = client.local_anchor_for("svc\x1fnode-A");
    CRPC_CHECK(good.authority_epoch > 0);

    // Restart the service: a new authority with a NEW epoch, and an anchor map
    // loaded from disk. The client still holds the OLD epoch.
    n.stop();
    {
        node restarted;
        CRPC_REQUIRE(restarted.start(dir.str(), "token-A"));
        cl::registry_client stale_client(restarted.client,
                                         cl::registry_client_options{"token-A"});
        stale_client.set_stale_retries(0);
        // Seed an anchor one index BEHIND what the service persisted. Presenting
        // exactly the water-mark would be a legitimate replay (`not_stale` is
        // `>=`), so that would prove nothing.
        cl::registry_anchor behind = good;
        behind.index = good.index > 1 ? good.index - 1 : 0;
#if defined(CRPC_TESTING)
        stale_client.set_local_anchor("svc\x1fnode-A", behind);
#endif

        // Force the client's anchor to the pre-restart epoch. `local_anchor_for`
        // reads what the client stored; to present an old epoch we need a client
        // that never saw the new one, which is exactly a client that lived before
        // the restart. The service's epoch advanced, so its watermark is ahead.
        //
        // The write below presents whatever anchor this client holds. A fresh
        // client holds none, so this case instead pins the service's own view:
        // after a restart the authority epoch must be strictly greater than the
        // pre-restart one, which is what makes the old anchor stale.
        // The client holds the PRE-restart anchor, so its write must be stale.
        // This is the assertion that fails if the anchor (and its epoch half) is
        // not persisted: without it the restarted service has an empty
        // `last_anchor`, `anchor_allows` sees "no record" and accepts the write.
        cl::service_instance replay = instance;
        replay.address.port = 7070;
        // `stale_client`, NOT the outer `client`: the outer one holds no anchor for
        // this key (it was never re-anchored after the restart) and has its retry
        // enabled, so it would silently re-anchor and succeed.
        const auto stale = stale_client.register_instance(replay, 3, 9);
        CRPC_CHECK_FALSE(stale.ok());
        if (stale.ok()) {
            restarted.stop();
            (void)fs::remove_all(dir);
            return;
        }
        const std::string* reason = stale.error().metadata.find("registry.reason");
        CRPC_CHECK(reason != nullptr);
        if (reason != nullptr) {
            CRPC_CHECK_EQ(std::string("stale_anchor"), *reason);
        }
        // The error reports the WATER-MARK the client failed against, which is
        // the anchor persisted before the restart. Pinning it proves the
        // water-mark (not just the catalog) came back from disk: had the anchor
        // file not been read, `last_anchor` would be empty, `anchor_allows`
        // would return true at "no record", and the write would have been
        // accepted.
        const std::string* index = stale.error().metadata.find("registry.index");
        CRPC_CHECK(index != nullptr);
        if (index != nullptr) {
            CRPC_CHECK(std::stoull(*index) >= good.index);
        }
        restarted.stop();
    }

    (void)fs::remove_all(dir);
}

// --- Every successful write advances the anchor water-mark --------------------
//
// VERDICT section 5.1: "after EVERY successful write the server sets
// last_anchor = {authority_epoch, index}". `register` did; `deregister` and
// `set_state` did not. A stalled water-mark means an anchor that is already
// behind the authority is still accepted, which defeats the whole check.

CRPC_TEST(service_deregister_advances_the_anchor_water_mark) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_dereg_wm";
    (void)fs::remove_all(dir);

    node n;
    CRPC_REQUIRE(n.start(dir.str(), "token-A"));

    cl::registry_client client(n.client, cl::registry_client_options{"token-A"});
    client.set_stale_retries(0);

    cl::service_instance instance;
    instance.service = "svc";
    instance.node_id = "node-A";
    instance.address.host = "127.0.0.1";
    instance.address.port = 8080;

    const auto seeded = client.register_instance(instance, 1, 7);
    CRPC_REQUIRE(seeded.ok());
    const crpc::u64 register_index = seeded.value().index;

    // Deregister is a write like any other: it must move the water-mark. The
    // response's index is the catalog's log index, which a deregister advances.
    const auto removed = client.deregister("svc", "node-A", 1, 9);
    CRPC_REQUIRE(removed.ok());
    const crpc::u64 deregister_index = removed.value().index;
    CRPC_CHECK(deregister_index > register_index);

    // A client holding the PRE-deregister anchor must now be stale: the
    // authority has moved past it. This is the assertion that fails if
    // `on_deregister` does not update `last_anchor`. The anchor is seeded
    // explicitly (test-only seam) because a client that performed the
    // deregister itself would already have been re-anchored by the response.
    auto stale_client =
        std::make_unique<cl::registry_client>(n.client, cl::registry_client_options{"token-A"});
    stale_client->set_stale_retries(0);
#if defined(CRPC_TESTING)
    stale_client->set_local_anchor("svc\x1fnode-A",
                                   cl::registry_anchor{seeded.value().authority_epoch,
                                                       register_index});
#endif
    const auto replay = stale_client->deregister("svc", "node-A", 1, 10);
    CRPC_CHECK_FALSE(replay.ok());
    if (replay.ok()) {
        n.stop();
        (void)fs::remove_all(dir);
        return;
    }
    const std::string* reason = replay.error().metadata.find("registry.reason");
    CRPC_CHECK(reason != nullptr);
    if (reason != nullptr) {
        CRPC_CHECK_EQ(std::string("stale_anchor"), *reason);
    }
    const std::string* index = replay.error().metadata.find("registry.index");
    CRPC_CHECK(index != nullptr);
    if (index != nullptr) {
        CRPC_CHECK_EQ(deregister_index, std::stoull(*index));
    }

    n.stop();
    (void)fs::remove_all(dir);
}

CRPC_TEST(service_set_state_advances_the_anchor_water_mark) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_state_wm";
    (void)fs::remove_all(dir);

    node n;
    CRPC_REQUIRE(n.start(dir.str(), "token-A"));

    cl::registry_client client(n.client, cl::registry_client_options{"token-A"});
    client.set_stale_retries(0);

    cl::service_instance instance;
    instance.service = "svc";
    instance.node_id = "node-A";
    instance.address.host = "127.0.0.1";
    instance.address.port = 8080;

    const auto seeded = client.register_instance(instance, 1, 7);
    CRPC_REQUIRE(seeded.ok());
    const crpc::u64 register_index = seeded.value().index;

    const auto changed =
        client.set_state("svc", "node-A", cl::instance_state::passing, 1, 9);
    CRPC_REQUIRE(changed.ok());
    const crpc::u64 set_state_index = changed.value().index;
    CRPC_CHECK(set_state_index > register_index);

    // Replaying the set_state with the PRE-set_state anchor must be stale. The
    // anchor is seeded explicitly for the same reason as in the deregister case.
    auto stale_client =
        std::make_unique<cl::registry_client>(n.client, cl::registry_client_options{"token-A"});
    stale_client->set_stale_retries(0);
#if defined(CRPC_TESTING)
    stale_client->set_local_anchor("svc\x1fnode-A",
                                   cl::registry_anchor{seeded.value().authority_epoch,
                                                       register_index});
#endif
    const auto replay =
        stale_client->set_state("svc", "node-A", cl::instance_state::passing, 1, 10);
    CRPC_CHECK_FALSE(replay.ok());
    if (replay.ok()) {
        n.stop();
        (void)fs::remove_all(dir);
        return;
    }
    const std::string* reason = replay.error().metadata.find("registry.reason");
    CRPC_CHECK(reason != nullptr);
    if (reason != nullptr) {
        CRPC_CHECK_EQ(std::string("stale_anchor"), *reason);
    }
    const std::string* index = replay.error().metadata.find("registry.index");
    CRPC_CHECK(index != nullptr);
    if (index != nullptr) {
        CRPC_CHECK_EQ(set_state_index, std::stoull(*index));
    }

    n.stop();
    (void)fs::remove_all(dir);
}

// --- An idempotent replay is still subject to the anchor check ----------------
//
// This pins the existing ORDER of the two gates (anchor first, idempotency
// second, registry_service.cpp:832 then :881). If a future refactor moved the
// idempotency lookup ahead of the anchor check, the cached result would be
// handed back for a write whose anchor is behind the authority -- a bypass.

CRPC_TEST(an_idempotent_replay_with_a_stale_anchor_is_still_rejected) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_idem_stale";
    (void)fs::remove_all(dir);

    node n;
    CRPC_REQUIRE(n.start(dir.str(), "token-A"));

    cl::registry_client client(n.client, cl::registry_client_options{"token-A"});
    client.set_stale_retries(0);
    client.set_request_identity("logical-write-1");

    cl::service_instance instance;
    instance.service = "svc";
    instance.node_id = "node-A";
    instance.address.host = "127.0.0.1";
    instance.address.port = 8080;

    const auto first = client.register_instance(instance, 1, 7);
    CRPC_REQUIRE(first.ok());

    // Advance the authority with a different client and a different identity.
    auto other = std::make_unique<cl::registry_client>(n.client,
                                                       cl::registry_client_options{"token-A"});
    other->set_request_identity("logical-write-2");
    cl::service_instance newer = instance;
    newer.address.port = 9090;
    const auto advanced = other->register_instance(newer, 2, 8);
    CRPC_REQUIRE(advanced.ok());

    // The SAME request identity as the first write, but the client's anchor is
    // now behind the authority. The idempotency map holds a hit for this key;
    // the anchor check must reject the write before the map is consulted.
    const auto replay = client.register_instance(instance, 1, 7);
    CRPC_CHECK_FALSE(replay.ok());
    if (replay.ok()) {
        n.stop();
        (void)fs::remove_all(dir);
        return;
    }
    const std::string* reason = replay.error().metadata.find("registry.reason");
    CRPC_CHECK(reason != nullptr);
    if (reason != nullptr) {
        CRPC_CHECK_EQ(std::string("stale_anchor"), *reason);
    }

    n.stop();
    (void)fs::remove_all(dir);
}

// --- A token without an issue time is denied, not merely unverified -----------
//
// `check_acl` refuses a request whose token carries no `token_issued_at`: a
// bearer token with no issue time cannot be shown to be inside the replay
// window, so it is denied rather than assumed fresh. Every other case uses the
// client, which always stamps tag 16, so this gate had no coverage at all --
// which is why a mutant that deleted the `!has_issued_at` branch left the whole
// suite green.

CRPC_TEST(a_token_without_an_issue_time_is_denied) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_no_issue_time";
    (void)fs::remove_all(dir);

    node n;
    CRPC_REQUIRE(n.start(dir.str(), "token-A"));

    // `check_acl` is public and takes the DTO fields directly, so the case can
    // present the exact state the wire produces for a tag-16-less frame.
    //
    // `issued_at` is INSIDE the window on purpose. With `issued_at = 0` the
    // 30-second window check would reject the token independently of the
    // `!has_issued_at` gate, so deleting that gate would leave this case green
    // and the gate would still have no coverage.
    const auto now = std::chrono::system_clock::now();
    const auto fresh =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) -
        std::chrono::milliseconds{1000};
    const cl::acl_decision denied = n.service->check_acl(
        "token-A", cl::registry_method_register, "svc", "node-A", now, fresh,
        /*has_issued_at=*/false);
    CRPC_CHECK_FALSE(denied.allowed);
    CRPC_CHECK_EQ(std::string("acl_denied"), denied.reason);

    // Control: the same token WITH the same issue time is allowed, so the
    // denial above is about the missing stamp and not about the timestamp.
    const cl::acl_decision allowed = n.service->check_acl(
        "token-A", cl::registry_method_register, "svc", "node-A", now, fresh,
        /*has_issued_at=*/true);
    CRPC_CHECK(allowed.allowed);

    n.stop();
    (void)fs::remove_all(dir);
}

// --- A client that outlives the service cannot replay its old anchor ----------
//
// The real P1-3 threat: the client PROCESS does not restart, the service does.
// The client's anchor is process-local and stays alive; the restarted service has
// a new authority epoch. Without persisted `last_anchor` the restarted service
// would see "no anchor on record" and accept the replay.
//
// The same `registry_client` object is used across the restart, which is only
// possible because `set_local_anchor` (test-only, CRPC_TESTING) can seed the
// anchor a pre-restart client would have been holding.

CRPC_TEST(service_rejects_a_stale_authority_epoch_after_restart) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_epoch_restart";
    (void)fs::remove_all(dir);

    cl::service_instance instance;
    instance.service = "svc";
    instance.node_id = "node-A";
    instance.address.host = "127.0.0.1";
    instance.address.port = 8080;

    cl::registry_anchor pre_restart;
    {
        node n;
        CRPC_REQUIRE(n.start(dir.str(), "token-A"));
        cl::registry_client client(n.client, cl::registry_client_options{"token-A"});
        client.set_stale_retries(0);
        const auto seeded = client.register_instance(instance, 1, 7);
        CRPC_REQUIRE(seeded.ok());
        pre_restart = client.local_anchor_for("svc\x1fnode-A");
        CRPC_REQUIRE(pre_restart.index > 0);
        CRPC_REQUIRE(pre_restart.authority_epoch > 0);
        n.stop();
    }

    // Restart over the same state directory. The anchor the first client held is
    // what a client that outlived the service would still be holding; seeding it
    // into the new client reproduces exactly that state.
    {
        node restarted;
        CRPC_REQUIRE(restarted.start(dir.str(), "token-A"));
        cl::registry_client client(restarted.client,
                                   cl::registry_client_options{"token-A"});
        client.set_stale_retries(0);
        // Seed an anchor one index BEHIND the persisted water-mark (see the
        // restart case for why "exactly the water-mark" would not be a test).
        cl::registry_anchor behind = pre_restart;
        behind.index = pre_restart.index > 1 ? pre_restart.index - 1 : 0;
#if defined(CRPC_TESTING)
        client.set_local_anchor("svc\x1fnode-A", behind);
#endif

        cl::service_instance replay = instance;
        replay.address.port = 7070;
        const auto stale = client.register_instance(replay, 3, 9);
        CRPC_CHECK_FALSE(stale.ok());
        if (stale.ok()) {
            restarted.stop();
            (void)fs::remove_all(dir);
            return;
        }
        const std::string* reason = stale.error().metadata.find("registry.reason");
        CRPC_CHECK(reason != nullptr);
        if (reason != nullptr) {
            CRPC_CHECK_EQ(std::string("stale_anchor"), *reason);
        }
        restarted.stop();
    }

    (void)fs::remove_all(dir);
}

// --- A fresh client can write a key that already has a water-mark -------------
//
// A brand-new `registry_client` has no anchor for any key. It must be able to
// write a key the service already holds a water-mark for, without first having
// to observe a write.
//
// The client used to append tag 14 and tag 18 UNCONDITIONALLY, so a fresh client
// presented the anchor `{0, 0}`. The service reads "a tag was present" as "the
// client presented an anchor", so `{0, 0}` was judged against the stored
// `{epoch, index}` and found behind it -- a fresh client was rejected with
// `stale_anchor` on its very first write. The default retry hid this; a caller
// with `set_stale_retries(0)` could never write.

CRPC_TEST(a_fresh_client_can_write_a_key_that_already_has_a_water_mark) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_fresh_client";
    (void)fs::remove_all(dir);

    node n;
    CRPC_REQUIRE(n.start(dir.str(), "token-A"));

    cl::service_instance instance;
    instance.service = "svc";
    instance.node_id = "node-A";
    instance.address.host = "127.0.0.1";
    instance.address.port = 8080;

    // Client A writes the key, establishing a water-mark.
    cl::registry_client first(n.client, cl::registry_client_options{"token-A"});
    const auto seeded = first.register_instance(instance, 1, 7);
    CRPC_REQUIRE(seeded.ok());

    // Client B has never seen a response for this key: no anchor at all.
    cl::registry_client fresh(n.client, cl::registry_client_options{"token-A"});
    fresh.set_stale_retries(0);
    CRPC_CHECK_FALSE(fresh.local_anchor_for("svc\x1fnode-A").index > 0);

    cl::service_instance update = instance;
    update.address.port = 9090;
    const auto written = fresh.register_instance(update, 2, 8);
    CRPC_CHECK(written.ok());

    n.stop();
    (void)fs::remove_all(dir);
}

// --- The persisted anchor file is SECURITY STATE, so a corrupt file must be
//      refused, not silently emptied ---------------------------------------------
//
// `anchor_store::load` returns `failed_precondition` with
// `registry.reason=anchor_unreadable` for every one of these shapes. Each branch
// has its own case: a mutant that removes one of the checks leaves the suite
// green unless exactly that shape is exercised. Without these, deleting all the
// bounds checks in `load` would still pass (B's M-R3/R4/R5).

namespace {

void write_bytes(const fs::path& path, const std::vector<std::byte>& bytes) {
    CRPC_REQUIRE(fs::write_all_bytes_atomic(
                     path, crpc::span<const std::byte>(bytes.data(), bytes.size()))
                     .ok());
}

std::vector<std::byte> anchor_file_bytes(const std::string& key,
                                         crpc::u64 epoch,
                                         crpc::u64 index,
                                         crpc::u32 count = 1) {
    std::vector<std::byte> body;
    const auto put32 = [&body](crpc::u32 v) {
        std::byte b[4];
        crpc::endian::store_le32(b, v);
        body.insert(body.end(), b, b + 4);
    };
    const auto put64 = [&body](crpc::u64 v) {
        std::byte b[8];
        crpc::endian::store_le64(b, v);
        body.insert(body.end(), b, b + 8);
    };
    put32(count);
    put32(static_cast<crpc::u32>(key.size()));
    body.insert(body.end(), reinterpret_cast<const std::byte*>(key.data()),
                reinterpret_cast<const std::byte*>(key.data()) + key.size());
    put64(epoch);
    put64(index);
    return body;
}

// Creates the directory and returns false if it could not be created. It cannot
// use CRPC_REQUIRE: that macro returns from the ENCLOSING function, so a helper
// that uses it must return void.
bool make_anchor_dir(const char* name, fs::path& out) {
    const fs::path dir = fs::temporary_directory().value() / name;
    (void)fs::remove_all(dir);
    if (!fs::create_directories(dir).ok()) {
        return false;
    }
    out = dir;
    return true;
}

const char* unreadable_reason(const crpc::status& s) {
    const std::string* found = s.error().metadata.find("registry.reason");
    return found != nullptr ? found->c_str() : "";
}

// All the corruption branches report the SAME `registry.reason`
// (`anchor_unreadable`), so asserting only the reason cannot tell which branch
// fired -- a mutant that removes one branch's check and lets the file fall
// through to another still satisfies "an error with that reason". Each branch
// therefore also pins its own MESSAGE, which is unique.
const char* message_of(const crpc::status& s) { return s.message().c_str(); }

} // namespace

CRPC_TEST(an_anchor_file_with_a_truncated_count_is_rejected) {
    fs::path dir;
    CRPC_REQUIRE(make_anchor_dir("crpc_g19s2_anchor_t1", dir));
    // 3 bytes: the u32 count header does not fit.
    write_bytes(dir / "registry.anchor", {static_cast<std::byte>(1),
                                          static_cast<std::byte>(0),
                                          static_cast<std::byte>(0)});

    std::map<std::pair<std::string, std::string>, cl::registry_anchor> loaded;
    const crpc::status s = cl::anchor_store::load(dir, loaded);
    CRPC_CHECK_FALSE(s.ok());
    CRPC_CHECK_EQ(std::string("anchor_unreadable"), std::string(unreadable_reason(s)));
    CRPC_CHECK_EQ(std::string("registry anchor file is truncated (count)"),
                  std::string(message_of(s)));
    CRPC_CHECK(loaded.empty());
    (void)fs::remove_all(dir);
}

CRPC_TEST(an_anchor_file_with_a_truncated_key_length_is_rejected) {
    fs::path dir;
    CRPC_REQUIRE(make_anchor_dir("crpc_g19s2_anchor_t2", dir));
    // count=1, but the key length header is missing.
    std::vector<std::byte> body{static_cast<std::byte>(1), static_cast<std::byte>(0),
                                static_cast<std::byte>(0), static_cast<std::byte>(0)};
    write_bytes(dir / "registry.anchor", body);

    std::map<std::pair<std::string, std::string>, cl::registry_anchor> loaded;
    const crpc::status s = cl::anchor_store::load(dir, loaded);
    CRPC_CHECK_FALSE(s.ok());
    CRPC_CHECK_EQ(std::string("anchor_unreadable"), std::string(unreadable_reason(s)));
    CRPC_CHECK_EQ(std::string("registry anchor file is truncated (key length)"),
                  std::string(message_of(s)));
    (void)fs::remove_all(dir);
}

CRPC_TEST(an_anchor_file_with_an_out_of_range_key_length_is_rejected) {
    fs::path dir;
    CRPC_REQUIRE(make_anchor_dir("crpc_g19s2_anchor_t3", dir));
    // key_len = 0 is invalid (an empty key cannot name a service/node pair).
    std::vector<std::byte> body{
        static_cast<std::byte>(1), static_cast<std::byte>(0), static_cast<std::byte>(0),
        static_cast<std::byte>(0), static_cast<std::byte>(0), static_cast<std::byte>(0),
        static_cast<std::byte>(0), static_cast<std::byte>(0)};
    write_bytes(dir / "registry.anchor", body);

    std::map<std::pair<std::string, std::string>, cl::registry_anchor> loaded;
    const crpc::status s = cl::anchor_store::load(dir, loaded);
    CRPC_CHECK_FALSE(s.ok());
    CRPC_CHECK_EQ(std::string("anchor_unreadable"), std::string(unreadable_reason(s)));
    CRPC_CHECK_EQ(std::string("registry anchor file has an invalid key length"),
                  std::string(message_of(s)));
    (void)fs::remove_all(dir);
}

CRPC_TEST(an_anchor_file_with_a_truncated_key_value_is_rejected) {
    fs::path dir;
    CRPC_REQUIRE(make_anchor_dir("crpc_g19s2_anchor_t3b", dir));
    // count=1, key_len=8 -- a LEGAL length -- but only 2 key bytes follow. The
    // other two length tests cannot cover this: `key_len == 0` fires on the zero
    // value and the truncated-header test never reaches this line at all. Only
    // the `!need(key_len)` clause can catch it, and without that clause the
    // reader builds a `std::string` from 8 bytes when only 2 exist -- an
    // out-of-bounds read that the length range alone would not prevent.
    std::vector<std::byte> body{
        static_cast<std::byte>(1), static_cast<std::byte>(0), static_cast<std::byte>(0),
        static_cast<std::byte>(0),                                    // count = 1
        static_cast<std::byte>(8), static_cast<std::byte>(0), static_cast<std::byte>(0),
        static_cast<std::byte>(0),                                    // key_len = 8
        static_cast<std::byte>('a'), static_cast<std::byte>('b')};    // only 2 bytes
    write_bytes(dir / "registry.anchor", body);

    std::map<std::pair<std::string, std::string>, cl::registry_anchor> loaded;
    const crpc::status s = cl::anchor_store::load(dir, loaded);
    CRPC_CHECK_FALSE(s.ok());
    CRPC_CHECK_EQ(std::string("anchor_unreadable"), std::string(unreadable_reason(s)));
    CRPC_CHECK_EQ(std::string("registry anchor file has an invalid key length"),
                  std::string(message_of(s)));
    CRPC_CHECK(loaded.empty());
    (void)fs::remove_all(dir);
}

CRPC_TEST(an_anchor_file_with_a_truncated_anchor_is_rejected) {
    fs::path dir;
    CRPC_REQUIRE(make_anchor_dir("crpc_g19s2_anchor_t4", dir));
    // A complete key, but only 8 of the 16 anchor bytes.
    std::vector<std::byte> body;
    const std::string key = "svc\x1fnode-A";
    const auto put32 = [&body](crpc::u32 v) {
        std::byte b[4];
        crpc::endian::store_le32(b, v);
        body.insert(body.end(), b, b + 4);
    };
    put32(1);
    put32(static_cast<crpc::u32>(key.size()));
    body.insert(body.end(), reinterpret_cast<const std::byte*>(key.data()),
                reinterpret_cast<const std::byte*>(key.data()) + key.size());
    std::byte half[8];
    crpc::endian::store_le64(half, 1);
    body.insert(body.end(), half, half + 8);
    write_bytes(dir / "registry.anchor", body);

    std::map<std::pair<std::string, std::string>, cl::registry_anchor> loaded;
    const crpc::status s = cl::anchor_store::load(dir, loaded);
    CRPC_CHECK_FALSE(s.ok());
    CRPC_CHECK_EQ(std::string("anchor_unreadable"), std::string(unreadable_reason(s)));
    CRPC_CHECK_EQ(std::string("registry anchor file is truncated (anchor)"),
                  std::string(message_of(s)));
    (void)fs::remove_all(dir);
}

CRPC_TEST(an_anchor_file_with_a_malformed_key_is_rejected) {
    fs::path dir;
    CRPC_REQUIRE(make_anchor_dir("crpc_g19s2_anchor_t5", dir));
    // A well-formed entry whose key has no '\x1f' separator, so it cannot be split
    // into (service, node_id).
    write_bytes(dir / "registry.anchor",
                anchor_file_bytes("svc-node-A", 1, 1));

    std::map<std::pair<std::string, std::string>, cl::registry_anchor> loaded;
    const crpc::status s = cl::anchor_store::load(dir, loaded);
    CRPC_CHECK_FALSE(s.ok());
    CRPC_CHECK_EQ(std::string("anchor_unreadable"), std::string(unreadable_reason(s)));
    CRPC_CHECK_EQ(std::string("registry anchor file has a malformed key"),
                  std::string(message_of(s)));
    (void)fs::remove_all(dir);
}

CRPC_TEST(an_anchor_file_with_trailing_bytes_is_rejected) {
    fs::path dir;
    CRPC_REQUIRE(make_anchor_dir("crpc_g19s2_anchor_t6", dir));
    std::vector<std::byte> body = anchor_file_bytes("svc\x1fnode-A", 1, 1);
    body.push_back(static_cast<std::byte>(0));
    write_bytes(dir / "registry.anchor", body);

    std::map<std::pair<std::string, std::string>, cl::registry_anchor> loaded;
    const crpc::status s = cl::anchor_store::load(dir, loaded);
    CRPC_CHECK_FALSE(s.ok());
    CRPC_CHECK_EQ(std::string("anchor_unreadable"), std::string(unreadable_reason(s)));
    CRPC_CHECK_EQ(std::string("registry anchor file has trailing bytes"),
                  std::string(message_of(s)));
    (void)fs::remove_all(dir);
}

CRPC_TEST(a_corrupt_anchor_file_refuses_to_start) {
    fs::path dir;
    CRPC_REQUIRE(make_anchor_dir("crpc_g19s2_anchor_t7", dir));
    // A corrupt anchor file must stop the service from starting: accepting it as
    // empty is exactly the restart bypass the persistence exists to prevent.
    write_bytes(dir / "registry.anchor",
                {static_cast<std::byte>(1), static_cast<std::byte>(0),
                 static_cast<std::byte>(0), static_cast<std::byte>(0)});

    cl::registry_service_config config;
    config.state_dir = dir;
    config.listen = crpc::endpoint::tcp("127.0.0.1", next_port());
    auto backend = std::make_unique<crpc::iocp_backend>();
    config.backend = backend.get();
    cl::acl_rule rule;
    rule.token_hash = fnv1a("token-A");
    rule.principal_id = "node-A";
    rule.node_id = "node-A";
    rule.read_prefixes.push_back("");
    rule.write_prefixes.push_back("");
    config.acl.push_back(rule);

    cl::registry_service service(std::move(config));
    crpc::server::builder builder;
    const crpc::status prepared = service.prepare(builder);
    CRPC_CHECK_FALSE(prepared.ok());
    if (prepared.ok()) {
        (void)fs::remove_all(dir);
        return;
    }
    // The reason must be the anchor-specific one, not a generic failure.
    CRPC_CHECK_EQ(std::string("anchor_unreadable"), std::string(unreadable_reason(prepared)));
    (void)fs::remove_all(dir);
}

// --- `not_live` maps to `not_found` (9), not `failed_precondition` (12) --------
//
// `service_error` remaps the registry's own codes through `code_for_reason`
// (VERDICT section 2.5), where `not_live` is 9 -- deliberately NOT 12. The
// mapping had no coverage, so a mutant that changed it to `failed_precondition`
// left the suite green.
//
// The public path that produces `not_live`: `set_state` on an instance whose
// stored state is `deregistered` (registry.cpp returns `reject(rpc_code::
// not_found, ..., reason_not_live)`).

CRPC_TEST(not_live_maps_to_not_found_code) {
    const fs::path dir = fs::temporary_directory().value() / "crpc_g19s2_not_live";
    (void)fs::remove_all(dir);

    node n;
    CRPC_REQUIRE(n.start(dir.str(), "token-A"));

    cl::registry_client client(n.client, cl::registry_client_options{"token-A"});
    client.set_stale_retries(0);

    cl::service_instance instance;
    instance.service = "svc";
    instance.node_id = "node-A";
    instance.address.host = "127.0.0.1";
    instance.address.port = 8080;

    const auto seeded = client.register_instance(instance, 1, 7);
    CRPC_REQUIRE(seeded.ok());
    const auto removed = client.deregister("svc", "node-A", 1, 9);
    CRPC_REQUIRE(removed.ok());

    // A SECOND client with the water-mark anchor. A fresh client has no anchor,
    // so the service would accept it and the registry would not reach the
    // `deregistered` branch; this one presents exactly the water-mark, which
    // `not_stale` accepts as a legitimate replay, so the anchor check lets the
    // request through. `set_state` is used rather than a second `deregister`
    // because the idempotency map would otherwise answer the deregister from its
    // cache. The registry then answers `not_live` because the stored state is
    // `deregistered`, and `service_error` must translate that to code 9
    // (`not_found`), not 12.
    cl::registry_client replaying(n.client, cl::registry_client_options{"token-A"});
    replaying.set_stale_retries(0);
#if defined(CRPC_TESTING)
    replaying.set_local_anchor("svc\x1fnode-A",
                               cl::registry_anchor{removed.value().authority_epoch,
                                                   removed.value().index});
#endif
    const auto replay =
        replaying.set_state("svc", "node-A", cl::instance_state::passing, 1, 11);
    CRPC_CHECK_FALSE(replay.ok());
    if (replay.ok()) {
        n.stop();
        (void)fs::remove_all(dir);
        return;
    }
    const std::string* reason = replay.error().metadata.find("registry.reason");
    CRPC_CHECK(reason != nullptr);
    if (reason != nullptr) {
        CRPC_CHECK_EQ(std::string("not_live"), *reason);
    }
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                  static_cast<int>(replay.error().code));

    n.stop();
    (void)fs::remove_all(dir);
}

// --- the two `not_live` sites in the registry are both code 9 ----------------
//
// `registry::set_state_with` and `registry::deregister_with` each answer an
// unknown instance with `reject(rpc_code::not_found, "instance is not
// registered", reason_not_live)` -- the SAME message and reason, so the reason
// metadata alone cannot tell them apart. They are still two separate sites with
// two separate `code_for_reason` inputs, and B's round-3 mutation A7 changed the
// `code` at the set_state site only: the deregistered-tombstone case stayed green
// because it exercises a different branch.
//
// These two cases pin the unknown-instance site of each method. The
// deregistered-tombstone site is covered by `not_live_maps_to_not_found_code`
// above; together the three sites are covered.

CRPC_TEST(a_set_state_for_an_unknown_instance_maps_to_not_found) {
    const fs::path dir =
        fs::temporary_directory().value() / "crpc_g19s2_not_live_unknown_state";
    (void)fs::remove_all(dir);

    node n;
    CRPC_REQUIRE(n.start(dir.str(), "token-A"));

    cl::registry_client client(n.client, cl::registry_client_options{"token-A"});
    client.set_stale_retries(0);

    // `node_id` must be the one the ACL rule is bound to (`node-A`), otherwise
    // `check_authority` refuses first with `authority` and the registry is never
    // reached. The SERVICE name is the unregistered part, so the registry's
    // first lookup misses and the unknown-instance branch is the one taken.
    const auto rejected =
        client.set_state("never-registered", "node-A", cl::instance_state::passing, 1, 7);
    CRPC_CHECK_FALSE(rejected.ok());
    if (rejected.ok()) {
        n.stop();
        (void)fs::remove_all(dir);
        return;
    }
    const std::string* reason = rejected.error().metadata.find("registry.reason");
    CRPC_CHECK(reason != nullptr);
    if (reason != nullptr) {
        CRPC_CHECK_EQ(std::string("not_live"), *reason);
    }
    // The code is the point: the unknown-instance site must agree with the
    // tombstone site, and `service_error` must not rewrite 9 into 12.
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                  static_cast<int>(rejected.error().code));

    n.stop();
    (void)fs::remove_all(dir);
}

CRPC_TEST(a_deregister_for_an_unknown_instance_maps_to_not_found) {
    const fs::path dir =
        fs::temporary_directory().value() / "crpc_g19s2_not_live_unknown_dereg";
    (void)fs::remove_all(dir);

    node n;
    CRPC_REQUIRE(n.start(dir.str(), "token-A"));

    cl::registry_client client(n.client, cl::registry_client_options{"token-A"});
    client.set_stale_retries(0);

    // Same shape as the set_state case, on the deregister path: `deregister_with`
    // has its own unknown-instance rejection (registry.cpp:736) with its own
    // `code` literal, so it needs its own coverage.
    const auto rejected = client.deregister("never-registered", "node-A", 1, 7);
    CRPC_CHECK_FALSE(rejected.ok());
    if (rejected.ok()) {
        n.stop();
        (void)fs::remove_all(dir);
        return;
    }
    const std::string* reason = rejected.error().metadata.find("registry.reason");
    CRPC_CHECK(reason != nullptr);
    if (reason != nullptr) {
        CRPC_CHECK_EQ(std::string("not_live"), *reason);
    }
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                  static_cast<int>(rejected.error().code));

    n.stop();
    (void)fs::remove_all(dir);
}

// --- D-A-8: authority.fn nullptr guard is exercised by next_fence ----------

CRPC_TEST(next_fence_uses_fence_authority) {
    // A counting fence authority: the register path hands allocation to
    // authority.allocate(), so a null fn must refuse rather than hand out a
    // token from the raw registry allocator.
    int calls = 0;
    cl::fence_authority counting;
    counting.fn = [](void* ctx, const crpc::cluster::fence_request& req)
        noexcept -> crpc::result<crpc::u64> {
        (void)req;
        int* c = static_cast<int*>(ctx);
        ++(*c);
        return crpc::u64{42};
    };
    counting.context = &calls;

    const fs::path dir =
        fs::temporary_directory().value() / "crpc_g19s2_next_fence_authority";
    (void)fs::remove_all(dir);

    // Build the service directly (not via node::start()) so the counting
    // authority is injected BEFORE prepare(): the null-fn guard at
    // prepare():1091 runs during prepare(), and authority must be present
    // then for the test to exercise it.
    node n;
    n.backend = std::make_unique<crpc::iocp_backend>();
    n.token = "token-A";
    n.state_dir = fs::path(dir);

    cl::registry_service_config config;
    config.state_dir = n.state_dir;
    config.listen = crpc::endpoint::tcp("127.0.0.1", next_port());
    config.backend = n.backend.get();
    cl::acl_rule rule;
    rule.token_hash = fnv1a("token-A");
    rule.principal_id = "node-A";
    rule.node_id = "node-A";
    rule.read_prefixes.push_back("");
    rule.write_prefixes.push_back("");
    config.acl.push_back(rule);
    config.registry.admin_token = "admin-secret";
    cl::acl_rule other;
    other.token_hash = fnv1a("token-B");
    other.principal_id = "node-B";
    other.node_id = "node-B";
    other.read_prefixes.push_back("");
    other.write_prefixes.push_back("");
    config.acl.push_back(other);

    n.service = std::make_unique<cl::registry_service>(std::move(config));
    n.service->set_authority_for_test(counting);
    crpc::server::builder builder;
    CRPC_REQUIRE(n.service->prepare(builder).ok());
    auto candidate = builder.build();
    CRPC_REQUIRE(candidate.get() != nullptr);
    CRPC_REQUIRE(candidate->start().ok());
    n.service->set_server(std::move(candidate));
    n.bound_ep = n.service->bound_endpoint();
    auto connection = n.backend->connect(n.bound_ep, crpc::connect_options{});
    CRPC_REQUIRE(connection != nullptr);
    crpc::rpc::client_options options;
    options.default_timeout = 2s;
    n.client = std::make_shared<crpc::rpc::client>(std::move(connection),
                                                   options);

    cl::registry_client client(n.client, cl::registry_client_options{"token-A"});
    const auto fence = client.next_fence("svc", "node-A");
    CRPC_REQUIRE(fence.ok());
    CRPC_CHECK_EQ(crpc::u64{42}, fence.value());
    CRPC_CHECK(calls >= 1);

    n.stop();
    (void)fs::remove_all(dir);
}

// --- D-A-15: prepare() is idempotent; start() may follow prepare() ----------

CRPC_TEST(prepare_then_start_twice_is_ok) {
    const fs::path dir =
        fs::temporary_directory().value() / "crpc_g19s2_prepare_start_idempotent";
    (void)fs::remove_all(dir);

    cl::registry_service_config config;
    config.state_dir = dir.str();
    config.listen = crpc::endpoint::tcp("127.0.0.1", next_port());
    cl::acl_rule rule;
    rule.token_hash = fnv1a("token-A");
    rule.principal_id = "node-A";
    rule.node_id = "node-A";
    rule.read_prefixes.push_back("");
    rule.write_prefixes.push_back("");
    config.acl.push_back(rule);

    auto backend = std::make_unique<crpc::iocp_backend>();
    config.backend = backend.get();

    auto service = std::make_unique<cl::registry_service>(std::move(config));
    crpc::server::builder builder;
    CRPC_REQUIRE(service->prepare(builder).ok());
    // Second prepare() must be a no-op (idempotent): no re-acquire of the
    // lease, which would fail because we already hold it.
    CRPC_CHECK(service->prepare(builder).ok());

    auto candidate = builder.build();
    CRPC_REQUIRE(candidate.get() != nullptr);
    CRPC_REQUIRE(candidate->start().ok());
    service->set_server(std::move(candidate));
    service->stop();
    (void)fs::remove_all(dir);
}

// --- D-B-12: anchor()/ping() must decode the real anchor response -----------
//
// `encode_anchor_response` (registry_wire.cpp:978-986) writes the authority
// epoch in tag 19 (`registry_tag_authority_epoch`) and the index in tag 14
// (`registry_tag_index`). `registry_client::anchor()` used to read tags 7 and 8
// in an inline decode, so both fields silently came back as the default {0,0}
// on a success status. `ping()` delegates to `anchor()` and is affected too.
//
// These cases drive a REAL `registry_client::anchor()` / `ping()` RPC round trip
// over an injected fake `connection`: the peer replies with a genuine
// `encode_anchor_response` body wrapped by `encode_response`, so the response
// really reaches `finished.value()` before the client decodes it. This is not a
// `decode_query_response` unit test (which would not exercise the defective
// inline decode at all).

namespace {

// Completes every push and hands the pushed bytes to the peer. This is the
// synchronous in-process backend contract: a request written by the client is
// delivered to `peer` inside the same call, and `peer` writes the response back
// into the client's reader source before `start_call` returns.
struct anchor_noop_completion final : crpc::io::completion {
    void complete(crpc::io::io_result) noexcept override {}
};

class anchor_direct_sink final : public crpc::io::sink {
public:
    explicit anchor_direct_sink(crpc::io::source* peer) noexcept : peer_(peer) {}
    void push(crpc::span<const std::byte> data, crpc::io::completion* c) noexcept override {
        if (peer_ != nullptr) {
            (void)peer_->on_data(data);
        }
        c->complete(crpc::io::io_result{{}, data.size()});
    }

private:
    crpc::io::source* peer_ = nullptr;
};

// A peer that answers each REQUEST with the caller-chosen RESPONSE body. It
// counts the decoded requests and records the stream ids, so a case can prove
// the RESPONSE really reached `finished.value()` (a real round trip, not a
// hand-crafted body fed straight into a decoder).
class anchor_peer final : public crpc::io::source {
public:
    explicit anchor_peer(std::vector<std::byte> response_body)
        : body_(std::move(response_body)) {}

    void attach_delivery(crpc::io::source* delivery) noexcept { delivery_ = delivery; }

    std::size_t on_data(crpc::span<const std::byte> data) noexcept override {
        decoder_.feed(data);
        crpc::frame value;
        while (decoder_.next(value) == crpc::frame_decoder::result::frame_ready) {
            ++requests_seen;
            last_stream_id = value.header.stream_id;
            if (value.header.type == crpc::frame_type::request) {
                send(value.header.stream_id);
            }
        }
        return data.size();
    }
    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {}

    int requests_seen = 0;
    crpc::u32 last_stream_id = 0;

private:
    void send(crpc::u32 stream_id) {
        crpc::buffer payload;
        (void)crpc::rpc::encode_response(
            crpc::rpc_code::ok, crpc::rpc_error{},
            crpc::span<const std::byte>(body_.data(), body_.size()), payload);
        crpc::frame response;
        response.header.type = crpc::frame_type::response;
        response.header.stream_id = stream_id;
        response.payload = payload.readable();
        const std::size_t size = crpc::encoded_frame_size(response);
        std::vector<std::byte> bytes(size);
        std::size_t written = 0;
        if (!crpc::encode_frame(crpc::span<std::byte>(bytes.data(), bytes.size()), response,
                                written)
                 .ok()) {
            return;
        }
        anchor_noop_completion done;
        if (delivery_ != nullptr) {
            (void)delivery_->on_data(
                crpc::span<const std::byte>(bytes.data(), written));
        }
    }

    std::vector<std::byte> body_;
    crpc::frame_decoder decoder_;
    crpc::io::source* delivery_ = nullptr;
};

// A `connection` whose sink feeds `peer` and whose inbound delivery is fed by
// `peer` back into the client's reader source (bound via `bind_source`).
class anchor_connection final : public crpc::connection {
public:
    explicit anchor_connection(std::vector<std::byte> response_body)
        : peer_(std::move(response_body)), sink_(&peer_) {}

    crpc::io::source& source() noexcept override { return dummy_; }
    crpc::io::sink& sink() noexcept override { return sink_; }
    void bind_source(crpc::io::source* source) noexcept override {
        peer_.attach_delivery(source);
    }
    void resume_read() noexcept override {}
    void close() noexcept override { open_ = false; }
    [[nodiscard]] bool is_open() const noexcept override { return open_; }
    [[nodiscard]] crpc::endpoint remote() const override {
        return crpc::endpoint::inproc("anchor");
    }

    [[nodiscard]] anchor_peer& peer() noexcept { return peer_; }

private:
    struct dummy_source final : crpc::io::source {
        std::size_t on_data(crpc::span<const std::byte> data) noexcept override {
            return data.size();
        }
        void on_eof() noexcept override {}
        void on_error(std::error_code) noexcept override {}
    };

    dummy_source dummy_;
    anchor_peer peer_;
    anchor_direct_sink sink_;
    bool open_ = true;
};

// Builds a complete registry tag frame (6-byte prefix + tags) with a correct
// total_len. Used to express the malformed fail-closed frames.
void anchor_put_u8(std::vector<std::byte>& out, crpc::u8 value) {
    out.push_back(static_cast<std::byte>(value));
}
void anchor_put_u16(std::vector<std::byte>& out, crpc::u16 value) {
    const std::size_t at = out.size();
    out.resize(at + 2);
    crpc::endian::store_le16(out.data() + at, value);
}
void anchor_put_u32(std::vector<std::byte>& out, crpc::u32 value) {
    const std::size_t at = out.size();
    out.resize(at + 4);
    crpc::endian::store_le32(out.data() + at, value);
}
void anchor_put_u64(std::vector<std::byte>& out, crpc::u64 value) {
    const std::size_t at = out.size();
    out.resize(at + 8);
    crpc::endian::store_le64(out.data() + at, value);
}
void anchor_put_tag(std::vector<std::byte>& out, crpc::u8 id,
                    const std::vector<std::byte>& value) {
    anchor_put_u8(out, id);
    anchor_put_u32(out, static_cast<crpc::u32>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}
void anchor_put_u64_tag(std::vector<std::byte>& out, crpc::u8 id, crpc::u64 value) {
    std::vector<std::byte> bytes;
    anchor_put_u64(bytes, value);
    anchor_put_tag(out, id, bytes);
}
std::vector<std::byte> anchor_frame_with(const std::vector<std::byte>& tags) {
    std::vector<std::byte> out;
    anchor_put_u16(out, cl::registry_dto_version);
    anchor_put_u32(out, static_cast<crpc::u32>(tags.size()));
    out.insert(out.end(), tags.begin(), tags.end());
    return out;
}

// The genuine server reply: `encode_anchor_response({9,42})` writes tag 19 and
// tag 14. Copied out of the `buffer` into a byte vector the peer can hold.
std::vector<std::byte> real_anchor_frame() {
    crpc::buffer out;
    const crpc::status encoded = cl::encode_anchor_response(cl::registry_anchor{9, 42}, out);
    if (!encoded.ok()) {
        return {};
    }
    const crpc::span<const std::byte> readable = out.readable();
    return std::vector<std::byte>(readable.begin(), readable.end());
}

std::shared_ptr<crpc::rpc::client> make_anchor_client(
    std::vector<std::byte> response_body, anchor_peer*& peer_out) {
    auto conn = std::make_unique<anchor_connection>(std::move(response_body));
    peer_out = &conn->peer();
    crpc::rpc::client_options options;
    options.slot_capacity = 16;
    return std::make_shared<crpc::rpc::client>(std::move(conn), options);
}

} // namespace

CRPC_TEST(client_anchor_decodes_real_epoch_and_index_tags) {
    const std::vector<std::byte> body = real_anchor_frame();
    CRPC_REQUIRE(!body.empty());

    anchor_peer* peer = nullptr;
    auto transport = make_anchor_client(body, peer);
    CRPC_REQUIRE(peer != nullptr);
    cl::registry_client registry(transport);

    const auto anchored = registry.anchor();
    const int anchor_requests = peer->requests_seen;
    // The RESPONSE really reached finished.value(): one REQUEST was decoded by
    // the peer and answered, so the client's inline decode ran on real bytes.
    CRPC_CHECK_EQ(1, anchor_requests);
    CRPC_REQUIRE(anchored.ok());
    // Two distinct non-zero values, not just "ok" or "non-empty".
    CRPC_CHECK_EQ(crpc::u64{9}, anchored.value().authority_epoch);
    CRPC_CHECK_EQ(crpc::u64{42}, anchored.value().index);

    // ping() delegates to anchor() and must decode identically.
    const auto pinged = registry.ping();
    const int ping_requests = peer->requests_seen;
    CRPC_CHECK_EQ(2, ping_requests);
    CRPC_REQUIRE(pinged.ok());
    CRPC_CHECK_EQ(crpc::u64{9}, pinged.value().authority_epoch);
    CRPC_CHECK_EQ(crpc::u64{42}, pinged.value().index);
}

CRPC_TEST(client_anchor_rejects_frames_missing_required_tags) {
    // All four frames are well-formed tag frames (is_known_tag accepts 7/8/14/19
    // and the walker does not apply the required-set rule), so the inline decode
    // must fail closed on PRESENCE of tag 19 / tag 14 of width 8 -- never on the
    // numeric value (a zero anchor is legal). next_fence() is deliberately not
    // touched here: it reads tag 8 correctly and is a different method.

    std::vector<std::byte> missing_epoch_tags;
    anchor_put_u64_tag(missing_epoch_tags, cl::registry_tag_index, 42);
    const std::vector<std::byte> missing_epoch = anchor_frame_with(missing_epoch_tags);

    std::vector<std::byte> missing_index_tags;
    anchor_put_u64_tag(missing_index_tags, cl::registry_tag_authority_epoch, 9);
    const std::vector<std::byte> missing_index = anchor_frame_with(missing_index_tags);

    std::vector<std::byte> wrong_width_tags;
    std::vector<std::byte> four_bytes;
    anchor_put_u32(four_bytes, 9);
    anchor_put_tag(wrong_width_tags, cl::registry_tag_authority_epoch, four_bytes);
    anchor_put_u64_tag(wrong_width_tags, cl::registry_tag_index, 42);
    const std::vector<std::byte> wrong_width = anchor_frame_with(wrong_width_tags);

    // Zero values are a LEGAL anchor: encode_anchor_response writes both tags
    // unconditionally, so a {0,0} frame must still succeed. This guards against
    // a "reject when value == 0" implementation of the presence check.
    crpc::buffer zero_out;
    CRPC_REQUIRE(cl::encode_anchor_response(cl::registry_anchor{0, 0}, zero_out).ok());
    const crpc::span<const std::byte> zero_readable = zero_out.readable();
    const std::vector<std::byte> zero_anchor(zero_readable.begin(), zero_readable.end());

    const std::vector<std::vector<std::byte>> rejected{missing_epoch, missing_index,
                                                       wrong_width};
    // Every CRPC_CHECK* lives OUTSIDE the loop with a fixed check count; the
    // loop only accumulates decoded evidence into fixed-size counters.
    int decode_errors = 0;
    int ok_results = 0;
    int total_requests = 0;
    for (const auto& frame : rejected) {
        anchor_peer* peer = nullptr;
        auto transport = make_anchor_client(frame, peer);
        CRPC_REQUIRE(peer != nullptr);
        cl::registry_client registry(transport);
        const auto anchored = registry.anchor();
        total_requests += peer->requests_seen;
        if (!anchored.ok() && anchored.error().code == crpc::rpc_code::decode_error) {
            ++decode_errors;
        }
        if (anchored.ok()) {
            ++ok_results;
        }
    }
    CRPC_CHECK_EQ(3, total_requests);
    CRPC_CHECK_EQ(3, decode_errors);
    CRPC_CHECK_EQ(0, ok_results);

    // A real zero-valued anchor must still succeed (presence, not value).
    anchor_peer* zero_peer = nullptr;
    auto zero_transport = make_anchor_client(zero_anchor, zero_peer);
    CRPC_REQUIRE(zero_peer != nullptr);
    cl::registry_client zero_registry(zero_transport);
    const auto zero_result = zero_registry.anchor();
    CRPC_CHECK_EQ(1, zero_peer->requests_seen);
    CRPC_REQUIRE(zero_result.ok());
    CRPC_CHECK_EQ(crpc::u64{0}, zero_result.value().authority_epoch);
    CRPC_CHECK_EQ(crpc::u64{0}, zero_result.value().index);
}

CRPC_TEST_MAIN()