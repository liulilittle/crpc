// CRPC microservice example.
//
// A self-contained service mesh in one process:
//
//   * two provider servers register themselves in the built-in registry,
//   * a health monitor aggregates per-instance probes,
//   * a consumer discovers the service through the registry and load-balances
//     across the healthy instances,
//   * every call is wrapped in a circuit breaker and a retry policy.
//
// The in-process backend keeps the whole mesh deterministic: no sockets, no
// extra threads. Exit code 0 means discovery, load balancing, health
// aggregation, the breaker and the retry policy all behaved as expected.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <crpc/cluster/health.hpp>
#include <crpc/cluster/registry.hpp>
#include <crpc/io.hpp>
#include <crpc/resilience/circuit_breaker.hpp>
#include <crpc/resilience/load_balancer.hpp>
#include <crpc/resilience/retry.hpp>
#include <crpc/rpc/call.hpp>
#include <crpc/rpc/client.hpp>
#include <crpc/server.hpp>

namespace {

namespace cl = crpc::cluster;
namespace res = crpc::resilience;

constexpr crpc::u64 kGreeterService = 10;
constexpr crpc::u64 kGreetMethod = 1;
constexpr const char* kServiceName = "greeter";

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::cerr << "microservice: FAILED: " << what << "\n";
        ++failures;
    }
}

// --- Provider ----------------------------------------------------------------

class greeter {
public:
    explicit greeter(std::string node) : node_(std::move(node)) {}

    std::string greet(const std::string& name) {
        return "Hello, " + name + " from " + node_;
    }

private:
    std::string node_;
};

// --- Registry catalog --------------------------------------------------------

cl::service_instance make_instance(const std::string& node, crpc::u16 port) {
    cl::service_instance instance;
    instance.service = kServiceName;
    instance.node_id = node;
    instance.address.host = "127.0.0.1";
    instance.address.port = port;
    instance.weight = 1;
    instance.state = cl::instance_state::passing;
    return instance;
}

cl::write_context make_context(const std::string& node) {
    cl::write_context context;
    context.node_id = node;
    return context;
}

// A read-only view over the registry's catalog result, used by the load
// balancer. The version is bumped whenever the set is rebuilt.
class catalog_set final : public res::instance_set {
public:
    std::vector<res::instance> items;
    crpc::u64 version_value = 1;

    void rebuild(const cl::catalog_result& catalog) {
        items.clear();
        crpc::u64 id = 1;
        for (const cl::service_instance& instance : catalog.instances) {
            res::instance candidate;
            candidate.id = id++;
            candidate.weight = instance.weight;
            candidate.healthy = instance.state == cl::instance_state::passing;
            items.push_back(candidate);
        }
        ++version_value;
    }

    [[nodiscard]] std::size_t size() const noexcept override {
        return items.size();
    }
    [[nodiscard]] res::instance at(std::size_t index) const noexcept override {
        return items[index];
    }
    [[nodiscard]] crpc::u64 version() const noexcept override {
        return version_value;
    }
};

// --- Health ------------------------------------------------------------------

struct probe_state {
    bool pass = true;
};

bool probe_callback(void* context) noexcept {
    return static_cast<probe_state*>(context)->pass;
}

// --- Call path ---------------------------------------------------------------

// One typed call to a provider. Returns 0 on success and -1 on any failure.
int call_greeter(crpc::inproc_backend& backend, const std::string& endpoint_name,
                 const std::string& name, std::string& out) {
    std::unique_ptr<crpc::connection> connection =
        backend.connect(crpc::endpoint::inproc(endpoint_name),
                        crpc::connect_options{});
    if (!connection) {
        return -1;
    }
    crpc::rpc::client client(std::move(connection), crpc::rpc::client_options{});

    crpc::buffer request;
    if (const crpc::status encoded =
            crpc::rpc::call_codec<std::string>::encode(name, request);
        !encoded.ok()) {
        client.close();
        return -1;
    }
    auto started = client.start_call<std::string>(
        kGreeterService, kGreetMethod, request.readable());
    if (!started.ok()) {
        client.close();
        return -1;
    }
    auto result = std::move(started).value().get();
    client.close();
    if (!result.ok()) {
        return -1;
    }
    out = result.value();
    return 0;
}

// One logical call wrapped in the circuit breaker. The retry policy is applied
// around the whole breaker-guarded attempt.
bool call_with_resilience(crpc::inproc_backend& backend,
                          const std::string& endpoint_name, const std::string& name,
                          std::string& out, res::circuit_breaker& breaker) {
    res::retry_policy policy;
    policy.max_attempts = 3;
    policy.initial_backoff = std::chrono::milliseconds(1);
    policy.max_backoff = std::chrono::milliseconds(5);
    policy.jitter_ratio = 0.0;
    res::retry_sequence sequence(policy);

    for (;;) {
        res::breaker_permit permit = breaker.allow();
        if (!permit.allowed()) {
            return false;
        }
        if (call_greeter(backend, endpoint_name, name, out) == 0) {
            permit.success();
            return true;
        }
        permit.failure();

        std::chrono::nanoseconds delay{0};
        if (!sequence.next(crpc::rpc_code::unavailable, true, delay)) {
            return false;
        }
    }
}

} // namespace

