// CRPC -- 3D intersection and ray queries. See collide3d.hpp.
#include <crpc/game/geom/collide3d.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

#include "geometry_detail.hpp"

namespace crpc {
namespace game {
namespace geom {

namespace {

rpc_error miss(const char* what) {
    return rpc_error{rpc_code::not_found, what};
}

rpc_error invalid(const char* what) {
    return rpc_error{rpc_code::invalid_argument, what};
}

bool valid_direction(vec3 direction) noexcept {
    return length_squared(direction) > k_epsilon * k_epsilon;
}

real ray_limit(const ray3& r) noexcept {
    return r.max_length > 0.0f ? r.max_length : 0.0f;
}

double clamp_d(double v, double lo, double hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

bool sphere_sphere(const sphere& a, const sphere& b) noexcept {
    const double ra = std::max(static_cast<double>(a.radius), 0.0);
    const double rb = std::max(static_cast<double>(b.radius), 0.0);
    const vec3 d = b.center - a.center;
    const double dx = static_cast<double>(d.x);
    const double dy = static_cast<double>(d.y);
    const double dz = static_cast<double>(d.z);
    const double reach = ra + rb + static_cast<double>(k_epsilon);
    return dx * dx + dy * dy + dz * dz <= reach * reach;
}

double distance_point_obb3(vec3 point, const obb3& box) noexcept {
    const vec3 d = point - box.center;
    const double hx = std::max(static_cast<double>(box.half_extents.x), 0.0);
    const double hy = std::max(static_cast<double>(box.half_extents.y), 0.0);
    const double hz = std::max(static_cast<double>(box.half_extents.z), 0.0);
    const double lx =
        clamp_d(static_cast<double>(dot(d, box.axis_x)), -hx, hx);
    const double ly =
        clamp_d(static_cast<double>(dot(d, box.axis_y)), -hy, hy);
    const double lz =
        clamp_d(static_cast<double>(dot(d, box.axis_z)), -hz, hz);
    const vec3 residual =
        d - box.axis_x * static_cast<real>(lx) -
        box.axis_y * static_cast<real>(ly) -
        box.axis_z * static_cast<real>(lz);
    return static_cast<double>(length(residual));
}

double distance_point_aabb3(vec3 point, const aabb3& box) noexcept {
    const double x =
        clamp_d(static_cast<double>(point.x), static_cast<double>(box.min.x),
                static_cast<double>(box.max.x));
    const double y =
        clamp_d(static_cast<double>(point.y), static_cast<double>(box.min.y),
                static_cast<double>(box.max.y));
    const double z =
        clamp_d(static_cast<double>(point.z), static_cast<double>(box.min.z),
                static_cast<double>(box.max.z));
    const double dx = static_cast<double>(point.x) - x;
    const double dy = static_cast<double>(point.y) - y;
    const double dz = static_cast<double>(point.z) - z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

double distance_point_cone(vec3 point, const cone& c) noexcept {
    const vec3 u = normalized(c.axis);
    if (length_squared(u) <= 0.0f) {
        return static_cast<double>(distance(point, c.apex));
    }
    const double half = clamp_d(static_cast<double>(c.half_angle), 0.0,
                                static_cast<double>(k_pi));
    const double R = std::max(static_cast<double>(c.reach), 0.0);
    if (c.contains(point)) {
        return 0.0;
    }
    const vec3 v = point - c.apex;
    const double d = static_cast<double>(length(v));
    if (d <= static_cast<double>(k_epsilon)) {
        return 0.0;
    }
    double best = d; // distance to the apex
    const double cosine =
        clamp_d(static_cast<double>(dot(v, u)) / d, -1.0, 1.0);
    const double beta = std::acos(cosine);
    // Spherical cap.
    if (beta <= half + 1e-9) {
        best = std::min(best, std::fabs(d - R));
    }
    // Lateral surface. Rotational symmetry makes the closest generator the one
    // in the plane of v and the axis.
    const vec3 v_perp = v - u * dot(v, u);
    if (static_cast<double>(length(v_perp)) <=
        static_cast<double>(k_epsilon)) {
        if (R > 0.0) {
            const double cz = R * std::cos(half);
            const double sz = R * std::sin(half);
            const double rim_dx = d - cz;
            best = std::min(best, std::sqrt(rim_dx * rim_dx + sz * sz));
        }
        return best;
    }
    const vec3 w = normalized(v_perp);
    const double delta = beta - half;
    const double t = d * std::cos(delta);
    if (t >= 0.0) {
        if (t <= R) {
            best = std::min(best, std::fabs(d * std::sin(delta)));
        } else if (R > 0.0) {
            const vec3 boundary =
                u * static_cast<real>(std::cos(half)) +
                w * static_cast<real>(std::sin(half));
            best = std::min(
                best,
                static_cast<double>(length(point - (c.apex + boundary *
                                                                static_cast<real>(R)))));
        }
    }
    return best;
}

bool obb3_sat(const obb3& a, const obb3& b) noexcept {
    const vec3 axes_a[3] = {a.axis_x, a.axis_y, a.axis_z};
    const vec3 axes_b[3] = {b.axis_x, b.axis_y, b.axis_z};
    const double ha[3] = {
        std::max(static_cast<double>(a.half_extents.x), 0.0),
        std::max(static_cast<double>(a.half_extents.y), 0.0),
        std::max(static_cast<double>(a.half_extents.z), 0.0)};
    const double hb[3] = {
        std::max(static_cast<double>(b.half_extents.x), 0.0),
        std::max(static_cast<double>(b.half_extents.y), 0.0),
        std::max(static_cast<double>(b.half_extents.z), 0.0)};
    const vec3 delta = b.center - a.center;

    const auto separated = [&](vec3 axis) noexcept {
        if (length_squared(axis) <= 1e-12f) {
            return false;
        }
        const vec3 n = normalized(axis);
        double ra = 0.0;
        double rb = 0.0;
        for (int i = 0; i < 3; ++i) {
            ra += ha[i] *
                  std::fabs(static_cast<double>(dot(axes_a[i], n)));
            rb += hb[i] *
                  std::fabs(static_cast<double>(dot(axes_b[i], n)));
        }
        const double distance_along =
            std::fabs(static_cast<double>(dot(delta, n)));
        return distance_along > ra + rb + static_cast<double>(k_epsilon);
    };

    // 3 + 3 face normals.
    for (int i = 0; i < 3; ++i) {
        if (separated(axes_a[i]) || separated(axes_b[i])) {
            return false;
        }
    }
    // 9 pairwise edge cross products.
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            if (separated(cross(axes_a[i], axes_b[j]))) {
                return false;
            }
        }
    }
    return true;
}

bool line_slab3(vec3 origin, vec3 direction, vec3 half, double& tmin,
                double& tmax) noexcept {
    const double o[3] = {static_cast<double>(origin.x),
                         static_cast<double>(origin.y),
                         static_cast<double>(origin.z)};
    const double d[3] = {static_cast<double>(direction.x),
                         static_cast<double>(direction.y),
                         static_cast<double>(direction.z)};
    const double h[3] = {std::max(static_cast<double>(half.x), 0.0),
                         std::max(static_cast<double>(half.y), 0.0),
                         std::max(static_cast<double>(half.z), 0.0)};
    tmin = -std::numeric_limits<double>::infinity();
    tmax = std::numeric_limits<double>::infinity();
    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(d[axis]) <= 1e-12) {
            if (o[axis] < -h[axis] - static_cast<double>(k_epsilon) ||
                o[axis] > h[axis] + static_cast<double>(k_epsilon)) {
                return false;
            }
            continue;
        }
        double t1 = (-h[axis] - o[axis]) / d[axis];
        double t2 = (h[axis] - o[axis]) / d[axis];
        if (t1 > t2) {
            std::swap(t1, t2);
        }
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
    }
    return tmax >= tmin - 1e-9;
}

result<real> ray_aabb3_local(vec3 origin, vec3 direction, real limit,
                             vec3 half) {
    const double o[3] = {static_cast<double>(origin.x),
                         static_cast<double>(origin.y),
                         static_cast<double>(origin.z)};
    const double d[3] = {static_cast<double>(direction.x),
                         static_cast<double>(direction.y),
                         static_cast<double>(direction.z)};
    const double h[3] = {std::max(static_cast<double>(half.x), 0.0),
                         std::max(static_cast<double>(half.y), 0.0),
                         std::max(static_cast<double>(half.z), 0.0)};
    if (o[0] >= -h[0] && o[0] <= h[0] && o[1] >= -h[1] && o[1] <= h[1] &&
        o[2] >= -h[2] && o[2] <= h[2]) {
        return real{0.0f};
    }
    double tmin = -std::numeric_limits<double>::infinity();
    double tmax = std::numeric_limits<double>::infinity();
    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(d[axis]) <= 1e-12) {
            if (o[axis] < -h[axis] - static_cast<double>(k_epsilon) ||
                o[axis] > h[axis] + static_cast<double>(k_epsilon)) {
                return miss("ray misses the box slab");
            }
            continue;
        }
        double t1 = (-h[axis] - o[axis]) / d[axis];
        double t2 = (h[axis] - o[axis]) / d[axis];
        if (t1 > t2) {
            std::swap(t1, t2);
        }
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
    }
    if (tmax < tmin - 1e-9 || tmax < -static_cast<double>(k_epsilon)) {
        return miss("ray misses the box");
    }
    const double t = tmin >= 0.0 ? tmin : 0.0;
    if (t > static_cast<double>(limit) + static_cast<double>(k_epsilon)) {
        return miss("box is beyond the ray length");
    }
    return static_cast<real>(t);
}

