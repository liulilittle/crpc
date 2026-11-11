// CRPC -- G19 step3 PR-5: the Raft-backed fence authority (S4).
//
// S4 is the partition contract: a write is committed only after a majority of
// nodes have durably accepted it, and a leader that cannot reach a quorum
// steps down and fails the write loudly rather than acknowledging it.
//
// The cases here drive a real `raft_node` cluster through an in-process
// transport, so the timing is deterministic. The authority under test is the
// adapter that turns "allocate a fence" into "propose, wait for commit, return
// the token".
#include "test_framework.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <crpc/cluster/raft_fence_authority.hpp>
#include <crpc/cluster/raft_node.hpp>
#include <crpc/cluster/raft_state.hpp>
#include <crpc/cluster/registry.hpp>
#include <crpc/cluster/registry_log.hpp>
#include <crpc/fs/temp.hpp>

namespace cl = crpc::cluster;
namespace fs = crpc::fs;
using namespace crpc;
using namespace std::chrono_literals;

namespace {

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

struct cluster_node {
    std::string id;
    manual_clock clock;
    fs::temp_dir state_dir;
    std::unique_ptr<cl::memory_registry_log> log;
    std::unique_ptr<cl::raft_node> raft;
    bool alive = true;
};

// Delivers messages between nodes in-process. A partition is a set of node ids
// that cannot be reached in EITHER direction: a one-way partition would let the
// majority's replies carry the minority leader's entries back, which is not a
// partition at all.
class test_network {
public:
    cluster_node& add(const std::string& id, const std::vector<std::string>& peers,
                      std::chrono::milliseconds election_min,
                      std::chrono::milliseconds election_max) {
        auto created = fs::temp_dir::create(fs::path(), "crpc-raftfence-");
        nodes_.push_back(std::make_unique<cluster_node>());
        cluster_node& node = *nodes_.back();
        node.id = id;
        if (created.ok()) {
            node.state_dir = std::move(created.value());
        }
        node.log = std::make_unique<cl::memory_registry_log>();

        cl::raft_config config;
        config.node_id = id;
        config.peers = peers;
        config.election_timeout_min = election_min;
        config.election_timeout_max = election_max;
        config.heartbeat_interval = 20ms;
        node.raft = std::make_unique<cl::raft_node>(
            config, *node.log, node.state_dir.native(), make_clock(node.clock));
        node.raft->set_send_hook([this](const cl::raft_message& msg) {
            deliver(msg);
        });
        return node;
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
            // Model a network round trip: the sender's clock moves while the
            // message is in flight. Without this a manual clock never advances
            // during a write, and a commit-latency measurement would always be
            // zero -- indistinguishable from "never recorded".
            if (cluster_node* sender = find(msg.from)) {
                sender->clock.advance(network_delay);
            }
            node->raft->handle(msg);
            return;
        }
    }

    // One-way delay applied to the sender's clock on every delivery.
    std::chrono::milliseconds network_delay{5};

    bool partitioned(const std::string& id) const {
        return std::find(partitioned_.begin(), partitioned_.end(), id) !=
               partitioned_.end();
    }

    void partition(const std::vector<std::string>& ids) { partitioned_ = ids; }

    void heal() { partitioned_.clear(); }

    void kill(const std::string& id) {
        if (cluster_node* node = find(id)) {
            node->alive = false;
        }
    }

    std::size_t size() const { return nodes_.size(); }

private:
    std::vector<std::unique_ptr<cluster_node>> nodes_;
    std::vector<std::string> partitioned_;
};

std::vector<std::string> peers_of_three(const std::string& id) {
    static const char* kAll[] = {"A", "B", "C"};
    std::vector<std::string> peers;
    for (const char* candidate : kAll) {
        if (id != candidate) {
            peers.emplace_back(candidate);
        }
    }
    return peers;
}

