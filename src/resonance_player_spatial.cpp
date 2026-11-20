#include "resonance_ambisonics_decode_binaural_policy.h"
#include "resonance_attenuation_callback_policy.h"
#include "resonance_constants.h"
#include "resonance_listener_attenuation_policy.h"
#include "resonance_listener_coords_util.h"
#include "resonance_log.h"
#include "resonance_math.h"
#include "resonance_playback_attenuation_policy.h"
#include "resonance_playback_lod_policy.h"
#include "resonance_player.h"
#include "resonance_player_reflections_policy.h"
#include "resonance_probe_volume.h"
#include "resonance_server.h"
#include "resonance_sim_distance_attenuation_policy.h"
#include "resonance_source_update_policy.h"
#include "resonance_utils.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/collision_object3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector4.hpp>
#include <limits>
#include <sstream>

using namespace godot;

void ResonancePlayer::_resolve_playback_occ_tx_directivity(const OcclusionData& occ_data, double delta_seconds,
                                                           bool reset_smooth_state_when_not_smoothing,
                                                           float& occ_val, float& tx_low, float& tx_mid, float& tx_high,
                                                           float& directivity_val) {
    const ConfigCache& c = config_cache_;

    occ_val = (c.occlusion_input == 1) ? CLAMP(c.occlusion_value, 0.0f, 1.0f) : occ_data.occlusion;
    tx_low = (c.transmission_input == 1) ? CLAMP(c.transmission_low, 0.0f, 1.0f) : occ_data.transmission[0];
    tx_mid = (c.transmission_input == 1) ? CLAMP(c.transmission_mid, 0.0f, 1.0f) : occ_data.transmission[1];
    tx_high = (c.transmission_input == 1) ? CLAMP(c.transmission_high, 0.0f, 1.0f) : occ_data.transmission[2];
    if (c.occlusion_input == 0 && !c.simulation_occlusion_enabled)
        occ_val = 1.0f;
    if (c.transmission_input == 0 && !c.simulation_transmission_enabled) {
        tx_low = 1.0f;
        tx_mid = 1.0f;
        tx_high = 1.0f;
    }
    directivity_val = (c.directivity_input == 1) ? CLAMP(c.directivity_value, 0.0f, 1.0f) : occ_data.directivity;

    const float tau = c.playback_coeff_smoothing_time;
    const bool smooth_occ = (tau > 0.0f) && (c.occlusion_input == 0);
    const bool smooth_tx = (tau > 0.0f) && (c.transmission_input == 0);
    if (smooth_occ || smooth_tx) {
        const bool reinit = !coeff_smooth_initialized_ || (coeff_smooth_source_handle_ != source_handle);
        if (reinit) {
            if (smooth_occ)
                coeff_smooth_occ_ = occ_val;
            if (smooth_tx) {
                coeff_smooth_tx_[0] = tx_low;
                coeff_smooth_tx_[1] = tx_mid;
                coeff_smooth_tx_[2] = tx_high;
            }
            coeff_smooth_initialized_ = true;
            coeff_smooth_source_handle_ = source_handle;
        } else {
            float alpha = 1.0f;
            if (delta_seconds > 0.0 && std::isfinite(static_cast<double>(tau)) && tau > 0.0f) {
                const double t = std::max(static_cast<double>(tau), 1.0e-6);
                alpha = 1.0f - static_cast<float>(std::exp(-delta_seconds / t));
            }
            if (smooth_occ)
                coeff_smooth_occ_ += alpha * (occ_val - coeff_smooth_occ_);
            if (smooth_tx) {
                coeff_smooth_tx_[0] += alpha * (tx_low - coeff_smooth_tx_[0]);
                coeff_smooth_tx_[1] += alpha * (tx_mid - coeff_smooth_tx_[1]);
                coeff_smooth_tx_[2] += alpha * (tx_high - coeff_smooth_tx_[2]);
            }
        }
        if (smooth_occ)
            occ_val = std::clamp(coeff_smooth_occ_, 0.0f, 1.0f);
        if (smooth_tx) {
            tx_low = std::clamp(coeff_smooth_tx_[0], 0.0f, 1.0f);
            tx_mid = std::clamp(coeff_smooth_tx_[1], 0.0f, 1.0f);
            tx_high = std::clamp(coeff_smooth_tx_[2], 0.0f, 1.0f);
        }
    } else if (reset_smooth_state_when_not_smoothing) {
        coeff_smooth_initialized_ = false;
    }
}

float ResonancePlayer::_config_float(const char* key, float default_val) const {
    if (!player_config.is_valid())
        return default_val;
    Variant v = player_config->get(StringName(key));
    return (v.get_type() != Variant::NIL) ? (float)v : default_val;
}
int ResonancePlayer::_config_int(const char* key, int default_val) const {
    if (!player_config.is_valid())
        return default_val;
    Variant v = player_config->get(StringName(key));
    return (v.get_type() != Variant::NIL) ? (int)v : default_val;
}
bool ResonancePlayer::_config_bool(const char* key, bool default_val) const {
    if (!player_config.is_valid())
        return default_val;
    Variant v = player_config->get(StringName(key));
    return (v.get_type() != Variant::NIL) ? (bool)v : default_val;
}

bool ResonancePlayer::internal_wants_wet_effects() const {
    if (!player_config.is_valid())
        return true;
    ResonanceServer* srv = ResonanceServer::get_singleton();
    const bool path_global = srv && srv->is_pathing_enabled();
    if (config_cache_valid_)
        return resonance::playback_wants_wet_effects(config_cache_.reflections_enabled,
                                                     config_cache_.pathing_enabled_override, path_global);
    return resonance::playback_wants_wet_effects(_config_bool("reflections_enabled", false) ? 1 : 0,
                                                 _config_int("pathing_enabled_override", -1), path_global);
}

