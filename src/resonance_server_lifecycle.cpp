#include "resonance_constants.h"
#include "resonance_epoch.h"
#include "resonance_geometry.h"
#include "resonance_log.h"
#include "resonance_math.h"
#include "resonance_pathing_inputs_policy.h"
#include "resonance_phonon_worker_policy.h"
#include "resonance_reflection_type_policy.h"
#include "resonance_runtime_config_policy.h"
#include "resonance_server.h"
#include "resonance_source_handle_policy.h"
#include "resonance_spatial_warmup_policy.h"
#include "resonance_utils.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <limits>
#include <vector>

using namespace godot;

// Server lifecycle: IPL init/reinit/shutdown, worker vs main-thread simulation, tick scheduling, and phonon run loop.

namespace {

String ambient_order_ordinal(int64_t n) {
    uint64_t abs_part;
    if (n == std::numeric_limits<int64_t>::min())
        abs_part = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1u;
    else if (n < 0)
        abs_part = static_cast<uint64_t>(-n);
    else
        abs_part = static_cast<uint64_t>(n);
    int64_t mod10 = static_cast<int64_t>(abs_part % 10ULL);
    int64_t mod100 = static_cast<int64_t>(abs_part % 100ULL);
    if (mod100 >= 11 && mod100 <= 13)
        return String::num_int64(n) + "th";
    if (mod10 == 1)
        return String::num_int64(n) + "st";
    if (mod10 == 2)
        return String::num_int64(n) + "nd";
    if (mod10 == 3)
        return String::num_int64(n) + "rd";
    return String::num_int64(n) + "th";
}

} // namespace

std::atomic<bool> ResonanceServer::is_shutting_down_flag{false};
static ResonanceServer* g_resonance_server_singleton = nullptr;

ResonanceServer::ResonanceServer() {
    g_resonance_server_singleton = this;
    // Init is explicit via init()/reinit(); ctor only primes cache slots.
    reverb_param_cache_front_.store(0, std::memory_order_release);
    reflection_param_cache_front_.store(0, std::memory_order_release);
    pathing_param_cache_front_.store(0, std::memory_order_release);
    occlusion_cache_front_.store(0, std::memory_order_release);
    for (size_t i = 0; i < reflections_pending_.size(); i++)
        reflections_pending_[i].store(false, std::memory_order_release);
    for (int slot = 0; slot < kCacheSlots; slot++) {
        for (int i = 0; i < kMaxCacheHandles; i++) {
            reverb_param_cache_[static_cast<size_t>(slot)][static_cast<size_t>(i)].epoch = 0;
            reflection_param_cache_[static_cast<size_t>(slot)][static_cast<size_t>(i)].epoch = 0;
            pathing_param_cache_[static_cast<size_t>(slot)][static_cast<size_t>(i)].epoch = 0;
            occlusion_cache_[static_cast<size_t>(slot)][static_cast<size_t>(i)].epoch = 0;
        }
    }
}

ResonanceServer::~ResonanceServer() {
    is_shutting_down_flag.store(true, std::memory_order_release);
    _shutdown_steam_audio();
    if (g_resonance_server_singleton == this)
        g_resonance_server_singleton = nullptr;
}

ResonanceServer* ResonanceServer::get_singleton() { return g_resonance_server_singleton; }

bool ResonanceServer::ipl_audio_teardown_active() {
    if (is_shutting_down_flag.load(std::memory_order_acquire))
        return true;
    ResonanceServer* s = get_singleton();
    return s && s->ipl_teardown_active_.load(std::memory_order_acquire);
}

void ResonanceServer::register_ipl_context_client(void* key, IplContextClientCleanup cleanup) {
    if (!key || !cleanup)
        return;
    std::lock_guard<std::mutex> lock(ipl_context_clients_mutex_);
    for (IplContextClient& e : ipl_context_clients_) {
        if (e.key == key) {
            e.cleanup = cleanup;
            return;
        }
    }
    ipl_context_clients_.push_back(IplContextClient{key, cleanup});
}

void ResonanceServer::unregister_ipl_context_client(void* key) {
    if (!key)
        return;
    std::lock_guard<std::mutex> lock(ipl_context_clients_mutex_);
    auto& v = ipl_context_clients_;
    v.erase(std::remove_if(v.begin(), v.end(), [key](const IplContextClient& e) { return e.key == key; }), v.end());
}

void ResonanceServer::_drain_ipl_context_clients_assume_audio_locked() {
    std::vector<IplContextClient> clients_copy;
    {
        std::lock_guard<std::mutex> lock(ipl_context_clients_mutex_);
        clients_copy = std::move(ipl_context_clients_);
        ipl_context_clients_.clear();
    }
    for (const IplContextClient& c : clients_copy) {
        if (c.cleanup && c.key)
            c.cleanup(c.key);
    }
}

void ResonanceServer::_drain_ipl_context_clients_before_context_destroy() {
    AudioServer* audio = AudioServer::get_singleton();
    if (audio)
        audio->lock();
    _drain_ipl_context_clients_assume_audio_locked();
    if (audio)
        audio->unlock();
}

void ResonanceServer::shutdown() {
    is_shutting_down_flag.store(true, std::memory_order_release);
    _shutdown_steam_audio();
}

