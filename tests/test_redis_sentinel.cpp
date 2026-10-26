// CRPC -- Redis Sentinel tests.
//
// The transport is replaced by an in-memory executor so the tests can drive
// failure detection, quorum voting, leader election and promotion without a
// real network. The state machine itself is production code.
#include "test_framework.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <map>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <crpc/io.hpp>
#include <crpc/redis/connection.hpp>
#include <crpc/redis/sentinel.hpp>

namespace {

using crpc::redis::sentinel;
using crpc::redis::sentinel_address;
using crpc::redis::sentinel_event;
using crpc::redis::sentinel_event_type;
using crpc::redis::sentinel_health;
using crpc::redis::sentinel_options;
using crpc::redis::sentinel_reply;
using crpc::redis::sentinel_reply_type;

using steady_clock = std::chrono::steady_clock;

sentinel_reply make_status(std::string text) {
    sentinel_reply reply;
    reply.type = sentinel_reply_type::status;
    reply.text = std::move(text);
    return reply;
}

sentinel_reply make_bulk(std::string text) {
    sentinel_reply reply;
    reply.type = sentinel_reply_type::bulk;
    reply.text = std::move(text);
    return reply;
}

sentinel_reply make_integer(std::int64_t value) {
    sentinel_reply reply;
    reply.type = sentinel_reply_type::integer;
    reply.integer = value;
    return reply;
}

sentinel_reply make_array(std::vector<sentinel_reply> elements) {
    sentinel_reply reply;
    reply.type = sentinel_reply_type::array;
    reply.elements = std::move(elements);
    return reply;
}

sentinel_reply make_null() {
    return sentinel_reply{};
}

sentinel_reply make_error(std::string text) {
    sentinel_reply reply;
    reply.type = sentinel_reply_type::error;
    reply.text = std::move(text);
    return reply;
}

sentinel_reply make_transport_error() {
    sentinel_reply reply = make_error("connection refused");
    reply.transport_failure = true;
    return reply;
}

sentinel_reply make_map(
    std::initializer_list<std::pair<std::string, std::string>> fields) {
    std::vector<sentinel_reply> elements;
    elements.reserve(fields.size() * 2);
    for (const auto& field : fields) {
        elements.push_back(make_bulk(field.first));
        elements.push_back(make_bulk(field.second));
    }
    return make_array(std::move(elements));
}

std::string node_key(const sentinel_address& address) {
    return address.host + ":" + std::to_string(address.port);
}

std::uint64_t parse_unsigned(const std::string& text) {
    std::uint64_t value = 0;
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            value = value * 10 + static_cast<std::uint64_t>(c - '0');
        }
    }
    return value;
}

std::uint16_t parse_port(const std::string& text) {
    return static_cast<std::uint16_t>(parse_unsigned(text));
}

struct fake_replica {
    sentinel_address address;
    std::string flags = "slave";
    int priority = 100;
    std::int64_t offset = 0;
};

struct fake_sentinel_state {
    sentinel_address master;
    std::uint64_t config_epoch = 0;
    std::vector<fake_replica> replicas;
    std::vector<sentinel_address> peers;
    bool reachable = true;
    std::string voted_run_id;
    std::uint64_t voted_epoch = 0;
};

struct fake_redis_state {
    bool master = false;
    sentinel_address master_addr;
    std::int64_t offset = 0;
    bool reachable = true;
    bool delay_role = false;
    int pending_role = 0;
};

struct fake_executor final : crpc::redis::sentinel_command_executor {
    std::map<std::string, fake_sentinel_state> sentinels;
    std::map<std::string, fake_redis_state> redises;

    sentinel_reply execute(const sentinel_address& target,
                           const std::vector<std::string>& command) override {
        const std::string key = node_key(target);
        auto sentinel_it = sentinels.find(key);
        if (sentinel_it != sentinels.end()) {
            return execute_sentinel(sentinel_it->second, command);
        }
        auto redis_it = redises.find(key);
        if (redis_it != redises.end()) {
            return execute_redis(redis_it->second, command);
        }
        return make_transport_error();
    }

private:
    sentinel_reply execute_sentinel(fake_sentinel_state& state,
                                    const std::vector<std::string>& command) {
        if (command.empty()) {
            return make_error("empty command");
        }
        if (!state.reachable) {
            return make_transport_error();
        }
        if (command[0] == "PING") {
            return make_status("PONG");
        }
        if (command[0] != "SENTINEL" || command.size() < 2) {
            return make_error("unknown command");
        }
        const std::string& subcommand = command[1];
        if (subcommand == "master") {
            if (!state.master.valid()) {
                return make_null();
            }
            return make_map({{"name", "mymaster"},
                             {"ip", state.master.host},
                             {"port", std::to_string(state.master.port)},
                             {"flags", "master"},
                             {"config-epoch", std::to_string(state.config_epoch)},
                             {"quorum", "2"},
                             {"num-replicas",
                              std::to_string(state.replicas.size())},
                             {"num-other-sentinels",
                              std::to_string(state.peers.size())}});
        }
        if (subcommand == "get-master-addr-by-name") {
            if (!state.master.valid()) {
                return make_null();
            }
            return make_array({make_bulk(state.master.host),
                               make_bulk(std::to_string(state.master.port))});
        }
        if (subcommand == "replicas" || subcommand == "slaves") {
            std::vector<sentinel_reply> entries;
            entries.reserve(state.replicas.size());
            for (const fake_replica& replica : state.replicas) {
                entries.push_back(make_map(
                    {{"ip", replica.address.host},
                     {"port", std::to_string(replica.address.port)},
                     {"flags", replica.flags},
                     {"slave-priority", std::to_string(replica.priority)},
                     {"slave-repl-offset", std::to_string(replica.offset)}}));
            }
            return make_array(std::move(entries));
        }
        if (subcommand == "sentinels") {
            std::vector<sentinel_reply> entries;
            entries.reserve(state.peers.size());
            for (const sentinel_address& peer : state.peers) {
                entries.push_back(make_map(
                    {{"ip", peer.host},
                     {"port", std::to_string(peer.port)},
                     {"flags", "sentinel"}}));
            }
            return make_array(std::move(entries));
        }
        if (subcommand == "is-master-down-by-addr" && command.size() >= 6) {
            bool down = false;
            const auto master_it = redises.find(node_key(state.master));
            if (master_it == redises.end() || !master_it->second.reachable ||
                !master_it->second.master) {
                down = true;
            }
            const std::string& run_id = command[5];
            const std::uint64_t epoch = parse_unsigned(command[4]);
            if (run_id != "*" &&
                (state.voted_run_id.empty() || state.voted_epoch < epoch)) {
                state.voted_run_id = run_id;
                state.voted_epoch = epoch;
            }
            return make_array(
                {make_integer(down ? 1 : 0), make_bulk(state.voted_run_id),
                 make_integer(static_cast<std::int64_t>(state.voted_epoch))});
        }
        return make_error("unknown sentinel subcommand");
    }

