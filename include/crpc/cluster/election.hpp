// CRPC -- leader election primitives.
//
// This is deliberately not a Raft implementation. v1 provides the pieces a
// consensus layer needs and that the registry can use today:
//
//   * PreVote: a candidate asks the cluster whether it could win without
//     incrementing its term. A node that has seen a live leader within the
//     election timeout rejects the PreVote, so a partitioned node cannot
//     disrupt a healthy leader and force an election storm.
//   * Vote: a term is incremented only after a successful PreVote.
//   * Lease-backed leadership: the winner holds a lease from lease_manager.
//     is_leader() is true only while that lease is active. resign() releases
//     the lease immediately.
//   * Fencing epoch: each successful campaign produces a monotonically
//     increasing epoch. The epoch is also used as the registry fencing token,
//     so a deposed leader cannot write after a new leader has written.
//
// NOT provided in v1 (Raft plugs in here):
//   * log replication and commit indices;
//   * persistent term / votedFor / log state;
//   * membership changes (joint consensus);
//   * leader transfer, snapshots, log compaction;
//   * network transport and message serialisation.
// The quorum interface is the seam: a Raft implementation supplies PreVote and
// vote RPCs, persists term/vote, replicates its log, and calls campaign() only
// after its log is at least as up to date as the quorum's.
//
// Deployment scope (v1)
// ---------------------
// This class is a SINGLE-PROCESS primitive, not a multi-node authority. It
// does not provide mutual exclusion on its own: leases are keyed per candidate
// (lease_service, candidate_id), so two candidates can both hold a lease
// against a permissive quorum stub, and the locally derived epoch is not
// comparable across processes (it comes from the local registry index/fence).
// v1 therefore runs exactly ONE registry authority, configured and fenced by an
// operator-supplied authority epoch; it never uses election to pick it. Real
// multi-node failover needs either a central leader lease key on the authority
// or a Raft implementation with persisted term/vote/log, both of which are
// v1.1 scope. Do not advertise this class as "multi-node leader election".
//
// Concurrency contract
// --------------------
//   * election is not thread-safe; it is owned by one authority shard thread.
//   * lease_manager and quorum may be shared with other components, subject to
//     their own contracts, and must outlive the election.
#ifndef CRPC_CLUSTER_ELECTION_HPP
#define CRPC_CLUSTER_ELECTION_HPP

#include <chrono>
#include <string>

#include <crpc/cluster/lease.hpp>

namespace crpc {
namespace cluster {

struct election_config {
    std::chrono::milliseconds election_timeout{1000};
    std::chrono::milliseconds leader_ttl{2000};
    // Votes needed including the candidate's own vote. Zero is treated as one.
    u32 quorum_size = 1;
    std::string lease_service{"crpc.election"};
};

struct prevote_result {
    u32 granted = 0;
    u32 rejected = 0;
    bool leader_alive = false;
    std::string leader_id;
};

struct vote_result {
    u32 granted = 0;
    u32 rejected = 0;
};

// Cluster-facing voting transport. A real implementation sends RPCs to peers;
// tests use a deterministic stub. Implementations must not throw.
class quorum {
public:
    virtual ~quorum() = default;

    // PreVote must not mutate the candidate's term. A peer that has seen a
    // live leader reports leader_alive.
    [[nodiscard]] virtual prevote_result prevote(const std::string& candidate,
                                                 u64 proposed_term) = 0;

    [[nodiscard]] virtual vote_result vote(const std::string& candidate,
                                           u64 term) = 0;
};

struct leadership_token {
    std::string leader_id;
    u64 term = 0;
    u64 epoch = 0;
    u64 lease_id = 0;
    monotonic_clock::time_point expiry;
};

#if defined(CRPC_COMPILER_MSVC)
// Private STL members are an implementation detail; the exported ABI is the
// method set. C4251 would otherwise reject every class with a std:: member in
// a shared build.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
class CRPC_API election {
public:
    election(lease_manager& leases, quorum& voters, monotonic_clock clock,
             election_config config = election_config{});

    election(const election&) = delete;
    election& operator=(const election&) = delete;
    election(election&&) = delete;
    election& operator=(election&&) = delete;

    // PreVote, vote, then lease acquisition. Returns a leadership token on
    // success. A rejection leaves the term unchanged when it happens during
    // PreVote.
    [[nodiscard]] result<leadership_token> campaign(const std::string& candidate_id);

    // Extend the leadership lease. Fails when this node is not the leader.
    [[nodiscard]] status renew();

    // Release leadership immediately (idempotent).
    [[nodiscard]] status resign();

    [[nodiscard]] bool is_leader() const;
    [[nodiscard]] std::string leader_id() const;
    [[nodiscard]] u64 term() const noexcept { return term_; }
    [[nodiscard]] u64 epoch() const noexcept { return epoch_; }

    // Record a leader heartbeat observed from the cluster. A higher term makes
    // a local leader step down. Used by the PreVote live-leader guard.
    void observe_leader(const std::string& leader_id, u64 term);

    // True while this node holds an active leadership lease or a remote leader
    // was observed within election_timeout.
    [[nodiscard]] bool leader_alive() const;

private:
    [[nodiscard]] u32 required_votes() const noexcept;
    [[nodiscard]] bool leader_alive_at(monotonic_clock::time_point now) const;
    [[nodiscard]] leadership_token current_token() const;

    lease_manager& leases_;
    quorum& voters_;
    monotonic_clock clock_;
    election_config config_;

    std::string self_id_;
    std::string leader_id_;
    std::string last_leader_id_;
    monotonic_clock::time_point last_leader_seen_;
    bool has_last_leader_ = false;

    u64 term_ = 0;
    u64 epoch_ = 0;
    u64 lease_id_ = 0;
    monotonic_clock::time_point expiry_;
};

#if defined(CRPC_COMPILER_MSVC)
#pragma warning(pop)
#endif
} // namespace cluster
} // namespace crpc

#endif // CRPC_CLUSTER_ELECTION_HPP
