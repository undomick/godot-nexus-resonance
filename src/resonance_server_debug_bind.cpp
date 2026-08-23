#include "resonance_constants.h"
#include "resonance_pathing_inputs_policy.h"
#include "resonance_server.h"
#include "resonance_utils.h"
#include <algorithm>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector3.hpp>

using namespace godot;

// --- SETTERS ---
void ResonanceServer::set_debug_occlusion(bool p_enabled) { debug_occlusion.store(p_enabled, std::memory_order_release); }
bool ResonanceServer::is_debug_occlusion_enabled() const { return debug_occlusion.load(std::memory_order_acquire); }
void ResonanceServer::set_debug_reflections(bool p_enabled) { debug_reflections.store(p_enabled, std::memory_order_release); }
bool ResonanceServer::is_debug_reflections_enabled() const { return debug_reflections.load(std::memory_order_acquire); }
void ResonanceServer::set_debug_pathing(bool p_enabled) { debug_pathing.store(p_enabled, std::memory_order_release); }
bool ResonanceServer::is_debug_pathing_enabled() const { return debug_pathing.load(std::memory_order_acquire); }

void ResonanceServer::set_pathing_enabled(bool p_enabled) {
    if (resonance::pathing_enable_requires_simulator_recreate(p_enabled, simulator_created_with_pathing_)) {
        UtilityFunctions::push_warning(
            "Nexus Resonance: pathing_enabled cannot turn on until the simulator is recreated with "
            "IPL_SIMULATIONFLAGS_PATHING (toggle ResonanceRuntimeConfig.pathing_enabled so the runtime reinits).");
        return;
    }
    pathing_enabled = p_enabled;
}

Array ResonanceServer::get_pathing_visualization_segments() {
    Array result;
    std::lock_guard<std::mutex> lock(pathing_vis_mutex);
    const int n = static_cast<int>(pathing_vis_segments.size());
    const int cap = resonance::kPathingVisMaxSegmentsReturned;
    const int start = (n > cap) ? (n - cap) : 0;
    for (int i = start; i < n; i++) {
        const PathVisSegment& seg = pathing_vis_segments[static_cast<size_t>(i)];
        Dictionary d;
        d["from"] = seg.from;
        d["to"] = seg.to;
        d["occluded"] = seg.occluded;
        result.push_back(d);
    }
    return result;
}

Array ResonanceServer::get_ray_debug_segments() {
    Array result;
    if (!wants_debug_reflection_viz())
        return result;
    IPLCoordinateSpace3 listener = get_current_listener_coords();
    IPLVector3 origin = listener.origin;
    const int viz_rays = (max_rays > resonance::kRayDebugVizMaxRaysPerCall) ? resonance::kRayDebugVizMaxRaysPerCall : max_rays;
    ray_trace_debug_context_.trace_reflection_rays_for_viz(origin, viz_rays, resonance::kRayDebugMaxDistance, result);
    return result;
}

Array ResonanceServer::get_ray_debug_segments_at(Vector3 origin) {
    Array result;
    if (!wants_debug_reflection_viz())
        return result;
    IPLVector3 o;
    o.x = origin.x;
    o.y = origin.y;
    o.z = origin.z;
    const int viz_rays = (max_rays > resonance::kRayDebugVizMaxRaysPerCall) ? resonance::kRayDebugVizMaxRaysPerCall : max_rays;
    ray_trace_debug_context_.trace_reflection_rays_for_viz(o, viz_rays, resonance::kRayDebugMaxDistance, result);
    return result;
}

bool ResonanceServer::uses_custom_ray_tracer() const {
    return _scene_type() == IPL_SCENETYPE_CUSTOM;
}

int ResonanceServer::register_debug_mesh(const std::vector<IPLVector3>& vertices,
                                         const std::vector<IPLTriangle>& triangles,
                                         const IPLint32* material_indices, const IPLMatrix4x4* transform, const IPLMaterial* material) {
    return ray_trace_debug_context_.register_mesh(vertices, triangles, material_indices, transform, material);
}

void ResonanceServer::reset_reverb_bus_instrumentation() {
    reverb_effect_process_calls.store(0, std::memory_order_relaxed);
    reverb_effect_mixer_null.store(0, std::memory_order_relaxed);
    reverb_effect_success.store(0, std::memory_order_relaxed);
    reverb_effect_frames_written.store(0, std::memory_order_relaxed);
    reverb_effect_output_peak.store(0.0f, std::memory_order_relaxed);
    reverb_effect_click_guard_triggers.store(0, std::memory_order_relaxed);
    reverb_mixer_return_hold_last_count.store(0, std::memory_order_relaxed);
    reverb_mixer_feed_count.store(0, std::memory_order_relaxed);
    reverb_convolution_valid_fetches.store(0, std::memory_order_relaxed);
    reverb_convolution_feed_ir_null.store(0, std::memory_order_relaxed);
    reverb_convolution_gain_min.store(1.0f, std::memory_order_relaxed);
    reverb_convolution_gain_max.store(0.0f, std::memory_order_relaxed);
    reverb_convolution_input_rms_max.store(0.0f, std::memory_order_relaxed);
    instrumentation_fetch_cache_hit.store(0, std::memory_order_relaxed);
    instrumentation_fetch_cache_miss.store(0, std::memory_order_relaxed);
    instrumentation_fetch_refl_stale_epoch_fallback.store(0, std::memory_order_relaxed);
    instrumentation_fetch_cache_skip.store(0, std::memory_order_relaxed);
    instrumentation_audio_conv_refl_apply_last_us_.store(0, std::memory_order_relaxed);
    instrumentation_audio_conv_reverb_bus_last_us_.store(0, std::memory_order_relaxed);
    instrumentation_audio_mixer_sanitize_ambi_last_us_.store(0, std::memory_order_relaxed);
    instrumentation_audio_mixer_sanitize_stereo_last_us_.store(0, std::memory_order_relaxed);
    reset_pathing_instrumentation();
}