    sentinel_reply slave_role(const fake_redis_state& state) {
        return make_array({make_bulk("slave"), make_bulk(state.master_addr.host),
                           make_integer(state.master_addr.port),
                           make_bulk("connected"), make_integer(state.offset)});
    }

    sentinel_reply execute_redis(fake_redis_state& state,
                                 const std::vector<std::string>& command) {
        if (command.empty()) {
            return make_error("empty command");
        }
        if (!state.reachable) {
            return make_transport_error();
        }
        if (command[0] == "PING") {
            return make_status("PONG");
        }
        if (command[0] == "ROLE") {
            if (state.pending_role > 0) {
                --state.pending_role;
                return slave_role(state);
            }
            if (state.master) {
                return make_array(
                    {make_bulk("master"), make_integer(state.offset),
                     make_array(std::vector<sentinel_reply>{})});
            }
            return slave_role(state);
        }
        if (command[0] == "REPLICAOF" && command.size() >= 3) {
            if (command[1] == "NO" && command[2] == "ONE") {
                state.master = true;
                state.master_addr = sentinel_address{};
                if (state.delay_role) {
                    state.pending_role = 1;
                }
                return make_status("OK");
            }
            state.master = false;
            state.master_addr.host = command[1];
            state.master_addr.port = parse_port(command[2]);
            return make_status("OK");
        }
        return make_error("unknown command");
    }
};

bool has_event(const std::vector<sentinel_event>& events,
               sentinel_event_type type) {
    for (const sentinel_event& event : events) {
        if (event.type == type) {
            return true;
        }
    }
    return false;
}

const sentinel_event* find_event(const std::vector<sentinel_event>& events,
                                 sentinel_event_type type) {
    for (const sentinel_event& event : events) {
        if (event.type == type) {
            return &event;
        }
    }
    return nullptr;
}

fake_sentinel_state make_sentinel(const sentinel_address& master,
                                  std::uint64_t epoch,
                                  std::vector<fake_replica> replicas,
                                  std::vector<sentinel_address> peers = {}) {
    fake_sentinel_state state;
    state.master = master;
    state.config_epoch = epoch;
    state.replicas = std::move(replicas);
    state.peers = std::move(peers);
    return state;
}

fake_replica make_replica(const sentinel_address& address, int priority,
                          std::int64_t offset, std::string flags = "slave") {
    fake_replica replica;
    replica.address = address;
    replica.priority = priority;
    replica.offset = offset;
    replica.flags = std::move(flags);
    return replica;
}

} // namespace

CRPC_TEST(sentinel_discovers_master_and_reports_health) {
    const sentinel_address s1{"10.0.0.1", 26379};
    const sentinel_address master{"10.0.0.10", 6379};
    const sentinel_address replica{"10.0.0.11", 6379};

    fake_executor executor;
    executor.sentinels[node_key(s1)] =
        make_sentinel(master, 0, {make_replica(replica, 100, 10)});
    fake_redis_state master_state;
    master_state.master = true;
    master_state.offset = 100;
    executor.redises[node_key(master)] = master_state;
    fake_redis_state replica_state;
    replica_state.master_addr = master;
    replica_state.offset = 10;
    executor.redises[node_key(replica)] = replica_state;

    sentinel_options options;
    options.sentinels = {s1};
    options.down_after = std::chrono::milliseconds{0};
    options.quorum = 1;
    options.run_id = "self-a";

    sentinel monitor(options, executor);
    std::vector<sentinel_event> events;
    monitor.set_event_handler(
        [&events](const sentinel_event& event) { events.push_back(event); });

    monitor.poll_once_at(steady_clock::now());

    const auto status = monitor.status();
    CRPC_CHECK(status.health == sentinel_health::up);
    CRPC_CHECK(status.master == master);
    CRPC_CHECK_EQ(std::size_t{1}, status.replicas);
    CRPC_CHECK_EQ(std::size_t{1}, status.sentinels);
    CRPC_CHECK_EQ(std::uint64_t{0}, status.config_epoch);
    CRPC_CHECK_FALSE(status.failover_in_progress);
    CRPC_CHECK_EQ(std::string("self-a"), monitor.run_id());
}

