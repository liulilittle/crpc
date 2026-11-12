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
// D-B-11: there are TWO distinct log concepts with TWO separate index spaces,
// and they must never be confused.
//
//   1. `registry_log` (the WAL, persistent): dense index space 1..N across ALL
//      record kinds (registry_event AND raft_entry). `restore_from_log` replays
//      only registry_event records (term==0 ones); raft_entry records with a
//      real term are not interpreted here because they belong to the Raft
//      state machine, not the catalog.
//
//   2. Raft's own log (in-memory on `raft_node`, term-carrying entries): its
//      index is the position in the sequence of proposed raft commands. It
//      shares the dense numbering with registry_log but lives in the same
//      index space: a raft entry proposed at registry_log index k occupies
//      raft log index k. Committing raft index k advances `commit_index` to k.
//
// A command's raft index IS its registry_log index: they are one dense sequence.
// Two separate structures (the WAL file and the in-memory entry vector), but a
// single index space. Writing to one without the other, or reading raft state
// from registry_event records (or vice versa), is a bug.
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
#include <mutex>
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
//
// Thread safety (G19 step4 D-A-2 / D-B-3)
// ---------------------------------------
// A node is reached from more than one thread in production: `registryd` drives
// `tick()` from a timer thread while the HTTP handler threads call `propose()`
// and `apply_committed()` reads `commit_index()`/`log()`. Every public method
// therefore takes `mutex_`.
//
// The lock is NOT held across the send hook. `send_to` calls the hook
// synchronously, and a test transport (and a real in-process one) delivers the
// message by calling `handle()` on the peer, which can call back into THIS node
// (`append_reply` -> `advance_commit_locked`). Holding a non-recursive mutex
// across that re-entry self-deadlocks, and a recursive one would let the
// re-entrant call observe a half-updated `match_index`. So the rule is:
//
//   * a public method takes the lock, mutates state, and builds the outbound
//     messages into a local vector;
//   * it releases the lock;
//   * it then delivers the messages.
//
// A re-entrant `handle()` therefore sees a fully consistent state, and the
// outer call's post-delivery work (`advance_commit_locked`) is a no-op because
// the re-entrant call already advanced the commit index.
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
    [[nodiscard]] result<u64> last_log_term() const noexcept;
    [[nodiscard]] std::string leader_id() const;

    // The injected clock's current reading. Used by callers (e.g. the registry
    // service) to build wait_for_commit deadlines on the same clock source the
    // node compares against, so a real-time deadline and an injected clock
    // never disagree.
    [[nodiscard]] monotonic_clock::time_point now() const noexcept;

    // The log this node replicates. The state machine reads committed entries
    // from here: the service's own catalog log is a different file, and an
    // apply loop that read the wrong one would silently apply nothing.
    [[nodiscard]] registry_log& log() noexcept { return log_; }

    // Installs the outbound sink. A test collects messages here and delivers
    // them by calling `handle()` on the peer; production supplies a transport.
    void set_send_hook(std::function<void(const raft_message&)> hook);

    // Test seam: the term of the entry at `index`, or an error when the entry
    // is absent / the log is corrupt.
    [[nodiscard]] result<u64> entry_term(u64 index) const noexcept;

    // Drives `tick()` until `commit_index() >= index` or `deadline` passes.
    //
    // `propose` returns as soon as the entry is appended and replicated; it
    // does NOT wait for a majority, so a leader that has lost contact with its
    // peers still gets a success from it. A caller that must not acknowledge an
    // uncommitted write (S4) calls this afterwards.
    //
    // Fails with `unavailable` when the deadline passes, and immediately when
    // this node is no longer the leader: waiting after a leadership loss would
    // hide the loss from the caller, which is the failure S4 forbids.
    [[nodiscard]] result<u64> wait_for_commit(
        u64 index, monotonic_clock::time_point deadline);

private:
    struct peer_state {
        u64 next_index = 0;
        u64 match_index = 0;
        // When this peer last answered an AppendEntries. CheckQuorum reads it:
        // a leader that has not heard from a majority within one election
        // timeout is a leader that cannot commit, and must step down rather
        // than keep acknowledging writes it can never make durable.
        monotonic_clock::time_point last_ack{};
    };

    [[nodiscard]] status persist_locked() const;
    [[nodiscard]] u64 random_election_timeout() const;
    void become_follower(u64 term, std::string leader);
    void become_leader();
    void send_to(const std::string& peer, const raft_message& msg);
    void broadcast(const raft_message& msg);
    void advance_commit_locked();
    // Sends each peer the entries it is missing. Used by `wait_for_commit`,
    // which cannot rely on the heartbeat interval because the injected clock
    // does not advance on its own.
    void pump_replication();
    [[nodiscard]] bool log_ok_for_vote(u64 last_term, u64 last_index) const noexcept;
    [[nodiscard]] u64 quorum_size() const noexcept;
    // A node with no peers is its own quorum, so it must not wait for replies
    // that can never arrive.
    [[nodiscard]] bool can_win_alone() const noexcept;

    // Lock-held variants of the public readers. The public ones take `mutex_`
    // and forward here; internal callers already hold it.
    [[nodiscard]] u64 last_log_term_locked() const noexcept;
    [[nodiscard]] u64 entry_term_locked(u64 index) const noexcept;

    // Delivers a batch of messages built under the lock. Called with the lock
    // RELEASED: the hook may re-enter `handle()` on this node.
    void deliver(const std::vector<raft_message>& messages);

    // Serialises every public entry point. See the class comment for why the
    // lock is never held across `send_to`.
    mutable std::mutex mutex_;

    raft_config config_;
    registry_log& log_;
    fs::path state_dir_;
    monotonic_clock clock_;
    // D-B-9: O(1) (index -> term) cache so `entry_term_locked` no longer
    // replays the whole log per lookup. Maintained on start() (single replay),
    // append (insert new entry) and truncate_suffix (drop evicted entries).
    mutable std::map<u64, u64> index_term_cache_;

    raft_role role_ = raft_role::follower;
    u64 term_ = 0;
    std::string voted_for_;
    std::string leader_id_;
    u64 commit_index_ = 0;
    u64 last_applied_ = 0;
    monotonic_clock::time_point last_heartbeat_{};
    monotonic_clock::time_point election_deadline_{};
    // When this node last became leader. CheckQuorum is suppressed for one
    // election window after this instant so a single lost heartbeat cannot
    // depose a leader that was just elected.
    monotonic_clock::time_point leader_since_{};
    bool voted_this_term_ = false;
    // True while a PreVote round is collecting grants, false once it has
    // converted to a real election. This is the key the `prevote_reply` guard
    // uses to reject a PreVote grant that arrives after the round is over.
    //
    // It is deliberately NOT `voted_this_term_`/`voted_for_`: those are the
    // per-term vote record and are NOT reset when a new PreVote round starts in
    // the same term, so keying the guard on them would reject every grant in a
    // second election round and the node could never win one (a liveness
    // defect). A dedicated round flag is set on each new round and cleared on
    // conversion, which is exactly the window the guard needs.
    bool prevote_round_active_ = false;
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
