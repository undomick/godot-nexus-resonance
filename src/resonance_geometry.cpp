#include "resonance_geometry.h"
#include "resonance_constants.h"
#include "resonance_debug_log.h"
#include "resonance_geometry_asset.h"
#include "resonance_log.h"
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

static IPLMaterial get_default_ipl_material() {
    IPLMaterial m{};
    m.absorption[0] = resonance::kSceneExportAbsorptionLow;
    m.absorption[1] = resonance::kSceneExportAbsorptionMid;
    m.absorption[2] = resonance::kSceneExportAbsorptionHigh;
    m.scattering = resonance::kSceneExportScattering;
    m.transmission[0] = m.transmission[1] = m.transmission[2] = resonance::kSceneExportTransmission;
    return m;
}

/// Parse Godot mesh to IPL vertices/triangles. Returns true if at least one triangle was added.
static bool parse_mesh_to_ipl(const Ref<Mesh>& mesh, const Transform3D& xform,
                              std::vector<IPLVector3>& out_vertices, std::vector<IPLTriangle>& out_triangles,
                              std::vector<IPLint32>& out_mat_indices) {
    if (mesh.is_null())
        return false;
    for (int i = 0; i < mesh->get_surface_count(); i++) {
        Array arrays = mesh->surface_get_arrays(i);
        if (arrays.size() != Mesh::ARRAY_MAX)
            continue;

        PackedVector3Array vertices = arrays[Mesh::ARRAY_VERTEX];
        PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];

        if (vertices.size() < 3)
            continue;

        size_t v_offset = out_vertices.size();

        for (int v = 0; v < vertices.size(); v++) {
            Vector3 vec = xform.xform(vertices[v]);
            out_vertices.push_back({vec.x, vec.y, vec.z});
        }

        if (!indices.is_empty()) {
            for (int idx = 0; idx < indices.size(); idx += 3) {
                if (idx + 2 >= (int)indices.size())
                    break;
                out_triangles.push_back({(int)indices[idx] + (int)v_offset,
                                         (int)indices[idx + 1] + (int)v_offset,
                                         (int)indices[idx + 2] + (int)v_offset});
                out_mat_indices.push_back(0);
            }
        } else {
            for (int v = 0; v < vertices.size(); v += 3) {
                if (v + 2 >= vertices.size())
                    break;
                out_triangles.push_back({(int)v + (int)v_offset,
                                         (int)(v + 1) + (int)v_offset,
                                         (int)(v + 2) + (int)v_offset});
                out_mat_indices.push_back(0);
            }
        }
    }
    return !out_triangles.empty();
}

/// When ResonanceDynamicGeometry sits next to a glTF/instance subtree (parent is Node3D, not MeshInstance3D),
/// geometry_override matches the visual mesh but vertices are in mesh space - use that MeshInstance3D's global transform.
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
    // Godot frees child nodes automatically when parent is removed; do not queue_free.
    viz_geometry_override = nullptr;
    // Always detach IPL handles under simulation_mutex. Bake uses an isolated temp scene, so
    // editor scene switches no longer race a multi-minute live-scene bake lock.
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
    if (export_all_children != p_export) {
        export_all_children = p_export;
    }
}

bool ResonanceGeometry::get_export_all_children() const {
    return export_all_children;
}