CRPC_TEST(sentinel_marks_sdown_then_odown_with_quorum) {
    const sentinel_address s1{"10.0.0.1", 26379};
    const sentinel_address s2{"10.0.0.2", 26379};
    const sentinel_address master{"10.0.0.10", 6379};
    const sentinel_address replica{"10.0.0.11", 6379};

    fake_executor executor;
    executor.sentinels[node_key(s1)] =
        make_sentinel(master, 0, {make_replica(replica, 100, 10)}, {s2});
    executor.sentinels[node_key(s2)] =
        make_sentinel(master, 0, {make_replica(replica, 100, 10)}, {s1});
    fake_redis_state master_state;
    master_state.master = true;
    master_state.reachable = false;
    executor.redises[node_key(master)] = master_state;
    fake_redis_state replica_state;
    replica_state.master_addr = master;
    executor.redises[node_key(replica)] = replica_state;

    sentinel_options options;
    options.sentinels = {s1, s2};
    options.down_after = std::chrono::milliseconds{1000};
    options.quorum = 2;
    options.auto_failover = false;
    options.run_id = "self-b";

    sentinel monitor(options, executor);
    std::vector<sentinel_event> events;
    monitor.set_event_handler(
        [&events](const sentinel_event& event) { events.push_back(event); });

    const auto start = steady_clock::now();
    monitor.poll_once_at(start);
    CRPC_CHECK(monitor.status().health == sentinel_health::up);

    monitor.poll_once_at(start + std::chrono::milliseconds{500});
    CRPC_CHECK(monitor.status().health == sentinel_health::up);

    monitor.poll_once_at(start + std::chrono::milliseconds{1000});
    CRPC_CHECK(monitor.status().health == sentinel_health::objectively_down);
    CRPC_CHECK(has_event(events, sentinel_event_type::sdown));
    CRPC_CHECK(has_event(events, sentinel_event_type::odown));
    CRPC_CHECK_FALSE(has_event(events, sentinel_event_type::failover_started));
}

CRPC_TEST(sentinel_failover_promotes_best_replica_and_reconfigures) {
    const sentinel_address s1{"10.0.0.1", 26379};
    const sentinel_address s2{"10.0.0.2", 26379};
    const sentinel_address master{"10.0.0.10", 6379};
    const sentinel_address r1{"10.0.0.11", 6379};
    const sentinel_address r2{"10.0.0.12", 6379};
    const sentinel_address r3{"10.0.0.13", 6379};

    fake_executor executor;
    const std::vector<fake_replica> replicas{
        make_replica(r1, 100, 100), make_replica(r2, 10, 50),
        make_replica(r3, 1, 200, "slave,s_down")};
    executor.sentinels[node_key(s1)] = make_sentinel(master, 0, replicas, {s2});
    executor.sentinels[node_key(s2)] = make_sentinel(master, 0, replicas, {s1});
    fake_redis_state master_state;
    master_state.master = true;
    master_state.reachable = false;
    executor.redises[node_key(master)] = master_state;
    for (const sentinel_address& address : {r1, r2}) {
        fake_redis_state replica_state;
        replica_state.master_addr = master;
        replica_state.reachable = true;
        executor.redises[node_key(address)] = replica_state;
    }
    fake_redis_state down_replica;
    down_replica.master_addr = master;
    down_replica.reachable = false;
    executor.redises[node_key(r3)] = down_replica;

    sentinel_options options;
    options.sentinels = {s1, s2};
    options.self = sentinel_address{"10.0.0.99", 26379};
    options.down_after = std::chrono::milliseconds{0};
    options.failover_timeout = std::chrono::milliseconds{5000};
    options.quorum = 2;
    options.run_id = "self-c";

    sentinel monitor(options, executor);
    std::vector<sentinel_event> events;
    monitor.set_event_handler(
        [&events](const sentinel_event& event) { events.push_back(event); });

    const auto start = steady_clock::now();
    monitor.poll_once_at(start);

    const auto status = monitor.status();
    CRPC_CHECK(status.health == sentinel_health::up);
    CRPC_CHECK(status.master == r2);
    CRPC_CHECK_EQ(std::uint64_t{1}, status.config_epoch);
    CRPC_CHECK_FALSE(status.failover_in_progress);
    CRPC_CHECK_FALSE(status.leader);
    CRPC_CHECK(executor.redises[node_key(r2)].master);
    CRPC_CHECK_FALSE(executor.redises[node_key(r1)].master);
    CRPC_CHECK(executor.redises[node_key(r1)].master_addr == r2);
    CRPC_CHECK(has_event(events, sentinel_event_type::failover_started));
    CRPC_CHECK(has_event(events, sentinel_event_type::promoted));
    CRPC_CHECK(has_event(events, sentinel_event_type::replica_reconfigured));
    const sentinel_event* switched =
        find_event(events, sentinel_event_type::master_switch);
    CRPC_CHECK(switched != nullptr);
    if (switched != nullptr) {
        CRPC_CHECK(switched->previous_master == master);
        CRPC_CHECK(switched->master == r2);
    }

    // A stale sentinel view (old master, lower config epoch) must not undo the
    // failover.
    monitor.poll_once_at(start + std::chrono::milliseconds{1000});
    CRPC_CHECK(monitor.status().master == r2);
    CRPC_CHECK(monitor.status().health == sentinel_health::up);

    // The old master comes back and is demoted to a replica of the new master.
    executor.redises[node_key(master)].reachable = true;
    executor.redises[node_key(master)].master = true;
    monitor.poll_once_at(start + std::chrono::milliseconds{2000});
    CRPC_CHECK_FALSE(executor.redises[node_key(master)].master);
    CRPC_CHECK(executor.redises[node_key(master)].master_addr == r2);
    CRPC_CHECK(has_event(events, sentinel_event_type::master_demoted));
}

