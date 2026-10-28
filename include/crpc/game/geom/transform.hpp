// CRPC -- 2D affine transforms for hitbox placement.
//
// Representation: a 2x2 linear basis plus a translation, stored as three vec2
// (basis_x, basis_y, origin) -- six floats. A 3x3 homogeneous matrix would
// carry exactly one constant row (0,0,1); the 2x3 split removes that row, so
// composition is six multiply-adds instead of nine and there is no constant to
// drift or be overwritten. The 3D placement used by the shapes is an explicit
// orthonormal basis (`basis3` in vec.hpp), because a 3D hitbox needs a rigid
// orientation, not a general projective transform.
//
// `inverse()` returns `result<transform2d>`; a singular matrix is
// `rpc_code::failed_precondition` (the precondition is a non-zero
// determinant). Nothing throws.
#ifndef CRPC_GAME_GEOM_TRANSFORM_HPP
#define CRPC_GAME_GEOM_TRANSFORM_HPP

#include <crpc/config.hpp>
#include <crpc/error.hpp>

#include "vec.hpp"

namespace crpc {
namespace game {
namespace geom {

struct CRPC_API transform2d {
    vec2 basis_x{1.0f, 0.0f};
    vec2 basis_y{0.0f, 1.0f};
    vec2 origin{0.0f, 0.0f};

    static transform2d identity() noexcept;
    static transform2d translation(vec2 offset) noexcept;
    static transform2d rotation(real angle) noexcept;
    static transform2d scaling(real scale) noexcept;
    static transform2d scaling(vec2 scale) noexcept;
    // Applies scale, then rotation, then translation.
    static transform2d from_trs(vec2 offset, real angle, vec2 scale) noexcept;

    vec2 apply(vec2 point) const noexcept;
    // Applies only the linear part (no translation).
    vec2 apply_vector(vec2 direction) const noexcept;
    real determinant() const noexcept;

    // Returns the inverse, or failed_precondition when the determinant is
    // within `k_epsilon` of zero.
    [[nodiscard]] result<transform2d> inverse() const;

    // this * other: `other` is applied first.
    transform2d compose(const transform2d& other) const noexcept;
};

} // namespace geom
} // namespace game
} // namespace crpc

#endif // CRPC_GAME_GEOM_TRANSFORM_HPP