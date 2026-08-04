#include "resonance_probe_volume.h"
#include "resonance_constants.h"
#include "resonance_log.h"
#include "resonance_player.h"
#include "resonance_probe_exclusion.h"
#include "resonance_probe_exclusion_filter.h"
#include "resonance_server.h"
#include "resonance_source_handle_policy.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/physics_direct_space_state3d.hpp>
#include <godot_cpp/classes/physics_ray_query_parameters3d.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <utility>
#include <vector>

using namespace godot;

namespace {

const char* kBakeConfigScriptPath = "res://addons/nexus_resonance/scripts/resonance_bake_config.gd";

Ref<Resource> create_default_bake_config() {
    Ref<Script> bake_script = ResourceLoader::get_singleton()->load(kBakeConfigScriptPath);
    if (bake_script.is_null()) {
        UtilityFunctions::push_warning(String("ResonanceProbeVolume: failed to load ResonanceBakeConfig script at ") +
                                       kBakeConfigScriptPath);
        return Ref<Resource>();
    }
    // Prefer create_default(); fall back to Script.new() (static call can fail on some Script loads).
    Variant created = bake_script->call("create_default");
    if (created.get_type() != Variant::OBJECT || created.operator Object*() == nullptr) {
        created = bake_script->call("new");
    }
    Object* def = Object::cast_to<Object>(created);
    Ref<Resource> out(Object::cast_to<Resource>(def));
    if (out.is_null()) {
        UtilityFunctions::push_warning("ResonanceProbeVolume: failed to instantiate ResonanceBakeConfig");
    }
    return out;
}

bool array_has_nodepath(const Array& arr, const NodePath& path) {
    for (int i = 0; i < arr.size(); i++) {
        if (NodePath(arr[i]) == path) {
            return true;
        }
    }
    return false;
}

NodePath bake_target_path_from_variant(Node* self, const Variant& p_value) {
    if (p_value.get_type() == Variant::NODE_PATH) {
        return p_value;
    }
    if (p_value.get_type() == Variant::STRING || p_value.get_type() == Variant::STRING_NAME) {
        return NodePath(String(p_value));
    }
    Node* n = Object::cast_to<Node>(p_value);
    if (n == nullptr || self == nullptr) {
        return NodePath();
    }
    if (!self->is_inside_tree() || !n->is_inside_tree()) {
        return NodePath();
    }
    return self->get_path_to(n);
}

} // namespace

ResonanceProbeVolume::ResonanceProbeVolume() {}

ResonanceProbeVolume::~ResonanceProbeVolume() {
    // Safety: ensure probe batch is removed when volume is destroyed (e.g. deleted, never added to tree, undo edge cases).
    _release_probe_batch_if_live();
}

void ResonanceProbeVolume::_release_probe_batch_if_live() {
    if (probe_batch_handle < 0)
        return;
    ResonanceServer* srv = ResonanceServer::get_singleton();
    // After reinit, IDs are recycled; removing a recycled ID would drop another volume's batch.
    if (srv && !ResonanceServer::is_shutting_down() &&
        resonance::probe_batch_handle_matches_lifecycle_epoch(probe_batch_handle, probe_batch_lifecycle_epoch_,
                                                              srv->get_probe_batch_lifecycle_epoch()))
        srv->remove_probe_batch(probe_batch_handle);
    probe_batch_handle = -1;
    probe_batch_lifecycle_epoch_ = 0;
}

void ResonanceProbeVolume::_store_probe_batch_handle(int32_t handle) {
    probe_batch_handle = handle;
    if (handle < 0) {
        probe_batch_lifecycle_epoch_ = 0;
        return;
    }
    ResonanceServer* srv = ResonanceServer::get_singleton();
    probe_batch_lifecycle_epoch_ = srv ? srv->get_probe_batch_lifecycle_epoch() : 0;
}

void ResonanceProbeVolume::_ensure_viz_instance() {
    // Lazy: never allocate RenderingServer mesh RIDs in the constructor (ClassDB/doc temps free them off the render thread).
    if (!viz_multimesh.is_valid())
        _create_visuals_resources();
    if (viz_instance)
        return;
    viz_instance = memnew(MultiMeshInstance3D);
    viz_instance->set_multimesh(viz_multimesh);
    viz_instance->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
    add_child(viz_instance);
    viz_instance->set_visible(viz_visible);
}

void ResonanceProbeVolume::_create_visuals_resources() {
    viz_multimesh.instantiate();
    viz_multimesh->set_transform_format(MultiMesh::TRANSFORM_3D);
    viz_multimesh->set_use_colors(true);

    viz_mesh.instantiate();
    viz_mesh->set_radius(resonance::kProbeVizMeshRadius);
    viz_mesh->set_height(resonance::kProbeVizMeshHeight);

    Ref<StandardMaterial3D> mat;
    mat.instantiate();
    mat->set_albedo(Color(1.0, 1.0, 1.0));
    mat->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
    mat->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
    mat->set_flag(BaseMaterial3D::FLAG_DISABLE_DEPTH_TEST, true);
    mat->set("vertex_color_is_srgb", true);
    viz_mesh->set_material(mat);

    viz_multimesh->set_mesh(viz_mesh);
}

void ResonanceProbeVolume::_notification(int p_what) {
    if (p_what == Node::NOTIFICATION_ENTER_TREE) {
        // Editor: _ready often does not run when the node is first created/inspected.
        // ENTER_TREE + ensure_default_resources covers create / open / reparent.
        if (Engine::get_singleton() && Engine::get_singleton()->is_editor_hint()) {
            _ensure_editor_default_resources();
        }
    } else if (p_what == NOTIFICATION_TRANSFORM_CHANGED) {
        _queue_update();
    } else if (p_what == Node::NOTIFICATION_EXIT_TREE) {
        _clear_player_refs_to_this();
    }
}

