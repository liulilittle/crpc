// CRPC -- G19 step4 PR-9: replicated state machine tests.
//
// PR-7 wrote a KV value locally and proposed an EMPTY raft entry. On one node
// that is indistinguishable from correct; on three it means the followers store
// an index with no command and never learn the value. ACL changes had the same
// hole: a rule change would be committed and applied on the leader only.
//
// These cases drive a real three-node `raft_node` cluster through an in-process
// transport and assert the property that was missing: every node that applies
// the same log ends with the same KV table and the same ACL table.
//
// The cluster here is deliberately minimal -- one `registry_service` per node,
// each with its own state directory and its own `kv_store` -- because the
// assertion is about convergence, not about the wire.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "test_framework.hpp"

#include <crpc/cluster/consul_http.hpp>
#include <crpc/cluster/kv_store.hpp>
#include <crpc/cluster/raft_command.hpp>
#include <crpc/cluster/raft_node.hpp>
#include <crpc/cluster/raft_state.hpp>
#include <crpc/cluster/registry_log.hpp>
#include <crpc/cluster/registry_service.hpp>
#include <crpc/fs/temp.hpp>

namespace cl = crpc::cluster;
namespace fs = crpc::fs;
using namespace crpc;
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

std::vector<std::string> peers_of_three(const std::string& self) {
    std::vector<std::string> peers;
    for (const char* id : {"A", "B", "C"}) {
        if (self != id) {
            peers.push_back(id);
        }
    }
    return peers;
}

struct cluster_node {
    std::string id;
    manual_clock clock;
    fs::temp_dir state_dir;
    std::unique_ptr<cl::memory_registry_log> log;
    std::unique_ptr<cl::raft_node> raft;
    std::unique_ptr<cl::kv_store> store;
    bool alive = true;
};

// Delivers messages in-process, advancing the sender's clock so a wait bounded
// by the injected clock can make progress. A partition is two-way: a one-way
// cut would let the majority's replies carry the minority leader's entries
// back, which is not a partition.
class test_network {
public:
    cluster_node& add(const std::string& id,
                      const std::vector<std::string>& peers) {
        auto created = fs::temp_dir::create(fs::path(), "crpc-raftapply-");
        nodes_.push_back(std::make_unique<cluster_node>());
        cluster_node& node = *nodes_.back();
        node.id = id;
        if (created.ok()) {
            node.state_dir = std::move(created.value());
        }
        node.log = std::make_unique<cl::memory_registry_log>();
        node.store = std::make_unique<cl::kv_store>();

        cl::raft_config config;
        config.node_id = id;
        config.peers = peers;
        config.election_timeout_min = 40ms;
        config.election_timeout_max = 60ms;
        config.heartbeat_interval = 20ms;
        node.raft = std::make_unique<cl::raft_node>(
            config, *node.log, node.state_dir.native(), make_clock(node.clock));
        node.raft->set_send_hook([this](const cl::raft_message& msg) {
            deliver(msg);
        });
        return node;
    }

    cluster_node& add(const std::string& id) {
        return add(id, peers_of_three(id));
    }

    cluster_node* find(const std::string& id) {
        for (const auto& node : nodes_) {
            if (node->id == id) {
                return node.get();
            }
        }
        return nullptr;
    }

    void deliver(const cl::raft_message& msg) {
        if (partitioned(msg.from) || partitioned(msg.to)) {
            return;
        }
        for (const auto& node : nodes_) {
            if (!node->alive || node->id != msg.to) {
                continue;
            }
            if (cluster_node* sender = find(msg.from)) {
                sender->clock.advance(network_delay);
            }
            node->raft->handle(msg);
            return;
        }
    }

    std::chrono::milliseconds network_delay{5};

    bool partitioned(const std::string& id) const {
        return std::find(partitioned_.begin(), partitioned_.end(), id) !=
               partitioned_.end();
    }

    void partition(const std::vector<std::string>& ids) { partitioned_ = ids; }

    void heal() { partitioned_.clear(); }

