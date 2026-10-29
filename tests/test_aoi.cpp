// CRPC -- tests for crpc/game/aoi: grid partitioning, visibility diffing and
// nine-grid forwarding.
//
// The correctness gate is a differential comparison against a brute-force
// O(n^2) reference: for every watcher we recompute the visible set by testing
// every live entity with inclusive geometry, the same filter callable and the
// same self-exclusion rule. The comparison runs over 20 random seeds with
// adds, removes, moves, attribute changes and watcher motion between ticks.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <limits>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <crpc/game/aoi/aoi.hpp>

#include "test_framework.hpp"

namespace {

using namespace crpc::game::aoi;
using crpc::i64;
using crpc::rpc_code;
using crpc::status;
using crpc::u32;
using crpc::u64;

// --- Deterministic randomness ------------------------------------------------

std::uint64_t xorshift64(std::uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

double random_unit(std::uint64_t& state) {
    return static_cast<double>(xorshift64(state) >> 11) *
           (1.0 / 9007199254740992.0);
}

double random_range(std::uint64_t& state, double low, double high) {
    return low + (high - low) * random_unit(state);
}

vec2 random_position(std::uint64_t& state, double extent) {
    return vec2{random_range(state, 0.0, extent),
                random_range(state, 0.0, extent)};
}

// --- State helpers -----------------------------------------------------------

entity_state make_state(vec2 position,
                        std::vector<attribute> attributes = {}) {
    entity_state state;
    state.position = position;
    state.attributes = std::move(attributes);
    return state;
}

bool attributes_equal(const std::vector<attribute>& a,
                      const std::vector<attribute>& b) {
    return a == b;
}

bool state_equal(const entity_state& a, const entity_state& b) {
    return a.position == b.position &&
           attributes_equal(a.attributes, b.attributes);
}

state_delta make_delta(const entity_state& before, const entity_state& after) {
    state_delta delta;
    delta.position_changed = before.position != after.position;
    delta.position = after.position;

    std::size_t i = 0;
    std::size_t j = 0;
    while (i < before.attributes.size() || j < after.attributes.size()) {
        if (j >= after.attributes.size() ||
            (i < before.attributes.size() &&
             before.attributes[i].id < after.attributes[j].id)) {
            delta.attributes_removed.push_back(before.attributes[i].id);
            ++i;
        } else if (i >= before.attributes.size() ||
                   after.attributes[j].id < before.attributes[i].id) {
            delta.attributes_set.push_back(after.attributes[j]);
            ++j;
        } else {
            if (before.attributes[i].value != after.attributes[j].value) {
                delta.attributes_set.push_back(after.attributes[j]);
            }
            ++i;
            ++j;
        }
    }
    return delta;
}

bool delta_equal(const state_delta& a, const state_delta& b) {
    return a.position_changed == b.position_changed && a.position == b.position &&
           attributes_equal(a.attributes_set, b.attributes_set) &&
           a.attributes_removed == b.attributes_removed;
}

entity_state apply_delta(entity_state state, const state_delta& delta) {
    if (delta.position_changed) {
        state.position = delta.position;
    }

    std::vector<attribute> merged;
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < state.attributes.size() || j < delta.attributes_set.size()) {
        if (j >= delta.attributes_set.size() ||
            (i < state.attributes.size() &&
             state.attributes[i].id < delta.attributes_set[j].id)) {
            merged.push_back(state.attributes[i]);
            ++i;
        } else if (i >= state.attributes.size() ||
                   delta.attributes_set[j].id < state.attributes[i].id) {
            merged.push_back(delta.attributes_set[j]);
            ++j;
        } else {
            merged.push_back(delta.attributes_set[j]);
            ++i;
            ++j;
        }
    }
    state.attributes.clear();
    for (const attribute& value : merged) {
        const bool removed = std::binary_search(delta.attributes_removed.begin(),
                                                delta.attributes_removed.end(),
                                                value.id);
        if (!removed) {
            state.attributes.push_back(value);
        }
    }
    return state;
}

// --- Brute-force reference ---------------------------------------------------

struct ref_entity {
    entity_state state;
    u32 faction = 0;
    u32 instance = 0;
};

struct ref_watcher {
    watcher_options options;
    std::unordered_set<entity_id> visible;
    std::unordered_map<entity_id, entity_state> sent;
};

bool brute_visible(watcher_id id, const watcher_options& options,
                   entity_id entity, const ref_entity& value) {
    if (options.has_entity && options.entity == entity) {
        return false;
    }

    bool inside = false;
    if (options.shape == aoi_shape::circle) {
        const double dx = value.state.position.x - options.position.x;
        const double dy = value.state.position.y - options.position.y;
        inside = dx * dx + dy * dy <= options.radius * options.radius;
    } else {
        inside = std::fabs(value.state.position.x - options.position.x) <=
                     options.half_width &&
                 std::fabs(value.state.position.y - options.position.y) <=
                     options.half_height;
    }
    if (!inside) {
        return false;
    }

    if (options.filter) {
        const watcher_info watcher{id, options.faction, options.instance};
        const entity_info target{entity, value.faction, value.instance};
        if (!options.filter(watcher, target)) {
            return false;
        }
    }
    return true;
}

std::size_t visible_mismatches(watcher_id id, const ref_watcher& watcher,
                               const std::unordered_map<entity_id, ref_entity>&
                                   entities) {
    std::size_t mismatches = 0;
    std::unordered_set<entity_id> expected;
    for (const auto& entry : entities) {
        if (brute_visible(id, watcher.options, entry.first, entry.second)) {
            expected.insert(entry.first);
            if (watcher.visible.count(entry.first) == 0) {
                ++mismatches;
                std::printf("    MISSING w=%llu e=%llu epos=(%.3f,%.3f) "
                            "wpos=(%.3f,%.3f) shape=%d r=%.3f hw=%.3f hh=%.3f "
                            "self=%d %llu\n",
                            static_cast<unsigned long long>(id),
                            static_cast<unsigned long long>(entry.first),
                            entry.second.state.position.x,
                            entry.second.state.position.y,
                            watcher.options.position.x, watcher.options.position.y,
                            static_cast<int>(watcher.options.shape),
                            watcher.options.radius, watcher.options.half_width,
                            watcher.options.half_height,
                            watcher.options.has_entity ? 1 : 0,
                            static_cast<unsigned long long>(watcher.options.entity));
            }
        } else if (watcher.visible.count(entry.first) != 0) {
            ++mismatches;
            std::printf("    EXTRA w=%llu e=%llu epos=(%.3f,%.3f) "
                        "wpos=(%.3f,%.3f) shape=%d r=%.3f hw=%.3f hh=%.3f "
                        "self=%d %llu\n",
                        static_cast<unsigned long long>(id),
                        static_cast<unsigned long long>(entry.first),
                        entry.second.state.position.x,
                        entry.second.state.position.y,
                        watcher.options.position.x, watcher.options.position.y,
                        static_cast<int>(watcher.options.shape),
                        watcher.options.radius, watcher.options.half_width,
                        watcher.options.half_height,
                        watcher.options.has_entity ? 1 : 0,
                        static_cast<unsigned long long>(watcher.options.entity));
        }
    }
    for (entity_id entity : watcher.visible) {
        if (entities.count(entity) == 0) {
            ++mismatches;
        }
    }
    return mismatches;
}

void check_and_apply_events(
    const std::vector<aoi_event>& events,
    std::unordered_map<watcher_id, ref_watcher>& watchers,
    const std::unordered_map<entity_id, ref_entity>& entities) {
    std::set<std::pair<watcher_id, entity_id>> seen;
    for (const aoi_event& event : events) {
        CRPC_CHECK(seen.insert({event.watcher, event.entity}).second);

        auto watcher_it = watchers.find(event.watcher);
        CRPC_CHECK(watcher_it != watchers.end());
        if (watcher_it == watchers.end()) {
            continue;
        }
        ref_watcher& watcher = watcher_it->second;

        auto entity_it = entities.find(event.entity);
        switch (event.kind) {
        case aoi_event_kind::enter: {
            CRPC_CHECK(watcher.visible.count(event.entity) == 0);
            CRPC_CHECK(event.delta.attributes_set.empty());
            CRPC_CHECK_FALSE(event.delta.position_changed);
            CRPC_CHECK(entity_it != entities.end());
            if (entity_it != entities.end()) {
                CRPC_CHECK(state_equal(event.state, entity_it->second.state));
                CRPC_CHECK_EQ(static_cast<unsigned>(entity_it->second.faction),
                              static_cast<unsigned>(event.faction));
                CRPC_CHECK_EQ(static_cast<unsigned>(entity_it->second.instance),
                              static_cast<unsigned>(event.instance));
                CRPC_CHECK(brute_visible(event.watcher, watcher.options,
                                         event.entity, entity_it->second));
                watcher.visible.insert(event.entity);
                watcher.sent[event.entity] = event.state;
            }
            break;
        }
        case aoi_event_kind::leave: {
            CRPC_CHECK_EQ(static_cast<std::size_t>(1),
                          watcher.visible.erase(event.entity));
            CRPC_CHECK(event.state.attributes.empty());
            watcher.sent.erase(event.entity);
            if (entity_it != entities.end()) {
                CRPC_CHECK_FALSE(brute_visible(event.watcher, watcher.options,
                                               event.entity,
                                               entity_it->second));
            }
            break;
        }
        case aoi_event_kind::update: {
            CRPC_CHECK_EQ(static_cast<std::size_t>(1),
                          watcher.visible.count(event.entity));
            CRPC_CHECK(event.state.attributes.empty());
            CRPC_CHECK(entity_it != entities.end());
            if (entity_it != entities.end()) {
                auto sent_it = watcher.sent.find(event.entity);
                CRPC_CHECK(sent_it != watcher.sent.end());
                if (sent_it != watcher.sent.end()) {
                    const state_delta expected =
                        make_delta(sent_it->second, entity_it->second.state);
                    CRPC_CHECK(delta_equal(expected, event.delta));
                    const entity_state applied =
                        apply_delta(sent_it->second, event.delta);
                    CRPC_CHECK(state_equal(applied, entity_it->second.state));
                    watcher.sent[event.entity] = entity_it->second.state;
                }
                CRPC_CHECK(brute_visible(event.watcher, watcher.options,
                                         event.entity, entity_it->second));
            }
            break;
        }
        }
    }
}

std::unordered_map<watcher_id, ref_watcher>
make_reference(const std::unordered_map<watcher_id, watcher_options>& options) {
    std::unordered_map<watcher_id, ref_watcher> watchers;
    for (const auto& entry : options) {
        ref_watcher watcher;
        watcher.options = entry.second;
        watchers.emplace(entry.first, std::move(watcher));
    }
    return watchers;
}

// --- Grid tests --------------------------------------------------------------

bool has_cell(const std::vector<cell_coord>& cells, std::size_t column,
              std::size_t row) {
    return std::find(cells.begin(), cells.end(), cell_coord{column, row}) !=
           cells.end();
}

CRPC_TEST(aoi_grid_rectangle_cells) {
    grid_options options;
    options.cell_size = 10.0;
    options.columns = 10;
    options.rows = 10;
    auto created = aoi_grid::create(options);
    CRPC_CHECK(created.ok());
    if (!created.ok()) {
        return;
    }
    const aoi_grid& grid = created.value();

    auto point = grid.rectangle_cells(vec2{5.0, 5.0}, 0.0, 0.0);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), point.size());
    CRPC_CHECK(has_cell(point, 0, 0));

    auto corner = grid.rectangle_cells(vec2{5.0, 5.0}, 5.0, 5.0);
    CRPC_CHECK_EQ(static_cast<std::size_t>(4), corner.size());
    CRPC_CHECK(has_cell(corner, 0, 0));
    CRPC_CHECK(has_cell(corner, 1, 0));
    CRPC_CHECK(has_cell(corner, 0, 1));
    CRPC_CHECK(has_cell(corner, 1, 1));

    auto one = grid.rectangle_cells(vec2{15.0, 15.0}, 4.0, 4.0);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), one.size());
    CRPC_CHECK(has_cell(one, 1, 1));

    auto straddle = grid.rectangle_cells(vec2{9.0, 10.0}, 1.0, 0.0);
    CRPC_CHECK_EQ(static_cast<std::size_t>(2), straddle.size());
    CRPC_CHECK(has_cell(straddle, 0, 1));
    CRPC_CHECK(has_cell(straddle, 1, 1));

    auto edge = grid.rectangle_cells(vec2{99.0, 99.0}, 1.0, 1.0);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), edge.size());
    CRPC_CHECK(has_cell(edge, 9, 9));

    CRPC_CHECK(grid.rectangle_cells(vec2{100.001, 5.0}, 1.0, 1.0).empty());
    CRPC_CHECK(grid.rectangle_cells(vec2{5.0, 5.0}, -1.0, 1.0).empty());
}