static void clear_player_ref_to_volume(ResonanceProbeVolume* self, ResonancePlayer* player) {
    if (!player)
        return;
    NodePath pv = player->get_pathing_probe_volume();
    if (pv.is_empty())
        return;
    Node* target = player->get_node_or_null(pv);
    if (target == self) {
        player->clear_pathing_probe_immediate();
    }
}

// Godot engine bug: Deleting a referenced ResonanceProbeVolume triggers
// "ERROR: core/string/node_path.cpp:272 - Condition "!p_np.is_absolute()" is true. Returning: NodePath()"
// when ResonancePlayer.pathing_probe_volume points to it. We clear those refs on EXIT_TREE to avoid this.
// Also try tree root as fallback when edited scene root is null (e.g. during editor teardown).
// IMPORTANT: We must call clear_pathing_probe_immediate() to sync update_source(pathing_batch=-1) before
// _exit_tree removes the probe batch; otherwise the worker may use freed batch data (use-after-free).
void ResonanceProbeVolume::_clear_player_refs_to_this() {
    SceneTree* st = get_tree();
    if (!st)
        return;
    Array players = st->get_nodes_in_group("resonance_player");
    for (int i = 0; i < players.size(); i++) {
        Node* n = Object::cast_to<Node>(players[i]);
        ResonancePlayer* rp = Object::cast_to<ResonancePlayer>(n);
        clear_player_ref_to_volume(this, rp);
    }
}

void ResonanceProbeVolume::_ensure_editor_default_resources() {
    ensure_default_resources();
}

void ResonanceProbeVolume::ensure_default_resources() {
    if (probe_data.is_null()) {
        Ref<ResonanceProbeData> pd;
        pd.instantiate();
        set_probe_data(pd);
    }
    if (bake_config.is_null()) {
        Ref<Resource> bc = create_default_bake_config();
        if (bc.is_valid()) {
            set_bake_config(bc);
        }
    }
}

static void collect_exclusion_boxes_recursive(Node* node, std::vector<std::pair<String, Dictionary>>& out) {
    if (node == nullptr) {
        return;
    }
    ResonanceProbeExclusion* ex = Object::cast_to<ResonanceProbeExclusion>(node);
    if (ex != nullptr && ex->is_enabled() && ex->is_inside_tree()) {
        Dictionary d;
        d["xform"] = ex->get_global_transform();
        d["size"] = ex->get_region_size();
        out.push_back({String(ex->get_path()), d});
    }
    for (int i = 0; i < node->get_child_count(); i++) {
        collect_exclusion_boxes_recursive(node->get_child(i), out);
    }
}

Array ResonanceProbeVolume::collect_exclusion_boxes() const {
    std::vector<std::pair<String, Dictionary>> items;
    for (int i = 0; i < get_child_count(); i++) {
        collect_exclusion_boxes_recursive(get_child(i), items);
    }
    std::sort(items.begin(), items.end(),
              [](const std::pair<String, Dictionary>& a, const std::pair<String, Dictionary>& b) {
                  return a.first < b.first;
              });
    Array out;
    for (const auto& it : items) {
        out.push_back(it.second);
    }
    return out;
}

void ResonanceProbeVolume::notify_exclusion_changed() {
    _queue_update();
}

void ResonanceProbeVolume::_ready() {
    set_notify_transform(true);
    add_to_group("resonance_probe_volume");
    if (Engine::get_singleton() && Engine::get_singleton()->is_editor_hint()) {
        // Defaults also on ENTER_TREE; keep here for nodes that skip ENTER_TREE edge cases.
        _ensure_editor_default_resources();
        if (probe_data.is_valid() && !probe_data->get_path().is_empty()) {
            call_deferred("_check_probe_data_loaded");
        }

        // Setup Visualization (only create when show_probes is On)
        if (viz_visible) {
            _ensure_viz_instance();
            call_deferred("_update_visuals");
        }

        set_process(true);
    } else {
        // RUNTIME
        set_process(false);
        if (viz_instance)
            viz_instance->set_visible(false);

        ResonanceServer* srv = ResonanceServer::get_singleton();
        if (srv && srv->is_initialized() && probe_data.is_valid()) {
            _store_probe_batch_handle(srv->load_probe_batch(probe_data));
        } else if (probe_data.is_valid()) {
            // Server may not be ready yet (autoload order); retry deferred
            call_deferred("_runtime_load_probe_batch");
        }
    }
}

void ResonanceProbeVolume::_check_probe_data_loaded() {
    if (probe_data.is_valid() && probe_data->get_size() > 0) {
        _queue_update();
    }
}

void ResonanceProbeVolume::_runtime_load_probe_batch() {
    if (!_has_valid_resonance_config()) {
        if (_runtime_load_retry_count < resonance::kProbeVolumeMaxRuntimeLoadRetries) {
            _runtime_load_retry_count++;
            call_deferred("_runtime_load_probe_batch");
            return;
        }
        _runtime_load_retry_count = 0;
        UtilityFunctions::push_error("Nexus Resonance: ResonanceProbeVolume requires a ResonanceRuntime node with a valid ResonanceRuntimeConfig in the scene.");
        return;
    }
    _runtime_load_retry_count = 0;
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (srv && srv->is_initialized() && probe_data.is_valid()) {
        _store_probe_batch_handle(srv->load_probe_batch(probe_data));
    }
}

void ResonanceProbeVolume::set_headless_baking_mode(bool p_mode) {
    headless_baking_mode = p_mode;
}

bool ResonanceProbeVolume::is_headless_baking_mode() const {
    return headless_baking_mode;
}

void ResonanceProbeVolume::reload_probe_batch() {
    if (!_has_valid_resonance_config()) {
        UtilityFunctions::push_error("Nexus Resonance: ResonanceProbeVolume requires a ResonanceRuntime node with a valid ResonanceRuntimeConfig in the scene.");
        return;
    }
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!srv || !srv->is_initialized()) {
        UtilityFunctions::push_warning(
            "Nexus Resonance: reload_probe_batch skipped - ResonanceServer is not initialized (probe data may have changed on disk only).");
        return;
    }
    if (!probe_data.is_valid() || probe_data->get_size() <= 0) {
        if (!headless_baking_mode) {
            UtilityFunctions::push_warning("Nexus Resonance: reload_probe_batch skipped - probe_data is missing or empty.");
        }
        return;
    }
    _release_probe_batch_if_live();
    _store_probe_batch_handle(srv->load_probe_batch(probe_data));
    if (Engine::get_singleton() && Engine::get_singleton()->is_editor_hint() && viz_visible) {
        _update_visuals();
    }
}

