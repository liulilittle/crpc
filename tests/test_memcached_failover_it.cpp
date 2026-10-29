// CRPC -- memcached node-health and failover integration tests.
//
// Skipped unless CRPC_MEMCACHED_IT=1. These cases need two real memcached
// instances, because the whole point is that one process disappears while the
// other keeps serving.
//
// Configuration comes from the environment:
//   CRPC_MEMCACHED_IT=1
//   CRPC_MEMCACHED_HOST        (default 127.0.0.1)
//   CRPC_MEMCACHED_PORT        (default 11211)
//   CRPC_MEMCACHED_PORT2       (default 11212)
//   CRPC_MEMCACHED_IT_STOP     shell command that stops the second instance
//   CRPC_MEMCACHED_IT_START    shell command that starts the second instance
//
// STOP/START are deliberately supplied by the operator: a library test cannot
// know whether the second node is local, in WSL, in a container, or on another
// host. When they are absent the destructive case skips; the two-node round
// trip still runs. The destructive case installs a guard that runs START on
// every exit path, so a failed assertion does not leave the node stopped.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "test_framework.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/error.hpp>
#include "test_tcp_backend.hpp"
#include <crpc/memcached/client.hpp>
#include <crpc/memcached/protocol.hpp>
#include <crpc/memcached/router.hpp>

namespace {

using namespace std::chrono_literals;

[[nodiscard]] std::string env_or(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::string(value)
                                                : std::string(fallback);
}

[[nodiscard]] const char* require_it() {
    const char* value = std::getenv("CRPC_MEMCACHED_IT");
    if (value == nullptr || std::strcmp(value, "1") != 0) {
        crpc_test::skip("CRPC_MEMCACHED_IT is not 1");
    }
    return "";
}

[[nodiscard]] std::uint16_t port_from_env(const char* name, const char* fallback) {
    return static_cast<std::uint16_t>(std::stoi(env_or(name, fallback)));
}

struct memcached_rig {
    tcp_backend backend;
    std::unique_ptr<crpc::memcached::router> instance;

    memcached_rig(const std::string& host, std::uint16_t first, std::uint16_t second,
                  crpc::memcached::failover_options failover) {
        std::vector<crpc::memcached::router::server> servers;
        const std::uint16_t ports[] = {first, second};
        for (const std::uint16_t port : ports) {
            crpc::memcached::connection_transport_config config;
            config.pool.max_size = 2;
            config.pool.connection.ep = crpc::endpoint::tcp(host, port);
            config.pool.connection.operation_timeout = 2s;
            const std::string name = host + ":" + std::to_string(port);
            servers.push_back(crpc::memcached::router::server{
                name, std::make_shared<crpc::memcached::connection_transport>(
                          config, backend),
                1});
        }
        instance.reset(new crpc::memcached::router(std::move(servers), {}, failover));
    }

    [[nodiscard]] std::string key_owned_by(std::size_t owner,
                                           const std::string& prefix) const {
        for (int i = 0; i < 20000; ++i) {
            std::string candidate = prefix + std::to_string(i);
            const std::ptrdiff_t current = instance->owner_of(candidate);
            if (current >= 0 && static_cast<std::size_t>(current) == owner) {
                return candidate;
            }
        }
        return {};
    }
};

struct node_guard {
    std::string start_command;
    bool stopped = false;

    ~node_guard() {
        if (stopped && !start_command.empty()) {
            // Best-effort restore; the exit status cannot be acted on in a
            // destructor, but it must be consumed for glibc's
            // warn_unused_result.
            const int rc = std::system(start_command.c_str());
            (void)rc;
        }
    }
};

// Runs `command` and sleeps briefly so a daemonised process can bind its port.
void run_command(const std::string& command) {
    if (!command.empty()) {
        const int rc = std::system(command.c_str());
        (void)rc;
    }
    std::this_thread::sleep_for(400ms);
}

[[nodiscard]] bool has_value(const crpc::result<std::vector<crpc::memcached::value_entry>>& values,
                             const std::string& expected) {
    if (!values.ok()) {
        return false;
    }
    for (const auto& entry : values.value()) {
        if (entry.data == expected) {
            return true;
        }
    }
    return false;
}

} // namespace

CRPC_TEST(memcached_it_two_nodes_route_and_round_trip) {
    (void)require_it();
    const std::string host = env_or("CRPC_MEMCACHED_HOST", "127.0.0.1");
    memcached_rig rig(host, port_from_env("CRPC_MEMCACHED_PORT", "11211"),
                      port_from_env("CRPC_MEMCACHED_PORT2", "11212"),
                      crpc::memcached::failover_options{});

    CRPC_CHECK_EQ(std::size_t{2}, rig.instance->server_count());
    const auto health = rig.instance->health();
    CRPC_CHECK_EQ(std::size_t{2}, health.size());
    for (const auto& node : health) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::memcached::node_condition::alive),
                      static_cast<int>(node.condition));
    }

    // One store/read on each node proves both real instances answer through the
    // ring, not just the first one.
    for (std::size_t owner = 0; owner < 2; ++owner) {
        const std::string key = rig.key_owned_by(owner, "it-roundtrip-" + std::to_string(owner) + "-");
        CRPC_CHECK(!key.empty());
        if (key.empty()) {
            continue;
        }
        const std::string value = "value-" + std::to_string(owner);
        const auto stored = rig.instance->store(crpc::memcached::store_kind::set, key, value);
        CRPC_CHECK(stored.ok());
        if (!stored.ok()) {
            continue;
        }
        CRPC_CHECK(has_value(rig.instance->get({key}), value));
        (void)rig.instance->erase(key);
    }
}