    // Drives every node's election timer until one becomes leader. Returns the
    // leader's id, or an empty string when none was elected.
    std::string elect() {
        for (int round = 0; round < 200; ++round) {
            for (const auto& node : nodes_) {
                if (!node->alive) {
                    continue;
                }
                node->clock.advance(10ms);
                node->raft->tick();
                if (node->raft->role() == cl::raft_role::leader) {
                    return node->id;
                }
            }
        }
        return {};
    }

    std::vector<std::unique_ptr<cluster_node>>& nodes() { return nodes_; }

private:
    std::vector<std::unique_ptr<cluster_node>> nodes_;
    std::vector<std::string> partitioned_;
};

// A service attached to one cluster node. The raft node commits; the service
// applies. Two services over two nodes' logs are two independent state
// machines over the same command stream -- which is exactly the property under
// test.
struct service_node {
    std::unique_ptr<cl::registry_service> service;
    fs::temp_dir state_dir;

    bool start(cluster_node& node, cl::kv_store& store,
               const std::vector<cl::acl_rule>& seed) {
        auto created = fs::temp_dir::create(fs::path(), "crpc-raftapply-svc-");
        if (!created.ok()) {
            return false;
        }
        state_dir = std::move(created.value());

        cl::registry_service_config config;
        config.state_dir = state_dir.native();
        config.listen = crpc::endpoint{};
        config.kv = &store;
        config.raft = node.raft.get();
        config.registry.admin_token = "admin-secret";
        config.acl = seed;

        service = std::make_unique<cl::registry_service>(std::move(config));
        crpc::server::builder builder;
        return service->prepare(builder).ok();
    }
};

cl::acl_rule writer_rule(const std::string& token) {
    cl::acl_rule rule;
    rule.token_hash = fnv1a(token);
    rule.principal_id = "node-A";
    rule.read_prefixes.push_back("");
    rule.write_prefixes.push_back("");
    return rule;
}

} // namespace

// --- The KV value reaches every node -----------------------------------------

CRPC_TEST(a_kv_put_reaches_every_node) {
    test_network net;
    for (const char* id : {"A", "B", "C"}) {
        net.add(id);
    }
    for (auto& node : net.nodes()) {
        CRPC_REQUIRE(node->raft->start().ok());
    }
    const std::string leader = net.elect();
    CRPC_REQUIRE(!leader.empty());

    std::vector<std::unique_ptr<service_node>> services;
    const std::vector<cl::acl_rule> seed{writer_rule("token-A")};
    for (const char* id : {"A", "B", "C"}) {
        cluster_node* node = net.find(id);
        CRPC_REQUIRE(node != nullptr);
        auto attached = std::make_unique<service_node>();
        CRPC_REQUIRE(attached->start(*node, *node->store, seed));
        services.push_back(std::move(attached));
    }

    // The write goes through the leader's service. The followers' services
    // apply it when their commit index moves, which the delivered AppendEntries
    // does on its own: `handle` advances `commit_index_` from `leader_commit`.
    service_node& leader_service = *services[leader == "A" ? 0 : (leader == "B" ? 1 : 2)];
    CRPC_REQUIRE(leader_service.service->set_acl_rule(writer_rule("token-A")).ok());

    cl::raft_command command;
    command.kind = cl::raft_command_kind::kv_put;
    command.key = "shared";
    command.value = {std::byte{'v'}, std::byte{'1'}};
    // Drive the same path `on_kv_put` uses: commit through raft, then apply.
    std::vector<std::byte> payload;
    CRPC_REQUIRE(cl::encode_raft_command(command, payload).ok());
    const auto proposed = net.find(leader)->raft->propose(
        crpc::span<const std::byte>(payload.data(), payload.size()));
    CRPC_REQUIRE(proposed.ok());
    const auto committed = net.find(leader)->raft->wait_for_commit(
        proposed.value(), std::chrono::steady_clock::now() + 5s);
    CRPC_REQUIRE(committed.ok());

    // The follower's commit index advanced during delivery; its service applies
    // on the next call. Delivering a heartbeat AFTER the commit is what makes
    // the follower learn the new commit index -- a real cluster does this on the
    // next heartbeat, and the test does it explicitly rather than sleeping.
    for (auto& node : net.nodes()) {
        if (node->id == leader) {
            continue;
        }
        cl::raft_message beat;
        beat.kind = cl::raft_message::type::append;
        beat.term = net.find(leader)->raft->term();
        beat.from = leader;
        beat.to = node->id;
        beat.prev_log_index = node->raft->last_log_index();
        beat.prev_log_term = node->raft->entry_term(beat.prev_log_index);
        beat.leader_commit = net.find(leader)->raft->commit_index();
        node->raft->handle(beat);
    }

    for (auto& attached : services) {
        CRPC_REQUIRE(attached->service->apply_committed().ok());
    }

    // The assertion: every node holds the value, at the same index.
    for (auto& node : net.nodes()) {
        const auto entry = node->store->get("shared");
        CRPC_REQUIRE(entry.ok());
        CRPC_CHECK_EQ(std::size_t{2}, entry.value().value.size());
        CRPC_CHECK(entry.value().value[0] == std::byte{'v'});
        CRPC_CHECK(entry.value().value[1] == std::byte{'1'});
        CRPC_CHECK(entry.value().modify_index > 0);
    }
}

