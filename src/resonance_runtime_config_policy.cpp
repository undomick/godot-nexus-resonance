#include "resonance_runtime_config_policy.h"

#include <cstring>

namespace resonance {
namespace {

bool cstr_equal(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) {
        return a == b;
    }
    return std::strcmp(a, b) == 0;
}

bool name_in_list(const char* property, const char* const* names, size_t count) {
    if (property == nullptr) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (cstr_equal(property, names[i])) {
            return true;
        }
    }
    return false;
}

const char* const kEngineReinitProperties[] = {
    "ambisonic_order",
    "simulation_cpu_cores_percent",
    "realtime_rays",
    "realtime_irradiance_min_distance",
    "realtime_num_diffuse_samples",
    "realtime_bounces",
    "realtime_simulation_duration",
    "reflection_type",
    "hybrid_reverb_transition_time",
    "hybrid_reverb_overlap_percent",
    "use_virtual_surround",
    "direct_speaker_channels",
    "direct_binaural",
    "reverb_binaural",
    "pathing_binaural",
    "hrtf_volume_db",
    "hrtf_normalization_type",
    "hrtf_sofa_assets",
    "hrtf_sofa_selected_index",
    "hrtf_interpolation_bilinear",
    "pathing_num_samples",
    "transmission_type",
    "max_transmission_surfaces",
    "occlusion_type",
    "max_occlusion_samples",
    "max_simulation_sources",
    "scene_type",
    "physics_ray_collision_mask",
    "physics_ray_batch_size",
    "opencl_device_type",
    "opencl_device_index",
    "batch_source_updates",
};

const char* const kLivePatchDictKeys[] = {
    "perspective_correction_enabled",
    "perspective_correction_factor",
    "default_reflections_mode",
    "reflections_sim_interval",
    "pathing_sim_interval",
    "direct_sim_interval",
    "dynamic_scene_commit_min_interval",
    "realtime_reflection_max_distance_m",
    "reflections_adaptive_budget_us",
    "reflections_adaptive_ray_min",
    "reflections_adaptive_ray_recover_frac",
    "reflections_adaptive_ray_recover_cap",
    "reflections_adaptive_step_sec",
    "reflections_adaptive_max_extra_interval",
    "reflections_adaptive_decay_per_sec",
    "reflections_defer_after_scene_commit_us",
    "convolution_ir_max_samples",
    "pathing_vis_range",
    "pathing_vis_radius",
    "pathing_vis_threshold",
    "pathing_enabled",
    "pathing_normalize_eq",
    "path_validation_enabled",
    "find_alternate_paths",
};

const char* const kRoutingProperties[] = {
    "bus",
    "reverb_bus_name",
};

const char* const kLiveProperties[] = {
    "perspective_correction_enabled",
    "perspective_correction_factor",
    "default_reflections_mode",
    "direct_sim_interval",
    "reflections_sim_interval",
    "pathing_sim_interval",
    "dynamic_scene_commit_min_interval",
    "realtime_reflection_max_distance",
    "reflections_adaptive_budget_us",
    "reflections_adaptive_ray_min",
    "reflections_adaptive_ray_recover_frac",
    "reflections_adaptive_ray_recover_cap",
    "reflections_adaptive_step_sec",
    "reflections_adaptive_max_extra_interval",
    "reflections_adaptive_decay_per_sec",
    "reflections_defer_after_scene_commit_us",
    "convolution_ir_max_samples",
    "pathing_vis_range",
    "pathing_vis_radius",
    "pathing_vis_threshold",
    "pathing_enabled",
    "pathing_normalize_eq",
    "path_validation_enabled",
    "find_alternate_paths",
};

} // namespace

bool runtime_config_property_requires_engine_reinit(const char* property) {
    if (runtime_config_property_requires_routing_refresh(property) || runtime_config_property_is_live_patchable(property)) {
        return false;
    }
    return name_in_list(property, kEngineReinitProperties,
                        sizeof(kEngineReinitProperties) / sizeof(kEngineReinitProperties[0]));
}

bool runtime_config_property_is_live_patchable(const char* property) {
    return name_in_list(property, kLiveProperties, sizeof(kLiveProperties) / sizeof(kLiveProperties[0]));
}

bool runtime_config_property_requires_routing_refresh(const char* property) {
    return name_in_list(property, kRoutingProperties, sizeof(kRoutingProperties) / sizeof(kRoutingProperties[0]));
}

bool runtime_config_dict_key_is_live_patchable(const char* key) {
    return name_in_list(key, kLivePatchDictKeys, sizeof(kLivePatchDictKeys) / sizeof(kLivePatchDictKeys[0]));
}

} // namespace resonance
