// CRPC -- 2D intersection and ray queries.
//
// Boolean `intersects(a, b)` answers "do the closed regions share a point?".
// Touching counts. `*_hit` functions return the nearest intersection
// parameter as `result<real>`; a miss is `rpc_code::not_found` (the library's
// "no result" code, not a transport failure), and a degenerate query (zero
// direction, invalid shape) is `invalid_argument`. Nothing throws.
//
// Parameter conventions on `*_hit`:
//   * ray:     distance from the origin along the unit direction, in [0, max_length];
//              a ray whose origin is already inside returns 0.
//   * line:    signed distance from the line's reference point; the hit with
//              the smallest absolute t is returned (a line is infinite both ways).
//   * segment: fraction in [0,1] from `a` towards `b`.
//
// Intersection coverage is exact for circle-circle, circle-polygon,
// polygon-polygon, OBB-OBB (SAT), sector-circle, umbrella-circle and every
// ray/line/segment vs circle/AABB/OBB/polygon query. A polygon is treated as
// a simple polygon; a concave polygon uses edge crossing plus vertex
// containment, a convex one uses SAT.
#ifndef CRPC_GAME_GEOM_COLLIDE2D_HPP
#define CRPC_GAME_GEOM_COLLIDE2D_HPP

#include <crpc/config.hpp>
#include <crpc/error.hpp>

#include "shapes2d.hpp"

namespace crpc {
namespace game {
namespace geom {

// --- Region / region ---------------------------------------------------------

[[nodiscard]] CRPC_API bool intersects(const circle& a, const circle& b) noexcept;
[[nodiscard]] CRPC_API bool intersects(const aabb& a, const aabb& b) noexcept;
[[nodiscard]] CRPC_API bool intersects(const obb& a, const obb& b) noexcept;
[[nodiscard]] CRPC_API bool intersects(const circle& a, const obb& b) noexcept;
[[nodiscard]] CRPC_API bool intersects(const circle& a, const aabb& b) noexcept;
[[nodiscard]] CRPC_API bool intersects(const circle& a, const polygon& b) noexcept;
[[nodiscard]] CRPC_API bool intersects(const polygon& a, const polygon& b) noexcept;
// A sector is a circular wedge; this is exact (centre-in-region, radial edge
// distance and arc distance), not a bounding-box test.
[[nodiscard]] CRPC_API bool intersects(const sector& a, const circle& b) noexcept;
// Umbrella = canopy sector + shaft OBB; exact because both parts are exact.
[[nodiscard]] CRPC_API bool intersects(const umbrella& a,
                                       const circle& b) noexcept;

// --- Ray hits ----------------------------------------------------------------

[[nodiscard]] CRPC_API result<real> ray_circle_hit(const ray& r,
                                                   const circle& c);
[[nodiscard]] CRPC_API result<real> ray_aabb_hit(const ray& r, const aabb& box);
[[nodiscard]] CRPC_API result<real> ray_obb_hit(const ray& r, const obb& box);
[[nodiscard]] CRPC_API result<real> ray_polygon_hit(const ray& r,
                                                    const polygon& poly);

[[nodiscard]] CRPC_API bool intersects(const ray& r, const circle& c) noexcept;
[[nodiscard]] CRPC_API bool intersects(const ray& r, const aabb& box) noexcept;
[[nodiscard]] CRPC_API bool intersects(const ray& r, const obb& box) noexcept;
[[nodiscard]] CRPC_API bool intersects(const ray& r,
                                       const polygon& poly) noexcept;

// --- Infinite line hits ------------------------------------------------------

[[nodiscard]] CRPC_API result<real> line_circle_hit(const line& l,
                                                    const circle& c);
[[nodiscard]] CRPC_API result<real> line_aabb_hit(const line& l,
                                                  const aabb& box);
[[nodiscard]] CRPC_API result<real> line_obb_hit(const line& l, const obb& box);
[[nodiscard]] CRPC_API result<real> line_polygon_hit(const line& l,
                                                     const polygon& poly);

[[nodiscard]] CRPC_API bool intersects(const line& l, const circle& c) noexcept;
[[nodiscard]] CRPC_API bool intersects(const line& l, const aabb& box) noexcept;
[[nodiscard]] CRPC_API bool intersects(const line& l, const obb& box) noexcept;
[[nodiscard]] CRPC_API bool intersects(const line& l,
                                       const polygon& poly) noexcept;

// --- Segment hits ------------------------------------------------------------

[[nodiscard]] CRPC_API result<real> segment_circle_hit(const segment& s,
                                                       const circle& c);
[[nodiscard]] CRPC_API result<real> segment_aabb_hit(const segment& s,
                                                     const aabb& box);
[[nodiscard]] CRPC_API result<real> segment_obb_hit(const segment& s,
                                                    const obb& box);
[[nodiscard]] CRPC_API result<real> segment_polygon_hit(const segment& s,
                                                        const polygon& poly);

[[nodiscard]] CRPC_API bool intersects(const segment& s,
                                       const circle& c) noexcept;
[[nodiscard]] CRPC_API bool intersects(const segment& s,
                                       const aabb& box) noexcept;
[[nodiscard]] CRPC_API bool intersects(const segment& s,
                                       const obb& box) noexcept;
[[nodiscard]] CRPC_API bool intersects(const segment& s,
                                       const polygon& poly) noexcept;

} // namespace geom
} // namespace game
} // namespace crpc

#endif // CRPC_GAME_GEOM_COLLIDE2D_HPP