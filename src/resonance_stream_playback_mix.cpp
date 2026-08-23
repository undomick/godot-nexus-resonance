#include "resonance_constants.h"
#include "resonance_log.h"
#include "resonance_math.h"
#include "resonance_pathing_inputs_policy.h"
#include "resonance_playback_fade.h"
#include "resonance_player.h"
#include "resonance_probe_volume.h"
#include "resonance_server.h"
#include "resonance_utils.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/classes/audio_stream_player.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector4.hpp>
#include <limits>
#include <sstream>

using namespace godot;

// ResonanceStreamPlayback mix path: Steam blocks, rings, EOS/tails, instrumentation readouts.

namespace {

/// Godot does not apply AudioStreamPlayer3D volume to GDExtension AudioStreamPlayback::_mix buffers.
/// Source loudness = min(volume_db, max_db) applied to the dry input before Steam DSP.
float owner_effective_volume_linear(const ResonancePlayer* owner) {
    if (!owner)
        return 1.0f;
    return resonance::sanitize_audio_float(owner->get_effective_volume_linear_cached());
}

bool ipl_all_channel_ptrs_ok(const IPLAudioBuffer& b, int nch) {
    if (!b.data || b.numChannels < nch)
        return false;
    for (int c = 0; c < nch; ++c) {
        if (!b.data[c])
            return false;
    }
    return true;
}

/// Fold one sample instant to stereo (IPL channel order for quad / 5.1 / 7.1).
void downmix_multichannel_sample(const float* const* d, int nch, int sample_idx, float& out_l, float& out_r) {
    constexpr float k = 0.70710678f;
    switch (nch) {
    case 1:
        out_l = out_r = d[0][sample_idx];
        break;
    case 2:
        out_l = d[0][sample_idx];
        out_r = d[1][sample_idx];
        break;
    case 4:
        out_l = d[0][sample_idx] + k * d[2][sample_idx];
        out_r = d[1][sample_idx] + k * d[3][sample_idx];
        break;
    case 6: // FL, FR, C, LFE, RL, RR
        out_l = d[0][sample_idx] + k * d[2][sample_idx] + k * d[4][sample_idx];
        out_r = d[1][sample_idx] + k * d[2][sample_idx] + k * d[5][sample_idx];
        break;
    case 8: // FL, FR, C, LFE, BL, BR, SL, SR
        out_l = d[0][sample_idx] + k * d[2][sample_idx] + k * d[4][sample_idx] + k * d[6][sample_idx];
        out_r = d[1][sample_idx] + k * d[2][sample_idx] + k * d[5][sample_idx] + k * d[7][sample_idx];
        break;
    default:
        out_l = out_r = 0.0f;
    }
}
} // namespace

void ResonanceStreamPlayback::_sync_params() {
    if (params_dirty.load(std::memory_order_acquire)) {
        instrumentation_param_sync_count.fetch_add(1, std::memory_order_relaxed);
        params_current = params_next;
        params_dirty.store(false, std::memory_order_release);

        current_source_handle = params_current.source_handle;
    }
}

void ResonanceStreamPlayback::_cleanup_steam_audio() {
    if (ResonanceServer* reg_srv = ResonanceServer::get_singleton())
        reg_srv->unregister_ipl_context_client(this);

    _release_retained_source();

    direct_processor.cleanup();
    reflection_processor.cleanup();
    path_processor.cleanup();
    mixer_processor.cleanup();

    if (context) {
        if (sa_in_buffer.data)
            iplAudioBufferFree(context, &sa_in_buffer);
        if (sa_direct_out_buffer.data)
            iplAudioBufferFree(context, &sa_direct_out_buffer);
        if (sa_path_out_buffer.data)
            iplAudioBufferFree(context, &sa_path_out_buffer);
        if (sa_final_mix_buffer.data)
            iplAudioBufferFree(context, &sa_final_mix_buffer);
    }

    // IMPORTANT: Reset structs to 0 to prevent double-free or invalid access
    memset(&sa_in_buffer, 0, sizeof(IPLAudioBuffer));
    memset(&sa_direct_out_buffer, 0, sizeof(IPLAudioBuffer));
    memset(&sa_path_out_buffer, 0, sizeof(IPLAudioBuffer));
    memset(&sa_final_mix_buffer, 0, sizeof(IPLAudioBuffer));

    is_initialized = false;
    steam_context_stale_.store(false, std::memory_order_release);
    direct_out_channels_ = 2;

    input_ring_l.clear();
    input_ring_r.clear();
    output_ring_l.clear();
    output_ring_r.clear();
    output_ring_reverb_l.clear();
    output_ring_reverb_r.clear();

    prev_direct_weight = 0.0f;
    prev_conv_reflections_mix_level_ = -1.0f;
    prev_parametric_reflections_mix_level_ = 0.0f;
    prev_pathing_mix_level_ = 0.0f;
    input_started = false;
    params_ever_synced_.store(false, std::memory_order_release);
}

void ResonanceStreamPlayback::_lazy_init_steam_audio(int ignored_rate) {
    (void)ignored_rate; // Sample rate comes from ResonanceServer::get_sample_rate(); param kept for API consistency.
    // Main-thread only (via prewarm_steam_audio). Do not call from _mix.
    if (is_initialized)
        return;
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!srv || !srv->is_initialized())
        return;

    current_sample_rate = srv->get_sample_rate();
    frame_size_ = srv->get_audio_frame_size();
    context = srv->get_context_handle();
    int order = srv->get_ambisonic_order();
    int refl_type = srv->get_reflection_type();
    direct_out_channels_ = srv->get_direct_speaker_channels();
    if (direct_out_channels_ < 1)
        direct_out_channels_ = 2;

    temp_process_buffer_l.resize(frame_size_);
    temp_process_buffer_r.resize(frame_size_);

    direct_processor.initialize(context, current_sample_rate, frame_size_, order, true, direct_out_channels_);
    reflection_processor.initialize(context, current_sample_rate, frame_size_, order, refl_type, srv->get_max_reverb_duration(),
                                    srv->get_convolution_ir_max_samples());
    path_processor.initialize(context, current_sample_rate, frame_size_, order);
    mixer_processor.initialize(context, current_sample_rate, frame_size_, order);

    // Direct path matches server speaker layout; path processor stays stereo.
    if (iplAudioBufferAllocate(context, 2, frame_size_, &sa_in_buffer) != IPL_STATUS_SUCCESS ||
        iplAudioBufferAllocate(context, direct_out_channels_, frame_size_, &sa_direct_out_buffer) != IPL_STATUS_SUCCESS ||
        iplAudioBufferAllocate(context, 2, frame_size_, &sa_path_out_buffer) != IPL_STATUS_SUCCESS ||
        iplAudioBufferAllocate(context, direct_out_channels_, frame_size_, &sa_final_mix_buffer) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonancePlayer: Playback Init Failed: Buffer allocation failed (IPLerror).");
        _cleanup_steam_audio();
        return;
    }
    if (!ipl_all_channel_ptrs_ok(sa_in_buffer, 2) || !ipl_all_channel_ptrs_ok(sa_direct_out_buffer, direct_out_channels_) ||
        !ipl_all_channel_ptrs_ok(sa_path_out_buffer, 2) || !ipl_all_channel_ptrs_ok(sa_final_mix_buffer, direct_out_channels_)) {
        ResonanceLog::error("ResonancePlayer: Playback Init Failed: Buffer allocation returned null.");
        _cleanup_steam_audio();
        return;
    }

    is_initialized = true;
    if (ResonanceServer* reg_srv = ResonanceServer::get_singleton())
        reg_srv->register_ipl_context_client(this, &ResonanceStreamPlayback::ipl_context_reinit_cleanup);
    ResonanceLog::info("Playback Initialized.");
}

bool ResonanceStreamPlayback::prewarm_steam_audio() {
    // Main-thread IPL alloc before first _mix. Audio thread never creates effects.
    if (is_initialized)
        return true;
    _lazy_init_steam_audio(0);
    return is_initialized;
}

void ResonanceStreamPlayback::resolve_stale_steam_context_on_main() {
    // Stale: teardown old IPL. Always retry prewarm when uninitialized (late server
    // ready, failed first prewarm, or reinit cleanup which clears the stale flag).
    if (steam_context_stale_.load(std::memory_order_acquire)) {
        _cleanup_steam_audio();
        steam_context_stale_.store(false, std::memory_order_release);
    }
    if (!is_initialized)
        prewarm_steam_audio();
}

