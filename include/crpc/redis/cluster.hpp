// CRPC Redis -- Redis Cluster client: slot routing, topology and redirects.
//
// A Redis Cluster shards the key space over 16384 hash slots. This client keeps
// a slot table, sends every command to the node that owns its slot, and follows
// MOVED/ASK redirects. The table is learned from `CLUSTER SHARDS` (Redis 7) with
// a fallback to `CLUSTER SLOTS`, so a cluster does not have to be pre-described
// by the caller: seed nodes are enough.
//
// Execution model
// ---------------
// The cluster is a `command_executor`: `execute()` and `execute_batch()` park
// the calling coroutine exactly like `command_channel` does, because the node
// executors are ordinary CRPC Redis command streams. There are no hidden
// threads, timers or background refreshes; topology is refreshed lazily when a
// slot is unknown and explicitly through `refresh()`.
//
// Cross-slot rules
// ----------------
// A single command whose keys hash to different slots (for example
// `MGET a b`), and a pipeline whose commands span slots, are rejected locally
// with `rpc_code::failed_precondition` and a `CROSSSLOT` message before any I/O
// happens. Redis transactions have the same restriction; `pin()` validates a
// key set once and returns a `command_executor` pinned to that slot, which can
// be handed to `transaction` so MULTI/EXEC and every queued command stay on one
// node.
//
// Ownership
// ---------
// A cluster is single-owner, like the command stream it uses. It is not safe
// for concurrent calls from several coroutines; use one cluster per logical
// command stream (the underlying connection pools are still shared by the
// provider).
#ifndef CRPC_REDIS_CLUSTER_HPP
#define CRPC_REDIS_CLUSTER_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/redis/connection.hpp>
#include <crpc/redis/pipeline.hpp>

namespace crpc {
class runtime;
class backend_factory;

namespace redis {

class sentinel;

// Number of hash slots in a Redis Cluster.
inline constexpr std::uint16_t cluster_slot_count = 16384;

// Maps a key to its cluster slot using the Redis Cluster CRC16/XMODEM hash.
// Hash tags are honoured: when the key contains `{...}` with a non-empty body,
// only the first such body is hashed. This is the same rule as
// `CLUSTER KEYSLOT`, so keys that share a tag can be used in one transaction or
// pipeline.
[[nodiscard]] CRPC_API std::uint16_t cluster_slot_of(
    std::string_view key) noexcept;

[[nodiscard]] inline std::uint16_t cluster_slot_of(
    const std::string& key) noexcept {
    return cluster_slot_of(std::string_view(key));
}

[[nodiscard]] inline std::uint16_t cluster_slot_of(const char* key) noexcept {
    return cluster_slot_of(std::string_view(key == nullptr ? "" : key));
}

// A cluster node address. `host` may be a hostname, an IPv4 literal or an IPv6
// literal (without brackets; `to_string()` adds them when needed).
#if defined(CRPC_COMPILER_MSVC)
// Private STL members are an implementation detail; the exported ABI is the
// method set. C4251 would otherwise reject the class in a shared build.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
struct CRPC_API cluster_address {
    std::string host;
    std::uint16_t port = 0;

    cluster_address() = default;
    cluster_address(std::string h, std::uint16_t p)
        : host(std::move(h)), port(p) {}

    [[nodiscard]] bool valid() const noexcept {
        return !host.empty() && port != 0;
    }
    [[nodiscard]] std::string to_string() const;

    friend bool operator==(const cluster_address& a,
                           const cluster_address& b) noexcept {
        return a.port == b.port && a.host == b.host;
    }
    friend bool operator!=(const cluster_address& a,
                           const cluster_address& b) noexcept {
        return !(a == b);
    }
    friend bool operator<(const cluster_address& a,
                           const cluster_address& b) noexcept {
        return a.host < b.host || (a.host == b.host && a.port < b.port);
    }
};

#if defined(CRPC_COMPILER_MSVC)
#pragma warning(pop)
#endif

// One contiguous slot range and the nodes serving it. `master` owns writes;
// `replicas` is retained for introspection (the client always routes to the
// master, because it has no read-from-replica policy).
struct cluster_slot_range {
    std::uint16_t begin = 0;
    std::uint16_t end = 0;
    cluster_address master;
    std::vector<cluster_address> replicas;

    [[nodiscard]] bool contains(std::uint16_t slot) const noexcept {
        return slot >= begin && slot <= end;
    }
    [[nodiscard]] std::size_t slot_count() const noexcept {
        return static_cast<std::size_t>(end) - begin + 1;
    }
};

// A slot table. Assigning a range overwrites every slot it covers (an existing
// range is split or dropped as needed), which is what a MOVED redirect needs.
// The table is a plain value and can be copied or moved.
class CRPC_API cluster_topology {
public:
    cluster_topology();
    ~cluster_topology();

    cluster_topology(const cluster_topology&);
    cluster_topology& operator=(const cluster_topology&);
    cluster_topology(cluster_topology&&) noexcept;
    cluster_topology& operator=(cluster_topology&&) noexcept;

