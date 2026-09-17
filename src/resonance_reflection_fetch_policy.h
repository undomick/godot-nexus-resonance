#ifndef RESONANCE_REFLECTION_FETCH_POLICY_H
#define RESONANCE_REFLECTION_FETCH_POLICY_H

#include "resonance_constants.h"
#include "resonance_reflection_cache_publish_policy.h"
#include <phonon.h>

namespace resonance {

/// Steam Audio: GetOutputs(REFLECTIONS) always returns the source-owned TripleBuffer IR handle. EffectApply
/// every audio block with a non-null ir continues via mPrevFFTIR when RunReflections has not committed a new
/// IR. Null ir aborts convolution (no hold). Call iplReflectionEffectApply every block with that handle.
/// Hybrid may mix parametric times on a stale cache epoch; Conv/TAN mix when ir != nullptr (TripleBuffer).
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

/// Convolution/TAN mixer feed: reflections_mix_level scaled by baked wet occlusion (same as parametric).
inline float conv_reflection_wet_mix_level(float reflections_mix_level, float wet_occlusion_factor) {
    return reflections_mix_level * wet_occlusion_factor;
}

/// Worker sync: last-good Conv IR only between RunReflections passes (null IR after RunReflections is invalidated).
inline bool reflection_worker_use_last_good_conv(bool has_fresh_ir, int reflection_type, bool last_good_valid,
                                                 bool sync_after_run_reflections) {
    if (has_fresh_ir || sync_after_run_reflections)
        return false;
    if (reflection_type != resonance::kReflectionConvolution)
        return false;
    return last_good_valid;
}

/// Hybrid EOS: drop stale IR but keep parametric tail when epoch advanced.
inline void reflection_eos_tail_strip_stale_ir(int reflection_type, uint32_t slot_epoch, uint32_t tail_capture_epoch,
                                               IPLReflectionEffectParams& params) {
    if (reflection_type != resonance::kReflectionHybrid || params.ir == nullptr)
        return;
    if (reflection_cache_entry_epoch_fresh(slot_epoch, tail_capture_epoch))
        return;
    params.ir = nullptr;
    params.type = IPL_REFLECTIONEFFECTTYPE_PARAMETRIC;
}

/// EOS GetTail snapshots may retain a stale tanDevice; refresh from the live device each pull.
inline void reflection_eos_tail_refresh_tan_device(int reflection_type, IPLReflectionEffectParams& params,
                                                   IPLTrueAudioNextDevice tan_device) {
    if (reflection_type == resonance::kReflectionTan && tan_device != nullptr)
        params.tanDevice = tan_device;
}

/// After RunReflections, open the parametric/hybrid pending gate for handles that will sync-fetch this tick.
/// Must run before _worker_sync_fetch_caches (fetch is blocked while pending). Attach-pending and no-reflection
/// handles stay gated until attach completes or reflections are disabled.
inline bool reflection_pending_should_clear_after_run(bool ran_reflections, bool handle_wants_reflections,
                                                      bool attach_pending) {
    if (!ran_reflections)
        return false;
    if (!handle_wants_reflections)
        return false;
    if (attach_pending)
        return false;
    return true;
}

} // namespace resonance

#endif // RESONANCE_REFLECTION_FETCH_POLICY_H
