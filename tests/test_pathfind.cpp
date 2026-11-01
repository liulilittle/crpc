// CRPC -- tests for crpc/algorithm/pathfind.hpp: the dense grid search family.
//
// Every optimal algorithm (A*, B*, BFS, bidirectional BFS, IDDFS) is compared
// against an independent reference on random obstacle grids over many seeds.
// DFS is checked for completeness and path validity (it is deliberately not
// optimal). Edge cases and the not_found contract are covered explicitly.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <queue>
#include <string>
#include <utility>
#include <vector>

#include <crpc/algorithm/pathfind.hpp>

#include "test_framework.hpp"

namespace {

using crpc::algorithm::connectivity;
using crpc::algorithm::grid_map;
using crpc::algorithm::grid_search_config;

constexpr std::size_t k_none = static_cast<std::size_t>(-1);
constexpr double k_inf = std::numeric_limits<double>::infinity();
constexpr double k_sqrt2 = 1.4142135623730951;

std::uint64_t xorshift64(std::uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

grid_map make_grid(std::size_t width, std::size_t height, std::uint64_t seed,
                   std::size_t density_pct) {
    grid_map g;
    g.width = width;
    g.height = height;
    g.walkable.assign(width * height, 1);
    std::uint64_t state = seed;
    for (std::size_t i = 0; i < g.walkable.size(); ++i) {
        if (xorshift64(state) % 100 < density_pct) {
            g.walkable[i] = 0;
        }
    }
    if (!g.walkable.empty()) {
        g.walkable.front() = 1;
        g.walkable.back() = 1;
    }
    return g;
}

grid_map open_grid(std::size_t width, std::size_t height) {
    grid_map g;
    g.width = width;
    g.height = height;
    g.walkable.assign(width * height, 1);
    return g;
}

template <typename Visit>
void for_each_neighbor(const grid_map& g, std::size_t n, connectivity moves,
                       Visit&& visit) {
    const std::size_t x = n % g.width;
    const std::size_t y = n / g.width;
    const auto try_cell = [&](std::size_t nx, std::size_t ny) {
        if (nx < g.width && ny < g.height) {
            const std::size_t m = ny * g.width + nx;
            if (g.walkable[m] != 0) {
                visit(m);
            }
        }
    };
    if (x > 0) {
        try_cell(x - 1, y);
    }
    if (x + 1 < g.width) {
        try_cell(x + 1, y);
    }
    if (y > 0) {
        try_cell(x, y - 1);
    }
    if (y + 1 < g.height) {
        try_cell(x, y + 1);
    }
    if (moves == connectivity::eight) {
        if (x > 0 && y > 0) {
            try_cell(x - 1, y - 1);
        }
        if (x + 1 < g.width && y > 0) {
            try_cell(x + 1, y - 1);
        }
        if (x > 0 && y + 1 < g.height) {
            try_cell(x - 1, y + 1);
        }
        if (x + 1 < g.width && y + 1 < g.height) {
            try_cell(x + 1, y + 1);
        }
    }
}

double step_unit(const grid_map&, std::size_t, std::size_t) { return 1.0; }

double step_octile(const grid_map& g, std::size_t a, std::size_t b) {
    const std::size_t ax = a % g.width;
    const std::size_t ay = a / g.width;
    const std::size_t bx = b % g.width;
    const std::size_t by = b / g.width;
    const std::size_t dx = ax > bx ? ax - bx : bx - ax;
    const std::size_t dy = ay > by ? ay - by : by - ay;
    return (dx != 0 && dy != 0) ? k_sqrt2 : 1.0;
}

double h_manhattan(const grid_map& g, std::size_t a, std::size_t b) {
    const std::size_t ax = a % g.width;
    const std::size_t ay = a / g.width;
    const std::size_t bx = b % g.width;
    const std::size_t by = b / g.width;
    const std::size_t dx = ax > bx ? ax - bx : bx - ax;
    const std::size_t dy = ay > by ? ay - by : by - ay;
    return static_cast<double>(dx + dy);
}

double h_euclidean(const grid_map& g, std::size_t a, std::size_t b) {
    const std::size_t ax = a % g.width;
    const std::size_t ay = a / g.width;
    const std::size_t bx = b % g.width;
    const std::size_t by = b / g.width;
    const double dx = static_cast<double>(ax > bx ? ax - bx : bx - ax);
    const double dy = static_cast<double>(ay > by ? ay - by : by - ay);
    return std::sqrt(dx * dx + dy * dy);
}

double h_chebyshev(const grid_map& g, std::size_t a, std::size_t b) {
    const std::size_t ax = a % g.width;
    const std::size_t ay = a / g.width;
    const std::size_t bx = b % g.width;
    const std::size_t by = b / g.width;
    const std::size_t dx = ax > bx ? ax - bx : bx - ax;
    const std::size_t dy = ay > by ? ay - by : by - ay;
    return static_cast<double>(dx > dy ? dx : dy);
}

double h_octile(const grid_map& g, std::size_t a, std::size_t b) {
    const std::size_t ax = a % g.width;
    const std::size_t ay = a / g.width;
    const std::size_t bx = b % g.width;
    const std::size_t by = b / g.width;
    const double dx = static_cast<double>(ax > bx ? ax - bx : bx - ax);
    const double dy = static_cast<double>(ay > by ? ay - by : by - ay);
    const double major = dx > dy ? dx : dy;
    const double minor = dx > dy ? dy : dx;
    return major + (k_sqrt2 - 1.0) * minor;
}

using heuristic_fn = double (*)(const grid_map&, std::size_t, std::size_t);

heuristic_fn pick_heuristic(int kind) {
    switch (kind) {
    case 0:
        return h_manhattan;
    case 1:
        return h_euclidean;
    case 2:
        return h_chebyshev;
    default:
        return h_octile;
    }
}

grid_search_config config_for(connectivity moves) {
    grid_search_config config;
    config.moves = moves;
    return config;
}

bool close_to(double a, double b) {
    return std::fabs(a - b) <=
           1e-9 * std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
}

template <typename Cost>
double dijkstra_cost(const grid_map& g, std::size_t start, std::size_t goal,
                     connectivity moves, Cost cost) {
    using entry = std::pair<double, std::size_t>;
    std::priority_queue<entry, std::vector<entry>, std::greater<entry>> open;
    std::vector<double> best(g.walkable.size(), k_inf);
    best[start] = 0.0;
    open.push({0.0, start});
    while (!open.empty()) {
        const double d = open.top().first;
        const std::size_t n = open.top().second;
        open.pop();
        if (n == goal) {
            return d;
        }
        if (d != best[n]) {
            continue;
        }
        for_each_neighbor(g, n, moves, [&](std::size_t m) {
            const double nd = d + cost(g, n, m);
            if (nd < best[m]) {
                best[m] = nd;
                open.push({nd, m});
            }
        });
    }
    return k_inf;
}

std::size_t bfs_hops(const grid_map& g, std::size_t start, std::size_t goal,
                     connectivity moves) {
    std::vector<std::size_t> dist(g.walkable.size(), k_none);
    std::vector<std::size_t> queue;
    queue.reserve(g.walkable.size());
    dist[start] = 0;
    queue.push_back(start);
    for (std::size_t head = 0; head < queue.size(); ++head) {
        const std::size_t n = queue[head];
        if (n == goal) {
            return dist[n];
        }
        for_each_neighbor(g, n, moves, [&](std::size_t m) {
            if (dist[m] == k_none) {
                dist[m] = dist[n] + 1;
                queue.push_back(m);
            }
        });
    }
    return k_none;
}

bool is_neighbor(const grid_map& g, std::size_t a, std::size_t b,
                 connectivity moves) {
    if (a >= g.walkable.size() || b >= g.walkable.size()) {
        return false;
    }
    if (g.walkable[a] == 0 || g.walkable[b] == 0) {
        return false;
    }
    const std::size_t ax = a % g.width;
    const std::size_t ay = a / g.width;
    const std::size_t bx = b % g.width;
    const std::size_t by = b / g.width;
    const std::size_t dx = ax > bx ? ax - bx : bx - ax;
    const std::size_t dy = ay > by ? ay - by : by - ay;
    if (dx > 1 || dy > 1 || (dx == 0 && dy == 0)) {
        return false;
    }
    if (moves == connectivity::four && dx != 0 && dy != 0) {
        return false;
    }
    return true;
}

void check_path(const grid_map& g, connectivity moves,
                const std::vector<std::size_t>& path, std::size_t start,
                std::size_t goal) {
    CRPC_CHECK(!path.empty());
    if (path.empty()) {
        return;
    }
    CRPC_CHECK_EQ(start, path.front());
    CRPC_CHECK_EQ(goal, path.back());
    for (std::size_t i = 1; i < path.size(); ++i) {
        if (!is_neighbor(g, path[i - 1], path[i], moves)) {
            CRPC_CHECK(false);
            return;
        }
    }
}

template <typename Cost>
double path_cost(const grid_map& g, const std::vector<std::size_t>& path,
                 Cost cost) {
    double total = 0.0;
    for (std::size_t i = 1; i < path.size(); ++i) {
        total += cost(g, path[i - 1], path[i]);
    }
    return total;
}

template <typename T>
void expect_not_found(const crpc::result<T>& r) {
    CRPC_CHECK_FALSE(r.ok());
    if (!r.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                      static_cast<int>(r.code()));
    }
}

// --- Optimality sweeps --------------------------------------------------------

void astar_sweep(connectivity moves, int heuristic_kind, std::size_t seeds) {
    constexpr std::size_t k_size = 20;
    const auto cost = moves == connectivity::eight ? step_octile : step_unit;
    const heuristic_fn h = pick_heuristic(heuristic_kind);
    const grid_search_config config = config_for(moves);
    for (std::size_t seed = 1; seed <= seeds; ++seed) {
        const grid_map g = make_grid(k_size, k_size, seed, 25);
        const std::size_t start = 0;
        const std::size_t goal = g.walkable.size() - 1;
        const double reference = dijkstra_cost(g, start, goal, moves, cost);
        const auto found =
            crpc::algorithm::astar_grid(g, start, goal, cost, h, config);
        if (std::isinf(reference)) {
            expect_not_found(found);
            continue;
        }
        CRPC_CHECK(found.ok());
        if (!found.ok()) {
            continue;
        }
        check_path(g, moves, found.value().nodes, start, goal);
        CRPC_CHECK(close_to(path_cost(g, found.value().nodes, cost), reference));
        CRPC_CHECK(found.value().stats.expanded > 0);
    }
}

void bstar_sweep(connectivity moves, heuristic_fn h, std::size_t seeds) {
    constexpr std::size_t k_size = 20;
    const auto cost = moves == connectivity::eight ? step_octile : step_unit;
    const grid_search_config config = config_for(moves);
    for (std::size_t seed = 1; seed <= seeds; ++seed) {
        const grid_map g = make_grid(k_size, k_size, seed, 25);
        const std::size_t start = 0;
        const std::size_t goal = g.walkable.size() - 1;
        const double reference = dijkstra_cost(g, start, goal, moves, cost);
        const auto star =
            crpc::algorithm::bstar_grid(g, start, goal, cost, h, config);
        const auto astar =
            crpc::algorithm::astar_grid(g, start, goal, cost, h, config);
        if (std::isinf(reference)) {
            expect_not_found(star);
            expect_not_found(astar);
            continue;
        }
        CRPC_CHECK(star.ok());
        CRPC_CHECK(astar.ok());
        if (!star.ok() || !astar.ok()) {
            continue;
        }
        check_path(g, moves, star.value().nodes, start, goal);
        const double star_cost = path_cost(g, star.value().nodes, cost);
        const double astar_cost = path_cost(g, astar.value().nodes, cost);
        CRPC_CHECK(close_to(star_cost, reference));
        CRPC_CHECK(close_to(star_cost, astar_cost));
        CRPC_CHECK(star.value().stats.expanded > 0);
    }
}

void bfs_sweep(connectivity moves, std::size_t seeds) {
    constexpr std::size_t k_size = 20;
    const grid_search_config config = config_for(moves);
    for (std::size_t seed = 1; seed <= seeds; ++seed) {
        const grid_map g = make_grid(k_size, k_size, seed, 25);
        const std::size_t start = 0;
        const std::size_t goal = g.walkable.size() - 1;
        const std::size_t reference = bfs_hops(g, start, goal, moves);
        const auto found = crpc::algorithm::bfs_grid(g, start, goal, config);
        if (reference == k_none) {
            expect_not_found(found);
            continue;
        }
        CRPC_CHECK(found.ok());
        if (!found.ok()) {
            continue;
        }
        check_path(g, moves, found.value().nodes, start, goal);
        CRPC_CHECK_EQ(reference, found.value().nodes.size() - 1);
    }
}

void bidirectional_bfs_sweep(connectivity moves, std::size_t seeds) {
    constexpr std::size_t k_size = 20;
    const grid_search_config config = config_for(moves);
    for (std::size_t seed = 1; seed <= seeds; ++seed) {
        const grid_map g = make_grid(k_size, k_size, seed, 25);
        const std::size_t start = 0;
        const std::size_t goal = g.walkable.size() - 1;
        const std::size_t reference = bfs_hops(g, start, goal, moves);
        const auto found =
            crpc::algorithm::bidirectional_bfs_grid(g, start, goal, config);
        if (reference == k_none) {
            expect_not_found(found);
            continue;
        }
        CRPC_CHECK(found.ok());
        if (!found.ok()) {
            continue;
        }
        check_path(g, moves, found.value().nodes, start, goal);
        CRPC_CHECK_EQ(reference, found.value().nodes.size() - 1);
    }
}

} // namespace