result<real> ray_sphere_impl(vec3 origin, vec3 direction, real limit,
                             const sphere& s) {
    const vec3 u = normalized(direction);
    if (length_squared(u) <= 0.0f) {
        return invalid("ray3_sphere_hit: zero ray direction");
    }
    if (s.radius <= 0.0f) {
        const vec3 d = s.center - origin;
        const real t = dot(d, u);
        const vec3 perpendicular = d - u * t;
        if (length(perpendicular) > k_epsilon || t < -k_epsilon ||
            t > limit + k_epsilon) {
            return miss("ray misses the zero-radius sphere");
        }
        return t > 0.0f ? t : real{0.0f};
    }
    const vec3 oc = origin - s.center;
    const double b = static_cast<double>(dot(oc, u));
    const double c0 = static_cast<double>(dot(oc, oc)) -
                      static_cast<double>(s.radius) *
                          static_cast<double>(s.radius);
    if (c0 <= 0.0) {
        return real{0.0f};
    }
    const double disc = b * b - c0;
    if (disc < -static_cast<double>(k_epsilon)) {
        return miss("ray misses the sphere");
    }
    const double root = std::sqrt(std::max(0.0, disc));
    const auto accept = [&](double t) -> result<real> {
        if (t < -static_cast<double>(k_epsilon) ||
            t > static_cast<double>(limit) +
                    static_cast<double>(k_epsilon)) {
            return miss("sphere intersection is outside the ray");
        }
        return static_cast<real>(std::max(0.0, t));
    };
    const auto first = accept(-b - root);
    if (first.ok()) {
        return first;
    }
    return accept(-b + root);
}

} // namespace

