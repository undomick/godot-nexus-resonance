#ifndef RESONANCE_FMOD_EVENT_EMITTER_H
#define RESONANCE_FMOD_EVENT_EMITTER_H

#include "resonance_fmod_bridge.h"
#include <godot_cpp/classes/node3d.hpp>
#include <limits>

namespace godot {

class ResonanceFmodEventEmitter : public Node3D {
    GDCLASS(ResonanceFmodEventEmitter, Node3D)

  private:
    String event_path = "event:/";
    bool auto_play = true;

    int32_t resonance_handle = -1;
    int32_t fmod_handle = -1;
    ResonanceFMODBridge* bridge = nullptr;
    Node* fmod_emitter_parent = nullptr;
    Vector3 last_sync_pos = Vector3(
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::infinity());

    static bool is_fmod_emitter_parent(Node* node);
    ResonanceFMODBridge* find_runtime_fmod_bridge();
    void warn_if_parent_not_fmod_emitter();
    void sync_fmod_source_position(const Vector3& world_pos);
    void register_fmod_source();
    void release_fmod_source_handles();
    void try_push_simulation_handle_to_fmod(int32_t fmod_plugin_handle);

    void deferred_resolve_bridge();
    void deferred_register_source();

  protected:
    static void _bind_methods();

  public:
    void _enter_tree() override;
    void _ready() override;
    void _exit_tree() override;
    void _process(double delta) override;

    void set_event_path(const String& p_path);
    String get_event_path() const { return event_path; }

    void set_auto_play(bool p_enabled);
    bool is_auto_play() const { return auto_play; }
};

} // namespace godot

#endif // RESONANCE_FMOD_EVENT_EMITTER_H
