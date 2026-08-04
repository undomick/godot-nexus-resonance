#include "resonance_constants.h"
#include "resonance_geometry.h"
#include "resonance_log.h"
#include "resonance_math.h"
#include "resonance_server.h"
#include "resonance_source_handle_policy.h"
#include "resonance_utils.h"
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
    // No auto-init here!
    reverb_param_cache_front_.store(0, std::memory_order_release);
    reflection_param_cache_front_.store(0, std::memory_order_release);
    pathing_param_cache_front_.store(0, std::memory_order_release);
    occlusion_cache_front_.store(0, std::memory_order_release);
    for (size_t i = 0; i < reflections_pending_.size(); i++)
        reflections_pending_[i].store(false, std::memory_order_release);
    for (size_t i = 0; i < _source_baked_reverb_listener_probe_override_.size(); i++)
        _source_baked_reverb_listener_probe_override_[i].store(-1, std::memory_order_release);
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
    max_reverb_duration = config_.max_reverb_duration;
    simulation_threads = config_.simulation_threads;
    simulation_cpu_cores_percent = config_.simulation_cpu_cores_percent;
    max_rays = config_.max_rays;
    max_bounces = config_.max_bounces;
    reverb_influence_radius = config_.reverb_influence_radius;
    reverb_transmission_amount = config_.reverb_transmission_amount;
    apply_occlusion_to_baked_reflections = config_.apply_occlusion_to_baked_reflections;
    baked_reverb_use_listener_probe = config_.baked_reverb_use_listener_probe;
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
    pathing_num_vis_samples = config_.pathing_num_vis_samples;
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
    reflection_sim_heavy_requested.store(false, std::memory_order_relaxed);
    pathing_sim_heavy_requested.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> b(source_update_batch_mutex_);
        source_update_batch_.clear();
    }
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

    // Fresh init after teardown/reinit: clear shutdown/teardown flags so a new context is usable in-process (e.g. editor play again).
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
    if (!_uses_main_thread_phonon_simulation())
        _start_worker_thread();

    String version_str = String::num_int64(STEAMAUDIO_VERSION_MAJOR) + "." + String::num_int64(STEAMAUDIO_VERSION_MINOR) + "." + String::num_int64(STEAMAUDIO_VERSION_PATCH);
    const char* refl_names[] = {"Convolution", "Parametric", "Hybrid", "TrueAudio Next"};
    int refl_idx = (reflection_type >= resonance::kReflectionConvolution && reflection_type <= resonance::kReflectionTan) ? reflection_type : resonance::kReflectionConvolution;
    String rays_str = (max_rays == 0) ? "Rays: Baked Only (0)" : "Rays (Realtime): " + String::num_int64(max_rays);
    String order_msg = " | Ambisonics: " + ambient_order_ordinal(ambisonic_order);
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
    ctx_config.max_reverb_duration = max_reverb_duration;
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
    if (_scene_type() == IPL_SCENETYPE_CUSTOM) {
        sceneSettings.closestHitCallback = &ResonanceGodotPhysicsSceneBridge::closest_hit_callback;
        sceneSettings.anyHitCallback = &ResonanceGodotPhysicsSceneBridge::any_hit_callback;
        const int batch = resonance::clamp_physics_ray_batch_size(physics_ray_batch_size);
        if (batch > 1) {
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

    simulation_settings.flags = static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS);
    // Pathing simulators are only allocated when PATHING is set at iplSimulatorCreate.
    if (pathing_enabled)
        simulation_settings.flags = static_cast<IPLSimulationFlags>(simulation_settings.flags | IPL_SIMULATIONFLAGS_PATHING);
    // scene_type: Default / Embree / Radeon Rays / Custom (Godot physics callbacks).
    simulation_settings.sceneType = _scene_type();
    simulation_settings.reflectionType =
        (reflection_type == resonance::kReflectionParametric) ? IPL_REFLECTIONEFFECTTYPE_PARAMETRIC : (reflection_type == resonance::kReflectionHybrid) ? IPL_REFLECTIONEFFECTTYPE_HYBRID
                                                                                                  : (reflection_type == resonance::kReflectionTan)      ? IPL_REFLECTIONEFFECTTYPE_TAN
                                                                                                                                                        : IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
    simulation_settings.openCLDevice = _opencl();
    simulation_settings.tanDevice = _tan();
    simulation_settings.maxNumOcclusionSamples = max_occlusion_samples;
    // maxNumRays==0 is valid (baked-only); no need to force a minimum for convolution-style modes.
    simulation_settings.maxNumRays = max_rays;
    simulation_settings.numDiffuseSamples = realtime_num_diffuse_samples;
    simulation_settings.maxDuration = max_reverb_duration;
    simulation_settings.samplingRate = current_sample_rate;
    simulation_settings.frameSize = frame_size;
    simulation_settings.maxOrder = ambisonic_order;
    simulation_settings.numThreads = simulation_threads;
    simulation_settings.maxNumSources = max_simulation_sources;
    simulation_settings.numVisSamples = pathing_enabled ? pathing_num_vis_samples : 1;
    {
        const int batch = (_scene_type() == IPL_SCENETYPE_CUSTOM) ? resonance::clamp_physics_ray_batch_size(physics_ray_batch_size) : 1;
        simulation_settings.rayBatchSize = batch;
    }

    if (iplSimulatorCreate(_ctx(), &simulation_settings, &simulator) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceServer: iplSimulatorCreate failed.");
        iplSceneRelease(&scene);
        steam_audio_context_.reset();
        return false;
    }
    simulator_created_with_pathing_ = pathing_enabled;

    // Lazy FMOD reverb IPLSource: ensure_fmod_reverb_source() when the bridge is used.

    if (reflection_type == resonance::kReflectionConvolution || reflection_type == resonance::kReflectionTan) {
        IPLReflectionEffectSettings rs{};
        rs.type = (reflection_type == resonance::kReflectionTan) ? IPL_REFLECTIONEFFECTTYPE_TAN : IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
        rs.numChannels = get_num_channels_for_order();
        rs.irSize = static_cast<IPLint32>(
            std::lroundf(resonance::sanitize_audio_float(max_reverb_duration) * static_cast<float>(current_sample_rate)));
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
        const int batch = (_scene_type() == IPL_SCENETYPE_CUSTOM) ? resonance::clamp_physics_ray_batch_size(physics_ray_batch_size) : 1;
        const bool batched_path = (_scene_type() == IPL_SCENETYPE_CUSTOM && batch > 1);
        const char* st_label = "DEFAULT";
        if (_scene_type() == IPL_SCENETYPE_EMBREE)
            st_label = "EMBREE";
        else if (_scene_type() == IPL_SCENETYPE_RADEONRAYS)
            st_label = "RADEONRAYS";
        else if (_scene_type() == IPL_SCENETYPE_CUSTOM)
            st_label = "CUSTOM";
        if (batched_path)
            ResonanceLog::info(String("Nexus Resonance: simulator ") + st_label + ", rayBatchSize=" + String::num(batch) +
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
    thread_running = true;
    worker_thread = std::thread(&ResonanceServer::_worker_thread_func, this);
}

void ResonanceServer::tick(float delta) {
    static std::atomic<bool> s_log_main_bound{false};
    if (!s_log_main_bound.exchange(true, std::memory_order_relaxed))
        resonance_log_bind_main_thread();
    resonance_log_drain_pending();

    std::vector<int32_t> tick_source_handles;
    source_manager.get_all_handles(tick_source_handles);
    const bool run_direct_this_wake = _tick_schedule_simulation(delta, tick_source_handles);

    if (_uses_main_thread_phonon_simulation()) {
        IPLCoordinateSpace3 listener_cs = _snapshot_listener_for_simulation();
        const bool run_refl = reflection_sim_heavy_requested.exchange(false, std::memory_order_acq_rel);
        const bool run_path = pathing_sim_heavy_requested.exchange(false, std::memory_order_acq_rel);
        {
            std::lock_guard<std::mutex> sim_lock(simulation_mutex);
            _run_phonon_simulation_locked(listener_cs, run_direct_this_wake, run_refl, run_path);
        }
        return;
    }

    {
        std::lock_guard<std::mutex> lock(worker_mutex);
        simulation_requested = true;
        worker_run_direct_next.store(run_direct_this_wake, std::memory_order_release);
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

            std::lock_guard<std::mutex> sim_lock(simulation_mutex);
            const bool run_direct = worker_run_direct_next.load(std::memory_order_acquire);
            const bool run_refl = reflection_sim_heavy_requested.exchange(false, std::memory_order_acq_rel);
            const bool run_path = pathing_sim_heavy_requested.exchange(false, std::memory_order_acq_rel);
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
    // Invalidate client-held source/probe-batch handles before recycling IDs (Auto frame-size reinit, etc.).
    source_lifecycle_epoch_.store(resonance::next_source_lifecycle_epoch(source_lifecycle_epoch_.load(std::memory_order_relaxed)),
                                  std::memory_order_release);
    _clear_physics_ray_excludes_state();
    godot_physics_bridge_.clear_world();
    if (!_ctx())
        return; // Idempotent; safe to call multiple times

    ipl_teardown_active_.store(true, std::memory_order_release);

    // Reset atomic flags first to prevent late accesses during/after shutdown
    listener_seq_.store(0, std::memory_order_release);
    pending_listener_valid.store(false);
    simulation_requested.store(false);
    reflection_sim_heavy_requested.store(false);
    pathing_sim_heavy_requested.store(false);
    scene_dirty.store(false);
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

    if (thread_running) {
        thread_running = false;
        worker_cv.notify_all();
        if (worker_thread.joinable())
            worker_thread.join();
    }
    // Worker stopped: release queued Remove retains; pending Adds without a worker attach are dropped with later release_all.
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
    // Clear caches via epoch bump (avoid O(N) clears during teardown).
    reverb_param_cache_front_.store(0, std::memory_order_release);
    reflection_param_cache_front_.store(0, std::memory_order_release);
    pathing_param_cache_front_.store(0, std::memory_order_release);
    occlusion_cache_front_.store(0, std::memory_order_release);
    for (int slot = 0; slot < kCacheSlots; slot++) {
        reverb_param_cache_epoch_[slot]++;
        reflection_param_cache_epoch_[slot]++;
        pathing_param_cache_epoch_[slot]++;
        occlusion_cache_epoch_[slot]++;
        if (reverb_param_cache_epoch_[slot] == 0u)
            reverb_param_cache_epoch_[slot] = 1u;
        if (reflection_param_cache_epoch_[slot] == 0u)
            reflection_param_cache_epoch_[slot] = 1u;
        if (pathing_param_cache_epoch_[slot] == 0u)
            pathing_param_cache_epoch_[slot] = 1u;
        if (occlusion_cache_epoch_[slot] == 0u)
            occlusion_cache_epoch_[slot] = 1u;
    }
    _clear_reverb_params_likely_available_hints();

    // Drain ResonanceAudioEffect / InternalPlayback IPL users under AudioServer::lock before destroying IPLSource handles.
    {
        AudioServer* audio = AudioServer::get_singleton();
        if (audio)
            audio->lock();
        _drain_ipl_context_clients_assume_audio_locked();
        if (audio)
            audio->unlock();
    }

    // Clean probe batches (remove from simulator before releasing). Snapshot under registry mutex only;
    // simulator Remove runs below under simulation_mutex (never nest registry after holding sim here:
    // worker is already joined).
    std::vector<IPLProbeBatch> batches_to_release;
    probe_batch_registry_.get_all_batches_for_shutdown(batches_to_release);

    // Hold simulation_mutex for all IPL simulator/scene/source teardown so audio-thread try_lock paths cannot interleave.
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

        // FMOD Bridge: destroy reverb source before simulator release (cannot use destroy_source_handle: is_shutting_down blocks it).
        if (fmod_reverb_source_handle_ >= 0) {
            _destroy_source_handle_under_simulation_lock(fmod_reverb_source_handle_);
            fmod_reverb_source_handle_ = -1;
        }

        // SourceManager retains each IPLSource; release the simulator only after every source is removed and released.
        // Otherwise the audio thread can win try_lock after the worker joined and call iplSourceGetOutputs on stale handles.
        {
            std::vector<int32_t> source_handles;
            source_manager.get_all_handles(source_handles);
            for (int32_t h : source_handles) {
                _destroy_source_handle_under_simulation_lock(h);
            }
        }

        _set_reflection_mixer(nullptr);
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

void ResonanceServer::_worker_note_direct_sim_pass_completed() {
    int v = spatial_audio_warmup_passes_remaining_.load(std::memory_order_relaxed);
    if (v > 0)
        spatial_audio_warmup_passes_remaining_.store(v - 1, std::memory_order_release);
}
