// CRPC -- Redis Cluster tests.
//
// The cluster layer is exercised with a scripted executor per node, so slot
// routing, MOVED/ASK handling, topology refresh and CROSSSLOT rejection are
// pinned without a live Redis Cluster. The production code is still the code
// under test: only the transport boundary (cluster_executor_provider) is
// replaced.
#include "test_framework.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <crpc/error.hpp>
#include <crpc/redis/cluster.hpp>
#include <crpc/redis/pipeline.hpp>
#include <crpc/redis/sentinel.hpp>
#include <crpc/redis/transaction.hpp>

#include "redis_test_support.hpp"

namespace {

using crpc::redis::cluster;
using crpc::redis::cluster_address;
using crpc::redis::cluster_command_keys;
using crpc::redis::cluster_executor_provider;
using crpc::redis::cluster_options;
using crpc::redis::cluster_slot_range;
using crpc::redis::cluster_slot_of;
using crpc::redis::cluster_topology;
using crpc::redis::command;
using crpc::redis::resp_value;

using redis_test::array;
using redis_test::error;
using redis_test::integer;
using redis_test::map;
using redis_test::simple;
using redis_test::text;

cluster_address node(const char* host, std::uint16_t port) {
    return cluster_address(host, port);
}

resp_value slots_entry(std::uint16_t begin, std::uint16_t end,
                       const cluster_address& master,
                       const std::vector<cluster_address>& replicas = {}) {
    std::vector<resp_value> parts;
    parts.push_back(integer(begin));
    parts.push_back(integer(end));
    parts.push_back(array({text(master.host), integer(master.port),
                           text("master-id")}));
    for (const cluster_address& replica : replicas) {
        parts.push_back(array({text(replica.host), integer(replica.port),
                               text("replica-id")}));
    }
    return array(std::move(parts));
}

resp_value slots_reply(std::vector<resp_value> entries) {
    return array(std::move(entries));
}

resp_value shard_node(const cluster_address& address, const char* role,
                      const char* id) {
    std::vector<std::pair<resp_value, resp_value>> fields;
    fields.emplace_back(text("id"), text(id));
    fields.emplace_back(text("ip"), text(address.host));
    fields.emplace_back(text("port"), integer(address.port));
    fields.emplace_back(text("role"), text(role));
    return map(std::move(fields));
}

resp_value shards_entry(std::uint16_t begin, std::uint16_t end,
                        const cluster_address& master,
                        const std::vector<cluster_address>& replicas) {
    std::vector<resp_value> nodes;
    nodes.push_back(shard_node(master, "master", "master-id"));
    for (const cluster_address& replica : replicas) {
        nodes.push_back(shard_node(replica, "replica", "replica-id"));
    }
    std::vector<std::pair<resp_value, resp_value>> fields;
    fields.emplace_back(text("slots"), array({integer(begin), integer(end)}));
    fields.emplace_back(text("nodes"), array(std::move(nodes)));
    return map(std::move(fields));
}

resp_value shards_reply(std::vector<resp_value> entries) {
    return array(std::move(entries));
}

std::size_t count_command(const redis_test::scripted_executor& executor,
                          const std::string& name) {
    return static_cast<std::size_t>(std::count_if(
        executor.executed.begin(), executor.executed.end(),
        [&name](const command& cmd) {
            return !cmd.empty() && cmd[0] == name;
        }));
}

std::size_t count_subcommand(const redis_test::scripted_executor& executor,
                             const std::string& first,
                             const std::string& second) {
    return static_cast<std::size_t>(std::count_if(
        executor.executed.begin(), executor.executed.end(),
        [&first, &second](const command& cmd) {
            return cmd.size() >= 2 && cmd[0] == first && cmd[1] == second;
        }));
}

class map_provider final : public cluster_executor_provider {
public:
    redis_test::scripted_executor& add(const cluster_address& address) {
        auto executor = std::make_unique<redis_test::scripted_executor>();
        redis_test::scripted_executor* raw = executor.get();
        nodes_.emplace(address, std::move(executor));
        return *raw;
    }

