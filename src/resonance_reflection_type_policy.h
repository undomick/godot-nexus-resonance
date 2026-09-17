#ifndef RESONANCE_REFLECTION_TYPE_POLICY_H
#define RESONANCE_REFLECTION_TYPE_POLICY_H

#include "resonance_constants.h"
#include <phonon.h>

namespace resonance {

/// Maps runtime reflection_type to the bake layer stored on ResonanceProbeData (0-2).
/// TrueAudio Next bakes Convolution energy fields.
inline int bake_reflection_type_from_runtime(int runtime_reflection_type) {
    if (runtime_reflection_type == kReflectionParametric)
        return kBakedReflectionParametric;
    if (runtime_reflection_type == kReflectionHybrid)
        return kBakedReflectionHybrid;
    // Convolution and TAN (and unknown) -> Convolution bake layer.
    return kBakedReflectionConvolution;
}

/// Exact bake/runtime match for gizmos and load_probe_batch. Legacy baked_type -1 is always OK.
/// Hybrid bake is not a universal key: type change after bake requires rebake.
inline bool baked_reflection_type_matches_runtime(int baked_type, int runtime_reflection_type) {
    if (baked_type < 0)
        return true;
    return baked_type == bake_reflection_type_from_runtime(runtime_reflection_type);
}

/// Effect params for Apply/process_mix. Hybrid may fall back to parametric when IR or bands are missing.
inline IPLReflectionEffectType reflection_effect_type_for_mode(int reflection_type, bool hybrid_convolution_and_parametric) {
    if (reflection_type == kReflectionParametric)
        return IPL_REFLECTIONEFFECTTYPE_PARAMETRIC;
    if (reflection_type == kReflectionHybrid) {
        if (hybrid_convolution_and_parametric)
            return IPL_REFLECTIONEFFECTTYPE_HYBRID;
        return IPL_REFLECTIONEFFECTTYPE_PARAMETRIC;
    }
    if (reflection_type == kReflectionTan)
        return IPL_REFLECTIONEFFECTTYPE_TAN;
    return IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
}

/// Simulator create settings: Hybrid stays HYBRID (unlike effect params).
inline IPLReflectionEffectType reflection_type_for_simulator(int reflection_type) {
    if (reflection_type == kReflectionParametric)
        return IPL_REFLECTIONEFFECTTYPE_PARAMETRIC;
    if (reflection_type == kReflectionHybrid)
        return IPL_REFLECTIONEFFECTTYPE_HYBRID;
    if (reflection_type == kReflectionTan)
        return IPL_REFLECTIONEFFECTTYPE_TAN;
    return IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
}

} // namespace resonance

#endif // RESONANCE_REFLECTION_TYPE_POLICY_H
