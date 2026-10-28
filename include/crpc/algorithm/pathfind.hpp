// CRPC -- dense grid pathfinding: A*, B*, BFS, bidirectional BFS, DFS, IDDFS.
//
// The generic `astar.hpp` works on any node type through std::unordered_map
// state storage. That is the right trade-off for an arbitrary state space, but
// it is the wrong one for a grid: a grid has dense integer node ids, and most
// of the cost of the generic version is hashing and pointer chasing. This
// header is the grid fast path. It stores every per-node value in a flat array
// indexed by `y * width + x` and the open set in a reserved binary heap, so a
// search does a handful of sequential array reads per node and no hashing.
//
// Contract shared by every algorithm here:
//
//   grid_map     walkable cells; a zero byte is a wall. `width * height` is the
//                node count. Start/goal must be in range and walkable;
//                otherwise the result is `rpc_code::not_found` (a wall is not a
//                path) rather than `invalid_argument`, matching astar_path.
//   cost         `double (const grid_map&, std::size_t from, std::size_t to)`;
//                non-negative step cost.
//   heuristic    `double (const grid_map&, std::size_t node, std::size_t goal)`;
//                an estimate of the remaining cost.
//   connectivity four or eight neighbours; diagonal moves never cut corners
//                through a wall because a diagonal target is only emitted when
//                the target cell itself is walkable (identical to the
//                neighbourhood used by the existing A* tests).
//
// Optimality conditions:
//
//   astar_grid      admissible heuristic (consistent is faster: the closed set
//                   is then never reopened). With an inconsistent but still
//                   admissible heuristic the node is reopened, matching the
//                   graph-search A* theorem used by astar.hpp.
//   bstar_grid      B* here is *bidirectional A* with an upper bound*: forward
//                   and backward A* run with the same cost and heuristic, and a
//                   candidate path cost `mu` (the best meeting found so far)
//                   prunes both queues: as soon as min(f_forward, f_backward)
//                   >= mu the search stops. This is the practical "two-sided"
//                   B* variant. It is NOT Berliner's interval B* (which keeps
//                   explicit lower/upper bounds per node and selects the most
//                   promising node by interval); readers looking for that
//                   algorithm should not mistake this for it. It requires a
//                   *consistent* heuristic in both directions (h(node,goal) and
//                   h(start,node)); with that condition the returned path is
//                   provably optimal, and the random-grid suite checks it
//                   against Dijkstra and against A*. With an inconsistent
//                   heuristic it still terminates but may be suboptimal.
//                   The graph must be undirected (the backward search expands
//                   the same neighbour relation); grids are. Note that with a
//                   strong heuristic A* already expands close to the optimal
//                   path length on an open board, so B* cannot beat it there:
//                   the benchmark reports B* at roughly twice A*'s time (two
//                   open sets and two g/parent arrays) with the same expansion
//                   count. B* is not a blanket replacement for A*; it is the
//                   two-sided variant with a proven upper-bound stop rule.
//   bfs_grid        unweighted, optimal number of moves.
//   bidirectional_bfs_grid
//                   unweighted, optimal number of moves; expands two balls of
//                   radius d/2 instead of one of radius d. Stops only once the
//                   expanded radii prove no shorter path can exist, so optimal.
//                   On a 2-D grid a ball of radius r holds O(r^2) cells, so the
//                   saving is a constant factor (about half the expanded area
//                   for same-row endpoints measured in the benchmark), not the
//                   O(b^(d/2)) of a tree, where the frontier grows
//                   exponentially. Two distance/parent sets also double the
//                   memory traffic; the benchmark reports the time trade-off
//                   rather than assuming the asymptotic tree win.
//   depth_first_grid
//                   complete on finite graphs (it visits every reachable node),
//                   NOT optimal.
//   iterative_deepening_grid
//                   complete and optimal in move count; repeated depth-limited
//                   DFS with a depth-first stack (O(d) frontier, versus O(n) for
//                   BFS) and a per-iteration best-depth table for pruning.
//
// Every search accepts `grid_search_config::max_expanded`. Exceeding it returns
// `rpc_code::resource_exhausted`; it is a caller-side guard, never required for
// termination. An unreachable goal is always `rpc_code::not_found`, never a
// hang.
#ifndef CRPC_ALGORITHM_PATHFIND_HPP
#define CRPC_ALGORITHM_PATHFIND_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include <crpc/error.hpp>

