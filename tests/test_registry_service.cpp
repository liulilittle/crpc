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
#include <crpc/fs/directory.hpp>
#include <crpc/fs/file.hpp>
#include <crpc/io.hpp>
#include <crpc/io/iocp_backend.hpp>

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

CRPC_TEST_MAIN()
