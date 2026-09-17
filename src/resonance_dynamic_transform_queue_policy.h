#ifndef RESONANCE_DYNAMIC_TRANSFORM_QUEUE_POLICY_H
#define RESONANCE_DYNAMIC_TRANSFORM_QUEUE_POLICY_H

namespace resonance {

/// force_apply from flush must survive a later coalesce enqueue for the same instanced mesh.
inline bool dynamic_instanced_transform_force_apply_merge(bool existing_force_apply, bool incoming_force_apply) {
    return existing_force_apply || incoming_force_apply;
}

} // namespace resonance

#endif // RESONANCE_DYNAMIC_TRANSFORM_QUEUE_POLICY_H