CRPC_TEST(aoi_grid_circle_cells) {
    grid_options options;
    options.cell_size = 10.0;
    options.columns = 10;
    options.rows = 10;
    auto created = aoi_grid::create(options);
    CRPC_CHECK(created.ok());
    if (!created.ok()) {
        return;
    }
    const aoi_grid& grid = created.value();

    auto zero = grid.circle_cells(vec2{5.0, 5.0}, 0.0);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), zero.size());
    CRPC_CHECK(has_cell(zero, 0, 0));

    auto small = grid.circle_cells(vec2{5.0, 5.0}, 1.0);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), small.size());
    CRPC_CHECK(has_cell(small, 0, 0));

    // The bounding box reaches cells (1,0), (0,1) and (1,1); the far corner
    // cell is 7.07 away from the centre, so it does not intersect this disc.
    auto disc = grid.circle_cells(vec2{5.0, 5.0}, 6.0);
    CRPC_CHECK_EQ(static_cast<std::size_t>(3), disc.size());
    CRPC_CHECK(has_cell(disc, 0, 0));
    CRPC_CHECK(has_cell(disc, 1, 0));
    CRPC_CHECK(has_cell(disc, 0, 1));
    CRPC_CHECK_FALSE(has_cell(disc, 1, 1));

    auto wider = grid.circle_cells(vec2{5.0, 5.0}, 7.1);
    CRPC_CHECK_EQ(static_cast<std::size_t>(4), wider.size());
    CRPC_CHECK(has_cell(wider, 1, 1));

    auto far_edge = grid.circle_cells(vec2{100.0, 100.0}, 5.0);
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), far_edge.size());
    CRPC_CHECK(has_cell(far_edge, 9, 9));

    CRPC_CHECK(grid.circle_cells(vec2{5.0, 5.0}, -1.0).empty());
}

