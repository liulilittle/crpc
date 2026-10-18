#include <crpc/resilience/load_balancer.hpp>

#include <algorithm>
#include <limits>

namespace crpc::resilience {

namespace {

constexpr u64 k_fnv_offset = 14695981039346656037ull;
constexpr u64 k_fnv_prime = 1099511628211ull;
constexpr u64 k_default_seed = 0x9E3779B97F4A7C15ull;
constexpr u64 k_max_virtual_nodes = 1ull << 20;

u64 fnv1a(const u8* data, std::size_t size) noexcept {
    u64 hash = k_fnv_offset;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<u64>(data[i]);
        hash *= k_fnv_prime;
    }
    return hash;
}

// splitmix64's finalizer. FNV-1a alone is sensitive to the mostly-zero byte
// layout of small ids/replicas, which correlates vnode positions with small
// keys. The finalizer restores avalanche without changing the stability of the
// function (it is still a pure function of the input bytes).
u64 mix64(u64 value) noexcept {
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

u64 hash_key(u64 key) noexcept {
    u8 bytes[8];
    for (u32 i = 0; i < 8; ++i) {
        bytes[i] = static_cast<u8>((key >> (8 * i)) & 0xFFu);
    }
    return mix64(fnv1a(bytes, sizeof(bytes)));
}

u64 hash_vnode(u64 id, u64 replica) noexcept {
    u8 bytes[16];
    for (u32 i = 0; i < 8; ++i) {
        bytes[i] = static_cast<u8>((id >> (8 * i)) & 0xFFu);
        bytes[8 + i] = static_cast<u8>((replica >> (8 * i)) & 0xFFu);
    }
    return mix64(fnv1a(bytes, sizeof(bytes)));
}

result<instance> no_healthy_instance() {
    return result<instance>(rpc_error(rpc_code::unavailable, "no healthy instance"));
}

bool find_instance(const instance_set& set, u64 id, instance& out) {
    const std::size_t count = set.size();
    for (std::size_t i = 0; i < count; ++i) {
        const instance candidate = set.at(i);
        if (candidate.id == id) {
            out = candidate;
            return true;
        }
    }
    return false;
}

} // namespace

load_balancer::load_balancer(lb_strategy strategy, u32 virtual_nodes,
                             u64 seed) noexcept
    : strategy_(strategy),
      virtual_nodes_(virtual_nodes == 0 ? 1 : virtual_nodes),
      random_state_(seed == 0 ? k_default_seed : seed) {}

result<instance> load_balancer::select(const instance_set& set) {
    return select(set, 0);
}

result<instance> load_balancer::select(const instance_set& set, u64 key) {
    switch (strategy_) {
    case lb_strategy::round_robin:
        return select_round_robin(set);
    case lb_strategy::random:
        return select_random(set);
    case lb_strategy::least_inflight:
        return select_least_inflight(set);
    case lb_strategy::consistent_hash:
        return select_hash(set, key);
    case lb_strategy::weighted_round_robin:
        return select_weighted(set);
    }
    return no_healthy_instance();
}

void load_balancer::update(const instance_set& set) {
    switch (strategy_) {
    case lb_strategy::consistent_hash:
        ensure_ring(set);
        break;
    case lb_strategy::weighted_round_robin:
        ensure_weighted(set);
        break;
    default:
        break;
    }
}

result<instance> load_balancer::select_round_robin(const instance_set& set) noexcept {
    const std::size_t count = set.size();
    if (count == 0) {
        return no_healthy_instance();
    }

    const u64 start = cursor_.fetch_add(1, std::memory_order_relaxed);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t index =
            static_cast<std::size_t>((start + static_cast<u64>(i)) %
                                     static_cast<u64>(count));
        const instance candidate = set.at(index);
        if (candidate.healthy) {
            return candidate;
        }
    }
    return no_healthy_instance();
}

result<instance> load_balancer::select_random(const instance_set& set) noexcept {
    const std::size_t count = set.size();
    if (count == 0) {
        return no_healthy_instance();
    }

    const u64 start = next_random();
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t index =
            static_cast<std::size_t>((start + static_cast<u64>(i)) %
                                     static_cast<u64>(count));
        const instance candidate = set.at(index);
        if (candidate.healthy) {
            return candidate;
        }
    }
    return no_healthy_instance();
}

result<instance> load_balancer::select_least_inflight(const instance_set& set) noexcept {
    const std::size_t count = set.size();
    if (count == 0) {
        return no_healthy_instance();
    }

    const u64 start = cursor_.fetch_add(1, std::memory_order_relaxed);
    std::size_t best_index = count;
    u32 best_inflight = std::numeric_limits<u32>::max();

    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t index =
            static_cast<std::size_t>((start + static_cast<u64>(i)) %
                                     static_cast<u64>(count));
        const instance candidate = set.at(index);
        if (!candidate.healthy) {
            continue;
        }
        if (best_index == count || candidate.inflight < best_inflight) {
            best_index = index;
            best_inflight = candidate.inflight;
        }
    }

    if (best_index == count) {
        return no_healthy_instance();
    }
    return set.at(best_index);
}

