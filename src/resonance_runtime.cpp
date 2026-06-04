#include "resonance_runtime.h"
#include "resonance_constants.h"
#include "resonance_key_enum_hint.h"
#include "resonance_server.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable.hpp>

using namespace godot;

namespace {
const char* CONFIG_SCRIPT_PATH = "res://addons/nexus_resonance/scripts/resonance_runtime_config.gd";
const char* BUS_SCRIPT_PATH = "res://addons/nexus_resonance/scripts/resonance_runtime_bus.gd";
const char* ACTIVATOR_SCRIPT_PATH = "res://addons/nexus_resonance/scripts/resonance_reverb_activator.gd";

PropertyInfo key_property_info(const char* p_name) {
    return PropertyInfo(
        Variant::INT,
        p_name,
        PROPERTY_HINT_ENUM,
        resonance::KEY_ENUM_HINT_STRING,
        PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CLASS_IS_ENUM,
        "Key");
}
} // namespace

int ResonanceRuntime::live_game_runtime_count = 0;

bool ResonanceRuntime::editor_hint() {
    Engine* eng = Engine::get_singleton();
    return eng && eng->is_editor_hint();
}

// Instantiates a GDScript global class from C++. ClassDB::instantiate only covers native classes, so script-based
// helpers (bus, activator, config) are created through the loaded script's `new`.
Variant ResonanceRuntime::script_new(const String& script_path) {
    Ref<Script> scr = ResourceLoader::get_singleton()->load(script_path);
    if (scr.is_null()) {
        return Variant();
    }
    return scr->call("new");
}

int ResonanceRuntime::get_live_game_runtime_count() {
    return live_game_runtime_count;
}

void ResonanceRuntime::set_runtime(const Ref<Resource>& p_config) {
    disconnect_runtime_signals();
    runtime = p_config;
    connect_runtime_signals();
    warn_restart_if_needed();
}

void ResonanceRuntime::_ready() {
    if (runtime.is_null()) {
        runtime = Ref<Resource>(Object::cast_to<Resource>((Object*)script_new(CONFIG_SCRIPT_PATH)));
    }
    add_to_group("resonance_runtime");
    if (!editor_hint()) {
        live_game_runtime_count++;
    }
    set_process_priority(resonance::kResonanceRuntimeProcessPriority);
    set_physics_process_priority(resonance::kResonanceRuntimeProcessPriority);
    // Physics tick runs only with the Custom (Godot Physics) tracer; sync_physics_process_for_custom_tracer
    // enables it after the server reports the tracer (1.3d). Off until then.
    set_physics_process(false);
    set_process_input(true);
    connect_runtime_signals();
    const bool editor = editor_hint();
    if (editor) {
        call_deferred("notify_volumes_runtime_config_changed");
    }
    create_debug_overlay();
    initialize_server(); // returns early in editor
    if (!editor) {
        setup_activator();
        call_deferred("apply_bus_to_players");
        if (!is_connected("tree_exiting", Callable(this, "on_scene_tree_exiting"))) {
            connect("tree_exiting", Callable(this, "on_scene_tree_exiting"));
        }
    }
    update_debug_overlay_visibility();
    if (!editor) {
        refresh_performance_custom_monitors_if_ready();
    }
}

void ResonanceRuntime::on_scene_tree_exiting() {
    cleanup_reverb_activator();
}

void ResonanceRuntime::_exit_tree() {
    reset_viewport_sync_cache();
    if (is_connected("tree_exiting", Callable(this, "on_scene_tree_exiting"))) {
        disconnect("tree_exiting", Callable(this, "on_scene_tree_exiting"));
    }
    unregister_nexus_performance_monitors();
    cleanup_reverb_activator();
    disable_performance_overlay_node();
    if (!editor_hint()) {
        live_game_runtime_count = live_game_runtime_count > 0 ? live_game_runtime_count - 1 : 0;
        if (live_game_runtime_count == 0) {
            ResonanceServer* srv = ResonanceServer::get_singleton();
            if (srv && srv->is_initialized()) {
                srv->shutdown();
            }
        }
    }
    if (Object* bridge = Object::cast_to<Object>(fmod_bridge)) {
        bridge->call("shutdown_bridge");
        fmod_bridge = Variant();
    }
    if (Object* bridge = Object::cast_to<Object>(coda_bridge)) {
        bridge->call("shutdown");
        coda_bridge = Variant();
    }
}

void ResonanceRuntime::setup_activator() {
    runtime_bus = script_new(BUS_SCRIPT_PATH);
    if (Object* bus = Object::cast_to<Object>(runtime_bus)) {
        bus->call(
            "setup",
            Callable(this, "get_bus_effective"),
            Callable(this, "get_reverb_bus_name"),
            Callable(this, "get_reverb_bus_send"));
    }
    reverb_activator = script_new(ACTIVATOR_SCRIPT_PATH);
    if (Object* activator = Object::cast_to<Object>(reverb_activator)) {
        activator->call("setup", this, runtime_bus);
    }
}

