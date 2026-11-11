// CRPC -- G19 step4 PR-7: the in-memory KV store.
//
// This is the data plane for the four KV methods (kv_get / kv_put / kv_cas /
// kv_delete). It owns nothing but a `std::map` of entries and a monotonic
// high-water index. Writes are submitted by the registry service, which is
// responsible for Raft commitment before calling these; the store itself is a
// plain key/value table with Consul-style CAS semantics.
#ifndef CRPC_CLUSTER_KV_STORE_HPP
#define CRPC_CLUSTER_KV_STORE_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc {
namespace cluster {

// One KV entry. `modify_index` is the global index of the write that produced
// this value; it is what `cas=N` compares against.
struct kv_entry {
    std::string key;
    std::vector<std::byte> value;   // raw bytes; base64 at the wire boundary
    u64 create_index = 0;
    u64 modify_index = 0;
    u64 lock_index = 0;             // 0 = unlocked (Consul semantics)
    std::vector<std::string> flags; // optional, Consul-compatible
};

enum class kv_result_kind { ok, not_found, cas_conflict, invalid_key };

struct kv_write_result {
    kv_result_kind kind = kv_result_kind::ok;
    u64 index = 0;                  // the index of this write
    u64 modify_index = 0;           // the entry's modify index after the write
};

class CRPC_API kv_store {
public:
    // A key is a non-empty UTF-8 string without control characters, at most
    // 512 bytes. An invalid key is `invalid_key`, never a silent rejection.
    [[nodiscard]] static bool valid_key(const std::string& key) noexcept;

    [[nodiscard]] result<kv_entry> get(const std::string& key) const;

    // Always succeeds (unless the key is invalid). `modify_index = index`.
    [[nodiscard]] kv_write_result put(const std::string& key,
                                      std::vector<std::byte> value,
                                      u64 index);

    // cas == 0: create only (fails with cas_conflict when the key exists)
    // cas == N: succeeds only when the entry's modify_index == N
    [[nodiscard]] kv_write_result cas(const std::string& key,
                                      std::vector<std::byte> value,
                                      u64 cas_index, u64 index);

    [[nodiscard]] kv_write_result erase(const std::string& key, u64 index);

    // Every key with the given prefix, sorted by key.
    [[nodiscard]] std::vector<std::string> keys_with_prefix(
        const std::string& prefix) const;

    // Drops every entry but keeps `highest_index()`. Used only by the apply
    // loop when an applied suffix was truncated: the state machine must be
    // rebuilt from the log, and replaying from index 1 over the old table would
    // leave keys the new log no longer contains. The high-water mark is NOT
    // reset, because it is the store's index contract: a replayed write may
    // legitimately carry a lower index than one already observed, and a
    // regression here would hand `cas=N` a stale expectation.
    void clear() noexcept { entries_.clear(); }

    [[nodiscard]] u64 highest_index() const noexcept;

private:
    static constexpr u64 kMaxKeyBytes = 512;

    std::map<std::string, kv_entry> entries_;
    u64 highest_index_ = 0;
};

}} // namespace crpc::cluster

#endif // CRPC_CLUSTER_KV_STORE_HPP
