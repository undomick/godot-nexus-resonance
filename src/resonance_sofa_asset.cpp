#include "resonance_sofa_asset.h"
#include "resonance_constants.h"
#include <cmath>
#include <cstdint>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>

using namespace godot;

void ResonanceSOFAAsset::set_sofa_data(const PackedByteArray& p_data) {
    sofa_data = p_data;
}

PackedByteArray ResonanceSOFAAsset::get_sofa_data() const {
    return sofa_data;
}

void ResonanceSOFAAsset::set_volume_db(float p_db) {
    volume_db = CLAMP(p_db, resonance::kHRTFVolumeDBMin, resonance::kHRTFVolumeDBMax);
}

void ResonanceSOFAAsset::set_norm_type(int p_type) {
    norm_type = (p_type == NORM_RMS) ? NORM_RMS : NORM_NONE;
}

const uint8_t* ResonanceSOFAAsset::get_data_ptr() const {
    return sofa_data.ptr();
}

int64_t ResonanceSOFAAsset::get_size() const {
    return sofa_data.size();
}

Error ResonanceSOFAAsset::load_from_file(const String& p_path) {
    String path = p_path;
    if (path.begins_with("res://") || path.begins_with("user://")) {
        if (ProjectSettings* ps = ProjectSettings::get_singleton())
            path = ps->globalize_path(path);
    }
    Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
    if (f.is_null())
        return ERR_FILE_CANT_OPEN;
    const int64_t file_len = static_cast<int64_t>(f->get_length());
    sofa_data = f->get_buffer(file_len);
    return !sofa_data.is_empty() ? OK : ERR_FILE_CORRUPT;
}

float ResonanceSOFAAsset::db_to_gain(float db) {
    if (db <= resonance::kHRTFMinDB)
        return 0.0f;
    return static_cast<float>(std::pow(10.0, static_cast<double>(db) / 20.0));
}

void ResonanceSOFAAsset::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_sofa_data", "p_data"), &ResonanceSOFAAsset::set_sofa_data);
    ClassDB::bind_method(D_METHOD("get_sofa_data"), &ResonanceSOFAAsset::get_sofa_data);
    ClassDB::bind_method(D_METHOD("set_volume_db", "p_db"), &ResonanceSOFAAsset::set_volume_db);
    ClassDB::bind_method(D_METHOD("get_volume_db"), &ResonanceSOFAAsset::get_volume_db);
    ClassDB::bind_method(D_METHOD("set_norm_type", "p_type"), &ResonanceSOFAAsset::set_norm_type);
    ClassDB::bind_method(D_METHOD("get_norm_type"), &ResonanceSOFAAsset::get_norm_type);
    ClassDB::bind_method(D_METHOD("load_from_file", "p_path"), &ResonanceSOFAAsset::load_from_file);

    ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "sofa_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_sofa_data", "get_sofa_data");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "volume_db", PROPERTY_HINT_RANGE, "-60,24,0.5"), "set_volume_db", "get_volume_db");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "norm_type", PROPERTY_HINT_ENUM, "NORM_NONE,NORM_RMS"), "set_norm_type", "get_norm_type");

    BIND_ENUM_CONSTANT(NORM_NONE);
    BIND_ENUM_CONSTANT(NORM_RMS);
}
