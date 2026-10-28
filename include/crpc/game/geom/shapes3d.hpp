// CRPC -- 3D hitbox shapes.
//
// These are the genuine 3D counterparts of the 2D shapes, not a 2D shape with
// a z field:
//
//   circle  <-> sphere
//   sector  <-> cone        (spherical cone: apex + axis + half angle + reach)
//   umbrella<-> umbrella3   (spherical-cone canopy + rectangular-prism shaft)
//   obb     <-> obb3        (15-axis SAT, see collide3d.hpp)
//   ray     <-> ray3
//   segment <-> segment3
//   line    <-> line3
//   polygon <-> convex_polyhedron (half-space containment)
//
// The shared low-level work (vectors, length, dot/cross, orthonormal bases,
// the projectile quadratics) lives in vec.hpp; the shapes themselves stay
// explicit because the 3D primitives are genuinely different solids. See the
// "2D/3D organisation" note in vec.hpp.
//
// Angle/direction convention: all directions are arbitrary unit vectors, so
// any heading in 360 degrees is representable. The cone's `half_angle` is the
// half opening measured from `axis` and is in [0, pi]; a cone with half_angle
// >= pi is the closed ball of radius `reach`. The 350/20-degree wrap problem
// is a 2D angle-interval issue; in 3D it is handled by comparing against the
// axis directly (`dot(direction, axis) >= cos(half_angle)`), which is
// wrap-free for every heading.
//
// Umbrella3 is the union of the spherical-cone canopy and a rectangular-prism
// shaft of half-width `handle_half_width` extending `handle_length` from the
// apex along `-axis`. The apex is always inside.
#ifndef CRPC_GAME_GEOM_SHAPES3D_HPP
#define CRPC_GAME_GEOM_SHAPES3D_HPP

#include <cstddef>
#include <limits>
#include <vector>

#include <crpc/config.hpp>

#include "vec.hpp"

namespace crpc {
namespace game {
namespace geom {

// --- 3D axis-aligned box -----------------------------------------------------

struct CRPC_API aabb3 {
    vec3 min{std::numeric_limits<real>::infinity(),
             std::numeric_limits<real>::infinity(),
             std::numeric_limits<real>::infinity()};
    vec3 max{-std::numeric_limits<real>::infinity(),
             -std::numeric_limits<real>::infinity(),
             -std::numeric_limits<real>::infinity()};

    static aabb3 empty() noexcept;
    static aabb3 unbounded() noexcept;

    bool valid() const noexcept;
    bool finite() const noexcept;
    bool contains(vec3 point, real eps = k_epsilon) const noexcept;
    bool overlaps(const aabb3& other, real eps = k_epsilon) const noexcept;
    void expand(vec3 point) noexcept;
    void expand(const aabb3& other) noexcept;

    vec3 center() const noexcept;
    vec3 extents() const noexcept;
    vec3 centroid() const noexcept;
    vec3 edge_anchor() const noexcept; // +x face midpoint
    aabb3 bounds() const noexcept;
};

// --- Sphere ------------------------------------------------------------------

struct CRPC_API sphere {
    vec3 center{};
    real radius = 0.0f;

    bool contains(vec3 point) const noexcept;
    aabb3 bounds() const noexcept;
    vec3 centroid() const noexcept;
    vec3 edge_anchor() const noexcept; // +x rim point
};

// --- Spherical cone ----------------------------------------------------------

struct CRPC_API cone {
    vec3 apex{};
    vec3 axis{1.0f, 0.0f, 0.0f}; // normalised on use
    real half_angle = 0.0f;      // [0, pi]; >= pi is a full ball
    real reach = 0.0f;           // radius from the apex

    bool contains(vec3 point) const noexcept;
    // Exact AABB of the spherical cone (not the bounding sphere).
    aabb3 bounds() const noexcept;
    vec3 centroid() const noexcept;
    vec3 edge_anchor() const noexcept; // apex
};

// --- Oriented box (3D OBB) ---------------------------------------------------

struct CRPC_API obb3 {
    vec3 center{};
    vec3 half_extents{};
    vec3 axis_x{1.0f, 0.0f, 0.0f};
    vec3 axis_y{0.0f, 1.0f, 0.0f};
    vec3 axis_z{0.0f, 0.0f, 1.0f};

    // Builds an OBB from a yaw/pitch/roll triple (radians), applied as
    // Rz(yaw) * Ry(pitch) * Rx(roll); useful for tests and gameplay data.
    static obb3 from_euler(vec3 center, vec3 half_extents, real yaw, real pitch,
                           real roll) noexcept;

