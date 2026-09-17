#ifndef RESONANCE_AMBISONIC_TAIL_POLICY_H
#define RESONANCE_AMBISONIC_TAIL_POLICY_H

#include "resonance_processor_hrtf_policy.h"
#include <phonon.h>

namespace resonance {

/// Align with direct_spatial_tail_produced (P-H1): any state except TAILCOMPLETE produced output.
inline bool ambisonic_effect_tail_produced(IPLAudioEffectState state) {
    return state != IPL_AUDIOEFFECTSTATE_TAILCOMPLETE;
}

inline bool ambisonic_decode_tail_produced(IPLAudioEffectState state) {
    return ambisonic_effect_tail_produced(state);
}

inline bool ambisonic_rotation_tail_produced(IPLAudioEffectState state) {
    return ambisonic_effect_tail_produced(state);
}

/// Rotation GetTail writes HOA; stereo EOS requires decode Apply before out_buffer is valid.
inline bool ambisonic_rotation_tail_needs_decode_apply(IPLAudioEffectState rotation_tail_state) {
    return ambisonic_effect_tail_produced(rotation_tail_state);
}

/// EOS hold: keep playback alive while decode or rotation tails remain, or output rings are non-empty.
inline bool ambisonic_stop_has_pending_output(bool output_ring_nonempty, bool input_ring_nonempty, int rotation_tail_samples,
                                              int decode_tail_samples) {
    if (output_ring_nonempty || input_ring_nonempty)
        return true;
    return ambisonic_spatial_tail_active(rotation_tail_samples, decode_tail_samples);
}

} // namespace resonance

#endif // RESONANCE_AMBISONIC_TAIL_POLICY_H
