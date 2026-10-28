// CRPC -- area-of-interest world: visibility diffing, event batching and
// nine-grid forwarding.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <unordered_map>
#include <utility>
#include <vector>

#include <crpc/game/aoi/world.hpp>

namespace crpc::game::aoi {
namespace {

struct cell_slot {
    std::vector<entity_id> entities;
    std::vector<watcher_id> watchers;
};

struct entity_record {
    entity_state state;
    u32 faction = 0;
    u32 instance = 0;
    std::size_t cell_index = 0;
    std::size_t cell_position = 0;
    bool dirty = false;
    // Scratch stamps used by a dirty-watcher refresh.
    u64 seen_epoch = 0;
    u64 visible_epoch = 0;
};

struct watcher_record {
    watcher_options options;
    std::size_t cell_index = 0;
    std::size_t cell_position = 0;
    std::unordered_map<entity_id, entity_state> emitted;
    u64 refreshed_epoch = 0;
    u64 visit_stamp = 0;
    bool dirty = false;
};

status normalize_state(entity_state& state) {
    std::sort(state.attributes.begin(), state.attributes.end(),
              [](const attribute& left, const attribute& right) {
                  return left.id < right.id;
              });
    for (std::size_t i = 1; i < state.attributes.size(); ++i) {
        if (state.attributes[i - 1].id == state.attributes[i].id) {
            return status(rpc_code::invalid_argument,
                          "aoi_world: duplicate attribute id");
        }
    }
    return status{};
}

bool state_equal(const entity_state& left, const entity_state& right) {
    return left.position == right.position &&
           left.attributes == right.attributes;
}

state_delta make_delta(const entity_state& before, const entity_state& after) {
    state_delta delta;
    delta.position_changed = before.position != after.position;
    delta.position = after.position;

    std::size_t left = 0;
    std::size_t right = 0;
    while (left < before.attributes.size() ||
           right < after.attributes.size()) {
        if (right >= after.attributes.size() ||
            (left < before.attributes.size() &&
             before.attributes[left].id < after.attributes[right].id)) {
            delta.attributes_removed.push_back(before.attributes[left].id);
            ++left;
        } else if (left >= before.attributes.size() ||
                   after.attributes[right].id < before.attributes[left].id) {
            delta.attributes_set.push_back(after.attributes[right]);
            ++right;
        } else {
            if (before.attributes[left].value != after.attributes[right].value) {
                delta.attributes_set.push_back(after.attributes[right]);
            }
            ++left;
            ++right;
        }
    }
    return delta;
}

} // namespace

struct aoi_world::impl {
    explicit impl(aoi_grid grid_value) : grid(std::move(grid_value)) {}

    aoi_grid grid;
    event_sink sink;
    std::vector<cell_slot> cells;
    std::unordered_map<entity_id, entity_record> entities;
    std::unordered_map<watcher_id, watcher_record> watchers;
    // reverse[entity] is the set of watchers whose last emitted visible set
    // still contains the entity. It is what makes a teleport correct without
    // scanning the world for the old side.
    std::unordered_map<entity_id, std::vector<watcher_id>> reverse;
    std::vector<entity_id> dirty_entities;
    std::vector<watcher_id> dirty_watchers;
    std::vector<aoi_event> pending;
    std::vector<watcher_id> candidates;
    aoi_stats counters;
    u64 tick_epoch = 0;
    u64 visit_epoch = 0;
    u64 refresh_epoch = 0;

    [[nodiscard]] std::size_t index_of(cell_coord cell) const {
        return cell.row * grid.options().columns + cell.column;
    }

    void add_entity_to_cell(entity_id id, entity_record& record,
                            cell_coord cell) {
        const std::size_t index = index_of(cell);
        record.cell_index = index;
        record.cell_position = cells[index].entities.size();
        cells[index].entities.push_back(id);
    }

    void remove_entity_from_cell(entity_id id, entity_record& record) {
        std::vector<entity_id>& list = cells[record.cell_index].entities;
        const std::size_t position = record.cell_position;
        if (position >= list.size() || list[position] != id) {
            return;
        }
        const entity_id moved = list.back();
        list[position] = moved;
        list.pop_back();
        if (position < list.size()) {
            entities[moved].cell_position = position;
        }
    }