void ResonanceProbeVolume::_reload_probe_batch_after_reinit() {
    // Server already released every batch and reset handle allocation; local IDs are stale.
    probe_batch_handle = -1;
    probe_batch_lifecycle_epoch_ = 0;
    reload_probe_batch();
}

void ResonanceProbeVolume::release_probe_batch() {
    _release_probe_batch_if_live();
}

void ResonanceProbeVolume::_exit_tree() {
    _release_probe_batch_if_live();
    // viz_instance is a child node; Godot frees children when parent is removed.
    viz_instance = nullptr;
}

void ResonanceProbeVolume::_process(double delta) {
    if (!Engine::get_singleton() || !Engine::get_singleton()->is_editor_hint())
        return;

    // Sync viz_instance visibility with viz_visible (handles editor property load order / setter not firing)
    if (viz_instance && viz_instance->is_visible() != viz_visible) {
        viz_instance->set_visible(viz_visible);
    }

    if (viz_visible && viz_multimesh.is_valid()) {
        if (viz_multimesh->get_instance_count() == 0) {
            viz_retry_timer += delta;
            if (viz_retry_timer > resonance::kProbeVizRetryIntervalSec) {
                viz_retry_timer = 0.0;
                ResonanceServer* srv = ResonanceServer::get_singleton();
                if (srv && srv->is_initialized()) {
                    _queue_update();
                }
            }
        }
    }

    if (update_pending) {
        debounce_timer -= delta;
        if (debounce_timer <= 0.0) {
            _update_visuals();
            update_pending = false;
        }
    }
}

void ResonanceProbeVolume::_queue_update() {
    update_pending = true;
    debounce_timer = resonance::kProbeVizDebounceSec;
}

uint32_t ResonanceProbeVolume::_get_bake_params_hash() const {
    uint32_t h = HASH_MURMUR3_SEED;
    h = hash_murmur3_one_float(region_size.x, h);
    h = hash_murmur3_one_float(region_size.y, h);
    h = hash_murmur3_one_float(region_size.z, h);
    h = hash_murmur3_one_float(spacing, h);
    h = hash_murmur3_one_float(height_above_floor, h);
    h = hash_murmur3_one_32(static_cast<uint32_t>(generation_type), h);
    Transform3D t = get_global_transform();
    h = hash_murmur3_one_float(t.origin.x, h);
    h = hash_murmur3_one_float(t.origin.y, h);
    h = hash_murmur3_one_float(t.origin.z, h);
    h = hash_murmur3_one_float(t.basis.rows[0].x, h);
    h = hash_murmur3_one_float(t.basis.rows[0].y, h);
    h = hash_murmur3_one_float(t.basis.rows[0].z, h);
    h = hash_murmur3_one_float(t.basis.rows[1].x, h);
    h = hash_murmur3_one_float(t.basis.rows[1].y, h);
    h = hash_murmur3_one_float(t.basis.rows[1].z, h);
    h = hash_murmur3_one_float(t.basis.rows[2].x, h);
    h = hash_murmur3_one_float(t.basis.rows[2].y, h);
    h = hash_murmur3_one_float(t.basis.rows[2].z, h);

    // Include bake_config reflection params so changing quality triggers re-bake
    int refl_type = 2;
    int num_rays = resonance::kBakeDefaultNumRays;
    int num_bounces = resonance::kBakeDefaultNumBounces;
    int ambisonics_order = resonance::kBakeDefaultAmbisonicsOrder;
    if (bake_config.is_valid()) {
        Variant v_refl = bake_config->get("reflection_type");
        if (v_refl.get_type() == Variant::INT)
            refl_type = static_cast<int>(v_refl);
        Variant v_rays = bake_config->get("bake_num_rays");
        if (v_rays.get_type() == Variant::INT)
            num_rays = static_cast<int>(v_rays);
        Variant v_bounces = bake_config->get("bake_num_bounces");
        if (v_bounces.get_type() == Variant::INT)
            num_bounces = static_cast<int>(v_bounces);
        Variant v_order = bake_config->get("bake_ambisonics_order");
        if (v_order.get_type() == Variant::INT)
            ambisonics_order = resonance::clamp_bake_ambisonics_order(static_cast<int>(v_order));
    }
    h = hash_murmur3_one_32(static_cast<uint32_t>(refl_type), h);
    h = hash_murmur3_one_32(static_cast<uint32_t>(num_rays), h);
    h = hash_murmur3_one_32(static_cast<uint32_t>(num_bounces), h);
    h = hash_murmur3_one_32(static_cast<uint32_t>(ambisonics_order), h);

    Array excl = collect_exclusion_boxes();
    h = hash_murmur3_one_32(static_cast<uint32_t>(excl.size()), h);
    for (int i = 0; i < excl.size(); i++) {
        if (excl[i].get_type() != Variant::DICTIONARY) {
            continue;
        }
        Dictionary d = excl[i];
        Variant vx = d.get("xform", Variant());
        Variant vs = d.get("size", Variant());
        if (vx.get_type() == Variant::TRANSFORM3D) {
            Transform3D et = vx;
            h = hash_murmur3_one_float(et.origin.x, h);
            h = hash_murmur3_one_float(et.origin.y, h);
            h = hash_murmur3_one_float(et.origin.z, h);
            h = hash_murmur3_one_float(et.basis.rows[0].x, h);
            h = hash_murmur3_one_float(et.basis.rows[0].y, h);
            h = hash_murmur3_one_float(et.basis.rows[0].z, h);
            h = hash_murmur3_one_float(et.basis.rows[1].x, h);
            h = hash_murmur3_one_float(et.basis.rows[1].y, h);
            h = hash_murmur3_one_float(et.basis.rows[1].z, h);
            h = hash_murmur3_one_float(et.basis.rows[2].x, h);
            h = hash_murmur3_one_float(et.basis.rows[2].y, h);
            h = hash_murmur3_one_float(et.basis.rows[2].z, h);
        }
        if (vs.get_type() == Variant::VECTOR3) {
            Vector3 es = vs;
            h = hash_murmur3_one_float(es.x, h);
            h = hash_murmur3_one_float(es.y, h);
            h = hash_murmur3_one_float(es.z, h);
        }
    }

    return h;
}

