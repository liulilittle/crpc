// CRPC -- tests for crpc/game/geom: hitbox shape library.
//
// The suite is deliberately built as an oracle comparison rather than a set of
// "looks right" examples:
//
//   * point-in-polygon is compared against an independent winding-number
//     reference on 20 random star-shaped simple polygons x random points, plus
//     explicit concave and degenerate polygons;
//   * OBB-OBB is compared against a dense interior sampling of one box tested
//     with an independent point-in-OBB routine;
//   * ray hits are compared against independent analytic quadratic / slab /
//     edge-crossing solutions;
//   * center and edge anchoring are exercised on every shape, and the same
//     probe point must give a different answer for the two modes wherever the
//     shape actually has an interior centre distinct from its boundary anchor.
//
// All scalar comparisons go through explicit tolerances (never operator==).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <crpc/game/geom/collide2d.hpp>
#include <crpc/game/geom/hitbox.hpp>
#include <crpc/game/geom/shapes2d.hpp>
#include <crpc/game/geom/transform.hpp>
#include <crpc/game/geom/vec.hpp>

#include "test_framework.hpp"

namespace {

namespace geom = crpc::game::geom;
using geom::real;
using geom::vec2;

// The public tolerance is 1e-5 (see vec2.hpp for the float rationale). The
// comparisons below use a slightly looser 1e-4 when a value has gone through
// several rotations / square roots, because float has ~7 significant decimal
// digits and trig round-trips accumulate a few ulps.
constexpr real k_close = 1e-4f;

bool close(real a, real b, real eps = k_close) {
    return std::fabs(a - b) <= eps;
}

bool close_vec(vec2 a, vec2 b, real eps = k_close) {
    return close(a.x, b.x, eps) && close(a.y, b.y, eps);
}

bool close_double(double a, double b, double eps) {
    return std::fabs(a - b) <= eps;
}

std::uint64_t xorshift64(std::uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

double unit_random(std::uint64_t& state) {
    // 53 random bits scaled into [0,1).
    return static_cast<double>(xorshift64(state) >> 11) / 9007199254740992.0;
}

real uniform_real(real lo, real hi, std::uint64_t& state) {
    return static_cast<real>(static_cast<double>(lo) +
                             (static_cast<double>(hi) - static_cast<double>(lo)) *
                                 unit_random(state));
}

geom::polygon poly_of(std::initializer_list<vec2> points) {
    geom::polygon p;
    p.vertices.assign(points.begin(), points.end());
    return p;
}

double point_segment_distance(vec2 p, vec2 a, vec2 b) {
    const double dx = static_cast<double>(b.x - a.x);
    const double dy = static_cast<double>(b.y - a.y);
    const double len2 = dx * dx + dy * dy;
    if (len2 <= 1e-18) {
        const double px = static_cast<double>(p.x - a.x);
        const double py = static_cast<double>(p.y - a.y);
        return std::sqrt(px * px + py * py);
    }
    double t = (static_cast<double>(p.x - a.x) * dx +
                static_cast<double>(p.y - a.y) * dy) /
               len2;
    t = std::max(0.0, std::min(1.0, t));
    const double qx = static_cast<double>(a.x) + dx * t;
    const double qy = static_cast<double>(a.y) + dy * t;
    const double ex = static_cast<double>(p.x) - qx;
    const double ey = static_cast<double>(p.y) - qy;
    return std::sqrt(ex * ex + ey * ey);
}

int winding_number(const geom::polygon& p, vec2 q) {
    int wn = 0;
    const std::size_t n = p.vertices.size();
    for (std::size_t i = 0; i < n; ++i) {
        const vec2 a = p.vertices[i];
        const vec2 b = p.vertices[(i + 1) % n];
        if (a.y <= q.y) {
            if (b.y > q.y && geom::cross(b - a, q - a) > 0.0f) {
                ++wn;
            }
        } else {
            if (b.y <= q.y && geom::cross(b - a, q - a) < 0.0f) {
                --wn;
            }
        }
    }
    return wn;
}

double min_edge_distance(const geom::polygon& p, vec2 q) {
    double best = std::numeric_limits<double>::infinity();
    const std::size_t n = p.vertices.size();
    for (std::size_t i = 0; i < n; ++i) {
        best = std::min(best, point_segment_distance(q, p.vertices[i],
                                                    p.vertices[(i + 1) % n]));
    }
    return best;
}

// A star-shaped simple polygon: vertices are generated at strictly increasing
// angles around a common centre, so consecutive edges never cross and the
// polygon is simple by construction (any ray from the centre meets the
// boundary exactly once). This is what lets the winding-number oracle stand in
// for an arbitrary simple polygon.
geom::polygon random_star_polygon(std::uint64_t& state, std::size_t n,
                                  vec2 center, real rmin, real rmax) {
    std::vector<real> angles;
    angles.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        angles.push_back(uniform_real(0.0f, geom::k_two_pi, state));
    }
    std::sort(angles.begin(), angles.end());
    geom::polygon p;
    p.vertices.reserve(n);
    for (const real a : angles) {
        const real r = uniform_real(rmin, rmax, state);
        p.vertices.push_back(center + vec2(std::cos(a) * r, std::sin(a) * r));
    }
    return p;
}

vec2 obb_point(const geom::obb& o, real u, real v) {
    return o.center + o.axis_x() * u + o.axis_y() * v;
}

// Independent analytic ray/circle solution used as the oracle.
bool analytic_ray_circle(const geom::ray& r, const geom::circle& c, double& t) {
    const vec2 d = r.unit_direction();
    const vec2 oc = r.origin - c.center;
    const double b = static_cast<double>(geom::dot(oc, d));
    const double cc = static_cast<double>(geom::dot(oc, oc)) -
                      static_cast<double>(c.radius) * static_cast<double>(c.radius);
    const double disc = b * b - cc;
    if (cc <= 0.0) {
        t = 0.0;
        return true;
    }
    if (disc < 0.0) {
        return false;
    }
    const double s = std::sqrt(std::max(0.0, disc));
    const double t0 = -b - s;
    const double t1 = -b + s;
    const double limit = static_cast<double>(r.max_length);
    if (t0 >= 0.0 && t0 <= limit + 1e-6) {
        t = t0;
        return true;
    }
    if (t1 >= 0.0 && t1 <= limit + 1e-6) {
        t = t1;
        return true;
    }
    return false;
}

// Independent slab solution in the box's local frame.
bool analytic_ray_obb(const geom::ray& r, const geom::obb& o, double& t) {
    const vec2 ux = o.axis_x();
    const vec2 uy = o.axis_y();
    const vec2 d = r.unit_direction();
    const vec2 rel = r.origin - o.center;
    const double ox = static_cast<double>(geom::dot(rel, ux));
    const double oy = static_cast<double>(geom::dot(rel, uy));
    const double dx = static_cast<double>(geom::dot(d, ux));
    const double dy = static_cast<double>(geom::dot(d, uy));
    const double hx = static_cast<double>(o.half_extents.x);
    const double hy = static_cast<double>(o.half_extents.y);

    double tmin = -std::numeric_limits<double>::infinity();
    double tmax = std::numeric_limits<double>::infinity();
    const double origins[2] = {ox, oy};
    const double dirs[2] = {dx, dy};
    const double halves[2] = {hx, hy};
    for (int axis = 0; axis < 2; ++axis) {
        const double p = origins[axis];
        const double v = dirs[axis];
        const double h = halves[axis];
        if (std::fabs(v) < 1e-12) {
            if (p < -h || p > h) {
                return false;
            }
            continue;
        }
        double t1 = (-h - p) / v;
        double t2 = (h - p) / v;
        if (t1 > t2) {
            std::swap(t1, t2);
        }
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
    }
    if (tmax < tmin) {
        return false;
    }
    const double limit = static_cast<double>(r.max_length);
    if (tmin < 0.0) {
        if (tmax < 0.0) {
            return false;
        }
        t = 0.0;
        return t <= limit + 1e-6;
    }
    if (tmin > limit + 1e-6) {
        return false;
    }
    t = tmin;
    return true;
}

bool analytic_ray_polygon(const geom::ray& r, const geom::polygon& p, double& t) {
    const vec2 d = r.unit_direction();
    const double limit = static_cast<double>(r.max_length);
    if (p.contains(r.origin)) {
        t = 0.0;
        return true;
    }
    bool found = false;
    double best = std::numeric_limits<double>::infinity();
    const std::size_t n = p.vertices.size();
    for (std::size_t i = 0; i < n; ++i) {
        const vec2 a = p.vertices[i];
        const vec2 b = p.vertices[(i + 1) % n];
        const vec2 e = b - a;
        const double denom = static_cast<double>(geom::cross(d, e));
        if (std::fabs(denom) < 1e-12) {
            continue;
        }
        const double tt =
            static_cast<double>(geom::cross(a - r.origin, e)) / denom;
        const double uu =
            static_cast<double>(geom::cross(a - r.origin, d)) / denom;
        if (tt >= -1e-6 && tt <= limit + 1e-6 && uu >= -1e-6 && uu <= 1.0 + 1e-6) {
            if (!found || tt < best) {
                found = true;
                best = std::max(0.0, tt);
            }
        }
    }
    if (found) {
        t = best;
    }
    return found;
}

} // namespace