void ResonanceRuntime::cleanup_reverb_activator() {
    if (Object* activator = Object::cast_to<Object>(reverb_activator)) {
        activator->call("cleanup");
    }
    reverb_activator = Variant();
}

StringName ResonanceRuntime::get_bus_effective() const {
    if (runtime.is_valid() && runtime->has_method("get_bus_effective")) {
        return runtime->call("get_bus_effective");
    }
    return StringName("Master");
}

StringName ResonanceRuntime::get_reverb_bus_name() const {
    if (runtime.is_valid() && runtime->has_method("get_reverb_bus_name_effective")) {
        return runtime->call("get_reverb_bus_name_effective");
    }
    return StringName("ResonanceReverb");
}

StringName ResonanceRuntime::get_reverb_bus_send() const {
    return get_bus_effective();
}

void ResonanceRuntime::refresh_player_bus_routing() {
    apply_bus_to_players();
}

void ResonanceRuntime::apply_bus_to_players() {
    if (!is_inside_tree()) {
        return;
    }
    Object* bus = Object::cast_to<Object>(runtime_bus);
    SceneTree* tree = get_tree();
    if (!bus || !tree) {
        return;
    }
    // ResonanceRuntimeBus.apply_bus_to_players collects the resonance_player group when none is passed.
    bus->call("apply_bus_to_players", tree);
}

Dictionary ResonanceRuntime::get_activator_instrumentation() const {
    if (Object* activator = Object::cast_to<Object>(reverb_activator)) {
        return activator->get("instrumentation");
    }
    return Dictionary();
}

