// CRPC -- 3D geometry, 360-degree headings, shotgun spread and the
// instant/projectile (speed + distance + leading) semantics.
//
// The cross-checks are the same style as test_geom.cpp: analytic solutions for
// ray queries, dense sampling for the 15-axis OBB test, and explicit
// before/after scenarios for leading and lifetime.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <utility>
#include <vector>

#include <crpc/game/geom/attack.hpp>
#include <crpc/game/geom/collide3d.hpp>
#include <crpc/game/geom/hitbox.hpp>
#include <crpc/game/geom/shapes3d.hpp>
#include <crpc/game/geom/vec.hpp>

#include "test_framework.hpp"

namespace {

namespace geom = crpc::game::geom;
using geom::real;
using geom::vec2;
using geom::vec3;

constexpr real k_close = 1e-4f;

bool close(real a, real b, real eps = k_close) {
    return std::fabs(a - b) <= eps;
}

bool close_vec3(vec3 a, vec3 b, real eps = k_close) {
    return close(a.x, b.x, eps) && close(a.y, b.y, eps) &&
           close(a.z, b.z, eps);
}



std::uint64_t xorshift64(std::uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

double unit_random(std::uint64_t& state) {
    return static_cast<double>(xorshift64(state) >> 11) / 9007199254740992.0;
}

real uniform_real(real lo, real hi, std::uint64_t& state) {
    return static_cast<real>(static_cast<double>(lo) +
                             (static_cast<double>(hi) - static_cast<double>(lo)) *
                                 unit_random(state));
}

vec3 random_unit_vec3(std::uint64_t& state) {
    while (true) {
        const real x = uniform_real(-1.0f, 1.0f, state);
        const real y = uniform_real(-1.0f, 1.0f, state);
        const real z = uniform_real(-1.0f, 1.0f, state);
        const real len2 = x * x + y * y + z * z;
        if (len2 > 0.04f && len2 <= 1.0f) {
            const real inv = 1.0f / std::sqrt(len2);
            return vec3(x * inv, y * inv, z * inv);
        }
    }
}

geom::convex_polyhedron box_polyhedron(vec3 lo, vec3 hi) {
    geom::aabb3 box;
    box.min = lo;
    box.max = hi;
    return geom::convex_polyhedron::from_box(box);
}

bool analytic_ray3_sphere(const geom::ray3& r, const geom::sphere& s,
                          double& t) {
    const vec3 u = r.unit_direction();
    const vec3 oc = r.origin - s.center;
    const double b = static_cast<double>(geom::dot(oc, u));
    const double c = static_cast<double>(geom::dot(oc, oc)) -
                     static_cast<double>(s.radius) *
                         static_cast<double>(s.radius);
    if (c <= 0.0) {
        t = 0.0;
        return true;
    }
    const double disc = b * b - c;
    if (disc < 0.0) {
        return false;
    }
    const double root = std::sqrt(std::max(0.0, disc));
    const double limit = static_cast<double>(r.max_length);
    const double t0 = -b - root;
    const double t1 = -b + root;
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

bool analytic_ray3_obb(const geom::ray3& r, const geom::obb3& o, double& t) {
    const vec3 u = r.unit_direction();
    const vec3 rel = r.origin - o.center;
    const double ox = static_cast<double>(geom::dot(rel, o.axis_x));
    const double oy = static_cast<double>(geom::dot(rel, o.axis_y));
    const double oz = static_cast<double>(geom::dot(rel, o.axis_z));
    const double dx = static_cast<double>(geom::dot(u, o.axis_x));
    const double dy = static_cast<double>(geom::dot(u, o.axis_y));
    const double dz = static_cast<double>(geom::dot(u, o.axis_z));
    const double hx = std::fabs(static_cast<double>(o.half_extents.x));
    const double hy = std::fabs(static_cast<double>(o.half_extents.y));
    const double hz = std::fabs(static_cast<double>(o.half_extents.z));
    const double origins[3] = {ox, oy, oz};
    const double dirs[3] = {dx, dy, dz};
    const double halves[3] = {hx, hy, hz};
    double tmin = -std::numeric_limits<double>::infinity();
    double tmax = std::numeric_limits<double>::infinity();
    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(dirs[axis]) < 1e-12) {
            if (origins[axis] < -halves[axis] ||
                origins[axis] > halves[axis]) {
                return false;
            }
            continue;
        }
        double t1 = (-halves[axis] - origins[axis]) / dirs[axis];
        double t2 = (halves[axis] - origins[axis]) / dirs[axis];
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

bool analytic_ray3_aabb(const geom::ray3& r, const geom::aabb3& box,
                        double& t) {
    const vec3 u = r.unit_direction();
    const double o[3] = {static_cast<double>(r.origin.x),
                         static_cast<double>(r.origin.y),
                         static_cast<double>(r.origin.z)};
    const double d[3] = {static_cast<double>(u.x), static_cast<double>(u.y),
                         static_cast<double>(u.z)};
    const double lo[3] = {static_cast<double>(box.min.x),
                          static_cast<double>(box.min.y),
                          static_cast<double>(box.min.z)};
    const double hi[3] = {static_cast<double>(box.max.x),
                          static_cast<double>(box.max.y),
                          static_cast<double>(box.max.z)};
    double tmin = -std::numeric_limits<double>::infinity();
    double tmax = std::numeric_limits<double>::infinity();
    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(d[axis]) < 1e-12) {
            if (o[axis] < lo[axis] || o[axis] > hi[axis]) {
                return false;
            }
            continue;
        }
        double t1 = (lo[axis] - o[axis]) / d[axis];
        double t2 = (hi[axis] - o[axis]) / d[axis];
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

vec3 obb3_point(const geom::obb3& o, real x, real y, real z) {
    return o.center + o.axis_x * x + o.axis_y * y + o.axis_z * z;
}

// Face-only SAT (6 axes). Used to prove the 15-axis implementation has extra
// discriminating power: some disjoint pairs are separated only by an edge
// cross product.
bool face_only_sat(const geom::obb3& a, const geom::obb3& b) {
    const vec3 axes[6] = {a.axis_x, a.axis_y, a.axis_z,
                          b.axis_x, b.axis_y, b.axis_z};
    const vec3 axes_a[3] = {a.axis_x, a.axis_y, a.axis_z};
    const vec3 axes_b[3] = {b.axis_x, b.axis_y, b.axis_z};
    const double ha[3] = {std::fabs(static_cast<double>(a.half_extents.x)),
                          std::fabs(static_cast<double>(a.half_extents.y)),
                          std::fabs(static_cast<double>(a.half_extents.z))};
    const double hb[3] = {std::fabs(static_cast<double>(b.half_extents.x)),
                          std::fabs(static_cast<double>(b.half_extents.y)),
                          std::fabs(static_cast<double>(b.half_extents.z))};
    const vec3 delta = b.center - a.center;
    for (const vec3 axis : axes) {
        const vec3 n = geom::normalized(axis);
        if (geom::length_squared(n) <= 0.0f) {
            continue;
        }
        double ra = 0.0;
        double rb = 0.0;
        for (int i = 0; i < 3; ++i) {
            ra += ha[i] * std::fabs(static_cast<double>(geom::dot(axes_a[i], n)));
            rb += hb[i] * std::fabs(static_cast<double>(geom::dot(axes_b[i], n)));
        }
        const double distance_along =
            std::fabs(static_cast<double>(geom::dot(delta, n)));
        if (distance_along > ra + rb + static_cast<double>(geom::k_epsilon)) {
            return false;
        }
    }
    return true;
}

geom::obb3 random_obb3(std::uint64_t& state) {
    return geom::obb3::from_euler(
        vec3(uniform_real(-3.0f, 3.0f, state),
             uniform_real(-3.0f, 3.0f, state),
             uniform_real(-3.0f, 3.0f, state)),
        vec3(uniform_real(0.4f, 2.0f, state),
             uniform_real(0.4f, 2.0f, state),
             uniform_real(0.4f, 2.0f, state)),
        uniform_real(-geom::k_pi, geom::k_pi, state),
        uniform_real(-geom::k_pi, geom::k_pi, state),
        uniform_real(-geom::k_pi, geom::k_pi, state));
}

double degrees_to_radians(double degrees) {
    return degrees * static_cast<double>(geom::k_pi) / 180.0;
}

} // namespace

// --- Vectors and bases -------------------------------------------------------

CRPC_TEST(geom3d_vector_and_basis) {
    const vec3 a(1.0f, 2.0f, 3.0f);
    const vec3 b(4.0f, -5.0f, 6.0f);
    CRPC_CHECK(close(geom::length(vec3(0.0f, 3.0f, 4.0f)), 5.0f));
    CRPC_CHECK(close(geom::dot(a, b), 4.0f - 10.0f + 18.0f));
    CRPC_CHECK(close_vec3(geom::cross(vec3(1.0f, 0.0f, 0.0f),
                                      vec3(0.0f, 1.0f, 0.0f)),
                          vec3(0.0f, 0.0f, 1.0f)));
    const geom::basis3 basis = geom::orthonormal_basis(vec3(1.0f, 2.0f, -1.0f));
    CRPC_CHECK(close(geom::length(basis.forward), 1.0f));
    CRPC_CHECK(close(geom::length(basis.right), 1.0f));
    CRPC_CHECK(close(geom::length(basis.up), 1.0f));
    CRPC_CHECK(close(geom::dot(basis.right, basis.up), 0.0f));
    CRPC_CHECK(close(geom::dot(basis.right, basis.forward), 0.0f));
    CRPC_CHECK(close(geom::dot(basis.up, basis.forward), 0.0f));
    CRPC_CHECK(
        close(geom::angle_between(vec3(0.0f, 1.0f, 0.0f),
                                  vec3(0.0f, -1.0f, 0.0f)),
              geom::k_pi));
    CRPC_CHECK(close(geom::angle_between(vec3(1.0f, 0.0f, 0.0f),
                                         vec3(1.0f, 0.0f, 0.0f)),
                     0.0f));
}

// --- Spheres -----------------------------------------------------------------

CRPC_TEST(geom3d_sphere_contains_and_bounds) {
    const geom::sphere s{vec3(1.0f, 2.0f, -3.0f), 5.0f};
    CRPC_CHECK(s.contains(vec3(1.0f, 2.0f, -3.0f)));
    CRPC_CHECK(s.contains(vec3(6.0f, 2.0f, -3.0f)));
    CRPC_CHECK(s.contains(vec3(6.0f + 1e-6f, 2.0f, -3.0f)));
    CRPC_CHECK_FALSE(s.contains(vec3(6.1f, 2.0f, -3.0f)));
    const geom::aabb3 b = s.bounds();
    CRPC_CHECK(close_vec3(b.min, vec3(-4.0f, -3.0f, -8.0f)));
    CRPC_CHECK(close_vec3(b.max, vec3(6.0f, 7.0f, 2.0f)));
    const geom::sphere point{vec3(1.0f, 1.0f, 1.0f), 0.0f};
    CRPC_CHECK(point.contains(vec3(1.0f, 1.0f, 1.0f)));
    CRPC_CHECK_FALSE(point.contains(vec3(1.001f, 1.0f, 1.0f)));
}

// --- Cone (sector in 3D) -----------------------------------------------------

CRPC_TEST(geom3d_cone_contains_360_headings) {
    const real headings[] = {0.0f, 90.0f, 180.0f, 270.0f, 350.0f, 359.9f};
    for (const real degrees : headings) {
        const real angle =
            static_cast<real>(degrees_to_radians(static_cast<double>(degrees)));
        const vec3 axis(std::cos(angle), std::sin(angle), 0.0f);
        const geom::cone c{vec3(0.0f, 0.0f, 0.0f), axis,
                           static_cast<real>(degrees_to_radians(10.0)), 10.0f};
        CRPC_CHECK(c.contains(axis * 5.0f));
        CRPC_CHECK(c.contains(vec3(0.0f, 0.0f, 0.0f)));
        CRPC_CHECK_FALSE(c.contains(axis * -5.0f));
        // A direction +5 degrees off the axis is inside a 10-degree cone.
        const vec3 nearby(std::cos(angle + static_cast<real>(
                                        degrees_to_radians(5.0))),
                          std::sin(angle + static_cast<real>(
                                        degrees_to_radians(5.0))),
                          0.0f);
        CRPC_CHECK(c.contains(nearby * 4.0f));
        // A direction +30 degrees off the axis is outside.
        const vec3 outside(std::cos(angle + static_cast<real>(
                                         degrees_to_radians(30.0))),
                           std::sin(angle + static_cast<real>(
                                         degrees_to_radians(30.0))),
                           0.0f);
        CRPC_CHECK_FALSE(c.contains(outside * 4.0f));
        // 360-degree wrap: the 350-degree axis accepts the 0-degree direction
        // (10 degrees away, exactly the half angle) and rejects 20 degrees.
        if (close(degrees, 350.0f, 0.01f)) {
            CRPC_CHECK(c.contains(vec3(4.0f, 0.0f, 0.0f)));
        }
    }
    const geom::cone full{vec3(0.0f, 0.0f, 0.0f), vec3(1.0f, 0.0f, 0.0f),
                          geom::k_pi, 3.0f};
    CRPC_CHECK(full.contains(vec3(-3.0f, 0.0f, 0.0f)));
    CRPC_CHECK_FALSE(full.contains(vec3(3.1f, 0.0f, 0.0f)));
}

CRPC_TEST(geom3d_cone_bounds_centroid_and_zero) {
    const geom::cone c{vec3(0.0f, 0.0f, 0.0f), vec3(1.0f, 0.0f, 0.0f),
                       geom::k_pi / 4.0f, 10.0f};
    const geom::aabb3 b = c.bounds();
    CRPC_CHECK(close(b.min.x, 0.0f));
    CRPC_CHECK(close(b.max.x, 10.0f));
    const real extent = 10.0f * std::sin(geom::k_pi / 4.0f);
    CRPC_CHECK(close(b.max.y, extent));
    CRPC_CHECK(close(b.min.y, -extent));
    CRPC_CHECK(close(b.max.z, extent));
    CRPC_CHECK(close(b.min.z, -extent));
    CRPC_CHECK(close_vec3(c.edge_anchor(), c.apex));

    const geom::cone ninety{vec3(0.0f, 0.0f, 0.0f), vec3(1.0f, 0.0f, 0.0f),
                            geom::k_pi / 2.0f, 8.0f};
    // d = 3R/8 * (1 + cos(90deg)) = 3.
    CRPC_CHECK(close_vec3(ninety.centroid(), vec3(3.0f, 0.0f, 0.0f)));

    const geom::cone point{vec3(2.0f, 2.0f, 2.0f), vec3(1.0f, 0.0f, 0.0f),
                           0.3f, 0.0f};
    CRPC_CHECK(point.contains(vec3(2.0f, 2.0f, 2.0f)));
    CRPC_CHECK_FALSE(point.contains(vec3(2.001f, 2.0f, 2.0f)));
}

// --- Umbrella3 ---------------------------------------------------------------

CRPC_TEST(geom3d_umbrella_canopy_and_handle) {
    const geom::umbrella3 u{vec3(0.0f, 0.0f, 0.0f), vec3(1.0f, 0.0f, 0.0f),
                            geom::k_pi / 4.0f, 10.0f, 5.0f, 1.0f};
    CRPC_CHECK(u.contains(vec3(0.0f, 0.0f, 0.0f)));
    CRPC_CHECK(u.contains(vec3(5.0f, 0.0f, 0.0f)));
    CRPC_CHECK(u.contains(vec3(5.0f, 3.0f, 0.0f)));  // inside the +-45 canopy
    CRPC_CHECK_FALSE(u.contains(vec3(5.0f, 7.0f, 0.0f)));
    CRPC_CHECK(u.contains(vec3(-3.0f, 0.0f, 0.0f))); // shaft
    CRPC_CHECK_FALSE(u.contains(vec3(-3.0f, 2.0f, 0.0f)));
    CRPC_CHECK_FALSE(u.contains(vec3(-6.0f, 0.0f, 0.0f)));
    const geom::aabb3 b = u.bounds();
    CRPC_CHECK(close(b.max.x, 10.0f));
    CRPC_CHECK(close(b.min.x, -5.0f));
    const real extent = 10.0f * std::sin(geom::k_pi / 4.0f);
    CRPC_CHECK(close(b.max.z, extent));
    CRPC_CHECK(close(b.min.z, -extent));
    CRPC_CHECK(close_vec3(u.edge_anchor(), u.apex));

    const geom::umbrella3 dot{vec3(1.0f, 1.0f, 1.0f), vec3(0.0f, 0.0f, 1.0f),
                              0.0f, 0.0f, 0.0f, 0.0f};
    CRPC_CHECK(dot.contains(vec3(1.0f, 1.0f, 1.0f)));
    CRPC_CHECK_FALSE(dot.contains(vec3(1.0f, 1.0f, 1.01f)));
}

// --- OBB3 and the 15-axis SAT ------------------------------------------------

CRPC_TEST(geom3d_obb_contains_bounds_and_euler) {
    const geom::obb3 o = geom::obb3::from_euler(
        vec3(0.0f, 0.0f, 0.0f), vec3(5.0f, 1.0f, 2.0f), geom::k_pi / 2.0f,
        0.0f, 0.0f);
    // Yaw 90: local x now points along world +y, local y along world -x.
    CRPC_CHECK(o.contains(vec3(0.0f, 4.0f, 0.0f)));
    CRPC_CHECK(o.contains(vec3(-0.5f, 0.0f, 1.0f)));
    CRPC_CHECK_FALSE(o.contains(vec3(4.0f, 0.0f, 0.0f)));
    CRPC_CHECK(close_vec3(o.axis_x, vec3(0.0f, 1.0f, 0.0f)));
    CRPC_CHECK(close_vec3(o.axis_y, vec3(-1.0f, 0.0f, 0.0f)));
    const geom::aabb3 b = o.bounds();
    CRPC_CHECK(close(b.min.x, -1.0f));
    CRPC_CHECK(close(b.max.x, 1.0f));
    CRPC_CHECK(close(b.min.y, -5.0f));
    CRPC_CHECK(close(b.max.y, 5.0f));
    CRPC_CHECK(close(b.min.z, -2.0f));
    CRPC_CHECK(close(b.max.z, 2.0f));
    CRPC_CHECK(o.contains(o.corner(0)));
    CRPC_CHECK(close_vec3(o.centroid(), o.center));
    CRPC_CHECK(close_vec3(o.edge_anchor(),
                          o.center + o.axis_x * o.half_extents.x));

    const geom::obb3 flat{vec3(1.0f, 2.0f, 3.0f), vec3(0.0f, 0.0f, 0.0f),
                          vec3(1.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f),
                          vec3(0.0f, 0.0f, 1.0f)};
    CRPC_CHECK(flat.contains(vec3(1.0f, 2.0f, 3.0f)));
    CRPC_CHECK_FALSE(flat.contains(vec3(1.001f, 2.0f, 3.0f)));
}

CRPC_TEST(geom3d_obb_sat_hand_cases) {
    const geom::obb3 a{vec3(0.0f, 0.0f, 0.0f), vec3(5.0f, 5.0f, 5.0f),
                       vec3(1.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f),
                       vec3(0.0f, 0.0f, 1.0f)};
    const geom::obb3 apart{vec3(12.0f, 0.0f, 0.0f), vec3(5.0f, 5.0f, 5.0f),
                           vec3(1.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f),
                           vec3(0.0f, 0.0f, 1.0f)};
    const geom::obb3 touching{vec3(10.0f, 0.0f, 0.0f), vec3(5.0f, 5.0f, 5.0f),
                              vec3(1.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f),
                              vec3(0.0f, 0.0f, 1.0f)};
    CRPC_CHECK(geom::intersects(a, a));
    CRPC_CHECK_FALSE(geom::intersects(a, apart));
    CRPC_CHECK(geom::intersects(a, touching));
    const geom::obb3 diamond = geom::obb3::from_euler(
        vec3(12.0f, 0.0f, 0.0f), vec3(5.0f, 5.0f, 5.0f), geom::k_pi / 4.0f,
        0.0f, 0.0f);
    CRPC_CHECK(geom::intersects(a, diamond));
    const geom::obb3 diamond_far = geom::obb3::from_euler(
        vec3(20.0f, 0.0f, 0.0f), vec3(5.0f, 5.0f, 5.0f), geom::k_pi / 4.0f,
        0.0f, 0.0f);
    CRPC_CHECK_FALSE(geom::intersects(a, diamond_far));
    const geom::obb3 point_inside{vec3(5.0f, 5.0f, 5.0f), vec3(0.0f, 0.0f, 0.0f),
                                  vec3(1.0f, 0.0f, 0.0f),
                                  vec3(0.0f, 1.0f, 0.0f),
                                  vec3(0.0f, 0.0f, 1.0f)};
    const geom::obb3 point_outside{vec3(5.1f, 5.0f, 5.0f), vec3(0.0f, 0.0f, 0.0f),
                                   vec3(1.0f, 0.0f, 0.0f),
                                   vec3(0.0f, 1.0f, 0.0f),
                                   vec3(0.0f, 0.0f, 1.0f)};
    CRPC_CHECK(geom::intersects(a, point_inside));
    CRPC_CHECK_FALSE(geom::intersects(a, point_outside));
}

CRPC_TEST(geom3d_obb_sat_matches_sampling_and_needs_cross_axes) {
    // Sampling oracle: a point of A's interior inside B is a guaranteed
    // overlap, so the 15-axis SAT must agree. SAT may report a thin overlap
    // the grid misses; those are bounded below.
    std::uint64_t state = 0x1BADB002ULL;
    constexpr int k_steps = 25;
    std::size_t sat_true = 0;
    std::size_t sat_without_sample = 0;
    std::size_t sampled_overlaps = 0;
    std::size_t face_only_true_full_false = 0;
    for (int pair = 0; pair < 200; ++pair) {
        const geom::obb3 a = random_obb3(state);
        geom::obb3 b = random_obb3(state);
        if ((pair % 2) == 0) {
            b.center = a.center + vec3(uniform_real(-2.0f, 2.0f, state),
                                       uniform_real(-2.0f, 2.0f, state),
                                       uniform_real(-2.0f, 2.0f, state));
        }
        bool sampled = false;
        for (int i = 0; i < k_steps && !sampled; ++i) {
            const real u =
                -a.half_extents.x +
                (2.0f * a.half_extents.x) *
                    (static_cast<real>(i) / static_cast<real>(k_steps - 1));
            for (int j = 0; j < k_steps && !sampled; ++j) {
                const real v =
                    -a.half_extents.y +
                    (2.0f * a.half_extents.y) *
                        (static_cast<real>(j) / static_cast<real>(k_steps - 1));
                for (int k = 0; k < k_steps; ++k) {
                    const real w =
                        -a.half_extents.z +
                        (2.0f * a.half_extents.z) *
                            (static_cast<real>(k) /
                             static_cast<real>(k_steps - 1));
                    if (b.contains(obb3_point(a, u, v, w))) {
                        sampled = true;
                        break;
                    }
                }
            }
        }
        const bool sat = geom::intersects(a, b);
        if (sampled) {
            ++sampled_overlaps;
            CRPC_CHECK(sat);
        }
        if (sat) {
            ++sat_true;
            if (!sampled) {
                ++sat_without_sample;
            }
        } else {
            CRPC_CHECK_FALSE(sampled);
            if (face_only_sat(a, b)) {
                ++face_only_true_full_false;
            }
        }
    }
    CRPC_CHECK(sampled_overlaps >= 20);
    CRPC_CHECK(sat_true >= 20);
    CRPC_CHECK(sat_without_sample * 5 <= sat_true);
    // If this were zero, the 6 face axes would have been sufficient for every
    // sampled pair and the test would not prove the 9 edge cross products are
    // implemented. Random 3D orientations produce such pairs routinely.
    CRPC_CHECK(face_only_true_full_false > 0);
}

// --- Convex polyhedron -------------------------------------------------------

CRPC_TEST(geom3d_polyhedron_halfspace_contains_and_ray) {
    const geom::convex_polyhedron p =
        box_polyhedron(vec3(-1.0f, -2.0f, -3.0f), vec3(1.0f, 2.0f, 3.0f));
    CRPC_CHECK(p.valid());
    CRPC_CHECK(p.contains(vec3(0.0f, 0.0f, 0.0f)));
    CRPC_CHECK(p.contains(vec3(1.0f, 2.0f, 3.0f)));
    CRPC_CHECK_FALSE(p.contains(vec3(1.1f, 0.0f, 0.0f)));
    CRPC_CHECK_FALSE(p.contains(vec3(0.0f, -2.1f, 0.0f)));
    const geom::aabb3 b = p.bounds();
    CRPC_CHECK(close_vec3(b.min, vec3(-1.0f, -2.0f, -3.0f)));
    CRPC_CHECK(close_vec3(b.max, vec3(1.0f, 2.0f, 3.0f)));
    const auto hit = geom::ray3_polyhedron_hit(
        geom::ray3{vec3(-10.0f, 0.0f, 0.0f), vec3(1.0f, 0.0f, 0.0f), 30.0f},
        p);
    CRPC_CHECK(hit.ok());
    if (hit.ok()) {
        CRPC_CHECK(close(hit.value(), 9.0f));
    }
    const auto inside = geom::ray3_polyhedron_hit(
        geom::ray3{vec3(0.0f, 0.0f, 0.0f), vec3(1.0f, 0.0f, 0.0f), 30.0f},
        p);
    CRPC_CHECK(inside.ok());
    if (inside.ok()) {
        CRPC_CHECK(close(inside.value(), 0.0f));
    }
    const geom::convex_polyhedron empty;
    CRPC_CHECK_FALSE(empty.valid());
    CRPC_CHECK_FALSE(empty.contains(vec3(0.0f, 0.0f, 0.0f)));
}

// --- 3D ray / line / segment -------------------------------------------------

CRPC_TEST(geom3d_ray_sphere_plane_and_analytic_oracle) {
    const geom::sphere s{vec3(0.0f, 0.0f, 0.0f), 5.0f};
    const auto tangent = geom::ray3_sphere_hit(
        geom::ray3{vec3(-10.0f, 5.0f, 0.0f), vec3(1.0f, 0.0f, 0.0f), 30.0f},
        s);
    CRPC_CHECK(tangent.ok());
    if (tangent.ok()) {
        CRPC_CHECK(close(tangent.value(), 10.0f));
    }
    const auto inside = geom::ray3_sphere_hit(
        geom::ray3{vec3(0.0f, 0.0f, 0.0f), vec3(1.0f, 0.0f, 0.0f), 30.0f}, s);
    CRPC_CHECK(inside.ok());
    if (inside.ok()) {
        CRPC_CHECK(close(inside.value(), 0.0f));
    }
    const geom::plane3 plane{vec3(0.0f, 0.0f, 5.0f), vec3(0.0f, 0.0f, 1.0f)};
    const auto plane_hit = geom::ray3_plane_hit(
        geom::ray3{vec3(0.0f, 0.0f, 0.0f), vec3(0.0f, 0.0f, 1.0f), 20.0f},
        plane);
    CRPC_CHECK(plane_hit.ok());
    if (plane_hit.ok()) {
        CRPC_CHECK(close(plane_hit.value(), 5.0f));
    }
    const auto parallel = geom::ray3_plane_hit(
        geom::ray3{vec3(0.0f, 0.0f, 0.0f), vec3(1.0f, 0.0f, 0.0f), 20.0f},
        plane);
    CRPC_CHECK_FALSE(parallel.ok());

    std::uint64_t state = 0xC0DEULL;
    for (int i = 0; i < 400; ++i) {
        const geom::sphere target{vec3(uniform_real(-5.0f, 5.0f, state),
                                       uniform_real(-5.0f, 5.0f, state),
                                       uniform_real(-5.0f, 5.0f, state)),
                                  uniform_real(0.2f, 4.0f, state)};
        const geom::ray3 r{vec3(uniform_real(-10.0f, 10.0f, state),
                                uniform_real(-10.0f, 10.0f, state),
                                uniform_real(-10.0f, 10.0f, state)),
                           random_unit_vec3(state),
                           uniform_real(0.5f, 20.0f, state)};
        double expected = 0.0;
        const bool reference = analytic_ray3_sphere(r, target, expected);
        const auto got = geom::ray3_sphere_hit(r, target);
        CRPC_CHECK_EQ(reference, got.ok());
        if (reference && got.ok()) {
            CRPC_CHECK(close(std::fabs(got.value()),
                             static_cast<real>(expected), 2e-3f));
        }
    }
}

CRPC_TEST(geom3d_ray_box_and_obb_analytic_oracle) {
    const geom::aabb3 box{vec3(-1.0f, -1.0f, -1.0f), vec3(1.0f, 1.0f, 1.0f)};
    const auto box_hit = geom::ray3_aabb_hit(
        geom::ray3{vec3(-10.0f, 0.0f, 0.0f), vec3(1.0f, 0.0f, 0.0f), 30.0f},
        box);
    CRPC_CHECK(box_hit.ok());
    if (box_hit.ok()) {
        CRPC_CHECK(close(box_hit.value(), 9.0f));
    }
    const geom::obb3 obb = geom::obb3::from_euler(
        vec3(0.0f, 0.0f, 0.0f), vec3(5.0f, 1.0f, 2.0f), geom::k_pi / 2.0f,
        0.0f, 0.0f);
    const auto obb_hit = geom::ray3_obb_hit(
        geom::ray3{vec3(-10.0f, 0.0f, 0.0f), vec3(1.0f, 0.0f, 0.0f), 30.0f},
        obb);
    CRPC_CHECK(obb_hit.ok());
    if (obb_hit.ok()) {
        CRPC_CHECK(close(obb_hit.value(), 9.0f));
    }

    std::uint64_t state = 0xAB1EULL;
    for (int i = 0; i < 300; ++i) {
        const geom::obb3 target = geom::obb3::from_euler(
            vec3(uniform_real(-3.0f, 3.0f, state),
                 uniform_real(-3.0f, 3.0f, state),
                 uniform_real(-3.0f, 3.0f, state)),
            vec3(uniform_real(0.3f, 3.0f, state),
                 uniform_real(0.3f, 3.0f, state),
                 uniform_real(0.3f, 3.0f, state)),
            uniform_real(-geom::k_pi, geom::k_pi, state),
            uniform_real(-geom::k_pi, geom::k_pi, state),
            uniform_real(-geom::k_pi, geom::k_pi, state));
        const geom::ray3 r{vec3(uniform_real(-10.0f, 10.0f, state),
                                uniform_real(-10.0f, 10.0f, state),
                                uniform_real(-10.0f, 10.0f, state)),
                           random_unit_vec3(state),
                           uniform_real(0.5f, 20.0f, state)};
        double expected = 0.0;
        const bool reference = analytic_ray3_obb(r, target, expected);
        const auto got = geom::ray3_obb_hit(r, target);
        CRPC_CHECK_EQ(reference, got.ok());
        if (reference && got.ok()) {
            CRPC_CHECK(close(std::fabs(got.value()),
                             static_cast<real>(expected), 2e-3f));
        }
    }

    std::uint64_t box_state = 0x5A5AULL;
    for (int i = 0; i < 200; ++i) {
        geom::aabb3 target;
        const vec3 center(uniform_real(-3.0f, 3.0f, box_state),
                          uniform_real(-3.0f, 3.0f, box_state),
                          uniform_real(-3.0f, 3.0f, box_state));
        const vec3 half(uniform_real(0.3f, 3.0f, box_state),
                        uniform_real(0.3f, 3.0f, box_state),
                        uniform_real(0.3f, 3.0f, box_state));
        target.min = center - half;
        target.max = center + half;
        const geom::ray3 r{vec3(uniform_real(-10.0f, 10.0f, box_state),
                                uniform_real(-10.0f, 10.0f, box_state),
                                uniform_real(-10.0f, 10.0f, box_state)),
                           random_unit_vec3(box_state),
                           uniform_real(0.5f, 20.0f, box_state)};
        double expected = 0.0;
        const bool reference = analytic_ray3_aabb(r, target, expected);
        const auto got = geom::ray3_aabb_hit(r, target);
        CRPC_CHECK_EQ(reference, got.ok());
        if (reference && got.ok()) {
            CRPC_CHECK(close(std::fabs(got.value()),
                             static_cast<real>(expected), 2e-3f));
        }
    }
}

CRPC_TEST(geom3d_line_and_segment_semantics) {
    const geom::sphere s{vec3(0.0f, 0.0f, 0.0f), 1.0f};
    const geom::line3 infinite{vec3(10.0f, 0.0f, 0.0f), vec3(1.0f, 0.0f, 0.0f)};
    const auto line_hit = geom::line3_sphere_hit(infinite, s);
    CRPC_CHECK(line_hit.ok());
    if (line_hit.ok()) {
        CRPC_CHECK(close(static_cast<real>(std::fabs(line_hit.value())), 9.0f));
    }
    const geom::segment3 away{vec3(5.0f, 0.0f, 0.0f), vec3(20.0f, 0.0f, 0.0f)};
    CRPC_CHECK_FALSE(geom::segment3_sphere_hit(away, s).ok());
    const geom::segment3 crossing{vec3(-5.0f, 0.0f, 0.0f),
                                  vec3(5.0f, 0.0f, 0.0f)};
    const auto crossing_hit = geom::segment3_sphere_hit(crossing, s);
    CRPC_CHECK(crossing_hit.ok());
    if (crossing_hit.ok()) {
        CRPC_CHECK(close(crossing_hit.value(), 0.4f));
    }
    const geom::segment3 point{vec3(0.5f, 0.0f, 0.0f), vec3(0.5f, 0.0f, 0.0f)};
    const auto point_hit = geom::segment3_sphere_hit(point, s);
    CRPC_CHECK(point_hit.ok());
    if (point_hit.ok()) {
        CRPC_CHECK(close(point_hit.value(), 0.0f));
    }
}

// --- hitbox3: center vs edge -------------------------------------------------

CRPC_TEST(geom3d_hitbox_center_edge_and_orientation) {
    const vec3 where(10.0f, -2.0f, 3.0f);

    const geom::sphere sphere{vec3(0.0f, 0.0f, 0.0f), 5.0f};
    const geom::hitbox3 sphere_center(sphere, geom::anchor_mode::center, where);
    const geom::hitbox3 sphere_edge(sphere, geom::anchor_mode::edge, where);
    CRPC_CHECK(sphere_center.contains(where + vec3(1.0f, 0.0f, 0.0f)));
    CRPC_CHECK_FALSE(sphere_edge.contains(where + vec3(1.0f, 0.0f, 0.0f)));

    const geom::obb3 box{vec3(0.0f, 0.0f, 0.0f), vec3(5.0f, 3.0f, 2.0f),
                         vec3(1.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f),
                         vec3(0.0f, 0.0f, 1.0f)};
    const geom::hitbox3 box_center(box, geom::anchor_mode::center, where);
    const geom::hitbox3 box_edge(box, geom::anchor_mode::edge, where);
    CRPC_CHECK(box_center.contains(where + vec3(1.0f, 0.0f, 0.0f)));
    CRPC_CHECK_FALSE(box_edge.contains(where + vec3(1.0f, 0.0f, 0.0f)));

    const geom::cone cone{vec3(0.0f, 0.0f, 0.0f), vec3(1.0f, 0.0f, 0.0f),
                          geom::k_pi / 4.0f, 10.0f};
    const geom::hitbox3 cone_center(cone, geom::anchor_mode::center, where);
    const geom::hitbox3 cone_edge(cone, geom::anchor_mode::edge, where);
    CRPC_CHECK(cone_center.contains(where + vec3(-0.1f, 0.0f, 0.0f)));
    CRPC_CHECK_FALSE(cone_edge.contains(where + vec3(-0.1f, 0.0f, 0.0f)));

    const geom::segment3 segment{vec3(0.0f, 0.0f, 0.0f),
                                 vec3(10.0f, 0.0f, 0.0f)};
    const geom::hitbox3 segment_center(segment, geom::anchor_mode::center, where);
    const geom::hitbox3 segment_edge(segment, geom::anchor_mode::edge, where);
    CRPC_CHECK(segment_center.contains(where + vec3(-1.0f, 0.0f, 0.0f)));
    CRPC_CHECK_FALSE(segment_edge.contains(where + vec3(-1.0f, 0.0f, 0.0f)));

    const geom::convex_polyhedron cube =
        box_polyhedron(vec3(-5.0f, -5.0f, -5.0f), vec3(5.0f, 5.0f, 5.0f));
    const geom::hitbox3 cube_center(cube, geom::anchor_mode::center, where);
    const geom::hitbox3 cube_edge(cube, geom::anchor_mode::edge, where);
    CRPC_CHECK(cube_center.contains(where + vec3(0.0f, 0.0f, -1.0f)));
    CRPC_CHECK_FALSE(cube_edge.contains(where + vec3(0.0f, 0.0f, -1.0f)));

    // Explicit orientation: local +x maps to world +y.
    geom::basis3 orientation;
    orientation.right = vec3(0.0f, 1.0f, 0.0f);
    orientation.up = vec3(1.0f, 0.0f, 0.0f);
    orientation.forward = vec3(0.0f, 0.0f, 1.0f);
    const geom::hitbox3 rotated(segment, geom::anchor_mode::center, where,
                                orientation, 1.0f);
    CRPC_CHECK(rotated.contains(where + vec3(0.0f, 4.0f, 0.0f)));
    CRPC_CHECK_FALSE(rotated.contains(where + vec3(4.0f, 0.0f, 0.0f)));
    const geom::aabb3 wb = rotated.world_bounds();
    CRPC_CHECK(wb.valid());
    CRPC_CHECK(wb.contains(where + vec3(0.0f, 4.0f, 0.0f)));

    const geom::hitbox3 empty(box, geom::anchor_mode::center, where,
                              geom::orthonormal_basis(vec3(0.0f, 0.0f, 1.0f)),
                              0.0f);
    CRPC_CHECK_FALSE(empty.contains(where));
    CRPC_CHECK_FALSE(empty.world_bounds().valid());
}

// --- 360-degree shotgun spread ----------------------------------------------

CRPC_TEST(geom3d_spread_2d_is_bounded_deterministic_and_wraps) {
    const real forward_degrees[] = {0.0f, 90.0f, 180.0f, 270.0f, 350.0f,
                                    359.9f};
    for (const real degrees : forward_degrees) {
        const real base =
            static_cast<real>(degrees_to_radians(static_cast<double>(degrees)));
        const vec2 forward(std::cos(base), std::sin(base));
        geom::spread_config config;
        config.projectiles = 10;
        config.total_angle =
            static_cast<real>(degrees_to_radians(30.0));
        config.uniform = true;
        const auto directions = geom::spread_directions2(forward, config);
        CRPC_CHECK(directions.ok());
        if (!directions.ok()) {
            continue;
        }
        CRPC_CHECK_EQ(std::size_t(10), directions.value().size());
        real max_offset = 0.0f;
        for (const vec2 d : directions.value()) {
            CRPC_CHECK(close(geom::length(d), 1.0f, 1e-4f));
            const real offset = geom::angle_difference(base, geom::angle_of(d));
            CRPC_CHECK(std::fabs(offset) <=
                       static_cast<real>(degrees_to_radians(15.0)) + 1e-4f);
            max_offset = std::max(max_offset, std::fabs(offset));
        }
        CRPC_CHECK(max_offset > 0.01f); // not all pellets pointing the same way
    }

    // Same seed -> exactly the same random pellets.
    geom::spread_config random_config;
    random_config.projectiles = 25;
    random_config.total_angle = static_cast<real>(degrees_to_radians(60.0));
    random_config.uniform = false;
    random_config.seed = 0xDEADBEEFULL;
    const vec2 forward(std::cos(1.0f), std::sin(1.0f));
    const auto first = geom::spread_directions2(forward, random_config);
    const auto second = geom::spread_directions2(forward, random_config);
    CRPC_CHECK(first.ok());
    CRPC_CHECK(second.ok());
    if (first.ok() && second.ok()) {
        CRPC_CHECK_EQ(first.value().size(), second.value().size());
        for (std::size_t i = 0; i < first.value().size(); ++i) {
            CRPC_CHECK(geom::nearly_equal(first.value()[i], second.value()[i],
                                          0.0f));
            const real offset = geom::angle_difference(
                1.0f, geom::angle_of(first.value()[i]));
            CRPC_CHECK(std::fabs(offset) <=
                       static_cast<real>(degrees_to_radians(30.0)) + 1e-4f);
        }
    }

    const auto empty = geom::spread_directions2(
        forward, geom::spread_config{0, 1.0f, true, 0});
    CRPC_CHECK(empty.ok());
    if (empty.ok()) {
        CRPC_CHECK(empty.value().empty());
    }
    const auto bad = geom::spread_directions2(
        forward, geom::spread_config{3, -1.0f, true, 0});
    CRPC_CHECK_FALSE(bad.ok());
    if (!bad.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(bad.code()));
    }
}

CRPC_TEST(geom3d_spread_3d_is_a_spherical_cap_and_deterministic) {
    const real base =
        static_cast<real>(degrees_to_radians(359.9));
    const vec3 forward(std::cos(base), std::sin(base), 0.0f);
    geom::spread_config config;
    config.projectiles = 10;
    config.total_angle = static_cast<real>(degrees_to_radians(30.0));
    config.uniform = true;
    const auto directions = geom::spread_directions3(forward, config);
    CRPC_CHECK(directions.ok());
    if (!directions.ok()) {
        return;
    }
    CRPC_CHECK_EQ(std::size_t(10), directions.value().size());
    real max_angle = 0.0f;
    for (const vec3 d : directions.value()) {
        CRPC_CHECK(close(geom::length(d), 1.0f, 1e-4f));
        const real angle = geom::angle_between(d, forward);
        CRPC_CHECK(angle <= static_cast<real>(degrees_to_radians(15.0)) + 1e-4f);
        max_angle = std::max(max_angle, angle);
    }
    CRPC_CHECK(max_angle > 0.01f);

    config.uniform = false;
    config.seed = 123456789ULL;
    const auto first = geom::spread_directions3(forward, config);
    const auto second = geom::spread_directions3(forward, config);
    CRPC_CHECK(first.ok());
    CRPC_CHECK(second.ok());
    if (first.ok() && second.ok()) {
        CRPC_CHECK_EQ(first.value().size(), second.value().size());
        for (std::size_t i = 0; i < first.value().size(); ++i) {
            CRPC_CHECK(geom::nearly_equal(first.value()[i], second.value()[i],
                                          0.0f));
            CRPC_CHECK(geom::angle_between(first.value()[i], forward) <=
                       static_cast<real>(degrees_to_radians(15.0)) + 1e-4f);
        }
    }
}

// --- Instant vs projectile (speed + distance) --------------------------------

CRPC_TEST(geom3d_attack_speed_zero_is_instant_and_hits_static_targets) {
    const vec2 origin2(0.0f, 0.0f);
    const geom::attack_target2 static2{vec2(100.0f, 0.0f), vec2(0.0f, 0.0f),
                                       1.0f};
    const geom::attack_profile instant{0.0f, 0.0f};
    const geom::attack_profile projectile{10.0f, 20.0f};
    const auto instant_hit = geom::attack_hit2(origin2, vec2(1.0f, 0.0f),
                                               static2, instant);
    const auto projectile_hit = geom::attack_hit2(origin2, vec2(1.0f, 0.0f),
                                                  static2, projectile);
    CRPC_CHECK(instant_hit.ok());
    CRPC_CHECK(projectile_hit.ok());

    const vec3 origin3(0.0f, 0.0f, 0.0f);
    const geom::attack_target3 static3{vec3(100.0f, 0.0f, 0.0f),
                                       vec3(0.0f, 0.0f, 0.0f), 1.0f};
    const auto instant_hit3 = geom::attack_hit3(origin3, vec3(1.0f, 0.0f, 0.0f),
                                                static3, instant);
    const auto projectile_hit3 = geom::attack_hit3(
        origin3, vec3(1.0f, 0.0f, 0.0f), static3, projectile);
    CRPC_CHECK(instant_hit3.ok());
    CRPC_CHECK(projectile_hit3.ok());
}

CRPC_TEST(geom3d_projectile_lifetime_is_the_distance_limit) {
    // Radius 0 makes arrival time exactly distance / speed.
    const geom::attack_target2 target{vec2(100.0f, 0.0f), vec2(0.0f, 0.0f),
                                      0.0f};
    const auto in_range = geom::projectile_fire2(
        vec2(0.0f, 0.0f), vec2(1.0f, 0.0f), target, 10.0f, 10.0f);
    CRPC_CHECK(in_range.ok());
    if (in_range.ok()) {
        CRPC_CHECK(close(in_range.value().time, 10.0f));
    }
    const auto out_of_range = geom::projectile_fire2(
        vec2(0.0f, 0.0f), vec2(1.0f, 0.0f), target, 10.0f, 9.9f);
    CRPC_CHECK_FALSE(out_of_range.ok());
    if (!out_of_range.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                      static_cast<int>(out_of_range.code()));
    }
    const auto bad_lifetime = geom::projectile_fire2(
        vec2(0.0f, 0.0f), vec2(1.0f, 0.0f), target, 10.0f, -1.0f);
    CRPC_CHECK_FALSE(bad_lifetime.ok());
    if (!bad_lifetime.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::invalid_argument),
                      static_cast<int>(bad_lifetime.code()));
    }
    const auto zero_speed = geom::projectile_fire2(
        vec2(0.0f, 0.0f), vec2(1.0f, 0.0f), target, 0.0f, 10.0f);
    CRPC_CHECK_FALSE(zero_speed.ok());

    // Instant attacks have no travel time, so distance alone never rejects.
    const auto instant_far = geom::instant_hit2(
        vec2(0.0f, 0.0f), vec2(1.0f, 0.0f),
        geom::attack_target2{vec2(1.0e6f, 0.0f), vec2(0.0f, 0.0f), 1.0f});
    CRPC_CHECK(instant_far.ok());
    const auto projectile_far = geom::projectile_fire2(
        vec2(0.0f, 0.0f), vec2(1.0f, 0.0f),
        geom::attack_target2{vec2(1.0e6f, 0.0f), vec2(0.0f, 0.0f), 1.0f}, 10.0f,
        5.0f);
    CRPC_CHECK_FALSE(projectile_far.ok());
}

