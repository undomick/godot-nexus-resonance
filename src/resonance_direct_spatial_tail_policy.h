#ifndef RESONANCE_DIRECT_SPATIAL_TAIL_POLICY_H
#define RESONANCE_DIRECT_SPATIAL_TAIL_POLICY_H

#include <phonon.h>

namespace resonance {

/// Direct effect has no internal tail in Steam Audio; EOS spatial drain is binaural/HOA GetTail.
inline bool direct_effect_has_eos_tail(int direct_tail_samples) {
    return direct_tail_samples > 0;
}

/// True while any spatialization stage in the direct chain still reports tail samples.
inline bool direct_spatial_tail_active(bool use_binaural, bool use_hoa_binaural_path, int direct_tail_samples,
                                       int binaural_tail_samples, int hoa_binaural_tail_samples) {
    if (direct_effect_has_eos_tail(direct_tail_samples))
        return true;
    if (use_binaural && binaural_tail_samples > 0)
        return true;
    if (use_hoa_binaural_path && use_binaural && hoa_binaural_tail_samples > 0)
        return true;
    return false;
}

/// FMOD cascade: while direct GetTail is active, downstream spatialization uses Apply on that mono.
inline bool direct_spatial_tail_uses_apply_on_direct_mono(int direct_tail_samples) {
    return direct_effect_has_eos_tail(direct_tail_samples);
}

/// After direct tail is complete, binaural/HOA must drain via GetTail (not Apply).
inline bool direct_spatial_tail_uses_spatial_get_tail(int direct_tail_samples) {
    return !direct_effect_has_eos_tail(direct_tail_samples);
}

inline bool direct_spatial_tail_produced(IPLAudioEffectState state) {
    return state != IPL_AUDIOEFFECTSTATE_TAILCOMPLETE;
}

/// spatial_blend mid-path: keep draining until both binaural and HOA binaural tails are complete.
inline bool direct_spatial_blend_tail_active(IPLAudioEffectState binaural_state, IPLAudioEffectState hoa_binaural_state) {
    return direct_spatial_tail_produced(binaural_state) || direct_spatial_tail_produced(hoa_binaural_state);
}

/// Max reported tail length across active direct-chain spatial stages (for EOS hold / residue checks).
inline int direct_spatial_tail_sample_budget(bool use_binaural, bool use_hoa_binaural_path, int direct_tail_samples,
                                             int binaural_tail_samples, int hoa_binaural_tail_samples) {
    if (!direct_spatial_tail_active(use_binaural, use_hoa_binaural_path, direct_tail_samples, binaural_tail_samples,
                                    hoa_binaural_tail_samples))
        return 0;
    int budget = direct_tail_samples;
    if (binaural_tail_samples > budget)
        budget = binaural_tail_samples;
    if (hoa_binaural_tail_samples > budget)
        budget = hoa_binaural_tail_samples;
    return budget;
}

} // namespace resonance

#endif // RESONANCE_DIRECT_SPATIAL_TAIL_POLICY_H