    void add_watcher_to_cell(watcher_id id, watcher_record& record,
                             cell_coord cell) {
        const std::size_t index = index_of(cell);
        record.cell_index = index;
        record.cell_position = cells[index].watchers.size();
        cells[index].watchers.push_back(id);
    }

    void remove_watcher_from_cell(watcher_id id, watcher_record& record) {
        std::vector<watcher_id>& list = cells[record.cell_index].watchers;
        const std::size_t position = record.cell_position;
        if (position >= list.size() || list[position] != id) {
            return;
        }
        const watcher_id moved = list.back();
        list[position] = moved;
        list.pop_back();
        if (position < list.size()) {
            watchers[moved].cell_position = position;
        }
    }

    void mark_entity_dirty(entity_id id, entity_record& record) {
        if (!record.dirty) {
            record.dirty = true;
            dirty_entities.push_back(id);
        }
    }

    void mark_watcher_dirty(watcher_id id, watcher_record& record) {
        if (!record.dirty) {
            record.dirty = true;
            dirty_watchers.push_back(id);
        }
    }

    void remove_reverse(entity_id id, watcher_id watcher) {
        auto found = reverse.find(id);
        if (found == reverse.end()) {
            return;
        }
        std::vector<watcher_id>& list = found->second;
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (list[i] == watcher) {
                list[i] = list.back();
                list.pop_back();
                break;
            }
        }
        if (list.empty()) {
            reverse.erase(found);
        }
    }

    void emit(aoi_event event) {
        switch (event.kind) {
        case aoi_event_kind::enter:
            ++counters.enter_events;
            break;
        case aoi_event_kind::leave:
            ++counters.leave_events;
            break;
        case aoi_event_kind::update:
            ++counters.update_events;
            break;
        }
        if (sink) {
            sink(event);
        } else {
            pending.push_back(std::move(event));
        }
    }

    void emit_enter(watcher_id watcher, watcher_record& record,
                    entity_id entity, const entity_record& target) {
        aoi_event event;
        event.kind = aoi_event_kind::enter;
        event.watcher = watcher;
        event.entity = entity;
        event.faction = target.faction;
        event.instance = target.instance;
        event.state = target.state;
        record.emitted.emplace(entity, target.state);
        reverse[entity].push_back(watcher);
        emit(std::move(event));
    }

    void emit_update(watcher_id watcher, watcher_record& record,
                     entity_id entity, const entity_record& target,
                     const entity_state& previous) {
        aoi_event event;
        event.kind = aoi_event_kind::update;
        event.watcher = watcher;
        event.entity = entity;
        event.faction = target.faction;
        event.instance = target.instance;
        event.delta = make_delta(previous, target.state);
        record.emitted[entity] = target.state;
        emit(std::move(event));
    }

    void emit_leave(watcher_id watcher, watcher_record& record,
                    entity_id entity, u32 faction, u32 instance) {
        aoi_event event;
        event.kind = aoi_event_kind::leave;
        event.watcher = watcher;
        event.entity = entity;
        event.faction = faction;
        event.instance = instance;
        record.emitted.erase(entity);
        remove_reverse(entity, watcher);
        emit(std::move(event));
    }

    [[nodiscard]] bool visible_for(watcher_id watcher,
                                   const watcher_record& record,
                                   entity_id entity,
                                   const entity_record& target) const {
        if (record.options.has_entity && record.options.entity == entity) {
            return false;
        }

        bool inside = false;
        if (record.options.shape == aoi_shape::circle) {
            const double dx = target.state.position.x - record.options.position.x;
            const double dy = target.state.position.y - record.options.position.y;
            inside =
                dx * dx + dy * dy <= record.options.radius * record.options.radius;
        } else {
            inside = std::fabs(target.state.position.x -
                               record.options.position.x) <=
                         record.options.half_width &&
                     std::fabs(target.state.position.y -
                               record.options.position.y) <=
                         record.options.half_height;
        }
        if (!inside) {
            return false;
        }

        if (record.options.filter) {
            const watcher_info watcher_value{
                watcher, record.options.faction, record.options.instance};
            const entity_info entity_value{entity, target.faction,
                                           target.instance};
            if (!record.options.filter(watcher_value, entity_value)) {
                return false;
            }
        }
        return true;
    }

