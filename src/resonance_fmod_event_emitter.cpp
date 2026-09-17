#include "resonance_fmod_event_emitter.h"
#include "resonance_fmod_source_sync_policy.h"
#include "resonance_server.h"
#include "resonance_source_handle_policy.h"
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <limits>

namespace godot {

namespace {
constexpr float kSyncPosEpsSq = 1e-4f * 1e-4f;
constexpr float kSyncOrientEpsSq = 1e-4f * 1e-4f;

bool editor_hint() {
    Engine* eng = Engine::get_singleton();
    return eng && eng->is_editor_hint();
}

void invalidate_sync_pose(Vector3& out_pos, Vector3& out_forward, Vector3& out_up) {
    out_pos = Vector3(
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity());
    out_forward = out_pos;
    out_up = out_pos;
}
} // namespace

bool ResonanceFmodEventEmitter::is_fmod_emitter_parent(Node* node) {
    return node != nullptr && node->is_class(StringName("FmodEventEmitter3D"));
}

void ResonanceFmodEventEmitter::warn_if_parent_not_fmod_emitter() {
    if (is_fmod_emitter_parent(get_parent())) {
        return;
    }
    const String parent_class = get_parent() ? get_parent()->get_class() : String("null");
    UtilityFunctions::push_warning(
        "ResonanceFmodEventEmitter: Parent must be FmodEventEmitter3D. Found: " + parent_class);
}

Object* ResonanceFmodEventEmitter::find_runtime_fmod_bridge() {
    SceneTree* tree = get_tree();
    if (!tree) {
        return nullptr;
    }
    TypedArray<Node> runtimes = tree->get_nodes_in_group(StringName("resonance_runtime"));
    for (int i = 0; i < runtimes.size(); i++) {
        Node* rt = Object::cast_to<Node>(runtimes[i]);
        if (!rt || !rt->has_method(StringName("get_fmod_bridge"))) {
            continue;
        }
        const Variant bridge_var = rt->call(StringName("get_fmod_bridge"));
        // Runtime stores the GDScript ResonanceFMODBridgeScript wrapper, not ResonanceFMODBridge.
        Object* candidate = Object::cast_to<Object>(bridge_var);
        if (!candidate || !candidate->has_method(StringName("is_bridge_loaded"))) {
            continue;
        }
        if ((bool)candidate->call(StringName("is_bridge_loaded"))) {
            return candidate;
        }
    }
    return nullptr;
}

void ResonanceFmodEventEmitter::sync_fmod_source_pose(const Transform3D& world_xform) {
    if (resonance_handle < 0) {
        return;
    }
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!srv || !srv->is_initialized()) {
        return;
    }
    if (!resonance::source_handle_matches_lifecycle_epoch(resonance_handle, source_lifecycle_epoch_,
                                                          srv->get_source_lifecycle_epoch())) {
        invalidate_handles_after_engine_reinit();
        return;
    }
    ResonanceServer::SourceUpdateParams params;
    params.position = world_xform.origin;
    params.radius = 1.0f;
    params.source_forward = -world_xform.basis.get_column(2);
    params.source_up = world_xform.basis.get_column(1);
    params.num_transmission_rays = srv->get_max_transmission_surfaces();
    if (resonance::fmod_source_sync_should_enqueue_on_try_update_failure(
            srv->try_update_source(resonance_handle, params)))
        srv->enqueue_source_update(resonance_handle, params);
}

void ResonanceFmodEventEmitter::_enter_tree() {
    add_to_group("resonance_fmod_event_emitter");
    fmod_emitter_parent = is_fmod_emitter_parent(get_parent()) ? get_parent() : nullptr;
    if (fmod_emitter_parent == nullptr && !editor_hint()) {
        warn_if_parent_not_fmod_emitter();
    }
}

void ResonanceFmodEventEmitter::_ready() {
    if (editor_hint()) {
        return;
    }
    if (!is_fmod_emitter_parent(get_parent())) {
        return;
    }
    fmod_emitter_parent = get_parent();
    call_deferred("deferred_resolve_bridge");
    if (auto_play && fmod_emitter_parent->has_method(StringName("play"))) {
        call_deferred("deferred_register_source");
    }
}

void ResonanceFmodEventEmitter::_exit_tree() {
    release_fmod_source_handles();
}

void ResonanceFmodEventEmitter::_process(double /*delta*/) {
    if (editor_hint() || resonance_handle < 0 || bridge == nullptr) {
        return;
    }
    const Transform3D xf = get_global_transform();
    const Vector3 forward = -xf.basis.get_column(2);
    const Vector3 up = xf.basis.get_column(1);
    const bool pos_same = last_sync_pos.distance_squared_to(xf.origin) < kSyncPosEpsSq;
    const bool fwd_same = last_sync_forward.distance_squared_to(forward) < kSyncOrientEpsSq;
    const bool up_same = last_sync_up.distance_squared_to(up) < kSyncOrientEpsSq;
    if (pos_same && fwd_same && up_same) {
        return;
    }
    last_sync_pos = xf.origin;
    last_sync_forward = forward;
    last_sync_up = up;
    sync_fmod_source_pose(xf);
}

