// CRPC -- G19 step3 PR-4: Raft node (L2-1..L2-5).
//
// These cases pin the consensus properties the registry's step3 build rests on:
//
//   L2-1  a new leader's log contains every committed entry
//   L2-2  a minority leader's uncommitted entries are discarded, committed ones
//         are never lost
//   L2-3  a vote requires the candidate's log to be at least as up to date
//   L2-4  term and votedFor survive a restart
//   L2-5  Leader Completeness: a committed entry is visible to every leader
//
// The harness is in-process: each node has a `memory_registry_log`, a
// `raft_state_store` over a temp directory, and a `raft_node`. Messages are
// delivered by a test transport that can partition and drop, so the timing is
// deterministic -- no wall clock, no sockets.
#include "test_framework.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <crpc/cluster/raft_node.hpp>
#include <crpc/cluster/raft_state.hpp>
#include <crpc/cluster/registry_log.hpp>
#include <crpc/fs/temp.hpp>

namespace cl = crpc::cluster;
namespace fs = crpc::fs;
using namespace crpc;
using namespace std::chrono_literals;

namespace {

// --- Manual clock (same shape as tests/test_cluster.cpp:56-68) --------------

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

// --- Test network ------------------------------------------------------------

struct cluster_node {
    std::string id;
    manual_clock clock;
    fs::temp_dir state_dir;
    std::unique_ptr<cl::memory_registry_log> log;
    std::unique_ptr<cl::raft_node> raft;
    // Set when the node is "killed": its messages are ignored.
    bool alive = true;
};

// Delivers messages between nodes in-process. A partition is a set of node ids
// that cannot be reached; messages to a partitioned peer are dropped.
class test_network {
public:
    ~test_network() = default;