bool ResonancePlayer::internal_wants_ambisonics_encode() const {
    if (config_cache_valid_)
        return config_cache_.use_ambisonics_encode;
    return _config_bool("use_ambisonics_encode", false);
}

bool ResonancePlayer::_playback_wet_enabled(const ResonanceServer* srv) const {
    if (!resonance::playback_enable_reverb(!srv || srv->is_output_reverb_enabled()))
        return false;
    const ConfigCache& c = config_cache_;
    return resonance::playback_wants_wet_effects(c.reflections_enabled, c.pathing_enabled_override,
                                                 srv && srv->is_pathing_enabled());
}

ResonanceStreamPlayback* ResonancePlayer::_get_resonance_playback() {
    if (!is_playing())
        return nullptr;
    Ref<AudioStreamPlayback> pb = get_stream_playback();
    return pb.is_valid() ? Object::cast_to<ResonanceStreamPlayback>(pb.ptr()) : nullptr;
}

Ref<Curve> ResonancePlayer::_config_curve(const char* key, const Ref<Curve>& default_val) const {
    if (!player_config.is_valid())
        return default_val;
    Variant v = player_config->get(StringName(key));
    if (v.get_type() == Variant::OBJECT) {
        Ref<Curve> r = v;
        if (r.is_valid())
            return r;
    }
    return default_val;
}

