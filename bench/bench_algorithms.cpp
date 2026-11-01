// CRPC -- degeneracy-gate benchmarks moved out of the test suite (plan.md D3).
//
// Seven wall-clock upper bounds used to live in the unit tests. None of them
// had a reachable V_bad: they only said "slower than expected", with 20-40x of
// margin, so on a shared runner a noisy neighbour turned a correct build red.
// They are reported here instead, at the same sizes the tests used:
//
//   * A* over a 200x200 open board (4-connected) and a 150x150 open board
//     (8-connected), the two searches test_astar timed;
//   * the four searches of test_pathfind over a 256x256 open board;
//   * 10,000 point-in-polygon and 10,000 circle-polygon tests on a 32-gon;
//   * the AOI stress world (10,000 entities, 1,000 watchers, 20 ticks of 500
//     moved entities), reporting build/ticks/verify separately;
//   * CART training on 10,000 samples x 20 features at depth 8.
//
// A benchmark is not a gate: it prints numbers and never fails the build.
#include <crpc/algorithm/astar.hpp>
#include <crpc/algorithm/heuristic.hpp>
#include <crpc/algorithm/pathfind.hpp>
#include <crpc/game/ai/decision_tree.hpp>
#include <crpc/game/aoi/aoi.hpp>
#include <crpc/game/geom/collide2d.hpp>
#include <crpc/game/geom/shapes2d.hpp>
#include <crpc/game/geom/vec.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "bench_harness.hpp"

namespace alg = crpc::algorithm;
namespace ai = crpc::game::ai;
namespace aoi = crpc::game::aoi;
namespace geom = crpc::game::geom;

namespace {

using clock_type = std::chrono::steady_clock;

double elapsed_ms(clock_type::time_point start, clock_type::time_point stop) {
    return std::chrono::duration<double, std::milli>(stop - start).count();
}

struct sample_stats {
    double median_ms = 0.0;
    double spread_ms = 0.0;
};

// One discarded warm-up pass, then `runs` timed passes; the median is reported
// because the best sample describes a machine that does not exist.
template <class Body>
sample_stats sample_ms(int runs, Body&& body) {
    body();
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(runs));
    for (int run = 0; run < runs; ++run) {
        const auto start = clock_type::now();
        body();
        const auto stop = clock_type::now();
        samples.push_back(elapsed_ms(start, stop));
    }
    std::sort(samples.begin(), samples.end());
    sample_stats stats;
    stats.median_ms = samples[samples.size() / 2];
    stats.spread_ms = samples.back() - samples.front();
    return stats;
}

template <class Body>
void report_ms(const char* name, int runs, Body&& body) {
    if (!crpc_bench::enabled(name)) {
        return;
    }
    const sample_stats stats = sample_ms(runs, body);
    crpc_bench::record(name, "ms/op", stats.median_ms, stats.spread_ms);
}

// --- A* over generic open grids (test_astar sizes) ---------------------------

struct bench_grid {
    std::size_t width = 0;
    std::size_t height = 0;
    std::vector<bool> walkable;

    std::size_t index(std::size_t x, std::size_t y) const {
        return y * width + x;
    }
    bool ok(std::size_t node) const {
        return node < walkable.size() && walkable[node];
    }
    std::size_t cell_x(std::size_t node) const { return node % width; }
    std::size_t cell_y(std::size_t node) const { return node / width; }
};

void grid_neighbours(const bench_grid& grid, std::size_t node, bool diagonal,
                     std::vector<std::size_t>& out) {
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) {
                continue;
            }
            if (!diagonal && dx != 0 && dy != 0) {
                continue;
            }
            const long long nx =
                static_cast<long long>(grid.cell_x(node)) + dx;
            const long long ny =
                static_cast<long long>(grid.cell_y(node)) + dy;
            if (nx < 0 || ny < 0 ||
                static_cast<std::size_t>(nx) >= grid.width ||
                static_cast<std::size_t>(ny) >= grid.height) {
                continue;
            }
            const std::size_t target =
                grid.index(static_cast<std::size_t>(nx),
                           static_cast<std::size_t>(ny));
            if (grid.ok(target)) {
                out.push_back(target);
            }
        }
    }
}

double edge_cost_4(const bench_grid&, std::size_t, std::size_t) { return 1.0; }