    crpc::result<crpc::redis::command_executor*> executor_for(
        const cluster_address& address) override {
        ++lookups;
        auto it = nodes_.find(address);
        if (it == nodes_.end()) {
            return crpc::rpc_error{crpc::rpc_code::unavailable,
                                   "no scripted node for " + address.to_string()};
        }
        return static_cast<crpc::redis::command_executor*>(it->second.get());
    }

    std::size_t lookups = 0;
    std::map<cluster_address,
             std::unique_ptr<redis_test::scripted_executor>>
        nodes_;
};

class null_sentinel_executor final
    : public crpc::redis::sentinel_command_executor {
public:
    crpc::redis::sentinel_reply execute(
        const crpc::redis::sentinel_address&,
        const std::vector<std::string>&) override {
        return crpc::redis::sentinel_reply{};
    }
};

cluster_options base_options(const cluster_address& seed) {
    cluster_options options;
    options.seeds.push_back(seed);
    options.prefer_cluster_shards = false;
    return options;
}

} // namespace

CRPC_TEST(cluster_slot_of_uses_crc16_and_hash_tags) {
    CRPC_CHECK_EQ(std::uint16_t{12739}, cluster_slot_of("123456789"));
    CRPC_CHECK_EQ(std::uint16_t{12182}, cluster_slot_of("foo"));
    CRPC_CHECK_EQ(std::uint16_t{5061}, cluster_slot_of("bar"));
    CRPC_CHECK_EQ(std::uint16_t{0}, cluster_slot_of(""));

    // A hash tag restricts hashing to the bytes between the first '{' and the
    // following '}'. An empty tag is not a tag.
    CRPC_CHECK_EQ(cluster_slot_of("user1000"),
                  cluster_slot_of("{user1000}.following"));
    CRPC_CHECK_EQ(cluster_slot_of("user1000"),
                  cluster_slot_of("{user1000}.followers"));
    CRPC_CHECK_EQ(std::uint16_t{15257}, cluster_slot_of("{}"));
    CRPC_CHECK_EQ(std::uint16_t{15495}, cluster_slot_of("a"));
    CRPC_CHECK_EQ(std::uint16_t{3300}, cluster_slot_of("b"));
}

CRPC_TEST(cluster_topology_tracks_ranges_and_moved_overrides) {
    const cluster_address a = node("127.0.0.1", 7000);
    const cluster_address b = node("127.0.0.1", 7001);

    cluster_topology topology;
    CRPC_CHECK(topology.empty());
    CRPC_CHECK(topology.assign(cluster_slot_range{0, 8191, a, {}}));
    CRPC_CHECK(topology.assign(cluster_slot_range{8192, 16383, b, {}}));
    CRPC_CHECK(topology.complete());
    CRPC_CHECK_EQ(std::size_t{16384}, topology.assigned_slots());
    CRPC_CHECK(topology.master_for(3300) != nullptr);
    CRPC_CHECK(*topology.master_for(3300) == a);
    CRPC_CHECK(topology.master_for(12539) != nullptr);
    CRPC_CHECK(*topology.master_for(12539) == b);
    CRPC_CHECK(topology.range_for(0) != nullptr);
    CRPC_CHECK_EQ(std::uint16_t{0}, topology.range_for(0)->begin);

    const std::uint64_t before = topology.generation();
    CRPC_CHECK(topology.assign(cluster_slot_range{3300, 3300, b, {}}));
    CRPC_CHECK(topology.generation() > before);
    CRPC_CHECK(*topology.master_for(3300) == b);
    CRPC_CHECK(*topology.master_for(3299) == a);
    CRPC_CHECK(*topology.master_for(3301) == a);
    CRPC_CHECK(topology.complete());

    CRPC_CHECK(!topology.assign(cluster_slot_range{16384, 16384, a, {}}));
    CRPC_CHECK(!topology.assign(cluster_slot_range{10, 9, a, {}}));
    CRPC_CHECK(topology.master_for(16384) == nullptr);
    CRPC_CHECK(topology.range_for(65535) == nullptr);
}