bool ResonanceProbeVolume::_compute_is_probe_dirty() const {
    if (!probe_data.is_valid() || probe_data->get_size() <= 0)
        return true;
    uint32_t stored = static_cast<uint32_t>(probe_data->get_bake_params_hash() & 0xFFFFFFFF);
    // Probe data without a recorded params hash predates incremental detection. We are pre-release
    // (no shipped users), so treat that as dirty and force a re-bake instead of silently trusting it.
    if (stored == 0)
        return true;
    return stored != _get_bake_params_hash();
}

static Node* find_resonance_config_in_tree(Node* n) {
    if (!n)
        return nullptr;
    if (n->has_method(StringName("get_config_dict")))
        return n;
    for (int i = 0; i < n->get_child_count(); i++) {
        Node* found = find_resonance_config_in_tree(n->get_child(i));
        if (found)
            return found;
    }
    return nullptr;
}

bool ResonanceProbeVolume::_has_valid_resonance_config() const {
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (srv && srv->is_initialized())
        return true;
    SceneTree* tree = get_tree();
    if (!tree)
        return false;
    Node* config_node = nullptr;
    TypedArray<Node> nodes = tree->get_nodes_in_group(StringName("resonance_runtime"));
    if (!nodes.is_empty()) {
        config_node = Object::cast_to<Node>(nodes[0]);
    }
    Node* root = nullptr;
    if (!config_node) {
        root = tree->get_edited_scene_root();
        if (!root) {
            Node* n = const_cast<ResonanceProbeVolume*>(this);
            while (n && n->get_parent())
                n = n->get_parent();
            root = n;
        }
        if (root)
            config_node = find_resonance_config_in_tree(root);
    }
    if (!config_node)
        return false;
    if (!config_node->has_method(StringName("get_config_dict")))
        return false;
    Array empty_args;
    Variant cfg_var = config_node->callv(StringName("get_config_dict"), empty_args);
    if (cfg_var.get_type() != Variant::DICTIONARY)
        return false;
    Dictionary cfg = cfg_var.operator Dictionary();
    return !cfg.is_empty() && cfg.has("reflection_type");
}

void ResonanceProbeVolume::set_viz_visible(bool p_visible) {
    viz_visible = p_visible;
    if (viz_instance) {
        viz_instance->set_visible(p_visible);
    }
    if (!p_visible && viz_multimesh.is_valid()) {
        viz_multimesh->set_instance_count(0);
    }
    if (p_visible)
        _queue_update();
}

bool ResonanceProbeVolume::is_viz_visible() const {
    return viz_visible;
}

void ResonanceProbeVolume::_update_visuals() {
    if (!viz_visible)
        return;

    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!srv || !srv->is_initialized())
        return;

    if (Engine::get_singleton() && Engine::get_singleton()->is_editor_hint()) {
        _ensure_viz_instance();
        // Update viz_color_state from probe dirty check when not overridden by runtime config mismatch (red).
        if (viz_color_state != 2) {
            viz_color_state = _compute_is_probe_dirty() ? 0 : 1;
        }
    }
    if (!viz_multimesh.is_valid())
        return;

    Transform3D volume_transform = get_global_transform();
    Transform3D to_local_xform = volume_transform.affine_inverse();
    Vector3 extents = region_size * 0.5f;

    PackedVector3Array points;
    if (probe_data.is_valid() && !probe_data->get_probe_positions().is_empty()) {
        points = probe_data->get_probe_positions();
    }
    if (points.is_empty() && generation_type == GEN_UNIFORM_FLOOR) {
        points = srv->generate_probes_scene_aware(volume_transform, extents, spacing, generation_type, height_above_floor);
    }
    if (points.is_empty()) {
        points = srv->generate_manual_grid(volume_transform, extents, spacing, generation_type, height_above_floor);
    }

    points = resonance::filter_points_outside_exclusion_boxes(points, collect_exclusion_boxes());

    if (points.is_empty()) {
        viz_multimesh->set_instance_count(0);
        return;
    }

    viz_multimesh->set_instance_count(static_cast<int32_t>(points.size()));

    Color probe_color;
    if (viz_color_state == 2)
        probe_color = Color(resonance::kProbeVizColorRedR, resonance::kProbeVizColorRedG, resonance::kProbeVizColorRedB, resonance::kProbeVizColorRedA);
    else if (viz_color_state == 1)
        probe_color = Color(resonance::kProbeVizColorBlueR, resonance::kProbeVizColorBlueG, resonance::kProbeVizColorBlueB, resonance::kProbeVizColorBlueA);
    else
        probe_color = Color(resonance::kProbeVizColorGrayR, resonance::kProbeVizColorGrayG, resonance::kProbeVizColorGrayB, resonance::kProbeVizColorGrayA);

    float scale = viz_probe_scale <= 0.0f ? 1.0f : viz_probe_scale;
    for (int i = 0; i < points.size(); i++) {
        Transform3D t;
        t.origin = to_local_xform.xform(points[i]);
        t.basis = t.basis.scaled(Vector3(scale, scale, scale));
        viz_multimesh->set_instance_transform(i, t);
        viz_multimesh->set_instance_color(i, probe_color);
    }

    if (viz_instance) {
        AABB aabb;
        aabb.position = -extents;
        aabb.size = region_size;
        viz_instance->set_custom_aabb(aabb);
    }

    viz_retry_timer = 0.0; // Reset so next 0-instance cycle starts fresh
}

