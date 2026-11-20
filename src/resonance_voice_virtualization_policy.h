#ifndef RESONANCE_VOICE_VIRTUALIZATION_POLICY_H
#define RESONANCE_VOICE_VIRTUALIZATION_POLICY_H

#include <algorithm>

namespace resonance {

constexpr float kVoiceVirtualEnterDelaySec = 0.25f;

struct VoiceAudibility {
    bool attenuation_uses_max_distance = false;
    float distance = 0.0f;
    float max_distance = 0.0f;
    float attenuation = 1.0f;
    bool reflections_simulated = false;
    bool reflections_follow_distance_curve = false;
};

struct VoiceVirtualState {
    double inaudible_sec = 0.0;
    bool virtualized = false;
};

// Beyond max_distance the pathing curve is silent too, since a path is never shorter than the
// straight line. Reflections only fall silent there when they follow the distance curve.
inline bool voice_is_inaudible(const VoiceAudibility& a) {
    if (!a.attenuation_uses_max_distance)
        return false;
    if (a.distance < a.max_distance || a.attenuation > 0.0f)
        return false;
    return !a.reflections_simulated || a.reflections_follow_distance_curve;
}

// With reflections the voice waits for its reverb tail to decay before it goes silent.
inline float voice_virtual_enter_delay_sec(bool reflections_simulated, float reflections_duration_sec) {
    if (!reflections_simulated)
        return kVoiceVirtualEnterDelaySec;
    return std::max(kVoiceVirtualEnterDelaySec, reflections_duration_sec);
}

inline VoiceVirtualState next_voice_virtual_state(VoiceVirtualState state, bool inaudible, double delta_sec,
                                                  float enter_delay_sec) {
    if (!inaudible)
        return {};
    state.inaudible_sec += std::max(0.0, delta_sec);
    state.virtualized = state.inaudible_sec >= static_cast<double>(enter_delay_sec);
    return state;
}

} // namespace resonance

#endif // RESONANCE_VOICE_VIRTUALIZATION_POLICY_H
