#include "resonance_directivity_drawer.h"
#include "resonance_player.h"

#include <godot_cpp/classes/mesh.hpp>

using namespace godot;

ResonanceDirectivityDrawer::ResonanceDirectivityDrawer() {
    material_dipole_.instantiate();
    material_dipole_->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
    material_dipole_->set_flag(BaseMaterial3D::FLAG_DISABLE_DEPTH_TEST, true);
    material_dipole_->set_albedo(Color(1.0f, 0.75f, 0.15f));

    material_omni_.instantiate();
    material_omni_->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
    material_omni_->set_flag(BaseMaterial3D::FLAG_DISABLE_DEPTH_TEST, true);
    material_omni_->set_albedo(Color(0.25f, 0.85f, 1.0f));
}

ResonanceDirectivityDrawer::~ResonanceDirectivityDrawer() {
    cleanup();
}

void ResonanceDirectivityDrawer::initialize(Node3D* p_parent) {
    parent_node_ = p_parent;
}

void ResonanceDirectivityDrawer::cleanup() {
    if (parent_node_ && mesh_instance_) {
        parent_node_->remove_child(mesh_instance_);
        memdelete(mesh_instance_);
    }
    mesh_instance_ = nullptr;
    immediate_mesh_ = nullptr;
    parent_node_ = nullptr;
    have_last_params_ = false;
    dirty_ = false;
}

void ResonanceDirectivityDrawer::_create_visuals_if_needed() {
    if (!parent_node_ || mesh_instance_)
        return;
    mesh_instance_ = memnew(MeshInstance3D);
    immediate_mesh_ = memnew(ImmediateMesh);
    mesh_instance_->set_mesh(immediate_mesh_);
    mesh_instance_->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
    parent_node_->add_child(mesh_instance_);
}

void ResonanceDirectivityDrawer::_rebuild_mesh(const Params& p) {
    if (!immediate_mesh_)
        return;
    immediate_mesh_->clear_surfaces();

    PackedVector3Array lines = ResonancePlayer::build_directivity_gizmo_lines(
        p.enabled, p.input_mode, p.weight, p.power, p.user_value, p.size);
    if (lines.is_empty())
        return;

    const bool is_dipole = p.enabled && p.input_mode == 0;
    Ref<StandardMaterial3D> mat = is_dipole ? material_dipole_ : material_omni_;

    immediate_mesh_->surface_begin(Mesh::PRIMITIVE_LINES, mat);
    const int n = lines.size();
    for (int i = 0; i < n; ++i)
        immediate_mesh_->surface_add_vertex(lines[i]);
    immediate_mesh_->surface_end();
}

void ResonanceDirectivityDrawer::process(const Params& p, bool visible) {
    if (!visible) {
        if (mesh_instance_ && mesh_instance_->is_visible())
            mesh_instance_->set_visible(false);
        return;
    }
    _create_visuals_if_needed();
    if (!mesh_instance_)
        return;
    const bool params_changed = !have_last_params_ || last_params_ != p || dirty_;
    if (params_changed) {
        _rebuild_mesh(p);
        last_params_ = p;
        have_last_params_ = true;
        dirty_ = false;
    }
    if (!mesh_instance_->is_visible())
        mesh_instance_->set_visible(true);
}