CRPC_TEST(cluster_routes_by_slot_through_cluster_slots) {
    const cluster_address a = node("127.0.0.1", 7000);
    const cluster_address b = node("127.0.0.1", 7001);

    map_provider provider;
    redis_test::scripted_executor& node_a = provider.add(a);
    redis_test::scripted_executor& node_b = provider.add(b);

    node_a.set_handler([&](const command& cmd) -> crpc::result<resp_value> {
        if (cmd.size() >= 2 && cmd[0] == "CLUSTER" && cmd[1] == "SLOTS") {
            return slots_reply({slots_entry(0, 8191, a),
                                slots_entry(8192, 16383, b)});
        }
        if (cmd[0] == "SET") {
            return simple("OK");
        }
        if (cmd[0] == "PING") {
            return simple("PONG");
        }
        return error("unexpected command on A");
    });
    node_b.set_handler([&](const command& cmd) -> crpc::result<resp_value> {
        if (cmd[0] == "GET") {
            return text("value");
        }
        return error("unexpected command on B");
    });

    cluster client(base_options(a), provider);
    CRPC_CHECK(client.topology().empty());

    crpc::result<resp_value> set = client.execute({"SET", "b", "1"});
    CRPC_CHECK(set.ok());
    CRPC_CHECK(set.value().is_simple_string());
    CRPC_CHECK_EQ(std::string("OK"), set.value().as_simple_string());
    CRPC_CHECK_EQ(std::size_t{1}, count_command(node_a, "CLUSTER"));
    CRPC_CHECK_EQ(std::size_t{1}, count_command(node_a, "SET"));
    CRPC_CHECK_EQ(std::size_t{0}, count_command(node_b, "SET"));

    crpc::result<resp_value> get = client.execute({"GET", "key"});
    CRPC_CHECK(get.ok());
    CRPC_CHECK_EQ(std::string("value"), get.value().as_bulk_string());
    CRPC_CHECK_EQ(std::size_t{1}, count_command(node_b, "GET"));
    // The second command used the cached topology; no second refresh.
    CRPC_CHECK_EQ(std::size_t{1}, count_command(node_a, "CLUSTER"));

    CRPC_CHECK(client.topology().master_for(3300) != nullptr);
    CRPC_CHECK(*client.topology().master_for(3300) == a);
    CRPC_CHECK(client.topology().master_for(12539) != nullptr);
    CRPC_CHECK(*client.topology().master_for(12539) == b);

    // A keyless command is legal on any node.
    crpc::result<resp_value> ping = client.execute({"PING"});
    CRPC_CHECK(ping.ok());
    CRPC_CHECK_EQ(std::string("PONG"), ping.value().as_simple_string());
    CRPC_CHECK_EQ(std::size_t{1}, count_command(node_a, "PING"));
}

CRPC_TEST(cluster_refresh_prefers_cluster_shards_and_reads_replicas) {
    const cluster_address a = node("127.0.0.1", 7000);
    const cluster_address b = node("127.0.0.1", 7001);
    const cluster_address replica = node("127.0.0.1", 7002);

    map_provider provider;
    redis_test::scripted_executor& node_a = provider.add(a);
    provider.add(b);

    node_a.set_handler([&](const command& cmd) -> crpc::result<resp_value> {
        if (cmd.size() >= 2 && cmd[0] == "CLUSTER" && cmd[1] == "SHARDS") {
            return shards_reply(
                {shards_entry(0, 8191, a, {replica}),
                 shards_entry(8192, 16383, b, {})});
        }
        return error("CLUSTER SHARDS expected");
    });

    cluster_options options = base_options(a);
    options.prefer_cluster_shards = true;
    cluster client(options, provider);

    CRPC_CHECK(client.refresh().ok());
    CRPC_CHECK_EQ(std::size_t{1}, count_subcommand(node_a, "CLUSTER", "SHARDS"));
    CRPC_CHECK_EQ(std::size_t{0}, count_subcommand(node_a, "CLUSTER", "SLOTS"));
    CRPC_CHECK(client.topology().complete());
    CRPC_CHECK(*client.topology().master_for(0) == a);
    CRPC_CHECK(*client.topology().master_for(12539) == b);
    const cluster_slot_range* range = client.topology().range_for(0);
    CRPC_CHECK(range != nullptr);
    CRPC_CHECK_EQ(std::size_t{1}, range->replicas.size());
    CRPC_CHECK(range->replicas[0] == replica);
}