void ResonanceServer::reset_pathing_instrumentation() {
    instrumentation_pathing_fetch_early_exit.store(0, std::memory_order_relaxed);
    instrumentation_pathing_fetch_src_null.store(0, std::memory_order_relaxed);
    instrumentation_pathing_fetch_sh_ok.store(0, std::memory_order_relaxed);
    instrumentation_pathing_fetch_sh_null.store(0, std::memory_order_relaxed);
    instrumentation_pathing_fetch_sh_bad_order.store(0, std::memory_order_relaxed);
    instrumentation_pathing_fetch_cache_hit.store(0, std::memory_order_relaxed);
    instrumentation_pathing_fetch_cache_miss.store(0, std::memory_order_relaxed);
    instrumentation_pathing_sim_attempt.store(0, std::memory_order_relaxed);
    instrumentation_pathing_sim_ran.store(0, std::memory_order_relaxed);
    instrumentation_pathing_sim_seh_fail.store(0, std::memory_order_relaxed);
    instrumentation_pathing_sim_skip_listener.store(0, std::memory_order_relaxed);
    instrumentation_pathing_sim_skip_cooldown.store(0, std::memory_order_relaxed);
    instrumentation_pathing_player_gate.store(0, std::memory_order_relaxed);
    instrumentation_pathing_player_applied.store(0, std::memory_order_relaxed);
    instrumentation_pathing_player_fetch_miss.store(0, std::memory_order_relaxed);
    instrumentation_worker_us_run_direct.store(0, std::memory_order_relaxed);
    instrumentation_worker_us_run_reflections.store(0, std::memory_order_relaxed);
    instrumentation_worker_us_run_pathing.store(0, std::memory_order_relaxed);
    instrumentation_worker_us_sync_fetch.store(0, std::memory_order_relaxed);
    instrumentation_worker_us_simulator_commit.store(0, std::memory_order_relaxed);
    instrumentation_worker_us_scene_graph_commit.store(0, std::memory_order_relaxed);
    instrumentation_worker_us_dynamic_instanced_apply.store(0, std::memory_order_relaxed);
    instrumentation_worker_last_wake_was_heavy.store(false, std::memory_order_relaxed);
    instrumentation_main_us_dynamic_transform_enqueue_.store(0, std::memory_order_relaxed);
    instrumentation_main_us_last_dynamic_transform_enqueue_.store(0, std::memory_order_relaxed);
    instrumentation_dynamic_transform_enqueue_events_.store(0, std::memory_order_relaxed);
    instrumentation_worker_us_sync_fetch_occlusion.store(0, std::memory_order_relaxed);
    instrumentation_worker_us_sync_fetch_reflections.store(0, std::memory_order_relaxed);
    instrumentation_worker_us_sync_fetch_pathing.store(0, std::memory_order_relaxed);
    instrumentation_worker_last_num_rays_.store(0, std::memory_order_relaxed);
    instrumentation_worker_last_adaptive_num_rays_target_.store(0, std::memory_order_relaxed);
    instrumentation_worker_active_reflection_sources_.store(0, std::memory_order_relaxed);
    instrumentation_worker_active_realtime_reflection_sources_.store(0, std::memory_order_relaxed);
}

Dictionary ResonanceServer::get_simulation_worker_timing() const {
    Dictionary d;
    d["us_run_direct"] = (int64_t)instrumentation_worker_us_run_direct.load(std::memory_order_relaxed);
    d["us_run_reflections"] = (int64_t)instrumentation_worker_us_run_reflections.load(std::memory_order_relaxed);
    d["us_run_pathing"] = (int64_t)instrumentation_worker_us_run_pathing.load(std::memory_order_relaxed);
    d["us_sync_fetch"] = (int64_t)instrumentation_worker_us_sync_fetch.load(std::memory_order_relaxed);
    d["us_simulator_commit"] = (int64_t)instrumentation_worker_us_simulator_commit.load(std::memory_order_relaxed);
    d["us_scene_graph_commit"] = (int64_t)instrumentation_worker_us_scene_graph_commit.load(std::memory_order_relaxed);
    d["us_dynamic_instanced_apply"] = (int64_t)instrumentation_worker_us_dynamic_instanced_apply.load(std::memory_order_relaxed);
    d["us_sync_fetch_occlusion"] = (int64_t)instrumentation_worker_us_sync_fetch_occlusion.load(std::memory_order_relaxed);
    d["us_sync_fetch_reflections"] = (int64_t)instrumentation_worker_us_sync_fetch_reflections.load(std::memory_order_relaxed);
    d["us_sync_fetch_pathing"] = (int64_t)instrumentation_worker_us_sync_fetch_pathing.load(std::memory_order_relaxed);
    d["worker_last_wake_heavy"] = instrumentation_worker_last_wake_was_heavy.load(std::memory_order_relaxed);
    d["shared_num_rays"] = (int64_t)instrumentation_worker_last_num_rays_.load(std::memory_order_relaxed);
    d["shared_adaptive_num_rays_target"] = (int64_t)instrumentation_worker_last_adaptive_num_rays_target_.load(std::memory_order_relaxed);
    d["active_reflection_sources"] = (int64_t)instrumentation_worker_active_reflection_sources_.load(std::memory_order_relaxed);
    d["active_realtime_reflection_sources"] = (int64_t)instrumentation_worker_active_realtime_reflection_sources_.load(std::memory_order_relaxed);
    d["reflections_adaptive_extra_interval"] = reflections_adaptive_extra_interval_;
    d["main_us_dynamic_transform_enqueue"] = (int64_t)instrumentation_main_us_dynamic_transform_enqueue_.load(std::memory_order_relaxed);
    d["main_last_dynamic_transform_enqueue_us"] = (int64_t)instrumentation_main_us_last_dynamic_transform_enqueue_.load(std::memory_order_relaxed);
    d["dynamic_transform_enqueue_events"] = (int64_t)instrumentation_dynamic_transform_enqueue_events_.load(std::memory_order_relaxed);
    d["active_source_count"] = (int64_t)source_manager.size_approx();
    d["active_probe_batch_count"] = (int64_t)probe_batch_registry_.get_manager().size_approx();
    return d;
}

