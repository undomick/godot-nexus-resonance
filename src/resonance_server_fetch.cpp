#include "resonance_constants.h"
#include "resonance_epoch.h"
#include "resonance_math.h"
#include "resonance_param_cache_invalidate_policy.h"
#include "resonance_pathing_deviation_policy.h"
#include "resonance_pathing_fetch_policy.h"
#include "resonance_pathing_inputs_policy.h"
#include "resonance_reflection_cache_publish_policy.h"
#include "resonance_reflection_fetch_policy.h"
#include "resonance_reflection_type_policy.h"
#include "resonance_server.h"
#include "resonance_transmission_fetch_policy.h"
#include "resonance_transmission_hit_policy.h"
#include "resonance_utils.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

using namespace godot;

// Audio-thread fetch of occlusion / reverb / pathing caches (lock-free double buffers) plus worker-side publish (_worker_sync_fetch_caches).

namespace {

void fill_hit0_custom_physics(ResonanceGodotPhysicsSceneBridge& bridge, const IPLVector3& listener_origin,
                              const IPLVector3& source_origin, float* out_hit0_lmh, bool* out_hit0_valid) {
    if (out_hit0_valid)
        *out_hit0_valid = false;
    if (!out_hit0_lmh)
        return;
    out_hit0_lmh[0] = out_hit0_lmh[1] = out_hit0_lmh[2] = 1.0f;

    const float dx = source_origin.x - listener_origin.x;
    const float dy = source_origin.y - listener_origin.y;
    const float dz = source_origin.z - listener_origin.z;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist < 1e-6f)
        return;

    IPLRay ray{};
    ray.origin = listener_origin;
    ray.direction = {dx / dist, dy / dist, dz / dist};

    // Match Steam DirectSimulator::transmission first segment (minDistance 0, maxDistance = LOS length).
    // Custom bridge applies the same start-t epsilon as occlusion.
    IPLHit hit{};
    ResonanceGodotPhysicsSceneBridge::closest_hit_callback(&ray, 0.0f, dist, &hit, bridge.user_data());
    if (!hit.material || !(hit.distance < std::numeric_limits<float>::infinity()))
        return;
    out_hit0_lmh[0] = hit.material->transmission[0];
    out_hit0_lmh[1] = hit.material->transmission[1];
    out_hit0_lmh[2] = hit.material->transmission[2];
    if (out_hit0_valid)
        *out_hit0_valid = true;
}

} // namespace

float ResonanceServer::_static_source_interpolated_baked_energy(const SourceUpdateParams& params, const Vector3& listener_pos) const {
    if (params.baked_data_variation != 1)
        return 0.0f;
    IPLContext ctx = _ctx();
    if (!ctx)
        return 0.0f;
    float best = 0.0f;
    probe_batch_registry_.for_each_probe_data([&](int32_t /*batch_handle*/, const Ref<ResonanceProbeData>& data) {
        if (!data.is_valid())
            return;
        const float e = baker.probe_data_static_source_interpolated_energy(
            ctx, data, params.baked_endpoint_center, params.baked_endpoint_radius, listener_pos,
            resonance::kStaticSourceProbeNeighborRadiusM, nullptr, nullptr);
        if (e > best)
            best = e;
    });
    return best;
}

bool ResonanceServer::_pathing_copy_sh_coeffs(std::array<float, kMaxPathingSHCoeffs>& dst, const float* src, int sh_count) {
    return resonance::pathing_fetch_copy_sh_coeffs(dst, src, sh_count);
}

OcclusionData ResonanceServer::get_source_occlusion_data(int32_t handle) {
    OcclusionData result;
    result.occlusion = resonance::kOcclusionFetchDefaultVisible;
    result.transmission[0] = 1.0f;
    result.transmission[1] = 1.0f;
    result.transmission[2] = 1.0f;
    result.air_absorption[0] = 1.0f;
    result.air_absorption[1] = 1.0f;
    result.air_absorption[2] = 1.0f;
    result.directivity = 1.0f;
    result.distance_attenuation = 1.0f;
    result.hit0_transmission[0] = 1.0f;
    result.hit0_transmission[1] = 1.0f;
    result.hit0_transmission[2] = 1.0f;
    result.hit0_valid = false;
    result.num_transmission_rays = 1;
    if (handle < 0 || !_ctx() || handle >= kMaxCacheHandles)
        return result;

    const int front = occlusion_cache_front_.load(std::memory_order_acquire);
    const uint32_t epoch = occlusion_cache_epoch_[front];
    const CachedOcclusionData& e = occlusion_cache_[static_cast<size_t>(front)][static_cast<size_t>(handle)];
    if (e.epoch == epoch)
        return e.data;
    return result;
}