// --- A*: optimality against Dijkstra -----------------------------------------

CRPC_TEST(pathfind_astar_matches_dijkstra_4_connected_manhattan) {
    astar_sweep(connectivity::four, 0, 40);
}

CRPC_TEST(pathfind_astar_matches_dijkstra_4_connected_euclidean) {
    astar_sweep(connectivity::four, 1, 40);
}

CRPC_TEST(pathfind_astar_matches_dijkstra_8_connected_octile) {
    astar_sweep(connectivity::eight, 3, 40);
}

CRPC_TEST(pathfind_astar_matches_dijkstra_8_connected_chebyshev) {
    astar_sweep(connectivity::eight, 2, 40);
}

CRPC_TEST(pathfind_astar_matches_dijkstra_8_connected_euclidean) {
    astar_sweep(connectivity::eight, 1, 40);
}

// --- B*: optimality against Dijkstra and A* -----------------------------------

CRPC_TEST(pathfind_bstar_matches_dijkstra_and_astar_4_connected) {
    bstar_sweep(connectivity::four, h_manhattan, 40);
}

CRPC_TEST(pathfind_bstar_matches_dijkstra_and_astar_8_connected) {
    bstar_sweep(connectivity::eight, h_octile, 40);
}

CRPC_TEST(pathfind_bstar_and_astar_agree_on_open_grids) {
    constexpr std::size_t k_size = 96;
    const grid_map g = open_grid(k_size, k_size);
    const std::size_t start = 0;
    const std::size_t goal = g.walkable.size() - 1;
    const auto config = config_for(connectivity::four);
    const auto star =
        crpc::algorithm::bstar_grid(g, start, goal, step_unit, h_manhattan, config);
    const auto astar =
        crpc::algorithm::astar_grid(g, start, goal, step_unit, h_manhattan, config);
    CRPC_CHECK(star.ok());
    CRPC_CHECK(astar.ok());
    if (star.ok() && astar.ok()) {
        check_path(g, connectivity::four, star.value().nodes, start, goal);
        CRPC_CHECK(close_to(path_cost(g, star.value().nodes, step_unit),
                            path_cost(g, astar.value().nodes, step_unit)));
        CRPC_CHECK_EQ(std::size_t(2) * (k_size - 1),
                      star.value().nodes.size() - 1);
    }
}

