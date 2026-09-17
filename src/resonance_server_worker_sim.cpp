#include "resonance_constants.h"
#include "resonance_pathing_inputs_policy.h"
#include "resonance_phonon_worker_policy.h"
#include "resonance_server.h"
#include "resonance_simulator_commit_policy.h"
#include "resonance_source_handle_policy.h"
#include "resonance_spatial_warmup_policy.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>
#if defined(_WIN32) && defined(_MSC_VER)
#include <excpt.h>
#endif

using namespace godot;

// Worker/main-thread Phonon simulation: RunDirect/Reflections/Pathing, adaptive realtime rays.
// Heavy tick: SetSharedInputs -> Commit (if SetScene) -> RunDirect -> RunReflections -> sync fetch.
// Baked reverb still needs RunReflections when the listener moves (numRays may be 0).

namespace {

#if defined(_WIN32) && defined(_MSC_VER)
void run_pathing_seh(IPLSimulator sim, int* out_ok) {
    *out_ok = 0;
    __try {
        iplSimulatorRunPathing(sim);
        *out_ok = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out_ok = 0;
    }
}
#endif

} // namespace

bool ResonanceServer::_tick_schedule_simulation(float delta, const std::vector<int32_t>& handles) {
    const bool output_reverb = output_reverb_enabled.load(std::memory_order_acquire);
    if (reflection_force_heavy_next_tick_.exchange(false, std::memory_order_acq_rel)) {
        if (output_reverb && _any_source_has_reflection_outputs(handles))
            reflection_sim_heavy_requested.store(true, std::memory_order_release);
    }

    const bool need_refl_heavy = output_reverb && _any_source_has_reflection_outputs(handles);
    const bool adaptive_refl = (reflections_adaptive_budget_us_ > 0);
    const float eff_r = reflections_sim_interval + (adaptive_refl ? reflections_adaptive_extra_interval_ : 0.0f);
    reflections_interval_elapsed += delta;
    pathing_interval_elapsed += delta;
    if (need_refl_heavy && (eff_r <= 0.0f || reflections_interval_elapsed >= eff_r ||
                            !reflections_have_run_once_.load(std::memory_order_acquire))) {
        reflections_interval_elapsed = 0.0f;
        reflection_sim_heavy_requested.store(true, std::memory_order_release);
    }
    if (resonance::simulator_supports_pathing_run(pathing_enabled, simulator_created_with_pathing_) && output_reverb &&
        _any_source_has_pathing_outputs(handles) &&
        (pathing_sim_interval <= 0.0f || pathing_interval_elapsed >= pathing_sim_interval)) {
        pathing_interval_elapsed = 0.0f;
        pathing_sim_heavy_requested.store(true, std::memory_order_release);
    }

    if (reflections_adaptive_budget_us_ > 0) {
        const uint64_t last = instrumentation_worker_us_run_reflections.load(std::memory_order_relaxed);
        const uint64_t budget = static_cast<uint64_t>(reflections_adaptive_budget_us_);
        if (last > budget) {
            reflections_adaptive_extra_interval_ = std::min(
                reflections_adaptive_extra_interval_ + reflections_adaptive_step_sec_,
                reflections_adaptive_max_extra_interval_);
        } else {
            reflections_adaptive_extra_interval_ = std::max(
                0.0f,
                reflections_adaptive_extra_interval_ - reflections_adaptive_decay_per_sec_ * delta);
        }
    }

    direct_sim_time_elapsed += delta;

    const bool heavy_pending = reflection_sim_heavy_requested.load(std::memory_order_acquire) ||
                               pathing_sim_heavy_requested.load(std::memory_order_acquire);
    if (direct_sim_interval <= 0.0f)
        return true;
    if (heavy_pending) {
        direct_sim_time_elapsed = 0.0f;
        return true;
    }
    if (direct_sim_time_elapsed < direct_sim_interval)
        return false;
    direct_sim_time_elapsed = 0.0f;
    return true;
}

ResonanceServer::ReflectionSourceCounts ResonanceServer::_count_reflection_source_flags(const std::vector<int32_t>& handles) const {
    ReflectionSourceCounts counts;
    for (int32_t h : handles) {
        if (h < 0 || h >= kMaxCacheHandles)
            continue;
        if (source_outputs_reflections_[static_cast<size_t>(h)].load(std::memory_order_relaxed) != 0)
            counts.active_reflection_sources++;
        if (source_outputs_realtime_reflections_[static_cast<size_t>(h)].load(std::memory_order_relaxed) != 0)
            counts.active_realtime_sources++;
    }
    return counts;
}

