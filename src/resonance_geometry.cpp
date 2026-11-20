#include "resonance_geometry.h"
#include "resonance_constants.h"
#include "resonance_debug_log.h"
#include "resonance_export_transform.h"
#include "resonance_geometry_asset.h"
#include "resonance_geometry_transform_coalesce_policy.h"
#include "resonance_log.h"
#include "resonance_material_update_policy.h"
#include "resonance_mesh_ipl_godot.h"
#include "resonance_scene_manager.h"
#include "resonance_server.h"
#include "resonance_static_scene.h"
#include "resonance_utils.h"
#include <cstdint>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <mutex>
#include <phonon.h>
#include <vector>

using namespace godot;

static bool steam_audio_verbose_logs() {
    ProjectSettings* ps = ProjectSettings::get_singleton();
    if (!ps)
        return false;
    const String key = String(resonance::kProjectSettingsResonancePrefix) + "logger/steam_audio_verbose";
    return ps->has_setting(key) && ps->get_setting(key).booleanize();
}

static const char* ipl_scene_type_label(IPLSceneType t) {
    switch (t) {
    case IPL_SCENETYPE_EMBREE:
        return "Embree";
    case IPL_SCENETYPE_RADEONRAYS:
        return "RadeonRays";
    case IPL_SCENETYPE_CUSTOM:
        return "Custom";
    default:
        return "Default";
    }
}

static void log_dynamic_instanced_mesh_registered(ResonanceServer* server, int tri_count) {
    if (!steam_audio_verbose_logs() || !server)
        return;
    ResonanceLog::info(String("ResonanceGeometry: dynamic InstancedMesh registered (scene_type=") + ipl_scene_type_label(server->get_scene_type()) +
                       ", triangles=" + String::num_int64(tri_count) + ").");
}

/// Under an imported glTF the parent is a plain Node3D: the transform for geometry_override then comes from the
/// MeshInstance3D that draws the same mesh.
static MeshInstance3D* find_mesh_instance_using_mesh(Node* node, const Ref<Mesh>& mesh, int depth_left) {
    if (!node || mesh.is_null() || depth_left < 0)
        return nullptr;
    MeshInstance3D* mi = Object::cast_to<MeshInstance3D>(node);
    if (mi && mi->get_mesh() == mesh)
        return mi;
    for (int i = 0; i < node->get_child_count(); i++) {
        MeshInstance3D* found = find_mesh_instance_using_mesh(node->get_child(i), mesh, depth_left - 1);
        if (found)
            return found;
    }
    return nullptr;
}

ResonanceGeometry::ResonanceGeometry() {}

void ResonanceGeometry::_invalidate_topology_caches(bool include_static_scene_query) {
    bake_transform_override_cache_valid_ = false;
    bake_override_mesh_instance_id_ = ObjectID();
    if (include_static_scene_query) {
        static_scene_asset_query_valid_ = false;
        cached_scene_tree_root_id_ = ObjectID();
        cached_root_has_static_scene_asset_ = false;
    }
    reflection_debug_parsed_mesh_.unref();
    reflection_debug_parsed_vertices_.clear();
    reflection_debug_parsed_triangles_.clear();
    reflection_debug_parsed_mat_indices_.clear();
}

ResonanceGeometry::~ResonanceGeometry() {
    _clear_meshes();
}

void ResonanceGeometry::_ready() {
    add_to_group("resonance_geometry");
    _propagate_material_and_geometry_to_descendants();
    _create_meshes();
    _update_viz_geometry_override();
}

void ResonanceGeometry::_exit_tree() {
    server_init_retry_pending_ = false;
    server_init_retry_count_ = 0;
    // Godot frees the child with us; do not queue_free it.
    viz_geometry_override = nullptr;
    // A live despawn must not call begin_tree_teardown; that is for quit only.
    _clear_meshes();
}

void ResonanceGeometry::_notification(int p_what) {
    if (p_what == NOTIFICATION_TRANSFORM_CHANGED) {
        if (dynamic_object && is_inside_tree()) {
            _update_dynamic_transform();
        }
    } else if (p_what == NOTIFICATION_ENTER_TREE || p_what == NOTIFICATION_PARENTED || p_what == NOTIFICATION_UNPARENTED) {
        _invalidate_topology_caches();
    }
}

void ResonanceGeometry::_schedule_retry_create_meshes_when_server_ready() {
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint())
        return;
    if (!is_inside_tree())
        return;
    if (server_init_retry_pending_)
        return;
    server_init_retry_pending_ = true;
    call_deferred("_deferred_retry_create_meshes");
}

void ResonanceGeometry::_deferred_retry_create_meshes() {
    server_init_retry_pending_ = false;
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint())
        return;
    if (!is_inside_tree())
        return;
    ResonanceServer* server = ResonanceServer::get_singleton();
    if (!server || !server->is_initialized()) {
        if (server_init_retry_count_ < kMaxServerInitRetries) {
            server_init_retry_count_++;
            server_init_retry_pending_ = true;
            call_deferred("_deferred_retry_create_meshes");
        }
        return;
    }
    server_init_retry_count_ = 0;
    _create_meshes();
    _update_viz_geometry_override();
}

void ResonanceGeometry::set_dynamic(bool p_dynamic) {
    if (dynamic_object != p_dynamic) {
        _invalidate_topology_caches();
        dynamic_object = p_dynamic;
        if (is_inside_tree())
            _create_meshes();
        set_notify_transform(dynamic_object);
    }
}