CRPC_TEST(sentinel_does_not_fail_over_without_quorum) {
    const sentinel_address s1{"10.0.0.1", 26379};
    const sentinel_address master{"10.0.0.10", 6379};
    const sentinel_address replica{"10.0.0.11", 6379};

    fake_executor executor;
    executor.sentinels[node_key(s1)] =
        make_sentinel(master, 0, {make_replica(replica, 100, 10)});
    executor.sentinels[node_key(s1)].reachable = false;
    fake_redis_state master_state;
    master_state.master = true;
    master_state.reachable = false;
    executor.redises[node_key(master)] = master_state;
    fake_redis_state replica_state;
    replica_state.master_addr = master;
    executor.redises[node_key(replica)] = replica_state;

    sentinel_options options;
    options.master = master;
    options.sentinels = {s1};
    options.down_after = std::chrono::milliseconds{0};
    options.quorum = 2;
    options.run_id = "self-d";

    sentinel monitor(options, executor);
    std::vector<sentinel_event> events;
    monitor.set_event_handler(
        [&events](const sentinel_event& event) { events.push_back(event); });

    monitor.poll_once_at(steady_clock::now());

    const auto status = monitor.status();
    CRPC_CHECK(status.health == sentinel_health::subjectively_down);
    CRPC_CHECK(status.master == master);
    CRPC_CHECK_FALSE(status.failover_in_progress);
    CRPC_CHECK_FALSE(has_event(events, sentinel_event_type::odown));
    CRPC_CHECK_FALSE(has_event(events, sentinel_event_type::failover_started));
}

CRPC_TEST(sentinel_background_monitor_runs_and_stops) {
    const sentinel_address s1{"10.0.0.1", 26379};
    const sentinel_address master{"10.0.0.10", 6379};

    fake_executor executor;
    executor.sentinels[node_key(s1)] = make_sentinel(master, 0, {});
    fake_redis_state master_state;
    master_state.master = true;
    executor.redises[node_key(master)] = master_state;

    sentinel_options options;
    options.master = master;
    options.sentinels = {s1};
    options.down_after = std::chrono::milliseconds{0};
    options.poll_interval = std::chrono::milliseconds{20};
    options.quorum = 1;
    options.run_id = "self-e";

    sentinel monitor(options, executor);
    CRPC_CHECK(monitor.start());
    CRPC_CHECK(crpc_test::wait_for(
        [&monitor] {
            return monitor.status().health == sentinel_health::up;
        },
        5000));
    monitor.stop();
    CRPC_CHECK_FALSE(monitor.running());
}

CRPC_TEST(sentinel_waits_for_promotion_before_switching_master) {
    const sentinel_address s1{"10.0.0.1", 26379};
    const sentinel_address master{"10.0.0.10", 6379};
    const sentinel_address replica{"10.0.0.11", 6379};

    fake_executor executor;
    executor.sentinels[node_key(s1)] =
        make_sentinel(master, 0, {make_replica(replica, 10, 50)});
    fake_redis_state master_state;
    master_state.master = true;
    master_state.reachable = false;
    executor.redises[node_key(master)] = master_state;
    fake_redis_state replica_state;
    replica_state.master_addr = master;
    replica_state.delay_role = true;
    executor.redises[node_key(replica)] = replica_state;

    sentinel_options options;
    options.master = master;
    options.sentinels = {s1};
    options.down_after = std::chrono::milliseconds{0};
    options.failover_timeout = std::chrono::milliseconds{5000};
    options.quorum = 1;
    options.run_id = "self-f";

    sentinel monitor(options, executor);
    std::vector<sentinel_event> events;
    monitor.set_event_handler(
        [&events](const sentinel_event& event) { events.push_back(event); });

    const auto start = steady_clock::now();
    monitor.poll_once_at(start);

    auto status = monitor.status();
    CRPC_CHECK(status.master == master);
    CRPC_CHECK(status.failover_in_progress);
    CRPC_CHECK(has_event(events, sentinel_event_type::failover_started));
    CRPC_CHECK_FALSE(has_event(events, sentinel_event_type::master_switch));

    monitor.poll_once_at(start + std::chrono::milliseconds{10});
    status = monitor.status();
    CRPC_CHECK(status.master == replica);
    CRPC_CHECK_FALSE(status.failover_in_progress);
    CRPC_CHECK(has_event(events, sentinel_event_type::promoted));
    CRPC_CHECK(has_event(events, sentinel_event_type::master_switch));
}