void ResonanceRuntime::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_runtime", "config"), &ResonanceRuntime::set_runtime);
    ClassDB::bind_method(D_METHOD("get_runtime"), &ResonanceRuntime::get_runtime);

    ClassDB::bind_static_method(
        "ResonanceRuntime",
        D_METHOD("get_live_game_runtime_count"),
        &ResonanceRuntime::get_live_game_runtime_count);

    ClassDB::bind_method(D_METHOD("get_bus_effective"), &ResonanceRuntime::get_bus_effective);
    ClassDB::bind_method(D_METHOD("get_reverb_bus_name"), &ResonanceRuntime::get_reverb_bus_name);
    ClassDB::bind_method(D_METHOD("get_reverb_bus_send"), &ResonanceRuntime::get_reverb_bus_send);
    ClassDB::bind_method(D_METHOD("refresh_player_bus_routing"), &ResonanceRuntime::refresh_player_bus_routing);
    ClassDB::bind_method(D_METHOD("apply_bus_to_players"), &ResonanceRuntime::apply_bus_to_players);
    ClassDB::bind_method(
        D_METHOD("get_activator_instrumentation"),
        &ResonanceRuntime::get_activator_instrumentation);
    ClassDB::bind_method(D_METHOD("get_frame_timings"), &ResonanceRuntime::get_frame_timings);
    ClassDB::bind_method(D_METHOD("on_scene_tree_exiting"), &ResonanceRuntime::on_scene_tree_exiting);

    ClassDB::bind_method(D_METHOD("get_config_dict"), &ResonanceRuntime::get_config_dict);
    ClassDB::bind_method(D_METHOD("request_static_scene_reload"), &ResonanceRuntime::request_static_scene_reload);
    ClassDB::bind_method(
        D_METHOD("perform_deferred_static_scene_reload"),
        &ResonanceRuntime::perform_deferred_static_scene_reload);
    ClassDB::bind_method(D_METHOD("reload_after_reinit"), &ResonanceRuntime::reload_after_reinit);
    ClassDB::bind_method(
        D_METHOD("deferred_reset_spatial_audio_warmup_passes"),
        &ResonanceRuntime::deferred_reset_spatial_audio_warmup_passes);
    ClassDB::bind_method(
        D_METHOD("notify_volumes_runtime_config_changed"),
        &ResonanceRuntime::notify_volumes_runtime_config_changed);
    ClassDB::bind_method(
        D_METHOD("on_reflection_type_changed", "value"),
        &ResonanceRuntime::on_reflection_type_changed);
    ClassDB::bind_method(
        D_METHOD("on_audio_frame_size_changed", "value"),
        &ResonanceRuntime::on_audio_frame_size_changed);
    ClassDB::bind_method(
        D_METHOD("on_runtime_affecting_probes_changed", "value"),
        &ResonanceRuntime::on_runtime_affecting_probes_changed);
    ClassDB::bind_method(D_METHOD("get_fmod_bridge"), &ResonanceRuntime::get_fmod_bridge);
    ClassDB::bind_method(D_METHOD("get_coda_bridge"), &ResonanceRuntime::get_coda_bridge);

    ClassDB::bind_method(D_METHOD("set_fmod_bridge_enabled", "enabled"), &ResonanceRuntime::set_fmod_bridge_enabled);
    ClassDB::bind_method(D_METHOD("is_fmod_bridge_enabled"), &ResonanceRuntime::is_fmod_bridge_enabled);
    ClassDB::bind_method(D_METHOD("set_coda_bridge_enabled", "enabled"), &ResonanceRuntime::set_coda_bridge_enabled);
    ClassDB::bind_method(D_METHOD("is_coda_bridge_enabled"), &ResonanceRuntime::is_coda_bridge_enabled);
    ClassDB::bind_method(D_METHOD("set_context_simd_level", "level"), &ResonanceRuntime::set_context_simd_level);
    ClassDB::bind_method(D_METHOD("get_context_simd_level"), &ResonanceRuntime::get_context_simd_level);
    ClassDB::bind_method(D_METHOD("set_context_validation", "enabled"), &ResonanceRuntime::set_context_validation);
    ClassDB::bind_method(D_METHOD("get_context_validation"), &ResonanceRuntime::get_context_validation);

    ClassDB::bind_method(D_METHOD("set_enable_debug", "enabled"), &ResonanceRuntime::set_enable_debug);
    ClassDB::bind_method(D_METHOD("is_enable_debug"), &ResonanceRuntime::is_enable_debug);
    ClassDB::bind_method(
        D_METHOD("set_performance_custom_monitors", "level"),
        &ResonanceRuntime::set_performance_custom_monitors);
    ClassDB::bind_method(
        D_METHOD("get_performance_custom_monitors"),
        &ResonanceRuntime::get_performance_custom_monitors);
    ClassDB::bind_method(
        D_METHOD("set_debug_overlay_toggle_key", "key"), &ResonanceRuntime::set_debug_overlay_toggle_key);
    ClassDB::bind_method(
        D_METHOD("get_debug_overlay_toggle_key"), &ResonanceRuntime::get_debug_overlay_toggle_key);
    ClassDB::bind_method(
        D_METHOD("set_performance_overlay_toggle_key", "key"),
        &ResonanceRuntime::set_performance_overlay_toggle_key);
    ClassDB::bind_method(
        D_METHOD("get_performance_overlay_toggle_key"),
        &ResonanceRuntime::get_performance_overlay_toggle_key);
    ClassDB::bind_method(
        D_METHOD("set_player_overlay_toggle_key", "key"), &ResonanceRuntime::set_player_overlay_toggle_key);
    ClassDB::bind_method(
        D_METHOD("get_player_overlay_toggle_key"), &ResonanceRuntime::get_player_overlay_toggle_key);

    ADD_PROPERTY(
        PropertyInfo(Variant::OBJECT, "runtime", PROPERTY_HINT_RESOURCE_TYPE, "ResonanceRuntimeConfig"),
        "set_runtime",
        "get_runtime");
    ADD_PROPERTY(
        PropertyInfo(Variant::DICTIONARY, "activator_instrumentation", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE),
        "",
        "get_activator_instrumentation");

    ADD_GROUP("FMOD Bridge", "");
    ADD_PROPERTY(
        PropertyInfo(Variant::BOOL, "fmod_bridge_enabled"), "set_fmod_bridge_enabled", "is_fmod_bridge_enabled");
    ADD_GROUP("Coda Bridge", "");
    ADD_PROPERTY(
        PropertyInfo(Variant::BOOL, "coda_bridge_enabled"), "set_coda_bridge_enabled", "is_coda_bridge_enabled");
    ADD_GROUP("Steam Audio Context", "context_");
    ADD_PROPERTY(
        PropertyInfo(
            Variant::INT, "context_simd_level", PROPERTY_HINT_ENUM, "Default:-1,AVX-512:0,AVX2:1,AVX:2,SSE4:3,SSE2:4"),
        "set_context_simd_level",
        "get_context_simd_level");
    ADD_PROPERTY(
        PropertyInfo(Variant::BOOL, "context_validation"), "set_context_validation", "get_context_validation");

    ADD_GROUP("Runtime Debug", "");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enable_debug"), "set_enable_debug", "is_enable_debug");
    ADD_PROPERTY(key_property_info("debug_overlay_toggle_key"), "set_debug_overlay_toggle_key", "get_debug_overlay_toggle_key");
    ADD_PROPERTY(
        key_property_info("performance_overlay_toggle_key"),
        "set_performance_overlay_toggle_key",
        "get_performance_overlay_toggle_key");
    ADD_PROPERTY(key_property_info("player_overlay_toggle_key"), "set_player_overlay_toggle_key", "get_player_overlay_toggle_key");
    ADD_PROPERTY(
        PropertyInfo(Variant::INT, "performance_custom_monitors", PROPERTY_HINT_ENUM, "Off:0,Core:1,Standard:2,Full:3"),
        "set_performance_custom_monitors",
        "get_performance_custom_monitors");
}