CRPC_TEST(memcached_it_ejects_dead_node_and_recovers_it) {
    (void)require_it();
    const std::string stop_command = env_or("CRPC_MEMCACHED_IT_STOP", "");
    const std::string start_command = env_or("CRPC_MEMCACHED_IT_START", "");
    if (stop_command.empty() || start_command.empty()) {
        crpc_test::skip("CRPC_MEMCACHED_IT_STOP/START are not set");
    }

    const std::string host = env_or("CRPC_MEMCACHED_HOST", "127.0.0.1");
    crpc::memcached::failover_options failover;
    failover.failure_threshold = 1;
    failover.retry_cooldown = 300ms;
    failover.retry_reads = false;
    memcached_rig rig(host, port_from_env("CRPC_MEMCACHED_PORT", "11211"),
                      port_from_env("CRPC_MEMCACHED_PORT2", "11212"), failover);

    // Make sure the second instance is in a known-good state before the case.
    run_command(start_command);

    const std::string key = rig.key_owned_by(1, "it-failover-");
    CRPC_CHECK(!key.empty());
    if (key.empty()) {
        return;
    }
    const auto before = rig.instance->store(crpc::memcached::store_kind::set, key, "before");
    CRPC_CHECK(before.ok());
    if (!before.ok()) {
        return;
    }
    CRPC_CHECK(has_value(rig.instance->get({key}), "before"));

    node_guard guard{start_command, true};
    run_command(stop_command);

    // Wait until the router itself observes the failure: the key is owned by
    // node 2, so the next read fails at the transport level and ejects it.
    bool ejected = false;
    for (int attempt = 0; attempt < 50 && !ejected; ++attempt) {
        const auto values = rig.instance->get({key});
        (void)values;
        ejected = rig.instance->health()[1].condition ==
                  crpc::memcached::node_condition::dead;
        if (!ejected) {
            std::this_thread::sleep_for(100ms);
        }
    }
    CRPC_CHECK(ejected);
    if (!ejected) {
        return;
    }

    const auto dead_health = rig.instance->health();
    CRPC_CHECK_EQ(static_cast<int>(crpc::memcached::node_condition::dead),
                  static_cast<int>(dead_health[1].condition));
    CRPC_CHECK(dead_health[1].consecutive_failures >= 1);
    CRPC_CHECK(dead_health[1].last_error != crpc::rpc_code::ok);

    // Dead nodes leave the ring. The key now maps elsewhere and is a miss
    // there: the default policy deliberately does not read another node.
    const std::ptrdiff_t remapped = rig.instance->owner_of(key);
    CRPC_CHECK(remapped >= 0);
    CRPC_CHECK(remapped != 1);
    std::printf("IT: %s condition=dead failures=%u last_error=%s; key %s remapped "
                "from server 1 to %td\n",
                dead_health[1].name.c_str(),
                static_cast<unsigned>(dead_health[1].consecutive_failures),
                crpc::to_string(dead_health[1].last_error), key.c_str(), remapped);
    const auto miss = rig.instance->get({key});
    CRPC_CHECK(miss.ok());
    if (miss.ok()) {
        CRPC_CHECK(miss.value().empty());
    }

    // Restart the node, let the cooldown expire, and let a public call run the
    // recovery probe. No explicit probe is used here: this exercises the
    // automatic path.
    std::this_thread::sleep_for(400ms);
    run_command(start_command);
    bool recovered = false;
    for (int attempt = 0; attempt < 50 && !recovered; ++attempt) {
        const auto versions = rig.instance->versions();
        (void)versions;
        recovered = rig.instance->health()[1].condition ==
                    crpc::memcached::node_condition::alive;
        if (!recovered) {
            std::this_thread::sleep_for(100ms);
        }
    }
    CRPC_CHECK(recovered);
    guard.stopped = !recovered;
    if (!recovered) {
        return;
    }

    const auto alive_health = rig.instance->health();
    CRPC_CHECK_EQ(static_cast<int>(crpc::memcached::node_condition::alive),
                  static_cast<int>(alive_health[1].condition));
    CRPC_CHECK_EQ(std::uint32_t{0}, alive_health[1].consecutive_failures);
    std::printf("IT: %s condition=alive after restart and cooldown probe\n",
                alive_health[1].name.c_str());

    // The ring includes it again: a fresh write to the same key is readable.
    const auto after = rig.instance->store(crpc::memcached::store_kind::set, key, "after");
    CRPC_CHECK(after.ok());
    if (after.ok()) {
        CRPC_CHECK(has_value(rig.instance->get({key}), "after"));
        (void)rig.instance->erase(key);
    }
}

CRPC_TEST_MAIN()