CRPC_TEST(aoi_grid_validation) {
    grid_options options;
    options.cell_size = 0.0;
    CRPC_CHECK_FALSE(aoi_grid::create(options).ok());

    options.cell_size = std::numeric_limits<double>::quiet_NaN();
    CRPC_CHECK_FALSE(aoi_grid::create(options).ok());

    options = grid_options{};
    options.columns = 0;
    CRPC_CHECK_FALSE(aoi_grid::create(options).ok());

    options = grid_options{};
    options.origin_x = std::numeric_limits<double>::infinity();
    CRPC_CHECK_FALSE(aoi_grid::create(options).ok());

    options = grid_options{};
    options.cell_size = 10.0;
    options.columns = 10;
    options.rows = 10;
    auto created = aoi_grid::create(options);
    CRPC_CHECK(created.ok());
    if (!created.ok()) {
        return;
    }
    const aoi_grid& grid = created.value();

    CRPC_CHECK(grid.contains(vec2{0.0, 0.0}));
    CRPC_CHECK(grid.contains(vec2{100.0, 100.0}));
    CRPC_CHECK_FALSE(grid.contains(vec2{100.001, 100.0}));
    CRPC_CHECK_FALSE(grid.contains(vec2{-0.001, 0.0}));
    CRPC_CHECK_FALSE(grid.contains(vec2{std::numeric_limits<double>::quiet_NaN(),
                                        0.0}));

    auto far_cell = grid.cell_of(vec2{100.0, 100.0});
    CRPC_CHECK(far_cell.ok());
    if (far_cell.ok()) {
        CRPC_CHECK(far_cell.value() == (cell_coord{9, 9}));
    }
    CRPC_CHECK_FALSE(grid.cell_of(vec2{0.0, 101.0}).ok());

    auto middle = grid.cell_of(vec2{10.0, 20.0});
    CRPC_CHECK(middle.ok());
    if (middle.ok()) {
        CRPC_CHECK(middle.value() == (cell_coord{1, 2}));
    }
}

// --- World tests -------------------------------------------------------------

bool status_ok(const status& value) {
    if (!value.ok()) {
        std::printf("  operation failed: %s\n", value.message().c_str());
    }
    return value.ok();
}

CRPC_TEST(aoi_world_validation) {
    aoi_options options;
    options.grid.cell_size = 0.0;
    CRPC_CHECK_FALSE(aoi_world::create(options).ok());

    options = aoi_options{};
    options.grid.cell_size = 10.0;
    options.grid.columns = 10;
    options.grid.rows = 10;
    auto created = aoi_world::create(options);
    CRPC_CHECK(created.ok());
    if (!created.ok()) {
        return;
    }
    aoi_world& world = *created.value();

    CRPC_CHECK(status_ok(world.add_entity(1, make_state(vec2{5.0, 5.0}))));
    CRPC_CHECK_EQ(static_cast<int>(rpc_code::failed_precondition),
                  static_cast<int>(
                      world.add_entity(1, make_state(vec2{5.0, 5.0})).code()));
    CRPC_CHECK_EQ(static_cast<int>(rpc_code::invalid_argument),
                  static_cast<int>(
                      world.add_entity(2, make_state(vec2{500.0, 5.0})).code()));

    entity_state duplicate;
    duplicate.position = vec2{5.0, 5.0};
    duplicate.attributes.push_back(attribute{1, 2});
    duplicate.attributes.push_back(attribute{1, 3});
    CRPC_CHECK_EQ(static_cast<int>(rpc_code::invalid_argument),
                  static_cast<int>(world.add_entity(3, duplicate).code()));

    CRPC_CHECK_EQ(static_cast<int>(rpc_code::not_found),
                  static_cast<int>(
                      world.update_entity(99, make_state(vec2{5.0, 5.0}))
                          .code()));
    CRPC_CHECK_EQ(
        static_cast<int>(rpc_code::not_found),
        static_cast<int>(world.remove_entity(99).code()));

    watcher_options watcher;
    watcher.position = vec2{5.0, 5.0};
    watcher.shape = aoi_shape::circle;
    watcher.radius = 11.0; // larger than cell_size
    CRPC_CHECK_EQ(static_cast<int>(rpc_code::invalid_argument),
                  static_cast<int>(world.add_watcher(1, watcher).code()));

    watcher.shape = aoi_shape::rectangle;
    watcher.half_width = 11.0;
    watcher.half_height = 1.0;
    CRPC_CHECK_EQ(static_cast<int>(rpc_code::invalid_argument),
                  static_cast<int>(world.add_watcher(1, watcher).code()));

    watcher.half_width = 1.0;
    CRPC_CHECK(status_ok(world.add_watcher(1, watcher)));
    CRPC_CHECK_EQ(static_cast<int>(rpc_code::failed_precondition),
                  static_cast<int>(world.add_watcher(1, watcher).code()));
    CRPC_CHECK_EQ(static_cast<int>(rpc_code::not_found),
                  static_cast<int>(world.move_watcher(7, vec2{5.0, 5.0}).code()));
}

CRPC_TEST(aoi_enter_update_leave_delta) {
    aoi_options options;
    options.grid.cell_size = 10.0;
    options.grid.columns = 10;
    options.grid.rows = 10;
    auto created = aoi_world::create(options);
    CRPC_CHECK(created.ok());
    if (!created.ok()) {
        return;
    }
    aoi_world& world = *created.value();

    watcher_options viewer;
    viewer.position = vec2{50.0, 50.0};
    viewer.shape = aoi_shape::circle;
    viewer.radius = 10.0;
    CRPC_CHECK(status_ok(world.add_watcher(7, viewer)));

    CRPC_CHECK(status_ok(
        world.add_entity(1, make_state(vec2{59.0, 50.0}, {attribute{1, 7}}))));
    world.tick();
    auto events = world.drain_events();
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), events.size());
    if (events.size() == 1) {
        const aoi_event& event = events[0];
        CRPC_CHECK(event.kind == aoi_event_kind::enter);
        CRPC_CHECK_EQ(static_cast<u64>(7), event.watcher);
        CRPC_CHECK_EQ(static_cast<u64>(1), event.entity);
        CRPC_CHECK(event.state.position == (vec2{59.0, 50.0}));
        CRPC_CHECK_EQ(static_cast<std::size_t>(1), event.state.attributes.size());
        CRPC_CHECK(event.state.attributes[0] == (attribute{1, 7}));
    }

    CRPC_CHECK(status_ok(world.update_entity(
        1, make_state(vec2{55.0, 50.0}, {attribute{1, 9}, attribute{2, 3}}))));
    world.tick();
    events = world.drain_events();
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), events.size());
    if (events.size() == 1) {
        const aoi_event& event = events[0];
        CRPC_CHECK(event.kind == aoi_event_kind::update);
        CRPC_CHECK(event.delta.position_changed);
        CRPC_CHECK(event.delta.position == (vec2{55.0, 50.0}));
        CRPC_CHECK_EQ(static_cast<std::size_t>(2),
                      event.delta.attributes_set.size());
        CRPC_CHECK(event.delta.attributes_set[0] == (attribute{1, 9}));
        CRPC_CHECK(event.delta.attributes_set[1] == (attribute{2, 3}));
        CRPC_CHECK(event.delta.attributes_removed.empty());
        CRPC_CHECK(event.state.attributes.empty());
    }

    CRPC_CHECK(status_ok(world.update_entity(
        1, make_state(vec2{55.0, 50.0}, {attribute{2, 3}}))));
    world.tick();
    events = world.drain_events();
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), events.size());
    if (events.size() == 1) {
        const aoi_event& event = events[0];
        CRPC_CHECK(event.kind == aoi_event_kind::update);
        CRPC_CHECK_FALSE(event.delta.position_changed);
        CRPC_CHECK(event.delta.attributes_set.empty());
        CRPC_CHECK_EQ(static_cast<std::size_t>(1),
                      event.delta.attributes_removed.size());
        CRPC_CHECK_EQ(static_cast<field_id>(1),
                      event.delta.attributes_removed[0]);
    }

    CRPC_CHECK(status_ok(world.update_entity(
        1, make_state(vec2{70.0, 50.0}, {attribute{2, 3}}))));
    world.tick();
    events = world.drain_events();
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), events.size());
    if (events.size() == 1) {
        CRPC_CHECK(events[0].kind == aoi_event_kind::leave);
        CRPC_CHECK(events[0].state.attributes.empty());
    }

    auto visible = world.visible(7);
    CRPC_CHECK(visible.ok());
    if (visible.ok()) {
        CRPC_CHECK(visible.value().empty());
    }
}