bool ResonanceGeometry::is_dynamic() const { return dynamic_object; }

void ResonanceGeometry::set_mesh_asset(const Ref<ResonanceGeometryAsset>& p_asset) {
    if (mesh_asset != p_asset) {
        _invalidate_topology_caches(false);
        mesh_asset = p_asset;
        if (is_inside_tree() && dynamic_object)
            _create_meshes();
    }
}

Ref<ResonanceGeometryAsset> ResonanceGeometry::get_mesh_asset() const {
    return mesh_asset;
}

void ResonanceGeometry::set_geometry_override(const Ref<Mesh>& p_mesh) {
    if (geometry_override != p_mesh) {
        _invalidate_topology_caches(false);
        geometry_override = p_mesh;
        if (is_inside_tree())
            _create_meshes();
        _update_viz_geometry_override();
    }
}

Ref<Mesh> ResonanceGeometry::get_geometry_override() const {
    return geometry_override;
}

void ResonanceGeometry::set_show_geometry_override_in_viewport(bool p_show) {
    if (show_geometry_override_in_viewport != p_show) {
        show_geometry_override_in_viewport = p_show;
        _update_viz_geometry_override();
    }
}

bool ResonanceGeometry::is_show_geometry_override_in_viewport() const {
    return show_geometry_override_in_viewport;
}

void ResonanceGeometry::set_export_all_children(bool p_export) {
    if (export_all_children == p_export)
        return;
    export_all_children = p_export;
    if (is_inside_tree() && !dynamic_object)
        _create_meshes();
}

bool ResonanceGeometry::get_export_all_children() const {
    return export_all_children;
}

Transform3D ResonanceGeometry::get_mesh_bake_transform() const {
    if (geometry_override.is_valid()) {
        MeshInstance3D* parent_mi = Object::cast_to<MeshInstance3D>(get_parent());
        if (parent_mi && parent_mi->get_mesh() == geometry_override)
            return ResonanceUtils::node3d_export_transform(parent_mi);
        if (bake_transform_override_cache_valid_ && bake_override_mesh_instance_id_.is_valid()) {
            Object* o = ObjectDB::get_instance(static_cast<uint64_t>(bake_override_mesh_instance_id_));
            MeshInstance3D* mi = Object::cast_to<MeshInstance3D>(o);
            if (mi && mi->get_mesh() == geometry_override)
                return ResonanceUtils::node3d_export_transform(mi);
        }
        Node* par = get_parent();
        if (par) {
            MeshInstance3D* found = find_mesh_instance_using_mesh(par, geometry_override, 64);
            bake_transform_override_cache_valid_ = true;
            if (found)
                bake_override_mesh_instance_id_ = ObjectID(found->get_instance_id());
            else
                bake_override_mesh_instance_id_ = ObjectID();
            if (found)
                return ResonanceUtils::node3d_export_transform(found);
        }
        return ResonanceUtils::node3d_export_transform(this);
    }
    MeshInstance3D* mi = Object::cast_to<MeshInstance3D>(get_parent());
    if (mi)
        return ResonanceUtils::node3d_export_transform(mi);
    return ResonanceUtils::node3d_export_transform(this);
}

Ref<Mesh> ResonanceGeometry::get_source_mesh() const {
    if (geometry_override.is_valid())
        return geometry_override;
    MeshInstance3D* mesh_instance = Object::cast_to<MeshInstance3D>(get_parent());
    return mesh_instance ? mesh_instance->get_mesh() : Ref<Mesh>();
}

static bool has_resonance_geometry_child(Node* node) {
    for (int i = 0; i < node->get_child_count(); i++) {
        if (Object::cast_to<ResonanceGeometry>(node->get_child(i)))
            return true;
    }
    return false;
}

// Visible MeshInstance3D descendants that no other ResonanceGeometry claims: the walk stops at a nested
// ResonanceGeometry and skips a MeshInstance3D whose own child geometry already uses its mesh.
static void collect_unclaimed_mesh_instances(Node* from, std::vector<MeshInstance3D*>& out) {
    for (int i = 0; i < from->get_child_count(); i++) {
        Node* child = from->get_child(i);
        if (!child || Object::cast_to<ResonanceGeometry>(child))
            continue;
        MeshInstance3D* mi = Object::cast_to<MeshInstance3D>(child);
        if (mi && mi->get_mesh().is_valid() && ResonanceUtils::node3d_visible_for_export(mi) && !has_resonance_geometry_child(mi))
            out.push_back(mi);
        collect_unclaimed_mesh_instances(child, out);
    }
}

bool ResonanceGeometry::includes_child_meshes() const {
    return export_all_children && !dynamic_object;
}

bool ResonanceGeometry::append_meshes_to_ipl(std::vector<IPLVector3>& out_vertices, std::vector<IPLTriangle>& out_triangles,
                                             std::vector<IPLint32>& out_mat_indices, IPLint32 mat_index) {
    const size_t triangles_before = out_triangles.size();
    const Transform3D xform = dynamic_object ? Transform3D() : get_mesh_bake_transform();
    append_godot_mesh_to_ipl(get_source_mesh(), xform, out_vertices, out_triangles, &out_mat_indices, mat_index);
    if (includes_child_meshes()) {
        std::vector<MeshInstance3D*> child_meshes;
        collect_unclaimed_mesh_instances(this, child_meshes);
        for (MeshInstance3D* mi : child_meshes) {
            append_godot_mesh_to_ipl(mi->get_mesh(), ResonanceUtils::node3d_export_transform(mi), out_vertices, out_triangles,
                                     &out_mat_indices, mat_index);
        }
    }
    return out_triangles.size() > triangles_before;
}

