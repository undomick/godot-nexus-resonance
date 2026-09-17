#ifndef RESONANCE_SIMULATOR_COMMIT_POLICY_H
#define RESONANCE_SIMULATOR_COMMIT_POLICY_H

namespace resonance {

/// Steam Audio 4.8.1: iplSimulatorCommit stages SetScene / probe-batch / source Add|Remove changes.
/// SetSharedInputs and iplSourceSetInputs do not require Commit (inputs apply on the next Run*).
/// iplSimulatorCommit API: scene or probe batches only (simulation.html), not listener motion or pathing inputs.
/// F-14 skipping Commit when the scene graph is unchanged does not block RunPathing or path SH refresh.
struct PhononSimulatorCommitInputs {
    bool lifecycle_graph_changed = false;
    bool simulator_scene_graph_committed = false;
};

/// True when any IPL simulator graph mutation this tick still needs Commit before Run*.
inline bool phonon_simulator_commit_required(const PhononSimulatorCommitInputs& in) {
    return in.lifecycle_graph_changed || in.simulator_scene_graph_committed;
}

/// After a heavy scene commit, optionally skip RunReflections for one tick - but never the first IR.
inline bool phonon_worker_should_defer_reflections_after_heavy_scene_commit(bool execute_run_reflections,
                                                                            bool scene_commit_over_threshold,
                                                                            bool reflections_have_run_once) {
    if (!execute_run_reflections || !scene_commit_over_threshold)
        return false;
    return reflections_have_run_once;
}

} // namespace resonance

#endif // RESONANCE_SIMULATOR_COMMIT_POLICY_H
