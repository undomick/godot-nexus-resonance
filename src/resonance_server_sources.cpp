#include "resonance_constants.h"
#include "resonance_log.h"
#include "resonance_math.h"
#include "resonance_server.h"
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

// baked_data_variation: -1 realtime, 0 REVERB, 1 STATICSOURCE, 2 STATICLISTENER
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

// IPL source handles: main-thread create/destroy queue real Add/Remove/Commit on the worker. Updates batch here and flush
// from ResonanceRuntime; try_update_source can bypass the queue when simulation_mutex is available.

int32_t ResonanceServer::create_source_handle(Vector3 pos, float radius) {
    if (!_ctx() || !simulator)
        return -1;
    IPLSourceSettings settings{};
    settings.flags = static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_DIRECT | IPL_SIMULATIONFLAGS_REFLECTIONS);
    if (pathing_enabled)
        settings.flags = static_cast<IPLSimulationFlags>(settings.flags | IPL_SIMULATIONFLAGS_PATHING);
    IPLSource src = nullptr;
    if (iplSourceCreate(simulator, &settings, &src) != IPL_STATUS_SUCCESS || !src) {
        ResonanceLog::error("ResonanceServer: iplSourceCreate failed (create_source_handle).");
        return -1;
    }
    // Main thread: register the IPLSource in SourceManager; iplSourceAdd runs later on the worker (see pending adds).
    const int32_t handle = source_manager.add_source(src);
    if (handle < 0) {
        iplSourceRelease(&src);
        return -1;
    }
    if (handle < kMaxCacheHandles) {
        // Clear caches for recycled handle IDs.
        for (int slot = 0; slot < kCacheSlots; slot++) {
            occlusion_cache_[static_cast<size_t>(slot)][static_cast<size_t>(handle)].epoch = 0;
            reverb_param_cache_[static_cast<size_t>(slot)][static_cast<size_t>(handle)].epoch = 0;
            reflection_param_cache_[static_cast<size_t>(slot)][static_cast<size_t>(handle)].epoch = 0;
            pathing_param_cache_[static_cast<size_t>(slot)][static_cast<size_t>(handle)].epoch = 0;
        }
        reflections_pending_[static_cast<size_t>(handle)].store(true, std::memory_order_release);
        // Default per-handle flags until first _update_source_internal runs on the worker.
        source_outputs_reflections_[static_cast<size_t>(handle)].store(1, std::memory_order_release);
        source_outputs_realtime_reflections_[static_cast<size_t>(handle)].store(0, std::memory_order_release);
        source_outputs_pathing_[static_cast<size_t>(handle)].store(pathing_enabled ? 1 : 0, std::memory_order_release);
        // Reset listener-probe override to "use global flag" for recycled handle IDs.
        _source_baked_reverb_listener_probe_override_[static_cast<size_t>(handle)].store(-1, std::memory_order_release);
        last_good_reflection_valid_[static_cast<size_t>(handle)].store(0, std::memory_order_relaxed);
        reflection_baked_energy_last_[static_cast<size_t>(handle)] = 0.0f;
    }
    {
        std::lock_guard<std::mutex> lock(pending_attach_handles_mutex_);
        pending_attach_handles_.insert(handle);
    }
    {
        PendingSourceAdd pa{};
        pa.handle = handle;
        pa.initial = _default_new_source_params();
        pa.initial.position = pos;
        pa.initial.radius = radius;
        std::lock_guard<std::mutex> lock(pending_source_lifecycle_mutex_);
        pending_source_adds_.push_back(pa);
    }
    iplSourceRelease(&src); // balance create retain; SourceManager still holds the source until destroy
    // Wake worker so pending SourceAdd is processed without waiting for the next sim interval.
    {
        std::lock_guard<std::mutex> lock(worker_mutex);
        simulation_requested = true;
    }
    worker_cv.notify_one();
    return handle;
}

bool ResonanceServer::_is_source_attach_pending(int32_t handle) const {
    if (handle < 0)
        return false;
    std::lock_guard<std::mutex> lock(pending_attach_handles_mutex_);
    return pending_attach_handles_.count(handle) != 0;
}