double edge_cost_8(const bench_grid& grid, std::size_t a, std::size_t b) {
    const std::size_t ax = grid.cell_x(a);
    const std::size_t ay = grid.cell_y(a);
    const std::size_t bx = grid.cell_x(b);
    const std::size_t by = grid.cell_y(b);
    const std::size_t dx = ax > bx ? ax - bx : bx - ax;
    const std::size_t dy = ay > by ? ay - by : by - ay;
    return (dx != 0 && dy != 0) ? 1.4142135623730951 : 1.0;
}

void run_open_area(std::size_t size, bool diagonal, const char* name) {
    bench_grid grid;
    grid.width = size;
    grid.height = size;
    grid.walkable.assign(size * size, true);
    const std::size_t start = grid.index(0, 0);
    const std::size_t goal = grid.index(size - 1, size - 1);

    report_ms(name, 5, [&] {
        const auto found = alg::astar_path<std::size_t>(
            start, goal,
            [&](std::size_t node, std::vector<std::size_t>& out) {
                grid_neighbours(grid, node, diagonal, out);
            },
            [&](std::size_t a, std::size_t b) {
                return diagonal ? edge_cost_8(grid, a, b)
                                : edge_cost_4(grid, a, b);
            },
            [&](std::size_t node, std::size_t target) {
                if (diagonal) {
                    return alg::octile_distance(
                        grid.cell_x(node), grid.cell_y(node),
                        grid.cell_x(target), grid.cell_y(target));
                }
                return static_cast<double>(alg::manhattan_distance(
                    grid.cell_x(node), grid.cell_y(node), grid.cell_x(target),
                    grid.cell_y(target)));
            });
        if (!found.ok()) {
            std::printf("%s: search FAILED\n", name);
        }
    });
}

// --- The four pathfind searches over one 256x256 board ----------------------

alg::grid_map open_grid(std::size_t width, std::size_t height) {
    alg::grid_map grid;
    grid.width = width;
    grid.height = height;
    grid.walkable.assign(width * height, 1);
    return grid;
}

double step_unit(const alg::grid_map&, std::size_t, std::size_t) { return 1.0; }

double h_manhattan(const alg::grid_map& grid, std::size_t a, std::size_t b) {
    const std::size_t ax = a % grid.width;
    const std::size_t ay = a / grid.width;
    const std::size_t bx = b % grid.width;
    const std::size_t by = b / grid.width;
    const std::size_t dx = ax > bx ? ax - bx : bx - ax;
    const std::size_t dy = ay > by ? ay - by : by - ay;
    return static_cast<double>(dx + dy);
}

void run_pathfind() {
    constexpr std::size_t k_size = 256;
    const alg::grid_map grid = open_grid(k_size, k_size);
    const std::size_t start = 0;
    const std::size_t goal = k_size - 1;
    alg::grid_search_config config;
    config.moves = alg::connectivity::four;

    report_ms("pathfind/256x256/four-searches", 5, [&] {
        const auto astar =
            alg::astar_grid(grid, start, goal, step_unit, h_manhattan, config);
        const auto star =
            alg::bstar_grid(grid, start, goal, step_unit, h_manhattan, config);
        const auto bfs = alg::bfs_grid(grid, start, goal, config);
        const auto two_way =
            alg::bidirectional_bfs_grid(grid, start, goal, config);
        if (!astar.ok() || !star.ok() || !bfs.ok() || !two_way.ok()) {
            std::printf("pathfind bench: search FAILED\n");
        }
    });
}

// --- Point-in-polygon and circle-polygon (test_geom sizes) ------------------