CRPC_TEST(aoi_tick_collapses_repeated_moves) {
    aoi_options options;
    options.grid.cell_size = 32.0;
    options.grid.columns = 30;
    options.grid.rows = 30;
    auto created = aoi_world::create(options);
    CRPC_CHECK(created.ok());
    if (!created.ok()) {
        return;
    }
    aoi_world& world = *created.value();

    watcher_options viewer;
    viewer.position = vec2{50.0, 50.0};
    viewer.shape = aoi_shape::circle;
    viewer.radius = 20.0;
    CRPC_CHECK(status_ok(world.add_watcher(1, viewer)));
    CRPC_CHECK(status_ok(world.add_entity(1, make_state(vec2{50.0, 50.0}))));

    CRPC_CHECK(status_ok(world.update_entity(1, make_state(vec2{52.0, 50.0}))));
    CRPC_CHECK(status_ok(world.update_entity(1, make_state(vec2{54.0, 50.0}))));
    CRPC_CHECK(status_ok(world.update_entity(1, make_state(vec2{56.0, 50.0}))));
    world.tick();
    auto events = world.drain_events();
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), events.size());
    if (events.size() == 1) {
        CRPC_CHECK(events[0].kind == aoi_event_kind::enter);
        CRPC_CHECK(events[0].state.position == (vec2{56.0, 50.0}));
    }

    CRPC_CHECK(status_ok(world.update_entity(1, make_state(vec2{58.0, 50.0}))));
    CRPC_CHECK(status_ok(world.update_entity(1, make_state(vec2{60.0, 50.0}))));
    CRPC_CHECK(status_ok(world.update_entity(1, make_state(vec2{62.0, 50.0}))));
    world.tick();
    events = world.drain_events();
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), events.size());
    if (events.size() == 1) {
        CRPC_CHECK(events[0].kind == aoi_event_kind::update);
        CRPC_CHECK(events[0].delta.position == (vec2{62.0, 50.0}));
    }

    // Out and back in during one tick: the final state differs from the last
    // emitted state, so the result is one update, never leave+enter.
    CRPC_CHECK(status_ok(world.update_entity(1, make_state(vec2{200.0, 200.0}))));
    CRPC_CHECK(status_ok(world.update_entity(1, make_state(vec2{60.0, 50.0}))));
    world.tick();
    events = world.drain_events();
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), events.size());
    if (events.size() == 1) {
        CRPC_CHECK(events[0].kind == aoi_event_kind::update);
        CRPC_CHECK(events[0].delta.position == (vec2{60.0, 50.0}));
    }

    // Out and back to exactly the last emitted state: no event at all.
    CRPC_CHECK(status_ok(world.update_entity(1, make_state(vec2{200.0, 200.0}))));
    CRPC_CHECK(status_ok(world.update_entity(1, make_state(vec2{60.0, 50.0}))));
    world.tick();
    CRPC_CHECK(world.drain_events().empty());

    CRPC_CHECK(status_ok(world.update_entity(1, make_state(vec2{200.0, 200.0}))));
    CRPC_CHECK(status_ok(world.update_entity(1, make_state(vec2{200.0, 200.0}))));
    world.tick();
    events = world.drain_events();
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), events.size());
    if (events.size() == 1) {
        CRPC_CHECK(events[0].kind == aoi_event_kind::leave);
    }

    // A watcher that moves several times in one tick is reconciled once, at
    // its final position.
    watcher_options watcher_2;
    watcher_2.position = vec2{50.0, 50.0};
    watcher_2.shape = aoi_shape::circle;
    watcher_2.radius = 10.0;
    CRPC_CHECK(status_ok(world.add_watcher(2, watcher_2)));
    CRPC_CHECK(status_ok(world.add_entity(2, make_state(vec2{75.0, 50.0}))));
    world.tick();
    CRPC_CHECK(world.drain_events().empty()); // entity 2 is outside watcher 2
    CRPC_CHECK(status_ok(world.move_watcher(2, vec2{60.0, 50.0})));
    CRPC_CHECK(status_ok(world.move_watcher(2, vec2{70.0, 50.0})));
world.tick();
    events = world.drain_events();
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), events.size());
    if (events.size() == 1) {
        CRPC_CHECK(events[0].kind == aoi_event_kind::enter);
        CRPC_CHECK_EQ(static_cast<u64>(2), events[0].watcher);
        CRPC_CHECK_EQ(static_cast<u64>(2), events[0].entity);
    }
}

CRPC_TEST(aoi_enter_leave_pairing) {
    aoi_options options;
    options.grid.cell_size = 8.0;
    options.grid.columns = 16;
    options.grid.rows = 16;
    auto created = aoi_world::create(options);
    CRPC_CHECK(created.ok());
    if (!created.ok()) {
        return;
    }
    aoi_world& world = *created.value();

    std::uint64_t rng = 0x1234567812345678ull;
    for (watcher_id id = 1; id <= 12; ++id) {
        watcher_options viewer;
        viewer.position = random_position(rng, 128.0);
        viewer.shape = (id % 2 == 0) ? aoi_shape::circle : aoi_shape::rectangle;
        viewer.radius = random_range(rng, 0.0, 8.0);
        viewer.half_width = random_range(rng, 0.0, 8.0);
        viewer.half_height = random_range(rng, 0.0, 8.0);
        CRPC_CHECK(status_ok(world.add_watcher(id, viewer)));
    }
    std::unordered_map<entity_id, ref_entity> entities;
    for (entity_id id = 1; id <= 40; ++id) {
        ref_entity entity;
        entity.state = make_state(random_position(rng, 128.0),
                                  {attribute{1, static_cast<i64>(id)}});
        entity.faction = static_cast<u32>(id % 3);
        entity.instance = static_cast<u32>(id % 2);
        entities.emplace(id, entity);
        CRPC_CHECK(status_ok(world.add_entity(id, entity.state, entity.faction,
                                              entity.instance)));
    }

    world.tick();
    auto first = world.drain_events();
    std::unordered_map<watcher_id, std::size_t> enters;
    std::unordered_map<watcher_id, std::size_t> leaves;
    for (const aoi_event& event : first) {
        CRPC_CHECK(event.kind == aoi_event_kind::enter);
        ++enters[event.watcher];
    }

    for (entity_id id = 1; id <= 40; ++id) {
        CRPC_CHECK(status_ok(world.remove_entity(id)));
    }
    auto second = world.drain_events();
    for (const aoi_event& event : second) {
        CRPC_CHECK(event.kind == aoi_event_kind::leave);
        ++leaves[event.watcher];
    }

    for (watcher_id id = 1; id <= 12; ++id) {
        CRPC_CHECK_EQ(enters[id], leaves[id]);
        auto visible = world.visible(id);
        CRPC_CHECK(visible.ok());
        if (visible.ok()) {
            CRPC_CHECK(visible.value().empty());
        }
    }
}

CRPC_TEST(aoi_self_is_never_visible) {
    aoi_options options;
    options.grid.cell_size = 10.0;
    options.grid.columns = 10;
    options.grid.rows = 10;
    auto created = aoi_world::create(options);
    CRPC_CHECK(created.ok());
    if (!created.ok()) {
        return;
    }
    aoi_world& world = *created.value();

    watcher_options viewer;
    viewer.position = vec2{50.0, 50.0};
    viewer.shape = aoi_shape::circle;
    viewer.radius = 10.0;
    viewer.has_entity = true;
    viewer.entity = 1;
    CRPC_CHECK(status_ok(world.add_watcher(1, viewer)));
    CRPC_CHECK(status_ok(world.add_entity(1, make_state(vec2{50.0, 50.0}))));
    CRPC_CHECK(status_ok(world.add_entity(2, make_state(vec2{50.0, 50.0}))));

    world.tick();
    auto events = world.drain_events();
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), events.size());
    if (events.size() == 1) {
        CRPC_CHECK_EQ(static_cast<u64>(2), events[0].entity);
    }
    auto visible = world.visible(1);
    CRPC_CHECK(visible.ok());
    if (visible.ok()) {
        CRPC_CHECK_EQ(static_cast<std::size_t>(1), visible.value().size());
        if (!visible.value().empty()) {
            CRPC_CHECK_EQ(static_cast<u64>(2), visible.value()[0]);
        }
    }
}

