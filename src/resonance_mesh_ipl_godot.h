#ifndef RESONANCE_MESH_IPL_GODOT_H
#define RESONANCE_MESH_IPL_GODOT_H

#include "resonance_mesh_ipl.h"
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <vector>

namespace godot {

/// Parse Godot mesh surfaces into IPL vertex/triangle buffers. Returns true if this mesh added at least one triangle.
bool append_godot_mesh_to_ipl(const Ref<Mesh>& mesh, const Transform3D& xform, std::vector<IPLVector3>& out_vertices,
                              std::vector<IPLTriangle>& out_triangles, std::vector<IPLint32>* out_mat_indices = nullptr,
                              IPLint32 mat_index = 0);

} // namespace godot

#endif // RESONANCE_MESH_IPL_GODOT_H