void ResonanceServer::ensure_fmod_reverb_source() {
    if (fmod_reverb_source_handle_ >= 0)
        return;
    fmod_reverb_source_handle_ = create_source_handle(Vector3(0, 0, 0), 1.0f);
}

void ResonanceServer::_destroy_source_handle_under_simulation_lock(int32_t handle) {
    IPLSource src = source_manager.get_source(handle);
    if (src) {
        if (simulator) {
            iplSourceRemove(src, simulator);
            iplSimulatorCommit(simulator); // apply staging (same as after Add)
        }
        iplSourceRelease(&src);
    }
    {
        std::lock_guard<std::recursive_mutex> cb_lock(_attenuation_callback_mutex);
        _source_attenuation_entries.erase(handle);
    }
    _source_update_snapshot_.erase(handle);
    realtime_reflection_log_once_handles_.erase(handle);
    if (handle >= 0 && handle < kMaxCacheHandles) {
        source_outputs_reflections_[static_cast<size_t>(handle)].store(0, std::memory_order_release);
        source_outputs_realtime_reflections_[static_cast<size_t>(handle)].store(0, std::memory_order_release);
        source_outputs_pathing_[static_cast<size_t>(handle)].store(0, std::memory_order_release);
        _source_baked_reverb_listener_probe_override_[static_cast<size_t>(handle)].store(-1, std::memory_order_release);
    }
    source_manager.remove_source(handle);
}

