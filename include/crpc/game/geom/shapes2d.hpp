// CRPC -- 2D hitbox shapes.
//
// Every shape below answers three questions:
//   * contains(point)   -- is the point in the closed region?
//   * bounds()          -- an axis-aligned bounding box for the broad phase;
//   * centroid() / edge_anchor() -- the two placement points used by
//     `anchor_mode::center` and `anchor_mode::edge`.
//
// Boundary convention: a point exactly on the boundary is INSIDE. Predicates
// use `k_epsilon`, so "exactly" means within float noise; the tests pin this
// down for vertices, edges and tangencies.
//
// ---------------------------------------------------------------------------
// Umbrella definition
// ---------------------------------------------------------------------------
// "Umbrella" is not a standard computational-geometry primitive, so the
// library defines it explicitly. An umbrella is the union of:
//
//   1. a CANOPY: the closed circular sector with apex `apex`, axis
//      `direction`, half opening `half_angle` and radius `reach`; and
//   2. a SHAFT: a rotated rectangle of half-width `handle_half_width` that
//      extends `handle_length` from the apex along `-direction` (the grip).
//
// This is the literal 2D silhouette of a held umbrella. It also covers the
// alternative "cone with a straight extension" reading: with
// `handle_length == 0` the shaft disappears and the umbrella is exactly the
// canopy sector; with `half_angle == 0` it is the shaft plus the axis ray.
// The apex itself is always inside (it is the shared vertex of canopy and
// shaft). The canopy is a filled sector, not a ring: the user asked for
// "弧形/扇形", and a hitbox needs the filled area.
//
// `direction` is normalised on use; a zero direction is treated as +x for the
// shaft angle.
//
// ---------------------------------------------------------------------------
// Angle convention
// ---------------------------------------------------------------------------
// All angles are radians, measured counter-clockwise from +x. `start_angle`
// may be any real; the angular interval is
// [normalise(start), normalise(start) + sweep] taken modulo 2*pi. A sweep of
// 2*pi is a full disc; a sweep <= 0 is the empty set. The 350..370 degree
// case is the canonical wrap test.
#ifndef CRPC_GAME_GEOM_SHAPES2D_HPP
#define CRPC_GAME_GEOM_SHAPES2D_HPP

#include <cstddef>
#include <limits>
#include <vector>

#include <crpc/config.hpp>

#include "vec.hpp"

namespace crpc {
namespace game {
namespace geom {

// --- Axis-aligned box --------------------------------------------------------

struct CRPC_API aabb {
    vec2 min{std::numeric_limits<real>::infinity(),
             std::numeric_limits<real>::infinity()};
    vec2 max{-std::numeric_limits<real>::infinity(),
             -std::numeric_limits<real>::infinity()};

    static aabb empty() noexcept;
    static aabb unbounded() noexcept;

    bool valid() const noexcept;
    bool finite() const noexcept;
    bool contains(vec2 point, real eps = k_epsilon) const noexcept;
    bool overlaps(const aabb& other, real eps = k_epsilon) const noexcept;
    void expand(vec2 point) noexcept;
    void expand(const aabb& other) noexcept;

    vec2 center() const noexcept;
    vec2 extents() const noexcept;

    // Anchor points when the box is used as a shape.
    vec2 centroid() const noexcept;
    vec2 edge_anchor() const noexcept;
    aabb bounds() const noexcept;
};

// --- Circle ------------------------------------------------------------------

struct CRPC_API circle {
    vec2 center{};
    real radius = 0.0f;

    bool contains(vec2 point) const noexcept;
    aabb bounds() const noexcept;
    vec2 centroid() const noexcept;
    vec2 edge_anchor() const noexcept;
};

// --- Sector / arc ------------------------------------------------------------

struct CRPC_API sector {
    vec2 center{};
    real radius = 0.0f;
    real start_angle = 0.0f; // radians, CCW from +x; any real
    real sweep = 0.0f;       // radians; <=0 empty, >=2*pi full disc

    bool full() const noexcept;
    bool contains(vec2 point) const noexcept;
    aabb bounds() const noexcept;
    vec2 centroid() const noexcept;
    vec2 edge_anchor() const noexcept; // apex (the fan opens from here)
};

// --- Oriented box ------------------------------------------------------------

struct CRPC_API obb {
    vec2 center{};
    vec2 half_extents{};
    real rotation = 0.0f; // radians, CCW

