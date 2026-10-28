// CRPC -- area-of-interest grid: cell mapping and view cell sets.

#include <cmath>
#include <limits>
#include <string>

#include <crpc/game/aoi/grid.hpp>

namespace crpc::game::aoi {
namespace {

bool is_finite(double value) noexcept {
    return std::isfinite(value);
}

// Cell index for a coordinate already known to belong to [origin, origin +
// extent]. The far edge (and anything beyond it, which callers only pass after
// clamping) maps to the last cell.
std::size_t axis_cell(double value, double origin, double extent, double size,
                      std::size_t count) noexcept {
    if (value <= origin) {
        return 0;
    }
    if (value >= origin + extent) {
        return count - 1;
    }
    const double local = (value - origin) / size;
    if (local <= 0.0) {
        return 0;
    }
    const double floored = std::floor(local);
    if (floored >= static_cast<double>(count)) {
        return count - 1;
    }
    return static_cast<std::size_t>(floored);
}

// Closest point on [low, high] to `value`.
double clamp_to(double value, double low, double high) noexcept {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

} // namespace

result<aoi_grid> aoi_grid::create(const grid_options& options) {
    if (!is_finite(options.origin_x) || !is_finite(options.origin_y)) {
        return rpc_error(rpc_code::invalid_argument,
                         "aoi_grid: origin must be finite");
    }
    if (!is_finite(options.cell_size) || options.cell_size <= 0.0) {
        return rpc_error(rpc_code::invalid_argument,
                         "aoi_grid: cell_size must be finite and positive");
    }
    if (options.columns == 0 || options.rows == 0) {
        return rpc_error(rpc_code::invalid_argument,
                         "aoi_grid: columns and rows must be non-zero");
    }
    if (options.columns >
        std::numeric_limits<std::size_t>::max() / options.rows) {
        return rpc_error(rpc_code::invalid_argument,
                         "aoi_grid: cell count overflows size_t");
    }

    aoi_grid grid;
    grid.options_ = options;
    grid.width_ = static_cast<double>(options.columns) * options.cell_size;
    grid.height_ = static_cast<double>(options.rows) * options.cell_size;
    if (!is_finite(grid.width_) || !is_finite(grid.height_)) {
        return rpc_error(rpc_code::invalid_argument,
                         "aoi_grid: world extent must be finite");
    }
    return grid;
}

bool aoi_grid::contains(vec2 point) const noexcept {
    if (!is_finite(point.x) || !is_finite(point.y)) {
        return false;
    }
    return point.x >= options_.origin_x &&
           point.x <= options_.origin_x + width_ &&
           point.y >= options_.origin_y &&
           point.y <= options_.origin_y + height_;
}

result<cell_coord> aoi_grid::cell_of(vec2 point) const {
    if (!contains(point)) {
        return rpc_error(rpc_code::invalid_argument,
                         "aoi_grid: position is outside the world");
    }
    return cell_coord{
        axis_cell(point.x, options_.origin_x, width_, options_.cell_size,
                  options_.columns),
        axis_cell(point.y, options_.origin_y, height_, options_.cell_size,
                  options_.rows)};
}

std::vector<cell_coord>
aoi_grid::rectangle_cells(vec2 center, double half_width,
                          double half_height) const {
    std::vector<cell_coord> cells;
    if (!contains(center) || !is_finite(half_width) ||
        !is_finite(half_height) || half_width < 0.0 || half_height < 0.0) {
        return cells;
    }

    const double left = center.x - half_width;
    const double right = center.x + half_width;
    const double bottom = center.y - half_height;
    const double top = center.y + half_height;
    if (right < options_.origin_x || left > options_.origin_x + width_ ||
        top < options_.origin_y || bottom > options_.origin_y + height_) {
        return cells;
    }

    const std::size_t first_column =
        axis_cell(left, options_.origin_x, width_, options_.cell_size,
                  options_.columns);
    const std::size_t last_column =
        axis_cell(right, options_.origin_x, width_, options_.cell_size,
                  options_.columns);
    const std::size_t first_row =
        axis_cell(bottom, options_.origin_y, height_, options_.cell_size,
                  options_.rows);
    const std::size_t last_row =
        axis_cell(top, options_.origin_y, height_, options_.cell_size,
                  options_.rows);

    cells.reserve((last_column - first_column + 1) *
                  (last_row - first_row + 1));
    for (std::size_t row = first_row; row <= last_row; ++row) {
        for (std::size_t column = first_column; column <= last_column;
             ++column) {
            cells.push_back(cell_coord{column, row});
        }
    }
    return cells;
}

std::vector<cell_coord> aoi_grid::circle_cells(vec2 center,
                                               double radius) const {
    std::vector<cell_coord> cells;
    if (!contains(center) || !is_finite(radius) || radius < 0.0) {
        return cells;
    }

    const double left = center.x - radius;
    const double right = center.x + radius;
    const double bottom = center.y - radius;
    const double top = center.y + radius;

    const std::size_t first_column =
        axis_cell(left, options_.origin_x, width_, options_.cell_size,
                  options_.columns);
    const std::size_t last_column =
        axis_cell(right, options_.origin_x, width_, options_.cell_size,
                  options_.columns);
    const std::size_t first_row =
        axis_cell(bottom, options_.origin_y, height_, options_.cell_size,
                  options_.rows);
    const std::size_t last_row =
        axis_cell(top, options_.origin_y, height_, options_.cell_size,
                  options_.rows);

    const double radius_squared = radius * radius;
    for (std::size_t row = first_row; row <= last_row; ++row) {
        const double low_y =
            options_.origin_y + static_cast<double>(row) * options_.cell_size;
        const double high_y = low_y + options_.cell_size;
        for (std::size_t column = first_column; column <= last_column;
             ++column) {
            const double low_x = options_.origin_x +
                                 static_cast<double>(column) *
                                     options_.cell_size;
            const double high_x = low_x + options_.cell_size;
            const double nearest_x = clamp_to(center.x, low_x, high_x);
            const double nearest_y = clamp_to(center.y, low_y, high_y);
            const double dx = center.x - nearest_x;
            const double dy = center.y - nearest_y;
            if (dx * dx + dy * dy <= radius_squared) {
                cells.push_back(cell_coord{column, row});
            }
        }
    }
    return cells;
}

std::vector<cell_coord> aoi_grid::neighborhood(cell_coord center) const {
    std::vector<cell_coord> cells;
    if (center.column >= options_.columns || center.row >= options_.rows) {
        return cells;
    }

    const std::size_t first_column = center.column == 0 ? 0 : center.column - 1;
    const std::size_t first_row = center.row == 0 ? 0 : center.row - 1;
    const std::size_t last_column =
        center.column + 1 < options_.columns ? center.column + 1
                                             : options_.columns - 1;
    const std::size_t last_row = center.row + 1 < options_.rows
                                     ? center.row + 1
                                     : options_.rows - 1;

    cells.reserve((last_column - first_column + 1) *
                  (last_row - first_row + 1));
    for (std::size_t row = first_row; row <= last_row; ++row) {
        for (std::size_t column = first_column; column <= last_column;
             ++column) {
            cells.push_back(cell_coord{column, row});
        }
    }
    return cells;
}

} // namespace crpc::game::aoi