void ResonanceStreamPlayback::_add_reverb_to_output(IPLAudioBuffer* reverb_buf, float refl_mix, bool split_output,
                                                    const IPLCoordinateSpace3& listener_coords) {
    if (reflection_processor.is_parametric()) {
        for (int i = 0; i < frame_size_; i++) {
            float mono = reverb_buf->data[0][i] * refl_mix;
            if (split_output) {
                output_ring_reverb_l.write(&mono, 1);
                output_ring_reverb_r.write(&mono, 1);
            } else {
                if (sa_final_mix_buffer.data[0])
                    sa_final_mix_buffer.data[0][i] += mono;
                if (direct_out_channels_ >= 2 && sa_final_mix_buffer.data[1])
                    sa_final_mix_buffer.data[1][i] += mono;
            }
        }
    } else {
        AudioFrame reverb_frames[resonance::kMaxAudioFrameSize];
        memset(reverb_frames, 0, sizeof(reverb_frames));
        bool decode_ok = mixer_processor.decode_ambisonic_to_stereo(reverb_buf, listener_coords, reverb_frames, frame_size_);
        if (decode_ok) {
            for (int i = 0; i < frame_size_; i++) {
                float l = reverb_frames[i].left * refl_mix;
                float r = reverb_frames[i].right * refl_mix;
                if (split_output) {
                    output_ring_reverb_l.write(&l, 1);
                    output_ring_reverb_r.write(&r, 1);
                } else {
                    if (sa_final_mix_buffer.data[0])
                        sa_final_mix_buffer.data[0][i] += l;
                    if (direct_out_channels_ >= 2 && sa_final_mix_buffer.data[1])
                        sa_final_mix_buffer.data[1][i] += r;
                }
            }
        }
    }
}

void ResonanceStreamPlayback::_zero_sa_final_mix() {
    if (!sa_final_mix_buffer.data)
        return;
    for (int c = 0; c < direct_out_channels_ && c < sa_final_mix_buffer.numChannels; ++c) {
        if (sa_final_mix_buffer.data[c])
            memset(sa_final_mix_buffer.data[c], 0, frame_size_ * sizeof(float));
    }
}

void ResonanceStreamPlayback::_write_output_rings_folded() {
    if (!sa_final_mix_buffer.data || direct_out_channels_ < 1)
        return;
    if (direct_out_channels_ == 1 && sa_final_mix_buffer.data[0]) {
        output_ring_l.write(sa_final_mix_buffer.data[0], frame_size_);
        output_ring_r.write(sa_final_mix_buffer.data[0], frame_size_);
        return;
    }
    if (direct_out_channels_ == 2 && sa_final_mix_buffer.data[0] && sa_final_mix_buffer.data[1]) {
        output_ring_l.write(sa_final_mix_buffer.data[0], frame_size_);
        output_ring_r.write(sa_final_mix_buffer.data[1], frame_size_);
        return;
    }
    for (int i = 0; i < frame_size_; ++i) {
        float ol = 0.0f;
        float orv = 0.0f;
        downmix_multichannel_sample(sa_final_mix_buffer.data, direct_out_channels_, i, ol, orv);
        temp_process_buffer_l[i] = ol;
        temp_process_buffer_r[i] = orv;
    }
    output_ring_l.write(temp_process_buffer_l.data(), frame_size_);
    output_ring_r.write(temp_process_buffer_r.data(), frame_size_);
}

float ResonanceStreamPlayback::_debug_sa_in_mono_rms() const {
    float input_rms = 0.0f;
#ifdef DEBUG_ENABLED
    float sum_sq = 0.0f;
    const int nch = sa_in_buffer.numChannels;
    if (nch > 0 && sa_in_buffer.data) {
        for (int i = 0; i < frame_size_; i++) {
            float mono = 0.0f;
            for (int c = 0; c < nch && sa_in_buffer.data[c]; c++)
                mono += sa_in_buffer.data[c][i];
            mono /= static_cast<float>(nch);
            sum_sq += mono * mono;
        }
    }
    input_rms = (frame_size_ > 0) ? std::sqrt(sum_sq / static_cast<float>(frame_size_)) : 0.0f;
#else
    (void)this;
#endif
    return input_rms;
}