CRPC_TEST(cluster_refresh_falls_back_to_cluster_slots) {
    const cluster_address a = node("127.0.0.1", 7000);
    const cluster_address b = node("127.0.0.1", 7001);

    map_provider provider;
    redis_test::scripted_executor& node_a = provider.add(a);
    provider.add(b);

    node_a.set_handler([&](const command& cmd) -> crpc::result<resp_value> {
        if (cmd.size() >= 2 && cmd[0] == "CLUSTER" && cmd[1] == "SHARDS") {
            return error("ERR unknown command 'SHARDS'");
        }
        if (cmd.size() >= 2 && cmd[0] == "CLUSTER" && cmd[1] == "SLOTS") {
            return slots_reply({slots_entry(0, 8191, a),
                                slots_entry(8192, 16383, b)});
        }
        return error("unexpected command");
    });

    cluster_options options = base_options(a);
    options.prefer_cluster_shards = true;
    cluster client(options, provider);

    CRPC_CHECK(client.refresh().ok());
    CRPC_CHECK_EQ(std::size_t{1}, count_subcommand(node_a, "CLUSTER", "SHARDS"));
    CRPC_CHECK_EQ(std::size_t{1}, count_subcommand(node_a, "CLUSTER", "SLOTS"));
    CRPC_CHECK(*client.topology().master_for(3300) == a);
    CRPC_CHECK(*client.topology().master_for(12539) == b);
}

CRPC_TEST(cluster_moved_redirect_updates_slot_and_retries) {
    const cluster_address a = node("127.0.0.1", 7000);
    const cluster_address b = node("127.0.0.1", 7001);

    map_provider provider;
    redis_test::scripted_executor& node_a = provider.add(a);
    redis_test::scripted_executor& node_b = provider.add(b);

    node_a.set_handler([&](const command& cmd) -> crpc::result<resp_value> {
        if (cmd.size() >= 2 && cmd[0] == "CLUSTER" && cmd[1] == "SLOTS") {
            return slots_reply({slots_entry(0, 8191, a),
                                slots_entry(8192, 16383, b)});
        }
        if (cmd[0] == "SET") {
            return error("MOVED 3300 127.0.0.1:7001");
        }
        return error("unexpected command on A");
    });
    node_b.set_handler([&](const command& cmd) -> crpc::result<resp_value> {
        if (cmd[0] == "SET") {
            return simple("OK");
        }
        return error("unexpected command on B");
    });

    cluster client(base_options(a), provider);
    crpc::result<resp_value> reply = client.execute({"SET", "b", "1"});
    CRPC_CHECK(reply.ok());
    CRPC_CHECK(reply.value().is_simple_string());
    CRPC_CHECK_EQ(std::string("OK"), reply.value().as_simple_string());
    CRPC_CHECK_EQ(std::size_t{1}, count_command(node_a, "SET"));
    CRPC_CHECK_EQ(std::size_t{1}, count_command(node_b, "SET"));
    CRPC_CHECK(client.topology().master_for(3300) != nullptr);
    CRPC_CHECK(*client.topology().master_for(3300) == b);
}

CRPC_TEST(cluster_ask_redirect_uses_asking_without_updating_topology) {
    const cluster_address a = node("127.0.0.1", 7000);
    const cluster_address b = node("127.0.0.1", 7001);

    map_provider provider;
    redis_test::scripted_executor& node_a = provider.add(a);
    redis_test::scripted_executor& node_b = provider.add(b);

    node_a.set_handler([&](const command& cmd) -> crpc::result<resp_value> {
        if (cmd.size() >= 2 && cmd[0] == "CLUSTER" && cmd[1] == "SLOTS") {
            return slots_reply({slots_entry(0, 16383, a)});
        }
        if (cmd[0] == "SET") {
            return error("ASK 3300 127.0.0.1:7001");
        }
        return error("unexpected command on A");
    });
    node_b.set_handler([&](const command& cmd) -> crpc::result<resp_value> {
        if (cmd[0] == "ASKING") {
            return simple("OK");
        }
        if (cmd[0] == "SET") {
            return simple("OK");
        }
        return error("unexpected command on B");
    });

    cluster client(base_options(a), provider);
    crpc::result<resp_value> reply = client.execute({"SET", "b", "1"});
    CRPC_CHECK(reply.ok());
    CRPC_CHECK_EQ(std::string("OK"), reply.value().as_simple_string());
    CRPC_CHECK_EQ(std::size_t{2}, node_b.executed.size());
    CRPC_CHECK_EQ(std::string("ASKING"), node_b.executed[0][0]);
    CRPC_CHECK_EQ(std::string("SET"), node_b.executed[1][0]);
    // ASK is a temporary redirect: the slot still belongs to A.
    CRPC_CHECK(*client.topology().master_for(3300) == a);
}

