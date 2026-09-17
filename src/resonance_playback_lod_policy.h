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

} // namespace resonance

#endif // RESONANCE_PLAYBACK_LOD_POLICY_H