bool ResonanceStreamPlayback::_feed_convolution_mixer(ResonanceServer* srv, IPLReflectionEffectParams& params,
                                                      float curr_refl_mix, float refl_wet_output_gain,
                                                      bool store_tail_params, float& out_dbg_reverb) {
    if (!srv)
        return false;
    auto mixer_guard = srv->scoped_mixer_read();
    IPLReflectionMixer mixer = mixer_guard.get();
    if (!mixer) {
        instrumentation_conv_mixer_null_blocks.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Shared mixer feed: reflections_mix_level only as extra wet scale. Node volume already
    // scaled sa_in_buffer pre-Steam, so dry and wet follow it.
    const float conv_reverb_gain = curr_refl_mix;
    out_dbg_reverb = conv_reverb_gain;
    const float input_rms = _debug_sa_in_mono_rms();
    const auto conv_apply_t0 = std::chrono::steady_clock::now();
    const bool reflection_applied =
        reflection_processor.process_mix(sa_in_buffer, params, mixer, prev_conv_reflections_mix_level_, curr_refl_mix,
                                         1.0f, params_current.apply_air_absorption_to_wet, params_current.air_absorption);
    const auto conv_apply_t1 = std::chrono::steady_clock::now();
    if (!reflection_applied) {
        instrumentation_conv_mix_failed_blocks.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    srv->record_convolution_reflection_apply_usec(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(conv_apply_t1 - conv_apply_t0).count()));
    srv->record_convolution_feed(params.ir != nullptr, conv_reverb_gain, input_rms);
    prev_conv_reflections_mix_level_ = curr_refl_mix;
    srv->record_mixer_feed();
    if (store_tail_params) {
        reflection_tail_params_ = params;
        reflection_tail_have_params_ = true;
    }
    reflection_tail_wet_gain_ = resonance::sanitize_audio_float(refl_wet_output_gain);
    reflection_tail_split_output_ = params_current.reverb_split_output;
    return true;
}

void ResonanceStreamPlayback::_apply_reflections_wet(ResonanceServer* srv, float wet_occ, float refl_wet_output_gain,
                                                     bool& out_reverb_to_player, float& out_dbg_reverb) {
    out_reverb_to_player = false;
    if (!srv)
        return;
    if (!params_current.enable_reverb) {
        instrumentation_enable_reverb_false_blocks.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    IPLReflectionEffectParams reverb_params{};
    const bool has_reverb = srv->fetch_reverb_params(current_source_handle, reverb_params);
    const int refl_type = srv->get_reflection_type();

    if (has_reverb) {
        if (refl_type == resonance::kReflectionHybrid) {
            if (params_current.reflections_eq[0] != 1.0f || params_current.reflections_eq[1] != 1.0f ||
                params_current.reflections_eq[2] != 1.0f) {
                reverb_params.eq[0] *= params_current.reflections_eq[0];
                reverb_params.eq[1] *= params_current.reflections_eq[1];
                reverb_params.eq[2] *= params_current.reflections_eq[2];
            }
            if (params_current.reflections_delay >= 0)
                reverb_params.delay = params_current.reflections_delay;
        }

        if (refl_type == resonance::kReflectionConvolution || refl_type == resonance::kReflectionTan) {
            const float curr_refl_mix = resonance::sanitize_audio_float(params_current.reflections_mix_level);
            _feed_convolution_mixer(srv, reverb_params, curr_refl_mix, refl_wet_output_gain, true, out_dbg_reverb);
            return;
        }

        out_reverb_to_player = true;
        const float parametric_mix_level =
            resonance::sanitize_audio_float(params_current.reflections_mix_level * wet_occ);
        if (reflection_processor.process_mix_direct(sa_in_buffer, reverb_params, prev_parametric_reflections_mix_level_,
                                                    parametric_mix_level, params_current.apply_air_absorption_to_wet,
                                                    params_current.air_absorption)) {
            prev_parametric_reflections_mix_level_ = parametric_mix_level;
            reflection_tail_params_ = reverb_params;
            reflection_tail_have_params_ = true;
            reflection_tail_wet_gain_ = resonance::sanitize_audio_float(refl_wet_output_gain);
            reflection_tail_split_output_ = params_current.reverb_split_output;
        }
        return;
    }

    if (reflection_tail_have_params_ &&
        (refl_type == resonance::kReflectionParametric || refl_type == resonance::kReflectionHybrid)) {
        // Stale fetch: keep last params stepping to avoid wet pumping.
        IPLReflectionEffectParams rp = reflection_tail_params_;
        if (refl_type == resonance::kReflectionHybrid && params_current.reflections_delay >= 0)
            rp.delay = params_current.reflections_delay;
        out_reverb_to_player = true;
        const float parametric_mix_level_stale =
            resonance::sanitize_audio_float(params_current.reflections_mix_level * wet_occ);
        if (reflection_processor.process_mix_direct(sa_in_buffer, rp, prev_parametric_reflections_mix_level_,
                                                    parametric_mix_level_stale, params_current.apply_air_absorption_to_wet,
                                                    params_current.air_absorption)) {
            prev_parametric_reflections_mix_level_ = parametric_mix_level_stale;
            reflection_tail_wet_gain_ = 1.0f;
            reflection_tail_split_output_ = params_current.reverb_split_output;
        }
        return;
    }

    if (reflection_tail_have_params_ &&
        (refl_type == resonance::kReflectionConvolution || refl_type == resonance::kReflectionTan)) {
        // Brief fetch miss: reuse last good Conv/TAN params for one block.
        IPLReflectionEffectParams rp = reflection_tail_params_;
        const float curr_refl_mix = resonance::sanitize_audio_float(params_current.reflections_mix_level);
        _feed_convolution_mixer(srv, rp, curr_refl_mix, refl_wet_output_gain, false, out_dbg_reverb);
        return;
    }

    instrumentation_reverb_miss_blocks.fetch_add(1, std::memory_order_relaxed);
    const float refl_mix_gate = resonance::sanitize_audio_float(params_current.reflections_mix_level);
    if (refl_mix_gate <= 0.0f) {
        no_reverb_warn_count = 0;
        return;
    }
    ++no_reverb_warn_count;
    if (no_reverb_warn_count > resonance::kPlayerNoReverbWarnThreshold) {
        ResonanceLog::warn_cstr(
            "Playback: No reflection effect params from simulation while reflections_mix > 0. "
            "Wait for worker cache / probes, or check mix gating.");
        no_reverb_warn_count = 0;
    }
}

float ResonanceStreamPlayback::_apply_pathing_wet(ResonanceServer* srv, const IPLCoordinateSpace3& listener_cs) {
    if (!srv || !srv->is_pathing_enabled() || !params_current.enable_reverb ||
        params_current.pathing_mix_level <= 0.0f) {
        prev_pathing_mix_level_ = params_current.pathing_mix_level;
        return 0.0f;
    }

    srv->record_pathing_player_gate_enter();
    IPLPathEffectParams path_params{};
    if (!srv->fetch_pathing_params(current_source_handle, path_params)) {
        srv->record_pathing_player_fetch_miss();
        return 0.0f;
    }

    path_params.order = resonance::pathing_apply_order(srv->get_ambisonic_order());
    path_params.listener = listener_cs;
    if (!params_current.apply_hrtf_to_pathing) {
        path_params.hrtf = nullptr;
        path_params.binaural = IPL_FALSE;
    }
    // Deep-copy SH so live Apply and EOS never read server cache memory.
    pathing_tail_params_ = path_params;
    pathing_tail_have_params_ = true;
    const int n = resonance::pathing_sh_coeff_count(path_params.order);
    if (path_params.shCoeffs && path_params.order >= 0) {
        const int to_copy = std::min(n, static_cast<int>(pathing_tail_sh_coeffs_.size()));
        for (int i = 0; i < to_copy; i++)
            pathing_tail_sh_coeffs_[static_cast<size_t>(i)] = path_params.shCoeffs[i];
        for (size_t i = static_cast<size_t>(to_copy); i < pathing_tail_sh_coeffs_.size(); i++)
            pathing_tail_sh_coeffs_[i] = 0.0f;
    } else {
        for (size_t i = 0; i < pathing_tail_sh_coeffs_.size(); i++)
            pathing_tail_sh_coeffs_[i] = 0.0f;
    }
    path_params.shCoeffs = pathing_tail_sh_coeffs_.data();
    pathing_tail_params_.shCoeffs = pathing_tail_sh_coeffs_.data();

    const int32_t path_order = path_params.order;
    instrumentation_last_pathing_order.store(path_order, std::memory_order_relaxed);
    if (path_order >= 0) {
        double sum_sq = 0.0;
        for (int i = 0; i < n && i < static_cast<int>(pathing_tail_sh_coeffs_.size()); i++) {
            const double c = static_cast<double>(pathing_tail_sh_coeffs_[static_cast<size_t>(i)]);
            sum_sq += c * c;
        }
        const float energy = static_cast<float>(sum_sq);
        const float sh_rms = (n > 0) ? static_cast<float>(std::sqrt(sum_sq / static_cast<double>(n))) : 0.0f;
        instrumentation_last_pathing_sh_energy.store(energy, std::memory_order_relaxed);
        instrumentation_last_pathing_sh_rms.store(sh_rms, std::memory_order_relaxed);
    }

    if (sa_path_out_buffer.data[0])
        memset(sa_path_out_buffer.data[0], 0, frame_size_ * sizeof(float));
    if (sa_path_out_buffer.data[1])
        memset(sa_path_out_buffer.data[1], 0, frame_size_ * sizeof(float));
    path_processor.process(sa_in_buffer, path_params, sa_path_out_buffer, prev_pathing_mix_level_,
                           params_current.pathing_mix_level);

    float path_sum_sq = 0.0f;
    for (int i = 0; i < frame_size_; i++) {
        const float pl = sa_path_out_buffer.data[0][i];
        const float pr = sa_path_out_buffer.data[1][i];
        path_sum_sq += pl * pl + pr * pr;
    }
    const float path_out_rms =
        (frame_size_ > 0) ? std::sqrt(path_sum_sq / (2.0f * static_cast<float>(frame_size_))) : 0.0f;
    instrumentation_last_pathing_out_rms.store(path_out_rms, std::memory_order_relaxed);
    const float dbg_path = resonance::sanitize_audio_float(std::clamp(path_out_rms, 0.0f, 1.0f));
    for (int i = 0; i < frame_size_; i++) {
        if (sa_final_mix_buffer.data[0])
            sa_final_mix_buffer.data[0][i] += sa_path_out_buffer.data[0][i];
        if (direct_out_channels_ >= 2 && sa_final_mix_buffer.data[1])
            sa_final_mix_buffer.data[1][i] += sa_path_out_buffer.data[1][i];
    }
    prev_pathing_mix_level_ = params_current.pathing_mix_level;
    srv->record_pathing_player_applied();
    return dbg_path;
}

void ResonanceStreamPlayback::_process_passthrough_block() {
    debug_signal_direct.store(1.0f, std::memory_order_relaxed);
    debug_signal_reverb.store(0.0f, std::memory_order_relaxed);
    debug_signal_pathing.store(0.0f, std::memory_order_relaxed);
    instrumentation_last_pathing_sh_rms.store(0.0f, std::memory_order_relaxed);
    instrumentation_last_pathing_sh_energy.store(0.0f, std::memory_order_relaxed);
    instrumentation_last_pathing_out_rms.store(0.0f, std::memory_order_relaxed);
    instrumentation_last_pathing_order.store(-1, std::memory_order_relaxed);
    instrumentation_passthrough_blocks.fetch_add(1, std::memory_order_relaxed);
    if (sa_final_mix_buffer.data[0] && sa_in_buffer.data[0])
        memcpy(sa_final_mix_buffer.data[0], sa_in_buffer.data[0], frame_size_ * sizeof(float));
    if (direct_out_channels_ >= 2 && sa_final_mix_buffer.data[1] && sa_in_buffer.data[1])
        memcpy(sa_final_mix_buffer.data[1], sa_in_buffer.data[1], frame_size_ * sizeof(float));
    for (int c = 2; c < direct_out_channels_; c++) {
        if (sa_final_mix_buffer.data[c])
            memset(sa_final_mix_buffer.data[c], 0, frame_size_ * sizeof(float));
    }
    prev_direct_weight = 0.0f;
    prev_parametric_reflections_mix_level_ = 0.0f;
    prev_pathing_mix_level_ = 0.0f;
    prev_conv_reflections_mix_level_ = -1.0f;
}

void ResonanceStreamPlayback::_process_steam_audio_block() {
    auto t0 = std::chrono::steady_clock::now();

    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!context || !srv || !srv->is_initialized())
        return;

    if (!ipl_all_channel_ptrs_ok(sa_in_buffer, 2))
        return;
    if (!ipl_all_channel_ptrs_ok(sa_direct_out_buffer, direct_out_channels_))
        return;
    if (!ipl_all_channel_ptrs_ok(sa_path_out_buffer, 2))
        return;
    if (!ipl_all_channel_ptrs_ok(sa_final_mix_buffer, direct_out_channels_))
        return;

    input_ring_l.read(temp_process_buffer_l.data(), frame_size_);
    input_ring_r.read(temp_process_buffer_r.data(), frame_size_);
    // Source loudness before Steam: dry + wet (incl. convolution mixer feed) all see this gain.
    // Dry-only level changes belong on direct_mix_level, not volume_db.
    const float node_vol = owner_effective_volume_linear(owner_player_);
    if (std::abs(node_vol - 1.0f) > 1.0e-5f) {
        for (int i = 0; i < frame_size_; i++) {
            temp_process_buffer_l[i] *= node_vol;
            temp_process_buffer_r[i] *= node_vol;
        }
    }
    memcpy(sa_in_buffer.data[0], temp_process_buffer_l.data(), frame_size_ * sizeof(float));
    memcpy(sa_in_buffer.data[1], temp_process_buffer_r.data(), frame_size_ * sizeof(float));

    // Wait for first non-zero sample (avoids ramp artifacts before real audio).
    if (!input_started) {
        for (int i = 0; i < frame_size_; i++) {
            if (std::fabs(temp_process_buffer_l[i]) != 0.0f || std::fabs(temp_process_buffer_r[i]) != 0.0f) {
                input_started = true;
                break;
            }
        }
        if (!input_started) {
            _zero_sa_final_mix();
            _write_output_rings_folded();
            return;
        }
    }

    if (current_source_handle < 0) {
        _process_passthrough_block();
    } else if (!srv->is_spatial_audio_output_ready()) {
        _zero_sa_final_mix();
    } else {
        float dbg_direct = 0.0f;
        float dbg_reverb = 0.0f;
        float dbg_path = 0.0f;

        instrumentation_last_pathing_sh_rms.store(0.0f, std::memory_order_relaxed);
        instrumentation_last_pathing_sh_energy.store(0.0f, std::memory_order_relaxed);
        instrumentation_last_pathing_out_rms.store(0.0f, std::memory_order_relaxed);
        instrumentation_last_pathing_order.store(-1, std::memory_order_relaxed);

        const float wet_occ = resonance::sanitize_audio_float(params_current.wet_occlusion_factor);
        const IPLCoordinateSpace3 listener_cs = srv->get_current_listener_coords();
        bool reverb_to_player_output = false;

        direct_processor.process(
            params_current.use_ambisonics_encode, sa_in_buffer, sa_direct_out_buffer, params_current.attenuation,
            params_current.occlusion, params_current.transmission, params_current.air_absorption.data(),
            params_current.apply_air_absorption, params_current.directivity_value, params_current.apply_directivity,
            params_current.enable_direct, params_current.use_binaural, params_current.direct_effect_transmission_type,
            params_current.direct_effect_hrtf_bilinear, params_current.spatial_blend, listener_cs,
            ResonanceUtils::to_ipl_vector3(params_current.source_position));

        _apply_reflections_wet(srv, wet_occ, 1.0f, reverb_to_player_output, dbg_reverb);

        float target_direct = (params_current.enable_direct ? 1.0f : 0.0f) * params_current.direct_mix_level;
        for (int c = 0; c < direct_out_channels_; c++) {
            if (sa_direct_out_buffer.data[c])
                resonance::apply_volume_ramp(prev_direct_weight, target_direct, frame_size_, sa_direct_out_buffer.data[c]);
        }
        prev_direct_weight = target_direct;

        double sum_sq_direct = 0.0;
        int direct_samples = 0;
        for (int c = 0; c < direct_out_channels_; c++) {
            const float* ch = (sa_direct_out_buffer.data) ? sa_direct_out_buffer.data[c] : nullptr;
            if (!ch)
                continue;
            for (int i = 0; i < frame_size_; i++) {
                const float s = resonance::sanitize_audio_float(ch[i]);
                sum_sq_direct += static_cast<double>(s) * static_cast<double>(s);
            }
            direct_samples += frame_size_;
        }
        if (direct_samples > 0) {
            const double mean_sq = sum_sq_direct / static_cast<double>(direct_samples);
            dbg_direct = resonance::sanitize_audio_float(static_cast<float>(std::sqrt(std::max(0.0, mean_sq))));
            dbg_direct = std::clamp(dbg_direct, 0.0f, 1.0f);
        }

        for (int c = 0; c < direct_out_channels_; c++) {
            if (sa_direct_out_buffer.data[c] && sa_final_mix_buffer.data[c])
                memcpy(sa_final_mix_buffer.data[c], sa_direct_out_buffer.data[c], frame_size_ * sizeof(float));
        }

        if (reverb_to_player_output) {
            IPLAudioBuffer* reverb_buf = reflection_processor.get_direct_output_buffer();
            if (reverb_buf && reverb_buf->data) {
                const float refl_mix = 1.0f;
                dbg_reverb = resonance::sanitize_audio_float(params_current.reflections_mix_level * node_vol * wet_occ);
                _add_reverb_to_output(reverb_buf, refl_mix, params_current.reverb_split_output, listener_cs);
            }
        }

        dbg_path = _apply_pathing_wet(srv, listener_cs);

        debug_signal_direct.store(dbg_direct, std::memory_order_relaxed);
        debug_signal_reverb.store(dbg_reverb, std::memory_order_relaxed);
        debug_signal_pathing.store(dbg_path, std::memory_order_relaxed);
    }

    for (int c = 0; c < direct_out_channels_; c++) {
        if (!sa_final_mix_buffer.data[c])
            continue;
        for (int i = 0; i < frame_size_; i++)
            sa_final_mix_buffer.data[c][i] = std::clamp(sa_final_mix_buffer.data[c][i], -1.0f, 1.0f);
    }

    float sum_sq = 0.0f;
    for (int i = 0; i < frame_size_; i++) {
        float l = 0.0f;
        float r = 0.0f;
        downmix_multichannel_sample(sa_final_mix_buffer.data, direct_out_channels_, i, l, r);
        sum_sq += l * l + r * r;
    }
    float rms = (frame_size_ > 0) ? std::sqrt(sum_sq / (2.0f * static_cast<float>(frame_size_))) : 0.0f;
    instrumentation_last_output_rms_q8.store((uint32_t)(rms * 256.0f), std::memory_order_relaxed);
    if (current_source_handle >= 0 && rms < resonance::kInstrumentationSilentBlockThreshold)
        instrumentation_silent_output_blocks.fetch_add(1, std::memory_order_relaxed);

    _write_output_rings_folded();

    auto t1 = std::chrono::steady_clock::now();
    uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    instrumentation_last_block_time_us.store(us, std::memory_order_relaxed);
    uint64_t cur_max = instrumentation_max_block_time_us.load(std::memory_order_relaxed);
    while (us > cur_max && !instrumentation_max_block_time_us.compare_exchange_weak(cur_max, us, std::memory_order_relaxed)) {
    }
}

void ResonanceStreamPlayback::get_instrumentation_snapshot(uint64_t& out_input_dropped, uint64_t& out_output_underrun,
                                                           uint64_t& out_output_blocked, uint64_t& out_mix_calls, uint64_t& out_blocks_processed,
                                                           uint64_t& out_passthrough_blocks, uint64_t& out_reverb_miss_blocks, uint64_t& out_max_block_time_us,
                                                           uint64_t& out_last_block_time_us,
                                                           uint64_t& out_late_mix, uint64_t& out_last_mix_gap_us, uint64_t& out_max_mix_gap_us,
                                                           uint64_t& out_expected_mix_gap_us, uint64_t& out_param_syncs, uint64_t& out_zero_input,
                                                           int32_t& out_mix_frames_min, int32_t& out_mix_frames_max,
                                                           uint64_t& out_silent_blocks, float& out_last_rms,
                                                           float& out_pathing_sh_rms, float& out_pathing_sh_energy, float& out_pathing_out_rms,
                                                           int32_t& out_pathing_order,
                                                           uint64_t& out_conv_mixer_null_blocks, uint64_t& out_conv_mix_failed_blocks,
                                                           uint64_t& out_enable_reverb_false_blocks) const {
    out_input_dropped = instrumentation_input_dropped.load(std::memory_order_relaxed);
    out_output_underrun = instrumentation_output_underrun.load(std::memory_order_relaxed);
    out_output_blocked = instrumentation_output_blocked.load(std::memory_order_relaxed);
    out_mix_calls = instrumentation_mix_call_count.load(std::memory_order_relaxed);
    out_blocks_processed = instrumentation_blocks_processed.load(std::memory_order_relaxed);
    out_passthrough_blocks = instrumentation_passthrough_blocks.load(std::memory_order_relaxed);
    out_reverb_miss_blocks = instrumentation_reverb_miss_blocks.load(std::memory_order_relaxed);
    out_max_block_time_us = instrumentation_max_block_time_us.load(std::memory_order_relaxed);
    out_last_block_time_us = instrumentation_last_block_time_us.load(std::memory_order_relaxed);
    out_late_mix = instrumentation_late_mix_count.load(std::memory_order_relaxed);
    out_last_mix_gap_us = instrumentation_last_mix_gap_us_.load(std::memory_order_relaxed);
    out_max_mix_gap_us = instrumentation_max_mix_gap_us_.load(std::memory_order_relaxed);
    out_expected_mix_gap_us = instrumentation_expected_mix_gap_us_.load(std::memory_order_relaxed);
    out_param_syncs = instrumentation_param_sync_count.load(std::memory_order_relaxed);
    out_zero_input = instrumentation_zero_input_count.load(std::memory_order_relaxed);
    out_mix_frames_min = instrumentation_mix_frames_min.load(std::memory_order_relaxed);
    out_mix_frames_max = instrumentation_mix_frames_max.load(std::memory_order_relaxed);
    out_silent_blocks = instrumentation_silent_output_blocks.load(std::memory_order_relaxed);
    out_last_rms = instrumentation_last_output_rms_q8.load(std::memory_order_relaxed) / 256.0f;
    out_pathing_sh_rms = instrumentation_last_pathing_sh_rms.load(std::memory_order_relaxed);
    out_pathing_sh_energy = instrumentation_last_pathing_sh_energy.load(std::memory_order_relaxed);
    out_pathing_out_rms = instrumentation_last_pathing_out_rms.load(std::memory_order_relaxed);
    out_pathing_order = instrumentation_last_pathing_order.load(std::memory_order_relaxed);
    out_conv_mixer_null_blocks = instrumentation_conv_mixer_null_blocks.load(std::memory_order_relaxed);
    out_conv_mix_failed_blocks = instrumentation_conv_mix_failed_blocks.load(std::memory_order_relaxed);
    out_enable_reverb_false_blocks = instrumentation_enable_reverb_false_blocks.load(std::memory_order_relaxed);
}

// out_pathing: path wet stereo RMS after simulation, clamped to [0,1] (not pathing_mix_level).
void ResonanceStreamPlayback::get_debug_signal_levels(float& out_direct, float& out_reverb, float& out_pathing) const {
    out_direct = debug_signal_direct.load(std::memory_order_relaxed);
    out_reverb = debug_signal_reverb.load(std::memory_order_relaxed);
    out_pathing = debug_signal_pathing.load(std::memory_order_relaxed);
}

int32_t ResonanceStreamPlayback::read_reverb_frames(AudioFrame* buffer, int32_t frames) {
    if (!buffer || frames <= 0)
        return 0;
    size_t avail = output_ring_reverb_l.get_available_read();
    int32_t to_read = (int32_t)std::min((size_t)frames, avail);
    to_read = std::min(to_read, (int32_t)resonance::kMaxAudioFrameSize);
    if (to_read <= 0) {
        reverb_ring_gap_fade_armed_ = false;
        reverb_ring_gap_fade_index_ = 0;
        reverb_ring_gap_fade_total_ = 0;
        // Ring ran dry: if last callback ended non-zero, fade to zero to avoid a click.
        // Samples already include pre-Steam volume_db (written into the reverb ring).
        if (reverb_ring_prev_valid_ && (std::abs(reverb_ring_prev_l_) > 1.0e-6f || std::abs(reverb_ring_prev_r_) > 1.0e-6f)) {
            const float start_l = reverb_ring_prev_l_;
            const float start_r = reverb_ring_prev_r_;
            // pad_n<=1 makes input_ring_eos_pad_gain return 0 - use at least 2 so a single-frame callback fades correctly.
            const int32_t n = std::max(2, static_cast<int32_t>(frames));
            for (int32_t i = 0; i < frames; i++) {
                const float fade = resonance::input_ring_eos_pad_gain(static_cast<int>(i), n, true);
                buffer[i].left = start_l * fade;
                buffer[i].right = start_r * fade;
            }
        } else {
            for (int32_t i = 0; i < frames; i++) {
                buffer[i].left = 0.0f;
                buffer[i].right = 0.0f;
            }
        }
        reverb_ring_prev_l_ = 0.0f;
        reverb_ring_prev_r_ = 0.0f;
        reverb_ring_prev_valid_ = true;
        return frames;
    }
    output_ring_reverb_l.read(temp_reverb_buffer_l.data(), to_read);
    output_ring_reverb_r.read(temp_reverb_buffer_r.data(), to_read);
    for (int32_t i = 0; i < to_read; i++) {
        buffer[i].left = temp_reverb_buffer_l[i];
        buffer[i].right = temp_reverb_buffer_r[i];
    }
    // If caller asked for more than available, taper the remainder (cosine; single envelope across repeated underruns).
    if (to_read < frames) {
        const int32_t rem = frames - to_read;
        ResonanceServer* srv_wr = ResonanceServer::get_singleton();
        const bool zero_fill_underrun = srv_wr && srv_wr->is_reverb_bus_wet_ring_underrun_zero_fill();
        if (zero_fill_underrun) {
            reverb_ring_gap_fade_armed_ = false;
            reverb_ring_gap_fade_index_ = 0;
            reverb_ring_gap_fade_total_ = 0;
            for (int32_t k = 0; k < rem; k++) {
                buffer[to_read + k].left = 0.0f;
                buffer[to_read + k].right = 0.0f;
            }
        } else {
            const int sr_gap = current_sample_rate > 0 ? current_sample_rate : 48000;
            if (!reverb_ring_gap_fade_armed_) {
                reverb_ring_gap_fade_armed_ = true;
                reverb_ring_gap_fade_index_ = 0;
                reverb_ring_gap_fade_total_ = std::max(rem * 3, sr_gap / 40);
                reverb_ring_gap_fade_total_ = std::max(reverb_ring_gap_fade_total_, 2);
            }
            const float boundary_l = buffer[to_read - 1].left;
            const float boundary_r = buffer[to_read - 1].right;
            for (int32_t k = 0; k < rem; k++) {
                const int gi = reverb_ring_gap_fade_index_ + k;
                float g = 0.0f;
                if (reverb_ring_gap_fade_total_ > 1 && gi < reverb_ring_gap_fade_total_ - 1)
                    g = resonance::input_ring_eos_pad_gain(gi, reverb_ring_gap_fade_total_, true);
                buffer[to_read + k].left = boundary_l * g;
                buffer[to_read + k].right = boundary_r * g;
            }
            reverb_ring_gap_fade_index_ += rem;
        }
        reverb_ring_prev_l_ = temp_reverb_buffer_l[to_read - 1];
        reverb_ring_prev_r_ = temp_reverb_buffer_r[to_read - 1];
        reverb_ring_prev_valid_ = true;
    } else {
        reverb_ring_gap_fade_armed_ = false;
        reverb_ring_gap_fade_index_ = 0;
        reverb_ring_gap_fade_total_ = 0;
        reverb_ring_prev_l_ = temp_reverb_buffer_l[to_read - 1];
        reverb_ring_prev_r_ = temp_reverb_buffer_r[to_read - 1];
        reverb_ring_prev_valid_ = true;
    }
    return frames;
}

void ResonanceStreamPlayback::apply_playback_host_fades(AudioFrame* buffer, int32_t frames) {
    if (!buffer || frames <= 0) {
        return;
    }
    const int fi_total = std::max(1, playback_host_fade_in_total_samples_);
    const int fo_total = std::max(1, playback_host_fade_out_total_samples_);
    for (int i = 0; i < frames; i++) {
        float g = 1.0f;
        if (resonance::kPlaybackHostFadeInEnabled && playback_host_fade_in_elapsed_ < fi_total) {
            const int denom = std::max(1, fi_total - 1);
            g *= static_cast<float>(playback_host_fade_in_elapsed_) / static_cast<float>(denom);
            playback_host_fade_in_elapsed_++;
        }
        if (resonance::kPlaybackHostFadeOutEnabled && playback_host_fade_out_remaining_ > 0) {
            g *= static_cast<float>(playback_host_fade_out_remaining_) / static_cast<float>(fo_total);
            playback_host_fade_out_remaining_--;
        }
        buffer[i].left *= g;
        buffer[i].right *= g;
    }
}

void ResonanceStreamPlayback::reset_instrumentation() {
    instrumentation_input_dropped.store(0, std::memory_order_relaxed);
    instrumentation_output_underrun.store(0, std::memory_order_relaxed);
    instrumentation_output_blocked.store(0, std::memory_order_relaxed);
    instrumentation_mix_call_count.store(0, std::memory_order_relaxed);
    instrumentation_blocks_processed.store(0, std::memory_order_relaxed);
    instrumentation_passthrough_blocks.store(0, std::memory_order_relaxed);
    instrumentation_reverb_miss_blocks.store(0, std::memory_order_relaxed);
    instrumentation_max_block_time_us.store(0, std::memory_order_relaxed);
    instrumentation_last_block_time_us.store(0, std::memory_order_relaxed);
    instrumentation_late_mix_count.store(0, std::memory_order_relaxed);
    instrumentation_last_mix_gap_us_.store(0, std::memory_order_relaxed);
    instrumentation_max_mix_gap_us_.store(0, std::memory_order_relaxed);
    instrumentation_expected_mix_gap_us_.store(0, std::memory_order_relaxed);
    instrumentation_param_sync_count.store(0, std::memory_order_relaxed);
    instrumentation_zero_input_count.store(0, std::memory_order_relaxed);
    instrumentation_mix_frames_min.store(std::numeric_limits<int32_t>::max(), std::memory_order_relaxed);
    instrumentation_mix_frames_max.store(0, std::memory_order_relaxed);
    instrumentation_silent_output_blocks.store(0, std::memory_order_relaxed);
    instrumentation_last_pathing_sh_rms.store(0.0f, std::memory_order_relaxed);
    instrumentation_last_pathing_sh_energy.store(0.0f, std::memory_order_relaxed);
    instrumentation_last_pathing_out_rms.store(0.0f, std::memory_order_relaxed);
    instrumentation_last_pathing_order.store(-1, std::memory_order_relaxed);
    instrumentation_conv_mixer_null_blocks.store(0, std::memory_order_relaxed);
    instrumentation_conv_mix_failed_blocks.store(0, std::memory_order_relaxed);
    instrumentation_enable_reverb_false_blocks.store(0, std::memory_order_relaxed);
}

int32_t ResonanceStreamPlayback::_mix_drain_zero_input_tails(AudioFrame* buffer, int32_t frames,
                                                              ResonanceServer* srv_guard) {
    prev_mix_had_partial_input_pad_ = false;
    prev_mix_had_eos_tapered_input_pad_ = false;
    if (!is_initialized || steam_context_stale_.load(std::memory_order_acquire))
        return 0;
    if (!srv_guard || !srv_guard->is_initialized() || context != srv_guard->get_context_handle()) {
        steam_context_stale_.store(true, std::memory_order_release);
        return 0;
    }
    // Cap how long a stuck IPL tail can keep playback alive.
    if (tail_grace_blocks_remaining_.load(std::memory_order_acquire) < 0) {
        const int sr = current_sample_rate > 0 ? current_sample_rate : 48000;
        const int fs = frame_size_ > 0 ? frame_size_ : resonance::kGodotDefaultFrameSize;
        const float max_reverb_duration = srv_guard->get_max_reverb_duration();
        const int64_t blocks = (int64_t)((max_reverb_duration * (float)sr) / (float)fs) + 8;
        tail_grace_blocks_remaining_.store(blocks > 0 ? blocks : 8, std::memory_order_release);
    }
    // Flush remaining full input blocks before GetTail.
    while (input_ring_l.get_available_read() >= (size_t)frame_size_ &&
           output_ring_l.get_available_write() >= (size_t)frame_size_) {
        _process_steam_audio_block();
        instrumentation_blocks_processed.fetch_add(1, std::memory_order_relaxed);
    }
    {
        const size_t rem = input_ring_l.get_available_read();
        if (rem > 0 && rem < (size_t)frame_size_) {
            // Pad last partial frame with linear fade of hold sample to zero.
            if (input_ring_r.get_available_read() == rem &&
                rem <= temp_process_buffer_l.size() && rem <= temp_process_buffer_r.size() &&
                (size_t)frame_size_ <= temp_process_buffer_l.size() &&
                (size_t)frame_size_ <= temp_process_buffer_r.size()) {
                input_ring_l.read(temp_process_buffer_l.data(), rem);
                input_ring_r.read(temp_process_buffer_r.data(), rem);
                const float hold_l = temp_process_buffer_l[rem - 1];
                const float hold_r = temp_process_buffer_r[rem - 1];
                const size_t pad_count = (size_t)frame_size_ - rem;
                for (size_t k = 0; k < pad_count; k++) {
                    const float fade = resonance::linear_pad_fade_hold_to_zero(static_cast<int>(k), static_cast<int>(pad_count));
                    temp_process_buffer_l[rem + k] = hold_l * fade;
                    temp_process_buffer_r[rem + k] = hold_r * fade;
                }
                if (input_ring_l.get_available_write() >= (size_t)frame_size_ &&
                    input_ring_r.get_available_write() >= (size_t)frame_size_) {
                    input_ring_l.write(temp_process_buffer_l.data(), (size_t)frame_size_);
                    input_ring_r.write(temp_process_buffer_r.data(), (size_t)frame_size_);
                } else {
                    input_ring_l.write(temp_process_buffer_l.data(), rem);
                    input_ring_r.write(temp_process_buffer_r.data(), rem);
                    for (size_t k = 0; k < pad_count; k++) {
                        if (input_ring_l.get_available_write() == 0 || input_ring_r.get_available_write() == 0)
                            break;
                        const float fade = resonance::linear_pad_fade_hold_to_zero(static_cast<int>(k), static_cast<int>(pad_count));
                        float pl = hold_l * fade;
                        float pr = hold_r * fade;
                        input_ring_l.write(&pl, 1);
                        input_ring_r.write(&pr, 1);
                    }
                }
            }
            if (input_ring_l.get_available_read() >= (size_t)frame_size_ &&
                output_ring_l.get_available_write() >= (size_t)frame_size_) {
                _process_steam_audio_block();
                instrumentation_blocks_processed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    bool produced_any = false;
    while (output_ring_l.get_available_read() < (size_t)frames) {
        if (tail_grace_blocks_remaining_.load(std::memory_order_acquire) <= 0)
            break;
        if (!ipl_all_channel_ptrs_ok(sa_final_mix_buffer, direct_out_channels_) ||
            !ipl_all_channel_ptrs_ok(sa_direct_out_buffer, direct_out_channels_))
            break;

        bool produced = false;
        _zero_sa_final_mix();

        if (direct_processor.process_tail(sa_direct_out_buffer)) {
            for (int c = 0; c < direct_out_channels_; c++) {
                if (sa_direct_out_buffer.data[c] && sa_final_mix_buffer.data[c])
                    memcpy(sa_final_mix_buffer.data[c], sa_direct_out_buffer.data[c], frame_size_ * sizeof(float));
            }
            produced = true;
        }

        if (current_source_handle >= 0 && reflection_tail_have_params_ && reflection_processor.get_tail_size_samples() > 0) {
            const int eos_refl_type = srv_guard->get_reflection_type();
            if (eos_refl_type == resonance::kReflectionConvolution || eos_refl_type == resonance::kReflectionTan) {
                auto eos_mixer_guard = srv_guard->scoped_mixer_read();
                IPLReflectionMixer eos_mixer = eos_mixer_guard.get();
                if (eos_mixer) {
                    IPLReflectionEffectParams rp = reflection_tail_params_;
                    // Conv/TAN EOS: feed Apply with silence so the shared mixer advances.
                    if (!conv_reverb_eos_silence_apply_done_)
                        conv_reverb_eos_silence_apply_done_ = true;
                    if (sa_in_buffer.data[0])
                        memset(sa_in_buffer.data[0], 0, static_cast<size_t>(frame_size_) * sizeof(float));
                    if (sa_in_buffer.data[1])
                        memset(sa_in_buffer.data[1], 0, static_cast<size_t>(frame_size_) * sizeof(float));

                    const float curr_refl_mix_eos = resonance::sanitize_audio_float(params_current.reflections_mix_level);
                    if (reflection_processor.process_mix(sa_in_buffer, rp, eos_mixer, prev_conv_reflections_mix_level_,
                                                         curr_refl_mix_eos, 1.0f,
                                                         params_current.apply_air_absorption_to_wet, params_current.air_absorption)) {
                        prev_conv_reflections_mix_level_ = curr_refl_mix_eos;
                        srv_guard->record_mixer_feed();
                        produced = true;
                    } else {
                        instrumentation_conv_mix_failed_blocks.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    instrumentation_conv_mixer_null_blocks.fetch_add(1, std::memory_order_relaxed);
                }
            } else if (eos_refl_type == resonance::kReflectionParametric || eos_refl_type == resonance::kReflectionHybrid) {
                IPLReflectionEffectParams rp = reflection_tail_params_;
                reflection_processor.tail_apply_direct(&rp);
                IPLAudioBuffer* reverb_buf = reflection_processor.get_direct_output_buffer();
                if (reverb_buf && reverb_buf->data) {
                    _add_reverb_to_output(reverb_buf, reflection_tail_wet_gain_, reflection_tail_split_output_,
                                          srv_guard->get_current_listener_coords());
                    produced = true;
                }
            }
        }
        if (reflection_processor.get_tail_size_samples() <= 0)
            reflection_tail_have_params_ = false;

        if (current_source_handle >= 0 && srv_guard && srv_guard->is_pathing_enabled() && path_processor.get_tail_size_samples() > 0 &&
            sa_path_out_buffer.data && sa_path_out_buffer.data[0] && sa_path_out_buffer.data[1]) {
            // Pathing EOS: Apply(silence) with cached params each tick until tail ends.
            if (pathing_tail_have_params_ && pathing_tail_params_.shCoeffs) {
                if (sa_in_buffer.data[0])
                    memset(sa_in_buffer.data[0], 0, static_cast<size_t>(frame_size_) * sizeof(float));
                if (sa_in_buffer.data[1])
                    memset(sa_in_buffer.data[1], 0, static_cast<size_t>(frame_size_) * sizeof(float));

                if (sa_path_out_buffer.data[0])
                    memset(sa_path_out_buffer.data[0], 0, frame_size_ * sizeof(float));
                if (sa_path_out_buffer.data[1])
                    memset(sa_path_out_buffer.data[1], 0, frame_size_ * sizeof(float));

                path_processor.process(sa_in_buffer, pathing_tail_params_, sa_path_out_buffer,
                                       prev_pathing_mix_level_, prev_pathing_mix_level_);
                for (int i = 0; i < frame_size_; i++) {
                    if (sa_final_mix_buffer.data[0])
                        sa_final_mix_buffer.data[0][i] += sa_path_out_buffer.data[0][i];
                    if (direct_out_channels_ >= 2 && sa_final_mix_buffer.data[1])
                        sa_final_mix_buffer.data[1][i] += sa_path_out_buffer.data[1][i];
                }
                produced = true;
            }
        }

        if (!produced)
            break;
        produced_any = true;

        for (int c = 0; c < direct_out_channels_; c++) {
            if (!sa_final_mix_buffer.data[c])
                continue;
            for (int i = 0; i < frame_size_; i++)
                sa_final_mix_buffer.data[c][i] = std::clamp(sa_final_mix_buffer.data[c][i], -1.0f, 1.0f);
        }
        _write_output_rings_folded();
        int64_t remaining = tail_grace_blocks_remaining_.load(std::memory_order_acquire);
        if (remaining > 0)
            tail_grace_blocks_remaining_.store(remaining - 1, std::memory_order_release);
    }
    int available = (int)output_ring_l.get_available_read();
    int to_copy = (frames < available) ? frames : available;
    for (int i = 0; i < to_copy; i++) {
        float l, r;
        output_ring_l.read(&l, 1);
        output_ring_r.read(&r, 1);
        buffer[i].left = l;
        buffer[i].right = r;
    }
    if (to_copy < frames) {
        resonance::pad_output_with_cosine_underrun_fade(buffer, frames, to_copy, last_mix_out_l_, last_mix_out_r_,
                                            last_mix_out_valid_);
    }
    // Near-silence with no tail residue: end grace early.
    if (to_copy == 0 && !produced_any && last_mix_out_valid_ &&
        std::abs(last_mix_out_l_) < 1.0e-5f && std::abs(last_mix_out_r_) < 1.0e-5f &&
        !has_active_tail_residue()) {
        tail_grace_blocks_remaining_.store(0, std::memory_order_release);
    }
    if (!produced_any && to_copy == 0) {
        int64_t g = tail_grace_blocks_remaining_.load(std::memory_order_acquire);
        if (g > 0) {
            tail_grace_blocks_remaining_.store(g - 1, std::memory_order_release);
        }
    }
    const bool drained = (to_copy == 0) && !has_active_tail_residue() &&
                         (tail_grace_blocks_remaining_.load(std::memory_order_acquire) <= 0);
    if (frames > 0) {
        apply_playback_host_fades(buffer, frames);
        last_mix_out_l_ = buffer[frames - 1].left;
        last_mix_out_r_ = buffer[frames - 1].right;
        last_mix_out_valid_ = true;
    }
    // Keep returning frames until drained so finished fires at dry-EOS (player), not wet end.
    if (drained)
        tail_drain_complete_.store(true, std::memory_order_release);
    return frames;
}

int32_t ResonanceStreamPlayback::_mix_passthrough_pre_steam(AudioFrame* buffer, int32_t frames, const Vector2* src,
                                                            int32_t samples_read, bool base_playing) {
    // No IPL create/teardown on the audio thread. Main-thread prewarm / resolve_stale must run first.
    const float v = owner_effective_volume_linear(owner_player_);
    const bool eos_pt = samples_read > 0 && !base_playing;
    const int eos_tw =
        eos_pt ? std::min(samples_read, resonance::kEosInputEndTaperMaxSamples) : 0;
    for (int i = 0; i < samples_read; i++) {
        const float g_am = resonance::eos_input_end_am_gain(i, samples_read, eos_tw);
        buffer[i].left = src[i].x * v * g_am;
        buffer[i].right = src[i].y * v * g_am;
    }
    if (samples_read < frames) {
        const int pad_n = frames - samples_read;
        if (eos_pt) {
            for (int i = samples_read; i < frames; i++) {
                buffer[i].left = 0.0f;
                buffer[i].right = 0.0f;
            }
        } else {
            const float raw_last_l = (samples_read > 0) ? src[samples_read - 1].x : 0.0f;
            const float raw_last_r = (samples_read > 0) ? src[samples_read - 1].y : 0.0f;
            const float last_l = raw_last_l * v;
            const float last_r = raw_last_r * v;
            for (int i = samples_read; i < frames; i++) {
                const int k = i - samples_read;
                const float fade = resonance::linear_pad_fade_hold_to_zero(k, pad_n);
                buffer[i].left = last_l * fade;
                buffer[i].right = last_r * fade;
            }
        }
    }
    if (frames > 0) {
        apply_playback_host_fades(buffer, frames);
        last_mix_out_l_ = buffer[frames - 1].left;
        last_mix_out_r_ = buffer[frames - 1].right;
        last_mix_out_valid_ = true;
    }
    return frames;
}

void ResonanceStreamPlayback::_mix_ingest_decoder_samples(const Vector2* src_ptr, int32_t samples_read, int32_t frames,
                                                          bool base_playing) {
    int dec_i = 0;
    const bool eos_input_tail = samples_read > 0 && !base_playing;
    const int eos_taper_w =
        eos_input_tail ? std::min(samples_read, resonance::kEosInputEndTaperMaxSamples) : 0;
    // Only crossfade after a partial mix_audio chunk - otherwise every full buffer warps a continuous sine.
    const bool do_input_chunk_crossfade = input_ring_tail_valid_ && samples_read > 0 &&
                                          prev_mix_had_partial_input_pad_ && !prev_mix_had_eos_tapered_input_pad_;
    if (do_input_chunk_crossfade) {
        const int K = std::min(samples_read, resonance::kInputRingChunkCrossfadeSamples);
        for (; dec_i < K; dec_i++) {
            const float g = static_cast<float>(dec_i + 1) / static_cast<float>(K);
            const float g_am = resonance::eos_input_end_am_gain(dec_i, samples_read, eos_taper_w);
            const float l = (src_ptr[dec_i].x * g + input_ring_tail_l_ * (1.0f - g)) * g_am;
            const float r = (src_ptr[dec_i].y * g + input_ring_tail_r_ * (1.0f - g)) * g_am;
            if (input_ring_l.get_available_write() > 0) {
                input_ring_l.write(&l, 1);
                input_ring_r.write(&r, 1);
            } else {
                instrumentation_input_dropped.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    for (; dec_i < samples_read; dec_i++) {
        const float g_am = resonance::eos_input_end_am_gain(dec_i, samples_read, eos_taper_w);
        float l = src_ptr[dec_i].x * g_am;
        float r = src_ptr[dec_i].y * g_am;
        if (input_ring_l.get_available_write() > 0) {
            input_ring_l.write(&l, 1);
            input_ring_r.write(&r, 1);
        } else {
            instrumentation_input_dropped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Pad short decode callbacks so downstream always consumes full frame_size_ from rings per block.
    // Live: linear hold-last pad. EOS: zeros - dry tail tapered via eos_input_end_am_gain on real samples.
    if (samples_read < frames) {
        const int pad_n = frames - samples_read;
        if (eos_input_tail) {
            for (int i = samples_read; i < frames; i++) {
                float z = 0.0f;
                if (input_ring_l.get_available_write() > 0) {
                    input_ring_l.write(&z, 1);
                    input_ring_r.write(&z, 1);
                } else {
                    instrumentation_input_dropped.fetch_add(1, std::memory_order_relaxed);
                }
            }
        } else {
            const float last_l = src_ptr[samples_read - 1].x;
            const float last_r = src_ptr[samples_read - 1].y;
            for (int i = samples_read; i < frames; i++) {
                const int k = i - samples_read;
                const float fade = resonance::linear_pad_fade_hold_to_zero(k, pad_n);
                float l = last_l * fade;
                float r = last_r * fade;
                if (input_ring_l.get_available_write() > 0) {
                    input_ring_l.write(&l, 1);
                    input_ring_r.write(&r, 1);
                } else {
                    instrumentation_input_dropped.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }

    if (samples_read > 0) {
        input_ring_tail_l_ = src_ptr[samples_read - 1].x;
        input_ring_tail_r_ = src_ptr[samples_read - 1].y;
        input_ring_tail_valid_ = true;
    }
    prev_mix_had_partial_input_pad_ = (samples_read < frames);
    prev_mix_had_eos_tapered_input_pad_ =
        (samples_read < frames) && !base_playing;
}

void ResonanceStreamPlayback::_mix_pump_available_steam_blocks() {
    int blocks_processed_this_call = 0;
    while (blocks_processed_this_call < kMaxBlocksPerMixCall && input_ring_l.get_available_read() >= frame_size_) {
        if (output_ring_l.get_available_write() >= frame_size_) {
            _process_steam_audio_block();
            instrumentation_blocks_processed.fetch_add(1, std::memory_order_relaxed);
            blocks_processed_this_call++;
        } else {
            instrumentation_output_blocked.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
}

void ResonanceStreamPlayback::_mix_emit_output_frames(AudioFrame* buffer, int32_t frames, bool count_underrun) {
    // Always report frames produced so AudioServer does not detach before wet-tail drain.
    int available = (int)output_ring_l.get_available_read();
    int valid_copy = (frames < available) ? frames : available;
    if (count_underrun && valid_copy < frames) {
        instrumentation_output_underrun.fetch_add((uint64_t)(frames - valid_copy), std::memory_order_relaxed);
    }

    for (int i = 0; i < valid_copy; i++) {
        float l = 0.0f;
        float r = 0.0f;
        output_ring_l.read(&l, 1);
        output_ring_r.read(&r, 1);
        buffer[i].left = l;
        buffer[i].right = r;
    }

    if (valid_copy < frames) {
        resonance::pad_output_with_cosine_underrun_fade(buffer, frames, valid_copy, last_mix_out_l_, last_mix_out_r_,
                                            last_mix_out_valid_);
    }

    if (frames > 0) {
        apply_playback_host_fades(buffer, frames);
        last_mix_out_l_ = buffer[frames - 1].left;
        last_mix_out_r_ = buffer[frames - 1].right;
        last_mix_out_valid_ = true;
    }
}

int32_t ResonanceStreamPlayback::_mix(AudioFrame* buffer, float rate_scale, int32_t frames) {
    if (ResonanceServer::ipl_audio_teardown_active()) {
        for (int32_t i = 0; i < frames; i++) {
            buffer[i].left = 0.0f;
            buffer[i].right = 0.0f;
        }
        last_mix_out_l_ = 0.0f;
        last_mix_out_r_ = 0.0f;
        last_mix_out_valid_ = true;
        return frames;
    }
    if (base_playback.is_null())
        return 0;
    // Silence until first spatial params arrive. Do not advance the decoder while gated.
    if (!params_ever_synced_.load(std::memory_order_acquire)) {
        for (int32_t i = 0; i < frames; i++) {
            buffer[i].left = 0.0f;
            buffer[i].right = 0.0f;
        }
        last_mix_out_l_ = 0.0f;
        last_mix_out_r_ = 0.0f;
        last_mix_out_valid_ = true;
        return frames;
    }
    auto now = std::chrono::steady_clock::now();
    instrumentation_mix_call_count.fetch_add(1, std::memory_order_relaxed);
    if (instrumentation_mix_call_count.load(std::memory_order_relaxed) > 1) {
        auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - last_mix_time_).count();
        const uint64_t gap_us = (elapsed_us > 0) ? static_cast<uint64_t>(elapsed_us) : 0;
        instrumentation_last_mix_gap_us_.store(gap_us, std::memory_order_relaxed);
        uint64_t prev_max = instrumentation_max_mix_gap_us_.load(std::memory_order_relaxed);
        if (gap_us > prev_max)
            instrumentation_max_mix_gap_us_.store(gap_us, std::memory_order_relaxed);
        if (current_sample_rate > 0 && frames > 0) {
            const uint64_t expected_us = static_cast<uint64_t>(
                (static_cast<double>(frames) * 1000000.0) / static_cast<double>(current_sample_rate));
            instrumentation_expected_mix_gap_us_.store(expected_us, std::memory_order_relaxed);
        }
        if (elapsed_us > resonance::kLateMixThresholdUs)
            instrumentation_late_mix_count.fetch_add(1, std::memory_order_relaxed);
    }
    last_mix_time_ = now;
    _sync_params();

    ResonanceServer* srv_guard = ResonanceServer::get_singleton();
    // Auto frame size: request reinit when Godot mix frames != Steam frame_size.
    if (srv_guard && srv_guard->is_initialized() && frames > 0 && srv_guard->get_audio_frame_size_was_auto()) {
        const int srv_fs = srv_guard->get_audio_frame_size();
        if (srv_fs > 0 && frames != srv_fs) {
            srv_guard->request_reinit_with_frame_size(frames);
        }
    }
    if (is_initialized && srv_guard && srv_guard->is_initialized() && context != srv_guard->get_context_handle())
        steam_context_stale_.store(true, std::memory_order_release);

    // Strong ref so teardown cannot drop base_playback mid-call.
    const Ref<AudioStreamPlayback> base_guard = base_playback;
    if (base_guard.is_null())
        return 0;
    PackedVector2Array mixed_frames = base_guard->mix_audio(rate_scale, frames);
    int32_t samples_read = static_cast<int32_t>(mixed_frames.size());
    // EOS: !is_playing with a full silent buffer -> treat as zero input; keep non-silent last buffer.
    if (samples_read == frames && !base_guard->is_playing()) {
        constexpr float k_eos_silent_eps = 1.0e-8f;
        const Vector2* p = mixed_frames.ptr();
        bool all_silent = true;
        for (int32_t i = 0; i < samples_read; i++) {
            if (std::fabs(p[i].x) > k_eos_silent_eps || std::fabs(p[i].y) > k_eos_silent_eps) {
                all_silent = false;
                break;
            }
        }
        if (all_silent) {
            samples_read = 0;
        }
    }
    if (samples_read > frames) {
        static std::atomic<int> oversize_warn_count{0};
        const int n = oversize_warn_count.fetch_add(1, std::memory_order_relaxed);
        if (n < 4) {
            ResonanceLog::warn(
                "ResonanceStreamPlayback: mix_audio returned more samples than requested; clamping to avoid buffer overflow.");
        }
        samples_read = frames;
    }
    if (samples_read == 0)
        instrumentation_zero_input_count.fetch_add(1, std::memory_order_relaxed);
    else {
        int32_t cur_min = instrumentation_mix_frames_min.load(std::memory_order_relaxed);
        if (samples_read < cur_min)
            instrumentation_mix_frames_min.store(samples_read, std::memory_order_relaxed);
        int32_t cur_max = instrumentation_mix_frames_max.load(std::memory_order_relaxed);
        if (samples_read > cur_max)
            instrumentation_mix_frames_max.store(samples_read, std::memory_order_relaxed);
    }

    if (samples_read == 0)
        return _mix_drain_zero_input_tails(buffer, frames, srv_guard);

    if (!is_initialized || steam_context_stale_.load(std::memory_order_acquire)) {
        return _mix_passthrough_pre_steam(buffer, frames, mixed_frames.ptr(), samples_read, base_guard->is_playing());
    }

    // Clear stale host fade-out unless we are in a real stop-tail (not EOS partial dry).
    if (playback_host_fade_out_remaining_ > 0) {
        const bool sr = stop_requested_.load(std::memory_order_acquire);
        const bool live = base_guard->is_playing() && samples_read > 0;
        const bool fade_in_active = resonance::kPlaybackHostFadeInEnabled &&
                                    playback_host_fade_in_total_samples_ > 0 &&
                                    playback_host_fade_in_elapsed_ < playback_host_fade_in_total_samples_;
        const bool eos_partial_dry = samples_read > 0 && !base_guard->is_playing();
        if (!sr || live || (fade_in_active && samples_read > 0) || (sr && eos_partial_dry)) {
            playback_host_fade_out_remaining_ = 0;
        }
    }

    _mix_ingest_decoder_samples(mixed_frames.ptr(), samples_read, frames, base_guard->is_playing());
    _mix_pump_available_steam_blocks();
    _mix_emit_output_frames(buffer, frames, true);
    return frames;
}