Transform3D ResonanceGeometry::get_mesh_bake_transform() const {
    if (geometry_override.is_valid()) {
        MeshInstance3D* parent_mi = Object::cast_to<MeshInstance3D>(get_parent());
        if (parent_mi && parent_mi->get_mesh() == geometry_override)
            return parent_mi->get_global_transform();
        if (bake_transform_override_cache_valid_ && bake_override_mesh_instance_id_.is_valid()) {
            Object* o = ObjectDB::get_instance(static_cast<uint64_t>(bake_override_mesh_instance_id_));
            MeshInstance3D* mi = Object::cast_to<MeshInstance3D>(o);
            if (mi && mi->get_mesh() == geometry_override && mi->is_inside_tree())
                return mi->get_global_transform();
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
                return found->get_global_transform();
        }
        return get_global_transform();
    }
    MeshInstance3D* mi = Object::cast_to<MeshInstance3D>(get_parent());
    if (mi)
        return mi->get_global_transform();
    return get_global_transform();
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

void ResonanceGeometry::_clear_meshes_impl(bool notify_server) {
    // Do not drop static-scene-root cache here: _create_meshes often clears then rebuilds the same frame.
    _invalidate_topology_caches(false);
    ResonanceServer* server = ResonanceServer::get_singleton();

    if (server && debug_mesh_id >= 0) {
        server->unregister_debug_mesh(debug_mesh_id);
        debug_mesh_id = -1;
    }

    // Clean up Steam Audio resources. Caller must hold simulation_mutex when server is valid.
    // Dynamic geometry: static_meshes live in sub_scene; instanced_mesh is in the global scene.
    // Detach InstancedMesh from the global scene before removing static meshes from sub_scene (runtime crash otherwise).
    if (server && server->is_initialized()) {
        if (dynamic_object && instanced_mesh) {
            server->cancel_pending_dynamic_instanced_mesh_transform(instanced_mesh);
            IPLScene global_scene_handle = server->get_scene_handle();
            iplInstancedMeshRemove(instanced_mesh, global_scene_handle);
            iplInstancedMeshRelease(&instanced_mesh);
            instanced_mesh = nullptr;
            // Phonon: InstancedMeshRemove requires iplSceneCommit on the parent scene before other scene edits.
            if (global_scene_handle)
                iplSceneCommit(global_scene_handle);
        }

        IPLScene scene_for_static = dynamic_object ? sub_scene : server->get_scene_handle();
        // Dynamic sub_scene: after InstancedMesh detach+commit on global scene, per-mesh Remove/Release on
        // sub_scene can crash (Phonon 4.8.x). Drop handles; iplSceneRelease(sub_scene) owns mesh cleanup.
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

        // Notify change if we removed triangles (caller holds simulation_mutex via _clear_meshes).
        // Rebuild paths pass notify_server=false and issue a single net notify after create.
        if (notify_server && triangle_count > 0) {
            server->notify_geometry_changed_assume_locked(-triangle_count);
        }
    } else {
        // Fallback cleanup (just release handles, don't touch scene)
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

    // Sub-scene belongs to this object, not the global scene directly
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

void ResonanceGeometry::_clear_meshes() {
    ResonanceServer* server = ResonanceServer::get_singleton();
    if (server && server->is_initialized()) {
        auto lock = server->scoped_simulation_lock();
        _clear_meshes_impl();
    } else {
        _clear_meshes_impl();
    }
}

static bool _scene_has_static_scene_asset(Node* node) {
    if (!node)
        return false;
    if (node->is_class("ResonanceStaticScene")) {
        ResonanceStaticScene* ss = Object::cast_to<ResonanceStaticScene>(node);
        if (ss && ss->has_valid_asset())
            return true;
    }
    for (int i = 0; i < node->get_child_count(); i++) {
        if (_scene_has_static_scene_asset(node->get_child(i)))
            return true;
    }
    return false;
}

void ResonanceGeometry::_create_meshes() {
    Node* parent = get_parent();
    Node3D* node3d = Object::cast_to<Node3D>(parent);
    if (!node3d || !node3d->is_visible_in_tree())
        return;

    // static geometry is in the exported scene; skip when ResonanceStaticScene provides it
    if (!dynamic_object && (!Engine::get_singleton() || !Engine::get_singleton()->is_editor_hint())) {
        Node* root = this;
        while (root->get_parent())
            root = root->get_parent();
        const ObjectID root_oid(root->get_instance_id());
        if (!static_scene_asset_query_valid_ || cached_scene_tree_root_id_ != root_oid) {
            cached_scene_tree_root_id_ = root_oid;
            cached_root_has_static_scene_asset_ = _scene_has_static_scene_asset(root);
            static_scene_asset_query_valid_ = true;
        }
        if (cached_root_has_static_scene_asset_)
            return;
    }

    const int previous_triangle_count = triangle_count;
    const bool use_asset_path = dynamic_object && mesh_asset.is_valid() && mesh_asset->is_valid();
    MeshInstance3D* mesh_instance = Object::cast_to<MeshInstance3D>(parent);

    Ref<Mesh> mesh_for_geom;
    if (!use_asset_path) {
        if (geometry_override.is_valid()) {
            mesh_for_geom = geometry_override;
        } else if (mesh_instance) {
            mesh_for_geom = mesh_instance->get_mesh();
        }
        if (mesh_for_geom.is_null())
            return;
    }

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
                "Optional String meta resonance_physics_material_preset on colliders: generic, concrete, wood, metal, glass, carpet, brick.");
        }
        return;
    }

    Transform3D xform = dynamic_object ? Transform3D() : get_mesh_bake_transform();

    if (use_asset_path) {
        // Build new IPL resources into locals first; only clear the previous mesh after success
        // so a failed reload does not leave global_triangle_count permanently reduced.
        IPLSceneSettings sceneSettings{};
        sceneSettings.type = server->get_phonon_mesh_scene_type();
        sceneSettings.embreeDevice = server->get_embree_device_handle();
        sceneSettings.radeonRaysDevice = server->get_radeon_rays_device_handle();

        const int64_t blob_sz = mesh_asset->get_size();
        const uint8_t* blob = mesh_asset->get_data_ptr();
        if (!blob || blob_sz <= 0) {
            ResonanceLog::error("ResonanceGeometry: mesh_asset has no serialized data (asset path).");
            return;
        }

        IPLScene new_sub = nullptr;
        IPLStaticMesh local_mesh = nullptr;
        IPLInstancedMesh new_inst = nullptr;
        auto lock = server->scoped_simulation_lock();

        if (iplSceneCreate(server->get_context_handle(), &sceneSettings, &new_sub) != IPL_STATUS_SUCCESS) {
            ResonanceLog::error("ResonanceGeometry: iplSceneCreate failed (asset path).");
            return;
        }

        IPLSerializedObjectSettings serialSettings{};
        serialSettings.data = const_cast<IPLbyte*>(reinterpret_cast<const IPLbyte*>(blob));
        serialSettings.size = static_cast<IPLsize>(blob_sz);
        IPLSerializedObject serialObj = nullptr;
        if (iplSerializedObjectCreate(server->get_context_handle(), &serialSettings, &serialObj) != IPL_STATUS_SUCCESS) {
            ResonanceLog::error("ResonanceGeometry: iplSerializedObjectCreate failed.");
            iplSceneRelease(&new_sub);
            return;
        }

        IPLerror loadErr = iplStaticMeshLoad(new_sub, serialObj, nullptr, nullptr, &local_mesh);
        iplSerializedObjectRelease(&serialObj);
        if (loadErr != IPL_STATUS_SUCCESS || !local_mesh) {
            ResonanceLog::error("ResonanceGeometry: iplStaticMeshLoad failed.");
            iplSceneRelease(&new_sub);
            return;
        }

        iplStaticMeshAdd(local_mesh, new_sub);
        if (material.is_valid()) {
            IPLMaterial mat = material->get_ipl_material();
            iplStaticMeshSetMaterial(local_mesh, new_sub, &mat, 0);
        }
        iplSceneCommit(new_sub);

        IPLInstancedMeshSettings instSettings{};
        instSettings.subScene = new_sub;
        instSettings.transform = ResonanceUtils::to_ipl_matrix(get_mesh_bake_transform());
        if (iplInstancedMeshCreate(server->get_scene_handle(), &instSettings, &new_inst) != IPL_STATUS_SUCCESS) {
            ResonanceLog::error("ResonanceGeometry: iplInstancedMeshCreate failed (asset path).");
            iplStaticMeshRemove(local_mesh, new_sub);
            iplStaticMeshRelease(&local_mesh);
            iplSceneRelease(&new_sub);
            return;
        }

        _clear_meshes_impl(false);
        sub_scene = new_sub;
        static_meshes.push_back(local_mesh);
        instanced_mesh = new_inst;
        iplInstancedMeshAdd(instanced_mesh, server->get_scene_handle());
        triangle_count = mesh_asset->get_triangle_count();
        log_dynamic_instanced_mesh_registered(server, triangle_count);
    } else {
        // --- Runtime mesh parsing: parse outside lock to reduce lock duration ---
        Ref<Mesh> mesh = mesh_for_geom;
        std::vector<IPLVector3> ipl_vertices;
        std::vector<IPLTriangle> ipl_triangles;
        std::vector<IPLint32> ipl_mat_indices;

        if (!parse_mesh_to_ipl(mesh, xform, ipl_vertices, ipl_triangles, ipl_mat_indices))
            return;

        IPLMaterial mat_settings = material.is_valid() ? material->get_ipl_material() : get_default_ipl_material();

        IPLStaticMeshSettings mesh_settings{};
        mesh_settings.numVertices = (IPLint32)ipl_vertices.size();
        mesh_settings.numTriangles = (IPLint32)ipl_triangles.size();
        mesh_settings.numMaterials = 1;
        mesh_settings.vertices = ipl_vertices.data();
        mesh_settings.triangles = ipl_triangles.data();
        mesh_settings.materials = &mat_settings;
        mesh_settings.materialIndices = ipl_mat_indices.data();

        {
            auto lock = server->scoped_simulation_lock();

            if (dynamic_object) {
                IPLSceneSettings sceneSettings{};
                sceneSettings.type = server->get_phonon_mesh_scene_type();
                sceneSettings.embreeDevice = server->get_embree_device_handle();
                sceneSettings.radeonRaysDevice = server->get_radeon_rays_device_handle();

                IPLScene new_sub = nullptr;
                IPLStaticMesh local_mesh = nullptr;
                IPLInstancedMesh new_inst = nullptr;

                if (iplSceneCreate(server->get_context_handle(), &sceneSettings, &new_sub) != IPL_STATUS_SUCCESS) {
                    ResonanceLog::error("ResonanceGeometry: iplSceneCreate failed (dynamic).");
                    return;
                }
                if (iplStaticMeshCreate(new_sub, &mesh_settings, &local_mesh) != IPL_STATUS_SUCCESS) {
                    ResonanceLog::error("ResonanceGeometry: iplStaticMeshCreate failed (dynamic).");
                    iplSceneRelease(&new_sub);
                    return;
                }
                iplStaticMeshAdd(local_mesh, new_sub);
                iplSceneCommit(new_sub);

                IPLInstancedMeshSettings instSettings{};
                instSettings.subScene = new_sub;
                instSettings.transform = ResonanceUtils::to_ipl_matrix(get_mesh_bake_transform());
                if (iplInstancedMeshCreate(server->get_scene_handle(), &instSettings, &new_inst) != IPL_STATUS_SUCCESS) {
                    ResonanceLog::error("ResonanceGeometry: iplInstancedMeshCreate failed (dynamic).");
                    iplStaticMeshRemove(local_mesh, new_sub);
                    iplStaticMeshRelease(&local_mesh);
                    iplSceneRelease(&new_sub);
                    return;
                }

                _clear_meshes_impl(false);
                sub_scene = new_sub;
                static_meshes.push_back(local_mesh);
                instanced_mesh = new_inst;
                iplInstancedMeshAdd(instanced_mesh, server->get_scene_handle());
                triangle_count = (int)ipl_triangles.size();
                log_dynamic_instanced_mesh_registered(server, triangle_count);
            } else {
                IPLStaticMesh new_mesh = nullptr;
                if (iplStaticMeshCreate(server->get_scene_handle(), &mesh_settings, &new_mesh) != IPL_STATUS_SUCCESS) {
                    ResonanceLog::error("ResonanceGeometry: iplStaticMeshCreate failed (static).");
                    return;
                }
                _clear_meshes_impl(false);
                iplStaticMeshAdd(new_mesh, server->get_scene_handle());
                static_meshes.push_back(new_mesh);
                triangle_count = (int)ipl_triangles.size();
            }

            if (triangle_count > 0 && server->wants_debug_reflection_viz()) {
                Transform3D tr = dynamic_object ? get_mesh_bake_transform() : Transform3D();
                IPLMatrix4x4 ipl_xform = ResonanceUtils::to_ipl_matrix(tr);
                debug_mesh_id = server->register_debug_mesh(ipl_vertices, ipl_triangles, ipl_mat_indices.data(), &ipl_xform, &mat_settings);
                resonance::debug_log_raw("resonance_geometry:register_debug", "mesh_registered", triangle_count, debug_mesh_id);
            }
        }
    }

    const int net_triangles = triangle_count - previous_triangle_count;
    if (net_triangles != 0) {
        server->notify_geometry_changed(net_triangles);
    } else {
        // Mesh replaced with the same triangle count; still need iplSceneCommit.
        server->mark_scene_commit_pending();
    }
}

