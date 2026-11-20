#include "resonance_attenuation_callback_policy.h"
#include "resonance_attenuation_dirty_policy.h"
#include "resonance_constants.h"
#include "resonance_log.h"
#include "resonance_math.h"
#include "resonance_pathing_inputs_policy.h"
#include "resonance_polyphonic_voice_policy.h"
#include "resonance_server.h"
#include "resonance_sim_distance_attenuation_policy.h"
#include "resonance_source_handle_policy.h"
#include "resonance_source_update_policy.h"
#include "resonance_utils.h"
#include <cstdint>
#include <godot_cpp/variant/utility_functions.hpp>
#include <mutex>
#include <vector>

using namespace godot;

namespace {

void fill_source_coordinate_space(IPLCoordinateSpace3& source, Vector3 pos, Vector3 source_forward, Vector3 source_up) {
    source.origin = ResonanceUtils::to_ipl_vector3(pos);
    const Vector3 ahead_n = ResonanceUtils::safe_unit_vector(source_forward, Vector3(0, 0, -1));
    const Vector3 up_raw = ResonanceUtils::safe_unit_vector(source_up, Vector3(0, 1, 0));
    const Vector3 right_n = ResonanceUtils::safe_unit_vector(ahead_n.cross(up_raw), Vector3(1, 0, 0));
    const Vector3 up_n = ResonanceUtils::safe_unit_vector(right_n.cross(ahead_n), Vector3(0, 1, 0));
    source.ahead = ResonanceUtils::to_ipl_vector3(ahead_n);
    source.up = ResonanceUtils::to_ipl_vector3(up_n);
    source.right = ResonanceUtils::to_ipl_vector3(right_n);
}

// baked_data_variation: -1 = realtime, 0 = reverb, 1 = static source, 2 = static listener.
void fill_baked_reflection_identifier(IPLSimulationInputs& inputs, int baked_data_variation, Vector3 baked_endpoint_center,
                                      float baked_endpoint_radius, float reverb_influence_radius) {
    if (baked_data_variation == -1) {
        inputs.baked = IPL_FALSE;
        inputs.bakedDataIdentifier.type = IPL_BAKEDDATATYPE_REFLECTIONS;
        inputs.bakedDataIdentifier.variation = IPL_BAKEDDATAVARIATION_REVERB;
        inputs.bakedDataIdentifier.endpointInfluence.center = {0.0f, 0.0f, 0.0f};
        inputs.bakedDataIdentifier.endpointInfluence.radius = 0.0f;
        return;
    }
    inputs.baked = IPL_TRUE;
    inputs.bakedDataIdentifier.type = IPL_BAKEDDATATYPE_REFLECTIONS;
    if (baked_data_variation == 1) {
        inputs.bakedDataIdentifier.variation = IPL_BAKEDDATAVARIATION_STATICSOURCE;
        inputs.bakedDataIdentifier.endpointInfluence.center = ResonanceUtils::to_ipl_vector3(baked_endpoint_center);
        inputs.bakedDataIdentifier.endpointInfluence.radius =
            (baked_endpoint_radius > 0.0f) ? baked_endpoint_radius : reverb_influence_radius;
    } else if (baked_data_variation == 2) {
        inputs.bakedDataIdentifier.variation = IPL_BAKEDDATAVARIATION_STATICLISTENER;
        inputs.bakedDataIdentifier.endpointInfluence.center = ResonanceUtils::to_ipl_vector3(baked_endpoint_center);
        inputs.bakedDataIdentifier.endpointInfluence.radius =
            (baked_endpoint_radius > 0.0f) ? baked_endpoint_radius : reverb_influence_radius;
    } else {
        inputs.bakedDataIdentifier.variation = IPL_BAKEDDATAVARIATION_REVERB;
        inputs.bakedDataIdentifier.endpointInfluence.center = {0.0f, 0.0f, 0.0f};
        inputs.bakedDataIdentifier.endpointInfluence.radius = 0.0f;
    }
}

} // namespace

int32_t ResonanceServer::_capped_source_count() const {
    const int32_t count = source_manager.size_approx();
    if (get_listener_reverb_source_handle() < 0 || count == 0)
        return count;
    return count - 1;
}

bool ResonanceServer::simulation_source_cap_blocks_create() const {
    return simulation_source_cap_error_emitted_ &&
           resonance::source_count_at_simulation_limit(_capped_source_count(), max_simulation_sources);
}

IPLSourceSettings ResonanceServer::_player_source_settings() {
    IPLSourceSettings settings{};
    // Always created with PATHING; each source's inputs decide whether it takes part in RunPathing.
    settings.flags = static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS |
                                                     IPL_SIMULATIONFLAGS_PATHING);
    return settings;
}

