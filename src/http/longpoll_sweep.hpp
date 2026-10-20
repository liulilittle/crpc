// CRPC -- HTTP long-poll TTL victim selection.
//
// The long-poll bookkeeping keeps two maps: `owners` (subscriber -> session
// token) and `last_seen` (subscriber -> monotonic poll/release time). A
// subscription is normally in both. Two residues are possible after a partial
// request:
//   * ownerless: the subscription exists but the owner was released (normal
//     keep-queue session end), so only `last_seen` remains;
//   * owner-only: the subscription was created but the request failed before
//     recording activity (defensive; the server writes `last_seen` at subscribe
//     success and at owner insertion, so this should be unreachable).
// The sweeper therefore considers the UNION of both maps. An owner without a
// `last_seen` entry is treated as expired immediately; everything else expires
// after the TTL. `busy` subscribers (an in-flight poll) are never swept.
#ifndef CRPC_HTTP_LONGPOLL_SWEEP_HPP
#define CRPC_HTTP_LONGPOLL_SWEEP_HPP

#include <cstdint>
#include <vector>

namespace crpc {
namespace http {
namespace detail {

template <typename Owners, typename LastSeen, typename Busy>
void collect_expired_longpoll_ids(const Owners& owners, const LastSeen& last_seen,
                                  const Busy& busy, std::uint64_t now_ns,
                                  std::uint64_t ttl_ns,
                                  std::vector<std::uint64_t>& out) {
    if (ttl_ns == 0) {
        return;
    }
    for (const auto& entry : owners) {
        const std::uint64_t id = entry.first;
        if (busy.find(id) != busy.end()) {
            continue;
        }
        const auto seen = last_seen.find(id);
        if (seen == last_seen.end()) {
            out.push_back(id);
            continue;
        }
        if (now_ns >= seen->second && now_ns - seen->second >= ttl_ns) {
            out.push_back(id);
        }
    }
    for (const auto& entry : last_seen) {
        const std::uint64_t id = entry.first;
        if (owners.find(id) != owners.end() || busy.find(id) != busy.end()) {
            continue;
        }
        if (now_ns >= entry.second && now_ns - entry.second >= ttl_ns) {
            out.push_back(id);
        }
    }
}

} // namespace detail
} // namespace http
} // namespace crpc

#endif // CRPC_HTTP_LONGPOLL_SWEEP_HPP
