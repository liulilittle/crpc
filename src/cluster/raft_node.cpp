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
//
// Threading (G19 step4 D-A-2 / D-B-3)
// -----------------------------------
// Every public method takes `mutex_`. The lock is NEVER held across the send
// hook: `send_to` calls the hook synchronously, and a transport delivers the
// message by calling `handle()` on the peer, which can re-enter THIS node
// (`append_reply` -> `advance_commit_locked`). Holding a non-recursive mutex
// across that re-entry self-deadlocks; a recursive one would let the re-entrant
// call observe a half-updated `match_index`. So each public method mutates
// state and builds its outbound messages under the lock, releases it, and only
// then delivers. A re-entrant `handle()` therefore sees a fully consistent
// state, and the outer call's post-delivery `advance_commit_locked` is a no-op
// because the re-entrant call already advanced the commit index.
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

} // namespace

raft_node::raft_node(raft_config config, registry_log& log, fs::path state_dir,
                     monotonic_clock clock)
    : config_(std::move(config)), log_(log), state_dir_(std::move(state_dir)),
      clock_(clock) {
    peers_.resize(config_.peers.size());
}

status raft_node::start() {
    std::lock_guard<std::mutex> lock(mutex_);
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
    index_term_cache_.clear();
    for (const auto& record : replayed.value()) {
        index_term_cache_[record.index] = record.term;
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
    // But only entries written in THIS term are safe to adopt: an entry whose
    // term differs from the persisted term was written by a previous (ghost)
    // incarnation and must not be committed just because this node is alone.
    // D-B-5: a 3->1 shrink followed by restart must not "ghost-commit" an entry
    // a quorum never accepted.
    //
    // A multi-node node deliberately keeps commit_index at zero. Its own log may
    // hold entries a quorum never accepted, and committing them locally is the
    // exact mistake Raft 5.4.2 forbids; it learns the real value from the
    // leader's next AppendEntries.
    if (can_win_alone()) {
        const u64 last_term = last_log_term_locked();
        if (last_term == 0 || last_term == term_) {
            commit_index_ = last;
        }
    }
    last_heartbeat_ = clock_.read();
    election_deadline_ = last_heartbeat_ +
                         std::chrono::milliseconds(random_election_timeout());
    return status{};
}

void raft_node::set_send_hook(std::function<void(const raft_message&)> hook) {
    std::lock_guard<std::mutex> lock(mutex_);
    send_hook_ = std::move(hook);
}

raft_role raft_node::role() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return role_;
}

u64 raft_node::term() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return term_;
}

u64 raft_node::commit_index() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return commit_index_;
}

u64 raft_node::last_log_index() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return log_.last_index();
}

u64 raft_node::last_log_term_locked() const noexcept {
    const u64 last = log_.last_index();
    const auto it = index_term_cache_.find(last);
    if (it != index_term_cache_.end()) {
        return it->second;
    }
    // Cache miss: full replay fallback and populate the cache so subsequent
    // calls are O(1). Entries appended directly to `node->log` after `start()`
    // bypass raft_node's cache maintenance.
    const auto replayed = log_.replay(0);
    if (!replayed.ok()) {
        return 0;
    }
    u64 term = 0;
    for (const auto& record : replayed.value()) {
        index_term_cache_[record.index] = record.term;
        if (record.index == last) {
            term = record.term;
        }
    }
    return term;
}

result<u64> raft_node::last_log_term() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    // Always replay, mirroring entry_term(): a cache hit above would answer
    // ok(term) without observing a failing/corrupt log, which a result<u64>
    // signature must not hide. O(n) replay is acceptable for this public
    // reader; the internal hot paths keep using the O(1) locked helper.
    const auto replayed = log_.replay(0);
    if (!replayed.ok()) {
        return replayed.error();
    }
    const u64 last = log_.last_index();
    u64 term = 0;
    for (const auto& record : replayed.value()) {
        if (record.index == last) {
            term = record.term;
            break;
        }
    }
    return u64{term};
}

u64 raft_node::entry_term_locked(u64 index) const noexcept {
    if (index == 0) {
        return 0;
    }
    const auto it = index_term_cache_.find(index);
    return it == index_term_cache_.end() ? 0 : it->second;
}