// --- Region / region ---------------------------------------------------------

bool intersects(const sphere& a, const sphere& b) noexcept {
    return sphere_sphere(a, b);
}

bool intersects(const aabb3& a, const aabb3& b) noexcept {
    return a.overlaps(b);
}

bool intersects(const obb3& a, const obb3& b) noexcept { return obb3_sat(a, b); }

bool intersects(const sphere& a, const obb3& b) noexcept {
    const double radius = std::max(static_cast<double>(a.radius), 0.0);
    return distance_point_obb3(a.center, b) <=
           radius + static_cast<double>(k_epsilon);
}

bool intersects(const sphere& a, const aabb3& b) noexcept {
    if (!b.valid()) {
        return false;
    }
    const double radius = std::max(static_cast<double>(a.radius), 0.0);
    return distance_point_aabb3(a.center, b) <=
           radius + static_cast<double>(k_epsilon);
}

bool intersects(const sphere& a, const cone& b) noexcept {
    const double radius = std::max(static_cast<double>(a.radius), 0.0) +
                          static_cast<double>(k_epsilon);
    return distance_point_cone(a.center, b) <= radius;
}

bool intersects(const sphere& a, const umbrella3& b) noexcept {
    if (intersects(a, b.canopy())) {
        return true;
    }
    if (b.handle_length > 0.0f || b.handle_half_width > 0.0f) {
        return intersects(a, b.handle());
    }
    return false;
}