void ResonanceServer::_apply_config(Dictionary config) {
    config_.apply(config, [this](const char* key, float def) { return _get_bake_pathing_param(key, def); });
    if (config.has("audio_frame_size_was_auto")) {
        Variant v = config["audio_frame_size_was_auto"];
        audio_frame_size_was_auto_.store(v.operator bool(), std::memory_order_release);
    } else {
        audio_frame_size_was_auto_.store(true, std::memory_order_release);
    }
    current_sample_rate = config_.sample_rate;
    frame_size = config_.frame_size;
    ambisonic_order = config_.ambisonic_order;
    simulation_threads = config_.simulation_threads;
    simulation_cpu_cores_percent = config_.simulation_cpu_cores_percent;
    max_rays = config_.max_rays;
    max_bounces = config_.max_bounces;
    reverb_influence_radius = config_.reverb_influence_radius;
    reflection_type = config_.reflection_type;
    default_reflections_mode = config_.default_reflections_mode;
    hybrid_reverb_transition_time = config_.hybrid_reverb_transition_time;
    hybrid_reverb_overlap_percent = config_.hybrid_reverb_overlap_percent;
    transmission_type = config_.transmission_type;
    max_transmission_surfaces = config_.max_transmission_surfaces;
    occlusion_type = config_.occlusion_type;
    max_occlusion_samples = config_.max_occlusion_samples;
    max_simulation_sources = config_.max_simulation_sources;
    hrtf_volume_db = config_.hrtf_volume_db;
    hrtf_normalization_type = config_.hrtf_normalization_type;
    hrtf_sofa_asset = config_.hrtf_sofa_asset;
    direct_binaural = config_.direct_binaural;
    reverb_binaural = config_.reverb_binaural;
    pathing_binaural = config_.pathing_binaural;
    use_virtual_surround = config_.use_virtual_surround;
    direct_speaker_channels = config_.direct_speaker_channels;
    hrtf_interpolation_bilinear = config_.hrtf_interpolation_bilinear;
    pathing_enabled = config_.pathing_enabled;
    pathing_vis_radius = config_.pathing_vis_radius;
    pathing_vis_threshold = config_.pathing_vis_threshold;
    pathing_vis_range = config_.pathing_vis_range;
    pathing_normalize_eq = config_.pathing_normalize_eq;
    pathing_num_samples = config_.pathing_num_samples;
    path_validation_enabled = config_.path_validation_enabled;
    find_alternate_paths = config_.find_alternate_paths;
    scene_type = config_.scene_type;
    physics_ray_batch_size = config_.physics_ray_batch_size;
    {
        int pm = config_.physics_ray_collision_mask;
        uint32_t um = (pm < 0) ? 0xFFFFFFFFu : static_cast<uint32_t>(pm);
        godot_physics_bridge_.set_collision_mask(um);
    }
    opencl_device_type = config_.opencl_device_type;
    opencl_device_index = config_.opencl_device_index;
    context_validation = config_.context_validation;
    context_simd_level = config_.context_simd_level;
    realtime_irradiance_min_distance = config_.realtime_irradiance_min_distance;
    realtime_simulation_duration = config_.realtime_simulation_duration;
    realtime_num_diffuse_samples = config_.realtime_num_diffuse_samples;
    output_direct_enabled.store(config_.output_direct_enabled, std::memory_order_relaxed);
    output_reverb_enabled.store(config_.output_reverb_enabled, std::memory_order_relaxed);
    debug_occlusion.store(config_.debug_occlusion, std::memory_order_relaxed);
    debug_reflections.store(config_.debug_reflections, std::memory_order_relaxed);
    debug_pathing.store(config_.debug_pathing, std::memory_order_relaxed);
    perspective_correction_enabled.store(config_.perspective_correction_enabled, std::memory_order_relaxed);
    perspective_correction_factor.store(config_.perspective_correction_factor, std::memory_order_relaxed);
    dynamic_scene_commit_min_interval_ = config_.dynamic_scene_commit_min_interval;
    reflections_sim_interval = config_.reflections_sim_interval;
    pathing_sim_interval = config_.pathing_sim_interval;
    realtime_reflection_max_distance_m = config_.realtime_reflection_max_distance_m;
    reflections_adaptive_budget_us_ = static_cast<uint32_t>(config_.reflections_adaptive_budget_us);
    reflections_adaptive_ray_min_ = config_.reflections_adaptive_ray_min;
    reflections_adaptive_ray_recover_frac_ = config_.reflections_adaptive_ray_recover_frac;
    reflections_adaptive_ray_recover_cap_ = config_.reflections_adaptive_ray_recover_cap;
    reflections_adaptive_step_sec_ = config_.reflections_adaptive_step_sec;
    reflections_adaptive_max_extra_interval_ = config_.reflections_adaptive_max_extra_interval;
    reflections_adaptive_decay_per_sec_ = config_.reflections_adaptive_decay_per_sec;
    reflections_defer_after_scene_commit_us_ = static_cast<uint32_t>(config_.reflections_defer_after_scene_commit_us);
    convolution_ir_max_samples_ = config_.convolution_ir_max_samples;
    reflections_adaptive_extra_interval_ = 0.0f;
    // Reset adaptive ray scaler state when configuration changes (rays/budget/min knobs).
    _adaptive_realtime_num_rays_initialized_ = false;
    direct_sim_interval = config_.direct_sim_interval;
    batch_source_updates = config_.batch_source_updates;
    reflections_interval_elapsed = 0.0f;
    pathing_interval_elapsed = 0.0f;
    direct_sim_time_elapsed = (direct_sim_interval > 0.0f) ? direct_sim_interval : 0.0f;
    worker_run_direct_next.store(true, std::memory_order_relaxed);
    direct_after_inline_inputs_pending_.store(false, std::memory_order_relaxed);
    reflection_sim_heavy_requested.store(false, std::memory_order_relaxed);
    pathing_sim_heavy_requested.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> b(source_update_batch_mutex_);
        source_update_batch_.clear();
    }
}

void ResonanceServer::set_default_reflections_mode(int p_mode) {
    if (p_mode < 0) {
        p_mode = 0;
    }
    if (p_mode > 1) {
        p_mode = 1;
    }
    default_reflections_mode = p_mode;
}