CRPC_TEST(cluster_rejects_cross_slot_batch_and_pin) {
    const cluster_address a = node("127.0.0.1", 7000);

    map_provider provider;
    provider.add(a);

    cluster client(base_options(a), provider);
    crpc::result<std::vector<resp_value>> batch = client.execute_batch(
        {{"SET", "a", "1"}, {"SET", "b", "1"}});
    CRPC_CHECK(!batch.ok());
    CRPC_CHECK(batch.error().code == crpc::rpc_code::failed_precondition);
    CRPC_CHECK(batch.error().message.find("CROSSSLOT") != std::string::npos);
    // Rejected locally: no topology refresh, no node lookup.
    CRPC_CHECK_EQ(std::size_t{0}, provider.lookups);

    crpc::result<std::unique_ptr<crpc::redis::cluster_slot_executor>> pin =
        client.pin({"a", "b"});
    CRPC_CHECK(!pin.ok());
    CRPC_CHECK(pin.error().code == crpc::rpc_code::failed_precondition);
    CRPC_CHECK(pin.error().message.find("CROSSSLOT") != std::string::npos);

    crpc::result<std::unique_ptr<crpc::redis::cluster_slot_executor>> same =
        client.pin({"{user1000}.following", "{user1000}.followers"});
    CRPC_CHECK(same.ok());
    CRPC_CHECK_EQ(std::uint16_t{3443}, same.value()->slot());
}

CRPC_TEST(cluster_same_slot_batch_runs_on_one_node) {
    const cluster_address a = node("127.0.0.1", 7000);

    map_provider provider;
    redis_test::scripted_executor& node_a = provider.add(a);
    node_a.set_handler([&](const command& cmd) -> crpc::result<resp_value> {
        if (cmd.size() >= 2 && cmd[0] == "CLUSTER" && cmd[1] == "SLOTS") {
            return slots_reply({slots_entry(0, 16383, a)});
        }
        return simple("OK");
    });

    cluster client(base_options(a), provider);
    crpc::result<std::vector<resp_value>> replies = client.execute_batch(
        {{"SET", "{user1000}.a", "1"}, {"SET", "{user1000}.b", "2"}});
    CRPC_CHECK(replies.ok());
    CRPC_CHECK_EQ(std::size_t{2}, replies.value().size());
    CRPC_CHECK_EQ(std::size_t{1}, node_a.batch_calls);
    CRPC_CHECK_EQ(std::size_t{1}, node_a.batches.size());
    CRPC_CHECK_EQ(std::size_t{2}, node_a.batches[0].size());
    CRPC_CHECK_EQ(std::string("OK"), replies.value()[0].as_simple_string());
}

