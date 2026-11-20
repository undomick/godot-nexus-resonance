#ifndef RESONANCE_STATIC_EXPORT_POLICY_H
#define RESONANCE_STATIC_EXPORT_POLICY_H

namespace resonance {

/// A nested ResonanceStaticScene, or a PackedScene instance that contains one, exports its own geometry,
/// so the parent export skips that subtree.
inline bool should_prune_static_export_subtree(bool is_export_root, bool is_resonance_static_scene,
                                               bool has_scene_file_path, bool subtree_has_resonance_static_scene) {
    if (is_export_root)
        return false;
    if (is_resonance_static_scene)
        return true;
    if (has_scene_file_path && subtree_has_resonance_static_scene)
        return true;
    return false;
}

} // namespace resonance

#endif // RESONANCE_STATIC_EXPORT_POLICY_H
