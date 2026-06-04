#include "resonance_listener.h"
#include "resonance_runtime.h"
#include "resonance_server.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/collision_object3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace {
const int PERF_MONITORS_FULL = 3;

int64_t usec_since(uint64_t start) {
    const uint64_t now = Time::get_singleton()->get_ticks_usec();
    return now > start ? (int64_t)(now - start) : 0;
}
} // namespace

bool ResonanceRuntime::uses_full_frame_timing() const {
    return performance_custom_monitors == PERF_MONITORS_FULL;
}

void ResonanceRuntime::reset_viewport_sync_cache() {
    vp_sync_cache_valid = false;
    vp_sync_last_world_rid = RID();
    vp_sync_last_exclude_ids = PackedInt64Array();
    vp_sync_last_cam_xform = Transform3D();
    vp_sync_last_had_listener_nodes = false;
}

void ResonanceRuntime::refresh_group_caches_for_frame(bool use_physics_frame) {
    Engine* eng = Engine::get_singleton();
    int64_t fid = use_physics_frame ? (int64_t)eng->get_physics_frames() : (int64_t)eng->get_process_frames();
    if (group_cache_frame == fid && group_cache_use_physics == use_physics_frame) {
        return;
    }
    group_cache_frame = fid;
    group_cache_use_physics = use_physics_frame;
    SceneTree* tree = get_tree();
    if (tree == nullptr) {
        cached_listener_nodes = TypedArray<Node>();
        cached_player_nodes = TypedArray<Node>();
        return;
    }
    cached_listener_nodes = tree->get_nodes_in_group("resonance_listener");
    cached_player_nodes = tree->get_nodes_in_group("resonance_player");
}

// CollisionObject3D RIDs along the active camera parent chain plus resonance_listener group nodes. With the Custom
// tracer these are excluded so occlusion/reflection rays do not hit the listener body.
TypedArray<RID> ResonanceRuntime::collect_listener_physics_exclude_rids(
    Viewport* vp, const TypedArray<Node>& listener_nodes) const {
    TypedArray<RID> out;
    Camera3D* cam = vp->get_camera_3d();
    if (cam) {
        Node* n = cam;
        while (n) {
            if (CollisionObject3D* co = Object::cast_to<CollisionObject3D>(n)) {
                out.append(co->get_rid());
            }
            n = n->get_parent();
        }
    }
    for (int i = 0; i < listener_nodes.size(); i++) {
        if (CollisionObject3D* co = Object::cast_to<CollisionObject3D>(listener_nodes[i])) {
            out.append(co->get_rid());
        }
    }
    return out;
}

PackedInt64Array ResonanceRuntime::sorted_rid_int_ids(const TypedArray<RID>& rids) const {
    PackedInt64Array ids;
    for (int i = 0; i < rids.size(); i++) {
        const RID r = rids[i];
        if (r.is_valid()) {
            ids.push_back((int64_t)r.get_id());
        }
    }
    ids.sort();
    return ids;
}

bool ResonanceRuntime::camera_listener_xform_changed(Camera3D* cam) const {
    const Transform3D xf = cam->get_global_transform();
    return !xf.origin.is_equal_approx(vp_sync_last_cam_xform.origin) || !xf.basis.is_equal_approx(vp_sync_last_cam_xform.basis);
}

