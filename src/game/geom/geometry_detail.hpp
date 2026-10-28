// CRPC -- internal helpers shared by the 2D/3D geometry translation units.
// Not installed and not part of the public API.
#ifndef CRPC_GAME_GEOM_SRC_DETAIL_HPP
#define CRPC_GAME_GEOM_SRC_DETAIL_HPP

#include <algorithm>
#include <cmath>

#include <crpc/game/geom/vec.hpp>

namespace crpc {
namespace game {
namespace geom {
namespace detail {

inline double clamp_double(double value, double lo, double hi) noexcept {
    return value < lo ? lo : (value > hi ? hi : value);
}

inline bool finite_real(real value) noexcept {
    return std::isfinite(static_cast<double>(value));
}

inline double point_segment_distance(vec2 p, vec2 a, vec2 b) noexcept {
    const double ax = static_cast<double>(a.x);
    const double ay = static_cast<double>(a.y);
    const double bx = static_cast<double>(b.x);
    const double by = static_cast<double>(b.y);
    const double dx = bx - ax;
    const double dy = by - ay;
    const double len2 = dx * dx + dy * dy;
    const double px = static_cast<double>(p.x);
    const double py = static_cast<double>(p.y);
    if (len2 <= 1e-20) {
        const double ex = px - ax;
        const double ey = py - ay;
        return std::sqrt(ex * ex + ey * ey);
    }
    const double t =
        clamp_double(((px - ax) * dx + (py - ay) * dy) / len2, 0.0, 1.0);
    const double qx = ax + dx * t;
    const double qy = ay + dy * t;
    const double ex = px - qx;
    const double ey = py - qy;
    return std::sqrt(ex * ex + ey * ey);
}

inline bool point_on_segment(vec2 p, vec2 a, vec2 b, real eps) noexcept {
    return point_segment_distance(p, a, b) <= static_cast<double>(eps);
}

template <typename T, std::size_t N>
inline double point_segment_distance3d(const basic_vec<T, N>& p,
                                       const basic_vec<T, N>& a,
                                       const basic_vec<T, N>& b) noexcept {
    double len2 = 0.0;
    double dotp = 0.0;
    for (std::size_t i = 0; i < N; ++i) {
        const double d =
            static_cast<double>(b[i]) - static_cast<double>(a[i]);
        len2 += d * d;
        dotp += (static_cast<double>(p[i]) - static_cast<double>(a[i])) * d;
    }
    if (len2 <= 1e-20) {
        double sum = 0.0;
        for (std::size_t i = 0; i < N; ++i) {
            const double e =
                static_cast<double>(p[i]) - static_cast<double>(a[i]);
            sum += e * e;
        }
        return std::sqrt(sum);
    }
    const double t = clamp_double(dotp / len2, 0.0, 1.0);
    double sum = 0.0;
    for (std::size_t i = 0; i < N; ++i) {
        const double q = static_cast<double>(a[i]) +
                         (static_cast<double>(b[i]) -
                          static_cast<double>(a[i])) *
                             t;
        const double e = static_cast<double>(p[i]) - q;
        sum += e * e;
    }
    return std::sqrt(sum);
}

inline double orient(vec2 a, vec2 b, vec2 c) noexcept {
    return static_cast<double>(cross(b - a, c - a));
}

// Closed segment/segment test: touching (including collinear overlap) counts.
inline bool segments_intersect(vec2 a, vec2 b, vec2 c, vec2 d,
                               real eps) noexcept {
    const double e = static_cast<double>(eps);
    const double o1 = orient(a, b, c);
    const double o2 = orient(a, b, d);
    const double o3 = orient(c, d, a);
    const double o4 = orient(c, d, b);
    if (std::fabs(o1) <= e && point_on_segment(c, a, b, eps)) {
        return true;
    }
    if (std::fabs(o2) <= e && point_on_segment(d, a, b, eps)) {
        return true;
    }
    if (std::fabs(o3) <= e && point_on_segment(a, c, d, eps)) {
        return true;
    }
    if (std::fabs(o4) <= e && point_on_segment(b, c, d, eps)) {
        return true;
    }
    return (o1 > e) != (o2 > e) && (o3 > e) != (o4 > e);
}

} // namespace detail
} // namespace geom
} // namespace game
} // namespace crpc

#endif // CRPC_GAME_GEOM_SRC_DETAIL_HPP