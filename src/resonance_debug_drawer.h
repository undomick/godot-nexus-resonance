#ifndef RESONANCE_DEBUG_DRAWER_H
#define RESONANCE_DEBUG_DRAWER_H

#include "resonance_constants.h"
#include <godot_cpp/classes/immediate_mesh.hpp>
#include <godot_cpp/classes/label3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/variant/color.hpp>
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

    bool air_abs_enabled;
    bool directivity_enabled;

    float signal_direct = 0.0f;
    float signal_reverb = 0.0f;
    float signal_pathing = 0.0f;
};

class ResonanceDebugDrawer {
  private:
    Node3D* parent_node = nullptr;

    MeshInstance3D* mesh_instance = nullptr;
    ImmediateMesh* immediate_mesh = nullptr;
    Label3D* label_instance = nullptr;
    Ref<StandardMaterial3D> material;

    double update_timer = 0.0;

    void _create_visuals_if_needed();
    void _draw_line(const Vector3& from, const Vector3& to, float occlusion);
    void _update_label_text(const ResonanceDebugData& data, String node_name);

    void set_debug_visuals_visible(bool visible);
    void sync_occlusion_line(const ResonanceDebugData& data, bool show_occ);
    void tick_debug_label(double delta, const ResonanceDebugData& data, const String& node_name);

  public:
    ResonanceDebugDrawer();
    ~ResonanceDebugDrawer();

    ResonanceDebugDrawer(const ResonanceDebugDrawer&) = delete;
    ResonanceDebugDrawer& operator=(const ResonanceDebugDrawer&) = delete;
    ResonanceDebugDrawer(ResonanceDebugDrawer&&) = delete;
    ResonanceDebugDrawer& operator=(ResonanceDebugDrawer&&) = delete;

    void initialize(Node3D* p_parent);
    void cleanup();

    /// Per-player debug HUD. show_occ draws the source->listener line; show_reverb enables the label only
    /// (reflection rays use the global debug overlay). hud_active is false during idle or after grace expires.
    void process(double delta, const ResonanceDebugData& data, bool show_occ, bool show_reverb, String node_name, bool hud_active);
};

} // namespace godot

#endif
