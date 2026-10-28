// CRPC -- game geometry: scalar choice, 2D/3D vectors and small helpers.
//
// ---------------------------------------------------------------------------
// Scalar: float
// ---------------------------------------------------------------------------
// The public scalar is `float`. The reasons, in order of weight:
//
//   1. Hitboxes come from gameplay/physics transforms, which are float in
//      every mainstream engine (Unity, Unreal, Box2D, Bullet). Storing double
//      here would force a conversion on every value that crosses that boundary,
//      and would double the memory touched by the per-frame pair sweep.
//   2. The precision that matters is the one at the *decision boundary*. The
//      library's tolerance (`k_epsilon = 1e-5`) is ~80 ulps at magnitude 1 and
//      ~0.001 at magnitude 100, which is far below any gameplay-meaningful
//      distance; the tests prove the boundary cases at that tolerance.
//   3. All trigonometric and SAT projection work is accumulated in `double`
//      inside the implementation, so a rotation through many angles does not
//      lose more than a few ulps before it is stored back as float.
//
// A caller that genuinely needs double-precision world coordinates should
// transform into a local float frame before hit testing (the usual large-world
// technique); the library does not pretend float is a substitute for that.
//
// ---------------------------------------------------------------------------
// 2D/3D organisation
// ---------------------------------------------------------------------------
// `basic_vec<T, N>` is the shared vector template; `vec2` and `vec3` are its
// two concrete instantiations. Low-level algorithms (dot/cross/length, AABB
// slab tests, SAT projections, point-to-segment distance, projectile
// quadratics) are written once over the template. Shapes are NOT forced into a
// single N-dimensional template: a 2D sector and a 3D spherical cone are
// different solids, a 2D polygon uses ray casting while a convex polyhedron
// uses half-spaces, and OBB-OBB is a 4-axis problem in 2D but a 15-axis one in
// 3D. Pretending otherwise would produce a template full of `if constexpr`
// branches that is harder to audit than the two small, explicit shape sets in
// `shapes2d.hpp` and `shapes3d.hpp`.
#ifndef CRPC_GAME_GEOM_VEC_HPP
#define CRPC_GAME_GEOM_VEC_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <type_traits>

namespace crpc {
namespace game {
namespace geom {

using real = float;

// Absolute tolerance for geometric predicates. See the scalar rationale above:
// float has ~1.19e-7 relative precision, so 1e-5 is a comfortable number of
// ulps without swallowing gameplay-meaningful separations.
constexpr real k_epsilon = 1e-5f;
constexpr real k_pi = 3.14159265358979323846f;
constexpr real k_two_pi = 6.28318530717958647692f;

// How a shape is placed relative to an actor position.
//
//   center -- the actor sits at the shape's interior reference point (the
//             circle centre, the box centre, the polygon centroid, ...). This
//             is the "AOE centred on me" case.
//   edge   -- the actor sits at a designated point on the shape's boundary
//             (the circle rim, the sector/umbrella apex, the ray origin, ...).
//             This is the "fan/umbrella opens from me" case.
//
// The two modes are deliberately a property of the *placement*, not of the
// shape, so every shape can be used either way. An infinite line has no
// boundary; for it both anchors are its reference point (documented on the
// shape).
enum class anchor_mode : unsigned char { center, edge };

template <typename T, std::size_t N>
struct basic_vec;

template <typename T>
struct basic_vec<T, 2> {
    static constexpr std::size_t dimension = 2;
    T x{};
    T y{};

    constexpr basic_vec() = default;
    constexpr basic_vec(T x_, T y_) : x(x_), y(y_) {}

    constexpr T& operator[](std::size_t i) noexcept { return i == 0 ? x : y; }
    constexpr const T& operator[](std::size_t i) const noexcept {
        return i == 0 ? x : y;
    }
};

template <typename T>
struct basic_vec<T, 3> {
    static constexpr std::size_t dimension = 3;
    T x{};
    T y{};
    T z{};

    constexpr basic_vec() = default;
    constexpr basic_vec(T x_, T y_, T z_) : x(x_), y(y_), z(z_) {}

