// CRPC -- 3D shape predicates, bounds and anchors. See shapes3d.hpp.
#include <crpc/game/geom/shapes3d.hpp>

#include <algorithm>
#include <cmath>

#include "geometry_detail.hpp"

namespace crpc {
namespace game {
namespace geom {

namespace {

using detail::finite_real;

real non_negative(real value) noexcept {
    return value > 0.0f ? value : 0.0f;
}

double extent_along(const vec3& axis, double half_angle, double reach,
                    const vec3& query) noexcept {
    if (reach <= 0.0) {
        return 0.0;
    }
    const double cosine = detail::clamp_double(
        static_cast<double>(dot(axis, query)), -1.0, 1.0);
    const double beta = std::acos(cosine);
    if (half_angle >= static_cast<double>(k_pi) - 1e-6) {
        return reach;
    }
    if (beta <= half_angle + 1e-9) {
        return reach;
    }
    const double value = reach * std::cos(beta - half_angle);
    return value > 0.0 ? value : 0.0;
}

vec3 safe_axis(vec3 axis) noexcept {
    vec3 u = normalized(axis);
    if (length_squared(u) <= 0.0f) {
        u = vec3(1.0f, 0.0f, 0.0f);
    }
    return u;
}

} // namespace

// --- aabb3 -------------------------------------------------------------------

aabb3 aabb3::empty() noexcept { return aabb3{}; }

aabb3 aabb3::unbounded() noexcept {
    aabb3 b;
    b.min = vec3(-std::numeric_limits<real>::infinity(),
                 -std::numeric_limits<real>::infinity(),
                 -std::numeric_limits<real>::infinity());
    b.max = vec3(std::numeric_limits<real>::infinity(),
                 std::numeric_limits<real>::infinity(),
                 std::numeric_limits<real>::infinity());
    return b;
}

bool aabb3::valid() const noexcept {
    return min.x <= max.x && min.y <= max.y && min.z <= max.z;
}

bool aabb3::finite() const noexcept { return is_finite(min) && is_finite(max); }

bool aabb3::contains(vec3 point, real eps) const noexcept {
    return valid() && point.x >= min.x - eps && point.x <= max.x + eps &&
           point.y >= min.y - eps && point.y <= max.y + eps &&
           point.z >= min.z - eps && point.z <= max.z + eps;
}

bool aabb3::overlaps(const aabb3& other, real eps) const noexcept {
    return valid() && other.valid() && min.x <= other.max.x + eps &&
           max.x >= other.min.x - eps && min.y <= other.max.y + eps &&
           max.y >= other.min.y - eps && min.z <= other.max.z + eps &&
           max.z >= other.min.z - eps;
}

void aabb3::expand(vec3 point) noexcept {
    if (point.x < min.x) {
        min.x = point.x;
    }
    if (point.y < min.y) {
        min.y = point.y;
    }
    if (point.z < min.z) {
        min.z = point.z;
    }
    if (point.x > max.x) {
        max.x = point.x;
    }
    if (point.y > max.y) {
        max.y = point.y;
    }
    if (point.z > max.z) {
        max.z = point.z;
    }
}

void aabb3::expand(const aabb3& other) noexcept {
    if (!other.valid()) {
        return;
    }
    expand(other.min);
    expand(other.max);
}

vec3 aabb3::center() const noexcept {
    return vec3((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f,
                (min.z + max.z) * 0.5f);
}

vec3 aabb3::extents() const noexcept {
    return vec3((max.x - min.x) * 0.5f, (max.y - min.y) * 0.5f,
                (max.z - min.z) * 0.5f);
}

vec3 aabb3::centroid() const noexcept { return center(); }

vec3 aabb3::edge_anchor() const noexcept {
    const vec3 c = center();
    return vec3(max.x, c.y, c.z);
}

aabb3 aabb3::bounds() const noexcept { return *this; }

// --- sphere ------------------------------------------------------------------

bool sphere::contains(vec3 point) const noexcept {
    if (!finite_real(radius)) {
        return false;
    }
    if (radius <= 0.0f) {
        return nearly_equal(point, center);
    }
    const double r = static_cast<double>(radius) +
                     static_cast<double>(k_epsilon);
    const vec3 d = point - center;
    const double dx = static_cast<double>(d.x);
    const double dy = static_cast<double>(d.y);
    const double dz = static_cast<double>(d.z);
    return dx * dx + dy * dy + dz * dz <= r * r;
}

aabb3 sphere::bounds() const noexcept {
    aabb3 b;
    const real r = non_negative(radius);
    b.expand(vec3(center.x - r, center.y - r, center.z - r));
    b.expand(vec3(center.x + r, center.y + r, center.z + r));
    return b;
}

vec3 sphere::centroid() const noexcept { return center; }

vec3 sphere::edge_anchor() const noexcept {
    return vec3(center.x + non_negative(radius), center.y, center.z);
}

// --- cone --------------------------------------------------------------------

bool cone::contains(vec3 point) const noexcept {
    if (!finite_real(half_angle) || !finite_real(reach)) {
        return false;
    }
    const vec3 d = point - apex;
    const double dist = static_cast<double>(length(d));
    if (dist <= static_cast<double>(k_epsilon)) {
        return true;
    }
    if (dist > std::max(static_cast<double>(reach), 0.0) +
                   static_cast<double>(k_epsilon)) {
        return false;
    }
    const vec3 u = normalized(axis);
    if (length_squared(u) <= 0.0f) {
        return false;
    }
    const double half = detail::clamp_double(
        static_cast<double>(half_angle), 0.0, static_cast<double>(k_pi));
    if (half >= static_cast<double>(k_pi) - 1e-6) {
        return true;
    }
    const double cosine = detail::clamp_double(
        static_cast<double>(dot(d, u)) / dist, -1.0, 1.0);
    const double angle = std::acos(cosine);
    const double tolerance =
        static_cast<double>(k_epsilon) / std::max(dist, 1.0);
    return angle <= half + tolerance;
}

aabb3 cone::bounds() const noexcept {
    aabb3 b;
    const vec3 u = safe_axis(axis);
    const double half = detail::clamp_double(
        static_cast<double>(half_angle), 0.0, static_cast<double>(k_pi));
    const double r = std::max(static_cast<double>(reach), 0.0);
    const double ext[3] = {
        extent_along(u, half, r, vec3(1.0f, 0.0f, 0.0f)),
        extent_along(u, half, r, vec3(0.0f, 1.0f, 0.0f)),
        extent_along(u, half, r, vec3(0.0f, 0.0f, 1.0f))};
    const double ext_min[3] = {
        extent_along(u, half, r, vec3(-1.0f, 0.0f, 0.0f)),
        extent_along(u, half, r, vec3(0.0f, -1.0f, 0.0f)),
        extent_along(u, half, r, vec3(0.0f, 0.0f, -1.0f))};
    const real av[3] = {apex.x, apex.y, apex.z};
    b.min = vec3(static_cast<real>(static_cast<double>(av[0]) - ext_min[0]),
                 static_cast<real>(static_cast<double>(av[1]) - ext_min[1]),
                 static_cast<real>(static_cast<double>(av[2]) - ext_min[2]));
    b.max = vec3(static_cast<real>(static_cast<double>(av[0]) + ext[0]),
                 static_cast<real>(static_cast<double>(av[1]) + ext[1]),
                 static_cast<real>(static_cast<double>(av[2]) + ext[2]));
    return b;
}

vec3 cone::centroid() const noexcept {
    const vec3 u = safe_axis(axis);
    const double half = detail::clamp_double(
        static_cast<double>(half_angle), 0.0, static_cast<double>(k_pi));
    const double r = std::max(static_cast<double>(reach), 0.0);
    // Volume centroid of a spherical cone: d = 3R/8 * (1 + cos(half)).
    const double d = 3.0 * r / 8.0 * (1.0 + std::cos(half));
    return apex + u * static_cast<real>(d);
}

vec3 cone::edge_anchor() const noexcept { return apex; }

// --- obb3 --------------------------------------------------------------------

obb3 obb3::from_euler(vec3 center_value, vec3 half_extents_value, real yaw,
                      real pitch, real roll) noexcept {
    const double cy = std::cos(static_cast<double>(yaw));
    const double sy = std::sin(static_cast<double>(yaw));
    const double cp = std::cos(static_cast<double>(pitch));
    const double sp = std::sin(static_cast<double>(pitch));
    const double cr = std::cos(static_cast<double>(roll));
    const double sr = std::sin(static_cast<double>(roll));
    obb3 box;
    box.center = center_value;
    box.half_extents = half_extents_value;
    box.axis_x = vec3(static_cast<real>(cy * cp), static_cast<real>(sy * cp),
                      static_cast<real>(-sp));
    box.axis_y = vec3(static_cast<real>(cy * sp * sr - sy * cr),
                      static_cast<real>(sy * sp * sr + cy * cr),
                      static_cast<real>(cp * sr));
    box.axis_z = vec3(static_cast<real>(cy * sp * cr + sy * sr),
                      static_cast<real>(sy * sp * cr - cy * sr),
                      static_cast<real>(cp * cr));
    return box;
}

vec3 obb3::corner(std::size_t index) const noexcept {
    const real sx = (index & 1U) != 0U ? 1.0f : -1.0f;
    const real sy = (index & 2U) != 0U ? 1.0f : -1.0f;
    const real sz = (index & 4U) != 0U ? 1.0f : -1.0f;
    return center + axis_x * (half_extents.x * sx) +
           axis_y * (half_extents.y * sy) + axis_z * (half_extents.z * sz);
}

bool obb3::contains(vec3 point) const noexcept {
    const vec3 d = point - center;
    const real hx = non_negative(half_extents.x) + k_epsilon;
    const real hy = non_negative(half_extents.y) + k_epsilon;
    const real hz = non_negative(half_extents.z) + k_epsilon;
    return std::fabs(dot(d, axis_x)) <= hx &&
           std::fabs(dot(d, axis_y)) <= hy &&
           std::fabs(dot(d, axis_z)) <= hz;
}

aabb3 obb3::bounds() const noexcept {
    const real hx = non_negative(half_extents.x);
    const real hy = non_negative(half_extents.y);
    const real hz = non_negative(half_extents.z);
    const vec3 h(
        std::fabs(axis_x.x) * hx + std::fabs(axis_y.x) * hy +
            std::fabs(axis_z.x) * hz,
        std::fabs(axis_x.y) * hx + std::fabs(axis_y.y) * hy +
            std::fabs(axis_z.y) * hz,
        std::fabs(axis_x.z) * hx + std::fabs(axis_y.z) * hy +
            std::fabs(axis_z.z) * hz);
    aabb3 b;
    b.min = center - h;
    b.max = center + h;
    return b;
}

vec3 obb3::centroid() const noexcept { return center; }

vec3 obb3::edge_anchor() const noexcept {
    return center + axis_x * non_negative(half_extents.x);
}

// --- umbrella3 ---------------------------------------------------------------

cone umbrella3::canopy() const noexcept {
    cone c;
    c.apex = apex;
    c.axis = safe_axis(axis);
    c.half_angle = half_angle;
    c.reach = reach;
    return c;
}

obb3 umbrella3::handle() const noexcept {
    const vec3 u = safe_axis(axis);
    const basis3 basis = orthonormal_basis(u);
    const real length = non_negative(handle_length);
    const real width = non_negative(handle_half_width);
    obb3 h;
    h.center = apex - u * (length * 0.5f);
    h.half_extents = vec3(length * 0.5f, width, width);
    h.axis_x = u;
    h.axis_y = basis.right;
    h.axis_z = basis.up;
    return h;
}

bool umbrella3::contains(vec3 point) const noexcept {
    if (nearly_equal(point, apex)) {
        return true;
    }
    if (canopy().contains(point)) {
        return true;
    }
    if (handle_length > 0.0f || handle_half_width > 0.0f) {
        return handle().contains(point);
    }
    return false;
}

aabb3 umbrella3::bounds() const noexcept {
    aabb3 b;
    b.expand(apex);
    b.expand(canopy().bounds());
    if (handle_length > 0.0f || handle_half_width > 0.0f) {
        b.expand(handle().bounds());
    }
    return b;
}

vec3 umbrella3::centroid() const noexcept {
    const cone cap = canopy();
    const double half = detail::clamp_double(
        static_cast<double>(cap.half_angle), 0.0,
        static_cast<double>(k_pi));
    const double r = std::max(static_cast<double>(cap.reach), 0.0);
    const double cap_volume =
        (2.0 * static_cast<double>(k_pi) / 3.0) * r * r * r *
        (1.0 - std::cos(half));
    const double handle_volume =
        4.0 * std::max(static_cast<double>(handle_length), 0.0) *
        std::max(static_cast<double>(handle_half_width), 0.0) *
        std::max(static_cast<double>(handle_half_width), 0.0);
    const double total = cap_volume + handle_volume;
    if (total <= static_cast<double>(k_epsilon)) {
        return apex;
    }
    const vec3 cap_center = cap_volume > 0.0 ? cap.centroid() : apex;
    const vec3 handle_center =
        handle_volume > 0.0 ? handle().center : apex;
    return (cap_center * static_cast<real>(cap_volume) +
            handle_center * static_cast<real>(handle_volume)) /
           static_cast<real>(total);
}

vec3 umbrella3::edge_anchor() const noexcept { return apex; }

// --- ray3 --------------------------------------------------------------------

vec3 ray3::unit_direction() const noexcept { return normalized(direction); }

bool ray3::contains(vec3 point) const noexcept {
    const vec3 u = unit_direction();
    if (length_squared(u) <= 0.0f) {
        return nearly_equal(point, origin);
    }
    const real limit = non_negative(max_length);
    const vec3 d = point - origin;
    const real t = dot(d, u);
    if (t < -k_epsilon || t > limit + k_epsilon) {
        return false;
    }
    const vec3 perpendicular = d - u * t;
    return length(perpendicular) <= k_epsilon;
}

aabb3 ray3::bounds() const noexcept {
    aabb3 b;
    b.expand(origin);
    const vec3 u = unit_direction();
    if (length_squared(u) > 0.0f) {
        b.expand(origin + u * non_negative(max_length));
    }
    return b;
}

vec3 ray3::centroid() const noexcept {
    const vec3 u = unit_direction();
    if (length_squared(u) <= 0.0f) {
        return origin;
    }
    return origin + u * (non_negative(max_length) * 0.5f);
}

vec3 ray3::edge_anchor() const noexcept { return origin; }

// --- segment3 ----------------------------------------------------------------

bool segment3::contains(vec3 point) const noexcept {
    return detail::point_segment_distance3d(point, a, b) <=
           static_cast<double>(k_epsilon);
}

aabb3 segment3::bounds() const noexcept {
    aabb3 box;
    box.expand(a);
    box.expand(b);
    return box;
}

vec3 segment3::centroid() const noexcept {
    return vec3((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f,
                (a.z + b.z) * 0.5f);
}

vec3 segment3::edge_anchor() const noexcept { return a; }

// --- line3 -------------------------------------------------------------------

vec3 line3::unit_direction() const noexcept { return normalized(direction); }

bool line3::contains(vec3 query) const noexcept {
    const vec3 u = unit_direction();
    if (length_squared(u) <= 0.0f) {
        return false;
    }
    const vec3 d = query - point;
    const vec3 perpendicular = d - u * dot(d, u);
    return length(perpendicular) <= k_epsilon;
}

aabb3 line3::bounds() const noexcept { return aabb3::unbounded(); }

vec3 line3::centroid() const noexcept { return point; }

vec3 line3::edge_anchor() const noexcept { return point; }

// --- plane3 ------------------------------------------------------------------

real plane3::signed_distance(vec3 query) const noexcept {
    const vec3 n = normalized(normal);
    if (length_squared(n) <= 0.0f) {
        return 0.0f;
    }
    return dot(n, query - point);
}

bool plane3::contains(vec3 query) const noexcept {
    if (length_squared(normalized(normal)) <= 0.0f) {
        return false;
    }
    return std::fabs(signed_distance(query)) <= k_epsilon;
}

// --- convex_polyhedron -------------------------------------------------------

convex_polyhedron convex_polyhedron::from_box(const aabb3& box) noexcept {
    convex_polyhedron p;
    if (!box.valid()) {
        return p;
    }
    p.vertices = {vec3(box.min.x, box.min.y, box.min.z),
                  vec3(box.max.x, box.min.y, box.min.z),
                  vec3(box.min.x, box.max.y, box.min.z),
                  vec3(box.max.x, box.max.y, box.min.z),
                  vec3(box.min.x, box.min.y, box.max.z),
                  vec3(box.max.x, box.min.y, box.max.z),
                  vec3(box.min.x, box.max.y, box.max.z),
                  vec3(box.max.x, box.max.y, box.max.z)};
    p.planes = {plane3{vec3(box.min.x, box.min.y, box.min.z),
                       vec3(-1.0f, 0.0f, 0.0f)},
                plane3{vec3(box.max.x, box.max.y, box.max.z),
                       vec3(1.0f, 0.0f, 0.0f)},
                plane3{vec3(box.min.x, box.min.y, box.min.z),
                       vec3(0.0f, -1.0f, 0.0f)},
                plane3{vec3(box.max.x, box.max.y, box.max.z),
                       vec3(0.0f, 1.0f, 0.0f)},
                plane3{vec3(box.min.x, box.min.y, box.min.z),
                       vec3(0.0f, 0.0f, -1.0f)},
                plane3{vec3(box.max.x, box.max.y, box.max.z),
                       vec3(0.0f, 0.0f, 1.0f)}};
    return p;
}

bool convex_polyhedron::valid() const noexcept {
    return !vertices.empty() && !planes.empty();
}

bool convex_polyhedron::contains(vec3 point) const noexcept {
    if (!valid()) {
        return false;
    }
    for (const plane3& p : planes) {
        const vec3 n = normalized(p.normal);
        if (length_squared(n) <= 0.0f) {
            continue;
        }
        if (dot(n, point - p.point) > k_epsilon) {
            return false;
        }
    }
    return true;
}

aabb3 convex_polyhedron::bounds() const noexcept {
    aabb3 b;
    for (const vec3 v : vertices) {
        b.expand(v);
    }
    return b;
}

vec3 convex_polyhedron::centroid() const noexcept {
    if (vertices.empty()) {
        return vec3(0.0f, 0.0f, 0.0f);
    }
    vec3 sum{};
    for (const vec3 v : vertices) {
        sum += v;
    }
    return sum / static_cast<real>(vertices.size());
}

vec3 convex_polyhedron::edge_anchor() const noexcept {
    return vertices.empty() ? vec3(0.0f, 0.0f, 0.0f) : vertices.front();
}

// --- anchors -----------------------------------------------------------------

vec3 anchor_point(const sphere& shape, anchor_mode mode) noexcept {
    return mode == anchor_mode::center ? shape.centroid() : shape.edge_anchor();
}

vec3 anchor_point(const cone& shape, anchor_mode mode) noexcept {
    return mode == anchor_mode::center ? shape.centroid() : shape.edge_anchor();
}

vec3 anchor_point(const umbrella3& shape, anchor_mode mode) noexcept {
    return mode == anchor_mode::center ? shape.centroid() : shape.edge_anchor();
}

vec3 anchor_point(const aabb3& shape, anchor_mode mode) noexcept {
    return mode == anchor_mode::center ? shape.centroid() : shape.edge_anchor();
}

vec3 anchor_point(const obb3& shape, anchor_mode mode) noexcept {
    return mode == anchor_mode::center ? shape.centroid() : shape.edge_anchor();
}

vec3 anchor_point(const ray3& shape, anchor_mode mode) noexcept {
    return mode == anchor_mode::center ? shape.centroid() : shape.edge_anchor();
}

vec3 anchor_point(const segment3& shape, anchor_mode mode) noexcept {
    return mode == anchor_mode::center ? shape.centroid() : shape.edge_anchor();
}

vec3 anchor_point(const line3& shape, anchor_mode mode) noexcept {
    return mode == anchor_mode::center ? shape.centroid() : shape.edge_anchor();
}

vec3 anchor_point(const convex_polyhedron& shape, anchor_mode mode) noexcept {
    return mode == anchor_mode::center ? shape.centroid() : shape.edge_anchor();
}

} // namespace geom
} // namespace game
} // namespace crpc