void ResonanceServer::patch_live_runtime_scheduling(const Dictionary& config) {
    if (config.has("reflections_sim_interval")) {
        reflections_sim_interval = (float)config["reflections_sim_interval"];
    }
    if (config.has("pathing_sim_interval")) {
        pathing_sim_interval = (float)config["pathing_sim_interval"];
    }
    if (config.has("direct_sim_interval")) {
        direct_sim_interval = (float)config["direct_sim_interval"];
        direct_sim_time_elapsed = (direct_sim_interval > 0.0f) ? direct_sim_interval : 0.0f;
    }
    if (config.has("dynamic_scene_commit_min_interval")) {
        dynamic_scene_commit_min_interval_ = (float)config["dynamic_scene_commit_min_interval"];
    }
    if (config.has("realtime_reflection_max_distance_m")) {
        realtime_reflection_max_distance_m = (float)config["realtime_reflection_max_distance_m"];
    }
    if (config.has("reflections_adaptive_budget_us")) {
        reflections_adaptive_budget_us_ = static_cast<uint32_t>((int)config["reflections_adaptive_budget_us"]);
        reflections_adaptive_extra_interval_ = 0.0f;
        _adaptive_realtime_num_rays_initialized_ = false;
    }
    if (config.has("reflections_adaptive_ray_min")) {
        reflections_adaptive_ray_min_ = (int)config["reflections_adaptive_ray_min"];
        _adaptive_realtime_num_rays_initialized_ = false;
    }
    if (config.has("reflections_adaptive_ray_recover_frac")) {
        reflections_adaptive_ray_recover_frac_ = (float)config["reflections_adaptive_ray_recover_frac"];
    }
    if (config.has("reflections_adaptive_ray_recover_cap")) {
        reflections_adaptive_ray_recover_cap_ = (int)config["reflections_adaptive_ray_recover_cap"];
    }
    if (config.has("reflections_adaptive_step_sec")) {
        reflections_adaptive_step_sec_ = (float)config["reflections_adaptive_step_sec"];
    }
    if (config.has("reflections_adaptive_max_extra_interval")) {
        reflections_adaptive_max_extra_interval_ = (float)config["reflections_adaptive_max_extra_interval"];
    }
    if (config.has("reflections_adaptive_decay_per_sec")) {
        reflections_adaptive_decay_per_sec_ = (float)config["reflections_adaptive_decay_per_sec"];
    }
    if (config.has("reflections_defer_after_scene_commit_us")) {
        reflections_defer_after_scene_commit_us_ = static_cast<uint32_t>((int)config["reflections_defer_after_scene_commit_us"]);
    }
    if (config.has("convolution_ir_max_samples")) {
        convolution_ir_max_samples_ = (int)config["convolution_ir_max_samples"];
    }
}

bool ResonanceServer::patch_live_runtime_config(Dictionary config) {
    if (!is_initialized()) {
        return false;
    }
    // Pathing enable needs recreate only if this simulator was built without PATHING (legacy).
    // Current init always sets PATHING at iplSimulatorCreate.
    if (config.has("pathing_enabled")) {
        const bool want_pathing = (bool)config["pathing_enabled"];
        if (resonance::pathing_enable_requires_simulator_recreate(want_pathing, simulator_created_with_pathing_)) {
            return true;
        }
        if (want_pathing != pathing_enabled) {
            set_pathing_enabled(want_pathing);
            _invalidate_all_source_update_snapshots();
        }
    }
    if (config.has("perspective_correction_enabled")) {
        set_perspective_correction_enabled((bool)config["perspective_correction_enabled"]);
    }
    if (config.has("perspective_correction_factor")) {
        set_perspective_correction_factor((float)config["perspective_correction_factor"]);
    }
    if (config.has("default_reflections_mode")) {
        set_default_reflections_mode((int)config["default_reflections_mode"]);
    }
    // Vis range/radius/threshold update without reinit.
    if (config.has("pathing_vis_range")) {
        float v = (float)config["pathing_vis_range"];
        if (v < 0.0f)
            v = 0.0f;
        if (v > 1000.0f)
            v = 1000.0f;
        pathing_vis_range = v;
        config_.pathing_vis_range = v;
    }
    if (config.has("pathing_vis_radius")) {
        float v = (float)config["pathing_vis_radius"];
        if (v < 0.0f)
            v = 0.0f;
        if (v > 2.0f)
            v = 2.0f;
        pathing_vis_radius = v;
        config_.pathing_vis_radius = v;
    }
    if (config.has("pathing_vis_threshold")) {
        float v = (float)config["pathing_vis_threshold"];
        if (v < 0.0f)
            v = 0.0f;
        if (v > 1.0f)
            v = 1.0f;
        pathing_vis_threshold = v;
        config_.pathing_vis_threshold = v;
    }
    if (config.has("pathing_normalize_eq")) {
        pathing_normalize_eq = (bool)config["pathing_normalize_eq"];
        config_.pathing_normalize_eq = pathing_normalize_eq;
    }
    if (config.has("path_validation_enabled")) {
        const bool want = (bool)config["path_validation_enabled"];
        if (want != path_validation_enabled) {
            path_validation_enabled = want;
            config_.path_validation_enabled = want;
            _invalidate_all_source_update_snapshots();
        }
    }
    if (config.has("find_alternate_paths")) {
        const bool want = (bool)config["find_alternate_paths"];
        if (want != find_alternate_paths) {
            find_alternate_paths = want;
            config_.find_alternate_paths = want;
            _invalidate_all_source_update_snapshots();
        }
    }
    patch_live_runtime_scheduling(config);
    return false;
}

bool ResonanceServer::runtime_config_property_requires_engine_reinit(const StringName& property) {
    const String utf8 = property;
    return resonance::runtime_config_property_requires_engine_reinit(utf8.utf8().get_data());
}

bool ResonanceServer::runtime_config_property_is_live_patchable(const StringName& property) {
    const String utf8 = property;
    return resonance::runtime_config_property_is_live_patchable(utf8.utf8().get_data());
}

