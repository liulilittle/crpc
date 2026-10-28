// CRPC -- grid pathfinding benchmarks.
//
// What this measures, and why:
//
//   * expansion counts and wall time for A*, B*, BFS and bidirectional BFS on
//     the two sizes the goal requires (512x512 and 1024x1024), on open boards.
//     Open boards isolate the algorithm/container cost from obstacle layout.
//   * the open-set choice: the library's reserved binary heap versus a
//     std::priority_queue implementation of the same A* (same arrays, same
//     neighbour iteration, same tie-break), so the delta is the container.
//   * the visited-set choice: the dense flat arrays used by pathfind.hpp versus
//     the generic astar_path<std::size_t>, which stores g and came_from in
//     std::unordered_map. The allocation counters below are the real number of
//     heap allocations and bytes requested during one search.
//
// The machine is a shared CI runner, so this is a benchmark, not a test: it
// reports medians of five runs and is never part of the pass/fail gate.
#include <crpc/algorithm/astar.hpp>
#include <crpc/algorithm/pathfind.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "bench_harness.hpp"

namespace {

std::atomic<std::size_t> g_alloc_calls{0};
std::atomic<std::size_t> g_alloc_bytes{0};
std::atomic<bool> g_counting{false};

constexpr std::size_t k_unknown = static_cast<std::size_t>(-1);

} // namespace

void* operator new(std::size_t size) {
    if (g_counting.load(std::memory_order_relaxed)) {
        g_alloc_calls.fetch_add(1, std::memory_order_relaxed);
        g_alloc_bytes.fetch_add(size, std::memory_order_relaxed);
    }
    void* pointer = std::malloc(size == 0 ? 1 : size);
    if (pointer == nullptr) {
        throw std::bad_alloc();
    }
    return pointer;
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (g_counting.load(std::memory_order_relaxed)) {
        g_alloc_calls.fetch_add(1, std::memory_order_relaxed);
        g_alloc_bytes.fetch_add(size, std::memory_order_relaxed);
    }
    return std::malloc(size == 0 ? 1 : size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return ::operator new(size, std::nothrow);
}

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
    std::free(pointer);
}