// Copies the player config into config_cache_ for the hot paths, mapping legacy keys and
// "use global" values and clamping ranges.
void ResonancePlayer::_refresh_config_cache() {
    if (!player_config.is_valid())
        return;
    config_cache_.min_distance = _config_float("min_distance", 1.0f);
    config_cache_.max_distance = _config_float("max_distance", 500.0f);
    {
        // occlusion_radius is an alias of source_radius and wins when set.
        Variant occ_r = player_config->get(StringName("occlusion_radius"));
        if (occ_r.get_type() != Variant::NIL)
            config_cache_.source_radius = (float)occ_r;
        else
            config_cache_.source_radius = _config_float("source_radius", 1.0f);
    }
    config_cache_.distance_attenuation = _config_bool("distance_attenuation", true);
    config_cache_.use_distance_curve_for_reflections = _config_bool("use_distance_curve_for_reflections", false);
    config_cache_.reverb_send = _config_bool("reverb_send", true);
    const bool legacy_dist_sim = _config_bool("distance_attenuation_simulation_enabled", true);
    int am = _config_int("attenuation_mode", 0);
    if (am == 0 && !legacy_dist_sim)
        am = ATTENUATION_DISABLED;
    if (am < ATTENUATION_INVERSE || am > ATTENUATION_DISABLED)
        am = ATTENUATION_INVERSE;
    config_cache_.attenuation_mode = am;
    config_cache_.linear_curve_use_sim_distance_attenuation = legacy_dist_sim;
    config_cache_.attenuation_curve = _config_curve("attenuation_curve", Ref<Curve>());
    config_cache_.air_absorption_enabled = _config_bool("air_absorption_enabled", true);
    config_cache_.air_absorption_input = _config_int("air_absorption_input", 0);
    config_cache_.air_absorption_low = _config_float("air_absorption_low", 1.0f);
    config_cache_.air_absorption_mid = _config_float("air_absorption_mid", 1.0f);
    config_cache_.air_absorption_high = _config_float("air_absorption_high", 1.0f);
    config_cache_.direct_binaural_override = _config_int("direct_binaural_override", -1);
    config_cache_.directivity_enabled = _config_bool("directivity_enabled", false);
    config_cache_.directivity_weight = _config_float("directivity_weight", 0.0f);
    config_cache_.directivity_power = _config_float("directivity_power", 1.0f);
    config_cache_.spatial_blend = _config_float("spatial_blend", 1.0f);
    config_cache_.use_ambisonics_encode = _config_bool("use_ambisonics_encode", false);
    auto read_tri_state = [this](const char* override_key, const char* legacy_bool_key) -> int {
        if (!player_config.is_valid())
            return -1;
        Variant v_ov = player_config->get(StringName(override_key));
        if (v_ov.get_type() != Variant::NIL) {
            int o = (int)v_ov;
            if (o < -1 || o > 1)
                o = -1;
            return o;
        }
        Variant v_leg = player_config->get(StringName(legacy_bool_key));
        if (v_leg.get_type() != Variant::NIL)
            return (bool)v_leg ? 1 : 0;
        return -1;
    };
    config_cache_.path_validation_override = read_tri_state("path_validation_override", "path_validation_enabled");
    config_cache_.find_alternate_paths_override = read_tri_state("find_alternate_paths_override", "find_alternate_paths");
    config_cache_.reflections_type = _config_int("reflections_type", resonance::kPlayerReflectionsRealtime);
    config_cache_.reflections_enabled = _config_bool("reflections_enabled", false) ? 1 : 0;
    config_cache_.pathing_enabled_override = _config_int("pathing_enabled_override", -1);
    {
        auto read_tri_state_hrtf = [this](const char* new_key, const char* legacy_key) -> int {
            if (!player_config.is_valid())
                return -1;
            Variant v_new = player_config->get(StringName(new_key));
            if (v_new.get_type() != Variant::NIL) {
                int o = static_cast<int>(v_new);
                if (o < -1 || o > 1)
                    o = -1;
                return o;
            }
            return _config_int(legacy_key, -1);
        };
        config_cache_.reverb_binaural_override = read_tri_state_hrtf("reverb_binaural_override", "apply_hrtf_to_reflections");
        config_cache_.pathing_binaural_override = read_tri_state_hrtf("pathing_binaural_override", "apply_hrtf_to_pathing");
    }
    config_cache_.occlusion_input = _config_int("occlusion_input", 0);
    config_cache_.transmission_input = _config_int("transmission_input", 0);
    config_cache_.directivity_input = _config_int("directivity_input", 0);
    config_cache_.occlusion_value = _config_float("occlusion_value", 1.0f);
    config_cache_.transmission_low = _config_float("transmission_low", 1.0f);
    config_cache_.transmission_mid = _config_float("transmission_mid", 1.0f);
    config_cache_.transmission_high = _config_float("transmission_high", 1.0f);
    config_cache_.directivity_value = _config_float("directivity_value", 1.0f);
    config_cache_.occlusion_samples = _config_int("occlusion_samples", resonance::kDefaultOcclusionSamples);
    {
        ResonanceServer* srv = ResonanceServer::get_singleton();
        int tx_surfaces_def = srv ? srv->get_max_transmission_surfaces() : resonance::kDefaultPlayerConfigTransmissionRays;
        int mts_ov = _config_int("max_transmission_surfaces_override", 0);
        if (mts_ov == -1)
            mts_ov = 0; // older resources store Use Global as -1
        if (mts_ov != 0 && mts_ov != 1)
            mts_ov = 0;
        config_cache_.max_transmission_surfaces_override = mts_ov;
        if (mts_ov == 0) {
            config_cache_.max_transmission_surfaces = tx_surfaces_def;
        } else {
            int mts = _config_int("max_transmission_surfaces", tx_surfaces_def);
            if (mts < 1)
                mts = 1;
            if (mts > resonance::kMaxTransmissionRays)
                mts = resonance::kMaxTransmissionRays;
            config_cache_.max_transmission_surfaces = mts;
        }
    }
    config_cache_.master_mix_level = _config_float("master_mix_level", 1.0f);
    config_cache_.direct_mix_level = _config_float("direct_mix_level", 1.0f);
    config_cache_.reflections_mix_level = _config_float("reflections_mix_level", 1.0f);
    config_cache_.pathing_mix_level = _config_float("pathing_mix_level", 1.0f);
    config_cache_.reflections_eq_low = _config_float("reflections_eq_low", 1.0f);
    config_cache_.reflections_eq_mid = _config_float("reflections_eq_mid", 1.0f);
    config_cache_.reflections_eq_high = _config_float("reflections_eq_high", 1.0f);
    config_cache_.reflections_delay = _config_int("reflections_delay", -1);
    config_cache_.perspective_override = _config_int("perspective_correction_override", -1);
    config_cache_.perspective_factor = _config_float("perspective_factor", 1.0f);
    config_cache_.playback_parameter_min_interval = _config_float("playback_parameter_min_interval", 0.0f);
    config_cache_.playback_parameter_min_move = _config_float("playback_parameter_min_move", 0.0f);
    config_cache_.playback_coeff_smoothing_time = _config_float("playback_coeff_smoothing_time", 0.0f);
    config_cache_.simulation_occlusion_enabled = _config_bool("simulation_occlusion_enabled", true);
    config_cache_.simulation_transmission_enabled = _config_bool("simulation_transmission_enabled", true);
    {
        int occ_ov = _config_int("occlusion_type_override", 2);
        // 2 = Use Global, keeping .tres values non-negative; older resources store -1.
        if (occ_ov == 2 || occ_ov == -1)
            config_cache_.occlusion_type_override = -1;
        else if (occ_ov == 0 || occ_ov == 1)
            config_cache_.occlusion_type_override = occ_ov;
        else
            config_cache_.occlusion_type_override = -1;
    }
    config_cache_.transmission_type_override = _config_int("transmission_type_override", -1);
    if (config_cache_.transmission_type_override < -1 || config_cache_.transmission_type_override > 1)
        config_cache_.transmission_type_override = -1;
    config_cache_.hrtf_interpolation_override = _config_int("hrtf_interpolation_override", -1);
    if (config_cache_.hrtf_interpolation_override < -1 || config_cache_.hrtf_interpolation_override > 1)
        config_cache_.hrtf_interpolation_override = -1;
    config_cache_valid_ = true;
    attenuation_setup_cache_.valid = false;
}

bool ResonancePlayer::_steam_sim_distance_attenuation_enabled(const ConfigCache& c) {
    // Only Inverse sets the direct simulation flag; Linear and Curve attenuate the direct path at playback.
    return resonance::sim_direct_distance_attenuation_flag(c.distance_attenuation, c.attenuation_mode);
}
void ResonancePlayer::_ensure_config_valid() {
    if (config_cache_frame_countdown <= 0 || !config_cache_valid_) {
        _refresh_config_cache();
        _sync_bus_routing_with_config();
        config_cache_frame_countdown = kConfigCacheRefreshInterval;
    }
}

void ResonancePlayer::_ensure_config_and_apply_source(int32_t pathing_batch) {
    _ensure_config_valid();
    _apply_update_source(pathing_batch, false);
}