PackedVector3Array ResonanceProbeVolume::generate_probes_on_floor_raycast() const {
    PackedVector3Array points;
    Ref<World3D> world = get_world_3d();
    if (!world.is_valid())
        return points;
    PhysicsDirectSpaceState3D* space = world->get_direct_space_state();
    if (!space)
        return points;

    Transform3D volume_transform = get_global_transform();
    Vector3 extents = region_size * 0.5f;
    Vector3 size = region_size;
    float plane_y = -extents.y + height_above_floor;
    int count_x = (int)std::floor(size.x / spacing);
    int count_z = (int)std::floor(size.z / spacing);
    if (count_x <= 0)
        count_x = 1;
    if (count_z <= 0)
        count_z = 1;
    float offset_x = (size.x < spacing) ? extents.x : spacing * 0.5f;
    float offset_z = (size.z < spacing) ? extents.z : spacing * 0.5f;

    const float ray_down = resonance::kProbeFloorRaycastDepth;
    int hit_count = 0;
    for (int ix = 0; ix < count_x; ix++) {
        for (int iz = 0; iz < count_z; iz++) {
            Vector3 local_pos(-extents.x + (static_cast<float>(ix) * spacing) + offset_x, plane_y,
                              -extents.z + (static_cast<float>(iz) * spacing) + offset_z);
            Vector3 from = volume_transform.xform(local_pos);
            Vector3 to = from + Vector3(0, -ray_down, 0);
            Ref<PhysicsRayQueryParameters3D> query = PhysicsRayQueryParameters3D::create(from, to);
            Dictionary result = space->intersect_ray(query);
            // Misses return {}. Dictionary.get("position", Vector3()) would still be VECTOR3
            // (default origin) and plant every miss at world (0, height, 0).
            if (result.is_empty())
                continue;
            Variant pos_var = result.get("position", Variant());
            if (pos_var.get_type() != Variant::VECTOR3)
                continue;
            Vector3 hit_pos = pos_var;
            points.push_back(hit_pos + Vector3(0, height_above_floor, 0));
            hit_count++;
        }
    }
    if (Engine::get_singleton() && Engine::get_singleton()->is_editor_hint() && hit_count > 0) {
        UtilityFunctions::print_rich("[color=cyan]Nexus Resonance:[/color] Uniform Floor raycast placed " + String::num(hit_count) + "/" + String::num((int)points.size()) + " probes on collision geometry.");
    }
    return points;
}

void ResonanceProbeVolume::_prepare_and_execute_bake(const PackedVector3Array* p_precomputed_points) {
    if (!_has_valid_resonance_config()) {
        UtilityFunctions::push_error("Nexus Resonance: ResonanceProbeVolume requires a ResonanceRuntime node with a valid ResonanceRuntimeConfig in the scene.");
        return;
    }
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!srv || !srv->is_initialized()) {
        UtilityFunctions::push_error("ResonanceProbeVolume: Resonance Server not initialized!");
        return;
    }

    if (probe_data.is_null()) {
        probe_data.instantiate();
        set_probe_data(probe_data);
    }

    if (Engine::get_singleton() && Engine::get_singleton()->is_editor_hint()) {
        String scene_name = "unsaved";
        String node_name = get_name().to_lower().replace(" ", "_");
        SceneTree* tree = get_tree();
        if (tree) {
            Node* root = tree->get_edited_scene_root();
            if (root) {
                String scene_path = root->get_scene_file_path();
                if (!scene_path.is_empty()) {
                    scene_name = scene_path.get_file().get_basename();
                }
            }
        }
        ProjectSettings* ps = ProjectSettings::get_singleton();
        const String base_dir = resonance_bake_batches_dir_from_settings();
        const String ext = resonance_probe_data_save_extension_from_settings();
        String path = base_dir + scene_name + String("_") + node_name + String("_batch.") + ext;
        String dir = path.get_base_dir();
        if (!dir.is_empty() && ps) {
            String abs_dir = ps->globalize_path(dir);
            DirAccess::make_dir_recursive_absolute(abs_dir);
        }
        probe_data->take_over_path(path);
        probe_data->emit_changed();
    }

    Transform3D volume_transform = get_global_transform();
    Vector3 extents = region_size * 0.5f;
    Array exclusion_boxes = collect_exclusion_boxes();

    bool success = false;
    if (p_precomputed_points && !p_precomputed_points->is_empty()) {
        PackedVector3Array filtered =
            resonance::filter_points_outside_exclusion_boxes(*p_precomputed_points, exclusion_boxes);
        if (!filtered.is_empty()) {
            success = srv->bake_manual_grid(filtered, probe_data);
            if (success && Engine::get_singleton() && Engine::get_singleton()->is_editor_hint()) {
                UtilityFunctions::print_rich("[color=cyan]Nexus Resonance:[/color] Uniform Floor used geometry raycast. Probes placed on floor. (Requires CollisionShape3D on floor geometry.)");
            }
        }
    }
    if (!success) {
        success = srv->bake_probes_for_volume(volume_transform, extents, spacing, (int)generation_type,
                                              height_above_floor, probe_data, exclusion_boxes);
    }

    if (!success) {
        UtilityFunctions::push_error("Nexus Resonance: Bake failed. Previous probe data kept. Check ResonanceGeometry / ResonanceStaticScene.");
    } else {
        probe_data->set_bake_params_hash(static_cast<int64_t>(_get_bake_params_hash()));
        if (viz_visible)
            _update_visuals();
        _release_probe_batch_if_live();
        _store_probe_batch_handle(srv->load_probe_batch(probe_data));
    }
}