void ResonanceServer::destroy_source_handle(int32_t handle) {
    if (handle < 0 || is_shutting_down_flag.load(std::memory_order_acquire) || !_ctx())
        return;
    // Invalidate handle on this thread immediately; worker finishes Remove + Commit + final Release.
    IPLSource src = source_manager.get_source(handle); // retains
    source_manager.remove_source(handle);              // releases the map retain
    {
        std::lock_guard<std::mutex> lock(pending_attach_handles_mutex_);
        pending_attach_handles_.erase(handle);
    }
    if (src) {
        std::lock_guard<std::mutex> lock(pending_source_lifecycle_mutex_);
        pending_source_removes_.push_back(src);
        pending_source_post_remove_cleanup_.push_back(handle);
    }
    if (handle < kMaxCacheHandles) {
        for (int slot = 0; slot < kCacheSlots; slot++) {
            reverb_param_cache_[static_cast<size_t>(slot)][static_cast<size_t>(handle)].epoch = 0;
            reflection_param_cache_[static_cast<size_t>(slot)][static_cast<size_t>(handle)].epoch = 0;
            pathing_param_cache_[static_cast<size_t>(slot)][static_cast<size_t>(handle)].epoch = 0;
            occlusion_cache_[static_cast<size_t>(slot)][static_cast<size_t>(handle)].epoch = 0;
        }
        reflections_pending_[static_cast<size_t>(handle)].store(false, std::memory_order_release);
        source_outputs_reflections_[static_cast<size_t>(handle)].store(0, std::memory_order_release);
        source_outputs_realtime_reflections_[static_cast<size_t>(handle)].store(0, std::memory_order_release);
        source_outputs_pathing_[static_cast<size_t>(handle)].store(0, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> h_lock(reverb_params_likely_available_mutex_);
        reverb_params_likely_available_.erase(handle);
    }
    {
        std::lock_guard<std::mutex> b(source_update_batch_mutex_);
        source_update_batch_.erase(handle);
    }
    {
        std::lock_guard<std::mutex> lock(worker_mutex);
        simulation_requested = true;
    }
    worker_cv.notify_one();
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
    IPLSource src = source_manager.get_source(handle);
    if (!src)
        return false;
    _update_source_internal(src, handle, params);
    iplSourceRelease(&src);
    return true;
}

void ResonanceServer::enqueue_source_update(int32_t handle, const SourceUpdateParams& params) {
    if (handle < 0)
        return;
    std::lock_guard<std::mutex> lock(source_update_batch_mutex_);
    source_update_batch_[handle] = params;
}

int ResonanceServer::_apply_source_update_batch(const std::vector<std::pair<int32_t, SourceUpdateParams>>& batch) {
    int applied = 0;
    for (const auto& kv : batch) {
        IPLSource src = source_manager.get_source(kv.first);
        if (!src)
            continue;
        _update_source_internal(src, kv.first, kv.second);
        iplSourceRelease(&src);
        applied++;
    }
    return applied;
}

void ResonanceServer::_flush_pending_source_updates_assume_locked() {
    std::vector<std::pair<int32_t, SourceUpdateParams>> batch;
    {
        std::lock_guard<std::mutex> lock(source_update_batch_mutex_);
        if (source_update_batch_.empty())
            return;
        batch.reserve(source_update_batch_.size());
        for (const auto& kv : source_update_batch_)
            batch.push_back(kv);
        source_update_batch_.clear();
    }
    _apply_source_update_batch(batch);
}

void ResonanceServer::flush_pending_source_updates() {
    // If the worker holds simulation_mutex, merge back only handles not updated while flushing - never block main.
    std::vector<std::pair<int32_t, SourceUpdateParams>> batch;
    {
        std::lock_guard<std::mutex> lock(source_update_batch_mutex_);
        if (source_update_batch_.empty())
            return;
        batch.reserve(source_update_batch_.size());
        for (const auto& kv : source_update_batch_)
            batch.push_back(kv);
        source_update_batch_.clear();
    }
    std::unique_lock<std::mutex> sim_lock(simulation_mutex, std::defer_lock);
    if (!sim_lock.try_lock()) {
        std::lock_guard<std::mutex> lock(source_update_batch_mutex_);
        for (const auto& kv : batch) {
            if (source_update_batch_.find(kv.first) == source_update_batch_.end())
                source_update_batch_.emplace(kv.first, kv.second);
        }
        return;
    }
    _apply_source_update_batch(batch);
}

void ResonanceServer::set_source_attenuation_callback_data(int32_t handle, int attenuation_mode, float min_distance, float max_distance, const PackedFloat32Array& curve_samples) {
    if (handle < 0)
        return;
    if (!source_manager.has_handle(handle))
        return;
    std::lock_guard<std::recursive_mutex> lock(_attenuation_callback_mutex);
    auto& entry_ptr = _source_attenuation_entries[handle];
    if (!entry_ptr)
        entry_ptr = std::make_unique<AttenuationEntry>();
    if (!entry_ptr->data)
        entry_ptr->data = std::make_unique<AttenuationCallbackData>();
    AttenuationCallbackData& d = *entry_ptr->data;
    d.mode = attenuation_mode;
    d.min_distance = min_distance;
    d.max_distance = max_distance;
    const int64_t curve_sz = curve_samples.size();
    d.num_curve_samples = static_cast<int>((!curve_samples.is_empty() && curve_sz <= resonance::kAttenuationCurveSamples) ? curve_sz
                                                                                                                          : static_cast<int64_t>(resonance::kAttenuationCurveSamples));
    for (int i = 0; i < d.num_curve_samples && i < curve_samples.size(); i++) {
        d.curve_samples[i] = curve_samples[i];
    }
}

void ResonanceServer::set_source_baked_reverb_use_listener_probe_override(int32_t handle, int override_value) {
    if (handle < 0 || handle >= kMaxCacheHandles)
        return;
    // Encode tri-state as int8_t: -1 = use global flag, 0 = off, 1 = on. Lock-free so the player can flip it
    // from any thread (main during config refresh) without contending with the worker's simulation_mutex.
    int8_t encoded = (override_value < 0) ? -1 : ((override_value > 0) ? 1 : 0);
    _source_baked_reverb_listener_probe_override_[static_cast<size_t>(handle)].store(encoded, std::memory_order_release);
}

void ResonanceServer::clear_source_attenuation_callback_data(int32_t handle) {
    if (handle < 0 || !_ctx())
        return;
    std::lock_guard<std::mutex> sim_lock(simulation_mutex);
    std::lock_guard<std::recursive_mutex> cb_lock(_attenuation_callback_mutex);
    _source_attenuation_entries.erase(handle);
    IPLSource src = source_manager.get_source(handle);
    if (!src)
        return;
    auto snap_it = _source_update_snapshot_.find(handle);
    if (snap_it == _source_update_snapshot_.end() || !snap_it->second.valid) {
        iplSourceRelease(&src);
        return;
    }
    _update_source_internal(src, handle, snap_it->second.params);
    iplSourceRelease(&src);
}

// IPL callback: mode 1 = linear 1→0, mode 2 = interpolate user curve samples between min/max distance.
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
    return p;
}

