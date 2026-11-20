#ifndef RESONANCE_PATHING_INPUTS_POLICY_H
#define RESONANCE_PATHING_INPUTS_POLICY_H

#include <algorithm>
#include <cstdint>

namespace resonance {

/// Max pathing Ambisonic order (order 3 -> 16 SH coeffs).
constexpr int kPathingApplyOrderMax = 3;

/// Always DIRECT alone. PATHING and REFLECTIONS go through the wet SetInputs.
inline int source_direct_set_inputs_selector_flags(int direct_flag) {
    return direct_flag;
}

/// iplSourceSetInputs only updates the types in its selector; a type left out keeps its last flags and
/// stays traced. So the wet selector always carries REFLECTIONS, and PATHING while pathing is globally
/// enabled, letting a cleared inputs.flags turn them off.
inline int source_set_inputs_selector_flags(int inputs_flags, int reflections_flag, int pathing_flag,
                                            bool pathing_globally_enabled) {
    const int selector = inputs_flags | reflections_flag;
    return pathing_globally_enabled ? (selector | pathing_flag) : selector;
}

/// Direct sim flags never include reflections (baked REVERB uses a separate REFLECTIONS SetInputs).
inline int source_direct_sim_flags_mask(int direct_flag) {
    return direct_flag;
}

/// Whether a source that last requested preferred_handle must drop pathing before removing_handle is
/// detached from the simulator. preferred_is_usable_pathing: resolve_pathing_batch found an explicit volume hit.
inline bool source_should_clear_pathing_on_batch_remove(int32_t preferred_handle, int32_t removing_handle,
                                                        bool preferred_is_usable_pathing) {
    if (removing_handle < 0)
        return false;
    if (preferred_handle < 0 || preferred_handle == removing_handle)
        return true;
    return !preferred_is_usable_pathing;
}

/// Phonon allocates pathing internals only when IPL_SIMULATIONFLAGS_PATHING is set at iplSimulatorCreate.
inline bool simulator_supports_pathing_run(bool pathing_enabled, bool simulator_created_with_pathing) {
    return pathing_enabled && simulator_created_with_pathing;
}

/// The server always creates its simulator with PATHING, so this is true only before a successful init.
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
