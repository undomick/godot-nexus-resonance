#ifndef RESONANCE_AMBISONIC_TAIL_POLICY_H
#define RESONANCE_AMBISONIC_TAIL_POLICY_H

#include "resonance_processor_hrtf_policy.h"
#include <phonon.h>

namespace resonance {

inline bool ambisonic_effect_tail_produced(IPLAudioEffectState state) {
    return state != IPL_AUDIOEFFECTSTATE_TAILCOMPLETE;
}

inline bool ambisonic_decode_tail_produced(IPLAudioEffectState state) {
    return ambisonic_effect_tail_produced(state);
}

inline bool ambisonic_rotation_tail_produced(IPLAudioEffectState state) {
    return ambisonic_effect_tail_produced(state);
}

/// Rotation GetTail writes ambisonic channels; the stereo out buffer is only valid after a decode Apply.
inline bool ambisonic_rotation_tail_needs_decode_apply(IPLAudioEffectState rotation_tail_state) {
    return ambisonic_effect_tail_produced(rotation_tail_state);
}

inline bool ambisonic_stop_has_pending_output(bool output_ring_nonempty, bool input_ring_nonempty, int rotation_tail_samples,
                                              int decode_tail_samples) {
    if (output_ring_nonempty || input_ring_nonempty)
        return true;
    return ambisonic_spatial_tail_active(rotation_tail_samples, decode_tail_samples);
}

} // namespace resonance

#endif // RESONANCE_AMBISONIC_TAIL_POLICY_H