    constexpr T& operator[](std::size_t i) noexcept {
        return i == 0 ? x : (i == 1 ? y : z);
    }
    constexpr const T& operator[](std::size_t i) const noexcept {
        return i == 0 ? x : (i == 1 ? y : z);
    }
};

using vec2 = basic_vec<real, 2>;
using vec3 = basic_vec<real, 3>;

template <typename T, std::size_t N>
constexpr basic_vec<T, N> operator+(const basic_vec<T, N>& a,
                                    const basic_vec<T, N>& b) noexcept {
    basic_vec<T, N> r{};
    for (std::size_t i = 0; i < N; ++i) {
        r[i] = a[i] + b[i];
    }
    return r;
}

template <typename T, std::size_t N>
constexpr basic_vec<T, N> operator-(const basic_vec<T, N>& a,
                                    const basic_vec<T, N>& b) noexcept {
    basic_vec<T, N> r{};
    for (std::size_t i = 0; i < N; ++i) {
        r[i] = a[i] - b[i];
    }
    return r;
}

template <typename T, std::size_t N>
constexpr basic_vec<T, N> operator-(const basic_vec<T, N>& a) noexcept {
    basic_vec<T, N> r{};
    for (std::size_t i = 0; i < N; ++i) {
        r[i] = -a[i];
    }
    return r;
}

template <typename T, std::size_t N>
constexpr basic_vec<T, N> operator*(const basic_vec<T, N>& a, T s) noexcept {
    basic_vec<T, N> r{};
    for (std::size_t i = 0; i < N; ++i) {
        r[i] = a[i] * s;
    }
    return r;
}

template <typename T, std::size_t N>
constexpr basic_vec<T, N> operator*(T s, const basic_vec<T, N>& a) noexcept {
    return a * s;
}

template <typename T, std::size_t N>
constexpr basic_vec<T, N> operator/(const basic_vec<T, N>& a, T s) noexcept {
    basic_vec<T, N> r{};
    for (std::size_t i = 0; i < N; ++i) {
        r[i] = a[i] / s;
    }
    return r;
}

template <typename T, std::size_t N>
constexpr basic_vec<T, N>& operator+=(basic_vec<T, N>& a,
                                      const basic_vec<T, N>& b) noexcept {
    for (std::size_t i = 0; i < N; ++i) {
        a[i] += b[i];
    }
    return a;
}

template <typename T, std::size_t N>
constexpr basic_vec<T, N>& operator-=(basic_vec<T, N>& a,
                                      const basic_vec<T, N>& b) noexcept {
    for (std::size_t i = 0; i < N; ++i) {
        a[i] -= b[i];
    }
    return a;
}

template <typename T, std::size_t N>
constexpr basic_vec<T, N>& operator*=(basic_vec<T, N>& a, T s) noexcept {
    for (std::size_t i = 0; i < N; ++i) {
        a[i] *= s;
    }
    return a;
}

template <typename T, std::size_t N>
constexpr T dot(const basic_vec<T, N>& a, const basic_vec<T, N>& b) noexcept {
    T sum{};
    for (std::size_t i = 0; i < N; ++i) {
        sum += a[i] * b[i];
    }
    return sum;
}

// 2D scalar cross product (the z component of the 3D cross).
template <typename T>
constexpr T cross(const basic_vec<T, 2>& a, const basic_vec<T, 2>& b) noexcept {
    return a.x * b.y - a.y * b.x;
}

// 3D vector cross product.
template <typename T>
constexpr basic_vec<T, 3> cross(const basic_vec<T, 3>& a,
                                const basic_vec<T, 3>& b) noexcept {
    return basic_vec<T, 3>(a.y * b.z - a.z * b.y,
                           a.z * b.x - a.x * b.z,
                           a.x * b.y - a.y * b.x);
}

template <typename T, std::size_t N>
constexpr T length_squared(const basic_vec<T, N>& a) noexcept {
    return dot(a, a);
}

template <typename T, std::size_t N>
inline T length(const basic_vec<T, N>& a) noexcept {
    return static_cast<T>(std::sqrt(static_cast<double>(length_squared(a))));
}

// Returns the zero vector for a zero-length input; callers that need to reject
// a degenerate direction do so explicitly (the ray constructors document it).
template <typename T, std::size_t N>
inline basic_vec<T, N> normalized(const basic_vec<T, N>& a) noexcept {
    const T len = length(a);
    if (len <= static_cast<T>(k_epsilon)) {
        return basic_vec<T, N>{};
    }
    return a / len;
}

template <typename T, std::size_t N>
inline T distance(const basic_vec<T, N>& a, const basic_vec<T, N>& b) noexcept {
    return length(a - b);
}

template <typename T, std::size_t N>
inline bool nearly_equal(const basic_vec<T, N>& a, const basic_vec<T, N>& b,
                         T eps = static_cast<T>(k_epsilon)) noexcept {
    for (std::size_t i = 0; i < N; ++i) {
        if (std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i])) >
            static_cast<double>(eps)) {
            return false;
        }
    }
    return true;
}

