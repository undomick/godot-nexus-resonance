#ifndef RESONANCE_SOURCE_UPDATE_POLICY_H
#define RESONANCE_SOURCE_UPDATE_POLICY_H

#include <cstring>

namespace resonance {

/// True when two simulation source snapshots are bitwise identical (including position).
template <typename SourceUpdateParamsT>
inline bool source_update_params_equal(const SourceUpdateParamsT& a, const SourceUpdateParamsT& b) {
    return std::memcmp(&a, &b, sizeof(SourceUpdateParamsT)) == 0;
}

/// Skip iplSourceSetInputs when params are unchanged.
///
/// Invariant: Linear/Curve attenuation tables live outside SourceUpdateParams (AttenuationEntry). When
/// set_source_attenuation_callback_data changes a curve it sets curve_dirty; skip must stay false until
/// SetInputs applies distanceAttenuationModel.dirty and clears curve_dirty.
inline bool source_update_skip_unchanged_allowed(bool have_valid_snapshot, bool params_equal, bool enable_reflections,
                                                 int baked_data_variation, bool use_listener_probe,
                                                 bool attenuation_callback_curve_dirty) {
    if (!have_valid_snapshot || !params_equal)
        return false;
    if (attenuation_callback_curve_dirty)
        return false;
    if (enable_reflections && use_listener_probe && baked_data_variation == 0)
        return false;
    return true;
}

/// IPL reflections flag: override + output_reverb. Mix level ramps DSP only.
/// Override: -1 = on, 0 = off, 1 = on.
inline bool source_sim_reflections_enabled(int reflections_enabled_override, bool output_reverb) {
    if (!output_reverb)
        return false;
    if (reflections_enabled_override == 0)
        return false;
    return true;
}

/// IPL pathing flag: override/global + output_reverb. Mix level ramps DSP only.
/// Override: -1 = use pathing_enabled_global, 0 = off, 1 = on.
inline bool source_sim_pathing_enabled(int pathing_enabled_override, bool pathing_enabled_global, bool output_reverb) {
    if (!output_reverb)
        return false;
    if (pathing_enabled_override == -1)
        return pathing_enabled_global;
    return pathing_enabled_override != 0;
}

/// Playback DSP enable: output_* only. Mix levels ramp Apply gain separately.
inline bool playback_enable_direct(bool output_direct) {
    return output_direct;
}

/// Wet send enable: output_reverb only. Mix 0 still runs EffectApply with silence so Overlap-Save drains.
inline bool playback_enable_reverb(bool output_reverb) {
    return output_reverb;
}

/// Whether playback should allocate reflection/path IPL effects. Reflections override: -1/1 = on, 0 = off.
/// Pathing override: -1 = use pathing_global, 0 = off, 1 = on.
inline bool playback_wants_wet_effects(int reflections_enabled_override, int pathing_enabled_override,
                                       bool pathing_enabled_global) {
    const bool want_refl = reflections_enabled_override != 0;
    const bool want_path =
        (pathing_enabled_override == -1) ? pathing_enabled_global : (pathing_enabled_override != 0);
    return want_refl || want_path;
}

} // namespace resonance

#endif // RESONANCE_SOURCE_UPDATE_POLICY_H