// --- An ACL change reaches every node ----------------------------------------

CRPC_TEST(an_acl_upsert_and_remove_reach_every_node) {
    test_network net;
    for (const char* id : {"A", "B", "C"}) {
        net.add(id);
    }
    for (auto& node : net.nodes()) {
        CRPC_REQUIRE(node->raft->start().ok());
    }
    const std::string leader = net.elect();
    CRPC_REQUIRE(!leader.empty());

    const std::vector<cl::acl_rule> seed{writer_rule("token-A")};
    std::vector<std::unique_ptr<service_node>> services;
    for (const char* id : {"A", "B", "C"}) {
        auto attached = std::make_unique<service_node>();
        CRPC_REQUIRE(attached->start(*net.find(id), *net.find(id)->store, seed));
        services.push_back(std::move(attached));
    }

    // Add a second rule through the leader.
    cl::acl_rule added;
    added.token_hash = fnv1a("token-NEW");
    added.principal_id = "node-NEW";
    added.read_prefixes.push_back("svc/");
    added.write_prefixes.push_back("svc/");
    const std::size_t leader_index =
        leader == "A" ? 0 : (leader == "B" ? 1 : 2);
    CRPC_REQUIRE(services[leader_index]->service->set_acl_rule(added).ok());

    // Push the new commit index to every follower and apply.
    for (auto& node : net.nodes()) {
        if (node->id != leader) {
            cl::raft_message beat;
            beat.kind = cl::raft_message::type::append;
            beat.term = net.find(leader)->raft->term();
            beat.from = leader;
            beat.to = node->id;
            beat.prev_log_index = node->raft->last_log_index();
            beat.prev_log_term = node->raft->entry_term(beat.prev_log_index);
            beat.leader_commit = net.find(leader)->raft->commit_index();
            node->raft->handle(beat);
        }
    }
    for (auto& attached : services) {
        CRPC_REQUIRE(attached->service->apply_committed().ok());
    }

    // Two rules on every node, and the new one is usable for a check.
    for (auto& attached : services) {
        const std::vector<cl::acl_rule> rules = attached->service->acl_rules();
        CRPC_REQUIRE(rules.size() == 2);
        bool found_new = false;
        for (const cl::acl_rule& rule : rules) {
            if (rule.token_hash == added.token_hash) {
                found_new = true;
                CRPC_CHECK_EQ(std::string("node-NEW"), rule.principal_id);
                CRPC_REQUIRE(rule.write_prefixes.size() == 1);
                CRPC_CHECK_EQ(std::string("svc/"), rule.write_prefixes[0]);
            }
        }
        CRPC_CHECK(found_new);
    }

    // Remove it again; the table must shrink on every node.
    CRPC_REQUIRE(
        services[leader_index]->service->remove_acl_rule(added.token_hash).ok());
    for (auto& node : net.nodes()) {
        if (node->id != leader) {
            cl::raft_message beat;
            beat.kind = cl::raft_message::type::append;
            beat.term = net.find(leader)->raft->term();
            beat.from = leader;
            beat.to = node->id;
            beat.prev_log_index = node->raft->last_log_index();
            beat.prev_log_term = node->raft->entry_term(beat.prev_log_index);
            beat.leader_commit = net.find(leader)->raft->commit_index();
            node->raft->handle(beat);
        }
    }
    for (auto& attached : services) {
        CRPC_REQUIRE(attached->service->apply_committed().ok());
    }
    for (auto& attached : services) {
        const std::vector<cl::acl_rule> rules = attached->service->acl_rules();
        CRPC_REQUIRE(rules.size() == 1);
        CRPC_CHECK(rules[0].token_hash == fnv1a("token-A"));
    }
}