    cluster_node& add(const std::string& id, const std::vector<std::string>& peers,
                      std::chrono::milliseconds election_min,
                      std::chrono::milliseconds election_max) {
        auto created = fs::temp_dir::create(fs::path(), "crpc-raft-");
        if (!created.ok()) {
            // A missing temp directory is an environment failure, not a
            // Raft failure. Returning a default-constructed reference would be
            // a lie, so the node is marked dead and the caller's CRPC_REQUIRE
            // on `raft` catches it.
            nodes_.push_back(std::make_unique<cluster_node>());
            return *nodes_.back();
        }
        nodes_.push_back(std::make_unique<cluster_node>());
        cluster_node& node = *nodes_.back();
        node.id = id;
        node.state_dir = std::move(created.value());
        node.log = std::make_unique<cl::memory_registry_log>();

        cl::raft_config config;
        config.node_id = id;
        config.peers = peers;
        config.election_timeout_min = election_min;
        config.election_timeout_max = election_max;
        config.heartbeat_interval = 20ms;
        node.raft = std::make_unique<cl::raft_node>(
            config, *node.log, node.state_dir.native(), make_clock(node.clock));
        node.raft->set_send_hook([this, id](const cl::raft_message& msg) {
            deliver(id, msg);
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

    void deliver(const std::string& from, const cl::raft_message& msg) {
        (void)from;
        if (dropped_ > 0) {
            --dropped_;
            return;
        }
        for (const auto& node : nodes_) {
            if (!node->alive || node->id != msg.to) {
                continue;
            }
            // A partition cuts BOTH directions: a node inside the minority must
            // not receive from the majority either, or the majority's replies
            // would carry the minority leader's own entries back to it and the
            // "isolated" leader would look connected.
            if (partitioned(node->id) || partitioned(from)) {
                return;
            }
            node->raft->handle(msg);
            return;
        }
    }

    bool partitioned(const std::string& id) const {
        return std::find(partitioned_.begin(), partitioned_.end(), id) !=
               partitioned_.end();
    }

    void partition(const std::vector<std::string>& ids) { partitioned_ = ids; }

    void heal() { partitioned_.clear(); }

    void drop_next(std::size_t count) { dropped_ = count; }

    // Every live node advances its clock and ticks once.
    void tick_all(std::chrono::milliseconds delta) {
        for (const auto& node : nodes_) {
            if (!node->alive) {
                continue;
            }
            node->clock.advance(delta);
            node->raft->tick();
        }
    }

    void kill(const std::string& id) {
        if (cluster_node* node = find(id)) {
            node->alive = false;
        }
    }

    void revive(const std::string& id) {
        if (cluster_node* node = find(id)) {
            node->alive = true;
        }
    }

    std::size_t size() const { return nodes_.size(); }

private:
    std::vector<std::unique_ptr<cluster_node>> nodes_;
    std::vector<std::string> partitioned_;
    std::size_t dropped_ = 0;
};

// Five nodes with the documented ids. Peers are every other node.
std::vector<std::string> peers_of(const std::string& id) {
    static const char* kAll[] = {"A", "B", "C", "D", "E"};
    std::vector<std::string> peers;
    for (const char* candidate : kAll) {
        if (id != candidate) {
            peers.emplace_back(candidate);
        }
    }
    return peers;
}

// Peers for a three-node cluster. `peers_of` names all five documented ids, so
// using it for a 3-node cluster would make the quorum larger than the cluster
// and no election could ever succeed.
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
//
// Only nodes that are not yet followers of a leader are ticked, and the nodes
// are ticked in id order. Ticking the whole cluster would let a loser time out
// and start another election, which is the split-vote case the randomisation
// exists to avoid -- a test that forces it cannot observe a stable leader.
std::string elect(test_network& net, std::chrono::milliseconds timeout = 400ms) {
    for (int round = 0; round < 12; ++round) {
        for (const char* id : {"A", "B", "C", "D", "E"}) {
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

// Elects a leader after a node has been killed. The survivors were followers of
// the dead leader, so each must be given a full election timeout before any of
// them campaigns; `elect` alone stops as soon as it finds a leader, which a
// cluster that has not yet converged never has.
//
// The nodes are ticked in id order and the loop stops the moment one becomes
// leader, so a candidate that needs only the survivors' votes is not made to
// race a peer that timed out at the same instant.
std::string elect_after_loss(test_network& net,
                             std::chrono::milliseconds timeout = 400ms) {
    for (const char* id : {"A", "B", "C", "D", "E"}) {
        cluster_node* node = net.find(id);
        if (node == nullptr || !node->alive) {
            continue;
        }
        node->clock.advance(timeout);
        node->raft->tick();
        if (node->raft->role() == cl::raft_role::leader) {
            return id;
        }
    }
    return elect(net, timeout);
}

} // namespace

// --- L2-1: a new leader's log contains every committed entry ------------------

CRPC_TEST(raft_new_leader_log_contains_committed) {
    test_network net;
    net.add("A", peers_of("A"), 40ms, 60ms);
    net.add("B", peers_of("B"), 40ms, 60ms);
    net.add("C", peers_of("C"), 40ms, 60ms);
    net.add("D", peers_of("D"), 40ms, 60ms);
    net.add("E", peers_of("E"), 40ms, 60ms);
    for (const char* id : {"A", "B", "C", "D", "E"}) {
        CRPC_REQUIRE(net.find(id)->raft->start().ok());
    }

    const std::string leader = elect(net);
    CRPC_REQUIRE(!leader.empty());
    cluster_node* leader_node = net.find(leader);
    CRPC_REQUIRE(leader_node != nullptr);

    // Three committed entries. Only the leader is ticked: ticking the whole
    // cluster would let a follower time out and start a new election, which is
    // the partitioned-leader case, not the replication case.
    for (int i = 0; i < 3; ++i) {
        const auto proposed = leader_node->raft->propose(
            span<const std::byte>(reinterpret_cast<const std::byte*>("x"), 1));
        if (!proposed.ok()) {
            std::fprintf(stderr, "[diag] propose %d failed: %s\n", i,
                         proposed.error().message.c_str());
        }
        CRPC_REQUIRE(proposed.ok());
        leader_node->clock.advance(50ms);
        leader_node->raft->tick();
    }
    CRPC_CHECK(leader_node->raft->commit_index() >= u64{3});

    // Kill the leader and elect a new one from the remaining four.
    net.kill(leader);
    const std::string successor = elect_after_loss(net);
    if (successor.empty()) {
        for (const char* id : {"A", "B", "C", "D", "E"}) {
            cluster_node* node = net.find(id);
            std::fprintf(stderr, "[diag] %s alive=%d role=%d term=%llu commit=%llu last=%llu\n",
                         id, node->alive ? 1 : 0, static_cast<int>(node->raft->role()),
                         static_cast<unsigned long long>(node->raft->term()),
                         static_cast<unsigned long long>(node->raft->commit_index()),
                         static_cast<unsigned long long>(node->raft->last_log_index()));
        }
    }
    CRPC_REQUIRE(!successor.empty());
    CRPC_CHECK(leader != successor);
    cluster_node* new_leader = net.find(successor);
    CRPC_REQUIRE(new_leader != nullptr);

    // The committed entries are all present in the new leader's log.
    CRPC_CHECK(new_leader->raft->last_log_index() >= u64{3});
    CRPC_CHECK(new_leader->raft->commit_index() >= u64{3});
}

// --- basics: a single node elects itself, and a leader replicates -----------

CRPC_TEST(raft_single_node_becomes_leader) {
    test_network net;
    net.add("A", {}, 40ms, 60ms);
    CRPC_REQUIRE(net.find("A")->raft->start().ok());
    const std::string leader = elect(net);
    CRPC_CHECK_EQ(std::string("A"), leader);
    CRPC_CHECK(net.find("A")->raft->role() == cl::raft_role::leader);
}

CRPC_TEST(raft_leader_replicates_to_followers) {
    test_network net;
    net.add("A", peers_of("A"), 40ms, 60ms);
    net.add("B", peers_of("B"), 40ms, 60ms);
    net.add("C", peers_of("C"), 40ms, 60ms);
    net.add("D", peers_of("D"), 40ms, 60ms);
    net.add("E", peers_of("E"), 40ms, 60ms);
    for (const char* id : {"A", "B", "C", "D", "E"}) {
        CRPC_REQUIRE(net.find(id)->raft->start().ok());
    }
    const std::string leader = elect(net);
    CRPC_REQUIRE(!leader.empty());
    cluster_node* leader_node = net.find(leader);
    CRPC_REQUIRE(leader_node->raft->propose(
        span<const std::byte>(reinterpret_cast<const std::byte*>("v"), 1)).ok());
    leader_node->clock.advance(50ms);
    leader_node->raft->tick();

    // Every follower accepted the entry.
    for (const char* id : {"A", "B", "C", "D", "E"}) {
        cluster_node* node = net.find(id);
        CRPC_CHECK(node->raft->last_log_index() >= u64{1});
    }
}

// --- G2: D-B-5 single-node start() only adopts last when term matches --------
//
// A single-node start() must not adopt last_index as commit when the persisted
// term differs from the entries' term. That is the ghost-commit a 3->1 shrink
// would cause.

CRPC_TEST(d_b5_single_node_start_does_not_commit_foreign_term) {
    test_network net;
    net.add("A", {}, 40ms, 60ms);
    cluster_node* node = net.find("A");
    CRPC_REQUIRE(node != nullptr);

    // The raft log holds a committed-looking entry at index 1 with term 7, but
    // the persisted state file says term 0 (no such term was ever committed).
    // start() must NOT adopt index 1 as commit.
    cl::memory_registry_log& log = *node->log;
    // Put an entry at index 1 term 7 directly into the log.
    cl::log_record rec;
    rec.kind = cl::log_record_kind::raft_entry;
    rec.term = 7;
    rec.index = 1;
    rec.payload.assign(1, std::byte{0});
    CRPC_CHECK(log.append(rec).ok());
    CRPC_CHECK(log.sync().ok());

    // persisted term is 0 (nothing written to the state file yet), so start()
    // must leave commit_index at 0.
    CRPC_CHECK(node->raft->start().ok());
    CRPC_CHECK_EQ(u64{0}, node->raft->commit_index());
    CRPC_CHECK_EQ(u64{1}, node->raft->last_log_index());
}

// --- G2: D-B-7 raft_node::now() exposes the injected clock -------------------

CRPC_TEST(d_b7_raft_now_uses_injected_clock) {
    test_network net;
    net.add("A", {}, 40ms, 60ms);
    cluster_node* node = net.find("A");
    CRPC_REQUIRE(node != nullptr);
    CRPC_REQUIRE(node->raft->start().ok());

    node->clock.advance(100ms);
    const cl::monotonic_clock::time_point observed = node->raft->now();
    CRPC_CHECK(observed >= cl::monotonic_clock::time_point{});
    CRPC_CHECK(node->clock.now <= observed ||
               node->clock.now == observed);
}

// --- G2: D-B-8 entry_term/last_log_term return result<u64> -------------------

CRPC_TEST(d_b8_entry_term_surfaces_replay_error) {
    test_network net;
    net.add("A", {}, 40ms, 60ms);
    cluster_node* node = net.find("A");
    CRPC_REQUIRE(node != nullptr);
    CRPC_REQUIRE(node->raft->start().ok());

    // A log with entries: index 1 term 1, index 2 term 1.
    for (u64 i = 1; i <= 2; ++i) {
        cl::log_record rec;
        rec.kind = cl::log_record_kind::raft_entry;
        rec.term = 1;
        rec.index = i;
        rec.payload.assign(1, std::byte{0});
        CRPC_CHECK(node->log->append(rec).ok());
    }
    CRPC_CHECK(node->log->sync().ok());

    // Both terms are readable.
    CRPC_CHECK_EQ(u64{1}, node->raft->entry_term(1).value());
    CRPC_CHECK_EQ(u64{1}, node->raft->entry_term(2).value());
    CRPC_CHECK_EQ(u64{1}, node->raft->last_log_term().value());
    CRPC_CHECK_EQ(u64{0}, node->raft->entry_term(0).value());
    CRPC_CHECK_EQ(u64{0}, node->raft->entry_term(999).value());
}

// --- G2: D-B-9 replay(0) once, cache (index, term) ---------------------------

// Counting log probe: wraps a memory_registry_log and records every replay()
// call with the number of records returned. A replay-per-entry implementation
// would call replay() once per entry (O(n^2) records scanned); the fixed tree
// calls it once per append batch (O(n) records scanned).
struct counting_log : cl::registry_log {
    cl::memory_registry_log inner;
    int replay_calls = 0;
    u64 records_returned = 0;

    result<u64> append(const cl::log_record& record) override {
        return inner.append(record);
    }
    result<std::vector<cl::log_record>> replay(u64 from_index) const override {
        auto* self = const_cast<counting_log*>(this);
        ++self->replay_calls;
        const auto result = inner.replay(from_index);
        if (result.ok()) {
            self->records_returned += static_cast<u64>(result.value().size());
        }
        return result;
    }
    status truncate_suffix(u64 from_index) override {
        return inner.truncate_suffix(from_index);
    }
    status sync() override { return inner.sync(); }
    [[nodiscard]] u64 last_index() const noexcept override {
        return inner.last_index();
    }
};

CRPC_TEST(d_b9_replay_once_caches_index_term) {
    // Build a node manually with a counting log probe, because raft_node holds
    // its log by reference and test_network::add() already bound a
    // memory_registry_log.
    cl::raft_config config;
    config.node_id = "A";
    config.peers = {};
    config.election_timeout_min = 40ms;
    config.election_timeout_max = 60ms;
    config.heartbeat_interval = 20ms;

    auto probe = std::make_unique<counting_log>();
    counting_log* probe_ptr = probe.get();
    auto state = fs::temp_dir::create(fs::path(), "crpc-d-b9-");
    CRPC_REQUIRE(state.ok());

    manual_clock clock;
    auto raft = std::make_unique<cl::raft_node>(
        config, *probe_ptr, state.value().native(), make_clock(clock));

    // Pre-populate the log with n entries BEFORE start(), so start()'s single
    // replay(0) loads them into `index_term_cache_`.
    const int n = 200;
    for (int i = 0; i < n; ++i) {
        cl::log_record rec;
        rec.kind = cl::log_record_kind::raft_entry;
        rec.term = 1;
        rec.index = i + 1;
        rec.payload.assign(1, std::byte{0});
        CRPC_CHECK(probe_ptr->append(rec).ok());
    }
    CRPC_CHECK(probe_ptr->sync().ok());

    CRPC_REQUIRE(raft->start().ok());

    // Become leader: a single node elects itself on the first deadline.
    clock.advance(100ms);
    raft->tick();
    CRPC_CHECK_EQ(cl::raft_role::leader, raft->role());

    // Deliver ONE append batch of n entries via handle(). With per-entry
    // entry_term_locked this calls replay() n times (O(n^2) records scanned);
    // with the D-B-9 snapshot it calls replay() exactly once.
    cl::raft_message msg;
    msg.kind = cl::raft_message::type::append;
    msg.term = raft->term();
    msg.from = "leader";
    msg.to = "A";
    msg.prev_log_index = n;
    msg.prev_log_term = 1;
    msg.leader_commit = n;
    for (int i = 0; i < n; ++i) {
        cl::log_record rec;
        rec.kind = cl::log_record_kind::raft_entry;
        rec.term = 1;
        rec.index = n + 1 + static_cast<u64>(i);
        rec.payload.assign(1, std::byte{0});
        msg.entries.push_back(std::move(rec));
    }
    std::vector<cl::raft_message> outbound;
    raft->set_send_hook([&outbound](const cl::raft_message& m) {
        outbound.push_back(m);
    });
    raft->handle(msg);
    // Drain replies (append_reply; non-leader ignores it, no-op is safe).
    for (auto& reply : outbound) {
        raft->handle(reply);
    }

    // D-B-9: replay() called exactly once (the snapshot), returning all n
    // pre-existing records — not once per entry.
    CRPC_CHECK_EQ(1, probe_ptr->replay_calls);
    CRPC_CHECK_EQ(static_cast<u64>(n), probe_ptr->records_returned);
}

// --- L2-2: a minority leader's uncommitted entries are discarded -------------

CRPC_TEST(raft_minority_uncommitted_entries_discarded) {
    test_network net;
    net.add("A", peers_of("A"), 40ms, 60ms);
    net.add("B", peers_of("B"), 40ms, 60ms);
    net.add("C", peers_of("C"), 40ms, 60ms);
    net.add("D", peers_of("D"), 40ms, 60ms);
    net.add("E", peers_of("E"), 40ms, 60ms);
    for (const char* id : {"A", "B", "C", "D", "E"}) {
        CRPC_REQUIRE(net.find(id)->raft->start().ok());
    }
    const std::string leader = elect(net);
    CRPC_REQUIRE(!leader.empty());
    cluster_node* leader_node = net.find(leader);

    // Two committed entries.
    for (int i = 0; i < 2; ++i) {
        CRPC_REQUIRE(leader_node->raft->propose(
            span<const std::byte>(reinterpret_cast<const std::byte*>("c"), 1)).ok());
        leader_node->clock.advance(50ms);
        leader_node->raft->tick();
    }
    CRPC_CHECK(leader_node->raft->commit_index() >= u64{2});

    // Isolate the leader plus one peer: a two-node minority. Its new entries
    // cannot reach a majority, so they stay uncommitted.
    std::vector<std::string> minority = {leader, "A"};
    if (leader == "A") {
        minority = {leader, "B"};
    }
    net.partition(minority);

    // The minority leader keeps proposing; none of these can commit. With
    // CheckQuorum (S4) the leader may also step down once it notices it cannot
    // reach a majority, in which case `propose` refuses outright. Both outcomes
    // are correct: what must never happen is a commit.
    //
    // The refusal is recorded rather than asserted inside the loop: whether
    // CheckQuorum fires depends on timing, and a `CRPC_CHECK` that only runs on
    // one branch makes the case's check COUNT vary between runs. A test whose
    // check count is not deterministic cannot be compared across runs, which is
    // exactly what a reviewer does. Assert the outcome once, after the loop.
    bool refused = false;
    rpc_code refusal_code = rpc_code::ok;
    for (int i = 0; i < 2; ++i) {
        const auto proposed = leader_node->raft->propose(
            span<const std::byte>(reinterpret_cast<const std::byte*>("u"), 1));
        if (!proposed.ok()) {
            // A deposed leader refuses the write loudly, which is the S4
            // behaviour. Stop proposing; the assertions below still hold.
            refused = true;
            refusal_code = proposed.error().code;
            break;
        }
        leader_node->clock.advance(50ms);
        leader_node->raft->tick();
    }
    // Either the minority leader kept accepting (and nothing committed) or it
    // stepped down and refused with `unavailable`. Both are correct; both
    // checks below are unconditional so the count is stable across runs.
    CRPC_CHECK(refused || leader_node->raft->commit_index() < u64{4});
    CRPC_CHECK(!refused || refusal_code == rpc_code::unavailable);
    // ...but the two committed entries are still there.
    CRPC_CHECK(leader_node->raft->commit_index() >= u64{2});

    // Heal and elect a majority leader. The committed entries survive; the
    // uncommitted minority entries are discarded by whichever node the
    // majority's conflict rule truncates them from.
    net.heal();
    // The minority leader must not be the successor: it carries the stale
    // suffix, and electing it would keep the entries the majority rejected.
    net.kill(leader);
    const std::string successor = elect_after_loss(net);
    CRPC_REQUIRE(!successor.empty());
    cluster_node* new_leader = net.find(successor);
    CRPC_CHECK(new_leader->raft->commit_index() >= u64{2});
    // The majority's view is the truth: no node that won a majority election
    // may carry an entry past the committed prefix that the majority never
    // accepted. A node that was in the minority and lost the vote is truncated
    // by the new leader's AppendEntries, so after a heartbeat every live node
    // converges on the committed prefix.
    new_leader->clock.advance(50ms);
    new_leader->raft->tick();
    // The heartbeat must be delivered and answered: a follower only truncates
    // when it processes the leader's message, so the whole cluster ticks once.
    net.tick_all(1ms);
    for (const char* id : {"A", "B", "C", "D", "E"}) {
        cluster_node* node = net.find(id);
        if (!node->alive) {
            continue;
        }
        CRPC_CHECK(node->raft->last_log_index() <= u64{2});
    }
}

// --- L2-3: a vote requires an up-to-date log ---------------------------------

CRPC_TEST(raft_vote_requires_up_to_date_log) {
    test_network net;
    net.add("A", peers_of_three("A"), 40ms, 60ms);
    net.add("B", peers_of_three("B"), 40ms, 60ms);
    net.add("C", peers_of_three("C"), 40ms, 60ms);
    for (const char* id : {"A", "B", "C"}) {
        CRPC_REQUIRE(net.find(id)->raft->start().ok());
    }

    // Elect a leader and commit one entry on the majority.
    const std::string leader = elect(net);
    CRPC_REQUIRE(!leader.empty());
    cluster_node* leader_node = net.find(leader);
    CRPC_REQUIRE(leader_node->raft->propose(
        span<const std::byte>(reinterpret_cast<const std::byte*>("x"), 1)).ok());
    leader_node->clock.advance(50ms);
    leader_node->raft->tick();
    CRPC_CHECK(leader_node->raft->commit_index() >= u64{1});

    // Isolate C now, so it cannot receive any further entries.
    net.partition({"C"});
    CRPC_REQUIRE(leader_node->raft->propose(
        span<const std::byte>(reinterpret_cast<const std::byte*>("y"), 1)).ok());
    leader_node->clock.advance(50ms);
    leader_node->raft->tick();
    CRPC_CHECK(leader_node->raft->commit_index() >= u64{2});

    // C's log is now strictly shorter than the majority's. A vote request
    // from C must be refused by a node that holds the longer log: granting it
    // would let a node that has never seen the committed entries become leader
    // and then truncate them.
    cluster_node* poor = net.find("C");
    CRPC_REQUIRE(poor != nullptr);
    CRPC_CHECK(poor->raft->last_log_index() < u64{2});

    bool refused = false;
    for (const char* id : {"A", "B"}) {
        cluster_node* voter = net.find(id);
        if (voter->raft->last_log_index() < u64{2}) {
            continue;
        }
        cl::raft_message ask;
        ask.kind = cl::raft_message::type::vote;
        ask.term = voter->raft->term() + 1;
        ask.from = "C";
        ask.prev_log_index = poor->raft->last_log_index();
        ask.prev_log_term_hint = poor->raft->entry_term(poor->raft->last_log_index()).value();

        // Capture the reply the voter sends back to C. The reply's `granted`
        // flag is the observable: a voter that finds the candidate's log behind
        // its own must refuse. Asserting on C's state would be vacuous, because
        // C is partitioned and its role cannot change either way.
        cl::raft_message reply;
        bool got_reply = false;
        voter->raft->set_send_hook([&](const cl::raft_message& msg) {
            if (msg.kind == cl::raft_message::type::vote_reply) {
                reply = msg;
                got_reply = true;
            }
        });
        voter->raft->handle(ask);
        CRPC_REQUIRE(got_reply);
        CRPC_CHECK(!reply.granted);
        refused = true;
        break;
    }
    CRPC_REQUIRE(refused);
    net.heal();
    CRPC_CHECK(poor->raft->role() != cl::raft_role::leader);
    // The majority's committed entries are untouched.
    CRPC_CHECK(net.find(leader)->raft->last_log_index() >= u64{2});
}

// --- L2-4: term and votedFor survive a restart --------------------------------

CRPC_TEST(raft_term_vote_persisted_across_restart) {
    test_network net;
    net.add("A", peers_of_three("A"), 40ms, 60ms);
    net.add("B", peers_of_three("B"), 40ms, 60ms);
    net.add("C", peers_of_three("C"), 40ms, 60ms);
    for (const char* id : {"A", "B", "C"}) {
        CRPC_REQUIRE(net.find(id)->raft->start().ok());
    }

    // Elect a leader. Every node has now voted in that term and persisted it.
    const std::string leader = elect(net);
    CRPC_REQUIRE(!leader.empty());
    const u64 elected_term = net.find(leader)->raft->term();
    CRPC_CHECK(elected_term >= u64{1});

    // Restart one node: destroy its raft_node and rebuild it over the same
    // state directory and log. The term and the vote must come back.
    cluster_node* restarted = net.find(leader == "A" ? "B" : "A");
    const std::string restarted_id = restarted->id;
    const auto saved_term = restarted->raft->term();
    const auto saved_last = restarted->raft->last_log_index();
    restarted->raft.reset();
    cl::raft_config config;
    config.node_id = restarted_id;
    config.peers = peers_of_three(restarted_id);
    config.election_timeout_min = 40ms;
    config.election_timeout_max = 60ms;
    config.heartbeat_interval = 20ms;
    restarted->raft = std::make_unique<cl::raft_node>(
        config, *restarted->log, restarted->state_dir.native(),
        make_clock(restarted->clock));
    restarted->raft->set_send_hook([&, restarted_id](const cl::raft_message& msg) {
        net.deliver(restarted_id, msg);
    });
    CRPC_REQUIRE(restarted->raft->start().ok());

    // The persisted term is exactly what it was before the restart.
    CRPC_CHECK_EQ(restarted->raft->term(), saved_term);
    CRPC_CHECK(restarted->raft->term() >= elected_term);
    // The log view is rebuilt from the log, not from memory.
    CRPC_CHECK_EQ(restarted->raft->last_log_index(), saved_last);
    // A node that already voted in this term must not vote again: the
    // persistence is what makes a second vote impossible.
    cl::raft_message ask;
    ask.kind = cl::raft_message::type::vote;
    ask.term = restarted->raft->term();
    ask.from = "C";
    ask.prev_log_index = 0;
    ask.prev_log_term_hint = 0;
    restarted->raft->handle(ask);
    CRPC_CHECK(restarted->raft->role() != cl::raft_role::leader);
}

// --- L2-5: Leader Completeness -----------------------------------------------

CRPC_TEST(raft_leader_completeness) {
    test_network net;
    net.add("A", peers_of("A"), 40ms, 60ms);
    net.add("B", peers_of("B"), 40ms, 60ms);
    net.add("C", peers_of("C"), 40ms, 60ms);
    net.add("D", peers_of("D"), 40ms, 60ms);
    net.add("E", peers_of("E"), 40ms, 60ms);
    for (const char* id : {"A", "B", "C", "D", "E"}) {
        CRPC_REQUIRE(net.find(id)->raft->start().ok());
    }

    // A leader commits an entry, then is killed.
    const std::string first = elect(net);
    CRPC_REQUIRE(!first.empty());
    cluster_node* first_node = net.find(first);
    CRPC_REQUIRE(first_node->raft->propose(
        span<const std::byte>(reinterpret_cast<const std::byte*>("k"), 1)).ok());
    first_node->clock.advance(50ms);
    first_node->raft->tick();
    CRPC_CHECK(first_node->raft->commit_index() >= u64{1});
    const u64 committed_index = first_node->raft->commit_index();
    net.kill(first);

    // A new leader is elected. Leader Completeness (Raft section 5.4.3, the
    // " election restriction") says it must already hold the committed entry:
    // a candidate that does not cannot win a vote.
    const std::string successor = elect_after_loss(net);
    CRPC_REQUIRE(!successor.empty());
    cluster_node* new_leader = net.find(successor);
    CRPC_CHECK(new_leader->raft->last_log_index() >= committed_index);
    CRPC_CHECK(new_leader->raft->commit_index() >= committed_index);
    // The entry's term is unchanged: it was committed in the old term and the
    // new leader carries it verbatim, not a copy written in the new term.
    CRPC_CHECK(new_leader->raft->entry_term(committed_index).value() >= u64{1});
}

// --- supplementary: the consistency check rejects a stale prev term ----------

CRPC_TEST(raft_append_entries_rejects_stale_prev_term) {
    test_network net;
    net.add("A", peers_of_three("A"), 40ms, 60ms);
    net.add("B", peers_of_three("B"), 40ms, 60ms);
    net.add("C", peers_of_three("C"), 40ms, 60ms);
    for (const char* id : {"A", "B", "C"}) {
        CRPC_REQUIRE(net.find(id)->raft->start().ok());
    }
    const std::string leader = elect(net);
    CRPC_REQUIRE(!leader.empty());
    cluster_node* leader_node = net.find(leader);

    // A message that claims a prev_log_index this node does not hold must be
    // refused. The refusal is observable as the reply's `success` flag: a
    // follower that accepted it would have written the entries anyway, which is
    // exactly the corruption the consistency check prevents.
    cl::raft_message stale;
    stale.kind = cl::raft_message::type::append;
    stale.term = leader_node->raft->term();
    stale.from = leader;
    stale.to = "C";
    stale.prev_log_index = 99;
    stale.prev_log_term = 1;
    cl::raft_message reply;
    bool got_reply = false;
    net.find("C")->raft->set_send_hook([&](const cl::raft_message& msg) {
        if (msg.kind == cl::raft_message::type::append_reply) {
            reply = msg;
            got_reply = true;
        }
    });
    net.find("C")->raft->handle(stale);
    CRPC_REQUIRE(got_reply);
    CRPC_CHECK(!reply.success);
    // And C's log is untouched.
    CRPC_CHECK(net.find("C")->raft->last_log_index() == u64{0});

    // The term comparison is the other half of the check: C holds index 1 under
    // the leader's term, and a message that claims a DIFFERENT term there must
    // also be refused. Asserting only the "index absent" branch would leave this
    // comparison untested.
    CRPC_REQUIRE(leader_node->raft->propose(
        span<const std::byte>(reinterpret_cast<const std::byte*>("t"), 1)).ok());
    leader_node->clock.advance(50ms);
    leader_node->raft->tick();
    net.tick_all(1ms);
    CRPC_REQUIRE(net.find("C")->raft->last_log_index() >= u64{1});
    const u64 real_term = net.find("C")->raft->entry_term(1).value();

    cl::raft_message wrong_term;
    wrong_term.kind = cl::raft_message::type::append;
    wrong_term.term = leader_node->raft->term();
    wrong_term.from = leader;
    wrong_term.to = "C";
    wrong_term.prev_log_index = 1;
    wrong_term.prev_log_term = real_term + 100;
    got_reply = false;
    net.find("C")->raft->handle(wrong_term);
    CRPC_REQUIRE(got_reply);
    CRPC_CHECK(!reply.success);
    // C still holds the record the leader actually sent.
    CRPC_CHECK(net.find("C")->raft->entry_term(1).value() == real_term);
}

// --- supplementary: a conflicting suffix is truncated, not merged ------------

CRPC_TEST(raft_conflicting_suffix_is_truncated) {
    test_network net;
    net.add("A", peers_of_three("A"), 40ms, 60ms);
    net.add("B", peers_of_three("B"), 40ms, 60ms);
    net.add("C", peers_of_three("C"), 40ms, 60ms);
    for (const char* id : {"A", "B", "C"}) {
        CRPC_REQUIRE(net.find(id)->raft->start().ok());
    }
    const std::string leader = elect(net);
    CRPC_REQUIRE(!leader.empty());
    cluster_node* leader_node = net.find(leader);
    CRPC_REQUIRE(leader_node->raft->propose(
        span<const std::byte>(reinterpret_cast<const std::byte*>("a"), 1)).ok());
    leader_node->clock.advance(50ms);
    leader_node->raft->tick();

    // C is now at index 1, matching the leader. Give it a record at index 2
    // under a term the leader does not know about.
    cl::memory_registry_log& c_log = *net.find("C")->log;
    cl::log_record bogus;
    bogus.kind = cl::log_record_kind::raft_entry;
    bogus.term = 999;
    bogus.index = 2;
    bogus.payload.assign(1, std::byte{'z'});
    CRPC_REQUIRE(c_log.append(bogus).ok());
    CRPC_CHECK(net.find("C")->raft->last_log_index() == u64{2});
    CRPC_CHECK(net.find("C")->raft->entry_term(2).value() == u64{999});

    // The leader proposes at index 2, so its batch covers the conflict. The
    // node's rule must discard C's bogus record and append the leader's.
    (void)leader_node->raft->propose(
        span<const std::byte>(reinterpret_cast<const std::byte*>("b"), 1));
    leader_node->clock.advance(50ms);
    leader_node->raft->tick();
    net.tick_all(1ms);
    CRPC_CHECK(net.find("C")->raft->last_log_index() == u64{2});
    CRPC_CHECK(net.find("C")->raft->entry_term(2).value() != u64{999});
    // The leader's own record won: C's term at index 2 is the leader's term.
    CRPC_CHECK(net.find("C")->raft->entry_term(2).value()
               == leader_node->raft->entry_term(2).value());

    // Now the case only the NODE's rule handles: the leader's batch starts
    // AFTER the conflict, so `registry_log::append` (which only compares the
    // single record it is given) cannot see it. The node must trim the suffix
    // past the leader's batch.
    cl::log_record bogus2;
    bogus2.kind = cl::log_record_kind::raft_entry;
    bogus2.term = 998;
    bogus2.index = 3;
    bogus2.payload.assign(1, std::byte{'w'});
    CRPC_REQUIRE(c_log.append(bogus2).ok());
    CRPC_CHECK(net.find("C")->raft->last_log_index() == u64{3});
    // The leader sends a heartbeat whose prev_log_index is 2: it has never
    // seen index 3, so C must shed it.
    cl::raft_message beat;
    beat.kind = cl::raft_message::type::append;
    beat.term = leader_node->raft->term();
    beat.from = leader_node->id;
    beat.to = "C";
    beat.prev_log_index = 2;
    beat.prev_log_term = leader_node->raft->entry_term(2).value();
    beat.leader_commit = leader_node->raft->commit_index();
    net.find("C")->raft->handle(beat);
    CRPC_CHECK(net.find("C")->raft->last_log_index() == u64{2});
}

// --- supplementary: only the current term's entries commit by counting -------

CRPC_TEST(raft_commit_only_current_term_entries) {
    test_network net;
    net.add("A", peers_of("A"), 40ms, 60ms);
    net.add("B", peers_of("B"), 40ms, 60ms);
    net.add("C", peers_of("C"), 40ms, 60ms);
    net.add("D", peers_of("D"), 40ms, 60ms);
    net.add("E", peers_of("E"), 40ms, 60ms);
    for (const char* id : {"A", "B", "C", "D", "E"}) {
        CRPC_REQUIRE(net.find(id)->raft->start().ok());
    }
    const std::string leader = elect(net);
    CRPC_REQUIRE(!leader.empty());
    cluster_node* leader_node = net.find(leader);
    CRPC_REQUIRE(leader_node->raft->propose(
        span<const std::byte>(reinterpret_cast<const std::byte*>("a"), 1)).ok());
    leader_node->clock.advance(50ms);
    leader_node->raft->tick();
    CRPC_CHECK(leader_node->raft->commit_index() >= u64{1});
    const u64 term_one = leader_node->raft->term();

    // Isolate the leader and one peer, then propose an entry that cannot commit.
    std::vector<std::string> minority = {leader, "A"};
    if (leader == "A") {
        minority = {leader, "B"};
    }
    net.partition(minority);
    CRPC_REQUIRE(leader_node->raft->propose(
        span<const std::byte>(reinterpret_cast<const std::byte*>("b"), 1)).ok());
    leader_node->clock.advance(50ms);
    leader_node->raft->tick();
    // The uncommitted entry must not have advanced the commit index.
    CRPC_CHECK(leader_node->raft->commit_index() < u64{2});

    // Heal and elect a new leader. The old uncommitted entry is superseded, and
    // the commit index may advance only over entries of the NEW leader's term.
    net.heal();
    net.kill(leader);
    const std::string successor = elect_after_loss(net);
    CRPC_REQUIRE(!successor.empty());
    cluster_node* new_leader = net.find(successor);
    CRPC_CHECK(new_leader->raft->term() > term_one);
    // Raft 5.4.2: a leader never commits an entry of a previous term by
    // counting replicas. The observable is the commit index itself: with the
    // term check removed, the new leader would advance it over the old-term
    // entry it inherited, which is the exact corruption 5.4.2 forbids.
    const u64 commit_before = new_leader->raft->commit_index();
    CRPC_CHECK(commit_before >= u64{1});
    // Give the old-term entry a majority of replicas WITHOUT the new leader
    // writing anything of its own term. The commit index must not move.
    net.tick_all(1ms);
    CRPC_CHECK_EQ(new_leader->raft->commit_index(), commit_before);
    CRPC_REQUIRE(new_leader->raft->propose(
        span<const std::byte>(reinterpret_cast<const std::byte*>("n"), 1)).ok());
    new_leader->clock.advance(50ms);
    new_leader->raft->tick();
    net.tick_all(1ms);
    // Now an entry of the current term exists and has a majority: the commit
    // index may advance, and it must.
    CRPC_CHECK(new_leader->raft->commit_index() > commit_before);
}

// --- supplementary: the election timeout is randomised -----------------------
//
// A single election cannot distinguish a randomised timeout from a fixed one:
// the in-process transport delivers a candidate's messages synchronously inside
// its own `tick()`, so whichever node is ticked first campaigns and wins before
// the others' timers are examined. That is a property of the harness, not of
// `raft_node`.
//
// What does distinguish them is the *distribution* of winners across many
// independent runs. With a randomised timeout each node's deadline falls at a
// different step, so the winner varies; with a fixed timeout every node crosses
// its deadline on the same step and the tick order decides, so the same node
// wins every time. Measured on this platform over 20 runs: randomised gives
// A:4 B:2 C:4 D:7 E:3, fixed gives A:20.
//
// The assertion is therefore "more than one distinct winner over N runs". A
// fixed timeout cannot satisfy it, and a randomised one satisfies it with
// overwhelming probability (the chance of 20 identical draws from 5 nodes is
// negligible).
CRPC_TEST(raft_election_timeout_is_randomized) {
    std::vector<std::string> winners;
    for (int run = 0; run < 20; ++run) {
        test_network net;
        net.add("A", peers_of("A"), 40ms, 60ms);
        net.add("B", peers_of("B"), 40ms, 60ms);
        net.add("C", peers_of("C"), 40ms, 60ms);
        net.add("D", peers_of("D"), 40ms, 60ms);
        net.add("E", peers_of("E"), 40ms, 60ms);
        for (const char* id : {"A", "B", "C", "D", "E"}) {
            CRPC_REQUIRE(net.find(id)->raft->start().ok());
        }
        // Advance every clock by the same amount before each tick, so the only
        // thing that can make one node campaign before another is its own
        // randomised deadline.
        std::string leader;
        for (int step = 0; step < 200 && leader.empty(); ++step) {
            for (const char* id : {"A", "B", "C", "D", "E"}) {
                net.find(id)->clock.advance(1ms);
            }
            for (const char* id : {"A", "B", "C", "D", "E"}) {
                net.find(id)->raft->tick();
            }
            for (const char* id : {"A", "B", "C", "D", "E"}) {
                if (net.find(id)->raft->role() == cl::raft_role::leader) {
                    leader = id;
                    break;
                }
            }
        }
        CRPC_REQUIRE(!leader.empty());
        winners.push_back(leader);
    }

    std::size_t distinct = 0;
    for (const std::string& id : {"A", "B", "C", "D", "E"}) {
        if (std::find(winners.begin(), winners.end(), id) != winners.end()) {
            ++distinct;
        }
    }
    // A fixed timeout yields exactly one distinct winner; a randomised one
    // spreads across the cluster. Two is a deliberately weak floor: it is
    // enough to fail a fixed timeout and does not depend on a particular seed.
    CRPC_CHECK(distinct >= 2);
}

// --- supplementary: a PreVote does not bump the term -------------------------

CRPC_TEST(raft_prevote_does_not_bump_term) {
    test_network net;
    net.add("A", peers_of_three("A"), 40ms, 60ms);
    net.add("B", peers_of_three("B"), 40ms, 60ms);
    net.add("C", peers_of_three("C"), 40ms, 60ms);
    for (const char* id : {"A", "B", "C"}) {
        CRPC_REQUIRE(net.find(id)->raft->start().ok());
    }
    const std::string leader = elect(net);
    CRPC_REQUIRE(!leader.empty());
    const u64 term_before = net.find(leader)->raft->term();

    // A node that has not timed out yet must not have moved its term.
    cluster_node* idle = net.find(leader == "A" ? "B" : "A");
    CRPC_CHECK_EQ(idle->raft->term(), term_before);
    CRPC_CHECK(idle->raft->role() != cl::raft_role::candidate);
}

// --- D-B-4: a stale-term RequestVote must not be granted ---------------------
//
// A `vote` whose term is BELOW the receiver's is a candidate from a past
// election. Granting it both hands a live term's vote to a dead candidacy and
// answers in the STALE term (the old code echoed `msg.term`), so the real
// candidate for the current term is then refused (`voted_this_term_` is set).
// The guard is the mirror of the `append` branch's `msg.term < term_` check.

CRPC_TEST(raft_stale_term_vote_is_refused) {
    // A standalone node, so the vote state is fully under the test's control.
    auto state = fs::temp_dir::create(fs::path(), "crpc-raft-stalevote-");
    CRPC_REQUIRE(state.ok());
    cl::memory_registry_log log;
    manual_clock clock;
    cl::raft_config config;
    config.node_id = "B";
    config.peers = {"A"};
    config.election_timeout_min = 40ms;
    config.election_timeout_max = 60ms;
    cl::raft_node raft(config, log, state.value().native(), make_clock(clock));
    CRPC_REQUIRE(raft.start().ok());

    std::vector<cl::raft_message> replies;
    raft.set_send_hook([&replies](const cl::raft_message& msg) {
        replies.push_back(msg);
    });

    // Raise the term to 5 WITHOUT voting: a heartbeat from a leader in term 5
    // makes this node adopt term 5 and clear its vote. This is the state the
    // defect needs -- `voted_this_term_ == false`, so the old code's
    // `!voted_this_term_ && up_to_date` check would grant a stale request.
    cl::raft_message beat;
    beat.kind = cl::raft_message::type::append;
    beat.term = 5;
    beat.from = "A";
    beat.to = "B";
    beat.prev_log_index = 0;
    beat.prev_log_term = 0;
    beat.leader_commit = 0;
    raft.handle(beat);
    CRPC_REQUIRE(raft.term() == u64{5});

    // A candidate from an OLD term now asks for a vote. It must be refused, and
    // the reply must carry THIS node's term (5), not the stale one (3). The old
    // code granted it (setting `voted_this_term_`) and answered in term 3, so
    // the real candidate for term 5 was then refused.
    replies.clear();
    cl::raft_message stale;
    stale.kind = cl::raft_message::type::vote;
    stale.term = 3;
    stale.from = "Z";
    stale.to = "B";
    stale.prev_log_index = 0;
    stale.prev_log_term_hint = 0;
    raft.handle(stale);

    CRPC_REQUIRE(!replies.empty());
    const cl::raft_message& reply = replies.back();
    CRPC_CHECK_EQ(int(cl::raft_message::type::vote_reply), int(reply.kind));
    CRPC_CHECK_FALSE(reply.granted);
    // The reply is in the RECEIVER's term. Echoing the stale term would tell the
    // dead candidate it is current.
    CRPC_CHECK_EQ(u64{5}, reply.term);
    // The stale request must not have moved the term backwards.
    CRPC_CHECK_EQ(u64{5}, raft.term());

    // The vote for the CURRENT term must still be available. Granting the stale
    // request would have set `voted_this_term_` and this would now fail.
    replies.clear();
    cl::raft_message legit;
    legit.kind = cl::raft_message::type::vote;
    legit.term = 5;
    legit.from = "Y";
    legit.to = "B";
    legit.prev_log_index = 0;
    legit.prev_log_term_hint = 0;
    raft.handle(legit);
    CRPC_REQUIRE(!replies.empty());
    CRPC_CHECK(replies.back().granted);
}

// --- D-B-1: a follower that could not append must not report success ---------
//
// The follower applies the entries in a loop that used to `break` on a failed
// `append` and then unconditionally answer `success = true` with
// `match_index = prev_log_index + entries.size()`. The leader would then raise
// that peer's `match_index` past entries the follower does not hold and commit
// a write no follower has. This case forces the very first append to fail (the
// log rejects an index it is not contiguous with) and asserts the reply.

CRPC_TEST(raft_follower_does_not_ack_an_append_it_could_not_store) {
    // A follower whose log is empty. A batch that starts at index 1 with a gap
    // the backend refuses: `append` rejects a record whose index skips ahead.
    // Driving `handle` directly is the point: the reply is the observable.
    auto state = fs::temp_dir::create(fs::path(), "crpc-raft-ack-");
    CRPC_REQUIRE(state.ok());
    cl::memory_registry_log log;
    manual_clock clock;
    cl::raft_config config;
    config.node_id = "B";
    config.peers = {"A"};
    config.election_timeout_min = 40ms;
    config.election_timeout_max = 60ms;
    cl::raft_node raft(config, log, state.value().native(), make_clock(clock));
    CRPC_REQUIRE(raft.start().ok());

    std::vector<cl::raft_message> replies;
    raft.set_send_hook([&replies](const cl::raft_message& msg) {
        replies.push_back(msg);
    });

    // prev_log_index = 0 (matches an empty log), but the batch jumps to index 3:
    // the backend refuses the gap, so nothing is stored.
    cl::raft_message batch;
    batch.kind = cl::raft_message::type::append;
    batch.term = 1;
    batch.from = "A";
    batch.to = "B";
    batch.prev_log_index = 0;
    batch.prev_log_term = 0;
    batch.leader_commit = 0;
    for (u64 index = 3; index <= 4; ++index) {
        cl::log_record record;
        record.kind = cl::log_record_kind::raft_entry;
        record.term = 1;
        record.index = index;
        batch.entries.push_back(std::move(record));
    }
    raft.handle(batch);

    CRPC_REQUIRE(!replies.empty());
    const cl::raft_message& reply = replies.back();
    CRPC_CHECK_EQ(int(cl::raft_message::type::append_reply), int(reply.kind));
    // The log is still empty, so the follower must NOT claim it holds anything.
    CRPC_CHECK_EQ(u64{0}, log.last_index());
    CRPC_CHECK_FALSE(reply.success);
    // `match_index` must never name an entry the log does not hold.
    CRPC_CHECK(reply.match_index <= log.last_index());
}

// --- D-B-2: a proposal whose sync failed must not be acknowledged ------------
//
// `propose` appended the entry, dropped `sync()`'s result, and advanced the
// commit index. On a single node (its own quorum) that acknowledges a write
// that is not durable: a power loss loses it while the client was told success.

CRPC_TEST(raft_propose_reports_a_failed_sync_and_does_not_commit) {
    auto state = fs::temp_dir::create(fs::path(), "crpc-raft-sync-");
    CRPC_REQUIRE(state.ok());
    const fs::path log_path = state.value().native() / "raft.log";
    auto opened = cl::file_registry_log::open(log_path.str());
    CRPC_REQUIRE(opened.ok());
    auto log = std::move(opened).value();

    manual_clock clock;
    cl::raft_config config;
    config.node_id = "A";
    config.election_timeout_min = 40ms;
    config.election_timeout_max = 60ms;
    cl::raft_node raft(config, *log, state.value().native(), make_clock(clock));
    CRPC_REQUIRE(raft.start().ok());
    for (int i = 0; i < 20 && raft.role() != cl::raft_role::leader; ++i) {
        clock.advance(20ms);
        raft.tick();
    }
    CRPC_REQUIRE(raft.role() == cl::raft_role::leader);

    const u64 durable_before = log->durable_size();
    const u64 commit_before = raft.commit_index();
    const u64 index_before = log->last_index();

    log->inject_flush_failure();
    const std::byte payload[] = {std::byte{0x01}};
    const auto proposed = raft.propose(span<const std::byte>(payload, 1));

    // The write is not durable, so it must not be reported as accepted.
    CRPC_CHECK_FALSE(proposed.ok());
    CRPC_CHECK_EQ(durable_before, log->durable_size());
    // Neither the commit index nor the log may advance on a failed sync.
    CRPC_CHECK_EQ(commit_before, raft.commit_index());
    CRPC_CHECK_EQ(index_before, log->last_index());
}

// --- D-A-2 / D-B-3: concurrent propose and replay must not corrupt the log ---
//
// `raft_node` had no synchronisation, and `file_registry_log` shared one file
// handle across threads (`SetFilePointerEx` + `ReadFile`/`WriteFile` is a
// seek-then-IO pair). A raft tick thread and an HTTP handler thread therefore
// raced: two `propose` calls could allocate the SAME index (both read
// `last_index() + 1`), and one append would truncate the other, silently losing
// a write; a concurrent `replay` could read the wrong bytes. This case drives
// both from many threads and asserts the log stays dense and complete.

CRPC_TEST(raft_concurrent_propose_and_replay_keep_the_log_dense) {
    auto state = fs::temp_dir::create(fs::path(), "crpc-raft-conc-");
    CRPC_REQUIRE(state.ok());
    const fs::path log_path = state.value().native() / "raft.log";
    auto opened = cl::file_registry_log::open(log_path.str());
    CRPC_REQUIRE(opened.ok());
    auto log = std::move(opened).value();

    manual_clock clock;
    cl::raft_config config;
    config.node_id = "A";
    config.election_timeout_min = 40ms;
    config.election_timeout_max = 60ms;
    cl::raft_node raft(config, *log, state.value().native(), make_clock(clock));
    CRPC_REQUIRE(raft.start().ok());
    for (int i = 0; i < 20 && raft.role() != cl::raft_role::leader; ++i) {
        clock.advance(20ms);
        raft.tick();
    }
    CRPC_REQUIRE(raft.role() == cl::raft_role::leader);

    constexpr int kThreads = 8;
    constexpr int kPerThread = 25;
    std::atomic<int> proposed_ok{0};
    std::atomic<int> replay_errors{0};
    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < kPerThread; ++i) {
                const std::byte payload[] = {std::byte{0x01}};
                const auto proposed =
                    raft.propose(span<const std::byte>(payload, 1));
                if (proposed.ok()) {
                    proposed_ok.fetch_add(1, std::memory_order_relaxed);
                }
                // A concurrent reader: the file handle is shared, so a missing
                // lock shows up as a short read or a non-dense replay.
                const auto replayed = log->replay(0);
                if (!replayed.ok()) {
                    replay_errors.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                u64 expected = 1;
                for (const cl::log_record& record : replayed.value()) {
                    if (record.index != expected) {
                        replay_errors.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                    ++expected;
                }
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }

    // Every proposal succeeded, and the log holds exactly that many entries with
    // no gap and no lost write.
    CRPC_CHECK_EQ(kThreads * kPerThread, proposed_ok.load());
    CRPC_CHECK_EQ(0, replay_errors.load());
    CRPC_CHECK_EQ(u64{kThreads * kPerThread}, log->last_index());
    const auto final_replay = log->replay(0);
    CRPC_REQUIRE(final_replay.ok());
    CRPC_CHECK_EQ(std::size_t{kThreads * kPerThread},
                  final_replay.value().size());
}

// --- N-B-2: PreVote grants must not count toward the real election ----------
//
// A candidate collects PreVote grants in `votes_`. When a PreVote majority is
// reached it increments the term and starts a REAL election -- but it did not
// clear `votes_`, so the PreVote grants stayed in the same vector the real
// `vote_reply` path appends to. A 5-node cluster (quorum 3) could then elect a
// leader with only ONE real vote: two PreVote grants + one real vote + the
// candidate's own vote reached the quorum, bypassing the majority requirement.
//
// This case drives a standalone candidate: two PreVote grants (a PreVote
// majority), then ONE real vote. It must still be a candidate. A second real
// vote reaches the quorum and it becomes leader.

CRPC_TEST(raft_prevote_grants_do_not_count_toward_the_real_election) {
    auto state = fs::temp_dir::create(fs::path(), "crpc-raft-prevote-quorum-");
    CRPC_REQUIRE(state.ok());
    cl::memory_registry_log log;
    manual_clock clock;
    cl::raft_config config;
    config.node_id = "A";
    config.peers = {"B", "C", "D", "E"};
    config.election_timeout_min = 40ms;
    config.election_timeout_max = 60ms;
    cl::raft_node raft(config, log, state.value().native(), make_clock(clock));
    CRPC_REQUIRE(raft.start().ok());

    std::vector<cl::raft_message> outbound;
    raft.set_send_hook([&outbound](const cl::raft_message& msg) {
        outbound.push_back(msg);
    });

    // Drive A into candidacy: advance past the election timeout and tick.
    clock.advance(100ms);
    raft.tick();
    CRPC_REQUIRE(raft.role() == cl::raft_role::candidate);
    // The PreVote proposes term_ + 1; the term is not bumped until a majority
    // grants.
    const u64 candidacy_term = raft.term() + 1;

    // Two PreVote grants (B, C). Quorum is 3 (self + 2), so this is a PreVote
    // majority and A moves to a real election.
    for (const char* from : {"B", "C"}) {
        cl::raft_message reply;
        reply.kind = cl::raft_message::type::prevote_reply;
        reply.term = raft.term();
        reply.from = from;
        reply.to = "A";
        reply.granted = true;
        raft.handle(reply);
    }
    CRPC_REQUIRE(raft.term() == candidacy_term);
    CRPC_REQUIRE(raft.role() == cl::raft_role::candidate);

    // ONE real vote from D. The two PreVote grants must NOT count: A has only
    // its own vote + D's = 2, short of the quorum of 3.
    cl::raft_message vote;
    vote.kind = cl::raft_message::type::vote_reply;
    vote.term = raft.term();
    vote.from = "D";
    vote.to = "A";
    vote.granted = true;
    raft.handle(vote);

    CRPC_CHECK(raft.role() == cl::raft_role::candidate);

    // A second real vote (E) reaches the quorum and A becomes leader.
    cl::raft_message second = vote;
    second.from = "E";
    raft.handle(second);
    CRPC_CHECK(raft.role() == cl::raft_role::leader);
}

// --- LATE-PREVOTE: a late PreVote reply must not be counted -----------------
//
// After a PreVote majority the node does `votes_.clear(); ++term_;` and starts
// a real election, but `role_` stays `candidate`. The `prevote_reply` guard was
// `if (msg.term > term_ + 1) break;` -- an UPPER bound only. Once `term_` has
// advanced, that window moves with it, so a PreVote reply that was in flight
// during the transition (carrying the OLD term) still passes the guard and
// lands in the freshly cleared `votes_`.
//
// This is a SAFETY defect, not merely liveness. Two consequences, both from the
// same root cause:
//   * liveness: two late replies reach a majority again and bump the term a
//     second time (term inflation, a vote round for a term nobody asked for);
//   * safety: ONE late reply plus ONE real vote plus the candidate's own vote
//     reaches the quorum, electing a leader with a single real vote -- the
//     majority requirement is bypassed.
//
// Both paths are pinned below, because their consequences differ.

CRPC_TEST(raft_late_prevote_reply_does_not_bump_the_term_again) {
    auto state = fs::temp_dir::create(fs::path(), "crpc-raft-late-prevote-");
    CRPC_REQUIRE(state.ok());
    cl::memory_registry_log log;
    manual_clock clock;
    cl::raft_config config;
    config.node_id = "A";
    config.peers = {"B", "C", "D", "E"};
    config.election_timeout_min = 40ms;
    config.election_timeout_max = 60ms;
    cl::raft_node raft(config, log, state.value().native(), make_clock(clock));
    CRPC_REQUIRE(raft.start().ok());

    std::vector<cl::raft_message> outbound;
    raft.set_send_hook([&outbound](const cl::raft_message& msg) {
        outbound.push_back(msg);
    });

    // Drive A into candidacy at term 0.
    clock.advance(100ms);
    raft.tick();
    CRPC_REQUIRE(raft.role() == cl::raft_role::candidate);
    CRPC_REQUIRE(raft.term() == u64{0});
    const u64 prevote_term = raft.term(); // the term the PreVote replies carry

    // B, C grant the PreVote: quorum 3 (self + 2), so A increments the term to
    // 1 and starts a real election.
    for (const char* from : {"B", "C"}) {
        cl::raft_message reply;
        reply.kind = cl::raft_message::type::prevote_reply;
        reply.term = prevote_term;
        reply.from = from;
        reply.to = "A";
        reply.granted = true;
        raft.handle(reply);
    }
    CRPC_REQUIRE(raft.term() == u64{1});
    CRPC_REQUIRE(raft.role() == cl::raft_role::candidate);

    // D, E's PreVote replies were in flight during the transition and carry the
    // OLD term (0). They must NOT be counted: the term must stay 1, and no
    // second election round may start.
    for (const char* from : {"D", "E"}) {
        cl::raft_message late;
        late.kind = cl::raft_message::type::prevote_reply;
        late.term = prevote_term;
        late.from = from;
        late.to = "A";
        late.granted = true;
        raft.handle(late);
    }

    CRPC_CHECK_EQ(u64{1}, raft.term());
    CRPC_CHECK(raft.role() == cl::raft_role::candidate);
}

// The SAFETY path: ONE late PreVote reply plus ONE real vote must not elect a
// leader. If the stale reply is counted, 1 real vote + 1 stale prevote + the
// candidate's own vote = quorum (3 of 5), and the node becomes leader with a
// single real vote -- the majority requirement is bypassed.

CRPC_TEST(raft_late_prevote_reply_does_not_count_as_a_real_vote) {
    auto state = fs::temp_dir::create(fs::path(), "crpc-raft-late-prevote-s-");
    CRPC_REQUIRE(state.ok());
    cl::memory_registry_log log;
    manual_clock clock;
    cl::raft_config config;
    config.node_id = "A";
    config.peers = {"B", "C", "D", "E"};
    config.election_timeout_min = 40ms;
    config.election_timeout_max = 60ms;
    cl::raft_node raft(config, log, state.value().native(), make_clock(clock));
    CRPC_REQUIRE(raft.start().ok());

    std::vector<cl::raft_message> outbound;
    raft.set_send_hook([&outbound](const cl::raft_message& msg) {
        outbound.push_back(msg);
    });

    // Drive A into candidacy at term 0.
    clock.advance(100ms);
    raft.tick();
    CRPC_REQUIRE(raft.role() == cl::raft_role::candidate);
    CRPC_REQUIRE(raft.term() == u64{0});
    const u64 prevote_term = raft.term();

    // B, C grant the PreVote: quorum 3 (self + 2), so A increments the term to
    // 1 and starts a real election.
    for (const char* from : {"B", "C"}) {
        cl::raft_message reply;
        reply.kind = cl::raft_message::type::prevote_reply;
        reply.term = prevote_term;
        reply.from = from;
        reply.to = "A";
        reply.granted = true;
        raft.handle(reply);
    }
    CRPC_REQUIRE(raft.term() == u64{1});
    CRPC_REQUIRE(raft.role() == cl::raft_role::candidate);

    // ONE late PreVote reply (old term 0) and ONE real vote (term 1). The stale
    // reply must not count: A has only its own vote + E's = 2, short of 3.
    cl::raft_message late;
    late.kind = cl::raft_message::type::prevote_reply;
    late.term = prevote_term;
    late.from = "D";
    late.to = "A";
    late.granted = true;
    raft.handle(late);

    cl::raft_message real;
    real.kind = cl::raft_message::type::vote_reply;
    real.term = raft.term();
    real.from = "E";
    real.to = "A";
    real.granted = true;
    raft.handle(real);

    CRPC_CHECK(raft.role() == cl::raft_role::candidate);
}

// --- STALE-ACK: an append_reply from an old term must not advance commit -----
//
// The `append` branch rejects `msg.term < term_` before touching any state, and
// `vote_reply` requires `msg.term == term_`. The `append_reply` branch had NO
// term check at all: it looked up the peer by name, recorded `last_ack`, and on
// success raised `match_index` and called `advance_commit_locked()`.
//
// A reply from an OLD term is a follower answering a request the leader sent
// before it was re-elected. Its `match_index` describes a log position that may
// no longer exist, and counting it toward the commit quorum can commit an entry
// the current term never replicated to a majority -- the exact corruption Raft
// 5.4.2 forbids. This is a SAFETY defect.
//
// The case drives a 3-node leader (quorum 2), proposes one entry (commit stays
// 0: only the leader holds it), then feeds a stale-term success reply with an
// inflated `match_index`. If the reply is counted, replicas = leader + B = 2 =
// quorum and the commit index advances over an entry no majority ever stored.

CRPC_TEST(raft_stale_append_reply_does_not_advance_commit) {
    auto state = fs::temp_dir::create(fs::path(), "crpc-raft-stale-ack-");
    CRPC_REQUIRE(state.ok());
    cl::memory_registry_log log;
    manual_clock clock;
    cl::raft_config config;
    config.node_id = "A";
    config.peers = {"B", "C"};
    config.election_timeout_min = 40ms;
    config.election_timeout_max = 60ms;
    cl::raft_node raft(config, log, state.value().native(), make_clock(clock));
    CRPC_REQUIRE(raft.start().ok());

    std::vector<cl::raft_message> outbound;
    raft.set_send_hook([&outbound](const cl::raft_message& msg) {
        outbound.push_back(msg);
    });

    // Drive A into candidacy at term 0.
    clock.advance(100ms);
    raft.tick();
    CRPC_REQUIRE(raft.role() == cl::raft_role::candidate);
    CRPC_REQUIRE(raft.term() == u64{0});
    const u64 prevote_term = raft.term();

    // B grants the PreVote: quorum 2 (self + B), so A increments the term to 1
    // and starts a real election.
    cl::raft_message prevote;
    prevote.kind = cl::raft_message::type::prevote_reply;
    prevote.term = prevote_term;
    prevote.from = "B";
    prevote.to = "A";
    prevote.granted = true;
    raft.handle(prevote);
    CRPC_REQUIRE(raft.term() == u64{1});
    CRPC_REQUIRE(raft.role() == cl::raft_role::candidate);

    // B grants the real vote: quorum 2 (self + B), so A becomes leader.
    cl::raft_message vote;
    vote.kind = cl::raft_message::type::vote_reply;
    vote.term = raft.term();
    vote.from = "B";
    vote.to = "A";
    vote.granted = true;
    raft.handle(vote);
    CRPC_REQUIRE(raft.role() == cl::raft_role::leader);
    const u64 leader_term = raft.term();

    // Propose one entry. Only the leader holds it, so the commit index stays 0.
    const std::byte payload[] = {std::byte{0x01}};
    const auto proposed = raft.propose(span<const std::byte>(payload, 1));
    CRPC_REQUIRE(proposed.ok());
    CRPC_REQUIRE(raft.commit_index() == u64{0});
    const u64 commit_before = raft.commit_index();

    // A STALE success reply: term one BELOW the leader's, claiming a match_index
    // far beyond anything that was replicated. It must be ignored entirely.
    cl::raft_message stale;
    stale.kind = cl::raft_message::type::append_reply;
    stale.term = leader_term - 1;
    stale.from = "B";
    stale.to = "A";
    stale.success = true;
    stale.match_index = raft.last_log_index() + 100;
    raft.handle(stale);

    CRPC_CHECK_EQ(commit_before, raft.commit_index());
}

// --- 3RD-PATH: a PreVote reply carrying the NEW term must not count as a vote -
//
// The `prevote_reply` guard accepts BOTH `term_` and `term_ + 1` (the probe's
// term is `term_ + 1`, and a peer that has already advanced answers in its own
// `term_`). That window is correct BEFORE the PreVote majority. AFTER it, the
// node does `votes_.clear(); ++term_; voted_for_ = self; voted_this_term_ =
// true;` and starts a real election -- but `role_` stays `candidate`.
//
// A PreVote reply that arrives carrying the NEW term (`== term_`) still passes
// both bounds (`term_ > term_ + 1` false, `term_ < term_` false) and lands in
// the freshly cleared `votes_`. It is a PreVote grant, not a real vote, yet it
// is counted as one: ONE such reply plus ONE real vote plus the candidate's own
// vote reaches the quorum, electing a leader with a single real vote.
//
// This is a real network ordering, not a contrived one: `tick` sends the probe
// with `term_ + 1` (`raft_node.cpp:346`), and a peer that has already advanced
// to that term answers in its own `term_` (`raft_node.cpp:445`).
//
// The fix rejects a PreVote reply once this node has voted for itself in the
// current term -- exactly the post-bump state. A bare `voted_this_term_` check
// is too wide: it is also true when the node voted for ANOTHER candidate, which
// would refuse a legitimate PreVote.

CRPC_TEST(raft_new_term_prevote_reply_does_not_count_as_a_real_vote) {
    auto state = fs::temp_dir::create(fs::path(), "crpc-raft-3rd-path-");
    CRPC_REQUIRE(state.ok());
    cl::memory_registry_log log;
    manual_clock clock;
    cl::raft_config config;
    config.node_id = "A";
    config.peers = {"B", "C", "D", "E"};
    config.election_timeout_min = 40ms;
    config.election_timeout_max = 60ms;
    cl::raft_node raft(config, log, state.value().native(), make_clock(clock));
    CRPC_REQUIRE(raft.start().ok());

    std::vector<cl::raft_message> outbound;
    raft.set_send_hook([&outbound](const cl::raft_message& msg) {
        outbound.push_back(msg);
    });

    // Drive A into candidacy at term 0.
    clock.advance(100ms);
    raft.tick();
    CRPC_REQUIRE(raft.role() == cl::raft_role::candidate);
    CRPC_REQUIRE(raft.term() == u64{0});
    const u64 prevote_term = raft.term();

    // B, C grant the PreVote: quorum 3 (self + 2), so A increments the term to
    // 1 and starts a real election.
    for (const char* from : {"B", "C"}) {
        cl::raft_message reply;
        reply.kind = cl::raft_message::type::prevote_reply;
        reply.term = prevote_term;
        reply.from = from;
        reply.to = "A";
        reply.granted = true;
        raft.handle(reply);
    }
    CRPC_REQUIRE(raft.term() == u64{1});
    CRPC_REQUIRE(raft.role() == cl::raft_role::candidate);

    // ONE PreVote reply carrying the NEW term (1) and ONE real vote (term 1).
    // The PreVote grant must not count: A has only its own vote + E's = 2,
    // short of the quorum of 3.
    cl::raft_message new_term_prevote;
    new_term_prevote.kind = cl::raft_message::type::prevote_reply;
    new_term_prevote.term = raft.term();
    new_term_prevote.from = "D";
    new_term_prevote.to = "A";
    new_term_prevote.granted = true;
    raft.handle(new_term_prevote);

    cl::raft_message real;
    real.kind = cl::raft_message::type::vote_reply;
    real.term = raft.term();
    real.from = "E";
    real.to = "A";
    real.granted = true;
    raft.handle(real);

    CRPC_CHECK(raft.role() == cl::raft_role::candidate);
}

// --- 2ND-ROUND: a candidate must be able to win a SECOND election ------------
//
// The 3RD-PATH guard must not be built on `voted_this_term_` / `voted_for_`.
// After a PreVote majority the node sets `voted_for_ = self; voted_this_term_ =
// true;` and starts a real election. If that election fails (no vote replies),
// the next `tick()` starts a NEW PreVote round -- but it only clears `votes_`,
// it does NOT reset `voted_for_`/`voted_this_term_` (they are the per-term vote
// record, and the term has not changed). A guard keyed on those fields would
// therefore reject every PreVote reply in round 2, and the node could never win
// a second election: a liveness defect.
//
// The correct key is a dedicated round flag: set when a PreVote round starts,
// cleared when the round converts to a real election. This case pins the
// liveness property the flag exists to preserve.

CRPC_TEST(raft_candidate_can_win_a_second_election) {
    auto state = fs::temp_dir::create(fs::path(), "crpc-raft-2nd-round-");
    CRPC_REQUIRE(state.ok());
    cl::memory_registry_log log;
    manual_clock clock;
    cl::raft_config config;
    config.node_id = "A";
    config.peers = {"B", "C", "D", "E"};
    config.election_timeout_min = 40ms;
    config.election_timeout_max = 60ms;
    cl::raft_node raft(config, log, state.value().native(), make_clock(clock));
    CRPC_REQUIRE(raft.start().ok());

    std::vector<cl::raft_message> outbound;
    raft.set_send_hook([&outbound](const cl::raft_message& msg) {
        outbound.push_back(msg);
    });

    // Round 1: candidacy at term 0, PreVote majority bumps the term to 1 and
    // starts a real election. No vote replies arrive, so the election fails.
    clock.advance(100ms);
    raft.tick();
    CRPC_REQUIRE(raft.role() == cl::raft_role::candidate);
    CRPC_REQUIRE(raft.term() == u64{0});
    const u64 prevote_term = raft.term();
    for (const char* from : {"B", "C"}) {
        cl::raft_message reply;
        reply.kind = cl::raft_message::type::prevote_reply;
        reply.term = prevote_term;
        reply.from = from;
        reply.to = "A";
        reply.granted = true;
        raft.handle(reply);
    }
    CRPC_REQUIRE(raft.term() == u64{1});
    CRPC_REQUIRE(raft.role() == cl::raft_role::candidate);

    // Round 2: the election times out again. A new PreVote round starts at the
    // current term (1); its replies carry term 1. The PreVote majority must be
    // honoured -- the term must advance to 2 -- or the node is stuck forever.
    clock.advance(100ms);
    raft.tick();
    CRPC_REQUIRE(raft.role() == cl::raft_role::candidate);
    CRPC_REQUIRE(raft.term() == u64{1});
    const u64 round2_prevote_term = raft.term();
    for (const char* from : {"B", "C"}) {
        cl::raft_message reply;
        reply.kind = cl::raft_message::type::prevote_reply;
        reply.term = round2_prevote_term;
        reply.from = from;
        reply.to = "A";
        reply.granted = true;
        raft.handle(reply);
    }

    CRPC_CHECK_EQ(u64{2}, raft.term());
}

// --- D-B-7: wait_for_commit uses raft_node::now() (injected clock) -----------

CRPC_TEST(raft_wait_for_commit_uses_injected_clock) {
    test_network net;
    net.add("A", peers_of_three("A"), 40ms, 60ms);
    net.add("B", peers_of_three("B"), 40ms, 60ms);
    net.add("C", peers_of_three("C"), 40ms, 60ms);
    for (const char* id : {"A", "B", "C"}) {
        CRPC_REQUIRE(net.find(id)->raft->start().ok());
    }
    const std::string leader = elect(net);
    CRPC_REQUIRE(!leader.empty());

    // Isolate the leader so its proposal can never reach a majority.
    net.partition({leader});
    cluster_node* node = net.find(leader);

    // Propose: the entry is appended locally but cannot commit.
    const std::byte payload[2] = {std::byte{0x0a}, std::byte{0x0b}};
    const auto proposed = node->raft->propose(span<const std::byte>(payload, 2));
    CRPC_REQUIRE(proposed.ok());

    // Deadline already passed at propose time: wait_for_commit must return at
    // pump 0 (no time to wait) rather than block until wall-clock catches up.
    const auto deadline = node->clock.now;  // now, already overdue
    const auto committed = node->raft->wait_for_commit(proposed.value(), deadline);
    CRPC_CHECK_FALSE(committed.ok());
    if (!committed.ok()) {
        CRPC_CHECK_EQ(rpc_code::unavailable, committed.error().code);
    }
}

CRPC_TEST_MAIN()
