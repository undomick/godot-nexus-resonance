#include "resonance_probe_data.h"
#include "resonance_constants.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>

namespace godot {

String resonance_probe_data_save_extension_from_settings() {
    ProjectSettings* ps = ProjectSettings::get_singleton();
    if (!ps)
        return String("tres");
    const String key = String(resonance::kProjectSettingsResonancePrefix) + String(resonance::kProjectSettingsProbeDataFormat);
    if (!ps->has_setting(key))
        return String("tres");
    const Variant vv = ps->get_setting(key);
    if (vv.get_type() == Variant::NIL)
        return String("tres");
    if (vv.get_type() != Variant::INT && vv.get_type() != Variant::FLOAT)
        return String("tres");
    const int v = static_cast<int>(vv.operator int64_t());
    return (v == 1) ? String("res") : String("tres");
}

String resonance_bake_output_root_from_settings() {
    String base_dir = String(resonance::kProbeBakeOutputDir);
    ProjectSettings* ps = ProjectSettings::get_singleton();
    if (ps) {
        const String prefix = String(resonance::kProjectSettingsResonancePrefix);
        const String key_new = prefix + String(resonance::kProjectSettingsBakeDefaultOutputDirectory);
        const String key_old = prefix + String(resonance::kProjectSettingsBakeOutputDirectoryLegacy);
        if (ps->has_setting(key_new)) {
            base_dir = String(ps->get_setting(key_new));
        } else if (ps->has_setting(key_old)) {
            base_dir = String(ps->get_setting(key_old));
        }
    }
    if (base_dir.is_empty()) {
        base_dir = String(resonance::kProbeBakeOutputDir);
    }
    if (!base_dir.ends_with("/")) {
        base_dir += "/";
    }
    return base_dir;
}

String resonance_bake_batches_dir_from_settings() {
    return resonance_bake_output_root_from_settings() + String(resonance::kProbeBakeBatchesSubdir);
}

} // namespace godot

using namespace godot;

ResonanceProbeData::ResonanceProbeData() {}
ResonanceProbeData::~ResonanceProbeData() {}

void ResonanceProbeData::set_data(const PackedByteArray& p_data) {
    internal_data = p_data;
}

PackedByteArray ResonanceProbeData::get_data() const {
    return internal_data;
}

void ResonanceProbeData::set_probe_positions(const PackedVector3Array& p_positions) {
    probe_positions = p_positions;
}

PackedVector3Array ResonanceProbeData::get_probe_positions() const {
    return probe_positions;
}

void ResonanceProbeData::set_bake_params_hash(int64_t p_hash) {
    bake_params_hash = static_cast<uint32_t>(p_hash & 0xFFFFFFFFu);
}

void ResonanceProbeData::set_pathing_params_hash(int64_t p_hash) {
    pathing_params_hash = static_cast<uint32_t>(p_hash & 0xFFFFFFFFu);
}

int64_t ResonanceProbeData::get_pathing_params_hash() const {
    return static_cast<int64_t>(pathing_params_hash);
}

void ResonanceProbeData::set_static_scene_params_hash(int64_t p_hash) {
    static_scene_params_hash = static_cast<uint32_t>(p_hash & 0xFFFFFFFFu);
}

int64_t ResonanceProbeData::get_static_scene_params_hash() const {
    return static_cast<int64_t>(static_scene_params_hash);
}

void ResonanceProbeData::set_static_source_params_hash(int64_t p_hash) {
    static_source_params_hash = static_cast<uint32_t>(p_hash & 0xFFFFFFFFu);
}

int64_t ResonanceProbeData::get_static_source_params_hash() const {
    return static_cast<int64_t>(static_source_params_hash);
}

void ResonanceProbeData::set_static_listener_params_hash(int64_t p_hash) {
    static_listener_params_hash = static_cast<uint32_t>(p_hash & 0xFFFFFFFFu);
}

int64_t ResonanceProbeData::get_static_listener_params_hash() const {
    return static_cast<int64_t>(static_listener_params_hash);
}

void ResonanceProbeData::set_baked_reflection_type(int p_type) {
    baked_reflection_type = (p_type >= 0 && p_type <= 2) ? p_type : -1;
}

int ResonanceProbeData::get_baked_reflection_type() const {
    return baked_reflection_type;
}

int64_t ResonanceProbeData::get_bake_params_hash() const {
    return static_cast<int64_t>(bake_params_hash);
}

