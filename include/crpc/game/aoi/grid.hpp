// CRPC -- area-of-interest grid.
//
// A bounded, uniform grid. The world is the closed rectangle
// [origin_x, origin_x + columns * cell_size] x [origin_y, origin_y + rows *
// cell_size]: a position exactly on the far edge is valid and belongs to the
// last cell. This is deliberate -- game coordinates are commonly inclusive on
// both ends, and clamping the far edge keeps an entity on the map boundary out
// of the "outside world" error path.
//
// The two view helpers implement the two AOI shapes:
//
//   rectangle_cells  every cell overlapped by the axis-aligned rectangle
//                    [center +/- half_width] x [center +/- half_height];
//   circle_cells     every cell that actually intersects the disc, not merely
//                    every cell in its bounding box.
//
// Both are closed-interval tests: a shape that only touches a cell edge still
// includes that cell. That matches the visibility test in `aoi_world`, which is
// also inclusive, so a cell set can never omit an entity the exact test accepts.
#ifndef CRPC_GAME_AOI_GRID_HPP
#define CRPC_GAME_AOI_GRID_HPP

#include <cstddef>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

namespace crpc::game::aoi {

struct vec2 {
    double x = 0.0;
    double y = 0.0;
};

constexpr bool operator==(const vec2& a, const vec2& b) noexcept {
    return a.x == b.x && a.y == b.y;
}

constexpr bool operator!=(const vec2& a, const vec2& b) noexcept {
    return !(a == b);
}

struct cell_coord {
    std::size_t column = 0;
    std::size_t row = 0;
};

constexpr bool operator==(const cell_coord& a, const cell_coord& b) noexcept {
    return a.column == b.column && a.row == b.row;
}

constexpr bool operator!=(const cell_coord& a, const cell_coord& b) noexcept {
    return !(a == b);
}

struct grid_options {
    double origin_x = 0.0;
    double origin_y = 0.0;
    double cell_size = 16.0;
    std::size_t columns = 64;
    std::size_t rows = 64;
};

class CRPC_API aoi_grid {
public:
    // Validates the options and builds a grid. The failure is
    // `rpc_code::invalid_argument` for a non-finite origin, a non-positive or
    // non-finite cell size, a zero column/row count, or a size that overflows.
    [[nodiscard]] static result<aoi_grid> create(const grid_options& options);

    [[nodiscard]] const grid_options& options() const noexcept { return options_; }
    [[nodiscard]] double width() const noexcept { return width_; }
    [[nodiscard]] double height() const noexcept { return height_; }

    // True when `point` is finite and lies inside the closed world rectangle.
    [[nodiscard]] bool contains(vec2 point) const noexcept;

    // The cell that owns `point`. The far edge belongs to the last cell.
    // A point outside the world (or non-finite) is `invalid_argument`.
    [[nodiscard]] result<cell_coord> cell_of(vec2 point) const;

    // Cells overlapped by the rectangle. Empty when the centre is outside the
    // world or an extent is negative or non-finite.
    [[nodiscard]] std::vector<cell_coord>
    rectangle_cells(vec2 center, double half_width, double half_height) const;

    // Cells intersecting the disc. Empty when the centre is outside the world
    // or the radius is negative or non-finite.
    [[nodiscard]] std::vector<cell_coord> circle_cells(vec2 center,
                                                       double radius) const;

    // The 3x3 neighbourhood of `center`, clamped to the grid. Empty when
    // `center` is not a cell of this grid.
    [[nodiscard]] std::vector<cell_coord> neighborhood(cell_coord center) const;

private:
    aoi_grid() = default;

    grid_options options_{};
    double width_ = 0.0;
    double height_ = 0.0;
};

} // namespace crpc::game::aoi

#endif // CRPC_GAME_AOI_GRID_HPP