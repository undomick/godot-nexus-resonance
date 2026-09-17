#include "resonance_reverb_data_point.h"

#include "resonance_probe_volume.h"
#include "resonance_reverb_data_point_query_policy.h"
#include "resonance_server.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>

using namespace godot;

void ResonanceReverbDataPoint::_bind_methods() {
    ClassDB::bind_method(D_METHOD("set_probe_data", "data"), &ResonanceReverbDataPoint::set_probe_data);
    ClassDB::bind_method(D_METHOD("get_probe_data"), &ResonanceReverbDataPoint::get_probe_data);
    ClassDB::bind_method(D_METHOD("set_baked_variation", "variation"), &ResonanceReverbDataPoint::set_baked_variation);
    ClassDB::bind_method(D_METHOD("get_baked_variation"), &ResonanceReverbDataPoint::get_baked_variation);
    ClassDB::bind_method(D_METHOD("set_static_endpoint", "endpoint"), &ResonanceReverbDataPoint::set_static_endpoint);
    ClassDB::bind_method(D_METHOD("get_static_endpoint"), &ResonanceReverbDataPoint::get_static_endpoint);
    ClassDB::bind_method(D_METHOD("set_static_influence_radius", "radius"), &ResonanceReverbDataPoint::set_static_influence_radius);
    ClassDB::bind_method(D_METHOD("get_static_influence_radius"), &ResonanceReverbDataPoint::get_static_influence_radius);
    ClassDB::bind_method(D_METHOD("set_neighbor_radius", "radius"), &ResonanceReverbDataPoint::set_neighbor_radius);
    ClassDB::bind_method(D_METHOD("get_neighbor_radius"), &ResonanceReverbDataPoint::get_neighbor_radius);
    ClassDB::bind_method(D_METHOD("set_reconstruct_impulse_response", "enabled"), &ResonanceReverbDataPoint::set_reconstruct_impulse_response);
    ClassDB::bind_method(D_METHOD("get_reconstruct_impulse_response"), &ResonanceReverbDataPoint::get_reconstruct_impulse_response);
    ClassDB::bind_method(D_METHOD("get_last_query"), &ResonanceReverbDataPoint::get_last_query);
    ClassDB::bind_method(D_METHOD("query_baked_reverb"), &ResonanceReverbDataPoint::query_baked_reverb);
    ClassDB::bind_method(D_METHOD("query_baked_reverb_at", "world_position"), &ResonanceReverbDataPoint::query_baked_reverb_at);

    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "probe_data", PROPERTY_HINT_RESOURCE_TYPE, "ResonanceProbeData"), "set_probe_data",
                 "get_probe_data");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "baked_variation", PROPERTY_HINT_ENUM, "Reverb:0,Static Source:1,Static Listener:2"), "set_baked_variation",
                 "get_baked_variation");
    ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "static_endpoint"), "set_static_endpoint", "get_static_endpoint");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "static_influence_radius", PROPERTY_HINT_RANGE, "0,100000,0.1,or_greater"), "set_static_influence_radius",
                 "get_static_influence_radius");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "neighbor_radius", PROPERTY_HINT_RANGE, "0.1,64,0.1,or_greater"), "set_neighbor_radius", "get_neighbor_radius");
    ADD_PROPERTY(
        PropertyInfo(Variant::BOOL, "reconstruct_impulse_response", PROPERTY_HINT_NONE,
                     "Build IR preview stats from the sampled energy field (analysis only; not used for audio)."),
        "set_reconstruct_impulse_response", "get_reconstruct_impulse_response");
    ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "last_query", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_SCRIPT_VARIABLE),
                 "", "get_last_query");
}

void ResonanceReverbDataPoint::_validate_property(PropertyInfo& p_property) const {
    if (baked_variation == 0) {
        if (p_property.name == StringName("static_endpoint") || p_property.name == StringName("static_influence_radius")) {
            p_property.usage &= ~PROPERTY_USAGE_EDITOR;
        }
    }
}