    // The entity is known to be visible: enter or update.
    void attach_visible(watcher_id watcher, watcher_record& record,
                        entity_id entity, const entity_record& target) {
        auto found = record.emitted.find(entity);
        if (found == record.emitted.end()) {
            emit_enter(watcher, record, entity, target);
        } else if (!state_equal(found->second, target.state)) {
            emit_update(watcher, record, entity, target, found->second);
        }
    }

    // Reconciles one entity for one watcher; used by the forwarding path.
    void reconcile(watcher_id watcher, watcher_record& record, entity_id entity,
                   const entity_record& target) {
        ++counters.visibility_checks;
        if (visible_for(watcher, record, entity, target)) {
            attach_visible(watcher, record, entity, target);
        } else {
            auto found = record.emitted.find(entity);
            if (found != record.emitted.end()) {
                emit_leave(watcher, record, entity, target.faction,
                           target.instance);
            }
        }
    }

    void refresh_watcher(watcher_id watcher, watcher_record& record) {
        ++refresh_epoch;
        std::vector<cell_coord> view;
        if (record.options.shape == aoi_shape::circle) {
            view = grid.circle_cells(record.options.position,
                                     record.options.radius);
        } else {
            view = grid.rectangle_cells(record.options.position,
                                        record.options.half_width,
                                        record.options.half_height);
        }

        for (const cell_coord cell : view) {
            for (const entity_id entity : cells[index_of(cell)].entities) {
                auto found = entities.find(entity);
                if (found == entities.end()) {
                    continue;
                }
                entity_record& target = found->second;
                if (target.seen_epoch == refresh_epoch) {
                    continue;
                }
                target.seen_epoch = refresh_epoch;
                ++counters.watcher_cell_entity_checks;
                ++counters.visibility_checks;
                if (!visible_for(watcher, record, entity, target)) {
                    continue;
                }
                target.visible_epoch = refresh_epoch;
                attach_visible(watcher, record, entity, target);
            }
        }

        std::vector<entity_id> leaving;
        leaving.reserve(record.emitted.size());
        for (const auto& entry : record.emitted) {
            auto found = entities.find(entry.first);
            if (found == entities.end() ||
                found->second.visible_epoch != refresh_epoch) {
                leaving.push_back(entry.first);
            }
        }
        for (const entity_id entity : leaving) {
            auto found = entities.find(entity);
            const u32 faction = found != entities.end() ? found->second.faction : 0;
            const u32 instance =
                found != entities.end() ? found->second.instance : 0;
            emit_leave(watcher, record, entity, faction, instance);
        }
    }

    void forward_entity(entity_id entity, entity_record& target) {
        ++visit_epoch;
        candidates.clear();

        const std::size_t columns = grid.options().columns;
        const std::size_t rows = grid.options().rows;
        const std::size_t column = target.cell_index % columns;
        const std::size_t row = target.cell_index / columns;

        const std::size_t first_column = column == 0 ? 0 : column - 1;
        const std::size_t first_row = row == 0 ? 0 : row - 1;
        const std::size_t last_column =
            column + 1 < columns ? column + 1 : columns - 1;
        const std::size_t last_row = row + 1 < rows ? row + 1 : rows - 1;

        for (std::size_t current_row = first_row; current_row <= last_row;
             ++current_row) {
            for (std::size_t current_column = first_column;
                 current_column <= last_column; ++current_column) {
                for (const watcher_id watcher :
                     cells[current_row * columns + current_column].watchers) {
                    ++counters.neighbor_visits;
                    auto found = watchers.find(watcher);
                    if (found == watchers.end()) {
                        continue;
                    }
                    if (found->second.visit_stamp == visit_epoch) {
                        continue;
                    }
                    found->second.visit_stamp = visit_epoch;
                    candidates.push_back(watcher);
                }
            }
        }

        auto viewers = reverse.find(entity);
        if (viewers != reverse.end()) {
            for (const watcher_id watcher : viewers->second) {
                ++counters.reverse_visits;
                auto found = watchers.find(watcher);
                if (found == watchers.end()) {
                    continue;
                }
                if (found->second.visit_stamp == visit_epoch) {
                    continue;
                }
                found->second.visit_stamp = visit_epoch;
                candidates.push_back(watcher);
            }
        }

        counters.full_scan_visits += static_cast<u64>(watchers.size());
        counters.candidate_checks += static_cast<u64>(candidates.size());

        for (const watcher_id watcher : candidates) {
            auto found = watchers.find(watcher);
            if (found == watchers.end()) {
                continue;
            }
            if (found->second.refreshed_epoch == tick_epoch) {
                // A dirty watcher was already reconciled against the final
                // state this tick; emitting again would duplicate events.
                continue;
            }
            reconcile(watcher, found->second, entity, target);
        }
    }

