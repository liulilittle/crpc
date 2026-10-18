// CRPC -- leader election primitives implementation.
#include <crpc/cluster/election.hpp>

#include <limits>
#include <utility>

namespace crpc {
namespace cluster {

election::election(lease_manager& leases, quorum& voters, monotonic_clock clock,
                   election_config config)
    : leases_(leases), voters_(voters), clock_(clock), config_(std::move(config)) {
    if (clock_.now == nullptr) {
        clock_ = system_monotonic_clock();
    }
    if (config_.election_timeout.count() <= 0) {
        config_.election_timeout = std::chrono::milliseconds{1};
    }
    if (config_.leader_ttl.count() <= 0) {
        config_.leader_ttl = std::chrono::milliseconds{1};
    }
    if (config_.quorum_size == 0) {
        config_.quorum_size = 1;
    }
    if (config_.lease_service.empty()) {
        config_.lease_service = "crpc.election";
    }
}

u32 election::required_votes() const noexcept {
    return config_.quorum_size == 0 ? 1 : config_.quorum_size;
}

bool election::is_leader() const {
    if (self_id_.empty() || leader_id_ != self_id_ || lease_id_ == 0) {
        return false;
    }
    return leases_.is_active(lease_id_);
}

bool election::leader_alive_at(monotonic_clock::time_point now) const {
    if (!self_id_.empty() && leader_id_ == self_id_ && lease_id_ != 0 &&
        leases_.is_active(lease_id_, now)) {
        return true;
    }
    if (!has_last_leader_) {
        return false;
    }
    if (now < last_leader_seen_) {
        return true;
    }
    return now - last_leader_seen_ <= config_.election_timeout;
}

bool election::leader_alive() const {
    return leader_alive_at(clock_.read());
}

std::string election::leader_id() const {
    if (is_leader()) {
        return self_id_;
    }
    if (has_last_leader_ && leader_alive_at(clock_.read())) {
        return last_leader_id_;
    }
    return std::string{};
}

leadership_token election::current_token() const {
    leadership_token token;
    token.leader_id = leader_id_;
    token.term = term_;
    token.epoch = epoch_;
    token.lease_id = lease_id_;
    token.expiry = expiry_;
    return token;
}

result<leadership_token> election::campaign(const std::string& candidate_id) {
    if (candidate_id.empty()) {
        return rpc_error(rpc_code::invalid_argument, "candidate_id is required");
    }

    const monotonic_clock::time_point now = clock_.read();

    if (leader_id_ == candidate_id && is_leader()) {
        const status extended = renew();
        if (!extended.ok()) {
            return extended.error();
        }
        return current_token();
    }

    if (leader_alive_at(now) && last_leader_id_ != candidate_id) {
        return rpc_error(rpc_code::failed_precondition,
                         "a live leader was observed within the election timeout");
    }

    if (is_leader() && leader_id_ != candidate_id) {
        const status stepped_down = resign();
        if (!stepped_down.ok()) {
            return stepped_down.error();
        }
    }

    if (term_ == std::numeric_limits<u64>::max()) {
        return rpc_error(rpc_code::resource_exhausted, "election term exhausted");
    }
    const u64 proposed_term = term_ + 1;

    // PreVote: ask without mutating term_.
    const prevote_result pre = voters_.prevote(candidate_id, proposed_term);
    if (pre.leader_alive) {
        if (!pre.leader_id.empty()) {
            leader_id_ = pre.leader_id;
            last_leader_id_ = pre.leader_id;
        }
        last_leader_seen_ = now;
        has_last_leader_ = true;
        return rpc_error(rpc_code::failed_precondition,
                         "pre-vote rejected: a live leader exists");
    }
    if (pre.granted + 1 < required_votes()) {
        return rpc_error(rpc_code::unavailable, "pre-vote quorum not reached");
    }

    term_ = proposed_term;
    const vote_result voted = voters_.vote(candidate_id, term_);
    if (voted.granted + 1 < required_votes()) {
        return rpc_error(rpc_code::unavailable, "vote quorum not reached");
    }

    const result<lease> granted =
        leases_.grant(config_.lease_service, candidate_id, config_.leader_ttl);
    if (!granted.ok()) {
        return granted.error();
    }
    const lease& held = granted.value();

    u64 next_epoch = epoch_;
    if (next_epoch != std::numeric_limits<u64>::max()) {
        ++next_epoch;
    }
    if (next_epoch < held.fence) {
        next_epoch = held.fence;
    }
    if (next_epoch == 0) {
        next_epoch = 1;
    }
    epoch_ = next_epoch;

    self_id_ = candidate_id;
    leader_id_ = candidate_id;
    lease_id_ = held.id;
    expiry_ = held.expiry;
    last_leader_id_ = candidate_id;
    last_leader_seen_ = now;
    has_last_leader_ = true;

    return current_token();
}

status election::renew() {
    if (!is_leader()) {
        return status{rpc_code::failed_precondition};
    }
    const result<lease> extended = leases_.renew(lease_id_, config_.leader_ttl);
    if (!extended.ok()) {
        leader_id_.clear();
        lease_id_ = 0;
        expiry_ = monotonic_clock::time_point{};
        return extended.error();
    }
    lease_id_ = extended.value().id;
    expiry_ = extended.value().expiry;
    last_leader_id_ = self_id_;
    last_leader_seen_ = clock_.read();
    has_last_leader_ = true;
    return status{};
}

status election::resign() {
    if (lease_id_ != 0) {
        const status released = leases_.revoke(lease_id_);
        lease_id_ = 0;
        expiry_ = monotonic_clock::time_point{};
        leader_id_.clear();
        self_id_.clear();
        last_leader_id_.clear();
        has_last_leader_ = false;
        if (!released.ok()) {
            return released;
        }
        return status{};
    }
    leader_id_.clear();
    self_id_.clear();
    last_leader_id_.clear();
    has_last_leader_ = false;
    expiry_ = monotonic_clock::time_point{};
    return status{};
}

void election::observe_leader(const std::string& leader_id, u64 term) {
    if (leader_id.empty()) {
        return;
    }
    if (term < term_) {
        // A stale heartbeat from an older term must not suppress PreVote.
        return;
    }
    const monotonic_clock::time_point now = clock_.read();
    if (term > term_) {
        term_ = term;
    }
    if (!self_id_.empty() && leader_id != self_id_ && lease_id_ != 0) {
        (void)leases_.revoke(lease_id_);
        lease_id_ = 0;
        expiry_ = monotonic_clock::time_point{};
    }
    leader_id_ = leader_id;
    last_leader_id_ = leader_id;
    last_leader_seen_ = now;
    has_last_leader_ = true;
}

} // namespace cluster
} // namespace crpc
