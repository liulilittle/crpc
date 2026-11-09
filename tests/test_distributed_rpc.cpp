// CRPC -- distributed RPC tests.
//
// These tests combine the pieces a multi-node deployment is built from:
//
//   * the cluster registry as the discovery source (a node registers an
//     endpoint, a caller builds a client from the passing instances);
//   * the client facade's load balancer, retry loop and per-endpoint breaker
//     for failover when a provider disappears;
//   * the health monitor driving the registry state that discovery filters on;
//   * a transport partition (an injected read error) followed by a reconnect
//     and an idempotent replay, so the final state is the same and the side
//     effect happened exactly once;
//   * registry partition recovery (stale incarnation/fence rejected) and
//     watch replay/out-of-order events, which must not duplicate state.
//
// The in-process backend is used on purpose: it is deterministic and it makes
// the "connection is gone" condition observable without a real network. The
// multi-hop push suite lives in test_distributed_push.cpp.
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/client_facade.hpp>
#include <crpc/cluster/health.hpp>
#include <crpc/cluster/registry.hpp>
#include <crpc/cluster/watch.hpp>
#include <crpc/io.hpp>
#include <crpc/resilience/circuit_breaker.hpp>
#include <crpc/resilience/retry.hpp>
#include <crpc/rpc/client.hpp>
#include <crpc/rpc/dispatcher.hpp>
#include <crpc/server.hpp>

using namespace std::chrono_literals;

namespace cl = crpc::cluster;
using crpc::rpc_code;

namespace {

constexpr crpc::u64 k_service = 701;
constexpr crpc::u64 k_who = 1;
constexpr crpc::u64 k_apply = 2;
constexpr crpc::u64 k_dump = 3;
constexpr const char* k_service_name = "distributed.echo";

std::atomic<unsigned> fixture_counter{0};

std::string unique_name(const char* prefix) {
    return std::string(prefix) + "-" +
           std::to_string(fixture_counter.fetch_add(1, std::memory_order_relaxed));
}

// --- fixture -----------------------------------------------------------------

struct node_state {
    std::string id;
    std::atomic<int> who_calls{0};
    std::atomic<int> apply_calls{0};
    std::atomic<int> side_effects{0};

    std::mutex mutex;
    std::map<std::string, std::string> applied;

    // Test-only gate: the handler applies the request and then waits here until
    // the test releases it. That makes "the request was applied, then the
    // connection was lost before the response" a deterministic window instead
    // of a race.
    std::atomic<bool> block_apply{false};
    std::atomic<bool> release_apply{true};

