// CRPC -- 2D shape predicates, bounds and anchors. See shapes2d.hpp.
#include <crpc/game/geom/shapes2d.hpp>

#include <algorithm>
#include <cmath>

#include "geometry_detail.hpp"

namespace crpc {
namespace game {
namespace geom {

namespace {

using detail::finite_real;

constexpr double k_angle_eps = 1e-6;

real non_negative(real value) noexcept {
    return value > 0.0f ? value : 0.0f;
}

// Angular width used to accept a point that is positionally on the boundary.
// A positional tolerance `eps` at distance `dist` from the apex spans roughly
// eps/dist radians, which keeps the test consistent under scaling.
double angular_tolerance(real dist) noexcept {
    const double d = std::max(static_cast<double>(dist), 1.0);
    return static_cast<double>(k_epsilon) / d;
}

bool angle_in_sweep(double angle, double start, double sweep) noexcept {
    if (sweep >= static_cast<double>(k_two_pi) - static_cast<double>(k_epsilon)) {
        return true;
    }
    if (sweep <= 0.0) {
        return false;
    }
    const double delta =
        static_cast<double>(normalize_angle(static_cast<real>(angle - start)));
    return delta <= sweep + k_angle_eps;
}

} // namespace

// --- aabb --------------------------------------------------------------------

aabb aabb::empty() noexcept { return aabb{}; }

aabb aabb::unbounded() noexcept {
    aabb b;
    b.min = vec2(-std::numeric_limits<real>::infinity(),
                 -std::numeric_limits<real>::infinity());
    b.max = vec2(std::numeric_limits<real>::infinity(),
                 std::numeric_limits<real>::infinity());
    return b;
}

bool aabb::valid() const noexcept {
    return min.x <= max.x && min.y <= max.y;
}

bool aabb::finite() const noexcept {
    return is_finite(min) && is_finite(max);
}

bool aabb::contains(vec2 point, real eps) const noexcept {
    return valid() && point.x >= min.x - eps && point.x <= max.x + eps &&
           point.y >= min.y - eps && point.y <= max.y + eps;
}

bool aabb::overlaps(const aabb& other, real eps) const noexcept {
    return valid() && other.valid() && min.x <= other.max.x + eps &&
           max.x >= other.min.x - eps && min.y <= other.max.y + eps &&
           max.y >= other.min.y - eps;
}

void aabb::expand(vec2 point) noexcept {
    if (point.x < min.x) {
        min.x = point.x;
    }
    if (point.y < min.y) {
        min.y = point.y;
    }
    if (point.x > max.x) {
        max.x = point.x;
    }
    if (point.y > max.y) {
        max.y = point.y;
    }
}

void aabb::expand(const aabb& other) noexcept {
    if (!other.valid()) {
        return;
    }
    expand(other.min);
    expand(other.max);
}

vec2 aabb::center() const noexcept {
    return vec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
}

vec2 aabb::extents() const noexcept {
    return vec2((max.x - min.x) * 0.5f, (max.y - min.y) * 0.5f);
}

vec2 aabb::centroid() const noexcept { return center(); }

vec2 aabb::edge_anchor() const noexcept {
    const vec2 c = center();
    return vec2(max.x, c.y);
}

aabb aabb::bounds() const noexcept { return *this; }

// --- circle ------------------------------------------------------------------

bool circle::contains(vec2 point) const noexcept {
    if (!finite_real(radius)) {
        return false;
    }
    if (radius <= 0.0f) {
        return nearly_equal(point, center);
    }
    const double r = static_cast<double>(radius) +
                     static_cast<double>(k_epsilon);
    const double dx = static_cast<double>(point.x - center.x);
    const double dy = static_cast<double>(point.y - center.y);
    return dx * dx + dy * dy <= r * r;
}

aabb circle::bounds() const noexcept {
    aabb b;
    const real r = non_negative(radius);
    b.expand(vec2(center.x - r, center.y - r));
    b.expand(vec2(center.x + r, center.y + r));
    return b;
}

vec2 circle::centroid() const noexcept { return center; }

vec2 circle::edge_anchor() const noexcept {
    return vec2(center.x + non_negative(radius), center.y);
}

// --- sector ------------------------------------------------------------------

bool sector::full() const noexcept {
    return sweep >= k_two_pi - k_epsilon;
}

bool sector::contains(vec2 point) const noexcept {
    if (!finite_real(radius) || !finite_real(sweep)) {
        return false;
    }
    if (sweep <= 0.0f) {
        return false;
    }
    if (radius <= 0.0f) {
        return nearly_equal(point, center);
    }
    const double dx = static_cast<double>(point.x - center.x);
    const double dy = static_cast<double>(point.y - center.y);
    const double dist = std::sqrt(dx * dx + dy * dy);
    if (dist > static_cast<double>(radius) + static_cast<double>(k_epsilon)) {
        return false;
    }
    if (dist <= static_cast<double>(k_epsilon)) {
        return true;
    }
    if (full() || sweep > k_two_pi) {
        return true;
    }
    const double angle = std::atan2(dy, dx);
    const double start = static_cast<double>(normalize_angle(start_angle));
    double delta = static_cast<double>(
        normalize_angle(static_cast<real>(angle - start)));
    if (delta > static_cast<double>(k_two_pi) - k_angle_eps) {
        delta = 0.0;
    }
    return delta <= static_cast<double>(sweep) + angular_tolerance(radius);
}

aabb sector::bounds() const noexcept {
    aabb b;
    if (sweep <= 0.0f || !finite_real(sweep) || !finite_real(radius)) {
        return b;
    }
    b.expand(center);
    const real r = non_negative(radius);
    if (r <= 0.0f) {
        return b;
    }
    const double start = static_cast<double>(normalize_angle(start_angle));
    const double width = std::min(static_cast<double>(sweep),
                                  static_cast<double>(k_two_pi));
    if (width >= static_cast<double>(k_two_pi) -
                     static_cast<double>(k_epsilon)) {
        b.expand(vec2(center.x - r, center.y));
        b.expand(vec2(center.x + r, center.y));
        b.expand(vec2(center.x, center.y - r));
        b.expand(vec2(center.x, center.y + r));
        return b;
    }
    const double angles[] = {start,
                             start + width,
                             0.0,
                             static_cast<double>(k_pi) * 0.5,
                             static_cast<double>(k_pi),
                             static_cast<double>(k_pi) * 1.5};
    for (const double angle : angles) {
        if (!angle_in_sweep(angle, start, width)) {
            continue;
        }
        b.expand(vec2(center.x + static_cast<real>(
                                 static_cast<double>(r) * std::cos(angle)),
                      center.y + static_cast<real>(
                                     static_cast<double>(r) * std::sin(angle))));
    }
    return b;
}

vec2 sector::centroid() const noexcept {
    if (sweep <= 0.0f || radius <= 0.0f || full()) {
        return center;
    }
    const double half = static_cast<double>(sweep) * 0.5;
    const double d = (4.0 * static_cast<double>(radius) * std::sin(half)) /
                     (3.0 * static_cast<double>(sweep));
    const double bisector =
        static_cast<double>(normalize_angle(start_angle)) + half;
    return vec2(center.x + static_cast<real>(d * std::cos(bisector)),
                center.y + static_cast<real>(d * std::sin(bisector)));
}

vec2 sector::edge_anchor() const noexcept { return center; }

// --- obb ---------------------------------------------------------------------

vec2 obb::axis_x() const noexcept {
    const double c = std::cos(static_cast<double>(rotation));
    const double s = std::sin(static_cast<double>(rotation));
    return vec2(static_cast<real>(c), static_cast<real>(s));
}

vec2 obb::axis_y() const noexcept {
    const double c = std::cos(static_cast<double>(rotation));
    const double s = std::sin(static_cast<double>(rotation));
    return vec2(static_cast<real>(-s), static_cast<real>(c));
}

vec2 obb::corner(std::size_t index) const noexcept {
    const vec2 ux = axis_x();
    const vec2 uy = axis_y();
    const real sx = (index & 1U) != 0U ? 1.0f : -1.0f;
    const real sy = (index & 2U) != 0U ? 1.0f : -1.0f;
    return center + ux * (half_extents.x * sx) + uy * (half_extents.y * sy);
}

bool obb::contains(vec2 point) const noexcept {
    if (!finite_real(half_extents.x) || !finite_real(half_extents.y)) {
        return false;
    }
    const vec2 ux = axis_x();
    const vec2 uy = axis_y();
    const vec2 d = point - center;
    const real hx = non_negative(half_extents.x) + k_epsilon;
    const real hy = non_negative(half_extents.y) + k_epsilon;
    return std::fabs(dot(d, ux)) <= hx && std::fabs(dot(d, uy)) <= hy;
}

aabb obb::bounds() const noexcept {
    aabb b;
    for (std::size_t i = 0; i < 4; ++i) {
        b.expand(corner(i));
    }
    return b;
}

vec2 obb::centroid() const noexcept { return center; }

vec2 obb::edge_anchor() const noexcept {
    return center + axis_x() * non_negative(half_extents.x);
}

// --- umbrella ----------------------------------------------------------------

sector umbrella::canopy() const noexcept {
    const vec2 u = normalized(direction);
    real angle = 0.0f;
    if (length_squared(u) > 0.0f) {
        angle = angle_of(u);
    }
    sector s;
    s.center = apex;
    s.radius = reach;
    s.start_angle = angle - half_angle;
    s.sweep = 2.0f * half_angle;
    return s;
}

obb umbrella::handle() const noexcept {
    const vec2 u = normalized(direction);
    const vec2 axis = length_squared(u) > 0.0f ? u : vec2(1.0f, 0.0f);
    obb h;
    const real length = non_negative(handle_length);
    h.center = apex - axis * (length * 0.5f);
    h.half_extents = vec2(length * 0.5f, non_negative(handle_half_width));
    h.rotation = angle_of(axis);
    return h;
}

bool umbrella::contains(vec2 point) const noexcept {
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

aabb umbrella::bounds() const noexcept {
    aabb b;
    b.expand(apex);
    b.expand(canopy().bounds());
    if (handle_length > 0.0f || handle_half_width > 0.0f) {
        b.expand(handle().bounds());
    }
    return b;
}

vec2 umbrella::centroid() const noexcept {
    const sector cap = canopy();
    const double cap_sweep =
        std::min(std::max(static_cast<double>(cap.sweep), 0.0),
                 static_cast<double>(k_two_pi));
    const double cap_radius =
        std::max(static_cast<double>(cap.radius), 0.0);
    const double cap_area = 0.5 * cap_radius * cap_radius * cap_sweep;
    const double handle_area =
        2.0 * std::max(static_cast<double>(handle_length), 0.0) *
        std::max(static_cast<double>(handle_half_width), 0.0);
    const double total = cap_area + handle_area;
    if (total <= static_cast<double>(k_epsilon)) {
        return apex;
    }
    const vec2 cap_center = cap_area > 0.0 ? cap.centroid() : apex;
    const vec2 handle_center =
        handle_area > 0.0 ? handle().center : apex;
    return vec2(static_cast<real>(
                    (static_cast<double>(cap_center.x) * cap_area +
                     static_cast<double>(handle_center.x) * handle_area) /
                    total),
                static_cast<real>(
                    (static_cast<double>(cap_center.y) * cap_area +
                     static_cast<double>(handle_center.y) * handle_area) /
                    total));
}

vec2 umbrella::edge_anchor() const noexcept { return apex; }

// --- ray ---------------------------------------------------------------------

vec2 ray::unit_direction() const noexcept { return normalized(direction); }

bool ray::contains(vec2 point) const noexcept {
    const vec2 u = unit_direction();
    if (length_squared(u) <= 0.0f) {
        return nearly_equal(point, origin);
    }
    const real limit = non_negative(max_length);
    const vec2 d = point - origin;
    const real t = dot(d, u);
    if (t < -k_epsilon || t > limit + k_epsilon) {
        return false;
    }
    const vec2 perpendicular = d - u * t;
    return length(perpendicular) <= k_epsilon;
}

aabb ray::bounds() const noexcept {
    aabb b;
    b.expand(origin);
    const vec2 u = unit_direction();
    if (length_squared(u) > 0.0f) {
        b.expand(origin + u * non_negative(max_length));
    }
    return b;
}

vec2 ray::centroid() const noexcept {
    const vec2 u = unit_direction();
    if (length_squared(u) <= 0.0f) {
        return origin;
    }
    return origin + u * (non_negative(max_length) * 0.5f);
}

vec2 ray::edge_anchor() const noexcept { return origin; }

// --- segment -----------------------------------------------------------------

bool segment::contains(vec2 point) const noexcept {
    return detail::point_on_segment(point, a, b, k_epsilon);
}

aabb segment::bounds() const noexcept {
    aabb box;
    box.expand(a);
    box.expand(b);
    return box;
}

vec2 segment::centroid() const noexcept {
    return vec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
}

vec2 segment::edge_anchor() const noexcept { return a; }

// --- line --------------------------------------------------------------------

vec2 line::unit_direction() const noexcept { return normalized(direction); }

bool line::contains(vec2 query) const noexcept {
    const vec2 u = unit_direction();
    if (length_squared(u) <= 0.0f) {
        return false;
    }
    const vec2 d = query - point;
    const vec2 perpendicular = d - u * dot(d, u);
    return length(perpendicular) <= k_epsilon;
}

aabb line::bounds() const noexcept { return aabb::unbounded(); }

vec2 line::centroid() const noexcept { return point; }

vec2 line::edge_anchor() const noexcept { return point; }

// --- polygon -----------------------------------------------------------------

std::size_t polygon::size() const noexcept { return vertices.size(); }

bool polygon::contains(vec2 point) const noexcept {
    const std::size_t n = vertices.size();
    if (n == 0) {
        return false;
    }
    if (n == 1) {
        return nearly_equal(point, vertices[0]);
    }
    if (n == 2) {
        return detail::point_on_segment(point, vertices[0], vertices[1],
                                        k_epsilon);
    }
    // Boundary first, so vertices/edges have the documented closed-set
    // behaviour even when the crossing test would be a coin flip.
    for (std::size_t i = 0; i < n; ++i) {
        if (detail::point_on_segment(point, vertices[i],
                                     vertices[(i + 1) % n], k_epsilon)) {
            return true;
        }
    }
    bool inside = false;
    const double px = static_cast<double>(point.x);
    const double py = static_cast<double>(point.y);
    for (std::size_t i = 0; i < n; ++i) {
        const vec2 a = vertices[i];
        const vec2 b = vertices[(i + 1) % n];
        const bool a_above = static_cast<double>(a.y) > py;
        const bool b_above = static_cast<double>(b.y) > py;
        if (a_above == b_above) {
            continue;
        }
        const double x_intersection =
            static_cast<double>(a.x) +
            (py - static_cast<double>(a.y)) *
                (static_cast<double>(b.x) - static_cast<double>(a.x)) /
                (static_cast<double>(b.y) - static_cast<double>(a.y));
        if (px < x_intersection) {
            inside = !inside;
        }
    }
    return inside;
}

aabb polygon::bounds() const noexcept {
    aabb b;
    for (const vec2 v : vertices) {
        b.expand(v);
    }
    return b;
}

vec2 polygon::centroid() const noexcept {
    const std::size_t n = vertices.size();
    if (n == 0) {
        return vec2(0.0f, 0.0f);
    }
    if (n < 3) {
        vec2 sum{};
        for (const vec2 v : vertices) {
            sum += v;
        }
        return sum / static_cast<real>(n);
    }
    double area2 = 0.0;
    double cx = 0.0;
    double cy = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const vec2 a = vertices[i];
        const vec2 b = vertices[(i + 1) % n];
        const double cross_value =
            static_cast<double>(a.x) * static_cast<double>(b.y) -
            static_cast<double>(b.x) * static_cast<double>(a.y);
        area2 += cross_value;
        cx += (static_cast<double>(a.x) + static_cast<double>(b.x)) *
              cross_value;
        cy += (static_cast<double>(a.y) + static_cast<double>(b.y)) *
              cross_value;
    }
    if (std::fabs(area2) <= static_cast<double>(k_epsilon)) {
        vec2 sum{};
        for (const vec2 v : vertices) {
            sum += v;
        }
        return sum / static_cast<real>(n);
    }
    const double scale = 1.0 / (3.0 * area2);
    return vec2(static_cast<real>(cx * scale),
                static_cast<real>(cy * scale));
}

vec2 polygon::edge_anchor() const noexcept {
    return vertices.empty() ? vec2(0.0f, 0.0f) : vertices.front();
}

bool polygon::is_convex() const noexcept {
    const std::size_t n = vertices.size();
    if (n < 3) {
        return false;
    }
    bool positive = false;
    bool negative = false;
    for (std::size_t i = 0; i < n; ++i) {
        const vec2 a = vertices[i];
        const vec2 b = vertices[(i + 1) % n];
        const vec2 c = vertices[(i + 2) % n];
        const vec2 e1 = b - a;
        const vec2 e2 = c - b;
        if (length_squared(e1) <= k_epsilon * k_epsilon ||
            length_squared(e2) <= k_epsilon * k_epsilon) {
            continue;
        }
        const real turn = cross(e1, e2);
        if (turn > k_epsilon) {
            positive = true;
        } else if (turn < -k_epsilon) {
            negative = true;
        }
        if (positive && negative) {
            return false;
        }
    }
    return true;
}

// --- anchors -----------------------------------------------------------------

vec2 anchor_point(const circle& shape, anchor_mode mode) noexcept {
    return mode == anchor_mode::center ? shape.centroid() : shape.edge_anchor();
}

vec2 anchor_point(const sector& shape, anchor_mode mode) noexcept {
    return mode == anchor_mode::center ? shape.centroid() : shape.edge_anchor();
}

vec2 anchor_point(const umbrella& shape, anchor_mode mode) noexcept {
    return mode == anchor_mode::center ? shape.centroid() : shape.edge_anchor();
}

vec2 anchor_point(const aabb& shape, anchor_mode mode) noexcept {
    return mode == anchor_mode::center ? shape.centroid() : shape.edge_anchor();
}

vec2 anchor_point(const obb& shape, anchor_mode mode) noexcept {
    return mode == anchor_mode::center ? shape.centroid() : shape.edge_anchor();
}

vec2 anchor_point(const ray& shape, anchor_mode mode) noexcept {
    return mode == anchor_mode::center ? shape.centroid() : shape.edge_anchor();
}

vec2 anchor_point(const segment& shape, anchor_mode mode) noexcept {
    return mode == anchor_mode::center ? shape.centroid() : shape.edge_anchor();
}

vec2 anchor_point(const line& shape, anchor_mode mode) noexcept {
    return mode == anchor_mode::center ? shape.centroid() : shape.edge_anchor();
}

vec2 anchor_point(const polygon& shape, anchor_mode mode) noexcept {
    return mode == anchor_mode::center ? shape.centroid() : shape.edge_anchor();
}

} // namespace geom
} // namespace game
} // namespace crpc