// Native bake entry points (bake_probes, bake_probes_with_floor_points) are DEPRECATED.
// They run only the reflection layer and skip pathing, static-source/listener, automatic
// stale-asset re-export, undo backups, and the static-scene hash bookkeeping. Everything they do is
// a strict subset of `ResonanceBakeRunner.run_bake([volume])`. Scheduled for removal in 1.0.
void ResonanceProbeVolume::_warn_native_bake_deprecated() const {
    UtilityFunctions::push_warning(
        "Nexus Resonance: ResonanceProbeVolume.bake_probes() / bake_probes_with_floor_points() "
        "are deprecated and scheduled for removal in 1.0. Use ResonanceBakeRunner.run_bake([volume]) "
        "instead - it covers the same reflection bake plus pathing, static-source / static-listener "
        "passes, automatic re-export of stale ResonanceStaticScene assets, undo backup, and full "
        "incremental-rebake bookkeeping. The native API only updates the reflection layer and the "
        "inspector will report 'Outdated' afterwards.");
}

void ResonanceProbeVolume::bake_probes_with_floor_points(const PackedVector3Array& p_points) {
    _warn_native_bake_deprecated();
    _prepare_and_execute_bake(!p_points.is_empty() ? &p_points : nullptr);
}

void ResonanceProbeVolume::bake_probes() {
    _warn_native_bake_deprecated();
    PackedVector3Array raycast_points;
    if (generation_type == GEN_UNIFORM_FLOOR) {
        raycast_points = generate_probes_on_floor_raycast();
    }
    _prepare_and_execute_bake(!raycast_points.is_empty() ? &raycast_points : nullptr);
}

void ResonanceProbeVolume::set_probe_data(const Ref<ResonanceProbeData>& p_data) {
    if (probe_data == p_data)
        return;
    probe_data = p_data;
    _queue_update();
    // Play-mode hot-swap: assigning a different ResonanceProbeData while the volume is live must
    // re-register the IPL probe batch. Otherwise the stale `probe_batch_handle` from the registry
    // keeps serving the previously baked probes (parametric/pathing/hybrid all stay on old data).
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint())
        return;
    if (!is_inside_tree())
        return;
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!srv || !srv->is_initialized())
        return;
    reload_probe_batch();
}
Ref<ResonanceProbeData> ResonanceProbeVolume::get_probe_data() const { return probe_data; }

void ResonanceProbeVolume::set_scan_targets(const Array& p_targets) {
    scan_targets = p_targets;
}
Array ResonanceProbeVolume::get_scan_targets() const {
    return scan_targets;
}
void ResonanceProbeVolume::set_bake_sources(const Array& p_sources) {
    bake_sources = p_sources;
}
Array ResonanceProbeVolume::get_bake_sources() const {
    return bake_sources;
}
void ResonanceProbeVolume::set_bake_listeners(const Array& p_listeners) {
    bake_listeners = p_listeners;
}
Array ResonanceProbeVolume::get_bake_listeners() const {
    return bake_listeners;
}
void ResonanceProbeVolume::set_bake_influence_radius(float p_radius) {
    bake_influence_radius = std::max(resonance::kProbeBakeInfluenceRadiusMin, p_radius);
}
float ResonanceProbeVolume::get_bake_influence_radius() const {
    return bake_influence_radius;
}

void ResonanceProbeVolume::add_bake_source(const Variant& p_source) {
    NodePath path = bake_target_path_from_variant(this, p_source);
    if (path.is_empty() || array_has_nodepath(bake_sources, path)) {
        return;
    }
    Array next = bake_sources;
    next.push_back(path);
    set_bake_sources(next);
}

void ResonanceProbeVolume::remove_bake_source(const Variant& p_source) {
    NodePath path = bake_target_path_from_variant(this, p_source);
    if (path.is_empty()) {
        return;
    }
    Array next;
    for (int i = 0; i < bake_sources.size(); i++) {
        if (NodePath(bake_sources[i]) != path) {
            next.push_back(bake_sources[i]);
        }
    }
    set_bake_sources(next);
}

void ResonanceProbeVolume::add_bake_listener(const Variant& p_listener) {
    NodePath path = bake_target_path_from_variant(this, p_listener);
    if (path.is_empty() || array_has_nodepath(bake_listeners, path)) {
        return;
    }
    Array next = bake_listeners;
    next.push_back(path);
    set_bake_listeners(next);
}

void ResonanceProbeVolume::remove_bake_listener(const Variant& p_listener) {
    NodePath path = bake_target_path_from_variant(this, p_listener);
    if (path.is_empty()) {
        return;
    }
    Array next;
    for (int i = 0; i < bake_listeners.size(); i++) {
        if (NodePath(bake_listeners[i]) != path) {
            next.push_back(bake_listeners[i]);
        }
    }
    set_bake_listeners(next);
}

void ResonanceProbeVolume::set_bake_config(const Ref<Resource>& p_config) {
    bake_config = p_config;
    _queue_update();
}
Ref<Resource> ResonanceProbeVolume::get_bake_config() const { return bake_config; }

void ResonanceProbeVolume::set_region_size(Vector3 p_size) {
    region_size.x = MAX(p_size.x, resonance::kProbeRegionSizeMin);
    region_size.y = MAX(p_size.y, resonance::kProbeRegionSizeMin);
    region_size.z = MAX(p_size.z, resonance::kProbeRegionSizeMin);
    _queue_update();
}
Vector3 ResonanceProbeVolume::get_region_size() const { return region_size; }

void ResonanceProbeVolume::set_spacing(float p_spacing) {
    spacing = CLAMP(p_spacing, resonance::kProbeSpacingMin, resonance::kProbeSpacingMax);
    _queue_update();
}
float ResonanceProbeVolume::get_spacing() const { return spacing; }

void ResonanceProbeVolume::set_generation_type(ProbeGenerationType p_type) {
    generation_type = p_type;
    notify_property_list_changed();
    _queue_update();
}
ResonanceProbeVolume::ProbeGenerationType ResonanceProbeVolume::get_generation_type() const { return generation_type; }

