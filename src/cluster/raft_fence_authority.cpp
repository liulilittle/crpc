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

namespace crpc {
namespace cluster {

namespace {

// The payload a fence allocation commits. It is deliberately small and
// self-describing: the token, the key it belongs to, and the lease binding.
// A reader that replays the log can tell which allocation this was without
// consulting the catalog.
//
//   u64 token
//   u32 service_len | service bytes
//   u32 node_len    | node bytes
//   u64 lease_id
std::vector<std::byte> encode_allocation(u64 token, const fence_request& request) {
    std::vector<std::byte> out;
    const std::string service = request.service != nullptr ? request.service : "";
    const std::string node = request.node_id != nullptr ? request.node_id : "";
    out.reserve(8 + 4 + service.size() + 4 + node.size() + 8);

    const auto put_u64 = [&out](u64 v) {
        for (int i = 0; i < 8; ++i) {
            out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
        }
    };
    const auto put_u32 = [&out](u32 v) {
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFF));
        }
    };
    const auto put_str = [&out](const std::string& s) {
        for (const char c : s) {
            out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
        }
    };

    put_u64(token);
    put_u32(static_cast<u32>(service.size()));
    put_str(service);
    put_u32(static_cast<u32>(node.size()));
    put_str(node);
    put_u64(request.lease_id);
    return out;
}

} // namespace

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