// --- BFS: optimal hop count ---------------------------------------------------

CRPC_TEST(pathfind_bfs_matches_reference_4_connected) {
    bfs_sweep(connectivity::four, 40);
}

CRPC_TEST(pathfind_bfs_matches_reference_8_connected) {
    bfs_sweep(connectivity::eight, 40);
}

// --- Bidirectional BFS: same optimal hop count, fewer expansions --------------

CRPC_TEST(pathfind_bidirectional_bfs_matches_bfs_4_connected) {
    bidirectional_bfs_sweep(connectivity::four, 40);
}

CRPC_TEST(pathfind_bidirectional_bfs_matches_bfs_8_connected) {
    bidirectional_bfs_sweep(connectivity::eight, 40);
}

CRPC_TEST(pathfind_bidirectional_bfs_expands_fewer_nodes_on_open_grid) {
    constexpr std::size_t k_size = 96;
    const grid_map g = open_grid(k_size, k_size);
    const std::size_t start = 0;
    // Same row, not the opposite corner: with corner-to-corner endpoints the
    // two radius-d/2 balls already cover nearly the whole board, so the
    // expansion win would be invisible. The claim is about the general shape
    // (two small balls vs one large ball), not that particular diagonal.
    const std::size_t goal = k_size - 1;
    const auto config = config_for(connectivity::four);
    const auto one_way = crpc::algorithm::bfs_grid(g, start, goal, config);
    const auto two_way =
        crpc::algorithm::bidirectional_bfs_grid(g, start, goal, config);
    CRPC_CHECK(one_way.ok());
    CRPC_CHECK(two_way.ok());
    if (one_way.ok() && two_way.ok()) {
        check_path(g, connectivity::four, two_way.value().nodes, start, goal);
        CRPC_CHECK_EQ(one_way.value().nodes.size(), two_way.value().nodes.size());
        // Bidirectional breadth-first search grows two balls of radius d/2
        // instead of one ball of radius d: the expansion count is strictly
        // smaller on an open board, which is the practical point of the
        // optimization.
        CRPC_CHECK(two_way.value().stats.expanded <
                   one_way.value().stats.expanded);
    }
}