CRPC_TEST(aoi_teleport_across_cells) {
    aoi_options options;
    options.grid.cell_size = 10.0;
    options.grid.columns = 20;
    options.grid.rows = 20;
    auto created = aoi_world::create(options);
    CRPC_CHECK(created.ok());
    if (!created.ok()) {
        return;
    }
    aoi_world& world = *created.value();

    watcher_options first;
    first.position = vec2{10.0, 10.0};
    first.shape = aoi_shape::circle;
    first.radius = 5.0;
    watcher_options second;
    second.position = vec2{190.0, 190.0};
    second.shape = aoi_shape::circle;
    second.radius = 5.0;
    CRPC_CHECK(status_ok(world.add_watcher(1, first)));
    CRPC_CHECK(status_ok(world.add_watcher(2, second)));
    CRPC_CHECK(status_ok(world.add_entity(1, make_state(vec2{10.0, 10.0}))));

    world.tick();
    auto events = world.drain_events();
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), events.size());
    if (events.size() == 1) {
        CRPC_CHECK(events[0].kind == aoi_event_kind::enter);
        CRPC_CHECK_EQ(static_cast<u64>(1), events[0].watcher);
    }

    CRPC_CHECK(status_ok(world.update_entity(1, make_state(vec2{190.0, 190.0}))));
    world.tick();
    events = world.drain_events();
    CRPC_CHECK_EQ(static_cast<std::size_t>(2), events.size());
    bool saw_leave = false;
    bool saw_enter = false;
    for (const aoi_event& event : events) {
        if (event.kind == aoi_event_kind::leave && event.watcher == 1) {
            saw_leave = true;
        }
        if (event.kind == aoi_event_kind::enter && event.watcher == 2) {
            saw_enter = true;
        }
    }
    CRPC_CHECK(saw_leave);
    CRPC_CHECK(saw_enter);

    auto first_visible = world.visible(1);
    auto second_visible = world.visible(2);
    CRPC_CHECK(first_visible.ok());
    CRPC_CHECK(second_visible.ok());
    if (first_visible.ok()) {
        CRPC_CHECK(first_visible.value().empty());
    }
    if (second_visible.ok()) {
        CRPC_CHECK_EQ(static_cast<std::size_t>(1), second_visible.value().size());
    }
}

CRPC_TEST(aoi_many_entities_in_one_cell) {
    aoi_options options;
    options.grid.cell_size = 20.0;
    options.grid.columns = 10;
    options.grid.rows = 10;
    auto created = aoi_world::create(options);
    CRPC_CHECK(created.ok());
    if (!created.ok()) {
        return;
    }
    aoi_world& world = *created.value();

    watcher_options viewer;
    viewer.position = vec2{50.5, 50.5};
    viewer.shape = aoi_shape::circle;
    viewer.radius = 5.0;
    CRPC_CHECK(status_ok(world.add_watcher(1, viewer)));
    for (entity_id id = 1; id <= 100; ++id) {
        CRPC_CHECK(status_ok(world.add_entity(id, make_state(vec2{50.5, 50.5}))));
    }

    world.tick();
    auto events = world.drain_events();
    CRPC_CHECK_EQ(static_cast<std::size_t>(100), events.size());
    auto visible = world.visible(1);
    CRPC_CHECK(visible.ok());
    if (visible.ok()) {
        CRPC_CHECK_EQ(static_cast<std::size_t>(100), visible.value().size());
    }

    for (entity_id id = 1; id <= 100; ++id) {
        CRPC_CHECK(status_ok(world.update_entity(id, make_state(vec2{150.0, 150.0}))));
    }
    world.tick();
    events = world.drain_events();
    CRPC_CHECK_EQ(static_cast<std::size_t>(100), events.size());
    for (const aoi_event& event : events) {
        CRPC_CHECK(event.kind == aoi_event_kind::leave);
    }

    for (entity_id id = 1; id <= 100; ++id) {
        CRPC_CHECK(status_ok(world.update_entity(id, make_state(vec2{50.5, 50.5}))));
    }
    world.tick();
    CRPC_CHECK_EQ(static_cast<std::size_t>(100), world.drain_events().size());
}

CRPC_TEST(aoi_filters_scope_broadcasts) {
    aoi_options options;
    options.grid.cell_size = 32.0;
    options.grid.columns = 10;
    options.grid.rows = 10;
    auto created = aoi_world::create(options);
    CRPC_CHECK(created.ok());
    if (!created.ok()) {
        return;
    }
    aoi_world& world = *created.value();

    watcher_options combined;
    combined.position = vec2{50.0, 50.0};
    combined.shape = aoi_shape::circle;
    combined.radius = 30.0;
    combined.faction = 1;
    combined.instance = 7;
    combined.filter = same_faction_instance_filter();

    watcher_options instance_only = combined;
    instance_only.faction = 9;
    instance_only.filter = same_instance_filter();

    watcher_options unfiltered = combined;
    unfiltered.filter = {};

    CRPC_CHECK(status_ok(world.add_watcher(1, combined)));
    CRPC_CHECK(status_ok(world.add_watcher(2, instance_only)));
    CRPC_CHECK(status_ok(world.add_watcher(3, unfiltered)));

    CRPC_CHECK(status_ok(world.add_entity(1, make_state(vec2{50.0, 50.0}), 1, 7)));
    CRPC_CHECK(status_ok(world.add_entity(2, make_state(vec2{51.0, 50.0}), 2, 7)));
    CRPC_CHECK(status_ok(world.add_entity(3, make_state(vec2{52.0, 50.0}), 1, 8)));
    CRPC_CHECK(status_ok(world.add_entity(4, make_state(vec2{53.0, 50.0}), 1, 7)));

    world.tick();
    (void)world.drain_events();

    auto first = world.visible(1);
    auto second = world.visible(2);
    auto third = world.visible(3);
    CRPC_CHECK(first.ok());
    CRPC_CHECK(second.ok());
    CRPC_CHECK(third.ok());
    if (first.ok()) {
        CRPC_CHECK_EQ(static_cast<std::size_t>(2), first.value().size());
        CRPC_CHECK_EQ(static_cast<u64>(1), first.value()[0]);
        CRPC_CHECK_EQ(static_cast<u64>(4), first.value()[1]);
    }
    if (second.ok()) {
        CRPC_CHECK_EQ(static_cast<std::size_t>(3), second.value().size());
    }
    if (third.ok()) {
        CRPC_CHECK_EQ(static_cast<std::size_t>(4), third.value().size());
    }
}