    // Drops every assignment.
    void reset() noexcept;

    // Assigns `range`. Returns false (and changes nothing) when the range is
    // empty, out of bounds or has an invalid master. A successful assignment
    // bumps `generation()`.
    bool assign(const cluster_slot_range& range);

    // Null when the slot is unassigned. The returned pointer is owned by the
    // table and is invalidated by the next assign/reset.
    [[nodiscard]] const cluster_address* master_for(
        std::uint16_t slot) const noexcept;
    [[nodiscard]] const cluster_slot_range* range_for(
        std::uint16_t slot) const noexcept;

    // Snapshot of the current non-overlapping ranges, in insertion order.
    [[nodiscard]] std::vector<cluster_slot_range> ranges() const;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool complete() const noexcept;
    [[nodiscard]] std::size_t assigned_slots() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;

private:
    void rebuild_index() noexcept;

    std::vector<cluster_slot_range> ranges_;
    std::vector<std::uint32_t> range_index_;
    std::size_t assigned_ = 0;
    std::uint64_t generation_ = 0;
};

// Keys a command addresses, in wire order. The table covers the command shapes
// that matter for routing: single-key commands, variadic key commands
// (MGET/DEL/...), key/value pairs (MSET), EVAL/FCALL key counts, stream
// STREAMS sections, BITOP, *STORE variants and BLPOP-style timeout tails.
// Commands without keys return an empty vector. Unknown commands fall back to
// "the first argument is a key", which is correct for the large single-key
// family; a command that has a non-key first argument must be listed above.
[[nodiscard]] CRPC_API std::vector<std::string> cluster_command_keys(
    const command& cmd);

// How the cluster reaches one node. A provider owns its executors; the pointer
// returned by `executor_for()` stays valid until the provider is destroyed.
// The concrete `cluster_pool_provider` keeps one connection pool per node.
class CRPC_API cluster_executor_provider {
public:
    virtual ~cluster_executor_provider();

    cluster_executor_provider(const cluster_executor_provider&) = delete;
    cluster_executor_provider& operator=(const cluster_executor_provider&) =
        delete;

    // Returns the executor for `node`. A transport-level failure to create it
    // is reported as an error; an executor that later fails a command returns
    // that error from `execute()`.
    [[nodiscard]] virtual result<command_executor*> executor_for(
        const cluster_address& node) = 0;

    // Optional hook for providers that recycle executors. The default is a
    // no-op; the cluster calls it once it is done with the pointer.
    virtual void release(command_executor* executor) noexcept;

protected:
    cluster_executor_provider() = default;
};

// Provider over the CRPC connection layer: one `connection_pool` per node,
// created lazily. Every command acquires a pooled connection, runs one
// `command_channel` request (or batch) and releases the connection. The pool
// inherits `pool_config` from `cluster_options::pool`; the node endpoint is
// filled in per node.
class CRPC_API cluster_pool_provider final : public cluster_executor_provider {
public:
    explicit cluster_pool_provider(crpc::backend_factory& backend,
                                   runtime* shard = nullptr);
    cluster_pool_provider(crpc::backend_factory& backend,
                          const pool_config& config, runtime* shard = nullptr);
    ~cluster_pool_provider() override;

    cluster_pool_provider(const cluster_pool_provider&) = delete;
    cluster_pool_provider& operator=(const cluster_pool_provider&) = delete;

    [[nodiscard]] result<command_executor*> executor_for(
        const cluster_address& node) override;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

// Tunables for the cluster client. Everything is a run-time value.
struct cluster_options {
    // Nodes used to discover the topology. A seed only has to answer
    // CLUSTER SLOTS/SHARDS; it need not own the slot being addressed.
    std::vector<cluster_address> seeds;

    // Optional dynamic seed source (for example Sentinel). It is consulted on
    // every refresh, so a failover that changes the master address is picked
    // up without rebuilding the client. Seeds returned here are appended to
    // `seeds` and de-duplicated.
    std::function<std::vector<cluster_address>()> seed_provider;

    // Maximum number of MOVED/ASK hops for one logical command.
    std::size_t max_redirects = 5;

    // Maximum number of topology refreshes for one logical command when the
    // slot is unknown or the server reports CLUSTERDOWN/TRYAGAIN.
    std::size_t max_refresh_attempts = 3;

    // Ask for CLUSTER SHARDS first and fall back to CLUSTER SLOTS when the
    // server does not support it (Redis < 7). Set false to use CLUSTER SLOTS
    // directly.
    bool prefer_cluster_shards = true;

    // Connection pool used by the default backend provider. Ignored when the
    // caller supplies its own `cluster_executor_provider`.
    pool_config pool{};
};

class cluster;
class cluster_slot_executor;

// A Redis Cluster client. It is a `command_executor`, so a `pipeline` can be
// built on it; `execute_batch()` enforces that the whole pipeline stays in one
// slot.
class CRPC_API cluster final : public command_executor {
public:
    cluster(cluster_options options, cluster_executor_provider& provider);
    cluster(cluster_options options, crpc::backend_factory& backend,
            runtime* shard = nullptr);
    ~cluster() override;