float ResonanceServer::get_source_occlusion_linear_gain(int32_t handle) {
    const OcclusionData d = get_source_occlusion_data(handle);
    const float occlusion = std::clamp(d.occlusion, 0.0f, 1.0f);
    const float distance_atten = std::max(0.0f, d.distance_attenuation);
    const float tx_avg = (d.transmission[0] + d.transmission[1] + d.transmission[2]) / 3.0f;
    constexpr float kMinLinearGain = 0.0001f;
    return std::max(kMinLinearGain, (1.0f - occlusion) * tx_avg * distance_atten);
}

Dictionary ResonanceServer::get_source_occlusion_data_dict(int32_t handle) {
    const OcclusionData d = get_source_occlusion_data(handle);
    Dictionary out;
    out["occlusion"] = d.occlusion;
    PackedFloat32Array tx;
    tx.resize(3);
    tx.set(0, d.transmission[0]);
    tx.set(1, d.transmission[1]);
    tx.set(2, d.transmission[2]);
    out["transmission"] = tx;
    PackedFloat32Array air;
    air.resize(3);
    air.set(0, d.air_absorption[0]);
    air.set(1, d.air_absorption[1]);
    air.set(2, d.air_absorption[2]);
    out["air_absorption"] = air;
    out["directivity"] = d.directivity;
    out["distance_attenuation"] = d.distance_attenuation;
    PackedFloat32Array hit0;
    hit0.resize(3);
    hit0.set(0, d.hit0_transmission[0]);
    hit0.set(1, d.hit0_transmission[1]);
    hit0.set(2, d.hit0_transmission[2]);
    out["hit0_transmission"] = hit0;
    out["hit0_valid"] = d.hit0_valid;
    out["num_transmission_rays"] = d.num_transmission_rays;
    return out;
}

void ResonanceServer::update_source_position(int32_t handle, Vector3 position, float radius,
                                             bool use_sim_distance_attenuation, float min_distance) {
    SourceUpdateParams params = _default_new_source_params();
    params.position = position;
    params.radius = radius;
    params.use_sim_distance_attenuation = use_sim_distance_attenuation;
    params.min_distance = min_distance;
    update_source(handle, params);
}

void ResonanceServer::_clear_reverb_params_likely_available_hints() {
    std::lock_guard<std::mutex> lock(reverb_params_likely_available_mutex_);
    reverb_params_likely_available_.clear();
}

bool ResonanceServer::peek_reverb_params_likely_available(int32_t handle) const {
    if (handle < 0)
        return false;
    std::lock_guard<std::mutex> lock(reverb_params_likely_available_mutex_);
    auto it = reverb_params_likely_available_.find(handle);
    return it != reverb_params_likely_available_.end() && it->second;
}

bool ResonanceServer::_source_reflection_fetch_allowed(int32_t handle, bool reflections_have_run) const {
    if (_uses_parametric_or_hybrid() || reflection_type == resonance::kReflectionTan) {
        if (max_rays == 0 && !probe_batch_registry_.has_any_batches())
            return false;
    }
    if (_uses_parametric_or_hybrid() && !reflections_have_run)
        return false;
    if (_uses_parametric_or_hybrid() && reflections_pending_[static_cast<size_t>(handle)].load(std::memory_order_relaxed))
        return false;
    return true;
}

uint32_t ResonanceServer::get_reflection_param_cache_epoch() const {
    const int front = reflection_param_cache_front_.load(std::memory_order_acquire);
    return reflection_param_cache_epoch_[front];
}

uint32_t ResonanceServer::get_pathing_param_cache_epoch() const {
    const int front = pathing_param_cache_front_.load(std::memory_order_acquire);
    return pathing_param_cache_epoch_[front];
}

void ResonanceServer::_invalidate_pathing_cache_for_active_sources_assume_locked() {
    // Both slots: same rationale as _invalidate_param_caches_for_handles (no global epoch bump).
    std::vector<int32_t> handles;
    source_manager.get_all_handles(handles);
    for (int32_t h : handles) {
        if (h < 0 || h >= kMaxCacheHandles)
            continue;
        if (source_outputs_pathing_[static_cast<size_t>(h)].load(std::memory_order_relaxed) == 0)
            continue;
        const size_t idx = static_cast<size_t>(h);
        for (int slot = 0; slot < resonance::kParamCacheSlotCount; ++slot) {
            pathing_param_cache_[static_cast<size_t>(slot)][idx].order = -1;
            pathing_param_cache_[static_cast<size_t>(slot)][idx].epoch = 0;
        }
    }
}