bool ResonanceServer::runtime_config_property_requires_routing_refresh(const StringName& property) {
    const String utf8 = property;
    return resonance::runtime_config_property_requires_routing_refresh(utf8.utf8().get_data());
}

void ResonanceServer::init_audio_engine(Dictionary config) {
    if (_ctx() != nullptr) {
        UtilityFunctions::push_warning("Nexus Resonance: Already initialized.");
        return;
    }
    _apply_config(config);
    _init_internal();
}

void ResonanceServer::reinit_audio_engine(Dictionary config) {
    if (_ctx() == nullptr) {
        init_audio_engine(config);
        return;
    }
    _shutdown_steam_audio();
    _apply_config(config);
    _init_internal();
}

void ResonanceServer::_init_internal() {
    reverb_effect_process_calls.store(0, std::memory_order_relaxed);
    reverb_effect_mixer_null.store(0, std::memory_order_relaxed);
    reverb_effect_success.store(0, std::memory_order_relaxed);
    reverb_effect_frames_written.store(0, std::memory_order_relaxed);
    reverb_effect_output_peak.store(0.0f, std::memory_order_relaxed);
    reverb_mixer_feed_count.store(0, std::memory_order_relaxed);
    reverb_convolution_valid_fetches.store(0, std::memory_order_relaxed);
    reverb_convolution_feed_ir_null.store(0, std::memory_order_relaxed);
    reverb_convolution_gain_min.store(1.0f, std::memory_order_relaxed);
    reverb_convolution_gain_max.store(0.0f, std::memory_order_relaxed);
    reverb_convolution_input_rms_max.store(0.0f, std::memory_order_relaxed);
    instrumentation_fetch_cache_hit.store(0, std::memory_order_relaxed);
    instrumentation_fetch_cache_miss.store(0, std::memory_order_relaxed);
    instrumentation_fetch_cache_skip.store(0, std::memory_order_relaxed);
    reset_pathing_instrumentation();

    // Clear shutdown/teardown so a fresh context is usable after editor play / reinit.
    is_shutting_down_flag.store(false, std::memory_order_release);
    ipl_teardown_active_.store(false, std::memory_order_release);

    _init_context_and_devices();
    if (!steam_audio_context_) {
        ipl_teardown_active_.store(false, std::memory_order_release);
        is_shutting_down_flag.store(false, std::memory_order_release);
        return;
    }
    if (!_init_scene_and_simulator()) {
        ipl_teardown_active_.store(false, std::memory_order_release);
        is_shutting_down_flag.store(false, std::memory_order_release);
        return;
    }
    // Coalesce geometry/probe registration before the first RunReflections.
    cold_start_settle_pending_.store(true, std::memory_order_release);
    if (!_uses_main_thread_phonon_simulation())
        _start_worker_thread();

    String version_str = String::num_int64(STEAMAUDIO_VERSION_MAJOR) + "." + String::num_int64(STEAMAUDIO_VERSION_MINOR) + "." + String::num_int64(STEAMAUDIO_VERSION_PATCH);
    const char* refl_names[] = {"Convolution", "Parametric", "Hybrid", "TrueAudio Next"};
    int refl_idx = (reflection_type >= resonance::kReflectionConvolution && reflection_type <= resonance::kReflectionTan) ? reflection_type : resonance::kReflectionConvolution;
    String rays_str = (max_rays == 0) ? "Rays: Baked Only (0)" : "Rays (Realtime): " + String::num_int64(max_rays);
    String order_msg = " | Realtime Ambisonic: " + ambient_order_ordinal(ambisonic_order) + " Order" +
                       " | Bake Ambisonic: " + ambient_order_ordinal(_get_bake_ambisonics_order()) + " Order";
    String engine_msg = "Engine Started (Steam Audio " + version_str + "). Rate: " + String::num_int64(current_sample_rate) + order_msg +
                        " | Reflection: " + refl_names[refl_idx] + " | " + rays_str;
    UtilityFunctions::print_rich("[color=cyan]Nexus Resonance:[/color] " + engine_msg);
    ipl_teardown_active_.store(false, std::memory_order_release);
    is_shutting_down_flag.store(false, std::memory_order_release);
}

void ResonanceServer::_init_context_and_devices() {
    steam_audio_context_ = std::make_unique<ResonanceSteamAudioContext>();
    ResonanceSteamAudioContextConfig ctx_config{};
    ctx_config.sample_rate = current_sample_rate;
    ctx_config.frame_size = frame_size;
    ctx_config.ambisonic_order = ambisonic_order;
    ctx_config.max_reverb_duration = realtime_simulation_duration;
    ctx_config.reflection_type = reflection_type;
    ctx_config.scene_type = scene_type;
    ctx_config.opencl_device_type = opencl_device_type;
    ctx_config.opencl_device_index = opencl_device_index;
    ctx_config.context_validation = context_validation;
    ctx_config.context_simd_level = context_simd_level;
    ctx_config.hrtf_volume_db = hrtf_volume_db;
    ctx_config.hrtf_normalization_type = hrtf_normalization_type;
    ctx_config.max_simulation_sources = max_simulation_sources;
    ctx_config.hrtf_sofa_asset = hrtf_sofa_asset;

    if (!steam_audio_context_->init(ctx_config)) {
        steam_audio_context_.reset();
        return;
    }
    reflection_type = ctx_config.reflection_type;
    scene_type = ctx_config.scene_type;
    config_.scene_type = ctx_config.scene_type;

    if (debug_reflections.load(std::memory_order_acquire) && max_rays > 0) {
        ray_trace_debug_context_.clear();
        UtilityFunctions::print_rich(
            "[color=cyan]Nexus Resonance:[/color] Debug Reflections enabled – using Embree + standalone ray viz (independent of runtime scene_type; not Custom-scene raycasts).");
    }
}