ResonanceServer::SourceUpdateParams ResonancePlayer::_build_source_update_params(ResonanceServer* srv, int32_t pathing_batch) const {
    const ConfigCache& c = config_cache_;
    Transform3D gt = get_global_transform();
    const Vector3 forward = -gt.basis.get_column(2);
    const Vector3 up = gt.basis.get_column(1);
    const int baked_var = resonance::player_reflections_baked_data_variation(c.reflections_type);

    // Static Source: this player's pose. Static Listener: active listener pose.
    Vector3 baked_center = get_global_position();
    if (baked_var == 2) {
        if (srv && srv->is_initialized())
            baked_center = resonance::listener_origin_godot(srv->get_current_listener_coords());
    }

    const bool eff_path_validation = (c.path_validation_override == -1) ? srv->get_default_path_validation_enabled()
                                                                        : (c.path_validation_override != 0);
    const bool eff_find_alternate = (c.find_alternate_paths_override == -1) ? srv->get_default_find_alternate_paths()
                                                                            : (c.find_alternate_paths_override != 0);

    ResonanceServer::SourceUpdateParams params;
    params.position = get_global_position();
    params.radius = c.source_radius;
    params.source_forward = forward;
    params.source_up = up;
    params.directivity_weight = c.directivity_weight;
    params.directivity_power = c.directivity_power;
    params.air_absorption_enabled = c.air_absorption_enabled && (c.air_absorption_input == 0);
    params.use_sim_distance_attenuation = _steam_sim_distance_attenuation_enabled(c);
    params.min_distance = c.min_distance;
    params.distance_attenuation = c.distance_attenuation && c.attenuation_mode != ATTENUATION_DISABLED;
    params.attenuation_mode = c.attenuation_mode;
    params.use_distance_curve_for_reflections =
        c.use_distance_curve_for_reflections &&
        resonance::use_distance_curve_for_reflections_editable(c.distance_attenuation, c.attenuation_mode);
    params.path_validation_enabled = eff_path_validation;
    params.find_alternate_paths = eff_find_alternate;
    params.occlusion_samples = c.occlusion_samples;
    params.num_transmission_rays = c.max_transmission_surfaces;
    params.baked_data_variation = baked_var;
    params.baked_endpoint_center = baked_center;
    params.baked_endpoint_radius = resonance::kBakedEndpointRadius;
    params.pathing_probe_batch_handle = pathing_batch;
    params.simulation_active = _source_simulation_wanted();
    params.reflections_enabled_override = c.reflections_enabled;
    params.pathing_enabled_override = c.pathing_enabled_override;
    params.occlusion_type_override = c.occlusion_type_override;
    params.simulation_occlusion_enabled = c.simulation_occlusion_enabled;
    params.simulation_transmission_enabled = c.simulation_transmission_enabled;
    {
        const float master = resonance::sanitize_audio_float(c.master_mix_level);
        params.direct_mix_level = resonance::sanitize_audio_float(c.direct_mix_level * master);
        params.reflections_mix_level = resonance::sanitize_audio_float(c.reflections_mix_level * master);
        params.pathing_mix_level = resonance::sanitize_audio_float(c.pathing_mix_level * master);
    }
    return params;
}

void ResonancePlayer::_apply_update_source(int32_t pathing_batch, bool defer_if_sim_mutex_busy) {
    ResonanceServer* srv = ResonanceServer::get_singleton();
    _invalidate_source_handle_if_stale(srv);
    _ensure_voice_sources(srv);
    if (!srv)
        return;
    std::vector<int32_t> handles;
    _collect_simulation_source_handles(handles);
    if (handles.empty())
        return;

    const ResonanceServer::SourceUpdateParams params = _build_source_update_params(srv, pathing_batch);
    bool delivered = true;
    for (int32_t handle : handles) {
        if (!defer_if_sim_mutex_busy) {
            srv->update_source(handle, params);
            continue;
        }
        if (srv->uses_batch_source_updates())
            srv->enqueue_source_update(handle, params);
        else if (!srv->try_update_source(handle, params))
            delivered = false;
    }
    // A dropped activity change must stay unpushed so the next frame's sync sends it again.
    if (delivered)
        source_simulation_active_pushed_ = params.simulation_active;
}