bool ResonanceGeometry::_has_mesh_source() const {
    return get_source_mesh().is_valid() || includes_child_meshes();
}

void ResonanceGeometry::_update_viz_geometry_override() {
    if (!Engine::get_singleton() || !Engine::get_singleton()->is_editor_hint())
        return;

    if (!show_geometry_override_in_viewport || !geometry_override.is_valid()) {
        if (viz_geometry_override) {
            viz_geometry_override->queue_free();
            viz_geometry_override = nullptr;
        }
        return;
    }

    if (!viz_geometry_override) {
        viz_geometry_override = memnew(MeshInstance3D);
        viz_geometry_override->set_name("VizGeometryOverride");
        viz_geometry_override->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
        Ref<StandardMaterial3D> mat;
        mat.instantiate();
        mat->set_albedo(Color(resonance::kGeometryOverrideVizR, resonance::kGeometryOverrideVizG, resonance::kGeometryOverrideVizB, resonance::kGeometryOverrideVizA));
        mat->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
        mat->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
        viz_geometry_override->set_material_override(mat);
        add_child(viz_geometry_override);
    }
    viz_geometry_override->set_mesh(geometry_override);
    viz_geometry_override->set_visible(show_geometry_override_in_viewport);
}

void ResonanceGeometry::_clear_meshes_impl(bool notify_server, bool for_shutdown) {
    // Keep the static scene lookup: _create_meshes often clears and rebuilds in the same frame.
    _invalidate_topology_caches(false);
    ResonanceServer* server = ResonanceServer::get_singleton();

    if (server && debug_mesh_id >= 0) {
        server->unregister_debug_mesh(debug_mesh_id);
        debug_mesh_id = -1;
    }

    if (server && server->is_initialized()) {
        if (for_shutdown)
            _detach_meshes_now(server);
        else
            _queue_mesh_detach(server);
        // On shutdown the worker is already joined; do not dirty the scene or wake it.
        if (notify_server && triangle_count > 0 && !for_shutdown) {
            server->notify_geometry_changed(-triangle_count);
        }
    } else {
        // Without a live server, release the handles without touching the scene.
        if (dynamic_object && instanced_mesh) {
            iplInstancedMeshRelease(&instanced_mesh);
            instanced_mesh = nullptr;
        }
        if (dynamic_object && sub_scene) {
            static_meshes.clear();
        } else {
            for (auto& mesh : static_meshes)
                iplStaticMeshRelease(&mesh);
            static_meshes.clear();
        }
        if (instanced_mesh)
            iplInstancedMeshRelease(&instanced_mesh);
    }

    if (sub_scene) {
        if (dynamic_object)
            iplSceneCommit(sub_scene);
        iplSceneRelease(&sub_scene);
        sub_scene = nullptr;
    }

    static_meshes.clear();
    instanced_mesh = nullptr;
    triangle_count = 0;
}

// Hands every handle to the server queue; the worker detaches them from the global scene between passes.
void ResonanceGeometry::_queue_mesh_detach(ResonanceServer* server) {
    if (dynamic_object) {
        if (instanced_mesh) {
            server->cancel_pending_dynamic_instanced_mesh_transform(instanced_mesh);
            server->queue_scene_instanced_mesh_remove(instanced_mesh, sub_scene);
            instanced_mesh = nullptr;
            sub_scene = nullptr;
        }
        // Per-mesh Remove/Release on a sub_scene can crash Phonon 4.8.x; releasing the sub_scene frees the meshes.
        if (sub_scene) {
            static_meshes.clear();
            return;
        }
        for (auto& mesh : static_meshes)
            iplStaticMeshRelease(&mesh);
        static_meshes.clear();
        return;
    }
    for (IPLStaticMesh mesh : static_meshes)
        server->queue_scene_static_mesh_remove(mesh);
    static_meshes.clear();
}

// Caller holds the simulation lock; used while the scene is torn down and the worker is gone.
void ResonanceGeometry::_detach_meshes_now(ResonanceServer* server) {
    // Detach the instance from the global scene before touching its sub_scene, or Phonon crashes.
    if (dynamic_object && instanced_mesh) {
        server->cancel_pending_dynamic_instanced_mesh_transform(instanced_mesh);
        iplInstancedMeshRemove(instanced_mesh, server->get_scene_handle());
        iplInstancedMeshRelease(&instanced_mesh);
        instanced_mesh = nullptr;
    }

    IPLScene scene_for_static = dynamic_object ? sub_scene : server->get_scene_handle();
    // Per-mesh Remove/Release on a sub_scene can crash Phonon 4.8.x; releasing the sub_scene frees the meshes.
    if (dynamic_object && sub_scene) {
        static_meshes.clear();
        return;
    }
    for (auto& mesh : static_meshes) {
        if (scene_for_static)
            iplStaticMeshRemove(mesh, scene_for_static);
        iplStaticMeshRelease(&mesh);
    }
    static_meshes.clear();
}