int32_t ResonanceServer::get_active_source_count() const { return source_manager.size_approx(); }

int32_t ResonanceServer::get_active_probe_batch_count() const { return probe_batch_registry_.get_manager().size_approx(); }

Dictionary ResonanceServer::get_convolution_audio_timing() const {
    Dictionary d;
    d["us_reflection_apply_last"] = (int64_t)instrumentation_audio_conv_refl_apply_last_us_.load(std::memory_order_relaxed);
    d["us_reverb_bus_last"] = (int64_t)instrumentation_audio_conv_reverb_bus_last_us_.load(std::memory_order_relaxed);
    d["us_mixer_sanitize_ambi_last"] = (int64_t)instrumentation_audio_mixer_sanitize_ambi_last_us_.load(std::memory_order_relaxed);
    d["us_mixer_sanitize_stereo_last"] = (int64_t)instrumentation_audio_mixer_sanitize_stereo_last_us_.load(std::memory_order_relaxed);
    return d;
}

void ResonanceServer::record_convolution_reflection_apply_usec(uint64_t us) {
    instrumentation_audio_conv_refl_apply_last_us_.store(us, std::memory_order_relaxed);
}

void ResonanceServer::record_convolution_reverb_bus_usec(uint64_t us) {
    instrumentation_audio_conv_reverb_bus_last_us_.store(us, std::memory_order_relaxed);
}

void ResonanceServer::record_mixer_sanitize_ambi_usec(uint64_t us) {
    instrumentation_audio_mixer_sanitize_ambi_last_us_.store(us, std::memory_order_relaxed);
}

void ResonanceServer::record_mixer_sanitize_stereo_usec(uint64_t us) {
    instrumentation_audio_mixer_sanitize_stereo_last_us_.store(us, std::memory_order_relaxed);
}

Dictionary ResonanceServer::get_simulation_tracer_profile() const {
    Dictionary d;
    const IPLSceneType st = _scene_type();
    d["scene_type"] = scene_type;
    d["ipl_scene_type"] = static_cast<int>(st);
    const int eff = (st == IPL_SCENETYPE_CUSTOM) ? resonance::clamp_physics_ray_batch_size(physics_ray_batch_size) : 1;
    d["ray_batch_size_effective"] = eff;
    const bool batched_custom = (st == IPL_SCENETYPE_CUSTOM && eff > 1);
    d["custom_batched_callbacks_registered"] = batched_custom;
    d["phonon_batched_reflection_sim_path"] = batched_custom;
    return d;
}

Dictionary ResonanceServer::get_pathing_instrumentation() const {
    Dictionary d;
    d["fetch_early_exit"] = (int64_t)instrumentation_pathing_fetch_early_exit.load(std::memory_order_relaxed);
    d["fetch_src_null"] = (int64_t)instrumentation_pathing_fetch_src_null.load(std::memory_order_relaxed);
    d["fetch_sh_ok"] = (int64_t)instrumentation_pathing_fetch_sh_ok.load(std::memory_order_relaxed);
    d["fetch_sh_null"] = (int64_t)instrumentation_pathing_fetch_sh_null.load(std::memory_order_relaxed);
    d["fetch_sh_bad_order"] = (int64_t)instrumentation_pathing_fetch_sh_bad_order.load(std::memory_order_relaxed);
    d["fetch_cache_hit"] = (int64_t)instrumentation_pathing_fetch_cache_hit.load(std::memory_order_relaxed);
    d["fetch_cache_miss"] = (int64_t)instrumentation_pathing_fetch_cache_miss.load(std::memory_order_relaxed);
    d["sim_attempt"] = (int64_t)instrumentation_pathing_sim_attempt.load(std::memory_order_relaxed);
    d["sim_ran"] = (int64_t)instrumentation_pathing_sim_ran.load(std::memory_order_relaxed);
    d["sim_seh_fail"] = (int64_t)instrumentation_pathing_sim_seh_fail.load(std::memory_order_relaxed);
    d["sim_skip_listener"] = (int64_t)instrumentation_pathing_sim_skip_listener.load(std::memory_order_relaxed);
    d["sim_skip_cooldown"] = (int64_t)instrumentation_pathing_sim_skip_cooldown.load(std::memory_order_relaxed);
    d["player_gate"] = (int64_t)instrumentation_pathing_player_gate.load(std::memory_order_relaxed);
    d["player_applied"] = (int64_t)instrumentation_pathing_player_applied.load(std::memory_order_relaxed);
    d["player_fetch_miss"] = (int64_t)instrumentation_pathing_player_fetch_miss.load(std::memory_order_relaxed);
    d["pathing_enabled"] = pathing_enabled;
    d["pending_listener_valid"] = pending_listener_valid.load(std::memory_order_relaxed);
    d["pathing_crash_cooldown"] = pathing_crash_cooldown.load(std::memory_order_relaxed);
    d["path_validation_enabled"] = path_validation_enabled;
    d["find_alternate_paths"] = find_alternate_paths;
    d["pathing_ran_this_tick"] = pathing_ran_this_tick.load(std::memory_order_relaxed);
    return d;
}