    cluster(const cluster&) = delete;
    cluster& operator=(const cluster&) = delete;
    cluster(cluster&&) = delete;
    cluster& operator=(cluster&&) = delete;

    // Routes a single command by the slot of its keys. A command with no keys
    // is sent to any known node. A command whose keys span slots fails with
    // `failed_precondition` (CROSSSLOT).
    [[nodiscard]] result<resp_value> execute(const command& cmd) override;

    // Sends a pipeline. Every command must address the same slot (keyless
    // commands are allowed anywhere in the batch). Cross-slot batches are
    // rejected locally before I/O. MOVED/ASK replies inside the batch are
    // followed per command, so a successful command is never replayed.
    [[nodiscard]] result<std::vector<resp_value>> execute_batch(
        const std::vector<command>& commands) override;

    // Explicit routing helpers. `slot` must be < cluster_slot_count.
    [[nodiscard]] result<resp_value> execute_on_slot(std::uint16_t slot,
                                                     const command& cmd);
    [[nodiscard]] result<std::vector<resp_value>> execute_batch_on_slot(
        std::uint16_t slot, const std::vector<command>& commands);

    // Sends directly to `node` without consulting the slot table. MOVED/ASK
    // replies are returned as ordinary RESP errors; use `execute_on_slot()`
    // when redirects should be followed.
    [[nodiscard]] result<resp_value> execute_on_node(
        const cluster_address& node, const command& cmd);

    // Reloads the slot table from the seeds, the dynamic seed provider and the
    // currently known masters. Tries CLUSTER SHARDS then CLUSTER SLOTS per
    // candidate. The current table is replaced only on success.
    [[nodiscard]] status refresh();

    [[nodiscard]] const cluster_topology& topology() const noexcept;
    [[nodiscard]] const cluster_options& options() const noexcept;

    // Validates `keys` (non-empty, all in one slot) and returns an executor
    // pinned to that slot. Hand it to `transaction` to keep MULTI/EXEC on one
    // node. Cross-slot keys fail with `failed_precondition` (CROSSSLOT).
    [[nodiscard]] result<std::unique_ptr<cluster_slot_executor>> pin(
        const std::vector<std::string>& keys);
    [[nodiscard]] result<std::unique_ptr<cluster_slot_executor>> pin_slot(
        std::uint16_t slot);

    // Convenience wrapper around `cluster_command_keys`.
    [[nodiscard]] static std::vector<std::string> keys_of(const command& cmd) {
        return cluster_command_keys(cmd);
    }

private:
    [[nodiscard]] result<resp_value> execute_keyless(const command& cmd);
    [[nodiscard]] result<std::vector<resp_value>> execute_batch_keyless(
        const std::vector<command>& commands);
    [[nodiscard]] result<resp_value> execute_ask(std::uint16_t slot,
                                                 const cluster_address& target,
                                                 const command& cmd);
    [[nodiscard]] const cluster_address* any_node() const noexcept;

    struct impl;
    std::unique_ptr<impl> impl_;
};

// A command executor pinned to one slot. Every command is routed through the
// owning node with the same MOVED/ASK handling as the parent cluster. It is
// what makes a Redis transaction cluster-safe: all keys were validated by
// `cluster::pin()`, and MULTI/EXEC cannot drift to another node.
class CRPC_API cluster_slot_executor final : public command_executor {
public:
    cluster_slot_executor(cluster& owner, std::uint16_t slot) noexcept;
    ~cluster_slot_executor() override;

    cluster_slot_executor(const cluster_slot_executor&) = delete;
    cluster_slot_executor& operator=(const cluster_slot_executor&) = delete;

    [[nodiscard]] result<resp_value> execute(const command& cmd) override;
    [[nodiscard]] result<std::vector<resp_value>> execute_batch(
        const std::vector<command>& commands) override;

    [[nodiscard]] std::uint16_t slot() const noexcept { return slot_; }

private:
    cluster* owner_;
    std::uint16_t slot_;
};

// --- Sentinel cooperation ---------------------------------------------------
//
// Sentinel does not know about cluster slots, but it does know which node is
// the current master. A cluster seed provider can therefore ask Sentinel for
// the master address and let the node itself report the topology. Multiple
// monitors (one per shard, or a monitor plus a static seed) are supported.
[[nodiscard]] CRPC_API std::vector<cluster_address> cluster_seeds_from_sentinel(
    const sentinel& monitor);
[[nodiscard]] CRPC_API std::vector<cluster_address> cluster_seeds_from_sentinels(
    const std::vector<const sentinel*>& monitors);

} // namespace redis
} // namespace crpc

#endif // CRPC_REDIS_CLUSTER_HPP
