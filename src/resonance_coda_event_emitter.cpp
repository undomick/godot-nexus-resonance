#include "resonance_coda_event_emitter.h"
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

namespace {

bool editor_hint() {
    Engine* eng = Engine::get_singleton();
    return eng && eng->is_editor_hint();
}

} // namespace

Node* ResonanceCodaEventEmitter::get_coda_runtime() {
    if (!is_inside_tree()) {
        return nullptr;
    }
    SceneTree* tree = get_tree();
    if (!tree || !tree->get_root()) {
        return nullptr;
    }
    return tree->get_root()->get_node_or_null(NodePath("Coda"));
}

bool ResonanceCodaEventEmitter::coda_bridge_is_active(Object* bridge) const {
    if (bridge == nullptr || !bridge->has_method(StringName("is_active"))) {
        return false;
    }
    const Variant active = bridge->call(StringName("is_active"));
    return active.get_type() == Variant::BOOL && (bool)active;
}

Object* ResonanceCodaEventEmitter::find_runtime_coda_bridge() {
    SceneTree* tree = get_tree();
    if (!tree) {
        return nullptr;
    }
    TypedArray<Node> runtimes = tree->get_nodes_in_group(StringName("resonance_runtime"));
    for (int i = 0; i < runtimes.size(); i++) {
        Node* rt = Object::cast_to<Node>(runtimes[i]);
        if (!rt || !rt->has_method(StringName("get_coda_bridge"))) {
            continue;
        }
        const Variant bridge_var = rt->call(StringName("get_coda_bridge"));
        Object* candidate = Object::cast_to<Object>(bridge_var);
        if (coda_bridge_is_active(candidate)) {
            return candidate;
        }
    }
    return nullptr;
}

void ResonanceCodaEventEmitter::warn_if_coda_bridge_missing() {
    UtilityFunctions::push_warning(
        "ResonanceCodaEventEmitter: Enable coda_bridge_enabled on ResonanceRuntime for spatial sync.");
}

void ResonanceCodaEventEmitter::_ready() {
    if (editor_hint()) {
        return;
    }
    call_deferred("deferred_resolve_bridge");
    if (auto_play) {
        call_deferred("deferred_play_event");
    }
}

void ResonanceCodaEventEmitter::_exit_tree() {
    stop_event(0);
}

void ResonanceCodaEventEmitter::deferred_resolve_bridge() {
    if (editor_hint()) {
        return;
    }
    coda_bridge = find_runtime_coda_bridge();
    if (coda_bridge == nullptr) {
        warn_if_coda_bridge_missing();
    }
}

void ResonanceCodaEventEmitter::deferred_play_event() {
    play_event(Dictionary());
}

Variant ResonanceCodaEventEmitter::play_event(const Dictionary& params) {
    if (editor_hint()) {
        return Variant();
    }
    Node* coda = get_coda_runtime();
    if (coda == nullptr) {
        UtilityFunctions::push_warning("ResonanceCodaEventEmitter: Coda autoload not found.");
        return Variant();
    }
    stop_event(0);

    Dictionary play_params = params;
    play_params[StringName("_coda_spatial_emitter")] = get_path();
    if (!play_params.has(StringName("volume_db"))) {
        play_params[StringName("volume_db")] = 0.0;
    }
    if (!coda->has_method(StringName("play"))) {
        return Variant();
    }
    coda_voice_handle = coda->call(StringName("play"), event_path, play_params);
    return coda_voice_handle;
}

void ResonanceCodaEventEmitter::stop_event(int fade_ms) {
    if (coda_voice_handle.get_type() == Variant::NIL) {
        return;
    }
    Node* coda = get_coda_runtime();
    if (coda != nullptr && coda->has_method(StringName("stop"))) {
        coda->call(StringName("stop"), coda_voice_handle, fade_ms);
    }
    coda_voice_handle = Variant();
}

void ResonanceCodaEventEmitter::set_event_parameter(const String& name_or_id, const Variant& value) {
    if (coda_voice_handle.get_type() == Variant::NIL) {
        return;
    }
    Node* coda = get_coda_runtime();
    if (coda != nullptr && coda->has_method(StringName("set_parameter"))) {
        coda->call(StringName("set_parameter"), coda_voice_handle, name_or_id, value);
    }
}

void ResonanceCodaEventEmitter::set_event_path(const String& p_path) {
    event_path = p_path;
}

void ResonanceCodaEventEmitter::set_auto_play(bool p_enabled) {
    auto_play = p_enabled;
}

void ResonanceCodaEventEmitter::set_source_radius(float p_radius) {
    source_radius = p_radius;
}

void ResonanceCodaEventEmitter::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_event_path", "path"), &ResonanceCodaEventEmitter::set_event_path);
    ClassDB::bind_method(D_METHOD("get_event_path"), &ResonanceCodaEventEmitter::get_event_path);
    ClassDB::bind_method(D_METHOD("set_auto_play", "enabled"), &ResonanceCodaEventEmitter::set_auto_play);
    ClassDB::bind_method(D_METHOD("is_auto_play"), &ResonanceCodaEventEmitter::is_auto_play);
    ClassDB::bind_method(D_METHOD("set_source_radius", "radius"), &ResonanceCodaEventEmitter::set_source_radius);
    ClassDB::bind_method(D_METHOD("get_source_radius"), &ResonanceCodaEventEmitter::get_source_radius);

    ClassDB::bind_method(D_METHOD("play_event", "params"), &ResonanceCodaEventEmitter::play_event, DEFVAL(Dictionary()));
    ClassDB::bind_method(D_METHOD("stop_event", "fade_ms"), &ResonanceCodaEventEmitter::stop_event, DEFVAL(0));
    ClassDB::bind_method(D_METHOD("set_event_parameter", "name_or_id", "value"),
                         &ResonanceCodaEventEmitter::set_event_parameter);

    ClassDB::bind_method(D_METHOD("deferred_resolve_bridge"), &ResonanceCodaEventEmitter::deferred_resolve_bridge);
    ClassDB::bind_method(D_METHOD("deferred_play_event"), &ResonanceCodaEventEmitter::deferred_play_event);

    ADD_PROPERTY(PropertyInfo(Variant::STRING, "event_path"), "set_event_path", "get_event_path");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_play"), "set_auto_play", "is_auto_play");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "source_radius", PROPERTY_HINT_RANGE, "0.01,100,0.01"),
                 "set_source_radius", "get_source_radius");
}

} // namespace godot