CRPC_TEST(cluster_pinned_executor_runs_transaction_on_one_node) {
    const cluster_address a = node("127.0.0.1", 7000);
    const cluster_address b = node("127.0.0.1", 7001);

    map_provider provider;
    redis_test::scripted_executor& node_a = provider.add(a);
    redis_test::scripted_executor& node_b = provider.add(b);

    node_a.set_handler([&](const command& cmd) -> crpc::result<resp_value> {
        if (cmd.size() >= 2 && cmd[0] == "CLUSTER" && cmd[1] == "SLOTS") {
            return slots_reply({slots_entry(0, 8191, a),
                                slots_entry(8192, 16383, b)});
        }
        if (cmd[0] == "MULTI") {
            return simple("OK");
        }
        if (cmd[0] == "SET") {
            return simple("QUEUED");
        }
        if (cmd[0] == "EXEC") {
            return array({simple("OK"), simple("OK")});
        }
        return error("unexpected command on A");
    });
    node_b.set_handler([](const command&) -> crpc::result<resp_value> {
        return error("transaction must not reach B");
    });

    cluster client(base_options(a), provider);
    crpc::result<std::unique_ptr<crpc::redis::cluster_slot_executor>> pinned =
        client.pin({"{user1000}.following", "{user1000}.followers"});
    CRPC_CHECK(pinned.ok());
    CRPC_CHECK_EQ(std::uint16_t{3443}, pinned.value()->slot());

    crpc::redis::transaction txn(*pinned.value());
    CRPC_CHECK(txn.multi().ok());
    CRPC_CHECK(txn.queue({"SET", "{user1000}.following", "1"}).ok());
    CRPC_CHECK(txn.queue({"SET", "{user1000}.followers", "1"}).ok());
    crpc::result<crpc::redis::transaction_exec_result> executed = txn.exec();
    CRPC_CHECK(executed.ok());
    CRPC_CHECK(!executed.value().aborted);
    CRPC_CHECK_EQ(std::size_t{2}, executed.value().replies.size());
    CRPC_CHECK_EQ(std::size_t{0}, node_b.executed.size());
    CRPC_CHECK_EQ(std::size_t{1}, count_command(node_a, "MULTI"));
    CRPC_CHECK_EQ(std::size_t{1}, count_command(node_a, "EXEC"));
}

CRPC_TEST(cluster_uses_sentinel_discovered_master_as_seed) {
    const cluster_address a = node("127.0.0.1", 7000);
    const cluster_address b = node("127.0.0.1", 7001);

    null_sentinel_executor sentinel_transport;
    crpc::redis::sentinel_options sentinel_options;
    sentinel_options.master_name = "mymaster";
    sentinel_options.master = crpc::redis::sentinel_address(a.host, a.port);
    crpc::redis::sentinel monitor(sentinel_options, sentinel_transport);

    std::vector<cluster_address> seeds =
        crpc::redis::cluster_seeds_from_sentinel(monitor);
    CRPC_CHECK_EQ(std::size_t{1}, seeds.size());
    CRPC_CHECK(seeds[0] == a);

    std::vector<const crpc::redis::sentinel*> monitors{&monitor};
    std::vector<cluster_address> many =
        crpc::redis::cluster_seeds_from_sentinels(monitors);
    CRPC_CHECK_EQ(std::size_t{1}, many.size());
    CRPC_CHECK(many[0] == a);

    map_provider provider;
    redis_test::scripted_executor& node_a = provider.add(a);
    redis_test::scripted_executor& node_b = provider.add(b);
    node_a.set_handler([&](const command& cmd) -> crpc::result<resp_value> {
        if (cmd.size() >= 2 && cmd[0] == "CLUSTER" && cmd[1] == "SLOTS") {
            return slots_reply({slots_entry(0, 8191, a),
                                slots_entry(8192, 16383, b)});
        }
        return simple("OK");
    });
    node_b.set_handler([](const command&) -> crpc::result<resp_value> {
        return simple("OK");
    });

    cluster_options options;
    options.prefer_cluster_shards = false;
    options.seed_provider = [&monitor]() {
        return crpc::redis::cluster_seeds_from_sentinel(monitor);
    };
    cluster client(options, provider);
    crpc::result<resp_value> reply = client.execute({"SET", "b", "1"});
    CRPC_CHECK(reply.ok());
    CRPC_CHECK_EQ(std::string("OK"), reply.value().as_simple_string());
    CRPC_CHECK_EQ(std::size_t{1}, count_command(node_a, "CLUSTER"));
    CRPC_CHECK_EQ(std::size_t{1}, count_command(node_a, "SET"));
}