void ResonanceGeometry::_clear_meshes() {
    ResonanceServer* server = ResonanceServer::get_singleton();
    if (server && server->is_initialized()) {
        if (!ResonanceServer::is_shutting_down()) {
            _clear_meshes_impl(true, false);
            return;
        }
        auto lock = server->scoped_simulation_lock();
        server->apply_queued_scene_mesh_changes_assume_locked();
        _clear_meshes_impl(false, true);
    } else {
        _clear_meshes_impl();
    }
}

bool ResonanceGeometry::_is_covered_by_static_scene() const {
    if (dynamic_object)
        return false;
    // The editor never registers static packs, so live meshes stay there.
    if (Engine::get_singleton() && Engine::get_singleton()->is_editor_hint())
        return false;
    Node* root = get_parent();
    if (!root)
        return false;
    while (root->get_parent())
        root = root->get_parent();
    const ObjectID root_oid(root->get_instance_id());
    if (!static_scene_asset_query_valid_ || cached_scene_tree_root_id_ != root_oid) {
        cached_scene_tree_root_id_ = root_oid;
        cached_root_has_static_scene_asset_ = subtree_has_static_scene(root, StaticSceneQuery::WithValidAsset);
        static_scene_asset_query_valid_ = true;
    }
    return cached_root_has_static_scene_asset_;
}

void ResonanceGeometry::sync_static_scene_coverage() {
    // A pending server-init retry builds the meshes itself; building here too would rebuild them twice.
    if (dynamic_object || server_init_retry_pending_)
        return;
    _invalidate_topology_caches();
    if (_is_covered_by_static_scene()) {
        if (!static_meshes.empty())
            _clear_meshes();
        return;
    }
    refresh_geometry();
}

void ResonanceGeometry::_create_meshes() {
    Node* parent = get_parent();
    Node3D* node3d = Object::cast_to<Node3D>(parent);
    if (!node3d || !node3d->is_visible_in_tree())
        return;

    if (_is_covered_by_static_scene()) {
        if (!static_meshes.empty())
            _clear_meshes();
        // The serialized static scene asset already carries mesh and material; live changes do not apply.
        if (material.is_valid()) {
            static bool warned_static_skips_live_material = false;
            if (!warned_static_skips_live_material) {
                warned_static_skips_live_material = true;
                UtilityFunctions::push_warning(
                    "Nexus Resonance: ResonanceGeometry material is ignored at runtime while a "
                    "ResonanceStaticScene asset is loaded. Re-export the static scene so transmission/"
                    "absorption match the Inspector (F3 Hit0 T shows the first closestHit material).");
            }
        }
        return;
    }

    const int previous_triangle_count = triangle_count;
    const bool use_asset_path = dynamic_object && mesh_asset.is_valid() && mesh_asset->is_valid();
    if (!use_asset_path && !_has_mesh_source())
        return;

    ResonanceServer* server = ResonanceServer::get_singleton();
    if (!server || !server->is_initialized()) {
        _schedule_retry_create_meshes_when_server_ready();
        return;
    }
    server_init_retry_count_ = 0;

    if (server->get_scene_type() == IPL_SCENETYPE_CUSTOM) {
        static bool warned_custom_geometry = false;
        if (!warned_custom_geometry) {
            warned_custom_geometry = true;
            UtilityFunctions::push_warning(
                "Nexus Resonance: ResonanceGeometry is ignored when scene_type is Custom; use Godot 3D colliders. "
                "Optional collider meta: resonance_physics_material (ResonanceMaterial resource) or "
                "resonance_physics_material_preset (basename of a .tres under nexus/resonance/physics/material_search_paths). "
                "Spatial output gate may open with triangle_count==0; physics world bind can lag ResonanceRuntime by a frame (see ARCHITECTURE).");
        }
        return;
    }

    const bool created = use_asset_path ? _create_meshes_from_asset(server) : _create_meshes_from_mesh(server);
    if (!created)
        return;

    const int net_triangles = triangle_count - previous_triangle_count;
    if (net_triangles != 0) {
        server->notify_geometry_changed(net_triangles);
    } else {
        // Same triangle count, but the replaced mesh still needs a commit.
        server->mark_scene_commit_pending();
    }
}

static IPLSceneSettings mesh_sub_scene_settings(ResonanceServer* server) {
    IPLSceneSettings settings{};
    settings.type = server->get_phonon_mesh_scene_type();
    settings.embreeDevice = server->get_embree_device_handle();
    settings.radeonRaysDevice = server->get_radeon_rays_device_handle();
    return settings;
}

// Builds the instance before releasing the previous mesh, so a failed rebuild keeps the old one and the
// global triangle count stays correct. Takes ownership of new_sub and new_mesh.
bool ResonanceGeometry::_install_dynamic_instance(ResonanceServer* server, IPLScene new_sub, IPLStaticMesh new_mesh,
                                                  int new_triangle_count) {
    IPLInstancedMeshSettings inst_settings{};
    inst_settings.subScene = new_sub;
    inst_settings.transform = ResonanceUtils::to_ipl_matrix(get_mesh_bake_transform());
    IPLInstancedMesh new_inst = nullptr;
    if (iplInstancedMeshCreate(server->get_scene_handle(), &inst_settings, &new_inst) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceGeometry: iplInstancedMeshCreate failed (dynamic).");
        iplStaticMeshRemove(new_mesh, new_sub);
        iplStaticMeshRelease(&new_mesh);
        iplSceneRelease(&new_sub);
        return false;
    }

    _clear_meshes_impl(false);
    sub_scene = new_sub;
    static_meshes.push_back(new_mesh);
    instanced_mesh = new_inst;
    server->queue_scene_instanced_mesh_add(iplInstancedMeshRetain(instanced_mesh));
    triangle_count = new_triangle_count;
    log_dynamic_instanced_mesh_registered(server, triangle_count);
    return true;
}