void ResonanceServer::_maybe_apply_baked_reverb_listener_reflection_inputs(IPLSource src, int32_t handle, const IPLSimulationInputs& inputs,
                                                                           const SourceUpdateParams& params, IPLSimulationFlags sim_flags,
                                                                           bool enable_reflections) {
    bool use_listener_probe = baked_reverb_use_listener_probe;
    if (handle >= 0 && handle < kMaxCacheHandles) {
        const int8_t ov = _source_baked_reverb_listener_probe_override_[static_cast<size_t>(handle)].load(std::memory_order_acquire);
        if (ov >= 0)
            use_listener_probe = (ov != 0);
    }
    if (!use_listener_probe || params.baked_data_variation != 0 || !enable_reflections)
        return;
    if ((sim_flags & IPL_SIMULATIONFLAGS_REFLECTIONS) == 0)
        return;
    if (!pending_listener_valid.load(std::memory_order_acquire))
        return;

    const IPLCoordinateSpace3 listener_cs = _read_listener_coords_seqlock();
    IPLSimulationInputs reflections_inputs = inputs;
    reflections_inputs.flags = static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_REFLECTIONS);
    reflections_inputs.source.origin = listener_cs.origin;
    reflections_inputs.source.ahead = listener_cs.ahead;
    reflections_inputs.source.up = listener_cs.up;
    reflections_inputs.source.right = listener_cs.right;
    iplSourceSetInputs(src, IPL_SIMULATIONFLAGS_REFLECTIONS, &reflections_inputs);
}

