#include "resonance_material.h"
#include "resonance_constants.h"
#include <godot_cpp/core/class_db.hpp>

using namespace godot;

void ResonanceMaterial::set_low_freq_absorption(float p_val) {
    low_freq_absorption = CLAMP(p_val, 0.0f, 1.0f);
}
float ResonanceMaterial::get_low_freq_absorption() const { return low_freq_absorption; }

void ResonanceMaterial::set_mid_freq_absorption(float p_val) {
    mid_freq_absorption = CLAMP(p_val, 0.0f, 1.0f);
}
float ResonanceMaterial::get_mid_freq_absorption() const { return mid_freq_absorption; }

void ResonanceMaterial::set_high_freq_absorption(float p_val) {
    high_freq_absorption = CLAMP(p_val, 0.0f, 1.0f);
}
float ResonanceMaterial::get_high_freq_absorption() const { return high_freq_absorption; }

void ResonanceMaterial::set_scattering(float p_val) {
    scattering = CLAMP(p_val, 0.0f, 1.0f);
}
float ResonanceMaterial::get_scattering() const { return scattering; }

void ResonanceMaterial::set_transmission_low(float p_val) {
    transmission_low = CLAMP(p_val, 0.0f, 1.0f);
}
float ResonanceMaterial::get_transmission_low() const { return transmission_low; }

void ResonanceMaterial::set_transmission_mid(float p_val) {
    transmission_mid = CLAMP(p_val, 0.0f, 1.0f);
}
float ResonanceMaterial::get_transmission_mid() const { return transmission_mid; }

void ResonanceMaterial::set_transmission_high(float p_val) {
    transmission_high = CLAMP(p_val, 0.0f, 1.0f);
}
float ResonanceMaterial::get_transmission_high() const { return transmission_high; }

void ResonanceMaterial::set_transmission(float p_val) {
    float v = CLAMP(p_val, 0.0f, 1.0f);
    transmission_low = v;
    transmission_mid = v;
    transmission_high = v;
}
float ResonanceMaterial::get_transmission() const {
    return (transmission_low + transmission_mid + transmission_high) / 3.0f;
}

IPLMaterial ResonanceMaterial::get_ipl_material() const {
    static_assert(IPL_NUM_BANDS == 3, "ResonanceMaterial expects IPL_NUM_BANDS == 3");
    IPLMaterial mat{};
    mat.absorption[0] = low_freq_absorption;
    mat.absorption[1] = mid_freq_absorption;
    mat.absorption[2] = high_freq_absorption;
    mat.scattering = scattering;
    mat.transmission[0] = transmission_low;
    mat.transmission[1] = transmission_mid;
    mat.transmission[2] = transmission_high;
    return mat;
}

IPLMaterial ResonanceMaterial::default_ipl_material() {
    IPLMaterial mat{};
    mat.absorption[0] = resonance::kSceneExportAbsorptionLow;
    mat.absorption[1] = resonance::kSceneExportAbsorptionMid;
    mat.absorption[2] = resonance::kSceneExportAbsorptionHigh;
    mat.scattering = resonance::kSceneExportScattering;
    mat.transmission[0] = mat.transmission[1] = mat.transmission[2] = resonance::kSceneExportTransmission;
    return mat;
}

IPLMaterial ResonanceMaterial::ipl_material_or_default(const Ref<ResonanceMaterial>& p_material) {
    return p_material.is_valid() ? p_material->get_ipl_material() : default_ipl_material();
}

void ResonanceMaterial::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_low_freq_absorption", "p_val"), &ResonanceMaterial::set_low_freq_absorption);
    ClassDB::bind_method(D_METHOD("get_low_freq_absorption"), &ResonanceMaterial::get_low_freq_absorption);
    ClassDB::bind_method(D_METHOD("set_mid_freq_absorption", "p_val"), &ResonanceMaterial::set_mid_freq_absorption);
    ClassDB::bind_method(D_METHOD("get_mid_freq_absorption"), &ResonanceMaterial::get_mid_freq_absorption);
    ClassDB::bind_method(D_METHOD("set_high_freq_absorption", "p_val"), &ResonanceMaterial::set_high_freq_absorption);
    ClassDB::bind_method(D_METHOD("get_high_freq_absorption"), &ResonanceMaterial::get_high_freq_absorption);

    ClassDB::bind_method(D_METHOD("set_scattering", "p_val"), &ResonanceMaterial::set_scattering);
    ClassDB::bind_method(D_METHOD("get_scattering"), &ResonanceMaterial::get_scattering);

    ClassDB::bind_method(D_METHOD("set_transmission_low", "p_val"), &ResonanceMaterial::set_transmission_low);
    ClassDB::bind_method(D_METHOD("get_transmission_low"), &ResonanceMaterial::get_transmission_low);
    ClassDB::bind_method(D_METHOD("set_transmission_mid", "p_val"), &ResonanceMaterial::set_transmission_mid);
    ClassDB::bind_method(D_METHOD("get_transmission_mid"), &ResonanceMaterial::get_transmission_mid);
    ClassDB::bind_method(D_METHOD("set_transmission_high", "p_val"), &ResonanceMaterial::set_transmission_high);
    ClassDB::bind_method(D_METHOD("get_transmission_high"), &ResonanceMaterial::get_transmission_high);
    ClassDB::bind_method(D_METHOD("set_transmission", "p_val"), &ResonanceMaterial::set_transmission);
    ClassDB::bind_method(D_METHOD("get_transmission"), &ResonanceMaterial::get_transmission);

    ADD_GROUP("Absorption", "");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "absorption_low", PROPERTY_HINT_RANGE, "0,1"), "set_low_freq_absorption", "get_low_freq_absorption");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "absorption_mid", PROPERTY_HINT_RANGE, "0,1"), "set_mid_freq_absorption", "get_mid_freq_absorption");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "absorption_high", PROPERTY_HINT_RANGE, "0,1"), "set_high_freq_absorption", "get_high_freq_absorption");

    ADD_GROUP("Physics", "");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "scattering", PROPERTY_HINT_RANGE, "0,1"), "set_scattering", "get_scattering");
    ADD_GROUP("Transmission", "");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "transmission_low", PROPERTY_HINT_RANGE, "0,1"), "set_transmission_low", "get_transmission_low");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "transmission_mid", PROPERTY_HINT_RANGE, "0,1"), "set_transmission_mid", "get_transmission_mid");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "transmission_high", PROPERTY_HINT_RANGE, "0,1"), "set_transmission_high", "get_transmission_high");
    // Display-only average without storage usage; storing it would overwrite the per-band values on load.
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "transmission", PROPERTY_HINT_RANGE, "0,1", PROPERTY_USAGE_READ_ONLY), "", "get_transmission");
}