const uint8_t* ResonanceProbeData::get_data_ptr() const {
    return internal_data.ptr();
}

int64_t ResonanceProbeData::get_size() const {
    return internal_data.size();
}

Dictionary ResonanceProbeData::get_bake_layer_info() const {
    Dictionary info;
    info["total_size"] = static_cast<int64_t>(internal_data.size());

    Array layers;
    Dictionary refl;
    refl["type"] = String("reflections");
    refl["baked"] = (baked_reflection_type >= 0);
    if (baked_reflection_type >= 0) {
        refl["reflection_type"] = baked_reflection_type;
    }
    layers.push_back(refl);

    Dictionary path;
    path["type"] = String("pathing");
    path["baked"] = (pathing_params_hash > 0);
    layers.push_back(path);

    Dictionary st_src;
    st_src["type"] = String("static_source");
    st_src["baked"] = (static_source_params_hash > 0);
    layers.push_back(st_src);

    Dictionary st_list;
    st_list["type"] = String("static_listener");
    st_list["baked"] = (static_listener_params_hash > 0);
    layers.push_back(st_list);

    info["layers"] = layers;
    return info;
}

void ResonanceProbeData::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_data", "p_data"), &ResonanceProbeData::set_data);
    ClassDB::bind_method(D_METHOD("get_data"), &ResonanceProbeData::get_data);
    ClassDB::bind_method(D_METHOD("set_probe_positions", "p_positions"), &ResonanceProbeData::set_probe_positions);
    ClassDB::bind_method(D_METHOD("get_probe_positions"), &ResonanceProbeData::get_probe_positions);
    ClassDB::bind_method(D_METHOD("set_bake_params_hash", "p_hash"), &ResonanceProbeData::set_bake_params_hash);
    ClassDB::bind_method(D_METHOD("get_bake_params_hash"), &ResonanceProbeData::get_bake_params_hash);
    ClassDB::bind_method(D_METHOD("set_pathing_params_hash", "p_hash"), &ResonanceProbeData::set_pathing_params_hash);
    ClassDB::bind_method(D_METHOD("get_pathing_params_hash"), &ResonanceProbeData::get_pathing_params_hash);
    ClassDB::bind_method(D_METHOD("set_static_scene_params_hash", "p_hash"), &ResonanceProbeData::set_static_scene_params_hash);
    ClassDB::bind_method(D_METHOD("get_static_scene_params_hash"), &ResonanceProbeData::get_static_scene_params_hash);
    ClassDB::bind_method(D_METHOD("set_static_source_params_hash", "p_hash"), &ResonanceProbeData::set_static_source_params_hash);
    ClassDB::bind_method(D_METHOD("get_static_source_params_hash"), &ResonanceProbeData::get_static_source_params_hash);
    ClassDB::bind_method(D_METHOD("set_static_listener_params_hash", "p_hash"), &ResonanceProbeData::set_static_listener_params_hash);
    ClassDB::bind_method(D_METHOD("get_static_listener_params_hash"), &ResonanceProbeData::get_static_listener_params_hash);
    ClassDB::bind_method(D_METHOD("set_baked_reflection_type", "p_type"), &ResonanceProbeData::set_baked_reflection_type);
    ClassDB::bind_method(D_METHOD("get_baked_reflection_type"), &ResonanceProbeData::get_baked_reflection_type);
    ClassDB::bind_method(D_METHOD("get_bake_layer_info"), &ResonanceProbeData::get_bake_layer_info);

    // Using PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR ensures it's saved to disk
    // but typically huge byte arrays are hidden from the Inspector to prevent lag,
    // unless explicitly needed. We hide it from the inspector view but keep storage.
    ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_data", "get_data");
    ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR3_ARRAY, "probe_positions", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_probe_positions", "get_probe_positions");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "bake_params_hash", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_bake_params_hash", "get_bake_params_hash");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "pathing_params_hash", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_pathing_params_hash", "get_pathing_params_hash");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "static_scene_params_hash", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_static_scene_params_hash", "get_static_scene_params_hash");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "static_source_params_hash", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_static_source_params_hash", "get_static_source_params_hash");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "static_listener_params_hash", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_static_listener_params_hash", "get_static_listener_params_hash");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "baked_reflection_type", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_baked_reflection_type", "get_baked_reflection_type");
}
