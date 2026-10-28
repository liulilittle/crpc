// CRPC -- 2D intersection and ray queries. See collide2d.hpp.
#include <crpc/game/geom/collide2d.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

#include "geometry_detail.hpp"

namespace crpc {
namespace game {
namespace geom {

namespace {

using detail::finite_real;

rpc_error miss(const char* what) {
    return rpc_error{rpc_code::not_found, what};
}

rpc_error invalid(const char* what) {
    return rpc_error{rpc_code::invalid_argument, what};
}

bool valid_direction(vec2 direction) noexcept {
    return length_squared(direction) > k_epsilon * k_epsilon;
}

real ray_limit(const ray& r) noexcept {
    return r.max_length > 0.0f ? r.max_length : 0.0f;
}

double clamp_d(double v, double lo, double hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

bool circle_circle(const circle& a, const circle& b) noexcept {
    const double ra = std::max(static_cast<double>(a.radius), 0.0);
    const double rb = std::max(static_cast<double>(b.radius), 0.0);
    const double dx = static_cast<double>(b.center.x - a.center.x);
    const double dy = static_cast<double>(b.center.y - a.center.y);
    const double reach = ra + rb + static_cast<double>(k_epsilon);
    return dx * dx + dy * dy <= reach * reach;
}

double distance_point_obb(vec2 point, const obb& box) noexcept {
    const vec2 ux = box.axis_x();
    const vec2 uy = box.axis_y();
    const vec2 d = point - box.center;
    const double hx = std::max(static_cast<double>(box.half_extents.x), 0.0);
    const double hy = std::max(static_cast<double>(box.half_extents.y), 0.0);
    const double lx = clamp_d(static_cast<double>(dot(d, ux)), -hx, hx);
    const double ly = clamp_d(static_cast<double>(dot(d, uy)), -hy, hy);
    const double dx = static_cast<double>(dot(d, ux)) - lx;
    const double dy = static_cast<double>(dot(d, uy)) - ly;
    return std::sqrt(dx * dx + dy * dy);
}

double distance_point_aabb(vec2 point, const aabb& box) noexcept {
    const double cx =
        clamp_d(static_cast<double>(point.x), static_cast<double>(box.min.x),
                static_cast<double>(box.max.x));
    const double cy =
        clamp_d(static_cast<double>(point.y), static_cast<double>(box.min.y),
                static_cast<double>(box.max.y));
    const double dx = static_cast<double>(point.x) - cx;
    const double dy = static_cast<double>(point.y) - cy;
    return std::sqrt(dx * dx + dy * dy);
}

bool polygon_polygon_convex_sat(const polygon& a, const polygon& b) noexcept {
    const auto separated = [](const polygon& p, const polygon& q,
                              vec2 axis) noexcept {
        double min_p = std::numeric_limits<double>::infinity();
        double max_p = -std::numeric_limits<double>::infinity();
        double min_q = std::numeric_limits<double>::infinity();
        double max_q = -std::numeric_limits<double>::infinity();
        for (const vec2 v : p.vertices) {
            const double x = static_cast<double>(dot(v, axis));
            min_p = std::min(min_p, x);
            max_p = std::max(max_p, x);
        }
        for (const vec2 v : q.vertices) {
            const double x = static_cast<double>(dot(v, axis));
            min_q = std::min(min_q, x);
            max_q = std::max(max_q, x);
        }
        return max_p < min_q - static_cast<double>(k_epsilon) ||
               max_q < min_p - static_cast<double>(k_epsilon);
    };
    const auto test_edges = [&](const polygon& p) noexcept {
        const std::size_t n = p.vertices.size();
        for (std::size_t i = 0; i < n; ++i) {
            const vec2 edge = p.vertices[(i + 1) % n] - p.vertices[i];
            if (length_squared(edge) <= k_epsilon * k_epsilon) {
                continue;
            }
            const vec2 axis = normalized(vec2(-edge.y, edge.x));
            if (separated(a, b, axis)) {
                return true;
            }
        }
        return false;
    };
    return !test_edges(a) && !test_edges(b);
}

bool polygon_polygon_boundary(const polygon& a, const polygon& b) noexcept {
    const std::size_t na = a.vertices.size();
    const std::size_t nb = b.vertices.size();
    for (std::size_t i = 0; i < na; ++i) {
        for (std::size_t j = 0; j < nb; ++j) {
            if (detail::segments_intersect(a.vertices[i],
                                           a.vertices[(i + 1) % na],
                                           b.vertices[j],
                                           b.vertices[(j + 1) % nb],
                                           k_epsilon)) {
                return true;
            }
        }
    }
    if (na > 0 && b.contains(a.vertices.front())) {
        return true;
    }
    if (nb > 0 && a.contains(b.vertices.front())) {
        return true;
    }
    return false;
}

result<real> ray_aabb_local(vec2 origin, vec2 direction, real limit,
                            vec2 half) {
    const double ox = static_cast<double>(origin.x);
    const double oy = static_cast<double>(origin.y);
    const double dx = static_cast<double>(direction.x);
    const double dy = static_cast<double>(direction.y);
    const double hx = std::max(static_cast<double>(half.x), 0.0);
    const double hy = std::max(static_cast<double>(half.y), 0.0);
    if (ox >= -hx && ox <= hx && oy >= -hy && oy <= hy) {
        return real{0.0f};
    }
    double tmin = -std::numeric_limits<double>::infinity();
    double tmax = std::numeric_limits<double>::infinity();
    const double origins[2] = {ox, oy};
    const double dirs[2] = {dx, dy};
    const double halves[2] = {hx, hy};
    for (int axis = 0; axis < 2; ++axis) {
        const double o = origins[axis];
        const double d = dirs[axis];
        const double h = halves[axis];
        if (std::fabs(d) <= 1e-12) {
            if (o < -h - static_cast<double>(k_epsilon) ||
                o > h + static_cast<double>(k_epsilon)) {
                return miss("ray does not intersect the box slab");
            }
            continue;
        }
        double t1 = (-h - o) / d;
        double t2 = (h - o) / d;
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

// Slab interval for an infinite line. Returns [entry, exit]; when the
// reference point is inside the interval both signs straddle zero.
bool line_slab(vec2 origin, vec2 direction, vec2 half, double& tmin,
               double& tmax) noexcept {
    const double ox = static_cast<double>(origin.x);
    const double oy = static_cast<double>(origin.y);
    const double dx = static_cast<double>(direction.x);
    const double dy = static_cast<double>(direction.y);
    const double hx = std::max(static_cast<double>(half.x), 0.0);
    const double hy = std::max(static_cast<double>(half.y), 0.0);
    tmin = -std::numeric_limits<double>::infinity();
    tmax = std::numeric_limits<double>::infinity();
    const double origins[2] = {ox, oy};
    const double dirs[2] = {dx, dy};
    const double halves[2] = {hx, hy};
    for (int axis = 0; axis < 2; ++axis) {
        const double o = origins[axis];
        const double d = dirs[axis];
        const double h = halves[axis];
        if (std::fabs(d) <= 1e-12) {
            if (o < -h - static_cast<double>(k_epsilon) ||
                o > h + static_cast<double>(k_epsilon)) {
                return false;
            }
            continue;
        }
        double t1 = (-h - o) / d;
        double t2 = (h - o) / d;
        if (t1 > t2) {
            std::swap(t1, t2);
        }
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
    }
    return tmax >= tmin - 1e-9;
}

} // namespace

// --- Region / region ---------------------------------------------------------

bool intersects(const circle& a, const circle& b) noexcept {
    return circle_circle(a, b);
}

bool intersects(const aabb& a, const aabb& b) noexcept {
    return a.overlaps(b);
}

bool intersects(const obb& a, const obb& b) noexcept {
    const vec2 axes[4] = {a.axis_x(), a.axis_y(), b.axis_x(), b.axis_y()};
    const vec2 d = b.center - a.center;
    const double hax =
        std::max(static_cast<double>(a.half_extents.x), 0.0);
    const double hay =
        std::max(static_cast<double>(a.half_extents.y), 0.0);
    const double hbx =
        std::max(static_cast<double>(b.half_extents.x), 0.0);
    const double hby =
        std::max(static_cast<double>(b.half_extents.y), 0.0);
    for (const vec2 axis : axes) {
        const double distance_along =
            std::fabs(static_cast<double>(dot(d, axis)));
        const double ra =
            hax * std::fabs(static_cast<double>(dot(a.axis_x(), axis))) +
            hay * std::fabs(static_cast<double>(dot(a.axis_y(), axis)));
        const double rb =
            hbx * std::fabs(static_cast<double>(dot(b.axis_x(), axis))) +
            hby * std::fabs(static_cast<double>(dot(b.axis_y(), axis)));
        if (distance_along > ra + rb + static_cast<double>(k_epsilon)) {
            return false;
        }
    }
    return true;
}

bool intersects(const circle& a, const obb& b) noexcept {
    const double radius = std::max(static_cast<double>(a.radius), 0.0);
    return distance_point_obb(a.center, b) <=
           radius + static_cast<double>(k_epsilon);
}

bool intersects(const circle& a, const aabb& b) noexcept {
    if (!b.valid()) {
        return false;
    }
    const double radius = std::max(static_cast<double>(a.radius), 0.0);
    return distance_point_aabb(a.center, b) <=
           radius + static_cast<double>(k_epsilon);
}

bool intersects(const circle& a, const polygon& b) noexcept {
    if (b.vertices.empty()) {
        return false;
    }
    if (b.contains(a.center)) {
        return true;
    }
    const double radius = std::max(static_cast<double>(a.radius), 0.0) +
                          static_cast<double>(k_epsilon);
    const std::size_t n = b.vertices.size();
    if (n == 1) {
        return detail::point_segment_distance(a.center, b.vertices[0],
                                              b.vertices[0]) <= radius;
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (detail::point_segment_distance(a.center, b.vertices[i],
                                           b.vertices[(i + 1) % n]) <= radius) {
            return true;
        }
    }
    return false;
}

bool intersects(const polygon& a, const polygon& b) noexcept {
    if (a.vertices.empty() || b.vertices.empty()) {
        return false;
    }
    if (a.vertices.size() < 3 || b.vertices.size() < 3) {
        // Degenerate polygon: its boundary is the whole shape, so the generic
        // edge/vertex test is still exact (a segment can cross a polygon with
        // both endpoints outside it).
        return polygon_polygon_boundary(a, b);
    }
    if (a.is_convex() && b.is_convex()) {
        return polygon_polygon_convex_sat(a, b);
    }
    return polygon_polygon_boundary(a, b);
}

bool intersects(const sector& a, const circle& b) noexcept {
    if (a.sweep <= 0.0f) {
        return false;
    }
    if (a.radius <= 0.0f) {
        return b.contains(a.center);
    }
    if (b.contains(a.center)) {
        return true;
    }
    if (a.contains(b.center)) {
        return true;
    }
    if (a.full()) {
        return circle_circle(circle{a.center, a.radius}, b);
    }
    const double radius = std::max(static_cast<double>(b.radius), 0.0) +
                          static_cast<double>(k_epsilon);
    const double start =
        static_cast<double>(normalize_angle(a.start_angle));
    const double half = static_cast<double>(a.sweep) * 0.5;
    const double bisector = start + half;
    const vec2 d0(static_cast<real>(std::cos(start)),
                  static_cast<real>(std::sin(start)));
    const vec2 d1(static_cast<real>(std::cos(bisector + half)),
                  static_cast<real>(std::sin(bisector + half)));
    const vec2 edge0 = a.center + d0 * a.radius;
    const vec2 edge1 = a.center + d1 * a.radius;
    if (detail::point_segment_distance(b.center, a.center, edge0) <= radius ||
        detail::point_segment_distance(b.center, a.center, edge1) <= radius) {
        return true;
    }
    const double dx = static_cast<double>(b.center.x - a.center.x);
    const double dy = static_cast<double>(b.center.y - a.center.y);
    const double dist = std::sqrt(dx * dx + dy * dy);
    if (dist <= static_cast<double>(k_epsilon)) {
        return true;
    }
    const double angle = std::atan2(dy, dx);
    const double delta = static_cast<double>(
        normalize_angle(static_cast<real>(angle - start)));
    if (delta <= static_cast<double>(a.sweep) +
                     static_cast<double>(k_epsilon) /
                         std::max(dist, 1.0)) {
        return std::fabs(dist - static_cast<double>(a.radius)) <= radius;
    }
    return false;
}

bool intersects(const umbrella& a, const circle& b) noexcept {
    if (intersects(a.canopy(), b)) {
        return true;
    }
    if (a.handle_length > 0.0f || a.handle_half_width > 0.0f) {
        return intersects(b, a.handle());
    }
    return false;
}

// --- Ray hits ----------------------------------------------------------------

result<real> ray_circle_hit(const ray& r, const circle& c) {
    if (!valid_direction(r.direction)) {
        return invalid("ray_circle_hit: zero ray direction");
    }
    if (!finite_real(r.max_length) || r.max_length < 0.0f) {
        return invalid("ray_circle_hit: invalid max_length");
    }
    const vec2 u = normalized(r.direction);
    const real limit = ray_limit(r);
    if (c.radius <= 0.0f) {
        if (!r.contains(c.center)) {
            return miss("ray misses the zero-radius circle");
        }
        const real t = dot(c.center - r.origin, u);
        if (t < -k_epsilon || t > limit + k_epsilon) {
            return miss("zero-radius circle is beyond the ray");
        }
        return t > 0.0f ? t : real{0.0f};
    }
    const vec2 oc = r.origin - c.center;
    const double b = static_cast<double>(dot(oc, u));
    const double c0 = static_cast<double>(dot(oc, oc)) -
                      static_cast<double>(c.radius) *
                          static_cast<double>(c.radius);
    if (c0 <= 0.0) {
        return real{0.0f};
    }
    const double disc = b * b - c0;
    if (disc < -static_cast<double>(k_epsilon)) {
        return miss("ray misses the circle");
    }
    const double root = std::sqrt(std::max(0.0, disc));
    const double t0 = -b - root;
    const double t1 = -b + root;
    const auto accept = [&](double t) -> result<real> {
        if (t < -static_cast<double>(k_epsilon) ||
            t > static_cast<double>(limit) +
                    static_cast<double>(k_epsilon)) {
            return miss("circle intersection is outside the ray");
        }
        return static_cast<real>(std::max(0.0, t));
    };
    const auto first = accept(t0);
    if (first.ok()) {
        return first;
    }
    return accept(t1);
}

result<real> ray_aabb_hit(const ray& r, const aabb& box) {
    if (!valid_direction(r.direction)) {
        return invalid("ray_aabb_hit: zero ray direction");
    }
    if (!finite_real(r.max_length) || r.max_length < 0.0f) {
        return invalid("ray_aabb_hit: invalid max_length");
    }
    if (!box.valid() || !box.finite()) {
        return miss("ray_aabb_hit: empty or non-finite box");
    }
    const vec2 u = normalized(r.direction);
    if (box.contains(r.origin)) {
        return real{0.0f};
    }
    const double ox = static_cast<double>(r.origin.x);
    const double oy = static_cast<double>(r.origin.y);
    const double dx = static_cast<double>(u.x);
    const double dy = static_cast<double>(u.y);
    double tmin = -std::numeric_limits<double>::infinity();
    double tmax = std::numeric_limits<double>::infinity();
    const double origins[2] = {ox, oy};
    const double dirs[2] = {dx, dy};
    const double mins[2] = {static_cast<double>(box.min.x),
                            static_cast<double>(box.min.y)};
    const double maxs[2] = {static_cast<double>(box.max.x),
                            static_cast<double>(box.max.y)};
    for (int axis = 0; axis < 2; ++axis) {
        const double o = origins[axis];
        const double d = dirs[axis];
        if (std::fabs(d) <= 1e-12) {
            if (o < mins[axis] - static_cast<double>(k_epsilon) ||
                o > maxs[axis] + static_cast<double>(k_epsilon)) {
                return miss("ray misses the AABB slab");
            }
            continue;
        }
        double t1 = (mins[axis] - o) / d;
        double t2 = (maxs[axis] - o) / d;
        if (t1 > t2) {
            std::swap(t1, t2);
        }
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
    }
    if (tmax < tmin - 1e-9 || tmax < -static_cast<double>(k_epsilon)) {
        return miss("ray misses the AABB");
    }
    const double t = tmin >= 0.0 ? tmin : 0.0;
    if (t > static_cast<double>(ray_limit(r)) +
                static_cast<double>(k_epsilon)) {
        return miss("AABB is beyond the ray length");
    }
    return static_cast<real>(t);
}

result<real> ray_obb_hit(const ray& r, const obb& box) {
    if (!valid_direction(r.direction)) {
        return invalid("ray_obb_hit: zero ray direction");
    }
    if (!finite_real(r.max_length) || r.max_length < 0.0f) {
        return invalid("ray_obb_hit: invalid max_length");
    }
    const vec2 u = normalized(r.direction);
    const vec2 ux = box.axis_x();
    const vec2 uy = box.axis_y();
    const vec2 rel = r.origin - box.center;
    const vec2 local_origin(dot(rel, ux), dot(rel, uy));
    const vec2 local_dir(dot(u, ux), dot(u, uy));
    return ray_aabb_local(local_origin, local_dir, ray_limit(r),
                          vec2(std::fabs(box.half_extents.x),
                               std::fabs(box.half_extents.y)));
}

result<real> ray_polygon_hit(const ray& r, const polygon& poly) {
    if (!valid_direction(r.direction)) {
        return invalid("ray_polygon_hit: zero ray direction");
    }
    if (!finite_real(r.max_length) || r.max_length < 0.0f) {
        return invalid("ray_polygon_hit: invalid max_length");
    }
    const std::size_t n = poly.vertices.size();
    if (n == 0) {
        return miss("ray_polygon_hit: empty polygon");
    }
    if (poly.contains(r.origin)) {
        return real{0.0f};
    }
    const vec2 u = normalized(r.direction);
    const double limit = static_cast<double>(ray_limit(r));
    const double eps = static_cast<double>(k_epsilon);
    double best = std::numeric_limits<double>::infinity();
    bool found = false;
    for (std::size_t i = 0; i < n; ++i) {
        const vec2 a = poly.vertices[i];
        const vec2 b = poly.vertices[(i + 1) % n];
        const vec2 edge = b - a;
        const double denom =
            static_cast<double>(cross(u, edge));
        if (std::fabs(denom) <= 1e-12) {
            continue;
        }
        const double t =
            static_cast<double>(cross(a - r.origin, edge)) / denom;
        const double s =
            static_cast<double>(cross(a - r.origin, u)) / denom;
        if (t >= -eps && t <= limit + eps && s >= -1e-6 && s <= 1.0 + 1e-6) {
            if (!found || t < best) {
                found = true;
                best = std::max(0.0, t);
            }
        }
    }
    if (!found) {
        return miss("ray misses the polygon");
    }
    return static_cast<real>(best);
}

bool intersects(const ray& r, const circle& c) noexcept {
    return ray_circle_hit(r, c).ok();
}

bool intersects(const ray& r, const aabb& box) noexcept {
    return ray_aabb_hit(r, box).ok();
}

bool intersects(const ray& r, const obb& box) noexcept {
    return ray_obb_hit(r, box).ok();
}

bool intersects(const ray& r, const polygon& poly) noexcept {
    return ray_polygon_hit(r, poly).ok();
}

// --- Infinite line hits ------------------------------------------------------

namespace {

result<real> line_circle_impl(const line& l, const circle& c) {
    if (!valid_direction(l.direction)) {
        return invalid("line_circle_hit: zero line direction");
    }
    const vec2 u = normalized(l.direction);
    if (c.radius <= 0.0f) {
        if (!l.contains(c.center)) {
            return miss("line misses the zero-radius circle");
        }
        return dot(c.center - l.point, u);
    }
    const vec2 oc = l.point - c.center;
    const double b = static_cast<double>(dot(oc, u));
    const double c0 = static_cast<double>(dot(oc, oc)) -
                      static_cast<double>(c.radius) *
                          static_cast<double>(c.radius);
    const double disc = b * b - c0;
    if (disc < -static_cast<double>(k_epsilon)) {
        return miss("line misses the circle");
    }
    const double root = std::sqrt(std::max(0.0, disc));
    const double t0 = -b - root;
    const double t1 = -b + root;
    return static_cast<real>(std::fabs(t0) <= std::fabs(t1) ? t0 : t1);
}

result<real> line_aabb_impl(const line& l, const aabb& box) {
    if (!valid_direction(l.direction)) {
        return invalid("line_aabb_hit: zero line direction");
    }
    if (!box.valid() || !box.finite()) {
        return miss("line_aabb_hit: empty or non-finite box");
    }
    const vec2 u = normalized(l.direction);
    if (box.contains(l.point)) {
        return real{0.0f};
    }
    const vec2 half((box.max.x - box.min.x) * 0.5f,
                    (box.max.y - box.min.y) * 0.5f);
    const vec2 center = box.center();
    double tmin = 0.0;
    double tmax = 0.0;
    if (!line_slab(l.point - center, u, half, tmin, tmax)) {
        return miss("line misses the AABB");
    }
    if (tmin <= 0.0 && tmax >= 0.0) {
        return real{0.0f};
    }
    return static_cast<real>(std::fabs(tmin) <= std::fabs(tmax) ? tmin : tmax);
}

result<real> line_obb_impl(const line& l, const obb& box) {
    if (!valid_direction(l.direction)) {
        return invalid("line_obb_hit: zero line direction");
    }
    const vec2 u = normalized(l.direction);
    const vec2 ux = box.axis_x();
    const vec2 uy = box.axis_y();
    const vec2 rel = l.point - box.center;
    const vec2 local_point(dot(rel, ux), dot(rel, uy));
    const vec2 local_dir(dot(u, ux), dot(u, uy));
    const vec2 half(std::fabs(box.half_extents.x),
                    std::fabs(box.half_extents.y));
    if (std::fabs(local_point.x) <= half.x + k_epsilon &&
        std::fabs(local_point.y) <= half.y + k_epsilon) {
        return real{0.0f};
    }
    double tmin = 0.0;
    double tmax = 0.0;
    if (!line_slab(local_point, local_dir, half, tmin, tmax)) {
        return miss("line misses the OBB");
    }
    if (tmin <= 0.0 && tmax >= 0.0) {
        return real{0.0f};
    }
    return static_cast<real>(std::fabs(tmin) <= std::fabs(tmax) ? tmin : tmax);
}

result<real> line_polygon_impl(const line& l, const polygon& poly) {
    if (!valid_direction(l.direction)) {
        return invalid("line_polygon_hit: zero line direction");
    }
    const std::size_t n = poly.vertices.size();
    if (n == 0) {
        return miss("line_polygon_hit: empty polygon");
    }
    if (poly.contains(l.point)) {
        return real{0.0f};
    }
    const vec2 u = normalized(l.direction);
    double best = std::numeric_limits<double>::infinity();
    bool found = false;
    for (std::size_t i = 0; i < n; ++i) {
        const vec2 a = poly.vertices[i];
        const vec2 b = poly.vertices[(i + 1) % n];
        const vec2 edge = b - a;
        const double denom = static_cast<double>(cross(u, edge));
        if (std::fabs(denom) <= 1e-12) {
            continue;
        }
        const double t =
            static_cast<double>(cross(a - l.point, edge)) / denom;
        const double s =
            static_cast<double>(cross(a - l.point, u)) / denom;
        if (s >= -1e-6 && s <= 1.0 + 1e-6) {
            if (!found || std::fabs(t) < std::fabs(best)) {
                found = true;
                best = t;
            }
        }
    }
    if (!found) {
        return miss("line misses the polygon");
    }
    return static_cast<real>(best);
}

} // namespace

result<real> line_circle_hit(const line& l, const circle& c) {
    return line_circle_impl(l, c);
}

result<real> line_aabb_hit(const line& l, const aabb& box) {
    return line_aabb_impl(l, box);
}

result<real> line_obb_hit(const line& l, const obb& box) {
    return line_obb_impl(l, box);
}

result<real> line_polygon_hit(const line& l, const polygon& poly) {
    return line_polygon_impl(l, poly);
}

bool intersects(const line& l, const circle& c) noexcept {
    return line_circle_hit(l, c).ok();
}

bool intersects(const line& l, const aabb& box) noexcept {
    return line_aabb_hit(l, box).ok();
}

bool intersects(const line& l, const obb& box) noexcept {
    return line_obb_hit(l, box).ok();
}

bool intersects(const line& l, const polygon& poly) noexcept {
    return line_polygon_hit(l, poly).ok();
}

// --- Segment hits ------------------------------------------------------------

namespace {

result<real> segment_fraction(result<real> ray_result, real length) {
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

result<real> segment_circle_impl(const segment& s, const circle& c) {
    const vec2 d = s.b - s.a;
    const real len = length(d);
    if (len <= k_epsilon) {
        return c.contains(s.a) ? result<real>{real{0.0f}}
                               : result<real>{miss("segment point misses")};
    }
    const ray r{s.a, d, len};
    return segment_fraction(ray_circle_hit(r, c), len);
}

result<real> segment_aabb_impl(const segment& s, const aabb& box) {
    const vec2 d = s.b - s.a;
    const real len = length(d);
    if (len <= k_epsilon) {
        return box.contains(s.a) ? result<real>{real{0.0f}}
                                 : result<real>{miss("segment point misses")};
    }
    return segment_fraction(ray_aabb_hit(ray{s.a, d, len}, box), len);
}

result<real> segment_obb_impl(const segment& s, const obb& box) {
    const vec2 d = s.b - s.a;
    const real len = length(d);
    if (len <= k_epsilon) {
        return box.contains(s.a) ? result<real>{real{0.0f}}
                                 : result<real>{miss("segment point misses")};
    }
    return segment_fraction(ray_obb_hit(ray{s.a, d, len}, box), len);
}

result<real> segment_polygon_impl(const segment& s, const polygon& poly) {
    const vec2 d = s.b - s.a;
    const real len = length(d);
    if (len <= k_epsilon) {
        return poly.contains(s.a) ? result<real>{real{0.0f}}
                                  : result<real>{miss("segment point misses")};
    }
    return segment_fraction(ray_polygon_hit(ray{s.a, d, len}, poly), len);
}

} // namespace

result<real> segment_circle_hit(const segment& s, const circle& c) {
    return segment_circle_impl(s, c);
}

result<real> segment_aabb_hit(const segment& s, const aabb& box) {
    return segment_aabb_impl(s, box);
}

result<real> segment_obb_hit(const segment& s, const obb& box) {
    return segment_obb_impl(s, box);
}

result<real> segment_polygon_hit(const segment& s, const polygon& poly) {
    return segment_polygon_impl(s, poly);
}

bool intersects(const segment& s, const circle& c) noexcept {
    return segment_circle_hit(s, c).ok();
}

bool intersects(const segment& s, const aabb& box) noexcept {
    return segment_aabb_hit(s, box).ok();
}

bool intersects(const segment& s, const obb& box) noexcept {
    return segment_obb_hit(s, box).ok();
}

bool intersects(const segment& s, const polygon& poly) noexcept {
    return segment_polygon_hit(s, poly).ok();
}

} // namespace geom
} // namespace game
} // namespace crpc