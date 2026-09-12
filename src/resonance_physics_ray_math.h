#ifndef RESONANCE_PHYSICS_RAY_MATH_H
#define RESONANCE_PHYSICS_RAY_MATH_H

#include "resonance_constants.h"

namespace resonance {

/// Ray parameter [code]t[/code] along [code]origin + direction * t[/code] where Godot [code]intersect_ray[/code] should start
/// for Custom-scene occlusion [code]anyHit[/code] (avoids immediate inside-collider hits at the listener).
inline float custom_scene_occlusion_ray_start_t(float min_distance, float max_distance) {
    float t_from = min_distance;
    if (max_distance > min_distance) {
        const float padded = min_distance + kCustomSceneOcclusionRayStartEpsilon;
        if (padded < max_distance)
            t_from = padded;
    }
    return t_from;
}

/// [code]PhysicsRayQueryParameters3D.hit_from_inside[/code] for Custom-scene
/// closest-hit tracing (transmission march, reflection bounces, probe placement).
/// Must stay false: Steam Audio's DirectSimulator::transmission advances
/// min_distance past each hit (~1 cm, kRayOffset), so the next segment starts
/// inside the surface just crossed and expects to reach the NEXT surface.
/// Reporting an inside-start as a hit at distance ~0 makes the march
/// re-accumulate the same collider until transmission collapses to ~0 (all
/// occluders fully opaque). Reflection bounces also place the origin exactly
/// on the hit surface (ray.origin = hitPoint) and rely on no self-hit.
constexpr bool custom_scene_closest_hit_from_inside() {
    return false;
}

/// [code]PhysicsRayQueryParameters3D.hit_from_inside[/code] for Custom-scene
/// any-hit occlusion rays (raycast/volumetric occlusion, probe neighborhood
/// checkOcclusion culling). Must stay false: rays start at the listener or at
/// volumetric source samples, which can sit inside a collider; reporting that
/// as an immediate hit would occlude every ray.
constexpr bool custom_scene_any_hit_from_inside() {
    return false;
}

} // namespace resonance

#endif
