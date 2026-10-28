// CRPC -- area-of-interest world: visibility tracking and forwarding.
//
// `aoi_world` owns the spatial index and every watcher's "currently visible"
// set. The caller mutates the world (add/update/remove entities and watchers)
// and calls `tick()` once per server frame. `tick()` reconciles dirty watchers
// and dirty entities and produces at most one event per (watcher, entity) pair:
//
//   enter   the entity just became visible; `state` is the complete state;
//   update  the entity stayed visible and its state changed; `delta` carries
//           only the fields that changed since the last event sent for it;
//   leave   the entity stopped being visible (moved away, the watcher moved,
//           the filter stopped accepting it, or the entity was removed).
//
// Events are queued and retrieved with `drain_events()` unless an `event_sink`
// is configured, in which case each event is delivered to the sink instead.
// Nothing is emitted twice for a pair in one tick: all mutations between two
// `tick()` calls collapse into the final state.
//
// Forwarding
// ----------
//   A dirty entity only inspects watchers in its own cell and the eight
//   neighbouring cells (the "nine-grid" optimisation), plus the watchers that
//   had the entity in their last emitted visible set (needed so a teleport can
//   send a leave to the old side). A watcher may have an extent of at most one
//   cell; that is what makes the nine-grid complete: an entity inside a watcher
//   view is never more than one cell away from that watcher. `add_watcher`
//   rejects a larger extent with `rpc_code::invalid_argument`.
//
// Visibility semantics
// --------------------
//   * The circle test is inclusive: distance squared <= radius squared.
//   * The rectangle test is inclusive on all four edges.
//   * A watcher never sees its own entity: when `watcher_options::has_entity`
//     is set and the entity id matches, that pair is invisible regardless of
//     geometry or filter. The client already owns that state.
//   * `visibility_filter` runs after the geometry test; a false answer means
//     "not visible", so a filter change (or a faction/instance mismatch) is
//     represented by enter/leave, not by update.
//
// Thread safety: an `aoi_world` is owned by one shard/frame thread and is not
// internally synchronised.
#ifndef CRPC_GAME_AOI_WORLD_HPP
#define CRPC_GAME_AOI_WORLD_HPP

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>
#include <crpc/game/aoi/grid.hpp>

namespace crpc::game::aoi {

using entity_id = u64;
using watcher_id = u64;
using field_id = u32;

struct attribute {
    field_id id = 0;
    i64 value = 0;
};

constexpr bool operator==(const attribute& a, const attribute& b) noexcept {
    return a.id == b.id && a.value == b.value;
}

constexpr bool operator!=(const attribute& a, const attribute& b) noexcept {
    return !(a == b);
}

// The state an entity update replaces. `attributes` must be sorted by id and
// contain each id at most once; `add_entity` / `update_entity` normalise a copy
// and answer `invalid_argument` for duplicate ids.
struct entity_state {
    vec2 position;
    std::vector<attribute> attributes;
};

// The immutable identity parts a filter sees. Faction and instance are fixed at
// entity creation; a filter that needs different data can be any callable.
struct entity_info {
    entity_id id = 0;
    u32 faction = 0;
    u32 instance = 0;
};

struct watcher_info {
    watcher_id id = 0;
    u32 faction = 0;
    u32 instance = 0;
};

// Returns true when the watcher may see the entity. The geometry test has
// already passed when the filter is called; the filter only narrows the result.
using visibility_filter =
    std::function<bool(const watcher_info&, const entity_info&)>;

// Convenience filters for the two common broadcast scopes.
inline visibility_filter same_faction_instance_filter() {
    return [](const watcher_info& watcher, const entity_info& entity) {
        return watcher.faction == entity.faction &&
               watcher.instance == entity.instance;
    };
}

inline visibility_filter same_instance_filter() {
    return [](const watcher_info& watcher, const entity_info& entity) {
        return watcher.instance == entity.instance;
    };
}

enum class aoi_shape : u8 {
    rectangle = 0,
    circle = 1
};

struct watcher_options {
    vec2 position;
    aoi_shape shape = aoi_shape::circle;
    // Active when shape == circle. Must be in [0, cell_size].
    double radius = 0.0;
    // Active when shape == rectangle. Each must be in [0, cell_size].
    double half_width = 0.0;
    double half_height = 0.0;
    u32 faction = 0;
    u32 instance = 0;
    // Self-visibility: when `has_entity` is true, `entity` is this watcher's
    // own entity and is never reported to it. The view centre stays at
    // `position`; moving both is the caller's responsibility.
    bool has_entity = false;
    entity_id entity = 0;
    // Optional scope filter; empty means "no additional filter".
    visibility_filter filter;
};

enum class aoi_event_kind : u8 {
    enter = 0,
    leave = 1,
    update = 2
};

// The difference between the last state sent to a watcher and the current one.
// Every member is empty/false when the corresponding field did not change.
struct state_delta {
    bool position_changed = false;
    vec2 position;
    std::vector<attribute> attributes_set;
    std::vector<field_id> attributes_removed;
};

struct aoi_event {
    aoi_event_kind kind = aoi_event_kind::enter;
    watcher_id watcher = 0;
    entity_id entity = 0;
    u32 faction = 0;
    u32 instance = 0;
    // Filled only for `enter`: the complete state. Empty for update/leave.
    entity_state state;
    // Filled only for `update`: changed fields only. Empty for enter/leave.
    state_delta delta;
};

using event_sink = std::function<void(const aoi_event&)>;

struct aoi_options {
    grid_options grid;
    // When set, events are handed to the sink instead of being queued.
    event_sink sink;
};

struct aoi_stats {
    u64 live_entities = 0;
    u64 live_watchers = 0;
    u64 ticks = 0;
    u64 enter_events = 0;
    u64 leave_events = 0;
    u64 update_events = 0;
    // Watcher slots examined by the nine-grid forwarding path.
    u64 neighbor_visits = 0;
    // Watcher slots pulled from the reverse "who currently sees this entity"
    // index. They are usually a subset of the nine-grid slots; the index is
    // what lets a teleport leave the old watchers without a world scan.
    u64 reverse_visits = 0;
    // Watchers a naive full scan would have examined for the same dirty
    // entities (dirty entities * live watchers). Directly comparable with
    // neighbor_visits.
    u64 full_scan_visits = 0;
    // Unique (entity, watcher) pairs reconciled by forwarding.
    u64 candidate_checks = 0;
    // Exact geometry/filter tests performed (both refresh and forwarding).
    u64 visibility_checks = 0;
    // Entity slots examined while rebuilding a dirty watcher's view.
    u64 watcher_cell_entity_checks = 0;
};

class CRPC_API aoi_world {
public:
    [[nodiscard]] static result<std::unique_ptr<aoi_world>>
    create(const aoi_options& options = {});

