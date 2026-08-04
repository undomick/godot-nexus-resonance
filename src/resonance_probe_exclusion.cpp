#include "resonance_probe_exclusion.h"
#include "resonance_probe_volume.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

ResonanceProbeExclusion::ResonanceProbeExclusion() {
    set_notify_transform(true);
}

void ResonanceProbeExclusion::_ready() {
    add_to_group("resonance_probe_exclusion");
    _notify_parent_probe_volume();
}

void ResonanceProbeExclusion::_notification(int p_what) {
    if (p_what == NOTIFICATION_TRANSFORM_CHANGED || p_what == NOTIFICATION_ENTER_TREE ||
        p_what == NOTIFICATION_EXIT_TREE) {
        _notify_parent_probe_volume();
    }
}

void ResonanceProbeExclusion::_notify_parent_probe_volume() {
    Node* n = get_parent();
    while (n) {
        ResonanceProbeVolume* vol = Object::cast_to<ResonanceProbeVolume>(n);
        if (vol) {
            vol->notify_exclusion_changed();
            return;
        }
        n = n->get_parent();
    }
}

void ResonanceProbeExclusion::set_region_size(Vector3 p_size) {
    region_size.x = MAX(p_size.x, resonance::kProbeRegionSizeMin);
    region_size.y = MAX(p_size.y, resonance::kProbeRegionSizeMin);
    region_size.z = MAX(p_size.z, resonance::kProbeRegionSizeMin);
    update_gizmos();
    _notify_parent_probe_volume();
}

Vector3 ResonanceProbeExclusion::get_region_size() const {
    return region_size;
}

void ResonanceProbeExclusion::set_enabled(bool p_enabled) {
    if (enabled == p_enabled) {
        return;
    }
    enabled = p_enabled;
    update_gizmos();
    _notify_parent_probe_volume();
}

bool ResonanceProbeExclusion::is_enabled() const {
    return enabled;
}

void ResonanceProbeExclusion::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_region_size", "p_size"), &ResonanceProbeExclusion::set_region_size);
    ClassDB::bind_method(D_METHOD("get_region_size"), &ResonanceProbeExclusion::get_region_size);
    ClassDB::bind_method(D_METHOD("set_enabled", "p_enabled"), &ResonanceProbeExclusion::set_enabled);
    ClassDB::bind_method(D_METHOD("is_enabled"), &ResonanceProbeExclusion::is_enabled);

    ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "region_size"), "set_region_size", "get_region_size");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
}