// --- Ray hits ----------------------------------------------------------------

result<real> ray3_sphere_hit(const ray3& r, const sphere& s) {
    if (!valid_direction(r.direction)) {
        return invalid("ray3_sphere_hit: zero ray direction");
    }
    if (!detail::finite_real(r.max_length) || r.max_length < 0.0f) {
        return invalid("ray3_sphere_hit: invalid max_length");
    }
    return ray_sphere_impl(r.origin, r.direction, ray_limit(r), s);
}

result<real> ray3_aabb_hit(const ray3& r, const aabb3& box) {
    if (!valid_direction(r.direction)) {
        return invalid("ray3_aabb_hit: zero ray direction");
    }
    if (!detail::finite_real(r.max_length) || r.max_length < 0.0f) {
        return invalid("ray3_aabb_hit: invalid max_length");
    }
    if (!box.valid() || !box.finite()) {
        return miss("ray3_aabb_hit: empty or non-finite box");
    }
    const vec3 u = normalized(r.direction);
    if (box.contains(r.origin)) {
        return real{0.0f};
    }
    const vec3 half = box.extents();
    const vec3 center = box.center();
    const vec3 rel = r.origin - center;
    double tmin = 0.0;
    double tmax = 0.0;
    if (!line_slab3(rel, u, half, tmin, tmax)) {
        return miss("ray misses the AABB");
    }
    if (tmax < -static_cast<double>(k_epsilon)) {
        return miss("AABB is behind the ray");
    }
    const double t = tmin >= 0.0 ? tmin : 0.0;
    if (t > static_cast<double>(ray_limit(r)) +
                static_cast<double>(k_epsilon)) {
        return miss("AABB is beyond the ray length");
    }
    return static_cast<real>(t);
}

result<real> ray3_obb_hit(const ray3& r, const obb3& box) {
    if (!valid_direction(r.direction)) {
        return invalid("ray3_obb_hit: zero ray direction");
    }
    if (!detail::finite_real(r.max_length) || r.max_length < 0.0f) {
        return invalid("ray3_obb_hit: invalid max_length");
    }
    const vec3 u = normalized(r.direction);
    const vec3 rel = r.origin - box.center;
    const vec3 local_origin(dot(rel, box.axis_x), dot(rel, box.axis_y),
                            dot(rel, box.axis_z));
    const vec3 local_dir(dot(u, box.axis_x), dot(u, box.axis_y),
                         dot(u, box.axis_z));
    return ray_aabb3_local(local_origin, local_dir, ray_limit(r),
                           vec3(std::fabs(box.half_extents.x),
                                std::fabs(box.half_extents.y),
                                std::fabs(box.half_extents.z)));
}