bool ResonanceServer::_init_scene_and_simulator() {
    IPLAudioSettings audioSettings{current_sample_rate, frame_size};
    IPLSceneSettings sceneSettings{};
    sceneSettings.type = _scene_type();
    sceneSettings.embreeDevice = _embree();
    sceneSettings.radeonRaysDevice = _radeon();
    const int ray_batch =
        (_scene_type() == IPL_SCENETYPE_CUSTOM) ? resonance::clamp_physics_ray_batch_size(physics_ray_batch_size) : 1;
    if (_scene_type() == IPL_SCENETYPE_CUSTOM) {
        sceneSettings.closestHitCallback = &ResonanceGodotPhysicsSceneBridge::closest_hit_callback;
        sceneSettings.anyHitCallback = &ResonanceGodotPhysicsSceneBridge::any_hit_callback;
        if (ray_batch > 1) {
            sceneSettings.batchedClosestHitCallback = &ResonanceGodotPhysicsSceneBridge::batched_closest_hit_callback;
            sceneSettings.batchedAnyHitCallback = &ResonanceGodotPhysicsSceneBridge::batched_any_hit_callback;
        } else {
            sceneSettings.batchedClosestHitCallback = nullptr;
            sceneSettings.batchedAnyHitCallback = nullptr;
        }
        sceneSettings.userData = godot_physics_bridge_.user_data();
    }
    if (iplSceneCreate(_ctx(), &sceneSettings, &scene) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceServer: iplSceneCreate failed.");
        steam_audio_context_.reset();
        return false;
    }

    // Always allocate Direct|Reflections|Pathing at simulator create.
    // Pathing is gated at RunPathing / SetInputs via pathing_enabled, not at simulator create.
    simulation_settings.flags = static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS |
                                                                IPL_SIMULATIONFLAGS_PATHING);
    simulation_settings.sceneType = _scene_type();
    simulation_settings.reflectionType = resonance::reflection_type_for_simulator(reflection_type);
    simulation_settings.openCLDevice = _opencl();
    simulation_settings.tanDevice = _tan();
    simulation_settings.maxNumOcclusionSamples = max_occlusion_samples;
    // maxNumRays==0 is valid (baked-only).
    simulation_settings.maxNumRays = max_rays;
    simulation_settings.numDiffuseSamples = realtime_num_diffuse_samples;
    simulation_settings.maxDuration = realtime_simulation_duration;
    simulation_settings.samplingRate = current_sample_rate;
    simulation_settings.frameSize = frame_size;
    simulation_settings.maxOrder = ambisonic_order;
    simulation_settings.numThreads = simulation_threads;
    simulation_settings.maxNumSources = max_simulation_sources;
    // bakingVisibilitySamples / numVisSamples is always set on the runtime simulator.
    simulation_settings.numVisSamples = pathing_num_samples;
    simulation_settings.rayBatchSize = ray_batch;

    if (iplSimulatorCreate(_ctx(), &simulation_settings, &simulator) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceServer: iplSimulatorCreate failed.");
        iplSceneRelease(&scene);
        steam_audio_context_.reset();
        return false;
    }
    simulator_created_with_pathing_ = true;

    if (reflection_type == resonance::kReflectionConvolution || reflection_type == resonance::kReflectionTan) {
        IPLReflectionEffectSettings rs{};
        rs.type = (reflection_type == resonance::kReflectionTan) ? IPL_REFLECTIONEFFECTTYPE_TAN : IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
        rs.numChannels = get_num_channels_for_order();
        rs.irSize = resonance::reverb_ir_size_samples(current_sample_rate, realtime_simulation_duration);
        IPLReflectionMixer tmp_mixer = nullptr;
        if (iplReflectionMixerCreate(_ctx(), &audioSettings, &rs, &tmp_mixer) != IPL_STATUS_SUCCESS) {
            ResonanceLog::error("ResonanceServer: iplReflectionMixerCreate failed.");
            iplSimulatorRelease(&simulator);
            simulator_created_with_pathing_ = false;
            iplSceneRelease(&scene);
            steam_audio_context_.reset();
            return false;
        }
        _set_reflection_mixer(tmp_mixer);
    }

    iplSceneCommit(scene);
    iplSimulatorSetScene(simulator, scene);
    iplSimulatorCommit(simulator);

    {
        const bool batched_path = (_scene_type() == IPL_SCENETYPE_CUSTOM && ray_batch > 1);
        const char* st_label = "DEFAULT";
        if (_scene_type() == IPL_SCENETYPE_EMBREE)
            st_label = "EMBREE";
        else if (_scene_type() == IPL_SCENETYPE_RADEONRAYS)
            st_label = "RADEONRAYS";
        else if (_scene_type() == IPL_SCENETYPE_CUSTOM)
            st_label = "CUSTOM";
        if (batched_path)
            ResonanceLog::info(String("Nexus Resonance: simulator ") + st_label + ", rayBatchSize=" + String::num(ray_batch) +
                               " (Godot physics batched trace callbacks; Phonon BatchedReflectionSimulator path).");
        else if (_scene_type() == IPL_SCENETYPE_CUSTOM)
            ResonanceLog::info(String("Nexus Resonance: simulator ") + st_label + ", rayBatchSize=1 (single-ray callbacks per job).");
        else
            ResonanceLog::info(String("Nexus Resonance: simulator ") + st_label +
                               ", rayBatchSize=1 (Custom-only batching; Default/Embree use native tracer job layout).");
    }

    update_listener(Vector3(0, 0, 0), Vector3(0, 0, -1), Vector3(0, 1, 0));
    return true;
}

void ResonanceServer::_start_worker_thread() {
    thread_running.store(true, std::memory_order_release);
    worker_thread = std::thread(&ResonanceServer::_worker_thread_func, this);
}