bool ResonanceServer::_any_source_has_reflection_outputs(const std::vector<int32_t>& handles) const {
    return resonance::any_source_has_reflection_outputs(handles, [this](int32_t h) {
        return source_outputs_reflections_[static_cast<size_t>(h)].load(std::memory_order_relaxed);
    });
}

bool ResonanceServer::_any_source_has_realtime_reflection_outputs(const std::vector<int32_t>& handles) const {
    if (handles.empty())
        return false;
    for (int32_t h : handles) {
        if (h < 0 || h >= kMaxCacheHandles)
            continue;
        if (source_outputs_realtime_reflections_[static_cast<size_t>(h)].load(std::memory_order_relaxed) != 0)
            return true;
    }
    return false;
}

bool ResonanceServer::_any_source_has_pathing_outputs(const std::vector<int32_t>& handles) const {
    return resonance::any_source_has_pathing_outputs(handles, [this](int32_t h) {
        return source_outputs_pathing_[static_cast<size_t>(h)].load(std::memory_order_relaxed);
    });
}

bool ResonanceServer::_any_source_has_direct_outputs(const std::vector<int32_t>& handles) const {
    return resonance::any_source_has_output_flag(handles, [this](int32_t h) {
        return source_outputs_direct_[static_cast<size_t>(h)].load(std::memory_order_relaxed);
    });
}

bool ResonanceServer::_has_pending_source_lifecycle() {
    std::lock_guard<std::mutex> lock(pending_source_lifecycle_mutex_);
    return !pending_source_adds_.empty() || !pending_source_removes_.empty();
}

bool ResonanceServer::_has_pending_source_updates() {
    std::lock_guard<std::mutex> lock(source_update_batch_mutex_);
    return !source_update_batch_.empty();
}

bool ResonanceServer::_has_pending_dynamic_instanced_transforms() {
    std::lock_guard<std::mutex> lock(dynamic_instanced_transform_queue_mutex_);
    return !dynamic_instanced_transform_queue_.empty();
}

bool ResonanceServer::_phonon_worker_tick_has_work(bool run_direct_scheduled, bool run_reflection_heavy, bool run_pathing_heavy,
                                                   const std::vector<int32_t>& handles) {
    resonance::PhononWorkerTickInputs in{};
    in.pending_lifecycle = _has_pending_source_lifecycle();
    in.pending_source_updates = _has_pending_source_updates();
    in.scene_dirty = scene_dirty.load(std::memory_order_acquire);
    in.pending_dynamic_transforms = _has_pending_dynamic_instanced_transforms();
    in.run_direct_scheduled = run_direct_scheduled;
    in.run_reflection_heavy = run_reflection_heavy;
    in.run_pathing_heavy = run_pathing_heavy;
    in.output_reverb_enabled = output_reverb_enabled.load(std::memory_order_acquire);
    in.any_direct_outputs = _any_source_has_direct_outputs(handles);
    in.any_reflection_outputs = _any_source_has_reflection_outputs(handles);
    in.any_pathing_outputs = _any_source_has_pathing_outputs(handles);
    return resonance::phonon_worker_tick_has_work(in);
}

void ResonanceServer::_compute_adaptive_eff_num_rays(bool any_realtime_reflections, bool run_reflection_sim, int& eff_num_rays,
                                                     int& adaptive_target) {
    eff_num_rays = 0;
    adaptive_target = 0;
    if (!any_realtime_reflections || !run_reflection_sim)
        return;

    eff_num_rays = max_rays;
    adaptive_target = max_rays;
    if (reflections_adaptive_budget_us_ <= 0)
        return;

    const int min_rays_cfg = std::max(1, std::min(reflections_adaptive_ray_min_, max_rays));
    if (!_adaptive_realtime_num_rays_initialized_) {
        _adaptive_realtime_num_rays_ = max_rays;
        _adaptive_realtime_num_rays_initialized_ = true;
    } else {
        const uint64_t last_us = instrumentation_worker_us_run_reflections.load(std::memory_order_relaxed);
        const uint64_t budget = static_cast<uint64_t>(reflections_adaptive_budget_us_);
        if (budget > 0 && last_us > budget) {
            const double ratio = static_cast<double>(budget) / static_cast<double>(last_us);
            int next = static_cast<int>(static_cast<double>(_adaptive_realtime_num_rays_) * ratio);
            next = std::max(min_rays_cfg, next);
            _adaptive_realtime_num_rays_ = std::min(max_rays, next);
        } else {
            const float frac = std::max(0.0f, std::min(1.0f, reflections_adaptive_ray_recover_frac_));
            int step = static_cast<int>(std::ceil(static_cast<float>(max_rays) * frac));
            const int cap = reflections_adaptive_ray_recover_cap_;
            if (cap > 0)
                step = std::min(step, cap);
            step = std::max(1, step);
            _adaptive_realtime_num_rays_ = std::min(max_rays, _adaptive_realtime_num_rays_ + step);
        }
    }
    eff_num_rays = std::max(min_rays_cfg, std::min(max_rays, _adaptive_realtime_num_rays_));
    adaptive_target = _adaptive_realtime_num_rays_;
}

