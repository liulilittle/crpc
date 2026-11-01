// CRPC -- tests for crpc/algorithm: A* pathfinding over a generic graph.
//
// Optimality is proven by exhaustive comparison against a Dijkstra reference on
// random 20x20 obstacle grids over many seeds, for every supported heuristic.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <crpc/algorithm/astar.hpp>
#include <crpc/algorithm/heuristic.hpp>
#include <crpc/error.hpp>

#include "test_framework.hpp"

namespace {

constexpr double k_inf = std::numeric_limits<double>::infinity();
constexpr double k_sqrt2 = 1.4142135623730951;

// A deterministic PRNG keeps the suite reproducible on every machine.
std::uint64_t xorshift64(std::uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

struct world {
    std::size_t width = 0;
    std::size_t height = 0;
    std::vector<bool> walkable;

    std::size_t index(std::size_t x, std::size_t y) const { return y * width + x; }
    bool in_bounds(std::size_t x, std::size_t y) const {
        return x < width && y < height;
    }
    bool ok(std::size_t n) const { return n < walkable.size() && walkable[n]; }
    std::size_t cell_x(std::size_t n) const { return n % width; }
    std::size_t cell_y(std::size_t n) const { return n / width; }
};

// Builds a random obstacle field; start (0,0) and goal (w-1,h-1) stay walkable.
void obstacle_world(std::size_t width, std::size_t height, std::uint64_t seed,
                    std::size_t density_pct, world& out) {
    out.width = width;
    out.height = height;
    out.walkable.assign(width * height, true);
    if (width == 0 || height == 0) {
        return;
    }
    std::uint64_t state = seed;
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            if (xorshift64(state) % 100 < density_pct) {
                out.walkable[out.index(x, y)] = false;
            }
        }
    }
    out.walkable[0] = true;
    out.walkable[out.index(width - 1, height - 1)] = true;
}

// Neighbourhood shared by A* and the Dijkstra reference, so both explore the
// same graph. `diagonal` enables the 8-move (octile) neighbourhood.
void neighbours(const world& w, std::size_t n, bool diagonal,
                std::vector<std::size_t>& out) {
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) {
                continue;
            }
            if (!diagonal && dx != 0 && dy != 0) {
                continue;
            }
            const long long nx = static_cast<long long>(w.cell_x(n)) + dx;
            const long long ny = static_cast<long long>(w.cell_y(n)) + dy;
            if (nx < 0 || ny < 0 ||
                static_cast<std::size_t>(nx) >= w.width ||
                static_cast<std::size_t>(ny) >= w.height) {
                continue;
            }
            const std::size_t target =
                w.index(static_cast<std::size_t>(nx), static_cast<std::size_t>(ny));
            if (w.ok(target)) {
                out.push_back(target);
            }
        }
    }
}

double edge_cost_4(const world&, std::size_t, std::size_t) {
    return 1.0;
}

double edge_cost_8(const world& w, std::size_t a, std::size_t b) {
    const std::size_t ax = w.cell_x(a);
    const std::size_t ay = w.cell_y(a);
    const std::size_t bx = w.cell_x(b);
    const std::size_t by = w.cell_y(b);
    const std::size_t dx = ax > bx ? ax - bx : bx - ax;
    const std::size_t dy = ay > by ? ay - by : by - ay;
    return (dx != 0 && dy != 0) ? k_sqrt2 : 1.0;
}

double path_cost(const world& w, bool diagonal,
                 const std::vector<std::size_t>& path) {
    double total = 0.0;
    for (std::size_t i = 1; i < path.size(); ++i) {
        total += diagonal ? edge_cost_8(w, path[i - 1], path[i])
                          : edge_cost_4(w, path[i - 1], path[i]);
    }
    return total;
}

// Validates that each consecutive pair is a real neighbour move, that every
// node is walkable, and that the endpoints match the request.
bool valid_moves(const world& w, bool diagonal,
                 const std::vector<std::size_t>& path, std::string& why) {
    if (path.empty()) {
        why = "empty path";
        return false;
    }
    for (std::size_t i = 1; i < path.size(); ++i) {
        const std::size_t a = path[i - 1];
        const std::size_t b = path[i];
        if (!w.ok(a) || !w.ok(b)) {
            why = "path crosses a blocked cell";
            return false;
        }
        const std::size_t ax = w.cell_x(a);
        const std::size_t ay = w.cell_y(a);
        const std::size_t bx = w.cell_x(b);
        const std::size_t by = w.cell_y(b);
        const std::size_t dx = ax > bx ? ax - bx : bx - ax;
        const std::size_t dy = ay > by ? ay - by : by - ay;
        if (dx > 1 || dy > 1 || (dx == 0 && dy == 0)) {
            why = "path makes an illegal jump";
            return false;
        }
        if (!diagonal && dx != 0 && dy != 0) {
            why = "diagonal move in a 4-connected world";
            return false;
        }
        std::vector<std::size_t> next;
        neighbours(w, a, diagonal, next);
        bool found = false;
        for (const std::size_t m : next) {
            if (m == b) {
                found = true;
                break;
            }
        }
        if (!found) {
            why = "move not in the neighbour set";
            return false;
        }
    }
    return true;
}