CRPC_TEST(sentinel_adopts_newer_master_configuration) {
    const sentinel_address s1{"10.0.0.1", 26379};
    const sentinel_address old_master{"10.0.0.10", 6379};
    const sentinel_address new_master{"10.0.0.20", 6379};

    fake_executor executor;
    executor.sentinels[node_key(s1)] = make_sentinel(new_master, 5, {});
    fake_redis_state old_state;
    old_state.master = true;
    executor.redises[node_key(old_master)] = old_state;
    fake_redis_state new_state;
    new_state.master = true;
    executor.redises[node_key(new_master)] = new_state;

    sentinel_options options;
    options.master = old_master;
    options.sentinels = {s1};
    options.down_after = std::chrono::milliseconds{0};
    options.quorum = 1;
    options.run_id = "self-g";

    sentinel monitor(options, executor);
    std::vector<sentinel_event> events;
    monitor.set_event_handler(
        [&events](const sentinel_event& event) { events.push_back(event); });

    monitor.poll_once_at(steady_clock::now());

    const auto status = monitor.status();
    CRPC_CHECK(status.master == new_master);
    CRPC_CHECK(status.health == sentinel_health::up);
    CRPC_CHECK_EQ(std::uint64_t{5}, status.config_epoch);
    const sentinel_event* switched =
        find_event(events, sentinel_event_type::master_switch);
    CRPC_CHECK(switched != nullptr);
    if (switched != nullptr) {
        CRPC_CHECK(switched->previous_master == old_master);
        CRPC_CHECK(switched->master == new_master);
    }
}

CRPC_TEST(sentinel_aborts_failover_without_a_replica) {
    const sentinel_address s1{"10.0.0.1", 26379};
    const sentinel_address master{"10.0.0.10", 6379};

    fake_executor executor;
    executor.sentinels[node_key(s1)] = make_sentinel(master, 0, {});
    fake_redis_state master_state;
    master_state.master = true;
    master_state.reachable = false;
    executor.redises[node_key(master)] = master_state;

    sentinel_options options;
    options.master = master;
    options.sentinels = {s1};
    options.down_after = std::chrono::milliseconds{0};
    options.quorum = 1;
    options.run_id = "self-h";

    sentinel monitor(options, executor);
    std::vector<sentinel_event> events;
    monitor.set_event_handler(
        [&events](const sentinel_event& event) { events.push_back(event); });

    monitor.poll_once_at(steady_clock::now());

    const auto status = monitor.status();
    CRPC_CHECK(status.master == master);
    CRPC_CHECK(status.health == sentinel_health::objectively_down);
    CRPC_CHECK_FALSE(status.failover_in_progress);
    CRPC_CHECK(has_event(events, sentinel_event_type::failover_started));
    CRPC_CHECK(has_event(events, sentinel_event_type::failover_aborted));
    CRPC_CHECK_FALSE(has_event(events, sentinel_event_type::master_switch));
}

CRPC_TEST(sentinel_adopts_highest_config_epoch_across_peers) {
    const sentinel_address stale{"10.0.0.1", 26379};
    const sentinel_address authoritative{"10.0.0.2", 26379};
    const sentinel_address old_master{"10.0.0.10", 6379};
    const sentinel_address new_master{"10.0.0.20", 6379};

    fake_executor executor;
    // The stale peer is listed first and still reports the old master at epoch
    // 0; the authoritative peer is second and reports the new master at epoch 5.
    // The best configuration must win, not the first valid one.
    executor.sentinels[node_key(stale)] = make_sentinel(old_master, 0, {});
    executor.sentinels[node_key(authoritative)] = make_sentinel(new_master, 5, {});
    fake_redis_state old_state;
    old_state.master = true;
    executor.redises[node_key(old_master)] = old_state;
    fake_redis_state new_state;
    new_state.master = true;
    executor.redises[node_key(new_master)] = new_state;

    sentinel_options options;
    options.master = old_master;
    options.sentinels = {stale, authoritative};
    options.down_after = std::chrono::milliseconds{0};
    options.quorum = 1;
    options.run_id = "self-epoch";

    sentinel monitor(options, executor);
    monitor.poll_once_at(steady_clock::now());

    const auto status = monitor.status();
    CRPC_CHECK(status.master == new_master);
    CRPC_CHECK_EQ(std::uint64_t{5}, status.config_epoch);
    // The election epoch must advance with the accepted configuration, or a
    // later election would present an epoch the peers already voted at.
    CRPC_CHECK_EQ(std::uint64_t{5}, monitor.epoch());
}

CRPC_TEST(sentinel_keeps_higher_config_epoch_against_stale_peer) {
    const sentinel_address stale{"10.0.0.1", 26379};
    const sentinel_address authoritative{"10.0.0.2", 26379};
    const sentinel_address old_master{"10.0.0.10", 6379};
    const sentinel_address new_master{"10.0.0.20", 6379};

    fake_executor executor;
    executor.sentinels[node_key(stale)] = make_sentinel(old_master, 0, {});
    executor.sentinels[node_key(authoritative)] = make_sentinel(new_master, 5, {});
    fake_redis_state old_state;
    old_state.master = true;
    executor.redises[node_key(old_master)] = old_state;
    fake_redis_state new_state;
    new_state.master = true;
    executor.redises[node_key(new_master)] = new_state;

    sentinel_options options;
    options.master = old_master;
    options.sentinels = {stale, authoritative};
    options.down_after = std::chrono::milliseconds{0};
    options.quorum = 1;
    options.run_id = "self-epoch2";

    sentinel monitor(options, executor);
    const auto start = steady_clock::now();
    monitor.poll_once_at(start);
    CRPC_CHECK_EQ(std::uint64_t{5}, monitor.status().config_epoch);

    // The authoritative peer disappears; only the stale epoch-0 peer remains.
    executor.sentinels[node_key(authoritative)].reachable = false;
    monitor.poll_once_at(start + std::chrono::milliseconds{1000});

    const auto status = monitor.status();
    CRPC_CHECK(status.master == new_master);
    CRPC_CHECK_EQ(std::uint64_t{5}, status.config_epoch);
}

