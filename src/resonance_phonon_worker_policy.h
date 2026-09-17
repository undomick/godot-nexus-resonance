#ifndef RESONANCE_PHONON_WORKER_POLICY_H
#define RESONANCE_PHONON_WORKER_POLICY_H

#include <cstdint>

namespace resonance {

/// Inputs for deciding whether a worker/main-thread phonon tick should run IPL simulation.
struct PhononWorkerTickInputs {
    bool pending_lifecycle = false;
    bool pending_source_updates = false;
    bool scene_dirty = false;
    bool pending_dynamic_transforms = false;
    bool run_direct_scheduled = false;
    bool run_reflection_heavy = false;
    bool run_pathing_heavy = false;
    bool output_reverb_enabled = true;
    bool any_direct_outputs = false;
    bool any_reflection_outputs = false;
    bool any_pathing_outputs = false;
};

/// After lifecycle drain or SetInputs flush, force one direct pass when sources need direct output.
inline bool phonon_worker_run_direct_after_input_drain(bool input_drain_had_work, bool run_direct_scheduled,
                                                       bool any_direct_outputs) {
    if (!any_direct_outputs)
        return run_direct_scheduled;
    return run_direct_scheduled || input_drain_had_work;
}

/// Geo-05: after iplSceneCommit on a dirty tick, refresh direct/occlusion caches even when tick() did not schedule RunDirect.
inline bool phonon_worker_run_direct_after_scene_graph_commit(bool scene_graph_committed, bool run_direct_scheduled,
                                                              bool any_direct_outputs) {
    if (!any_direct_outputs)
        return run_direct_scheduled;
    return run_direct_scheduled || scene_graph_committed;
}

/// tick() direct schedule: interval tick, batched/lifecycle pending, or inline try_update_source.
inline bool phonon_worker_run_direct_for_tick(bool run_direct_scheduled, bool lifecycle_or_batched_updates_pending,
                                              bool direct_after_inline_inputs_pending) {
    return run_direct_scheduled || lifecycle_or_batched_updates_pending || direct_after_inline_inputs_pending;
}

/// Inline-input pending is stale when no source needs Direct; clear to avoid perpetual tick/worker wakes.
inline bool phonon_worker_should_clear_stale_direct_after_inline_inputs(bool direct_after_inline_inputs_pending,
                                                                        bool any_direct_outputs) {
    return direct_after_inline_inputs_pending && !any_direct_outputs;
}

/// Heavy/pathing requests armed before per-source outputs exist must survive idle tick() early-outs.
inline bool phonon_worker_should_clear_idle_heavy_flags(const PhononWorkerTickInputs& in) {
    if (in.pending_lifecycle || in.pending_source_updates || in.scene_dirty || in.pending_dynamic_transforms)
        return true;
    if (in.run_reflection_heavy && in.output_reverb_enabled && !in.any_reflection_outputs)
        return false;
    if (in.run_pathing_heavy && in.output_reverb_enabled && !in.any_pathing_outputs)
        return false;
    return true;
}

/// True when lifecycle, scene, or scheduled simulation work requires a phonon tick.
inline bool phonon_worker_tick_has_work(const PhononWorkerTickInputs& in) {
    if (in.pending_lifecycle || in.pending_source_updates || in.scene_dirty || in.pending_dynamic_transforms)
        return true;

    const bool gated_refl = in.output_reverb_enabled && in.any_reflection_outputs;
    const bool gated_path = in.output_reverb_enabled && in.any_pathing_outputs;

    if (in.run_reflection_heavy && gated_refl)
        return true;
    if (in.run_pathing_heavy && gated_path)
        return true;
    if (in.run_direct_scheduled && in.any_direct_outputs)
        return true;

    return false;
}

inline bool phonon_worker_shared_reflections_enabled(bool output_reverb_enabled, bool any_reflection_outputs) {
    return output_reverb_enabled && any_reflection_outputs;
}

inline bool phonon_worker_shared_pathing_enabled(bool pathing_enabled, bool output_reverb_enabled, bool any_pathing_outputs) {
    return pathing_enabled && output_reverb_enabled && any_pathing_outputs;
}

inline bool phonon_worker_run_reflection_heavy(bool reflection_heavy_requested, bool output_reverb_enabled,
                                               bool any_reflection_outputs) {
    return reflection_heavy_requested &&
           phonon_worker_shared_reflections_enabled(output_reverb_enabled, any_reflection_outputs);
}

inline bool phonon_worker_run_pathing_heavy(bool pathing_heavy_requested, bool pathing_enabled, bool output_reverb_enabled,
                                            bool any_pathing_outputs) {
    return pathing_heavy_requested &&
           phonon_worker_shared_pathing_enabled(pathing_enabled, output_reverb_enabled, any_pathing_outputs);
}

/// Shutdown / cold-start settle / stopped worker: never start a new RunReflections or RunPathing.
/// simulation_active is true for the dedicated worker (thread_running) or Custom main-thread sim.
inline bool phonon_worker_allow_new_heavy_simulation(bool shutting_down, bool simulation_active,
                                                     bool cold_start_settle = false) {
    return simulation_active && !shutting_down && !cold_start_settle;
}

} // namespace resonance

#endif // RESONANCE_PHONON_WORKER_POLICY_H
