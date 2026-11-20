#ifndef RESONANCE_DIRECTIVITY_DRAWER_H
#define RESONANCE_DIRECTIVITY_DRAWER_H

#include <godot_cpp/classes/immediate_mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/variant/color.hpp>

namespace godot {

/// Shares ResonancePlayer::build_directivity_gizmo_lines with the editor gizmo so both draw the same pattern.
class ResonanceDirectivityDrawer {
  public:
    struct Params {
        bool enabled = false;
        int input_mode = 0; // 0 = Simulation Defined, 1 = User Defined
        float weight = 0.0f;
        float power = 1.0f;
        float user_value = 1.0f;
        float size = 1.0f;
        bool operator==(const Params& o) const {
            return enabled == o.enabled && input_mode == o.input_mode && weight == o.weight &&
                   power == o.power && user_value == o.user_value && size == o.size;
        }
        bool operator!=(const Params& o) const { return !(*this == o); }
    };

    ResonanceDirectivityDrawer();
    ~ResonanceDirectivityDrawer();

    ResonanceDirectivityDrawer(const ResonanceDirectivityDrawer&) = delete;
    ResonanceDirectivityDrawer& operator=(const ResonanceDirectivityDrawer&) = delete;
    ResonanceDirectivityDrawer(ResonanceDirectivityDrawer&&) = delete;
    ResonanceDirectivityDrawer& operator=(ResonanceDirectivityDrawer&&) = delete;

    void initialize(Node3D* p_parent);
    void cleanup();

    /// The next process() rebuilds the mesh even if the params are unchanged.
    void mark_dirty() { dirty_ = true; }

    /// Rebuilds the mesh only when the params change.
    void process(const Params& p, bool visible);

  private:
    Node3D* parent_node_ = nullptr;
    MeshInstance3D* mesh_instance_ = nullptr;
    ImmediateMesh* immediate_mesh_ = nullptr;
    Ref<StandardMaterial3D> material_dipole_;
    Ref<StandardMaterial3D> material_omni_;

    Params last_params_{};
    bool have_last_params_ = false;
    bool dirty_ = false;

    void _create_visuals_if_needed();
    void _rebuild_mesh(const Params& p);
};

} // namespace godot

#endif
