#include "resonance_listener.h"
#include "resonance_constants.h"
#include "resonance_server.h"
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
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
    reflection_mesh_instance = nullptr;
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

void ResonanceListener::_push_listener_pose_if_active(Camera3D* cam) {
    ResonanceServer* server = ResonanceServer::get_singleton();
    if (!server || !server->is_initialized())
        return;

    const bool drives_server = cam && cam->is_ancestor_of(this);
    if (!drives_server) {
        server->set_listener_valid(false);
        return;
    }

    server->set_listener_valid(listener_valid);

    Transform3D gt = get_global_transform();
    Vector3 position = gt.origin;
    Vector3 forward = -gt.basis.get_column(2);
    Vector3 up = gt.basis.get_column(1);
    server->update_listener(position, forward, up);
}

void ResonanceListener::sync_viewport_listeners_to_server(Viewport* vp, const TypedArray<Node>& listener_nodes) {
    if (!vp)
        return;
    ResonanceServer* server = ResonanceServer::get_singleton();
    if (!server || !server->is_initialized())
        return;
    Camera3D* cam = vp->get_camera_3d();
    for (int i = 0; i < listener_nodes.size(); i++) {
        ResonanceListener* rl = Object::cast_to<ResonanceListener>(listener_nodes[i]);
        if (rl)
            rl->_push_listener_pose_if_active(cam);
    }
}

void ResonanceListener::_sync_reflection_debug_viz(ResonanceServer* server) {
    if (!server)
        return;
    if (server->wants_debug_reflection_viz()) {
        Array segments = server->get_ray_debug_segments();
        if (!segments.is_empty()) {
            _ensure_reflection_viz();
            _draw_reflection_rays(segments);
            if (reflection_mesh_instance)
                reflection_mesh_instance->set_visible(true);
        } else {
            if (reflection_mesh_instance)
                reflection_mesh_instance->set_visible(false);
        }
    } else {
        if (reflection_mesh_instance)
            reflection_mesh_instance->set_visible(false);
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
    _push_listener_pose_if_active(cam);
    _sync_reflection_debug_viz(server);
}

void ResonanceListener::_process(double delta) {
    _sync_listener_tick(delta, false);
}

void ResonanceListener::_physics_process(double delta) {
    _sync_listener_tick(delta, true);
}

void ResonanceListener::_ensure_reflection_viz() {
    if (reflection_mesh_instance)
        return;

    reflection_material.instantiate();
    reflection_material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
    reflection_material->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
    reflection_material->set_flag(BaseMaterial3D::FLAG_DISABLE_DEPTH_TEST, true);

    reflection_immediate_mesh.instantiate();
    reflection_mesh_instance = memnew(MeshInstance3D);
    reflection_mesh_instance->set_mesh(reflection_immediate_mesh);
    reflection_mesh_instance->set_material_override(reflection_material);
    reflection_mesh_instance->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
    reflection_mesh_instance->set_name("ResonanceReflectionRayDebugViz");
    add_child(reflection_mesh_instance);
}

void ResonanceListener::_draw_reflection_rays(const Array& segments) {
    if (!reflection_immediate_mesh.is_valid())
        return;

    reflection_immediate_mesh->clear_surfaces();
    reflection_immediate_mesh->surface_begin(Mesh::PRIMITIVE_LINES);

    const Color col = Color(resonance::kListenerReflectionRayR, resonance::kListenerReflectionRayG, resonance::kListenerReflectionRayB);
    reflection_immediate_mesh->surface_set_color(col);

    for (int i = 0; i < segments.size(); i++) {
        Variant v = segments[i];
        if (v.get_type() != Variant::DICTIONARY)
            continue;
        Dictionary d = v;
        Variant from_v = d.get("from", Vector3());
        Variant to_v = d.get("to", Vector3());
        Vector3 from_pt = from_v;
        Vector3 to_pt = to_v;
        reflection_immediate_mesh->surface_add_vertex(to_local(from_pt));
        reflection_immediate_mesh->surface_add_vertex(to_local(to_pt));
    }
    reflection_immediate_mesh->surface_end();
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