void ResonanceServer::_stop_and_join_worker() {
    if (!thread_running.load(std::memory_order_acquire) && !worker_thread.joinable())
        return;
    thread_running.store(false, std::memory_order_release);
    worker_cv.notify_all();
    if (worker_thread.joinable())
        worker_thread.join();
}

void ResonanceServer::begin_tree_teardown() {
    // First EXIT_TREE among Geometry/Probe/Runtime: mute mix and join worker before N per-node Embree commits.
    if (!_ctx())
        return;
    if (is_shutting_down_flag.exchange(true, std::memory_order_acq_rel))
        return;
    ipl_teardown_active_.store(true, std::memory_order_release);
    simulation_requested.store(false, std::memory_order_release);
    reflection_sim_heavy_requested.store(false, std::memory_order_release);
    pathing_sim_heavy_requested.store(false, std::memory_order_release);
    cold_start_settle_pending_.store(false, std::memory_order_release);
    _stop_and_join_worker();
}

void ResonanceServer::finish_cold_start_settle() {
    if (!_ctx())
        return;
    cold_start_settle_pending_.store(false, std::memory_order_release);
    // First IR immediately after one coalesced scene commit (no reflections_sim_interval wait).
    // Dedicated worker: wake below. Custom (main-thread sim): wake is a no-op; next tick() runs First-IR.
    if (!reflections_have_run_once_.load(std::memory_order_acquire))
        reflection_sim_heavy_requested.store(true, std::memory_order_release);
    if (pathing_enabled)
        pathing_sim_heavy_requested.store(true, std::memory_order_release);
    _wake_phonon_worker_for_lifecycle();
}