// --- DFS: complete, but not optimal ------------------------------------------

CRPC_TEST(pathfind_dfs_is_complete_and_paths_are_valid) {
    constexpr std::size_t k_size = 20;
    const auto config = config_for(connectivity::four);
    std::size_t reachable = 0;
    for (std::size_t seed = 1; seed <= 40; ++seed) {
        const grid_map g = make_grid(k_size, k_size, seed, 25);
        const std::size_t start = 0;
        const std::size_t goal = g.walkable.size() - 1;
        const std::size_t reference = bfs_hops(g, start, goal, connectivity::four);
        const auto found = crpc::algorithm::depth_first_grid(g, start, goal, config);
        if (reference == k_none) {
            expect_not_found(found);
            continue;
        }
        ++reachable;
        CRPC_CHECK(found.ok());
        if (!found.ok()) {
            continue;
        }
        check_path(g, connectivity::four, found.value().nodes, start, goal);
        // DFS is not optimal: it must never beat the BFS optimum, but it may
        // return a longer path.
        CRPC_CHECK(found.value().nodes.size() - 1 >= reference);
    }
    // The sweep must actually exercise the reachable branch.
    CRPC_CHECK(reachable >= 20);
}

CRPC_TEST(pathfind_dfs_terminates_on_unreachable_goal) {
    grid_map g = open_grid(8, 8);
    for (std::size_t y = 0; y < g.height; ++y) {
        g.walkable[y * g.width + 3] = 0;
    }
    const auto config = config_for(connectivity::four);
    expect_not_found(crpc::algorithm::depth_first_grid(g, 0, 63, config));
    expect_not_found(crpc::algorithm::iterative_deepening_grid(g, 0, 63, config));
}