// --- vec2 and tolerance helpers ---------------------------------------------

CRPC_TEST(geom_vec2_arithmetic_and_tolerances) {
    const vec2 a(3.0f, 4.0f);
    const vec2 b(1.0f, -2.0f);
    CRPC_CHECK(close(geom::length(a), 5.0f));
    CRPC_CHECK(close(geom::length_squared(a), 25.0f));
    CRPC_CHECK(close(geom::dot(a, b), 3.0f - 8.0f));
    CRPC_CHECK(close(geom::cross(a, b), 3.0f * -2.0f - 4.0f * 1.0f));
    CRPC_CHECK(close_vec(a + b, vec2(4.0f, 2.0f)));
    CRPC_CHECK(close_vec(a - b, vec2(2.0f, 6.0f)));
    CRPC_CHECK(close_vec(a * 2.0f, vec2(6.0f, 8.0f)));
    CRPC_CHECK(close_vec(2.0f * a, vec2(6.0f, 8.0f)));
    CRPC_CHECK(close_vec(-a, vec2(-3.0f, -4.0f)));
    CRPC_CHECK(close_vec(geom::normalized(vec2(0.0f, 2.0f)), vec2(0.0f, 1.0f)));
    CRPC_CHECK(geom::nearly_equal(vec2(1.0f, 2.0f), vec2(1.0f + 1e-6f, 2.0f)));
    CRPC_CHECK_FALSE(geom::nearly_equal(vec2(1.0f, 2.0f), vec2(1.01f, 2.0f)));
    // 90 degrees CCW maps +x onto +y.
    CRPC_CHECK(close_vec(geom::rotated(vec2(1.0f, 0.0f), geom::k_pi / 2.0f),
                         vec2(0.0f, 1.0f)));
    // Angle normalisation keeps the result in [0, 2*pi).
    CRPC_CHECK(close(geom::normalize_angle(-0.5f), geom::k_two_pi - 0.5f));
    CRPC_CHECK(close(geom::normalize_angle(geom::k_two_pi + 0.25f), 0.25f));
    CRPC_CHECK(close(geom::angle_of(vec2(0.0f, -1.0f)), -geom::k_pi / 2.0f));
}

// --- transform2d -------------------------------------------------------------

CRPC_TEST(geom_transform_trs_compose_and_inverse) {
    const geom::transform2d t = geom::transform2d::from_trs(
        vec2(10.0f, -4.0f), geom::k_pi / 2.0f, vec2(2.0f, 3.0f));
    // scale first: (1,1) -> (2,3); rotate 90: (-3,2); translate: (7,-2).
    CRPC_CHECK(close_vec(t.apply(vec2(1.0f, 1.0f)), vec2(7.0f, -2.0f)));
    CRPC_CHECK(close_vec(t.apply_vector(vec2(1.0f, 0.0f)), vec2(0.0f, 2.0f)));

    const geom::transform2d shift = geom::transform2d::translation(vec2(1.0f, 2.0f));
    const geom::transform2d spin = geom::transform2d::rotation(geom::k_pi / 2.0f);
    // compose(shift, spin) applies spin first, then shift.
    const geom::transform2d both = shift.compose(spin);
    CRPC_CHECK(close_vec(both.apply(vec2(1.0f, 0.0f)), vec2(1.0f, 3.0f)));

    const auto inv = t.inverse();
    CRPC_CHECK(inv.ok());
    if (inv.ok()) {
        const vec2 p(3.0f, -7.0f);
        CRPC_CHECK(close_vec(inv.value().apply(t.apply(p)), p));
    }

    const auto singular = geom::transform2d::scaling(vec2(0.0f, 1.0f)).inverse();
    CRPC_CHECK_FALSE(singular.ok());
    if (!singular.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::failed_precondition),
                      static_cast<int>(singular.code()));
    }
}

// --- circle -------------------------------------------------------------------

CRPC_TEST(geom_circle_contains_bounds_and_boundary) {
    const geom::circle c{vec2(2.0f, -3.0f), 5.0f};
    CRPC_CHECK(c.contains(vec2(2.0f, -3.0f)));
    CRPC_CHECK(c.contains(vec2(2.0f, 2.0f)));
    CRPC_CHECK(c.contains(vec2(7.0f, -3.0f))); // exact boundary
    CRPC_CHECK(c.contains(vec2(7.0f + 1e-6f, -3.0f))); // within epsilon
    CRPC_CHECK_FALSE(c.contains(vec2(7.1f, -3.0f)));
    const geom::aabb b = c.bounds();
    CRPC_CHECK(close_vec(b.min, vec2(-3.0f, -8.0f)));
    CRPC_CHECK(close_vec(b.max, vec2(7.0f, 2.0f)));
    CRPC_CHECK(close_vec(c.centroid(), c.center));
    CRPC_CHECK(close_vec(c.edge_anchor(), vec2(7.0f, -3.0f)));
}

CRPC_TEST(geom_circle_zero_radius) {
    const geom::circle c{vec2(4.0f, 5.0f), 0.0f};
    CRPC_CHECK(c.contains(vec2(4.0f, 5.0f)));
    CRPC_CHECK_FALSE(c.contains(vec2(4.001f, 5.0f)));
    const geom::aabb b = c.bounds();
    CRPC_CHECK(close_vec(b.min, c.center));
    CRPC_CHECK(close_vec(b.max, c.center));
}

// --- sector / arc -------------------------------------------------------------

CRPC_TEST(geom_sector_contains_angles_and_zero_sweep) {
    const geom::sector s{vec2(0.0f, 0.0f), 10.0f, -0.5f, 1.0f};
    CRPC_CHECK(s.contains(vec2(9.0f, 0.0f)));        // along bisector
    CRPC_CHECK(s.contains(vec2(0.0f, 0.0f)));        // apex is part of the closed sector
    CRPC_CHECK_FALSE(s.contains(vec2(-9.0f, 0.0f))); // outside the angular range
    CRPC_CHECK_FALSE(s.contains(vec2(9.0f, 9.0f)));  // outside the radius and angle
    CRPC_CHECK(s.contains(vec2(10.0f * std::cos(-0.5f), 10.0f * std::sin(-0.5f))));

    const geom::sector zero{vec2(0.0f, 0.0f), 10.0f, 0.0f, 0.0f};
    CRPC_CHECK_FALSE(zero.contains(vec2(5.0f, 0.0f)));
    CRPC_CHECK_FALSE(zero.contains(vec2(0.0f, 0.0f)));
    CRPC_CHECK_FALSE(zero.bounds().valid() || zero.bounds().contains(vec2(0.0f, 0.0f)));
}

CRPC_TEST(geom_sector_wraps_across_zero) {
    // 350 degrees .. 370 degrees == 350..360 plus 0..10.
    const real start = geom::k_two_pi * 350.0f / 360.0f;
    const real sweep = geom::k_two_pi * 20.0f / 360.0f;
    const geom::sector s{vec2(0.0f, 0.0f), 10.0f, start, sweep};
    CRPC_CHECK(s.contains(vec2(5.0f, 0.0f)));                    // 0 degrees
    CRPC_CHECK(s.contains(vec2(5.0f, 0.5f)));                    // ~5.7 degrees
    CRPC_CHECK(s.contains(vec2(5.0f * std::cos(-0.1f), 5.0f * std::sin(-0.1f)))); // 350.3
    CRPC_CHECK_FALSE(s.contains(vec2(0.0f, 5.0f)));              // 90 degrees
    CRPC_CHECK_FALSE(s.contains(vec2(-5.0f, 0.0f)));             // 180 degrees
    // AABB of the wrapped wedge: the apex at the origin is the leftmost point
    // (min.x == 0); the arc reaches x = 10 at 0 degrees, and y = +-sin(10deg).
    const geom::aabb b = s.bounds();
    CRPC_CHECK(close(b.max.x, 10.0f));
    CRPC_CHECK(close(b.min.x, 0.0f));
    CRPC_CHECK(close(b.max.y, 10.0f * std::sin(sweep / 2.0f)));
    CRPC_CHECK(close(b.min.y, -10.0f * std::sin(sweep / 2.0f)));
}

