#ifndef RESONANCE_SERVER_CONFIG_H
#define RESONANCE_SERVER_CONFIG_H

#include <functional>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/variant.hpp>

#include "resonance_constants.h"
#include "resonance_sofa_asset.h"

namespace godot {

struct ResonanceServerConfig {
    int sample_rate = 48000;
    int frame_size = resonance::kGodotDefaultFrameSize;
    int ambisonic_order = 1;

    int simulation_threads = 1;
    float simulation_cpu_cores_percent = resonance::kDefaultSimulationCpuCoresPercent;
    int max_rays = resonance::kDefaultRealtimeRays;
    int max_bounces = 4;
    /// Endpoint radius for STATICSOURCE/STATICLISTENER lookups whose bake sets no radius.
    float reverb_influence_radius = resonance::kBakedEndpointRadius;

    int reflection_type = 0;
    float hybrid_reverb_transition_time = 1.0f;
    float hybrid_reverb_overlap_percent = 0.25f;
    int transmission_type = 1;
    /// Default numTransmissionRays for new sources and players on the global default (1-256).
    int max_transmission_surfaces = resonance::kDefaultPlayerConfigTransmissionRays;
    int occlusion_type = 1;
    /// IPLSimulationSettings::maxNumOcclusionSamples (cap for volumetric occlusion)
    int max_occlusion_samples = resonance::kMaxOcclusionSamples;
    /// User source cap. maxNumSources and TAN maxSources add the listener reverb slots on top.
    int max_simulation_sources = resonance::kMaxSimulationSources;

    float hrtf_volume_db = 0.0f;
    /// 0 = none, 1 = RMS. Only for the built-in HRTF; a SOFA asset uses its own norm_type.
    int hrtf_normalization_type = 0;
    Ref<ResonanceSOFAAsset> hrtf_sofa_asset;
    /// HRTF instead of panning on the direct path, for players on the global default.
    bool direct_binaural = true;
    /// HRTF when decoding reflection and reverb Ambisonics.
    bool reverb_binaural = true;
    bool pathing_binaural = true;
    bool hrtf_interpolation_bilinear = false;
    bool use_virtual_surround = false;
    /// Speaker count for direct-path panning: 1, 2, 4, 6 or 8. Other values become 2.
    int direct_speaker_channels = 2;

    bool pathing_enabled = false;
    float pathing_vis_radius = 1.0f;
    float pathing_vis_threshold = 0.1f;
    float pathing_vis_range = 1000.0f;
    bool pathing_normalize_eq = true;
    /// Visibility samples for pathing bake and runtime (1-16).
    int pathing_num_samples = resonance::kRuntimePathingDefaultNumVisSamples;
    /// Default for players on Use Global (-1): re-check baked paths against dynamic occluders.
    bool path_validation_enabled = true;
    /// Default for players on Use Global (-1). Has no effect without path validation.
    bool find_alternate_paths = false;

    /// 0=Default (built-in Phonon), 1=Embree, 2=Radeon Rays, 3=Custom (Godot Physics)
    int scene_type = 0;
    /// Godot PhysicsRayQueryParameters3D collision_mask when scene_type==3; -1 = all layers.
    int physics_ray_collision_mask = -1;
    /// rayBatchSize for scene_type 3 only (1-256); other scene types use 1.
    int physics_ray_batch_size = resonance::kDefaultPhysicsRayBatchSize;
    int opencl_device_type = 0; // 0=GPU, 1=CPU, 2=Any
    int opencl_device_index = 0;

    /// Steam Audio API validation; slow, for debugging bad simulation inputs.
    bool context_validation = false;
    int context_simd_level = -1;

    float realtime_irradiance_min_distance = 0.1f;
    float realtime_simulation_duration = 2.0f;
    int realtime_num_diffuse_samples = 32;

    bool output_direct_enabled = true;
    bool output_reverb_enabled = true;
    bool debug_occlusion = false;
    bool debug_reflections = false;
    bool debug_pathing = false;

    bool perspective_correction_enabled = false;
    float perspective_correction_factor = 1.0f;

    /// Seconds between applying queued dynamic instance transforms and committing the scene. 0 = every drain.
    float dynamic_scene_commit_min_interval = 0.0f;
    /// Min seconds between reflection simulation ticks.
    float reflections_sim_interval = 0.1f;
    float pathing_sim_interval = 0.1f;
    /// Sources farther than this from the listener skip realtime reflections. 0 = no limit.
    float realtime_reflection_max_distance_m = 0.0f;
    /// Microseconds. A slower RunReflections stretches the reflection interval and lowers rays. 0 = off.
    int reflections_adaptive_budget_us = 0;
    /// Lowest numRays adaptive scaling may use.
    int reflections_adaptive_ray_min = 128;
    /// Fraction of max_rays added back per under-budget reflection tick.
    float reflections_adaptive_ray_recover_frac = 0.125f;
    /// Max rays added back per tick.
    int reflections_adaptive_ray_recover_cap = 512;
    /// Seconds added to the reflection interval per over-budget tick.
    float reflections_adaptive_step_sec = 0.02f;
    float reflections_adaptive_max_extra_interval = 0.2f;
    /// Seconds of extra interval removed per second while under budget.
    float reflections_adaptive_decay_per_sec = 0.05f;
    /// Microseconds. A scene commit at least this slow skips RunReflections until the next wake. 0 = off.
    int reflections_defer_after_scene_commit_us = 0;
    /// Max applied convolution IR length in samples. 0 = full allocated IR.
    int convolution_ir_max_samples = 0;
    /// Seconds between direct simulation runs on non-heavy ticks. 0 = every worker wake.
    float direct_sim_interval = 0.0f;
    /// Players queue source updates and ResonanceRuntime flushes them once per frame.
    bool batch_source_updates = true;

    /// Clamps every value and accepts legacy keys. get_bake_pathing_param supplies pathing_vis_* the config omits.
    void apply(const Dictionary& config,
               std::function<float(const char*, float)> get_bake_pathing_param = nullptr);

  private:
    static int config_int(const Dictionary& c, const char* key, int def);
    static float config_float(const Dictionary& c, const char* key, float def);
    static bool config_bool(const Dictionary& c, const char* key, bool def);
    static void config_sofa_asset(const Dictionary& c, const char* key, Ref<ResonanceSOFAAsset>& out);
    /// Prefers new_key, then legacy per-band interval, then simulation_update_interval; clamped to [0, 1] s.
    static float resolve_sim_interval_sec(const Dictionary& c, const char* new_key, const char* legacy_sub_key);
};

} // namespace godot

#endif // RESONANCE_SERVER_CONFIG_H