bool close_to(double a, double b) {
    return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
}

template <typename FNeighbors, typename FCost>
double dijkstra_cost(std::size_t start, std::size_t goal, FNeighbors neighbors,
                     FCost cost) {
    using queue_type = std::pair<double, std::size_t>;
    std::priority_queue<queue_type, std::vector<queue_type>,
                        std::greater<queue_type>>
        open;
    std::unordered_map<std::size_t, double> best;
    best[start] = 0.0;
    open.push({0.0, start});
    while (!open.empty()) {
        const double d = open.top().first;
        const std::size_t n = open.top().second;
        open.pop();
        if (n == goal) {
            return d;
        }
        if (best[n] != d) {
            continue;
        }
        std::vector<std::size_t> next;
        neighbors(n, next);
        for (const std::size_t m : next) {
            const double nd = d + cost(n, m);
            const auto found = best.find(m);
            if (found == best.end() || nd < found->second) {
                best[m] = nd;
                open.push({nd, m});
            }
        }
    }
    return k_inf;
}

// A* with a specific heuristic on a random grid, checked against Dijkstra on
// the very same graph. Both algorithms must agree on reachability and, when a
// path exists, on its exact cost.
void compare_on_random_grids(bool diagonal, int heuristic_kind,
                             std::size_t seeds) {
    constexpr std::size_t k_size = 20;
    world w;
    for (std::size_t seed = 1; seed <= seeds; ++seed) {
        obstacle_world(k_size, k_size, seed, 25, w);
        const std::size_t start = 0;
        const std::size_t goal = w.index(k_size - 1, k_size - 1);

        auto neighbors = [&](std::size_t n, std::vector<std::size_t>& out) {
            neighbours(w, n, diagonal, out);
        };
        auto cost = [&](std::size_t a, std::size_t b) {
            return diagonal ? edge_cost_8(w, a, b) : edge_cost_4(w, a, b);
        };
        auto heuristic = [&](std::size_t n, std::size_t g) {
            const std::size_t gx = w.cell_x(g);
            const std::size_t gy = w.cell_y(g);
            const std::size_t nx = w.cell_x(n);
            const std::size_t ny = w.cell_y(n);
            switch (heuristic_kind) {
            case 0:
                return static_cast<double>(
                    crpc::algorithm::manhattan_distance(nx, ny, gx, gy));
            case 1:
                return crpc::algorithm::euclidean_distance(nx, ny, gx, gy);
            case 2:
                return static_cast<double>(
                    crpc::algorithm::chebyshev_distance(nx, ny, gx, gy));
            default:
                return crpc::algorithm::octile_distance(nx, ny, gx, gy);
            }
        };

        const double reference = dijkstra_cost(start, goal, neighbors, cost);
        const auto astar = crpc::algorithm::astar_path<std::size_t>(
            start, goal, neighbors, cost, heuristic);

        if (std::isinf(reference)) {
            // Completeness: an unreachable goal is a clear "no path", never a
            // hang and never a bogus path.
            CRPC_CHECK_FALSE(astar.ok());
            if (!astar.ok()) {
                CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                              static_cast<int>(astar.code()));
            }
            continue;
        }
        CRPC_CHECK(astar.ok());
        if (!astar.ok()) {
            continue;
        }
        std::string why;
        CRPC_CHECK(valid_moves(w, diagonal, astar.value(), why));
        if (astar.value().front() != start || astar.value().back() != goal) {
            CRPC_CHECK_EQ(start, astar.value().front());
            CRPC_CHECK_EQ(goal, astar.value().back());
        }
        // Optimality: A* cost must equal the Dijkstra shortest-path cost.
        CRPC_CHECK(close_to(path_cost(w, diagonal, astar.value()), reference));
    }
}

} // namespace

// --- Edge cases ----------------------------------------------------------------