// --- IDDFS: optimal depth with a depth-first frontier -------------------------

CRPC_TEST(pathfind_iddfs_matches_bfs_hops) {
    constexpr std::size_t k_size = 10;
    const auto config = config_for(connectivity::four);
    for (std::size_t seed = 1; seed <= 20; ++seed) {
        const grid_map g = make_grid(k_size, k_size, seed, 20);
        const std::size_t start = 0;
        const std::size_t goal = g.walkable.size() - 1;
        const std::size_t reference = bfs_hops(g, start, goal, connectivity::four);
        const auto found =
            crpc::algorithm::iterative_deepening_grid(g, start, goal, config);
        if (reference == k_none) {
            expect_not_found(found);
            continue;
        }
        CRPC_CHECK(found.ok());
        if (!found.ok()) {
            continue;
        }
        check_path(g, connectivity::four, found.value().nodes, start, goal);
        CRPC_CHECK_EQ(reference, found.value().nodes.size() - 1);
    }
}

// --- Edge cases ---------------------------------------------------------------

CRPC_TEST(pathfind_start_equals_goal_is_a_single_node_path) {
    const grid_map g = open_grid(5, 5);
    const auto config = config_for(connectivity::four);
    const std::size_t p = 12;
    const auto a = crpc::algorithm::astar_grid(g, p, p, step_unit, h_manhattan, config);
    const auto b = crpc::algorithm::bstar_grid(g, p, p, step_unit, h_manhattan, config);
    const auto c = crpc::algorithm::bfs_grid(g, p, p, config);
    const auto d = crpc::algorithm::bidirectional_bfs_grid(g, p, p, config);
    const auto e = crpc::algorithm::depth_first_grid(g, p, p, config);
    const auto f = crpc::algorithm::iterative_deepening_grid(g, p, p, config);
    const crpc::result<crpc::algorithm::grid_path>* all[] = {&a, &b, &c, &d, &e, &f};
    for (const auto* r : all) {
        CRPC_CHECK(r->ok());
        if (r->ok()) {
            CRPC_CHECK_EQ(std::size_t(1), r->value().nodes.size());
            CRPC_CHECK_EQ(p, r->value().nodes.front());
        }
    }
}