CRPC_TEST(geom_sector_full_turn_and_radius_zero) {
    const geom::sector full{vec2(1.0f, 1.0f), 3.0f, 1.234f, geom::k_two_pi};
    CRPC_CHECK(full.contains(vec2(4.0f, 1.0f)));
    CRPC_CHECK(full.contains(vec2(-2.0f, 1.0f)));
    CRPC_CHECK_FALSE(full.contains(vec2(4.1f, 1.0f)));

    const geom::sector point{vec2(2.0f, 2.0f), 0.0f, 0.0f, 1.0f};
    CRPC_CHECK(point.contains(vec2(2.0f, 2.0f)));
    CRPC_CHECK_FALSE(point.contains(vec2(2.0f, 2.001f)));
}

CRPC_TEST(geom_sector_centroid_and_edge_anchor) {
    const geom::sector s{vec2(0.0f, 0.0f), 9.0f, -0.25f, 0.5f};
    const real d = (4.0f * s.radius * std::sin(s.sweep / 2.0f)) / (3.0f * s.sweep);
    CRPC_CHECK(close_vec(s.centroid(), vec2(d, 0.0f)));
    CRPC_CHECK(close_vec(s.edge_anchor(), s.center));
}

// --- umbrella -----------------------------------------------------------------

CRPC_TEST(geom_umbrella_canopy_and_handle) {
    // Apex at the origin, opening towards +x with a 90 degree canopy of radius
    // 10 and a 5-long shaft of half-width 1 extending backwards.
    const geom::umbrella u{vec2(0.0f, 0.0f), vec2(1.0f, 0.0f),
                           geom::k_pi / 4.0f, 10.0f, 5.0f, 1.0f};
    CRPC_CHECK(u.contains(vec2(0.0f, 0.0f)));
    CRPC_CHECK(u.contains(vec2(5.0f, 0.0f)));
    CRPC_CHECK(u.contains(vec2(5.0f, 2.0f)));   // inside the +-45 degree canopy
    CRPC_CHECK_FALSE(u.contains(vec2(5.0f, 6.0f))); // ~50 degrees: outside the canopy
    CRPC_CHECK(u.contains(vec2(-3.0f, 0.0f))); // handle
    CRPC_CHECK_FALSE(u.contains(vec2(-3.0f, 2.0f)));
    CRPC_CHECK_FALSE(u.contains(vec2(-6.0f, 0.0f)));
    CRPC_CHECK_FALSE(u.contains(vec2(5.0f, -6.0f)));
    const geom::aabb b = u.bounds();
    CRPC_CHECK(close(b.max.x, 10.0f));
    CRPC_CHECK(close(b.min.x, -5.0f));
    CRPC_CHECK(close(b.max.y, 10.0f * std::sin(geom::k_pi / 4.0f)));
    CRPC_CHECK(close(b.min.y, -10.0f * std::sin(geom::k_pi / 4.0f)));
    CRPC_CHECK(close_vec(u.edge_anchor(), u.apex));
}

CRPC_TEST(geom_umbrella_width_only_cone_and_degenerate) {
    // Without a handle the umbrella is exactly the canopy sector.
    const geom::umbrella cone{vec2(1.0f, 1.0f), vec2(0.0f, 1.0f),
                              0.2f, 4.0f, 0.0f, 0.0f};
    CRPC_CHECK(cone.contains(vec2(1.0f, 4.0f)));
    CRPC_CHECK_FALSE(cone.contains(vec2(3.0f, 4.0f))); // outside +-0.2 rad
    CRPC_CHECK_FALSE(cone.contains(vec2(1.0f, -1.0f)));

    const geom::umbrella dot{vec2(2.0f, 2.0f), vec2(1.0f, 0.0f), 0.0f, 0.0f, 0.0f, 0.0f};
    CRPC_CHECK(dot.contains(vec2(2.0f, 2.0f)));
    CRPC_CHECK_FALSE(dot.contains(vec2(2.0f, 2.01f)));
}

// --- AABB and OBB -------------------------------------------------------------

CRPC_TEST(geom_aabb_contains_and_overlap) {
    const geom::aabb a{vec2(0.0f, 0.0f), vec2(10.0f, 5.0f)};
    CRPC_CHECK(a.contains(vec2(0.0f, 0.0f)));
    CRPC_CHECK(a.contains(vec2(5.0f, 2.5f)));
    CRPC_CHECK(a.contains(vec2(10.0f, 5.0f)));
    CRPC_CHECK_FALSE(a.contains(vec2(10.1f, 5.0f)));
    const geom::aabb touching{vec2(10.0f, 0.0f), vec2(12.0f, 1.0f)};
    CRPC_CHECK(a.overlaps(touching));
    const geom::aabb apart{vec2(10.1f, 0.0f), vec2(12.0f, 1.0f)};
    CRPC_CHECK_FALSE(a.overlaps(apart));
    CRPC_CHECK_FALSE(a.overlaps(geom::aabb::empty()));
    CRPC_CHECK(geom::aabb::unbounded().contains(vec2(1e6f, -1e6f)));
}

CRPC_TEST(geom_obb_contains_rotated_and_bounds) {
    const geom::obb o{vec2(0.0f, 0.0f), vec2(5.0f, 1.0f), geom::k_pi / 2.0f};
    // Rotated 90 degrees: the long axis now runs along +y.
    CRPC_CHECK(o.contains(vec2(0.0f, 4.0f)));
    CRPC_CHECK(o.contains(vec2(1.0f, -4.0f)));
    CRPC_CHECK_FALSE(o.contains(vec2(4.0f, 0.0f)));
    const geom::aabb b = o.bounds();
    CRPC_CHECK(close(b.min.x, -1.0f));
    CRPC_CHECK(close(b.max.x, 1.0f));
    CRPC_CHECK(close(b.min.y, -5.0f));
    CRPC_CHECK(close(b.max.y, 5.0f));
    // Corner and edge points are inside (closed box).
    CRPC_CHECK(o.contains(o.corner(0)));
    CRPC_CHECK(o.contains(obb_point(o, o.half_extents.x, 0.0f)));
    CRPC_CHECK(close_vec(o.centroid(), o.center));
    CRPC_CHECK(close_vec(o.edge_anchor(), o.center + o.axis_x() * o.half_extents.x));

    const geom::obb flat{vec2(1.0f, 2.0f), vec2(0.0f, 0.0f), 0.7f};
    CRPC_CHECK(flat.contains(vec2(1.0f, 2.0f)));
    CRPC_CHECK_FALSE(flat.contains(vec2(1.001f, 2.0f)));
}

// --- polygon point-in (ray casting) ------------------------------------------

CRPC_TEST(geom_polygon_contains_convex_concave_and_boundary) {
    const geom::polygon square = poly_of({
        vec2(0.0f, 0.0f), vec2(10.0f, 0.0f), vec2(10.0f, 10.0f), vec2(0.0f, 10.0f)});
    CRPC_CHECK(square.contains(vec2(5.0f, 5.0f)));
    CRPC_CHECK(square.contains(vec2(0.0f, 0.0f)));   // vertex
    CRPC_CHECK(square.contains(vec2(5.0f, 0.0f)));   // edge
    CRPC_CHECK(square.contains(vec2(10.0f, 10.0f))); // vertex
    CRPC_CHECK_FALSE(square.contains(vec2(11.0f, 5.0f)));
    CRPC_CHECK_FALSE(square.contains(vec2(-0.01f, 5.0f)));
    CRPC_CHECK(close_vec(square.centroid(), vec2(5.0f, 5.0f)));
    CRPC_CHECK(close_vec(square.edge_anchor(), vec2(0.0f, 0.0f)));
    CRPC_CHECK(square.is_convex());

    // L-shape: the notch at the top right must be empty.
    const geom::polygon l_shape = poly_of({
        vec2(0.0f, 0.0f), vec2(4.0f, 0.0f), vec2(4.0f, 2.0f),
        vec2(2.0f, 2.0f), vec2(2.0f, 4.0f), vec2(0.0f, 4.0f)});
    CRPC_CHECK(l_shape.contains(vec2(1.0f, 1.0f)));
    CRPC_CHECK(l_shape.contains(vec2(3.0f, 1.0f)));
    CRPC_CHECK(l_shape.contains(vec2(1.0f, 3.0f)));
    CRPC_CHECK_FALSE(l_shape.contains(vec2(3.0f, 3.0f)));
    CRPC_CHECK_FALSE(l_shape.is_convex());
    CRPC_CHECK(l_shape.contains(vec2(2.0f, 2.0f))); // reflex vertex is in the closed region
}