void ResonanceServer::_wake_phonon_worker_for_lifecycle() {
    // Cold-start coalesce: scene_dirty accumulates; one wake from finish_cold_start_settle.
    if (cold_start_settle_pending_.load(std::memory_order_acquire))
        return;
    if (_uses_main_thread_phonon_simulation() || !thread_running.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::mutex> lock(worker_mutex);
    simulation_requested = true;
    worker_run_direct_next.store(true, std::memory_order_release);
    worker_cv.notify_one();
}

void ResonanceServer::_arm_phonon_direct_after_inline_inputs() {
    direct_after_inline_inputs_pending_.store(true, std::memory_order_release);
    _wake_phonon_worker_for_lifecycle();
}

void ResonanceServer::_consume_heavy_sim_flags(bool allow_heavy, bool drop_on_block, bool& run_refl, bool& run_path) {
    run_refl = false;
    run_path = false;
    if (allow_heavy) {
        run_refl = reflection_sim_heavy_requested.exchange(false, std::memory_order_acq_rel);
        run_path = pathing_sim_heavy_requested.exchange(false, std::memory_order_acq_rel);
        return;
    }
    if (!drop_on_block)
        return;
    // Drop pending heavy on shutdown/stop. During cold-start settle leave flags for finish_cold_start_settle.
    reflection_sim_heavy_requested.store(false, std::memory_order_release);
    pathing_sim_heavy_requested.store(false, std::memory_order_release);
}

void ResonanceServer::tick(float delta) {
    if (is_shutting_down_flag.load(std::memory_order_acquire))
        return;
    static std::atomic<bool> s_log_main_bound{false};
    if (!s_log_main_bound.exchange(true, std::memory_order_relaxed))
        resonance_log_bind_main_thread();
    resonance_log_drain_pending();
    _drain_deferred_reflection_mixers_if_pending();
    _try_release_deferred_reflection_mixers();
    // Cold-start: accumulate scene_dirty without schedule/wake; finish_cold_start_settle arms First-IR.
    if (cold_start_settle_pending_.load(std::memory_order_acquire))
        return;

    std::vector<int32_t> tick_source_handles;
    source_manager.get_all_handles(tick_source_handles);
    const bool any_direct_outputs = _any_source_has_direct_outputs(tick_source_handles);
    bool inline_inputs_pending = direct_after_inline_inputs_pending_.load(std::memory_order_acquire);
    if (resonance::phonon_worker_should_clear_stale_direct_after_inline_inputs(inline_inputs_pending, any_direct_outputs)) {
        direct_after_inline_inputs_pending_.store(false, std::memory_order_release);
        inline_inputs_pending = false;
    }
    const bool run_direct_this_wake = _tick_schedule_simulation(delta, tick_source_handles);
    const bool lifecycle_pending = _has_pending_source_lifecycle() || _has_pending_source_updates();
    const bool run_direct_for_sim =
        resonance::phonon_worker_run_direct_for_tick(run_direct_this_wake, lifecycle_pending, inline_inputs_pending);
    const bool run_refl_pending = reflection_sim_heavy_requested.load(std::memory_order_acquire);
    const bool run_path_pending = pathing_sim_heavy_requested.load(std::memory_order_acquire);
    if (!_phonon_worker_tick_has_work(run_direct_for_sim, run_refl_pending, run_path_pending, tick_source_handles)) {
        resonance::PhononWorkerTickInputs idle_in{};
        idle_in.pending_lifecycle = _has_pending_source_lifecycle();
        idle_in.pending_source_updates = _has_pending_source_updates();
        idle_in.scene_dirty = scene_dirty.load(std::memory_order_acquire);
        idle_in.pending_dynamic_transforms = _has_pending_dynamic_instanced_transforms();
        idle_in.run_reflection_heavy = run_refl_pending;
        idle_in.run_pathing_heavy = run_path_pending;
        idle_in.output_reverb_enabled = output_reverb_enabled.load(std::memory_order_acquire);
        idle_in.any_reflection_outputs = _any_source_has_reflection_outputs(tick_source_handles);
        idle_in.any_pathing_outputs = _any_source_has_pathing_outputs(tick_source_handles);
        if (resonance::phonon_worker_should_clear_idle_heavy_flags(idle_in)) {
            reflection_sim_heavy_requested.store(false, std::memory_order_release);
            pathing_sim_heavy_requested.store(false, std::memory_order_release);
        }
        return;
    }

    if (_uses_main_thread_phonon_simulation()) {
        // Custom (Godot Physics): Phonon on main/physics thread so IPL trace callbacks can use intersect_ray.
        // Steam Audio prefers RunDirect/Reflections/Pathing off the audio thread; see ARCHITECTURE.md.
        IPLCoordinateSpace3 listener_cs = _snapshot_listener_for_simulation();
        const bool shutting_down = is_shutting_down_flag.load(std::memory_order_acquire);
        const bool settling = cold_start_settle_pending_.load(std::memory_order_acquire);
        const bool allow_heavy =
            resonance::phonon_worker_allow_new_heavy_simulation(shutting_down, true, settling);
        bool run_refl = false;
        bool run_path = false;
        _consume_heavy_sim_flags(allow_heavy, shutting_down, run_refl, run_path);
        {
            std::lock_guard<std::mutex> exclusive_lock(phonon_context_exclusive_mutex_);
            std::lock_guard<std::mutex> sim_lock(simulation_mutex);
            _run_phonon_simulation_locked(listener_cs, run_direct_for_sim, run_refl, run_path);
        }
        return;
    }

    {
        std::lock_guard<std::mutex> lock(worker_mutex);
        simulation_requested = true;
        worker_run_direct_next.store(run_direct_for_sim, std::memory_order_release);
    }
    worker_cv.notify_one();
}

void ResonanceServer::_worker_thread_func() {
    // Waits for tick() to set simulation_requested; no thread when Custom scene uses main-thread simulation.
    while (thread_running) {
        std::unique_lock<std::mutex> lock(worker_mutex);
        worker_cv.wait(lock, [this] { return simulation_requested || !thread_running; });

        if (!thread_running)
            break;
        simulation_requested = false;

        IPLCoordinateSpace3 current_listener = _snapshot_listener_for_simulation();
        lock.unlock();

        if (_ctx() && simulator) {
            if (_uses_main_thread_phonon_simulation())
                continue;

            // Do not start a new heavy pass after shutdown / during cold-start settle; join waits only for in-flight Phonon.
            const bool shutting_down = is_shutting_down_flag.load(std::memory_order_acquire);
            const bool settling = cold_start_settle_pending_.load(std::memory_order_acquire);
            const bool thread_alive = thread_running.load(std::memory_order_acquire);
            const bool allow_heavy =
                resonance::phonon_worker_allow_new_heavy_simulation(shutting_down, thread_alive, settling);
            std::lock_guard<std::mutex> exclusive_lock(phonon_context_exclusive_mutex_);
            std::lock_guard<std::mutex> sim_lock(simulation_mutex);
            const bool run_direct = worker_run_direct_next.load(std::memory_order_acquire);
            bool run_refl = false;
            bool run_path = false;
            _consume_heavy_sim_flags(allow_heavy, shutting_down || !thread_alive, run_refl, run_path);
            _run_phonon_simulation_locked(current_listener, run_direct, run_refl, run_path);
        }
    }
}

IPLCoordinateSpace3 ResonanceServer::_snapshot_listener_for_simulation() {
    return _read_listener_coords_seqlock();
}

bool ResonanceServer::_uses_main_thread_phonon_simulation() const {
    return _scene_type() == IPL_SCENETYPE_CUSTOM;
}

IPLSceneType ResonanceServer::_tracer_type_for_mesh_operations() const {
    const IPLSceneType t = _scene_type();
    if (t == IPL_SCENETYPE_CUSTOM)
        return IPL_SCENETYPE_DEFAULT;
    return t;
}

void ResonanceServer::_shutdown_steam_audio() {
    // Bump client-held handle epoch before recycling IDs (e.g. auto frame-size reinit).
    source_lifecycle_epoch_.store(resonance::next_source_lifecycle_epoch(source_lifecycle_epoch_.load(std::memory_order_relaxed)),
                                  std::memory_order_release);
    _clear_physics_ray_excludes_state();
    godot_physics_bridge_.clear_world();
    if (!_ctx())
        return;

    ipl_teardown_active_.store(true, std::memory_order_release);

    // Reset gates before teardown so late audio/main paths stop using IPL.
    listener_seq_.store(0, std::memory_order_release);
    pending_listener_valid.store(false);
    simulation_requested.store(false);
    reflection_sim_heavy_requested.store(false);
    pathing_sim_heavy_requested.store(false);
    direct_after_inline_inputs_pending_.store(false, std::memory_order_release);
    scene_dirty.store(false);
    cold_start_settle_pending_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> q(dynamic_instanced_transform_queue_mutex_);
        dynamic_instanced_transform_queue_.clear();
    }
    spatial_audio_warmup_passes_remaining_.store(0, std::memory_order_release);
    phonon_scene_audio_ready_.store(true, std::memory_order_release);
    pathing_ran_this_tick.store(false);
    reflections_have_run_once_.store(false);
    for (size_t i = 0; i < reflections_pending_.size(); i++)
        reflections_pending_[i].store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> b(source_update_batch_mutex_);
        source_update_batch_.clear();
    }

    if (thread_running.load(std::memory_order_acquire) || worker_thread.joinable()) {
        _stop_and_join_worker();
    }
    // Worker stopped: drain queued Remove retains; pending Adds without attach are dropped with later release_all.
    {
        std::vector<PendingSourceAdd> local_adds;
        std::vector<IPLSource> local_removes;
        std::vector<int32_t> local_post_remove;
        {
            std::lock_guard<std::mutex> lock(pending_source_lifecycle_mutex_);
            local_adds.swap(pending_source_adds_);
            local_removes.swap(pending_source_removes_);
            local_post_remove.swap(pending_source_post_remove_cleanup_);
        }
        for (IPLSource src : local_removes) {
            if (src) {
                IPLSource tmp = src;
                iplSourceRelease(&tmp);
            }
        }
        (void)local_adds;
        (void)local_post_remove;
    }
    for (int i = 0; i < kMaxCacheHandles; i++)
        source_attach_pending_[static_cast<size_t>(i)].store(0, std::memory_order_release);
    {
        std::lock_guard<std::recursive_mutex> cb_lock(_attenuation_callback_mutex);
        _source_attenuation_entries.clear();
    }
    _source_update_snapshot_.clear();
    // Invalidate lock-free caches via epoch bump (avoid O(N) clears during teardown).
    reverb_param_cache_front_.store(0, std::memory_order_release);
    reflection_param_cache_front_.store(0, std::memory_order_release);
    pathing_param_cache_front_.store(0, std::memory_order_release);
    occlusion_cache_front_.store(0, std::memory_order_release);
    for (int slot = 0; slot < kCacheSlots; slot++) {
        resonance::bump_slot_epoch(reverb_param_cache_epoch_[slot]);
        resonance::bump_slot_epoch(reflection_param_cache_epoch_[slot]);
        resonance::bump_slot_epoch(pathing_param_cache_epoch_[slot]);
        resonance::bump_slot_epoch(occlusion_cache_epoch_[slot]);
    }
    _clear_reverb_params_likely_available_hints();

    // Drain AudioEffect / InternalPlayback IPL users under AudioServer::lock before destroying IPLSource handles.
    _drain_ipl_context_clients_before_context_destroy();

    // Snapshot probe batches under registry mutex only; Remove runs below under simulation_mutex
    // (never nest registry after holding sim - worker is already joined).
    std::vector<IPLProbeBatch> batches_to_release;
    probe_batch_registry_.get_all_batches_for_shutdown(batches_to_release);

    // Hold simulation_mutex for IPL teardown so audio-thread try_lock paths cannot interleave.
    {
        std::lock_guard<std::mutex> sim_lock(simulation_mutex);
        _drain_pathing_probe_batch_releases();
        for (IPLProbeBatch batch : batches_to_release) {
            if (simulator && batch) {
                iplSimulatorRemoveProbeBatch(simulator, batch);
            }
            if (batch)
                iplProbeBatchRelease(&batch);
        }
        if (simulator && !batches_to_release.empty())
            iplSimulatorCommit(simulator);

        // FMOD: destroy reverb source before simulator release (destroy_source_handle blocked while shutting down).
        if (fmod_reverb_source_handle_ >= 0) {
            _destroy_source_handle_under_simulation_lock(fmod_reverb_source_handle_);
            fmod_reverb_source_handle_ = -1;
        }

        // Release every IPLSource before the simulator; otherwise audio can try_lock and GetOutputs on stale handles.
        {
            std::vector<int32_t> source_handles;
            source_manager.get_all_handles(source_handles);
            for (int32_t h : source_handles) {
                _destroy_source_handle_under_simulation_lock(h);
            }
        }

        _set_reflection_mixer(nullptr);
        _flush_deferred_reflection_mixers_on_shutdown();
        if (simulator)
            iplSimulatorRelease(&simulator);
        simulator_created_with_pathing_ = false;
        _clear_static_packs_assume_locked();
        if (scene)
            iplSceneRelease(&scene);
    }
    if (steam_audio_context_) {
        steam_audio_context_->shutdown();
        steam_audio_context_.reset();
    }
}
String ResonanceServer::get_version() { return String("Nexus Resonance v") + resonance::kVersion; }
bool ResonanceServer::is_initialized() const { return (_ctx() != nullptr); }
bool ResonanceServer::is_simulating() const {
    if (!is_initialized())
        return false;
    if (_scene_type() == IPL_SCENETYPE_CUSTOM)
        return godot_physics_bridge_.has_valid_world();
    return global_triangle_count.load(std::memory_order_acquire) > 0;
}