void ResonancePlayer::_setup_attenuation(ResonanceServer* srv) {
    if (!srv)
        return;
    std::vector<int32_t> handles;
    _collect_simulation_source_handles(handles);
    if (handles.empty())
        return;
    const ConfigCache& c = config_cache_;
    int mode_for_server = 0;
    float curve_local[resonance::kAttenuationCurveSamples]{};
    int num_curve = 0;
    if (c.attenuation_mode == ATTENUATION_LINEAR || c.attenuation_mode == ATTENUATION_CUSTOM_CURVE) {
        mode_for_server = c.attenuation_mode;
        const int n = resonance::kAttenuationCurveSamples;
        num_curve = n;
        if (c.attenuation_mode == ATTENUATION_LINEAR) {
            for (int i = 0; i < n; i++)
                curve_local[i] = 1.0f - (float)i / (n - 1);
        } else if (c.attenuation_curve.is_valid()) {
            for (int i = 0; i < n; i++) {
                float t = (float)i / (n - 1);
                curve_local[i] = c.attenuation_curve->sample(t);
            }
        } else {
            for (int i = 0; i < n; i++)
                curve_local[i] = (i == 0) ? 1.0f : 0.0f;
        }
    } else if (c.attenuation_mode == ATTENUATION_INVERSE || c.attenuation_mode == ATTENUATION_DISABLED) {
        mode_for_server = 0;
        num_curve = 0;
    } else {
        return;
    }

    const bool cache_hit =
        attenuation_setup_cache_.valid &&
        resonance::attenuation_callback_data_equal(attenuation_setup_cache_.mode, attenuation_setup_cache_.min_distance,
                                                   attenuation_setup_cache_.max_distance, attenuation_setup_cache_.curve_samples,
                                                   attenuation_setup_cache_.num_curve_samples, mode_for_server, c.min_distance,
                                                   c.max_distance, curve_local, num_curve);
    if (!cache_hit)
        attenuation_callback_handles_.clear();

    const int apply_mode = cache_hit ? attenuation_setup_cache_.mode : mode_for_server;
    const float apply_min = cache_hit ? attenuation_setup_cache_.min_distance : c.min_distance;
    const float apply_max = cache_hit ? attenuation_setup_cache_.max_distance : c.max_distance;
    const int apply_num_curve = cache_hit ? attenuation_setup_cache_.num_curve_samples : num_curve;
    const float* apply_curve = cache_hit ? attenuation_setup_cache_.curve_samples : curve_local;

    PackedFloat32Array curve_samples;
    if (apply_num_curve > 0) {
        curve_samples.resize(apply_num_curve);
        for (int i = 0; i < apply_num_curve; i++)
            curve_samples[i] = apply_curve[i];
    }
    for (int32_t handle : handles) {
        bool already = false;
        for (int32_t applied : attenuation_callback_handles_) {
            if (applied == handle) {
                already = true;
                break;
            }
        }
        if (already)
            continue;
        srv->set_source_attenuation_callback_data(handle, apply_mode, apply_min, apply_max, curve_samples);
        attenuation_callback_handles_.push_back(handle);
    }
    if (cache_hit)
        return;

    attenuation_setup_cache_.mode = mode_for_server;
    attenuation_setup_cache_.min_distance = c.min_distance;
    attenuation_setup_cache_.max_distance = c.max_distance;
    attenuation_setup_cache_.num_curve_samples = num_curve;
    for (int i = 0; i < num_curve; i++)
        attenuation_setup_cache_.curve_samples[i] = curve_local[i];
    attenuation_setup_cache_.valid = true;
}

void ResonancePlayer::_compute_listener_data(ResonanceServer* srv, Vector3& out_listener_pos,
                                             IPLCoordinateSpace3& out_listener_orient) {
    out_listener_pos = Vector3(0, 0, 0);
    out_listener_orient = IPLCoordinateSpace3{};
    if (srv && srv->is_initialized()) {
        out_listener_orient = srv->get_current_listener_coords();
        out_listener_pos = resonance::listener_origin_godot(out_listener_orient);
        return;
    }
    Viewport* vp = get_viewport();
    if (vp && vp->get_camera_3d()) {
        Camera3D* cam = vp->get_camera_3d();
        out_listener_pos = cam->get_global_position();
        Vector3 forward = -cam->get_global_transform().basis.get_column(2);
        Vector3 up = cam->get_global_transform().basis.get_column(1);
        Vector3 right = cam->get_global_transform().basis.get_column(0);
        out_listener_orient.origin = {out_listener_pos.x, out_listener_pos.y, out_listener_pos.z};
        out_listener_orient.ahead = {forward.x, forward.y, forward.z};
        out_listener_orient.up = {up.x, up.y, up.z};
        out_listener_orient.right = {right.x, right.y, right.z};
    }
}

void ResonancePlayer::_compute_attenuation(float dist, const OcclusionData& occ_data, float& out_attenuation) {
    const ConfigCache& c = config_cache_;
    out_attenuation = 1.0f;
    if (!c.distance_attenuation) {
        out_attenuation = 1.0f;
        return;
    }
    if (c.attenuation_mode == ATTENUATION_INVERSE) {
        // 1/d with a min_distance knee; max_distance is ignored, as in Phonon's InverseDistance.
        out_attenuation =
            resonance::playback_inverse_distance_attenuation(dist, c.min_distance, occ_data.distance_attenuation);
    } else if (c.attenuation_mode == ATTENUATION_DISABLED) {
        out_attenuation = 1.0f;
    } else if (c.attenuation_mode == ATTENUATION_LINEAR) {
        if (dist <= c.min_distance)
            out_attenuation = 1.0f;
        else if (c.max_distance <= c.min_distance || dist >= c.max_distance)
            out_attenuation = 0.0f;
        else
            out_attenuation = 1.0f - ((dist - c.min_distance) / (c.max_distance - c.min_distance));
    } else if (c.attenuation_mode == ATTENUATION_CUSTOM_CURVE) {
        if (c.attenuation_curve.is_valid() && c.max_distance > c.min_distance) {
            float t = (dist - c.min_distance) / (c.max_distance - c.min_distance);
            t = CLAMP(t, 0.0f, 1.0f);
            out_attenuation = c.attenuation_curve->sample(t);
        } else if (c.attenuation_curve.is_valid()) {
            out_attenuation = (dist <= c.min_distance) ? 1.0f : 0.0f;
        } else {
            out_attenuation = (dist >= c.max_distance) ? 0.0f : 1.0f;
        }
    }
}