bool ResonanceGeometry::_create_meshes_from_asset(ResonanceServer* server) {
    const int64_t blob_sz = mesh_asset->get_size();
    const uint8_t* blob = mesh_asset->get_data_ptr();
    if (!blob || blob_sz <= 0) {
        ResonanceLog::error("ResonanceGeometry: mesh_asset has no serialized data (asset path).");
        return false;
    }

    IPLSceneSettings scene_settings = mesh_sub_scene_settings(server);
    IPLScene new_sub = nullptr;
    if (iplSceneCreate(server->get_context_handle(), &scene_settings, &new_sub) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceGeometry: iplSceneCreate failed (asset path).");
        return false;
    }

    IPLSerializedObjectSettings serial_settings{};
    serial_settings.data = const_cast<IPLbyte*>(reinterpret_cast<const IPLbyte*>(blob));
    serial_settings.size = static_cast<IPLsize>(blob_sz);
    IPLSerializedObject serial_obj = nullptr;
    if (iplSerializedObjectCreate(server->get_context_handle(), &serial_settings, &serial_obj) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceGeometry: iplSerializedObjectCreate failed.");
        iplSceneRelease(&new_sub);
        return false;
    }

    IPLStaticMesh new_mesh = nullptr;
    const IPLerror load_err = iplStaticMeshLoad(new_sub, serial_obj, nullptr, nullptr, &new_mesh);
    iplSerializedObjectRelease(&serial_obj);
    if (load_err != IPL_STATUS_SUCCESS || !new_mesh) {
        ResonanceLog::error("ResonanceGeometry: iplStaticMeshLoad failed.");
        iplSceneRelease(&new_sub);
        return false;
    }

    iplStaticMeshAdd(new_mesh, new_sub);
    if (material.is_valid()) {
        IPLMaterial mat = material->get_ipl_material();
        iplStaticMeshSetMaterial(new_mesh, new_sub, &mat, 0);
    }
    iplSceneCommit(new_sub);

    return _install_dynamic_instance(server, new_sub, new_mesh, mesh_asset->get_triangle_count());
}

bool ResonanceGeometry::_create_meshes_from_mesh(ResonanceServer* server) {
    // Parse outside the simulation lock to keep the lock short.
    std::vector<IPLVector3> ipl_vertices;
    std::vector<IPLTriangle> ipl_triangles;
    std::vector<IPLint32> ipl_mat_indices;
    if (!append_meshes_to_ipl(ipl_vertices, ipl_triangles, ipl_mat_indices, 0))
        return false;

    IPLMaterial mat_settings = ResonanceMaterial::ipl_material_or_default(material);

    IPLStaticMeshSettings mesh_settings{};
    mesh_settings.numVertices = static_cast<IPLint32>(ipl_vertices.size());
    mesh_settings.numTriangles = static_cast<IPLint32>(ipl_triangles.size());
    mesh_settings.numMaterials = 1;
    mesh_settings.vertices = ipl_vertices.data();
    mesh_settings.triangles = ipl_triangles.data();
    mesh_settings.materials = &mat_settings;
    mesh_settings.materialIndices = ipl_mat_indices.data();

    if (dynamic_object) {
        IPLSceneSettings scene_settings = mesh_sub_scene_settings(server);
        IPLScene new_sub = nullptr;
        if (iplSceneCreate(server->get_context_handle(), &scene_settings, &new_sub) != IPL_STATUS_SUCCESS) {
            ResonanceLog::error("ResonanceGeometry: iplSceneCreate failed (dynamic).");
            return false;
        }
        IPLStaticMesh new_mesh = nullptr;
        if (iplStaticMeshCreate(new_sub, &mesh_settings, &new_mesh) != IPL_STATUS_SUCCESS) {
            ResonanceLog::error("ResonanceGeometry: iplStaticMeshCreate failed (dynamic).");
            iplSceneRelease(&new_sub);
            return false;
        }
        iplStaticMeshAdd(new_mesh, new_sub);
        iplSceneCommit(new_sub);
        if (!_install_dynamic_instance(server, new_sub, new_mesh, static_cast<int>(ipl_triangles.size())))
            return false;
    } else {
        IPLStaticMesh new_mesh = nullptr;
        if (iplStaticMeshCreate(server->get_scene_handle(), &mesh_settings, &new_mesh) != IPL_STATUS_SUCCESS) {
            ResonanceLog::error("ResonanceGeometry: iplStaticMeshCreate failed (static).");
            return false;
        }
        _clear_meshes_impl(false);
        server->queue_scene_static_mesh_add(iplStaticMeshRetain(new_mesh));
        static_meshes.push_back(new_mesh);
        triangle_count = static_cast<int>(ipl_triangles.size());
    }

    if (triangle_count > 0 && server->wants_debug_reflection_viz()) {
        _store_reflection_debug_parse(std::move(ipl_vertices), std::move(ipl_triangles), std::move(ipl_mat_indices));
        _register_reflection_debug_mesh(server, mat_settings);
    }
    return true;
}