void ResonanceProbeVolume::set_height_above_floor(float p_height) {
    height_above_floor = p_height;
    _queue_update();
}
float ResonanceProbeVolume::get_height_above_floor() const { return height_above_floor; }

void ResonanceProbeVolume::set_viz_probe_scale(float p_scale) {
    viz_probe_scale = CLAMP(p_scale, resonance::kProbeVizScaleMin, resonance::kProbeVizScaleMax);
    _queue_update();
}
float ResonanceProbeVolume::get_viz_probe_scale() const { return viz_probe_scale; }

void ResonanceProbeVolume::set_viz_color_state(int p_state) {
    viz_color_state = CLAMP(p_state, resonance::kProbeVizColorStateMin, resonance::kProbeVizColorStateMax);
    _queue_update();
}
int ResonanceProbeVolume::get_viz_color_state() const { return viz_color_state; }

void ResonanceProbeVolume::notify_runtime_config_changed(int p_runtime_refl, bool p_runtime_pathing) {
    if (!Engine::get_singleton() || !Engine::get_singleton()->is_editor_hint())
        return;

    int baked_refl = probe_data.is_valid() ? probe_data->get_baked_reflection_type() : -1;
    bool has_data = probe_data.is_valid() && probe_data->get_size() > 0;

    bool wants_path = false;
    bool want_ss = false;
    bool want_sl = false;
    if (bake_config.is_valid()) {
        wants_path = bake_config->get("pathing_enabled").booleanize();
        want_ss = bake_config->get("static_source_enabled").booleanize();
        want_sl = bake_config->get("static_listener_enabled").booleanize();
    }

    bool has_pathing = probe_data.is_valid() && probe_data->get_pathing_params_hash() > 0;
    int64_t pd_hash = probe_data.is_valid() ? probe_data->get_bake_params_hash() : 0;
    int64_t vol_hash = static_cast<int64_t>(_get_bake_params_hash());
    bool has_ss = probe_data.is_valid() && probe_data->get_static_source_params_hash() > 0;
    bool has_sl = probe_data.is_valid() && probe_data->get_static_listener_params_hash() > 0;

    bool config_compatible = (baked_refl == p_runtime_refl) ||
                             (baked_refl == resonance::kBakedReflectionHybrid && p_runtime_refl >= resonance::kReflectionConvolution && p_runtime_refl <= resonance::kReflectionHybrid) ||
                             (p_runtime_refl == resonance::kReflectionHybrid && baked_refl >= resonance::kBakedReflectionConvolution && baked_refl <= resonance::kBakedReflectionParametric) ||
                             (baked_refl == -1);
    bool reflection_ok = !has_data || (pd_hash == vol_hash && config_compatible);
    bool pathing_ok = !p_runtime_pathing || !wants_path || has_pathing;
    bool static_ok = (!want_ss || has_ss) && (!want_sl || has_sl);

    int out_state = 0;
    if (!config_compatible && has_data)
        out_state = 2;
    else if (reflection_ok && pathing_ok && static_ok)
        out_state = 1;

    set_viz_color_state(out_state);
    if (viz_visible && viz_instance)
        _update_visuals();
}

int64_t ResonanceProbeVolume::get_bake_params_hash() const {
    return static_cast<int64_t>(_get_bake_params_hash());
}

void ResonanceProbeVolume::_validate_property(PropertyInfo& p_property) const {
    if (p_property.name == StringName("height_above_floor") && generation_type != GEN_UNIFORM_FLOOR) {
        p_property.usage |= PROPERTY_USAGE_READ_ONLY;
    }
}