CRPC_TEST(aoi_watcher_removal_cleans_up) {
    aoi_options options;
    options.grid.cell_size = 20.0;
    options.grid.columns = 10;
    options.grid.rows = 10;
    auto created = aoi_world::create(options);
    CRPC_CHECK(created.ok());
    if (!created.ok()) {
        return;
    }
    aoi_world& world = *created.value();

    watcher_options viewer;
    viewer.position = vec2{50.0, 50.0};
    viewer.shape = aoi_shape::circle;
    viewer.radius = 20.0;
    CRPC_CHECK(status_ok(world.add_watcher(1, viewer)));
    CRPC_CHECK(status_ok(world.add_watcher(2, viewer)));
    CRPC_CHECK(status_ok(world.add_entity(1, make_state(vec2{50.0, 50.0}))));

    world.tick();
    (void)world.drain_events();

    CRPC_CHECK(status_ok(world.remove_watcher(1)));
    CRPC_CHECK_EQ(static_cast<int>(rpc_code::not_found),
                  static_cast<int>(world.remove_watcher(1).code()));
    auto gone = world.visible(1);
    CRPC_CHECK_FALSE(gone.ok());
    CRPC_CHECK_EQ(static_cast<int>(rpc_code::not_found),
                  static_cast<int>(gone.code()));

    CRPC_CHECK(status_ok(world.update_entity(1, make_state(vec2{55.0, 50.0}))));
    world.tick();
    auto events = world.drain_events();
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), events.size());
    for (const aoi_event& event : events) {
        CRPC_CHECK_EQ(static_cast<u64>(2), event.watcher);
        CRPC_CHECK(event.kind == aoi_event_kind::update);
    }

    CRPC_CHECK(status_ok(world.remove_entity(1)));
    events = world.drain_events();
    CRPC_CHECK_EQ(static_cast<std::size_t>(1), events.size());
    if (events.size() == 1) {
        CRPC_CHECK(events[0].kind == aoi_event_kind::leave);
        CRPC_CHECK_EQ(static_cast<u64>(2), events[0].watcher);
    }
}

CRPC_TEST(aoi_cell_boundaries_and_map_edges) {
    aoi_options options;
    options.grid.cell_size = 10.0;
    options.grid.columns = 10;
    options.grid.rows = 10;
    auto created = aoi_world::create(options);
    CRPC_CHECK(created.ok());
    if (!created.ok()) {
        return;
    }
    aoi_world& world = *created.value();

    watcher_options bottom_right;
    bottom_right.position = vec2{98.0, 98.0};
    bottom_right.shape = aoi_shape::circle;
    bottom_right.radius = 5.0;
    watcher_options bottom_left;
    bottom_left.position = vec2{2.0, 2.0};
    bottom_left.shape = aoi_shape::circle;
    bottom_left.radius = 3.0;
    watcher_options boundary;
    boundary.position = vec2{9.999, 50.0};
    boundary.shape = aoi_shape::circle;
    boundary.radius = 10.0;
    watcher_options boundary_rect;
    boundary_rect.position = vec2{9.999, 80.0};
    boundary_rect.shape = aoi_shape::rectangle;
    boundary_rect.half_width = 10.0;
    boundary_rect.half_height = 1.0;

    CRPC_CHECK(status_ok(world.add_watcher(1, bottom_right)));
    CRPC_CHECK(status_ok(world.add_watcher(2, bottom_left)));
    CRPC_CHECK(status_ok(world.add_watcher(3, boundary)));
    CRPC_CHECK(status_ok(world.add_watcher(4, boundary_rect)));

    CRPC_CHECK(status_ok(world.add_entity(1, make_state(vec2{100.0, 100.0}))));
    CRPC_CHECK(status_ok(world.add_entity(2, make_state(vec2{0.0, 0.0}))));
    CRPC_CHECK(status_ok(world.add_entity(3, make_state(vec2{19.999, 50.0}))));
    CRPC_CHECK(status_ok(world.add_entity(4, make_state(vec2{20.0, 50.0}))));
    CRPC_CHECK(status_ok(world.add_entity(5, make_state(vec2{19.999, 80.0}))));
    CRPC_CHECK(status_ok(world.add_entity(6, make_state(vec2{20.0, 80.0}))));

    world.tick();
    (void)world.drain_events();

    auto first = world.visible(1);
    auto second = world.visible(2);
    auto third = world.visible(3);
    auto fourth = world.visible(4);
    CRPC_CHECK(first.ok());
    CRPC_CHECK(second.ok());
    CRPC_CHECK(third.ok());
    CRPC_CHECK(fourth.ok());
    if (first.ok()) {
        CRPC_CHECK_EQ(static_cast<std::size_t>(1), first.value().size());
        if (!first.value().empty()) {
            CRPC_CHECK_EQ(static_cast<u64>(1), first.value()[0]);
        }
    }
    if (second.ok()) {
        CRPC_CHECK_EQ(static_cast<std::size_t>(1), second.value().size());
        if (!second.value().empty()) {
            CRPC_CHECK_EQ(static_cast<u64>(2), second.value()[0]);
        }
    }
    if (third.ok()) {
        // Entity 3 is exactly 10.0 away (inclusive); entity 4 is 10.001 away.
        CRPC_CHECK_EQ(static_cast<std::size_t>(1), third.value().size());
        if (!third.value().empty()) {
            CRPC_CHECK_EQ(static_cast<u64>(3), third.value()[0]);
        }
    }
    if (fourth.ok()) {
        // Rectangle edge is inclusive at exactly half_width.
        CRPC_CHECK_EQ(static_cast<std::size_t>(1), fourth.value().size());
        if (!fourth.value().empty()) {
            CRPC_CHECK_EQ(static_cast<u64>(5), fourth.value()[0]);
        }
    }
}

// --- Nine-grid forwarding: measured against a full scan ----------------------

CRPC_TEST(aoi_nine_grid_forwarding_visits_far_fewer_watchers) {
    aoi_options options;
    options.grid.cell_size = 16.0;
    options.grid.columns = 64;
    options.grid.rows = 64;
    auto created = aoi_world::create(options);
    CRPC_CHECK(created.ok());
    if (!created.ok()) {
        return;
    }
    aoi_world& world = *created.value();

    std::uint64_t rng = 0xABCDEF0123456789ull;
    std::unordered_map<watcher_id, watcher_options> watcher_map;
    for (watcher_id id = 1; id <= 50; ++id) {
        watcher_options viewer;
        viewer.position = random_position(rng, 1024.0);
        viewer.shape = aoi_shape::circle;
        viewer.radius = 8.0;
        watcher_map.emplace(id, viewer);
        CRPC_CHECK(status_ok(world.add_watcher(id, viewer)));
    }
    std::unordered_map<entity_id, ref_entity> entities;
    for (entity_id id = 1; id <= 500; ++id) {
        ref_entity entity;
        entity.state = make_state(random_position(rng, 1024.0));
        entities.emplace(id, entity);
        CRPC_CHECK(status_ok(world.add_entity(id, entity.state)));
    }

    world.tick();
    (void)world.drain_events();
    auto reference = make_reference(watcher_map);
    for (entity_id id = 1; id <= 500; ++id) {
        const ref_entity& entity = entities.at(id);
        for (auto& entry : reference) {
            if (brute_visible(entry.first, entry.second.options, id, entity)) {
                entry.second.visible.insert(id);
                entry.second.sent[id] = entity.state;
            }
        }
    }

    const aoi_stats before = world.stats();
    for (entity_id id = 1; id <= 500; ++id) {
        const entity_state next = make_state(random_position(rng, 1024.0));
        entities.at(id).state = next;
        CRPC_CHECK(status_ok(world.update_entity(id, next)));
    }
    world.tick();
    auto events = world.drain_events();
    const aoi_stats after = world.stats();

    const u64 neighbor = after.neighbor_visits - before.neighbor_visits;
    const u64 reverse = after.reverse_visits - before.reverse_visits;
    const u64 full = after.full_scan_visits - before.full_scan_visits;
    CRPC_CHECK_EQ(static_cast<u64>(500) * 50, full);
    CRPC_CHECK(full > 0);
    CRPC_CHECK(neighbor + reverse < full / 10);
    std::printf("AOI nine-grid: neighbor=%llu reverse=%llu full-scan=%llu "
                "ratio=%.1fx events=%zu\n",
                static_cast<unsigned long long>(neighbor),
                static_cast<unsigned long long>(reverse),
                static_cast<unsigned long long>(full),
                static_cast<double>(full) /
                    static_cast<double>(neighbor + reverse),
                events.size());

    check_and_apply_events(events, reference, entities);
    for (const auto& entry : reference) {
        const std::size_t mismatches =
            visible_mismatches(entry.first, entry.second, entities);
        if (mismatches != 0) {
            std::printf("  watcher %llu mismatches=%zu\n",
                        static_cast<unsigned long long>(entry.first),
                        mismatches);
        }
        CRPC_CHECK_EQ(static_cast<std::size_t>(0), mismatches);
    }
}

