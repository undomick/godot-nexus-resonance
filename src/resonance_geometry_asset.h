#ifndef RESONANCE_GEOMETRY_ASSET_H
#define RESONANCE_GEOMETRY_ASSET_H

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

namespace godot {

/// Mesh data serialized by iplStaticMeshSave. A static asset merges all ResonanceStaticGeometry of a scene;
/// a dynamic asset holds the mesh of one ResonanceDynamicGeometry.
class ResonanceGeometryAsset : public Resource {
    GDCLASS(ResonanceGeometryAsset, Resource)

  private:
    PackedByteArray mesh_data;
    int triangle_count = 0;
    PackedVector3Array debug_vertices; // For the reflection ray debug view
    PackedInt32Array debug_triangles;  // Flat, 3 indices per triangle

  protected:
    static void _bind_methods();

  public:
    ResonanceGeometryAsset();
    ~ResonanceGeometryAsset();

    void set_mesh_data(const PackedByteArray& p_data);
    /// Copies the bytes; loading should use get_data_ptr / get_size instead.
    PackedByteArray get_mesh_data() const;

    void set_triangle_count(int p_count);
    int get_triangle_count() const { return triangle_count; }

    void set_debug_vertices(const PackedVector3Array& p_verts);
    PackedVector3Array get_debug_vertices() const { return debug_vertices; }
    void set_debug_triangles(const PackedInt32Array& p_tris);
    PackedInt32Array get_debug_triangles() const { return debug_triangles; }
    bool has_debug_geometry() const { return debug_vertices.size() >= 3 && debug_triangles.size() >= 3; }

    /// Valid until mesh_data is modified.
    const uint8_t* get_data_ptr() const;
    int64_t get_size() const;

    bool is_valid() const { return !mesh_data.is_empty(); }
};

} // namespace godot

#endif