// Pushes active viewport world, optional Custom-tracer exclude RIDs, and default-camera listener into ResonanceServer.
void ResonanceRuntime::apply_resonance_viewport_to_server(Viewport* vp, bool use_physics_frame) {
    refresh_group_caches_for_frame(use_physics_frame);
    TypedArray<Node> listener_nodes = cached_listener_nodes;
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (srv == nullptr || !srv->is_initialized()) {
        return;
    }

    Ref<World3D> w3 = vp->get_world_3d();
    RID wrid = w3.is_valid() ? w3->get_rid() : RID();
    if (!vp_sync_cache_valid || wrid != vp_sync_last_world_rid) {
        srv->set_physics_world(w3);
        vp_sync_last_world_rid = wrid;
    }

    if (srv->uses_custom_ray_tracer()) {
        TypedArray<RID> collected = collect_listener_physics_exclude_rids(vp, listener_nodes);
        PackedInt64Array ex_ids = sorted_rid_int_ids(collected);
        if (!vp_sync_cache_valid || ex_ids != vp_sync_last_exclude_ids) {
            srv->set_listener_physics_ray_exclude_rids(collected);
            vp_sync_last_exclude_ids = ex_ids;
        }
    }

    Camera3D* cam = vp->get_camera_3d();
    if (cam && is_inside_tree()) {
        if (listener_nodes.is_empty()) {
            if (!vp_sync_cache_valid || vp_sync_last_had_listener_nodes || camera_listener_xform_changed(cam)) {
                const Transform3D gt = cam->get_global_transform();
                srv->update_listener(cam->get_global_position(), -gt.basis.get_column(2), gt.basis.get_column(1));
                vp_sync_last_cam_xform = gt;
            }
            vp_sync_last_had_listener_nodes = false;
        } else {
            vp_sync_last_had_listener_nodes = true;
            ResonanceListener::sync_viewport_listeners_to_server(vp, listener_nodes);
        }
    }
    vp_sync_cache_valid = true;
}

// Custom tracer: ResonanceServer.tick runs in _physics_process with the physics World3D. Toggle physics processing
// to match the active tracer; called after init/reinit (1.3d).
void ResonanceRuntime::sync_physics_process_for_custom_tracer() {
    if (editor_hint()) {
        set_physics_process(false);
        return;
    }
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (srv == nullptr || !srv->is_initialized()) {
        set_physics_process(false);
        return;
    }
    const bool custom = srv->uses_custom_ray_tracer();
    set_physics_process(custom);
    if (is_inside_tree()) {
        SceneTree* tree = get_tree();
        if (tree) {
            tree->call_group_flags(
                SceneTree::GROUP_CALL_DEFERRED, "resonance_listener", "_apply_process_mode_for_tracer");
        }
    }
    if (custom) {
        warn_custom_tracer_main_thread_sim(srv);
    }
}

void ResonanceRuntime::warn_custom_tracer_main_thread_sim(ResonanceServer* srv) {
    if (custom_tracer_main_sim_warned || srv == nullptr) {
        return;
    }
    if (!srv->is_initialized() || !srv->uses_custom_ray_tracer()) {
        return;
    }
    custom_tracer_main_sim_warned = true;
    const int rays = srv->get_realtime_rays();
    UtilityFunctions::push_warning(vformat(
        "Nexus Resonance: Custom (Godot Physics) runs Steam Audio simulation on the main/physics thread (no "
        "worker). High realtime ray counts can hitch frames; use get_simulation_worker_timing or reduce max_rays "
        "(current: %d).",
        rays));
    if ((bool)ProjectSettings::get_singleton()->get_setting("physics/3d/run_on_separate_thread", false)) {
        UtilityFunctions::push_warning(
            "Nexus Resonance: physics/3d/run_on_separate_thread is enabled; Custom tracer rays may see a stale "
            "physics state. Disable it or use Default/Embree scene type.");
    }
}

void ResonanceRuntime::fill_activator_buffer() {
    Object* activator = Object::cast_to<Object>(reverb_activator);
    if (activator == nullptr || Object::cast_to<Object>(runtime_bus) == nullptr) {
        return;
    }
    activator->call("fill_buffer", runtime_bus);
}

