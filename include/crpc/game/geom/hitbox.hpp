// CRPC -- anchored hitboxes: the center/edge placement requirement.
//
// A `hitbox` is one of the 2D shapes plus a placement:
//
//   * `position` is the player/actor position;
//   * `mode` selects which point of the local shape is pinned there
//     (`anchor_mode::center` -> centroid, `anchor_mode::edge` -> the shape's
//     designated boundary point);
//   * `rotation` and `scale` are applied about that pinned point.
//
// Concretely, with local anchor `a`, a world query `p` is mapped back to local
// space as
//
//     local = a + R(-rotation) * (p - position) / scale
//
// and the shape's own `contains(local)` decides the hit. The same shape under
// the two modes therefore gives different answers whenever the centroid and
// the boundary anchor differ -- exactly the property the tests assert for the
// circle, sector, umbrella, box/OBB, ray, segment and polygon. An infinite
// line has no boundary, so both modes coincide there (documented).
//
// `broad_phase` is the first stage: it only compares AABBs. `contains` is the
// exact second stage. A non-positive `scale` produces an empty hitbox (contains
// is false, bounds are empty) instead of dividing by zero.
//
// `hitbox3` is the 3D counterpart; its orientation is an explicit orthonormal
// basis (`basis3`), so any 3D heading is representable without Euler-angle
// wrap surprises.
#ifndef CRPC_GAME_GEOM_HITBOX_HPP
#define CRPC_GAME_GEOM_HITBOX_HPP

#include <variant>

#include <crpc/config.hpp>

#include "collide2d.hpp"
#include "collide3d.hpp"
#include "shapes2d.hpp"
#include "shapes3d.hpp"
#include "vec.hpp"

namespace crpc {
namespace game {
namespace geom {

using shape2d_variant =
    std::variant<circle, sector, umbrella, aabb, obb, ray, segment, line, polygon>;
using shape3d_variant =
    std::variant<sphere, cone, umbrella3, aabb3, obb3, ray3, segment3, line3,
                 convex_polyhedron>;

class CRPC_API hitbox {
public:
    hitbox(const circle& shape, anchor_mode mode, vec2 position,
           real rotation = 0.0f, real scale = 1.0f) noexcept;
    hitbox(const sector& shape, anchor_mode mode, vec2 position,
           real rotation = 0.0f, real scale = 1.0f) noexcept;
    hitbox(const umbrella& shape, anchor_mode mode, vec2 position,
           real rotation = 0.0f, real scale = 1.0f) noexcept;
    hitbox(const aabb& shape, anchor_mode mode, vec2 position,
           real rotation = 0.0f, real scale = 1.0f) noexcept;
    hitbox(const obb& shape, anchor_mode mode, vec2 position,
           real rotation = 0.0f, real scale = 1.0f) noexcept;
    hitbox(const ray& shape, anchor_mode mode, vec2 position,
           real rotation = 0.0f, real scale = 1.0f) noexcept;
    hitbox(const segment& shape, anchor_mode mode, vec2 position,
           real rotation = 0.0f, real scale = 1.0f) noexcept;
    hitbox(const line& shape, anchor_mode mode, vec2 position,
           real rotation = 0.0f, real scale = 1.0f) noexcept;
    hitbox(const polygon& shape, anchor_mode mode, vec2 position,
           real rotation = 0.0f, real scale = 1.0f) noexcept;

    // Exact narrow-phase test in world space.
    [[nodiscard]] bool contains(vec2 world_point) const noexcept;
    // First broad-phase stage.
    [[nodiscard]] bool broad_phase(vec2 world_point) const noexcept;
    [[nodiscard]] bool broad_phase(const aabb& world_bounds) const noexcept;
    // Conservative world AABB (the transformed local AABB).
    [[nodiscard]] aabb world_bounds() const noexcept;

    const shape2d_variant& shape() const noexcept;
    anchor_mode mode() const noexcept;
    vec2 position() const noexcept;
    real rotation() const noexcept;
    real scale() const noexcept;
    // The local point pinned to `position`.
    [[nodiscard]] vec2 local_anchor() const noexcept;
    // Always `position`; exposed for symmetry with local_anchor().
    [[nodiscard]] vec2 world_anchor() const noexcept;

private:
    shape2d_variant shape_;
    anchor_mode mode_ = anchor_mode::center;
    vec2 position_{};
    real rotation_ = 0.0f;
    real scale_ = 1.0f;
};

class CRPC_API hitbox3 {
public:
    hitbox3(const sphere& shape, anchor_mode mode, vec3 position,
            basis3 orientation = basis3{},
            real scale = 1.0f) noexcept;
    hitbox3(const cone& shape, anchor_mode mode, vec3 position,
            basis3 orientation = basis3{},
            real scale = 1.0f) noexcept;
    hitbox3(const umbrella3& shape, anchor_mode mode, vec3 position,
            basis3 orientation = basis3{},
            real scale = 1.0f) noexcept;
    hitbox3(const aabb3& shape, anchor_mode mode, vec3 position,
            basis3 orientation = basis3{},
            real scale = 1.0f) noexcept;
    hitbox3(const obb3& shape, anchor_mode mode, vec3 position,
            basis3 orientation = basis3{},
            real scale = 1.0f) noexcept;
    hitbox3(const ray3& shape, anchor_mode mode, vec3 position,
            basis3 orientation = basis3{},
            real scale = 1.0f) noexcept;
    hitbox3(const segment3& shape, anchor_mode mode, vec3 position,
            basis3 orientation = basis3{},
            real scale = 1.0f) noexcept;
    hitbox3(const line3& shape, anchor_mode mode, vec3 position,
            basis3 orientation = basis3{},
            real scale = 1.0f) noexcept;
    hitbox3(const convex_polyhedron& shape, anchor_mode mode, vec3 position,
            basis3 orientation = basis3{},
            real scale = 1.0f) noexcept;

    [[nodiscard]] bool contains(vec3 world_point) const noexcept;
    [[nodiscard]] bool broad_phase(vec3 world_point) const noexcept;
    [[nodiscard]] bool broad_phase(const aabb3& world_bounds) const noexcept;
    [[nodiscard]] aabb3 world_bounds() const noexcept;

    const shape3d_variant& shape() const noexcept;
    anchor_mode mode() const noexcept;
    vec3 position() const noexcept;
    basis3 orientation() const noexcept;
    real scale() const noexcept;
    [[nodiscard]] vec3 local_anchor() const noexcept;
    [[nodiscard]] vec3 world_anchor() const noexcept;

private:
    shape3d_variant shape_;
    anchor_mode mode_ = anchor_mode::center;
    vec3 position_{};
    basis3 orientation_{};
    real scale_ = 1.0f;
};

} // namespace geom
} // namespace game
} // namespace crpc

#endif // CRPC_GAME_GEOM_HITBOX_HPP