// --- Randomised differential test: optimised vs O(n^2) -----------------------

entity_state random_state(std::uint64_t& rng, double extent) {
    std::vector<attribute> attributes;
    const std::size_t count = static_cast<std::size_t>(xorshift64(rng) % 4);
    std::set<field_id> used;
    for (std::size_t i = 0; i < count; ++i) {
        const field_id id = static_cast<field_id>(1 + xorshift64(rng) % 4);
        if (!used.insert(id).second) {
            continue;
        }
        attributes.push_back(attribute{id,
                                       static_cast<i64>(xorshift64(rng) % 1000)});
    }
    std::sort(attributes.begin(), attributes.end(),
              [](const attribute& a, const attribute& b) { return a.id < b.id; });
    return make_state(random_position(rng, extent), std::move(attributes));
}

watcher_options random_watcher(std::uint64_t& rng, double extent,
                               entity_id entity_limit) {
    watcher_options options;
    options.position = random_position(rng, extent);
    options.shape = (xorshift64(rng) % 2 == 0) ? aoi_shape::circle
                                               : aoi_shape::rectangle;
    options.radius = random_range(rng, 0.0, 8.0);
    options.half_width = random_range(rng, 0.0, 8.0);
    options.half_height = random_range(rng, 0.0, 8.0);
    options.faction = static_cast<u32>(xorshift64(rng) % 3);
    options.instance = static_cast<u32>(xorshift64(rng) % 2);
    options.has_entity = (xorshift64(rng) % 4 == 0);
    options.entity = options.has_entity && entity_limit > 0
                         ? static_cast<entity_id>(1 + xorshift64(rng) %
                                                         entity_limit)
                         : 0;
    const std::uint64_t filter = xorshift64(rng) % 3;
    if (filter == 1) {
        options.filter = same_faction_instance_filter();
    } else if (filter == 2) {
        options.filter = same_instance_filter();
    }
    return options;
}

template <typename Map>
typename Map::value_type& pick(Map& map, std::uint64_t& rng) {
    const std::size_t index =
        static_cast<std::size_t>(xorshift64(rng) % map.size());
    auto it = map.begin();
    std::advance(it, static_cast<std::ptrdiff_t>(index));
    return *it;
}

CRPC_TEST(aoi_matches_brute_force_over_20_seeds) {
    constexpr double k_extent = 128.0;
    for (int seed = 1; seed <= 20; ++seed) {
        std::uint64_t rng =
            static_cast<std::uint64_t>(seed) * 0x9E3779B97F4A7C15ull + 1;

        aoi_options options;
        options.grid.cell_size = 8.0;
        options.grid.columns = 16;
        options.grid.rows = 16;
        auto created = aoi_world::create(options);
        CRPC_CHECK(created.ok());
        if (!created.ok()) {
            return;
        }
        aoi_world& world = *created.value();

        std::unordered_map<entity_id, ref_entity> entities;
        std::unordered_map<watcher_id, watcher_options> watcher_options_map;
        entity_id next_entity = 1;
        watcher_id next_watcher = 1;

        for (int i = 0; i < 30; ++i) {
            ref_entity entity;
            entity.state = random_state(rng, k_extent);
            entity.faction = static_cast<u32>(xorshift64(rng) % 3);
            entity.instance = static_cast<u32>(xorshift64(rng) % 2);
            entities.emplace(next_entity, entity);
            CRPC_CHECK(status_ok(world.add_entity(
                next_entity, entity.state, entity.faction, entity.instance)));
            ++next_entity;
        }
        for (int i = 0; i < 8; ++i) {
            const watcher_options viewer =
                random_watcher(rng, k_extent, next_entity - 1);
            watcher_options_map.emplace(next_watcher, viewer);
            CRPC_CHECK(status_ok(world.add_watcher(next_watcher, viewer)));
            ++next_watcher;
        }

        auto reference = make_reference(watcher_options_map);

        for (int tick = 0; tick < 25; ++tick) {
            // Move entities.
            for (int i = 0; i < 6 && !entities.empty(); ++i) {
                auto& entry = pick(entities, rng);
                entry.second.state.position = random_position(rng, k_extent);
                CRPC_CHECK(status_ok(
                    world.update_entity(entry.first, entry.second.state)));
            }
            // Rewrite attributes, sometimes to the same values.
            for (int i = 0; i < 4 && !entities.empty(); ++i) {
                auto& entry = pick(entities, rng);
                const entity_state fresh = random_state(rng, k_extent);
                entry.second.state.attributes = fresh.attributes;
                CRPC_CHECK(status_ok(
                    world.update_entity(entry.first, entry.second.state)));
            }
            // Move watchers.
            for (int i = 0; i < 3 && !watcher_options_map.empty(); ++i) {
                auto& entry = pick(watcher_options_map, rng);
                const vec2 target = random_position(rng, k_extent);
                entry.second.position = target;
                // The reference model holds its own copy of the options; it has to
                // move too, or brute_visible() judges visibility from the position
                // the watcher had before this tick and reports a mismatch that is
                // an artefact of the test rather than of the AOI implementation.
                auto reference_it = reference.find(entry.first);
                if (reference_it != reference.end()) {
                    reference_it->second.options.position = target;
                }
                CRPC_CHECK(status_ok(world.move_watcher(entry.first, target)));
            }
            if (entities.size() < 50) {
                ref_entity entity;
                entity.state = random_state(rng, k_extent);
                entity.faction = static_cast<u32>(xorshift64(rng) % 3);
                entity.instance = static_cast<u32>(xorshift64(rng) % 2);
                entities.emplace(next_entity, entity);
                CRPC_CHECK(status_ok(world.add_entity(next_entity, entity.state,
                                                      entity.faction,
                                                      entity.instance)));
                ++next_entity;
            }
            if (next_watcher <= 14) {
                const watcher_options viewer =
                    random_watcher(rng, k_extent, next_entity - 1);
                watcher_options_map.emplace(next_watcher, viewer);
                CRPC_CHECK(status_ok(world.add_watcher(next_watcher, viewer)));
                reference.emplace(next_watcher, ref_watcher{});
                reference[next_watcher].options = viewer;
                ++next_watcher;
            }
            if (tick % 5 == 4 && !entities.empty()) {
                const entity_id victim = pick(entities, rng).first;
                CRPC_CHECK(status_ok(world.remove_entity(victim)));
                entities.erase(victim);
            }
            if (tick % 7 == 6 && watcher_options_map.size() > 4) {
                const watcher_id victim = pick(watcher_options_map, rng).first;
                CRPC_CHECK(status_ok(world.remove_watcher(victim)));
                watcher_options_map.erase(victim);
                reference.erase(victim);
            }

            world.tick();
            const std::vector<aoi_event> events = world.drain_events();
            check_and_apply_events(events, reference, entities);

            for (const auto& entry : reference) {
                const std::size_t mismatches =
                    visible_mismatches(entry.first, entry.second, entities);
                if (mismatches != 0) {
                    std::printf("  seed=%d tick=%d watcher=%llu mismatches=%zu\n",
                                seed, tick,
                                static_cast<unsigned long long>(entry.first),
                                mismatches);
                }
                CRPC_CHECK_EQ(static_cast<std::size_t>(0), mismatches);

                auto listed = world.visible(entry.first);
                CRPC_CHECK(listed.ok());
                if (listed.ok()) {
                    std::unordered_set<entity_id> expected = entry.second.visible;
                    std::vector<entity_id> sorted = listed.value();
                    std::sort(sorted.begin(), sorted.end());
                    CRPC_CHECK_EQ(expected.size(), sorted.size());
                    bool same = true;
                    for (entity_id id : sorted) {
                        if (expected.count(id) == 0) {
                            same = false;
                        }
                    }
                    CRPC_CHECK(same);
                }
            }
        }
    }
}

