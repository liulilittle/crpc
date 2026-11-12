// CRPC -- G19 step3: a fence authority backed by a Raft node.
//
// The seam (`fence_authority.hpp`) is a `noexcept` function pointer returning
// `result<u64>`. It has no metadata channel and no registry parameter, so this
// adapter carries both itself:
//
//   * the token comes from the registry's own allocator (`next_fence`), not
//     from `raft_node::propose`. A log index is dense over the whole log and
//     restarts at 1 on a fresh log; using it as a fence would let an old
//     holder's token regress. The registry's allocator is per-key monotonic,
//     which is the property a fence needs.
//
//   * the metadata S4 requires (`raft.commit_latency_ms`, `registry.leader`)
//     is written to `last` by `allocate` and read by the caller afterwards.
//     The seam's return type is unchanged, so step2's contract still holds.
//
// What Raft adds is the durability of the allocation: the token is only
// returned after the entry that records it has committed on a majority. A
// leader that cannot reach a quorum fails the write loudly instead of handing
// out a token it cannot back.
#ifndef CRPC_CLUSTER_RAFT_FENCE_AUTHORITY_HPP
#define CRPC_CLUSTER_RAFT_FENCE_AUTHORITY_HPP

#include <chrono>
#include <string>
#include <utility>

#include <crpc/cluster/fence_authority.hpp>
#include <crpc/cluster/raft_node.hpp>
#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace cluster {

class registry;

// A `fence_authority` whose allocations are committed through Raft.
//
// Lifetime: the caller keeps this object alive for as long as the service uses
// the authority it handed out, because the function pointer's context is
// `this`.
struct CRPC_API raft_fence_authority {
    // The node that proposes and waits. Required.
    raft_node* node = nullptr;
    // The catalog that issues the token. Required.
    registry* catalog = nullptr;
    // The clock `wait_for_commit` reads. Defaults to the steady clock: a
    // default-constructed `monotonic_clock` has a null `now` and reads the
    // epoch, which would make every wait time out immediately.
    monotonic_clock clock = system_monotonic_clock();
    // How long a write may wait for a majority before it fails.
    std::chrono::milliseconds commit_timeout{2000};

    // The observables S4 requires on a write response. `allocate` writes them;
    // the caller reads them after a successful call, before any other write can
    // start on the same thread. Not thread-safe by design: the service calls
    // `allocate` and reads this on one thread.
    struct last_grant {
        u64 commit_latency_ms = 0;
        std::string leader_id;
        bool valid = false;
    };
    mutable last_grant last{};

    // The seam value. `context` is `this`.
    [[nodiscard]] fence_authority as_authority() noexcept;

    // The fn body. Exposed so a test can call it without the function pointer.
    //
    // `noexcept` because the seam is: every allocation below is inside a
    // try/catch that turns a `std::bad_alloc` into `resource_exhausted`. An
    // exception escaping here would call `std::terminate`, not return an error.
    [[nodiscard]] result<u64> allocate(const fence_request& request) noexcept;
};

// Decodes a fence_alloc raft_command payload back into (token, lease_id). Used
// by the apply loop to make a committed fence allocation observable.
[[nodiscard]] result<std::pair<u64, u64>> decode_allocation(span<const std::byte> in);

} // namespace cluster
} // namespace crpc

#endif // CRPC_CLUSTER_RAFT_FENCE_AUTHORITY_HPP