// --- Apply is idempotent ------------------------------------------------------

CRPC_TEST(applying_twice_does_not_duplicate_an_acl_rule) {
    // A restart replays from zero, and a leader that applies after every write
    // calls this repeatedly. Neither may add a rule twice.
    test_network net;
    const char* only = "A";
    net.add(only, {});
    CRPC_REQUIRE(net.find(only)->raft->start().ok());
    (void)net.elect();

    service_node attached;
    CRPC_REQUIRE(attached.start(*net.find(only), *net.find(only)->store,
                                std::vector<cl::acl_rule>{}));
    cl::acl_rule rule = writer_rule("token-A");
    CRPC_REQUIRE(attached.service->set_acl_rule(rule).ok());
    CRPC_REQUIRE(attached.service->apply_committed().ok());
    CRPC_REQUIRE(attached.service->apply_committed().ok());
    CRPC_CHECK_EQ(std::size_t{1}, attached.service->acl_rules().size());
}

CRPC_TEST(an_acl_upsert_replaces_an_existing_rule_in_place) {
    // The same token hash is the identity of a rule. A second upsert must
    // REPLACE it, not append a duplicate: two rules with one hash would make
    // `check_acl`'s first-match scan depend on insertion order, and a removal
    // would leave the stale copy behind.
    test_network net;
    const char* only = "A";
    net.add(only, {});
    CRPC_REQUIRE(net.find(only)->raft->start().ok());
    (void)net.elect();

    service_node attached;
    CRPC_REQUIRE(attached.start(*net.find(only), *net.find(only)->store,
                                std::vector<cl::acl_rule>{}));

    cl::acl_rule first = writer_rule("token-A");
    first.read_prefixes = {"old/"};
    CRPC_REQUIRE(attached.service->set_acl_rule(first).ok());

    cl::acl_rule second = writer_rule("token-A");
    second.read_prefixes = {"new/"};
    second.admin = true;
    CRPC_REQUIRE(attached.service->set_acl_rule(second).ok());

    const std::vector<cl::acl_rule> rules = attached.service->acl_rules();
    CRPC_REQUIRE(rules.size() == 1);
    CRPC_CHECK(rules[0].admin);
    CRPC_REQUIRE(rules[0].read_prefixes.size() == 1);
    CRPC_CHECK_EQ(std::string("new/"), rules[0].read_prefixes[0]);
}

// --- A missing command is a hard failure, not a silent skip -------------------