// The comparison against Dijkstra only proves the algorithm is right for an
// *admissible* heuristic -- every heuristic in that sweep never overestimates.
// It cannot tell a correct A* from one that merely happens to work because its
// heuristic is honest. This case supplies a heuristic that deliberately
// overestimates by a large factor: A* then explores greedily and returns a
// suboptimal path, and the check below confirms the test harness can see that.
//
// Without this, "A* matches Dijkstra" would be compatible with an implementation
// that ignores the heuristic entirely, or one that returns the greedy path.
CRPC_TEST(astar_with_an_inadmissible_heuristic_is_detectably_suboptimal) {
    world w;
    // A corridor of open cells with one cheap detour, so the greedy and the
    // optimal routes differ in cost.
    obstacle_world(20, 20, 7, 30, w);

    auto neighbors = [&](std::size_t n, std::vector<std::size_t>& out) {
        neighbours(w, n, false, out);
    };
    auto cost = [&](std::size_t a, std::size_t b) { return edge_cost_4(w, a, b); };
    auto admissible = [&](std::size_t n, std::size_t g) {
        return static_cast<double>(crpc::algorithm::manhattan_distance(
            w.cell_x(n), w.cell_y(n), w.cell_x(g), w.cell_y(g)));
    };
    // 50x the true remaining distance: no longer a lower bound.
    auto inadmissible = [&](std::size_t n, std::size_t g) {
        return 50.0 * static_cast<double>(crpc::algorithm::manhattan_distance(
                          w.cell_x(n), w.cell_y(n), w.cell_x(g), w.cell_y(g)));
    };

    const std::size_t start = 0;
    const std::size_t goal = w.index(19, 19);
    const double reference = dijkstra_cost(start, goal, neighbors, cost);
    if (std::isinf(reference)) {
        // Seed 7 must be reachable for this case to mean anything; if the grid
        // ever changes so it is not, the case would silently stop testing.
        CRPC_CHECK(false);
        return;
    }

    const auto honest =
        crpc::algorithm::astar_path<std::size_t>(start, goal, neighbors, cost, admissible);
    CRPC_CHECK(honest.ok());
    if (honest.ok()) {
        CRPC_CHECK(close_to(path_cost(w, false, honest.value()), reference));
    }

    const auto greedy = crpc::algorithm::astar_path<std::size_t>(start, goal, neighbors,
                                                                 cost, inadmissible);
    // It still finds *a* path (completeness is independent of admissibility)...
    CRPC_CHECK(greedy.ok());
    if (greedy.ok()) {
        // ...and this is the assertion that proves the comparison has teeth: an
        // overestimating heuristic produces a cost strictly worse than optimal.
        // If A* ignored the heuristic, this would equal `reference` and the case
        // would fail, which is exactly what makes the sweep above meaningful.
        const double greedy_cost = path_cost(w, false, greedy.value());
        CRPC_CHECK(greedy_cost > reference + 1e-9);
    }
}

CRPC_TEST(astar_start_equals_goal_returns_single_node_path) {
    world w;
    obstacle_world(20, 20, 1, 0, w);
    const auto result = crpc::algorithm::astar_path<std::size_t>(
        std::size_t(0), std::size_t(0),
        [&](std::size_t n, std::vector<std::size_t>& out) {
            neighbours(w, n, false, out);
        },
        [&](std::size_t a, std::size_t b) { return edge_cost_4(w, a, b); },
        [&](std::size_t n, std::size_t g) {
            return static_cast<double>(crpc::algorithm::manhattan_distance(
                n, n, g, g));
        });
    CRPC_CHECK(result.ok());
    if (result.ok()) {
        CRPC_CHECK_EQ(std::size_t(1), result.value().size());
        CRPC_CHECK_EQ(std::size_t(0), result.value()[0]);
    }
}

CRPC_TEST(astar_unreachable_goal_returns_no_path) {
    // A fully blocked column splits the 8x8 board in two.
    world w;
    obstacle_world(8, 8, 7, 0, w);
    for (std::size_t y = 0; y < w.height; ++y) {
        w.walkable[w.index(3, y)] = false;
    }
    const std::size_t start = w.index(0, 0);
    const std::size_t goal = w.index(7, 7);
    const bool diagonals[] = {false, true};
    for (const bool diagonal : diagonals) {
        const auto result = crpc::algorithm::astar_path<std::size_t>(
            start, goal,
            [&](std::size_t n, std::vector<std::size_t>& out) {
                neighbours(w, n, diagonal, out);
            },
            [&](std::size_t a, std::size_t b) {
                return diagonal ? edge_cost_8(w, a, b) : edge_cost_4(w, a, b);
            },
            [&](std::size_t n, std::size_t g) {
                return static_cast<double>(crpc::algorithm::manhattan_distance(
                    w.cell_x(n), w.cell_y(n), w.cell_x(g), w.cell_y(g)));
            });
        CRPC_CHECK_FALSE(result.ok());
        if (!result.ok()) {
            CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                          static_cast<int>(result.code()));
        }
    }
}

