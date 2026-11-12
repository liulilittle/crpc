// CRPC -- G19 step3: a fence authority backed by a Raft node.
//
// See the header for the contract. This file is the two-step body: allocate a
// token from the catalog, then make the allocation durable through Raft before
// returning it.
#include <crpc/cluster/raft_fence_authority.hpp>

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include <crpc/cluster/registry.hpp>
#include <crpc/cluster/raft_command.hpp>

namespace crpc {
namespace cluster {

namespace {

// The payload a fence allocation commits. It is a raft_command of kind
// fence_alloc, encoded with the shared raft_command codec:
//   u16 version(1) | u8 kind(8) | u64 fence_token | u64 lease_id
// A reader that replays the log can tell which allocation this was without
// consulting the catalog.
//
//   u64 token           (the allocation result, carried in fence_token)
//   u64 lease_id        (from the fence_request)
std::vector<std::byte> encode_allocation(u64 token, const fence_request& request) {
    raft_command cmd;
    cmd.kind = raft_command_kind::fence_alloc;
    cmd.fence_token = token;
    cmd.lease_id = request.lease_id;
    std::vector<std::byte> out;
    const status encoded = encode_raft_command(cmd, out);
    if (!encoded.ok()) {
        return {};
    }
    return out;
}

} // namespace

// Decodes a fence_alloc payload back into (token, lease_id). Used by the apply
// loop to make a committed fence allocation observable.
result<std::pair<u64, u64>> decode_allocation(span<const std::byte> in) {
    raft_command cmd;
    const status decoded = decode_raft_command(in, cmd);
    if (!decoded.ok()) {
        return decoded.error();
    }
    if (cmd.kind != raft_command_kind::fence_alloc) {
        return rpc_error(rpc_code::decode_error,
                         "raft fence_alloc payload has wrong kind");
    }
    return std::pair<u64, u64>{cmd.fence_token, cmd.lease_id};
}

fence_authority raft_fence_authority::as_authority() noexcept {
    fence_authority out;
    out.fn = [](void* context, const fence_request& request) noexcept {
        return static_cast<raft_fence_authority*>(context)->allocate(request);
    };
    out.context = this;
    return out;
}

result<u64> raft_fence_authority::allocate(const fence_request& request) noexcept {
    // The seam is `noexcept`, so every allocation below must be caught here.
    // An exception escaping would call `std::terminate` and the caller would
    // never see an error -- the opposite of the loud failure S4 requires.
    try {
        last = last_grant{};

        if (node == nullptr) {
            return rpc_error(rpc_code::failed_precondition,
                             "raft_fence_authority: no raft node configured");
        }
        if (catalog == nullptr) {
            return rpc_error(rpc_code::failed_precondition,
                             "raft_fence_authority: no catalog configured");
        }
        if (request.service == nullptr || request.node_id == nullptr) {
            return rpc_error(rpc_code::invalid_argument,
                             "raft_fence_authority: service and node_id are required");
        }

        // 1. The token comes from the catalog's allocator, which is per-key
        //    monotonic. `propose`'s return value is a log index and would
        //    regress on a fresh log.
        const result<u64> token =
            catalog->next_fence(request.service, request.node_id, request.lease_id);
        if (!token.ok()) {
            return token.error();
        }

        // 2. Make the allocation durable: propose it and wait for a majority.
        //    A leader that cannot reach a quorum fails here, so the token is
        //    never handed out on the strength of a local write alone.
        //
        //    The latency is measured from before the propose, not from before
        //    the wait: the write's cost to the caller includes getting the
        //    entry into the log, and a wait that returns immediately would
        //    otherwise report zero for every write.
        const auto started = clock.read();
        const std::vector<std::byte> payload =
            encode_allocation(token.value(), request);
        const result<u64> proposed =
            node->propose(span<const std::byte>(payload.data(), payload.size()));
        if (!proposed.ok()) {
            return proposed.error();
        }

        const auto deadline = started + commit_timeout;
        const result<u64> committed = node->wait_for_commit(proposed.value(), deadline);
        if (!committed.ok()) {
            return committed.error();
        }

        // 3. Publish the observables S4 requires. The caller reads them after
        //    this returns, on the same thread.
        const auto elapsed = clock.read() - started;
        last.commit_latency_ms = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
        last.leader_id = node->leader_id();
        last.valid = true;
        return token;
    } catch (...) {
        return rpc_error(rpc_code::resource_exhausted,
                         "raft_fence_authority: allocation failed");
    }
}

} // namespace cluster
} // namespace crpc
