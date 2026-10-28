// CRPC -- anchored hitbox placement. See hitbox.hpp.
#include <crpc/game/geom/hitbox.hpp>

#include <cmath>

namespace crpc {
namespace game {
namespace geom {

namespace {

bool usable_scale(real scale) noexcept {
    return scale > k_epsilon;
}

} // namespace

// --- hitbox (2D) -------------------------------------------------------------

hitbox::hitbox(const circle& shape, anchor_mode mode, vec2 position,
               real rotation, real scale) noexcept
    : shape_(shape), mode_(mode), position_(position), rotation_(rotation),
      scale_(scale) {}

hitbox::hitbox(const sector& shape, anchor_mode mode, vec2 position,
               real rotation, real scale) noexcept
    : shape_(shape), mode_(mode), position_(position), rotation_(rotation),
      scale_(scale) {}

hitbox::hitbox(const umbrella& shape, anchor_mode mode, vec2 position,
               real rotation, real scale) noexcept
    : shape_(shape), mode_(mode), position_(position), rotation_(rotation),
      scale_(scale) {}

hitbox::hitbox(const aabb& shape, anchor_mode mode, vec2 position,
               real rotation, real scale) noexcept
    : shape_(shape), mode_(mode), position_(position), rotation_(rotation),
      scale_(scale) {}

hitbox::hitbox(const obb& shape, anchor_mode mode, vec2 position,
               real rotation, real scale) noexcept
    : shape_(shape), mode_(mode), position_(position), rotation_(rotation),
      scale_(scale) {}

hitbox::hitbox(const ray& shape, anchor_mode mode, vec2 position,
               real rotation, real scale) noexcept
    : shape_(shape), mode_(mode), position_(position), rotation_(rotation),
      scale_(scale) {}

hitbox::hitbox(const segment& shape, anchor_mode mode, vec2 position,
               real rotation, real scale) noexcept
    : shape_(shape), mode_(mode), position_(position), rotation_(rotation),
      scale_(scale) {}

hitbox::hitbox(const line& shape, anchor_mode mode, vec2 position,
               real rotation, real scale) noexcept
    : shape_(shape), mode_(mode), position_(position), rotation_(rotation),
      scale_(scale) {}

hitbox::hitbox(const polygon& shape, anchor_mode mode, vec2 position,
               real rotation, real scale) noexcept
    : shape_(shape), mode_(mode), position_(position), rotation_(rotation),
      scale_(scale) {}

vec2 hitbox::local_anchor() const noexcept {
    return std::visit(
        [this](const auto& shape) { return anchor_point(shape, mode_); },
        shape_);
}

bool hitbox::contains(vec2 world_point) const noexcept {
    if (!usable_scale(scale_)) {
        return false;
    }
    const vec2 anchor = local_anchor();
    const vec2 local =
        anchor + rotated((world_point - position_) / scale_, -rotation_);
    return std::visit([&](const auto& shape) { return shape.contains(local); },
                      shape_);
}

bool hitbox::broad_phase(vec2 world_point) const noexcept {
    return world_bounds().contains(world_point);
}

bool hitbox::broad_phase(const aabb& world_box) const noexcept {
    return world_bounds().overlaps(world_box);
}

aabb hitbox::world_bounds() const noexcept {
    if (!usable_scale(scale_)) {
        return aabb::empty();
    }
    const aabb local = std::visit(
        [](const auto& shape) { return shape.bounds(); }, shape_);
    if (!local.valid()) {
        return aabb::empty();
    }
    if (!local.finite()) {
        return aabb::unbounded();
    }
    const vec2 anchor = local_anchor();
    aabb world;
    for (std::size_t i = 0; i < 4; ++i) {
        const vec2 corner(
            (i & 1U) != 0U ? local.max.x : local.min.x,
            (i & 2U) != 0U ? local.max.y : local.min.y);
        const vec2 mapped =
            position_ +
            rotated((corner - anchor) * scale_, rotation_);
        world.expand(mapped);
    }
    return world;
}

const shape2d_variant& hitbox::shape() const noexcept { return shape_; }

anchor_mode hitbox::mode() const noexcept { return mode_; }

vec2 hitbox::position() const noexcept { return position_; }

real hitbox::rotation() const noexcept { return rotation_; }

real hitbox::scale() const noexcept { return scale_; }

vec2 hitbox::world_anchor() const noexcept { return position_; }

// --- hitbox3 (3D) ------------------------------------------------------------

hitbox3::hitbox3(const sphere& shape, anchor_mode mode, vec3 position,
                 basis3 orientation, real scale) noexcept
    : shape_(shape), mode_(mode), position_(position),
      orientation_(orientation), scale_(scale) {}

hitbox3::hitbox3(const cone& shape, anchor_mode mode, vec3 position,
                 basis3 orientation, real scale) noexcept
    : shape_(shape), mode_(mode), position_(position),
      orientation_(orientation), scale_(scale) {}

hitbox3::hitbox3(const umbrella3& shape, anchor_mode mode, vec3 position,
                 basis3 orientation, real scale) noexcept
    : shape_(shape), mode_(mode), position_(position),
      orientation_(orientation), scale_(scale) {}

hitbox3::hitbox3(const aabb3& shape, anchor_mode mode, vec3 position,
                 basis3 orientation, real scale) noexcept
    : shape_(shape), mode_(mode), position_(position),
      orientation_(orientation), scale_(scale) {}

hitbox3::hitbox3(const obb3& shape, anchor_mode mode, vec3 position,
                 basis3 orientation, real scale) noexcept
    : shape_(shape), mode_(mode), position_(position),
      orientation_(orientation), scale_(scale) {}

hitbox3::hitbox3(const ray3& shape, anchor_mode mode, vec3 position,
                 basis3 orientation, real scale) noexcept
    : shape_(shape), mode_(mode), position_(position),
      orientation_(orientation), scale_(scale) {}

hitbox3::hitbox3(const segment3& shape, anchor_mode mode, vec3 position,
                 basis3 orientation, real scale) noexcept
    : shape_(shape), mode_(mode), position_(position),
      orientation_(orientation), scale_(scale) {}

hitbox3::hitbox3(const line3& shape, anchor_mode mode, vec3 position,
                 basis3 orientation, real scale) noexcept
    : shape_(shape), mode_(mode), position_(position),
      orientation_(orientation), scale_(scale) {}

hitbox3::hitbox3(const convex_polyhedron& shape, anchor_mode mode, vec3 position,
                 basis3 orientation, real scale) noexcept
    : shape_(shape), mode_(mode), position_(position),
      orientation_(orientation), scale_(scale) {}

vec3 hitbox3::local_anchor() const noexcept {
    return std::visit(
        [this](const auto& shape) { return anchor_point(shape, mode_); },
        shape_);
}

bool hitbox3::contains(vec3 world_point) const noexcept {
    if (!usable_scale(scale_)) {
        return false;
    }
    const vec3 anchor = local_anchor();
    const vec3 delta = (world_point - position_) / scale_;
    const vec3 local_offset(dot(delta, orientation_.right),
                            dot(delta, orientation_.up),
                            dot(delta, orientation_.forward));
    const vec3 local = anchor + local_offset;
    return std::visit([&](const auto& shape) { return shape.contains(local); },
                      shape_);
}

bool hitbox3::broad_phase(vec3 world_point) const noexcept {
    return world_bounds().contains(world_point);
}

bool hitbox3::broad_phase(const aabb3& world_box) const noexcept {
    return world_bounds().overlaps(world_box);
}

aabb3 hitbox3::world_bounds() const noexcept {
    if (!usable_scale(scale_)) {
        return aabb3::empty();
    }
    const aabb3 local = std::visit(
        [](const auto& shape) { return shape.bounds(); }, shape_);
    if (!local.valid()) {
        return aabb3::empty();
    }
    if (!local.finite()) {
        return aabb3::unbounded();
    }
    const vec3 anchor = local_anchor();
    aabb3 world;
    for (std::size_t i = 0; i < 8; ++i) {
        const vec3 corner((i & 1U) != 0U ? local.max.x : local.min.x,
                          (i & 2U) != 0U ? local.max.y : local.min.y,
                          (i & 4U) != 0U ? local.max.z : local.min.z);
        const vec3 offset = (corner - anchor) * scale_;
        const vec3 mapped = position_ + orientation_.right * offset.x +
                            orientation_.up * offset.y +
                            orientation_.forward * offset.z;
        world.expand(mapped);
    }
    return world;
}

const shape3d_variant& hitbox3::shape() const noexcept { return shape_; }

anchor_mode hitbox3::mode() const noexcept { return mode_; }

vec3 hitbox3::position() const noexcept { return position_; }

basis3 hitbox3::orientation() const noexcept { return orientation_; }

real hitbox3::scale() const noexcept { return scale_; }

vec3 hitbox3::world_anchor() const noexcept { return position_; }

} // namespace geom
} // namespace game
} // namespace crpc