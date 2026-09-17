#ifndef RESONANCE_REFLECTIONS_WET_POLICY_H
#define RESONANCE_REFLECTIONS_WET_POLICY_H

namespace resonance {

/// Reset wet-ramp state when reverb send is hard-gated off (output_reverb). Re-enable ramps from 0.
/// Do not use -1 here: that skips the first-block ramp and clicks on unmute.
inline void reflections_wet_reset_prev_levels_on_disable(float& prev_conv_reflections_mix_level,
                                                         float& prev_parametric_reflections_mix_level) {
    prev_conv_reflections_mix_level = 0.0f;
    prev_parametric_reflections_mix_level = 0.0f;
}

} // namespace resonance

#endif // RESONANCE_REFLECTIONS_WET_POLICY_H
