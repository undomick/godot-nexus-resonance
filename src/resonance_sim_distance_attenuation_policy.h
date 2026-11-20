#ifndef RESONANCE_SIM_DISTANCE_ATTENUATION_POLICY_H
#define RESONANCE_SIM_DISTANCE_ATTENUATION_POLICY_H

namespace resonance {

/// Values of ResonancePlayer::AttenuationMode.
constexpr int kAttenuationModeInverse = 0;
constexpr int kAttenuationModeLinear = 1;
constexpr int kAttenuationModeCurve = 2;
constexpr int kAttenuationModeDisabled = 3;

enum class SimDistanceModelKind {
    Default,         ///< IPL_DISTANCEATTENUATIONTYPE_DEFAULT (1/max(d, 1 m))
    InverseDistance, ///< IPL_DISTANCEATTENUATIONTYPE_INVERSEDISTANCE + minDistance
    Callback,        ///< Linear/Curve callback (path length or IR correction)
};

/// Linear and Curve are applied at playback, not in direct simulation.
inline SimDistanceModelKind sim_distance_model_for_direct(bool distance_attenuation, int attenuation_mode) {
    if (!distance_attenuation || attenuation_mode == kAttenuationModeDisabled)
        return SimDistanceModelKind::Default;
    if (attenuation_mode == kAttenuationModeInverse)
        return SimDistanceModelKind::InverseDistance;
    return SimDistanceModelKind::Default;
}

/// Pathing evaluates the model on the path length, so Linear and Curve need the callback here.
inline SimDistanceModelKind sim_distance_model_for_pathing(bool distance_attenuation, int attenuation_mode) {
    if (!distance_attenuation || attenuation_mode == kAttenuationModeDisabled)
        return SimDistanceModelKind::Default;
    if (attenuation_mode == kAttenuationModeInverse)
        return SimDistanceModelKind::InverseDistance;
    if (attenuation_mode == kAttenuationModeLinear || attenuation_mode == kAttenuationModeCurve)
        return SimDistanceModelKind::Callback;
    return SimDistanceModelKind::Default;
}

inline SimDistanceModelKind sim_distance_model_for_reflections(bool distance_attenuation, int attenuation_mode,
                                                               bool use_distance_curve_for_reflections) {
    if (!distance_attenuation || attenuation_mode == kAttenuationModeDisabled)
        return SimDistanceModelKind::Default;
    if (attenuation_mode == kAttenuationModeInverse)
        return SimDistanceModelKind::InverseDistance;
    if ((attenuation_mode == kAttenuationModeLinear || attenuation_mode == kAttenuationModeCurve) &&
        use_distance_curve_for_reflections)
        return SimDistanceModelKind::Callback;
    return SimDistanceModelKind::Default;
}

inline bool sim_distance_models_need_split_wet_set_inputs(SimDistanceModelKind reflections_kind,
                                                          SimDistanceModelKind pathing_kind) {
    return reflections_kind != pathing_kind;
}

inline bool sim_direct_distance_attenuation_flag(bool distance_attenuation, int attenuation_mode) {
    return sim_distance_model_for_direct(distance_attenuation, attenuation_mode) ==
           SimDistanceModelKind::InverseDistance;
}

/// Inverse ignores max_distance (Phonon InverseDistance has no max).
inline bool attenuation_mode_uses_max_distance(bool distance_attenuation, int attenuation_mode) {
    if (!distance_attenuation)
        return false;
    return attenuation_mode == kAttenuationModeLinear || attenuation_mode == kAttenuationModeCurve;
}

inline bool use_distance_curve_for_reflections_editable(bool distance_attenuation, int attenuation_mode) {
    if (!distance_attenuation)
        return false;
    return attenuation_mode == kAttenuationModeLinear || attenuation_mode == kAttenuationModeCurve;
}

} // namespace resonance

#endif // RESONANCE_SIM_DISTANCE_ATTENUATION_POLICY_H