CRPC_TEST(geom3d_projectile_leading_beats_no_leading) {
    // 2D: target crosses the shot line; firing at its current position misses,
    // the intercept solver hits.
    const vec2 origin2(0.0f, 0.0f);
    const geom::attack_target2 moving2{vec2(100.0f, 0.0f), vec2(0.0f, 10.0f),
                                       1.0f};
    const auto no_lead2 = geom::projectile_fire2(
        origin2, geom::normalized(moving2.position - origin2), moving2, 100.0f,
        5.0f);
    CRPC_CHECK_FALSE(no_lead2.ok());
    const auto intercept2 =
        geom::projectile_intercept2(origin2, moving2, 100.0f, 5.0f);
    CRPC_CHECK(intercept2.ok());
    if (intercept2.ok()) {
        const auto fired2 =
            geom::projectile_fire2(origin2, intercept2.value().direction, moving2,
                                   100.0f, 5.0f);
        CRPC_CHECK(fired2.ok());
        if (fired2.ok()) {
            CRPC_CHECK(fired2.value().time <= 5.0f + k_close);
        }
    }

    // 3D: same scenario, moving along +y from (100,0,0).
    const vec3 origin3(0.0f, 0.0f, 0.0f);
    const geom::attack_target3 moving3{vec3(100.0f, 0.0f, 0.0f),
                                       vec3(0.0f, 10.0f, 0.0f), 1.0f};
    const auto no_lead3 = geom::projectile_fire3(
        origin3, geom::normalized(moving3.position - origin3), moving3, 100.0f,
        5.0f);
    CRPC_CHECK_FALSE(no_lead3.ok());
    const auto intercept3 =
        geom::projectile_intercept3(origin3, moving3, 100.0f, 5.0f);
    CRPC_CHECK(intercept3.ok());
    if (intercept3.ok()) {
        const auto fired3 =
            geom::projectile_fire3(origin3, intercept3.value().direction, moving3,
                                   100.0f, 5.0f);
        CRPC_CHECK(fired3.ok());
        if (fired3.ok()) {
            CRPC_CHECK(fired3.value().time <= 5.0f + k_close);
        }
    }
}