void ResonanceGeometry::_update_dynamic_transform() {
    if (!instanced_mesh)
        return;

    ResonanceServer* server = ResonanceServer::get_singleton();
    if (!server)
        return;

    IPLMatrix4x4 mat = ResonanceUtils::to_ipl_matrix(get_mesh_bake_transform());

    // Queue for worker: avoids simulation_mutex on the main thread; see ResonanceServer::_apply_queued_dynamic_instanced_mesh_transforms_assume_locked.
    // Coalesce with transform-only geometry notifies (consume_geometry_transform_coalesce_tick); use flush_dynamic_acoustic_transform at motion end for an exact final pose when using dynamic_scene_commit_min_interval > 0.
    if (!server->consume_geometry_transform_coalesce_tick())
        return;
    server->enqueue_dynamic_instanced_mesh_transform(instanced_mesh, mat);
}

void ResonanceGeometry::flush_dynamic_acoustic_transform() {
    if (!dynamic_object || !instanced_mesh)
        return;
    ResonanceServer* server = ResonanceServer::get_singleton();
    if (!server)
        return;
    // Phase 1: do not take simulation_mutex on the main thread. The dynamic instanced mesh transform queue
    // drained by the worker in _apply_queued_dynamic_instanced_mesh_transforms_assume_locked already covers
    // this transform; the synchronous iplInstancedMeshUpdateTransform here was redundant with the queue and
    // caused main-thread stalls up to 200ms when the worker was mid-RunReflections. scene_dirty is atomic,
    // no lock required to request a scene commit.
    IPLMatrix4x4 mat = ResonanceUtils::to_ipl_matrix(get_mesh_bake_transform());
    server->enqueue_dynamic_instanced_mesh_transform(instanced_mesh, mat);
    server->mark_scene_commit_pending();
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
    // Material is baked into IPL static / instanced meshes at creation time. Without rebuilding the
    // mesh, swapping a ResonanceMaterial keeps Steam Audio on the previous absorption / scattering /
    // transmission coefficients - audibly stale for occlusion, reverb tone, and pathing transmission.
    if (is_inside_tree())
        _create_meshes();
}
Ref<ResonanceMaterial> ResonanceGeometry::get_material() const { return material; }

