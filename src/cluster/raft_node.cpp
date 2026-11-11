// CRPC -- G19 step3: Raft consensus node (single-process, test-driven).
//
// The rules implemented here are Raft sections 5.1-5.4. The three that are easy
// to get wrong, and that the tests pin explicitly:
//
//   * A leader may only advance the commit index over an entry of its OWN term
//     (Raft 5.4.2). Counting replicas of an older entry would let a stale
//     entry commit after a leader change.
//   * A vote is granted only when the candidate's log is at least as up to date
//     as the receiver's, compared by (last_log_term, last_log_index) in that
//     order.
//   * A conflicting suffix is discarded, not merged: when the prev_log_index
//     matches but the follower's term there differs from the leader's, every
//     record from that index is removed before the leader's entries are
//     appended.
#include <crpc/cluster/raft_node.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <limits>
#include <random>
#include <utility>

namespace crpc {
namespace cluster {

namespace {

// The leader appends a record of this kind; `registry_event` is the registry
// service's own kind. See raft_node.hpp for the term-semantics note.
constexpr log_record_kind kEntryKind = log_record_kind::raft_entry;

u64 term_of_index(const std::vector<log_record>& stream, u64 index) {
    for (const auto& record : stream) {
        if (record.index == index) {
            return record.term;
        }
    }
    return 0;
}

} // namespace

raft_node::raft_node(raft_config config, registry_log& log, fs::path state_dir,
                     monotonic_clock clock)
    : config_(std::move(config)), log_(log), state_dir_(std::move(state_dir)),
      clock_(clock) {
    peers_.resize(config_.peers.size());
}

status raft_node::start() {
    raft_persistent_state persisted;
    const status loaded = raft_state_store::load(state_dir_, persisted);
    if (!loaded.ok()) {
        return loaded;
    }
    term_ = persisted.term;
    voted_for_ = persisted.voted_for;
    voted_this_term_ = !voted_for_.empty();
    role_ = raft_role::follower;
    leader_id_.clear();
    commit_index_ = 0;
    last_applied_ = 0;

    // Rebuild the log view and the per-peer cursors from what is on disk. The
    // leader always starts `next_index` at its own last index + 1 and lets the
    // AppendEntries consistency check walk it back, so no persisted cursor is
    // needed.
    const auto replayed = log_.replay(0);
    if (!replayed.ok()) {
        return replayed.error();
    }
    const u64 last = log_.last_index();
    for (auto& peer : peers_) {
        peer.next_index = last + 1;
        peer.match_index = 0;
    }
    // A single-node cluster is its own quorum, so nothing on disk can be
    // uncommitted: there was never another node to withhold a majority. Adopting
    // the log's last index here is what lets a restarted single node serve reads
    // and apply its log without waiting for a leader that does not exist.
    //
    // A multi-node node deliberately keeps commit_index at zero. Its own log may
    // hold entries a quorum never accepted, and committing them locally is the
    // exact mistake Raft 5.4.2 forbids; it learns the real value from the
    // leader's next AppendEntries.
    if (can_win_alone()) {
        commit_index_ = last;
    }
    last_heartbeat_ = clock_.read();
    election_deadline_ = last_heartbeat_ +
                         std::chrono::milliseconds(random_election_timeout());
    return status{};
}

void raft_node::set_send_hook(std::function<void(const raft_message&)> hook) {
    send_hook_ = std::move(hook);
}

raft_role raft_node::role() const noexcept { return role_; }

u64 raft_node::term() const noexcept { return term_; }

u64 raft_node::commit_index() const noexcept { return commit_index_; }

u64 raft_node::last_log_index() const noexcept { return log_.last_index(); }

u64 raft_node::last_log_term() const noexcept {
    const auto replayed = log_.replay(0);
    if (!replayed.ok()) {
        return 0;
    }
    return term_of_index(replayed.value(), log_.last_index());
}

u64 raft_node::entry_term(u64 index) const noexcept {
    if (index == 0) {
        return 0;
    }
    const auto replayed = log_.replay(0);
    if (!replayed.ok()) {
        return 0;
    }
    return term_of_index(replayed.value(), index);
}

std::string raft_node::leader_id() const { return leader_id_; }

u64 raft_node::quorum_size() const noexcept {
    return (config_.peers.size() + 1) / 2 + 1;
}

u64 raft_node::random_election_timeout() const {
    std::chrono::milliseconds low = config_.election_timeout;
    if (config_.election_timeout_min.count() > 0) {
        low = config_.election_timeout_min;
    }
    std::chrono::milliseconds high = config_.election_timeout;
    if (config_.election_timeout_max.count() > 0) {
        high = config_.election_timeout_max;
    }
    if (high < low) {
        std::swap(low, high);
    }
    if (low == high) {
        return static_cast<u64>(low.count());
    }
    // A local generator rather than a global one: the node is single-threaded
    // and a global would couple unrelated nodes' election timing.
    static thread_local std::mt19937_64 engine{std::random_device{}()};
    std::uniform_int_distribution<long long> pick(
        static_cast<long long>(low.count()),
        static_cast<long long>(high.count()));
    return static_cast<u64>(pick(engine));
}

status raft_node::persist_locked() const {
    raft_persistent_state value;
    value.term = term_;
    value.voted_for = voted_for_;
    return raft_state_store::save(state_dir_, value);
}

void raft_node::become_follower(u64 term, std::string leader) {
    // The vote is per term. Entering a DIFFERENT term clears it; an election
    // timeout inside the same term does not un-vote (the node already voted and
    // must not vote again for a different candidate in that term).
    const bool term_changed = term != term_;
    if (term_changed) {
        voted_for_.clear();
        voted_this_term_ = false;
    }
    term_ = term;
    role_ = raft_role::follower;
    leader_id_ = std::move(leader);
    // The term is durable state (Raft section 5.1: "current term" is one of the
    // three values that must survive a restart). A follower that adopts a higher
    // term without persisting it would, after a restart, vote a second time in
    // that term -- which is exactly the split-brain the persistence prevents.
    if (term_changed) {
        (void)persist_locked();
    }
}

void raft_node::become_leader() {
    role_ = raft_role::leader;
    leader_id_ = config_.node_id;
    leader_since_ = clock_.read();
    const u64 last = log_.last_index();
    for (auto& peer : peers_) {
        peer.next_index = last + 1;
        peer.match_index = 0;
        // The epoch, not `now`: CheckQuorum must be able to tell "this peer has
        // never answered" from "this peer answered a moment ago". Seeding to
        // `now` would make a leader that has heard from nobody look healthy for
        // one window, which is the opposite of what the check is for.
        peer.last_ack = monotonic_clock::time_point{};
    }
}

bool raft_node::can_win_alone() const noexcept {
    return config_.peers.empty();
}

void raft_node::send_to(const std::string& peer, const raft_message& msg) {
    if (send_hook_) {
        send_hook_(msg);
    }
    (void)peer;
}

void raft_node::broadcast(const raft_message& msg) {
    for (const auto& peer : config_.peers) {
        // A node never sends to itself: it counts its own vote locally. Sending
        // to itself would let a peer-list that (incorrectly or not) contains the
        // local id consume the one vote the candidate is guaranteed.
        if (peer == config_.node_id) {
            continue;
        }
        raft_message copy = msg;
        copy.to = peer;
        send_to(peer, copy);
    }
}

void raft_node::tick() {
    const auto now = clock_.read();
    if (role_ == raft_role::leader) {
        // Heartbeats are periodic and empty: they carry no entries, so a
        // follower whose log already matches has nothing to write.
        if (now - last_heartbeat_ >= config_.heartbeat_interval) {
            raft_message beat;
            beat.kind = raft_message::type::append;
            beat.term = term_;
            beat.from = config_.node_id;
            beat.prev_log_index = log_.last_index();
            beat.prev_log_term = last_log_term();
            beat.leader_commit = commit_index_;
            broadcast(beat);
            last_heartbeat_ = now;

            // CheckQuorum runs AFTER the heartbeat, not before it: the
            // transport delivers replies synchronously, so a reachable peer
            // has already refreshed `last_ack` by the time this runs. Checking
            // first would count the interval that just elapsed as silence and
            // depose a healthy leader.
            //
            // Two guards keep it from firing too early:
            //   * a fresh leader is exempt for one election timeout, so a
            //     single lost heartbeat cannot depose it, and
            //   * a peer that has never answered is not counted as reachable
            //     (`last_ack` is the epoch until it answers).
            if (!can_win_alone()) {
                const auto window =
                    std::chrono::milliseconds(random_election_timeout());
                if (now - leader_since_ >= window) {
                    std::size_t reachable = 1; // the leader itself
                    for (const auto& peer : peers_) {
                        if (peer.last_ack != monotonic_clock::time_point{} &&
                            now - peer.last_ack < window) {
                            ++reachable;
                        }
                    }
                    if (reachable < quorum_size()) {
                        become_follower(term_, std::string{});
                        return;
                    }
                }
            }
        }
        return;
    }
    if (now >= election_deadline_) {
        // PreVote asks "could I win if I start an election now". It MUST NOT
        // increment the term: a probe that bumped the term would make every
        // peer adopt it, so a partitioned node could drag the whole cluster's
        // term upwards one step at a time (the failure mode
        // `election.cpp:111-113` and Raft section 8 both warn about).
        //
        // `tick()` therefore leaves `term_` alone and records the term this
        // candidacy is FOR. The increment happens in `prevote_reply` once a
        // majority has agreed.
        role_ = raft_role::candidate;
        leader_id_.clear();
        votes_.clear();
        election_deadline_ = now + std::chrono::milliseconds(random_election_timeout());
        // A single-node cluster has no one to ask: the pre-vote would never be
        // answered and the node would campaign forever.
        if (can_win_alone()) {
            ++term_;
            become_leader();
            (void)persist_locked();
            raft_message beat;
            beat.kind = raft_message::type::append;
            beat.term = term_;
            beat.from = config_.node_id;
            beat.prev_log_index = log_.last_index();
            beat.prev_log_term = last_log_term();
            beat.leader_commit = commit_index_;
            broadcast(beat);
            last_heartbeat_ = now;
            return;
        }
        raft_message pre;
        pre.kind = raft_message::type::prevote;
        // The term the candidate WOULD adopt. Peers must not adopt it; they
        // answer in their own term and the candidate recognises its own probe.
        pre.term = term_ + 1;
        pre.from = config_.node_id;
        pre.prev_log_index = log_.last_index();
        pre.prev_log_term_hint = last_log_term();
        broadcast(pre);
    }
}

bool raft_node::log_ok_for_vote(u64 last_term, u64 last_index) const noexcept {
    const u64 mine_term = last_log_term();
    const u64 mine_index = log_.last_index();
    if (last_term != mine_term) {
        return last_term > mine_term;
    }
    return last_index >= mine_index;
}

void raft_node::handle(const raft_message& msg) {
    // A PreVote is answered in the RECEIVER's term and never changes it: the
    // whole point is that a probe must not bump a healthy node's term. Any
    // other message from a higher term is adopted.
    if (msg.kind != raft_message::type::prevote && msg.term > term_) {
        // A higher term always wins, whatever the message is.
        become_follower(msg.term, role_ == raft_role::leader ? std::string{} : leader_id_);
        if (role_ == raft_role::leader) {
            leader_id_.clear();
        }
        has_leader_ = false;
        voted_for_.clear();
        voted_this_term_ = false;
        (void)persist_locked();
    }

    switch (msg.kind) {
    case raft_message::type::prevote: {
        // A candidate may not disrupt a leader that is still alive, but that
        // guard is deliberately NOT here. Raft's PreVote only requires the
        // log-up-to-date check; refusing a probe because a leader was seen
        // recently is an optional optimisation, and maintaining "was a leader
        // seen" reliably proved to be the source of a livelock (a node whose
        // own timer keeps being extended looks permanently fresh). A partitioned
        // node that prevotes does not bump anyone's term, so the cost of dropping
        // the guard is a harmless probe, not an election storm.
        raft_message reply;
        reply.kind = raft_message::type::prevote_reply;
        // Answer in THIS node's term, never the probe's. The probe's term is a
        // proposal, not a fact; echoing it would let a candidate drag a healthy
        // node's term upwards one probe at a time.
        reply.term = term_;
        reply.from = config_.node_id;
        reply.to = msg.from;
        reply.granted =
            log_ok_for_vote(msg.prev_log_term_hint, msg.prev_log_index);
        send_to(msg.from, reply);
        return;
    }
    case raft_message::type::prevote_reply: {
        if (role_ != raft_role::candidate) {
            return;
        }
        // A reply in a term other than the one this candidacy is for is not
        // about this probe. In particular, a reply in the candidate's own term
        // (term_ + 1 was the proposal, term_ is the current term) still counts:
        // the peer has not moved on, so the probe is still live.
        if (msg.term > term_ + 1) {
            return;
        }
        if (!msg.granted) {
            return;
        }
        const auto already = std::find(votes_.begin(), votes_.end(), msg.from);
        if (already != votes_.end()) {
            return;
        }
        votes_.push_back(msg.from);
        // A pre-vote majority means the candidate could win, so it now spends
        // the term increment on a real election. The PreVote itself never
        // touched `term_`, which is why this is safe.
        if (votes_.size() + 1 >= quorum_size()) {
            ++term_;
            voted_for_ = config_.node_id;
            voted_this_term_ = true;
            (void)persist_locked();
            raft_message vote;
            vote.kind = raft_message::type::vote;
            vote.term = term_;
            vote.from = config_.node_id;
            vote.prev_log_index = log_.last_index();
            vote.prev_log_term_hint = last_log_term();
            broadcast(vote);
        }
        return;
    }
    case raft_message::type::vote: {
        raft_message reply;
        reply.kind = raft_message::type::vote_reply;
        reply.term = msg.term;
        reply.from = config_.node_id;
        reply.to = msg.from;
        const bool up_to_date = log_ok_for_vote(msg.prev_log_term_hint,
                                                msg.prev_log_index);
        if (!voted_this_term_ && up_to_date) {
            voted_this_term_ = true;
            voted_for_ = msg.from;
            (void)persist_locked();
            reply.granted = true;
            election_deadline_ = clock_.read() +
                                std::chrono::milliseconds(random_election_timeout());
        } else {
            reply.granted = false;
        }
        send_to(msg.from, reply);
        return;
    }
    case raft_message::type::vote_reply: {
        if (role_ != raft_role::candidate) {
            return;
        }
        // A reply in a term other than the one this candidacy is for is stale.
        if (msg.term != term_) {
            return;
        }
        if (!msg.granted) {
            return;
        }
        const auto already = std::find(votes_.begin(), votes_.end(), msg.from);
        if (already != votes_.end()) {
            return;
        }
        votes_.push_back(msg.from);
        // The candidate's own vote is cast locally, so the count includes it.
        if (votes_.size() + 1 >= quorum_size()) {
            become_leader();
            raft_message beat;
            beat.kind = raft_message::type::append;
            beat.term = term_;
            beat.from = config_.node_id;
            beat.prev_log_index = log_.last_index();
            beat.prev_log_term = last_log_term();
            beat.leader_commit = commit_index_;
            broadcast(beat);
            last_heartbeat_ = clock_.read();
        }
        return;
    }
    case raft_message::type::append: {
        // Reject a message from a stale term before touching any state: a
        // delayed heartbeat from a deposed leader must not reset this node's
        // election timer, or a deposed leader could keep an election from ever
        // starting.
        if (msg.term < term_) {
            return;
        }
        // A leader heartbeat or replication. A higher term was handled above, so
        // this is the current leader (or this node's own term).
        leader_id_ = msg.from;
        has_leader_ = true;
        last_heartbeat_ = clock_.read();
        election_deadline_ = last_heartbeat_ +
                            std::chrono::milliseconds(random_election_timeout());
        role_ = raft_role::follower;

        const u64 last = log_.last_index();
        const bool prev_ok =
            msg.prev_log_index == 0 ||
            (msg.prev_log_index <= last &&
             entry_term(msg.prev_log_index) == msg.prev_log_term);
        if (!prev_ok) {
            raft_message reply;
            reply.kind = raft_message::type::append_reply;
            reply.term = term_;
            reply.from = config_.node_id;
            reply.to = msg.from;
            reply.success = false;
            reply.match_index = 0;
            // Tell the leader where to resume, so it does not walk back one
            // index at a time.
            reply.conflict_index = msg.prev_log_index == 0 ? 1 : msg.prev_log_index;
            send_to(msg.from, reply);
            return;
        }

        // Append the entries the leader sent, truncating a conflicting suffix.
        for (const auto& entry : msg.entries) {
            const u64 existing_term = entry_term(entry.index);
            if (existing_term != 0 && existing_term != entry.term) {
                (void)log_.truncate_suffix(entry.index);
                break;
            }
        }
        for (const auto& entry : msg.entries) {
            if (entry_term(entry.index) == entry.term) {
                continue;
            }
            const result<u64> appended = log_.append(entry);
            if (!appended.ok()) {
                break;
            }
        }
        // A follower discards a suffix the leader has superseded. The boundary is
        // the end of the leader's batch: everything through
        // `prev_log_index + entries.size()` is now consistent with the leader
        // (the loop above appended it), and anything beyond it is a stale record
        // from an older term or an entry a deposed leader appended.
        //
        // An empty batch (a heartbeat) therefore trims back to `prev_log_index`,
        // which is exactly how L2-2 removes a minority's uncommitted suffix: the
        // new leader's heartbeat carries its own last index, and the follower
        // sheds whatever lies past it.
        const u64 leader_prefix = msg.prev_log_index + msg.entries.size();
        if (log_.last_index() > leader_prefix) {
            (void)log_.truncate_suffix(leader_prefix + 1);
        }

        // The leader's commit index only ever advances this node's view; it can
        // never move it backwards.
        if (msg.leader_commit > commit_index_) {
            const u64 last_new = log_.last_index();
            commit_index_ = std::min(msg.leader_commit, last_new);
        }
        (void)log_.sync();

        raft_message reply;
        reply.kind = raft_message::type::append_reply;
        reply.term = term_;
        reply.from = config_.node_id;
        reply.to = msg.from;
        reply.success = true;
        reply.match_index = msg.prev_log_index + msg.entries.size();
        send_to(msg.from, reply);
        return;
    }
    case raft_message::type::append_reply: {
        if (role_ != raft_role::leader) {
            return;
        }
        // Any reply -- success or a conflict report -- proves the peer is
        // reachable, which is what CheckQuorum measures. Recording it only on
        // success would depose a leader whose followers are merely behind.
        {
            auto it = std::find_if(config_.peers.begin(), config_.peers.end(),
                                   [&msg](const std::string& id) {
                                       return id == msg.from;
                                   });
            if (it != config_.peers.end()) {
                const std::size_t i =
                    static_cast<std::size_t>(it - config_.peers.begin());
                peers_[i].last_ack = clock_.read();
            }
        }
        if (msg.success) {
            auto it = std::find_if(config_.peers.begin(), config_.peers.end(),
                                   [&msg](const std::string& id) {
                                       return id == msg.from;
                                   });
            if (it == config_.peers.end()) {
                return;
            }
            const std::size_t i = static_cast<std::size_t>(it - config_.peers.begin());
            peers_[i].match_index = std::max(peers_[i].match_index, msg.match_index);
            peers_[i].next_index = peers_[i].match_index + 1;
            advance_commit_locked();
            return;
        }
        // Retry from the follower's conflict point, or one before the current
        // cursor when it reported nothing.
        auto it = std::find_if(config_.peers.begin(), config_.peers.end(),
                               [&msg](const std::string& id) {
                                   return id == msg.from;
                               });
        if (it == config_.peers.end()) {
            return;
        }
        const std::size_t i = static_cast<std::size_t>(it - config_.peers.begin());
        if (msg.conflict_index > 0 && msg.conflict_index < peers_[i].next_index) {
            peers_[i].next_index = msg.conflict_index;
        } else if (peers_[i].next_index > 1) {
            --peers_[i].next_index;
        }
        // Send the corrected batch now rather than waiting for the next tick.
        raft_message retry;
        retry.kind = raft_message::type::append;
        retry.term = term_;
        retry.from = config_.node_id;
        retry.to = msg.from;
        retry.prev_log_index = peers_[i].next_index - 1;
        retry.prev_log_term = entry_term(retry.prev_log_index);
        retry.leader_commit = commit_index_;
        send_to(msg.from, retry);
        return;
    }
    }
}

void raft_node::advance_commit_locked() {
    // Raft 5.4.2: only an entry of the current term may be committed by
    // counting replicas. Advancing over an older entry by replica count would
    // let a stale entry commit after a leader change.
    const u64 last = log_.last_index();
    for (u64 index = commit_index_ + 1; index <= last; ++index) {
        if (entry_term(index) != term_) {
            continue;
        }
        u64 replicas = 1; // the leader holds it
        for (const auto& peer : peers_) {
            if (peer.match_index >= index) {
                ++replicas;
            }
        }
        if (replicas >= quorum_size()) {
            commit_index_ = index;
        } else {
            break;
        }
    }
}

result<u64> raft_node::propose(span<const std::byte> payload) {
    if (role_ != raft_role::leader) {
        return rpc_error(rpc_code::unavailable,
                         "raft: only the leader can propose");
    }
    log_record record;
    record.kind = kEntryKind;
    record.term = term_;
    record.index = log_.last_index() + 1;
    record.payload.assign(payload.data(), payload.data() + payload.size());
    const result<u64> appended = log_.append(record);
    if (!appended.ok()) {
        return appended.error();
    }
    (void)log_.sync();

    // Replicate immediately so a client does not wait for the next heartbeat.
    raft_message msg;
    msg.kind = raft_message::type::append;
    msg.term = term_;
    msg.from = config_.node_id;
    msg.prev_log_index = record.index - 1;
    msg.prev_log_term = entry_term(msg.prev_log_index);
    msg.leader_commit = commit_index_;
    msg.entries.push_back(std::move(record));
    broadcast(msg);

    // A single-node cluster has no peer whose reply would advance the commit
    // index, so `propose` must do it itself. `wait_for_commit` documents that
    // "propose already advanced the commit index" for exactly this case; without
    // this call that promise is false and every single-node proposal times out.
    // With peers the loop below is a no-op until a majority acknowledges.
    advance_commit_locked();
    return appended;
}

result<u64> raft_node::wait_for_commit(u64 index,
                                       monotonic_clock::time_point deadline) {
    // A single-node cluster commits on its own: `propose` already advanced the
    // commit index, so the loop body never runs. That is not a special case
    // here -- it falls out of the same condition.
    //
    // The clock is injected and read-only, so this loop cannot advance time
    // itself. It therefore drives replication directly (rather than waiting for
    // the heartbeat interval to elapse) and bounds the wait by BOTH the
    // deadline and an iteration cap. The cap is what terminates a partitioned
    // wait: with a frozen clock the deadline never arrives, and an unbounded
    // loop would hang instead of failing loudly.
    constexpr int kMaxPumps = 1000;
    for (int pump = 0; commit_index_ < index; ++pump) {
        // A leadership loss ends the wait immediately. Continuing would let a
        // deposed leader report a commit that a new leader may never make,
        // which is exactly the acknowledgement S4 forbids.
        if (role_ != raft_role::leader) {
            return rpc_error(rpc_code::unavailable,
                             "raft: leadership lost while waiting for commit");
        }
        if (clock_.read() >= deadline || pump >= kMaxPumps) {
            return rpc_error(rpc_code::unavailable,
                             "raft: commit timed out");
        }
        // Send whatever each peer is missing. The transport delivers replies
        // synchronously, so `commit_index_` can advance inside this call.
        pump_replication();
    }
    return commit_index_;
}

void raft_node::pump_replication() {
    const auto replayed = log_.replay(0);
    if (!replayed.ok()) {
        return;
    }
    const std::vector<log_record>& records = replayed.value();
    for (std::size_t i = 0; i < peers_.size(); ++i) {
        raft_message msg;
        msg.kind = raft_message::type::append;
        msg.term = term_;
        msg.from = config_.node_id;
        msg.to = config_.peers[i];
        msg.prev_log_index = peers_[i].next_index - 1;
        msg.prev_log_term = entry_term(msg.prev_log_index);
        msg.leader_commit = commit_index_;
        for (const auto& record : records) {
            if (record.index >= peers_[i].next_index) {
                msg.entries.push_back(record);
            }
        }
        send_to(config_.peers[i], msg);
    }
}

} // namespace cluster
} // namespace crpc