    vec3 corner(std::size_t index) const noexcept; // 0..7, wraps
    bool contains(vec3 point) const noexcept;
    aabb3 bounds() const noexcept;
    vec3 centroid() const noexcept;
    vec3 edge_anchor() const noexcept; // centre + axis_x * half_extents.x
};

// --- 3D umbrella -------------------------------------------------------------

struct CRPC_API umbrella3 {
    vec3 apex{};
    vec3 axis{1.0f, 0.0f, 0.0f};
    real half_angle = 0.0f;
    real reach = 0.0f;
    real handle_length = 0.0f;
    real handle_half_width = 0.0f;

    cone canopy() const noexcept;
    obb3 handle() const noexcept;
    bool contains(vec3 point) const noexcept;
    aabb3 bounds() const noexcept;
    vec3 centroid() const noexcept;
    vec3 edge_anchor() const noexcept; // apex
};

// --- Ray / segment / line ----------------------------------------------------

struct CRPC_API ray3 {
    vec3 origin{};
    vec3 direction{1.0f, 0.0f, 0.0f};
    real max_length = 0.0f;

    vec3 unit_direction() const noexcept;
    bool contains(vec3 point) const noexcept;
    aabb3 bounds() const noexcept;
    vec3 centroid() const noexcept;
    vec3 edge_anchor() const noexcept;
};

struct CRPC_API segment3 {
    vec3 a{};
    vec3 b{};

    bool contains(vec3 point) const noexcept;
    aabb3 bounds() const noexcept;
    vec3 centroid() const noexcept;
    vec3 edge_anchor() const noexcept;
};

struct CRPC_API line3 {
    vec3 point{};
    vec3 direction{1.0f, 0.0f, 0.0f};

    vec3 unit_direction() const noexcept;
    bool contains(vec3 query) const noexcept;
    aabb3 bounds() const noexcept; // unbounded
    vec3 centroid() const noexcept;
    vec3 edge_anchor() const noexcept;
};

// --- Plane -------------------------------------------------------------------

struct CRPC_API plane3 {
    vec3 point{};
    vec3 normal{0.0f, 1.0f, 0.0f}; // normalised on use

    real signed_distance(vec3 query) const noexcept;
    bool contains(vec3 query) const noexcept;
};

// --- Convex polyhedron -------------------------------------------------------

// A convex polyhedron described by its vertices (for the AABB/anchor) and one
// outward half-space per face. Point containment is the conjunction
// dot(normal, p - face_point) <= 0; a face winding is irrelevant because the
// normal is supplied explicitly. `from_box` builds the six planes of an AABB.
struct CRPC_API convex_polyhedron {
    std::vector<vec3> vertices;
    std::vector<plane3> planes;

    static convex_polyhedron from_box(const aabb3& box) noexcept;

    bool valid() const noexcept;
    bool contains(vec3 point) const noexcept;
    aabb3 bounds() const noexcept;
    vec3 centroid() const noexcept; // average of vertices
    vec3 edge_anchor() const noexcept;
};

// --- Anchor selection --------------------------------------------------------

[[nodiscard]] CRPC_API vec3 anchor_point(const sphere& shape,
                                         anchor_mode mode) noexcept;
[[nodiscard]] CRPC_API vec3 anchor_point(const cone& shape,
                                         anchor_mode mode) noexcept;
[[nodiscard]] CRPC_API vec3 anchor_point(const umbrella3& shape,
                                         anchor_mode mode) noexcept;
[[nodiscard]] CRPC_API vec3 anchor_point(const aabb3& shape,
                                         anchor_mode mode) noexcept;
[[nodiscard]] CRPC_API vec3 anchor_point(const obb3& shape,
                                         anchor_mode mode) noexcept;
[[nodiscard]] CRPC_API vec3 anchor_point(const ray3& shape,
                                         anchor_mode mode) noexcept;
[[nodiscard]] CRPC_API vec3 anchor_point(const segment3& shape,
                                         anchor_mode mode) noexcept;
[[nodiscard]] CRPC_API vec3 anchor_point(const line3& shape,
                                         anchor_mode mode) noexcept;
[[nodiscard]] CRPC_API vec3 anchor_point(const convex_polyhedron& shape,
                                         anchor_mode mode) noexcept;

} // namespace geom
} // namespace game
} // namespace crpc

#endif // CRPC_GAME_GEOM_SHAPES3D_HPP