int32_t ResonanceServer::create_source_handle(Vector3 pos, float radius, const String& pathing_owner_path,
                                              int pathing_enabled_override, bool listener_reverb_slot) {
    if (!_ctx() || !simulator)
        return -1;
    if (!listener_reverb_slot &&
        resonance::reject_simulation_source_create_at_cap(
            _capped_source_count(), max_simulation_sources, simulation_source_cap_error_emitted_, [] {
                ResonanceLog::error("ResonanceServer: max_simulation_sources exceeded (create_source_handle).");
            }))
        return -1;
    IPLSource src = resource_pool_.take_source();
    if (!src) {
        IPLSourceSettings settings = _player_source_settings();
        if (iplSourceCreate(simulator, &settings, &src) != IPL_STATUS_SUCCESS || !src) {
            ResonanceLog::error("ResonanceServer: iplSourceCreate failed (create_source_handle).");
            return -1;
        }
    }
    // iplSourceAdd runs later on the worker.
    const int32_t handle = source_manager.add_source(src);
    if (handle < 0) {
        iplSourceRelease(&src);
        return -1;
    }
    const bool attach_pathing = resonance::source_sim_pathing_enabled(
        pathing_enabled_override, pathing_enabled, output_reverb_enabled.load(std::memory_order_acquire));
    if (handle < kMaxCacheHandles)
        _retire_reflection_ir_for_handle(handle);
    SourceUpdateParams initial = _default_new_source_params();
    initial.position = pos;
    initial.radius = radius;
    source_table_.source_created(handle, initial, attach_pathing, [&] {
        if (handle >= kMaxCacheHandles)
            return;
        for (int slot = 0; slot < kCacheSlots; slot++) {
            occlusion_cache_[static_cast<size_t>(slot)][static_cast<size_t>(handle)].epoch = 0;
            reverb_param_cache_[static_cast<size_t>(slot)][static_cast<size_t>(handle)].epoch = 0;
            reflection_param_cache_[static_cast<size_t>(slot)][static_cast<size_t>(handle)].epoch = 0;
            pathing_param_cache_[static_cast<size_t>(slot)][static_cast<size_t>(handle)].epoch = 0;
        }
        last_good_reflection_valid_[static_cast<size_t>(handle)].store(0, std::memory_order_relaxed);
        reflection_baked_energy_last_[static_cast<size_t>(handle)].store(0.0f, std::memory_order_relaxed);
    });
    if (!pathing_owner_path.is_empty()) {
        std::lock_guard<std::mutex> o(source_pathing_owner_mutex_);
        source_pathing_owner_path_[handle] = pathing_owner_path;
    }
    iplSourceRelease(&src); // SourceManager keeps its own retain until destroy
    _wake_phonon_worker_for_lifecycle();
    return handle;
}

void ResonanceServer::_destroy_source_handle_under_simulation_lock(resonance::SimulationAccess access, int32_t handle) {
    IPLSource src = source_manager.get_source(handle);
    if (src) {
        if (simulator) {
            iplSourceRemove(src, simulator);
            iplSimulatorCommit(simulator);
            _forget_source_inputs_assume_locked(access, handle);
        }
        iplSourceRelease(&src);
    }
    {
        std::lock_guard<std::recursive_mutex> cb_lock(_attenuation_callback_mutex);
        _source_attenuation_entries.erase(handle);
    }
    source_table_.forget_snapshot(access, handle);
    source_table_.clear_pass_outputs(handle);
    source_manager.remove_source(handle);
}

void ResonanceServer::_retire_reflection_ir_for_handle(int32_t handle) {
    if (handle < 0 || handle >= kMaxCacheHandles)
        return;
    source_ir_generation_[static_cast<size_t>(handle)].fetch_add(1u, std::memory_order_acq_rel);
    for (int slot = 0; slot < kCacheSlots; slot++) {
        reflection_param_cache_[static_cast<size_t>(slot)][static_cast<size_t>(handle)].params.ir = nullptr;
        reflection_param_cache_[static_cast<size_t>(slot)][static_cast<size_t>(handle)].ir_generation = 0;
    }
    last_good_reflection_params_[static_cast<size_t>(handle)].ir = nullptr;
    last_good_reflection_valid_[static_cast<size_t>(handle)].store(0, std::memory_order_release);
}

uint32_t ResonanceServer::get_source_ir_generation(int32_t handle) const {
    if (handle < 0 || handle >= kMaxCacheHandles)
        return 0u;
    return source_ir_generation_[static_cast<size_t>(handle)].load(std::memory_order_acquire);
}