std::uint64_t xorshift64(std::uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

geom::real uniform_real(geom::real low, geom::real high,
                        std::uint64_t& state) {
    const double unit =
        static_cast<double>(xorshift64(state) >> 11) / 9007199254740992.0;
    return static_cast<geom::real>(static_cast<double>(low) +
                                   (static_cast<double>(high) -
                                    static_cast<double>(low)) *
                                       unit);
}

void run_geom() {
    std::vector<geom::vec2> vertices;
    vertices.reserve(32);
    for (int i = 0; i < 32; ++i) {
        const geom::real angle =
            geom::k_two_pi * static_cast<geom::real>(i) / 32.0f;
        vertices.push_back(geom::vec2(100.0f * std::cos(angle) + 3.0f,
                                      100.0f * std::sin(angle) - 2.0f));
    }
    const geom::polygon polygon{vertices};
    std::uint64_t state = 0x123456789ULL;
    volatile int sink = 0;

    crpc_bench::measure(
        "geom/10000-point-in-polygon/32-gon", 10000, 5,
        [&](std::size_t iterations) {
            for (std::size_t i = 0; i < iterations; ++i) {
                const geom::vec2 point(uniform_real(-120.0f, 130.0f, state),
                                       uniform_real(-120.0f, 130.0f, state));
                sink += polygon.contains(point) ? 1 : 0;
            }
        });

    crpc_bench::measure(
        "geom/10000-circle-polygon/32-gon", 10000, 5,
        [&](std::size_t iterations) {
            for (std::size_t i = 0; i < iterations; ++i) {
                const geom::vec2 centre(uniform_real(-120.0f, 130.0f, state),
                                        uniform_real(-120.0f, 130.0f, state));
                sink += geom::intersects(geom::circle{centre, 20.0f}, polygon)
                            ? 1
                            : 0;
            }
        });

    std::printf("geom bench: sink=%d\n", static_cast<int>(sink));
    std::fflush(stdout);
}

// --- AOI stress world (test_aoi sizes) --------------------------------------

struct aoi_phase_times {
    bool ok = false;
    double build_ms = 0.0;
    double ticks_ms = 0.0;
    double verify_ms = 0.0;
    std::size_t event_total = 0;
    std::size_t mismatches = 0;
};

bool brute_visible(const aoi::watcher_options& watcher,
                   const aoi::entity_state& entity) {
    const double dx = entity.position.x - watcher.position.x;
    const double dy = entity.position.y - watcher.position.y;
    return dx * dx + dy * dy <= watcher.radius * watcher.radius;
}

aoi::vec2 random_position(std::uint64_t& state, double extent) {
    const double unit = 1.0 / 9007199254740992.0;
    const double x =
        static_cast<double>(xorshift64(state) >> 11) * unit * extent;
    const double y =
        static_cast<double>(xorshift64(state) >> 11) * unit * extent;
    return aoi::vec2{x, y};
}

aoi_phase_times run_aoi_stress() {
    constexpr std::size_t k_entities = 10000;
    constexpr std::size_t k_watchers = 1000;
    constexpr std::size_t k_moved_per_tick = 500;
    constexpr int k_ticks = 20;
    constexpr double k_extent = 1024.0;

    aoi_phase_times times;
    aoi::aoi_options options;
    options.grid.cell_size = 32.0;
    options.grid.columns = 32;
    options.grid.rows = 32;
    auto created = aoi::aoi_world::create(options);
    if (!created.ok()) {
        return times;
    }
    aoi::aoi_world& world = *created.value();

    std::uint64_t rng = 0xC0FFEE123456789ull;
    std::unordered_map<aoi::watcher_id, aoi::watcher_options> watchers;
    std::unordered_map<aoi::entity_id, aoi::entity_state> entities;

    const auto build_start = clock_type::now();
    for (aoi::watcher_id id = 1; id <= k_watchers; ++id) {
        aoi::watcher_options viewer;
        viewer.position = random_position(rng, k_extent);
        viewer.shape = aoi::aoi_shape::circle;
        viewer.radius = 24.0;
        watchers.emplace(id, viewer);
        if (!world.add_watcher(id, viewer).ok()) {
            return times;
        }
    }
    for (aoi::entity_id id = 1; id <= k_entities; ++id) {
        aoi::entity_state state;
        state.position = random_position(rng, k_extent);
        state.attributes = {
            aoi::attribute{1, static_cast<crpc::i64>(id % 7)}};
        entities.emplace(id, state);
        if (!world.add_entity(id, state).ok()) {
            return times;
        }
    }
    world.tick();
    const std::vector<aoi::aoi_event> initial_events = world.drain_events();
    times.event_total += initial_events.size();
    const auto build_end = clock_type::now();

    std::vector<aoi::entity_id> ids;
    ids.reserve(entities.size());
    for (const auto& entry : entities) {
        ids.push_back(entry.first);
    }
    std::sort(ids.begin(), ids.end());

    std::uint64_t engine_ns = 0;
    for (int tick = 0; tick < k_ticks; ++tick) {
        for (std::size_t i = ids.size(); i > 1; --i) {
            const std::size_t j =
                static_cast<std::size_t>(xorshift64(rng) % i);
            std::swap(ids[i - 1], ids[j]);
        }
        const std::vector<aoi::entity_id> moved(
            ids.begin(), ids.begin() + k_moved_per_tick);

        for (aoi::entity_id id : moved) {
            aoi::entity_state& state = entities.at(id);
            state.position = random_position(rng, k_extent);
        }

        const auto tick_start = clock_type::now();
        for (aoi::entity_id id : moved) {
            if (!world.update_entity(id, entities.at(id)).ok()) {
                return times;
            }
        }
        world.tick();
        const std::vector<aoi::aoi_event> events = world.drain_events();
        times.event_total += events.size();
        const auto tick_end = clock_type::now();
        engine_ns += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(tick_end -
                                                                 tick_start)
                .count());
    }

    const auto verify_start = clock_type::now();
    for (const auto& watcher : watchers) {
        std::unordered_set<aoi::entity_id> expected;
        for (const auto& entity : entities) {
            if (brute_visible(watcher.second, entity.second)) {
                expected.insert(entity.first);
            }
        }
        const auto listed = world.visible(watcher.first);
        if (!listed.ok()) {
            ++times.mismatches;
            continue;
        }
        const std::vector<aoi::entity_id> got = listed.value();
        if (got.size() != expected.size()) {
            ++times.mismatches;
            continue;
        }
        for (aoi::entity_id id : got) {
            if (expected.count(id) == 0) {
                ++times.mismatches;
                break;
            }
        }
    }
    const auto verify_end = clock_type::now();

    times.ok = true;
    times.build_ms = elapsed_ms(build_start, build_end);
    times.ticks_ms = static_cast<double>(engine_ns) / 1.0e6;
    times.verify_ms = elapsed_ms(verify_start, verify_end);
    return times;
}

