#ifndef RESONANCE_PATHING_SKIP_POLICY_H
#define RESONANCE_PATHING_SKIP_POLICY_H

#include "resonance_reflection_skip_policy.h"
#include <cstdint>
#include <phonon.h>

namespace resonance {

// Everything a pathing pass reads. Per-source pathing settings and probe batch assignments are part of
// the source inputs generation; the deviation generation counts changes to the shared deviation model.
struct PathingPassInputs {
    uint64_t source_inputs_generation = 0;
    uint64_t scene_generation = 0;
    uint64_t probe_batch_generation = 0;
    uint64_t deviation_generation = 0;
    IPLCoordinateSpace3 listener{};
    bool debug_vis = false;
};

inline bool pathing_pass_inputs_equal(const PathingPassInputs& a, const PathingPassInputs& b) {
    return a.source_inputs_generation == b.source_inputs_generation && a.scene_generation == b.scene_generation &&
           a.probe_batch_generation == b.probe_batch_generation && a.deviation_generation == b.deviation_generation &&
           coordinate_spaces_equal(a.listener, b.listener) && a.debug_vis == b.debug_vis;
}

// A custom scene validates paths against the physics world, which can change without a scene commit.
inline bool pathing_pass_can_skip(bool have_last_pass, const PathingPassInputs& last, const PathingPassInputs& next,
                                  bool custom_scene) {
    if (!have_last_pass || custom_scene)
        return false;
    return pathing_pass_inputs_equal(last, next);
}

} // namespace resonance

#endif // RESONANCE_PATHING_SKIP_POLICY_H
