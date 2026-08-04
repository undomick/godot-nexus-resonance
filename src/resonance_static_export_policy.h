#ifndef RESONANCE_STATIC_EXPORT_POLICY_H
#define RESONANCE_STATIC_EXPORT_POLICY_H

namespace resonance {

/// Parent-export prune rules for static geometry collection (no Godot types; unit-testable).
/// - Nested ResonanceStaticScene always prunes (with or without asset).
/// - PackedScene instance roots (non-empty scene_file_path) that contain an RSS prune the whole instance.
/// - The export root itself is never pruned by these rules.
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
