#include "resonance_debug_drawer.h"
#include "resonance_constants.h"

namespace godot {

ResonanceDebugDrawer::ResonanceDebugDrawer() {
    material.instantiate();
    material->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
    material->set_flag(BaseMaterial3D::FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
    material->set_flag(BaseMaterial3D::FLAG_DISABLE_DEPTH_TEST, true);
}

ResonanceDebugDrawer::~ResonanceDebugDrawer() {
    cleanup();
}

void ResonanceDebugDrawer::initialize(Node3D* p_parent) {
    parent_node = p_parent;
}

void ResonanceDebugDrawer::cleanup() {
    if (parent_node) {
        if (mesh_instance) {
            parent_node->remove_child(mesh_instance);
            memdelete(mesh_instance);
            mesh_instance = nullptr;
            immediate_mesh = nullptr;
        }
        if (label_instance) {
            parent_node->remove_child(label_instance);
            memdelete(label_instance);
            label_instance = nullptr;
        }
    }
    parent_node = nullptr;
}

void ResonanceDebugDrawer::_create_visuals_if_needed() {
    if (!parent_node)
        return;

    if (!mesh_instance) {
        mesh_instance = memnew(MeshInstance3D);
        immediate_mesh = memnew(ImmediateMesh);
        mesh_instance->set_mesh(immediate_mesh);
        mesh_instance->set_material_override(material);
        mesh_instance->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
        parent_node->add_child(mesh_instance);
    }

    if (!label_instance) {
        label_instance = memnew(Label3D);
        label_instance->set_billboard_mode(BaseMaterial3D::BILLBOARD_ENABLED);
        label_instance->set_position(Vector3(0, resonance::kDebugDrawerLabelOffsetY, 0));
        label_instance->set_pixel_size(resonance::kDebugDrawerLabelPixelSize);
        label_instance->set_modulate(Color(1, 1, 0));
        label_instance->set("no_depth_test", true);
        label_instance->set_visible(false);
        parent_node->add_child(label_instance);
    }
}

void ResonanceDebugDrawer::set_debug_visuals_visible(bool visible) {
    if (mesh_instance)
        mesh_instance->set_visible(visible);
    if (label_instance)
        label_instance->set_visible(visible);
}

void ResonanceDebugDrawer::_draw_line(const Vector3& from, const Vector3& to, float occlusion) {
    if (!immediate_mesh || !parent_node)
        return;

    immediate_mesh->clear_surfaces();
    immediate_mesh->surface_begin(Mesh::PRIMITIVE_LINES);

    Color col = Color(1.0f - occlusion, 1.0f - (occlusion * 0.5f), 0.0f);
    immediate_mesh->surface_set_color(col);

    Vector3 p1 = parent_node->to_local(from);
    Vector3 p2 = parent_node->to_local(to);

    immediate_mesh->surface_add_vertex(p1);
    immediate_mesh->surface_add_vertex(p2);

    immediate_mesh->surface_end();
}

void ResonanceDebugDrawer::sync_occlusion_line(const ResonanceDebugData& data, bool show_occ) {
    if (!mesh_instance)
        return;

    if (show_occ) {
        _draw_line(data.source_pos, data.listener_pos, data.occlusion);
        mesh_instance->set_visible(true);
    } else {
        mesh_instance->set_visible(false);
    }
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
    text += "Occ: " + String::num(data.occlusion, 2) + "\n";
    text += "Trans L/M/H: " + String::num(data.transmission[0], 2) + " / " + String::num(data.transmission[1], 2) + " / " + String::num(data.transmission[2], 2) + "\n";
    text += "Atten: " + String::num(data.attenuation, 2) + "\n";
    text += air_str + "\n";
    if (data.directivity_enabled)
        text += "Dir: " + String::num(data.directivity_val, 2) + "\n";

    text += "Signal D/R/P: " + String::num(data.signal_direct, 2) + " / " +
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

void ResonanceDebugDrawer::process(double delta, const ResonanceDebugData& data, bool show_occ, bool show_reverb, String node_name, bool hud_active) {
    if (!show_occ && !show_reverb) {
        set_debug_visuals_visible(false);
        return;
    }

    if (!hud_active) {
        set_debug_visuals_visible(false);
        return;
    }

    _create_visuals_if_needed();
    sync_occlusion_line(data, show_occ);
    tick_debug_label(delta, data, node_name);
}

} // namespace godot