void ResonanceReverbDataPoint::_enter_tree() {
    _try_inherit_probe_data_from_ancestor();
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint())
        return;
    set_process(true);
}

void ResonanceReverbDataPoint::_process(double /*delta*/) {
    _try_play_query_once();
}

void ResonanceReverbDataPoint::_try_inherit_probe_data_from_ancestor() {
    if (probe_data.is_valid())
        return;
    Node* n = get_parent();
    while (n) {
        ResonanceProbeVolume* vol = Object::cast_to<ResonanceProbeVolume>(n);
        if (vol) {
            Ref<ResonanceProbeData> pd = vol->get_probe_data();
            if (pd.is_valid())
                probe_data = pd;
            return;
        }
        n = n->get_parent();
    }
}

void ResonanceReverbDataPoint::_try_play_query_once() {
    Engine* eng = Engine::get_singleton();
    const bool editor = eng && eng->is_editor_hint();
    ResonanceServer* srv = ResonanceServer::get_singleton();
    const bool server_ok = srv && srv->is_initialized();
    if (!resonance::reverb_data_point_should_query_once(editor, probe_data.is_valid(), server_ok, did_play_query_))
        return;
    did_play_query_ = true;
    query_baked_reverb();
    set_process(false);
}

void ResonanceReverbDataPoint::set_probe_data(const Ref<ResonanceProbeData>& p_data) {
    probe_data = p_data;
}

Ref<ResonanceProbeData> ResonanceReverbDataPoint::get_probe_data() const {
    return probe_data;
}

void ResonanceReverbDataPoint::set_baked_variation(int p_variation) {
    if (baked_variation == p_variation)
        return;
    baked_variation = p_variation;
    notify_property_list_changed();
}

int ResonanceReverbDataPoint::get_baked_variation() const {
    return baked_variation;
}

void ResonanceReverbDataPoint::set_static_endpoint(const Vector3& p_endpoint) {
    static_endpoint = p_endpoint;
}

Vector3 ResonanceReverbDataPoint::get_static_endpoint() const {
    return static_endpoint;
}

void ResonanceReverbDataPoint::set_static_influence_radius(float p_radius) {
    static_influence_radius = p_radius;
}

float ResonanceReverbDataPoint::get_static_influence_radius() const {
    return static_influence_radius;
}

void ResonanceReverbDataPoint::set_neighbor_radius(float p_radius) {
    neighbor_radius = p_radius;
    update_gizmos();
}

float ResonanceReverbDataPoint::get_neighbor_radius() const {
    return neighbor_radius;
}

void ResonanceReverbDataPoint::set_reconstruct_impulse_response(bool p_enabled) {
    reconstruct_impulse_response = p_enabled;
}

bool ResonanceReverbDataPoint::get_reconstruct_impulse_response() const {
    return reconstruct_impulse_response;
}

Dictionary ResonanceReverbDataPoint::get_last_query() const {
    return last_query;
}

Dictionary ResonanceReverbDataPoint::query_baked_reverb_at(const Vector3& world_position) {
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!srv) {
        Dictionary d;
        d["ok"] = false;
        d["error"] = "no_resonance_server";
        last_query = d;
        update_gizmos();
        return d;
    }
    if (probe_data.is_null()) {
        Dictionary d;
        d["ok"] = false;
        d["error"] = "probe_data_missing";
        last_query = d;
        update_gizmos();
        return d;
    }
    last_query = srv->probe_data_query_baked_at_point(probe_data, world_position, baked_variation, static_endpoint, static_influence_radius,
                                                      neighbor_radius, reconstruct_impulse_response);
    update_gizmos();
    return last_query;
}

Dictionary ResonanceReverbDataPoint::query_baked_reverb() {
    return query_baked_reverb_at(get_global_transform().origin);
}