result<u64> raft_node::entry_term(u64 index) const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index == 0) {
        return u64{0};
    }
    // Always replay: tests append directly to `node->log` after `start()`,
    // which bypasses raft_node's cache maintenance. `entry_term_locked()`
    // stays O(1) for the internal hot paths; this public reader is called
    // once per apply_committed(), so O(n) replay here is acceptable.
    const auto replayed = log_.replay(0);
    if (!replayed.ok()) {
        return replayed.error();
    }
    u64 term = 0;
    for (const auto& record : replayed.value()) {
        if (record.index == index) {
            term = record.term;
            break;
        }
    }
    return u64{term};
}

std::string raft_node::leader_id() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return leader_id_;
}

monotonic_clock::time_point raft_node::now() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return clock_.read();
}

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

void raft_node::deliver(const std::vector<raft_message>& messages) {
    // Called with the lock RELEASED. The hook may re-enter `handle()` on this
    // node, which is why the caller must have finished mutating state first.
    for (const raft_message& msg : messages) {
        send_to(msg.to, msg);
    }
}

void raft_node::tick() {
    std::vector<raft_message> outbound;
    bool sent_heartbeat = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
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
                beat.prev_log_term = last_log_term_locked();
                beat.leader_commit = commit_index_;
                for (const auto& peer : config_.peers) {
                    if (peer == config_.node_id) {
                        continue;
                    }
                    raft_message copy = beat;
                    copy.to = peer;
                    outbound.push_back(std::move(copy));
                }
                last_heartbeat_ = now;
                sent_heartbeat = true;
            }
        } else if (now >= election_deadline_) {
            // PreVote asks "could I win if I start an election now". It MUST NOT
            // increment the term: a probe that bumped the term would make every
            // peer adopt it, so a partitioned node could drag the whole
            // cluster's term upwards one step at a time (the failure mode
            // `election.cpp:111-113` and Raft section 8 both warn about).
            //
            // `tick()` therefore leaves `term_` alone and records the term this
            // candidacy is FOR. The increment happens in `prevote_reply` once a
            // majority has agreed.
            role_ = raft_role::candidate;
            leader_id_.clear();
            votes_.clear();
            // A new PreVote round is starting: grants are accepted again. The
            // flag is cleared when the round converts to a real election, so a
            // PreVote grant that arrives after the conversion is rejected.
            prevote_round_active_ = true;
            election_deadline_ =
                now + std::chrono::milliseconds(random_election_timeout());
            // A single-node cluster has no one to ask: the pre-vote would never
            // be answered and the node would campaign forever.
            if (can_win_alone()) {
                ++term_;
                become_leader();
                (void)persist_locked();
                raft_message beat;
                beat.kind = raft_message::type::append;
                beat.term = term_;
                beat.from = config_.node_id;
                beat.prev_log_index = log_.last_index();
                beat.prev_log_term = last_log_term_locked();
                beat.leader_commit = commit_index_;
                for (const auto& peer : config_.peers) {
                    if (peer == config_.node_id) {
                        continue;
                    }
                    raft_message copy = beat;
                    copy.to = peer;
                    outbound.push_back(std::move(copy));
                }
                last_heartbeat_ = now;
            } else {
                raft_message pre;
                pre.kind = raft_message::type::prevote;
                // The term the candidate WOULD adopt. Peers must not adopt it;
                // they answer in their own term and the candidate recognises
                // its own probe.
                pre.term = term_ + 1;
                pre.from = config_.node_id;
                pre.prev_log_index = log_.last_index();
                pre.prev_log_term_hint = last_log_term_locked();
                for (const auto& peer : config_.peers) {
                    if (peer == config_.node_id) {
                        continue;
                    }
                    raft_message copy = pre;
                    copy.to = peer;
                    outbound.push_back(std::move(copy));
                }
            }
        }
    }
    // Deliver with the lock released: the hook may re-enter `handle()` on this
    // node, which refreshes `last_ack` for a reachable peer.
    deliver(outbound);

    // CheckQuorum runs AFTER the heartbeat is delivered, not before it: the
    // transport delivers replies synchronously, so a reachable peer has already
    // refreshed `last_ack` by the time this runs. Checking first would count the
    // interval that just elapsed as silence and depose a healthy leader.
    //
    // Two guards keep it from firing too early:
    //   * a fresh leader is exempt for one election timeout, so a single lost
    //     heartbeat cannot depose it, and
    //   * a peer that has never answered is not counted as reachable
    //     (`last_ack` is the epoch until it answers).
    if (!sent_heartbeat) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (role_ != raft_role::leader || can_win_alone()) {
        return;
    }
    const auto now = clock_.read();
    const auto window = std::chrono::milliseconds(random_election_timeout());
    if (now - leader_since_ < window) {
        return;
    }
    std::size_t reachable = 1; // the leader itself
    for (const auto& peer : peers_) {
        if (peer.last_ack != monotonic_clock::time_point{} &&
            now - peer.last_ack < window) {
            ++reachable;
        }
    }
    if (reachable < quorum_size()) {
        become_follower(term_, std::string{});
    }
}