result<instance> load_balancer::select_weighted(const instance_set& set) {
    ensure_weighted(set);
    if (weighted_.empty()) {
        return no_healthy_instance();
    }

    i64 total = 0;
    for (wrr_entry& entry : weighted_) {
        entry.current += static_cast<i64>(entry.weight);
        total += static_cast<i64>(entry.weight);
    }
    if (total <= 0) {
        return no_healthy_instance();
    }

    std::size_t best = 0;
    for (std::size_t i = 1; i < weighted_.size(); ++i) {
        if (weighted_[i].current > weighted_[best].current) {
            best = i;
        }
    }
    weighted_[best].current -= total;

    instance selected;
    if (!find_instance(set, weighted_[best].id, selected) || !selected.healthy) {
        return no_healthy_instance();
    }
    return selected;
}

result<instance> load_balancer::select_hash(const instance_set& set, u64 key) {
    ensure_ring(set);

    const std::shared_ptr<const ring_data> ring =
        std::atomic_load_explicit(&ring_, std::memory_order_acquire);
    if (!ring || ring->nodes.empty()) {
        return no_healthy_instance();
    }

    const u64 hash = hash_key(key);
    const auto it = std::lower_bound(
        ring->nodes.begin(), ring->nodes.end(), hash,
        [](const ring_node& node, u64 value) { return node.hash < value; });
    const std::size_t start =
        it == ring->nodes.end() ? 0 : static_cast<std::size_t>(it - ring->nodes.begin());

    const std::size_t count = ring->nodes.size();
    for (std::size_t i = 0; i < count; ++i) {
        const ring_node& node = ring->nodes[(start + i) % count];
        instance candidate;
        // Re-check the live set: health may have changed since the ring was
        // built, and a dead instance must never be selected.
        if (find_instance(set, node.id, candidate) && candidate.healthy) {
            return candidate;
        }
    }
    return no_healthy_instance();
}

u64 load_balancer::next_random() noexcept {
    u64 state = random_state_.load(std::memory_order_relaxed);
    for (;;) {
        u64 next = state;
        next ^= next << 13;
        next ^= next >> 7;
        next ^= next << 17;
        if (next == 0) {
            next = k_default_seed;
        }
        if (random_state_.compare_exchange_weak(state, next,
                                                std::memory_order_relaxed,
                                                std::memory_order_relaxed)) {
            return next;
        }
    }
}

void load_balancer::ensure_ring(const instance_set& set) {
    const u64 version = set.version();
    std::shared_ptr<const ring_data> current =
        std::atomic_load_explicit(&ring_, std::memory_order_acquire);
    if (current && current->version == version) {
        return;
    }

    std::lock_guard<std::mutex> lock(ring_mutex_);
    current = std::atomic_load_explicit(&ring_, std::memory_order_acquire);
    if (current && current->version == version) {
        return;
    }

    auto fresh = std::make_shared<ring_data>();
    fresh->version = version;

    const std::size_t count = set.size();
    for (std::size_t i = 0; i < count; ++i) {
        const instance candidate = set.at(i);
        if (!candidate.healthy || candidate.weight == 0) {
            continue;
        }

        u64 replicas = static_cast<u64>(virtual_nodes_) *
                       static_cast<u64>(candidate.weight);
        if (replicas > k_max_virtual_nodes) {
            replicas = k_max_virtual_nodes;
        }
        for (u64 replica = 0; replica < replicas; ++replica) {
            fresh->nodes.push_back(ring_node{hash_vnode(candidate.id, replica),
                                             candidate.id});
        }
    }

    std::sort(fresh->nodes.begin(), fresh->nodes.end(),
              [](const ring_node& lhs, const ring_node& rhs) {
                  return lhs.hash != rhs.hash ? lhs.hash < rhs.hash
                                              : lhs.id < rhs.id;
              });

    const std::shared_ptr<const ring_data> immutable = fresh;
    std::atomic_store_explicit(&ring_, immutable, std::memory_order_release);
}

void load_balancer::ensure_weighted(const instance_set& set) {
    const u64 version = set.version();
    if (weighted_ready_ && weighted_version_ == version) {
        return;
    }

    weighted_.clear();
    const std::size_t count = set.size();
    for (std::size_t i = 0; i < count; ++i) {
        const instance candidate = set.at(i);
        if (!candidate.healthy || candidate.weight == 0) {
            continue;
        }
        weighted_.push_back(wrr_entry{candidate.id, candidate.weight, 0});
    }

    weighted_ready_ = true;
    weighted_version_ = version;
}

} // namespace crpc::resilience
