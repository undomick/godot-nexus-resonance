#ifndef RESONANCE_PROCESSOR_HRTF_POLICY_H
#define RESONANCE_PROCESSOR_HRTF_POLICY_H

#include <phonon.h>

namespace resonance {

inline bool hrtf_identity_changed(IPLHRTF bound, IPLHRTF runtime) {
    return bound != runtime;
}

/// Run on the main thread.
inline bool direct_needs_hrtf_effect_create(bool has_binaural_effect, IPLHRTF bound_hrtf, IPLHRTF runtime_hrtf) {
    return runtime_hrtf != nullptr && !has_binaural_effect;
}

/// SOFA reload or HRTF double-buffer replaces the handle captured at effect create.
inline bool direct_needs_hrtf_effect_recreate(bool has_binaural_effect, IPLHRTF bound_hrtf, IPLHRTF runtime_hrtf) {
    return has_binaural_effect && runtime_hrtf != nullptr && bound_hrtf != runtime_hrtf;
}

/// Path effect is created with spatialize and that call's HRTF handle.
inline bool path_needs_hrtf_effect_recreate(bool has_path_effect, IPLHRTF bound_hrtf, IPLHRTF runtime_hrtf) {
    if (!has_path_effect)
        return false;
    if (runtime_hrtf == nullptr)
        return false;
    return bound_hrtf != runtime_hrtf;
}

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
