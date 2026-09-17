#ifndef RESONANCE_DEBUG_DRAWER_H
#define RESONANCE_DEBUG_DRAWER_H

#include "resonance_constants.h"
#include <godot_cpp/classes/label3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace godot {

struct ResonanceDebugData {
    Vector3 source_pos;
    Vector3 listener_pos;

    float occlusion;
    float transmission[3];
    float attenuation;
    float distance;
    Vector3 air_absorption;
    float directivity_val;

    /// First closestHit T (diagnosis); valid when hit0_valid.
    float hit0_transmission[3] = {1.0f, 1.0f, 1.0f};
    bool hit0_valid = false;
    int num_transmission_rays = 1;

    bool air_abs_enabled;
    bool directivity_enabled;

    float signal_direct = 0.0f;
    float signal_reverb = 0.0f;
    float signal_pathing = 0.0f;
    /// Debug HUD: R column is shared reverb bus RMS (Convolution), not per-source wet send.
    bool signal_reverb_from_shared_bus = false;
    /// Sim occlusion/transmission snapshot held during overlay grace (signals may still update).
    bool sim_snapshot_hold = false;
};

class ResonanceDebugDrawer {
  private:
    Node3D* parent_node = nullptr;
    Label3D* label_instance = nullptr;
    double update_timer = 0.0;

    void _create_visuals_if_needed();
    void _update_label_text(const ResonanceDebugData& data, String node_name);

    void set_debug_visuals_visible(bool visible);
    void tick_debug_label(double delta, const ResonanceDebugData& data, const String& node_name);

  public:
    ResonanceDebugDrawer() = default;
    ~ResonanceDebugDrawer();

    ResonanceDebugDrawer(const ResonanceDebugDrawer&) = delete;
    ResonanceDebugDrawer& operator=(const ResonanceDebugDrawer&) = delete;
    ResonanceDebugDrawer(ResonanceDebugDrawer&&) = delete;
    ResonanceDebugDrawer& operator=(ResonanceDebugDrawer&&) = delete;

    void initialize(Node3D* p_parent);
    void cleanup();

    /// Per-player 3D HUD label. show_occ / show_player_hud OR-gate (server debug_occlusion / debug_reflections).
    void process(double delta, const ResonanceDebugData& data, bool show_occ, bool show_player_hud, String node_name, bool hud_active);
};

} // namespace godot

#endif
