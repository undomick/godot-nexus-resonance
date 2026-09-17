#ifndef RESONANCE_EXPORT_TRANSFORM_POLICY_H
#define RESONANCE_EXPORT_TRANSFORM_POLICY_H

#include "resonance_mesh_ipl.h"
#include <cstddef>

namespace resonance {

/// Godot Transform3D multiply: parent * local (column basis).
inline MeshBakeTransform multiply_mesh_bake_transform(const MeshBakeTransform& parent, const MeshBakeTransform& local) {
    MeshBakeTransform out{};
    for (int c = 0; c < 3; ++c) {
        const float* lc = (c == 0) ? local.basis_col0 : (c == 1) ? local.basis_col1
                                                                 : local.basis_col2;
        float* oc = (c == 0) ? out.basis_col0 : (c == 1) ? out.basis_col1
                                                         : out.basis_col2;
        oc[0] = parent.basis_col0[0] * lc[0] + parent.basis_col1[0] * lc[1] + parent.basis_col2[0] * lc[2];
        oc[1] = parent.basis_col0[1] * lc[0] + parent.basis_col1[1] * lc[1] + parent.basis_col2[1] * lc[2];
        oc[2] = parent.basis_col0[2] * lc[0] + parent.basis_col1[2] * lc[1] + parent.basis_col2[2] * lc[2];
    }
    IPLVector3 origin_ipl{};
    mesh_bake_transform_xyz(parent, local.origin[0], local.origin[1], local.origin[2], origin_ipl);
    out.origin[0] = origin_ipl.x;
    out.origin[1] = origin_ipl.y;
    out.origin[2] = origin_ipl.z;
    return out;
}

/// Compose local transforms root-first (index 0 = topmost ancestor), matching Godot global = parent * local.
inline MeshBakeTransform compose_mesh_bake_xforms_root_first(const MeshBakeTransform* locals, size_t count) {
    MeshBakeTransform out = identity_mesh_bake_transform();
    if (!locals || count == 0)
        return out;
    for (size_t i = 0; i < count; ++i)
        out = multiply_mesh_bake_transform(out, locals[i]);
    return out;
}

} // namespace resonance

#endif // RESONANCE_EXPORT_TRANSFORM_POLICY_H
