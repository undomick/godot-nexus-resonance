#include "resonance_audio_effect.h"
#include "resonance_geometry_asset.h"
#include "resonance_probe_data.h"
#include "resonance_probe_volume.h"
#include "resonance_reflection_type_policy.h"
#include "resonance_runtime.h"
#include "resonance_server.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace {
const char* BAKE_CONFIG_PATH = "res://addons/nexus_resonance/scripts/resonance_bake_config.gd";
const char* FMOD_BRIDGE_PATH = "res://addons/nexus_resonance/scripts/resonance_fmod_bridge.gd";
const char* CODA_BRIDGE_PATH = "res://addons/nexus_resonance/scripts/resonance_coda_bridge.gd";

void collect_static_scenes(Node* node, TypedArray<Node>& out) {
    if (node == nullptr) {
        return;
    }
    if (node->is_class("ResonanceStaticScene")) {
        out.append(node);
    }
    TypedArray<Node> children = node->get_children();
    for (int i = 0; i < children.size(); i++) {
        collect_static_scenes(Object::cast_to<Node>(children[i]), out);
    }
}

String join_packed_names(const PackedStringArray& names) {
    String out;
    for (int i = 0; i < names.size(); i++) {
        if (i > 0) {
            out += ", ";
        }
        out += names[i];
    }
    return out;
}

int runtime_global_bake_ambisonic_order(const Ref<Resource>& runtime) {
    if (runtime.is_null()) {
        return resonance::kBakeDefaultAmbisonicsOrder;
    }
    Variant bao = runtime->get("bake_ambisonic_order");
    if (bao.get_type() == Variant::INT || bao.get_type() == Variant::FLOAT) {
        return resonance::clamp_bake_ambisonics_order((int)bao);
    }
    return resonance::clamp_bake_ambisonics_order((int)runtime->get("ambisonic_order"));
}

} // namespace

Dictionary ResonanceRuntime::get_config_dict() const {
    Dictionary cfg;
    if (runtime.is_valid() && runtime->has_method("get_config")) {
        cfg = runtime->call("get_config");
    }
    cfg["debug_occlusion"] = player_overlay_visible;
    cfg["debug_reflections"] = player_overlay_visible;
    cfg["context_simd_level"] = context_simd_level;
    cfg["context_validation"] = context_validation;
    return cfg;
}

// Bake params from the first probe volume with a bake_config, else defaults. Set before init so pathing visibility
// params are available. bake_reflection_type always follows this runtime's reflection_type (TAN -> Convolution).
Dictionary ResonanceRuntime::get_bake_params_for_runtime() {
    Dictionary params;
    if (is_inside_tree()) {
        if (SceneTree* tree = get_tree()) {
            TypedArray<Node> volumes = tree->get_nodes_in_group("resonance_probe_volume");
            for (int i = 0; i < volumes.size(); i++) {
                Object* vol = Object::cast_to<Object>(volumes[i]);
                if (vol == nullptr) {
                    continue;
                }
                Object* bc = Object::cast_to<Object>(vol->get("bake_config"));
                if (bc && bc->has_method("get_bake_params")) {
                    params = bc->call("get_bake_params");
                    break;
                }
            }
        }
    }
    if (params.is_empty()) {
        Ref<Script> bake_script = ResourceLoader::get_singleton()->load(BAKE_CONFIG_PATH);
        if (bake_script.is_valid()) {
            // Keep the Variant alive: create_default() returns a RefCounted; a temporary would free it.
            Variant def_variant = bake_script->call("create_default");
            Object* def = Object::cast_to<Object>(def_variant);
            if (def && def->has_method("get_bake_params")) {
                params = def->call("get_bake_params");
            }
        }
    }
    if (runtime.is_valid()) {
        const int rt_refl = (int)runtime->get("reflection_type");
        params["bake_reflection_type"] = resonance::bake_reflection_type_from_runtime(rt_refl);
        params["bake_ambisonics_order"] = runtime_global_bake_ambisonic_order(runtime);
    }
    return params;
}