void ResonanceServer::record_pathing_player_gate_enter() {
    instrumentation_pathing_player_gate.fetch_add(1, std::memory_order_relaxed);
}

void ResonanceServer::record_pathing_player_applied() {
    instrumentation_pathing_player_applied.fetch_add(1, std::memory_order_relaxed);
}

void ResonanceServer::record_pathing_player_fetch_miss() {
    instrumentation_pathing_player_fetch_miss.fetch_add(1, std::memory_order_relaxed);
}

Dictionary ResonanceServer::get_reverb_bus_instrumentation() const {
    Dictionary d;
    d["effect_process_calls"] = (int64_t)reverb_effect_process_calls.load(std::memory_order_relaxed);
    d["effect_mixer_null"] = (int64_t)reverb_effect_mixer_null.load(std::memory_order_relaxed);
    d["effect_success"] = (int64_t)reverb_effect_success.load(std::memory_order_relaxed);
    d["effect_frames_written"] = (int64_t)reverb_effect_frames_written.load(std::memory_order_relaxed);
    d["effect_output_peak"] = reverb_effect_output_peak.load(std::memory_order_relaxed);
    d["effect_output_rms"] = reverb_effect_output_rms.load(std::memory_order_relaxed);
    d["effect_output_peak_pre_gain"] = reverb_effect_output_peak_pre_gain.load(std::memory_order_relaxed);
    d["effect_output_rms_pre_gain"] = reverb_effect_output_rms_pre_gain.load(std::memory_order_relaxed);
    d["effect_click_guard_triggers"] = (int64_t)reverb_effect_click_guard_triggers.load(std::memory_order_relaxed);
    d["mixer_return_hold_last_count"] = (int64_t)reverb_mixer_return_hold_last_count.load(std::memory_order_relaxed);
    d["mixer_feed_count"] = (int64_t)reverb_mixer_feed_count.load(std::memory_order_relaxed);
    d["mixer_exists"] = (reflection_mixer_.load(std::memory_order_acquire) != nullptr);
    d["reflection_type"] = reflection_type;
    d["convolution_valid_fetches"] = (int64_t)reverb_convolution_valid_fetches.load(std::memory_order_relaxed);
    d["convolution_feed_ir_null"] = (int64_t)reverb_convolution_feed_ir_null.load(std::memory_order_relaxed);
    d["convolution_gain_min"] = reverb_convolution_gain_min.load(std::memory_order_relaxed);
    d["convolution_gain_max"] = reverb_convolution_gain_max.load(std::memory_order_relaxed);
    d["convolution_input_rms_max"] = reverb_convolution_input_rms_max.load(std::memory_order_relaxed);
    d["fetch_cache_hit"] = (int64_t)instrumentation_fetch_cache_hit.load(std::memory_order_relaxed);
    d["fetch_cache_miss"] = (int64_t)instrumentation_fetch_cache_miss.load(std::memory_order_relaxed);
    d["fetch_refl_stale_epoch_fallback"] = (int64_t)instrumentation_fetch_refl_stale_epoch_fallback.load(std::memory_order_relaxed);
    d["fetch_cache_skip"] = (int64_t)instrumentation_fetch_cache_skip.load(std::memory_order_relaxed);
    return d;
}

void ResonanceServer::record_convolution_feed(bool ir_non_null, float reverb_gain, float input_rms) {
    if (!ir_non_null) {
        reverb_convolution_feed_ir_null.fetch_add(1, std::memory_order_relaxed);
    }
    float gmin = reverb_convolution_gain_min.load(std::memory_order_relaxed);
    if (reverb_gain < gmin)
        reverb_convolution_gain_min.store(reverb_gain, std::memory_order_relaxed);
    float gmax = reverb_convolution_gain_max.load(std::memory_order_relaxed);
    if (reverb_gain > gmax)
        reverb_convolution_gain_max.store(reverb_gain, std::memory_order_relaxed);
    float rmax = reverb_convolution_input_rms_max.load(std::memory_order_relaxed);
    if (input_rms > rmax)
        reverb_convolution_input_rms_max.store(input_rms, std::memory_order_relaxed);
}

void ResonanceServer::update_reverb_effect_instrumentation(bool mixer_null, bool success, int32_t frames_written,
                                                           float output_peak_post_gain, float output_rms_post_gain,
                                                           float output_peak_pre_gain, float output_rms_pre_gain) {
    reverb_effect_process_calls.fetch_add(1, std::memory_order_relaxed);
    if (mixer_null)
        reverb_effect_mixer_null.fetch_add(1, std::memory_order_relaxed);
    if (success)
        reverb_effect_success.fetch_add(1, std::memory_order_relaxed);
    reverb_effect_frames_written.store(frames_written, std::memory_order_relaxed);

    reverb_effect_output_peak.store(output_peak_post_gain, std::memory_order_relaxed);
    reverb_effect_output_rms.store(output_rms_post_gain, std::memory_order_relaxed);
    reverb_effect_output_peak_pre_gain.store(output_peak_pre_gain, std::memory_order_relaxed);
    reverb_effect_output_rms_pre_gain.store(output_rms_pre_gain, std::memory_order_relaxed);
}

