#ifndef RESONANCE_SIMULATOR_COMMIT_POLICY_H
#define RESONANCE_SIMULATOR_COMMIT_POLICY_H

namespace resonance {

/// iplSimulatorCommit applies scene, probe-batch and source add/remove changes. Shared and per-source inputs
/// apply on the next Run* without a Commit, so skipping it does not hold back listener motion or pathing.
struct PhononSimulatorCommitInputs {
    bool lifecycle_graph_changed = false;
    bool simulator_scene_graph_committed = false;
};

inline bool phonon_simulator_commit_required(const PhononSimulatorCommitInputs& in) {
    return in.lifecycle_graph_changed || in.simulator_scene_graph_committed;
}

/// After a heavy scene commit RunReflections waits one tick, but never before the first IR.
inline bool phonon_worker_should_defer_reflections_after_heavy_scene_commit(bool execute_run_reflections,
                                                                            bool scene_commit_over_threshold,
                                                                            bool reflections_have_run_once) {
    if (!execute_run_reflections || !scene_commit_over_threshold)
        return false;
    return reflections_have_run_once;
}

} // namespace resonance

#endif // RESONANCE_SIMULATOR_COMMIT_POLICY_H