// Loads every ResonanceStaticScene under tree_root into the server (one simulation_mutex batch).
void ResonanceRuntime::reload_static_scenes_from_tree(Node* tree_root) {
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (srv == nullptr || !srv->is_initialized() || tree_root == nullptr) {
        return;
    }
    TypedArray<Node> static_scenes;
    collect_static_scenes(tree_root, static_scenes);
    // Full replace: clear then re-add each pack keyed by Node ObjectID.
    srv->clear_static_scenes();
    for (int i = 0; i < static_scenes.size(); i++) {
        Node* ss_node = Object::cast_to<Node>(static_scenes[i]);
        Object* ss = Object::cast_to<Object>(static_scenes[i]);
        if (ss == nullptr || ss_node == nullptr) {
            continue;
        }
        Ref<ResonanceGeometryAsset> asset(Object::cast_to<ResonanceGeometryAsset>((Object*)ss->get("static_scene_asset")));
        bool valid = asset.is_valid();
        if (valid && ss->has_method("has_valid_asset")) {
            valid = (bool)ss->call("has_valid_asset");
        }
        if (!valid) {
            continue;
        }
        Node3D* n3 = Object::cast_to<Node3D>(ss_node);
        srv->add_or_replace_static_pack(ss_node->get_instance_id(), asset, n3 ? n3->get_global_transform() : Transform3D());
    }
}

void ResonanceRuntime::apply_primary_handoff_without_reinit() {
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (srv == nullptr || !srv->is_initialized()) {
        return;
    }
    reset_viewport_sync_cache();
    if (fmod_bridge_enabled && Object::cast_to<Object>(fmod_bridge) == nullptr) {
        init_fmod_bridge();
    }
    if (coda_bridge_enabled && Object::cast_to<Object>(coda_bridge) == nullptr) {
        init_coda_bridge();
    }
    if (is_inside_tree()) {
        if (SceneTree* tree = get_tree()) {
            // Static packs only. Do not deferred refresh_geometry: Geometry/_ready retry already
            // registers once; a second rebuild breaks Embree dynamic occlusion (same as F3 path).
            reload_static_scenes_from_tree(tree->get_root());
        }
    }
    apply_debug_flags();
    apply_runtime_live_config();
    call_deferred("deferred_reset_spatial_audio_warmup_passes");
    // Same as fresh init: probe volumes may still be loading; warn after settle.
    call_deferred("warn_probe_volume_runtime_mismatches");
    sync_physics_process_for_custom_tracer();
}

void ResonanceRuntime::initialize_server() {
    // In editor, the probe toolbar inits when needed for baking. Avoid Steam Audio init on scene load.
    if (editor_hint()) {
        return;
    }
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (srv == nullptr) {
        UtilityFunctions::push_error("Nexus Resonance: GDExtension not loaded.");
        return;
    }
    Dictionary cfg = get_config_dict();
    if (cfg.is_empty()) {
        UtilityFunctions::push_error("Nexus Resonance: Failed to build config.");
        return;
    }
    if (srv->is_initialized()) {
        // Scene handoff: keep the live engine; only primary owns tick after claim in _ready.
        apply_primary_handoff_without_reinit();
        return;
    }
    // Set bake params before init so pathing visibility params (from bake_config) are available.
    srv->set_bake_params(get_bake_params_for_runtime());
    srv->init_audio_engine(cfg);
    reset_viewport_sync_cache();
    if (fmod_bridge_enabled) {
        init_fmod_bridge();
    }
    if (coda_bridge_enabled) {
        init_coda_bridge();
    }
    if (is_inside_tree()) {
        // Static packs only. Geometry nodes register via _ready retry after server init;
        // deferred refresh_geometry would double-register and break Embree occlusion.
        reload_static_scenes_from_tree(get_tree()->get_root());
    }
    apply_debug_flags();
    apply_runtime_live_config();
    // Mute spatialized output until several worker RunDirect ticks after scene/geometry settle.
    call_deferred("deferred_reset_spatial_audio_warmup_passes");
    // Probe volumes load batches in their own _ready; warn after the tree settles.
    call_deferred("warn_probe_volume_runtime_mismatches");
    sync_physics_process_for_custom_tracer();
}

