#include "resonance_debug_drawer.h"
#include "resonance_constants.h"
#include "resonance_player_debug_hud_policy.h"
#include "resonance_transmission_hit_policy.h"

#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/variant/color.hpp>

namespace godot {

ResonanceDebugDrawer::~ResonanceDebugDrawer() {
    cleanup();
}

void ResonanceDebugDrawer::initialize(Node3D* p_parent) {
    parent_node = p_parent;
}

void ResonanceDebugDrawer::cleanup() {
    if (parent_node && label_instance) {
        parent_node->remove_child(label_instance);
        memdelete(label_instance);
        label_instance = nullptr;
    }
    parent_node = nullptr;
}

void ResonanceDebugDrawer::_create_visuals_if_needed() {
    if (!parent_node || label_instance)
        return;

    label_instance = memnew(Label3D);
    label_instance->set_billboard_mode(BaseMaterial3D::BILLBOARD_ENABLED);
    label_instance->set_position(Vector3(0, resonance::kDebugDrawerLabelOffsetY, 0));
    label_instance->set_pixel_size(resonance::kDebugDrawerLabelPixelSize);
    label_instance->set_modulate(Color(1, 1, 0));
    label_instance->set("no_depth_test", true);
    label_instance->set_visible(false);
    parent_node->add_child(label_instance);
}

void ResonanceDebugDrawer::set_debug_visuals_visible(bool visible) {
    if (label_instance)
        label_instance->set_visible(visible);
}

void ResonanceDebugDrawer::_update_label_text(const ResonanceDebugData& data, String node_name) {
    if (!label_instance)
        return;

    String air_str = data.air_abs_enabled
                         ? ("Air L/M/H: " + String::num(data.air_absorption.x, 2) + " / " + String::num(data.air_absorption.y, 2) + " / " + String::num(data.air_absorption.z, 2))
                         : "Air: OFF";

    String text = "";
    if (!node_name.is_empty())
        text += node_name + "\n";
    text += "Occ: " + String::num(data.occlusion, 2);
    if (data.sim_snapshot_hold)
        text += " [hold]";
    text += "\n";
    text += "Trans L/M/H: " + String::num(data.transmission[0], 3) + " / " + String::num(data.transmission[1], 3) + " / " + String::num(data.transmission[2], 3) + "\n";
    if (data.hit0_valid) {
        const auto label = resonance::classify_transmission_material(data.hit0_transmission[0], data.hit0_transmission[1],
                                                                     data.hit0_transmission[2]);
        text += "Hit0 T: " + String::num(data.hit0_transmission[0], 3) + " / " + String::num(data.hit0_transmission[1], 3) +
                " / " + String::num(data.hit0_transmission[2], 3) + " (" +
                String(resonance::transmission_material_label_cstr(label)) + ") rays=" + String::num_int64(data.num_transmission_rays) +
                "\n";
    }
    text += "Atten: " + String::num(data.attenuation, 2) + "\n";
    text += String(resonance::format_player_debug_hud_distance_line(data.distance).c_str());
    text += air_str + "\n";
    if (data.directivity_enabled)
        text += "Dir: " + String::num(data.directivity_val, 2) + "\n";

    const String reverb_col = data.signal_reverb_from_shared_bus ? "R(bus)" : "R";
    text += "Signal D/" + reverb_col + "/P: " + String::num(data.signal_direct, 2) + " / " +
            String::num(data.signal_reverb, 2) + " / " + String::num(data.signal_pathing, 2);

    label_instance->set_text(text);
}

void ResonanceDebugDrawer::tick_debug_label(double delta, const ResonanceDebugData& data, const String& node_name) {
    update_timer += delta;
    if (update_timer < resonance::kDebugDrawerLabelUpdateRate)
        return;
    update_timer = 0.0;

    if (!label_instance)
        return;

    _update_label_text(data, node_name);
    label_instance->set_visible(true);
}

void ResonanceDebugDrawer::process(double delta, const ResonanceDebugData& data, bool show_occ, bool show_player_hud, String node_name, bool hud_active) {
    if (!resonance::player_debug_drawer_wants_visuals(show_occ, show_player_hud)) {
        set_debug_visuals_visible(false);
        return;
    }

    if (!hud_active) {
        set_debug_visuals_visible(false);
        return;
    }

    _create_visuals_if_needed();
    tick_debug_label(delta, data, node_name);
}

} // namespace godot