void ResonanceServer::record_reverb_effect_click_guard_trigger() {
    reverb_effect_click_guard_triggers.fetch_add(1, std::memory_order_relaxed);
}

void ResonanceServer::record_mixer_return_hold_last() {
    reverb_mixer_return_hold_last_count.fetch_add(1, std::memory_order_relaxed);
}

void ResonanceServer::set_reverb_bus_click_guard_enabled(bool p_enabled) {
    reverb_bus_click_guard_enabled_.store(p_enabled, std::memory_order_release);
}

bool ResonanceServer::is_reverb_bus_click_guard_enabled() const {
    return reverb_bus_click_guard_enabled_.load(std::memory_order_acquire);
}

void ResonanceServer::set_reverb_bus_wet_ring_underrun_zero_fill(bool p_enabled) {
    reverb_bus_wet_ring_underrun_zero_fill_.store(p_enabled, std::memory_order_release);
}

bool ResonanceServer::is_reverb_bus_wet_ring_underrun_zero_fill() const {
    return reverb_bus_wet_ring_underrun_zero_fill_.load(std::memory_order_acquire);
}

void ResonanceServer::unregister_debug_mesh(int mesh_id) {
    ray_trace_debug_context_.unregister_mesh(mesh_id);
}

void ResonanceServer::set_output_direct_enabled(bool p_enabled) { output_direct_enabled.store(p_enabled, std::memory_order_release); }
bool ResonanceServer::is_output_direct_enabled() const { return output_direct_enabled.load(std::memory_order_acquire); }
void ResonanceServer::set_output_reverb_enabled(bool p_enabled) { output_reverb_enabled.store(p_enabled, std::memory_order_release); }
bool ResonanceServer::is_output_reverb_enabled() const { return output_reverb_enabled.load(std::memory_order_acquire); }
void ResonanceServer::set_reverb_influence_radius(float p_radius) { reverb_influence_radius = std::max(1.0f, p_radius); }
float ResonanceServer::get_reverb_influence_radius() const { return reverb_influence_radius; }
void ResonanceServer::set_reverb_transmission_amount(float p_amount) { reverb_transmission_amount = std::max(0.0f, std::min(1.0f, p_amount)); }
float ResonanceServer::get_reverb_transmission_amount() const { return reverb_transmission_amount; }
void ResonanceServer::set_apply_occlusion_to_baked_reflections(bool p_enabled) { apply_occlusion_to_baked_reflections = p_enabled; }
bool ResonanceServer::get_apply_occlusion_to_baked_reflections() const { return apply_occlusion_to_baked_reflections; }
void ResonanceServer::set_baked_reverb_use_listener_probe(bool p_enabled) { baked_reverb_use_listener_probe = p_enabled; }
bool ResonanceServer::get_baked_reverb_use_listener_probe() const { return baked_reverb_use_listener_probe; }
void ResonanceServer::set_perspective_correction_enabled(bool p_enabled) { perspective_correction_enabled.store(p_enabled, std::memory_order_release); }
bool ResonanceServer::is_perspective_correction_enabled() const { return perspective_correction_enabled.load(std::memory_order_acquire); }
void ResonanceServer::set_perspective_correction_factor(float p_factor) {
    perspective_correction_factor.store(std::max(0.1f, std::min(3.0f, p_factor)), std::memory_order_release);
}
float ResonanceServer::get_perspective_correction_factor() const { return perspective_correction_factor.load(std::memory_order_acquire); }

bool ResonanceServer::use_direct_binaural() const {
    return direct_binaural && _hrtf() != nullptr;
}

bool ResonanceServer::use_reverb_binaural() const {
    return reverb_binaural && _hrtf() != nullptr;
}

bool ResonanceServer::use_pathing_binaural() const {
    return pathing_binaural && _hrtf() != nullptr;
}

