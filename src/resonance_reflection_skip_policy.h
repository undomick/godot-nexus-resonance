#ifndef RESONANCE_REFLECTION_SKIP_POLICY_H
#define RESONANCE_REFLECTION_SKIP_POLICY_H

#include <cstdint>
#include <phonon.h>

namespace resonance {

// Everything a reflections pass reads. The generations count changes the worker cannot compare
// directly: source inputs, committed scene geometry and loaded probe batches.
struct ReflectionPassInputs {
    uint64_t source_inputs_generation = 0;
    uint64_t scene_generation = 0;
    uint64_t probe_batch_generation = 0;
    IPLCoordinateSpace3 listener{};
    int num_rays = 0;
    int num_bounces = 0;
    float duration = 0.0f;
    int order = 0;
    float irradiance_min_distance = 0.0f;
};

inline bool vectors_equal(const IPLVector3& a, const IPLVector3& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

inline bool coordinate_spaces_equal(const IPLCoordinateSpace3& a, const IPLCoordinateSpace3& b) {
    return vectors_equal(a.origin, b.origin) && vectors_equal(a.ahead, b.ahead) && vectors_equal(a.up, b.up) &&
           vectors_equal(a.right, b.right);
}

inline bool reflection_pass_inputs_equal(const ReflectionPassInputs& a, const ReflectionPassInputs& b) {
    return a.source_inputs_generation == b.source_inputs_generation && a.scene_generation == b.scene_generation &&
           a.probe_batch_generation == b.probe_batch_generation && coordinate_spaces_equal(a.listener, b.listener) &&
           a.num_rays == b.num_rays && a.num_bounces == b.num_bounces && a.duration == b.duration &&
           a.order == b.order && a.irradiance_min_distance == b.irradiance_min_distance;
}

// A custom scene traces against the physics world, which can change without a scene commit.
inline bool reflection_pass_can_skip(bool have_last_pass, const ReflectionPassInputs& last,
                                     const ReflectionPassInputs& next, bool custom_scene) {
    if (!have_last_pass || custom_scene)
        return false;
    return reflection_pass_inputs_equal(last, next);
}

} // namespace resonance

#endif // RESONANCE_REFLECTION_SKIP_POLICY_H