CRPC_TEST(pathfind_blocked_endpoints_report_not_found) {
    const auto config = config_for(connectivity::four);
    grid_map g = open_grid(4, 4);
    const std::size_t start = 0;
    const std::size_t goal = 15;

    g.walkable[start] = 0;
    expect_not_found(crpc::algorithm::astar_grid(g, start, goal, step_unit, h_manhattan, config));
    expect_not_found(crpc::algorithm::bstar_grid(g, start, goal, step_unit, h_manhattan, config));
    expect_not_found(crpc::algorithm::bfs_grid(g, start, goal, config));
    expect_not_found(crpc::algorithm::bidirectional_bfs_grid(g, start, goal, config));
    expect_not_found(crpc::algorithm::depth_first_grid(g, start, goal, config));
    expect_not_found(crpc::algorithm::iterative_deepening_grid(g, start, goal, config));

    g.walkable[start] = 1;
    g.walkable[goal] = 0;
    expect_not_found(crpc::algorithm::astar_grid(g, start, goal, step_unit, h_manhattan, config));
    expect_not_found(crpc::algorithm::bstar_grid(g, start, goal, step_unit, h_manhattan, config));
    expect_not_found(crpc::algorithm::bfs_grid(g, start, goal, config));
    expect_not_found(crpc::algorithm::bidirectional_bfs_grid(g, start, goal, config));
    expect_not_found(crpc::algorithm::depth_first_grid(g, start, goal, config));
    expect_not_found(crpc::algorithm::iterative_deepening_grid(g, start, goal, config));
}