bool raft_node::log_ok_for_vote(u64 last_term, u64 last_index) const noexcept {
    const u64 mine_term = last_log_term_locked();
    const u64 mine_index = log_.last_index();
    if (last_term != mine_term) {
        return last_term > mine_term;
    }
    return last_index >= mine_index;
}

void raft_node::handle(const raft_message& msg) {
    std::vector<raft_message> outbound;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // A PreVote is answered in the RECEIVER's term and never changes it: the
        // whole point is that a probe must not bump a healthy node's term. Any
        // other message from a higher term is adopted.
        if (msg.kind != raft_message::type::prevote && msg.term > term_) {
            // A higher term always wins, whatever the message is.
            become_follower(msg.term,
                            role_ == raft_role::leader ? std::string{}
                                                       : leader_id_);
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
            // recently is an optional optimisation, and maintaining "was a
            // leader seen" reliably proved to be the source of a livelock (a
            // node whose own timer keeps being extended looks permanently
            // fresh). A partitioned node that prevotes does not bump anyone's
            // term, so the cost of dropping the guard is a harmless probe, not
            // an election storm.
            raft_message reply;
            reply.kind = raft_message::type::prevote_reply;
            // Answer in THIS node's term, never the probe's. The probe's term is
            // a proposal, not a fact; echoing it would let a candidate drag a
            // healthy node's term upwards one probe at a time.
            reply.term = term_;
            reply.from = config_.node_id;
            reply.to = msg.from;
            reply.granted =
                log_ok_for_vote(msg.prev_log_term_hint, msg.prev_log_index);
            outbound.push_back(std::move(reply));
            break;
        }
        case raft_message::type::prevote_reply: {
            if (role_ != raft_role::candidate) {
                break;
            }
            // A reply in a term other than the one this candidacy is for is not
            // about this probe. In particular, a reply in the candidate's own
            // term (term_ + 1 was the proposal, term_ is the current term) still
            // counts: the peer has not moved on, so the probe is still live.
            if (msg.term > term_ + 1) {
                break;
            }
            // LATE-PREVOTE: a reply from a term BELOW the current one is a
            // PreVote that was in flight during the transition. After a PreVote
            // majority the node does `votes_.clear(); ++term_;` and starts a
            // real election, but `role_` stays `candidate`. Without this lower
            // bound, a late reply carrying the OLD term passes the upper-bound
            // check above (the window moves with `term_`), lands in the freshly
            // cleared `votes_`, and either (a) reaches a majority again and
            // bumps the term a second time, or (b) -- worse -- combines with ONE
            // real vote to reach the quorum, electing a leader with a single
            // real vote. Both are the same root cause: a stale PreVote reply
            // must not be counted once the term has advanced.
            if (msg.term < term_) {
                break;
            }
            // 3RD-PATH: once the PreVote majority has been reached, this node
            // has already done `votes_.clear(); ++term_;` and started a REAL
            // election. A PreVote reply carrying the NEW term (`== term_`)
            // still passes both bounds above, so without this guard it lands in
            // the freshly cleared `votes_` and is counted as a real vote: one
            // such grant plus one real vote plus the candidate's own vote
            // reaches the quorum, electing a leader with a single real vote.
            //
            // The key is the round flag, NOT `voted_this_term_`/`voted_for_`:
            // those are the per-term vote record and are not reset when a new
            // PreVote round starts in the same term, so keying on them would
            // reject every grant in a second election round (a liveness defect).
            if (!prevote_round_active_) {
                break;
            }
            if (!msg.granted) {
                break;
            }
            const auto already = std::find(votes_.begin(), votes_.end(), msg.from);
            if (already != votes_.end()) {
                break;
            }
            votes_.push_back(msg.from);
            // A pre-vote majority means the candidate could win, so it now
            // spends the term increment on a real election. The PreVote itself
            // never touched `term_`, which is why this is safe.
            if (votes_.size() + 1 >= quorum_size()) {
                // N-B-2: the PreVote grants are NOT votes for the real
                // election. `votes_` is the same vector the `vote_reply` path
                // appends to, so leaving the PreVote grants in it lets a
                // 5-node cluster (quorum 3) elect a leader with only ONE real
                // vote: two PreVote grants + one real vote + the candidate's
                // own vote reach the quorum, bypassing the majority
                // requirement. Clear it before the term increment so the real
                // election counts only real votes.
                votes_.clear();
                // The PreVote round is over: it has converted to a real
                // election. A PreVote grant that arrives from here on must not
                // be counted as a real vote.
                prevote_round_active_ = false;
                ++term_;
                voted_for_ = config_.node_id;
                voted_this_term_ = true;
                (void)persist_locked();
                raft_message vote;
                vote.kind = raft_message::type::vote;
                vote.term = term_;
                vote.from = config_.node_id;
                vote.prev_log_index = log_.last_index();
                vote.prev_log_term_hint = last_log_term_locked();
                for (const auto& peer : config_.peers) {
                    if (peer == config_.node_id) {
                        continue;
                    }
                    raft_message copy = vote;
                    copy.to = peer;
                    outbound.push_back(std::move(copy));
                }
            }
            break;
        }
        case raft_message::type::vote: {
            raft_message reply;
            reply.kind = raft_message::type::vote_reply;
            // D-B-4: a vote request from a STALE term must be refused, and the
            // reply must carry THIS node's term. Granting it would hand a live
            // term's vote to a dead candidacy (setting `voted_this_term_`), so
            // the real candidate for the current term is then refused. Echoing
            // `msg.term` would additionally tell the dead candidate it is
            // current. This mirrors the `append` branch's `msg.term < term_`
            // guard.
            if (msg.term < term_) {
                reply.term = term_;
                reply.from = config_.node_id;
                reply.to = msg.from;
                reply.granted = false;
                outbound.push_back(std::move(reply));
                break;
            }
            reply.term = term_;
            reply.from = config_.node_id;
            reply.to = msg.from;
            const bool up_to_date = log_ok_for_vote(msg.prev_log_term_hint,
                                                    msg.prev_log_index);
            if (!voted_this_term_ && up_to_date) {
                voted_this_term_ = true;
                voted_for_ = msg.from;
                (void)persist_locked();
                reply.granted = true;
                election_deadline_ =
                    clock_.read() +
                    std::chrono::milliseconds(random_election_timeout());
            } else {
                reply.granted = false;
            }
            outbound.push_back(std::move(reply));
            break;
        }
        case raft_message::type::vote_reply: {
            if (role_ != raft_role::candidate) {
                break;
            }
            // A reply in a term other than the one this candidacy is for is
            // stale.
            if (msg.term != term_) {
                break;
            }
            if (!msg.granted) {
                break;
            }
            const auto already = std::find(votes_.begin(), votes_.end(), msg.from);
            if (already != votes_.end()) {
                break;
            }
            votes_.push_back(msg.from);
            // The candidate's own vote is cast locally, so the count includes
            // it.
            if (votes_.size() + 1 >= quorum_size()) {
                become_leader();
                raft_message beat;
                beat.kind = raft_message::type::append;
                beat.term = term_;
                beat.from = config_.node_id;
                beat.prev_log_index = log_.last_index();
                beat.prev_log_term = last_log_term_locked();
                beat.leader_commit = commit_index_;
                for (const auto& peer : config_.peers) {
                    if (peer == config_.node_id) {
                        continue;
                    }
                    raft_message copy = beat;
                    copy.to = peer;
                    outbound.push_back(std::move(copy));
                }
                last_heartbeat_ = clock_.read();
            }
            break;
        }
        case raft_message::type::append: {
            // Reject a message from a stale term before touching any state: a
            // delayed heartbeat from a deposed leader must not reset this node's
            // election timer, or a deposed leader could keep an election from
            // ever starting.
            if (msg.term < term_) {
                break;
            }
            // A leader heartbeat or replication. A higher term was handled
            // above, so this is the current leader (or this node's own term).
            leader_id_ = msg.from;
            has_leader_ = true;
            last_heartbeat_ = clock_.read();
            election_deadline_ =
                last_heartbeat_ +
                std::chrono::milliseconds(random_election_timeout());
            role_ = raft_role::follower;

            const u64 last = log_.last_index();
            const bool prev_ok =
                msg.prev_log_index == 0 ||
                (msg.prev_log_index <= last &&
                 entry_term_locked(msg.prev_log_index) == msg.prev_log_term);
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
                reply.conflict_index =
                    msg.prev_log_index == 0 ? 1 : msg.prev_log_index;
                outbound.push_back(std::move(reply));
                break;
            }

            // D-B-1: track what was ACTUALLY stored. The old code broke out of
            // the append loop on a failure and then answered `success = true`
            // with `match_index = prev_log_index + entries.size()`, so the
            // leader raised this peer's match index past entries the follower
            // does not hold and committed a write no follower has.
            bool append_ok = true;
            u64 match = msg.prev_log_index;

            // D-B-9: `entry_term_locked` now uses the member-level
            // `index_term_cache_` (built once in `start()`, maintained on
            // append/truncate), so each lookup is O(1) instead of O(n) replay.

            // Append the entries the leader sent, truncating a conflicting
            // suffix. A failed truncate is a failed append: the follower cannot
            // claim a consistent prefix it could not establish.
            for (const auto& entry : msg.entries) {
                const u64 existing_term = entry_term_locked(entry.index);
                if (existing_term != 0 && existing_term != entry.term) {
                    // A committed entry must never be truncated. Both the
                    // index and the term come from the inbound message; a
                    // forged higher-term append is trusted without sender
                    // auth, so refuse rather than let :742-758 silently
                    // replace a committed command. The existing reply path
                    // turns append_ok=false into success=false / no ack.
                    if (entry.index <= commit_index_) {
                        append_ok = false;
                        break;
                    }
                    const status trimmed = log_.truncate_suffix(entry.index);
                    if (!trimmed.ok()) {
                        append_ok = false;
                    }
                    index_term_cache_.erase(
                        index_term_cache_.lower_bound(entry.index),
                        index_term_cache_.end());
                    break;
                }
            }
            if (append_ok) {
                for (const auto& entry : msg.entries) {
                    if (entry_term_locked(entry.index) == entry.term) {
                        // Already held and matching: it counts toward the
                        // follower's match index.
                        match = entry.index;
                        continue;
                    }
                    const result<u64> appended = log_.append(entry);
                    if (!appended.ok()) {
                        append_ok = false;
                        break;
                    }
                    match = entry.index;
                    index_term_cache_[entry.index] = entry.term;
                }
            }
            // A follower discards a suffix the leader has superseded. The
            // boundary is the end of the leader's batch: everything through
            // `prev_log_index + entries.size()` is now consistent with the
            // leader (the loop above appended it), and anything beyond it is a
            // stale record from an older term or an entry a deposed leader
            // appended.
            //
            // An empty batch (a heartbeat) therefore trims back to
            // `prev_log_index`, which is exactly how L2-2 removes a minority's
            // uncommitted suffix: the new leader's heartbeat carries its own
            // last index, and the follower sheds whatever lies past it.
            if (append_ok) {
                const u64 leader_prefix = msg.prev_log_index + msg.entries.size();
                if (log_.last_index() > leader_prefix) {
                    // Suffix trimming must not cross into the committed
                    // prefix: leader_prefix+1 is message-controlled, so an
                    // empty heartbeat from a forged higher term could delete
                    // committed entries with no refill. Refuse the append
                    // (append_ok=false -> reply.success=false) instead.
                    if (leader_prefix + 1 <= commit_index_) {
                        append_ok = false;
                    } else {
                        const status trimmed =
                            log_.truncate_suffix(leader_prefix + 1);
                        if (!trimmed.ok()) {
                            append_ok = false;
                        } else {
                            index_term_cache_.erase(
                                index_term_cache_.lower_bound(leader_prefix + 1),
                                index_term_cache_.end());
                        }
                    }
                }
            }

            // The leader's commit index only ever advances this node's view; it
            // can never move it backwards. It is advanced only when the batch
            // was stored: a failed append may have truncated a suffix, and
            // committing past what we hold would be a lie.
            if (append_ok && msg.leader_commit > commit_index_) {
                const u64 last_new = log_.last_index();
                commit_index_ = std::min(msg.leader_commit, last_new);
            }
            // A failed sync means the appended bytes are not durable, so the
            // follower must not acknowledge them.
            if (append_ok) {
                const status synced = log_.sync();
                if (!synced.ok()) {
                    append_ok = false;
                }
            }

            raft_message reply;
            reply.kind = raft_message::type::append_reply;
            reply.term = term_;
            reply.from = config_.node_id;
            reply.to = msg.from;
            reply.success = append_ok;
            // `match_index` names the last index the follower actually holds
            // from this batch, never an index it failed to store.
            reply.match_index = append_ok ? match : 0;
            reply.conflict_index = append_ok ? 0 : match + 1;
            outbound.push_back(std::move(reply));
            break;
        }
        case raft_message::type::append_reply: {
            if (role_ != raft_role::leader) {
                break;
            }
            // STALE-ACK: a reply from a term other than the one this leadership
            // is for is stale. It answers an AppendEntries the leader sent
            // before it was re-elected, so its `match_index` describes a log
            // position that may no longer exist. Counting it toward the commit
            // quorum would commit an entry the current term never replicated to
            // a majority -- the corruption Raft 5.4.2 forbids. The `append`
            // branch rejects `msg.term < term_` and `vote_reply` requires
            // equality; this branch had no check at all.
            if (msg.term != term_) {
                break;
            }
            // Any reply -- success or a conflict report -- proves the peer is
            // reachable, which is what CheckQuorum measures. Recording it only
            // on success would depose a leader whose followers are merely
            // behind.
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
                    break;
                }
                const std::size_t i =
                    static_cast<std::size_t>(it - config_.peers.begin());
                peers_[i].match_index =
                    std::max(peers_[i].match_index, msg.match_index);
                peers_[i].next_index = peers_[i].match_index + 1;
                advance_commit_locked();
                break;
            }
            // Retry from the follower's conflict point, or one before the
            // current cursor when it reported nothing.
            auto it = std::find_if(config_.peers.begin(), config_.peers.end(),
                                   [&msg](const std::string& id) {
                                       return id == msg.from;
                                   });
            if (it == config_.peers.end()) {
                break;
            }
            const std::size_t i =
                static_cast<std::size_t>(it - config_.peers.begin());
            if (msg.conflict_index > 0 &&
                msg.conflict_index < peers_[i].next_index) {
                peers_[i].next_index = msg.conflict_index;
            } else if (peers_[i].next_index > 1) {
                --peers_[i].next_index;
            }
            // Send the corrected batch now rather than waiting for the next
            // tick.
            raft_message retry;
            retry.kind = raft_message::type::append;
            retry.term = term_;
            retry.from = config_.node_id;
            retry.to = msg.from;
            retry.prev_log_index = peers_[i].next_index - 1;
            retry.prev_log_term = entry_term_locked(retry.prev_log_index);
            retry.leader_commit = commit_index_;
            outbound.push_back(std::move(retry));
            break;
        }
        }
    }
    deliver(outbound);
}

