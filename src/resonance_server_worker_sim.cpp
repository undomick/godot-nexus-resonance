#include "resonance_constants.h"
#include "resonance_pathing_inputs_policy.h"
#include "resonance_phonon_worker_policy.h"
#include "resonance_reflections_adaptive_budget_policy.h"
#include "resonance_server.h"
#include "resonance_simulator_commit_policy.h"
#include "resonance_source_handle_policy.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>
#if defined(_WIN32) && defined(_MSC_VER)
#include <excpt.h>
#endif

using namespace godot;

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

    if (reflections_adaptive_budget_us_ > 0)
        _update_adaptive_extra_interval(delta);

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

resonance::ReflectionPassInputs ResonanceServer::_reflection_pass_inputs(resonance::SimulationAccess access, const IPLSimulationSharedInputs& shared) const {
    resonance::ReflectionPassInputs p;
    p.source_inputs_generation = source_table_.inputs_generation(access);
    p.scene_generation = scene_generation_;
    p.probe_batch_generation = probe_batch_registry_.simulator_generation();
    p.listener = shared.listener;
    p.num_rays = shared.numRays;
    p.num_bounces = shared.numBounces;
    p.duration = shared.duration;
    p.order = shared.order;
    p.irradiance_min_distance = shared.irradianceMinDistance;
    return p;
}

resonance::PathingPassInputs ResonanceServer::_pathing_pass_inputs(resonance::SimulationAccess access, const IPLSimulationSharedInputs& shared) const {
    resonance::PathingPassInputs p;
    p.source_inputs_generation = source_table_.inputs_generation(access);
    p.scene_generation = scene_generation_;
    p.probe_batch_generation = probe_batch_registry_.simulator_generation();
    p.deviation_generation = pathing_deviation_generation_;
    p.listener = shared.listener;
    p.debug_vis = shared.pathingVisCallback != nullptr;
    return p;
}

ResonanceServer::ReflectionSourceCounts ResonanceServer::_count_reflection_source_flags(const std::vector<int32_t>& handles) const {
    ReflectionSourceCounts counts;
    for (int32_t h : handles) {
        if (h < 0 || h >= kMaxCacheHandles)
            continue;
        if (source_table_.takes_part_in(resonance::SourcePass::kReflections, h))
            counts.active_reflection_sources++;
        if (source_table_.takes_part_in(resonance::SourcePass::kRealtimeReflections, h))
            counts.active_realtime_sources++;
    }
    return counts;
}

bool ResonanceServer::_any_source_has_reflection_outputs(const std::vector<int32_t>& handles) const {
    return resonance::any_source_has_reflection_outputs(handles, [this](int32_t h) {
        return source_table_.takes_part_in(resonance::SourcePass::kReflections, h);
    });
}

bool ResonanceServer::_any_source_has_realtime_reflection_outputs(const std::vector<int32_t>& handles) const {
    if (handles.empty())
        return false;
    for (int32_t h : handles) {
        if (h < 0 || h >= kMaxCacheHandles)
            continue;
        if (source_table_.takes_part_in(resonance::SourcePass::kRealtimeReflections, h))
            return true;
    }
    return false;
}

bool ResonanceServer::_any_source_has_pathing_outputs(const std::vector<int32_t>& handles) const {
    return resonance::any_source_has_pathing_outputs(handles, [this](int32_t h) {
        return source_table_.takes_part_in(resonance::SourcePass::kPathing, h);
    });
}

bool ResonanceServer::_any_source_has_direct_outputs(const std::vector<int32_t>& handles) const {
    return resonance::any_source_has_output_flag(handles, [this](int32_t h) {
        return source_table_.takes_part_in(resonance::SourcePass::kDirect, h);
    });
}

bool ResonanceServer::_has_pending_source_lifecycle() {
    return source_table_.has_pending_lifecycle();
}

