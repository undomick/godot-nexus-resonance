#ifndef RESONANCE_SERVER_CONFIG_H
#define RESONANCE_SERVER_CONFIG_H

#include <functional>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/variant.hpp>

#include "resonance_constants.h"
#include "resonance_sofa_asset.h"

namespace godot {

/// Runtime config loaded from ProjectSettings / `init_audio_engine`: defaults, clamps, and Dictionary parsing (`apply`).
struct ResonanceServerConfig {
    // Audio
    int sample_rate = 48000;
    int frame_size = resonance::kGodotDefaultFrameSize;
    int ambisonic_order = 1;
    float max_reverb_duration = 2.0f;

    // Simulation
    int simulation_threads = 1;
    float simulation_cpu_cores_percent = resonance::kDefaultSimulationCpuCoresPercent;
    int max_rays = 4096;
    int max_bounces = 4;
    float reverb_influence_radius = 10000.0f;
    float reverb_transmission_amount = 1.0f;
    /// If true (default), damp baked REVERB wet by direct-path occlusion×transmission so walls reduce the wet IR (which itself does not encode source/listener geometry). Set false for always-on stylised reverb beds.
    bool apply_occlusion_to_baked_reflections = true;
    /// If true (default), baked REVERB picks the probe nearest the listener (Steam Audio IPL_BAKEDDATAVARIATION_REVERB assumes source==listener). Disable to fall back to legacy source-position lookup.
    bool baked_reverb_use_listener_probe = true;

    // Reflection
    int reflection_type = 0;
    /// When player reflections_type is Use Global: 0=Baked, 1=Realtime
    int default_reflections_mode = 0;
    float hybrid_reverb_transition_time = 1.0f;
    float hybrid_reverb_overlap_percent = 0.25f;
    int transmission_type = 1;
    /// Default numTransmissionRays for new sources and player-config fallback (1–256).
    int max_transmission_surfaces = resonance::kDefaultPlayerConfigTransmissionRays;
    int occlusion_type = 1;
    /// IPLSimulationSettings::maxNumOcclusionSamples (cap for volumetric occlusion)
    int max_occlusion_samples = resonance::kMaxOcclusionSamples;
    /// IPLSimulationSettings::maxNumSources (and TAN maxSources)
    int max_simulation_sources = resonance::kMaxSimulationSources;

    // HRTF
    float hrtf_volume_db = 0.0f;
    /// 0=None (IPL_HRTFNORMTYPE_NONE), 1=RMS - applies to embedded default HRTF only; SOFA uses asset norm_type.
    int hrtf_normalization_type = 0;
    Ref<ResonanceSOFAAsset> hrtf_sofa_asset;
    /// HRTF on direct dry path (vs panning) when ResonancePlayerConfig uses global default.
    bool direct_binaural = true;
    /// HRTF when decoding reflection/reverb Ambisonics (mixer return / ResonanceAudioEffect).
    bool reverb_binaural = true;
    /// HRTF in pathing effect output (indirect paths around obstacles).
    bool pathing_binaural = true;
    bool hrtf_interpolation_bilinear = false;
    bool use_virtual_surround = false;
    /// Direct path non-HRTF panning: 1,2,4,6,8 (Mono/Stereo/Quad/5.1/7.1). Invalid values become stereo.
    int direct_speaker_channels = 2;

    // Pathing
    bool pathing_enabled = false;
    float pathing_vis_radius = 0.5f;
    float pathing_vis_threshold = 0.1f;
    float pathing_vis_range = 100.0f;
    bool pathing_normalize_eq = true;
    /// Runtime pathing: IPL numVisSamples when pathing on (1–16). Independent of bake_pathing_num_samples.
    int pathing_num_vis_samples = resonance::kRuntimePathingDefaultNumVisSamples;
    /// Default when ResonancePlayerConfig uses Use Global (-1) for path validation (dynamic occlusion of baked paths).
    bool path_validation_enabled = true;
    /// Default when player uses Use Global (-1) for find-alternate-paths (requires validation on to take effect).
    bool find_alternate_paths = false;