CRPC_TEST(sentinel_odown_probe_does_not_cast_vote) {
    const sentinel_address s1{"10.0.0.1", 26379};
    const sentinel_address s2{"10.0.0.2", 26379};
    const sentinel_address master{"10.0.0.10", 6379};

    fake_executor executor;
    executor.sentinels[node_key(s1)] = make_sentinel(master, 0, {}, {s2});
    executor.sentinels[node_key(s2)] = make_sentinel(master, 0, {}, {s1});
    fake_redis_state master_state;
    master_state.master = true;
    master_state.reachable = false;
    executor.redises[node_key(master)] = master_state;

    sentinel_options options;
    options.sentinels = {s1, s2};
    options.down_after = std::chrono::milliseconds{0};
    options.quorum = 2;
    options.auto_failover = false;
    options.run_id = "self-odown";

    sentinel monitor(options, executor);
    monitor.poll_once_at(steady_clock::now());

    CRPC_CHECK(monitor.status().health == sentinel_health::objectively_down);
    // With auto-failover disabled the sentinel must not have spent s2's one vote
    // for the epoch; that is reserved for a real election.
    CRPC_CHECK(executor.sentinels[node_key(s2)].voted_run_id.empty());
}

CRPC_TEST(sentinel_registers_self_vote_before_asking_peers) {
    const sentinel_address self{"10.0.0.1", 26379};
    const sentinel_address peer{"10.0.0.2", 26379};
    const sentinel_address master{"10.0.0.10", 6379};
    const sentinel_address replica{"10.0.0.11", 6379};

    fake_executor executor;
    executor.sentinels[node_key(self)] =
        make_sentinel(master, 0, {make_replica(replica, 100, 10)});
    executor.sentinels[node_key(peer)] =
        make_sentinel(master, 0, {make_replica(replica, 100, 10)});
    fake_redis_state master_state;
    master_state.master = true;
    master_state.reachable = false;
    executor.redises[node_key(master)] = master_state;
    fake_redis_state replica_state;
    replica_state.master_addr = master;
    executor.redises[node_key(replica)] = replica_state;

    sentinel_options options;
    options.self = self;
    options.sentinels = {peer};
    options.down_after = std::chrono::milliseconds{0};
    options.quorum = 1;
    options.run_id = "self-vote";

    sentinel monitor(options, executor);
    std::vector<sentinel_event> events;
    monitor.set_event_handler(
        [&events](const sentinel_event& event) { events.push_back(event); });
    monitor.poll_once_at(steady_clock::now());

    // The candidate must have registered its own vote before asking the peer.
    CRPC_CHECK(executor.sentinels[node_key(self)].voted_run_id == "self-vote");
    CRPC_CHECK(has_event(events, sentinel_event_type::failover_started));
    CRPC_CHECK(monitor.status().master == replica);
}

CRPC_TEST(sentinel_concurrent_candidates_do_not_both_become_leader) {
    const sentinel_address a{"10.0.0.1", 26379};
    const sentinel_address b{"10.0.0.2", 26379};
    const sentinel_address master{"10.0.0.10", 6379};
    const sentinel_address replica{"10.0.0.11", 6379};

    fake_executor executor;
    executor.sentinels[node_key(a)] =
        make_sentinel(master, 0, {make_replica(replica, 100, 10)});
    executor.sentinels[node_key(b)] =
        make_sentinel(master, 0, {make_replica(replica, 100, 10)});
    fake_redis_state master_state;
    master_state.master = true;
    master_state.reachable = false;
    executor.redises[node_key(master)] = master_state;
    fake_redis_state replica_state;
    replica_state.master_addr = master;
    executor.redises[node_key(replica)] = replica_state;

    sentinel_options a_options;
    a_options.self = a;
    a_options.sentinels = {b};
    a_options.down_after = std::chrono::milliseconds{0};
    a_options.quorum = 1;
    a_options.run_id = "cand-a";

    sentinel_options b_options;
    b_options.self = b;
    b_options.sentinels = {a};
    b_options.down_after = std::chrono::milliseconds{0};
    b_options.quorum = 1;
    b_options.run_id = "cand-b";

    sentinel monitor_a(a_options, executor);
    sentinel monitor_b(b_options, executor);
    std::vector<sentinel_event> a_events;
    std::vector<sentinel_event> b_events;
    monitor_a.set_event_handler(
        [&a_events](const sentinel_event& event) { a_events.push_back(event); });
    monitor_b.set_event_handler(
        [&b_events](const sentinel_event& event) { b_events.push_back(event); });
    monitor_a.poll_once_at(steady_clock::now());
    monitor_b.poll_once_at(steady_clock::now());

    // Vote exclusivity must leave exactly one leader, not two concurrent ones.
    const bool a_started =
        has_event(a_events, sentinel_event_type::failover_started);
    const bool b_started =
        has_event(b_events, sentinel_event_type::failover_started);
    CRPC_CHECK(a_started != b_started);
}

