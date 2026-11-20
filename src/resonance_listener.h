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

    enum ReverbType {
        REVERB_REALTIME = 0,
        REVERB_BAKED = 1,
    };

    void set_listener_valid(bool valid) { listener_valid = valid; }
    bool is_listener_valid() const { return listener_valid; }
    void set_apply_reverb(bool enabled) { apply_reverb = enabled; }
    bool is_apply_reverb() const { return apply_reverb; }
    void set_reverb_type(ReverbType type) { reverb_type = type; }
    ReverbType get_reverb_type() const { return reverb_type; }

    /// Active listeners under the viewport camera; call before ResonanceServer::tick.
    static void sync_viewport_listeners_to_server(Viewport* vp, const TypedArray<Node>& listener_nodes);

    /// Fallback when no ResonanceListener drives the camera: pose and validity come from the camera.
    static void push_camera_fallback_listener_to_server(Camera3D* cam, ResonanceServer* server);

    void _enter_tree() override;
    void _exit_tree() override;
    void _process(double delta) override;
    void _physics_process(double delta) override;

  protected:
    static void _bind_methods();

    bool listener_valid = true;
    bool apply_reverb = false;
    ReverbType reverb_type = REVERB_REALTIME;
    bool reverb_pushed_to_server_ = false;
    bool listener_sync_uses_physics_ = false;

    bool _listener_sync_uses_physics() const;
    void _apply_process_mode_for_tracer();
    bool _push_listener_pose_if_active(Camera3D* cam);
    void _sync_listener_tick(double delta, bool use_physics_frame);
};

} // namespace godot

VARIANT_ENUM_CAST(ResonanceListener::ReverbType);

#endif // RESONANCE_LISTENER_H
