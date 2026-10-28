// CRPC -- shotgun spread, instant and projectile attacks. See attack.hpp.
#include <crpc/game/geom/attack.hpp>

#include <algorithm>
#include <cmath>

#include "geometry_detail.hpp"

namespace crpc {
namespace game {
namespace geom {

namespace {

// splitmix64: tiny, fast and bit-for-bit reproducible. The spread functions
// are pure functions of the seed, which is what replay/server validation
// needs; no global state and no std::random_device.
std::uint64_t splitmix64(std::uint64_t& state) noexcept {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

double uniform01(std::uint64_t& state) noexcept {
    return static_cast<double>(splitmix64(state) >> 11) /
           9007199254740992.0;
}

template <typename V>
struct raw_hit {
    real time = 0.0f;
    V point{};
    V direction{};
};

template <typename V>
V axis_x_vector() noexcept {
    V v{};
    v[0] = 1.0f;
    return v;
}

template <typename V>
V direction_from_axis(const basis3& basis, double theta, double phi) noexcept {
    const double ct = std::cos(theta);
    const double st = std::sin(theta);
    const double cp = std::cos(phi);
    const double sp = std::sin(phi);
    return basis.forward * static_cast<real>(ct) +
           basis.right * static_cast<real>(st * cp) +
           basis.up * static_cast<real>(st * sp);
}

template <typename V>
result<raw_hit<V>> instant_generic(V origin, V direction, V target_position,
                                   real target_radius) {
    V u = normalized(direction);
    if (length_squared(u) <= k_epsilon * k_epsilon) {
        return rpc_error{rpc_code::invalid_argument,
                         "instant_hit: zero direction"};
    }
    const double radius =
        std::max(static_cast<double>(target_radius), 0.0);
    const V oc = origin - target_position;
    const double rr = static_cast<double>(dot(oc, oc));
    if (rr <= radius * radius + static_cast<double>(k_epsilon)) {
        raw_hit<V> hit;
        hit.time = 0.0f;
        hit.point = target_position;
        hit.direction = u;
        return hit;
    }
    if (radius <= 0.0) {
        const V d = target_position - origin;
        const double t = static_cast<double>(dot(d, u));
        const V perpendicular = d - u * static_cast<real>(t);
        if (t < 0.0 ||
            static_cast<double>(length(perpendicular)) >
                static_cast<double>(k_epsilon)) {
            return rpc_error{rpc_code::not_found,
                             "instant_hit: target is not on the ray"};
        }
        raw_hit<V> hit;
        hit.time = 0.0f;
        hit.point = target_position;
        hit.direction = u;
        return hit;
    }
    const double b = static_cast<double>(dot(oc, u));
    const double c = rr - radius * radius;
    const double disc = b * b - c;
    if (disc < 0.0) {
        return rpc_error{rpc_code::not_found,
                         "instant_hit: ray misses the target"};
    }
    const double root = std::sqrt(std::max(0.0, disc));
    const double t0 = -b - root;
    const double t1 = -b + root;
    const double t = t0 >= 0.0 ? t0 : t1;
    if (t < 0.0) {
        return rpc_error{rpc_code::not_found,
                         "instant_hit: target is behind the ray"};
    }
    raw_hit<V> hit;
    hit.time = 0.0f;
    hit.point = target_position;
    hit.direction = u;
    return hit;
}

template <typename V>
result<raw_hit<V>> projectile_fire_generic(V origin, V direction,
                                           V target_position,
                                           V target_velocity,
                                           real target_radius, real speed,
                                           real max_lifetime) {
    if (speed <= 0.0f || !detail::finite_real(speed)) {
        return rpc_error{rpc_code::invalid_argument,
                         "projectile_fire: speed must be positive"};
    }
    if (!detail::finite_real(max_lifetime) || max_lifetime < 0.0f) {
        return rpc_error{rpc_code::invalid_argument,
                         "projectile_fire: invalid max_lifetime"};
    }
    const V u = normalized(direction);
    if (length_squared(u) <= k_epsilon * k_epsilon) {
        return rpc_error{rpc_code::invalid_argument,
                         "projectile_fire: zero direction"};
    }
    const double radius =
        std::max(static_cast<double>(target_radius), 0.0);
    const double lifetime = static_cast<double>(max_lifetime);
    const V relative_position = target_position - origin;
    const V relative_velocity = target_velocity - u * speed;
    const double rr = static_cast<double>(dot(relative_position,
                                              relative_position));
    if (rr <= radius * radius + static_cast<double>(k_epsilon)) {
        raw_hit<V> hit;
        hit.time = 0.0f;
        hit.point = target_position;
        hit.direction = u;
        return hit;
    }
    const double a = static_cast<double>(
        dot(relative_velocity, relative_velocity));
    const double b = static_cast<double>(
        dot(relative_position, relative_velocity));
    const double c = rr - radius * radius;
    double t = -1.0;
    if (a <= static_cast<double>(k_epsilon)) {
        // Same velocity as the projectile: only an already-overlapping target
        // can be hit (handled above).
        return rpc_error{rpc_code::not_found,
                         "projectile_fire: projectile cannot close the gap"};
    }
    const double disc = b * b - a * c;
    if (disc < -static_cast<double>(k_epsilon)) {
        return rpc_error{rpc_code::not_found,
                         "projectile_fire: no intercept"};
    }
    const double root = std::sqrt(std::max(0.0, disc));
    const double t0 = (-b - root) / a;
    const double t1 = (-b + root) / a;
    if (t0 >= -static_cast<double>(k_epsilon)) {
        t = std::max(0.0, t0);
    } else if (t1 >= -static_cast<double>(k_epsilon)) {
        t = std::max(0.0, t1);
    } else {
        return rpc_error{rpc_code::not_found,
                         "projectile_fire: intercept is in the past"};
    }
    if (t > lifetime + static_cast<double>(k_epsilon)) {
        return rpc_error{rpc_code::not_found,
                         "projectile_fire: target is beyond max_lifetime"};
    }
    raw_hit<V> hit;
    hit.time = static_cast<real>(t);
    hit.point = origin + u * (speed * static_cast<real>(t));
    hit.direction = u;
    return hit;
}

template <typename V>
result<raw_hit<V>> projectile_intercept_generic(V origin, V target_position,
                                                V target_velocity,
                                                real target_radius, real speed,
                                                real max_lifetime) {
    if (speed <= 0.0f || !detail::finite_real(speed)) {
        return rpc_error{rpc_code::invalid_argument,
                         "projectile_intercept: speed must be positive"};
    }
    if (!detail::finite_real(max_lifetime) || max_lifetime < 0.0f) {
        return rpc_error{rpc_code::invalid_argument,
                         "projectile_intercept: invalid max_lifetime"};
    }
    const double lifetime = static_cast<double>(max_lifetime);
    const V r = target_position - origin;
    const double rr = static_cast<double>(dot(r, r));
    const double radius =
        std::max(static_cast<double>(target_radius), 0.0);
    if (rr <= radius * radius + static_cast<double>(k_epsilon)) {
        const V fallback =
            length_squared(target_velocity) > k_epsilon * k_epsilon
                ? normalized(target_velocity)
                : normalized(r);
        raw_hit<V> hit;
        hit.time = 0.0f;
        hit.point = target_position;
        hit.direction = length_squared(fallback) > 0.0f
                            ? fallback
                            : axis_x_vector<V>();
        return hit;
    }
    const double a =
        static_cast<double>(dot(target_velocity, target_velocity)) -
        static_cast<double>(speed) * static_cast<double>(speed);
    const double b =
        2.0 * static_cast<double>(dot(r, target_velocity));
    const double c = rr;
    double t = -1.0;
    if (std::fabs(a) <= static_cast<double>(k_epsilon)) {
        if (std::fabs(b) <= static_cast<double>(k_epsilon)) {
            return rpc_error{rpc_code::not_found,
                             "projectile_intercept: target cannot be caught"};
        }
        const double candidate = -c / b;
        if (candidate < -static_cast<double>(k_epsilon)) {
            return rpc_error{rpc_code::not_found,
                             "projectile_intercept: intercept is in the past"};
        }
        t = std::max(0.0, candidate);
    } else {
        const double disc = b * b - 4.0 * a * c;
        if (disc < -static_cast<double>(k_epsilon)) {
            return rpc_error{rpc_code::not_found,
                             "projectile_intercept: no real solution"};
        }
        const double root = std::sqrt(std::max(0.0, disc));
        double t0 = (-b - root) / (2.0 * a);
        double t1 = (-b + root) / (2.0 * a);
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        if (t0 >= -static_cast<double>(k_epsilon)) {
            t = std::max(0.0, t0);
        } else if (t1 >= -static_cast<double>(k_epsilon)) {
            t = std::max(0.0, t1);
        } else {
            return rpc_error{rpc_code::not_found,
                             "projectile_intercept: intercept is in the past"};
        }
    }
    if (t > lifetime + static_cast<double>(k_epsilon)) {
        return rpc_error{rpc_code::not_found,
                         "projectile_intercept: intercept is beyond max_lifetime"};
    }
    const V predicted = target_position + target_velocity * static_cast<real>(t);
    const V direction = normalized(predicted - origin);
    if (length_squared(direction) <= 0.0f) {
        return rpc_error{rpc_code::not_found,
                         "projectile_intercept: degenerate aim direction"};
    }
    raw_hit<V> hit;
    hit.time = static_cast<real>(t);
    hit.point = predicted;
    hit.direction = direction;
    return hit;
}

} // namespace

// --- Spread ------------------------------------------------------------------

result<std::vector<vec2>> spread_directions2(vec2 forward,
                                             const spread_config& config) {
    if (!detail::finite_real(config.total_angle) ||
        config.total_angle < 0.0f ||
        config.total_angle > k_two_pi + k_epsilon) {
        return rpc_error{rpc_code::invalid_argument,
                         "spread_directions2: total_angle outside [0, 2*pi]"};
    }
    std::vector<vec2> out;
    if (config.projectiles == 0) {
        return out;
    }
    const vec2 base = normalized(forward);
    if (length_squared(base) <= k_epsilon * k_epsilon) {
        return rpc_error{rpc_code::invalid_argument,
                         "spread_directions2: zero forward direction"};
    }
    out.reserve(config.projectiles);
    const double half = static_cast<double>(config.total_angle) * 0.5;
    if (config.uniform) {
        if (config.projectiles == 1) {
            out.push_back(base);
            return out;
        }
        const double step =
            (2.0 * half) /
            static_cast<double>(config.projectiles - 1);
        for (std::size_t i = 0; i < config.projectiles; ++i) {
            const double offset =
                -half + step * static_cast<double>(i);
            out.push_back(rotated(base, static_cast<real>(offset)));
        }
        return out;
    }
    std::uint64_t state = config.seed;
    for (std::size_t i = 0; i < config.projectiles; ++i) {
        const double offset = (2.0 * uniform01(state) - 1.0) * half;
        out.push_back(rotated(base, static_cast<real>(offset)));
    }
    return out;
}

result<std::vector<vec3>> spread_directions3(vec3 forward,
                                             const spread_config& config) {
    if (!detail::finite_real(config.total_angle) ||
        config.total_angle < 0.0f ||
        config.total_angle > k_two_pi + k_epsilon) {
        return rpc_error{rpc_code::invalid_argument,
                         "spread_directions3: total_angle outside [0, 2*pi]"};
    }
    std::vector<vec3> out;
    if (config.projectiles == 0) {
        return out;
    }
    if (length_squared(forward) <= k_epsilon * k_epsilon) {
        return rpc_error{rpc_code::invalid_argument,
                         "spread_directions3: zero forward direction"};
    }
    const basis3 basis = orthonormal_basis(forward);
    const double half = static_cast<double>(config.total_angle) * 0.5;
    const double cos_half = std::cos(std::min(half, static_cast<double>(k_pi)));
    out.reserve(config.projectiles);
    if (config.uniform) {
        if (config.projectiles == 1) {
            out.push_back(basis.forward);
            return out;
        }
        // Deterministic Fibonacci-style distribution over the spherical cap:
        // uniform in solid angle, no two pellets share a direction.
        constexpr double golden_angle = 2.39996322972865332;
        for (std::size_t i = 0; i < config.projectiles; ++i) {
            const double fraction =
                (static_cast<double>(i) + 0.5) /
                static_cast<double>(config.projectiles);
            const double z = 1.0 - (1.0 - cos_half) * fraction;
            const double theta = std::acos(
                detail::clamp_double(z, -1.0, 1.0));
            const double phi = golden_angle * static_cast<double>(i);
            out.push_back(direction_from_axis<vec3>(basis, theta, phi));
        }
        return out;
    }
    std::uint64_t state = config.seed;
    for (std::size_t i = 0; i < config.projectiles; ++i) {
        const double z =
            cos_half + (1.0 - cos_half) * uniform01(state);
        const double theta =
            std::acos(detail::clamp_double(z, -1.0, 1.0));
        const double phi =
            2.0 * static_cast<double>(k_pi) * uniform01(state);
        out.push_back(direction_from_axis<vec3>(basis, theta, phi));
    }
    return out;
}

// --- Instant / projectile ----------------------------------------------------

result<hit_info2> instant_hit2(vec2 origin, vec2 direction,
                               const attack_target2& target) {
    const auto raw = instant_generic(origin, direction, target.position,
                                     target.radius);
    if (!raw.ok()) {
        return raw.error();
    }
    hit_info2 hit;
    hit.time = raw.value().time;
    hit.point = raw.value().point;
    hit.direction = raw.value().direction;
    return hit;
}

result<hit_info3> instant_hit3(vec3 origin, vec3 direction,
                               const attack_target3& target) {
    const auto raw = instant_generic(origin, direction, target.position,
                                     target.radius);
    if (!raw.ok()) {
        return raw.error();
    }
    hit_info3 hit;
    hit.time = raw.value().time;
    hit.point = raw.value().point;
    hit.direction = raw.value().direction;
    return hit;
}

result<hit_info2> projectile_fire2(vec2 origin, vec2 direction,
                                   const attack_target2& target, real speed,
                                   real max_lifetime) {
    const auto raw = projectile_fire_generic(
        origin, direction, target.position, target.velocity, target.radius,
        speed, max_lifetime);
    if (!raw.ok()) {
        return raw.error();
    }
    hit_info2 hit;
    hit.time = raw.value().time;
    hit.point = raw.value().point;
    hit.direction = raw.value().direction;
    return hit;
}

result<hit_info3> projectile_fire3(vec3 origin, vec3 direction,
                                   const attack_target3& target, real speed,
                                   real max_lifetime) {
    const auto raw = projectile_fire_generic(
        origin, direction, target.position, target.velocity, target.radius,
        speed, max_lifetime);
    if (!raw.ok()) {
        return raw.error();
    }
    hit_info3 hit;
    hit.time = raw.value().time;
    hit.point = raw.value().point;
    hit.direction = raw.value().direction;
    return hit;
}

result<hit_info2> projectile_intercept2(vec2 origin,
                                        const attack_target2& target,
                                        real speed, real max_lifetime) {
    const auto raw = projectile_intercept_generic(
        origin, target.position, target.velocity, target.radius, speed,
        max_lifetime);
    if (!raw.ok()) {
        return raw.error();
    }
    hit_info2 hit;
    hit.time = raw.value().time;
    hit.point = raw.value().point;
    hit.direction = raw.value().direction;
    return hit;
}

result<hit_info3> projectile_intercept3(vec3 origin,
                                        const attack_target3& target,
                                        real speed, real max_lifetime) {
    const auto raw = projectile_intercept_generic(
        origin, target.position, target.velocity, target.radius, speed,
        max_lifetime);
    if (!raw.ok()) {
        return raw.error();
    }
    hit_info3 hit;
    hit.time = raw.value().time;
    hit.point = raw.value().point;
    hit.direction = raw.value().direction;
    return hit;
}

// --- Mode dispatch -----------------------------------------------------------

result<hit_info2> attack_hit2(vec2 origin, vec2 direction,
                              const attack_target2& target,
                              const attack_profile& profile) {
    if (profile.speed <= 0.0f) {
        return instant_hit2(origin, direction, target);
    }
    return projectile_fire2(origin, direction, target, profile.speed,
                            profile.max_lifetime);
}

result<hit_info3> attack_hit3(vec3 origin, vec3 direction,
                              const attack_target3& target,
                              const attack_profile& profile) {
    if (profile.speed <= 0.0f) {
        return instant_hit3(origin, direction, target);
    }
    return projectile_fire3(origin, direction, target, profile.speed,
                            profile.max_lifetime);
}

} // namespace geom
} // namespace game
} // namespace crpc