void ResonanceServer::destroy_source_handle(int32_t handle) {
    if (handle < 0 || is_shutting_down_flag.load(std::memory_order_acquire) || !_ctx())
        return;
    _retire_reflection_ir_for_handle(handle);
    // The handle is invalid from here on; the worker does the Remove, Commit and final Release.
    IPLSource src = source_manager.get_source(handle); // Retains
    // Recycle only once no queued add or remove still refers to the handle; otherwise a new source on the
    // same handle could be added twice or lose its state to the old source's post-remove cleanup.
    source_manager.remove_source(handle, false /* recycle */);
    const resonance::SourceDestroyOutcome outcome = source_table_.source_destroyed(handle, src);
    if (outcome == resonance::SourceDestroyOutcome::kReleaseAndRecycleNow)
        iplSourceRelease(&src);
    if (outcome != resonance::SourceDestroyOutcome::kRemoveQueued)
        source_manager.recycle_source_handle(handle);
    {
        std::lock_guard<std::mutex> h_lock(reverb_params_likely_available_mutex_);
        reverb_params_likely_available_.erase(handle);
    }
    {
        std::lock_guard<std::mutex> o(source_pathing_owner_mutex_);
        source_pathing_owner_path_.erase(handle);
    }
    _wake_phonon_worker_for_lifecycle();
}

void ResonanceServer::update_source(int32_t handle, const SourceUpdateParams& params) {
    enqueue_source_update(handle, params);
}

bool ResonanceServer::try_update_source(int32_t handle, const SourceUpdateParams& params) {
    if (handle < 0)
        return false;
    std::unique_lock<std::mutex> lock(simulation_mutex, std::defer_lock);
    if (!lock.try_lock())
        return false;
    const resonance::SimulationAccess access(lock);
    if (source_table_.replace_pending_add_initial(handle, params))
        return true;
    IPLSource src = source_manager.get_source(handle);
    if (!src)
        return false;
    _update_source_internal(access, src, handle, params);
    iplSourceRelease(&src);
    if (handle >= 0 && handle < kMaxCacheHandles &&
        source_table_.takes_part_in(resonance::SourcePass::kDirect, handle)) {
        _arm_phonon_direct_after_inline_inputs();
    }
    return true;
}

void ResonanceServer::enqueue_source_update(int32_t handle, const SourceUpdateParams& params) {
    if (handle < 0)
        return;
    source_table_.enqueue_update(handle, params);
}

int ResonanceServer::_apply_source_update_batch(resonance::SimulationAccess access, const std::vector<SourceTable::QueuedUpdate>& batch) {
    int applied = 0;
    for (const SourceTable::QueuedUpdate& update : batch) {
        if (source_table_.replace_pending_add_initial(update.handle, update.params))
            continue;
        IPLSource src = source_manager.get_source(update.handle);
        if (!src)
            continue;
        if (_update_source_internal(access, src, update.handle, update.params))
            applied++;
        iplSourceRelease(&src);
    }
    return applied;
}

bool ResonanceServer::_flush_pending_source_updates_assume_locked(resonance::SimulationAccess access) {
    const std::vector<SourceTable::QueuedUpdate> batch = source_table_.take_updates();
    if (batch.empty())
        return false;
    const bool applied = _apply_source_update_batch(access, batch) > 0;
    if (access.direct_only())
        source_table_.requeue_not_superseded(batch);
    return applied;
}

void ResonanceServer::flush_pending_source_updates() {
    // Never block main: if the worker holds the mutex, requeue only the handles not updated meanwhile.
    const std::vector<SourceTable::QueuedUpdate> batch = source_table_.take_updates();
    if (batch.empty())
        return;
    std::unique_lock<std::mutex> sim_lock(simulation_mutex, std::defer_lock);
    if (!sim_lock.try_lock()) {
        instrumentation_source_flush_try_lock_fail_.fetch_add(1, std::memory_order_relaxed);
        source_table_.requeue_not_superseded(batch);
        return;
    }
    const resonance::SimulationAccess access(sim_lock);
    _apply_source_update_batch(access, batch);
}

