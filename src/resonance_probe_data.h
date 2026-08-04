#ifndef RESONANCE_PROBE_DATA_H
#define RESONANCE_PROBE_DATA_H

#include <cstdint>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

namespace godot {

class ResonanceProbeData : public Resource {
    GDCLASS(ResonanceProbeData, Resource)

  private:
    PackedByteArray internal_data;
    PackedVector3Array probe_positions; // Stored at bake time for visualization after restart
    uint32_t bake_params_hash = 0;
    uint32_t pathing_params_hash = 0;         // 0 = not baked
    uint32_t static_scene_params_hash = 0;    // Hash of static geometry asset used at bake; 0 = legacy
    uint32_t static_source_params_hash = 0;   // 0 = not baked
    uint32_t static_listener_params_hash = 0; // 0 = not baked
    int baked_reflection_type = -1;           // 0=Convolution, 1=Parametric, 2=Hybrid; -1=legacy (both baked)

  protected:
    static void _bind_methods();

  public:
    ResonanceProbeData();
    ~ResonanceProbeData();

    // Standard Setters/Getters for Godot serialization
    void set_data(const PackedByteArray& p_data);
    PackedByteArray get_data() const;

    void set_probe_positions(const PackedVector3Array& p_positions);
    PackedVector3Array get_probe_positions() const;

    void set_bake_params_hash(int64_t p_hash);
    int64_t get_bake_params_hash() const;

    void set_pathing_params_hash(int64_t p_hash);
    int64_t get_pathing_params_hash() const;

    void set_static_scene_params_hash(int64_t p_hash);
    int64_t get_static_scene_params_hash() const;

    void set_static_source_params_hash(int64_t p_hash);
    int64_t get_static_source_params_hash() const;

    void set_static_listener_params_hash(int64_t p_hash);
    int64_t get_static_listener_params_hash() const;

    void set_baked_reflection_type(int p_type);
    int get_baked_reflection_type() const;

    // C++ Helpers for efficient access
    /// Returns pointer to internal probe data. Valid only during the current call; do not hold across
    /// set_data() or other mutations – PackedByteArray may reallocate and invalidate the pointer.
    const uint8_t* get_data_ptr() const;
    int64_t get_size() const;

    /// Returns bake layer statistics for debug/editor. Dictionary with keys:
    /// "total_size" (int), "layers" (Array of {"type": String, "baked": bool, ...}).
    Dictionary get_bake_layer_info() const;
};

/// Returns [code]res[/code] or [code]tres[/code] from ProjectSettings [code]nexus/resonance/export/probe_data_format[/code].
String resonance_probe_data_save_extension_from_settings();

/// Bake output root from Project Settings (trailing slash). Default [code]res://resonance_data/[/code].
String resonance_bake_output_root_from_settings();

/// [code]{{root}}batches/[/code] under the bake output root (trailing slash).
String resonance_bake_batches_dir_from_settings();

} // namespace godot

#endif