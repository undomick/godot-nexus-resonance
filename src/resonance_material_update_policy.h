#ifndef RESONANCE_MATERIAL_UPDATE_POLICY_H
#define RESONANCE_MATERIAL_UPDATE_POLICY_H

namespace resonance {

/// How ResonanceGeometry::set_material should apply a new ResonanceMaterial to Steam Audio.
enum class GeometryMaterialUpdatePath {
    None,    // Same material ref, or node not in tree yet
    InPlace, // Live IPLStaticMesh handles: iplStaticMeshSetMaterial (no Embree rebuild)
    Rebuild, // No live meshes: full _create_meshes path (or StaticScene skip/warn)
};

inline GeometryMaterialUpdatePath geometry_material_update_path(bool material_unchanged, bool inside_tree,
                                                                bool has_live_static_meshes) {
    if (material_unchanged || !inside_tree)
        return GeometryMaterialUpdatePath::None;
    if (has_live_static_meshes)
        return GeometryMaterialUpdatePath::InPlace;
    return GeometryMaterialUpdatePath::Rebuild;
}

} // namespace resonance

#endif // RESONANCE_MATERIAL_UPDATE_POLICY_H