    // The payload is "key|value". The first value for a key wins; a replay of
    // the same key returns the stored value without applying it again. This is
    // the application-level idempotency an at-least-once transport needs.
    std::string apply(const std::string& encoded) {
        apply_calls.fetch_add(1, std::memory_order_relaxed);
        const std::size_t split = encoded.find('|');
        const std::string key = split == std::string::npos
                                    ? encoded
                                    : encoded.substr(0, split);
        const std::string value = split == std::string::npos
                                      ? std::string()
                                      : encoded.substr(split + 1);
        std::string result;
        {
            std::lock_guard<std::mutex> lk(mutex);
            const auto found = applied.find(key);
            if (found != applied.end()) {
                result = found->second;
            } else {
                applied.emplace(key, value);
                side_effects.fetch_add(1, std::memory_order_relaxed);
                result = value;
            }
        }
        if (block_apply.load(std::memory_order_acquire)) {
            while (!release_apply.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
        return result;
    }

    std::string who(const std::string& value) {
        who_calls.fetch_add(1, std::memory_order_relaxed);
        return id + ":" + value;
    }

    std::string dump() {
        std::lock_guard<std::mutex> lk(mutex);
        std::string out;
        for (const auto& entry : applied) {
            out += entry.first;
            out += '=';
            out += entry.second;
            out += '\n';
        }
        return out;
    }
};

struct node {
    crpc::inproc_backend* backend = nullptr;
    node_state* state = nullptr;
    std::unique_ptr<crpc::server> srv;
    std::string name;

    node() = default;
    node(const node&) = delete;
    node& operator=(const node&) = delete;
    ~node() { stop(); }

    bool start(std::size_t shards = 1, std::size_t workers = 4) {
        crpc::server::builder builder;
        builder.add_listener(crpc::endpoint::inproc(name), *backend);
        builder.add_service(k_service, k_service_name);
        builder.add_method<node_state, &node_state::who>(k_service, k_who, "who",
                                                         *state);
        builder.add_method<node_state, &node_state::apply>(
            k_service, k_apply, "apply", *state);
        builder.add_method<node_state, &node_state::dump>(
            k_service, k_dump, "dump", *state);
        builder.shards(shards);
        builder.handler_workers(workers);
        builder.drain_timeout(200ms);
        srv = builder.build();
        if (!srv) {
            return false;
        }
        return srv->start().ok();
    }

    void stop() noexcept {
        if (srv) {
            srv->stop();
        }
    }

    [[nodiscard]] crpc::endpoint ep() const {
        return crpc::endpoint::inproc(name);
    }

    [[nodiscard]] cl::endpoint registry_ep() const {
        cl::endpoint address;
        address.host = name;
        address.port = 0;
        return address;
    }
};

bool register_node(cl::registry& registry, const node& value,
                   crpc::u64 incarnation = 1) {
    cl::service_instance instance;
    instance.service = k_service_name;
    instance.node_id = value.state->id;
    instance.address = value.registry_ep();
    instance.weight = 1;
    instance.incarnation = incarnation;
    instance.state = cl::instance_state::passing;

    // The default config is strict: the token must be issued by the authority
    // for this exact key before the registration can present it.
    const auto token = registry.next_fence(k_service_name, value.state->id);
    if (!token.ok()) {
        return false;
    }
    cl::write_context context;
    context.node_id = value.state->id;
    context.incarnation = incarnation;
    context.fence = token.value();
    context.writer_epoch = 0;  // external writer: not epoch-tracked
    return registry.register_instance(instance, context).ok();
}

bool set_node_state(cl::registry& registry, const node& value,
                    cl::instance_state state, crpc::u64 incarnation = 1) {
    const auto token = registry.next_fence(k_service_name, value.state->id);
    if (!token.ok()) {
        return false;
    }
    cl::write_context context;
    context.node_id = value.state->id;
    context.incarnation = incarnation;
    context.fence = token.value();
    context.writer_epoch = 0;  // external writer: not epoch-tracked
    return registry.set_state(k_service_name, value.state->id, state, context)
        .ok();
}

// --- discovery ---------------------------------------------------------------

struct route_table {
    crpc::u64 index = 0;
    std::vector<std::string> node_ids;
    std::vector<crpc::endpoint> endpoints;
};

route_table discover(cl::registry& registry) {
    cl::query_options options;
    options.service = k_service_name;
    options.passing_only = true;

    route_table table;
    const auto result = registry.query(options);
    if (!result.ok()) {
        return table;
    }
    table.index = result.value().index;
    table.endpoints.reserve(result.value().instances.size());
    table.node_ids.reserve(result.value().instances.size());
    for (const cl::service_instance& instance : result.value().instances) {
        table.node_ids.push_back(instance.node_id);
        table.endpoints.push_back(crpc::endpoint::inproc(instance.address.host));
    }
    return table;
}

std::unique_ptr<crpc::client> build_client(const route_table& table,
                                           crpc::inproc_backend& backend,
                                           bool resilient) {
    crpc::client::builder builder;
    for (const crpc::endpoint& ep : table.endpoints) {
        builder.add_endpoint(ep, backend);
    }
    builder.pool_size(table.endpoints.empty() ? 1 : table.endpoints.size());
    builder.max_inflight_per_connection(4096);
    builder.default_deadline(2s);
    builder.plain_thread_timeout(2s);
    builder.default_idempotent(true);

    crpc::resilience::retry_policy retry;
    retry.max_attempts = resilient ? 3u : 1u;
    retry.initial_backoff = 1ms;
    retry.max_backoff = 5ms;
    retry.retry_budget = 10.0;
    builder.retry(retry);

    crpc::resilience::breaker_config breaker;
    breaker.min_requests = 1;
    breaker.failure_ratio_threshold = 0.5;
    breaker.slow_call_threshold = 10s;
    breaker.slow_call_ratio_threshold = 1.0;
    breaker.open_duration = 5s;
    builder.breaker(breaker);
    builder.resilience_enabled(resilient);
    return builder.build();
}

// --- calls -------------------------------------------------------------------

bool who_call(crpc::client& client, const std::string& value, std::string& out,
              rpc_code& code) {
    auto stub = client.service<std::string, std::string>(k_service, k_who);
    auto started = stub.call(value);
    if (!started.ok()) {
        code = started.code();
        return false;
    }
    auto handle = std::move(started).value();
    auto result = handle.get();
    if (!result.ok()) {
        code = result.code();
        return false;
    }
    out = std::move(result).value();
    code = rpc_code::ok;
    return true;
}

std::string node_of(const std::string& reply) {
    const std::size_t split = reply.find(':');
    return split == std::string::npos ? reply : reply.substr(0, split);
}

bool apply_call(crpc::rpc::client& client, const std::string& request,
                std::string& out, rpc_code& code) {
    crpc::buffer body;
    const crpc::status encoded =
        crpc::rpc::call_codec<std::string>::encode(request, body);
    if (!encoded.ok()) {
        code = encoded.code();
        return false;
    }
    auto started = client.start_call<std::string>(k_service, k_apply,
                                                  body.readable());
    if (!started.ok()) {
        code = started.code();
        return false;
    }
    auto handle = std::move(started).value();
    auto result = handle.get();
    if (!result.ok()) {
        code = result.code();
        return false;
    }
    out = std::move(result).value();
    code = rpc_code::ok;
    return true;
}

bool dump_call(crpc::rpc::client& client, std::string& out) {
    crpc::buffer body;
    (void)crpc::rpc::call_codec<std::string>::encode(std::string(), body);
    auto started = client.start_call<std::string>(k_service, k_dump,
                                                  body.readable());
    if (!started.ok()) {
        return false;
    }
    auto handle = std::move(started).value();
    auto result = handle.get();
    if (!result.ok()) {
        return false;
    }
    out = std::move(result).value();
    return true;
}

std::unique_ptr<crpc::rpc::client> raw_client(crpc::inproc_backend& backend,
                                              const crpc::endpoint& ep) {
    auto connection = backend.connect(ep, crpc::connect_options{});
    if (!connection) {
        return nullptr;
    }
    crpc::rpc::client_options options;
    options.slot_capacity = 16;
    options.default_timeout = 2s;
    return std::make_unique<crpc::rpc::client>(std::move(connection), options);
}

// --- health probe ------------------------------------------------------------

struct probe_flag {
    std::atomic<bool> pass{true};
};

bool flag_probe(void* context) noexcept {
    return static_cast<probe_flag*>(context)->pass.load(
        std::memory_order_acquire);
}

struct manual_clock {
    cl::monotonic_clock::time_point now{};

    void advance(std::chrono::milliseconds delta) { now += delta; }
};

cl::monotonic_clock::time_point manual_now(void* context) noexcept {
    return static_cast<manual_clock*>(context)->now;
}

cl::monotonic_clock make_clock(manual_clock& clock) noexcept {
    return cl::monotonic_clock{&manual_now, &clock};
}

// --- watch capture -----------------------------------------------------------

struct watch_capture {
    std::size_t snapshots = 0;
    std::size_t snapshot_instances = 0;
    std::size_t deltas = 0;
    std::size_t resyncs = 0;
    std::vector<crpc::u64> indexes;
};

void capture_event(void* context, const cl::watch_event& event) noexcept {
    auto* capture = static_cast<watch_capture*>(context);
    capture->indexes.push_back(event.index);
    switch (event.kind) {
    case cl::watch_event_kind::snapshot:
        ++capture->snapshots;
        if (event.snapshot != nullptr) {
            capture->snapshot_instances += event.snapshot->size();
        }
        break;
    case cl::watch_event_kind::delta:
        ++capture->deltas;
        break;
    case cl::watch_event_kind::resync:
        ++capture->resyncs;
        if (event.snapshot != nullptr) {
            capture->snapshot_instances += event.snapshot->size();
        }
        break;
    case cl::watch_event_kind::dropped:
        break;
    }
}

} // namespace

// --- discovery and routing convergence ---------------------------------------

CRPC_TEST(distributed_discovery_and_route_convergence) {
    crpc::inproc_backend backend;
    cl::registry registry;

    node_state states[4];
    node nodes[4];
    for (int i = 0; i < 4; ++i) {
        states[i].id = "node-" + std::to_string(i);
        nodes[i].backend = &backend;
        nodes[i].state = &states[i];
        nodes[i].name = unique_name("dist-disc");
        CRPC_CHECK(nodes[i].start());
        if (i < 3) {
            CRPC_CHECK(register_node(registry, nodes[i]));
        }
    }

    // Three providers are discovered; a client built from the snapshot reaches
    // every one of them through the facade's round-robin balancer.
    const route_table initial = discover(registry);
    CRPC_CHECK_EQ(std::size_t{3}, initial.endpoints.size());
    auto client = build_client(initial, backend, false);
    CRPC_CHECK(client != nullptr);
    if (!client) {
        return;
    }

    std::set<std::string> seen;
    int failures = 0;
    for (int i = 0; i < 30; ++i) {
        std::string reply;
        rpc_code code = rpc_code::ok;
        if (!who_call(*client, "v" + std::to_string(i), reply, code)) {
            ++failures;
            continue;
        }
        seen.insert(node_of(reply));
    }
    CRPC_CHECK_EQ(0, failures);
    CRPC_CHECK_EQ(std::size_t{3}, seen.size());

    // A node joins. The old route table cannot know it (that is the red side of
    // the convergence check); a fresh discovery includes it (green).
    CRPC_CHECK(register_node(registry, nodes[3]));
    for (int i = 0; i < 30; ++i) {
        std::string reply;
        rpc_code code = rpc_code::ok;
        CRPC_CHECK(who_call(*client, "old", reply, code));
        CRPC_CHECK(node_of(reply) != states[3].id);
    }

    const route_table joined = discover(registry);
    CRPC_CHECK_EQ(std::size_t{4}, joined.endpoints.size());
    auto joined_client = build_client(joined, backend, false);
    CRPC_CHECK(joined_client != nullptr);
    if (!joined_client) {
        return;
    }

    seen.clear();
    failures = 0;
    for (int i = 0; i < 40; ++i) {
        std::string reply;
        rpc_code code = rpc_code::ok;
        if (!who_call(*joined_client, "v" + std::to_string(i), reply, code)) {
            ++failures;
            continue;
        }
        seen.insert(node_of(reply));
    }
    CRPC_CHECK_EQ(0, failures);
    CRPC_CHECK_EQ(std::size_t{4}, seen.size());
    CRPC_CHECK(seen.count(states[3].id) == 1);

    // A provider leaves. The stale table still contains it, so calls that pick
    // it fail; the refreshed table excludes it and every call succeeds.
    nodes[2].stop();
    {
        // The deregistration is the node's own authority write, so it needs a
        // token issued for its key.
        const auto token = registry.next_fence(k_service_name, states[2].id);
        CRPC_REQUIRE(token.ok());
        cl::write_context context;
        context.node_id = states[2].id;
        context.incarnation = 1;
        context.fence = token.value();
        context.writer_epoch = 0;  // external writer: not epoch-tracked
        CRPC_CHECK(registry
                       .deregister(k_service_name, states[2].id, context)
                       .ok());
    }

    int stale_failures = 0;
    for (int i = 0; i < 60; ++i) {
        std::string reply;
        rpc_code code = rpc_code::ok;
        if (!who_call(*joined_client, "stale", reply, code)) {
            ++stale_failures;
        }
    }
    CRPC_CHECK(stale_failures > 0);

    const route_table converged = discover(registry);
    CRPC_CHECK_EQ(std::size_t{3}, converged.endpoints.size());
    auto converged_client = build_client(converged, backend, false);
    CRPC_CHECK(converged_client != nullptr);
    if (!converged_client) {
        return;
    }

    failures = 0;
    seen.clear();
    for (int i = 0; i < 60; ++i) {
        std::string reply;
        rpc_code code = rpc_code::ok;
        if (!who_call(*converged_client, "ok", reply, code)) {
            ++failures;
            continue;
        }
        seen.insert(node_of(reply));
    }
    CRPC_CHECK_EQ(0, failures);
    CRPC_CHECK_EQ(std::size_t{3}, seen.size());
    CRPC_CHECK(seen.count(states[2].id) == 0);

    converged_client->close();
    joined_client->close();
    client->close();
}

// --- failover ----------------------------------------------------------------

CRPC_TEST(distributed_failover_to_live_provider) {
    crpc::inproc_backend backend;
    cl::registry registry;

    node_state states[2];
    node nodes[2];
    for (int i = 0; i < 2; ++i) {
        states[i].id = "failover-" + std::to_string(i);
        nodes[i].backend = &backend;
        nodes[i].state = &states[i];
        nodes[i].name = unique_name("dist-failover");
        CRPC_CHECK(nodes[i].start());
        CRPC_CHECK(register_node(registry, nodes[i]));
    }

    const route_table table = discover(registry);
    CRPC_CHECK_EQ(std::size_t{2}, table.endpoints.size());
    auto client = build_client(table, backend, true);
    CRPC_CHECK(client != nullptr);
    if (!client) {
        return;
    }

    // The provider is gone but the registry has not been told yet. The facade
    // must retry the pre-send failure against the other endpoint.
    nodes[0].stop();

    int failures = 0;
    std::set<std::string> answered;
    for (int i = 0; i < 20; ++i) {
        std::string reply;
        rpc_code code = rpc_code::ok;
        if (!who_call(*client, "v" + std::to_string(i), reply, code)) {
            ++failures;
            continue;
        }
        answered.insert(node_of(reply));
    }

    CRPC_CHECK_EQ(0, failures);
    CRPC_CHECK_EQ(std::size_t{1}, answered.size());
    CRPC_CHECK(answered.count(states[1].id) == 1);
    CRPC_CHECK_EQ(0, states[0].who_calls.load());
    CRPC_CHECK_EQ(20, states[1].who_calls.load());

    const crpc::client::resilience_statistics stats =
        client->resilience_stats();
    CRPC_CHECK(stats.pre_send_retries >= 1 || stats.endpoint_open >= 1);

    client->close();
}

// --- health-driven discovery -------------------------------------------------

CRPC_TEST(distributed_health_state_evicts_and_restores_provider) {
    crpc::inproc_backend backend;
    cl::registry registry;

    node_state states[2];
    node nodes[2];
    for (int i = 0; i < 2; ++i) {
        states[i].id = "health-" + std::to_string(i);
        nodes[i].backend = &backend;
        nodes[i].state = &states[i];
        nodes[i].name = unique_name("dist-health");
        CRPC_CHECK(nodes[i].start());
        CRPC_CHECK(register_node(registry, nodes[i]));
    }

    manual_clock clock;
    cl::health_monitor monitor(make_clock(clock));
    probe_flag first_probe;
    CRPC_CHECK(monitor.add_callback("first", &flag_probe, &first_probe, 0ms) !=
               nullptr);

    CRPC_CHECK(monitor.poll() == cl::health_state::passing);

    // The probe fails: the monitor turns critical immediately (failure
    // threshold 1), and the registry state that discovery filters on follows.
    first_probe.pass.store(false, std::memory_order_release);
    CRPC_CHECK(monitor.poll() == cl::health_state::critical);
    CRPC_CHECK(set_node_state(registry, nodes[0],
                              cl::to_instance_state(monitor.state())));

    const route_table evicted = discover(registry);
    CRPC_CHECK_EQ(std::size_t{1}, evicted.endpoints.size());
    if (!evicted.node_ids.empty()) {
        CRPC_CHECK_EQ(states[1].id, evicted.node_ids[0]);
    }

    // Recovery needs two consecutive passes (the monitor's flap guard), then
    // the provider is discoverable again.
    first_probe.pass.store(true, std::memory_order_release);
    CRPC_CHECK(monitor.poll() == cl::health_state::critical);
    CRPC_CHECK(monitor.poll() == cl::health_state::passing);
    CRPC_CHECK(set_node_state(registry, nodes[0],
                              cl::to_instance_state(monitor.state())));

    const route_table restored = discover(registry);
    CRPC_CHECK_EQ(std::size_t{2}, restored.endpoints.size());
}

// --- partition, reconnect, idempotent replay ---------------------------------

CRPC_TEST(distributed_partition_replay_has_one_side_effect) {
    crpc::inproc_backend backend;

    node_state state;
    state.id = "partition-0";
    node provider;
    provider.backend = &backend;
    provider.state = &state;
    provider.name = unique_name("dist-partition");
    CRPC_CHECK(provider.start());
    if (!provider.srv) {
        return;
    }

    // First attempt: the request is delivered and applied, then the caller's
    // connection is closed before the response can be read (a partition). The
    // handler gate makes that window deterministic.
    auto partitioned = raw_client(backend, provider.ep());
    CRPC_CHECK(partitioned != nullptr);
    if (!partitioned) {
        return;
    }

    state.block_apply.store(true, std::memory_order_release);
    state.release_apply.store(false, std::memory_order_release);

    bool first_ok = true;
    rpc_code code = rpc_code::ok;
    std::string value;
    std::thread caller([&] {
        first_ok = apply_call(*partitioned, "req-1|v1", value, code);
    });

    const bool applied = crpc_test::wait_for(
        [&state] {
            return state.side_effects.load(std::memory_order_acquire) == 1;
        },
        2000);
    if (!applied) {
        state.release_apply.store(true, std::memory_order_release);
        caller.join();
        CRPC_CHECK(applied);
        return;
    }

    partitioned->close();
    state.release_apply.store(true, std::memory_order_release);
    caller.join();
    CRPC_CHECK_FALSE(first_ok);
    CRPC_CHECK_EQ(1, state.side_effects.load(std::memory_order_acquire));

    // The partition heals: a fresh connection replays the same request. The
    // handler deduplicates it, so the state still has one side effect.
    auto healed = raw_client(backend, provider.ep());
    CRPC_CHECK(healed != nullptr);
    if (!healed) {
        return;
    }

    std::string replay_value;
    CRPC_CHECK(apply_call(*healed, "req-1|v1", replay_value, code));
    CRPC_CHECK_EQ(std::string("v1"), replay_value);
    CRPC_CHECK_EQ(1, state.side_effects.load(std::memory_order_acquire));

    // An out-of-order duplicate of an older key must also not re-apply it.
    std::string second_value;
    CRPC_CHECK(apply_call(*healed, "req-2|v2", second_value, code));
    CRPC_CHECK_EQ(std::string("v2"), second_value);
    std::string duplicate_value;
    CRPC_CHECK(apply_call(*healed, "req-1|v1", duplicate_value, code));
    CRPC_CHECK_EQ(std::string("v1"), duplicate_value);
    CRPC_CHECK(apply_call(*healed, "req-2|v2", duplicate_value, code));
    CRPC_CHECK_EQ(std::string("v2"), duplicate_value);
    CRPC_CHECK_EQ(2, state.side_effects.load(std::memory_order_acquire));

    std::string final_state;
    CRPC_CHECK(dump_call(*healed, final_state));
    CRPC_CHECK_EQ(std::string("req-1=v1\nreq-2=v2\n"), final_state);
}

// --- registry partition recovery and watch replay ----------------------------

CRPC_TEST(distributed_registry_partition_and_watch_replay) {
    cl::registry registry;

    node_state states[3];
    node nodes[3];
    crpc::inproc_backend backend;
    for (int i = 0; i < 3; ++i) {
        states[i].id = "reg-" + std::to_string(i);
        nodes[i].backend = &backend;
        nodes[i].state = &states[i];
        nodes[i].name = unique_name("dist-reg");
        // The servers are not started here: this test exercises the catalog
        // authority and the watch stream, not RPC.
        CRPC_CHECK(register_node(registry, nodes[i]));
    }

    // Node 0 restarted with a higher incarnation. Node 1 was updated by the
    // authority while it was partitioned (its old endpoint is saved so the
    // stale re-registration can be attempted explicitly).
    const std::string partitioned_endpoint = nodes[1].name;
    nodes[0].name = unique_name("dist-reg-restarted");
    CRPC_CHECK(register_node(registry, nodes[0], 2));
    nodes[1].name = unique_name("dist-reg-authority");
    CRPC_CHECK(register_node(registry, nodes[1], 2));

    // The partitioned node comes back with its old incarnation: the authority
    // rejects the stale write instead of letting it overwrite the newer one.
    // The token is issued first so the rejection is provably the incarnation
    // rule and not a missing fence.
    cl::service_instance stale_instance;
    stale_instance.service = k_service_name;
    stale_instance.node_id = states[1].id;
    stale_instance.address.host = partitioned_endpoint;
    stale_instance.address.port = 0;
    stale_instance.incarnation = 1;
    stale_instance.state = cl::instance_state::passing;

    const auto stale_token = registry.next_fence(k_service_name, states[1].id);
    CRPC_REQUIRE(stale_token.ok());
    cl::write_context stale_context;
    stale_context.node_id = states[1].id;
    stale_context.incarnation = 1;
    stale_context.fence = stale_token.value();
    stale_context.writer_epoch = 0;  // external writer: not epoch-tracked
    const auto stale =
        registry.register_instance(stale_instance, stale_context);
    CRPC_CHECK_FALSE(stale.ok());
    CRPC_CHECK(stale.code() == rpc_code::failed_precondition);

    // Rejoining with a newer incarnation converges to exactly one instance per
    // node, with the newest endpoint for the nodes that restarted.
    nodes[1].name = unique_name("dist-reg-rejoined");
    CRPC_CHECK(register_node(registry, nodes[1], 3));

    // The strict replay default rejects a second write for the same
    // (key, incarnation) with a different endpoint: an endpoint change must
    // bump the incarnation, not silently overwrite.
    cl::service_instance conflicting;
    conflicting.service = k_service_name;
    conflicting.node_id = states[1].id;
    conflicting.address.host = partitioned_endpoint;
    conflicting.address.port = 0;
    conflicting.incarnation = 3;
    conflicting.state = cl::instance_state::passing;
    const auto conflict_token = registry.next_fence(k_service_name, states[1].id);
    CRPC_REQUIRE(conflict_token.ok());
    cl::write_context conflicting_context;
    conflicting_context.node_id = states[1].id;
    conflicting_context.incarnation = 3;
    conflicting_context.fence = conflict_token.value();
    conflicting_context.writer_epoch = 0;  // external writer: not epoch-tracked
    const auto conflicting_write =
        registry.register_instance(conflicting, conflicting_context);
    CRPC_CHECK_FALSE(conflicting_write.ok());
    CRPC_CHECK(conflicting_write.code() == rpc_code::failed_precondition);
    const std::string* conflict_reason =
        conflicting_write.error().metadata.find("registry.reason");
    CRPC_CHECK(conflict_reason != nullptr && *conflict_reason == "conflict");

    const route_table table = discover(registry);
    CRPC_CHECK_EQ(std::size_t{3}, table.endpoints.size());
    CRPC_CHECK_EQ(std::size_t{3}, table.node_ids.size());
    if (table.node_ids.size() == 3) {
        // The query is sorted by (service, node_id); check the endpoint that
        // belongs to each id rather than the ordering of the vector.
        for (std::size_t i = 0; i < table.node_ids.size(); ++i) {
            const std::string expected =
                table.node_ids[i] == states[0].id
                    ? nodes[0].name
                    : (table.node_ids[i] == states[1].id ? nodes[1].name
                                                         : nodes[2].name);
            CRPC_CHECK_EQ(expected, table.endpoints[i].name());
        }
    }

    // Replay and out-of-order events: a duplicate index and an older index are
    // dropped, so the watch never applies the same delta twice.
    const auto snapshot_result = registry.query([] {
        cl::query_options options;
        options.service = k_service_name;
        options.passing_only = true;
        return options;
    }());
    CRPC_CHECK(snapshot_result.ok());
    if (!snapshot_result.ok()) {
        return;
    }

    watch_capture capture;
    cl::watch_stream stream(k_service_name, snapshot_result.value().index,
                            snapshot_result.value().instances, &capture_event,
                            &capture);
    CRPC_CHECK_EQ(std::size_t{1}, stream.deliver_initial());
    CRPC_CHECK_EQ(std::size_t{1}, capture.snapshots);
    CRPC_CHECK_EQ(std::size_t{3}, capture.snapshot_instances);

    cl::service_instance first = snapshot_result.value().instances[0];
    first.state = cl::instance_state::warning;
    cl::service_instance second = snapshot_result.value().instances[1];
    second.state = cl::instance_state::critical;

    const crpc::u64 base = snapshot_result.value().index;
    stream.on_event(base + 1, k_service_name, first);
    stream.on_event(base + 1, k_service_name, first); // duplicate
    stream.on_event(base, k_service_name, first);     // older than last
    stream.on_event(base + 2, k_service_name, second);
    CRPC_CHECK_EQ(std::size_t{2}, stream.dispatch());
    CRPC_CHECK_EQ(std::size_t{2}, capture.deltas);
    CRPC_CHECK_EQ(base + 2, stream.last_index());

    // A resync installs the authoritative snapshot; the final view has one
    // entry per node and no duplicated delta was retained.
    std::vector<cl::service_instance> fresh = snapshot_result.value().instances;
    CRPC_CHECK_EQ(std::size_t{1},
                  stream.apply_resync(base + 2, std::move(fresh)));
    CRPC_CHECK_EQ(std::size_t{1}, capture.resyncs);
    CRPC_CHECK_EQ(std::size_t{6}, capture.snapshot_instances);
}

CRPC_TEST_MAIN()