// --- Pressure: 10k entities, 1k watchers ------------------------------------

CRPC_TEST(aoi_stress_ten_thousand_entities_one_thousand_watchers) {
    constexpr std::size_t k_entities = 10000;
    constexpr std::size_t k_watchers = 1000;
    constexpr std::size_t k_moved_per_tick = 500;
    constexpr int k_ticks = 20;
    constexpr double k_extent = 1024.0;

    aoi_options options;
    options.grid.cell_size = 32.0;
    options.grid.columns = 32;
    options.grid.rows = 32;
    auto created = aoi_world::create(options);
    CRPC_CHECK(created.ok());
    if (!created.ok()) {
        return;
    }
    aoi_world& world = *created.value();

    std::uint64_t rng = 0xC0FFEE123456789ull;
    using clock = std::chrono::steady_clock;
    const auto build_start = clock::now();

    std::unordered_map<watcher_id, watcher_options> watcher_options_map;
    for (watcher_id id = 1; id <= k_watchers; ++id) {
        watcher_options viewer;
        viewer.position = random_position(rng, k_extent);
        viewer.shape = aoi_shape::circle;
        viewer.radius = 24.0;
        watcher_options_map.emplace(id, viewer);
        CRPC_CHECK(status_ok(world.add_watcher(id, viewer)));
    }
    std::unordered_map<entity_id, ref_entity> entities;
    for (entity_id id = 1; id <= k_entities; ++id) {
        ref_entity entity;
        entity.state = make_state(
            random_position(rng, k_extent),
            {attribute{1, static_cast<i64>(id % 7)}});
        entities.emplace(id, entity);
        CRPC_CHECK(status_ok(world.add_entity(id, entity.state)));
    }

    world.tick();
    auto events = world.drain_events();
    const auto build_end = clock::now();

    auto reference = make_reference(watcher_options_map);
    std::size_t expected_initial = 0;
    for (const auto& watcher : reference) {
        for (const auto& entity : entities) {
            if (brute_visible(watcher.first, watcher.second.options,
                              entity.first, entity.second)) {
                ++expected_initial;
            }
        }
    }
    CRPC_CHECK_EQ(expected_initial, events.size());
    for (const aoi_event& event : events) {
        CRPC_CHECK(event.kind == aoi_event_kind::enter);
    }
    check_and_apply_events(events, reference, entities);

    std::vector<entity_id> ids;
    ids.reserve(entities.size());
    for (const auto& entry : entities) {
        ids.push_back(entry.first);
    }
    std::sort(ids.begin(), ids.end());

    std::uint64_t engine_ns = 0;
    std::uint64_t tick_expected_enter = 0;
    std::uint64_t tick_expected_leave = 0;
    std::uint64_t tick_expected_update = 0;
    std::uint64_t actual_enter = 0;
    std::uint64_t actual_leave = 0;
    std::uint64_t actual_update = 0;

    for (int tick = 0; tick < k_ticks; ++tick) {
        // Pick a random subset without replacement.
        for (std::size_t i = ids.size(); i > 1; --i) {
            const std::size_t j =
                static_cast<std::size_t>(xorshift64(rng) % i);
            std::swap(ids[i - 1], ids[j]);
        }

        std::vector<entity_id> moved(ids.begin(),
                                     ids.begin() + k_moved_per_tick);

        tick_expected_enter = 0;
        tick_expected_leave = 0;
        tick_expected_update = 0;

        // Compute the expected transition counts with the O(n^2) reference.
        for (entity_id id : moved) {
            ref_entity& entity = entities.at(id);
            const vec2 next = random_position(rng, k_extent);
            entity.state.position = next;
            for (const auto& watcher : reference) {
                const bool was = watcher.second.visible.count(id) != 0;
                const bool now =
                    brute_visible(watcher.first, watcher.second.options, id,
                                  entity);
                if (was && now) {
                    ++tick_expected_update;
                } else if (!was && now) {
                    ++tick_expected_enter;
                } else if (was && !now) {
                    ++tick_expected_leave;
                }
            }
        }

        const auto tick_start = clock::now();
        for (entity_id id : moved) {
            CRPC_CHECK(status_ok(world.update_entity(id, entities.at(id).state)));
        }
        world.tick();
        events = world.drain_events();
        const auto tick_end = clock::now();
        engine_ns += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(tick_end -
                                                                 tick_start)
                .count());

        std::uint64_t enter = 0;
        std::uint64_t leave = 0;
        std::uint64_t update = 0;
        for (const aoi_event& event : events) {
            if (event.kind == aoi_event_kind::enter) {
                ++enter;
            } else if (event.kind == aoi_event_kind::leave) {
                ++leave;
            } else {
                ++update;
            }
        }
        CRPC_CHECK_EQ(tick_expected_enter, enter);
        CRPC_CHECK_EQ(tick_expected_leave, leave);
        CRPC_CHECK_EQ(tick_expected_update, update);
        actual_enter += enter;
        actual_leave += leave;
        actual_update += update;

        check_and_apply_events(events, reference, entities);
    }

    const auto verify_start = clock::now();
    std::size_t final_mismatches = 0;
    for (const auto& watcher : reference) {
        final_mismatches +=
            visible_mismatches(watcher.first, watcher.second, entities);
    }
    CRPC_CHECK_EQ(static_cast<std::size_t>(0), final_mismatches);
    const auto verify_end = clock::now();

    const aoi_stats stats = world.stats();
    std::uint64_t visible_total = 0;
    for (const auto& watcher : reference) {
        visible_total += watcher.second.visible.size();
    }
    CRPC_CHECK_EQ(static_cast<u64>(expected_initial) + actual_enter - actual_leave,
                  visible_total);
    CRPC_CHECK_EQ(static_cast<u64>(expected_initial) + actual_enter,
                  stats.enter_events);
    CRPC_CHECK_EQ(actual_leave, stats.leave_events);
    CRPC_CHECK_EQ(actual_update, stats.update_events);
    CRPC_CHECK(stats.full_scan_visits >
               stats.neighbor_visits + stats.reverse_visits);

    const auto build_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              build_end - build_start)
                              .count();
    const auto engine_ms = static_cast<double>(engine_ns) / 1.0e6;
    const auto verify_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(verify_end -
                                                              verify_start)
            .count();
    std::printf("AOI stress: entities=%zu watchers=%zu ticks=%d "
                "build=%lldms ticks=%0.1fms verify-final=%lldms "
                "events: enter=%llu leave=%llu update=%llu "
                "neighbor=%llu reverse=%llu full-scan=%llu\n",
                k_entities, k_watchers, k_ticks, static_cast<long long>(build_ms),
                engine_ms, static_cast<long long>(verify_ms),
                static_cast<unsigned long long>(actual_enter),
                static_cast<unsigned long long>(actual_leave),
                static_cast<unsigned long long>(actual_update),
                static_cast<unsigned long long>(stats.neighbor_visits),
                static_cast<unsigned long long>(stats.reverse_visits),
                static_cast<unsigned long long>(stats.full_scan_visits));

    CRPC_CHECK(build_ms + static_cast<long long>(engine_ms) < 15000);
}

} // namespace

CRPC_TEST_MAIN()