void ResonanceServer::set_source_attenuation_callback_data(int32_t handle, int attenuation_mode, float min_distance, float max_distance, const PackedFloat32Array& curve_samples) {
    if (handle < 0)
        return;
    if (!source_manager.has_handle(handle))
        return;
    const int num_new =
        resonance::attenuation_callback_num_curve_samples(attenuation_mode, curve_samples.size());
    std::lock_guard<std::recursive_mutex> lock(_attenuation_callback_mutex);
    auto& entry_ptr = _source_attenuation_entries[handle];
    if (entry_ptr && entry_ptr->data) {
        const AttenuationCallbackData& prev = *entry_ptr->data;
        float new_curve[resonance::kAttenuationCurveSamples]{};
        for (int i = 0; i < num_new && i < curve_samples.size(); i++)
            new_curve[i] = curve_samples[i];
        if (resonance::attenuation_callback_data_equal(prev.mode, prev.min_distance, prev.max_distance, prev.curve_samples,
                                                       prev.num_curve_samples, attenuation_mode, min_distance, max_distance,
                                                       new_curve, num_new)) {
            instrumentation_attenuation_callback_skip_unchanged_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    if (!entry_ptr)
        entry_ptr = std::make_unique<AttenuationEntry>();
    if (!entry_ptr->data)
        entry_ptr->data = std::make_unique<AttenuationCallbackData>();
    AttenuationCallbackData& d = *entry_ptr->data;
    d.mode = attenuation_mode;
    d.min_distance = min_distance;
    d.max_distance = max_distance;
    d.num_curve_samples = num_new;
    for (int i = 0; i < d.num_curve_samples && i < curve_samples.size(); i++) {
        d.curve_samples[i] = curve_samples[i];
    }
    entry_ptr->curve_dirty = true;
    if (handle >= 0)
        source_table_.invalidate_snapshot(handle);
}

void ResonanceServer::clear_source_attenuation_callback_data(int32_t handle) {
    if (handle < 0 || !_ctx())
        return;
    std::lock_guard<std::mutex> sim_lock(simulation_mutex);
    const resonance::SimulationAccess access(sim_lock);
    std::lock_guard<std::recursive_mutex> cb_lock(_attenuation_callback_mutex);
    _source_attenuation_entries.erase(handle);
    IPLSource src = source_manager.get_source(handle);
    if (!src)
        return;
    const SourceUpdateParams* snapshot = source_table_.snapshot(access, handle);
    if (!snapshot) {
        iplSourceRelease(&src);
        return;
    }
    const SourceUpdateParams params = *snapshot;
    _update_source_internal(access, src, handle, params);
    iplSourceRelease(&src);
}

void ResonanceServer::set_source_pathing_owner_path(int32_t handle, const String& owner_path) {
    if (handle < 0)
        return;
    std::lock_guard<std::mutex> lock(source_pathing_owner_mutex_);
    if (owner_path.is_empty())
        source_pathing_owner_path_.erase(handle);
    else
        source_pathing_owner_path_[handle] = owner_path;
}

// Mode 1 = linear 1 -> 0, mode 2 = user curve samples between min and max distance.
static float IPLCALL distance_attenuation_callback(IPLfloat32 distance, void* userData) {
    const ResonanceServer::AttenuationCallbackContext* ctx = static_cast<const ResonanceServer::AttenuationCallbackContext*>(userData);
    if (!ctx || !ctx->mutex || !ctx->data)
        return 1.0f;
    std::lock_guard<std::recursive_mutex> lock(*ctx->mutex);
    const ResonanceServer::AttenuationCallbackData* d = ctx->data;
    if (!d || d->max_distance <= d->min_distance)
        return 1.0f;
    if (distance <= d->min_distance)
        return (d->mode == 2 && d->num_curve_samples > 0) ? resonance::sanitize_audio_float(d->curve_samples[0]) : 1.0f;
    if (distance >= d->max_distance)
        return (d->mode == 2 && d->num_curve_samples > 0) ? resonance::sanitize_audio_float(d->curve_samples[d->num_curve_samples - 1]) : 0.0f;
    float t = (distance - d->min_distance) / (d->max_distance - d->min_distance);
    t = (t < 0.0f) ? 0.0f : (t > 1.0f) ? 1.0f
                                       : t;
    if (d->mode == 1)
        return 1.0f - t;
    if (d->mode == 2 && d->num_curve_samples > 1) {
        float idx = t * static_cast<float>(d->num_curve_samples - 1);
        int i0 = (int)idx;
        int i1 = (i0 + 1 < d->num_curve_samples) ? i0 + 1 : i0;
        float frac = idx - (float)i0;
        float v = d->curve_samples[i0] * (1.0f - frac) + d->curve_samples[i1] * frac;
        return resonance::sanitize_audio_float(v);
    }
    return resonance::sanitize_audio_float(1.0f - t);
}

ResonanceServer::SourceUpdateParams ResonanceServer::_default_new_source_params() const {
    SourceUpdateParams p;
    p.occlusion_samples = resonance::kDefaultOcclusionSamples;
    p.num_transmission_rays = max_transmission_surfaces;
    // Pathing stays off until the owner's first update, so a source without a probe volume does not warn at
    // attach time.
    p.pathing_enabled_override = 0;
    return p;
}

void ResonanceServer::_set_source_inputs_assume_locked(resonance::SimulationAccess access, IPLSource src, int32_t handle, IPLSimulationFlags selector,
                                                       IPLSimulationInputs* inputs) {
    iplSourceSetInputs(src, selector, inputs);
    const uint64_t hash = resonance::fnv1a_hash(reinterpret_cast<const uint8_t*>(inputs), sizeof(*inputs));
    source_table_.record_inputs(access, handle, static_cast<size_t>(selector) & 7u, hash);
}

void ResonanceServer::_forget_source_inputs_assume_locked(resonance::SimulationAccess access, int32_t handle) {
    source_table_.forget_inputs(access, handle);
}

bool ResonanceServer::_update_source_internal(resonance::SimulationAccess access, IPLSource src, int32_t handle, const SourceUpdateParams& params) {
    if (!src || !_ctx())
        return false;

    bool attenuation_curve_dirty = false;
    {
        std::lock_guard<std::recursive_mutex> cb_lock(_attenuation_callback_mutex);
        auto att_it = _source_attenuation_entries.find(handle);
        if (att_it != _source_attenuation_entries.end() && att_it->second)
            attenuation_curve_dirty = att_it->second->curve_dirty;
    }
    const resonance::SourceUpdateDecision decision =
        source_table_.begin_update(access, handle, params, attenuation_curve_dirty);
    if (decision == resonance::SourceUpdateDecision::kSkipUnchanged)
        instrumentation_source_update_skip_unchanged_.fetch_add(1, std::memory_order_relaxed);
    if (decision != resonance::SourceUpdateDecision::kApply)
        return false;

    IPLSimulationInputs inputs{};
    const bool output_reverb = output_reverb_enabled.load(std::memory_order_acquire);
    bool enable_reflections =
        params.simulation_active && resonance::source_sim_reflections_enabled(params.reflections_enabled_override, output_reverb);
    // No realtime reflection rays for sources beyond the cap.
    if (enable_reflections && params.baked_data_variation == -1 && realtime_reflection_max_distance_m > 0.0f) {
        const Vector3 lip = ResonanceUtils::to_godot_vector3(get_current_listener_coords().origin);
        if (params.position.distance_to(lip) > static_cast<real_t>(realtime_reflection_max_distance_m))
            enable_reflections = false;
    }

    const bool enable_pathing =
        params.simulation_active &&
        resonance::source_sim_pathing_enabled(params.pathing_enabled_override, pathing_enabled, output_reverb);

    // Mix levels only affect the DSP. An active source always requests Direct; the wet flags follow the
    // enable flags and output_reverb.
    IPLSimulationFlags sim_flags =
        static_cast<IPLSimulationFlags>(resonance::source_direct_sim_flags_mask(static_cast<int>(IPL_SIMULATIONFLAGS_DIRECT)));
    if (enable_reflections)
        sim_flags = static_cast<IPLSimulationFlags>(sim_flags | IPL_SIMULATIONFLAGS_REFLECTIONS);

    IPLDirectSimulationFlags dflags = (IPLDirectSimulationFlags)0;
    if (params.simulation_occlusion_enabled)
        dflags = (IPLDirectSimulationFlags)(dflags | IPL_DIRECTSIMULATIONFLAGS_OCCLUSION);
    if (params.simulation_transmission_enabled)
        dflags = (IPLDirectSimulationFlags)(dflags | IPL_DIRECTSIMULATIONFLAGS_TRANSMISSION);
    inputs.directFlags = dflags;
    if (params.use_sim_distance_attenuation)
        inputs.directFlags = (IPLDirectSimulationFlags)(inputs.directFlags | IPL_DIRECTSIMULATIONFLAGS_DISTANCEATTENUATION);
    if (params.air_absorption_enabled)
        inputs.directFlags = (IPLDirectSimulationFlags)(inputs.directFlags | IPL_DIRECTSIMULATIONFLAGS_AIRABSORPTION);
    if (params.directivity_weight != 0.0f || params.directivity_power != 1.0f)
        inputs.directFlags = (IPLDirectSimulationFlags)(inputs.directFlags | IPL_DIRECTSIMULATIONFLAGS_DIRECTIVITY);
    if (!params.simulation_active)
        inputs.directFlags = static_cast<IPLDirectSimulationFlags>(0);

    fill_source_coordinate_space(inputs.source, params.position, params.source_forward, params.source_up);
    inputs.airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;
    inputs.directivity.dipoleWeight = params.directivity_weight;
    inputs.directivity.dipolePower = params.directivity_power;
    inputs.directivity.callback = nullptr;
    inputs.directivity.userData = nullptr;

    // The callback userData must stay valid through iplSourceSetInputs.
    bool have_callback_entry = false;
    AttenuationCallbackContext* callback_ctx = nullptr;
    {
        std::lock_guard<std::recursive_mutex> cb_lock(_attenuation_callback_mutex);
        auto it = _source_attenuation_entries.find(handle);
        if (it != _source_attenuation_entries.end() && it->second && it->second->data) {
            AttenuationCallbackData* pdata = it->second->data.get();
            if (pdata->mode == 1 || pdata->mode == 2) {
                have_callback_entry = true;
                AttenuationEntry& entry = *it->second;
                entry.ctx.mutex = &_attenuation_callback_mutex;
                entry.ctx.data = pdata;
                callback_ctx = &entry.ctx;
            }
        }
    }

    const bool da_enabled = params.distance_attenuation && params.attenuation_mode != resonance::kAttenuationModeDisabled;

    auto apply_distance_model = [&](IPLDistanceAttenuationModel& model, resonance::SimDistanceModelKind kind) {
        model = {};
        if (kind == resonance::SimDistanceModelKind::Callback && have_callback_entry && callback_ctx) {
            model.type = IPL_DISTANCEATTENUATIONTYPE_CALLBACK;
            model.minDistance = callback_ctx->data->min_distance;
            model.callback = distance_attenuation_callback;
            model.userData = callback_ctx;
            model.dirty = resonance::attenuation_callback_model_is_dirty(attenuation_curve_dirty) ? IPL_TRUE : IPL_FALSE;
            return;
        }
        if (kind == resonance::SimDistanceModelKind::InverseDistance) {
            model.type = IPL_DISTANCEATTENUATIONTYPE_INVERSEDISTANCE;
            model.minDistance = params.min_distance;
            return;
        }
        model.type = IPL_DISTANCEATTENUATIONTYPE_DEFAULT;
    };

    const resonance::SimDistanceModelKind direct_kind =
        resonance::sim_distance_model_for_direct(da_enabled, params.attenuation_mode);
    const resonance::SimDistanceModelKind pathing_kind =
        resonance::sim_distance_model_for_pathing(da_enabled, params.attenuation_mode);
    const resonance::SimDistanceModelKind reflections_kind = resonance::sim_distance_model_for_reflections(
        da_enabled, params.attenuation_mode, params.use_distance_curve_for_reflections);

    int eff_occlusion_type = occlusion_type;
    if (params.occlusion_type_override == 0 || params.occlusion_type_override == 1)
        eff_occlusion_type = params.occlusion_type_override;
    inputs.occlusionType = (eff_occlusion_type == 0) ? IPL_OCCLUSIONTYPE_RAYCAST : IPL_OCCLUSIONTYPE_VOLUMETRIC;
    inputs.occlusionRadius = params.radius;
    inputs.numOcclusionSamples = CLAMP(params.occlusion_samples, 1, simulation_settings.maxNumOcclusionSamples);
    inputs.numTransmissionRays = CLAMP(params.num_transmission_rays, 1, resonance::kMaxTransmissionRays);

    fill_baked_reflection_identifier(inputs, params.baked_data_variation, params.baked_endpoint_center, params.baked_endpoint_radius, reverb_influence_radius);

    inputs.reverbScale[0] = 1.0f;
    inputs.reverbScale[1] = 1.0f;
    inputs.reverbScale[2] = 1.0f;
    inputs.hybridReverbTransitionTime = hybrid_reverb_transition_time;
    inputs.hybridReverbOverlapPercent = hybrid_reverb_overlap_percent;

    // The probe batch stays retained past SetInputs; the worker drain releases it.
    IPLProbeBatch pathing_batch_retained = nullptr;
    if (enable_pathing && pathing_enabled) {
        IPLProbeBatch path_batch = _get_pathing_batch_for_source(handle, params.pathing_probe_batch_handle);
        if (path_batch) {
            sim_flags = static_cast<IPLSimulationFlags>(sim_flags | IPL_SIMULATIONFLAGS_PATHING);
            inputs.pathingProbes = path_batch;
            inputs.pathingOrder = ambisonic_order;
            inputs.visRadius = pathing_vis_radius;
            inputs.visThreshold = pathing_vis_threshold;
            inputs.visRange = pathing_vis_range;
            const bool eff_validation = params.path_validation_enabled;
            bool eff_alternate = params.find_alternate_paths;
            if (!eff_validation)
                eff_alternate = false;
            inputs.enableValidation = eff_validation ? IPL_TRUE : IPL_FALSE;
            inputs.findAlternatePaths = eff_alternate ? IPL_TRUE : IPL_FALSE;
            {
                std::lock_guard<std::mutex> d_lock(_pathing_deviation_mutex);
                inputs.deviationModel =
                    (_pathing_deviation_callback_enabled && _pathing_deviation_model.callback) ? &_pathing_deviation_model : nullptr;
            }
            pathing_batch_retained = path_batch;
        }
    }

    source_table_.set_direct_output(handle, params.simulation_active);

    // Direct inputs go in their own call; PATHING must never be in this selector.
    apply_distance_model(inputs.distanceAttenuationModel, direct_kind);
    inputs.flags = static_cast<IPLSimulationFlags>(
        resonance::source_direct_set_inputs_selector_flags(static_cast<int>(IPL_SIMULATIONFLAGS_DIRECT)));
    _set_source_inputs_assume_locked(access, src, handle, IPL_SIMULATIONFLAGS_DIRECT, &inputs);

    source_table_.direct_inputs_set(access, handle);
    if (access.direct_only()) {
        if (pathing_batch_retained)
            iplProbeBatchRelease(&pathing_batch_retained);
        return true;
    }

    source_table_.set_wet_outputs(access, handle, enable_reflections,
                                  enable_reflections && params.baked_data_variation == -1,
                                  (sim_flags & IPL_SIMULATIONFLAGS_PATHING) != 0);

    // Reflections and pathing share one call unless their distance models differ; otherwise the pathing
    // callback would rewrite the reflections IR correction.
    const bool want_reflections = enable_reflections && ((sim_flags & IPL_SIMULATIONFLAGS_REFLECTIONS) != 0);
    const bool want_pathing = (sim_flags & IPL_SIMULATIONFLAGS_PATHING) != 0;
    const int wet_inputs_flags =
        static_cast<int>(sim_flags) & (static_cast<int>(IPL_SIMULATIONFLAGS_REFLECTIONS) | static_cast<int>(IPL_SIMULATIONFLAGS_PATHING));
    const bool split = want_reflections && want_pathing &&
                       resonance::sim_distance_models_need_split_wet_set_inputs(reflections_kind, pathing_kind);
    if (split) {
        apply_distance_model(inputs.distanceAttenuationModel, reflections_kind);
        inputs.flags = static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_REFLECTIONS);
        _set_source_inputs_assume_locked(access, src, handle, IPL_SIMULATIONFLAGS_REFLECTIONS, &inputs);

        apply_distance_model(inputs.distanceAttenuationModel, pathing_kind);
        inputs.flags = static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_PATHING);
        _set_source_inputs_assume_locked(access, src, handle, IPL_SIMULATIONFLAGS_PATHING, &inputs);
    } else {
        resonance::SimDistanceModelKind wet_kind = resonance::SimDistanceModelKind::Default;
        if (want_pathing)
            wet_kind = pathing_kind;
        else if (want_reflections)
            wet_kind = reflections_kind;
        apply_distance_model(inputs.distanceAttenuationModel, wet_kind);
        inputs.flags = static_cast<IPLSimulationFlags>(wet_inputs_flags);
        const IPLSimulationFlags wet_selector = static_cast<IPLSimulationFlags>(resonance::source_set_inputs_selector_flags(
            wet_inputs_flags, static_cast<int>(IPL_SIMULATIONFLAGS_REFLECTIONS),
            static_cast<int>(IPL_SIMULATIONFLAGS_PATHING), pathing_enabled));
        _set_source_inputs_assume_locked(access, src, handle, wet_selector, &inputs);
    }

    if (have_callback_entry && attenuation_curve_dirty) {
        std::lock_guard<std::recursive_mutex> cb_lock(_attenuation_callback_mutex);
        auto it = _source_attenuation_entries.find(handle);
        if (it != _source_attenuation_entries.end() && it->second)
            it->second->curve_dirty = false;
    }

    if (pathing_batch_retained)
        pathing_probe_batches_pending_release_.push_back(pathing_batch_retained);
    return true;
}