result<real> ray3_plane_hit(const ray3& r, const plane3& p) {
    if (!valid_direction(r.direction)) {
        return invalid("ray3_plane_hit: zero ray direction");
    }
    if (!detail::finite_real(r.max_length) || r.max_length < 0.0f) {
        return invalid("ray3_plane_hit: invalid max_length");
    }
    const vec3 n = normalized(p.normal);
    if (length_squared(n) <= 0.0f) {
        return invalid("ray3_plane_hit: zero plane normal");
    }
    const vec3 u = normalized(r.direction);
    const double denom = static_cast<double>(dot(n, u));
    const double dist =
        static_cast<double>(dot(n, r.origin - p.point));
    if (std::fabs(denom) <= static_cast<double>(k_epsilon)) {
        if (std::fabs(dist) <= static_cast<double>(k_epsilon)) {
            return real{0.0f}; // the ray lies in the plane
        }
        return miss("ray is parallel to the plane");
    }
    const double t = -dist / denom;
    if (t < -static_cast<double>(k_epsilon) ||
        t > static_cast<double>(ray_limit(r)) +
                static_cast<double>(k_epsilon)) {
        return miss("plane intersection is outside the ray");
    }
    return static_cast<real>(std::max(0.0, t));
}

result<real> ray3_polyhedron_hit(const ray3& r,
                                 const convex_polyhedron& poly) {
    if (!valid_direction(r.direction)) {
        return invalid("ray3_polyhedron_hit: zero ray direction");
    }
    if (!detail::finite_real(r.max_length) || r.max_length < 0.0f) {
        return invalid("ray3_polyhedron_hit: invalid max_length");
    }
    if (!poly.valid()) {
        return miss("ray3_polyhedron_hit: empty polyhedron");
    }
    if (poly.contains(r.origin)) {
        return real{0.0f};
    }
    const vec3 u = normalized(r.direction);
    double tmin = -std::numeric_limits<double>::infinity();
    double tmax = std::numeric_limits<double>::infinity();
    for (const plane3& p : poly.planes) {
        const vec3 n = normalized(p.normal);
        if (length_squared(n) <= 0.0f) {
            continue;
        }
        const double denom = static_cast<double>(dot(n, u));
        const double dist =
            static_cast<double>(dot(n, r.origin - p.point));
        if (std::fabs(denom) <= static_cast<double>(k_epsilon)) {
            if (dist > static_cast<double>(k_epsilon)) {
                return miss("ray is outside a parallel face");
            }
            continue;
        }
        const double t = -dist / denom;
        if (denom < 0.0) {
            tmin = std::max(tmin, t);
        } else {
            tmax = std::min(tmax, t);
        }
    }
    if (tmax < tmin - 1e-9) {
        return miss("ray misses the polyhedron");
    }
    const double t = tmin >= 0.0 ? tmin : 0.0;
    if (t > static_cast<double>(ray_limit(r)) +
                static_cast<double>(k_epsilon)) {
        return miss("polyhedron is beyond the ray length");
    }
    return static_cast<real>(t);
}

bool intersects(const ray3& r, const sphere& s) noexcept {
    return ray3_sphere_hit(r, s).ok();
}

bool intersects(const ray3& r, const aabb3& box) noexcept {
    return ray3_aabb_hit(r, box).ok();
}

bool intersects(const ray3& r, const obb3& box) noexcept {
    return ray3_obb_hit(r, box).ok();
}

bool intersects(const ray3& r, const plane3& p) noexcept {
    return ray3_plane_hit(r, p).ok();
}

bool intersects(const ray3& r, const convex_polyhedron& poly) noexcept {
    return ray3_polyhedron_hit(r, poly).ok();
}

// --- Infinite line hits ------------------------------------------------------