CRPC_TEST(geom_polygon_degenerate_points) {
    // Duplicate vertices (including a repeated closing vertex) and collinear
    // vertices must not break the crossing test.
    const geom::polygon dup = poly_of({
        vec2(0.0f, 0.0f), vec2(0.0f, 0.0f), vec2(5.0f, 0.0f), vec2(10.0f, 0.0f),
        vec2(10.0f, 10.0f), vec2(0.0f, 10.0f), vec2(0.0f, 10.0f)});
    CRPC_CHECK(dup.contains(vec2(5.0f, 5.0f)));
    CRPC_CHECK(dup.contains(vec2(5.0f, 0.0f)));
    CRPC_CHECK_FALSE(dup.contains(vec2(11.0f, 5.0f)));

    // All vertices collinear: the "polygon" degenerates to its boundary.
    const geom::polygon line_poly = poly_of({
        vec2(0.0f, 0.0f), vec2(5.0f, 0.0f), vec2(10.0f, 0.0f)});
    CRPC_CHECK(line_poly.contains(vec2(5.0f, 0.0f)));
    CRPC_CHECK_FALSE(line_poly.contains(vec2(5.0f, 1.0f)));
    CRPC_CHECK_FALSE(line_poly.contains(vec2(11.0f, 0.0f)));
    CRPC_CHECK(close_vec(line_poly.centroid(), vec2(5.0f, 0.0f)));

    const geom::polygon empty;
    CRPC_CHECK_FALSE(empty.contains(vec2(0.0f, 0.0f)));
    CRPC_CHECK_FALSE(empty.bounds().valid());
}

CRPC_TEST(geom_polygon_matches_winding_number_oracle) {
    // 20 random simple (star-shaped) polygons x 200 random points each.
    std::uint64_t state = 0xC0FFEE1234ULL;
    std::size_t compared = 0;
    std::size_t inside_seen = 0;
    for (int poly_index = 0; poly_index < 20; ++poly_index) {
        const std::size_t n = 5 + static_cast<std::size_t>(xorshift64(state) % 12);
        const geom::polygon p = random_star_polygon(
            state, n, vec2(uniform_real(-2.0f, 2.0f, state),
                           uniform_real(-2.0f, 2.0f, state)),
            uniform_real(2.0f, 5.0f, state), uniform_real(6.0f, 9.0f, state));
        for (int sample = 0; sample < 200; ++sample) {
            const vec2 q(uniform_real(-15.0f, 15.0f, state),
                         uniform_real(-15.0f, 15.0f, state));
            // Skip points that are numerically on the boundary: the oracle and
            // the implementation both use open/closed conventions that are not
            // meaningful within float noise. Boundary behaviour is covered by
            // the explicit cases above.
            if (min_edge_distance(p, q) < 1e-3) {
                continue;
            }
            const int wn = winding_number(p, q);
            const bool expected = wn != 0;
            ++compared;
            if (expected) {
                ++inside_seen;
            }
            if (p.contains(q) != expected) {
                CRPC_CHECK(p.contains(q));
                CRPC_CHECK_FALSE(expected);
            }
        }
    }
    // Guard against an oracle sweep that accidentally tests almost nothing.
    CRPC_CHECK(compared >= 3000);
    CRPC_CHECK(inside_seen >= 100);
}

// --- intersection: circle / AABB / OBB / polygon ------------------------------

CRPC_TEST(geom_circle_circle_intersection) {
    const geom::circle a{vec2(0.0f, 0.0f), 2.0f};
    CRPC_CHECK(geom::intersects(a, geom::circle{vec2(3.0f, 0.0f), 2.0f}));
    CRPC_CHECK(geom::intersects(a, geom::circle{vec2(4.0f, 0.0f), 2.0f})); // touching
    CRPC_CHECK_FALSE(geom::intersects(a, geom::circle{vec2(4.1f, 0.0f), 2.0f}));
    CRPC_CHECK(geom::intersects(a, geom::circle{vec2(0.5f, 0.0f), 0.1f})); // inside
    const geom::circle point{vec2(2.0f, 0.0f), 0.0f};
    CRPC_CHECK(geom::intersects(a, point)); // zero-radius point on boundary
    CRPC_CHECK_FALSE(geom::intersects(a, geom::circle{vec2(2.1f, 0.0f), 0.0f}));
}

CRPC_TEST(geom_circle_polygon_intersection) {
    const geom::polygon square = poly_of({
        vec2(0.0f, 0.0f), vec2(10.0f, 0.0f), vec2(10.0f, 10.0f), vec2(0.0f, 10.0f)});
    // Centre inside.
    CRPC_CHECK(geom::intersects(geom::circle{vec2(5.0f, 5.0f), 1.0f}, square));
    // Circle crossing an edge, centre outside.
    CRPC_CHECK(geom::intersects(geom::circle{vec2(-1.0f, 5.0f), 2.0f}, square));
    // Circle touching the edge.
    CRPC_CHECK(geom::intersects(geom::circle{vec2(-2.0f, 5.0f), 2.0f}, square));
    // Circle outside, missing by a margin.
    CRPC_CHECK_FALSE(geom::intersects(geom::circle{vec2(-2.1f, 5.0f), 2.0f}, square));
    // Large circle containing the whole polygon, centre outside the polygon.
    CRPC_CHECK(geom::intersects(geom::circle{vec2(-100.0f, 5.0f), 105.5f}, square));
    // Large circle containing the polygon but not reaching it.
    CRPC_CHECK_FALSE(geom::intersects(geom::circle{vec2(-100.0f, 5.0f), 89.0f}, square));
    // Circle entirely inside the polygon.
    CRPC_CHECK(geom::intersects(geom::circle{vec2(8.0f, 8.0f), 1.0f}, square));
    // Zero-radius point on the boundary / inside.
    CRPC_CHECK(geom::intersects(geom::circle{vec2(5.0f, 0.0f), 0.0f}, square));
    CRPC_CHECK_FALSE(geom::intersects(geom::circle{vec2(-1.0f, 5.0f), 0.0f}, square));
    // Empty polygon never intersects.
    CRPC_CHECK_FALSE(geom::intersects(geom::circle{vec2(0.0f, 0.0f), 100.0f},
                                      geom::polygon{}));
}

CRPC_TEST(geom_polygon_polygon_intersection_convex_and_concave) {
    const geom::polygon a = poly_of({
        vec2(0.0f, 0.0f), vec2(10.0f, 0.0f), vec2(10.0f, 10.0f), vec2(0.0f, 10.0f)});
    const geom::polygon overlap = poly_of({
        vec2(5.0f, 5.0f), vec2(15.0f, 5.0f), vec2(15.0f, 15.0f), vec2(5.0f, 15.0f)});
    const geom::polygon touching = poly_of({
        vec2(10.0f, 0.0f), vec2(20.0f, 0.0f), vec2(20.0f, 10.0f), vec2(10.0f, 10.0f)});
    const geom::polygon apart = poly_of({
        vec2(10.1f, 0.0f), vec2(20.0f, 0.0f), vec2(20.0f, 10.0f), vec2(10.1f, 10.0f)});
    const geom::polygon inner = poly_of({
        vec2(2.0f, 2.0f), vec2(3.0f, 2.0f), vec2(3.0f, 3.0f), vec2(2.0f, 3.0f)});
    CRPC_CHECK(geom::intersects(a, overlap));
    CRPC_CHECK(geom::intersects(a, touching));
    CRPC_CHECK_FALSE(geom::intersects(a, apart));
    CRPC_CHECK(geom::intersects(a, inner));
    CRPC_CHECK(geom::intersects(inner, a));

    // L-shape notch vs a square that only occupies the notch: no overlap.
    const geom::polygon l_shape = poly_of({
        vec2(0.0f, 0.0f), vec2(4.0f, 0.0f), vec2(4.0f, 2.0f),
        vec2(2.0f, 2.0f), vec2(2.0f, 4.0f), vec2(0.0f, 4.0f)});
    const geom::polygon notch = poly_of({
        vec2(2.5f, 2.5f), vec2(3.5f, 2.5f), vec2(3.5f, 3.5f), vec2(2.5f, 3.5f)});
    CRPC_CHECK_FALSE(geom::intersects(l_shape, notch));
    // A square straddling the inner corner does overlap.
    const geom::polygon straddle = poly_of({
        vec2(1.5f, 1.5f), vec2(2.5f, 1.5f), vec2(2.5f, 2.5f), vec2(1.5f, 2.5f)});
    CRPC_CHECK(geom::intersects(l_shape, straddle));
}

