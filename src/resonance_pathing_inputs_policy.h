#ifndef RESONANCE_PATHING_INPUTS_POLICY_H
#define RESONANCE_PATHING_INPUTS_POLICY_H

#include <algorithm>
#include <cstdint>

namespace resonance {

/// Max pathing Ambisonic order (order 3 -> 16 SH coeffs).
constexpr int kPathingApplyOrderMax = 3;

/// Direct SetInputs selector: always DIRECT alone.
/// Never OR PATHING/REFLECTIONS here - those are wet-slot updates.
inline int source_direct_set_inputs_selector_flags(int direct_flag) {
    return direct_flag;
}

/// Wet SetInputs selector for Reflections and/or Pathing (no DIRECT).
///
/// Phonon iplSourceSetInputs quirk (Steam Audio 4.8.1):
/// pathingInputs is only mutated when the *selector* argument includes PATHING.
/// Passing the same mask for selector and inputs.flags therefore fails to clear
/// prior pathingProbes when pathing is disabled for this update.
///
/// When global pathing is enabled, always OR PATHING into the wet selector so a
/// disabled inputs.flags wipe clears stale pathing state before probe-batch removal.
inline int source_set_inputs_selector_flags(int inputs_flags, int pathing_flag, bool pathing_globally_enabled) {
    if (!pathing_globally_enabled)
        return inputs_flags;
    return inputs_flags | pathing_flag;
}

/// True when a wet SetInputs call is needed (reflections/pathing bits or pathing clear).
inline bool source_wet_set_inputs_needed(int wet_inputs_flags, bool pathing_globally_enabled) {
    return wet_inputs_flags != 0 || pathing_globally_enabled;
}

/// Direct sim flags never include reflections (baked REVERB uses a separate REFLECTIONS SetInputs).
inline int source_direct_sim_flags_mask(int direct_flag) {
    return direct_flag;
}

/// Whether a source that last requested preferred_handle must drop pathing before
/// removing_handle is detached from the simulator (PathSimulator map erase).
///
/// preferred_is_usable_pathing must reflect resolve_pathing_batch (explicit volume hit only).
/// Stale preferred handles no longer bind via silent multi-volume fallback.
inline bool source_should_clear_pathing_on_batch_remove(int32_t preferred_handle, int32_t removing_handle,
                                                        bool preferred_is_usable_pathing) {
    if (removing_handle < 0)
        return false;
    if (preferred_handle < 0 || preferred_handle == removing_handle)
        return true;
    return !preferred_is_usable_pathing;
}

/// Phonon allocates PathSimulator / probe-manager internals only when
/// IPL_SIMULATIONFLAGS_PATHING is set at iplSimulatorCreate. Steam Audio
/// creates the runtime simulator with Pathing; Nexus matches that so live
/// enable does not need recreate. This gate remains for a legacy simulator
/// built without PATHING.
inline bool simulator_supports_pathing_run(bool pathing_enabled, bool simulator_created_with_pathing) {
    return pathing_enabled && simulator_created_with_pathing;
}

/// True when toggling pathing on requires audio-engine reinit (new iplSimulatorCreate).
/// After init with PATHING (simulator_created_with_pathing always true) this is never true.
inline bool pathing_enable_requires_simulator_recreate(bool want_enabled, bool simulator_created_with_pathing) {
    return want_enabled && !simulator_created_with_pathing;
}

/// GetOutputs never writes pathing.order; Apply order comes from configured ambisonic_order.
inline int pathing_apply_order(int configured_ambisonic_order) {
    return std::clamp(configured_ambisonic_order, 0, kPathingApplyOrderMax);
}

/// Exact (order+1)^2; unlike ambisonic_num_channels_for_order this does not clamp to 1..3.
inline int pathing_sh_coeff_count(int order) {
    if (order < 0)
        return 0;
    return (order + 1) * (order + 1);
}

} // namespace resonance

#endif // RESONANCE_PATHING_INPUTS_POLICY_H
