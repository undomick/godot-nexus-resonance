#ifndef RESONANCE_GEOMETRY_H
#define RESONANCE_GEOMETRY_H

#include "resonance_geometry_asset.h"
#include "resonance_material.h"
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <phonon.h>
#include <vector>

namespace godot {

class ResonanceServer;

class ResonanceGeometry : public Node3D {
    GDCLASS(ResonanceGeometry, Node3D)

  private:
    Ref<ResonanceMaterial> material;
    Ref<ResonanceGeometryAsset> mesh_asset;
    Ref<Mesh> geometry_override;
    bool dynamic_object = false;
    bool show_geometry_override_in_viewport = false;
    bool export_all_children = false;

    MeshInstance3D* viz_geometry_override = nullptr; // Editor only

    std::vector<IPLStaticMesh> static_meshes;

    IPLScene sub_scene = nullptr;
    IPLInstancedMesh instanced_mesh = nullptr; // Places sub_scene in the global scene

    int triangle_count = 0;
    int debug_mesh_id = -1; // RayTraceDebugContext mesh id

    /// Per instance, so one busy mover cannot starve the others as a global round-robin would.
    uint32_t transform_coalesce_counter_ = 0;
    bool consume_transform_coalesce_tick();

    /// _create_meshes can run before ResonanceServer::init_audio_engine; it then retries on later frames.
    static constexpr int kMaxServerInitRetries = 64;
    bool server_init_retry_pending_ = false;
    int server_init_retry_count_ = 0;

    void _create_meshes();
    bool _create_meshes_from_asset(ResonanceServer* server);
    bool _create_meshes_from_mesh(ResonanceServer* server);
    bool _has_mesh_source() const;
    bool _install_dynamic_instance(ResonanceServer* server, IPLScene new_sub, IPLStaticMesh new_mesh, int new_triangle_count);
    /// True for static geometry outside the editor while a ResonanceStaticScene in this tree holds a valid asset.
    bool _is_covered_by_static_scene() const;
    void _schedule_retry_create_meshes_when_server_ready();
    void _deferred_retry_create_meshes();
    void _clear_meshes();
    /// Swaps the material on the existing IPLStaticMesh handles without a rebuild.
    void _update_material_inplace();
    void _propagate_material_and_geometry_to_descendants();
    /// Does not lock: the caller holds the simulation lock when the scene is touched.
    /// notify_server = false leaves triangle accounting to the caller. for_shutdown skips the per-node
    /// iplSceneCommit because the server releases the scene once.
    void _clear_meshes_impl(bool notify_server = true, bool for_shutdown = false);
    void _update_dynamic_transform();
    /// include_static_scene_query = false keeps the cached static scene lookup, which survives an IPL teardown.
    void _invalidate_topology_caches(bool include_static_scene_query = true);

    /// With geometry_override: the MeshInstance3D that owns the same mesh, cached to skip the subtree search.
    mutable ObjectID bake_override_mesh_instance_id_;
    mutable bool bake_transform_override_cache_valid_ = false;
    mutable bool static_scene_asset_query_valid_ = false;
    mutable ObjectID cached_scene_tree_root_id_;
    mutable bool cached_root_has_static_scene_asset_ = false;
    /// Reused by the reflection debug view while the source mesh and transform are unchanged.
    Ref<Mesh> reflection_debug_parsed_mesh_;
    Transform3D reflection_debug_parsed_xform_;
    std::vector<IPLVector3> reflection_debug_parsed_vertices_;
    std::vector<IPLTriangle> reflection_debug_parsed_triangles_;
    std::vector<IPLint32> reflection_debug_parsed_mat_indices_;
    void _store_reflection_debug_parse(std::vector<IPLVector3>&& vertices, std::vector<IPLTriangle>&& triangles,
                                       std::vector<IPLint32>&& mat_indices);
    bool _reflection_debug_parse_is_current() const;
    void _register_reflection_debug_mesh(ResonanceServer* server, const IPLMaterial& mat);
    void _queue_mesh_detach(ResonanceServer* server);
    void _detach_meshes_now(ResonanceServer* server);

  protected:
    static void _bind_methods();
    void _notification(int p_what);
    virtual void _validate_property(PropertyInfo& p_property) const;

  public:
    ResonanceGeometry();
    ~ResonanceGeometry();

    ResonanceGeometry(const ResonanceGeometry&) = delete;
    ResonanceGeometry(ResonanceGeometry&&) = delete;

    void _ready() override;
    void _exit_tree() override;

    void set_material(const Ref<ResonanceMaterial>& p_material);
    Ref<ResonanceMaterial> get_material() const;

    /// Re-registers with the server, e.g. after the server init for an editor bake.
    void refresh_geometry();

    /// Re-checks static scene coverage: removes live static meshes once a static pack covers them,
    /// registers them again once it no longer does. No-op for dynamic geometry.
    void sync_static_scene_coverage();

    /// Commits the current transform right away. Call when a tween or animation ends, so occlusion and path
    /// validation do not keep a stale pose from throttled updates.
    void flush_dynamic_acoustic_transform();

    /// Touches only the debug triangles: rebuilding the IPL meshes here would make Embree lose dynamic occlusion.
    void sync_reflection_debug_viz();

    /// Mesh space to world: this node's transform with geometry_override, else the parent MeshInstance3D's.
    Transform3D get_mesh_bake_transform() const;

    /// geometry_override when set, else the parent MeshInstance3D mesh.
    Ref<Mesh> get_source_mesh() const;

    /// Static vertices come out in world space, dynamic ones in mesh space. False when no triangle was added.
    bool append_meshes_to_ipl(std::vector<IPLVector3>& out_vertices, std::vector<IPLTriangle>& out_triangles,
                              std::vector<IPLint32>& out_mat_indices, IPLint32 mat_index);
    bool includes_child_meshes() const;

    /// Removes and releases the meshes; call before the server releases the scene.
    void discard_meshes_before_scene_release();

    void set_dynamic(bool p_dynamic);
    bool is_dynamic() const;

    void set_mesh_asset(const Ref<ResonanceGeometryAsset>& p_asset);
    Ref<ResonanceGeometryAsset> get_mesh_asset() const;

    void set_geometry_override(const Ref<Mesh>& p_mesh);
    Ref<Mesh> get_geometry_override() const;

    void set_show_geometry_override_in_viewport(bool p_show);
    bool is_show_geometry_override_in_viewport() const;

    /// No effect on dynamic geometry.
    void set_export_all_children(bool p_export);
    bool get_export_all_children() const;

    void _update_viz_geometry_override(); // Editor only

    /// Works in the editor without running the scene. Saves to p_path and assigns the result to mesh_asset.
    Error export_dynamic_mesh_to_asset(const String& p_path);
};

} // namespace godot

#endif // RESONANCE_GEOMETRY_H