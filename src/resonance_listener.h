#ifndef RESONANCE_LISTENER_H
#define RESONANCE_LISTENER_H

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/typed_array.hpp>

namespace godot {

class ResonanceServer;

class ResonanceListener : public Node3D {
    GDCLASS(ResonanceListener, Node3D)

  public:
    ResonanceListener() = default;

    ResonanceListener(const ResonanceListener&) = delete;
    ResonanceListener(ResonanceListener&&) = delete;

    void set_listener_valid(bool valid) { listener_valid = valid; }
    bool is_listener_valid() const { return listener_valid; }

    /// Active listeners under the viewport camera; call before ResonanceServer::tick.
    static void sync_viewport_listeners_to_server(Viewport* vp, const TypedArray<Node>& listener_nodes);

    /// Pose + validity from the viewport camera (Lis-01 / no-driver fallback).
    static void push_camera_fallback_listener_to_server(Camera3D* cam, ResonanceServer* server);

    void _enter_tree() override;
    void _exit_tree() override;
    void _process(double delta) override;
    void _physics_process(double delta) override;

  protected:
    static void _bind_methods();

    bool listener_valid = true;
    bool listener_sync_uses_physics_ = false;

    bool _listener_sync_uses_physics() const;
    void _apply_process_mode_for_tracer();
    bool _push_listener_pose_if_active(Camera3D* cam);
    void _sync_listener_tick(double delta, bool use_physics_frame);
};

} // namespace godot

#endif // RESONANCE_LISTENER_H
