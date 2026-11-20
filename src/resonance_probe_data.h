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
    PackedVector3Array probe_positions; // Saved at bake time so probes can be drawn after a restart
    uint32_t bake_params_hash = 0;
    uint32_t pathing_params_hash = 0;         // 0 = not baked
    uint32_t static_scene_params_hash = 0;    // Static geometry asset used at bake; 0 = not recorded
    uint32_t static_source_params_hash = 0;   // 0 = not baked
    uint32_t static_listener_params_hash = 0; // 0 = not baked
    int baked_reflection_type = -1;           // kBakedReflection*; -1 = unknown, accepted by every runtime type
    int baked_ambisonics_order = -1;          // Order of the baked convolution IRs; -1 = unknown, default assumed

  protected:
    static void _bind_methods();

  public:
    ResonanceProbeData();
    ~ResonanceProbeData();

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

    void set_baked_ambisonics_order(int p_order);
    int get_baked_ambisonics_order() const;

    /// Invalidated by set_data() or any other mutation of the data.
    const uint8_t* get_data_ptr() const;
    int64_t get_size() const;

    /// Keys: "total_size" (int), "layers" (Array of {"type": String, "baked": bool, ...}).
    Dictionary get_bake_layer_info() const;
};

/// "res" or "tres", from the export/probe_data_format project setting.
String resonance_probe_data_save_extension_from_settings();

/// With trailing slash. Default res://resonance_data/.
String resonance_bake_output_root_from_settings();

/// batches/ under the bake output root, with trailing slash.
String resonance_bake_batches_dir_from_settings();

} // namespace godot

#endif