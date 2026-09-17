#ifndef RESONANCE_PROCESSOR_HRTF_POLICY_H
#define RESONANCE_PROCESSOR_HRTF_POLICY_H

#include <phonon.h>

namespace resonance {

/// True when the runtime HRTF handle differs from the one used at IPL effect create (including null vs valid).
inline bool hrtf_identity_changed(IPLHRTF bound, IPLHRTF runtime) {
    return bound != runtime;
}

/// Direct binaural/HOA effects must be created on main when HRTF is valid but the effect object is missing.
inline bool direct_needs_hrtf_effect_create(bool has_binaural_effect, IPLHRTF bound_hrtf, IPLHRTF runtime_hrtf) {
    return runtime_hrtf != nullptr && !has_binaural_effect;
}

/// Recreate HRTF-bound direct effects when create-time HRTF identity no longer matches runtime (SOFA swap, double-buffer).
inline bool direct_needs_hrtf_effect_recreate(bool has_binaural_effect, IPLHRTF bound_hrtf, IPLHRTF runtime_hrtf) {
    return has_binaural_effect && runtime_hrtf != nullptr && bound_hrtf != runtime_hrtf;
}

/// Main-thread sync when create is missing or bound HRTF identity drifted.
inline bool direct_needs_hrtf_main_sync(bool has_binaural_effect, IPLHRTF bound_hrtf, IPLHRTF runtime_hrtf) {
    return direct_needs_hrtf_effect_create(has_binaural_effect, bound_hrtf, runtime_hrtf) ||
           direct_needs_hrtf_effect_recreate(has_binaural_effect, bound_hrtf, runtime_hrtf);
}

/// Path effect is created with spatialize+HRTF; recreate when HRTF was null at create or identity changed.
inline bool path_needs_hrtf_effect_recreate(bool has_path_effect, IPLHRTF bound_hrtf, IPLHRTF runtime_hrtf) {
    if (!has_path_effect)
        return false;
    if (runtime_hrtf == nullptr)
        return false;
    return bound_hrtf != runtime_hrtf;
}

/// After HRTF identity change with existing effects, reset internal filter state (no recreate).
inline bool hrtf_effects_need_reset_after_identity_change(IPLHRTF bound_hrtf, IPLHRTF runtime_hrtf) {
    return bound_hrtf != nullptr && runtime_hrtf != nullptr && bound_hrtf != runtime_hrtf;
}

/// Ambisonic decode/rotation EOS tail is active while either stage reports remaining samples.
inline bool ambisonic_spatial_tail_active(int rotation_tail_samples, int decode_tail_samples) {
    return rotation_tail_samples > 0 || decode_tail_samples > 0;
}

/// Path effect is created with spatialize=IPL_TRUE (stereo HRTF out); refuse Apply/GetTail if out is not stereo.
inline bool path_spatialize_stereo_out_ready(const IPLAudioBuffer& out, int frame_size) {
    return out.data != nullptr && out.numChannels >= 2 && out.data[0] != nullptr && out.data[1] != nullptr &&
           out.numSamples >= frame_size;
}

} // namespace resonance

#endif // RESONANCE_PROCESSOR_HRTF_POLICY_H