template <typename T, std::size_t N>
inline bool is_finite(const basic_vec<T, N>& a) noexcept {
    for (std::size_t i = 0; i < N; ++i) {
        if (!std::isfinite(static_cast<double>(a[i]))) {
            return false;
        }
    }
    return true;
}

// Rotates a 2D vector counter-clockwise by `angle` radians.
inline vec2 rotated(vec2 v, real angle) noexcept {
    const double c = std::cos(static_cast<double>(angle));
    const double s = std::sin(static_cast<double>(angle));
    return vec2(static_cast<real>(static_cast<double>(v.x) * c -
                                  static_cast<double>(v.y) * s),
                static_cast<real>(static_cast<double>(v.x) * s +
                                  static_cast<double>(v.y) * c));
}

// Returns the angle of a 2D vector in (-pi, pi].
inline real angle_of(vec2 v) noexcept {
    return static_cast<real>(std::atan2(static_cast<double>(v.y),
                                        static_cast<double>(v.x)));
}

// Normalises an angle into [0, 2*pi). This is the single place that handles
// angle wrap; sector/cone/spread code must go through it or through the
// equivalent per-shape helper.
inline real normalize_angle(real angle) noexcept {
    const double a = std::fmod(static_cast<double>(angle),
                               static_cast<double>(k_two_pi));
    const double wrapped = a < 0.0 ? a + static_cast<double>(k_two_pi) : a;
    return static_cast<real>(wrapped);
}

// Signed smallest difference b - a, in (-pi, pi]. Used by spread tests and by
// callers that compare directions rather than positions.
inline real angle_difference(real a, real b) noexcept {
    real d = normalize_angle(b) - normalize_angle(a);
    if (d > k_pi) {
        d -= k_two_pi;
    } else if (d < -k_pi) {
        d += k_two_pi;
    }
    return d;
}

inline real clamped_acos(real cosine) noexcept {
    const double c = std::min(1.0, std::max(-1.0, static_cast<double>(cosine)));
    return static_cast<real>(std::acos(c));
}

// The angle between two directions, in [0, pi].
template <typename T, std::size_t N>
inline T angle_between(const basic_vec<T, N>& a,
                       const basic_vec<T, N>& b) noexcept {
    const T la = length(a);
    const T lb = length(b);
    if (la <= static_cast<T>(k_epsilon) || lb <= static_cast<T>(k_epsilon)) {
        return T{};
    }
    return clamped_acos(static_cast<real>(dot(a, b) / (la * lb)));
}

// An orthonormal basis whose `forward` is the given direction. The `right` and
// `up` axes are chosen deterministically so a seeded 3D spread is reproducible
// on every machine. A zero forward falls back to (0,0,1).
struct basis3 {
    vec3 right{1.0f, 0.0f, 0.0f};
    vec3 up{0.0f, 1.0f, 0.0f};
    vec3 forward{0.0f, 0.0f, 1.0f};
};

inline basis3 orthonormal_basis(vec3 forward) noexcept {
    vec3 f = normalized(forward);
    if (length_squared(f) <= k_epsilon * k_epsilon) {
        f = vec3(0.0f, 0.0f, 1.0f);
    }
    // Pick the world axis least aligned with f, then Gram-Schmidt. Comparing
    // absolute components avoids a branch on a threshold that could flip under
    // rounding for 360-degree headings.
    vec3 helper(0.0f, 0.0f, 1.0f);
    const real ax = std::fabs(f.x);
    const real ay = std::fabs(f.y);
    const real az = std::fabs(f.z);
    if (ax <= ay && ax <= az) {
        helper = vec3(1.0f, 0.0f, 0.0f);
    } else if (ay <= ax && ay <= az) {
        helper = vec3(0.0f, 1.0f, 0.0f);
    }
    vec3 right = normalized(cross(helper, f));
    if (length_squared(right) <= k_epsilon * k_epsilon) {
        helper = vec3(1.0f, 0.0f, 0.0f);
        right = normalized(cross(helper, f));
    }
    const vec3 up = normalized(cross(f, right));
    return basis3{right, up, f};
}

// Convenience accessor used by the generic low-level helpers.
template <typename T, std::size_t N>
constexpr basic_vec<T, N> zero_vec() noexcept {
    return basic_vec<T, N>{};
}

static_assert(std::is_same<real, float>::value,
              "crpc::game::geom::real is float by design (see vec.hpp)");

} // namespace geom
} // namespace game
} // namespace crpc

#endif // CRPC_GAME_GEOM_VEC_HPP