bool ResonanceServer::_has_pending_source_updates() {
    return source_table_.has_queued_updates();
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
    in.pending_static_packs = _has_pending_static_pack_changes();
    in.pending_probe_batches = probe_batch_registry_.has_pending_simulator_changes();
    in.pending_scene_meshes = _has_pending_scene_mesh_changes();
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

    const resonance::AdaptiveRayLimits limits = _adaptive_ray_limits();
    const int min_rays = resonance::adaptive_min_num_rays(limits);

    const uint32_t pass = reflection_pass_count_.load(std::memory_order_acquire);
    if (!_adaptive_realtime_num_rays_initialized_) {
        _adaptive_realtime_num_rays_ = max_rays;
        _adaptive_realtime_num_rays_initialized_ = true;
        _adaptive_rays_seen_reflection_pass_ = pass;
    } else if (pass != _adaptive_rays_seen_reflection_pass_) {
        _adaptive_rays_seen_reflection_pass_ = pass;
        const uint64_t last_us = last_reflection_pass_us_.load(std::memory_order_relaxed);
        _adaptive_realtime_num_rays_ = resonance::adaptive_next_num_rays(
            _adaptive_realtime_num_rays_, last_us, static_cast<uint64_t>(reflections_adaptive_budget_us_), limits);
    }
    eff_num_rays = std::max(min_rays, std::min(max_rays, _adaptive_realtime_num_rays_));
    adaptive_target = _adaptive_realtime_num_rays_;
}

resonance::AdaptiveRayLimits ResonanceServer::_adaptive_ray_limits() const {
    resonance::AdaptiveRayLimits limits;
    limits.max_rays = max_rays;
    limits.min_rays = reflections_adaptive_ray_min_;
    limits.recover_frac = reflections_adaptive_ray_recover_frac_;
    limits.recover_cap = reflections_adaptive_ray_recover_cap_;
    return limits;
}

void ResonanceServer::_note_reflection_pass(uint64_t pass_us, int num_rays, bool realtime_rays) {
    const bool at_floor = !realtime_rays || num_rays <= resonance::adaptive_min_num_rays(_adaptive_ray_limits());
    reflections_adaptive_rays_at_floor_.store(at_floor, std::memory_order_relaxed);
    last_reflection_pass_us_.store(pass_us, std::memory_order_relaxed);
    reflection_pass_count_.fetch_add(1, std::memory_order_release);
}

void ResonanceServer::_note_realtime_reflection_sources(const std::vector<int32_t>& handles) {
    // No reflections pass is requested without realtime sources; the next one must not measure across the gap.
    if (_count_reflection_source_flags(handles).active_realtime_sources == 0)
        reflections_age_tracker_.reset();
}

void ResonanceServer::_note_reflections_age(bool realtime_reflections_active) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto now_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
    const std::optional<uint64_t> age = reflections_age_tracker_.note_pass(now_us, realtime_reflections_active);
    if (!age)
        return;
    instrumentation_reflections_age_us_last_.store(*age, std::memory_order_relaxed);
    resonance::store_max(instrumentation_reflections_age_us_max_, *age);
    instrumentation_reflections_age_samples_.fetch_add(1, std::memory_order_release);
}

void ResonanceServer::_update_adaptive_extra_interval(float delta) {
    const uint32_t pass = reflection_pass_count_.load(std::memory_order_acquire);
    const bool is_new = pass != reflections_adaptive_interval_seen_pass_;
    reflections_adaptive_interval_seen_pass_ = pass;
    const bool over_budget =
        last_reflection_pass_us_.load(std::memory_order_relaxed) > static_cast<uint64_t>(reflections_adaptive_budget_us_);

    resonance::AdaptiveIntervalLimits limits;
    limits.step_sec = reflections_adaptive_step_sec_;
    limits.max_extra_sec = reflections_adaptive_max_extra_interval_;
    limits.decay_per_sec = reflections_adaptive_decay_per_sec_;
    reflections_adaptive_extra_interval_ = resonance::adaptive_next_extra_interval(
        reflections_adaptive_extra_interval_, over_budget, is_new,
        reflections_adaptive_rays_at_floor_.load(std::memory_order_relaxed), delta, limits);
}

