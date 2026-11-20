#ifndef RESONANCE_REFLECTIONS_WET_POLICY_H
#define RESONANCE_REFLECTIONS_WET_POLICY_H

namespace resonance {

/// Re-enabling the reverb send then ramps up from 0. -1 would skip the first-block ramp and click on unmute.
inline void reflections_wet_reset_prev_levels_on_disable(float& prev_conv_reflections_mix_level,
                                                         float& prev_parametric_reflections_mix_level) {
    prev_conv_reflections_mix_level = 0.0f;
    prev_parametric_reflections_mix_level = 0.0f;
}

} // namespace resonance

#endif // RESONANCE_REFLECTIONS_WET_POLICY_H
