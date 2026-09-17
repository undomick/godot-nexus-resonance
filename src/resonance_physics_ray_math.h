#ifndef RESONANCE_PHYSICS_RAY_MATH_H
#define RESONANCE_PHYSICS_RAY_MATH_H

#include "resonance_constants.h"

namespace resonance {

/// Ray parameter [code]t[/code] along [code]origin + direction * t[/code] where Godot [code]intersect_ray[/code] should start
/// for Custom-scene [code]anyHit[/code] and [code]closestHit[/code] (avoids immediate inside-collider hits at the listener;
/// same start-t for occlusion and transmission, matching Custom scene raycast).
inline float custom_scene_occlusion_ray_start_t(float min_distance, float max_distance) {
    float t_from = min_distance;
    if (max_distance > min_distance) {
        const float padded = min_distance + kCustomSceneOcclusionRayStartEpsilon;
        if (padded < max_distance)
            t_from = padded;
    }
    return t_from;
}

} // namespace resonance

#endif
