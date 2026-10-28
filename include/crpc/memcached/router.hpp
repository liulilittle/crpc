// CRPC -- multi-server memcached routing.
//
// `client` owns one endpoint. `router` owns several and decides which one a key
// belongs to, using the libketama ring from `hash.hpp`. Splitting the two keeps a
// single-server deployment free of ring cost and keeps placement in one place.
//
// Three rules shape the behaviour:
//
//   * placement is per key, not per request. A multi-key `get` therefore fans out
//     to every server that owns one of the keys and merges the answers.
//   * a fan-out is all-or-nothing. Returning the keys that happened to succeed
//     would look like "these keys are absent", which is indistinguishable from a
//     cache miss and would let a caller cache a negative result that is really a
//     network error. A partial failure is reported as an error.
//   * fan-out is sequential. Each call parks its coroutine, so a caller that
//     wants concurrency issues concurrent calls; the router does not open an
//     unbounded number of in-flight requests behind the caller's back.
//
// Node health sits on top of placement. A node that fails repeatedly at the
// transport level is ejected from the ring, so a key it used to own is placed on
// another live node. That remapping is deliberate and is *not* transparent:
// memcached nodes share no data, so the key now lives in a different cache. See
// the health/ejection rules on `failover_options` before enabling anything that
// changes routing on failure.
#ifndef CRPC_MEMCACHED_ROUTER_HPP
#define CRPC_MEMCACHED_ROUTER_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/memcached/client.hpp>
#include <crpc/memcached/hash.hpp>
#include <crpc/memcached/protocol.hpp>

namespace crpc {
namespace memcached {

// Whether a node currently receives routed traffic.
enum class node_condition : std::uint8_t {
    // In the ring and eligible for routing.
    alive,
    // Ejected from the ring after `failure_threshold` consecutive transport
    // failures. It is re-admitted only after a successful probe.
    dead
};

// A point-in-time view of one node, for operations and for diagnostics.
struct CRPC_API node_health {
    // The `router::server::name` this state belongs to; the same string the ring
    // hashes.
    std::string name;
    node_condition condition = node_condition::alive;
    // Transport failures since the last successful exchange. Application replies
    // (`SERVER_ERROR`, `NOT_FOUND`, ...) prove the node is reachable and reset
    // this to zero; they are not counted as node failures.
    std::uint32_t consecutive_failures = 0;
    // The most recent transport failure. Retained after recovery as a diagnostic;
    // `consecutive_failures == 0` means the node is currently healthy.
    rpc_code last_error = rpc_code::ok;
    std::string last_error_message;
    // Time left before the next recovery probe. Zero while alive, or once the
    // cooldown has elapsed and a probe is due.
    std::chrono::milliseconds retry_in{0};
};

// Node-health policy for `router`.
//
// A node is ejected only for transport-level failures: connection refused/reset,
// broken pipe, EOF, deadline and `unavailable`. A `SERVER_ERROR`, a protocol
// violation or a routine `NOT_FOUND` means the endpoint answered, so it is alive
// for routing purposes even when the operation failed.
//
// Recovery is probing, not blind re-admission. Once `retry_cooldown` has elapsed
// the next public router call (or an explicit `probe_node`) sends one `version`
// command to the dead node. A reply re-admits it to the ring immediately; a
// failure leaves it dead and restarts the cooldown. Because re-admission changes
// the ring, keys owned by the recovered node move again - the same consistency
// consequence as ejection.
struct CRPC_API failover_options {
    // Consecutive transport failures before ejection. 0 disables ejection; the
    // counters and `health()` still update.
    std::uint32_t failure_threshold = 3;

    // How long a dead node stays out of the ring before a probe is attempted.
    // Zero means "probe on the next call".
    std::chrono::milliseconds retry_cooldown{30000};

