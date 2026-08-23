#ifndef RESONANCE_REFLECTION_TYPE_POLICY_H
#define RESONANCE_REFLECTION_TYPE_POLICY_H

#include "resonance_constants.h"
#include <phonon.h>

namespace resonance {

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