void ResonanceGeometry::_store_reflection_debug_parse(std::vector<IPLVector3>&& vertices, std::vector<IPLTriangle>&& triangles,
                                                      std::vector<IPLint32>&& mat_indices) {
    reflection_debug_parsed_mesh_ = get_source_mesh();
    reflection_debug_parsed_xform_ = dynamic_object ? Transform3D() : get_mesh_bake_transform();
    reflection_debug_parsed_vertices_ = std::move(vertices);
    reflection_debug_parsed_triangles_ = std::move(triangles);
    reflection_debug_parsed_mat_indices_ = std::move(mat_indices);
}

// Child meshes are not part of the cache key, so a parse that includes them is never reused.
bool ResonanceGeometry::_reflection_debug_parse_is_current() const {
    if (includes_child_meshes() || reflection_debug_parsed_triangles_.empty())
        return false;
    const Transform3D xform = dynamic_object ? Transform3D() : get_mesh_bake_transform();
    return reflection_debug_parsed_mesh_ == get_source_mesh() && reflection_debug_parsed_xform_ == xform;
}

void ResonanceGeometry::_register_reflection_debug_mesh(ResonanceServer* server, const IPLMaterial& mat) {
    if (debug_mesh_id >= 0) {
        server->unregister_debug_mesh(debug_mesh_id);
        debug_mesh_id = -1;
    }
    // Dynamic buffers are in mesh space and follow the instance; static buffers are already in world space.
    const Transform3D tr = dynamic_object ? get_mesh_bake_transform() : Transform3D();
    const IPLMatrix4x4 ipl_xform = ResonanceUtils::to_ipl_matrix(tr);
    debug_mesh_id = server->register_debug_mesh(reflection_debug_parsed_vertices_, reflection_debug_parsed_triangles_,
                                                reflection_debug_parsed_mat_indices_.data(), &ipl_xform, &mat);
    resonance::debug_log_raw("resonance_geometry:register_debug", "mesh_registered",
                             static_cast<int>(reflection_debug_parsed_triangles_.size()), debug_mesh_id);
}

bool ResonanceGeometry::consume_transform_coalesce_tick() {
    constexpr int interval = resonance::kGeometryTransformCoalesceInterval;
    const uint32_t c = ++transform_coalesce_counter_;
    return resonance::geometry_transform_coalesce_tick_due(c, interval);
}

void ResonanceGeometry::_update_dynamic_transform() {
    if (!instanced_mesh)
        return;

    ResonanceServer* server = ResonanceServer::get_singleton();
    if (!server)
        return;

    IPLMatrix4x4 mat = ResonanceUtils::to_ipl_matrix(get_mesh_bake_transform());

    // Queued to the worker so the main thread never takes simulation_mutex. Throttled; the final pose comes
    // from flush_dynamic_acoustic_transform.
    if (!consume_transform_coalesce_tick())
        return;
    server->enqueue_dynamic_instanced_mesh_transform(instanced_mesh, mat);
}

void ResonanceGeometry::flush_dynamic_acoustic_transform() {
    if (!dynamic_object || !instanced_mesh)
        return;
    ResonanceServer* server = ResonanceServer::get_singleton();
    if (!server)
        return;
    // Goes through the worker queue too: taking simulation_mutex here stalls the main thread while the
    // worker runs reflections.
    IPLMatrix4x4 mat = ResonanceUtils::to_ipl_matrix(get_mesh_bake_transform());
    server->flush_dynamic_instanced_mesh_transform(instanced_mesh, mat);
}

static void _propagate_recursive(Node* from, const Ref<ResonanceMaterial>& mat, const Ref<Mesh>& geom_override) {
    if (!from)
        return;
    for (int i = 0; i < from->get_child_count(); i++) {
        Node* c = from->get_child(i);
        if (c->is_class("ResonanceGeometry")) {
            ResonanceGeometry* child_geom = Object::cast_to<ResonanceGeometry>(c);
            if (child_geom) {
                if (mat.is_valid() && child_geom->get_material().is_null())
                    child_geom->set_material(mat);
                if (geom_override.is_valid() && child_geom->get_geometry_override().is_null())
                    child_geom->set_geometry_override(geom_override);
            }
        }
        _propagate_recursive(c, mat, geom_override);
    }
}

void ResonanceGeometry::_propagate_material_and_geometry_to_descendants() {
    if (!material.is_valid() && !geometry_override.is_valid())
        return;
    _propagate_recursive(this, material, geometry_override);
}

void ResonanceGeometry::set_material(const Ref<ResonanceMaterial>& p_material) {
    if (material == p_material)
        return;
    material = p_material;

    const resonance::GeometryMaterialUpdatePath path =
        resonance::geometry_material_update_path(false, is_inside_tree(), !static_meshes.empty());
    if (path == resonance::GeometryMaterialUpdatePath::None)
        return;
    // Setting the material on live handles keeps Embree intact; a rebuild can lose dynamic occlusion.
    if (path == resonance::GeometryMaterialUpdatePath::InPlace)
        _update_material_inplace();
    else
        _create_meshes();
}

void ResonanceGeometry::_update_material_inplace() {
    ResonanceServer* server = ResonanceServer::get_singleton();
    if (!server || !server->is_initialized() || static_meshes.empty())
        return;
    if (dynamic_object && !sub_scene)
        return;

    // The worker traces a dynamic sub-scene through its instance in the global scene, so it is queued too.
    IPLMaterial mat = ResonanceMaterial::ipl_material_or_default(material);
    for (IPLStaticMesh mesh : static_meshes) {
        if (!mesh)
            continue;
        IPLScene retained_sub_scene = dynamic_object ? iplSceneRetain(sub_scene) : nullptr;
        server->queue_scene_static_mesh_material(iplStaticMeshRetain(mesh), retained_sub_scene, mat);
    }

    if (server->wants_debug_reflection_viz() && debug_mesh_id >= 0)
        sync_reflection_debug_viz();
}