CRPC_TEST(cluster_extracts_keys_for_routing) {
    CRPC_CHECK(cluster_command_keys({"GET", "foo"}).size() == 1);
    CRPC_CHECK_EQ(std::string("foo"),
                  cluster_command_keys({"GET", "foo"})[0]);
    CRPC_CHECK(cluster_command_keys({"PING"}).empty());

    std::vector<std::string> mset =
        cluster_command_keys({"MSET", "k1", "v1", "k2", "v2"});
    CRPC_CHECK_EQ(std::size_t{2}, mset.size());
    CRPC_CHECK_EQ(std::string("k1"), mset[0]);
    CRPC_CHECK_EQ(std::string("k2"), mset[1]);

    std::vector<std::string> eval =
        cluster_command_keys({"EVAL", "return 1", "2", "a", "b", "arg"});
    CRPC_CHECK_EQ(std::size_t{2}, eval.size());
    CRPC_CHECK_EQ(std::string("a"), eval[0]);
    CRPC_CHECK_EQ(std::string("b"), eval[1]);

    std::vector<std::string> xread = cluster_command_keys(
        {"XREAD", "COUNT", "1", "STREAMS", "s1", "s2", "0", "0"});
    CRPC_CHECK_EQ(std::size_t{2}, xread.size());
    CRPC_CHECK_EQ(std::string("s1"), xread[0]);
    CRPC_CHECK_EQ(std::string("s2"), xread[1]);

    std::vector<std::string> bitop =
        cluster_command_keys({"BITOP", "AND", "dest", "s1", "s2"});
    CRPC_CHECK_EQ(std::size_t{3}, bitop.size());
    CRPC_CHECK_EQ(std::string("dest"), bitop[0]);

    std::vector<std::string> zunion = cluster_command_keys(
        {"ZUNIONSTORE", "dest", "2", "z1", "z2"});
    CRPC_CHECK_EQ(std::size_t{3}, zunion.size());
    CRPC_CHECK_EQ(std::string("z1"), zunion[1]);

    std::vector<std::string> sintercard =
        cluster_command_keys({"SINTERCARD", "2", "s1", "s2", "LIMIT", "1"});
    CRPC_CHECK_EQ(std::size_t{2}, sintercard.size());

    std::vector<std::string> blpop =
        cluster_command_keys({"BLPOP", "l1", "l2", "0"});
    CRPC_CHECK_EQ(std::size_t{2}, blpop.size());

    std::vector<std::string> sort =
        cluster_command_keys({"SORT", "mylist", "STORE", "dest"});
    CRPC_CHECK_EQ(std::size_t{2}, sort.size());
    CRPC_CHECK_EQ(std::string("dest"), sort[1]);

    std::vector<std::string> memory =
        cluster_command_keys({"MEMORY", "USAGE", "key"});
    CRPC_CHECK_EQ(std::size_t{1}, memory.size());
    CRPC_CHECK_EQ(std::string("key"), memory[0]);
}

// `ranges()` must hand back a reference, not a copy.
//
// The topology is read-mostly and every caller only iterates it, but a copy
// duplicates every range *and* each range's replica vector on each call.
// Nothing observable changes, which is exactly why a behavioural assertion
// cannot catch a regression here: the check has to be on the signature, at
// compile time.
static_assert(
    std::is_same_v<decltype(std::declval<const cluster_topology&>().ranges()),
                   const std::vector<cluster_slot_range>&>,
    "cluster_topology::ranges() must return a reference; returning by value copies every range and its replica list on each call");

CRPC_TEST(redis_cluster_topology_ranges_is_a_reference) {
    cluster_topology topology;
    cluster_slot_range range;
    range.begin = 0;
    range.end = 100;
    range.master = cluster_address("127.0.0.1", 7000);
    CRPC_CHECK(topology.assign(range));

    // The reference must observe a later assign through the same table, which
    // a by-value snapshot could not.
    const std::vector<cluster_slot_range>& view = topology.ranges();
    CRPC_CHECK_EQ(std::size_t{1}, view.size());

    cluster_slot_range second;
    second.begin = 101;
    second.end = 200;
    second.master = cluster_address("127.0.0.1", 7001);
    CRPC_CHECK(topology.assign(second));
    CRPC_CHECK_EQ(std::size_t{2}, topology.ranges().size());

    // Same object: the address is stable, so this is the table's own vector.
    CRPC_CHECK(topology.ranges().data() == topology.ranges().data());
}

CRPC_TEST_MAIN()