    vec2 axis_x() const noexcept;
    vec2 axis_y() const noexcept;
    vec2 corner(std::size_t index) const noexcept; // 0..3, wraps
    bool contains(vec2 point) const noexcept;
    aabb bounds() const noexcept;
    vec2 centroid() const noexcept;
    vec2 edge_anchor() const noexcept; // +x face midpoint
};

// --- Umbrella ----------------------------------------------------------------

struct CRPC_API umbrella {
    vec2 apex{};
    vec2 direction{1.0f, 0.0f};
    real half_angle = 0.0f;
    real reach = 0.0f;
    real handle_length = 0.0f;
    real handle_half_width = 0.0f;

    sector canopy() const noexcept;
    obb handle() const noexcept;
    bool contains(vec2 point) const noexcept;
    aabb bounds() const noexcept;
    vec2 centroid() const noexcept;
    vec2 edge_anchor() const noexcept; // apex
};

// --- Ray ---------------------------------------------------------------------

struct CRPC_API ray {
    vec2 origin{};
    vec2 direction{1.0f, 0.0f};
    real max_length = 0.0f;

    vec2 unit_direction() const noexcept;
    bool contains(vec2 point) const noexcept;
    aabb bounds() const noexcept;
    vec2 centroid() const noexcept;
    vec2 edge_anchor() const noexcept; // origin
};

// --- Segment -----------------------------------------------------------------

struct CRPC_API segment {
    vec2 a{};
    vec2 b{};

    bool contains(vec2 point) const noexcept;
    aabb bounds() const noexcept;
    vec2 centroid() const noexcept;
    vec2 edge_anchor() const noexcept; // a
};

// --- Infinite line -----------------------------------------------------------

// An infinite line has no boundary and therefore no finite AABB. `bounds()`
// returns `aabb::unbounded()`; `centroid()` and `edge_anchor()` are both the
// reference point `point`. This is the documented difference from `segment`.
struct CRPC_API line {
    vec2 point{};
    vec2 direction{1.0f, 0.0f};

    vec2 unit_direction() const noexcept;
    bool contains(vec2 query) const noexcept;
    aabb bounds() const noexcept;
    vec2 centroid() const noexcept;
    vec2 edge_anchor() const noexcept;
};

// --- Polygon -----------------------------------------------------------------

// An arbitrary simple polygon (convex or concave). Point containment uses ray
// casting with a boundary pre-check so vertices, edges and duplicate points
// are classified as inside. Degenerate (zero-area) input is treated as its
// boundary, which is the only sensible closed-set reading.
struct CRPC_API polygon {
    std::vector<vec2> vertices;

    std::size_t size() const noexcept;
    bool contains(vec2 point) const noexcept;
    aabb bounds() const noexcept;
    vec2 centroid() const noexcept;
    vec2 edge_anchor() const noexcept; // first vertex
    bool is_convex() const noexcept;
};

// --- Anchor selection --------------------------------------------------------

[[nodiscard]] CRPC_API vec2 anchor_point(const circle& shape,
                                         anchor_mode mode) noexcept;
[[nodiscard]] CRPC_API vec2 anchor_point(const sector& shape,
                                         anchor_mode mode) noexcept;
[[nodiscard]] CRPC_API vec2 anchor_point(const umbrella& shape,
                                         anchor_mode mode) noexcept;
[[nodiscard]] CRPC_API vec2 anchor_point(const aabb& shape,
                                         anchor_mode mode) noexcept;
[[nodiscard]] CRPC_API vec2 anchor_point(const obb& shape,
                                         anchor_mode mode) noexcept;
[[nodiscard]] CRPC_API vec2 anchor_point(const ray& shape,
                                         anchor_mode mode) noexcept;
[[nodiscard]] CRPC_API vec2 anchor_point(const segment& shape,
                                         anchor_mode mode) noexcept;
[[nodiscard]] CRPC_API vec2 anchor_point(const line& shape,
                                         anchor_mode mode) noexcept;
[[nodiscard]] CRPC_API vec2 anchor_point(const polygon& shape,
                                         anchor_mode mode) noexcept;

} // namespace geom
} // namespace game
} // namespace crpc

#endif // CRPC_GAME_GEOM_SHAPES2D_HPP