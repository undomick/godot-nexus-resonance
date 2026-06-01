#ifndef RESONANCE_PROBE_VOLUME_H
#define RESONANCE_PROBE_VOLUME_H

#include "resonance_constants.h"
#include "resonance_probe_data.h"
#include <cstdint>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/multi_mesh_instance3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/sphere_mesh.hpp>
#include <godot_cpp/core/property_info.hpp>
#include <godot_cpp/templates/hashfuncs.hpp>
#include <godot_cpp/variant/array.hpp>

namespace godot {

class ResonanceProbeVolume : public Node3D {
    GDCLASS(ResonanceProbeVolume, Node3D)

  public:
    enum ProbeGenerationType {
        GEN_CENTROID = 0,      // Single probe at volume center
        GEN_UNIFORM_FLOOR = 1, // Probes on horizontal plane at height_above_floor
        GEN_VOLUME = 2         // Uniform 3D grid filling the volume
    };

  private:
    Vector3 region_size = Vector3(10.0f, 4.0f, 10.0f);
    ProbeGenerationType generation_type = GEN_UNIFORM_FLOOR;
    float spacing = 2.0f;
    float height_above_floor = 1.5f;

    // Default True for better UX
    bool viz_visible = true;
    float viz_probe_scale = 1.0f;
    int viz_color_state = 0; // 0=gray (outdated/missing), 1=blue (up-to-date), 2=red (settings mismatch)

    Ref<ResonanceProbeData> probe_data;
    Ref<Resource> bake_config;
    int32_t probe_batch_handle = -1;

    // Bake targets: NodePaths to ResonancePlayer (sources) and ResonanceListener (listeners).
    // The editor bake pipeline issues one STATICSOURCE pass per valid bake_sources entry and one
    // STATICLISTENER pass per valid bake_listeners entry so fixed outdoor emitters get
    // position-dependent baked IRs that respect source->listener occlusion.
    Array bake_sources;
    Array bake_listeners;
    float bake_influence_radius = 10000.0f;

    bool update_pending = false;
    bool headless_baking_mode = false;
    double debounce_timer = 0.0;
    double viz_retry_timer = 0.0;

    int _runtime_load_retry_count = 0;

    MultiMeshInstance3D* viz_instance = nullptr;
    Ref<MultiMesh> viz_multimesh;
    Ref<SphereMesh> viz_mesh;

    void _create_visuals_resources();
    void _queue_update();
    void _clear_player_refs_to_this();
    uint32_t _get_bake_params_hash() const;
    /// Shared bake logic. p_precomputed_points: when non-null and non-empty, use bake_manual_grid; else bake_probes_for_volume.
    void _prepare_and_execute_bake(const PackedVector3Array* p_precomputed_points);
    void _warn_native_bake_deprecated() const;
    void _ensure_viz_instance();
    bool _compute_is_probe_dirty() const;
    bool _has_valid_resonance_config() const;

  protected:
    static void _bind_methods();
    void _notification(int p_what);
    void _validate_property(PropertyInfo& p_property) const;

  public:
    ResonanceProbeVolume();
    ~ResonanceProbeVolume();

    ResonanceProbeVolume(const ResonanceProbeVolume&) = delete;
    ResonanceProbeVolume(ResonanceProbeVolume&&) = delete;

    void _ready() override;
    void _process(double delta) override;
    void _exit_tree() override;

    void set_bake_config(const Ref<Resource>& p_config);
    Ref<Resource> get_bake_config() const;

    void set_bake_sources(const Array& p_sources);
    Array get_bake_sources() const;
    void set_bake_listeners(const Array& p_listeners);
    Array get_bake_listeners() const;
    void set_bake_influence_radius(float p_radius);
    float get_bake_influence_radius() const;

    void set_probe_data(const Ref<ResonanceProbeData>& p_data);
    Ref<ResonanceProbeData> get_probe_data() const;

    void set_region_size(Vector3 p_size);
    Vector3 get_region_size() const;

    void set_generation_type(ProbeGenerationType p_type);
    ProbeGenerationType get_generation_type() const;

    void set_spacing(float p_spacing);
    float get_spacing() const;

    void set_height_above_floor(float p_height);
    float get_height_above_floor() const;

    void set_viz_visible(bool p_visible);
    bool is_viz_visible() const;

    void set_viz_probe_scale(float p_scale);
    float get_viz_probe_scale() const;

    void set_viz_color_state(int p_state); // 0=gray, 1=blue, 2=red
    int get_viz_color_state() const;

    void set_headless_baking_mode(bool p_mode);
    bool is_headless_baking_mode() const;

    /// Called by ResonanceRuntime when runtime config affecting probe compatibility changes.
    void notify_runtime_config_changed(int p_runtime_refl, bool p_runtime_pathing);

    int64_t get_bake_params_hash() const;

    /// @deprecated Reflection-only bake. Use ResonanceBakeRunner.run_bake([volume]) instead - it
    /// runs the full pathing + static-source + static-listener pipeline, auto-re-exports stale
    /// ResonanceStaticScene assets, takes an undo backup, and updates all bookkeeping hashes.
    /// Scheduled for removal in 1.0.
    void bake_probes();
    /// @deprecated Reflection-only bake with pre-computed floor raycast points. Use
    /// ResonanceBakeRunner.run_bake([volume]) instead. Scheduled for removal in 1.0.
    void bake_probes_with_floor_points(const PackedVector3Array& p_points);
    /// For Uniform Floor: raycast down onto collision geometry. Returns empty if no collisions. Requires CollisionShape3D on floor. MUST be called from main thread.
    PackedVector3Array generate_probes_on_floor_raycast() const;
    /// Returns the probe batch handle for pathing (used when ResonancePlayer.pathing_probe_volume points here). -1 if not loaded.
    int32_t get_probe_batch_handle() const { return probe_batch_handle; }
    void reload_probe_batch();
    /// Unloads the native IPLProbeBatch from the Steam Audio simulator and resets the local handle
    /// to -1. Leaves [code]probe_data[/code] untouched so a subsequent [code]reload_probe_batch[/code]
    /// can re-register the same data. Safe to call when no batch is loaded (no-op).
    void release_probe_batch();
    void _update_visuals();
    void _runtime_load_probe_batch();
    void _reload_probe_batch_after_reinit();
    void _check_probe_data_loaded();
};
} // namespace godot

VARIANT_ENUM_CAST(godot::ResonanceProbeVolume::ProbeGenerationType);

#endif