// Drives an election to completion and returns the leader id.
std::string elect(test_network& net, std::chrono::milliseconds timeout = 400ms) {
    for (int round = 0; round < 12; ++round) {
        for (const char* id : {"A", "B", "C"}) {
            cluster_node* node = net.find(id);
            if (node == nullptr || !node->alive) {
                continue;
            }
            if (node->raft->role() == cl::raft_role::leader) {
                return id;
            }
            node->clock.advance(timeout / 12);
            node->raft->tick();
        }
    }
    return {};
}

} // namespace

// --- S4: a write needs a majority -------------------------------------------

CRPC_TEST(raft_write_needs_majority) {
    test_network net;
    net.add("A", peers_of_three("A"), 40ms, 60ms);
    net.add("B", peers_of_three("B"), 40ms, 60ms);
    net.add("C", peers_of_three("C"), 40ms, 60ms);
    for (const char* id : {"A", "B", "C"}) {
        CRPC_REQUIRE(net.find(id)->raft->start().ok());
    }
    const std::string leader = elect(net);
    CRPC_REQUIRE(!leader.empty());

    // A healthy 3-node cluster: the leader plus one follower is a majority.
    // The entry must commit, and the commit index must reach it.
    cluster_node* node = net.find(leader);
    const std::byte payload[2] = {std::byte{0x01}, std::byte{0x02}};
    const auto proposed = node->raft->propose(span<const std::byte>(payload, 2));
    CRPC_REQUIRE(proposed.ok());

    const auto deadline = node->clock.now + 2000ms;
    const auto committed = node->raft->wait_for_commit(proposed.value(), deadline);
    CRPC_CHECK(committed.ok());
    if (committed.ok()) {
        CRPC_CHECK(committed.value() >= proposed.value());
    }
    CRPC_CHECK(node->raft->commit_index() >= proposed.value());
}

// --- S4: a minority leader steps down ---------------------------------------

CRPC_TEST(raft_minority_leader_steps_down) {
    test_network net;
    net.add("A", peers_of_three("A"), 40ms, 60ms);
    net.add("B", peers_of_three("B"), 40ms, 60ms);
    net.add("C", peers_of_three("C"), 40ms, 60ms);
    for (const char* id : {"A", "B", "C"}) {
        CRPC_REQUIRE(net.find(id)->raft->start().ok());
    }
    const std::string leader = elect(net);
    CRPC_REQUIRE(!leader.empty());

    // Isolate the leader from both followers. It is now a minority of one.
    net.partition({leader});
    cluster_node* node = net.find(leader);

    // Give it more than one election timeout of ticks. A leader that cannot
    // reach a quorum must not stay a leader: it would keep acknowledging writes
    // it can never commit.
    for (int step = 0; step < 200; ++step) {
        node->clock.advance(1ms);
        node->raft->tick();
    }
    CRPC_CHECK(node->raft->role() != cl::raft_role::leader);
}

// --- S4: a minority write fails loudly --------------------------------------

CRPC_TEST(raft_minority_write_fails_loudly) {
    test_network net;
    net.add("A", peers_of_three("A"), 40ms, 60ms);
    net.add("B", peers_of_three("B"), 40ms, 60ms);
    net.add("C", peers_of_three("C"), 40ms, 60ms);
    for (const char* id : {"A", "B", "C"}) {
        CRPC_REQUIRE(net.find(id)->raft->start().ok());
    }
    const std::string leader = elect(net);
    CRPC_REQUIRE(!leader.empty());

    net.partition({leader});
    cluster_node* node = net.find(leader);

    // The write must not be acknowledged. `propose` may still append locally,
    // but `wait_for_commit` must fail: the entry can never reach a majority.
    const std::byte payload[2] = {std::byte{0x03}, std::byte{0x04}};
    const auto proposed = node->raft->propose(span<const std::byte>(payload, 2));
    if (proposed.ok()) {
        const auto deadline = node->clock.now + 100ms;
        const auto committed =
            node->raft->wait_for_commit(proposed.value(), deadline);
        CRPC_CHECK_FALSE(committed.ok());
        if (!committed.ok()) {
            CRPC_CHECK_EQ(rpc_code::unavailable, committed.error().code);
        }
    } else {
        // A leader that already stepped down refuses at propose time, which is
        // also a loud failure.
        CRPC_CHECK_EQ(rpc_code::unavailable, proposed.error().code);
    }
}