void ResonanceServer::_clear_source_pathing_inputs_assume_locked(resonance::SimulationAccess access, IPLSource source, int32_t handle) {
    if (!source)
        return;
    IPLSimulationInputs inputs{};
    inputs.flags = static_cast<IPLSimulationFlags>(0);
    inputs.pathingProbes = nullptr;
    _set_source_inputs_assume_locked(access, source, handle, IPL_SIMULATIONFLAGS_PATHING, &inputs);
    source_table_.clear_pathing_output(handle);
}

void ResonanceServer::_collect_handles_affected_by_probe_batch_remove_assume_locked(resonance::SimulationAccess access, int32_t removing_handle,
                                                                                    std::vector<int32_t>& out_handles) {
    if (removing_handle < 0 || !simulator)
        return;
    std::vector<int32_t> handles;
    source_manager.get_all_handles(handles);
    for (int32_t h : handles) {
        const SourceUpdateParams* snapshot = source_table_.snapshot(access, h);
        if (!snapshot)
            continue;
        const int32_t preferred = snapshot->pathing_probe_batch_handle;
        const bool preferred_usable = probe_batch_registry_.handle_has_pathing(preferred);
        if (!resonance::source_should_clear_pathing_on_batch_remove(preferred, removing_handle, preferred_usable))
            continue;
        out_handles.push_back(h);
    }
}