uint64_t ResonanceServer::_commit_simulator_scene_graph_if_dirty_assume_locked(bool* committed_out) {
    if (committed_out)
        *committed_out = false;
    if (!scene_dirty.load(std::memory_order_acquire))
        return 0;
    const auto ts0 = std::chrono::steady_clock::now();
    iplSceneCommit(scene);
    iplSimulatorSetScene(simulator, scene);
    scene_generation_++;
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

void ResonanceServer::_note_pathing_pass_ran(const resonance::PathingPassInputs& pass_inputs) {
    pathing_ran_this_tick.store(true);
    pathing_results_current_.store(true, std::memory_order_release);
    pathing_last_pass_inputs_ = pass_inputs;
    pathing_have_last_pass_ = true;
    instrumentation_pathing_sim_ran.fetch_add(1, std::memory_order_relaxed);
}

uint64_t ResonanceServer::_run_pathing_sim_assume_locked(resonance::SimulationAccess access, bool run_pathing_sim, const IPLSimulationSharedInputs& shared) {
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

    const resonance::PathingPassInputs pass_inputs = _pathing_pass_inputs(access, shared);
    if (resonance::pathing_pass_can_skip(pathing_have_last_pass_, pathing_last_pass_inputs_, pass_inputs,
                                         _scene_type() == IPL_SCENETYPE_CUSTOM)) {
        pathing_results_current_.store(true, std::memory_order_release);
        instrumentation_worker_pathing_passes_skipped_.fetch_add(1, std::memory_order_relaxed);
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
        _note_pathing_pass_ran(pass_inputs);
    } else {
        pathing_crash_cooldown.store(resonance::kPathingCrashCooldownTicks);
        instrumentation_pathing_sim_seh_fail.fetch_add(1, std::memory_order_relaxed);
    }
#else
    const auto tp0 = std::chrono::steady_clock::now();
    iplSimulatorRunPathing(simulator);
    const auto tp1 = std::chrono::steady_clock::now();
    us_path = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(tp1 - tp0).count());
    _note_pathing_pass_ran(pass_inputs);
#endif
    _drain_pathing_probe_batch_releases();
    return us_path;
}

void ResonanceServer::_run_phonon_simulation_locked(resonance::SimulationAccess access, const IPLCoordinateSpace3& current_listener, bool run_direct, bool run_reflection_sim,
                                                    bool run_pathing_sim) {
    const ReflectionsPassRequest request =
        _run_direct_pass(access, current_listener, run_direct, run_reflection_sim, run_pathing_sim);
    if (request.any())
        _run_reflections_pass_locked(access, request, nullptr);
}