    void run_tick() {
        ++counters.ticks;
        ++tick_epoch;

        for (const watcher_id watcher : dirty_watchers) {
            auto found = watchers.find(watcher);
            if (found == watchers.end()) {
                continue;
            }
            found->second.dirty = false;
            refresh_watcher(watcher, found->second);
            found->second.refreshed_epoch = tick_epoch;
        }
        dirty_watchers.clear();

        for (const entity_id entity : dirty_entities) {
            auto found = entities.find(entity);
            if (found == entities.end()) {
                continue;
            }
            found->second.dirty = false;
            forward_entity(entity, found->second);
        }
        dirty_entities.clear();
    }
};

result<std::unique_ptr<aoi_world>>
aoi_world::create(const aoi_options& options) {
    auto grid = aoi_grid::create(options.grid);
    if (!grid.ok()) {
        return grid.error();
    }

    std::unique_ptr<impl> state(new impl(std::move(grid.value())));
    state->sink = options.sink;
    state->cells.resize(state->grid.options().columns *
                        state->grid.options().rows);
    return std::unique_ptr<aoi_world>(new aoi_world(std::move(state)));
}

aoi_world::aoi_world(std::unique_ptr<impl> state) noexcept
    : impl_(std::move(state)) {}

aoi_world::~aoi_world() = default;

const aoi_grid& aoi_world::grid() const noexcept {
    return impl_->grid;
}

status aoi_world::add_entity(entity_id id, const entity_state& state,
                             u32 faction, u32 instance) {
    if (impl_->entities.find(id) != impl_->entities.end()) {
        return status(rpc_code::failed_precondition,
                      "aoi_world: entity already exists");
    }

    entity_state normalized = state;
    const status normalized_status = normalize_state(normalized);
    if (!normalized_status.ok()) {
        return normalized_status;
    }
    auto cell = impl_->grid.cell_of(normalized.position);
    if (!cell.ok()) {
        return status(cell.error());
    }

    entity_record record;
    record.state = std::move(normalized);
    record.faction = faction;
    record.instance = instance;
    impl_->add_entity_to_cell(id, record, cell.value());
    auto inserted = impl_->entities.emplace(id, std::move(record));
    impl_->mark_entity_dirty(id, inserted.first->second);
    return status{};
}

status aoi_world::update_entity(entity_id id, const entity_state& state) {
    auto found = impl_->entities.find(id);
    if (found == impl_->entities.end()) {
        return status(rpc_code::not_found, "aoi_world: entity not found");
    }

    entity_state normalized = state;
    const status normalized_status = normalize_state(normalized);
    if (!normalized_status.ok()) {
        return normalized_status;
    }
    auto cell = impl_->grid.cell_of(normalized.position);
    if (!cell.ok()) {
        return status(cell.error());
    }

    entity_record& record = found->second;
    const std::size_t index = impl_->index_of(cell.value());
    if (index != record.cell_index) {
        impl_->remove_entity_from_cell(id, record);
        impl_->add_entity_to_cell(id, record, cell.value());
    }
    record.state = std::move(normalized);
    impl_->mark_entity_dirty(id, record);
    return status{};
}