// Reverb bus reported a new Godot frame size; reinit with it. Called each _process frame while the server runs.
void ResonanceRuntime::handle_pending_reinit_frame_size() {
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (srv == nullptr || !srv->is_initialized()) {
        return;
    }
    const int pending = srv->consume_pending_reinit_frame_size();
    if (pending <= 0) {
        return;
    }
    const uint64_t t_reinit = Time::get_singleton()->get_ticks_usec();
    Dictionary cfg = get_config_dict();
    cfg["audio_frame_size"] = pending;
    prepare_geometry_before_reinit();
    srv->set_bake_params(get_bake_params_for_runtime());
    srv->reinit_audio_engine(cfg);
    reset_viewport_sync_cache();
    call_deferred("reload_after_reinit");
    const uint64_t now = Time::get_singleton()->get_ticks_usec();
    main_thread_reinit_usec = now > t_reinit ? (int64_t)(now - t_reinit) : 0;
}

void ResonanceRuntime::prepare_geometry_before_reinit() {
    if (is_inside_tree()) {
        get_tree()->call_group("resonance_geometry", "discard_meshes_before_scene_release");
    }
}

void ResonanceRuntime::reload_after_reinit() {
    if (!is_inside_tree()) {
        return;
    }
    SceneTree* tree = get_tree();
    if (tree == nullptr) {
        return;
    }
    reload_static_scenes_from_tree(tree->get_root());
    // Same frame: re-register probe batches after reinit so baked/parametric/hybrid/pathing outputs are valid.
    TypedArray<Node> volumes = tree->get_nodes_in_group("resonance_probe_volume");
    for (int i = 0; i < volumes.size(); i++) {
        Object* n = Object::cast_to<Object>(volumes[i]);
        if (n && n->has_method("_reload_probe_batch_after_reinit")) {
            n->call("_reload_probe_batch_after_reinit");
        }
    }
    // Recreate IPL sources: shutdown recycles handles while players still hold the old integers.
    TypedArray<Node> players = tree->get_nodes_in_group("resonance_player");
    for (int i = 0; i < players.size(); i++) {
        Object* n = Object::cast_to<Object>(players[i]);
        if (n && n->has_method("reload_source_after_reinit")) {
            n->call("reload_source_after_reinit");
        }
    }
    // FMOD plugin retained the destroyed IPLContext; rebind before recreating emitter sources.
    TypedArray<Node> fmod_emitters = tree->get_nodes_in_group("resonance_fmod_event_emitter");
    for (int i = 0; i < fmod_emitters.size(); i++) {
        Object* n = Object::cast_to<Object>(fmod_emitters[i]);
        if (n && n->has_method("invalidate_handles_after_engine_reinit")) {
            n->call("invalidate_handles_after_engine_reinit");
        }
    }
    if (fmod_bridge_enabled) {
        if (Object* bridge = Object::cast_to<Object>(fmod_bridge)) {
            if (bridge->has_method("rebind_after_reinit")) {
                bridge->call("rebind_after_reinit");
            }
        } else {
            init_fmod_bridge();
        }
    }
    for (int i = 0; i < fmod_emitters.size(); i++) {
        Object* n = Object::cast_to<Object>(fmod_emitters[i]);
        if (n && n->has_method("reload_source_after_reinit")) {
            n->call("reload_source_after_reinit");
        }
    }
    // Coda bridge keeps source handles in GDScript; same recycle hazard as ResonancePlayer.
    if (Object* coda = Object::cast_to<Object>(coda_bridge)) {
        if (coda->has_method("reload_sources_after_reinit")) {
            coda->call("reload_sources_after_reinit");
        }
    }
    tree->call_group_flags(SceneTree::GROUP_CALL_DEFERRED, "resonance_geometry", "refresh_geometry");
    call_deferred("deferred_reset_spatial_audio_warmup_passes");
    call_deferred("warn_probe_volume_runtime_mismatches");
    ResonanceAudioEffectInstance::try_prewarm_all_live_instances();
    sync_physics_process_for_custom_tracer();
}

void ResonanceRuntime::deferred_reset_spatial_audio_warmup_passes() {
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (srv && srv->is_initialized()) {
        // Geometry 0->N already armed the gate; re-arm would clear ready and re-dirty after mesh _ready.
        if (srv->get_global_triangle_count() <= 0)
            srv->arm_spatial_audio_output_gate();
        srv->finish_cold_start_settle();
    }
}