    // Ray tracer / OpenCL / Radeon Rays
    /// 0=Default (built-in Phonon), 1=Embree, 2=Radeon Rays, 3=Custom (Godot Physics)
    int scene_type = 0;
    /// Godot PhysicsRayQueryParameters3D collision_mask when scene_type==3; -1 = all layers.
    int physics_ray_collision_mask = -1;
    /// IPLSimulationSettings::rayBatchSize when scene_type is Custom (Godot physics); clamped 1–256. Ignored for other scene types (native uses 1).
    int physics_ray_batch_size = resonance::kDefaultPhysicsRayBatchSize;
    int opencl_device_type = 0; // 0=GPU, 1=CPU, 2=Any
    int opencl_device_index = 0;

    // Context
    /// Enable IPL context validation (stricter API checks; useful when debugging bad sim inputs).
    bool context_validation = false;
    int context_simd_level = -1;

    // Realtime reflection quality
    float realtime_irradiance_min_distance = 0.1f;
    float realtime_simulation_duration = 2.0f;
    int realtime_num_diffuse_samples = 32;

    // Output flags
    bool output_direct_enabled = true;
    bool output_reverb_enabled = true;
    bool debug_occlusion = false;
    bool debug_reflections = false;
    bool debug_pathing = false;

    // Perspective correction
    bool perspective_correction_enabled = false;
    float perspective_correction_factor = 1.0f;

    // Pacing / coalescing
    /// Min wall time between applying queued dynamic instanced transforms + scene commit (0 = every drain).
    float dynamic_scene_commit_min_interval = 0.0f;
    /// Min seconds between reflection-heavy sim ticks (`resolve_sim_interval_sec` reads legacy keys too).
    float reflections_sim_interval = 0.1f;
    /// Min seconds between pathing-heavy sim ticks.
    float pathing_sim_interval = 0.1f;
    /// 0 = disabled. >0: realtime reflection simulation inputs omit IPL_SIMULATIONFLAGS_REFLECTIONS when source is farther than this (meters) from listener.
    float realtime_reflection_max_distance_m = 0.0f;
    /// 0 = off. If last RunReflections exceeded this many microseconds, increase reflection cadence spacing (see reflections_adaptive_*).
    int reflections_adaptive_budget_us = 0;
    /// Minimum sharedInputs.numRays used when adaptive realtime ray scaling is active (budget > 0).
    int reflections_adaptive_ray_min = 128;
    /// Fraction of max realtime rays to add back per under-budget reflections tick when adaptive ray scaling is active.
    float reflections_adaptive_ray_recover_frac = 0.125f;
    /// Upper bound for per-tick ray recovery when adaptive ray scaling is active (prevents jumping straight back to max_rays).
    int reflections_adaptive_ray_recover_cap = 512;
    /// Seconds added to effective reflection interval per over-budget worker tick (capped by reflections_adaptive_max_extra_interval).
    float reflections_adaptive_step_sec = 0.02f;
    /// Upper bound for extra delay from adaptive scheduling (seconds).
    float reflections_adaptive_max_extra_interval = 0.2f;
    /// Per-second reduction of adaptive extra interval when under budget (or worker did not run reflections).
    float reflections_adaptive_decay_per_sec = 0.05f;
    /// 0 = off. If scene graph commit took at least this many microseconds, skip RunReflections this wake and force next tick to retry.
    int reflections_defer_after_scene_commit_us = 0;
    /// 0 = no cap. Convolution path: clamp applied IR size to this many samples (min with allocated effect IR).
    int convolution_ir_max_samples = 0;
    /// 0 = run iplSimulatorRunDirect every worker wake (default). >0 = at most once per this many seconds on non-heavy ticks.
    float direct_sim_interval = 0.0f;
    /// When true, ResonanceRuntime flushes pending source updates once per frame; players enqueue instead of try_update_source.
    bool batch_source_updates = true;

    /// Reads ProjectSettings-style keys, clamps ranges, migrates legacy names. `get_bake_pathing_param` fills pathing_vis_* if omitted.
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