Vector3 ResonancePlayer::_apply_perspective_correction(Vector3 listener_pos, Viewport* vp, bool apply_perspective, float perspective_factor_val) {
    if (!apply_perspective || !vp || !vp->get_camera_3d())
        return get_global_position();
    Camera3D* cam = vp->get_camera_3d();
    Transform3D view_xform = cam->get_global_transform().affine_inverse();
    Vector3 view_pos = view_xform.xform(get_global_position());
    Projection proj = cam->get_camera_projection();
    Vector4 clip = proj.xform(Vector4(view_pos.x, view_pos.y, view_pos.z, 1.0f));
    if (clip.w <= 0.01f)
        return get_global_position();
    float ndc_x = clip.x / clip.w;
    float ndc_y = clip.y / clip.w;
    ndc_x = CLAMP(ndc_x, -1.0f, 1.0f);
    ndc_y = CLAMP(ndc_y, -1.0f, 1.0f);
    float factor = perspective_factor_val;
    float sx = ndc_x * factor;
    float sy = ndc_y * factor;
    Vector3 dir_view(sx, sy, -1.0f);
    float len_sq = dir_view.length_squared();
    if (len_sq <= resonance::kDegenerateVectorEpsilon)
        return get_global_position();
    dir_view = dir_view / std::sqrt(len_sq);
    Vector3 dir_world = cam->get_global_transform().basis.xform(dir_view);
    return listener_pos + dir_world;
}

PlaybackParameters ResonancePlayer::_build_playback_params(const Vector3& listener_pos, const IPLCoordinateSpace3& listener_orient,
                                                           float attenuation, float dist, const Vector3& effective_source_pos,
                                                           float occ_val, float tx_low, float tx_mid, float tx_high, float directivity_val, const Vector3& air_abs,
                                                           bool has_reverb, bool direct_enabled, bool reverb_enabled) {
    const ConfigCache& c = config_cache_;
    ResonanceServer* srv = ResonanceServer::get_singleton();
    PlaybackParameters new_params;
    new_params.source_handle = source_handle;
    new_params.occlusion = occ_val;
    new_params.transmission[0] = tx_low;
    new_params.transmission[1] = tx_mid;
    new_params.transmission[2] = tx_high;
    new_params.attenuation = attenuation;
    new_params.distance = dist;
    new_params.source_position = effective_source_pos;
    bool eff_use_binaural = (c.direct_binaural_override == -1) ? srv->use_direct_binaural() : (c.direct_binaural_override == 1);
    new_params.use_binaural = eff_use_binaural;
    new_params.apply_air_absorption = c.air_absorption_enabled;
    new_params.air_absorption[0] = air_abs.x;
    new_params.air_absorption[1] = air_abs.y;
    new_params.air_absorption[2] = air_abs.z;
    new_params.apply_directivity = c.directivity_enabled;
    new_params.directivity_value = directivity_val;
    new_params.apply_hrtf_to_reflections =
        resonance::resolve_binaural_override(c.reverb_binaural_override, srv->use_reverb_binaural());
    new_params.apply_hrtf_to_pathing =
        resonance::resolve_binaural_override(c.pathing_binaural_override, srv->use_pathing_binaural());
    new_params.listener_orientation = listener_orient;
    new_params.enable_direct = direct_enabled;
    new_params.enable_reverb = reverb_enabled;
    new_params.enable_reflections = c.reflections_enabled != 0;
    new_params.has_valid_reverb = has_reverb;
    new_params.spatial_blend = c.spatial_blend;
    new_params.use_ambisonics_encode = c.use_ambisonics_encode;
    {
        const float master = resonance::sanitize_audio_float(c.master_mix_level);
        new_params.direct_mix_level = resonance::sanitize_audio_float(c.direct_mix_level * master);
        new_params.reflections_mix_level = resonance::sanitize_audio_float(c.reflections_mix_level * master);
        new_params.pathing_mix_level = resonance::sanitize_audio_float(c.pathing_mix_level * master);
    }
    new_params.reflections_eq[0] = c.reflections_eq_low;
    new_params.reflections_eq[1] = c.reflections_eq_mid;
    new_params.reflections_eq[2] = c.reflections_eq_high;
    new_params.reflections_delay = c.reflections_delay;
    new_params.reverb_send = reverb_send_active_;
    int eff_tx_type = resonance::kTransmissionFreqIndependent;
    bool eff_hrtf_bi = false;
    if (srv) {
        eff_tx_type = srv->get_transmission_type();
        eff_hrtf_bi = srv->get_hrtf_interpolation_bilinear();
    }
    if (c.transmission_type_override == resonance::kTransmissionFreqIndependent || c.transmission_type_override == resonance::kTransmissionFreqDependent)
        eff_tx_type = c.transmission_type_override;
    new_params.direct_effect_transmission_type = eff_tx_type;
    if (c.hrtf_interpolation_override == 0)
        eff_hrtf_bi = false;
    else if (c.hrtf_interpolation_override == 1)
        eff_hrtf_bi = true;
    new_params.direct_effect_hrtf_bilinear = eff_hrtf_bi;

    // Baked REVERB wet is not multiplied by direct-path occlusion/transmission.
    new_params.wet_occlusion_factor = 1.0f;
    return new_params;
}

void ResonancePlayer::_prepare_source_for_simulation(ResonanceServer* srv) {
    _ensure_config_valid();
    config_cache_frame_countdown--;
    _ensure_voice_sources(srv);

    _setup_attenuation(srv);
    _apply_update_source(_resolve_pathing_probe_batch(), true);
}

int32_t ResonancePlayer::_resolve_pathing_probe_batch() {
    if (pathing_probe_volume.is_empty())
        return -1;
    Node* node = get_node_or_null(pathing_probe_volume);
    if (ResonanceProbeVolume* pv = Object::cast_to<ResonanceProbeVolume>(node))
        return pv->get_probe_batch_handle();
    // Only the editor clears a dangling path: at runtime the volume may be out of the tree for now and come back.
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint())
        set_pathing_probe_volume(NodePath());
    return -1;
}

