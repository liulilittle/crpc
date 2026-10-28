// CRPC -- shotgun spread and the instant/projectile attack semantics.
//
// ---------------------------------------------------------------------------
// Spread
// ---------------------------------------------------------------------------
// `spread_config` fires `projectiles` pellets inside a total cone of
// `total_angle` radians centred on the forward direction.
//
//   2D: a planar angular fan in [-total/2, +total/2] around the forward
//       heading. Uniform mode spaces the angles evenly (endpoints included);
//       random mode draws them from a seeded PRNG.
//   3D: a spherical cap (solid-angle cone), NOT a flat fan. Uniform mode uses a
//       deterministic Fibonacci-style cap distribution (uniform in solid
//       angle, no duplicates); random mode samples z uniformly in
//       [cos(half), 1] and phi uniformly in [0, 2*pi), which is the standard
//       uniform distribution over a spherical cap.
//
// Both modes are deterministic: the uniform distribution ignores the seed by
// construction, and the random distribution is a pure function of the seed, so
// a replay or a server can regenerate the exact same pellets. `projectiles ==
// 0` yields an empty list; a negative or larger-than-2*pi total angle is
// `invalid_argument`.
//
// ---------------------------------------------------------------------------
// Instant vs projectile ("speed + distance")
// ---------------------------------------------------------------------------
// The user-facing rule is: no speed means the attack is instant; speed means
// travel time matters. That is encoded by `attack_profile::speed`:
//
//   speed <= 0  -> instant/hitscan. The ray is evaluated against the target's
//                  CURRENT position with no travel-time limit. Distance does
//                  not gate the hit: an instant attack has already arrived.
//
//   speed > 0   -> projectile. The projectile travels at `speed` and the only
//                  real deadline is `max_lifetime`:
//                      arrival_time t = distance / speed
//                      hit requires t <= max_lifetime
//                  This is deliberately NOT a "clip the range to speed *
//                  lifetime and then test a static target" shortcut. The
//                  target may move, so the solver intersects
//                      |target.position + target.velocity * t
//                       - (origin + projectile_direction * speed * t)| <= radius
//                  and returns the smallest t >= 0. If no positive root
//                  exists (the projectile cannot catch the target) or the root
//                  is beyond max_lifetime, the result is `not_found`: a clear
//                  miss, never a fabricated hit.
//
// Leading is the same quadratic with the target velocity in it. The tests
// contrast (a) firing at the target's current position (no lead -> miss) with
// (b) `projectile_intercept*`, which solves for the aim direction that meets
// the moving target (lead -> hit). A stationary target reduces to
// t = distance / speed, and speed <= 0 dispatches to the instant path, so both
// modes agree on the static in-range scenario.
//
// All angles are radians and every direction is an arbitrary unit vector, so
// headings at 0/90/180/270/350/359.9 degrees and across the 0-degree wrap are
// represented without special cases.
#ifndef CRPC_GAME_GEOM_ATTACK_HPP
#define CRPC_GAME_GEOM_ATTACK_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

#include <crpc/config.hpp>
#include <crpc/error.hpp>

#include "vec.hpp"

namespace crpc {
namespace game {
namespace geom {

struct CRPC_API spread_config {
    std::size_t projectiles = 1;
    real total_angle = 0.0f; // full cone angle in radians, in [0, 2*pi]
    bool uniform = true;     // false draws seeded random directions
    std::uint64_t seed = 0;  // ignored when uniform == true
};

[[nodiscard]] CRPC_API result<std::vector<vec2>>
spread_directions2(vec2 forward, const spread_config& config);

[[nodiscard]] CRPC_API result<std::vector<vec3>>
spread_directions3(vec3 forward, const spread_config& config);

// --- Attack targets and results ---------------------------------------------

struct CRPC_API attack_target2 {
    vec2 position{};
    vec2 velocity{};
    real radius = 0.0f;
};

struct CRPC_API attack_target3 {
    vec3 position{};
    vec3 velocity{};
    real radius = 0.0f;
};

// speed <= 0 selects the instant path; max_lifetime is only consulted on the
// projectile path. max_lifetime must be >= 0.
struct CRPC_API attack_profile {
    real speed = 0.0f;
    real max_lifetime = 0.0f;
};

struct CRPC_API hit_info2 {
    real time = 0.0f;   // seconds (0 for instant hits)
    vec2 point{};       // impact point
    vec2 direction{};   // the projectile/aim direction that produced the hit
};

struct CRPC_API hit_info3 {
    real time = 0.0f;
    vec3 point{};
    vec3 direction{};
};

// --- Instant / hitscan -------------------------------------------------------

[[nodiscard]] CRPC_API result<hit_info2>
instant_hit2(vec2 origin, vec2 direction, const attack_target2& target);

[[nodiscard]] CRPC_API result<hit_info3>
instant_hit3(vec3 origin, vec3 direction, const attack_target3& target);

// --- Projectile along a fixed direction (velocity-aware) --------------------

// Returns the first time the projectile's swept point enters the target sphere,
// checked against max_lifetime. The target's velocity is included, so this is
// already leading-capable when the caller supplies a leading direction.
[[nodiscard]] CRPC_API result<hit_info2>
projectile_fire2(vec2 origin, vec2 direction, const attack_target2& target,
                  real speed, real max_lifetime);

[[nodiscard]] CRPC_API result<hit_info3>
projectile_fire3(vec3 origin, vec3 direction, const attack_target3& target,
                  real speed, real max_lifetime);

// --- Projectile auto-aim / leading ------------------------------------------

// Solves for the aim direction that intercepts the moving target with the
// given projectile speed, and returns that intercept (time, point, direction).
// No solution (including "the projectile is slower than the target moving
// away") is `not_found`.
[[nodiscard]] CRPC_API result<hit_info2>
projectile_intercept2(vec2 origin, const attack_target2& target, real speed,
                      real max_lifetime);

[[nodiscard]] CRPC_API result<hit_info3>
projectile_intercept3(vec3 origin, const attack_target3& target, real speed,
                      real max_lifetime);

// --- Mode dispatch -----------------------------------------------------------

// speed <= 0 -> instant_hit; speed > 0 -> projectile_fire. This is the single
// entry point that implements "no speed means instant".
[[nodiscard]] CRPC_API result<hit_info2>
attack_hit2(vec2 origin, vec2 direction, const attack_target2& target,
            const attack_profile& profile);

[[nodiscard]] CRPC_API result<hit_info3>
attack_hit3(vec3 origin, vec3 direction, const attack_target3& target,
            const attack_profile& profile);

} // namespace geom
} // namespace game
} // namespace crpc

#endif // CRPC_GAME_GEOM_ATTACK_HPP