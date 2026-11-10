// CRPC -- G19 step3: Raft consensus node (single-process, test-driven).
//
// This is the consensus layer VERDICT-step3 D4 asks for: a Raft state machine
// that owns one node's view of the replicated log. It is a SINGLE-PROCESS
// primitive -- the messages arrive through `handle()` and leave through the
// send hook, so a test (or a real transport) decides what "the network" is. It
// does not perform I/O and does not apply committed entries to the registry;
// recovery is a full rebuild through `cluster::restore_from_log`, which is why
// there is no per-entry apply step here.
//
// Responsibility boundary
// -----------------------
//   * owns: current term, votedFor, the log view, commit index, match index
//     per peer, the role, and the election/heartbeat timers.
//   * does NOT own: durability of the log (`registry_log::sync`), the on-disk
//     state file (`raft_state_store`), the registry contents, or the transport.
//   * `propose` appends to the log and replicates; it does NOT commit. A entry
//     commits only when a majority of peers have accepted it, and -- per Raft
//     section 5.4.2 -- only an entry of the current term may be committed by
//     counting replicas.
//
// Log index space
// ---------------
// `registry_log` already rejects an index gap and truncates a conflicting
// suffix on append. The node therefore allocates `last_index + 1` itself and
// never reuses an index. `registry_event` records written by the registry
// service carry `term == 0` (a step1/step2 leftover); `raft_entry` records
// carry the real Raft term. The two kinds share one index space but not one
// term semantics, which is why `restore_from_log` reads only `registry_event`.
//
// PreVote
// -------
// A candidate first asks whether it could win *without* incrementing the term.
// A node that has heard from a live leader within its election timeout
// refuses, so a partitioned node cannot force an election storm. The term is
// incremented only after a successful PreVote.
#ifndef CRPC_CLUSTER_RAFT_NODE_HPP
#define CRPC_CLUSTER_RAFT_NODE_HPP

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <crpc/cluster/registry.hpp>
#include <crpc/cluster/registry_log.hpp>
#include <crpc/cluster/raft_state.hpp>
#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/fs/path.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace cluster {

// A node is exactly one of these at any time.
enum class raft_role { follower, candidate, leader };

// Election timeout is drawn uniformly from [min, max] so that two nodes do not
// time out together on every round (Raft section 5.2). `election_timeout` is
// the legacy single value; it is used as both bounds when the min/max pair is
// left at zero.
struct raft_config {
    std::string node_id;
    // Peer ids, excluding self.
    std::vector<std::string> peers;
    std::chrono::milliseconds election_timeout{1000};
    // When non-zero these bound the randomised timeout; otherwise the value
    // above is used for both.
    std::chrono::milliseconds election_timeout_min{0};
    std::chrono::milliseconds election_timeout_max{0};
    std::chrono::milliseconds heartbeat_interval{100};
};

// The messages that cross the (test-supplied) network. One struct carries all
// kinds; the fields that do not apply to `kind` are ignored.
struct raft_message {
    enum class type : u8 {
        prevote = 0,
        prevote_reply = 1,
        vote = 2,
        vote_reply = 3,
        append = 4,
        append_reply = 5
    };

    type kind = type::prevote;
    u64 term = 0;
    std::string from;
    std::string to;

    // prevote / vote
    bool granted = false;

    // append
    u64 prev_log_index = 0;
    u64 prev_log_term = 0;
    u64 leader_commit = 0;
    std::vector<log_record> entries;
    // The term of the entry at `prev_log_index`, as the sender sees it.
    u64 prev_log_term_hint = 0;

    // append_reply
    bool success = false;
    u64 match_index = 0;
    // The first index the follower does not hold, so the leader can retry from
    // there instead of walking backwards one by one.
    u64 conflict_index = 0;
};

// One node's view of the replicated log.
class CRPC_API raft_node {
public:
    raft_node(raft_config config, registry_log& log, fs::path state_dir,
              monotonic_clock clock);

    // Restores term/vote from the state file and the log view from `log`.
    // Idempotent: calling it again reloads from disk.
    [[nodiscard]] status start();

    // Advances the timers and starts an election when the election timeout
    // elapses without a leader heartbeat. Outbound messages go to the send
    // hook installed by `set_send_hook`.
    void tick();

    // Handles one inbound message. Outbound replies go to the send hook.
    void handle(const raft_message& msg);

    // Appends `payload` to the local log as a `raft_entry` of the current term
    // and replicates it. Returns the entry index on success. A node that is
    // not the leader fails with `unavailable`, because silently forwarding or
    // buffering the write would hide a leadership loss from the caller.
    [[nodiscard]] result<u64> propose(span<const std::byte> payload);

    [[nodiscard]] raft_role role() const noexcept;
    [[nodiscard]] u64 term() const noexcept;
    [[nodiscard]] u64 commit_index() const noexcept;
    [[nodiscard]] u64 last_log_index() const noexcept;
    [[nodiscard]] u64 last_log_term() const noexcept;
    [[nodiscard]] std::string leader_id() const;

    // Installs the outbound sink. A test collects messages here and delivers
    // them by calling `handle()` on the peer; production supplies a transport.
    void set_send_hook(std::function<void(const raft_message&)> hook);

    // Test seam: the term of the entry at `index`, or 0 when absent. Exposed
    // because `last_log_term()` alone cannot answer "what term is entry N in",
    // which several L2 cases need.
    [[nodiscard]] u64 entry_term(u64 index) const noexcept;

private:
    struct peer_state {
        u64 next_index = 0;
        u64 match_index = 0;
    };

    [[nodiscard]] status persist_locked() const;
    [[nodiscard]] u64 random_election_timeout() const;
    void become_follower(u64 term, std::string leader);
    void become_leader();
    void send_to(const std::string& peer, const raft_message& msg);
    void broadcast(const raft_message& msg);
    void advance_commit_locked();
    [[nodiscard]] bool log_ok_for_vote(u64 last_term, u64 last_index) const noexcept;
    [[nodiscard]] u64 quorum_size() const noexcept;
    // A node with no peers is its own quorum, so it must not wait for replies
    // that can never arrive.
    [[nodiscard]] bool can_win_alone() const noexcept;

    raft_config config_;
    registry_log& log_;
    fs::path state_dir_;
    monotonic_clock clock_;

    raft_role role_ = raft_role::follower;
    u64 term_ = 0;
    std::string voted_for_;
    std::string leader_id_;
    u64 commit_index_ = 0;
    u64 last_applied_ = 0;
    monotonic_clock::time_point last_heartbeat_{};
    monotonic_clock::time_point election_deadline_{};
    bool voted_this_term_ = false;
    // True once this node has accepted a leader (a valid AppendEntries). A node
    // that has never seen a leader must not refuse PreVotes: its election timer
    // has not expired either, so a timer-only check would block every election.
    bool has_leader_ = false;
    std::vector<peer_state> peers_;
    // Peers that granted a vote in the current election. Tracked separately
    // from `peer_state` because a vote is per election round while
    // `match_index` is the log cursor.
    std::vector<std::string> votes_;
    std::function<void(const raft_message&)> send_hook_;
};

} // namespace cluster
} // namespace crpc

#endif // CRPC_CLUSTER_RAFT_NODE_HPP