// Rebuild static IPL scenes from the tree after runtime asset swaps. Debounced to one reload per frame.
void ResonanceRuntime::request_static_scene_reload() {
    if (!is_inside_tree()) {
        return;
    }
    if (!is_primary_runtime()) {
        SceneTree* tree = get_tree();
        if (tree == nullptr) {
            return;
        }
        TypedArray<Node> runtimes = tree->get_nodes_in_group("resonance_runtime");
        for (int i = 0; i < runtimes.size(); i++) {
            ResonanceRuntime* rt = Object::cast_to<ResonanceRuntime>(runtimes[i]);
            if (rt && rt->is_primary_runtime()) {
                rt->request_static_scene_reload();
                return;
            }
        }
        return;
    }
    if (static_scene_reload_pending) {
        return;
    }
    static_scene_reload_pending = true;
    call_deferred("perform_deferred_static_scene_reload");
}

void ResonanceRuntime::perform_deferred_static_scene_reload() {
    static_scene_reload_pending = false;
    if (!is_inside_tree() || !is_primary_runtime()) {
        return;
    }
    reload_static_scenes_from_tree(get_tree()->get_root());
}

void ResonanceRuntime::apply_debug_flags() {
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (srv == nullptr || !srv->is_initialized()) {
        return;
    }
    srv->set_debug_occlusion(player_overlay_visible);
    srv->set_debug_reflections(player_overlay_visible);
}

void ResonanceRuntime::apply_perspective_correction() {
    apply_runtime_live_config();
}

void ResonanceRuntime::apply_runtime_live_config() {
    if (!is_inside_tree() || !is_primary_runtime()) {
        return;
    }
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (srv == nullptr || !srv->is_initialized() || runtime.is_null()) {
        return;
    }
    Dictionary cfg = get_config_dict();
    if (cfg.is_empty()) {
        return;
    }
    if (srv->patch_live_runtime_config(cfg)) {
        // Pathing on requires iplSimulatorCreate with PATHING when the live simulator lacks it.
        // Current init always includes PATHING, so this reinit path is unused.
        reinit_for_config_change();
    }
}

void ResonanceRuntime::on_runtime_live_config_changed(const Variant& arg) {
    apply_runtime_live_config();
    // Gizmo / mismatch UI follows pathing on/off without treating leftover baked layers as errors.
    if (arg.get_type() == Variant::STRING_NAME || arg.get_type() == Variant::STRING) {
        const StringName prop = arg;
        if (prop == StringName("pathing_enabled")) {
            notify_volumes_runtime_config_changed();
        }
    }
}

void ResonanceRuntime::apply_runtime_routing_refresh() {
    if (!is_inside_tree() || !is_primary_runtime()) {
        return;
    }
    apply_bus_to_players();
}

void ResonanceRuntime::connect_runtime_signals() {
    if (runtime.is_null()) {
        return;
    }
    const Callable reinit_cb(this, "on_native_engine_reinit_requested");
    if (runtime->has_signal("native_engine_reinit_requested") && !runtime->is_connected("native_engine_reinit_requested", reinit_cb)) {
        runtime->connect("native_engine_reinit_requested", reinit_cb);
    }
    const Callable live_cb(this, "on_runtime_live_config_changed");
    if (runtime->has_signal("runtime_live_config_changed") && !runtime->is_connected("runtime_live_config_changed", live_cb)) {
        runtime->connect("runtime_live_config_changed", live_cb);
    }
    const Callable routing_cb(this, "on_runtime_routing_changed");
    if (runtime->has_signal("runtime_routing_changed") && !runtime->is_connected("runtime_routing_changed", routing_cb)) {
        runtime->connect("runtime_routing_changed", routing_cb);
    }
    const Callable bake_amb_cb(this, "on_bake_ambisonic_order_changed");
    if (runtime->has_signal("bake_ambisonic_order_changed") && !runtime->is_connected("bake_ambisonic_order_changed", bake_amb_cb)) {
        runtime->connect("bake_ambisonic_order_changed", bake_amb_cb);
    }
}