    ~aoi_world();

    aoi_world(const aoi_world&) = delete;
    aoi_world& operator=(const aoi_world&) = delete;
    aoi_world(aoi_world&&) = delete;
    aoi_world& operator=(aoi_world&&) = delete;

    [[nodiscard]] const aoi_grid& grid() const noexcept;

    // Adds an entity. Duplicate id: `failed_precondition`. Position outside
    // the world, duplicate attribute id or non-finite position:
    // `invalid_argument`.
    [[nodiscard]] status add_entity(entity_id id, const entity_state& state,
                                    u32 faction = 0, u32 instance = 0);

    // Replaces the state (position and attributes). The cell membership is
    // updated immediately; events are produced at the next `tick()`.
    [[nodiscard]] status update_entity(entity_id id, const entity_state& state);

    // Removes the entity and immediately emits a `leave` to every watcher that
    // currently sees it. Missing id: `not_found`.
    [[nodiscard]] status remove_entity(entity_id id);

    // Adds a watcher. Duplicate id: `failed_precondition`. Invalid extent
    // (negative, non-finite, or larger than one cell): `invalid_argument`.
    [[nodiscard]] status add_watcher(watcher_id id,
                                     const watcher_options& options);

    // Removes the watcher and its visible-set bookkeeping. No events are
    // emitted for a watcher that no longer exists.
    [[nodiscard]] status remove_watcher(watcher_id id);

    // Moves the view centre (the watcher keeps its shape, extents and filter).
    [[nodiscard]] status move_watcher(watcher_id id, vec2 position);

    // Reconciles every dirty watcher and dirty entity. Emits at most one event
    // per (watcher, entity) pair, always using the final state.
    void tick();

    // Moves queued events into `out` (clearing the internal queue). Returns an
    // empty vector when a sink is configured.
    [[nodiscard]] std::vector<aoi_event> drain_events();

    // The current visible set, sorted by entity id. Missing watcher:
    // `not_found`.
    [[nodiscard]] result<std::vector<entity_id>>
    visible(watcher_id id) const;

    [[nodiscard]] aoi_stats stats() const noexcept;

private:
    struct impl;
    explicit aoi_world(std::unique_ptr<impl> state) noexcept;

    std::unique_ptr<impl> impl_;
};

} // namespace crpc::game::aoi

#endif // CRPC_GAME_AOI_WORLD_HPP