CRPC_TEST(sentinel_converges_on_deterministic_master_at_equal_epoch) {
    const sentinel_address s2{"10.0.0.2", 26379};
    const sentinel_address larger{"10.0.0.50", 6379};
    const sentinel_address smaller{"10.0.0.40", 6379};

    fake_executor executor;
    executor.sentinels[node_key(s2)] = make_sentinel(larger, 0, {});
    fake_redis_state larger_state;
    larger_state.master = true;
    executor.redises[node_key(larger)] = larger_state;
    fake_redis_state smaller_state;
    smaller_state.master = true;
    executor.redises[node_key(smaller)] = smaller_state;

    sentinel_options options;
    options.master = larger;
    options.sentinels = {s2};
    options.down_after = std::chrono::milliseconds{0};
    options.quorum = 1;
    options.run_id = "self-tie";

    sentinel monitor(options, executor);
    const auto start = steady_clock::now();
    monitor.poll_once_at(start);
    CRPC_CHECK(monitor.status().health == sentinel_health::up);
    CRPC_CHECK(monitor.status().master == larger);

    // The peer now reports a different master at the same config epoch. The
    // deterministic tie-break must converge on the smaller address instead of
    // each sentinel keeping its own healthy-looking master forever.
    executor.sentinels[node_key(s2)] = make_sentinel(smaller, 0, {});
    monitor.poll_once_at(start + std::chrono::milliseconds{1000});

    CRPC_CHECK(monitor.status().master == smaller);
}

CRPC_TEST(sentinel_demotes_old_master_to_the_new_master) {
    const sentinel_address s1{"10.0.0.1", 26379};
    const sentinel_address old_master{"10.0.0.10", 6379};
    const sentinel_address r2{"10.0.0.11", 6379};
    const sentinel_address r3{"10.0.0.12", 6379};

    fake_executor executor;
    executor.sentinels[node_key(s1)] =
        make_sentinel(old_master, 0, {make_replica(r2, 100, 10)});
    fake_redis_state old_state;
    old_state.master = true;
    old_state.reachable = false;
    executor.redises[node_key(old_master)] = old_state;
    fake_redis_state r2_state;
    r2_state.master_addr = old_master;
    executor.redises[node_key(r2)] = r2_state;
    fake_redis_state r3_state;
    r3_state.master_addr = old_master;
    executor.redises[node_key(r3)] = r3_state;

    sentinel_options options;
    options.sentinels = {s1};
    options.down_after = std::chrono::milliseconds{0};
    options.quorum = 1;
    options.run_id = "self-demote";

    sentinel monitor(options, executor);
    const auto start = steady_clock::now();
    monitor.poll_once_at(start);
    CRPC_CHECK(monitor.status().master == r2);

    // The old master comes back and a newer configuration (r3, higher epoch)
    // appears. The demotion must point the old master at the newest master, not
    // at the stale r2 that was current when the demote list was built.
    executor.redises[node_key(old_master)].reachable = true;
    executor.sentinels[node_key(s1)] = make_sentinel(r3, 5, {});
    monitor.poll_once_at(start + std::chrono::milliseconds{1000});

    CRPC_CHECK(monitor.status().master == r3);
    CRPC_CHECK(executor.redises[node_key(old_master)].master_addr == r3);
}

CRPC_TEST(sentinel_external_runtime_start_stop) {
    const sentinel_address s1{"10.0.0.1", 26379};
    const sentinel_address master{"10.0.0.10", 6379};

    fake_executor executor;
    executor.sentinels[node_key(s1)] = make_sentinel(master, 0, {});
    fake_redis_state master_state;
    master_state.master = true;
    executor.redises[node_key(master)] = master_state;

    sentinel_options options;
    options.master = master;
    options.sentinels = {s1};
    options.down_after = std::chrono::milliseconds{0};
    options.poll_interval = std::chrono::milliseconds{10};
    options.quorum = 1;
    options.run_id = "self-ext";

    crpc::runtime_options rt_options;
    rt_options.start_handler_pool = false;
    rt_options.idle_wait = std::chrono::milliseconds{10};
    crpc::runtime rt(rt_options);
    rt.start_thread();

    {
        sentinel monitor(options, executor);
        CRPC_CHECK(monitor.start(&rt));
        CRPC_CHECK(crpc_test::wait_for(
            [&monitor] {
                return monitor.status().health == sentinel_health::up;
            },
            5000));
        monitor.stop();
        CRPC_CHECK_FALSE(monitor.running());
    }

    rt.stop();
    rt.join();
}

namespace {

class one_shot_factory final : public crpc::backend_factory {
public:
    std::unique_ptr<crpc::connection> client;

    std::unique_ptr<crpc::connection> connect(
        const crpc::endpoint&, const crpc::connect_options&) override {
        return std::move(client);
    }

    std::unique_ptr<crpc::listener> listen(const crpc::endpoint&,
                                           const crpc::listen_options&) override {
        return nullptr;
    }
};

class pong_source final : public crpc::io::source {
public:
    crpc::io::sink* sink = nullptr;

    std::size_t on_data(crpc::span<const std::byte> data) noexcept override {
        if (!replied_) {
            replied_ = true;
            static const char response[] = "+PONG\r\n";
            sink->push(crpc::span<const std::byte>(
                           reinterpret_cast<const std::byte*>(response), 7),
                       &completion_);
        }
        return data.size();
    }

    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {}

private:
    struct noop_completion final : crpc::io::completion {
        void complete(crpc::io::io_result) noexcept override {}
    };

