#include "resonance_mesh_ipl_godot.h"
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace godot {

namespace {

resonance::MeshBakeTransform to_mesh_bake_transform(const Transform3D& xform) {
    resonance::MeshBakeTransform t{};
    const Vector3 c0 = xform.basis.get_column(0);
    const Vector3 c1 = xform.basis.get_column(1);
    const Vector3 c2 = xform.basis.get_column(2);
    const Vector3 o = xform.origin;

    t.basis_col0[0] = static_cast<float>(c0.x);
    t.basis_col0[1] = static_cast<float>(c0.y);
    t.basis_col0[2] = static_cast<float>(c0.z);

    t.basis_col1[0] = static_cast<float>(c1.x);
    t.basis_col1[1] = static_cast<float>(c1.y);
    t.basis_col1[2] = static_cast<float>(c1.z);

    t.basis_col2[0] = static_cast<float>(c2.x);
    t.basis_col2[1] = static_cast<float>(c2.y);
    t.basis_col2[2] = static_cast<float>(c2.z);

    t.origin[0] = static_cast<float>(o.x);
    t.origin[1] = static_cast<float>(o.y);
    t.origin[2] = static_cast<float>(o.z);
    return t;
}

} // namespace

bool append_godot_mesh_to_ipl(const Ref<Mesh>& mesh, const Transform3D& xform, std::vector<IPLVector3>& out_vertices,
                              std::vector<IPLTriangle>& out_triangles, std::vector<IPLint32>* out_mat_indices,
                              IPLint32 mat_index) {
    if (mesh.is_null())
        return false;

    const resonance::MeshBakeTransform bake_xform = to_mesh_bake_transform(xform);
    const size_t triangles_before = out_triangles.size();

    for (int i = 0; i < mesh->get_surface_count(); i++) {
        Array arrays = mesh->surface_get_arrays(i);
        if (arrays.size() != Mesh::ARRAY_MAX)
            continue;

        PackedVector3Array vertices = arrays[Mesh::ARRAY_VERTEX];
        PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];
        if (vertices.size() < 3)
            continue;

        const int32_t* index_data = indices.is_empty() ? nullptr : indices.ptr();
        const int index_count = indices.is_empty() ? 0 : indices.size();

        std::vector<float> local_xyz(static_cast<size_t>(vertices.size()) * 3);
        for (int v = 0; v < vertices.size(); v++) {
            const Vector3 p = vertices[v];
            local_xyz[static_cast<size_t>(v) * 3 + 0] = static_cast<float>(p.x);
            local_xyz[static_cast<size_t>(v) * 3 + 1] = static_cast<float>(p.y);
            local_xyz[static_cast<size_t>(v) * 3 + 2] = static_cast<float>(p.z);
        }

        resonance::append_mesh_surface_to_ipl(local_xyz.data(), vertices.size(), index_data, index_count, bake_xform,
                                              out_vertices, out_triangles, out_mat_indices, mat_index);
    }

    return out_triangles.size() > triangles_before;
}

} // namespace godot