void ResonanceServer::_clear_pathing_for_probe_batch_assume_locked(resonance::SimulationAccess access, int32_t removing_handle) {
    if (removing_handle < 0 || !simulator)
        return;
    std::vector<int32_t> affected;
    _collect_handles_affected_by_probe_batch_remove_assume_locked(access, removing_handle, affected);
    for (int32_t h : affected) {
        IPLSource src = source_manager.get_source(h);
        if (!src)
            continue;
        _clear_source_pathing_inputs_assume_locked(access, src, h);
        iplSourceRelease(&src);
    }
}

bool ResonanceServer::_drain_pending_source_lifecycle_assume_locked(resonance::SimulationAccess access, uint64_t* commit_us_out) {
    if (commit_us_out)
        *commit_us_out = 0;
    if (!_ctx() || !simulator)
        return false;

    SourceTable::LifecycleDrain drain = source_table_.take_lifecycle();
    if (drain.empty())
        return false;
    const auto& local_adds = drain.adds;
    const auto& local_removes = drain.removes;

    for (const SourceTable::PendingAdd& pa : local_adds) {
        IPLSource src = source_manager.get_source(pa.handle); // Retains; null if already destroyed
        if (!src)
            continue;
        iplSourceAdd(src, simulator);
        iplSourceRelease(&src);
        _forget_source_inputs_assume_locked(access, pa.handle);
    }
    for (IPLSource src : local_removes) {
        if (!src)
            continue;
        iplSourceRemove(src, simulator);
    }
    source_table_.bump_inputs_generation(access);
    // One batched commit covers every add and remove that happened since the last worker tick.
    const uint64_t commit_us = _ipl_simulator_commit_assume_locked();
    if (commit_us_out)
        *commit_us_out = commit_us;
    // The commit dropped the simulator's references, so the final retain can go.
    for (IPLSource src : local_removes) {
        if (src) {
            IPLSource tmp = src;
            iplSourceRelease(&tmp);
        }
    }
    // Includes adds skipped because the source was already destroyed.
    for (const SourceTable::PendingAdd& pa : local_adds)
        source_table_.attached(pa.handle);
    for (int32_t handle : drain.removed_handles) {
        {
            std::lock_guard<std::recursive_mutex> cb_lock(_attenuation_callback_mutex);
            _source_attenuation_entries.erase(handle);
        }
        source_table_.forget_snapshot(access, handle);
        source_table_.clear_pass_outputs(handle);
        // Recycle last, once nothing keyed by this handle is left.
        source_manager.recycle_source_handle(handle);
    }
    // Initial inputs only after the add is committed.
    for (const SourceTable::PendingAdd& pa : local_adds) {
        IPLSource src = source_manager.get_source(pa.handle);
        if (!src)
            continue;
        _update_source_internal(access, src, pa.handle, pa.initial);
        iplSourceRelease(&src);
    }
    return true;
}