bool ResonancePlayer::_playback_lod_should_apply_playback_params(double delta, bool debug_hud_active, const Vector3& source_pos) {
    if (debug_hud_active)
        return true;
    const ConfigCache& c = config_cache_;
    const float iv = c.playback_parameter_min_interval;
    const float mv = c.playback_parameter_min_move;
    if (iv <= 0.0f && mv <= 0.0f)
        return true;
    playback_lod_time_since_full_ += delta;
    const bool hit_time = (iv > 0.0f) && (playback_lod_time_since_full_ >= static_cast<double>(iv));
    const float mv_sq = mv * mv;
    const bool hit_move = (mv > 0.0f) &&
                          (!playback_lod_have_anchor_ || (source_pos - playback_lod_anchor_pos_).length_squared() >= mv_sq);
    if (!hit_time && !hit_move)
        return false;
    playback_lod_time_since_full_ = 0.0;
    playback_lod_have_anchor_ = true;
    playback_lod_anchor_pos_ = source_pos;
    return true;
}

// Builds PlaybackParameters from the latest simulation results and pushes them to the audio worker.
// opt_debug_out, when set, receives the values for the debug HUD.
void ResonancePlayer::_apply_playback_params_from_simulation(ResonanceServer* srv, ResonanceDebugData* opt_debug_out, double delta_seconds) {
    if (!srv)
        return;
    _ensure_voice_sources(srv);
    if (!srv->is_source_spatial_output_ready(source_handle))
        return;

    const ConfigCache& c = config_cache_;
    Viewport* vp = get_viewport();
    Vector3 listener_pos;
    IPLCoordinateSpace3 listener_orient;
    _compute_listener_data(srv, listener_pos, listener_orient);

    OcclusionData occ_data = srv->get_source_occlusion_data(source_handle);
    float dist = get_global_position().distance_to(listener_pos);
    float attenuation;
    _compute_attenuation(dist, occ_data, attenuation);

    Vector3 air_abs;
    if (c.air_absorption_enabled && c.air_absorption_input == 1) {
        air_abs.x = CLAMP(c.air_absorption_low, 0.0f, 1.0f);
        air_abs.y = CLAMP(c.air_absorption_mid, 0.0f, 1.0f);
        air_abs.z = CLAMP(c.air_absorption_high, 0.0f, 1.0f);
    } else {
        air_abs = Vector3(occ_data.air_absorption[0], occ_data.air_absorption[1], occ_data.air_absorption[2]);
    }
    float occ_val = 0.0f;
    float tx_low = 0.0f;
    float tx_mid = 0.0f;
    float tx_high = 0.0f;
    float directivity_val = 0.0f;
    _resolve_playback_occ_tx_directivity(occ_data, delta_seconds, true, occ_val, tx_low, tx_mid, tx_high, directivity_val);

    // Peek first to avoid fetch cost; fetch only when worker cache has no reverb hint yet.
    IPLReflectionEffectParams ignored_params{};
    bool has_reverb = srv->peek_reverb_params_likely_available(source_handle);
    if (!has_reverb)
        has_reverb = srv->fetch_reverb_params(source_handle, ignored_params);

    // Mix levels only scale the DSP ramps. The enable gates follow output_*, so a mix of 0 still runs the effects on silence.
    const bool direct_enabled = resonance::playback_enable_direct(!srv || srv->is_output_direct_enabled());
    const bool reverb_enabled = _playback_wet_enabled(srv);

    bool apply_perspective = (c.perspective_override == 1) || (c.perspective_override == -1 && srv->is_perspective_correction_enabled());
    float perspective_factor_val = (c.perspective_override == 1) ? CLAMP(c.perspective_factor, resonance::kPlayerPerspectiveFactorMin, resonance::kPlayerPerspectiveFactorMax) : srv->get_perspective_correction_factor();
    Vector3 effective_source_pos = _apply_perspective_correction(listener_pos, vp, apply_perspective, perspective_factor_val);

    PlaybackParameters new_params = _build_playback_params(listener_pos, listener_orient,
                                                           attenuation, dist, effective_source_pos,
                                                           occ_val, tx_low, tx_mid, tx_high, directivity_val, air_abs,
                                                           has_reverb, direct_enabled, reverb_enabled);
    new_params.virtualized = voice_virtual_state_.virtualized;

    _broadcast_update_parameters(new_params);
    last_pushed_playback_params_ = new_params;
    last_pushed_playback_params_valid_ = true;

    if (opt_debug_out) {
        opt_debug_out->source_pos = get_global_position();
        opt_debug_out->listener_pos = listener_pos;
        opt_debug_out->occlusion = occ_val;
        opt_debug_out->transmission[0] = tx_low;
        opt_debug_out->transmission[1] = tx_mid;
        opt_debug_out->transmission[2] = tx_high;
        opt_debug_out->hit0_transmission[0] = occ_data.hit0_transmission[0];
        opt_debug_out->hit0_transmission[1] = occ_data.hit0_transmission[1];
        opt_debug_out->hit0_transmission[2] = occ_data.hit0_transmission[2];
        opt_debug_out->hit0_valid = occ_data.hit0_valid;
        opt_debug_out->num_transmission_rays = occ_data.num_transmission_rays;
        opt_debug_out->attenuation = attenuation;
        opt_debug_out->distance = dist;
        opt_debug_out->air_absorption = air_abs;
        opt_debug_out->directivity_val = directivity_val;
        opt_debug_out->air_abs_enabled = c.air_absorption_enabled;
        opt_debug_out->directivity_enabled = c.directivity_enabled;
    }
}