bool ResonanceServer::fetch_reverb_params(int32_t handle, IPLReflectionEffectParams& out_params, bool* out_epoch_fresh) {
    if (handle < 0 || !_ctx() || handle >= kMaxCacheHandles)
        return false;
    if (_is_source_attach_pending(handle))
        return false;
    if (!_source_reflection_fetch_allowed(handle, reflections_have_run_once_.load(std::memory_order_acquire)))
        return false;

    bool result = false;
    bool epoch_fresh = false;
    if (reflection_type == resonance::kReflectionParametric) {
        const int front = reverb_param_cache_front_.load(std::memory_order_acquire);
        const uint32_t epoch = reverb_param_cache_epoch_[front];
        const CachedParametricReverb& e = reverb_param_cache_[static_cast<size_t>(front)][static_cast<size_t>(handle)];
        if (resonance::reflection_cache_entry_epoch_fresh(epoch, e.epoch)) {
            memset(&out_params, 0, sizeof(out_params));
            out_params.type = IPL_REFLECTIONEFFECTTYPE_PARAMETRIC;
            for (int i = 0; i < resonance::kReverbBandCount; i++) {
                out_params.reverbTimes[i] = resonance::clamp_reverb_time(e.reverbTimes[i]);
                out_params.eq[i] = resonance::sanitize_audio_float(e.eq[i]);
            }
            result = true;
            epoch_fresh = true;
            instrumentation_fetch_cache_hit.fetch_add(1, std::memory_order_relaxed);
        } else {
            if (source_outputs_reflections_[static_cast<size_t>(handle)].load(std::memory_order_relaxed) == 0) {
                instrumentation_fetch_cache_skip.fetch_add(1, std::memory_order_relaxed);
            } else {
                instrumentation_fetch_cache_miss.fetch_add(1, std::memory_order_relaxed);
            }
        }
    } else if (_uses_convolution_or_hybrid_or_tan()) {
        const int front = reflection_param_cache_front_.load(std::memory_order_acquire);
        const uint32_t epoch_front = reflection_param_cache_epoch_[front];
        const CachedReflectionParams& e_front = reflection_param_cache_[static_cast<size_t>(front)][static_cast<size_t>(handle)];

        auto copy_conv_entry = [&](const CachedReflectionParams& e) {
            out_params = e.params;
            if (reflection_type == resonance::kReflectionTan)
                out_params.tanDevice = _tan();
        };

        if (resonance::reflection_cache_entry_epoch_fresh(epoch_front, e_front.epoch)) {
            copy_conv_entry(e_front);
            result = true;
            epoch_fresh = true;
            instrumentation_fetch_cache_hit.fetch_add(1, std::memory_order_relaxed);
        } else if (resonance::reflection_stale_epoch_usable_for_mix(reflection_type, e_front.params)) {
            copy_conv_entry(e_front);
            result = true;
            instrumentation_fetch_refl_stale_epoch_fallback.fetch_add(1, std::memory_order_relaxed);
            instrumentation_fetch_cache_hit.fetch_add(1, std::memory_order_relaxed);
        } else if (source_outputs_reflections_[static_cast<size_t>(handle)].load(std::memory_order_relaxed) == 0) {
            instrumentation_fetch_cache_skip.fetch_add(1, std::memory_order_relaxed);
        } else {
            instrumentation_fetch_cache_miss.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (out_epoch_fresh)
        *out_epoch_fresh = epoch_fresh;
    return result;
}

bool ResonanceServer::fetch_pathing_params(int32_t handle, FetchedPathingParams& out) {
    if (handle < 0 || !_ctx() || !pathing_enabled || handle >= kMaxCacheHandles) {
        instrumentation_pathing_fetch_early_exit.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (source_outputs_pathing_[static_cast<size_t>(handle)].load(std::memory_order_relaxed) == 0) {
        instrumentation_pathing_fetch_early_exit.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (_is_source_attach_pending(handle)) {
        instrumentation_pathing_fetch_early_exit.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    bool result = false;
    const int front = pathing_param_cache_front_.load(std::memory_order_acquire);
    const uint32_t epoch = pathing_param_cache_epoch_[front];
    const CachedPathingParams& e = pathing_param_cache_[static_cast<size_t>(front)][static_cast<size_t>(handle)];
    if (e.epoch == epoch && e.order >= 0) {
        const int sh_count = resonance::pathing_sh_coeff_count(e.order);
        if (!resonance::pathing_fetch_copy_sh_coeffs(out.shCoeffs, e.shCoeffs.data(), sh_count)) {
            instrumentation_pathing_fetch_cache_miss.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        memset(&out.params, 0, sizeof(out.params));
        for (int i = 0; i < resonance::kReverbBandCount; i++)
            out.params.eqCoeffs[i] = e.eqCoeffs[i];
        out.params.shCoeffs = out.shCoeffs.data();
        out.params.order = e.order;
        out.params.binaural = pathing_binaural ? IPL_TRUE : IPL_FALSE;
        out.params.hrtf = _hrtf();
        out.params.normalizeEQ = pathing_normalize_eq ? IPL_TRUE : IPL_FALSE;
        result = true;
        instrumentation_pathing_fetch_cache_hit.fetch_add(1, std::memory_order_relaxed);
    } else {
        instrumentation_pathing_fetch_cache_miss.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

uint64_t ResonanceServer::_worker_fetch_occlusion_into_back(IPLSource src, int32_t handle, int occ_back) {
    const auto t0 = std::chrono::steady_clock::now();
    IPLSimulationOutputs direct_out{};
    iplSourceGetOutputs(src, IPL_SIMULATIONFLAGS_DIRECT, &direct_out);
    CachedOcclusionData cd{};
    cd.data.occlusion = direct_out.direct.occlusion;
    cd.data.transmission[0] = direct_out.direct.transmission[0];
    cd.data.transmission[1] = direct_out.direct.transmission[1];
    cd.data.transmission[2] = direct_out.direct.transmission[2];

    int num_tx_rays = resonance::kDefaultTransmissionRays;
    IPLVector3 source_origin = {0, 0, 0};
    bool have_source_pose = false;
    {
        const auto snap_it = _source_update_snapshot_.find(handle);
        if (snap_it != _source_update_snapshot_.end() && snap_it->second.valid) {
            num_tx_rays = snap_it->second.params.num_transmission_rays;
            source_origin = ResonanceUtils::to_ipl_vector3(snap_it->second.params.position);
            have_source_pose = true;
        }
    }
    cd.data.num_transmission_rays = num_tx_rays;

    // Hit0: Custom = independent physics closestHit; Embree/Default with rays==1 = Steam single closestHit T.
    cd.data.hit0_valid = false;
    cd.data.hit0_transmission[0] = cd.data.hit0_transmission[1] = cd.data.hit0_transmission[2] = 1.0f;
    if (_scene_type() == IPL_SCENETYPE_CUSTOM && have_source_pose && godot_physics_bridge_.has_valid_world()) {
        const IPLCoordinateSpace3 listener_cs = _read_listener_coords_seqlock();
        fill_hit0_custom_physics(godot_physics_bridge_, listener_cs.origin, source_origin, cd.data.hit0_transmission,
                                 &cd.data.hit0_valid);
    } else {
        resonance::fill_hit0_from_single_transmission_ray(num_tx_rays, cd.data.transmission, cd.data.hit0_transmission,
                                                          &cd.data.hit0_valid);
    }

    resonance::clear_transmission_on_line_of_sight(cd.data.occlusion, cd.data.transmission);
    cd.data.air_absorption[0] = direct_out.direct.airAbsorption[0];
    cd.data.air_absorption[1] = direct_out.direct.airAbsorption[1];
    cd.data.air_absorption[2] = direct_out.direct.airAbsorption[2];
    cd.data.directivity = direct_out.direct.directivity;
    cd.data.distance_attenuation = direct_out.direct.distanceAttenuation;
    cd.epoch = occlusion_cache_epoch_[occ_back];
    occlusion_cache_[static_cast<size_t>(occ_back)][static_cast<size_t>(handle)] = std::move(cd);
    const auto t1 = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
}

bool ResonanceServer::_worker_fetch_reflection_into_back(IPLSource src, int32_t handle, int reverb_back, int refl_back,
                                                         bool reflections_have_run, bool sync_after_run_reflections,
                                                         uint64_t& out_microseconds) {
    out_microseconds = 0;
    if (!_source_reflection_fetch_allowed(handle, reflections_have_run))
        return false;
    if (source_outputs_reflections_[static_cast<size_t>(handle)].load(std::memory_order_relaxed) == 0)
        return false;

    const auto t0 = std::chrono::steady_clock::now();
    IPLSimulationOutputs outputs{};
    if (_uses_parametric_or_hybrid())
        outputs.reflections.type = IPL_REFLECTIONEFFECTTYPE_PARAMETRIC;
    iplSourceGetOutputs(src, IPL_SIMULATIONFLAGS_REFLECTIONS, &outputs);

    bool has_convolution = (outputs.reflections.ir != nullptr);
    const bool last_good_valid = handle >= 0 && handle < kMaxCacheHandles &&
                                 last_good_reflection_valid_[static_cast<size_t>(handle)].load(std::memory_order_relaxed) != 0;
    const bool use_last_good_conv =
        resonance::reflection_worker_use_last_good_conv(has_convolution, reflection_type, last_good_valid,
                                                        sync_after_run_reflections);
    bool has_parametric = (outputs.reflections.reverbTimes[0] > 0 || outputs.reflections.reverbTimes[1] > 0 ||
                           outputs.reflections.reverbTimes[2] > 0);
    bool has_hybrid = (reflection_type == resonance::kReflectionHybrid &&
                       (has_convolution || outputs.reflections.reverbTimes[0] > 0));
    bool has_tan = (reflection_type == resonance::kReflectionTan && outputs.reflections.tanSlot >= 0 && _tan());

    const bool usable = has_convolution || use_last_good_conv ||
                        (reflection_type == resonance::kReflectionParametric && has_parametric) || has_hybrid || has_tan;
    if (!usable) {
        const auto t1 = std::chrono::steady_clock::now();
        out_microseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        return false;
    }

    IPLReflectionEffectParams out_params = outputs.reflections;
    bool conv_ir_from_last_good = false;
    if (use_last_good_conv) {
        out_params = last_good_reflection_params_[static_cast<size_t>(handle)];
        has_convolution = (out_params.ir != nullptr);
        conv_ir_from_last_good = has_convolution;
    }
    for (int i = 0; i < resonance::kReverbBandCount; i++) {
        out_params.reverbTimes[i] = resonance::clamp_reverb_time(out_params.reverbTimes[i]);
        out_params.eq[i] = resonance::sanitize_audio_float(out_params.eq[i]);
    }
    out_params.delay = resonance::sanitize_delay_samples(out_params.delay);

    if (has_convolution && out_params.ir != nullptr) {
        const bool ir_out_of_range = out_params.irSize <= 0 || out_params.irSize > resonance::kConvolutionIrSamplesHardMax ||
                                     out_params.numChannels <= 0 ||
                                     out_params.numChannels > resonance::kReflectionIrChannelsHardMax;
        if (ir_out_of_range) {
            if (!sync_after_run_reflections && last_good_valid) {
                out_params = last_good_reflection_params_[static_cast<size_t>(handle)];
                has_convolution = (out_params.ir != nullptr);
                conv_ir_from_last_good = has_convolution;
            } else {
                out_params.ir = nullptr;
                has_convolution = false;
                has_hybrid = (reflection_type == resonance::kReflectionHybrid && outputs.reflections.reverbTimes[0] > 0);
            }
        }
    }

    const bool hybrid_conv_and_param = (reflection_type == resonance::kReflectionHybrid && has_convolution && has_parametric);
    out_params.type = resonance::reflection_effect_type_for_mode(reflection_type, hybrid_conv_and_param);
    if (reflection_type == resonance::kReflectionTan)
        out_params.tanDevice = _tan();

    if (handle >= 0 && handle < kMaxCacheHandles) {
        const IPLCoordinateSpace3 listener_cs = _read_listener_coords_seqlock();
        const Vector3 listener_pos = ResonanceUtils::to_godot_vector3(listener_cs.origin);
        const auto snap_it = _source_update_snapshot_.find(handle);
        if (snap_it != _source_update_snapshot_.end() && snap_it->second.valid &&
            snap_it->second.params.baked_data_variation == 1) {
            reflection_baked_energy_last_[static_cast<size_t>(handle)] =
                _static_source_interpolated_baked_energy(snap_it->second.params, listener_pos);
        }
    }

    if (reflection_type == resonance::kReflectionParametric && has_parametric) {
        CachedParametricReverb cr{};
        for (int i = 0; i < resonance::kReverbBandCount; i++) {
            cr.reverbTimes[i] = resonance::clamp_reverb_time(outputs.reflections.reverbTimes[i]);
            cr.eq[i] = outputs.reflections.eq[i];
        }
        cr.epoch = reverb_param_cache_epoch_[reverb_back];
        reverb_param_cache_[static_cast<size_t>(reverb_back)][static_cast<size_t>(handle)] = std::move(cr);
    }
    if (_uses_convolution_or_hybrid_or_tan() && (has_convolution || has_hybrid || has_tan)) {
        CachedReflectionParams rp{};
        rp.params = out_params;
        rp.epoch = reflection_param_cache_epoch_[refl_back];
        reflection_param_cache_[static_cast<size_t>(refl_back)][static_cast<size_t>(handle)] = std::move(rp);
        if (has_convolution && out_params.ir != nullptr && !conv_ir_from_last_good && handle >= 0 && handle < kMaxCacheHandles) {
            last_good_reflection_params_[static_cast<size_t>(handle)] = out_params;
            last_good_reflection_valid_[static_cast<size_t>(handle)].store(1, std::memory_order_relaxed);
        }
    }

    if (handle >= 0 && handle < kMaxCacheHandles)
        reflections_pending_[static_cast<size_t>(handle)].store(false, std::memory_order_release);

    const auto t1 = std::chrono::steady_clock::now();
    out_microseconds = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    return true;
}

void ResonanceServer::_worker_clear_reflections_pending_after_run_assume_locked(bool ran_reflections) {
    if (!ran_reflections)
        return;

    std::vector<int32_t> handles;
    source_manager.get_all_handles(handles);
    for (int32_t handle : handles) {
        if (handle < 0 || handle >= kMaxCacheHandles)
            continue;
        const bool wants_refl =
            source_outputs_reflections_[static_cast<size_t>(handle)].load(std::memory_order_relaxed) != 0;
        const bool attach_pending = _is_source_attach_pending(handle);
        if (!resonance::reflection_pending_should_clear_after_run(ran_reflections, wants_refl, attach_pending))
            continue;
        reflections_pending_[static_cast<size_t>(handle)].store(false, std::memory_order_release);
    }
}

void ResonanceServer::_worker_sync_fetch_caches(bool refresh_direct_outputs, bool refresh_reflection_outputs) {
    if (!_ctx() || !simulator)
        return;

    uint64_t us_occ = 0;
    uint64_t us_refl = 0;
    uint64_t us_path = 0;

    std::vector<int32_t> handles;
    source_manager.get_all_handles(handles);

    const int occ_back = 1 - occlusion_cache_front_.load(std::memory_order_acquire);
    const int reverb_back = 1 - reverb_param_cache_front_.load(std::memory_order_acquire);
    const int refl_back = 1 - reflection_param_cache_front_.load(std::memory_order_acquire);
    const int path_back = 1 - pathing_param_cache_front_.load(std::memory_order_acquire);

    const bool pathing_refresh = pathing_enabled && pathing_ran_this_tick.load(std::memory_order_acquire);
    const bool reflections_have_run = reflections_have_run_once_.load(std::memory_order_acquire);

    // Bump epoch before fill so audio thread can detect stale slots after front flip.
    if (refresh_direct_outputs)
        resonance::bump_slot_epoch(occlusion_cache_epoch_[occ_back]);
    if (refresh_reflection_outputs && reflection_type == resonance::kReflectionParametric)
        resonance::bump_slot_epoch(reverb_param_cache_epoch_[reverb_back]);
    if (refresh_reflection_outputs && _uses_convolution_or_hybrid_or_tan())
        resonance::bump_slot_epoch(reflection_param_cache_epoch_[refl_back]);
    if (pathing_refresh)
        resonance::bump_slot_epoch(pathing_param_cache_epoch_[path_back]);

    std::vector<std::pair<int32_t, bool>> reverb_hint_batch;
    if (refresh_reflection_outputs)
        reverb_hint_batch.reserve(handles.size());

    // Skip sources still pending attach to avoid fetch during lifecycle transition.
    for (int32_t handle : handles) {
        if (handle < 0 || handle >= kMaxCacheHandles)
            continue;
        if (_is_source_attach_pending(handle))
            continue;
        IPLSource src = source_manager.get_source(handle);
        if (!src)
            continue;

        if (refresh_direct_outputs)
            us_occ += _worker_fetch_occlusion_into_back(src, handle, occ_back);

        if (refresh_reflection_outputs) {
            uint64_t us_one = 0;
            const bool refl_hint = _worker_fetch_reflection_into_back(src, handle, reverb_back, refl_back, reflections_have_run,
                                                                      refresh_reflection_outputs, us_one);
            us_refl += us_one;
            reverb_hint_batch.emplace_back(handle, refl_hint);
        }

        if (pathing_refresh) {
            if (source_outputs_pathing_[static_cast<size_t>(handle)].load(std::memory_order_relaxed) == 0) {
                iplSourceRelease(&src);
                continue;
            }
            const auto t0 = std::chrono::steady_clock::now();
            IPLSimulationOutputs pout{};
            iplSourceGetOutputs(src, IPL_SIMULATIONFLAGS_PATHING, &pout);
            if (pout.pathing.shCoeffs != nullptr) {
                // Order from ambisonic_order; pout.pathing.order is never written by Steam Audio.
                const int order = resonance::pathing_apply_order(ambisonic_order);
                const int sh_count = resonance::pathing_sh_coeff_count(order);
                CachedPathingParams pm{};
                pm.eqCoeffs[0] = pout.pathing.eqCoeffs[0];
                pm.eqCoeffs[1] = pout.pathing.eqCoeffs[1];
                pm.eqCoeffs[2] = pout.pathing.eqCoeffs[2];
                if (_pathing_copy_sh_coeffs(pm.shCoeffs, pout.pathing.shCoeffs, sh_count)) {
                    pm.order = order;
                    pm.epoch = pathing_param_cache_epoch_[path_back];
                    pathing_param_cache_[static_cast<size_t>(path_back)][static_cast<size_t>(handle)] = std::move(pm);
                    instrumentation_pathing_fetch_sh_ok.fetch_add(1, std::memory_order_relaxed);
                } else {
                    instrumentation_pathing_fetch_sh_bad_order.fetch_add(1, std::memory_order_relaxed);
                }
            }
            const auto t1 = std::chrono::steady_clock::now();
            us_path += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        }

        iplSourceRelease(&src);
    }

    if (!reverb_hint_batch.empty()) {
        std::lock_guard<std::mutex> h_lock(reverb_params_likely_available_mutex_);
        for (const auto& kv : reverb_hint_batch)
            reverb_params_likely_available_[kv.first] = kv.second;
    }

    // Publish back buffer only after all per-handle fetches for this tick complete.
    if (refresh_direct_outputs)
        occlusion_cache_front_.store(occ_back, std::memory_order_release);
    if (refresh_reflection_outputs && reflection_type == resonance::kReflectionParametric)
        reverb_param_cache_front_.store(reverb_back, std::memory_order_release);
    if (refresh_reflection_outputs && _uses_convolution_or_hybrid_or_tan())
        reflection_param_cache_front_.store(refl_back, std::memory_order_release);
    if (pathing_refresh)
        pathing_param_cache_front_.store(path_back, std::memory_order_release);

    instrumentation_worker_us_sync_fetch_occlusion.store(us_occ, std::memory_order_relaxed);
    instrumentation_worker_us_sync_fetch_reflections.store(us_refl, std::memory_order_relaxed);
    instrumentation_worker_us_sync_fetch_pathing.store(us_path, std::memory_order_relaxed);
}

void ResonanceServer::set_pathing_deviation_callback(IPLDeviationCallback callback, void* userData) {
    std::lock_guard<std::mutex> sim_lock(simulation_mutex);
    std::lock_guard<std::mutex> lock(_pathing_deviation_mutex);
    _pathing_deviation_godot_callable = Callable();
    _pathing_deviation_lut.reset();
    if (callback) {
        _pathing_deviation_model.type = IPL_DEVIATIONTYPE_CALLBACK;
        _pathing_deviation_model.callback = callback;
        _pathing_deviation_model.userData = userData;
        _pathing_deviation_callback_enabled = true;
    } else {
        _pathing_deviation_model.type = IPL_DEVIATIONTYPE_DEFAULT;
        _pathing_deviation_model.callback = nullptr;
        _pathing_deviation_model.userData = nullptr;
        _pathing_deviation_callback_enabled = false;
    }
}

const float* ResonanceServer::PathingDeviationLut::band_ptr(int band) const {
    switch (resonance::pathing_deviation_band_clamped(band)) {
    case 0:
        return band0.empty() ? nullptr : band0.data();
    case 1:
        return band1.empty() ? nullptr : band1.data();
    default:
        return band2.empty() ? nullptr : band2.data();
    }
}

float IPLCALL ResonanceServer::_pathing_deviation_lut_ipl_callback(IPLfloat32 angle, IPLint32 band, void* userData) {
    if (ResonanceServer::is_shutting_down_flag.load(std::memory_order_acquire) || ResonanceServer::ipl_audio_teardown_active())
        return 1.0f;
    ResonanceServer* server = static_cast<ResonanceServer*>(userData);
    if (!server)
        return 1.0f;
    std::shared_ptr<const PathingDeviationLut> lut;
    {
        std::lock_guard<std::mutex> lock(server->_pathing_deviation_mutex);
        lut = server->_pathing_deviation_lut;
    }
    if (!lut)
        return 1.0f;
    const float* samples = lut->band_ptr(band);
    return resonance::pathing_deviation_lut_lookup(samples, lut->samples_per_band, angle);
}

std::shared_ptr<ResonanceServer::PathingDeviationLut> ResonanceServer::_pathing_deviation_lut_from_callable(const Callable& callable,
                                                                                                            int samples_per_band) {
    const int samples = resonance::pathing_deviation_lut_samples_clamped(samples_per_band);
    auto lut = std::make_shared<ResonanceServer::PathingDeviationLut>();
    lut->samples_per_band = samples;
    lut->band0.assign(static_cast<size_t>(samples), 1.0f);
    lut->band1.assign(static_cast<size_t>(samples), 1.0f);
    lut->band2.assign(static_cast<size_t>(samples), 1.0f);
    if (!callable.is_valid())
        return lut;
    for (int band = 0; band < resonance::kPathingDeviationNumBands; ++band) {
        std::vector<float>* target = nullptr;
        switch (band) {
        case 0:
            target = &lut->band0;
            break;
        case 1:
            target = &lut->band1;
            break;
        default:
            target = &lut->band2;
            break;
        }
        for (int i = 0; i < samples; ++i) {
            constexpr float kPi = 3.14159265358979323846f;
            const float angle = (samples <= 1) ? 0.0f : (static_cast<float>(i) / static_cast<float>(samples - 1)) * kPi;
            const Variant ret = callable.call(angle, band);
            float v = 1.0f;
            if (ret.get_type() == Variant::FLOAT || ret.get_type() == Variant::INT)
                v = static_cast<float>(ret);
            (*target)[static_cast<size_t>(i)] = std::clamp(v, 0.0f, 1.0f);
        }
    }
    return lut;
}

void ResonanceServer::set_pathing_deviation_callable(const Callable& callable, int samples_per_band) {
    const int samples = resonance::pathing_deviation_lut_samples_clamped(samples_per_band);
    if (!callable.is_valid()) {
        std::lock_guard<std::mutex> sim_lock(simulation_mutex);
        std::lock_guard<std::mutex> lock(_pathing_deviation_mutex);
        _pathing_deviation_lut_samples = samples;
        _pathing_deviation_godot_callable = Callable();
        _pathing_deviation_lut.reset();
        _pathing_deviation_model.type = IPL_DEVIATIONTYPE_DEFAULT;
        _pathing_deviation_model.callback = nullptr;
        _pathing_deviation_model.userData = nullptr;
        _pathing_deviation_callback_enabled = false;
        return;
    }

    Callable callable_copy = callable;
    {
        std::lock_guard<std::mutex> lock(_pathing_deviation_mutex);
        _pathing_deviation_lut_samples = samples;
        _pathing_deviation_godot_callable = callable;
    }

    std::shared_ptr<PathingDeviationLut> lut = _pathing_deviation_lut_from_callable(callable_copy, samples);

    std::lock_guard<std::mutex> sim_lock(simulation_mutex);
    std::lock_guard<std::mutex> lock(_pathing_deviation_mutex);
    if (!_pathing_deviation_godot_callable.is_valid() || _pathing_deviation_godot_callable != callable_copy)
        return;
    _pathing_deviation_lut = lut;
    _pathing_deviation_model.type = IPL_DEVIATIONTYPE_CALLBACK;
    _pathing_deviation_model.callback = _pathing_deviation_lut_ipl_callback;
    _pathing_deviation_model.userData = this;
    _pathing_deviation_callback_enabled = true;
}

bool ResonanceServer::is_pathing_deviation_custom_enabled() {
    std::lock_guard<std::mutex> lock(_pathing_deviation_mutex);
    return _pathing_deviation_callback_enabled;
}

void ResonanceServer::clear_pathing_deviation_callback() {
    set_pathing_deviation_callable(Callable());
}