uint64_t ResonanceServer::_commit_simulator_scene_graph_if_dirty_assume_locked(bool* committed_out) {
    if (committed_out)
        *committed_out = false;
    if (!scene_dirty.load(std::memory_order_acquire))
        return 0;
    const auto ts0 = std::chrono::steady_clock::now();
    iplSceneCommit(scene);
    iplSimulatorSetScene(simulator, scene);
    scene_dirty.store(false, std::memory_order_release);
    phonon_scene_audio_ready_.store(true, std::memory_order_release);
    if (committed_out)
        *committed_out = true;
    const auto ts1 = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(ts1 - ts0).count());
}

uint64_t ResonanceServer::_ipl_simulator_commit_assume_locked() {
    const auto tc0 = std::chrono::steady_clock::now();
    iplSimulatorCommit(simulator);
    const auto tc1 = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(tc1 - tc0).count());
}

uint64_t ResonanceServer::_run_pathing_sim_assume_locked(bool run_pathing_sim) {
    if (!run_pathing_sim || !resonance::simulator_supports_pathing_run(pathing_enabled, simulator_created_with_pathing_))
        return 0;

    int cooldown = pathing_crash_cooldown.load();
    if (cooldown > 0)
        pathing_crash_cooldown.store(cooldown - 1);

    if (!pending_listener_valid.load(std::memory_order_acquire)) {
        instrumentation_pathing_sim_skip_listener.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    if (pathing_crash_cooldown.load(std::memory_order_acquire) > 0) {
        instrumentation_pathing_sim_skip_cooldown.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    if (debug_pathing.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> pv_lock(pathing_vis_mutex);
        pathing_vis_segments.clear();
    }
    instrumentation_pathing_sim_attempt.fetch_add(1, std::memory_order_relaxed);

    uint64_t us_path = 0;
#if defined(_WIN32) && defined(_MSC_VER)
    const auto tp0 = std::chrono::steady_clock::now();
    int pathing_ok = 0;
    run_pathing_seh(simulator, &pathing_ok);
    const auto tp1 = std::chrono::steady_clock::now();
    us_path = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(tp1 - tp0).count());
    if (pathing_ok) {
        pathing_ran_this_tick.store(true);
        instrumentation_pathing_sim_ran.fetch_add(1, std::memory_order_relaxed);
    } else {
        pathing_crash_cooldown.store(resonance::kPathingCrashCooldownTicks);
        instrumentation_pathing_sim_seh_fail.fetch_add(1, std::memory_order_relaxed);
    }
#else
    const auto tp0 = std::chrono::steady_clock::now();
    iplSimulatorRunPathing(simulator);
    const auto tp1 = std::chrono::steady_clock::now();
    us_path = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(tp1 - tp0).count());
    pathing_ran_this_tick.store(true);
    instrumentation_pathing_sim_ran.fetch_add(1, std::memory_order_relaxed);
#endif
    _drain_pathing_probe_batch_releases();
    return us_path;
}