void ResonanceServer::_update_source_internal(IPLSource src, int32_t handle, const SourceUpdateParams& params) {
    if (!src || !_ctx())
        return;

    SourceUpdateRecord& snap = _source_update_snapshot_[handle];
    snap.params = params;
    snap.valid = true;

    IPLSimulationInputs inputs{};
    const float dm = resonance::sanitize_audio_float(params.direct_mix_level);
    const float rm = resonance::sanitize_audio_float(params.reflections_mix_level);
    const float pm = resonance::sanitize_audio_float(params.pathing_mix_level);
    const bool any_mix = (dm > 0.0f) || (rm > 0.0f) || (pm > 0.0f);

    bool enable_reflections = (params.reflections_enabled_override == -1) ? true : (params.reflections_enabled_override != 0);
    enable_reflections = enable_reflections && (rm > 0.0f);
    // Drop realtime reflections beyond cap so IPL does not raycast far sources.
    if (enable_reflections && params.baked_data_variation == -1 && realtime_reflection_max_distance_m > 0.0f) {
        const Vector3 lip = ResonanceUtils::to_godot_vector3(get_current_listener_coords().origin);
        if (params.position.distance_to(lip) > static_cast<real_t>(realtime_reflection_max_distance_m))
            enable_reflections = false;
    }

    bool enable_pathing = (params.pathing_enabled_override == -1) ? pathing_enabled : (params.pathing_enabled_override != 0);
    enable_pathing = enable_pathing && (pm > 0.0f);

    IPLSimulationFlags sim_flags = static_cast<IPLSimulationFlags>(0);
    if (any_mix) {
        sim_flags = static_cast<IPLSimulationFlags>(IPL_SIMULATIONFLAGS_DIRECT);
        if (enable_reflections)
            sim_flags = static_cast<IPLSimulationFlags>(sim_flags | IPL_SIMULATIONFLAGS_REFLECTIONS);
    }

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

    fill_source_coordinate_space(inputs.source, params.position, params.source_forward, params.source_up);
    inputs.airAbsorptionModel.type = IPL_AIRABSORPTIONTYPE_DEFAULT;
    inputs.directivity.dipoleWeight = params.directivity_weight;
    inputs.directivity.dipolePower = params.directivity_power;
    inputs.directivity.callback = nullptr;
    inputs.directivity.userData = nullptr;

    // Callback userData must stay valid until iplSourceSetInputs; lookup under attenuation mutex.
    bool use_callback = false;
    AttenuationCallbackContext* callback_ctx = nullptr;
    {
        std::lock_guard<std::recursive_mutex> cb_lock(_attenuation_callback_mutex);
        auto it = _source_attenuation_entries.find(handle);
        if (it != _source_attenuation_entries.end() && it->second && it->second->data) {
            AttenuationCallbackData* pdata = it->second->data.get();
            if (pdata->mode == 1 || pdata->mode == 2) {
                use_callback = true;
                AttenuationEntry& entry = *it->second;
                entry.ctx.mutex = &_attenuation_callback_mutex;
                entry.ctx.data = pdata;
                callback_ctx = &entry.ctx;
            }
        }
    }
    if (params.use_sim_distance_attenuation) {
        if (use_callback && callback_ctx) {
            inputs.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_CALLBACK;
            inputs.distanceAttenuationModel.minDistance = callback_ctx->data->min_distance;
            inputs.distanceAttenuationModel.callback = distance_attenuation_callback;
            inputs.distanceAttenuationModel.userData = callback_ctx;
            inputs.distanceAttenuationModel.dirty = IPL_FALSE;
        } else {
            inputs.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_INVERSEDISTANCE;
            inputs.distanceAttenuationModel.minDistance = params.min_distance;
            inputs.distanceAttenuationModel.callback = nullptr;
            inputs.distanceAttenuationModel.userData = nullptr;
        }
    } else {
        inputs.distanceAttenuationModel.type = IPL_DISTANCEATTENUATIONTYPE_DEFAULT;
        inputs.distanceAttenuationModel.callback = nullptr;
        inputs.distanceAttenuationModel.userData = nullptr;
    }

    int eff_occlusion_type = occlusion_type;
    if (params.occlusion_type_override == 0 || params.occlusion_type_override == 1)
        eff_occlusion_type = params.occlusion_type_override;
    inputs.occlusionType = (eff_occlusion_type == 0) ? IPL_OCCLUSIONTYPE_RAYCAST : IPL_OCCLUSIONTYPE_VOLUMETRIC;
    inputs.occlusionRadius = params.radius;
    inputs.numOcclusionSamples = CLAMP(params.occlusion_samples, 1, simulation_settings.maxNumOcclusionSamples);
    inputs.numTransmissionRays = CLAMP(params.num_transmission_rays, 1, resonance::kMaxTransmissionRays);

    if (params.baked_data_variation == -1 && realtime_reflection_log_once_handles_.insert(handle).second) {
        const String src_msg = "Source " + String::num_int64(handle) + " first realtime reflections update (baked=FALSE). Rays: " + String::num_int64(max_rays);
        UtilityFunctions::print_rich("[color=cyan]Nexus Resonance:[/color] " + src_msg);
    }
    fill_baked_reflection_identifier(inputs, params.baked_data_variation, params.baked_endpoint_center, params.baked_endpoint_radius, reverb_influence_radius);

    inputs.reverbScale[0] = 1.0f;
    inputs.reverbScale[1] = 1.0f;
    inputs.reverbScale[2] = 1.0f;
    inputs.hybridReverbTransitionTime = hybrid_reverb_transition_time;
    inputs.hybridReverbOverlapPercent = hybrid_reverb_overlap_percent;

    // Retain pathing probe batch until after SetInputs; release queued on worker drain.
    IPLProbeBatch pathing_batch_retained = nullptr;
    if (enable_pathing && pathing_enabled) {
        IPLProbeBatch path_batch = _get_pathing_batch_for_source(params.pathing_probe_batch_handle);
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
    inputs.flags = sim_flags;

    if (handle >= 0 && handle < kMaxCacheHandles) {
        source_outputs_reflections_[static_cast<size_t>(handle)].store(enable_reflections ? 1 : 0, std::memory_order_release);
        source_outputs_realtime_reflections_[static_cast<size_t>(handle)].store(
            (enable_reflections && params.baked_data_variation == -1) ? 1 : 0, std::memory_order_release);
        source_outputs_pathing_[static_cast<size_t>(handle)].store(((sim_flags & IPL_SIMULATIONFLAGS_PATHING) != 0) ? 1 : 0,
                                                                   std::memory_order_release);
    }

    iplSourceSetInputs(src, sim_flags, &inputs);
    _maybe_apply_baked_reverb_listener_reflection_inputs(src, handle, inputs, params, sim_flags, enable_reflections);

    if (pathing_batch_retained)
        pathing_probe_batches_pending_release_.push_back(pathing_batch_retained);
}

void ResonanceServer::_drain_pending_source_lifecycle_assume_locked() {
    if (!_ctx() || !simulator)
        return;
    // Worker holds simulation_mutex: batch Add/Remove, single Commit, release removed retains, then SetInputs for new sources.

    std::vector<PendingSourceAdd> local_adds;
    std::vector<IPLSource> local_removes;
    std::vector<int32_t> local_post_remove;
    {
        std::lock_guard<std::mutex> lock(pending_source_lifecycle_mutex_);
        local_adds.swap(pending_source_adds_);
        local_removes.swap(pending_source_removes_);
        local_post_remove.swap(pending_source_post_remove_cleanup_);
    }
    if (local_adds.empty() && local_removes.empty())
        return;

    for (const PendingSourceAdd& pa : local_adds) {
        IPLSource src = source_manager.get_source(pa.handle); // retains; may be null if already destroyed
        if (!src)
            continue;
        iplSourceAdd(src, simulator);
        iplSourceRelease(&src);
    }
    for (IPLSource src : local_removes) {
        if (!src)
            continue;
        iplSourceRemove(src, simulator);
    }
    // One batched commit covers every add and remove that happened since the last worker tick.
    iplSimulatorCommit(simulator);
    // Now that the removed sources are no longer referenced by the simulator staging lists, drop the final retain.
    for (IPLSource src : local_removes) {
        if (src) {
            IPLSource tmp = src;
            iplSourceRelease(&tmp);
        }
    }
    // Mark attached handles ready for fetch/cache paths.
    if (!local_adds.empty()) {
        std::lock_guard<std::mutex> lock(pending_attach_handles_mutex_);
        for (const PendingSourceAdd& pa : local_adds)
            pending_attach_handles_.erase(pa.handle);
    }
    // Post-remove housekeeping (map entries keyed by handle that are worker-owned).
    for (int32_t handle : local_post_remove) {
        {
            std::lock_guard<std::recursive_mutex> cb_lock(_attenuation_callback_mutex);
            _source_attenuation_entries.erase(handle);
        }
        _source_update_snapshot_.erase(handle);
        realtime_reflection_log_once_handles_.erase(handle);
        if (handle >= 0 && handle < kMaxCacheHandles) {
            source_outputs_reflections_[static_cast<size_t>(handle)].store(0, std::memory_order_release);
            source_outputs_realtime_reflections_[static_cast<size_t>(handle)].store(0, std::memory_order_release);
            source_outputs_pathing_[static_cast<size_t>(handle)].store(0, std::memory_order_release);
            _source_baked_reverb_listener_probe_override_[static_cast<size_t>(handle)].store(-1, std::memory_order_release);
        }
    }
    // Apply initial inputs now that iplSourceAdd + Commit have run.
    for (const PendingSourceAdd& pa : local_adds) {
        IPLSource src = source_manager.get_source(pa.handle);
        if (!src)
            continue;
        _update_source_internal(src, pa.handle, pa.initial);
        iplSourceRelease(&src);
    }
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

// Thin IPL helpers for editor/UI distance/air/directivity preview (not used on the audio thread hot path).

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