CRPC_TEST(geom_obb_obb_sat_exact_cases) {
    const geom::obb a{vec2(0.0f, 0.0f), vec2(5.0f, 5.0f), 0.0f};
    CRPC_CHECK(geom::intersects(a, geom::obb{vec2(9.0f, 0.0f), vec2(5.0f, 5.0f), 0.0f}));
    CRPC_CHECK(geom::intersects(a, geom::obb{vec2(10.0f, 0.0f), vec2(5.0f, 5.0f), 0.0f}));
    CRPC_CHECK_FALSE(
        geom::intersects(a, geom::obb{vec2(10.1f, 0.0f), vec2(5.0f, 5.0f), 0.0f}));
    // A 45 degree diamond whose tip reaches x = 7.07 overlaps a half-width-5 box.
    CRPC_CHECK(geom::intersects(a, geom::obb{vec2(12.0f, 0.0f), vec2(5.0f, 5.0f),
                                             geom::k_pi / 4.0f}));
    // Same diamond moved just beyond the tip.
    CRPC_CHECK_FALSE(
        geom::intersects(a, geom::obb{vec2(12.2f, 0.0f), vec2(5.0f, 5.0f),
                                     geom::k_pi / 4.0f}));
    // Zero-extent boxes behave as points.
    CRPC_CHECK(geom::intersects(a, geom::obb{vec2(5.0f, 5.0f), vec2(0.0f, 0.0f), 0.0f}));
    CRPC_CHECK_FALSE(
        geom::intersects(a, geom::obb{vec2(5.1f, 5.0f), vec2(0.0f, 0.0f), 0.0f}));
    // Identical boxes overlap.
    CRPC_CHECK(geom::intersects(a, a));
}

CRPC_TEST(geom_obb_obb_matches_dense_sampling) {
    // The oracle samples one OBB's interior on a 65x65 grid and asks an
    // independent point-in-OBB routine whether a sample lands in the other.
    // A sampled overlap is a guaranteed true overlap, so SAT must agree. SAT
    // can legitimately report an overlap that the grid misses when the overlap
    // is a thin sliver; those are counted and bounded (documented tolerance).
    std::uint64_t state = 0x5EED1234ULL;
    constexpr int k_steps = 65;
    std::size_t sat_true = 0;
    std::size_t sat_without_sample = 0;
    std::size_t sampled_overlaps = 0;
    for (int pair = 0; pair < 160; ++pair) {
        const geom::obb a{vec2(uniform_real(-4.0f, 4.0f, state),
                               uniform_real(-4.0f, 4.0f, state)),
                          vec2(uniform_real(0.4f, 3.0f, state),
                               uniform_real(0.4f, 3.0f, state)),
                          uniform_real(-geom::k_pi, geom::k_pi, state)};
        vec2 bc;
        if ((pair % 2) == 0) {
            bc = a.center + vec2(uniform_real(-2.0f, 2.0f, state),
                                 uniform_real(-2.0f, 2.0f, state));
        } else {
            bc = vec2(uniform_real(-9.0f, 9.0f, state),
                      uniform_real(-9.0f, 9.0f, state));
        }
        const geom::obb b{bc,
                          vec2(uniform_real(0.4f, 3.0f, state),
                               uniform_real(0.4f, 3.0f, state)),
                          uniform_real(-geom::k_pi, geom::k_pi, state)};

        bool sampled = false;
        for (int i = 0; i < k_steps && !sampled; ++i) {
            const real u = -a.half_extents.x +
                           (2.0f * a.half_extents.x) *
                               (static_cast<real>(i) / static_cast<real>(k_steps - 1));
            for (int j = 0; j < k_steps; ++j) {
                const real v =
                    -a.half_extents.y +
                    (2.0f * a.half_extents.y) *
                        (static_cast<real>(j) / static_cast<real>(k_steps - 1));
                if (b.contains(obb_point(a, u, v))) {
                    sampled = true;
                    break;
                }
            }
        }
        const bool sat = geom::intersects(a, b);
        if (sampled) {
            ++sampled_overlaps;
            // No false negatives: a real sampled overlap must be detected.
            CRPC_CHECK(sat);
        }
        if (sat) {
            ++sat_true;
            if (!sampled) {
                ++sat_without_sample;
            }
        } else {
            // No false positives relative to the sampling oracle.
            CRPC_CHECK_FALSE(sampled);
        }
    }
    CRPC_CHECK(sampled_overlaps >= 20);
    CRPC_CHECK(sat_true >= 20);
    // Thin slivers can fall between grid lines, but only a small fraction.
    CRPC_CHECK(sat_without_sample * 10 <= sat_true);
}

CRPC_TEST(geom_sector_and_umbrella_circle_intersection) {
    const geom::sector s{vec2(0.0f, 0.0f), 10.0f, -0.5f, 1.0f};
    CRPC_CHECK(geom::intersects(s, geom::circle{vec2(5.0f, 0.0f), 1.0f})); // inside
    CRPC_CHECK(geom::intersects(s, geom::circle{vec2(0.0f, 0.0f), 1.0f})); // apex
    CRPC_CHECK(geom::intersects(s, geom::circle{vec2(10.0f, 0.0f), 1.0f})); // arc
    CRPC_CHECK_FALSE(geom::intersects(s, geom::circle{vec2(-5.0f, 0.0f), 1.0f}));
    // On the +x axis, 1 unit beyond the arc: radius 2 reaches the arc, 0.5 does not.
    CRPC_CHECK(geom::intersects(s, geom::circle{vec2(11.0f, 0.0f), 2.0f}));
    CRPC_CHECK_FALSE(geom::intersects(s, geom::circle{vec2(11.0f, 0.0f), 0.5f}));

    const geom::umbrella u{vec2(0.0f, 0.0f), vec2(1.0f, 0.0f),
                           geom::k_pi / 4.0f, 10.0f, 5.0f, 1.0f};
    CRPC_CHECK(geom::intersects(u, geom::circle{vec2(5.0f, 0.0f), 1.0f}));
    CRPC_CHECK(geom::intersects(u, geom::circle{vec2(-3.0f, 0.0f), 0.5f})); // shaft
    CRPC_CHECK_FALSE(geom::intersects(u, geom::circle{vec2(-8.0f, 0.0f), 1.0f}));
    CRPC_CHECK_FALSE(geom::intersects(u, geom::circle{vec2(5.0f, 8.0f), 1.0f}));
}

// --- ray / line / segment -----------------------------------------------------

CRPC_TEST(geom_ray_contains_and_bounds) {
    const geom::ray r{vec2(1.0f, 2.0f), vec2(2.0f, 0.0f), 10.0f};
    CRPC_CHECK(r.contains(vec2(1.0f, 2.0f)));
    CRPC_CHECK(r.contains(vec2(6.0f, 2.0f)));
    CRPC_CHECK(r.contains(vec2(11.0f, 2.0f))); // far end, direction is scaled
    CRPC_CHECK_FALSE(r.contains(vec2(11.1f, 2.0f)));
    CRPC_CHECK_FALSE(r.contains(vec2(0.9f, 2.0f))); // behind the origin
    CRPC_CHECK_FALSE(r.contains(vec2(6.0f, 2.1f)));
    const geom::aabb b = r.bounds();
    CRPC_CHECK(close_vec(b.min, vec2(1.0f, 2.0f)));
    CRPC_CHECK(close_vec(b.max, vec2(11.0f, 2.0f)));
    CRPC_CHECK(close_vec(r.edge_anchor(), r.origin));
    CRPC_CHECK(close_vec(r.centroid(), vec2(6.0f, 2.0f)));

    const geom::ray zero{vec2(3.0f, 3.0f), vec2(1.0f, 0.0f), 0.0f};
    CRPC_CHECK(zero.contains(vec2(3.0f, 3.0f)));
    CRPC_CHECK_FALSE(zero.contains(vec2(3.1f, 3.0f)));
}