double median_of(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

double spread_of(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return values.back() - values.front();
}

void run_aoi() {
    constexpr int k_runs = 3;
    std::vector<double> build;
    std::vector<double> ticks;
    std::vector<double> verify;
    std::size_t event_total = 0;
    std::size_t mismatches = 0;
    for (int run = 0; run <= k_runs; ++run) {
        const aoi_phase_times times = run_aoi_stress();
        if (!times.ok) {
            std::printf("aoi bench: setup FAILED\n");
            return;
        }
        event_total = times.event_total;
        mismatches = std::max(mismatches, times.mismatches);
        if (run == 0) {
            continue; // Warm-up.
        }
        build.push_back(times.build_ms);
        ticks.push_back(times.ticks_ms);
        verify.push_back(times.verify_ms);
    }
    crpc_bench::record("aoi/build/10000-entities-1000-watchers", "ms",
                       median_of(build), spread_of(build));
    crpc_bench::record("aoi/ticks/20-ticks-500-moves", "ms", median_of(ticks),
                       spread_of(ticks));
    crpc_bench::record("aoi/verify/10000-entities-1000-watchers", "ms",
                       median_of(verify), spread_of(verify));
    std::printf("aoi bench: events=%zu verify-mismatches=%zu\n", event_total,
                mismatches);
    std::fflush(stdout);
}

// --- CART training (test_game_ai_decision_tree size) -----------------------

void run_decision_tree() {
    constexpr std::size_t k_rows = 10000;
    constexpr std::size_t k_cols = 20;

    ai::decision_tree_dataset data;
    data.cols = k_cols;
    data.features.reserve(k_rows * k_cols);
    data.targets.reserve(k_rows);

    std::uint64_t state = 88172645463325252ull;
    auto next_unit = [&state]() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<double>(state >> 11) * (1.0 / 9007199254740992.0);
    };
    for (std::size_t row = 0; row < k_rows; ++row) {
        double sum = 0.0;
        for (std::size_t col = 0; col < k_cols; ++col) {
            const double value = next_unit();
            data.features.push_back(value);
            if (col < 3) {
                sum += value;
            }
        }
        data.targets.push_back(sum > 1.5 ? 1.0 : 0.0);
    }
    data.rows = k_rows;

    ai::decision_tree_config config;
    config.max_depth = 8;
    config.min_samples_leaf = 5;
    config.min_gain = 1e-7;

    report_ms("decision-tree/train/10000x20", 3, [&] {
        ai::decision_tree tree(config);
        const crpc::status trained = tree.train(data);
        if (!trained.ok()) {
            std::printf("decision-tree bench: training FAILED\n");
        }
    });
}

} // namespace

void run_all_cases() {
    if (crpc_bench::enabled("astar")) {
        run_open_area(200, false, "astar/200x200-4-connected");
        run_open_area(150, true, "astar/150x150-8-connected");
    }
    if (crpc_bench::enabled("pathfind")) {
        run_pathfind();
    }
    if (crpc_bench::enabled("geom")) {
        run_geom();
    }
    if (crpc_bench::enabled("aoi")) {
        run_aoi();
    }
    if (crpc_bench::enabled("decision-tree")) {
        run_decision_tree();
    }
}

CRPC_BENCH_MAIN()