result<real> line3_sphere_hit(const line3& l, const sphere& s) {
    if (!valid_direction(l.direction)) {
        return invalid("line3_sphere_hit: zero line direction");
    }
    const vec3 u = normalized(l.direction);
    if (s.radius <= 0.0f) {
        if (!l.contains(s.center)) {
            return miss("line misses the zero-radius sphere");
        }
        return dot(s.center - l.point, u);
    }
    const vec3 oc = l.point - s.center;
    const double b = static_cast<double>(dot(oc, u));
    const double c0 = static_cast<double>(dot(oc, oc)) -
                      static_cast<double>(s.radius) *
                          static_cast<double>(s.radius);
    const double disc = b * b - c0;
    if (disc < -static_cast<double>(k_epsilon)) {
        return miss("line misses the sphere");
    }
    const double root = std::sqrt(std::max(0.0, disc));
    const double t0 = -b - root;
    const double t1 = -b + root;
    return static_cast<real>(std::fabs(t0) <= std::fabs(t1) ? t0 : t1);
}

result<real> line3_aabb_hit(const line3& l, const aabb3& box) {
    if (!valid_direction(l.direction)) {
        return invalid("line3_aabb_hit: zero line direction");
    }
    if (!box.valid() || !box.finite()) {
        return miss("line3_aabb_hit: empty or non-finite box");
    }
    const vec3 u = normalized(l.direction);
    if (box.contains(l.point)) {
        return real{0.0f};
    }
    double tmin = 0.0;
    double tmax = 0.0;
    if (!line_slab3(l.point - box.center(), u, box.extents(), tmin, tmax)) {
        return miss("line misses the AABB");
    }
    if (tmin <= 0.0 && tmax >= 0.0) {
        return real{0.0f};
    }
    return static_cast<real>(std::fabs(tmin) <= std::fabs(tmax) ? tmin : tmax);
}

result<real> line3_obb_hit(const line3& l, const obb3& box) {
    if (!valid_direction(l.direction)) {
        return invalid("line3_obb_hit: zero line direction");
    }
    const vec3 u = normalized(l.direction);
    const vec3 rel = l.point - box.center;
    const vec3 local_point(dot(rel, box.axis_x), dot(rel, box.axis_y),
                           dot(rel, box.axis_z));
    const vec3 local_dir(dot(u, box.axis_x), dot(u, box.axis_y),
                         dot(u, box.axis_z));
    const vec3 half(std::fabs(box.half_extents.x),
                    std::fabs(box.half_extents.y),
                    std::fabs(box.half_extents.z));
    if (std::fabs(local_point.x) <= half.x + k_epsilon &&
        std::fabs(local_point.y) <= half.y + k_epsilon &&
        std::fabs(local_point.z) <= half.z + k_epsilon) {
        return real{0.0f};
    }
    double tmin = 0.0;
    double tmax = 0.0;
    if (!line_slab3(local_point, local_dir, half, tmin, tmax)) {
        return miss("line misses the OBB");
    }
    if (tmin <= 0.0 && tmax >= 0.0) {
        return real{0.0f};
    }
    return static_cast<real>(std::fabs(tmin) <= std::fabs(tmax) ? tmin : tmax);
}

result<real> line3_plane_hit(const line3& l, const plane3& p) {
    if (!valid_direction(l.direction)) {
        return invalid("line3_plane_hit: zero line direction");
    }
    const vec3 n = normalized(p.normal);
    if (length_squared(n) <= 0.0f) {
        return invalid("line3_plane_hit: zero plane normal");
    }
    const vec3 u = normalized(l.direction);
    const double denom = static_cast<double>(dot(n, u));
    const double dist = static_cast<double>(dot(n, l.point - p.point));
    if (std::fabs(denom) <= static_cast<double>(k_epsilon)) {
        if (std::fabs(dist) <= static_cast<double>(k_epsilon)) {
            return real{0.0f};
        }
        return miss("line is parallel to the plane");
    }
    return static_cast<real>(-dist / denom);
}

