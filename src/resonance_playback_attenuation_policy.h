#ifndef RESONANCE_PLAYBACK_ATTENUATION_POLICY_H
#define RESONANCE_PLAYBACK_ATTENUATION_POLICY_H

#include "resonance_math.h"

#include <algorithm>

namespace resonance {

/// IPL INVERSEDISTANCE playback rolloff: unity inside min distance, then minDistance / distance.
inline float inverse_distance_attenuation(float distance, float min_distance) {
    const float d = sanitize_audio_float(distance);
    const float md = std::fmax(sanitize_audio_float(min_distance), 1.0e-4f);
    if (d <= md)
        return 1.0f;
    return sanitize_audio_float(md / d);
}

/// Recomputed from the current distance on every call; the throttled
/// iplSourceGetOutputs(DIRECT).distanceAttenuation would lag behind moving sources.
inline float playback_inverse_distance_attenuation(float distance, float min_distance, float sim_cached_attenuation) {
    (void)sim_cached_attenuation;
    return inverse_distance_attenuation(distance, min_distance);
}

/// RunPathing already bakes distance attenuation into the path SH, so applying the direct
/// attenuation to the path wet would double-apply 1/d on line of sight.
inline bool pathing_wet_uses_playback_distance_attenuation() {
    return false;
}

inline float pathing_wet_playback_mix_level(float pathing_mix_level) {
    return sanitize_audio_float(pathing_mix_level);
}

} // namespace resonance

#endif // RESONANCE_PLAYBACK_ATTENUATION_POLICY_H