void ResonanceRuntime::disconnect_runtime_signals() {
    if (runtime.is_null()) {
        return;
    }
    const Callable reinit_cb(this, "on_native_engine_reinit_requested");
    if (runtime->has_signal("native_engine_reinit_requested") && runtime->is_connected("native_engine_reinit_requested", reinit_cb)) {
        runtime->disconnect("native_engine_reinit_requested", reinit_cb);
    }
    const Callable live_cb(this, "on_runtime_live_config_changed");
    if (runtime->has_signal("runtime_live_config_changed") && runtime->is_connected("runtime_live_config_changed", live_cb)) {
        runtime->disconnect("runtime_live_config_changed", live_cb);
    }
    const Callable routing_cb(this, "on_runtime_routing_changed");
    if (runtime->has_signal("runtime_routing_changed") && runtime->is_connected("runtime_routing_changed", routing_cb)) {
        runtime->disconnect("runtime_routing_changed", routing_cb);
    }
    const Callable bake_amb_cb(this, "on_bake_ambisonic_order_changed");
    if (runtime->has_signal("bake_ambisonic_order_changed") && runtime->is_connected("bake_ambisonic_order_changed", bake_amb_cb)) {
        runtime->disconnect("bake_ambisonic_order_changed", bake_amb_cb);
    }
}

void ResonanceRuntime::reinit_for_config_change() {
    if (!is_inside_tree()) {
        return;
    }
    // Editor never claims primary; still refresh gizmos / mismatch warnings.
    if (!is_primary_runtime()) {
        if (editor_hint()) {
            notify_volumes_runtime_config_changed();
        }
        return;
    }
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (srv == nullptr || !srv->is_initialized()) {
        notify_volumes_runtime_config_changed();
        return;
    }
    Dictionary cfg = get_config_dict();
    if (cfg.is_empty()) {
        return;
    }
    prepare_geometry_before_reinit();
    srv->set_bake_params(get_bake_params_for_runtime());
    srv->reinit_audio_engine(cfg);
    call_deferred("reload_after_reinit");
    notify_volumes_runtime_config_changed();
}

void ResonanceRuntime::on_native_engine_reinit_requested(const Variant&) {
    reinit_for_config_change();
}

void ResonanceRuntime::on_runtime_routing_changed(const Variant&) {
    apply_runtime_routing_refresh();
}

void ResonanceRuntime::on_bake_ambisonic_order_changed(const Variant&) {
    notify_volumes_runtime_config_changed();
}

void ResonanceRuntime::notify_volumes_runtime_config_changed() {
    if (!is_inside_tree() || runtime.is_null()) {
        return;
    }
    const int refl = (int)runtime->get("reflection_type");
    const bool pathing = (bool)runtime->get("pathing_enabled");
    const int bake_amb = runtime_global_bake_ambisonic_order(runtime);
    get_tree()->call_group_flags(SceneTree::GROUP_CALL_DEFERRED, "resonance_probe_volume",
                                 "notify_runtime_config_changed", refl, pathing, bake_amb);
    warn_probe_volume_runtime_mismatches();
}