namespace {

using crpc::algorithm::connectivity;
using crpc::algorithm::grid_map;

struct outcome {
    bool ok = false;
    std::size_t expanded = k_unknown;
    std::size_t generated = k_unknown;
};

grid_map open_board(std::size_t size) {
    grid_map map;
    map.width = size;
    map.height = size;
    map.walkable.assign(size * size, 1);
    return map;
}

std::uint64_t xorshift64(std::uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

grid_map obstacle_board(std::size_t size, std::uint64_t seed,
                        std::size_t density_pct) {
    grid_map map = open_board(size);
    std::uint64_t state = seed;
    for (std::size_t i = 0; i < map.walkable.size(); ++i) {
        if (xorshift64(state) % 100 < density_pct) {
            map.walkable[i] = 0;
        }
    }
    map.walkable.front() = 1;
    map.walkable.back() = 1;
    return map;
}

// Same A* as pathfind.hpp's astar_grid, but with std::priority_queue as the
// open set. Everything else (flat arrays, neighbour order, tie-break) is
// identical so the timing delta is attributable to the container.
struct pq_entry {
    double f;
    double g;
    std::size_t node;
};

struct pq_greater {
    bool operator()(const pq_entry& a, const pq_entry& b) const {
        if (a.f != b.f) {
            return a.f > b.f;
        }
        if (a.g != b.g) {
            return a.g < b.g;
        }
        return a.node > b.node;
    }
};

outcome astar_priority_queue(const grid_map& map, std::size_t start,
                             std::size_t goal) {
    outcome out;
    const std::size_t count = map.walkable.size();
    const double inf = std::numeric_limits<double>::infinity();
    std::vector<double> g(count, inf);
    std::vector<std::size_t> parent(count, k_unknown);
    std::vector<std::uint8_t> closed(count, 0);
    std::priority_queue<pq_entry, std::vector<pq_entry>, pq_greater> open;

    g[start] = 0.0;
    open.push({crpc::algorithm::grid_manhattan_heuristic(map, start, goal), 0.0,
               start});
    while (!open.empty()) {
        const pq_entry entry = open.top();
        open.pop();
        if (closed[entry.node] != 0 || entry.g != g[entry.node]) {
            continue;
        }
        if (entry.node == goal) {
            out.ok = true;
            return out;
        }
        closed[entry.node] = 1;
        ++out.expanded;
        auto relax = [&](std::size_t next) {
            const double tentative =
                entry.g + crpc::algorithm::grid_step_cost(map, entry.node, next);
            if (!(tentative < g[next])) {
                return;
            }
            g[next] = tentative;
            parent[next] = entry.node;
            if (closed[next] != 0) {
                closed[next] = 0;
            }
            open.push({tentative +
                           crpc::algorithm::grid_manhattan_heuristic(map, next,
                                                                     goal),
                       tentative, next});
            ++out.generated;
        };
        crpc::algorithm::detail::visit_grid_neighbors(
            map, entry.node, connectivity::four, relax);
    }
    return out;
}

struct row {
    std::string algorithm;
    std::size_t size = 0;
    std::string goal_kind;
    std::size_t expanded = k_unknown;
    std::size_t generated = k_unknown;
    double median_ms = 0.0;
    double spread_ms = 0.0;
    std::size_t allocations = 0;
    std::size_t bytes = 0;
};

template <typename Body>
row measure(const char* algorithm, std::size_t size, const char* goal_kind,
            Body&& body) {
    row r;
    r.algorithm = algorithm;
    r.size = size;
    r.goal_kind = goal_kind;

    outcome last = body(); // warm-up, also validates the case
    std::vector<double> samples;
    samples.reserve(5);
    for (int run = 0; run < 5; ++run) {
        const auto begin = std::chrono::steady_clock::now();
        last = body();
        const auto stop = std::chrono::steady_clock::now();
        samples.push_back(
            std::chrono::duration<double, std::milli>(stop - begin).count());
    }
    std::sort(samples.begin(), samples.end());
    r.median_ms = samples[samples.size() / 2];
    r.spread_ms = samples.back() - samples.front();
    r.expanded = last.expanded;
    r.generated = last.generated;

    g_alloc_calls.store(0, std::memory_order_relaxed);
    g_alloc_bytes.store(0, std::memory_order_relaxed);
    g_counting.store(true, std::memory_order_relaxed);
    const outcome counted = body();
    g_counting.store(false, std::memory_order_relaxed);
    r.allocations = g_alloc_calls.load(std::memory_order_relaxed);
    r.bytes = g_alloc_bytes.load(std::memory_order_relaxed);
    if (!counted.ok && !last.ok) {
        std::printf("%-20s %5zu %-12s FAILED\n", algorithm, size, goal_kind);
    }
    return r;
}

void report(const row& r) {
    const double mib = static_cast<double>(r.bytes) / (1024.0 * 1024.0);
    std::printf("%-20s %5zu %-12s %10zu %10zu %9.3f %8.3f %10zu %8.2f\n",
                r.algorithm.c_str(), r.size, r.goal_kind.c_str(), r.expanded,
                r.generated, r.median_ms, r.spread_ms, r.allocations, mib);
    crpc_bench::result recorded;
    recorded.name = r.algorithm + "/" + std::to_string(r.size) + "/" +
                    r.goal_kind;
    recorded.unit = "ms/search";
    recorded.value = r.median_ms;
    recorded.spread = r.spread_ms;
    crpc_bench::results().push_back(recorded);
}

outcome dense_astar(const grid_map& map, std::size_t goal,
                    const crpc::algorithm::grid_search_config& config) {
    const auto found = crpc::algorithm::astar_grid(
        map, 0, goal, crpc::algorithm::grid_step_cost,
        crpc::algorithm::grid_manhattan_heuristic, config);
    outcome out;
    out.ok = found.ok();
    if (found.ok()) {
        out.expanded = found.value().stats.expanded;
        out.generated = found.value().stats.generated;
    }
    return out;
}

outcome dense_bstar(const grid_map& map, std::size_t goal,
                    const crpc::algorithm::grid_search_config& config) {
    const auto found = crpc::algorithm::bstar_grid(
        map, 0, goal, crpc::algorithm::grid_step_cost,
        crpc::algorithm::grid_manhattan_heuristic, config);
    outcome out;
    out.ok = found.ok();
    if (found.ok()) {
        out.expanded = found.value().stats.expanded;
        out.generated = found.value().stats.generated;
    }
    return out;
}



outcome dense_bfs(const grid_map& map, std::size_t goal,
                  const crpc::algorithm::grid_search_config& config) {
    const auto found = crpc::algorithm::bfs_grid(map, 0, goal, config);
    outcome out;
    out.ok = found.ok();
    if (found.ok()) {
        out.expanded = found.value().stats.expanded;
        out.generated = found.value().stats.generated;
    }
    return out;
}

outcome dense_bidirectional_bfs(
    const grid_map& map, std::size_t goal,
    const crpc::algorithm::grid_search_config& config) {
    const auto found =
        crpc::algorithm::bidirectional_bfs_grid(map, 0, goal, config);
    outcome out;
    out.ok = found.ok();
    if (found.ok()) {
        out.expanded = found.value().stats.expanded;
        out.generated = found.value().stats.generated;
    }
    return out;
}

// The generic A* from astar.hpp: unordered_map for both g and came_from. It
// cannot report expansion counts without changing the public interface, so the
// row shows allocations/bytes/time only.
outcome generic_astar(const grid_map& map, std::size_t goal) {
    auto neighbors = [&](std::size_t node, std::vector<std::size_t>& out) {
        auto append = [&](std::size_t next) { out.push_back(next); };
        crpc::algorithm::detail::visit_grid_neighbors(map, node,
                                                       connectivity::four,
                                                       append);
    };
    auto cost = [&](std::size_t from, std::size_t to) {
        return crpc::algorithm::grid_step_cost(map, from, to);
    };
    auto heuristic = [&](std::size_t node, std::size_t target) {
        return crpc::algorithm::grid_manhattan_heuristic(map, node, target);
    };
    const auto found = crpc::algorithm::astar_path<std::size_t>(
        0, goal, neighbors, cost, heuristic);
    outcome out;
    out.ok = found.ok();
    return out;
}

void run_size(std::size_t size) {
    const grid_map map = open_board(size);
    const std::size_t corner = map.walkable.size() - 1;
    const std::size_t same_row = size - 1;
    crpc::algorithm::grid_search_config config;
    config.moves = connectivity::four;

    report(measure("astar", size, "corner-to-corner", [&] {
        return dense_astar(map, corner, config);
    }));
    report(measure("bstar", size, "corner-to-corner", [&] {
        return dense_bstar(map, corner, config);
    }));
    report(measure("bfs", size, "corner-to-corner", [&] {
        return dense_bfs(map, corner, config);
    }));
    report(measure("bidirectional-bfs", size, "corner-to-corner", [&] {
        return dense_bidirectional_bfs(map, corner, config);
    }));
    report(measure("bfs", size, "same-row", [&] {
        return dense_bfs(map, same_row, config);
    }));
    report(measure("bidirectional-bfs", size, "same-row", [&] {
        return dense_bidirectional_bfs(map, same_row, config);
    }));
    report(measure("astar-priority-queue", size, "corner-to-corner", [&] {
        return astar_priority_queue(map, 0, corner);
    }));
}

} // namespace

void run_all_cases() {
    if (!crpc_bench::enabled("pathfind")) {
        return;
    }
    std::printf("%-20s %5s %-12s %10s %10s %9s %8s %10s %8s\n", "algorithm",
                "grid", "goal", "expanded", "generated", "median ms", "spread",
                "allocs", "MiB");
    run_size(512);
    run_size(1024);

    // Obstacle board: the open-board rows above let the Manhattan heuristic
    // collapse A* onto the optimal corridor. A 25%-obstacle field forces both
    // the forward and the backward frontier to cover real area, so this is the
    // row where B*'s second direction can pay for its overhead (or fail to).
    const grid_map maze = obstacle_board(512, 20261028, 25);
    const std::size_t maze_goal = maze.walkable.size() - 1;
    crpc::algorithm::grid_search_config maze_config;
    maze_config.moves = connectivity::four;
    report(measure("astar", 512, "25%-obstacles", [&] {
        return dense_astar(maze, maze_goal, maze_config);
    }));
    report(measure("bstar", 512, "25%-obstacles", [&] {
        return dense_bstar(maze, maze_goal, maze_config);
    }));
    report(measure("bfs", 512, "25%-obstacles", [&] {
        return dense_bfs(maze, maze_goal, maze_config);
    }));
    report(measure("bidirectional-bfs", 512, "25%-obstacles", [&] {
        return dense_bidirectional_bfs(maze, maze_goal, maze_config);
    }));

    // Visited-structure comparison. 512x512 keeps the generic hashed version
    // (two unordered_maps) in the seconds range instead of minutes.
    const grid_map map = open_board(512);
    const std::size_t goal = map.walkable.size() - 1;
    report(measure("generic-astar", 512, "corner-to-corner", [&] {
        return generic_astar(map, goal);
    }));
}

CRPC_BENCH_MAIN()