CRPC_TEST(geom_ray_circle_hit_against_analytic) {
    std::uint64_t state = 0xA11CEULL;
    for (int i = 0; i < 400; ++i) {
        const geom::circle c{vec2(uniform_real(-5.0f, 5.0f, state),
                                  uniform_real(-5.0f, 5.0f, state)),
                             uniform_real(0.2f, 4.0f, state)};
        const geom::ray r{vec2(uniform_real(-10.0f, 10.0f, state),
                               uniform_real(-10.0f, 10.0f, state)),
                          vec2(uniform_real(-1.0f, 1.0f, state),
                               uniform_real(-1.0f, 1.0f, state)),
                          uniform_real(0.5f, 20.0f, state)};
        double expected = 0.0;
        const bool reference = analytic_ray_circle(r, c, expected);
        const auto got = geom::ray_circle_hit(r, c);
        CRPC_CHECK_EQ(reference, got.ok());
        if (reference && got.ok()) {
            CRPC_CHECK(close_double(static_cast<double>(got.value()), expected, 2e-3));
        }
        if (!got.ok()) {
            CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                          static_cast<int>(got.code()));
        }
    }

    // Tangent and max-length boundaries.
    const geom::circle unit{vec2(0.0f, 0.0f), 5.0f};
    const auto tangent = geom::ray_circle_hit(
        geom::ray{vec2(-10.0f, 5.0f), vec2(1.0f, 0.0f), 30.0f}, unit);
    CRPC_CHECK(tangent.ok());
    if (tangent.ok()) {
        CRPC_CHECK(close(tangent.value(), 10.0f));
    }
    const auto short_ray = geom::ray_circle_hit(
        geom::ray{vec2(-10.0f, 0.0f), vec2(1.0f, 0.0f), 4.0f}, unit);
    CRPC_CHECK_FALSE(short_ray.ok());
    const auto from_inside = geom::ray_circle_hit(
        geom::ray{vec2(0.0f, 0.0f), vec2(1.0f, 0.0f), 30.0f}, unit);
    CRPC_CHECK(from_inside.ok());
    if (from_inside.ok()) {
        CRPC_CHECK(close(from_inside.value(), 0.0f));
    }
    const auto degenerate = geom::ray_circle_hit(
        geom::ray{vec2(0.0f, 0.0f), vec2(0.0f, 0.0f), 1.0f}, unit);
    CRPC_CHECK_FALSE(degenerate.ok());
    if (!degenerate.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(degenerate.code()));
    }
}

CRPC_TEST(geom_ray_aabb_and_obb_hit_against_analytic) {
    const geom::aabb box{vec2(0.0f, 0.0f), vec2(10.0f, 10.0f)};
    const auto hit = geom::ray_aabb_hit(
        geom::ray{vec2(-10.0f, 5.0f), vec2(1.0f, 0.0f), 30.0f}, box);
    CRPC_CHECK(hit.ok());
    if (hit.ok()) {
        CRPC_CHECK(close(hit.value(), 10.0f));
    }
    const auto inside = geom::ray_aabb_hit(
        geom::ray{vec2(5.0f, 5.0f), vec2(1.0f, 0.0f), 30.0f}, box);
    CRPC_CHECK(inside.ok());
    if (inside.ok()) {
        CRPC_CHECK(close(inside.value(), 0.0f));
    }
    const auto miss = geom::ray_aabb_hit(
        geom::ray{vec2(-10.0f, 11.0f), vec2(1.0f, 0.0f), 30.0f}, box);
    CRPC_CHECK_FALSE(miss.ok());

    std::uint64_t state = 0xB0BULL;
    for (int i = 0; i < 300; ++i) {
        const geom::obb o{vec2(uniform_real(-3.0f, 3.0f, state),
                               uniform_real(-3.0f, 3.0f, state)),
                          vec2(uniform_real(0.3f, 3.0f, state),
                               uniform_real(0.3f, 3.0f, state)),
                          uniform_real(-geom::k_pi, geom::k_pi, state)};
        const geom::ray r{vec2(uniform_real(-10.0f, 10.0f, state),
                               uniform_real(-10.0f, 10.0f, state)),
                          vec2(uniform_real(-1.0f, 1.0f, state),
                               uniform_real(-1.0f, 1.0f, state)),
                          uniform_real(0.5f, 20.0f, state)};
        double expected = 0.0;
        const bool reference = analytic_ray_obb(r, o, expected);
        const auto got = geom::ray_obb_hit(r, o);
        CRPC_CHECK_EQ(reference, got.ok());
        if (reference && got.ok()) {
            CRPC_CHECK(close_double(static_cast<double>(got.value()), expected, 2e-3));
        }
    }
}

CRPC_TEST(geom_ray_polygon_hit_against_analytic) {
    const geom::polygon square = poly_of({
        vec2(0.0f, 0.0f), vec2(10.0f, 0.0f), vec2(10.0f, 10.0f), vec2(0.0f, 10.0f)});
    const auto hit = geom::ray_polygon_hit(
        geom::ray{vec2(-5.0f, 5.0f), vec2(1.0f, 0.0f), 30.0f}, square);
    CRPC_CHECK(hit.ok());
    if (hit.ok()) {
        CRPC_CHECK(close(hit.value(), 5.0f));
    }
    const auto origin_inside = geom::ray_polygon_hit(
        geom::ray{vec2(5.0f, 5.0f), vec2(-1.0f, 0.0f), 30.0f}, square);
    CRPC_CHECK(origin_inside.ok());
    if (origin_inside.ok()) {
        CRPC_CHECK(close(origin_inside.value(), 0.0f));
    }
    // A ray through the L-shape notch must pass over the empty corner and hit
    // the far arm, not the nearer bounding box.
    const geom::polygon l_shape = poly_of({
        vec2(0.0f, 0.0f), vec2(4.0f, 0.0f), vec2(4.0f, 2.0f),
        vec2(2.0f, 2.0f), vec2(2.0f, 4.0f), vec2(0.0f, 4.0f)});
    // x = 3 is in the notch above y = 2: the AABB would report a hit at y = 4,
    // the exact polygon at y = 2 (t = 3 for a ray starting at y = 5).
    const auto l_hit = geom::ray_polygon_hit(
        geom::ray{vec2(3.0f, 5.0f), vec2(0.0f, -1.0f), 20.0f}, l_shape);
    CRPC_CHECK(l_hit.ok());
    if (l_hit.ok()) {
        CRPC_CHECK(close(l_hit.value(), 3.0f));
    }

    std::uint64_t state = 0xF00DULL;
    for (int i = 0; i < 200; ++i) {
        const geom::polygon p = random_star_polygon(
            state, 6 + static_cast<std::size_t>(xorshift64(state) % 8),
            vec2(uniform_real(-2.0f, 2.0f, state), uniform_real(-2.0f, 2.0f, state)),
            2.0f, 7.0f);
        const geom::ray r{vec2(uniform_real(-12.0f, 12.0f, state),
                               uniform_real(-12.0f, 12.0f, state)),
                          vec2(uniform_real(-1.0f, 1.0f, state),
                               uniform_real(-1.0f, 1.0f, state)),
                          uniform_real(1.0f, 30.0f, state)};
        double expected = 0.0;
        const bool reference = analytic_ray_polygon(r, p, expected);
        const auto got = geom::ray_polygon_hit(r, p);
        CRPC_CHECK_EQ(reference, got.ok());
        if (reference && got.ok()) {
            CRPC_CHECK(close_double(static_cast<double>(got.value()), expected, 3e-3));
        }
    }
}