namespace crpc {
namespace algorithm {

enum class connectivity : std::uint8_t {
    four,
    eight
};

struct grid_map {
    std::size_t width = 0;
    std::size_t height = 0;
    // One byte per cell: 0 = wall, non-zero = walkable. Invariant:
    // walkable.size() == width * height (an empty board has size 0 and is
    // reported as not_found by every search here).
    std::vector<std::uint8_t> walkable;

    std::size_t size() const noexcept { return walkable.size(); }
    bool in_bounds(std::size_t x, std::size_t y) const noexcept {
        return x < width && y < height;
    }
    std::size_t index(std::size_t x, std::size_t y) const noexcept {
        return y * width + x;
    }
    std::size_t cell_x(std::size_t n) const noexcept { return n % width; }
    std::size_t cell_y(std::size_t n) const noexcept { return n / width; }
    bool passable(std::size_t n) const noexcept {
        return n < walkable.size() && walkable[n] != 0;
    }
};

// Counters for one search. `expanded` counts pops that were actually expanded
// (stale heap entries and already-closed duplicates are not counted).
struct grid_search_stats {
    std::size_t expanded = 0;
    std::size_t generated = 0;
    std::size_t reopened = 0;
    // IDDFS iterations (1 for a single-pass search).
    std::size_t restarts = 0;
    std::size_t max_frontier = 0;
};

struct grid_path {
    // Start first, goal last. Empty only on error.
    std::vector<std::size_t> nodes;
    grid_search_stats stats;
};

struct grid_search_config {
    connectivity moves = connectivity::four;
    // Zero = unlimited. Exceeding this budget returns resource_exhausted.
    std::size_t max_expanded = 0;
    // IDDFS only: zero = keep deepening until `size() - 1`.
    std::size_t max_depth = 0;
};

// Ready-made cost/heuristic helpers for the common grid metrics. Admissibility
// and consistency hold for the pairing named in the comment.
inline double grid_step_cost(const grid_map&, std::size_t, std::size_t) noexcept {
    return 1.0;
}

inline double grid_octile_step_cost(const grid_map& map, std::size_t from,
                                    std::size_t to) noexcept {
    return (map.cell_x(from) != map.cell_x(to) &&
            map.cell_y(from) != map.cell_y(to))
               ? 1.4142135623730951
               : 1.0;
}

inline double grid_manhattan_heuristic(const grid_map& map, std::size_t node,
                                       std::size_t goal) noexcept {
    const std::size_t dx = map.cell_x(node) > map.cell_x(goal)
                               ? map.cell_x(node) - map.cell_x(goal)
                               : map.cell_x(goal) - map.cell_x(node);
    const std::size_t dy = map.cell_y(node) > map.cell_y(goal)
                               ? map.cell_y(node) - map.cell_y(goal)
                               : map.cell_y(goal) - map.cell_y(node);
    return static_cast<double>(dx + dy);
}

inline double grid_euclidean_heuristic(const grid_map& map, std::size_t node,
                                       std::size_t goal) noexcept {
    const std::size_t ax = map.cell_x(node);
    const std::size_t ay = map.cell_y(node);
    const std::size_t bx = map.cell_x(goal);
    const std::size_t by = map.cell_y(goal);
    const double dx = static_cast<double>(ax > bx ? ax - bx : bx - ax);
    const double dy = static_cast<double>(ay > by ? ay - by : by - ay);
    return std::sqrt(dx * dx + dy * dy);
}

inline double grid_chebyshev_heuristic(const grid_map& map, std::size_t node,
                                       std::size_t goal) noexcept {
    const std::size_t dx = map.cell_x(node) > map.cell_x(goal)
                               ? map.cell_x(node) - map.cell_x(goal)
                               : map.cell_x(goal) - map.cell_x(node);
    const std::size_t dy = map.cell_y(node) > map.cell_y(goal)
                               ? map.cell_y(node) - map.cell_y(goal)
                               : map.cell_y(goal) - map.cell_y(node);
    return static_cast<double>(dx > dy ? dx : dy);
}

inline double grid_octile_heuristic(const grid_map& map, std::size_t node,
                                    std::size_t goal) noexcept {
    const double dx = static_cast<double>(
        map.cell_x(node) > map.cell_x(goal) ? map.cell_x(node) - map.cell_x(goal)
                                            : map.cell_x(goal) - map.cell_x(node));
    const double dy = static_cast<double>(
        map.cell_y(node) > map.cell_y(goal) ? map.cell_y(node) - map.cell_y(goal)
                                            : map.cell_y(goal) - map.cell_y(node));
    const double major = dx > dy ? dx : dy;
    const double minor = dx > dy ? dy : dx;
    return major + 0.4142135623730951 * minor;
}

namespace detail {

constexpr std::size_t grid_none = static_cast<std::size_t>(-1);
constexpr double grid_inf = std::numeric_limits<double>::infinity();

inline bool grid_endpoints_usable(const grid_map& map, std::size_t start,
                                  std::size_t goal) noexcept {
    return start < map.walkable.size() && goal < map.walkable.size() &&
           map.walkable[start] != 0 && map.walkable[goal] != 0;
}

inline bool grid_budget_exhausted(const grid_search_config& config,
                                  const grid_search_stats& stats) noexcept {
    return config.max_expanded != 0 && stats.expanded >= config.max_expanded;
}

template <typename Visit>
inline void visit_grid_neighbors(const grid_map& map, std::size_t node,
                                 connectivity moves, Visit& visit) {
    const std::size_t x = map.cell_x(node);
    const std::size_t y = map.cell_y(node);
    const std::size_t west = node - 1;
    const std::size_t east = node + 1;
    const std::size_t north = node - map.width;
    const std::size_t south = node + map.width;
    // Cardinal neighbours are written out rather than looped over a direction
    // table: this is the inner loop of every search here, and the explicit form
    // removes an indexed load and a bounds comparison per neighbour.
    if (x > 0 && map.walkable[west] != 0) {
        visit(west);
    }
    if (x + 1 < map.width && map.walkable[east] != 0) {
        visit(east);
    }
    if (y > 0 && map.walkable[north] != 0) {
        visit(north);
    }
    if (y + 1 < map.height && map.walkable[south] != 0) {
        visit(south);
    }
    if (moves == connectivity::eight) {
        if (x > 0 && y > 0 && map.walkable[north - 1] != 0) {
            visit(north - 1);
        }
        if (x + 1 < map.width && y > 0 && map.walkable[north + 1] != 0) {
            visit(north + 1);
        }
        if (x > 0 && y + 1 < map.height && map.walkable[south - 1] != 0) {
            visit(south - 1);
        }
        if (x + 1 < map.width && y + 1 < map.height &&
            map.walkable[south + 1] != 0) {
            visit(south + 1);
        }
    }
}

struct grid_heap_entry {
    double f = 0.0;
    double g = 0.0;
    std::size_t node = grid_none;
};

// Lower f wins; on equal f the entry with the larger g is preferred (it is
// closer to the goal, so fewer nodes are expanded on open grids); node id is the
// final tie-break, which keeps runs deterministic without requiring a custom
// node ordering.
inline bool grid_heap_better(const grid_heap_entry& a,
                             const grid_heap_entry& b) noexcept {
    if (a.f != b.f) {
        return a.f < b.f;
    }
    if (a.g != b.g) {
        return a.g > b.g;
    }
    return a.node < b.node;
}

// A binary min-heap over a reserved vector. std::priority_queue does not expose
// reserve() and cannot be extended with a decrease-key, so the searches use this
// small explicit heap with lazy deletion instead. Benchmarks in
// bench/bench_pathfind.cpp compare it with a std::priority_queue version.
class grid_heap {
public:
    void reserve(std::size_t capacity) { entries_.reserve(capacity); }
    bool empty() const noexcept { return entries_.empty(); }
    std::size_t size() const noexcept { return entries_.size(); }
    const grid_heap_entry& top() const noexcept { return entries_.front(); }

