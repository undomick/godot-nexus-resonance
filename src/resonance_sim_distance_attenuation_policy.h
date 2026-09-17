#ifndef RESONANCE_SIM_DISTANCE_ATTENUATION_POLICY_H
#define RESONANCE_SIM_DISTANCE_ATTENUATION_POLICY_H

namespace resonance {

/// Attenuation modes on ResonancePlayerConfig / ResonancePlayer::AttenuationMode.
constexpr int kAttenuationModeInverse = 0;
constexpr int kAttenuationModeLinear = 1;
constexpr int kAttenuationModeCurve = 2;
constexpr int kAttenuationModeDisabled = 3;

/// Phonon IPLDistanceAttenuationModel kind for a simulation slot.
enum class SimDistanceModelKind {
    Default,         ///< IPL_DISTANCEATTENUATIONTYPE_DEFAULT (1/max(d, 1 m))
    InverseDistance, ///< IPL_DISTANCEATTENUATIONTYPE_INVERSEDISTANCE + minDistance
    Callback,        ///< Linear/Curve callback (path length or IR correction)
};

/// Direct simulation: Inverse uses Phonon InverseDistance; Linear/Curve stay on playback only.
inline SimDistanceModelKind sim_distance_model_for_direct(bool distance_attenuation, int attenuation_mode) {
    if (!distance_attenuation || attenuation_mode == kAttenuationModeDisabled)
        return SimDistanceModelKind::Default;
    if (attenuation_mode == kAttenuationModeInverse)
        return SimDistanceModelKind::InverseDistance;
    return SimDistanceModelKind::Default;
}

/// Pathing: same TYPE as Direct mode, evaluated on LOS Euclid or occluded path length.
inline SimDistanceModelKind sim_distance_model_for_pathing(bool distance_attenuation, int attenuation_mode) {
    if (!distance_attenuation || attenuation_mode == kAttenuationModeDisabled)
        return SimDistanceModelKind::Default;
    if (attenuation_mode == kAttenuationModeInverse)
        return SimDistanceModelKind::InverseDistance;
    if (attenuation_mode == kAttenuationModeLinear || attenuation_mode == kAttenuationModeCurve)
        return SimDistanceModelKind::Callback;
    return SimDistanceModelKind::Default;
}

/// Reflections IR correction: Callback only when use_distance_curve_for_reflections.
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

/// True when Direct simulation should set IPL_DIRECTSIMULATIONFLAGS_DISTANCEATTENUATION.
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

/// use_distance_curve_for_reflections is only meaningful for Linear/Curve.
inline bool use_distance_curve_for_reflections_editable(bool distance_attenuation, int attenuation_mode) {
    if (!distance_attenuation)
        return false;
    return attenuation_mode == kAttenuationModeLinear || attenuation_mode == kAttenuationModeCurve;
}

} // namespace resonance

#endif // RESONANCE_SIM_DISTANCE_ATTENUATION_POLICY_H