CRPC_TEST(geom3d_projectile_cannot_catch_a_faster_target) {
    const geom::attack_target2 target2{vec2(100.0f, 0.0f), vec2(200.0f, 0.0f),
                                       1.0f};
    const auto fire2 = geom::projectile_fire2(
        vec2(0.0f, 0.0f), vec2(1.0f, 0.0f), target2, 100.0f, 10.0f);
    CRPC_CHECK_FALSE(fire2.ok());
    const auto intercept2 =
        geom::projectile_intercept2(vec2(0.0f, 0.0f), target2, 100.0f, 10.0f);
    CRPC_CHECK_FALSE(intercept2.ok());
    if (!intercept2.ok()) {
        CRPC_CHECK_EQ(static_cast<int>(crpc::rpc_code::not_found),
                      static_cast<int>(intercept2.code()));
    }

    const geom::attack_target3 target3{vec3(100.0f, 0.0f, 0.0f),
                                       vec3(200.0f, 0.0f, 0.0f), 1.0f};
    const auto fire3 = geom::projectile_fire3(
        vec3(0.0f, 0.0f, 0.0f), vec3(1.0f, 0.0f, 0.0f), target3, 100.0f,
        10.0f);
    CRPC_CHECK_FALSE(fire3.ok());
    const auto intercept3 = geom::projectile_intercept3(
        vec3(0.0f, 0.0f, 0.0f), target3, 100.0f, 10.0f);
    CRPC_CHECK_FALSE(intercept3.ok());
}

CRPC_TEST(geom3d_projectile_intercept_equations_match_fire) {
    // A target moving perpendicular at every heading: the intercept direction
    // must actually hit when fed back through the fixed-direction solver.
    std::uint64_t state = 0x987654321ULL;
    for (int i = 0; i < 100; ++i) {
        const vec3 position(uniform_real(20.0f, 120.0f, state),
                            uniform_real(-60.0f, 60.0f, state),
                            uniform_real(-60.0f, 60.0f, state));
        const vec3 velocity(uniform_real(-20.0f, 20.0f, state),
                            uniform_real(-20.0f, 20.0f, state),
                            uniform_real(-20.0f, 20.0f, state));
        const geom::attack_target3 target{position, velocity, 1.0f};
        const auto intercept =
            geom::projectile_intercept3(vec3(0.0f, 0.0f, 0.0f), target, 150.0f,
                                        5.0f);
        if (!intercept.ok()) {
            continue;
        }
        const auto fired = geom::projectile_fire3(
            vec3(0.0f, 0.0f, 0.0f), intercept.value().direction, target,
            150.0f, 5.0f);
        CRPC_CHECK(fired.ok());
    }
}

CRPC_TEST_MAIN()