Error ResonanceGeometry::export_dynamic_mesh_to_asset(const String& p_path) {
    Node* parent = get_parent();
    Node3D* node3d = Object::cast_to<Node3D>(parent);
    if (!parent || !node3d || !node3d->is_visible_in_tree())
        return ERR_INVALID_PARAMETER;

    MeshInstance3D* mesh_instance = Object::cast_to<MeshInstance3D>(parent);
    Ref<Mesh> mesh = geometry_override.is_valid() ? geometry_override : (mesh_instance ? mesh_instance->get_mesh() : Ref<Mesh>());
    if (mesh.is_null())
        return ERR_INVALID_PARAMETER;

    std::vector<IPLVector3> ipl_vertices;
    std::vector<IPLTriangle> ipl_triangles;
    std::vector<IPLint32> ipl_mat_indices;

    Transform3D xform; // Identity for dynamic (local space)
    if (!parse_mesh_to_ipl(mesh, xform, ipl_vertices, ipl_triangles, ipl_mat_indices))
        return ERR_INVALID_PARAMETER;

    IPLMaterial mat_settings = material.is_valid() ? material->get_ipl_material() : get_default_ipl_material();

    IPLStaticMeshSettings mesh_settings{};
    mesh_settings.numVertices = (IPLint32)ipl_vertices.size();
    mesh_settings.numTriangles = (IPLint32)ipl_triangles.size();
    mesh_settings.numMaterials = 1;
    mesh_settings.vertices = ipl_vertices.data();
    mesh_settings.triangles = ipl_triangles.data();
    mesh_settings.materials = &mat_settings;
    mesh_settings.materialIndices = ipl_mat_indices.data();

    // Standalone export: create minimal Phonon context/scene (no ResonanceServer required)
    IPLContext export_context = nullptr;
    IPLContextSettings ctx_settings{};
    ctx_settings.version = STEAMAUDIO_VERSION;
    if (iplContextCreate(&ctx_settings, &export_context) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceGeometry: iplContextCreate failed (export_dynamic_mesh_to_asset).");
        return ERR_CANT_CREATE;
    }

    IPLSceneSettings scene_settings{};
    scene_settings.type = IPL_SCENETYPE_DEFAULT;

    IPLScene temp_scene = nullptr;
    if (iplSceneCreate(export_context, &scene_settings, &temp_scene) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceGeometry: iplSceneCreate failed (export_dynamic_mesh_to_asset).");
        iplContextRelease(&export_context);
        return ERR_CANT_CREATE;
    }

    IPLStaticMesh temp_mesh = nullptr;
    if (iplStaticMeshCreate(temp_scene, &mesh_settings, &temp_mesh) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceGeometry: iplStaticMeshCreate failed (export_dynamic_mesh_to_asset).");
        iplSceneRelease(&temp_scene);
        iplContextRelease(&export_context);
        return ERR_CANT_CREATE;
    }

    iplStaticMeshAdd(temp_mesh, temp_scene);
    iplSceneCommit(temp_scene);

    IPLSerializedObjectSettings serial_settings{};
    IPLSerializedObject serial_obj = nullptr;
    if (iplSerializedObjectCreate(export_context, &serial_settings, &serial_obj) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceGeometry: iplSerializedObjectCreate failed (export_dynamic_mesh_to_asset).");
        iplStaticMeshRelease(&temp_mesh);
        iplSceneRelease(&temp_scene);
        iplContextRelease(&export_context);
        return ERR_CANT_CREATE;
    }

    iplStaticMeshSave(temp_mesh, serial_obj);

    IPLsize size = iplSerializedObjectGetSize(serial_obj);
    IPLbyte* data = iplSerializedObjectGetData(serial_obj);
    if (!data || size == 0) {
        ResonanceLog::error("ResonanceGeometry: iplStaticMeshSave produced no data (export_dynamic_mesh_to_asset).");
        iplSerializedObjectRelease(&serial_obj);
        iplStaticMeshRelease(&temp_mesh);
        iplSceneRelease(&temp_scene);
        iplContextRelease(&export_context);
        return ERR_CANT_CREATE;
    }

    Ref<ResonanceGeometryAsset> asset;
    asset.instantiate();
    PackedByteArray pba;
    pba.resize((int64_t)size);
    memcpy(pba.ptrw(), data, size);
    asset->set_mesh_data(pba);
    asset->set_triangle_count((int)ipl_triangles.size());

    iplSerializedObjectRelease(&serial_obj);
    iplStaticMeshRelease(&temp_mesh);
    iplSceneRelease(&temp_scene);
    iplContextRelease(&export_context);

    String path = p_path;
    if (!path.ends_with(".tres") && !path.ends_with(".res")) {
        path += ".tres";
    }
    ProjectSettings* ps = ProjectSettings::get_singleton();
    if (ps && (path.begins_with("res://") || path.begins_with("user://"))) {
        path = ps->globalize_path(path);
    }

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

    const auto clear_debug_registration = [&]() {
        if (debug_mesh_id < 0)
            return;
        auto lock = server->scoped_simulation_lock();
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

    if (!dynamic_object && (!Engine::get_singleton() || !Engine::get_singleton()->is_editor_hint())) {
        Node* root = this;
        while (root->get_parent())
            root = root->get_parent();
        const ObjectID root_oid(root->get_instance_id());
        if (!static_scene_asset_query_valid_ || cached_scene_tree_root_id_ != root_oid) {
            cached_scene_tree_root_id_ = root_oid;
            cached_root_has_static_scene_asset_ = _scene_has_static_scene_asset(root);
            static_scene_asset_query_valid_ = true;
        }
        if (cached_root_has_static_scene_asset_) {
            clear_debug_registration();
            return;
        }
    }

    const bool use_asset_path = dynamic_object && mesh_asset.is_valid() && mesh_asset->is_valid();
    if (use_asset_path) {
        clear_debug_registration();
        return;
    }

    MeshInstance3D* mesh_instance = Object::cast_to<MeshInstance3D>(parent);
    Ref<Mesh> mesh_for_geom;
    if (geometry_override.is_valid()) {
        mesh_for_geom = geometry_override;
    } else if (mesh_instance) {
        mesh_for_geom = mesh_instance->get_mesh();
    }
    if (mesh_for_geom.is_null()) {
        clear_debug_registration();
        return;
    }

    Transform3D xform = dynamic_object ? Transform3D() : get_mesh_bake_transform();

    const bool reuse_parsed =
        dynamic_object && reflection_debug_parsed_mesh_ == mesh_for_geom && !reflection_debug_parsed_triangles_.empty();
    if (!reuse_parsed) {
        reflection_debug_parsed_mesh_ = mesh_for_geom;
        reflection_debug_parsed_vertices_.clear();
        reflection_debug_parsed_triangles_.clear();
        reflection_debug_parsed_mat_indices_.clear();
        if (!parse_mesh_to_ipl(mesh_for_geom, xform, reflection_debug_parsed_vertices_, reflection_debug_parsed_triangles_,
                               reflection_debug_parsed_mat_indices_)) {
            clear_debug_registration();
            return;
        }
    }

    IPLMaterial mat_settings = material.is_valid() ? material->get_ipl_material() : get_default_ipl_material();

    {
        auto lock = server->scoped_simulation_lock();
        if (debug_mesh_id >= 0) {
            server->unregister_debug_mesh(debug_mesh_id);
            debug_mesh_id = -1;
        }
        Transform3D tr = dynamic_object ? get_mesh_bake_transform() : Transform3D();
        IPLMatrix4x4 ipl_xform = ResonanceUtils::to_ipl_matrix(tr);
        debug_mesh_id =
            server->register_debug_mesh(reflection_debug_parsed_vertices_, reflection_debug_parsed_triangles_,
                                        reflection_debug_parsed_mat_indices_.data(), &ipl_xform, &mat_settings);
        resonance::debug_log_raw("resonance_geometry:sync_reflection_debug_viz", "mesh_registered",
                                 (int)reflection_debug_parsed_triangles_.size(), debug_mesh_id);
    }
}

void ResonanceGeometry::refresh_geometry() {
    _create_meshes();
}

void ResonanceGeometry::discard_meshes_before_scene_release() {
    ResonanceServer* server = ResonanceServer::get_singleton();
    if (server && debug_mesh_id >= 0) {
        server->unregister_debug_mesh(debug_mesh_id);
        debug_mesh_id = -1;
    }

    // Release IPL resources before scene teardown. Phonon uses refcounting; meshes must be
    // explicitly removed and released. Static meshes are in global scene (static path) or sub_scene (dynamic path).
    if (server && server->is_initialized()) {
        auto lock = server->scoped_simulation_lock();
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