Ref<ResonanceMaterial> ResonanceGeometry::get_material() const { return material; }

Error ResonanceGeometry::export_dynamic_mesh_to_asset(const String& p_path) {
    Node* parent = get_parent();
    Node3D* node3d = Object::cast_to<Node3D>(parent);
    if (!parent || !node3d || !node3d->is_visible_in_tree())
        return ERR_INVALID_PARAMETER;

    const Ref<Mesh> mesh = get_source_mesh();
    if (mesh.is_null())
        return ERR_INVALID_PARAMETER;

    std::vector<IPLVector3> ipl_vertices;
    std::vector<IPLTriangle> ipl_triangles;
    std::vector<IPLint32> ipl_mat_indices;

    Transform3D xform; // Dynamic meshes are stored in mesh space
    if (!append_godot_mesh_to_ipl(mesh, xform, ipl_vertices, ipl_triangles, &ipl_mat_indices))
        return ERR_INVALID_PARAMETER;

    std::vector<IPLMaterial> ipl_materials{ResonanceMaterial::ipl_material_or_default(material)};
    ResonanceServer* live_server = ResonanceServer::get_singleton();
    const Ref<ResonanceGeometryAsset> asset = ResonanceSceneManager::build_geometry_asset(
        ipl_vertices, ipl_triangles, ipl_mat_indices, ipl_materials, live_server ? live_server->get_context_handle() : nullptr);
    if (asset.is_null())
        return ERR_CANT_CREATE;

    const String path = ResonanceSceneManager::resource_save_path(p_path);
    Error save_err = ResourceSaver::get_singleton()->save(asset, path, ResourceSaver::FLAG_CHANGE_PATH);
    if (save_err == OK) {
        mesh_asset = asset;
        if (is_inside_tree() && dynamic_object)
            _create_meshes();
    }
    return save_err;
}

void ResonanceGeometry::sync_reflection_debug_viz() {
    ResonanceServer* server = ResonanceServer::get_singleton();
    if (!server || !server->is_initialized())
        return;

    // Never block the main thread on simulation_mutex: the worker can hold it for seconds during
    // iplSimulatorRunReflections on the default tracer. Retry next idle frame.
    const auto try_sim_lock_or_defer = [&]() -> std::unique_lock<std::mutex> {
        auto lock = server->try_scoped_simulation_lock();
        if (!lock.owns_lock())
            call_deferred("sync_reflection_debug_viz");
        return lock;
    };

    const auto clear_debug_registration = [&]() {
        if (debug_mesh_id < 0)
            return;
        auto lock = try_sim_lock_or_defer();
        if (!lock.owns_lock())
            return;
        server->unregister_debug_mesh(debug_mesh_id);
        debug_mesh_id = -1;
    };

    if (!server->wants_debug_reflection_viz()) {
        clear_debug_registration();
        return;
    }

    Node* parent = get_parent();
    Node3D* node3d = Object::cast_to<Node3D>(parent);
    if (!node3d || !node3d->is_visible_in_tree()) {
        clear_debug_registration();
        return;
    }

    if (_is_covered_by_static_scene()) {
        clear_debug_registration();
        return;
    }

    const bool use_asset_path = dynamic_object && mesh_asset.is_valid() && mesh_asset->is_valid();
    if (use_asset_path) {
        clear_debug_registration();
        return;
    }

    if (!_has_mesh_source()) {
        clear_debug_registration();
        return;
    }

    if (!_reflection_debug_parse_is_current()) {
        std::vector<IPLVector3> vertices;
        std::vector<IPLTriangle> triangles;
        std::vector<IPLint32> mat_indices;
        if (!append_meshes_to_ipl(vertices, triangles, mat_indices, 0)) {
            clear_debug_registration();
            return;
        }
        _store_reflection_debug_parse(std::move(vertices), std::move(triangles), std::move(mat_indices));
    }

    const IPLMaterial mat_settings = ResonanceMaterial::ipl_material_or_default(material);
    auto lock = try_sim_lock_or_defer();
    if (!lock.owns_lock())
        return;
    _register_reflection_debug_mesh(server, mat_settings);
}

void ResonanceGeometry::refresh_geometry() {
    // Skip when the meshes are live: a needless tear-down and rebuild breaks Embree occlusion.
    if ((dynamic_object && instanced_mesh) || (!dynamic_object && !static_meshes.empty()))
        return;
    _create_meshes();
}

