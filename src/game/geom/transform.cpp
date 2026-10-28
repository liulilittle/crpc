// CRPC -- 2D affine transform implementation. See transform.hpp.
#include <crpc/game/geom/transform.hpp>

#include <cmath>

namespace crpc {
namespace game {
namespace geom {

transform2d transform2d::identity() noexcept { return transform2d{}; }

transform2d transform2d::translation(vec2 offset) noexcept {
    transform2d t;
    t.origin = offset;
    return t;
}

transform2d transform2d::rotation(real angle) noexcept {
    transform2d t;
    t.basis_x = rotated(vec2(1.0f, 0.0f), angle);
    t.basis_y = rotated(vec2(0.0f, 1.0f), angle);
    return t;
}

transform2d transform2d::scaling(real scale) noexcept {
    return scaling(vec2(scale, scale));
}

transform2d transform2d::scaling(vec2 scale) noexcept {
    transform2d t;
    t.basis_x = vec2(scale.x, 0.0f);
    t.basis_y = vec2(0.0f, scale.y);
    return t;
}

transform2d transform2d::from_trs(vec2 offset, real angle, vec2 scale) noexcept {
    transform2d t = rotation(angle);
    t.basis_x = t.basis_x * scale.x;
    t.basis_y = t.basis_y * scale.y;
    t.origin = offset;
    return t;
}

vec2 transform2d::apply(vec2 point) const noexcept {
    return origin + basis_x * point.x + basis_y * point.y;
}

vec2 transform2d::apply_vector(vec2 direction) const noexcept {
    return basis_x * direction.x + basis_y * direction.y;
}

real transform2d::determinant() const noexcept {
    return basis_x.x * basis_y.y - basis_x.y * basis_y.x;
}

result<transform2d> transform2d::inverse() const {
    const real det = determinant();
    if (std::fabs(static_cast<double>(det)) <=
        static_cast<double>(k_epsilon)) {
        return rpc_error{rpc_code::failed_precondition,
                         "transform2d::inverse: singular matrix"};
    }
    const real inv_det = 1.0f / det;
    transform2d out;
    out.basis_x = vec2(basis_y.y * inv_det, -basis_x.y * inv_det);
    out.basis_y = vec2(-basis_y.x * inv_det, basis_x.x * inv_det);
    // Inverse translation is -M^-1 * origin; evaluate it through the inverse
    // basis just built so there is no second, easy-to-miswrite formula.
    out.origin = vec2(-(out.basis_x.x * origin.x + out.basis_y.x * origin.y),
                      -(out.basis_x.y * origin.x + out.basis_y.y * origin.y));
    return out;
}

transform2d transform2d::compose(const transform2d& other) const noexcept {
    transform2d out;
    out.basis_x = apply_vector(other.basis_x);
    out.basis_y = apply_vector(other.basis_y);
    out.origin = apply(other.origin);
    return out;
}

} // namespace geom
} // namespace game
} // namespace crpc