bool ResonanceServer::is_spatial_audio_output_ready() const {
    if (!is_initialized())
        return true;
    return resonance::spatial_audio_geometry_gate_allows_output(
        spatial_audio_warmup_passes_remaining_.load(std::memory_order_acquire),
        global_triangle_count.load(std::memory_order_acquire),
        phonon_scene_audio_ready_.load(std::memory_order_acquire));
}

void ResonanceServer::reset_spatial_audio_warmup_passes() {
    if (!is_initialized())
        return;
    spatial_audio_warmup_passes_remaining_.store(resonance::kSpatialAudioWarmupWorkerPasses, std::memory_order_release);
}

void ResonanceServer::arm_spatial_audio_output_gate() {
    if (!is_initialized())
        return;
    // Clear ready and restart warmup; mark scene dirty so the next tick re-commits phonon_scene_audio_ready_.
    phonon_scene_audio_ready_.store(false, std::memory_order_release);
    reset_spatial_audio_warmup_passes();
    scene_dirty.store(true, std::memory_order_release);
    _wake_phonon_worker_for_lifecycle();
}

void ResonanceServer::_worker_decrement_spatial_warmup_if_pending() {
    int v = spatial_audio_warmup_passes_remaining_.load(std::memory_order_relaxed);
    if (v > 0)
        spatial_audio_warmup_passes_remaining_.store(v - 1, std::memory_order_release);
}

void ResonanceServer::_worker_note_spatial_warmup_progress(bool run_direct_executed, bool scene_graph_committed) {
    if (resonance::spatial_warmup_should_decrement(run_direct_executed, scene_graph_committed))
        _worker_decrement_spatial_warmup_if_pending();
    if (run_direct_executed)
        direct_after_inline_inputs_pending_.store(false, std::memory_order_release);
}

void ResonanceServer::_worker_note_direct_sim_pass_completed() {
    _worker_note_spatial_warmup_progress(true, false);
}
