#include "resonance_scene_manager.h"
#include "ray_trace_debug_context.h"
#include "resonance_constants.h"
#include "resonance_export_transform.h"
#include "resonance_geometry.h"
#include "resonance_geometry_asset.h"
#include "resonance_ipl_guard.h"
#include "resonance_log.h"
#include "resonance_material.h"
#include "resonance_static_export_policy.h"
#include "resonance_static_scene.h"
#include "resonance_steam_audio_context.h"
#include "resonance_utils.h"
#include <cstdint>
#include <cstring>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <unordered_map>

namespace godot {

namespace {

String globalize_scene_file_path(const String& filename) {
    String path = filename;
    ProjectSettings* ps = ProjectSettings::get_singleton();
    if (ps && (path.begins_with("res://") || path.begins_with("user://")))
        path = ps->globalize_path(path);
    return path;
}

// Packs the fields one by one so IPLMaterial padding bytes never reach the hash.
static void append_ipl_materials_hash_bytes(const std::vector<IPLMaterial>& materials, uint8_t* dst) {
    uint8_t* p = dst;
    for (const IPLMaterial& m : materials) {
        memcpy(p, m.absorption, sizeof(m.absorption));
        p += sizeof(m.absorption);
        memcpy(p, &m.scattering, sizeof(m.scattering));
        p += sizeof(m.scattering);
        memcpy(p, m.transmission, sizeof(m.transmission));
        p += sizeof(m.transmission);
    }
}

static size_t ipl_materials_hash_byte_count(size_t num_materials) {
    return num_materials * (sizeof(IPLfloat32) * (IPL_NUM_BANDS + 1 + IPL_NUM_BANDS));
}

void clear_nexus_obj_staging_best_effort(const String& staging_dir, const String& staging_obj, const String& staging_mtl) {
    if (FileAccess::file_exists(staging_obj))
        DirAccess::remove_absolute(staging_obj);
    if (FileAccess::file_exists(staging_mtl))
        DirAccess::remove_absolute(staging_mtl);
    if (!DirAccess::dir_exists_absolute(staging_dir))
        return;
    const PackedStringArray files = DirAccess::get_files_at(staging_dir);
    for (int i = 0; i < files.size(); ++i)
        DirAccess::remove_absolute(staging_dir.path_join(files[i]));
    const PackedStringArray subdirs = DirAccess::get_directories_at(staging_dir);
    for (int i = 0; i < subdirs.size(); ++i) {
        const String d = subdirs[i];
        if (!d.is_empty())
            DirAccess::remove_absolute(staging_dir.path_join(d));
    }
    if (DirAccess::dir_exists_absolute(staging_dir))
        DirAccess::remove_absolute(staging_dir);
}

} // namespace

void ResonanceSceneManager::collect_static_geometry_recursive(Node* node, Node* export_root, std::vector<ResonanceGeometry*>& out) {
    if (!node)
        return;
    const bool is_export_root = (node == export_root);
    const bool is_rss = node->is_class("ResonanceStaticScene");
    const bool has_scene_path = !node->get_scene_file_path().is_empty();
    // Instance prune needs a subtree scan; skip when already pruning as nested RSS.
    const bool subtree_rss = !is_export_root && !is_rss && has_scene_path && subtree_has_static_scene(node, StaticSceneQuery::Any);
    if (resonance::should_prune_static_export_subtree(is_export_root, is_rss, has_scene_path, subtree_rss))
        return;
    if (node->is_class("ResonanceGeometry")) {
        ResonanceGeometry* geom = Object::cast_to<ResonanceGeometry>(node);
        if (geom && !geom->is_dynamic()) {
            out.push_back(geom);
        }
    }
    for (int i = 0; i < node->get_child_count(); i++) {
        collect_static_geometry_recursive(node->get_child(i), export_root, out);
    }
}

// Flattens non-dynamic ResonanceGeometry meshes (plus child MeshInstance3D when export_all_children) into one
// buffer set. Each distinct ResonanceMaterial gets one table entry; index 0 is the default material.
StaticMeshData ResonanceSceneManager::collect_static_mesh_data(Node* scene_root) {
    StaticMeshData out;
    std::vector<ResonanceGeometry*> static_geoms;
    collect_static_geometry_recursive(scene_root, scene_root, static_geoms);

    std::unordered_map<uint64_t, int32_t> res_to_index;
    out.materials.push_back(ResonanceMaterial::default_ipl_material());

    auto material_index_for_geom = [&](ResonanceGeometry* geom) -> int32_t {
        Ref<ResonanceMaterial> rm = geom->get_material();
        if (rm.is_null())
            return 0;
        uint64_t rid = rm->get_instance_id();
        auto found = res_to_index.find(rid);
        if (found != res_to_index.end())
            return found->second;
        const int32_t idx = static_cast<int32_t>(out.materials.size());
        out.materials.push_back(rm->get_ipl_material());
        res_to_index.emplace(rid, idx);
        return idx;
    };

    for (ResonanceGeometry* geom : static_geoms) {
        Node3D* parent = Object::cast_to<Node3D>(geom->get_parent());
        if (!parent || !ResonanceUtils::node3d_visible_for_export(parent))
            continue;
        geom->append_meshes_to_ipl(out.vertices, out.triangles, out.mat_indices, material_index_for_geom(geom));
    }
    return out;
}

bool ResonanceSceneManager::_build_temp_scene_for_export(std::vector<IPLVector3>& vertices,
                                                         std::vector<IPLTriangle>& triangles, std::vector<IPLint32>& mat_indices,
                                                         std::vector<IPLMaterial>& materials, IPLContext live_context,
                                                         IPLContext* out_ctx, IPLScene* out_scene, IPLStaticMesh* out_mesh) {
    *out_ctx = nullptr;
    *out_scene = nullptr;
    *out_mesh = nullptr;

    if (materials.empty())
        materials.push_back(ResonanceMaterial::default_ipl_material());

    if (mat_indices.size() != triangles.size()) {
        ResonanceLog::error("ResonanceSceneManager: material index count does not match triangle count (_build_temp_scene_for_export).");
        return false;
    }
    const IPLint32 num_mats = (IPLint32)materials.size();
    for (IPLint32 mi : mat_indices) {
        if (mi < 0 || mi >= num_mats) {
            ResonanceLog::error("ResonanceSceneManager: invalid material index (_build_temp_scene_for_export).");
            return false;
        }
    }

    IPLStaticMeshSettings mesh_settings{};
    mesh_settings.materials = materials.data();
    mesh_settings.numVertices = (IPLint32)vertices.size();
    mesh_settings.numTriangles = (IPLint32)triangles.size();
    mesh_settings.numMaterials = num_mats;
    mesh_settings.vertices = vertices.data();
    mesh_settings.triangles = triangles.data();
    mesh_settings.materialIndices = mat_indices.data();

    IPLContext export_context = acquire_export_context(live_context);
    if (!export_context) {
        ResonanceLog::error("ResonanceSceneManager: no Steam Audio context for export (_build_temp_scene_for_export).");
        return false;
    }
    IPLScopedRelease<IPLContext> ctxGuard(export_context, iplContextRelease);

    IPLScene temp_scene = nullptr;
    IPLSceneSettings scene_settings{};
    scene_settings.type = IPL_SCENETYPE_DEFAULT;
    if (iplSceneCreate(export_context, &scene_settings, &temp_scene) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceSceneManager: iplSceneCreate failed (_build_temp_scene_for_export).");
        return false;
    }
    IPLScopedRelease<IPLScene> sceneGuard(temp_scene, iplSceneRelease);

    IPLStaticMesh temp_mesh = nullptr;
    if (iplStaticMeshCreate(temp_scene, &mesh_settings, &temp_mesh) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceSceneManager: iplStaticMeshCreate failed (_build_temp_scene_for_export).");
        return false;
    }
    IPLScopedRelease<IPLStaticMesh> meshGuard(temp_mesh, iplStaticMeshRelease);

    iplStaticMeshAdd(temp_mesh, temp_scene);
    iplSceneCommit(temp_scene);

    *out_ctx = export_context;
    *out_scene = temp_scene;
    *out_mesh = temp_mesh;
    ctxGuard.detach();
    sceneGuard.detach();
    meshGuard.detach();
    return true;
}

int ResonanceSceneManager::register_asset_debug_geometry(const Ref<ResonanceGeometryAsset>& asset, RayTraceDebugContext* debug_ctx,
                                                         const Transform3D& transform) {
    if (!asset.is_valid() || !asset->has_debug_geometry() || !debug_ctx)
        return -1;
    PackedVector3Array pv = asset->get_debug_vertices();
    PackedInt32Array pt = asset->get_debug_triangles();
    std::vector<IPLVector3> ipl_verts;
    ipl_verts.resize(pv.size());
    for (int i = 0; i < pv.size(); i++) {
        Vector3 v = pv[i];
        ipl_verts[i] = {v.x, v.y, v.z};
    }
    std::vector<IPLTriangle> ipl_tris;
    const int64_t tri_index_count = pt.size();
    int num_tris = static_cast<int>(tri_index_count / 3);
    ipl_tris.resize(num_tris);
    std::vector<IPLint32> ipl_mat_indices(num_tris, 0);
    for (int i = 0; i < num_tris && (i * 3 + 2) < pt.size(); i++) {
        ipl_tris[i].indices[0] = pt[i * 3 + 0];
        ipl_tris[i].indices[1] = pt[i * 3 + 1];
        ipl_tris[i].indices[2] = pt[i * 3 + 2];
    }
    // Uniform low absorption for debug visualization.
    IPLMaterial mat = ResonanceMaterial::default_ipl_material();
    mat.absorption[1] = mat.absorption[2] = resonance::kSceneExportAbsorptionLow;
    IPLMatrix4x4 ipl_xform = ResonanceUtils::to_ipl_matrix(transform);
    return debug_ctx->register_mesh(ipl_verts, ipl_tris, ipl_mat_indices.data(), &ipl_xform, &mat);
}

void ResonanceSceneManager::save_scene_data(IPLContext ctx, IPLScene scene, const String& filename) {
    const String path = globalize_scene_file_path(filename);
    if (!ctx) {
        ResonanceLog::error("ResonanceSceneManager: Context is null (save_scene_data).");
        return;
    }
    if (!scene) {
        UtilityFunctions::push_warning("Nexus Resonance: No scene to save.");
        return;
    }
    IPLSerializedObjectSettings serialSettings{};
    IPLSerializedObject serializedObject = nullptr;
    if (iplSerializedObjectCreate(ctx, &serialSettings, &serializedObject) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceSceneManager: iplSerializedObjectCreate failed (save_scene_data).");
        return;
    }
    // iplSceneSave returns nothing; empty serialized data is the only failure signal.
    iplSceneSave(scene, serializedObject);

    IPLsize size = iplSerializedObjectGetSize(serializedObject);
    IPLbyte* data = iplSerializedObjectGetData(serializedObject);
    if (size == 0 || !data) {
        UtilityFunctions::push_error("Nexus Resonance: Scene save produced no data. Possible causes: empty scene or no geometry committed. Check Steam Audio log output for details.");
        iplSerializedObjectRelease(&serializedObject);
        return;
    }

    Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
    if (file.is_valid()) {
        PackedByteArray pba;
        pba.resize((int64_t)size);
        memcpy(pba.ptrw(), data, size);
        file->store_buffer(pba);
        file->close();
        UtilityFunctions::print_rich("[color=cyan]Nexus Resonance:[/color] Scene saved successfully to " + path);
    } else {
        UtilityFunctions::push_error("Nexus Resonance: Failed to open file for writing: ", path);
    }
    iplSerializedObjectRelease(&serializedObject);
}

bool ResonanceSceneManager::load_scene_data(IPLContext ctx, IPLScene* out_scene, IPLSimulator sim,
                                            IPLSceneType scene_type, IPLEmbreeDevice embree, IPLRadeonRaysDevice radeon,
                                            const String& filename, std::atomic<int>* out_global_triangle_count) {
    const String path = globalize_scene_file_path(filename);
    if (!out_scene) {
        ResonanceLog::error("ResonanceSceneManager: out_scene is null (load_scene_data).");
        return false;
    }
    if (!sim) {
        ResonanceLog::error("ResonanceSceneManager: Simulator is null (load_scene_data).");
        return false;
    }
    if (!ctx) {
        ResonanceLog::error("ResonanceSceneManager: Context is null (load_scene_data).");
        return false;
    }
    if (!FileAccess::file_exists(path)) {
        UtilityFunctions::push_error("Nexus Resonance: File not found: ", path);
        return false;
    }
    Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
    if (file.is_null()) {
        ResonanceLog::error("ResonanceSceneManager: Failed to open file for reading: " + path);
        return false;
    }

    const int64_t file_len = static_cast<int64_t>(file->get_length());
    PackedByteArray pba = file->get_buffer(file_len);
    file->close();

    if (pba.is_empty()) {
        UtilityFunctions::push_error("Nexus Resonance: Scene file is empty.");
        return false;
    }

    if (*out_scene) {
        iplSceneRelease(out_scene);
        *out_scene = nullptr;
    }

    IPLSerializedObjectSettings serialSettings{};
    serialSettings.data = (IPLbyte*)pba.ptr();
    serialSettings.size = pba.size();

    IPLSerializedObject serializedObject = nullptr;
    if (iplSerializedObjectCreate(ctx, &serialSettings, &serializedObject) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceSceneManager: iplSerializedObjectCreate failed (load_scene_data).");
        return false;
    }

    IPLSceneSettings sceneSettings{};
    sceneSettings.type = scene_type;
    sceneSettings.embreeDevice = embree;
    sceneSettings.radeonRaysDevice = radeon;

    IPLerror status = iplSceneLoad(ctx, &sceneSettings, serializedObject, nullptr, nullptr, out_scene);
    iplSerializedObjectRelease(&serializedObject);

    if (status == IPL_STATUS_SUCCESS) {
        iplSimulatorSetScene(sim, *out_scene);
        iplSimulatorCommit(sim);
        // A loaded scene does not expose its triangle count; 1 marks it as loaded for is_simulating().
        if (out_global_triangle_count)
            out_global_triangle_count->store(1, std::memory_order_release);
        UtilityFunctions::print_rich("[color=cyan]Nexus Resonance:[/color] Scene loaded successfully from " + path);
        return true;
    }
    UtilityFunctions::push_error("Nexus Resonance: Failed to load scene.");
    *out_scene = nullptr;
    IPLerror fallback_status = iplSceneCreate(ctx, &sceneSettings, out_scene);
    if (fallback_status != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceSceneManager: iplSceneCreate failed (load_scene_data fallback).");
        return false;
    }
    iplSimulatorSetScene(sim, *out_scene);
    iplSimulatorCommit(sim);
    if (out_global_triangle_count)
        out_global_triangle_count->store(0, std::memory_order_release);
    return true;
}

void ResonanceSceneManager::add_static_scene_from_asset(IPLContext ctx, IPLScene scene, const Ref<ResonanceGeometryAsset>& asset,
                                                        RayTraceDebugContext* debug_ctx, bool wants_debug_viz, RuntimeSceneState& state,
                                                        const Transform3D& transform, IPLSceneType scene_type, IPLEmbreeDevice embree,
                                                        IPLRadeonRaysDevice radeon, bool force_instanced) {
    if (!ctx) {
        ResonanceLog::error("ResonanceSceneManager: null context (add_static_scene_from_asset).");
        return;
    }
    if (!asset.is_valid() || !asset->is_valid() || !scene || asset->get_size() == 0)
        return;

    IPLSerializedObjectSettings serialSettings{};
    serialSettings.data = const_cast<IPLbyte*>(reinterpret_cast<const IPLbyte*>(asset->get_data_ptr()));
    serialSettings.size = static_cast<IPLsize>(asset->get_size());
    IPLSerializedObject serialObj = nullptr;
    if (iplSerializedObjectCreate(ctx, &serialSettings, &serialObj) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceSceneManager: iplSerializedObjectCreate failed (add_static_scene_from_asset).");
        return;
    }

    const bool use_instanced = force_instanced || !transform.is_equal_approx(Transform3D());
    int tri = asset->get_triangle_count();

    if (use_instanced) {
        IPLSceneSettings subSceneSettings{};
        subSceneSettings.type = scene_type;
        subSceneSettings.embreeDevice = embree;
        subSceneSettings.radeonRaysDevice = radeon;
        IPLScene sub_scene = nullptr;
        if (iplSceneCreate(ctx, &subSceneSettings, &sub_scene) != IPL_STATUS_SUCCESS) {
            ResonanceLog::error("ResonanceSceneManager: iplSceneCreate failed (add_static_scene_from_asset instanced).");
            iplSerializedObjectRelease(&serialObj);
            return;
        }

        IPLStaticMesh loadMesh = nullptr;
        if (iplStaticMeshLoad(sub_scene, serialObj, nullptr, nullptr, &loadMesh) != IPL_STATUS_SUCCESS) {
            ResonanceLog::error("ResonanceSceneManager: iplStaticMeshLoad failed (add_static_scene_from_asset instanced).");
            iplSerializedObjectRelease(&serialObj);
            iplSceneRelease(&sub_scene);
            return;
        }
        iplSerializedObjectRelease(&serialObj);
        iplStaticMeshAdd(loadMesh, sub_scene);
        iplSceneCommit(sub_scene);

        IPLInstancedMeshSettings instSettings{};
        instSettings.subScene = sub_scene;
        instSettings.transform = ResonanceUtils::to_ipl_matrix(transform);

        IPLInstancedMesh inst_mesh = nullptr;
        if (iplInstancedMeshCreate(scene, &instSettings, &inst_mesh) != IPL_STATUS_SUCCESS) {
            ResonanceLog::error("ResonanceSceneManager: iplInstancedMeshCreate failed (add_static_scene_from_asset).");
            iplStaticMeshRemove(loadMesh, sub_scene);
            iplStaticMeshRelease(&loadMesh);
            iplSceneRelease(&sub_scene);
            return;
        }
        iplInstancedMeshAdd(inst_mesh, scene);
        iplSceneCommit(scene);

        state.sub_scenes.push_back(sub_scene);
        state.instanced_meshes.push_back(inst_mesh);
        state.instanced_transforms.push_back(ResonanceUtils::to_ipl_matrix(transform));
        // Keep the StaticMesh retain so callers can Remove+Release from the sub-scene.
        if (state.meshes_in_sub)
            state.meshes_in_sub->push_back(loadMesh);
    } else {
        IPLStaticMesh loadMesh = nullptr;
        if (iplStaticMeshLoad(scene, serialObj, nullptr, nullptr, &loadMesh) != IPL_STATUS_SUCCESS) {
            ResonanceLog::error("ResonanceSceneManager: iplStaticMeshLoad failed (add_static_scene_from_asset direct).");
            iplSerializedObjectRelease(&serialObj);
            return;
        }
        iplSerializedObjectRelease(&serialObj);

        iplStaticMeshAdd(loadMesh, scene);
        iplSceneCommit(scene);
        state.meshes.push_back(loadMesh);
    }

    state.tri_count += tri;
    if (tri > 0 && state.global_triangle_count)
        state.global_triangle_count->fetch_add(tri, std::memory_order_release);
    if (state.scene_dirty)
        state.scene_dirty->store(true);
    if (wants_debug_viz && debug_ctx) {
        int dbg_id = register_asset_debug_geometry(asset, debug_ctx, transform);
        if (dbg_id >= 0)
            state.debug_ids.push_back(dbg_id);
    }
}

void ResonanceSceneManager::load_static_scene_from_asset(IPLContext ctx, IPLScene scene, const Ref<ResonanceGeometryAsset>& asset,
                                                         RayTraceDebugContext* debug_ctx, bool wants_debug_viz, RuntimeSceneState& state,
                                                         const Transform3D& transform, IPLSceneType scene_type, IPLEmbreeDevice embree, IPLRadeonRaysDevice radeon) {
    if (!scene)
        return;
    clear_static_scenes(scene, debug_ctx, state);

    if (!asset.is_valid() || !asset->is_valid())
        return;
    add_static_scene_from_asset(ctx, scene, asset, debug_ctx, wants_debug_viz, state, transform, scene_type, embree, radeon);
}

void ResonanceSceneManager::clear_static_scenes(IPLScene scene, RayTraceDebugContext* debug_ctx, RuntimeSceneState& state) {
    if (!scene)
        return;
    for (int id : state.debug_ids) {
        if (debug_ctx)
            debug_ctx->unregister_mesh(id);
    }
    state.debug_ids.clear();
    for (IPLStaticMesh& m : state.meshes) {
        if (m) {
            iplStaticMeshRemove(m, scene);
            iplStaticMeshRelease(&m);
        }
    }
    state.meshes.clear();
    const bool had_instanced = !state.instanced_meshes.empty();
    for (IPLInstancedMesh& im : state.instanced_meshes) {
        if (im) {
            iplInstancedMeshRemove(im, scene);
            iplInstancedMeshRelease(&im);
        }
    }
    state.instanced_meshes.clear();
    state.instanced_transforms.clear();
    // Commit the parent after InstancedMeshRemove and before the sub-scene meshes are released.
    if (had_instanced && scene)
        iplSceneCommit(scene);
    if (state.meshes_in_sub) {
        const size_t n = state.meshes_in_sub->size();
        for (size_t i = 0; i < n; i++) {
            IPLStaticMesh& m = (*state.meshes_in_sub)[i];
            IPLScene sub = (i < state.sub_scenes.size()) ? state.sub_scenes[i] : nullptr;
            if (m && sub) {
                iplStaticMeshRemove(m, sub);
                iplStaticMeshRelease(&m);
            }
            m = nullptr;
        }
        state.meshes_in_sub->clear();
    }
    for (IPLScene& sub : state.sub_scenes) {
        if (sub)
            iplSceneRelease(&sub);
    }
    state.sub_scenes.clear();
    if (state.tri_count > 0 && state.global_triangle_count) {
        const int sub = state.tri_count;
        const int prev = state.global_triangle_count->fetch_sub(sub, std::memory_order_release);
        if (prev < sub)
            state.global_triangle_count->store(0, std::memory_order_release);
    }
    state.tri_count = 0;
    if (state.scene_dirty)
        state.scene_dirty->store(true);
}

Ref<ResonanceGeometryAsset> ResonanceSceneManager::build_geometry_asset(std::vector<IPLVector3>& vertices,
                                                                        std::vector<IPLTriangle>& triangles,
                                                                        std::vector<IPLint32>& mat_indices,
                                                                        std::vector<IPLMaterial>& materials, IPLContext live_context) {
    Ref<ResonanceGeometryAsset> empty;
    IPLContext export_context = nullptr;
    IPLScene temp_scene = nullptr;
    IPLStaticMesh temp_mesh = nullptr;
    if (!_build_temp_scene_for_export(vertices, triangles, mat_indices, materials, live_context, &export_context, &temp_scene, &temp_mesh))
        return empty;
    IPLScopedRelease<IPLContext> ctx_guard(export_context, iplContextRelease);
    IPLScopedRelease<IPLScene> scene_guard(temp_scene, iplSceneRelease);
    IPLScopedRelease<IPLStaticMesh> mesh_guard(temp_mesh, iplStaticMeshRelease);

    IPLSerializedObjectSettings serial_settings{};
    IPLSerializedObject serial_obj = nullptr;
    if (iplSerializedObjectCreate(export_context, &serial_settings, &serial_obj) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceSceneManager: iplSerializedObjectCreate failed (build_geometry_asset).");
        return empty;
    }
    IPLScopedRelease<IPLSerializedObject> serial_guard(serial_obj, iplSerializedObjectRelease);

    iplStaticMeshSave(temp_mesh, serial_obj);

    const IPLsize size = iplSerializedObjectGetSize(serial_obj);
    const IPLbyte* data = iplSerializedObjectGetData(serial_obj);
    if (!data || size == 0) {
        ResonanceLog::error("ResonanceSceneManager: iplStaticMeshSave produced no data (build_geometry_asset).");
        return empty;
    }

    Ref<ResonanceGeometryAsset> asset;
    asset.instantiate();
    PackedByteArray pba;
    pba.resize(static_cast<int64_t>(size));
    memcpy(pba.ptrw(), data, size);
    asset->set_mesh_data(pba);
    asset->set_triangle_count(static_cast<int>(triangles.size()));
    return asset;
}

static void set_asset_debug_geometry(ResonanceGeometryAsset& asset, const std::vector<IPLVector3>& vertices,
                                     const std::vector<IPLTriangle>& triangles) {
    PackedVector3Array dbg_verts;
    dbg_verts.resize(static_cast<int64_t>(vertices.size()));
    if (!vertices.empty()) {
        static_assert(sizeof(IPLVector3) == sizeof(Vector3), "IPLVector3 must match Godot Vector3 layout for bulk copy");
        std::memcpy(reinterpret_cast<Vector3*>(dbg_verts.ptrw()), vertices.data(), vertices.size() * sizeof(IPLVector3));
    }
    PackedInt32Array dbg_tris;
    dbg_tris.resize(static_cast<int64_t>(triangles.size() * 3));
    if (!triangles.empty()) {
        static_assert(sizeof(IPLTriangle) == 3 * sizeof(int32_t), "IPLTriangle must be three int32 indices for bulk copy");
        std::memcpy(dbg_tris.ptrw(), triangles.data(), triangles.size() * sizeof(IPLTriangle));
    }
    asset.set_debug_vertices(dbg_verts);
    asset.set_debug_triangles(dbg_tris);
}

Ref<ResonanceGeometryAsset> ResonanceSceneManager::_build_static_scene_asset(StaticMeshData& data, IPLContext live_context) {
    Ref<ResonanceGeometryAsset> asset = build_geometry_asset(data.vertices, data.triangles, data.mat_indices, data.materials, live_context);
    if (asset.is_valid())
        set_asset_debug_geometry(**asset, data.vertices, data.triangles);
    return asset;
}

Ref<ResonanceGeometryAsset> ResonanceSceneManager::export_static_scene_to_geometry_asset(Node* scene_root, IPLContext live_context) {
    if (!scene_root)
        return Ref<ResonanceGeometryAsset>();
    StaticMeshData data = collect_static_mesh_data(scene_root);
    if (data.triangles.empty()) {
        UtilityFunctions::push_warning("Nexus Resonance: No valid mesh data in static geometry.");
        return Ref<ResonanceGeometryAsset>();
    }
    return _build_static_scene_asset(data, live_context);
}

StaticSceneExport ResonanceSceneManager::build_static_scene_export(Node* scene_root, IPLContext live_context,
                                                                   const StaticSceneHashFn& hash_fn, int64_t skip_if_hash) {
    StaticSceneExport result;
    if (!scene_root || !hash_fn)
        return result;
    StaticMeshData data = collect_static_mesh_data(scene_root);
    result.hash = hash_static_mesh_data(data, hash_fn);
    if (result.hash == 0 || result.hash == skip_if_hash)
        return result;
    result.asset = _build_static_scene_asset(data, live_context);
    return result;
}

String ResonanceSceneManager::resource_save_path(const String& p_path) {
    String path = p_path;
    if (!path.ends_with(".tres") && !path.ends_with(".res"))
        path += ".tres";
    return globalize_scene_file_path(path);
}

Error ResonanceSceneManager::export_static_scene_to_asset(Node* scene_root, const String& p_path, IPLContext live_context) {
    if (!scene_root)
        return ERR_INVALID_PARAMETER;
    const Ref<ResonanceGeometryAsset> asset = export_static_scene_to_geometry_asset(scene_root, live_context);
    if (asset.is_null())
        return ERR_INVALID_PARAMETER;
    return save_geometry_asset_atomic(asset, p_path);
}

Error ResonanceSceneManager::save_geometry_asset_atomic(const Ref<ResonanceGeometryAsset>& asset, const String& p_path) {
    if (asset.is_null())
        return ERR_INVALID_PARAMETER;
    String path = resource_save_path(p_path);
    // Temp extension must match final (.tres vs .res) so ResourceSaver writes the correct format before rename.
    const String tmp_ext = path.ends_with(".res") ? String(".res") : String(".tres");
    int64_t ext_pos = path.rfind(".");
    String tmp_path = (ext_pos >= 0) ? (path.substr(0, ext_pos) + "_tmp" + tmp_ext) : (path + "_tmp" + tmp_ext);
    ResourceSaver* saver = ResourceSaver::get_singleton();
    if (!saver) {
        ResonanceLog::error("ResonanceSceneManager: ResourceSaver singleton is null (save_geometry_asset_atomic).");
        return ERR_UNAVAILABLE;
    }
    Error save_err = saver->save(asset, tmp_path, ResourceSaver::FLAG_CHANGE_PATH);
    if (save_err != OK) {
        DirAccess::remove_absolute(tmp_path);
        return save_err;
    }
    Error rename_err = DirAccess::rename_absolute(tmp_path, path);
    if (rename_err != OK) {
        DirAccess::remove_absolute(tmp_path);
        return rename_err;
    }
    if (Engine::get_singleton() && Engine::get_singleton()->is_editor_hint()) {
        UtilityFunctions::print_rich("[color=cyan]Nexus Resonance:[/color] Static scene exported to " + path + " (" + String::num(asset->get_triangle_count()) + " triangles).");
    }
    return OK;
}

Error ResonanceSceneManager::save_phonon_scene_obj_atomic(IPLScene phonon_scene, const String& absolute_obj_path) {
    if (!phonon_scene)
        return ERR_INVALID_PARAMETER;
    String path = absolute_obj_path;
    if (!path.ends_with(".obj"))
        path += ".obj";

    String parent = path.get_base_dir();
    String staging_dir = parent.path_join("_nexus_obj_staging");
    String file = path.get_file();
    String staging_obj = staging_dir.path_join(file);
    const String base = file.get_basename();
    const String mtl_file = base + String(".mtl");
    String staging_mtl = staging_dir.path_join(mtl_file);
    const String final_mtl = parent.path_join(mtl_file);

    Error mkerr = DirAccess::make_dir_recursive_absolute(staging_dir);
    if (mkerr != OK) {
        UtilityFunctions::push_warning("Nexus Resonance: Could not create OBJ staging directory: " + staging_dir);
        return mkerr;
    }

    CharString staging_utf8 = staging_obj.utf8();
    iplSceneSaveOBJ(phonon_scene, staging_utf8.get_data());

    if (!FileAccess::file_exists(staging_obj)) {
        UtilityFunctions::push_warning("Nexus Resonance: Staged OBJ was not written: " + staging_obj);
        clear_nexus_obj_staging_best_effort(staging_dir, staging_obj, staging_mtl);
        return ERR_CANT_CREATE;
    }

    if (!FileAccess::file_exists(staging_mtl)) {
        UtilityFunctions::push_warning("Nexus Resonance: Staged MTL missing after export: " + staging_mtl);
        clear_nexus_obj_staging_best_effort(staging_dir, staging_obj, staging_mtl);
        return ERR_FILE_CANT_WRITE;
    }

    if (FileAccess::file_exists(final_mtl)) {
        Error rm_mtl = DirAccess::remove_absolute(final_mtl);
        if (rm_mtl != OK && FileAccess::file_exists(final_mtl))
            return rm_mtl;
    }
    Error r_mtl = DirAccess::rename_absolute(staging_mtl, final_mtl);
    if (r_mtl != OK) {
        clear_nexus_obj_staging_best_effort(staging_dir, staging_obj, staging_mtl);
        return r_mtl;
    }

    if (FileAccess::file_exists(path)) {
        Error rm_obj = DirAccess::remove_absolute(path);
        if (rm_obj != OK && FileAccess::file_exists(path)) {
            clear_nexus_obj_staging_best_effort(staging_dir, staging_obj, staging_mtl);
            return rm_obj;
        }
    }
    Error r_obj = DirAccess::rename_absolute(staging_obj, path);
    if (r_obj != OK) {
        UtilityFunctions::push_warning("Nexus Resonance: Failed to finalize OBJ (rename from staging): " + path);
        clear_nexus_obj_staging_best_effort(staging_dir, staging_obj, staging_mtl);
        return r_obj;
    }

    if (DirAccess::dir_exists_absolute(staging_dir))
        DirAccess::remove_absolute(staging_dir);
    return OK;
}

Error ResonanceSceneManager::export_static_scene_to_obj(Node* scene_root, const String& file_base_name, IPLContext live_context) {
    if (!scene_root)
        return ERR_INVALID_PARAMETER;

    StaticMeshData data = collect_static_mesh_data(scene_root);
    if (data.triangles.empty()) {
        UtilityFunctions::push_warning("Nexus Resonance: No valid mesh data in static geometry.");
        return ERR_INVALID_PARAMETER;
    }

    // Flip winding for OBJ export so Godot importer shows correct normals
    for (auto& tri : data.triangles) {
        std::swap(tri.indices[1], tri.indices[2]);
    }

    IPLContext export_context = nullptr;
    IPLScene temp_scene = nullptr;
    IPLStaticMesh temp_mesh = nullptr;
    if (!_build_temp_scene_for_export(data.vertices, data.triangles, data.mat_indices, data.materials, live_context, &export_context,
                                      &temp_scene, &temp_mesh)) {
        return ERR_CANT_CREATE;
    }

    String path = file_base_name;
    if (!path.ends_with(".obj")) {
        path = path + ".obj";
    }
    path = globalize_scene_file_path(path);

    Error write_err = save_phonon_scene_obj_atomic(temp_scene, path);

    iplStaticMeshRelease(&temp_mesh);
    iplSceneRelease(&temp_scene);
    iplContextRelease(&export_context);

    if (write_err != OK)
        return write_err;

    if (Engine::get_singleton() && Engine::get_singleton()->is_editor_hint()) {
        UtilityFunctions::print_rich("[color=cyan]Nexus Resonance:[/color] Scene exported to OBJ: " + path + " (" + String::num(static_cast<int64_t>(data.triangles.size())) + " triangles).");
    }
    return OK;
}

int64_t ResonanceSceneManager::get_static_scene_hash(Node* scene_root, const StaticSceneHashFn& hash_fn) {
    if (!scene_root || !hash_fn)
        return 0;
    return hash_static_mesh_data(collect_static_mesh_data(scene_root), hash_fn);
}

int64_t ResonanceSceneManager::hash_static_mesh_data(const StaticMeshData& data, const StaticSceneHashFn& hash_fn) {
    if (data.triangles.empty())
        return 0;
    const size_t vert_bytes = data.vertices.size() * sizeof(IPLVector3);
    const size_t tri_bytes = data.triangles.size() * sizeof(IPLTriangle);
    const size_t mat_idx_bytes = data.mat_indices.size() * sizeof(IPLint32);
    const size_t mat_tbl_bytes = ipl_materials_hash_byte_count(data.materials.size());
    PackedByteArray pba;
    pba.resize(static_cast<int64_t>(vert_bytes + tri_bytes + mat_idx_bytes + mat_tbl_bytes));
    uint8_t* w = pba.ptrw();
    memcpy(w, data.vertices.data(), vert_bytes);
    memcpy(w + vert_bytes, data.triangles.data(), tri_bytes);
    memcpy(w + vert_bytes + tri_bytes, data.mat_indices.data(), mat_idx_bytes);
    append_ipl_materials_hash_bytes(data.materials, w + vert_bytes + tri_bytes + mat_idx_bytes);
    return static_cast<int64_t>(hash_fn(pba));
}

} // namespace godot