void ResonanceProbeVolume::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_bake_config", "p_config"), &ResonanceProbeVolume::set_bake_config);
    ClassDB::bind_method(D_METHOD("get_bake_config"), &ResonanceProbeVolume::get_bake_config);
    ClassDB::bind_method(D_METHOD("set_scan_targets", "p_targets"), &ResonanceProbeVolume::set_scan_targets);
    ClassDB::bind_method(D_METHOD("get_scan_targets"), &ResonanceProbeVolume::get_scan_targets);
    ClassDB::bind_method(D_METHOD("set_bake_sources", "p_sources"), &ResonanceProbeVolume::set_bake_sources);
    ClassDB::bind_method(D_METHOD("get_bake_sources"), &ResonanceProbeVolume::get_bake_sources);
    ClassDB::bind_method(D_METHOD("set_bake_listeners", "p_listeners"), &ResonanceProbeVolume::set_bake_listeners);
    ClassDB::bind_method(D_METHOD("get_bake_listeners"), &ResonanceProbeVolume::get_bake_listeners);
    ClassDB::bind_method(D_METHOD("set_bake_influence_radius", "p_radius"), &ResonanceProbeVolume::set_bake_influence_radius);
    ClassDB::bind_method(D_METHOD("get_bake_influence_radius"), &ResonanceProbeVolume::get_bake_influence_radius);
    ClassDB::bind_method(D_METHOD("add_bake_source", "source"), &ResonanceProbeVolume::add_bake_source);
    ClassDB::bind_method(D_METHOD("remove_bake_source", "source"), &ResonanceProbeVolume::remove_bake_source);
    ClassDB::bind_method(D_METHOD("add_bake_listener", "listener"), &ResonanceProbeVolume::add_bake_listener);
    ClassDB::bind_method(D_METHOD("remove_bake_listener", "listener"), &ResonanceProbeVolume::remove_bake_listener);
    ClassDB::bind_method(D_METHOD("set_probe_data", "p_data"), &ResonanceProbeVolume::set_probe_data);
    ClassDB::bind_method(D_METHOD("get_probe_data"), &ResonanceProbeVolume::get_probe_data);
    ClassDB::bind_method(D_METHOD("ensure_default_resources"), &ResonanceProbeVolume::ensure_default_resources);
    ClassDB::bind_method(D_METHOD("collect_exclusion_boxes"), &ResonanceProbeVolume::collect_exclusion_boxes);
    ClassDB::bind_method(D_METHOD("notify_exclusion_changed"), &ResonanceProbeVolume::notify_exclusion_changed);
    ClassDB::bind_method(D_METHOD("set_region_size", "p_size"), &ResonanceProbeVolume::set_region_size);
    ClassDB::bind_method(D_METHOD("get_region_size"), &ResonanceProbeVolume::get_region_size);
    ClassDB::bind_method(D_METHOD("set_generation_type", "p_type"), &ResonanceProbeVolume::set_generation_type);
    ClassDB::bind_method(D_METHOD("get_generation_type"), &ResonanceProbeVolume::get_generation_type);
    ClassDB::bind_method(D_METHOD("set_spacing", "p_spacing"), &ResonanceProbeVolume::set_spacing);
    ClassDB::bind_method(D_METHOD("get_spacing"), &ResonanceProbeVolume::get_spacing);
    ClassDB::bind_method(D_METHOD("set_height_above_floor", "p_height"), &ResonanceProbeVolume::set_height_above_floor);
    ClassDB::bind_method(D_METHOD("get_height_above_floor"), &ResonanceProbeVolume::get_height_above_floor);
    ClassDB::bind_method(D_METHOD("bake_probes"), &ResonanceProbeVolume::bake_probes);
    ClassDB::bind_method(D_METHOD("bake_probes_with_floor_points", "points"), &ResonanceProbeVolume::bake_probes_with_floor_points);
    ClassDB::bind_method(D_METHOD("generate_probes_on_floor_raycast"), &ResonanceProbeVolume::generate_probes_on_floor_raycast);
    ClassDB::bind_method(D_METHOD("reload_probe_batch"), &ResonanceProbeVolume::reload_probe_batch);
    ClassDB::bind_method(D_METHOD("release_probe_batch"), &ResonanceProbeVolume::release_probe_batch);
    ClassDB::bind_method(D_METHOD("get_probe_batch_handle"), &ResonanceProbeVolume::get_probe_batch_handle);
    ClassDB::bind_method(D_METHOD("set_viz_visible", "p_visible"), &ResonanceProbeVolume::set_viz_visible);
    ClassDB::bind_method(D_METHOD("is_viz_visible"), &ResonanceProbeVolume::is_viz_visible);
    ClassDB::bind_method(D_METHOD("set_viz_probe_scale", "p_scale"), &ResonanceProbeVolume::set_viz_probe_scale);
    ClassDB::bind_method(D_METHOD("get_viz_probe_scale"), &ResonanceProbeVolume::get_viz_probe_scale);
    ClassDB::bind_method(D_METHOD("set_viz_color_state", "p_state"), &ResonanceProbeVolume::set_viz_color_state);
    ClassDB::bind_method(D_METHOD("get_viz_color_state"), &ResonanceProbeVolume::get_viz_color_state);
    ClassDB::bind_method(D_METHOD("notify_runtime_config_changed", "p_runtime_refl", "p_runtime_pathing"), &ResonanceProbeVolume::notify_runtime_config_changed);
    ClassDB::bind_method(D_METHOD("get_bake_params_hash"), &ResonanceProbeVolume::get_bake_params_hash);
    ClassDB::bind_method(D_METHOD("_update_visuals"), &ResonanceProbeVolume::_update_visuals);
    ClassDB::bind_method(D_METHOD("_runtime_load_probe_batch"), &ResonanceProbeVolume::_runtime_load_probe_batch);
    ClassDB::bind_method(D_METHOD("_reload_probe_batch_after_reinit"), &ResonanceProbeVolume::_reload_probe_batch_after_reinit);
    ClassDB::bind_method(D_METHOD("_check_probe_data_loaded"), &ResonanceProbeVolume::_check_probe_data_loaded);
    ClassDB::bind_method(D_METHOD("set_headless_baking_mode", "p_mode"), &ResonanceProbeVolume::set_headless_baking_mode);
    ClassDB::bind_method(D_METHOD("is_headless_baking_mode"), &ResonanceProbeVolume::is_headless_baking_mode);

    ADD_GROUP("Data", "");
    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "probe_data", PROPERTY_HINT_RESOURCE_TYPE, "ResonanceProbeData"), "set_probe_data", "get_probe_data");
    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "bake_config", PROPERTY_HINT_RESOURCE_TYPE, "ResonanceBakeConfig"), "set_bake_config", "get_bake_config");
    ADD_GROUP("Bake Targets", "");
    ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "scan_targets", PROPERTY_HINT_ARRAY_TYPE, "NodePath"), "set_scan_targets", "get_scan_targets");
    ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "bake_sources", PROPERTY_HINT_ARRAY_TYPE, "NodePath"), "set_bake_sources", "get_bake_sources");
    ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "bake_listeners", PROPERTY_HINT_ARRAY_TYPE, "NodePath"), "set_bake_listeners", "get_bake_listeners");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "bake_influence_radius", PROPERTY_HINT_RANGE, "1,50000,1"), "set_bake_influence_radius", "get_bake_influence_radius");
    ADD_GROUP("Volume", "");
    ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "region_size"), "set_region_size", "get_region_size");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_probes"), "set_viz_visible", "is_viz_visible");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "viz_probe_scale", PROPERTY_HINT_RANGE, "0.1, 3.0, 0.1"), "set_viz_probe_scale", "get_viz_probe_scale");
    ADD_GROUP("Generation", "");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "generation_type", PROPERTY_HINT_ENUM, "Centroid,Uniform Floor,Volume"), "set_generation_type", "get_generation_type");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spacing", PROPERTY_HINT_RANGE, "0.5, 20.0"), "set_spacing", "get_spacing");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "height_above_floor", PROPERTY_HINT_RANGE, "0.1, 5.0"), "set_height_above_floor", "get_height_above_floor");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "headless_baking_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_headless_baking_mode", "is_headless_baking_mode");

    BIND_ENUM_CONSTANT(GEN_CENTROID);
    BIND_ENUM_CONSTANT(GEN_UNIFORM_FLOOR);
    BIND_ENUM_CONSTANT(GEN_VOLUME);
}
