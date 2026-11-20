#ifndef RESONANCE_SCENE_MANAGER_H
#define RESONANCE_SCENE_MANAGER_H

#include "resonance_geometry_asset.h"
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
class RayTraceDebugContext;

struct StaticMeshData {
    std::vector<IPLVector3> vertices;
    std::vector<IPLTriangle> triangles;
    std::vector<IPLint32> mat_indices;
    std::vector<IPLMaterial> materials;
};

/// hash is 0 when the scene has no exportable static geometry.
struct StaticSceneExport {
    int64_t hash = 0;
    Ref<ResonanceGeometryAsset> asset;
};

using StaticSceneHashFn = std::function<uint64_t(const PackedByteArray&)>;

/// The caller serializes all mutations, typically ResonanceServer under simulation_mutex.
struct RuntimeSceneState {
    std::vector<IPLStaticMesh>& meshes;
    int& tri_count;
    std::vector<int>& debug_ids;
    std::atomic<int>* global_triangle_count;
    std::atomic<bool>* scene_dirty;
    std::vector<IPLScene>& sub_scenes;
    std::vector<IPLInstancedMesh>& instanced_meshes;
    /// Parallel to instanced_meshes and sub_scenes; world transforms at add time.
    std::vector<IPLMatrix4x4>& instanced_transforms;
    /// Optional, parallel to sub_scenes. Each mesh must be removed from its sub-scene before release.
    std::vector<IPLStaticMesh>* meshes_in_sub = nullptr;

    RuntimeSceneState(std::vector<IPLStaticMesh>& m, int& tc, std::vector<int>& di, std::atomic<int>* gtc, std::atomic<bool>* sd,
                      std::vector<IPLScene>& ss, std::vector<IPLInstancedMesh>& im, std::vector<IPLMatrix4x4>& it,
                      std::vector<IPLStaticMesh>* mis = nullptr)
        : meshes(m), tri_count(tc), debug_ids(di), global_triangle_count(gtc), scene_dirty(sd), sub_scenes(ss), instanced_meshes(im),
          instanced_transforms(it), meshes_in_sub(mis) {}
};

/// The scene handle stays in ResonanceServer and is passed in.
class ResonanceSceneManager {
  public:
    ResonanceSceneManager() = default;

    ResonanceSceneManager(const ResonanceSceneManager&) = delete;
    ResonanceSceneManager& operator=(const ResonanceSceneManager&) = delete;
    ResonanceSceneManager(ResonanceSceneManager&&) = delete;
    ResonanceSceneManager& operator=(ResonanceSceneManager&&) = delete;

    void save_scene_data(IPLContext ctx, IPLScene scene, const String& filename);
    /// Releases a non-null *out_scene first. Returns true when a scene was installed: the loaded one, or an empty
    /// fallback with out_global_triangle_count at 0 when iplSceneLoad fails.
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

    /// Does not save the asset. Empty ref on failure.
    Ref<ResonanceGeometryAsset> export_static_scene_to_geometry_asset(Node* scene_root, IPLContext live_context);
    Error export_static_scene_to_asset(Node* scene_root, const String& path, IPLContext live_context);
    /// file_base_name has no extension, e.g. "res://debug/scene".
    Error export_static_scene_to_obj(Node* scene_root, const String& file_base_name, IPLContext live_context);
    /// Writes to a staging folder next to the destination, then renames, so editor reimport never reads a truncated file.
    static Error save_phonon_scene_obj_atomic(IPLScene phonon_scene, const String& absolute_obj_path);
    int64_t get_static_scene_hash(Node* scene_root, const StaticSceneHashFn& hash_fn);
    /// Hash and asset from one pass over the scene. The asset stays empty when the hash is 0, equals
    /// \p skip_if_hash, or the build fails.
    StaticSceneExport build_static_scene_export(Node* scene_root, IPLContext live_context, const StaticSceneHashFn& hash_fn,
                                                int64_t skip_if_hash);
    /// Saves through a temp file and rename so the editor never imports a half-written asset.
    static Error save_geometry_asset_atomic(const Ref<ResonanceGeometryAsset>& asset, const String& p_path);

    /// Serializes one IPL static mesh into a new asset (mesh data and triangle count). Empty ref on failure.
    /// Pass the running server's context as \p live_context, or nullptr when none exists.
    static Ref<ResonanceGeometryAsset> build_geometry_asset(std::vector<IPLVector3>& vertices, std::vector<IPLTriangle>& triangles,
                                                            std::vector<IPLint32>& mat_indices, std::vector<IPLMaterial>& materials,
                                                            IPLContext live_context);
    /// Appends .tres when the path has no resource extension and globalizes res:// and user:// paths.
    static String resource_save_path(const String& p_path);

  private:
    /// export_root is the initial scene_root; subtrees are pruned per should_prune_static_export_subtree.
    static void collect_static_geometry_recursive(Node* node, Node* export_root, std::vector<ResonanceGeometry*>& out);
    static StaticMeshData collect_static_mesh_data(Node* scene_root);
    static int64_t hash_static_mesh_data(const StaticMeshData& data, const StaticSceneHashFn& hash_fn);
    static Ref<ResonanceGeometryAsset> _build_static_scene_asset(StaticMeshData& data, IPLContext live_context);
    /// Builds temp context/scene/mesh from mesh data for export. Caller must release temp_mesh, temp_scene, export_context.
    /// Pass the running server's context as \p live_context, or nullptr when none exists.
    static bool _build_temp_scene_for_export(std::vector<IPLVector3>& vertices,
                                             std::vector<IPLTriangle>& triangles, std::vector<IPLint32>& mat_indices,
                                             std::vector<IPLMaterial>& materials, IPLContext live_context,
                                             IPLContext* out_ctx, IPLScene* out_scene, IPLStaticMesh* out_mesh);
    static int register_asset_debug_geometry(const Ref<ResonanceGeometryAsset>& asset, RayTraceDebugContext* debug_ctx,
                                             const Transform3D& transform = Transform3D());
};

} // namespace godot

#endif // RESONANCE_SCENE_MANAGER_H