void ResonanceFmodEventEmitter::deferred_resolve_bridge() {
    if (editor_hint()) {
        return;
    }
    bridge = find_runtime_fmod_bridge();
    if (bridge != nullptr || !auto_play) {
        return;
    }
    UtilityFunctions::push_warning(
        "ResonanceFmodEventEmitter: No ResonanceRuntime with FMOD bridge. Enable fmod_bridge_enabled on ResonanceRuntime.");
}

void ResonanceFmodEventEmitter::deferred_register_source() {
    register_fmod_source();
}

void ResonanceFmodEventEmitter::register_fmod_source() {
    if (bridge == nullptr) {
        bridge = find_runtime_fmod_bridge();
    }
    if (bridge == nullptr || !bridge->has_method(StringName("is_bridge_loaded")) ||
        !(bool)bridge->call(StringName("is_bridge_loaded"))) {
        return;
    }
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!srv || !srv->is_initialized()) {
        return;
    }
    resonance_handle = srv->create_source_handle(get_global_position(), 1.0f, get_path());
    if (resonance_handle < 0) {
        return;
    }
    source_lifecycle_epoch_ = srv->get_source_lifecycle_epoch();
    const Transform3D xf = get_global_transform();
    last_sync_pos = xf.origin;
    last_sync_forward = -xf.basis.get_column(2);
    last_sync_up = xf.basis.get_column(1);
    fmod_handle = (int32_t)bridge->call(StringName("add_fmod_source"), resonance_handle);
    if (fmod_handle < 0) {
        if (resonance::source_handle_matches_lifecycle_epoch(resonance_handle, source_lifecycle_epoch_,
                                                             srv->get_source_lifecycle_epoch())) {
            srv->destroy_source_handle(resonance_handle);
        }
        resonance_handle = -1;
        source_lifecycle_epoch_ = 0;
        invalidate_sync_pose(last_sync_pos, last_sync_forward, last_sync_up);
        return;
    }
    sync_fmod_source_pose(xf);
    try_push_simulation_handle_to_fmod(fmod_handle);
}

void ResonanceFmodEventEmitter::try_push_simulation_handle_to_fmod(int32_t fmod_plugin_handle) {
    (void)fmod_plugin_handle;
    // TODO: fmod-gdextension DSP API for Simulation Outputs Handle.
}

void ResonanceFmodEventEmitter::invalidate_handles_after_engine_reinit() {
    // Server already destroyed IPLSources; FMOD plugin terminate drops its source map.
    // Do not destroy_source_handle / remove_fmod_source - IDs may already be recycled.
    fmod_handle = -1;
    resonance_handle = -1;
    source_lifecycle_epoch_ = 0;
    invalidate_sync_pose(last_sync_pos, last_sync_forward, last_sync_up);
}

void ResonanceFmodEventEmitter::reload_source_after_reinit() {
    if (editor_hint()) {
        return;
    }
    invalidate_handles_after_engine_reinit();
    bridge = find_runtime_fmod_bridge();
    if (auto_play) {
        register_fmod_source();
    }
}

void ResonanceFmodEventEmitter::release_fmod_source_handles() {
    if (bridge != nullptr && fmod_handle >= 0 && bridge->has_method(StringName("remove_fmod_source"))) {
        bridge->call(StringName("remove_fmod_source"), fmod_handle);
        fmod_handle = -1;
    }
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (resonance_handle >= 0 && srv && srv->is_initialized() && !ResonanceServer::is_shutting_down() &&
        resonance::source_handle_matches_lifecycle_epoch(resonance_handle, source_lifecycle_epoch_,
                                                         srv->get_source_lifecycle_epoch())) {
        srv->destroy_source_handle(resonance_handle);
    }
    resonance_handle = -1;
    source_lifecycle_epoch_ = 0;
    invalidate_sync_pose(last_sync_pos, last_sync_forward, last_sync_up);
    bridge = nullptr;
}

void ResonanceFmodEventEmitter::set_event_path(const String& p_path) {
    event_path = p_path;
}

void ResonanceFmodEventEmitter::set_auto_play(bool p_enabled) {
    auto_play = p_enabled;
}

void ResonanceFmodEventEmitter::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_event_path", "path"), &ResonanceFmodEventEmitter::set_event_path);
    ClassDB::bind_method(D_METHOD("get_event_path"), &ResonanceFmodEventEmitter::get_event_path);
    ClassDB::bind_method(D_METHOD("set_auto_play", "enabled"), &ResonanceFmodEventEmitter::set_auto_play);
    ClassDB::bind_method(D_METHOD("is_auto_play"), &ResonanceFmodEventEmitter::is_auto_play);

    ClassDB::bind_method(D_METHOD("deferred_resolve_bridge"), &ResonanceFmodEventEmitter::deferred_resolve_bridge);
    ClassDB::bind_method(D_METHOD("deferred_register_source"), &ResonanceFmodEventEmitter::deferred_register_source);
    ClassDB::bind_method(D_METHOD("invalidate_handles_after_engine_reinit"),
                         &ResonanceFmodEventEmitter::invalidate_handles_after_engine_reinit);
    ClassDB::bind_method(D_METHOD("reload_source_after_reinit"), &ResonanceFmodEventEmitter::reload_source_after_reinit);

    ADD_PROPERTY(PropertyInfo(Variant::STRING, "event_path"), "set_event_path", "get_event_path");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_play"), "set_auto_play", "is_auto_play");
}

} // namespace godot