// --- S4: the commit latency is reported -------------------------------------

CRPC_TEST(raft_commit_latency_reported) {
    test_network net;
    net.add("A", peers_of_three("A"), 40ms, 60ms);
    net.add("B", peers_of_three("B"), 40ms, 60ms);
    net.add("C", peers_of_three("C"), 40ms, 60ms);
    for (const char* id : {"A", "B", "C"}) {
        CRPC_REQUIRE(net.find(id)->raft->start().ok());
    }
    const std::string leader = elect(net);
    CRPC_REQUIRE(!leader.empty());

    cluster_node* node = net.find(leader);
    cl::registry catalog;
    cl::raft_fence_authority authority;
    authority.node = node->raft.get();
    authority.catalog = &catalog;
    authority.clock = make_clock(node->clock);
    authority.commit_timeout = 2000ms;

    cl::fence_request request{"svc", "node-1", 0, 0};
    const auto granted = authority.allocate(request);
    CRPC_REQUIRE(granted.ok());
    CRPC_CHECK(authority.last.valid);
    CRPC_CHECK_EQ(leader, authority.last.leader_id);

    // The latency must be a real measurement, not a field that is always zero.
    // The transport advances the sender's clock on every delivery, so a write
    // that reaches a majority costs at least one network delay. A mutant that
    // drops the assignment leaves the field at zero and fails.
    CRPC_CHECK(authority.last.commit_latency_ms >= 1);
    CRPC_CHECK(authority.last.commit_latency_ms <= 2000);
}

// --- S4: a partition never acknowledges an uncommitted write ----------------

CRPC_TEST(raft_partition_never_acknowledges_uncommitted) {
    test_network net;
    net.add("A", peers_of_three("A"), 40ms, 60ms);
    net.add("B", peers_of_three("B"), 40ms, 60ms);
    net.add("C", peers_of_three("C"), 40ms, 60ms);
    for (const char* id : {"A", "B", "C"}) {
        CRPC_REQUIRE(net.find(id)->raft->start().ok());
    }
    const std::string leader = elect(net);
    CRPC_REQUIRE(!leader.empty());

    net.partition({leader});
    cluster_node* node = net.find(leader);
    cl::registry catalog;
    cl::raft_fence_authority authority;
    authority.node = node->raft.get();
    authority.catalog = &catalog;
    authority.clock = make_clock(node->clock);
    authority.commit_timeout = 100ms;

    // Every attempt during the partition must fail. A single success would be
    // an acknowledgement of a write that can never commit.
    for (int attempt = 0; attempt < 3; ++attempt) {
        cl::fence_request request{"svc", "node-1", 0, 0};
        const auto granted = authority.allocate(request);
        CRPC_CHECK_FALSE(granted.ok());
        if (!granted.ok()) {
            CRPC_CHECK_EQ(rpc_code::unavailable, granted.error().code);
        }
    }
    CRPC_CHECK_FALSE(authority.last.valid);
}

// --- S4: no two leaders in the same term ------------------------------------

CRPC_TEST(raft_no_two_leaders_same_term) {
    test_network net;
    net.add("A", peers_of_three("A"), 40ms, 60ms);
    net.add("B", peers_of_three("B"), 40ms, 60ms);
    net.add("C", peers_of_three("C"), 40ms, 60ms);
    for (const char* id : {"A", "B", "C"}) {
        CRPC_REQUIRE(net.find(id)->raft->start().ok());
    }
    const std::string leader = elect(net);
    CRPC_REQUIRE(!leader.empty());
    const u64 term = net.find(leader)->raft->term();

    // The precise claim is "at most one leader per term", not "at most one
    // leader at any instant": a deposed leader may still believe it leads
    // until it hears a higher term, and Raft permits that. What it forbids is
    // two leaders that both claim the SAME term.
    std::size_t leaders_in_term = 0;
    for (const char* id : {"A", "B", "C"}) {
        cluster_node* node = net.find(id);
        if (node->raft->role() == cl::raft_role::leader &&
            node->raft->term() == term) {
            ++leaders_in_term;
        }
    }
    CRPC_CHECK_EQ(std::size_t{1}, leaders_in_term);
}