void raft_node::advance_commit_locked() {
    // Raft 5.4.2: only an entry of the current term may be committed by
    // counting replicas. Advancing over an older entry by replica count would
    // let a stale entry commit after a leader change.
    const u64 last = log_.last_index();
    for (u64 index = commit_index_ + 1; index <= last; ++index) {
        if (entry_term_locked(index) != term_) {
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
    std::vector<raft_message> outbound;
    u64 index = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
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
        index = appended.value();
        index_term_cache_[index] = term_;

        // D-B-2: the entry is not durable until `sync()` succeeds. The old code
        // dropped the result and then advanced the commit index, so a
        // single-node cluster (its own quorum) acknowledged a write a power loss
        // would lose. On failure, roll the record back and report the error.
        const status synced = log_.sync();
        if (!synced.ok()) {
            (void)log_.truncate_suffix(index);
            index_term_cache_.erase(
                index_term_cache_.lower_bound(index),
                index_term_cache_.end());
            return synced.error();
        }

        // Replicate immediately so a client does not wait for the next
        // heartbeat.
        raft_message msg;
        msg.kind = raft_message::type::append;
        msg.term = term_;
        msg.from = config_.node_id;
        msg.prev_log_index = record.index - 1;
        msg.prev_log_term = entry_term_locked(msg.prev_log_index);
        msg.leader_commit = commit_index_;
        msg.entries.push_back(std::move(record));
        for (const auto& peer : config_.peers) {
            if (peer == config_.node_id) {
                continue;
            }
            raft_message copy = msg;
            copy.to = peer;
            outbound.push_back(std::move(copy));
        }
    }
    // Deliver with the lock released: the hook may re-enter `handle()` on this
    // node, which advances the commit index itself.
    deliver(outbound);

    // A single-node cluster has no peer whose reply would advance the commit
    // index, so `propose` must do it itself. `wait_for_commit` documents that
    // "propose already advanced the commit index" for exactly this case; without
    // this call that promise is false and every single-node proposal times out.
    // With peers the loop below is a no-op until a majority acknowledges. A
    // re-entrant `handle()` during `deliver` may already have advanced it, in
    // which case this is a no-op.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        advance_commit_locked();
    }
    return index;
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
    // Replay once before the pump loop: entries don't change while waiting for
    // commit (no appends happen here), so per-pump replay would be O(n²) —
    // exactly the D-B-9 defect `entry_term_locked` used to cause.
    const auto replayed = log_.replay(0);
    std::vector<log_record> records;
    if (replayed.ok()) {
        records = std::move(replayed.value());
    }
    for (int pump = 0;; ++pump) {
        std::vector<raft_message> outbound;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (commit_index_ >= index) {
                return commit_index_;
            }
            // A leadership loss ends the wait immediately. Continuing would let
            // a deposed leader report a commit that a new leader may never make,
            // which is exactly the acknowledgement S4 forbids.
            if (role_ != raft_role::leader) {
                return rpc_error(rpc_code::unavailable,
                                 "raft: leadership lost while waiting for commit");
            }
            if (clock_.read() >= deadline || pump >= kMaxPumps) {
                return rpc_error(rpc_code::unavailable,
                                 "raft: commit timed out");
            }
            // Build whatever each peer is missing. The transport delivers
            // replies synchronously, so `commit_index_` can advance inside the
            // delivery below.
            for (std::size_t i = 0; i < peers_.size(); ++i) {
                raft_message msg;
                msg.kind = raft_message::type::append;
                msg.term = term_;
                msg.from = config_.node_id;
                msg.to = config_.peers[i];
                msg.prev_log_index = peers_[i].next_index - 1;
                msg.prev_log_term = entry_term_locked(msg.prev_log_index);
                msg.leader_commit = commit_index_;
                for (const auto& record : records) {
                    if (record.index >= peers_[i].next_index) {
                        msg.entries.push_back(record);
                    }
                }
                outbound.push_back(std::move(msg));
            }
        }
        deliver(outbound);
    }
}

void raft_node::pump_replication() {
    // Retained for the header contract; `wait_for_commit` now builds and
    // delivers its own batch so it can release the lock before sending.
    std::vector<raft_message> outbound;
    const auto replayed = log_.replay(0);
    if (!replayed.ok()) {
        return;
    }
    const std::vector<log_record>& records = replayed.value();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < peers_.size(); ++i) {
            raft_message msg;
            msg.kind = raft_message::type::append;
            msg.term = term_;
            msg.from = config_.node_id;
            msg.to = config_.peers[i];
            msg.prev_log_index = peers_[i].next_index - 1;
            msg.prev_log_term = entry_term_locked(msg.prev_log_index);
            msg.leader_commit = commit_index_;
            for (const auto& record : records) {
                if (record.index >= peers_[i].next_index) {
                    msg.entries.push_back(record);
                }
            }
            outbound.push_back(std::move(msg));
        }
    }
    deliver(outbound);
}

} // namespace cluster
} // namespace crpc

