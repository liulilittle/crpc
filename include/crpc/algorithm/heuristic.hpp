// CRPC -- grid heuristics for A* search.
//
// Each function measures the distance between two points (x1,y1) and (x2,y2)
// on a grid. A* in this library takes a heuristic as an open callable, so
// these are coordinate helpers rather than a fixed graph API; connect one to
// your neighbour/cost functions like the test suite does.
//
// Admissibility (never overestimating the true remaining cost) is what makes
// A* optimal, and each helper is admissible for the connectivity it is named
// for:
//
//   manhattan  -- 4-connected grids (cardinal moves cost 1);
//   euclidean  -- any grid with a Euclidean distance metric;
//   chebyshev  -- 8-connected grids (uniform move cost);
//   octile     -- 8-connected grids where a diagonal move costs sqrt(2)
//                 (this is *exact* for an open board, the tightest admissible
//                 estimate, and provably never overestimates).
#ifndef CRPC_ALGORITHM_HEURISTIC_HPP
#define CRPC_ALGORITHM_HEURISTIC_HPP

#include <cmath>
#include <cstddef>

namespace crpc {
namespace algorithm {

// Absolute difference that is total for both signed and unsigned arithmetic
// (std::abs has no unsigned overload, and a naive `a - b` underflows).
template <typename T>
constexpr T absolute_difference(T a, T b) noexcept {
    return a > b ? a - b : b - a;
}

template <typename T>
constexpr T manhattan_distance(T x1, T y1, T x2, T y2) noexcept {
    return absolute_difference(x1, x2) + absolute_difference(y1, y2);
}

template <typename T>
constexpr T chebyshev_distance(T x1, T y1, T x2, T y2) noexcept {
    const T dx = absolute_difference(x1, x2);
    const T dy = absolute_difference(y1, y2);
    return dx > dy ? dx : dy;
}

template <typename T>
double euclidean_distance(T x1, T y1, T x2, T y2) noexcept {
    const double dx = static_cast<double>(absolute_difference(x1, x2));
    const double dy = static_cast<double>(absolute_difference(y1, y2));
    return std::sqrt(dx * dx + dy * dy);
}

template <typename T>
double octile_distance(T x1, T y1, T x2, T y2) noexcept {
    const double dx = static_cast<double>(absolute_difference(x1, x2));
    const double dy = static_cast<double>(absolute_difference(y1, y2));
    const double major = dx > dy ? dx : dy;
    const double minor = dx > dy ? dy : dx;
    // D=1 cardinal cost, D2=sqrt(2) diagonal: major + (sqrt(2)-1)*minor.
    constexpr double k_sqrt2_minus_one = 0.4142135623730951;
    return major + k_sqrt2_minus_one * minor;
}

} // namespace algorithm
} // namespace crpc

#endif // CRPC_ALGORITHM_HEURISTIC_HPP