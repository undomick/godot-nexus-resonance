#ifndef RESONANCE_SOFA_ASSET_H
#define RESONANCE_SOFA_ASSET_H

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/binder_common.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

namespace godot {

/// Custom HRTF from a SOFA file, used through ResonanceRuntimeConfig.hrtf_sofa_assets.
class ResonanceSOFAAsset : public Resource {
    GDCLASS(ResonanceSOFAAsset, Resource)

  public:
    enum NormType {
        NORM_NONE = 0,
        NORM_RMS = 1
    };

  private:
    PackedByteArray sofa_data;
    float volume_db = 0.0f;
    int norm_type = NORM_NONE;

  protected:
    static void _bind_methods();

  public:
    ResonanceSOFAAsset() = default;
    ~ResonanceSOFAAsset() = default;

    void set_sofa_data(const PackedByteArray& p_data);
    PackedByteArray get_sofa_data() const;

    void set_volume_db(float p_db);
    float get_volume_db() const { return volume_db; }

    void set_norm_type(int p_type);
    int get_norm_type() const { return norm_type; }

    /// nullptr when sofa_data is empty.
    const uint8_t* get_data_ptr() const;
    int64_t get_size() const;

    bool is_valid() const { return !sofa_data.is_empty(); }

    Error load_from_file(const String& p_path);

    /// Returns 0 at or below kHRTFMinDB.
    static float db_to_gain(float db);
};

} // namespace godot

VARIANT_ENUM_CAST(ResonanceSOFAAsset::NormType);

#endif