// --- S4: the vote is persisted ----------------------------------------------

CRPC_TEST(raft_vote_persisted) {
    test_network net;
    net.add("A", peers_of_three("A"), 40ms, 60ms);
    net.add("B", peers_of_three("B"), 40ms, 60ms);
    net.add("C", peers_of_three("C"), 40ms, 60ms);
    for (const char* id : {"A", "B", "C"}) {
        CRPC_REQUIRE(net.find(id)->raft->start().ok());
    }
    const std::string leader = elect(net);
    CRPC_REQUIRE(!leader.empty());

    // A follower that voted in term T must not vote for a different candidate
    // in T after a restart. The persisted (term, votedFor) is what prevents it.
    cluster_node* follower = nullptr;
    for (const char* id : {"A", "B", "C"}) {
        if (id != leader) {
            follower = net.find(id);
            break;
        }
    }
    CRPC_REQUIRE(follower != nullptr);
    const u64 term = follower->raft->term();

    // Restart the follower from the same state directory and log.
    cl::raft_config config;
    config.node_id = follower->id;
    config.peers = peers_of_three(follower->id);
    config.election_timeout_min = 40ms;
    config.election_timeout_max = 60ms;
    config.heartbeat_interval = 20ms;
    cl::raft_node restarted(config, *follower->log, follower->state_dir.native(),
                            make_clock(follower->clock));
    CRPC_REQUIRE(restarted.start().ok());
    CRPC_CHECK_EQ(term, restarted.term());
}

// --- S4: a leadership loss ends the wait immediately ------------------------

CRPC_TEST(raft_wait_for_commit_fails_on_leadership_loss) {
    test_network net;
    net.add("A", peers_of_three("A"), 40ms, 60ms);
    net.add("B", peers_of_three("B"), 40ms, 60ms);
    net.add("C", peers_of_three("C"), 40ms, 60ms);
    for (const char* id : {"A", "B", "C"}) {
        CRPC_REQUIRE(net.find(id)->raft->start().ok());
    }
    const std::string leader = elect(net);
    CRPC_REQUIRE(!leader.empty());
    cluster_node* node = net.find(leader);

    // Isolate the leader FIRST, so the entry it appends can never reach a
    // majority. Proposing before the partition would let the synchronous
    // transport commit it immediately, and the wait would return success
    // without ever exercising the leadership-loss path.
    net.partition({leader});
    const std::byte payload[2] = {std::byte{0x05}, std::byte{0x06}};
    const auto proposed = node->raft->propose(span<const std::byte>(payload, 2));
    CRPC_REQUIRE(proposed.ok());

    // Tick past CheckQuorum so the node steps down.
    for (int step = 0; step < 200; ++step) {
        node->clock.advance(1ms);
        node->raft->tick();
    }
    CRPC_REQUIRE(node->raft->role() != cl::raft_role::leader);

    // The wait must fail at once, not spin until the deadline: a deposed
    // leader that keeps waiting would report a commit a new leader may never
    // make. The deadline is far in the future, so only the role check can end
    // this call -- and the message must say so, because "timed out" and
    // "leadership lost" are different failures to an operator.
    const auto deadline = node->clock.now + 100000ms;
    const auto committed = node->raft->wait_for_commit(proposed.value(), deadline);
    CRPC_CHECK_FALSE(committed.ok());
    if (!committed.ok()) {
        CRPC_CHECK_EQ(rpc_code::unavailable, committed.error().code);
        CRPC_CHECK(committed.error().message.find("leadership lost") !=
                   std::string::npos);
    }
}

CRPC_TEST_MAIN()