void ResonanceRuntime::_process(double delta) {
    if (editor_hint()) {
        return;
    }
    refresh_group_caches_for_frame(false);
    main_thread_activator_usec = 0;
    main_thread_reinit_usec = 0;
    main_thread_viewport_usec = 0;
    main_thread_tick_usec = 0;
    main_thread_flush_usec = 0;

    const uint64_t t0 = Time::get_singleton()->get_ticks_usec();
    const uint64_t t_activator = t0;
    fill_activator_buffer();
    main_thread_activator_usec = usec_since(t_activator);

    ResonanceServer* srv = ResonanceServer::get_singleton();
    const bool srv_ready = srv != nullptr && srv->is_initialized();
    const bool custom_tracer = srv_ready && srv->uses_custom_ray_tracer();
    if (!srv_ready || !custom_tracer) {
        runtime_physics_tick_usec = 0;
        runtime_physics_viewport_usec = 0;
        runtime_physics_server_tick_usec = 0;
        runtime_physics_flush_usec = 0;
    }

    if (srv_ready) {
        handle_pending_reinit_frame_size();
        Viewport* vp = get_viewport();
        if (vp && !custom_tracer) {
            if (uses_full_frame_timing()) {
                const uint64_t tv = Time::get_singleton()->get_ticks_usec();
                apply_resonance_viewport_to_server(vp, false);
                main_thread_viewport_usec = usec_since(tv);
                const uint64_t tf = Time::get_singleton()->get_ticks_usec();
                srv->flush_pending_source_updates();
                main_thread_flush_usec = usec_since(tf);
                const uint64_t tt = Time::get_singleton()->get_ticks_usec();
                srv->tick(delta);
                main_thread_tick_usec = usec_since(tt);
            } else {
                apply_resonance_viewport_to_server(vp, false);
                srv->flush_pending_source_updates();
                srv->tick(delta);
            }
        }
    }

    main_thread_last_tick_usec = usec_since(t0);

    tick_performance_monitors();

    Object* coda = Object::cast_to<Object>(coda_bridge);
    if (coda && !custom_tracer) {
        coda->call("tick", delta);
    }
}

void ResonanceRuntime::_physics_process(double delta) {
    if (editor_hint()) {
        return;
    }
    refresh_group_caches_for_frame(true);
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (srv == nullptr || !srv->is_initialized() || !srv->uses_custom_ray_tracer()) {
        return;
    }
    runtime_physics_viewport_usec = 0;
    runtime_physics_server_tick_usec = 0;
    runtime_physics_flush_usec = 0;

    const uint64_t t0 = Time::get_singleton()->get_ticks_usec();
    Viewport* vp = get_viewport();
    if (vp) {
        if (uses_full_frame_timing()) {
            const uint64_t tv = Time::get_singleton()->get_ticks_usec();
            apply_resonance_viewport_to_server(vp, true);
            runtime_physics_viewport_usec = usec_since(tv);
        } else {
            apply_resonance_viewport_to_server(vp, true);
        }
    }
    if (uses_full_frame_timing()) {
        const uint64_t tf = Time::get_singleton()->get_ticks_usec();
        srv->flush_pending_source_updates();
        runtime_physics_flush_usec = usec_since(tf);
        const uint64_t tt = Time::get_singleton()->get_ticks_usec();
        srv->tick(delta);
        runtime_physics_server_tick_usec = usec_since(tt);
    } else {
        srv->flush_pending_source_updates();
        srv->tick(delta);
    }
    runtime_physics_tick_usec = usec_since(t0);

    Object* coda = Object::cast_to<Object>(coda_bridge);
    if (coda) {
        coda->call("tick", delta);
    }
}

Dictionary ResonanceRuntime::get_frame_timings() const {
    Dictionary d;
    d["main_thread_last_tick_usec"] = main_thread_last_tick_usec;
    d["main_thread_activator_usec"] = main_thread_activator_usec;
    d["main_thread_reinit_usec"] = main_thread_reinit_usec;
    d["main_thread_viewport_usec"] = main_thread_viewport_usec;
    d["main_thread_tick_usec"] = main_thread_tick_usec;
    d["main_thread_flush_usec"] = main_thread_flush_usec;
    d["runtime_physics_tick_usec"] = runtime_physics_tick_usec;
    d["runtime_physics_viewport_usec"] = runtime_physics_viewport_usec;
    d["runtime_physics_server_tick_usec"] = runtime_physics_server_tick_usec;
    d["runtime_physics_flush_usec"] = runtime_physics_flush_usec;
    return d;
}
