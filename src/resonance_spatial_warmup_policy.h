#ifndef RESONANCE_SPATIAL_WARMUP_POLICY_H
#define RESONANCE_SPATIAL_WARMUP_POLICY_H

namespace resonance {

/// Warmup counter advances once per worker tick on RunDirect and/or scene graph commit (reverb-only scenes).
inline bool spatial_warmup_should_decrement(bool run_direct_executed, bool scene_graph_committed) {
    return run_direct_executed || scene_graph_committed;
}

} // namespace resonance

#endif // RESONANCE_SPATIAL_WARMUP_POLICY_H
