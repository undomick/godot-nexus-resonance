#ifndef RESONANCE_CODA_EVENT_EMITTER_H
#define RESONANCE_CODA_EVENT_EMITTER_H

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot {

class ResonanceCodaEventEmitter : public Node3D {
    GDCLASS(ResonanceCodaEventEmitter, Node3D)

  private:
    String event_path = "event:/";
    bool auto_play = false;
    float source_radius = 1.0f;

    Variant coda_voice_handle;
    Object* coda_bridge = nullptr;

    Node* get_coda_runtime();
    Object* find_runtime_coda_bridge();
    bool coda_bridge_is_active(Object* bridge) const;
    void warn_if_coda_bridge_missing();

    void deferred_resolve_bridge();
    void deferred_play_event();

  protected:
    static void _bind_methods();

  public:
    void _ready() override;
    void _exit_tree() override;

    void set_event_path(const String& p_path);
    String get_event_path() const { return event_path; }

    void set_auto_play(bool p_enabled);
    bool is_auto_play() const { return auto_play; }

    void set_source_radius(float p_radius);
    float get_source_radius() const { return source_radius; }

    Variant play_event(const Dictionary& params = Dictionary());
    void stop_event(int fade_ms = 0);
    void set_event_parameter(const String& name_or_id, const Variant& value);
};

} // namespace godot

#endif // RESONANCE_CODA_EVENT_EMITTER_H