CRPC_TEST(geom_line_and_segment_semantics_differ) {
    const geom::circle unit{vec2(0.0f, 0.0f), 1.0f};
    // Infinite line through (10, 0) towards +x reaches the circle behind it.
    const geom::line infinite{vec2(10.0f, 0.0f), vec2(1.0f, 0.0f)};
    CRPC_CHECK(infinite.contains(vec2(-100.0f, 0.0f)));
    const auto line_hit = geom::line_circle_hit(infinite, unit);
    CRPC_CHECK(line_hit.ok());
    if (line_hit.ok()) {
        CRPC_CHECK(std::fabs(line_hit.value()) >= 0.0f);
        CRPC_CHECK(close(std::fabs(line_hit.value()), 9.0f));
    }
    // The same line restricted to a segment pointing away never hits.
    const geom::segment away{vec2(5.0f, 0.0f), vec2(20.0f, 0.0f)};
    CRPC_CHECK(away.contains(vec2(10.0f, 0.0f)));
    CRPC_CHECK_FALSE(away.contains(vec2(0.0f, 0.0f)));
    const auto segment_miss = geom::segment_circle_hit(away, unit);
    CRPC_CHECK_FALSE(segment_miss.ok());

    // A segment that crosses the circle hits at the entry fraction.
    const geom::segment crossing{vec2(-5.0f, 0.0f), vec2(5.0f, 0.0f)};
    const auto segment_hit = geom::segment_circle_hit(crossing, unit);
    CRPC_CHECK(segment_hit.ok());
    if (segment_hit.ok()) {
        CRPC_CHECK(close(segment_hit.value(), 0.4f));
    }
    // Endpoint exactly on the circle counts as a hit at t = 1.
    const geom::segment endpoint{vec2(2.0f, 0.0f), vec2(1.0f, 0.0f)};
    const auto endpoint_hit = geom::segment_circle_hit(endpoint, unit);
    CRPC_CHECK(endpoint_hit.ok());
    if (endpoint_hit.ok()) {
        CRPC_CHECK(close(endpoint_hit.value(), 1.0f));
    }
    // Origin inside: hit at t = 0.
    const geom::segment from_inside{vec2(0.0f, 0.0f), vec2(5.0f, 0.0f)};
    const auto inside_hit = geom::segment_circle_hit(from_inside, unit);
    CRPC_CHECK(inside_hit.ok());
    if (inside_hit.ok()) {
        CRPC_CHECK(close(inside_hit.value(), 0.0f));
    }
    // Degenerate segment is a point.
    const geom::segment point{vec2(0.5f, 0.0f), vec2(0.5f, 0.0f)};
    const auto point_hit = geom::segment_circle_hit(point, unit);
    CRPC_CHECK(point_hit.ok());
    if (point_hit.ok()) {
        CRPC_CHECK(close(point_hit.value(), 0.0f));
    }
}

CRPC_TEST(geom_line_and_segment_vs_rect_and_polygon) {
    const geom::aabb box{vec2(0.0f, 0.0f), vec2(10.0f, 10.0f)};
    const geom::line line{vec2(-5.0f, 5.0f), vec2(1.0f, 0.0f)};
    const auto line_box = geom::line_aabb_hit(line, box);
    CRPC_CHECK(line_box.ok());
    if (line_box.ok()) {
        CRPC_CHECK(close(line_box.value(), 5.0f));
    }
    const geom::line behind{vec2(20.0f, 5.0f), vec2(1.0f, 0.0f)};
    const auto behind_box = geom::line_aabb_hit(behind, box);
    CRPC_CHECK(behind_box.ok());
    if (behind_box.ok()) {
        CRPC_CHECK(close(behind_box.value(), -10.0f));
    }
    const geom::segment seg{vec2(-5.0f, 5.0f), vec2(5.0f, 5.0f)};
    const auto seg_box = geom::segment_aabb_hit(seg, box);
    CRPC_CHECK(seg_box.ok());
    if (seg_box.ok()) {
        CRPC_CHECK(close(seg_box.value(), 0.5f));
    }
    const geom::segment seg_short{vec2(-5.0f, 5.0f), vec2(-1.0f, 5.0f)};
    CRPC_CHECK_FALSE(geom::segment_aabb_hit(seg_short, box).ok());

    const geom::polygon square = poly_of({
        vec2(0.0f, 0.0f), vec2(10.0f, 0.0f), vec2(10.0f, 10.0f), vec2(0.0f, 10.0f)});
    const auto line_poly = geom::line_polygon_hit(line, square);
    CRPC_CHECK(line_poly.ok());
    if (line_poly.ok()) {
        CRPC_CHECK(close(line_poly.value(), 5.0f));
    }
    const auto seg_poly = geom::segment_polygon_hit(seg, square);
    CRPC_CHECK(seg_poly.ok());
    if (seg_poly.ok()) {
        CRPC_CHECK(close(seg_poly.value(), 0.5f));
    }

    // OBB variants use the same local-frame slab.
    const geom::obb obb{vec2(0.0f, 0.0f), vec2(5.0f, 1.0f), geom::k_pi / 2.0f};
    const auto line_obb = geom::line_obb_hit(
        geom::line{vec2(-10.0f, 0.0f), vec2(1.0f, 0.0f)}, obb);
    CRPC_CHECK(line_obb.ok());
    if (line_obb.ok()) {
        CRPC_CHECK(close(line_obb.value(), 9.0f));
    }
    const auto seg_obb = geom::segment_obb_hit(
        geom::segment{vec2(-10.0f, 0.0f), vec2(10.0f, 0.0f)}, obb);
    CRPC_CHECK(seg_obb.ok());
    if (seg_obb.ok()) {
        CRPC_CHECK(close(seg_obb.value(), 0.45f));
    }
}

// --- anchoring: centre versus edge -------------------------------------------

CRPC_TEST(geom_anchor_mode_changes_the_hit_result) {
    const vec2 where(10.0f, -2.0f);

    // Circle: +1 along x is inside a centred radius-5 circle but outside when
    // the player sits on the circumference.
    const geom::circle circle{vec2(0.0f, 0.0f), 5.0f};
    const geom::hitbox circle_center(circle, geom::anchor_mode::center, where);
    const geom::hitbox circle_edge(circle, geom::anchor_mode::edge, where);
    CRPC_CHECK(circle_center.contains(where + vec2(1.0f, 0.0f)));
    CRPC_CHECK_FALSE(circle_edge.contains(where + vec2(1.0f, 0.0f)));
    CRPC_CHECK(close_vec(circle_center.local_anchor(), circle.centroid()));
    CRPC_CHECK(close_vec(circle_edge.local_anchor(), circle.edge_anchor()));
    CRPC_CHECK(close_vec(circle_center.world_anchor(), where));
    CRPC_CHECK(close_vec(circle_edge.world_anchor(), where));

    // Sector: a point just behind the apex is inside the sector when its
    // centroid is placed at the player, and outside when the apex is.
    const geom::sector sector{vec2(0.0f, 0.0f), 10.0f, -0.5f, 1.0f};
    const geom::hitbox sector_center(sector, geom::anchor_mode::center, where);
    const geom::hitbox sector_edge(sector, geom::anchor_mode::edge, where);
    CRPC_CHECK(sector_center.contains(where + vec2(-0.1f, 0.0f)));
    CRPC_CHECK_FALSE(sector_edge.contains(where + vec2(-0.1f, 0.0f)));

    // Umbrella: same apex-versus-centroid distinction, with no handle so the
    // region behind the apex is empty.
    const geom::umbrella umbrella{vec2(0.0f, 0.0f), vec2(1.0f, 0.0f),
                                  geom::k_pi / 4.0f, 10.0f, 0.0f, 0.0f};
    const geom::hitbox umbrella_center(umbrella, geom::anchor_mode::center, where);
    const geom::hitbox umbrella_edge(umbrella, geom::anchor_mode::edge, where);
    CRPC_CHECK(umbrella_center.contains(where + vec2(-0.1f, 0.0f)));
    CRPC_CHECK_FALSE(umbrella_edge.contains(where + vec2(-0.1f, 0.0f)));

    // OBB: +1 along x is inside the centred box but outside once the right
    // edge midpoint is pinned to the player.
    const geom::obb box{vec2(0.0f, 0.0f), vec2(5.0f, 3.0f), 0.0f};
    const geom::hitbox box_center(box, geom::anchor_mode::center, where);
    const geom::hitbox box_edge(box, geom::anchor_mode::edge, where);
    CRPC_CHECK(box_center.contains(where + vec2(1.0f, 0.0f)));
    CRPC_CHECK_FALSE(box_edge.contains(where + vec2(1.0f, 0.0f)));

    // AABB shape (the unrotated box variant).
    const geom::aabb rect{vec2(-5.0f, -3.0f), vec2(5.0f, 3.0f)};
    const geom::hitbox rect_center(rect, geom::anchor_mode::center, where);
    const geom::hitbox rect_edge(rect, geom::anchor_mode::edge, where);
    CRPC_CHECK(rect_center.contains(where + vec2(1.0f, 0.0f)));
    CRPC_CHECK_FALSE(rect_edge.contains(where + vec2(1.0f, 0.0f)));

    // Ray: behind the origin is on the ray when the midpoint is at the player,
    // but not when the origin is.
    const geom::ray ray{vec2(0.0f, 0.0f), vec2(1.0f, 0.0f), 10.0f};
    const geom::hitbox ray_center(ray, geom::anchor_mode::center, where);
    const geom::hitbox ray_edge(ray, geom::anchor_mode::edge, where);
    CRPC_CHECK(ray_center.contains(where + vec2(-1.0f, 0.0f)));
    CRPC_CHECK_FALSE(ray_edge.contains(where + vec2(-1.0f, 0.0f)));

    // Segment: same.
    const geom::segment segment{vec2(0.0f, 0.0f), vec2(10.0f, 0.0f)};
    const geom::hitbox segment_center(segment, geom::anchor_mode::center, where);
    const geom::hitbox segment_edge(segment, geom::anchor_mode::edge, where);
    CRPC_CHECK(segment_center.contains(where + vec2(-1.0f, 0.0f)));
    CRPC_CHECK_FALSE(segment_edge.contains(where + vec2(-1.0f, 0.0f)));

    // Polygon: centroid vs first vertex.
    const geom::polygon square = poly_of({
        vec2(0.0f, 0.0f), vec2(10.0f, 0.0f), vec2(10.0f, 10.0f), vec2(0.0f, 10.0f)});
    const geom::hitbox polygon_center(square, geom::anchor_mode::center, where);
    const geom::hitbox polygon_edge(square, geom::anchor_mode::edge, where);
    CRPC_CHECK(polygon_center.contains(where + vec2(-1.0f, 0.0f)));
    CRPC_CHECK_FALSE(polygon_edge.contains(where + vec2(-1.0f, 0.0f)));

    // An infinite line has no boundary; both modes agree by definition.
    const geom::line line{vec2(0.0f, 0.0f), vec2(1.0f, 0.0f)};
    const geom::hitbox line_center(line, geom::anchor_mode::center, where);
    const geom::hitbox line_edge(line, geom::anchor_mode::edge, where);
    CRPC_CHECK(line_center.contains(where + vec2(-1.0f, 0.0f)));
    CRPC_CHECK(line_edge.contains(where + vec2(-1.0f, 0.0f)));
    CRPC_CHECK_FALSE(line_center.contains(where + vec2(-1.0f, 1.0f)));
}