CRPC_TEST(astar_blocked_start_and_goal_are_no_path) {
    world w;
    obstacle_world(3, 3, 3, 100, w);
    w.walkable.assign(9, false);
    const std::size_t start = w.index(0, 0);
    const std::size_t goal = w.index(2, 2);

    const auto blocked_start = crpc::algorithm::astar_path<std::size_t>(
        start, goal,
        [&](std::size_t n, std::vector<std::size_t>& out) {
            neighbours(w, n, false, out);
        },
        [&](std::size_t a, std::size_t b) { return edge_cost_4(w, a, b); },
        [&](std::size_t, std::size_t) { return 0.0; });
    CRPC_CHECK_FALSE(blocked_start.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                  static_cast<int>(blocked_start.code()));

    w.walkable.assign(9, true);
    w.walkable[goal] = false;
    const auto blocked_goal = crpc::algorithm::astar_path<std::size_t>(
        start, goal,
        [&](std::size_t n, std::vector<std::size_t>& out) {
            neighbours(w, n, false, out);
        },
        [&](std::size_t a, std::size_t b) { return edge_cost_4(w, a, b); },
        [&](std::size_t n, std::size_t g) {
            return static_cast<double>(crpc::algorithm::manhattan_distance(
                w.cell_x(n), w.cell_y(n), w.cell_x(g), w.cell_y(g)));
        });
    CRPC_CHECK_FALSE(blocked_goal.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                  static_cast<int>(blocked_goal.code()));
}

CRPC_TEST(astar_empty_graph_returns_no_path) {
    // A single-node world with a goal that does not exist: no neighbour ever
    // yields the goal, so the search must terminate with a clear "no path".
    world w;
    w.width = 1;
    w.height = 1;
    w.walkable = {true};
    const auto result = crpc::algorithm::astar_path<std::size_t>(
        std::size_t(0), std::size_t(1),
        [&](std::size_t n, std::vector<std::size_t>& out) {
            neighbours(w, n, false, out);
        },
        [&](std::size_t a, std::size_t b) { return edge_cost_4(w, a, b); },
        [&](std::size_t, std::size_t) { return 0.0; });
    CRPC_CHECK_FALSE(result.ok());
    CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                  static_cast<int>(result.code()));
}

// --- Optimality on a hand-verified board ---------------------------------------

CRPC_TEST(astar_finds_the_optimal_detour) {
    // 4x3 board, column x=1 blocked at rows 0 and 1:
    //   . X . .
    //   . X . .
    //   . . . .
    // From (0,0) to (3,0) the only 4-connected route dips to row 2 and then
    // climbs back: 7 moves of cost 1.
    world w;
    obstacle_world(4, 3, 11, 0, w);
    w.walkable[w.index(1, 0)] = false;
    w.walkable[w.index(1, 1)] = false;
    const std::size_t start = w.index(0, 0);
    const std::size_t goal = w.index(3, 0);

    const auto result = crpc::algorithm::astar_path<std::size_t>(
        start, goal,
        [&](std::size_t n, std::vector<std::size_t>& out) {
            neighbours(w, n, false, out);
        },
        [&](std::size_t a, std::size_t b) { return edge_cost_4(w, a, b); },
        [&](std::size_t n, std::size_t g) {
            return static_cast<double>(crpc::algorithm::manhattan_distance(
                w.cell_x(n), w.cell_y(n), w.cell_x(g), w.cell_y(g)));
        });
    CRPC_CHECK(result.ok());
    if (result.ok()) {
        std::string why;
        CRPC_CHECK(valid_moves(w, false, result.value(), why));
        CRPC_CHECK_EQ(7.0, path_cost(w, false, result.value()));
        CRPC_CHECK_EQ(start, result.value().front());
        CRPC_CHECK_EQ(goal, result.value().back());
    }
}

// --- Exhaustive optimum comparison against Dijkstra -------------------------------

CRPC_TEST(astar_matches_dijkstra_4_connected_manhattan) {
    compare_on_random_grids(/*diagonal=*/false, /*heuristic_kind=*/0,
                            /*seeds=*/40);
}