    void push(const grid_heap_entry& entry) {
        entries_.push_back(entry);
        sift_up(entries_.size() - 1);
    }

    void pop() {
        entries_.front() = entries_.back();
        entries_.pop_back();
        if (!entries_.empty()) {
            sift_down(0);
        }
    }

private:
    void sift_up(std::size_t index) {
        while (index != 0) {
            const std::size_t parent = (index - 1) / 2;
            if (!grid_heap_better(entries_[index], entries_[parent])) {
                break;
            }
            std::swap(entries_[index], entries_[parent]);
            index = parent;
        }
    }

    void sift_down(std::size_t index) {
        const std::size_t count = entries_.size();
        for (;;) {
            const std::size_t left = index * 2 + 1;
            if (left >= count) {
                break;
            }
            const std::size_t right = left + 1;
            std::size_t best = left;
            if (right < count &&
                grid_heap_better(entries_[right], entries_[left])) {
                best = right;
            }
            if (!grid_heap_better(entries_[best], entries_[index])) {
                break;
            }
            std::swap(entries_[best], entries_[index]);
            index = best;
        }
    }

    std::vector<grid_heap_entry> entries_;
};

inline std::vector<std::size_t>
grid_unwind_forward(const std::vector<std::size_t>& parent,
                    std::size_t start, std::size_t goal) {
    std::vector<std::size_t> path;
    std::size_t node = goal;
    while (node != start) {
        path.push_back(node);
        node = parent[node];
    }
    path.push_back(start);
    std::reverse(path.begin(), path.end());
    return path;
}

inline std::vector<std::size_t>
grid_unwind_backward(const std::vector<std::size_t>& parent,
                     std::size_t goal, std::size_t meeting) {
    std::vector<std::size_t> path;
    std::size_t node = meeting;
    while (node != goal) {
        path.push_back(node);
        node = parent[node];
    }
    path.push_back(goal);
    return path;
}

template <typename Cost, typename Heuristic>
result<grid_path> astar_grid_impl(const grid_map& map, std::size_t start,
                                  std::size_t goal, Cost cost,
                                  Heuristic heuristic,
                                  const grid_search_config& config) {
    grid_path out;
    if (!grid_endpoints_usable(map, start, goal)) {
        return rpc_error{rpc_code::not_found,
                         "astar_grid: start or goal is blocked or out of range"};
    }
    if (start == goal) {
        out.nodes.push_back(start);
        return out;
    }

    const std::size_t count = map.walkable.size();
    std::vector<double> g(count, grid_inf);
    std::vector<std::size_t> parent(count, grid_none);
    std::vector<std::uint8_t> closed(count, 0);
    grid_heap open;
    open.reserve(count < 4096 ? count : 4096);

    g[start] = 0.0;
    parent[start] = grid_none;
    open.push({static_cast<double>(heuristic(map, start, goal)), 0.0, start});
    out.stats.generated = 1;

    while (!open.empty()) {
        const grid_heap_entry entry = open.top();
        open.pop();
        if (closed[entry.node] != 0 || entry.g != g[entry.node]) {
            continue;
        }
        if (entry.node == goal) {
            out.nodes = grid_unwind_forward(parent, start, goal);
            return out;
        }
        if (grid_budget_exhausted(config, out.stats)) {
            return rpc_error{rpc_code::resource_exhausted,
                             "astar_grid: node expansion budget exceeded"};
        }
        closed[entry.node] = 1;
        ++out.stats.expanded;

        auto relax = [&](std::size_t next) {
            const double tentative =
                entry.g + static_cast<double>(cost(map, entry.node, next));
            if (!(tentative < g[next])) {
                return;
            }
            g[next] = tentative;
            parent[next] = entry.node;
            if (closed[next] != 0) {
                // Only possible with an inconsistent (but still admissible)
                // heuristic. Reopening preserves graph-search A* optimality.
                closed[next] = 0;
                ++out.stats.reopened;
            }
            open.push({tentative +
                           static_cast<double>(heuristic(map, next, goal)),
                       tentative, next});
            ++out.stats.generated;
        };
        visit_grid_neighbors(map, entry.node, config.moves, relax);
        out.stats.max_frontier =
            std::max(out.stats.max_frontier, open.size());
    }

    return rpc_error{rpc_code::not_found,
                     "astar_grid: no path from start to goal"};
}

inline result<grid_path> bfs_grid_impl(const grid_map& map, std::size_t start,
                                       std::size_t goal,
                                       const grid_search_config& config) {
    grid_path out;
    if (!grid_endpoints_usable(map, start, goal)) {
        return rpc_error{rpc_code::not_found,
                         "bfs_grid: start or goal is blocked or out of range"};
    }
    if (start == goal) {
        out.nodes.push_back(start);
        return out;
    }

    const std::size_t count = map.walkable.size();
    std::vector<std::size_t> parent(count, grid_none);
    std::vector<std::uint8_t> visited(count, 0);
    std::vector<std::size_t> queue;
    queue.reserve(count);
    queue.push_back(start);
    visited[start] = 1;
    out.stats.generated = 1;

    for (std::size_t head = 0; head < queue.size(); ++head) {
        const std::size_t node = queue[head];
        if (node == goal) {
            out.nodes = grid_unwind_forward(parent, start, goal);
            return out;
        }
        if (grid_budget_exhausted(config, out.stats)) {
            return rpc_error{rpc_code::resource_exhausted,
                             "bfs_grid: node expansion budget exceeded"};
        }
        ++out.stats.expanded;
        auto relax = [&](std::size_t next) {
            if (visited[next] != 0) {
                return;
            }
            visited[next] = 1;
            parent[next] = node;
            queue.push_back(next);
            ++out.stats.generated;
        };
        visit_grid_neighbors(map, node, config.moves, relax);
        out.stats.max_frontier =
            std::max(out.stats.max_frontier, queue.size() - head - 1);
    }

    return rpc_error{rpc_code::not_found,
                     "bfs_grid: no path from start to goal"};
}

inline result<grid_path>
bidirectional_bfs_grid_impl(const grid_map& map, std::size_t start,
                            std::size_t goal,
                            const grid_search_config& config) {
    grid_path out;
    if (!grid_endpoints_usable(map, start, goal)) {
        return rpc_error{
            rpc_code::not_found,
            "bidirectional_bfs_grid: start or goal is blocked or out of range"};
    }
    if (start == goal) {
        out.nodes.push_back(start);
        return out;
    }

    const std::size_t count = map.walkable.size();
    std::vector<std::size_t> dist_forward(count, grid_none);
    std::vector<std::size_t> dist_backward(count, grid_none);
    std::vector<std::size_t> parent_forward(count, grid_none);
    std::vector<std::size_t> parent_backward(count, grid_none);
    std::vector<std::size_t> frontier_forward{start};
    std::vector<std::size_t> frontier_backward{goal};
    std::vector<std::size_t> next_forward;
    std::vector<std::size_t> next_backward;
    dist_forward[start] = 0;
    dist_backward[goal] = 0;
    out.stats.generated = 2;

    // Number of layers fully expanded on each side. After `depth_forward`
    // forward layers, every node within that radius is known.
    std::size_t depth_forward = 0;
    std::size_t depth_backward = 0;
    std::size_t best = grid_none;
    std::size_t meeting = grid_none;

    while (!frontier_forward.empty() && !frontier_backward.empty()) {
        // If the two expanded balls cover every path shorter than `best`, no
        // improvement is possible. Coverage of length L needs
        // (depth_forward - 1) + (depth_backward - 1) >= L, so stopping at
        // depth_forward + depth_backward >= best + 1 is safe.
        if (best != grid_none && depth_forward + depth_backward >= best + 1) {
            break;
        }

        const bool expand_forward =
            frontier_forward.size() <= frontier_backward.size();
        const std::vector<std::size_t>& frontier =
            expand_forward ? frontier_forward : frontier_backward;
        const std::vector<std::size_t>& dist_this =
            expand_forward ? dist_forward : dist_backward;
        const std::vector<std::size_t>& dist_other =
            expand_forward ? dist_backward : dist_forward;
        std::vector<std::size_t>& dist_write =
            expand_forward ? dist_forward : dist_backward;
        std::vector<std::size_t>& parent_write =
            expand_forward ? parent_forward : parent_backward;
        std::vector<std::size_t>& next =
            expand_forward ? next_forward : next_backward;
        next.clear();

        for (const std::size_t node : frontier) {
            if (grid_budget_exhausted(config, out.stats)) {
                return rpc_error{rpc_code::resource_exhausted,
                                 "bidirectional_bfs_grid: expansion budget "
                                 "exceeded"};
            }
            ++out.stats.expanded;
            auto relax = [&](std::size_t neighbor) {
                if (dist_this[neighbor] == grid_none) {
                    dist_write[neighbor] = dist_this[node] + 1;
                    parent_write[neighbor] = node;
                    next.push_back(neighbor);
                    ++out.stats.generated;
                }
                if (dist_other[neighbor] != grid_none) {
                    const std::size_t candidate =
                        dist_this[neighbor] + dist_other[neighbor];
                    if (candidate < best) {
                        best = candidate;
                        meeting = neighbor;
                    }
                }
            };
            visit_grid_neighbors(map, node, config.moves, relax);
        }

        if (expand_forward) {
            frontier_forward.swap(next);
            ++depth_forward;
        } else {
            frontier_backward.swap(next);
            ++depth_backward;
        }
        const std::size_t active =
            (expand_forward ? frontier_forward.size() : frontier_backward.size()) +
            (expand_forward ? frontier_backward.size() : frontier_forward.size());
        out.stats.max_frontier = std::max(out.stats.max_frontier, active);
    }

    if (meeting == grid_none) {
        return rpc_error{rpc_code::not_found,
                         "bidirectional_bfs_grid: no path from start to goal"};
    }

    out.nodes = grid_unwind_forward(parent_forward, start, meeting);
    std::size_t node = parent_backward[meeting];
    if (meeting != goal) {
        for (;;) {
            out.nodes.push_back(node);
            if (node == goal) {
                break;
            }
            node = parent_backward[node];
        }
    }
    return out;
}

inline result<grid_path> depth_first_grid_impl(
    const grid_map& map, std::size_t start, std::size_t goal,
    const grid_search_config& config) {
    grid_path out;
    if (!grid_endpoints_usable(map, start, goal)) {
        return rpc_error{
            rpc_code::not_found,
            "depth_first_grid: start or goal is blocked or out of range"};
    }
    if (start == goal) {
        out.nodes.push_back(start);
        return out;
    }

    const std::size_t count = map.walkable.size();
    std::vector<std::size_t> parent(count, grid_none);
    std::vector<std::uint8_t> visited(count, 0);
    std::vector<std::size_t> stack;
    stack.reserve(count);
    stack.push_back(start);
    visited[start] = 1;
    out.stats.generated = 1;

    while (!stack.empty()) {
        const std::size_t node = stack.back();
        stack.pop_back();
        if (node == goal) {
            out.nodes = grid_unwind_forward(parent, start, goal);
            return out;
        }
        if (grid_budget_exhausted(config, out.stats)) {
            return rpc_error{rpc_code::resource_exhausted,
                             "depth_first_grid: node expansion budget exceeded"};
        }
        ++out.stats.expanded;
        auto relax = [&](std::size_t next) {
            if (visited[next] != 0) {
                return;
            }
            visited[next] = 1;
            parent[next] = node;
            stack.push_back(next);
            ++out.stats.generated;
        };
        visit_grid_neighbors(map, node, config.moves, relax);
        out.stats.max_frontier = std::max(out.stats.max_frontier, stack.size());
    }

    return rpc_error{rpc_code::not_found,
                     "depth_first_grid: no path from start to goal"};
}

inline result<grid_path> iterative_deepening_grid_impl(
    const grid_map& map, std::size_t start, std::size_t goal,
    const grid_search_config& config) {
    grid_path out;
    if (!grid_endpoints_usable(map, start, goal)) {
        return rpc_error{rpc_code::not_found,
                         "iterative_deepening_grid: start or goal is blocked "
                         "or out of range"};
    }
    if (start == goal) {
        out.nodes.push_back(start);
        return out;
    }

    const std::size_t count = map.walkable.size();
    const std::size_t limit_cap =
        config.max_depth != 0 ? config.max_depth : count - 1;
    std::vector<std::size_t> best_depth;
    std::vector<std::size_t> parent;

    for (std::size_t limit = 0; limit <= limit_cap; ++limit) {
        ++out.stats.restarts;
        best_depth.assign(count, grid_none);
        parent.assign(count, grid_none);
        std::vector<std::size_t> stack_node;
        std::vector<std::size_t> stack_depth;
        stack_node.push_back(start);
        stack_depth.push_back(0);
        best_depth[start] = 0;

        while (!stack_node.empty()) {
            const std::size_t node = stack_node.back();
            const std::size_t depth = stack_depth.back();
            stack_node.pop_back();
            stack_depth.pop_back();
            if (node == goal) {
                out.nodes = grid_unwind_forward(parent, start, goal);
                return out;
            }
            if (depth >= limit) {
                continue;
            }
            if (grid_budget_exhausted(config, out.stats)) {
                return rpc_error{rpc_code::resource_exhausted,
                                 "iterative_deepening_grid: expansion budget "
                                 "exceeded"};
            }
            ++out.stats.expanded;
            auto relax = [&](std::size_t next) {
                const std::size_t next_depth = depth + 1;
                // Re-expanding a node reached at a smaller depth is required
                // for completeness: the earlier visit may have run out of
                // remaining depth budget.
                if (next_depth < best_depth[next]) {
                    best_depth[next] = next_depth;
                    parent[next] = node;
                    stack_node.push_back(next);
                    stack_depth.push_back(next_depth);
                    ++out.stats.generated;
                }
            };
            visit_grid_neighbors(map, node, config.moves, relax);
            out.stats.max_frontier =
                std::max(out.stats.max_frontier, stack_node.size());
        }
    }

    return rpc_error{rpc_code::not_found,
                     "iterative_deepening_grid: no path from start to goal"};
}

template <typename Cost, typename Heuristic>
result<grid_path> bstar_grid_impl(const grid_map& map, std::size_t start,
                                  std::size_t goal, Cost cost,
                                  Heuristic heuristic,
                                  const grid_search_config& config) {
    grid_path out;
    if (!grid_endpoints_usable(map, start, goal)) {
        return rpc_error{rpc_code::not_found,
                         "bstar_grid: start or goal is blocked or out of range"};
    }
    if (start == goal) {
        out.nodes.push_back(start);
        return out;
    }

    const std::size_t count = map.walkable.size();
    std::vector<double> g_forward(count, grid_inf);
    std::vector<double> g_backward(count, grid_inf);
    std::vector<std::size_t> parent_forward(count, grid_none);
    std::vector<std::size_t> parent_backward(count, grid_none);
    std::vector<std::uint8_t> closed_forward(count, 0);
    std::vector<std::uint8_t> closed_backward(count, 0);
    grid_heap open_forward;
    grid_heap open_backward;
    open_forward.reserve(count < 4096 ? count : 4096);
    open_backward.reserve(count < 4096 ? count : 4096);

    g_forward[start] = 0.0;
    open_forward.push(
        {static_cast<double>(heuristic(map, start, goal)), 0.0, start});
    g_backward[goal] = 0.0;
    open_backward.push(
        {static_cast<double>(heuristic(map, start, goal)), 0.0, goal});
    out.stats.generated = 2;

    double best = grid_inf;
    std::size_t meeting = grid_none;

    auto clean = [](grid_heap& open, const std::vector<double>& g,
                    const std::vector<std::uint8_t>& closed) {
        for (;;) {
            if (open.empty()) {
                return;
            }
            const grid_heap_entry& entry = open.top();
            if (closed[entry.node] != 0 || entry.g != g[entry.node]) {
                open.pop();
                continue;
            }
            return;
        }
    };

    for (;;) {
        clean(open_forward, g_forward, closed_forward);
        clean(open_backward, g_backward, closed_backward);
        if (open_forward.empty() || open_backward.empty()) {
            break;
        }
        // Stop when neither side can still improve the incumbent. With a
        // consistent heuristic the minimum f in a non-exhausted direction is a
        // lower bound on any path that side could still discover, so
        // min(f_forward, f_backward) >= best proves `best` optimal.
        if (best != grid_inf &&
            (open_forward.top().f >= best || open_backward.top().f >= best)) {
            break;
        }

        const bool expand_forward =
            open_forward.top().f <= open_backward.top().f;
        if (expand_forward) {
            const grid_heap_entry entry = open_forward.top();
            open_forward.pop();
            closed_forward[entry.node] = 1;
            if (g_backward[entry.node] != grid_inf &&
                entry.g + g_backward[entry.node] < best) {
                best = entry.g + g_backward[entry.node];
                meeting = entry.node;
            }
            if (grid_budget_exhausted(config, out.stats)) {
                return rpc_error{rpc_code::resource_exhausted,
                                 "bstar_grid: node expansion budget exceeded"};
            }
            ++out.stats.expanded;
            auto relax = [&](std::size_t next) {
                if (closed_forward[next] != 0) {
                    return; // consistent heuristic: settled exactly once
                }
                const double tentative =
                    entry.g + static_cast<double>(cost(map, entry.node, next));
                if (!(tentative < g_forward[next])) {
                    return;
                }
                g_forward[next] = tentative;
                parent_forward[next] = entry.node;
                open_forward.push(
                    {tentative +
                         static_cast<double>(heuristic(map, next, goal)),
                     tentative, next});
                ++out.stats.generated;
            };
            visit_grid_neighbors(map, entry.node, config.moves, relax);
            out.stats.max_frontier = std::max(
                out.stats.max_frontier,
                open_forward.size() + open_backward.size());
        } else {
            const grid_heap_entry entry = open_backward.top();
            open_backward.pop();
            closed_backward[entry.node] = 1;
            if (g_forward[entry.node] != grid_inf &&
                entry.g + g_forward[entry.node] < best) {
                best = entry.g + g_forward[entry.node];
                meeting = entry.node;
            }
            if (grid_budget_exhausted(config, out.stats)) {
                return rpc_error{rpc_code::resource_exhausted,
                                 "bstar_grid: node expansion budget exceeded"};
            }
            ++out.stats.expanded;
            auto relax = [&](std::size_t next) {
                if (closed_backward[next] != 0) {
                    return;
                }
                // The grid graph is undirected, so the backward step cost is
                // the same as the forward edge cost.
                const double tentative =
                    entry.g + static_cast<double>(cost(map, next, entry.node));
                if (!(tentative < g_backward[next])) {
                    return;
                }
                g_backward[next] = tentative;
                parent_backward[next] = entry.node;
                open_backward.push(
                    {tentative +
                         static_cast<double>(heuristic(map, start, next)),
                     tentative, next});
                ++out.stats.generated;
            };
            visit_grid_neighbors(map, entry.node, config.moves, relax);
            out.stats.max_frontier = std::max(
                out.stats.max_frontier,
                open_forward.size() + open_backward.size());
        }
    }

    if (meeting == grid_none) {
        return rpc_error{rpc_code::not_found,
                         "bstar_grid: no path from start to goal"};
    }

    out.nodes = grid_unwind_forward(parent_forward, start, meeting);
    std::size_t node = parent_backward[meeting];
    if (meeting != goal) {
        for (;;) {
            out.nodes.push_back(node);
            if (node == goal) {
                break;
            }
            node = parent_backward[node];
        }
    }
    return out;
}

} // namespace detail

// A* over a dense grid. See the file comment for optimality conditions.
template <typename Cost, typename Heuristic>
[[nodiscard]] result<grid_path>
astar_grid(const grid_map& map, std::size_t start, std::size_t goal, Cost cost,
           Heuristic heuristic, const grid_search_config& config = {}) {
    return detail::astar_grid_impl(map, start, goal, cost, heuristic, config);
}

// B*: bidirectional A* with a best-path upper bound. Requires a consistent
// heuristic and an undirected graph; optimal under those conditions. This is
// not Berliner's interval B* (see the file comment).
template <typename Cost, typename Heuristic>
[[nodiscard]] result<grid_path>
bstar_grid(const grid_map& map, std::size_t start, std::size_t goal, Cost cost,
           Heuristic heuristic, const grid_search_config& config = {}) {
    return detail::bstar_grid_impl(map, start, goal, cost, heuristic, config);
}

// Breadth-first search: minimum number of moves on an unweighted grid.
[[nodiscard]] inline result<grid_path>
bfs_grid(const grid_map& map, std::size_t start, std::size_t goal,
         const grid_search_config& config = {}) {
    return detail::bfs_grid_impl(map, start, goal, config);
}

// Bidirectional breadth-first search: same minimum move count, fewer expanded
// nodes because both sides grow a ball of radius d/2.
[[nodiscard]] inline result<grid_path>
bidirectional_bfs_grid(const grid_map& map, std::size_t start,
                       std::size_t goal,
                       const grid_search_config& config = {}) {
    return detail::bidirectional_bfs_grid_impl(map, start, goal, config);
}

// Depth-first search: complete, not optimal (returns the first path found).
[[nodiscard]] inline result<grid_path>
depth_first_grid(const grid_map& map, std::size_t start, std::size_t goal,
                 const grid_search_config& config = {}) {
    return detail::depth_first_grid_impl(map, start, goal, config);
}

// Iterative-deepening DFS: optimal move count, depth-first memory profile.
[[nodiscard]] inline result<grid_path>
iterative_deepening_grid(const grid_map& map, std::size_t start,
                         std::size_t goal,
                         const grid_search_config& config = {}) {
    return detail::iterative_deepening_grid_impl(map, start, goal, config);
}

} // namespace algorithm
} // namespace crpc

#endif // CRPC_ALGORITHM_PATHFIND_HPP