CRPC_TEST(pathfind_empty_and_out_of_range_endpoints_report_not_found) {
    const auto config = config_for(connectivity::four);
    const grid_map empty;
    expect_not_found(crpc::algorithm::astar_grid(empty, 0, 0, step_unit, h_manhattan, config));
    expect_not_found(crpc::algorithm::bstar_grid(empty, 0, 0, step_unit, h_manhattan, config));
    expect_not_found(crpc::algorithm::bfs_grid(empty, 0, 0, config));
    expect_not_found(crpc::algorithm::bidirectional_bfs_grid(empty, 0, 0, config));
    expect_not_found(crpc::algorithm::depth_first_grid(empty, 0, 0, config));
    expect_not_found(crpc::algorithm::iterative_deepening_grid(empty, 0, 0, config));

    const grid_map one = open_grid(1, 1);
    expect_not_found(crpc::algorithm::astar_grid(one, 0, 1, step_unit, h_manhattan, config));
    expect_not_found(crpc::algorithm::bstar_grid(one, 0, 1, step_unit, h_manhattan, config));
    expect_not_found(crpc::algorithm::bfs_grid(one, 0, 1, config));
    expect_not_found(crpc::algorithm::bidirectional_bfs_grid(one, 0, 1, config));
    expect_not_found(crpc::algorithm::depth_first_grid(one, 0, 1, config));
    expect_not_found(crpc::algorithm::iterative_deepening_grid(one, 0, 1, config));
}

CRPC_TEST(pathfind_single_row_and_column_are_solved) {
    const auto config = config_for(connectivity::four);
    const grid_map row = open_grid(10, 1);
    const auto a = crpc::algorithm::astar_grid(row, 0, 9, step_unit, h_manhattan, config);
    const auto b = crpc::algorithm::bstar_grid(row, 0, 9, step_unit, h_manhattan, config);
    const auto c = crpc::algorithm::bfs_grid(row, 0, 9, config);
    const auto d = crpc::algorithm::bidirectional_bfs_grid(row, 0, 9, config);
    const auto e = crpc::algorithm::depth_first_grid(row, 0, 9, config);
    const auto f = crpc::algorithm::iterative_deepening_grid(row, 0, 9, config);
    const crpc::result<crpc::algorithm::grid_path>* all[] = {&a, &b, &c, &d, &e, &f};
    for (const auto* r : all) {
        CRPC_CHECK(r->ok());
        if (r->ok()) {
            check_path(row, connectivity::four, r->value().nodes, 0, 9);
            CRPC_CHECK_EQ(std::size_t(9), r->value().nodes.size() - 1);
        }
    }

    const grid_map column = open_grid(1, 10);
    const auto g = crpc::algorithm::astar_grid(column, 0, 9, step_unit, h_manhattan, config);
    const auto h = crpc::algorithm::bstar_grid(column, 0, 9, step_unit, h_manhattan, config);
    const auto i = crpc::algorithm::bfs_grid(column, 0, 9, config);
    const auto j = crpc::algorithm::bidirectional_bfs_grid(column, 0, 9, config);
    const auto k = crpc::algorithm::depth_first_grid(column, 0, 9, config);
    const auto l = crpc::algorithm::iterative_deepening_grid(column, 0, 9, config);
    const crpc::result<crpc::algorithm::grid_path>* tall[] = {&g, &h, &i, &j, &k, &l};
    for (const auto* r : tall) {
        CRPC_CHECK(r->ok());
        if (r->ok()) {
            check_path(column, connectivity::four, r->value().nodes, 0, 9);
            CRPC_CHECK_EQ(std::size_t(9), r->value().nodes.size() - 1);
        }
    }
}

CRPC_TEST(pathfind_all_obstacles_and_split_board_report_not_found) {
    const auto config = config_for(connectivity::four);
    grid_map blocked = open_grid(4, 4);
    std::fill(blocked.walkable.begin(), blocked.walkable.end(), std::uint8_t{0});
    expect_not_found(crpc::algorithm::astar_grid(blocked, 0, 15, step_unit, h_manhattan, config));
    expect_not_found(crpc::algorithm::bstar_grid(blocked, 0, 15, step_unit, h_manhattan, config));
    expect_not_found(crpc::algorithm::bfs_grid(blocked, 0, 15, config));
    expect_not_found(crpc::algorithm::bidirectional_bfs_grid(blocked, 0, 15, config));
    expect_not_found(crpc::algorithm::depth_first_grid(blocked, 0, 15, config));
    expect_not_found(crpc::algorithm::iterative_deepening_grid(blocked, 0, 15, config));

    grid_map split = open_grid(8, 8);
    for (std::size_t y = 0; y < split.height; ++y) {
        split.walkable[y * split.width + 3] = 0;
    }
    expect_not_found(crpc::algorithm::astar_grid(split, 0, 63, step_unit, h_manhattan, config));
    expect_not_found(crpc::algorithm::bstar_grid(split, 0, 63, step_unit, h_manhattan, config));
    expect_not_found(crpc::algorithm::bfs_grid(split, 0, 63, config));
    expect_not_found(crpc::algorithm::bidirectional_bfs_grid(split, 0, 63, config));
    expect_not_found(crpc::algorithm::depth_first_grid(split, 0, 63, config));
    expect_not_found(crpc::algorithm::iterative_deepening_grid(split, 0, 63, config));
}