void ResonanceServer::_run_phonon_simulation_locked(const IPLCoordinateSpace3& current_listener, bool run_direct, bool run_reflection_sim,
                                                    bool run_pathing_sim) {
    uint64_t us_lifecycle_commit = 0;
    const bool input_drain_had_work =
        _drain_pending_source_lifecycle_assume_locked(&us_lifecycle_commit) || _flush_pending_source_updates_assume_locked();
    const auto td0 = std::chrono::steady_clock::now();
    _apply_queued_dynamic_instanced_mesh_transforms_assume_locked();
    const auto td1 = std::chrono::steady_clock::now();
    instrumentation_worker_us_dynamic_instanced_apply.store(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(td1 - td0).count()),
        std::memory_order_relaxed);

    std::vector<int32_t> sim_handles;
    source_manager.get_all_handles(sim_handles);

    const bool output_reverb = output_reverb_enabled.load(std::memory_order_acquire);
    const bool any_reflection_outputs = _any_source_has_reflection_outputs(sim_handles);
    const bool any_pathing_outputs = _any_source_has_pathing_outputs(sim_handles);
    const bool shared_reflections = resonance::phonon_worker_shared_reflections_enabled(output_reverb, any_reflection_outputs);
    const bool shared_pathing =
        resonance::phonon_worker_shared_pathing_enabled(pathing_enabled, output_reverb, any_pathing_outputs);
    const bool any_realtime_reflections = _any_source_has_realtime_reflection_outputs(sim_handles);
    const ReflectionSourceCounts reflection_counts = _count_reflection_source_flags(sim_handles);
    instrumentation_worker_active_reflection_sources_.store(reflection_counts.active_reflection_sources,
                                                            std::memory_order_relaxed);
    instrumentation_worker_active_realtime_reflection_sources_.store(reflection_counts.active_realtime_sources,
                                                                     std::memory_order_relaxed);

    const bool any_direct_outputs = _any_source_has_direct_outputs(sim_handles);
    if (resonance::phonon_worker_should_clear_stale_direct_after_inline_inputs(
            direct_after_inline_inputs_pending_.load(std::memory_order_acquire), any_direct_outputs)) {
        direct_after_inline_inputs_pending_.store(false, std::memory_order_release);
    }
    run_direct = resonance::phonon_worker_run_direct_after_input_drain(input_drain_had_work, run_direct, any_direct_outputs);

    const bool simulation_active =
        thread_running.load(std::memory_order_acquire) || _uses_main_thread_phonon_simulation();
    const bool allow_new_heavy = resonance::phonon_worker_allow_new_heavy_simulation(
        is_shutting_down_flag.load(std::memory_order_acquire), simulation_active,
        cold_start_settle_pending_.load(std::memory_order_acquire));
    const bool run_reflections =
        allow_new_heavy &&
        resonance::phonon_worker_run_reflection_heavy(run_reflection_sim, output_reverb, any_reflection_outputs);
    const bool run_pathing =
        allow_new_heavy &&
        resonance::phonon_worker_run_pathing_heavy(run_pathing_sim, pathing_enabled, output_reverb, any_pathing_outputs);

    if (!_phonon_worker_tick_has_work(run_direct, run_reflection_sim, run_pathing_sim, sim_handles)) {
        _drain_pathing_probe_batch_releases();
        instrumentation_worker_us_scene_graph_commit.store(0, std::memory_order_relaxed);
        instrumentation_worker_us_simulator_commit.store(us_lifecycle_commit, std::memory_order_relaxed);
        instrumentation_worker_us_run_direct.store(0, std::memory_order_relaxed);
        instrumentation_worker_us_run_reflections.store(0, std::memory_order_relaxed);
        instrumentation_worker_us_run_pathing.store(0, std::memory_order_relaxed);
        instrumentation_worker_us_sync_fetch.store(0, std::memory_order_relaxed);
        instrumentation_worker_last_wake_was_heavy.store(false, std::memory_order_relaxed);
        return;
    }

    int eff_num_rays = 0;
    int adaptive_target = 0;
    _compute_adaptive_eff_num_rays(any_realtime_reflections, run_reflections, eff_num_rays, adaptive_target);

    IPLSimulationSharedInputs inputs{};
    inputs.listener = current_listener;
    inputs.numRays = eff_num_rays;
    instrumentation_worker_last_num_rays_.store(static_cast<int32_t>(inputs.numRays), std::memory_order_relaxed);
    instrumentation_worker_last_adaptive_num_rays_target_.store(static_cast<int32_t>(adaptive_target), std::memory_order_relaxed);
    inputs.numBounces = max_bounces;
    inputs.duration = realtime_simulation_duration;
    inputs.order = ambisonic_order;
    inputs.irradianceMinDistance = realtime_irradiance_min_distance;
    inputs.pathingVisCallback = (pathing_enabled && debug_pathing.load(std::memory_order_acquire)) ? _pathing_vis_callback : nullptr;
    inputs.pathingUserData = (pathing_enabled && debug_pathing.load(std::memory_order_acquire)) ? static_cast<void*>(this) : nullptr;
    IPLSimulationFlags sim_flags = static_cast<IPLSimulationFlags>(0);
    if (any_direct_outputs)
        sim_flags = static_cast<IPLSimulationFlags>(sim_flags | IPL_SIMULATIONFLAGS_DIRECT);
    if (shared_reflections)
        sim_flags = static_cast<IPLSimulationFlags>(sim_flags | IPL_SIMULATIONFLAGS_REFLECTIONS);
    if (shared_pathing)
        sim_flags = static_cast<IPLSimulationFlags>(sim_flags | IPL_SIMULATIONFLAGS_PATHING);
    iplSimulatorSetSharedInputs(simulator, sim_flags, &inputs);

    bool scene_graph_committed = false;
    const uint64_t us_scene_graph = _commit_simulator_scene_graph_if_dirty_assume_locked(&scene_graph_committed);
    uint64_t us_commit = 0;
    if (scene_graph_committed)
        us_commit = _ipl_simulator_commit_assume_locked();
    instrumentation_worker_us_scene_graph_commit.store(us_scene_graph, std::memory_order_relaxed);
    instrumentation_worker_us_simulator_commit.store(us_lifecycle_commit + us_commit, std::memory_order_relaxed);

    bool execute_run_reflections = run_reflections;
    bool reflections_deferred_this_tick = false;
    const bool scene_commit_over_threshold =
        reflections_defer_after_scene_commit_us_ > 0 &&
        us_scene_graph >= static_cast<uint64_t>(reflections_defer_after_scene_commit_us_);
    if (resonance::phonon_worker_should_defer_reflections_after_heavy_scene_commit(
            execute_run_reflections, scene_commit_over_threshold,
            reflections_have_run_once_.load(std::memory_order_acquire))) {
        execute_run_reflections = false;
        reflections_deferred_this_tick = true;
        reflection_force_heavy_next_tick_.store(true, std::memory_order_release);
    }

    uint64_t us_direct = 0;
    uint64_t us_refl = 0;
    const bool execute_run_direct = resonance::phonon_worker_run_direct_after_scene_graph_commit(
        scene_graph_committed, run_direct, any_direct_outputs);
    if (execute_run_direct) {
        const auto t0 = std::chrono::steady_clock::now();
        iplSimulatorRunDirect(simulator);
        const auto t1 = std::chrono::steady_clock::now();
        us_direct = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    }
    instrumentation_worker_us_run_direct.store(us_direct, std::memory_order_relaxed);
    _worker_note_spatial_warmup_progress(execute_run_direct, scene_graph_committed);

    pathing_ran_this_tick.store(false);
    const bool any_heavy = run_reflections || run_pathing;
    bool ran_reflections_this_pass = false;
    if (any_heavy) {
        if (execute_run_reflections) {
            const auto t0 = std::chrono::steady_clock::now();
            iplSimulatorRunReflections(simulator);
            const auto t1 = std::chrono::steady_clock::now();
            us_refl = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
            reflections_have_run_once_.store(true);
            ran_reflections_this_pass = true;
        }
        const bool run_pathing_this_tick = run_pathing && !reflections_deferred_this_tick;
        const uint64_t us_path = _run_pathing_sim_assume_locked(run_pathing_this_tick);
        instrumentation_worker_us_run_pathing.store(us_path, std::memory_order_relaxed);
        if (run_pathing_this_tick && !pathing_ran_this_tick.load(std::memory_order_acquire))
            _invalidate_pathing_cache_for_active_sources_assume_locked();
    }
    // Source updates retain pathing probe batches until after RunPathing. Drain on every tick so
    // early pathing skips (no listener, cooldown, light-only ticks) cannot accumulate retains.
    _drain_pathing_probe_batch_releases();
    instrumentation_worker_us_run_reflections.store(us_refl, std::memory_order_relaxed);

    // Clear pending only after RunReflections, before sync fetch (fetch blocks while pending).
    _worker_clear_reflections_pending_after_run_assume_locked(ran_reflections_this_pass);

    const auto t0 = std::chrono::steady_clock::now();
    _worker_sync_fetch_caches(execute_run_direct, ran_reflections_this_pass);
    const auto t1 = std::chrono::steady_clock::now();
    instrumentation_worker_us_sync_fetch.store(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()),
        std::memory_order_relaxed);
    instrumentation_worker_last_wake_was_heavy.store(any_heavy, std::memory_order_relaxed);
    _drain_deferred_reflection_mixers_if_pending();
}