status aoi_world::remove_entity(entity_id id) {
    auto found = impl_->entities.find(id);
    if (found == impl_->entities.end()) {
        return status(rpc_code::not_found, "aoi_world: entity not found");
    }
    entity_record& record = found->second;

    auto viewers = impl_->reverse.find(id);
    if (viewers != impl_->reverse.end()) {
        std::vector<watcher_id> list = std::move(viewers->second);
        impl_->reverse.erase(viewers);
        for (const watcher_id watcher : list) {
            auto watcher_found = impl_->watchers.find(watcher);
            if (watcher_found == impl_->watchers.end()) {
                continue;
            }
            if (watcher_found->second.emitted.find(id) !=
                watcher_found->second.emitted.end()) {
                impl_->emit_leave(watcher, watcher_found->second, id,
                                  record.faction, record.instance);
            }
        }
    }

    impl_->remove_entity_from_cell(id, record);
    impl_->entities.erase(found);
    return status{};
}

status aoi_world::add_watcher(watcher_id id, const watcher_options& options) {
    if (impl_->watchers.find(id) != impl_->watchers.end()) {
        return status(rpc_code::failed_precondition,
                      "aoi_world: watcher already exists");
    }
    auto cell = impl_->grid.cell_of(options.position);
    if (!cell.ok()) {
        return status(cell.error());
    }

    const double cell_size = impl_->grid.options().cell_size;
    if (options.shape == aoi_shape::circle) {
        if (!std::isfinite(options.radius) || options.radius < 0.0 ||
            options.radius > cell_size) {
            return status(rpc_code::invalid_argument,
                          "aoi_world: circle radius must be in [0, cell_size]");
        }
    } else {
        if (!std::isfinite(options.half_width) || options.half_width < 0.0 ||
            options.half_width > cell_size ||
            !std::isfinite(options.half_height) || options.half_height < 0.0 ||
            options.half_height > cell_size) {
            return status(
                rpc_code::invalid_argument,
                "aoi_world: rectangle extents must be in [0, cell_size]");
        }
    }

    watcher_record record;
    record.options = options;
    impl_->add_watcher_to_cell(id, record, cell.value());
    auto inserted = impl_->watchers.emplace(id, std::move(record));
    impl_->mark_watcher_dirty(id, inserted.first->second);
    return status{};
}

status aoi_world::remove_watcher(watcher_id id) {
    auto found = impl_->watchers.find(id);
    if (found == impl_->watchers.end()) {
        return status(rpc_code::not_found, "aoi_world: watcher not found");
    }
    watcher_record& record = found->second;
    for (const auto& entry : record.emitted) {
        impl_->remove_reverse(entry.first, id);
    }
    impl_->remove_watcher_from_cell(id, record);
    impl_->watchers.erase(found);
    return status{};
}

status aoi_world::move_watcher(watcher_id id, vec2 position) {
    auto found = impl_->watchers.find(id);
    if (found == impl_->watchers.end()) {
        return status(rpc_code::not_found, "aoi_world: watcher not found");
    }
    auto cell = impl_->grid.cell_of(position);
    if (!cell.ok()) {
        return status(cell.error());
    }

    watcher_record& record = found->second;
    const std::size_t index = impl_->index_of(cell.value());
    if (index != record.cell_index) {
        impl_->remove_watcher_from_cell(id, record);
        impl_->add_watcher_to_cell(id, record, cell.value());
    }
    record.options.position = position;
    impl_->mark_watcher_dirty(id, record);
    return status{};
}

void aoi_world::tick() {
    impl_->run_tick();
}

std::vector<aoi_event> aoi_world::drain_events() {
    std::vector<aoi_event> events;
    events.swap(impl_->pending);
    return events;
}

result<std::vector<entity_id>> aoi_world::visible(watcher_id id) const {
    auto found = impl_->watchers.find(id);
    if (found == impl_->watchers.end()) {
        return rpc_error(rpc_code::not_found, "aoi_world: watcher not found");
    }
    std::vector<entity_id> ids;
    ids.reserve(found->second.emitted.size());
    for (const auto& entry : found->second.emitted) {
        ids.push_back(entry.first);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

aoi_stats aoi_world::stats() const noexcept {
    aoi_stats result = impl_->counters;
    result.live_entities = static_cast<u64>(impl_->entities.size());
    result.live_watchers = static_cast<u64>(impl_->watchers.size());
    return result;
}

} // namespace crpc::game::aoi