    // Retry a failed read on the next live node in the ring.
    //
    // Default false, and that default is a correctness decision. memcached nodes
    // do not replicate: a key is stored on exactly one node. If the owner is
    // unreachable, asking another node can:
    //   * return a *different* value for a same-named key that happens to be
    //     stored there (a stale/unrelated value), or
    //   * return a miss even though the data exists on the unreachable node.
    // Both look like normal cache answers to the caller, so a cross-node retry
    // silently converts a detected failure into potentially wrong data.
    //
    // Enable it only when the deployment guarantees that a key exists on at most
    // one node and that a miss is an acceptable outcome for a failed primary
    // (for example a pure sharding key space where collisions are impossible, or
    // a replication layer this client does not know about). Writes are never
    // retried: sending a write to the wrong hash position would make the next
    // read of that key inconsistent, so a write failure is always reported.
    //
    // This flag only controls retrying the *same call*. Once a node is ejected,
    // later calls follow the rebuilt ring, so they can still land on another
    // node; that remapping is described above and in the guide.
    bool retry_reads = false;
};

// A keyed collection of endpoints.
class CRPC_API router {
public:
    // One endpoint. `name` is the string the ring hashes, conventionally
    // "host:port"; it must be unique and must be the same string every client in
    // the deployment uses, or the placement differs between clients.
    struct server {
        std::string name;
        std::shared_ptr<transport> endpoint;
        // Relative share of the key space. 0 removes the server from the ring
        // without removing it from the fan-out set used by `flush_all`.
        std::uint32_t weight = 1;
    };

    // `opts` is applied to every endpoint. It must not carry a key prefix that
    // differs per server: the ring hashes the key as written, so a prefix that
    // varies would place one key on different servers.
    //
    // `failover` is the node-health policy. Constructing a router never contacts
    // a server; the first public call may probe nodes that were ejected by an
    // earlier call and whose cooldown has elapsed.
    router(std::vector<server> servers, client::options opts = {},
           failover_options failover = {});
    ~router();

    router(const router&) = delete;
    router& operator=(const router&) = delete;
    router(router&&) = delete;
    router& operator=(router&&) = delete;

    // --- Keyed operations ----------------------------------------------------
    // Routed to the server that owns `key`. `get` accepts several keys and fans
    // out; a key on a different server is still fetched.
    [[nodiscard]] result<std::vector<value_entry>> get(const std::vector<std::string>& keys);
    [[nodiscard]] result<std::vector<value_entry>> gets(const std::vector<std::string>& keys);

    [[nodiscard]] result<store_status> store(store_kind kind, std::string_view key,
                                             std::string_view value, std::uint32_t flags = 0,
                                             std::uint64_t exptime = 0, std::uint64_t cas = 0);
    [[nodiscard]] result<store_status> erase(std::string_view key, std::uint64_t cas = 0);
    [[nodiscard]] result<std::uint64_t> increment(std::string_view key, std::uint64_t delta);
    [[nodiscard]] result<std::uint64_t> decrement(std::string_view key, std::uint64_t delta);
    [[nodiscard]] result<store_status> touch(std::string_view key, std::uint64_t exptime);

    // --- Fan-out -------------------------------------------------------------
    // Every server in the ring is contacted. A failure on any server fails the
    // call, because "flush half the cache" is not a useful success.
    [[nodiscard]] status flush_all();
    // Version of every server, in the order the servers were given.
    [[nodiscard]] result<std::vector<std::pair<std::string, std::string>>> versions();
    // Per-server stats. The outer vector follows the server order, and each entry
    // pairs the server name with its stats.
    [[nodiscard]] result<std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>>
    stats(std::string_view argument = {});

    // --- Placement -----------------------------------------------------------
    // The server index a key is placed on, or -1 when the ring is empty (no
    // server configured, or every node is dead). Dead nodes are not in the ring.
    // Exposed because a caller that migrates data needs to ask the same question
    // the router answers. This is a pure lookup: it never contacts a server and
    // never triggers a recovery probe.
    [[nodiscard]] std::ptrdiff_t owner_of(std::string_view key) const noexcept;
    [[nodiscard]] std::size_t server_count() const noexcept;
    [[nodiscard]] const std::vector<server>& servers() const noexcept;

    // --- Health --------------------------------------------------------------
    // A snapshot in server order. Cheap and local: it does not contact servers,
    // so a dead node with an elapsed cooldown stays `dead` here until a routed
    // call or `probe_node` actually probes it.
    [[nodiscard]] std::vector<node_health> health() const;

    // Probes one node immediately, regardless of the cooldown. Returns true when
    // the node answered (a reply of any kind makes it alive for routing) and
    // false when the probe failed at the transport level. With ejection disabled
    // (`failure_threshold == 0`) a failed probe does not mark a live node dead,
    // but the return value still reports the failed probe. An out-of-range index
    // is `invalid_argument`.
    //
    // The probe is `version`: it is a read-only command present in every
    // memcached, and any reply proves the endpoint is reachable.
    [[nodiscard]] result<bool> probe_node(std::size_t index);

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace memcached
} // namespace crpc

#endif // CRPC_MEMCACHED_ROUTER_HPP