void ResonanceGeometry::discard_meshes_before_scene_release() {
    ResonanceServer* server = ResonanceServer::get_singleton();
    if (server && debug_mesh_id >= 0) {
        server->unregister_debug_mesh(debug_mesh_id);
        debug_mesh_id = -1;
    }

    // Meshes must be removed and released explicitly before the scene goes; refcounting alone does not do it.
    if (server && server->is_initialized()) {
        auto lock = server->scoped_simulation_lock();
        server->apply_queued_scene_mesh_changes_assume_locked();
        if (dynamic_object && instanced_mesh) {
            server->cancel_pending_dynamic_instanced_mesh_transform(instanced_mesh);
            IPLScene global_scene_handle = server->get_scene_handle();
            iplInstancedMeshRemove(instanced_mesh, global_scene_handle);
            iplInstancedMeshRelease(&instanced_mesh);
            instanced_mesh = nullptr;
            if (global_scene_handle)
                iplSceneCommit(global_scene_handle);
        }
        IPLScene scene_for_static = dynamic_object ? sub_scene : server->get_scene_handle();
        if (dynamic_object && sub_scene) {
            static_meshes.clear();
        } else {
            for (auto& mesh : static_meshes) {
                if (scene_for_static)
                    iplStaticMeshRemove(mesh, scene_for_static);
                iplStaticMeshRelease(&mesh);
            }
            static_meshes.clear();
        }
        if (triangle_count > 0) {
            server->notify_geometry_changed_assume_locked(-triangle_count);
        }
        if (sub_scene) {
            if (dynamic_object)
                iplSceneCommit(sub_scene);
            iplSceneRelease(&sub_scene);
            sub_scene = nullptr;
        }
    } else {
        if (dynamic_object && instanced_mesh) {
            iplInstancedMeshRelease(&instanced_mesh);
            instanced_mesh = nullptr;
        }
        if (dynamic_object && sub_scene) {
            static_meshes.clear();
        } else {
            for (auto& mesh : static_meshes) {
                iplStaticMeshRelease(&mesh);
            }
            static_meshes.clear();
        }
        if (instanced_mesh) {
            iplInstancedMeshRelease(&instanced_mesh);
        }
        if (sub_scene) {
            iplSceneRelease(&sub_scene);
            sub_scene = nullptr;
        }
    }

    static_meshes.clear();
    instanced_mesh = nullptr;
    triangle_count = 0;
}

void ResonanceGeometry::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_material", "p_material"), &ResonanceGeometry::set_material);
    ClassDB::bind_method(D_METHOD("get_material"), &ResonanceGeometry::get_material);
    ClassDB::bind_method(D_METHOD("refresh_geometry"), &ResonanceGeometry::refresh_geometry);
    ClassDB::bind_method(D_METHOD("sync_static_scene_coverage"), &ResonanceGeometry::sync_static_scene_coverage);
    ClassDB::bind_method(D_METHOD("flush_dynamic_acoustic_transform"), &ResonanceGeometry::flush_dynamic_acoustic_transform);
    ClassDB::bind_method(D_METHOD("sync_reflection_debug_viz"), &ResonanceGeometry::sync_reflection_debug_viz);
    ClassDB::bind_method(D_METHOD("get_mesh_bake_transform"), &ResonanceGeometry::get_mesh_bake_transform);
    ClassDB::bind_method(D_METHOD("_deferred_retry_create_meshes"), &ResonanceGeometry::_deferred_retry_create_meshes);
    ClassDB::bind_method(D_METHOD("discard_meshes_before_scene_release"), &ResonanceGeometry::discard_meshes_before_scene_release);
    ClassDB::bind_method(D_METHOD("set_dynamic", "p_dynamic"), &ResonanceGeometry::set_dynamic);
    ClassDB::bind_method(D_METHOD("is_dynamic"), &ResonanceGeometry::is_dynamic);
    ClassDB::bind_method(D_METHOD("set_mesh_asset", "p_asset"), &ResonanceGeometry::set_mesh_asset);
    ClassDB::bind_method(D_METHOD("get_mesh_asset"), &ResonanceGeometry::get_mesh_asset);
    ClassDB::bind_method(D_METHOD("set_geometry_override", "p_mesh"), &ResonanceGeometry::set_geometry_override);
    ClassDB::bind_method(D_METHOD("get_geometry_override"), &ResonanceGeometry::get_geometry_override);
    ClassDB::bind_method(D_METHOD("set_show_geometry_override_in_viewport", "p_show"), &ResonanceGeometry::set_show_geometry_override_in_viewport);
    ClassDB::bind_method(D_METHOD("is_show_geometry_override_in_viewport"), &ResonanceGeometry::is_show_geometry_override_in_viewport);
    ClassDB::bind_method(D_METHOD("set_export_all_children", "p_export"), &ResonanceGeometry::set_export_all_children);
    ClassDB::bind_method(D_METHOD("get_export_all_children"), &ResonanceGeometry::get_export_all_children);
    ClassDB::bind_method(D_METHOD("export_dynamic_mesh_to_asset", "p_path"), &ResonanceGeometry::export_dynamic_mesh_to_asset);

    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "ResonanceMaterial"), "set_material", "get_material");
    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "mesh_asset", PROPERTY_HINT_RESOURCE_TYPE, "ResonanceGeometryAsset"), "set_mesh_asset", "get_mesh_asset");
    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "geometry_override", PROPERTY_HINT_RESOURCE_TYPE, "Mesh"), "set_geometry_override", "get_geometry_override");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_geometry_override_in_viewport"), "set_show_geometry_override_in_viewport", "is_show_geometry_override_in_viewport");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "export_all_children"), "set_export_all_children", "get_export_all_children");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "dynamic"), "set_dynamic", "is_dynamic");
}

void ResonanceGeometry::_validate_property(PropertyInfo& p_property) const {
    Node3D::_validate_property(p_property);
    if (p_property.name == StringName("export_all_children") && dynamic_object) {
        p_property.usage = PROPERTY_USAGE_NO_EDITOR;
    }
}