CRPC_TEST(pathfind_expansion_budget_is_resource_exhausted) {
    const grid_map g = open_grid(32, 32);
    grid_search_config config = config_for(connectivity::four);
    config.max_expanded = 1;
    const auto a = crpc::algorithm::astar_grid(g, 0, 1023, step_unit, h_manhattan, config);
    CRPC_CHECK_FALSE(a.ok());
    if (!a.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::resource_exhausted),
                      static_cast<int>(a.code()));
    }
    const auto b = crpc::algorithm::bfs_grid(g, 0, 1023, config);
    CRPC_CHECK_FALSE(b.ok());
    if (!b.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::resource_exhausted),
                      static_cast<int>(b.code()));
    }
    const auto c = crpc::algorithm::bstar_grid(g, 0, 1023, step_unit, h_manhattan, config);
    CRPC_CHECK_FALSE(c.ok());
    if (!c.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::resource_exhausted),
                      static_cast<int>(c.code()));
    }
}

// --- Large open board (kept deliberately loose so CI cannot flake) -----------

CRPC_TEST(pathfind_large_open_board_all_algorithms) {
    constexpr std::size_t k_size = 256;
    const grid_map g = open_grid(k_size, k_size);
    const std::size_t start = 0;
    // Same-row goal keeps the bidirectional ball comparison meaningful (see
    // pathfind_bidirectional_bfs_expands_fewer_nodes_on_open_grid).
    const std::size_t goal = k_size - 1;
    const std::size_t optimal = k_size - 1;
    const auto config = config_for(connectivity::four);

    const auto astar =
        crpc::algorithm::astar_grid(g, start, goal, step_unit, h_manhattan, config);
    const auto star =
        crpc::algorithm::bstar_grid(g, start, goal, step_unit, h_manhattan, config);
    const auto bfs = crpc::algorithm::bfs_grid(g, start, goal, config);
    const auto two_way =
        crpc::algorithm::bidirectional_bfs_grid(g, start, goal, config);

    CRPC_CHECK(astar.ok());
    CRPC_CHECK(star.ok());
    CRPC_CHECK(bfs.ok());
    CRPC_CHECK(two_way.ok());
    if (astar.ok() && star.ok() && bfs.ok() && two_way.ok()) {
        check_path(g, connectivity::four, astar.value().nodes, start, goal);
        check_path(g, connectivity::four, star.value().nodes, start, goal);
        check_path(g, connectivity::four, bfs.value().nodes, start, goal);
        check_path(g, connectivity::four, two_way.value().nodes, start, goal);
        CRPC_CHECK_EQ(optimal, astar.value().nodes.size() - 1);
        CRPC_CHECK_EQ(optimal, star.value().nodes.size() - 1);
        CRPC_CHECK_EQ(optimal, bfs.value().nodes.size() - 1);
        CRPC_CHECK_EQ(optimal, two_way.value().nodes.size() - 1);
        CRPC_CHECK(two_way.value().stats.expanded < bfs.value().stats.expanded);
    }
    // The 5 s wall-clock bound was a degeneration gate with no reachable V_bad;
    // it lives in bench/bench_algorithms.cpp now (plan.md D3).
}

CRPC_TEST_MAIN()