void ResonanceServer::_bind_methods() {
    ADD_SIGNAL(MethodInfo("bake_progress", PropertyInfo(Variant::FLOAT, "progress")));
    ClassDB::bind_method(D_METHOD("init_audio_engine", "config"), &ResonanceServer::init_audio_engine);
    ClassDB::bind_method(D_METHOD("reinit_audio_engine", "config"), &ResonanceServer::reinit_audio_engine);
    ClassDB::bind_method(D_METHOD("shutdown"), &ResonanceServer::shutdown);

    ClassDB::bind_method(D_METHOD("get_version"), &ResonanceServer::get_version);
    ClassDB::bind_method(D_METHOD("is_initialized"), &ResonanceServer::is_initialized);
    ClassDB::bind_method(D_METHOD("is_simulating"), &ResonanceServer::is_simulating);
    ClassDB::bind_method(D_METHOD("is_spatial_audio_output_ready"), &ResonanceServer::is_spatial_audio_output_ready);
    ClassDB::bind_method(D_METHOD("reset_spatial_audio_warmup_passes"), &ResonanceServer::reset_spatial_audio_warmup_passes);
    ClassDB::bind_method(D_METHOD("arm_spatial_audio_output_gate"), &ResonanceServer::arm_spatial_audio_output_gate);
    ClassDB::bind_method(D_METHOD("get_sample_rate"), &ResonanceServer::get_sample_rate);
    ClassDB::bind_method(D_METHOD("get_audio_frame_size"), &ResonanceServer::get_audio_frame_size);
    ClassDB::bind_method(D_METHOD("get_direct_speaker_channels"), &ResonanceServer::get_direct_speaker_channels);
    ClassDB::bind_method(D_METHOD("get_audio_frame_size_was_auto"), &ResonanceServer::get_audio_frame_size_was_auto);
    ClassDB::bind_method(D_METHOD("consume_pending_reinit_frame_size"), &ResonanceServer::consume_pending_reinit_frame_size);
    ClassDB::bind_method(D_METHOD("update_listener", "pos", "dir", "up"), &ResonanceServer::update_listener);
    ClassDB::bind_method(D_METHOD("set_listener_valid", "valid"), &ResonanceServer::set_listener_valid);
    ClassDB::bind_method(D_METHOD("notify_listener_changed"), &ResonanceServer::notify_listener_changed);
    ClassDB::bind_method(D_METHOD("notify_listener_changed_to", "listener_node"), &ResonanceServer::notify_listener_changed_to);
    ClassDB::bind_method(D_METHOD("tick", "delta"), &ResonanceServer::tick);
    ClassDB::bind_method(D_METHOD("set_physics_world", "world"), &ResonanceServer::set_physics_world);
    ClassDB::bind_method(D_METHOD("set_physics_ray_exclude_rids", "exclude"), &ResonanceServer::set_physics_ray_exclude_rids);
    ClassDB::bind_method(D_METHOD("set_listener_physics_ray_exclude_rids", "rids"), &ResonanceServer::set_listener_physics_ray_exclude_rids);
    ClassDB::bind_method(D_METHOD("flush_pending_source_updates"), &ResonanceServer::flush_pending_source_updates);
    ClassDB::bind_method(D_METHOD("create_source_handle", "position", "radius"), &ResonanceServer::create_source_handle);
    ClassDB::bind_method(D_METHOD("destroy_source_handle", "handle"), &ResonanceServer::destroy_source_handle);
    ClassDB::bind_method(D_METHOD("update_source", "handle", "position", "radius", "use_sim_distance_attenuation", "min_distance"),
                         &ResonanceServer::update_source_position, DEFVAL(false), DEFVAL(1.0f));
    ClassDB::bind_method(D_METHOD("get_source_occlusion_data", "handle"), &ResonanceServer::get_source_occlusion_data_dict);
    ClassDB::bind_method(D_METHOD("get_source_occlusion_linear_gain", "handle"), &ResonanceServer::get_source_occlusion_linear_gain);
    // TODO: enqueue_source_update batching parity with ResonancePlayer defer path.

    // Probes
    ClassDB::bind_method(D_METHOD("generate_manual_grid", "tr", "ext", "sp", "generation_type", "height_above_floor"),
                         &ResonanceServer::generate_manual_grid, DEFVAL(2), DEFVAL(1.5));
    ClassDB::bind_method(D_METHOD("generate_probes_scene_aware", "volume_transform", "extents", "spacing", "generation_type", "height_above_floor"),
                         &ResonanceServer::generate_probes_scene_aware, DEFVAL(1), DEFVAL(1.5));
    ClassDB::bind_method(D_METHOD("bake_manual_grid", "pts", "dat"), &ResonanceServer::bake_manual_grid);
    ClassDB::bind_method(D_METHOD("set_bake_params", "params"), &ResonanceServer::set_bake_params);
    ClassDB::bind_method(D_METHOD("set_bake_static_scene_asset", "p_asset"), &ResonanceServer::set_bake_static_scene_asset);
    ClassDB::bind_method(D_METHOD("set_bake_static_scenes_from_assets", "assets", "transforms"),
                         &ResonanceServer::set_bake_static_scenes_from_assets);
    ClassDB::bind_method(D_METHOD("load_static_scene_from_asset", "p_asset", "p_transform"), &ResonanceServer::load_static_scene_from_asset, DEFVAL(Transform3D()));
    ClassDB::bind_method(D_METHOD("add_static_scene_from_asset", "p_asset", "p_transform"), &ResonanceServer::add_static_scene_from_asset, DEFVAL(Transform3D()));
    ClassDB::bind_method(D_METHOD("add_or_replace_static_pack", "object_id", "p_asset", "p_transform"),
                         &ResonanceServer::add_or_replace_static_pack);
    ClassDB::bind_method(D_METHOD("remove_static_pack", "object_id"), &ResonanceServer::remove_static_pack);
    ClassDB::bind_method(D_METHOD("clear_static_scenes"), &ResonanceServer::clear_static_scenes);
    ClassDB::bind_method(D_METHOD("set_bake_pipeline_pathing", "pathing"), &ResonanceServer::set_bake_pipeline_pathing);
    ClassDB::bind_method(D_METHOD("bake_probes_for_volume", "volume_transform", "extents", "spacing", "generation_type", "height_above_floor", "probe_data", "exclusion_boxes"),
                         &ResonanceServer::bake_probes_for_volume, DEFVAL(Array()));
    ClassDB::bind_method(D_METHOD("bake_pathing", "dat"), &ResonanceServer::bake_pathing);
    ClassDB::bind_method(D_METHOD("bake_static_source", "dat", "endpoint_position", "influence_radius"), &ResonanceServer::bake_static_source);
    ClassDB::bind_method(D_METHOD("bake_static_listener", "dat", "endpoint_position", "influence_radius"), &ResonanceServer::bake_static_listener);
    ClassDB::bind_method(D_METHOD("save_scene_obj", "file_base_name"), &ResonanceServer::save_scene_obj);
    ClassDB::bind_method(D_METHOD("save_scene_data", "filename"), &ResonanceServer::save_scene_data);
    ClassDB::bind_method(D_METHOD("load_scene_data", "filename"), &ResonanceServer::load_scene_data);
    ClassDB::bind_method(D_METHOD("refresh_all_geometry_from_scene_tree"), &ResonanceServer::refresh_all_geometry_from_scene_tree);
    ClassDB::bind_method(D_METHOD("_deferred_refresh_all_geometry_after_scene_load"), &ResonanceServer::_deferred_refresh_all_geometry_after_scene_load);
    ClassDB::bind_method(D_METHOD("export_static_scene_to_asset", "scene_root", "path"), &ResonanceServer::export_static_scene_to_asset);
    ClassDB::bind_method(D_METHOD("export_static_scene_to_obj", "scene_root", "file_base_name"), &ResonanceServer::export_static_scene_to_obj);
    ClassDB::bind_method(D_METHOD("get_static_scene_hash", "scene_root"), &ResonanceServer::get_static_scene_hash);
    ClassDB::bind_method(D_METHOD("get_geometry_asset_hash", "asset"), &ResonanceServer::get_geometry_asset_hash);
    ClassDB::bind_method(D_METHOD("cancel_reflections_bake"), &ResonanceServer::cancel_reflections_bake);
    ClassDB::bind_method(D_METHOD("cancel_pathing_bake"), &ResonanceServer::cancel_pathing_bake);
    ClassDB::bind_method(D_METHOD("get_bake_progress"), &ResonanceServer::get_bake_progress);
    ClassDB::bind_method(D_METHOD("load_probe_batch", "dat"), &ResonanceServer::load_probe_batch);
    ClassDB::bind_method(D_METHOD("remove_probe_batch", "handle"), &ResonanceServer::remove_probe_batch);
    ClassDB::bind_method(D_METHOD("clear_probe_batches"), &ResonanceServer::clear_probe_batches);
    ClassDB::bind_method(D_METHOD("revalidate_probe_batches_with_config"), &ResonanceServer::revalidate_probe_batches_with_config);
    ClassDB::bind_method(D_METHOD("editor_probe_data_get_num_probes", "dat"), &ResonanceServer::editor_probe_data_get_num_probes);
    ClassDB::bind_method(D_METHOD("editor_probe_data_remove_probe", "dat", "index"), &ResonanceServer::editor_probe_data_remove_probe);
    ClassDB::bind_method(D_METHOD("editor_probe_data_remove_baked_layer", "dat", "baked_data_type", "variation", "endpoint", "influence_radius"),
                         &ResonanceServer::editor_probe_data_remove_baked_layer, DEFVAL(Vector3()), DEFVAL(0.0f));
    ClassDB::bind_method(D_METHOD("probe_data_static_source_energy_at", "dat", "endpoint", "listener", "influence_radius", "neighbor_radius"),
                         &ResonanceServer::probe_data_static_source_energy_at, DEFVAL(0.0f));

    // Bind Setter/Getters
    ClassDB::bind_method(D_METHOD("set_debug_occlusion", "p_enabled"), &ResonanceServer::set_debug_occlusion);
    ClassDB::bind_method(D_METHOD("is_debug_occlusion_enabled"), &ResonanceServer::is_debug_occlusion_enabled);

    ClassDB::bind_method(D_METHOD("set_debug_reflections", "p_enabled"), &ResonanceServer::set_debug_reflections);
    ClassDB::bind_method(D_METHOD("is_debug_reflections_enabled"), &ResonanceServer::is_debug_reflections_enabled);
    ClassDB::bind_method(D_METHOD("set_debug_pathing", "p_enabled"), &ResonanceServer::set_debug_pathing);
    ClassDB::bind_method(D_METHOD("is_debug_pathing_enabled"), &ResonanceServer::is_debug_pathing_enabled);
    ClassDB::bind_method(D_METHOD("get_pathing_visualization_segments"), &ResonanceServer::get_pathing_visualization_segments);
    ClassDB::bind_method(D_METHOD("get_ray_debug_segments"), &ResonanceServer::get_ray_debug_segments);
    ClassDB::bind_method(D_METHOD("get_ray_debug_segments_at", "origin"), &ResonanceServer::get_ray_debug_segments_at);
    ClassDB::bind_method(D_METHOD("uses_custom_ray_tracer"), &ResonanceServer::uses_custom_ray_tracer);
    ClassDB::bind_method(D_METHOD("wants_debug_reflection_viz"), &ResonanceServer::wants_debug_reflection_viz);
    ClassDB::bind_method(D_METHOD("is_pathing_enabled"), &ResonanceServer::is_pathing_enabled);
    ClassDB::bind_method(D_METHOD("set_pathing_enabled", "p_enabled"), &ResonanceServer::set_pathing_enabled);

    ClassDB::bind_method(D_METHOD("set_output_direct_enabled", "p_enabled"), &ResonanceServer::set_output_direct_enabled);
    ClassDB::bind_method(D_METHOD("is_output_direct_enabled"), &ResonanceServer::is_output_direct_enabled);

    ClassDB::bind_method(D_METHOD("set_output_reverb_enabled", "p_enabled"), &ResonanceServer::set_output_reverb_enabled);
    ClassDB::bind_method(D_METHOD("is_output_reverb_enabled"), &ResonanceServer::is_output_reverb_enabled);
    ClassDB::bind_method(D_METHOD("set_reverb_bus_click_guard_enabled", "p_enabled"), &ResonanceServer::set_reverb_bus_click_guard_enabled);
    ClassDB::bind_method(D_METHOD("is_reverb_bus_click_guard_enabled"), &ResonanceServer::is_reverb_bus_click_guard_enabled);
    ClassDB::bind_method(D_METHOD("set_reverb_bus_wet_ring_underrun_zero_fill", "p_enabled"),
                         &ResonanceServer::set_reverb_bus_wet_ring_underrun_zero_fill);
    ClassDB::bind_method(D_METHOD("is_reverb_bus_wet_ring_underrun_zero_fill"),
                         &ResonanceServer::is_reverb_bus_wet_ring_underrun_zero_fill);

    ClassDB::bind_method(D_METHOD("set_reverb_influence_radius", "p_radius"), &ResonanceServer::set_reverb_influence_radius);
    ClassDB::bind_method(D_METHOD("get_reverb_influence_radius"), &ResonanceServer::get_reverb_influence_radius);
    ClassDB::bind_method(D_METHOD("set_reverb_transmission_amount", "p_amount"), &ResonanceServer::set_reverb_transmission_amount);
    ClassDB::bind_method(D_METHOD("get_reverb_transmission_amount"), &ResonanceServer::get_reverb_transmission_amount);
    ClassDB::bind_method(D_METHOD("set_apply_occlusion_to_baked_reflections", "p_enabled"), &ResonanceServer::set_apply_occlusion_to_baked_reflections);
    ClassDB::bind_method(D_METHOD("get_apply_occlusion_to_baked_reflections"), &ResonanceServer::get_apply_occlusion_to_baked_reflections);
    ClassDB::bind_method(D_METHOD("set_baked_reverb_use_listener_probe", "p_enabled"), &ResonanceServer::set_baked_reverb_use_listener_probe);
    ClassDB::bind_method(D_METHOD("get_baked_reverb_use_listener_probe"), &ResonanceServer::get_baked_reverb_use_listener_probe);
    ClassDB::bind_method(D_METHOD("set_perspective_correction_enabled", "p_enabled"), &ResonanceServer::set_perspective_correction_enabled);
    ClassDB::bind_method(D_METHOD("is_perspective_correction_enabled"), &ResonanceServer::is_perspective_correction_enabled);
    ClassDB::bind_method(D_METHOD("set_perspective_correction_factor", "p_factor"), &ResonanceServer::set_perspective_correction_factor);
    ClassDB::bind_method(D_METHOD("get_perspective_correction_factor"), &ResonanceServer::get_perspective_correction_factor);
    ClassDB::bind_method(D_METHOD("get_reflection_type"), &ResonanceServer::get_reflection_type);
    ClassDB::bind_method(D_METHOD("get_realtime_rays"), &ResonanceServer::get_realtime_rays);
    ClassDB::bind_method(D_METHOD("get_ambisonic_order"), &ResonanceServer::get_ambisonic_order);
    ClassDB::bind_method(D_METHOD("get_default_reflections_mode"), &ResonanceServer::get_default_reflections_mode);
    ClassDB::bind_method(D_METHOD("get_reverb_bus_instrumentation"), &ResonanceServer::get_reverb_bus_instrumentation);
    ClassDB::bind_method(D_METHOD("reset_reverb_bus_instrumentation"), &ResonanceServer::reset_reverb_bus_instrumentation);
    ClassDB::bind_method(D_METHOD("get_pathing_instrumentation"), &ResonanceServer::get_pathing_instrumentation);
    ClassDB::bind_method(D_METHOD("reset_pathing_instrumentation"), &ResonanceServer::reset_pathing_instrumentation);
    ClassDB::bind_method(D_METHOD("get_simulation_worker_timing"), &ResonanceServer::get_simulation_worker_timing);
    ClassDB::bind_method(D_METHOD("get_active_source_count"), &ResonanceServer::get_active_source_count);
    ClassDB::bind_method(D_METHOD("get_active_probe_batch_count"), &ResonanceServer::get_active_probe_batch_count);
    ClassDB::bind_method(D_METHOD("get_source_lifecycle_epoch"), &ResonanceServer::get_source_lifecycle_epoch);
    ClassDB::bind_method(D_METHOD("peek_reverb_params_likely_available", "source_handle"), &ResonanceServer::peek_reverb_params_likely_available);
    ClassDB::bind_method(D_METHOD("get_convolution_audio_timing"), &ResonanceServer::get_convolution_audio_timing);
    ClassDB::bind_method(D_METHOD("get_simulation_tracer_profile"), &ResonanceServer::get_simulation_tracer_profile);

    // Properties
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_occlusion"), "set_debug_occlusion", "is_debug_occlusion_enabled");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_reflections"), "set_debug_reflections", "is_debug_reflections_enabled");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_pathing"), "set_debug_pathing", "is_debug_pathing_enabled");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "output_direct"), "set_output_direct_enabled", "is_output_direct_enabled");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "output_reverb"), "set_output_reverb_enabled", "is_output_reverb_enabled");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "reverb_bus_click_guard_enabled"), "set_reverb_bus_click_guard_enabled", "is_reverb_bus_click_guard_enabled");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "reverb_bus_wet_ring_underrun_zero_fill"), "set_reverb_bus_wet_ring_underrun_zero_fill",
                 "is_reverb_bus_wet_ring_underrun_zero_fill");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "reverb_influence_radius", PROPERTY_HINT_RANGE, "1,50000,1"), "set_reverb_influence_radius", "get_reverb_influence_radius");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "reverb_transmission_amount", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_reverb_transmission_amount", "get_reverb_transmission_amount");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "apply_occlusion_to_baked_reflections"), "set_apply_occlusion_to_baked_reflections", "get_apply_occlusion_to_baked_reflections");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "baked_reverb_use_listener_probe"), "set_baked_reverb_use_listener_probe", "get_baked_reverb_use_listener_probe");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "perspective_correction_enabled"), "set_perspective_correction_enabled", "is_perspective_correction_enabled");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "perspective_correction_factor", PROPERTY_HINT_RANGE, "0.1,3.0,0.1"), "set_perspective_correction_factor", "get_perspective_correction_factor");
}