    bool replied_ = false;
    noop_completion completion_;
};

// Blocks the first command to model a request that is still in flight when the
// caller stops the monitor.
class blocking_executor final : public crpc::redis::sentinel_command_executor {
public:
    std::atomic<int> calls{0};
    std::atomic<bool> release{false};

    sentinel_reply execute(const sentinel_address&,
                           const std::vector<std::string>&) override {
        const int index = calls.fetch_add(1);
        if (index == 0) {
            while (!release.load()) {
                std::this_thread::yield();
            }
        }
        return make_status("PONG");
    }
};

// Replies with a per-command tag so a stale reply cannot be mistaken for the
// current one.
class tagged_source final : public crpc::io::source {
public:
    crpc::io::sink* sink = nullptr;
    std::atomic<int>* counter = nullptr;

    std::size_t on_data(crpc::span<const std::byte> data) noexcept override {
        char buffer[32];
        const int index =
            counter != nullptr ? counter->fetch_add(1) : 0;
        const int n = std::snprintf(buffer, sizeof(buffer), "+REPLY%d\r\n",
                                    index);
        sink->push(crpc::span<const std::byte>(
                       reinterpret_cast<const std::byte*>(buffer),
                       static_cast<std::size_t>(n)),
                   &completion_);
        return data.size();
    }

    void on_eof() noexcept override {}
    void on_error(std::error_code) noexcept override {}

private:
    struct noop_completion final : crpc::io::completion {
        void complete(crpc::io::io_result) noexcept override {}
    };

    noop_completion completion_;
};

struct server_side {
    std::unique_ptr<crpc::inproc_connection> conn;
    std::unique_ptr<tagged_source> source;
};

// Creates a fresh in-process pair per connect so the adapter can reconnect
// after it drops a connection.
class reconnect_factory final : public crpc::backend_factory {
public:
    std::atomic<int> counter{0};
    std::vector<server_side> servers;

    std::unique_ptr<crpc::connection> connect(
        const crpc::endpoint&, const crpc::connect_options&) override {
        auto pair = crpc::inproc_pair::make_typed();
        auto client = std::move(pair.first);
        auto server = std::move(pair.second);
        // Hold the first reply only until `flush_delayed()` so the adapter's
        // read can time out first; later connections reply immediately.
        if (servers.empty()) {
            server->faults().delay_chunks = 1;
        }
        auto source = std::make_unique<tagged_source>();
        source->sink = &server->sink();
        source->counter = &counter;
        server->bind_source(source.get());
        servers.push_back(server_side{std::move(server), std::move(source)});
        return client;
    }

    std::unique_ptr<crpc::listener> listen(const crpc::endpoint&,
                                           const crpc::listen_options&) override {
        return nullptr;
    }
};

} // namespace

CRPC_TEST(sentinel_connection_executor_uses_crpc_io) {
    auto pair = crpc::inproc_pair::make();
    auto client_connection = std::move(pair.first);
    auto server_connection = std::move(pair.second);

    pong_source server;
    server_connection->bind_source(&server);
    server.sink = &server_connection->sink();

    one_shot_factory factory;
    factory.client = std::move(client_connection);

    crpc::redis::sentinel_connection_executor executor(factory);
    const auto reply =
        executor.execute(sentinel_address{"inproc", 6379}, {"PING"});
    CRPC_CHECK(reply.ok());
    CRPC_CHECK_EQ(std::string("PONG"), reply.text);
}

CRPC_TEST(sentinel_adapter_closes_connection_after_timeout) {
    reconnect_factory factory;
    crpc::redis::connection_config config;
    config.operation_timeout = std::chrono::milliseconds{100};
    crpc::redis::sentinel_connection_executor executor(factory, config);

    const sentinel_address target{"inproc", 6379};
    const auto first = executor.execute(target, {"PING"});
    CRPC_CHECK_FALSE(first.ok());
    CRPC_CHECK(first.transport_failure);

    // Release the late reply of the first command.
    factory.servers.front().conn->flush_delayed();

    const auto second = executor.execute(target, {"PING"});
    CRPC_CHECK(second.ok());
    // Without the fix the second command reads the first command's stale reply.
    CRPC_CHECK_EQ(std::string("REPLY1"), second.text);
}

CRPC_TEST(sentinel_stop_with_blocked_command_is_safe) {
    blocking_executor executor;

    const sentinel_address s1{"10.0.0.1", 26379};
    const sentinel_address master{"10.0.0.10", 6379};

    sentinel_options options;
    options.master = master;
    options.sentinels = {s1};
    options.down_after = std::chrono::milliseconds{0};
    options.poll_interval = std::chrono::milliseconds{10};
    options.quorum = 1;
    options.run_id = "self-blocked";

    crpc::runtime_options rt_options;
    rt_options.start_handler_pool = false;
    rt_options.idle_wait = std::chrono::milliseconds{10};
    crpc::runtime rt(rt_options);
    rt.start_thread();

    {
        sentinel monitor(options, executor);
        CRPC_CHECK(monitor.start(&rt));
        // Wait until the monitor coroutine is blocked inside execute().
        CRPC_CHECK(crpc_test::wait_for(
            [&executor] { return executor.calls.load() > 0; }, 5000));
        monitor.stop();
        // The monitor handle is destroyed here while the command is still in
        // flight; the loop must keep its own reference to the state, so this
        // cannot leave a dangling pointer for the coroutine to resume into.
    }

    executor.release = true;
    rt.stop();
    rt.join();
    CRPC_CHECK(executor.calls.load() >= 1);
}

CRPC_TEST_MAIN()