CRPC_TEST(geom_hitbox_rotation_scale_broad_phase_and_bounds) {
    // Rotate a rectangle 90 degrees about the player at the centre: the long
    // side now runs along y.
    const geom::obb box{vec2(0.0f, 0.0f), vec2(5.0f, 1.0f), 0.0f};
    const geom::hitbox rotated(box, geom::anchor_mode::center, vec2(10.0f, 10.0f),
                               geom::k_pi / 2.0f, 1.0f);
    CRPC_CHECK(rotated.contains(vec2(10.0f, 14.0f)));
    CRPC_CHECK_FALSE(rotated.contains(vec2(14.0f, 10.0f)));
    CRPC_CHECK(rotated.broad_phase(vec2(10.0f, 14.0f)));
    CRPC_CHECK_FALSE(rotated.broad_phase(vec2(100.0f, 100.0f)));

    // Scale 2 doubles the reach.
    const geom::hitbox scaled(box, geom::anchor_mode::center, vec2(0.0f, 0.0f),
                              0.0f, 2.0f);
    CRPC_CHECK(scaled.contains(vec2(9.0f, 0.0f)));
    CRPC_CHECK_FALSE(scaled.contains(vec2(11.0f, 0.0f)));
    CRPC_CHECK(scaled.broad_phase(vec2(9.0f, 0.0f)));
    CRPC_CHECK_FALSE(scaled.broad_phase(vec2(11.0f, 0.0f)));

    // World bounds are conservative and must contain every shape point.
    const geom::aabb wb = rotated.world_bounds();
    CRPC_CHECK(wb.valid());
    CRPC_CHECK(wb.contains(vec2(10.0f, 14.0f)));
    CRPC_CHECK(wb.contains(vec2(10.0f, 6.0f)));
    CRPC_CHECK_FALSE(wb.contains(vec2(14.0f, 10.0f)));

    // broad_phase vs an explicit AABB.
    CRPC_CHECK(rotated.broad_phase(geom::aabb{vec2(9.0f, 13.0f), vec2(11.0f, 15.0f)}));
    CRPC_CHECK_FALSE(
        rotated.broad_phase(geom::aabb{vec2(50.0f, 50.0f), vec2(60.0f, 60.0f)}));

    // A non-positive scale makes the hitbox empty instead of dividing by zero.
    const geom::hitbox empty(box, geom::anchor_mode::center, vec2(0.0f, 0.0f),
                             0.0f, 0.0f);
    CRPC_CHECK_FALSE(empty.contains(vec2(0.0f, 0.0f)));
    CRPC_CHECK_FALSE(empty.world_bounds().valid());
    CRPC_CHECK_FALSE(empty.broad_phase(vec2(0.0f, 0.0f)));
}

// --- performance --------------------------------------------------------------

CRPC_TEST(geom_performance_point_in_polygon_and_circle_polygon) {
    // A 32-gon with unit-ish size; the polygon is convex but the ray-casting
    // routine is the same O(n) path used for concave input.
    std::vector<vec2> vertices;
    vertices.reserve(32);
    for (int i = 0; i < 32; ++i) {
        const real a = geom::k_two_pi * static_cast<real>(i) / 32.0f;
        vertices.push_back(vec2(100.0f * std::cos(a) + 3.0f,
                                100.0f * std::sin(a) - 2.0f));
    }
    const geom::polygon poly{vertices};
    std::uint64_t state = 0x123456789ULL;

    volatile int sink = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 10000; ++i) {
        const vec2 p(uniform_real(-120.0f, 130.0f, state),
                     uniform_real(-120.0f, 130.0f, state));
        sink += poly.contains(p) ? 1 : 0;
    }
    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < 10000; ++i) {
        const vec2 c(uniform_real(-120.0f, 130.0f, state),
                     uniform_real(-120.0f, 130.0f, state));
        sink += geom::intersects(geom::circle{c, 20.0f}, poly) ? 1 : 0;
    }
    const auto t2 = std::chrono::steady_clock::now();
    const double point_ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double circle_ms =
        std::chrono::duration<double, std::milli>(t2 - t1).count();
    std::printf(
        "geom perf: 10000 point-in-polygon(32 verts) = %.3f ms; "
        "10000 circle-polygon = %.3f ms (sink=%d)\n",
        point_ms, circle_ms, static_cast<int>(sink));
    std::fflush(stdout);
    // The 2 s wall-clock bounds were degeneration gates with no reachable
    // V_bad; they live in bench/bench_algorithms.cpp now (plan.md D3).
    //
    // Removing them left this case with no assertion at all, which is worse than
    // having no case: a test that cannot fail still shows up as a green line in
    // the report. What is pinned here is chosen for distance from the boundary, not
    // for convenience. The 32-gon is centred on (3, -2) with radius 100, so its
    // inradius is about 99.5: a point 90 out is inside with ~9.5 units of margin and
    // one 110 out is outside with ~10, which is what makes a radius that is halved
    // or a centre that has drifted show up. A point on a vertex would have passed
    // whatever the radius was, because the boundary check returns true there.
    const vec2 centre(3.0f, -2.0f);
    CRPC_CHECK(poly.contains(centre));
    CRPC_CHECK(poly.contains(vec2(93.0f, -2.0f)));        // 90 from the centre
    CRPC_CHECK_FALSE(poly.contains(vec2(113.0f, -2.0f))); // 110 from the centre
    // A circle centred 110 out with radius 20 reaches back to 90, so it must share
    // points with the polygon; the same circle at radius 5 stays clear.
    CRPC_CHECK(geom::intersects(geom::circle{vec2(113.0f, -2.0f), 20.0f}, poly));
    CRPC_CHECK_FALSE(geom::intersects(geom::circle{vec2(113.0f, -2.0f), 5.0f}, poly));
    CRPC_CHECK_FALSE(geom::intersects(geom::circle{vec2(1000.0f, 1000.0f), 5.0f}, poly));
}

CRPC_TEST_MAIN()