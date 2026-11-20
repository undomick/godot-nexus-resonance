#ifndef RESONANCE_PLAYBACK_LOD_POLICY_H
#define RESONANCE_PLAYBACK_LOD_POLICY_H

namespace resonance {

/// When full playback-parameter LOD skips a push, occlusion/transmission/wet gates still need a refresh.
inline bool playback_lod_should_push_coeff_refresh(bool full_playback_params_applied) {
    return !full_playback_params_applied;
}

/// Coeff-refresh must keep source_position/attenuation in sync with HRTF (same fields as full push).
inline bool playback_lod_coeff_refresh_includes_position_attenuation() {
    return true;
}

// A flipped virtual state bypasses the LOD throttle so the mixer and the source simulation follow it at once.
inline bool playback_should_apply_full_params(bool lod_gate_open, bool coeff_smoothing_active,
                                              bool voice_virtual_state_changed) {
    return lod_gate_open || coeff_smoothing_active || voice_virtual_state_changed;
}

} // namespace resonance

#endif // RESONANCE_PLAYBACK_LOD_POLICY_H