result<real> line3_polyhedron_hit(const line3& l,
                                  const convex_polyhedron& poly) {
    if (!valid_direction(l.direction)) {
        return invalid("line3_polyhedron_hit: zero line direction");
    }
    if (!poly.valid()) {
        return miss("line3_polyhedron_hit: empty polyhedron");
    }
    if (poly.contains(l.point)) {
        return real{0.0f};
    }
    const vec3 u = normalized(l.direction);
    double tmin = -std::numeric_limits<double>::infinity();
    double tmax = std::numeric_limits<double>::infinity();
    for (const plane3& p : poly.planes) {
        const vec3 n = normalized(p.normal);
        if (length_squared(n) <= 0.0f) {
            continue;
        }
        const double denom = static_cast<double>(dot(n, u));
        const double dist = static_cast<double>(dot(n, l.point - p.point));
        if (std::fabs(denom) <= static_cast<double>(k_epsilon)) {
            if (dist > static_cast<double>(k_epsilon)) {
                return miss("line is outside a parallel face");
            }
            continue;
        }
        const double t = -dist / denom;
        if (denom < 0.0) {
            tmin = std::max(tmin, t);
        } else {
            tmax = std::min(tmax, t);
        }
    }
    if (tmax < tmin - 1e-9) {
        return miss("line misses the polyhedron");
    }
    if (tmin <= 0.0 && tmax >= 0.0) {
        return real{0.0f};
    }
    return static_cast<real>(std::fabs(tmin) <= std::fabs(tmax) ? tmin : tmax);
}

// --- Segment hits ------------------------------------------------------------

namespace {

result<real> segment_fraction3(result<real> ray_result, real length) {
    if (!ray_result.ok()) {
        return ray_result;
    }
    if (length <= k_epsilon) {
        return real{0.0f};
    }
    const real fraction = ray_result.value() / length;
    return fraction < 0.0f ? real{0.0f}
                          : (fraction > 1.0f ? real{1.0f} : fraction);
}

} // namespace

result<real> segment3_sphere_hit(const segment3& s, const sphere& sphere_value) {
    const vec3 d = s.b - s.a;
    const real len = length(d);
    if (len <= k_epsilon) {
        return sphere_value.contains(s.a)
                   ? result<real>{real{0.0f}}
                   : result<real>{miss("segment point misses")};
    }
    const ray3 r{s.a, d, len};
    return segment_fraction3(ray3_sphere_hit(r, sphere_value), len);
}

result<real> segment3_aabb_hit(const segment3& s, const aabb3& box) {
    const vec3 d = s.b - s.a;
    const real len = length(d);
    if (len <= k_epsilon) {
        return box.contains(s.a) ? result<real>{real{0.0f}}
                                 : result<real>{miss("segment point misses")};
    }
    return segment_fraction3(ray3_aabb_hit(ray3{s.a, d, len}, box), len);
}

result<real> segment3_obb_hit(const segment3& s, const obb3& box) {
    const vec3 d = s.b - s.a;
    const real len = length(d);
    if (len <= k_epsilon) {
        return box.contains(s.a) ? result<real>{real{0.0f}}
                                 : result<real>{miss("segment point misses")};
    }
    return segment_fraction3(ray3_obb_hit(ray3{s.a, d, len}, box), len);
}

result<real> segment3_plane_hit(const segment3& s, const plane3& p) {
    const vec3 d = s.b - s.a;
    const real len = length(d);
    if (len <= k_epsilon) {
        return p.contains(s.a) ? result<real>{real{0.0f}}
                               : result<real>{miss("segment point misses")};
    }
    return segment_fraction3(ray3_plane_hit(ray3{s.a, d, len}, p), len);
}

result<real> segment3_polyhedron_hit(const segment3& s,
                                     const convex_polyhedron& poly) {
    const vec3 d = s.b - s.a;
    const real len = length(d);
    if (len <= k_epsilon) {
        return poly.contains(s.a) ? result<real>{real{0.0f}}
                                  : result<real>{miss("segment point misses")};
    }
    return segment_fraction3(ray3_polyhedron_hit(ray3{s.a, d, len}, poly), len);
}

} // namespace geom
} // namespace game
} // namespace crpc