CRPC_TEST(an_undecodable_committed_entry_is_refused) {
    // A committed entry this node cannot decode means it can never reach the
    // same state as the rest of the cluster. Skipping it would hide that, so the
    // apply loop must refuse and leave the cursor in place.
    test_network net;
    const char* only = "A";
    net.add(only, {});
    CRPC_REQUIRE(net.find(only)->raft->start().ok());
    (void)net.elect();

    service_node attached;
    CRPC_REQUIRE(attached.start(*net.find(only), *net.find(only)->store,
                                std::vector<cl::acl_rule>{}));

    // Propose a payload that is not a valid command: version 1, kind 0x7F.
    // A wrong version would be a capability refusal (`failed_precondition`), so
    // the kind is what this case corrupts -- that is the decode_error path.
    const std::vector<std::byte> garbage{std::byte{0x01}, std::byte{0x00},
                                         std::byte{0x7F}};
    const auto proposed = net.find(only)->raft->propose(
        crpc::span<const std::byte>(garbage.data(), garbage.size()));
    CRPC_REQUIRE(proposed.ok());
    const auto committed = net.find(only)->raft->wait_for_commit(
        proposed.value(), std::chrono::steady_clock::now() + 5s);
    CRPC_REQUIRE(committed.ok());

    const crpc::status applied = attached.service->apply_committed();
    CRPC_CHECK(!applied.ok());
    CRPC_CHECK_EQ(crpc::rpc_code::decode_error, applied.code());
}

// --- The HTTP facade's KV write reaches every node ---------------------------

CRPC_TEST(a_kv_put_through_the_http_facade_reaches_every_node) {
    // The end-to-end path: `consul_http` -> `registry_service::dispatch` ->
    // `on_kv_put` -> `replicate_command` -> raft -> apply. This is the case that
    // fails when the handler proposes an EMPTY payload (the PR-7 defect): the
    // leader's own store is written by the old local path, so a single-node
    // assertion would still pass, and only a follower's store exposes the gap.
    test_network net;
    for (const char* id : {"A", "B", "C"}) {
        net.add(id);
    }
    for (auto& node : net.nodes()) {
        CRPC_REQUIRE(node->raft->start().ok());
    }
    const std::string leader = net.elect();
    CRPC_REQUIRE(!leader.empty());

    const std::vector<cl::acl_rule> seed{writer_rule("token-A")};
    std::vector<std::unique_ptr<service_node>> services;
    std::vector<std::unique_ptr<cl::consul_http>> facades;
    for (const char* id : {"A", "B", "C"}) {
        auto attached = std::make_unique<service_node>();
        CRPC_REQUIRE(attached->start(*net.find(id), *net.find(id)->store, seed));
        facades.push_back(std::make_unique<cl::consul_http>(*attached->service));
        services.push_back(std::move(attached));
    }

    const std::size_t leader_index =
        leader == "A" ? 0 : (leader == "B" ? 1 : 2);

    crpc::http::request put;
    put.method = crpc::http::method::put;
    put.method_token = crpc::http::to_token(crpc::http::method::put);
    put.target = "/v1/kv/via-http";
    put.path = "/v1/kv/via-http";
    put.headers.add("X-Consul-Token", "token-A");
    put.body = "payload";
    crpc::http::response put_response;
    facades[leader_index]->handle_kv(put, "via-http", cl::consul_kv_op::put,
                                     put_response);
    CRPC_REQUIRE(put_response.status_code == 200);

    // Push the new commit index to the followers, then let their services apply.
    for (auto& node : net.nodes()) {
        if (node->id != leader) {
            cl::raft_message beat;
            beat.kind = cl::raft_message::type::append;
            beat.term = net.find(leader)->raft->term();
            beat.from = leader;
            beat.to = node->id;
            beat.prev_log_index = node->raft->last_log_index();
            beat.prev_log_term = node->raft->entry_term(beat.prev_log_index);
            beat.leader_commit = net.find(leader)->raft->commit_index();
            node->raft->handle(beat);
        }
    }
    for (auto& attached : services) {
        CRPC_REQUIRE(attached->service->apply_committed().ok());
    }

    for (auto& node : net.nodes()) {
        const auto entry = node->store->get("via-http");
        CRPC_REQUIRE(entry.ok());
        CRPC_CHECK_EQ(std::string("payload"),
                      std::string(reinterpret_cast<const char*>(
                                      entry.value().value.data()),
                                  entry.value().value.size()));
    }
}

CRPC_TEST_MAIN()