int main() {
    // --- registry: two provider instances ------------------------------------
    cl::registry registry;
    check(registry.register_instance(make_instance("greeter-1", 8001),
                                     make_context("greeter-1"))
              .ok(),
          "register greeter-1");
    check(registry.register_instance(make_instance("greeter-2", 8002),
                                     make_context("greeter-2"))
              .ok(),
          "register greeter-2");

    cl::query_options query;
    query.service = kServiceName;
    query.passing_only = true;
    const auto catalog = registry.query(query);
    check(catalog.ok(), "query greeter catalog");
    if (!catalog.ok()) {
        return 1;
    }
    check(catalog.value().instances.size() == 2, "two healthy instances");
    std::cout << "microservice: registry has " << catalog.value().instances.size()
              << " passing instance(s) of '" << kServiceName << "'\n";

    // --- health: worst-of aggregation over two probes -------------------------
    cl::health_monitor monitor(cl::system_monotonic_clock());
    probe_state healthy;
    healthy.pass = true;
    probe_state sick;
    sick.pass = false;
    check(monitor.add_callback("greeter-1", &probe_callback, &healthy,
                               std::chrono::milliseconds(0)) != nullptr,
          "add healthy probe");
    check(monitor.add_callback("greeter-2", &probe_callback, &sick,
                               std::chrono::milliseconds(0)) != nullptr,
          "add sick probe");
    check(monitor.poll() == cl::health_state::critical,
          "worst-of aggregation reports critical");
    sick.pass = true;
    check(monitor.poll() == cl::health_state::critical,
          "recovery needs consecutive passes");
    check(monitor.poll() == cl::health_state::passing,
          "recovery after both probes pass");
    std::cout << "microservice: health aggregate is passing\n";

    // --- load balancing: round-robin over the discovered set -----------------
    catalog_set set;
    set.rebuild(catalog.value());
    res::load_balancer balancer(res::lb_strategy::round_robin);

    std::vector<crpc::u64> picks;
    for (int i = 0; i < 4; ++i) {
        const auto selected = balancer.select(set);
        check(selected.ok(), "load balancer selection");
        if (!selected.ok()) {
            return 1;
        }
        picks.push_back(selected.value().id);
    }
    check(picks[0] != picks[1], "round-robin alternates between instances");
    std::cout << "microservice: round-robin picked";
    for (const crpc::u64 id : picks) {
        std::cout << " " << id;
    }
    std::cout << "\n";

    // --- providers: two servers on two in-process endpoints ------------------
    crpc::inproc_backend backend;

    greeter provider1("greeter-1");
    greeter provider2("greeter-2");

    crpc::server::builder provider1_builder;
    provider1_builder.add_listener(crpc::endpoint::inproc("greeter-1"), backend);
    provider1_builder.add_service(kGreeterService, kServiceName);
    provider1_builder.add_method<greeter, &greeter::greet>(
        kGreeterService, kGreetMethod, "greet", provider1);

    crpc::server::builder provider2_builder;
    provider2_builder.add_listener(crpc::endpoint::inproc("greeter-2"), backend);
    provider2_builder.add_service(kGreeterService, kServiceName);
    provider2_builder.add_method<greeter, &greeter::greet>(
        kGreeterService, kGreetMethod, "greet", provider2);

    std::unique_ptr<crpc::server> provider1_server = provider1_builder.build();
    std::unique_ptr<crpc::server> provider2_server = provider2_builder.build();
    if (!provider1_server || !provider2_server) {
        std::cerr << "microservice: build provider failed\n";
        return 1;
    }
    if (const crpc::status started = provider1_server->start(); !started.ok()) {
        std::cerr << "microservice: start provider1 failed: "
                  << started.message() << "\n";
        return 1;
    }
    if (const crpc::status started = provider2_server->start(); !started.ok()) {
        std::cerr << "microservice: start provider2 failed: "
                  << started.message() << "\n";
        return 1;
    }

    // --- consumer: call each discovered instance through breaker + retry ------
    res::breaker_config breaker_config;
    breaker_config.min_requests = 1;
    breaker_config.failure_ratio_threshold = 0.5;
    breaker_config.open_duration = std::chrono::seconds(60);
    res::manual_clock breaker_clock;
    res::circuit_breaker breaker(breaker_config, breaker_clock.source());

    const std::array<std::string, 2> endpoints = {"greeter-1", "greeter-2"};
    for (const std::string& endpoint_name : endpoints) {
        std::string response;
        check(call_with_resilience(backend, endpoint_name, "CRPC", response,
                                   breaker),
              "resilient call succeeds");
        check(response.find("from " + endpoint_name) != std::string::npos,
              "response names the selected provider");
        std::cout << "microservice: " << endpoint_name << " -> " << response
                  << "\n";
    }
    check(breaker.state() == res::breaker_state::closed,
          "breaker stays closed while calls succeed");

    // --- breaker: repeated failures open it -----------------------------------
    for (int i = 0; i < 4; ++i) {
        std::string ignored;
        (void)call_with_resilience(backend, "greeter-missing", "CRPC", ignored,
                                   breaker);
    }
    check(breaker.state() == res::breaker_state::open,
          "breaker opens after repeated failures");
    std::cout << "microservice: breaker opened after failures\n";

    // --- teardown -------------------------------------------------------------
    provider1_server->stop();
    provider2_server->stop();

    if (failures != 0) {
        std::cerr << "microservice: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "microservice OK\n";
    return 0;
}
