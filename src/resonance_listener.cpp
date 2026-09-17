#include "resonance_listener.h"
#include "resonance_constants.h"
#include "resonance_listener_sync_policy.h"
#include "resonance_runtime.h"
#include "resonance_server.h"
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>
using namespace godot;

void ResonanceListener::_enter_tree() {
    add_to_group("resonance_listener");
    set_process_priority(resonance::kResonanceListenerProcessPriority);
    set_physics_process_priority(resonance::kResonanceListenerProcessPriority);
    call_deferred("_apply_process_mode_for_tracer");
}

void ResonanceListener::_exit_tree() {
    remove_from_group("resonance_listener");
}

bool ResonanceListener::_listener_sync_uses_physics() const {
    return listener_sync_uses_physics_;
}

void ResonanceListener::_apply_process_mode_for_tracer() {
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint()) {
        set_physics_process(false);
        listener_sync_uses_physics_ = false;
        return;
    }
    ResonanceServer* server = ResonanceServer::get_singleton();
    const bool custom = server && server->is_initialized() && server->uses_custom_ray_tracer();
    listener_sync_uses_physics_ = custom;
    set_physics_process(custom);
}

bool ResonanceListener::_push_listener_pose_if_active(Camera3D* cam) {
    ResonanceServer* server = ResonanceServer::get_singleton();
    if (!server || !server->is_initialized())
        return false;

    const bool drives_server = cam && cam->is_ancestor_of(this);
    if (!resonance::listener_node_should_publish_validity(drives_server))
        return false;

    server->set_listener_valid(listener_valid);

    Transform3D gt = get_global_transform();
    Vector3 position = gt.origin;
    Vector3 forward = -gt.basis.get_column(2);
    Vector3 up = gt.basis.get_column(1);
    server->update_listener(position, forward, up);
    return true;
}

void ResonanceListener::push_camera_fallback_listener_to_server(Camera3D* cam, ResonanceServer* server) {
    if (!cam || !server || !server->is_initialized())
        return;
    const Transform3D gt = cam->get_global_transform();
    server->update_listener(cam->get_global_position(), -gt.basis.get_column(2), gt.basis.get_column(1));
    server->set_listener_valid(resonance::camera_fallback_listener_validity());
}

void ResonanceListener::sync_viewport_listeners_to_server(Viewport* vp, const TypedArray<Node>& listener_nodes) {
    if (!vp)
        return;
    ResonanceServer* server = ResonanceServer::get_singleton();
    if (!server || !server->is_initialized())
        return;
    Camera3D* cam = vp->get_camera_3d();
    bool any_driver = false;
    for (int i = 0; i < listener_nodes.size(); i++) {
        ResonanceListener* rl = Object::cast_to<ResonanceListener>(listener_nodes[i]);
        if (rl && rl->_push_listener_pose_if_active(cam))
            any_driver = true;
    }
    if (!resonance::listener_sync_should_use_camera_fallback(any_driver)) {
        return;
    }
    if (cam) {
        push_camera_fallback_listener_to_server(cam, server);
    } else {
        server->set_listener_valid(resonance::listener_sync_validity_when_no_driver(false));
    }
}

void ResonanceListener::_sync_listener_tick(double delta, bool use_physics_frame) {
    (void)delta;
    if (use_physics_frame != listener_sync_uses_physics_)
        return;

    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint())
        return;

    ResonanceServer* server = ResonanceServer::get_singleton();
    if (!server || !server->is_initialized())
        return;

    Viewport* vp = get_viewport();
    Camera3D* cam = vp ? vp->get_camera_3d() : nullptr;
    // Pose SSOT: primary ResonanceRuntime::apply_resonance_viewport_to_server (prio 0) calls sync_viewport_listeners_to_server.
    if (resonance::listener_node_should_push_pose(ResonanceRuntime::primary_runtime_syncs_viewport_listeners()))
        _push_listener_pose_if_active(cam);
}

void ResonanceListener::_process(double delta) {
    _sync_listener_tick(delta, false);
}

void ResonanceListener::_physics_process(double delta) {
    _sync_listener_tick(delta, true);
}

void ResonanceListener::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_listener_valid", "valid"), &ResonanceListener::set_listener_valid);
    ClassDB::bind_method(D_METHOD("is_listener_valid"), &ResonanceListener::is_listener_valid);
    ClassDB::bind_static_method(
        "ResonanceListener",
        D_METHOD("sync_viewport_listeners_to_server", "viewport", "listener_nodes"),
        &ResonanceListener::sync_viewport_listeners_to_server);
    ClassDB::bind_method(D_METHOD("_apply_process_mode_for_tracer"), &ResonanceListener::_apply_process_mode_for_tracer);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "listener_valid"), "set_listener_valid", "is_listener_valid");
}