CRPC_TEST(astar_matches_dijkstra_4_connected_euclidean) {
    compare_on_random_grids(/*diagonal=*/false, /*heuristic_kind=*/1,
                            /*seeds=*/40);
}

CRPC_TEST(astar_matches_dijkstra_8_connected_euclidean) {
    compare_on_random_grids(/*diagonal=*/true, /*heuristic_kind=*/1,
                            /*seeds=*/40);
}

CRPC_TEST(astar_matches_dijkstra_8_connected_chebyshev) {
    compare_on_random_grids(/*diagonal=*/true, /*heuristic_kind=*/2,
                            /*seeds=*/40);
}

CRPC_TEST(astar_matches_dijkstra_8_connected_octile) {
    compare_on_random_grids(/*diagonal=*/true, /*heuristic_kind=*/3,
                            /*seeds=*/40);
}

// --- Large open areas ---------------------------------------------------------

CRPC_TEST(astar_large_open_area_stays_fast) {
    constexpr std::size_t k_size = 200;
    world w;
    w.width = k_size;
    w.height = k_size;
    w.walkable.assign(k_size * k_size, true);
    const std::size_t start = w.index(0, 0);
    const std::size_t goal = w.index(k_size - 1, k_size - 1);

    // A node budget, not a clock. Corner-to-corner on an open board is the case where
    // every node in the rectangle has the same f, so this search really does expand
    // about 40000 of the 40000 cells -- measured, and the reason no tighter count is
    // possible here. What the budget still catches is a search that revisits nodes
    // instead of closing them, which costs a multiple of the board rather than being
    // 10% slower. A per-node slowdown remains invisible; that is the gap the bench
    // covers now (plan.md D3/R1).
    crpc::algorithm::astar_config config;
    config.max_expanded = 60000;

    const auto result = crpc::algorithm::astar_path<std::size_t>(
        start, goal,
        [&](std::size_t n, std::vector<std::size_t>& out) {
            neighbours(w, n, false, out);
        },
        [&](std::size_t a, std::size_t b) { return edge_cost_4(w, a, b); },
        [&](std::size_t n, std::size_t g) {
            return static_cast<double>(crpc::algorithm::manhattan_distance(
                w.cell_x(n), w.cell_y(n), w.cell_x(g), w.cell_y(g)));
        },
        config);
    CRPC_CHECK(result.ok());
    if (result.ok()) {
        CRPC_CHECK_EQ(start, result.value().front());
        CRPC_CHECK_EQ(goal, result.value().back());
        CRPC_CHECK_EQ(k_size * 2 - 2, path_cost(w, false, result.value()));
    }
    // The 4 s wall-clock bound was a degeneration gate with no reachable V_bad;
    // it lives in bench/bench_algorithms.cpp now (plan.md D3).
}

CRPC_TEST(astar_8_connected_open_area_stays_fast) {
    constexpr std::size_t k_size = 150;
    world w;
    w.width = k_size;
    w.height = k_size;
    w.walkable.assign(k_size * k_size, true);
    const std::size_t start = w.index(0, 0);
    const std::size_t goal = w.index(k_size - 1, k_size - 1);

    // Same node budget as the 4-connected case, for the same reason: it catches a
    // search that re-expands instead of closing, which costs a multiple of the board.
    // No tighter bound is claimed -- with an exact heuristic on an open board the
    // equal-cost ties make the expansion count a property of the tie-break, and the
    // number was not measured for this shape (unlike the 4-connected case above).
    crpc::algorithm::astar_config config;
    config.max_expanded = 60000;

    const auto result = crpc::algorithm::astar_path<std::size_t>(
        start, goal,
        [&](std::size_t n, std::vector<std::size_t>& out) {
            neighbours(w, n, true, out);
        },
        [&](std::size_t a, std::size_t b) { return edge_cost_8(w, a, b); },
        [&](std::size_t n, std::size_t g) {
            return crpc::algorithm::octile_distance(
                w.cell_x(n), w.cell_y(n), w.cell_x(g), w.cell_y(g));
        },
        config);
    CRPC_CHECK(result.ok());
    if (result.ok()) {
        CRPC_CHECK_EQ(start, result.value().front());
        CRPC_CHECK_EQ(goal, result.value().back());
    }
    // The 4 s wall-clock bound was a degeneration gate with no reachable V_bad;
    // it lives in bench/bench_algorithms.cpp now (plan.md D3).
}

CRPC_TEST_MAIN()