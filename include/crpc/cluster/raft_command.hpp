// CRPC -- G19 step4 PR-9: the state-machine command carried by a raft entry.
//
// PR-7 proposes an entry and then writes the value to the local `kv_store`
// directly. That is enough for a single node and wrong for any other: a
// follower receives the entry, stores it, and never learns what it means. The
// same hole blocks "dynamic ACL through raft" -- a rule change would be
// committed and never applied.
//
// This type is the missing half. Every replicated mutation is encoded as one
// `raft_command`, committed, and then applied to the local state machine in
// commit order. The apply step is what makes two nodes agree: they apply the
// same commands in the same order to the same starting state.
//
// Encoding is explicit little-endian, the same discipline the registry DTO
// uses (registry_wire.cpp). A command that cannot be decoded is a fatal state
// machine error, never a skipped entry: skipping one command desynchronises the
// node from the rest of the cluster, and there is no way to notice from inside.
//
// The command does NOT carry the index it was committed at. The apply loop
// supplies that: the index is the entry's position in the log, and a command
// that carried its own index could disagree with the log it came from.
#ifndef CRPC_CLUSTER_RAFT_COMMAND_HPP
#define CRPC_CLUSTER_RAFT_COMMAND_HPP

#include <cstddef>
#include <map>
#include <string>
#include <vector>

#include <crpc/cluster/registry_acl.hpp>
#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/span.hpp>

namespace crpc {
namespace cluster {

enum class raft_command_kind : u8 {
    kv_put = 1,
    kv_cas = 2,
    kv_delete = 3,
    acl_upsert = 4,
    acl_remove = 5,
    catalog_upsert = 6,
    catalog_remove = 7,
    fence_alloc = 8
};

// One replicated mutation.
struct CRPC_API raft_command {
    raft_command_kind kind = raft_command_kind::kv_put;

    // KV fields (kv_put, kv_cas, kv_delete).
    std::string key;
    std::vector<std::byte> value;
    // Only for kv_cas: the modify index the write expects to match. Zero means
    // create-only.
    u64 cas = 0;

    // Catalog fields (catalog_upsert, catalog_remove).
    u64 fence = 0;
    u64 incarnation = 0;
    u8 state = 0;
    std::string service;
    std::string node_id;
    std::string host;
    u16 port = 0;
    u32 weight = 0;
    std::map<std::string, std::string> meta;

    u64 writer_epoch = 0;
    std::string admin_token;

    // Fence fields (fence_alloc).
    u64 fence_token = 0;
    u64 lease_id = 0;

    // ACL fields (acl_upsert). `acl_remove` uses only `token_hash`.
    acl_rule rule{};
};

// Encodes `value` into `out`. Fails only on an unrepresentable shape: a key
// longer than a u32 length prefix, or a vector of prefixes longer than that.
[[nodiscard]] CRPC_API status encode_raft_command(const raft_command& value,
                                                 std::vector<std::byte>& out);

// Decodes a command. Every shape error is `decode_error` with
// `registry.reason=dto_*`, matching the wire decoder's vocabulary, and `out` is
// left untouched: a half-decoded command must never reach the state machine.
[[nodiscard]] CRPC_API status decode_raft_command(span<const std::byte> in,
                                                 raft_command& out);

// The stable name of a kind, for diagnostics and for a test that wants to prove
// two nodes applied the same thing.
[[nodiscard]] CRPC_API const char* to_string(raft_command_kind kind) noexcept;

} // namespace cluster
} // namespace crpc

#endif // CRPC_CLUSTER_RAFT_COMMAND_HPP
