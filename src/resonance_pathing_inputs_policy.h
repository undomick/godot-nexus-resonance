#ifndef RESONANCE_PATHING_INPUTS_POLICY_H
#define RESONANCE_PATHING_INPUTS_POLICY_H

#include <cstdint>

namespace resonance {

/// Phonon iplSourceSetInputs quirk (Steam Audio 4.8.1):
/// pathingInputs is only mutated when the *selector* argument includes PATHING.
/// Passing the same mask for selector and inputs.flags therefore fails to clear
/// prior pathingProbes when pathing is disabled for this update.
///
/// When global pathing is enabled, always OR PATHING into the selector so a
/// disabled inputs.flags wipe clears stale pathing state before probe-batch removal.
inline int source_set_inputs_selector_flags(int inputs_flags, int pathing_flag, bool pathing_globally_enabled) {
    if (!pathing_globally_enabled)
        return inputs_flags;
    return inputs_flags | pathing_flag;
}

/// Whether a source that last requested preferred_handle must drop pathing before
/// removing_handle is detached from the simulator (PathSimulator map erase).
///
/// get_pathing_batch falls back to the first pathing batch when preferred is missing
/// or has no pathing. preferred_is_usable_pathing must reflect that same check so a
/// stale preferred does not skip clearing the actually-bound fallback batch.
inline bool source_should_clear_pathing_on_batch_remove(int32_t preferred_handle, int32_t removing_handle,
                                                        bool preferred_is_usable_pathing) {
    if (removing_handle < 0)
        return false;
    if (preferred_handle < 0 || preferred_handle == removing_handle)
        return true;
    return !preferred_is_usable_pathing;
}

/// Phonon allocates PathSimulator / probe-manager internals only when
/// IPL_SIMULATIONFLAGS_PATHING is set at iplSimulatorCreate. Enabling pathing
/// later without recreating the simulator makes RunPathing / AddProbeBatch unsafe.
inline bool simulator_supports_pathing_run(bool pathing_enabled, bool simulator_created_with_pathing) {
    return pathing_enabled && simulator_created_with_pathing;
}

/// True when toggling pathing on requires audio-engine reinit (new iplSimulatorCreate).
inline bool pathing_enable_requires_simulator_recreate(bool want_enabled, bool simulator_created_with_pathing) {
    return want_enabled && !simulator_created_with_pathing;
}

} // namespace resonance

#endif // RESONANCE_PATHING_INPUTS_POLICY_H
