#ifndef RESONANCE_MESH_IPL_H
#define RESONANCE_MESH_IPL_H

#include <cstdint>
#include <phonon.h>
#include <vector>

namespace resonance {

/// Column-basis + origin, matching Godot Transform3D::xform (basis * v + origin).
struct MeshBakeTransform {
    float basis_col0[3]{1.0f, 0.0f, 0.0f};
    float basis_col1[3]{0.0f, 1.0f, 0.0f};
    float basis_col2[3]{0.0f, 0.0f, 1.0f};
    float origin[3]{0.0f, 0.0f, 0.0f};
};

inline MeshBakeTransform identity_mesh_bake_transform() {
    return MeshBakeTransform{};
}

inline void mesh_bake_transform_xyz(const MeshBakeTransform& xform, float x, float y, float z, IPLVector3& out) {
    out.x = xform.basis_col0[0] * x + xform.basis_col1[0] * y + xform.basis_col2[0] * z + xform.origin[0];
    out.y = xform.basis_col0[1] * x + xform.basis_col1[1] * y + xform.basis_col2[1] * z + xform.origin[1];
    out.z = xform.basis_col0[2] * x + xform.basis_col1[2] * y + xform.basis_col2[2] * z + xform.origin[2];
}

/// Append one mesh surface (local positions + optional indices) to IPL export buffers.
/// Returns triangle count appended (0 if the surface is skipped).
inline int append_mesh_surface_to_ipl(const float* local_xyz, int vertex_count, const int32_t* indices, int index_count,
                                      const MeshBakeTransform& xform, std::vector<IPLVector3>& out_vertices,
                                      std::vector<IPLTriangle>& out_triangles, std::vector<IPLint32>* out_mat_indices,
                                      IPLint32 mat_index = 0) {
    if (local_xyz == nullptr || vertex_count < 3)
        return 0;

    const size_t v_offset = out_vertices.size();
    const int triangles_before = static_cast<int>(out_triangles.size());

    for (int v = 0; v < vertex_count; v++) {
        const float* p = local_xyz + v * 3;
        IPLVector3 world{};
        mesh_bake_transform_xyz(xform, p[0], p[1], p[2], world);
        out_vertices.push_back(world);
    }

    if (indices != nullptr && index_count > 0) {
        for (int idx = 0; idx < index_count; idx += 3) {
            if (idx + 2 >= index_count)
                break;
            out_triangles.push_back({static_cast<int>(indices[idx]) + static_cast<int>(v_offset),
                                     static_cast<int>(indices[idx + 1]) + static_cast<int>(v_offset),
                                     static_cast<int>(indices[idx + 2]) + static_cast<int>(v_offset)});
            if (out_mat_indices)
                out_mat_indices->push_back(mat_index);
        }
    } else {
        for (int v = 0; v < vertex_count; v += 3) {
            if (v + 2 >= vertex_count)
                break;
            out_triangles.push_back({v + static_cast<int>(v_offset), v + 1 + static_cast<int>(v_offset),
                                     v + 2 + static_cast<int>(v_offset)});
            if (out_mat_indices)
                out_mat_indices->push_back(mat_index);
        }
    }

    return static_cast<int>(out_triangles.size()) - triangles_before;
}

} // namespace resonance

#endif // RESONANCE_MESH_IPL_H