ResonanceServer::ReflectionsPassRequest ResonanceServer::_run_direct_pass(resonance::SimulationAccess access, const IPLCoordinateSpace3& current_listener, bool run_direct,
                                                                          bool run_reflection_sim, bool run_pathing_sim) {
    const bool full = !access.direct_only();
    // Read before the scene graph commit: a reload armed after this point must not count this pass's results.
    const uint32_t readiness_generation = source_table_.readiness_generation();
    // A handed-off reflections pass still owns the probe batch retains and the heavy timings until it runs.
    const bool reflections_pass_pending = reflections_worker_busy_.load(std::memory_order_acquire);
    uint64_t us_lifecycle_commit = 0;
    bool input_drain_had_work = false;
    if (full) {
        _apply_pending_probe_batch_changes_assume_locked(access);
        input_drain_had_work = _drain_pending_source_lifecycle_assume_locked(access, &us_lifecycle_commit);
    }
    input_drain_had_work = _flush_pending_source_updates_assume_locked(access) || input_drain_had_work;
    if (full) {
        apply_queued_scene_mesh_changes_assume_locked();
        const auto td0 = std::chrono::steady_clock::now();
        _apply_queued_dynamic_instanced_mesh_transforms_assume_locked();
        const auto td1 = std::chrono::steady_clock::now();
        instrumentation_worker_us_dynamic_instanced_apply.store(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(td1 - td0).count()),
            std::memory_order_relaxed);
        _apply_pending_static_pack_changes_assume_locked();
    }

    std::vector<int32_t> sim_handles;
    source_manager.get_all_handles(sim_handles);

    const bool output_reverb = output_reverb_enabled.load(std::memory_order_acquire);
    const bool any_reflection_outputs = _any_source_has_reflection_outputs(sim_handles);
    const bool any_pathing_outputs = _any_source_has_pathing_outputs(sim_handles);
    const bool any_direct_outputs = _any_source_has_direct_outputs(sim_handles);
    if (full)
        _note_realtime_reflection_sources(sim_handles);
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
    bool run_reflections =
        allow_new_heavy &&
        resonance::phonon_worker_run_reflection_heavy(run_reflection_sim, output_reverb, any_reflection_outputs);
    const bool run_pathing =
        allow_new_heavy &&
        resonance::phonon_worker_run_pathing_heavy(run_pathing_sim, pathing_enabled, output_reverb, any_pathing_outputs);

    if (!_phonon_worker_tick_has_work(run_direct, run_reflection_sim, run_pathing_sim, sim_handles)) {
        if (full) {
            instrumentation_worker_us_scene_graph_commit.store(0, std::memory_order_relaxed);
            instrumentation_worker_us_simulator_commit.store(us_lifecycle_commit, std::memory_order_relaxed);
        }
        if (full && !reflections_pass_pending) {
            _drain_pathing_probe_batch_releases();
            instrumentation_worker_us_run_reflections.store(0, std::memory_order_relaxed);
            instrumentation_worker_us_run_pathing.store(0, std::memory_order_relaxed);
            instrumentation_worker_last_wake_was_heavy.store(false, std::memory_order_relaxed);
        }
        instrumentation_worker_us_run_direct.store(0, std::memory_order_relaxed);
        instrumentation_worker_us_sync_fetch.store(0, std::memory_order_relaxed);
        return {};
    }

    if (any_direct_outputs) {
        IPLSimulationSharedInputs inputs{};
        inputs.listener = current_listener;
        iplSimulatorSetSharedInputs(simulator, IPL_SIMULATIONFLAGS_DIRECT, &inputs);
    }

    bool scene_graph_committed = false;
    uint64_t us_scene_graph = 0;
    if (full) {
        us_scene_graph = _commit_simulator_scene_graph_if_dirty_assume_locked(&scene_graph_committed);
        uint64_t us_commit = 0;
        if (scene_graph_committed)
            us_commit = _ipl_simulator_commit_assume_locked();
        instrumentation_worker_us_scene_graph_commit.store(us_scene_graph, std::memory_order_relaxed);
        instrumentation_worker_us_simulator_commit.store(us_lifecycle_commit + us_commit, std::memory_order_relaxed);
    }

    bool pathing_deferred = false;
    const bool scene_commit_over_threshold =
        reflections_defer_after_scene_commit_us_ > 0 &&
        us_scene_graph >= static_cast<uint64_t>(reflections_defer_after_scene_commit_us_);
    if (resonance::phonon_worker_should_defer_reflections_after_heavy_scene_commit(
            run_reflections, scene_commit_over_threshold, reflections_have_run_once_.load(std::memory_order_acquire))) {
        run_reflections = false;
        pathing_deferred = true;
        reflection_force_heavy_next_tick_.store(true, std::memory_order_release);
    }

    uint64_t us_direct = 0;
    const bool execute_run_direct =
        resonance::phonon_worker_run_direct_after_scene_graph_commit(scene_graph_committed, run_direct, any_direct_outputs);
    if (execute_run_direct) {
        const auto t0 = std::chrono::steady_clock::now();
        iplSimulatorRunDirect(simulator);
        const auto t1 = std::chrono::steady_clock::now();
        us_direct = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        if (!full)
            instrumentation_direct_passes_during_reflections_.fetch_add(1, std::memory_order_relaxed);
    }
    instrumentation_worker_us_run_direct.store(us_direct, std::memory_order_relaxed);
    if (execute_run_direct)
        _worker_note_run_direct_executed();

    const auto t0 = std::chrono::steady_clock::now();
    _worker_sync_fetch_caches(access, execute_run_direct, false, false);
    if (execute_run_direct)
        _worker_mark_direct_results_published(sim_handles, readiness_generation);
    const auto t1 = std::chrono::steady_clock::now();
    instrumentation_worker_us_sync_fetch.store(
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()),
        std::memory_order_relaxed);
    _drain_deferred_reflection_mixers_if_pending();

    ReflectionsPassRequest request;
    if (!full)
        return request;
    request.listener = current_listener;
    request.run_reflections = run_reflections;
    request.run_pathing = run_pathing && !pathing_deferred;
    if (reflections_pass_pending)
        return request;
    instrumentation_worker_last_wake_was_heavy.store(request.any(), std::memory_order_relaxed);
    // Source updates retain probe batches until a reflections pass drains them; with no pass following,
    // drain here so the retains cannot pile up.
    if (!request.any())
        _drain_pathing_probe_batch_releases();
    return request;
}