void ResonanceRuntime::warn_probe_volume_runtime_mismatches() {
    if (!is_inside_tree() || runtime.is_null()) {
        return;
    }
    SceneTree* tree = get_tree();
    if (!tree) {
        return;
    }
    const int refl = (int)runtime->get("reflection_type");
    const bool pathing = (bool)runtime->get("pathing_enabled");
    const int global_bake_amb = runtime_global_bake_ambisonic_order(runtime);

    TypedArray<Node> volumes = tree->get_nodes_in_group("resonance_probe_volume");
    PackedStringArray refl_names;
    PackedStringArray amb_details;
    PackedStringArray path_names;

    for (int i = 0; i < volumes.size(); i++) {
        ResonanceProbeVolume* vol = Object::cast_to<ResonanceProbeVolume>(volumes[i]);
        if (vol == nullptr) {
            continue;
        }
        Ref<ResonanceProbeData> pd = vol->get_probe_data();
        if (pd.is_null() || pd->get_size() <= 0) {
            continue;
        }
        const String name = vol->get_name();
        if (!resonance::baked_reflection_type_matches_runtime(pd->get_baked_reflection_type(), refl)) {
            refl_names.push_back(name);
        }
        const int setting_amb = vol->resolved_bake_ambisonic_order(global_bake_amb);
        const int baked_amb = resonance::effective_baked_ambisonics_order(pd->get_baked_ambisonics_order());
        if (resonance::ambisonics_order_mismatches_bake_setting(pd->get_baked_ambisonics_order(), setting_amb)) {
            amb_details.push_back(name + String(" (setting ") + String::num_int64(setting_amb) + String(", baked ") +
                                  String::num_int64(baked_amb) + String(")"));
        }
        const bool has_pathing = pd->get_pathing_params_hash() > 0;
        // Runtime pathing off with leftover baked layer is intentional (CPU save); only warn when enabled without bake.
        if (pathing && !has_pathing) {
            path_names.push_back(name);
        }
    }

    if (refl_names.is_empty() && amb_details.is_empty() && path_names.is_empty()) {
        return;
    }

    String msg = "Probe Volume bake does not match settings.";
    if (!refl_names.is_empty()) {
        msg += " Reflection type mismatch (rebake required): " + join_packed_names(refl_names) + ".";
    }
    if (!amb_details.is_empty()) {
        msg += " Ambisonics bake order mismatch (rebake or match the volume setting): " +
               join_packed_names(amb_details) + ".";
    }
    if (!path_names.is_empty()) {
        msg += " Pathing enabled but no pathing layer (pathing skipped): " + join_packed_names(path_names) + ".";
    }
    UtilityFunctions::print_rich("[color=orange]Nexus Resonance:[/color] " + msg);
    UtilityFunctions::push_warning(String("Nexus Resonance: ") + msg);
}

void ResonanceRuntime::warn_restart_if_needed() {
    if (editor_hint()) {
        return;
    }
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (srv && srv->is_initialized()) {
        UtilityFunctions::print_rich(
            "[color=yellow][Nexus Resonance] Change requires game restart to take effect.[/color]");
    }
}

void ResonanceRuntime::init_fmod_bridge() {
    fmod_bridge = script_new(FMOD_BRIDGE_PATH);
    Object* bridge = Object::cast_to<Object>(fmod_bridge);
    if (bridge == nullptr) {
        return;
    }
    if (!(bool)bridge->call("init_bridge")) {
        UtilityFunctions::push_warning(
            "Nexus Resonance: FMOD bridge init failed. Ensure phonon_fmod plugin is in FMOD path.");
    }
}

void ResonanceRuntime::init_coda_bridge() {
    SceneTree* tree = get_tree();
    Node* coda = (tree && tree->get_root()) ? tree->get_root()->get_node_or_null(NodePath("Coda")) : nullptr;
    if (coda == nullptr) {
        UtilityFunctions::push_warning(
            "Nexus Resonance: coda_bridge_enabled but Coda autoload not found. Add CodaRuntime as autoload 'Coda'.");
        return;
    }
    coda_bridge = script_new(CODA_BRIDGE_PATH);
    Object* bridge = Object::cast_to<Object>(coda_bridge);
    if (bridge == nullptr) {
        return;
    }
    if (!(bool)bridge->call("init", coda, this)) {
        UtilityFunctions::push_warning("Nexus Resonance: Coda bridge init failed.");
        coda_bridge = Variant();
    }
}

Ref<RefCounted> ResonanceRuntime::get_fmod_bridge() const {
    return Ref<RefCounted>(Object::cast_to<RefCounted>((Object*)fmod_bridge));
}

Ref<RefCounted> ResonanceRuntime::get_coda_bridge() const {
    return Ref<RefCounted>(Object::cast_to<RefCounted>((Object*)coda_bridge));
}

void ResonanceRuntime::set_fmod_bridge_enabled(bool p_enabled) {
    fmod_bridge_enabled = p_enabled;
}

void ResonanceRuntime::set_coda_bridge_enabled(bool p_enabled) {
    coda_bridge_enabled = p_enabled;
}

void ResonanceRuntime::set_context_simd_level(int p_level) {
    if (context_simd_level == p_level) {
        return;
    }
    context_simd_level = p_level;
    warn_restart_if_needed();
}

void ResonanceRuntime::set_context_validation(bool p_enabled) {
    if (context_validation == p_enabled) {
        return;
    }
    context_validation = p_enabled;
    warn_restart_if_needed();
}
