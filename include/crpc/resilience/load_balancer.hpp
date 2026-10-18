// CRPC -- load balancing over a set of service instances.
//
// Strategies:
//   * round_robin          -- atomic cursor, lock-free.
//   * random               -- atomic xorshift64*, lock-free.
//   * least_inflight       -- scans the set and picks the healthy instance with
//                             the smallest `inflight`; ties rotate.
//   * consistent_hash      -- FNV-1a ring (with a splitmix64 avalanche step)
//                             and 160 virtual nodes per weight unit. Only keys
//                             whose successor changes move when an instance is
//                             added, so roughly 1/(N+1) of keys are remapped.
//   * weighted_round_robin -- smooth weighted round-robin (nginx algorithm).
//
// Thread safety:
//   * `round_robin`, `random`, `least_inflight` and `consistent_hash` are safe
//     for concurrent calls on one `load_balancer`.
//   * `weighted_round_robin` keeps mutable per-instance current weights. It is
//     shard-local: callers must serialize calls to a given load_balancer (one
//     instance per shard, or external locking).
//   * `instance_set` is read concurrently by all strategies. Its `at()` and
//     `version()` must be safe for concurrent reads while selections run, and
//     `version()` must change whenever membership, health or weight changes.
//     The consistent-hash ring and the weighted-round-robin state are rebuilt
//     when the version changes.
//
// The hash function is FNV-1a over explicitly little-endian bytes. `std::hash`
// is deliberately not used because its result is not stable across standard
// library versions and would make consistent hashing non-portable.
#ifndef CRPC_RESILIENCE_LOAD_BALANCER_HPP
#define CRPC_RESILIENCE_LOAD_BALANCER_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc::resilience {

// Thread safety: a plain value type. `inflight` is a hint read by
// `least_inflight`; the caller is responsible for updating it.
struct instance {
    u64 id{0};
    u32 weight{1};
    bool healthy{true};
    u32 inflight{0};
};

// Read-only view of the candidate instances. Implementations must keep `at()`
// valid for concurrent reads and must monotonically increase `version()` on any
// change to membership, health or weight.
class instance_set {
public:
    virtual ~instance_set() = default;

    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual instance at(std::size_t index) const noexcept = 0;
    [[nodiscard]] virtual u64 version() const noexcept = 0;
};

enum class lb_strategy : u8 {
    round_robin = 0,
    random = 1,
    least_inflight = 2,
    consistent_hash = 3,
    weighted_round_robin = 4
};

class load_balancer {
public:
    explicit load_balancer(lb_strategy strategy = lb_strategy::round_robin,
                           u32 virtual_nodes = 160,
                           u64 seed = 0x9E3779B97F4A7C15ull) noexcept;

    load_balancer(const load_balancer&) = delete;
    load_balancer& operator=(const load_balancer&) = delete;
    load_balancer(load_balancer&&) = delete;
    load_balancer& operator=(load_balancer&&) = delete;

    // Selects without a hash key. For `consistent_hash` this is equivalent to
    // `select(set, 0)`, which is rarely what a caller wants; use the key
    // overload for hashing strategies.
    [[nodiscard]] result<instance> select(const instance_set& set);

    // Selects using `key` for `consistent_hash`; other strategies ignore it.
    [[nodiscard]] result<instance> select(const instance_set& set, u64 key);

    // Pre-builds the consistent-hash ring and the weighted-round-robin state.
    // Not required before selection, but useful on a configuration-change path
    // so the hot path never allocates.
    void update(const instance_set& set);

    [[nodiscard]] lb_strategy strategy() const noexcept { return strategy_; }
    [[nodiscard]] u32 virtual_nodes() const noexcept { return virtual_nodes_; }

private:
    struct ring_node {
        u64 hash = 0;
        u64 id = 0;
    };

    struct ring_data {
        u64 version = 0;
        std::vector<ring_node> nodes;
    };

    struct wrr_entry {
        u64 id = 0;
        u32 weight = 1;
        i64 current = 0;
    };

    [[nodiscard]] result<instance> select_round_robin(const instance_set& set) noexcept;
    [[nodiscard]] result<instance> select_random(const instance_set& set) noexcept;
    [[nodiscard]] result<instance> select_least_inflight(const instance_set& set) noexcept;
    [[nodiscard]] result<instance> select_weighted(const instance_set& set);
    [[nodiscard]] result<instance> select_hash(const instance_set& set, u64 key);

    [[nodiscard]] u64 next_random() noexcept;
    void ensure_ring(const instance_set& set);
    void ensure_weighted(const instance_set& set);

    lb_strategy strategy_;
    u32 virtual_nodes_;
    std::atomic<u64> cursor_{0};
    std::atomic<u64> random_state_;

    std::mutex ring_mutex_;
    std::shared_ptr<const ring_data> ring_;

    bool weighted_ready_ = false;
    u64 weighted_version_ = 0;
    std::vector<wrr_entry> weighted_;
};

} // namespace crpc::resilience

#endif // CRPC_RESILIENCE_LOAD_BALANCER_HPP
