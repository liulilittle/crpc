// CRPC -- 3D intersection and ray queries.
//
// OBB-OBB uses the full 15-axis separating-axis theorem: 3 face normals of A,
// 3 face normals of B, and the 9 pairwise edge cross products. A 4-axis test
// would be wrong for 3D boxes. Touching counts as intersecting.
//
// Sphere-cone (and therefore umbrella3-sphere) uses the exact point-to-cone
// distance: the minimum of the distance to the spherical cap, the distance to
// the finite lateral surface (with the rim clamp) and the apex distance. That
// is a closed-form test, not sampling.
//
// Ray/line/segment hits follow the same parameter conventions as collide2d.hpp:
// ray distance, signed line distance (smallest |t|), segment fraction [0,1].
// Ray vs convex_polyhedron clips the ray against every half-space interval;
// ray vs plane is the single-plane solution.
#ifndef CRPC_GAME_GEOM_COLLIDE3D_HPP
#define CRPC_GAME_GEOM_COLLIDE3D_HPP

#include <crpc/config.hpp>
#include <crpc/error.hpp>

#include "shapes3d.hpp"

namespace crpc {
namespace game {
namespace geom {

// --- Region / region ---------------------------------------------------------

[[nodiscard]] CRPC_API bool intersects(const sphere& a, const sphere& b) noexcept;
[[nodiscard]] CRPC_API bool intersects(const aabb3& a, const aabb3& b) noexcept;
// The 15-axis SAT required by the specification.
[[nodiscard]] CRPC_API bool intersects(const obb3& a, const obb3& b) noexcept;
[[nodiscard]] CRPC_API bool intersects(const sphere& a, const obb3& b) noexcept;
[[nodiscard]] CRPC_API bool intersects(const sphere& a, const aabb3& b) noexcept;
[[nodiscard]] CRPC_API bool intersects(const sphere& a, const cone& b) noexcept;
[[nodiscard]] CRPC_API bool intersects(const sphere& a,
                                       const umbrella3& b) noexcept;

// --- Ray hits ----------------------------------------------------------------

[[nodiscard]] CRPC_API result<real> ray3_sphere_hit(const ray3& r,
                                                    const sphere& s);
[[nodiscard]] CRPC_API result<real> ray3_aabb_hit(const ray3& r,
                                                  const aabb3& box);
[[nodiscard]] CRPC_API result<real> ray3_obb_hit(const ray3& r,
                                                 const obb3& box);
[[nodiscard]] CRPC_API result<real> ray3_plane_hit(const ray3& r,
                                                   const plane3& p);
[[nodiscard]] CRPC_API result<real> ray3_polyhedron_hit(
    const ray3& r, const convex_polyhedron& poly);

[[nodiscard]] CRPC_API bool intersects(const ray3& r, const sphere& s) noexcept;
[[nodiscard]] CRPC_API bool intersects(const ray3& r,
                                       const aabb3& box) noexcept;
[[nodiscard]] CRPC_API bool intersects(const ray3& r, const obb3& box) noexcept;
[[nodiscard]] CRPC_API bool intersects(const ray3& r, const plane3& p) noexcept;
[[nodiscard]] CRPC_API bool intersects(const ray3& r,
                                       const convex_polyhedron& poly) noexcept;

// --- Infinite line hits ------------------------------------------------------

[[nodiscard]] CRPC_API result<real> line3_sphere_hit(const line3& l,
                                                     const sphere& s);
[[nodiscard]] CRPC_API result<real> line3_aabb_hit(const line3& l,
                                                   const aabb3& box);
[[nodiscard]] CRPC_API result<real> line3_obb_hit(const line3& l,
                                                  const obb3& box);
[[nodiscard]] CRPC_API result<real> line3_plane_hit(const line3& l,
                                                    const plane3& p);
[[nodiscard]] CRPC_API result<real> line3_polyhedron_hit(
    const line3& l, const convex_polyhedron& poly);

// --- Segment hits ------------------------------------------------------------

[[nodiscard]] CRPC_API result<real> segment3_sphere_hit(const segment3& s,
                                                        const sphere& sphere);
[[nodiscard]] CRPC_API result<real> segment3_aabb_hit(const segment3& s,
                                                      const aabb3& box);
[[nodiscard]] CRPC_API result<real> segment3_obb_hit(const segment3& s,
                                                     const obb3& box);
[[nodiscard]] CRPC_API result<real> segment3_plane_hit(const segment3& s,
                                                       const plane3& p);
[[nodiscard]] CRPC_API result<real> segment3_polyhedron_hit(
    const segment3& s, const convex_polyhedron& poly);

} // namespace geom
} // namespace game
} // namespace crpc

#endif // CRPC_GAME_GEOM_COLLIDE3D_HPP