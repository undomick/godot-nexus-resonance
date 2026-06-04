#include "resonance_server_config.h"
#include "resonance_constants.h"
#include "resonance_speaker_layout.h"
#include <algorithm>
#include <climits>
#include <thread>

namespace godot {

// Dictionary helpers: tolerant Variant typing (int/float/bool), SOFA ref extraction, sim-interval migration chain.

int ResonanceServerConfig::config_int(const Dictionary& c, const char* key, int def) {
    if (!c.has(key))
        return def;
    Variant v = c[key];
    if (v.get_type() == Variant::INT)
        return (int)v;
    if (v.get_type() == Variant::FLOAT) {
        double d = (double)v;
        if (d > static_cast<double>(INT_MAX))
            return INT_MAX;
        if (d < static_cast<double>(INT_MIN))
            return INT_MIN;
        return static_cast<int>(d);
    }
    return def;
}

float ResonanceServerConfig::config_float(const Dictionary& c, const char* key, float def) {
    if (!c.has(key))
        return def;
    Variant v = c[key];
    if (v.get_type() == Variant::FLOAT)
        return (float)v;
    if (v.get_type() == Variant::INT)
        return (float)(int)v;
    return def;
}

bool ResonanceServerConfig::config_bool(const Dictionary& c, const char* key, bool def) {
    if (!c.has(key))
        return def;
    Variant v = c[key];
    if (v.get_type() == Variant::BOOL)
        return (bool)v;
    return def;
}

void ResonanceServerConfig::config_sofa_asset(const Dictionary& c, const char* key, Ref<ResonanceSOFAAsset>& out) {
    if (!c.has(key)) {
        out.unref();
        return;
    }
    Variant v = c[key];
    if (v.get_type() != Variant::OBJECT) {
        out.unref();
        return;
    }
    Object* o = v.operator Object*();
    if (!o) {
        out.unref();
        return;
    }
    ResonanceSOFAAsset* a = Object::cast_to<ResonanceSOFAAsset>(o);
    if (a && a->is_valid())
        out = Ref<ResonanceSOFAAsset>(a);
    else
        out.unref();
}

// new_key → legacy_sub_key → master simulation_update_interval; values clamped to [0, 1] seconds.
float ResonanceServerConfig::resolve_sim_interval_sec(const Dictionary& c, const char* new_key, const char* legacy_sub_key) {
    if (c.has(new_key)) {
        float v = config_float(c, new_key, 0.1f);
        if (v < 0.0f)
            v = 0.0f;
        if (v > 1.0f)
            v = 1.0f;
        return v;
    }
    if (c.has(legacy_sub_key)) {
        float sub = config_float(c, legacy_sub_key, -1.0f);
        if (sub >= 0.0f) {
            if (sub > 1.0f)
                sub = 1.0f;
            return sub;
        }
    }
    float master = config_float(c, "simulation_update_interval", 0.1f);
    if (master < 0.0f)
        master = 0.0f;
    if (master > 1.0f)
        master = 1.0f;
    return master;
}

// Loads and clamps all fields; handles legacy enums (pathing_validation_ab_mode, use_radeon_rays) and bake fallbacks for pathing_vis_*.
void ResonanceServerConfig::apply(const Dictionary& config,
                                  std::function<float(const char*, float)> get_bake_pathing_param) {
    sample_rate = config_int(config, "sample_rate", sample_rate);
    if (sample_rate < 8000)
        sample_rate = 8000;
    if (sample_rate > 192000)
        sample_rate = 192000;
    frame_size = config_int(config, "audio_frame_size", frame_size);
    {
        static const int kValidFrameSizes[] = {256, resonance::kGodotDefaultFrameSize, 1024, resonance::kMaxAudioFrameSize};
        bool valid = false;
        for (int fs : kValidFrameSizes) {
            if (frame_size == fs) {
                valid = true;
                break;
            }
        }
        if (!valid)
            frame_size = resonance::kGodotDefaultFrameSize;
    }
    ambisonic_order = config_int(config, "ambisonic_order", ambisonic_order);
    if (ambisonic_order < 1)
        ambisonic_order = 1;
    if (ambisonic_order > 3)
        ambisonic_order = 3;
    max_reverb_duration = config_float(config, "max_reverb_duration", max_reverb_duration);
    if (max_reverb_duration < 0.1f)
        max_reverb_duration = 0.1f;
    if (max_reverb_duration > 10.0f)
        max_reverb_duration = 10.0f;
    simulation_cpu_cores_percent = config_float(config, "simulation_cpu_cores_percent", simulation_cpu_cores_percent);
    if (simulation_cpu_cores_percent <= 0.0f || simulation_cpu_cores_percent > 1.0f)
        simulation_cpu_cores_percent = resonance::kDefaultSimulationCpuCoresPercent;
    unsigned int nc = std::thread::hardware_concurrency();
    if (nc == 0)
        nc = 1;
    simulation_threads = static_cast<int>(std::max(1u, static_cast<unsigned int>(simulation_cpu_cores_percent * static_cast<float>(nc))));
    max_rays = config_int(config, "realtime_rays", max_rays);
    if (max_rays < 0)
        max_rays = 0; // 0 = Baked Only
    if (max_rays > 65535)
        max_rays = 65535;
    max_bounces = config_int(config, "realtime_bounces", max_bounces);
    if (max_bounces < 1)
        max_bounces = 1;
    if (max_bounces > 64)
        max_bounces = 64;
    reverb_influence_radius = config_float(config, "reverb_influence_radius", reverb_influence_radius);
    if (reverb_influence_radius < 1.0f)
        reverb_influence_radius = 1.0f;
    reverb_transmission_amount = config_float(config, "reverb_transmission_amount", reverb_transmission_amount);
    if (reverb_transmission_amount < 0.0f)
        reverb_transmission_amount = 0.0f;
    if (reverb_transmission_amount > 1.0f)
        reverb_transmission_amount = 1.0f;
    apply_occlusion_to_baked_reflections = config_bool(config, "apply_occlusion_to_baked_reflections",
                                                       apply_occlusion_to_baked_reflections);
    baked_reverb_use_listener_probe = config_bool(config, "baked_reverb_use_listener_probe",
                                                  baked_reverb_use_listener_probe);
    reflection_type = config_int(config, "reflection_type", reflection_type);
    if (reflection_type < resonance::kReflectionConvolution)
        reflection_type = resonance::kReflectionConvolution;
    if (reflection_type > resonance::kReflectionTan)
        reflection_type = resonance::kReflectionTan;
    default_reflections_mode = config_int(config, "default_reflections_mode", default_reflections_mode);
    if (default_reflections_mode < 0)
        default_reflections_mode = 0;
    // Legacy three-value enum used Realtime = 2; clamp 2+ to Realtime (1).
    if (default_reflections_mode > 1)
        default_reflections_mode = 1;
    hybrid_reverb_transition_time = config_float(config, "hybrid_reverb_transition_time", hybrid_reverb_transition_time);
    if (hybrid_reverb_transition_time < 0.1f)
        hybrid_reverb_transition_time = 0.1f;
    if (hybrid_reverb_transition_time > 2.0f)
        hybrid_reverb_transition_time = 2.0f;
    float overlap_pct = config_float(config, "hybrid_reverb_overlap_percent", hybrid_reverb_overlap_percent * 100.0f);
    hybrid_reverb_overlap_percent = (overlap_pct >= 0.0f && overlap_pct <= 100.0f) ? (overlap_pct / 100.0f) : 0.25f;
    transmission_type = config_int(config, "transmission_type", transmission_type);
    if (transmission_type < 0)
        transmission_type = 0;
    if (transmission_type > 1)
        transmission_type = 1;
    max_transmission_surfaces = config_int(config, "max_transmission_surfaces", max_transmission_surfaces);
    if (max_transmission_surfaces < 1)
        max_transmission_surfaces = 1;
    if (max_transmission_surfaces > resonance::kMaxTransmissionRays)
        max_transmission_surfaces = resonance::kMaxTransmissionRays;
    occlusion_type = config_int(config, "occlusion_type", occlusion_type);
    if (occlusion_type < 0)
        occlusion_type = 0;
    if (occlusion_type > 1)
        occlusion_type = 1;
    max_occlusion_samples = config_int(config, "max_occlusion_samples", max_occlusion_samples);
    if (max_occlusion_samples < 1)
        max_occlusion_samples = 1;
    if (max_occlusion_samples > resonance::kMaxOcclusionSamplesUserMax)
        max_occlusion_samples = resonance::kMaxOcclusionSamplesUserMax;
    max_simulation_sources = config_int(config, "max_simulation_sources", max_simulation_sources);
    if (max_simulation_sources < 8)
        max_simulation_sources = 8;
    if (max_simulation_sources > resonance::kMaxSimulationSourcesUserMax)
        max_simulation_sources = resonance::kMaxSimulationSourcesUserMax;
    hrtf_volume_db = config_float(config, "hrtf_volume_db", hrtf_volume_db);
    if (hrtf_volume_db < resonance::kHRTFVolumeDBMin)
        hrtf_volume_db = resonance::kHRTFVolumeDBMin;
    if (hrtf_volume_db > resonance::kHRTFVolumeDBMax)
        hrtf_volume_db = resonance::kHRTFVolumeDBMax;
    hrtf_normalization_type = config_int(config, "hrtf_normalization_type", hrtf_normalization_type);
    if (hrtf_normalization_type < 0)
        hrtf_normalization_type = 0;
    if (hrtf_normalization_type > 1)
        hrtf_normalization_type = 1;
    config_sofa_asset(config, "hrtf_sofa_asset", hrtf_sofa_asset);
    {
        const bool rb_fallback = config_bool(config, "reverb_binaural", true);
        direct_binaural = config_bool(config, "direct_binaural", rb_fallback);
        reverb_binaural = config_bool(config, "reverb_binaural", rb_fallback);
        pathing_binaural = config_bool(config, "pathing_binaural", rb_fallback);
    }
    use_virtual_surround = config_bool(config, "use_virtual_surround", use_virtual_surround);
    direct_speaker_channels = resonance::clamp_direct_speaker_channels(config_int(config, "direct_speaker_channels", direct_speaker_channels));
    hrtf_interpolation_bilinear = config_bool(config, "hrtf_interpolation_bilinear", hrtf_interpolation_bilinear);
    pathing_enabled = config_bool(config, "pathing_enabled", pathing_enabled);

    if (config.has("pathing_vis_radius"))
        pathing_vis_radius = config_float(config, "pathing_vis_radius", pathing_vis_radius);
    else if (get_bake_pathing_param)
        pathing_vis_radius = get_bake_pathing_param("bake_pathing_radius", resonance::kBakePathingDefaultRadius);
    if (pathing_vis_radius < 0.0f)
        pathing_vis_radius = 0.0f;
    if (pathing_vis_radius > 2.0f)
        pathing_vis_radius = 2.0f;

    if (config.has("pathing_vis_threshold"))
        pathing_vis_threshold = config_float(config, "pathing_vis_threshold", pathing_vis_threshold);
    else if (get_bake_pathing_param)
        pathing_vis_threshold = get_bake_pathing_param("bake_pathing_threshold", resonance::kBakePathingDefaultThreshold);
    if (pathing_vis_threshold < 0.0f)
        pathing_vis_threshold = 0.0f;
    if (pathing_vis_threshold > 1.0f)
        pathing_vis_threshold = 1.0f;

    if (config.has("pathing_vis_range"))
        pathing_vis_range = config_float(config, "pathing_vis_range", pathing_vis_range);
    else if (get_bake_pathing_param)
        pathing_vis_range = get_bake_pathing_param("bake_pathing_vis_range", resonance::kBakePathingDefaultVisRange);
    if (pathing_vis_range < 1.0f)
        pathing_vis_range = 1.0f;
    if (pathing_vis_range > 1000.0f)
        pathing_vis_range = 1000.0f;

    pathing_normalize_eq = config_bool(config, "pathing_normalize_eq", pathing_normalize_eq);
    pathing_num_vis_samples = config_int(config, "pathing_num_vis_samples", pathing_num_vis_samples);
    if (pathing_num_vis_samples < 1)
        pathing_num_vis_samples = resonance::kRuntimePathingDefaultNumVisSamples;
    if (pathing_num_vis_samples > 16)
        pathing_num_vis_samples = 16;
    path_validation_enabled = config_bool(config, "path_validation_enabled", path_validation_enabled);
    find_alternate_paths = config_bool(config, "find_alternate_paths", find_alternate_paths);
    // Legacy: pathing_validation_ab_mode when new keys absent (old get_config() / hand-built dicts).
    if (!config.has("path_validation_enabled") && !config.has("find_alternate_paths") && config.has("pathing_validation_ab_mode")) {
        int ab = config_int(config, "pathing_validation_ab_mode", 0);
        if (ab == 1) {
            path_validation_enabled = true;
            find_alternate_paths = false;
        } else if (ab == 2) {
            path_validation_enabled = false;
            find_alternate_paths = false;
        } else if (ab == 3) {
            path_validation_enabled = true;
            find_alternate_paths = true;
        }
    }
    if (config.has("scene_type"))
        scene_type = config_int(config, "scene_type", scene_type);
    else
        scene_type = config_bool(config, "use_radeon_rays", false) ? 2 : 0; // backwards compatibility: missing key = Default
    if (scene_type < 0 || scene_type > 3)
        scene_type = 0;
    physics_ray_collision_mask = config_int(config, "physics_ray_collision_mask", physics_ray_collision_mask);
    physics_ray_batch_size = config_int(config, "physics_ray_batch_size", physics_ray_batch_size);
    physics_ray_batch_size = resonance::clamp_physics_ray_batch_size(physics_ray_batch_size);
    opencl_device_type = config_int(config, "opencl_device_type", opencl_device_type);
    if (opencl_device_type < 0)
        opencl_device_type = 0;
    if (opencl_device_type > 2)
        opencl_device_type = 2;
    opencl_device_index = config_int(config, "opencl_device_index", opencl_device_index);
    if (opencl_device_index < 0)
        opencl_device_index = 0;
    context_validation = config_bool(config, "context_validation", context_validation);
    context_simd_level = config_int(config, "context_simd_level", context_simd_level);
    realtime_irradiance_min_distance = config_float(config, "realtime_irradiance_min_distance", realtime_irradiance_min_distance);
    if (realtime_irradiance_min_distance < 0.05f)
        realtime_irradiance_min_distance = 0.05f;
    if (realtime_irradiance_min_distance > 10.0f)
        realtime_irradiance_min_distance = 10.0f;
    realtime_simulation_duration = config_float(config, "realtime_simulation_duration", realtime_simulation_duration);
    if (realtime_simulation_duration < 0.1f)
        realtime_simulation_duration = 0.1f;
    if (realtime_simulation_duration > 10.0f)
        realtime_simulation_duration = 10.0f;
    realtime_num_diffuse_samples = config_int(config, "realtime_num_diffuse_samples", realtime_num_diffuse_samples);
    if (realtime_num_diffuse_samples < 8)
        realtime_num_diffuse_samples = 8;
    if (realtime_num_diffuse_samples > 64)
        realtime_num_diffuse_samples = 64;
    debug_occlusion = config_bool(config, "debug_occlusion", debug_occlusion);
    debug_reflections = config_bool(config, "debug_reflections", debug_reflections);
    debug_pathing = config_bool(config, "debug_pathing", debug_pathing);
    output_direct_enabled = config_bool(config, "output_direct", output_direct_enabled);
    output_reverb_enabled = config_bool(config, "output_reverb", output_reverb_enabled);
    perspective_correction_enabled = config_bool(config, "perspective_correction_enabled", perspective_correction_enabled);
    perspective_correction_factor = config_float(config, "perspective_correction_factor", perspective_correction_factor);
    dynamic_scene_commit_min_interval = config_float(config, "dynamic_scene_commit_min_interval", dynamic_scene_commit_min_interval);
    if (dynamic_scene_commit_min_interval < 0.0f)
        dynamic_scene_commit_min_interval = 0.0f;
    if (dynamic_scene_commit_min_interval > 1.0f)
        dynamic_scene_commit_min_interval = 1.0f;
    reflections_sim_interval = resolve_sim_interval_sec(config, "reflections_sim_interval", "reflections_sim_update_interval");
    pathing_sim_interval = resolve_sim_interval_sec(config, "pathing_sim_interval", "pathing_sim_update_interval");
    realtime_reflection_max_distance_m = config_float(config, "realtime_reflection_max_distance_m", realtime_reflection_max_distance_m);
    if (realtime_reflection_max_distance_m < 0.0f)
        realtime_reflection_max_distance_m = 0.0f;
    reflections_adaptive_budget_us = config_int(config, "reflections_adaptive_budget_us", reflections_adaptive_budget_us);
    if (reflections_adaptive_budget_us < 0)
        reflections_adaptive_budget_us = 0;
    if (reflections_adaptive_budget_us > 2000000)
        reflections_adaptive_budget_us = 2000000;
    reflections_adaptive_ray_min = config_int(config, "reflections_adaptive_ray_min", reflections_adaptive_ray_min);
    if (reflections_adaptive_ray_min < 32)
        reflections_adaptive_ray_min = 32;
    if (reflections_adaptive_ray_min > 65535)
        reflections_adaptive_ray_min = 65535;
    reflections_adaptive_ray_recover_frac = config_float(config, "reflections_adaptive_ray_recover_frac", reflections_adaptive_ray_recover_frac);
    if (reflections_adaptive_ray_recover_frac < 0.0f)
        reflections_adaptive_ray_recover_frac = 0.0f;
    if (reflections_adaptive_ray_recover_frac > 1.0f)
        reflections_adaptive_ray_recover_frac = 1.0f;
    reflections_adaptive_ray_recover_cap = config_int(config, "reflections_adaptive_ray_recover_cap", reflections_adaptive_ray_recover_cap);
    if (reflections_adaptive_ray_recover_cap < 0)
        reflections_adaptive_ray_recover_cap = 0;
    if (reflections_adaptive_ray_recover_cap > 65535)
        reflections_adaptive_ray_recover_cap = 65535;
    reflections_adaptive_step_sec = config_float(config, "reflections_adaptive_step_sec", reflections_adaptive_step_sec);
    if (reflections_adaptive_step_sec < 0.0f)
        reflections_adaptive_step_sec = 0.0f;
    if (reflections_adaptive_step_sec > 1.0f)
        reflections_adaptive_step_sec = 1.0f;
    reflections_adaptive_max_extra_interval = config_float(config, "reflections_adaptive_max_extra_interval", reflections_adaptive_max_extra_interval);
    if (reflections_adaptive_max_extra_interval < 0.0f)
        reflections_adaptive_max_extra_interval = 0.0f;
    if (reflections_adaptive_max_extra_interval > 1.0f)
        reflections_adaptive_max_extra_interval = 1.0f;
    reflections_adaptive_decay_per_sec = config_float(config, "reflections_adaptive_decay_per_sec", reflections_adaptive_decay_per_sec);
    if (reflections_adaptive_decay_per_sec < 0.0f)
        reflections_adaptive_decay_per_sec = 0.0f;
    if (reflections_adaptive_decay_per_sec > 5.0f)
        reflections_adaptive_decay_per_sec = 5.0f;
    reflections_defer_after_scene_commit_us = config_int(config, "reflections_defer_after_scene_commit_us", reflections_defer_after_scene_commit_us);
    if (reflections_defer_after_scene_commit_us < 0)
        reflections_defer_after_scene_commit_us = 0;
    if (reflections_defer_after_scene_commit_us > 5000000)
        reflections_defer_after_scene_commit_us = 5000000;
    convolution_ir_max_samples = config_int(config, "convolution_ir_max_samples", convolution_ir_max_samples);
    if (convolution_ir_max_samples < 0)
        convolution_ir_max_samples = 0;
    if (convolution_ir_max_samples > 480000)
        convolution_ir_max_samples = 480000;
    direct_sim_interval = config_float(config, "direct_sim_interval", direct_sim_interval);
    if (direct_sim_interval < 0.0f)
        direct_sim_interval = 0.0f;
    if (direct_sim_interval > 1.0f)
        direct_sim_interval = 1.0f;
    batch_source_updates = config_bool(config, "batch_source_updates", batch_source_updates);
}

} // namespace godot