bool ResonancePlayer::_update_voice_virtual_state(ResonanceServer* srv, double delta_seconds) {
    if (!srv || !srv->is_source_spatial_output_ready(source_handle))
        return false;

    Vector3 listener_pos;
    IPLCoordinateSpace3 listener_orient;
    _compute_listener_data(srv, listener_pos, listener_orient);
    const float distance = get_global_position().distance_to(listener_pos);
    float attenuation = 1.0f;
    _compute_attenuation(distance, srv->get_source_occlusion_data(source_handle), attenuation);

    const ConfigCache& c = config_cache_;
    resonance::VoiceAudibility audibility;
    audibility.attenuation_uses_max_distance =
        resonance::attenuation_mode_uses_max_distance(c.distance_attenuation, c.attenuation_mode);
    audibility.distance = distance;
    audibility.max_distance = c.max_distance;
    audibility.attenuation = attenuation;
    audibility.reflections_simulated =
        resonance::source_sim_reflections_enabled(c.reflections_enabled, srv->is_output_reverb_enabled());
    audibility.reflections_follow_distance_curve = c.use_distance_curve_for_reflections;

    const float enter_delay = resonance::voice_virtual_enter_delay_sec(audibility.reflections_simulated,
                                                                       srv->get_realtime_simulation_duration());
    const bool was_virtual = voice_virtual_state_.virtualized;
    voice_virtual_state_ = resonance::next_voice_virtual_state(
        voice_virtual_state_, resonance::voice_is_inaudible(audibility), delta_seconds, enter_delay);
    if (voice_virtual_state_.virtualized == was_virtual)
        return false;
    _sync_source_simulation_activity(srv);
    return true;
}

void ResonancePlayer::_apply_playback_coeff_refresh_from_simulation(ResonanceServer* srv, double delta_seconds) {
    if (!srv || !srv->is_source_spatial_output_ready(source_handle) || !last_pushed_playback_params_valid_)
        return;
    _ensure_voice_sources(srv);

    const ConfigCache& c = config_cache_;
    OcclusionData occ_data = srv->get_source_occlusion_data(source_handle);

    float occ_val = 0.0f;
    float tx_low = 0.0f;
    float tx_mid = 0.0f;
    float tx_high = 0.0f;
    float directivity_val = 0.0f;
    _resolve_playback_occ_tx_directivity(occ_data, delta_seconds, false, occ_val, tx_low, tx_mid, tx_high, directivity_val);

    PlaybackParameters patched = last_pushed_playback_params_;
    patched.occlusion = occ_val;
    patched.transmission[0] = tx_low;
    patched.transmission[1] = tx_mid;
    patched.transmission[2] = tx_high;
    patched.directivity_value = directivity_val;

    patched.wet_occlusion_factor = 1.0f;

    patched.enable_direct = resonance::playback_enable_direct(srv->is_output_direct_enabled());
    patched.enable_reverb = _playback_wet_enabled(srv);
    patched.enable_reflections = config_cache_.reflections_enabled != 0;

    if (resonance::playback_lod_coeff_refresh_includes_position_attenuation()) {
        Viewport* vp = get_viewport();
        Vector3 listener_pos;
        IPLCoordinateSpace3 listener_orient;
        _compute_listener_data(srv, listener_pos, listener_orient);
        float dist = get_global_position().distance_to(listener_pos);
        float attenuation = 1.0f;
        _compute_attenuation(dist, occ_data, attenuation);
        bool apply_perspective = (c.perspective_override == 1) || (c.perspective_override == -1 && srv->is_perspective_correction_enabled());
        float perspective_factor_val = (c.perspective_override == 1)
                                           ? CLAMP(c.perspective_factor, resonance::kPlayerPerspectiveFactorMin, resonance::kPlayerPerspectiveFactorMax)
                                           : srv->get_perspective_correction_factor();
        Vector3 effective_source_pos = _apply_perspective_correction(listener_pos, vp, apply_perspective, perspective_factor_val);
        patched.attenuation = attenuation;
        patched.distance = dist;
        patched.source_position = effective_source_pos;
        patched.listener_orientation = listener_orient;
    }

    _broadcast_update_parameters(patched);
    last_pushed_playback_params_ = patched;
}

void ResonancePlayer::_sync_player_debug_drawer(double delta, ResonanceServer* srv, const ResonanceDebugData& dbg_data, bool hud_active) {
    if (exclude_from_debug_)
        return;
    const bool occ = srv && srv->is_debug_occlusion_enabled();
    const bool ref = srv && srv->is_debug_reflections_enabled();
    debug_drawer.process(delta, dbg_data, occ, ref, get_name(), hud_active);
}

void ResonancePlayer::_push_playback_parameters_from_simulation(ResonanceServer* srv, ResonanceDebugData* opt_debug_out, double delta_seconds,
                                                                bool run_prepare) {
    _ensure_voice_sources(srv);
    if (run_prepare)
        _prepare_source_for_simulation(srv);
    else if (srv)
        _setup_attenuation(srv);
    _apply_playback_params_from_simulation(srv, opt_debug_out, delta_seconds);
}

void ResonancePlayer::_deferred_push_playback_parameters() {
    Engine* eng = Engine::get_singleton();
    if (eng && eng->is_editor_hint())
        return;
    if (!player_config.is_valid() || !is_playing())
        return;
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!srv || !srv->is_initialized())
        return;
    _ensure_voice_sources(srv);
    if (!srv->is_simulating() || source_handle < 0)
        return;
    _push_playback_parameters_from_simulation(srv, nullptr, 0.0);
}