void ResonanceServer::_drain_pathing_probe_batch_releases() {
    for (IPLProbeBatch& b : pathing_probe_batches_pending_release_) {
        if (b)
            iplProbeBatchRelease(&b);
    }
    pathing_probe_batches_pending_release_.clear();
}

IPLSource ResonanceServer::get_source_from_handle(int32_t handle) {
    return source_manager.get_source(handle);
}

float ResonanceServer::calculate_distance_attenuation(Vector3 source_pos, Vector3 listener_pos, float min_dist, float max_dist) {
    if (!_ctx())
        return 1.0f;
    IPLDistanceAttenuationModel model{};
    model.type = IPL_DISTANCEATTENUATIONTYPE_INVERSEDISTANCE;
    model.minDistance = min_dist;
    IPLVector3 src = ResonanceUtils::to_ipl_vector3(source_pos);
    IPLVector3 dst = ResonanceUtils::to_ipl_vector3(listener_pos);
    return iplDistanceAttenuationCalculate(_ctx(), src, dst, &model);
}

Vector3 ResonanceServer::calculate_air_absorption(Vector3 source_pos, Vector3 listener_pos) {
    if (!_ctx())
        return Vector3(1, 1, 1);
    IPLAirAbsorptionModel model{};
    model.type = IPL_AIRABSORPTIONTYPE_DEFAULT;
    IPLVector3 src = ResonanceUtils::to_ipl_vector3(source_pos);
    IPLVector3 dst = ResonanceUtils::to_ipl_vector3(listener_pos);
    float air_abs[3] = {1.0f, 1.0f, 1.0f};
    iplAirAbsorptionCalculate(_ctx(), src, dst, &model, air_abs);
    return Vector3(air_abs[0], air_abs[1], air_abs[2]);
}

float ResonanceServer::calculate_directivity(Vector3 source_pos, Vector3 fwd, Vector3 up, Vector3 right, Vector3 listener_pos, float weight, float power) {
    if (!_ctx())
        return 1.0f;
    IPLDirectivity dSettings{};
    dSettings.dipoleWeight = weight;
    dSettings.dipolePower = power;
    IPLCoordinateSpace3 source_space{};
    source_space.origin = ResonanceUtils::to_ipl_vector3(source_pos);
    source_space.ahead = ResonanceUtils::to_ipl_vector3(fwd);
    source_space.up = ResonanceUtils::to_ipl_vector3(up);
    source_space.right = ResonanceUtils::to_ipl_vector3(right);
    IPLVector3 lst = ResonanceUtils::to_ipl_vector3(listener_pos);
    return iplDirectivityCalculate(_ctx(), source_space, lst, &dSettings);
}
