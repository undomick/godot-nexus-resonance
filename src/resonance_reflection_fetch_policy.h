#ifndef RESONANCE_REFLECTION_FETCH_POLICY_H
#define RESONANCE_REFLECTION_FETCH_POLICY_H

#include "resonance_constants.h"
#include "resonance_reflection_cache_publish_policy.h"
#include <phonon.h>

namespace resonance {

/// GetOutputs(REFLECTIONS) returns the source-owned IR handle: applying it keeps convolving the previous IR
/// until a new one is committed, while a null ir stops convolution. Hybrid can still mix stale parametric times.
inline bool reflection_stale_epoch_usable_for_mix(int reflection_type, const IPLReflectionEffectParams& p) {
    if (reflection_type == resonance::kReflectionConvolution || reflection_type == resonance::kReflectionTan)
        return p.ir != nullptr;
    if (p.ir != nullptr)
        return false;
    if (reflection_type == resonance::kReflectionHybrid) {
        return (p.reverbTimes[0] > 0.0f || p.reverbTimes[1] > 0.0f || p.reverbTimes[2] > 0.0f);
    }
    return false;
}

/// A fetch miss with no tail drops the decoded block, so a short transient is lost before the first IR arrives.
/// Without per-source reflections no IR ever arrives (pathing alone also enables reverb), so never park then.
inline bool reflection_decoder_should_wait_for_first_params(bool enable_reverb, bool enable_reflections,
                                                            bool voice_may_convolve, bool fetch_ok,
                                                            bool tail_applyable) {
    return enable_reverb && enable_reflections && voice_may_convolve && !fetch_ok && !tail_applyable;
}

inline float conv_reflection_wet_mix_level(float reflections_mix_level, float wet_occlusion_factor) {
    return reflections_mix_level * wet_occlusion_factor;
}

/// The last good convolution IR is reused only between RunReflections passes; a null IR right after a pass means invalidated.
inline bool reflection_worker_use_last_good_conv(bool has_fresh_ir, int reflection_type, bool last_good_valid,
                                                 bool sync_after_run_reflections) {
    if (has_fresh_ir || sync_after_run_reflections)
        return false;
    if (reflection_type != resonance::kReflectionConvolution)
        return false;
    return last_good_valid;
}

/// EOS GetTail snapshots may retain a stale tanDevice; refresh from the live device each pull.
inline void reflection_eos_tail_refresh_tan_device(int reflection_type, IPLReflectionEffectParams& params,
                                                   IPLTrueAudioNextDevice tan_device) {
    if (reflection_type == resonance::kReflectionTan && tan_device != nullptr)
        params.tanDevice = tan_device;
}

} // namespace resonance

#endif // RESONANCE_REFLECTION_FETCH_POLICY_H
