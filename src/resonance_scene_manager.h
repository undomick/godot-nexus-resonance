#ifndef RESONANCE_SCENE_MANAGER_H
#define RESONANCE_SCENE_MANAGER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <phonon.h>
#include <vector>

namespace godot {

class ResonanceGeometry;
class ResonanceGeometryAsset;
class RayTraceDebugContext;

/// Bundles runtime static scene state for add/load/clear operations (reduces parameter duplication).
/// All mutations are serialized by the caller (typically ResonanceServer holding simulation_mutex).
struct RuntimeSceneState {
    std::vector<IPLStaticMesh>& meshes;
    int& tri_count;
    std::vector<int>& debug_ids;
    std::atomic<int>* global_triangle_count;
    std::atomic<bool>* scene_dirty;
    std::vector<IPLScene>& sub_scenes;
    std::vector<IPLInstancedMesh>& instanced_meshes;
    /// Parallel to instanced_meshes / sub_scenes (world transform at add time).
    std::vector<IPLMatrix4x4>& instanced_transforms;
    /// Optional: parallel to sub_scenes. Instanced StaticMesh retains (must Remove from sub before Release).
    std::vector<IPLStaticMesh>* meshes_in_sub = nullptr;

    RuntimeSceneState(std::vector<IPLStaticMesh>& m, int& tc, std::vector<int>& di, std::atomic<int>* gtc, std::atomic<bool>* sd,
                      std::vector<IPLScene>& ss, std::vector<IPLInstancedMesh>& im, std::vector<IPLMatrix4x4>& it,
                      std::vector<IPLStaticMesh>* mis = nullptr)
        : meshes(m), tri_count(tc), debug_ids(di), global_triangle_count(gtc), scene_dirty(sd), sub_scenes(ss), instanced_meshes(im),
          instanced_transforms(it), meshes_in_sub(mis) {}
};

/// Manages scene I/O, static meshes from assets, save/load, export, and hash.
/// Scene handle remains in ResonanceServer; this class operates on it via parameters.
class ResonanceSceneManager {
  public:
    ResonanceSceneManager() = default;

    ResonanceSceneManager(const ResonanceSceneManager&) = delete;
    ResonanceSceneManager& operator=(const ResonanceSceneManager&) = delete;
    ResonanceSceneManager(ResonanceSceneManager&&) = delete;
    ResonanceSceneManager& operator=(ResonanceSceneManager&&) = delete;

    void save_scene_data(IPLContext ctx, IPLScene scene, const String& filename);
    /// Load scene from serialized file. On failure, optionally creates empty scene and sets out_global_triangle_count to 0.
    /// @param out_scene Must not be null. Will be released if non-null before loading.
    /// @return true if a new scene handle was installed and the simulator was updated.
    bool load_scene_data(IPLContext ctx, IPLScene* out_scene, IPLSimulator sim,
                         IPLSceneType scene_type, IPLEmbreeDevice embree, IPLRadeonRaysDevice radeon,
                         const String& filename, std::atomic<int>* out_global_triangle_count);

    void add_static_scene_from_asset(IPLContext ctx, IPLScene scene, const Ref<ResonanceGeometryAsset>& asset,
                                     RayTraceDebugContext* debug_ctx, bool wants_debug_viz, RuntimeSceneState& state,
                                     const Transform3D& transform, IPLSceneType scene_type, IPLEmbreeDevice embree, IPLRadeonRaysDevice radeon,
                                     bool force_instanced = false);

    void load_static_scene_from_asset(IPLContext ctx, IPLScene scene, const Ref<ResonanceGeometryAsset>& asset,
                                      RayTraceDebugContext* debug_ctx, bool wants_debug_viz, RuntimeSceneState& state,
                                      const Transform3D& transform, IPLSceneType scene_type, IPLEmbreeDevice embree, IPLRadeonRaysDevice radeon);

    void clear_static_scenes(IPLScene scene, RayTraceDebugContext* debug_ctx, RuntimeSceneState& state);

    Error export_static_scene_to_asset(Node* scene_root, const String& path);
    /// Export static ResonanceGeometry from scene to OBJ+MTL (iplSceneSaveOBJ). Path without extension, e.g. "res://debug/scene".
    Error export_static_scene_to_obj(Node* scene_root, const String& file_base_name);
    /// Writes Phonon scene to OBJ+MTL via _nexus_obj_staging next to the destination, then rename (avoids truncate races with editor reimport).
    static Error save_phonon_scene_obj_atomic(IPLScene phonon_scene, const String& absolute_obj_path);
    int64_t get_static_scene_hash(Node* scene_root, std::function<uint64_t(const PackedByteArray&)> hash_fn);

  private:
    /// export_root: same as initial scene_root. Nested ResonanceStaticScene always prunes. PackedScene instance
    /// roots (non-empty scene_file_path) that contain an RSS prune the whole instance. The export root is never pruned.
    static void collect_static_geometry_recursive(Node* node, Node* export_root, std::vector<ResonanceGeometry*>& out);
    /// When \p out_mat_indices is non-null, \p out_materials must also be non-null (and vice versa).
    static void collect_static_mesh_data(Node* scene_root, std::vector<IPLVector3>& out_vertices,
                                         std::vector<IPLTriangle>& out_triangles, std::vector<IPLint32>* out_mat_indices,
                                         std::vector<IPLMaterial>* out_materials);
    /// Builds temp context/scene/mesh from mesh data for export. Caller must release temp_mesh, temp_scene, export_context.
    static bool _build_temp_scene_for_export(std::vector<IPLVector3>& vertices,
                                             std::vector<IPLTriangle>& triangles, std::vector<IPLint32>& mat_indices,
                                             std::vector<IPLMaterial>& materials,
                                             IPLContext* out_ctx, IPLScene* out_scene, IPLStaticMesh* out_mesh);
    static int register_asset_debug_geometry(const Ref<ResonanceGeometryAsset>& asset, RayTraceDebugContext* debug_ctx,
                                             const Transform3D& transform = Transform3D());
};

} // namespace godot

#endif // RESONANCE_SCENE_MANAGER_H