void ResonanceServer::_run_reflections_pass_locked(resonance::SimulationAccess access, const ReflectionsPassRequest& request, resonance::ReflectionsPassGate* gate) {
    if (!resonance::phonon_worker_allow_new_heavy_simulation(is_shutting_down_flag.load(std::memory_order_acquire), true,
                                                             cold_start_settle_pending_.load(std::memory_order_acquire))) {
        _drain_pathing_probe_batch_releases();
        return;
    }

    std::vector<int32_t> sim_handles;
    source_manager.get_all_handles(sim_handles);

    const bool output_reverb = output_reverb_enabled.load(std::memory_order_acquire);
    const bool shared_reflections =
        resonance::phonon_worker_shared_reflections_enabled(output_reverb, _any_source_has_reflection_outputs(sim_handles));
    const bool shared_pathing = resonance::phonon_worker_shared_pathing_enabled(pathing_enabled, output_reverb,
                                                                                _any_source_has_pathing_outputs(sim_handles));
    const bool any_realtime_reflections = _any_source_has_realtime_reflection_outputs(sim_handles);

    int eff_num_rays = 0;
    int adaptive_target = 0;
    _compute_adaptive_eff_num_rays(any_realtime_reflections, request.run_reflections, eff_num_rays, adaptive_target);

    IPLSimulationSharedInputs inputs{};
    inputs.listener = request.listener;
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
    if (shared_reflections)
        sim_flags = static_cast<IPLSimulationFlags>(sim_flags | IPL_SIMULATIONFLAGS_REFLECTIONS);
    if (shared_pathing)
        sim_flags = static_cast<IPLSimulationFlags>(sim_flags | IPL_SIMULATIONFLAGS_PATHING);
    if (sim_flags != 0)
        iplSimulatorSetSharedInputs(simulator, sim_flags, &inputs);

    bool execute_run_reflections = request.run_reflections;
    const resonance::ReflectionPassInputs reflection_pass_inputs = _reflection_pass_inputs(access, inputs);
    if (execute_run_reflections &&
        resonance::reflection_pass_can_skip(reflection_have_last_pass_, reflection_last_pass_inputs_,
                                            reflection_pass_inputs, _scene_type() == IPL_SCENETYPE_CUSTOM)) {
        execute_run_reflections = false;
        instrumentation_worker_reflection_passes_skipped_.fetch_add(1, std::memory_order_relaxed);
    }

    pathing_ran_this_tick.store(false);
    pathing_results_current_.store(false, std::memory_order_release);
    if (gate)
        gate->begin_run();
    uint64_t us_refl = 0;
    if (execute_run_reflections) {
        const auto t0 = std::chrono::steady_clock::now();
        iplSimulatorRunReflections(simulator);
        const auto t1 = std::chrono::steady_clock::now();
        us_refl = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    }
    const uint64_t us_path = _run_pathing_sim_assume_locked(access, request.run_pathing, inputs);
    if (gate)
        gate->end_run();

    if (request.run_reflections)
        _note_reflections_age(any_realtime_reflections);
    // Readers sample us_run_reflections when the pass count changes, so publish the duration first.
    instrumentation_worker_us_run_reflections.store(us_refl, std::memory_order_relaxed);
    if (execute_run_reflections) {
        reflections_have_run_once_.store(true);
        reflection_last_pass_inputs_ = reflection_pass_inputs;
        reflection_have_last_pass_ = true;
        _note_reflection_pass(us_refl, eff_num_rays, any_realtime_reflections);
    }
    instrumentation_worker_us_run_pathing.store(us_path, std::memory_order_relaxed);
    if (request.run_pathing && !pathing_results_current_.load(std::memory_order_acquire)) {
        _invalidate_pathing_cache_for_active_sources_assume_locked();
        pathing_have_last_pass_ = false;
    }
    _drain_pathing_probe_batch_releases();

    // After RunReflections but before the fetch, which blocks while reflections are pending.
    _worker_clear_reflections_pending_after_run_assume_locked(execute_run_reflections);

    const bool pathing_refresh = pathing_enabled && pathing_ran_this_tick.load(std::memory_order_acquire);
    _worker_sync_fetch_caches(access, false, execute_run_reflections, pathing_refresh);
    _drain_deferred_reflection_mixers_if_pending();
}
