#ifndef RESONANCE_PHYSICS_RAY_MATH_H
#define RESONANCE_PHYSICS_RAY_MATH_H

#include "resonance_constants.h"

namespace resonance {

/// Start t for Custom-scene intersect_ray, nudged past min_distance so a ray does not hit the collider it
/// starts in. Occlusion and transmission share it.
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
