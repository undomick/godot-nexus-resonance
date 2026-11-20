#include "resonance_audio_mix_lock.h"
#include "resonance_constants.h"
#include "resonance_geometry_gate_ingest_policy.h"
#include "resonance_log.h"
#include "resonance_math.h"
#include "resonance_pathing_fetch_policy.h"
#include "resonance_pathing_inputs_policy.h"
#include "resonance_playback_fade.h"
#include "resonance_playback_host_fade_policy.h"
#include "resonance_playback_input_started_policy.h"
#include "resonance_player.h"
#include "resonance_polyphonic_voice_policy.h"
#include "resonance_probe_volume.h"
#include "resonance_reflection_fetch_policy.h"
#include "resonance_reflections_wet_policy.h"
#include "resonance_server.h"
#include "resonance_source_update_policy.h"
#include "resonance_tail_drain_policy.h"
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

namespace {

/// Godot does not apply the AudioStreamPlayer3D volume to a GDExtension playback's _mix output,
/// so min(volume_db, max_db) scales the input before Steam Audio.
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

/// Folds one sample to stereo; channel order follows Steam Audio's quad, 5.1 and 7.1 layouts.
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
    if (!params_slot_.consume())
        return;
    instrumentation_param_sync_count.fetch_add(1, std::memory_order_relaxed);
    params_current = params_slot_.front();
    current_source_handle = params_current.source_handle;
}

void ResonanceStreamPlayback::_cleanup_steam_audio() {
    if (ResonanceServer* reg_srv = ResonanceServer::get_singleton())
        reg_srv->unregister_ipl_context_client(this);

    _release_retained_source();

    direct_processor.cleanup();
    path_processor.cleanup();
    mixer_processor.cleanup();
    wet_effects_initialized_ = false;

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

    // Zeroed so a second cleanup cannot double-free.
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
    prev_conv_reflections_mix_level_ = 0.0f;
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
    const uint64_t now_msec = Time::get_singleton()->get_ticks_msec();
    if (now_msec < next_prewarm_retry_msec_)
        return;
    ResonancePlayer* owner = owner_player();
    resonance::ScopedSpawnPhase phase(owner ? owner->internal_spawn_timing() : nullptr, resonance::SpawnPhase::VoiceDsp);

    current_sample_rate = srv->get_sample_rate();
    frame_size_ = srv->get_audio_frame_size();
    context = srv->get_context_handle();
    int order = srv->get_ambisonic_order();
    direct_out_channels_ = srv->get_direct_speaker_channels();
    if (direct_out_channels_ < 1)
        direct_out_channels_ = 2;

    temp_process_buffer_l.resize(frame_size_);
    temp_process_buffer_r.resize(frame_size_);

    const bool want_ambisonics = owner && owner->internal_wants_ambisonics_encode();
    direct_processor.initialize(context, current_sample_rate, frame_size_, order, want_ambisonics, direct_out_channels_);

    // A Direct-only voice (e.g. UI click) skips the wet IPL; _ensure_wet_effects_on_main adds it once wanted.
    if (!owner || owner->internal_wants_wet_effects())
        _init_wet_effects(srv);

    // Direct path matches server speaker layout; path processor stays stereo.
    if (iplAudioBufferAllocate(context, 2, frame_size_, &sa_in_buffer) != IPL_STATUS_SUCCESS ||
        iplAudioBufferAllocate(context, direct_out_channels_, frame_size_, &sa_direct_out_buffer) != IPL_STATUS_SUCCESS ||
        iplAudioBufferAllocate(context, 2, frame_size_, &sa_path_out_buffer) != IPL_STATUS_SUCCESS ||
        iplAudioBufferAllocate(context, direct_out_channels_, frame_size_, &sa_final_mix_buffer) != IPL_STATUS_SUCCESS ||
        !ipl_all_channel_ptrs_ok(sa_in_buffer, 2) || !ipl_all_channel_ptrs_ok(sa_direct_out_buffer, direct_out_channels_) ||
        !ipl_all_channel_ptrs_ok(sa_path_out_buffer, 2) || !ipl_all_channel_ptrs_ok(sa_final_mix_buffer, direct_out_channels_)) {
        ResonanceLog::error("ResonancePlayer: Playback Init Failed: Buffer allocation failed; retrying in " +
                            String::num_uint64(resonance::kPrewarmRetryIntervalMsec) + " ms.");
        _cleanup_steam_audio();
        next_prewarm_retry_msec_ = now_msec + resonance::kPrewarmRetryIntervalMsec;
        return;
    }

    is_initialized = true;
    if (ResonanceServer* reg_srv = ResonanceServer::get_singleton())
        reg_srv->register_ipl_context_client(this, &ResonanceStreamPlayback::ipl_context_reinit_cleanup);
    ResonanceLog::info("Playback Initialized.");
}

void ResonanceStreamPlayback::_init_wet_effects(ResonanceServer* srv) {
    const int order = srv->get_ambisonic_order();
    // One effect lives on the player. A per-voice effect would Apply the same IR and free the read buffer.
    if (ResonancePlayer* owner = owner_player())
        owner->ensure_shared_reflection_on_main(srv);
    path_processor.initialize(context, current_sample_rate, frame_size_, order);
    if (resonance::voice_needs_reverb_decoder(true, srv->get_reflection_type()))
        mixer_processor.initialize(context, current_sample_rate, frame_size_, order);
    wet_effects_initialized_ = true;
}

void ResonanceStreamPlayback::_ensure_wet_effects_on_main(ResonanceServer* srv) {
    if (wet_effects_initialized_)
        return;
    ResonancePlayer* owner = owner_player();
    if (!owner || !owner->internal_wants_wet_effects())
        return;
    const AudioMixBlockLock mix_lock;
    _init_wet_effects(srv);
}

bool ResonanceStreamPlayback::prewarm_steam_audio() {
    // Main-thread IPL alloc before first _mix. Audio thread never creates effects.
    if (is_initialized)
        return true;
    _lazy_init_steam_audio(0);
    return is_initialized;
}

void ResonanceStreamPlayback::resolve_stale_steam_context_on_main() {
    // Retry prewarm whenever uninitialized: the server came up late, the first prewarm failed, or reinit cleanup ran.
    if (steam_context_stale_.load(std::memory_order_acquire)) {
        const AudioMixBlockLock mix_lock;
        _cleanup_steam_audio();
        steam_context_stale_.store(false, std::memory_order_release);
    }
    if (!is_initialized)
        prewarm_steam_audio();
    if (!is_initialized)
        return;
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!srv || !srv->is_initialized())
        return;
    const IPLHRTF hrtf = srv->get_hrtf_handle();
    direct_processor.ensure_hrtf_effects_on_main(hrtf);
    ResonancePlayer* owner = owner_player();
    direct_processor.ensure_ambisonics_encode_on_main(owner && owner->internal_wants_ambisonics_encode());
    path_processor.ensure_hrtf_on_main(hrtf);
    _ensure_wet_effects_on_main(srv);
}

void ResonanceStreamPlayback::_add_reverb_to_output(IPLAudioBuffer* reverb_buf, float refl_mix,
                                                    const IPLCoordinateSpace3& listener_coords, bool apply_reverb_binaural) {
    ResonancePlayer* owner = owner_player();
    const bool parametric = owner && owner->shared_reflection_is_parametric();
    if (parametric) {
        for (int i = 0; i < frame_size_; i++) {
            const float mono = reverb_buf->data[0][i] * refl_mix;
            if (sa_final_mix_buffer.data[0])
                sa_final_mix_buffer.data[0][i] += mono;
            if (direct_out_channels_ >= 2 && sa_final_mix_buffer.data[1])
                sa_final_mix_buffer.data[1][i] += mono;
        }
    } else {
        AudioFrame reverb_frames[resonance::kMaxAudioFrameSize];
        memset(reverb_frames, 0, sizeof(reverb_frames));
        bool decode_ok =
            mixer_processor.decode_ambisonic_to_stereo(reverb_buf, listener_coords, reverb_frames, frame_size_, apply_reverb_binaural);
        if (decode_ok) {
            for (int i = 0; i < frame_size_; i++) {
                if (sa_final_mix_buffer.data[0])
                    sa_final_mix_buffer.data[0][i] += reverb_frames[i].left * refl_mix;
                if (direct_out_channels_ >= 2 && sa_final_mix_buffer.data[1])
                    sa_final_mix_buffer.data[1][i] += reverb_frames[i].right * refl_mix;
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
        _write_output_block(sa_final_mix_buffer.data[0], sa_final_mix_buffer.data[0]);
        return;
    }
    if (direct_out_channels_ == 2 && sa_final_mix_buffer.data[0] && sa_final_mix_buffer.data[1]) {
        _write_output_block(sa_final_mix_buffer.data[0], sa_final_mix_buffer.data[1]);
        return;
    }
    for (int i = 0; i < frame_size_; ++i) {
        float ol = 0.0f;
        float orv = 0.0f;
        downmix_multichannel_sample(sa_final_mix_buffer.data, direct_out_channels_, i, ol, orv);
        temp_process_buffer_l[i] = ol;
        temp_process_buffer_r[i] = orv;
    }
    _write_output_block(temp_process_buffer_l.data(), temp_process_buffer_r.data());
}

void ResonanceStreamPlayback::_write_output_block(const float* left, const float* right) {
    output_ring_l.write(left, frame_size_);
    output_ring_r.write(right, frame_size_);
    if (!params_current.reverb_send) {
        // Nobody plays the send ring once the send stops; stale samples would hold the voice open.
        if (output_ring_reverb_l.get_available_read() > 0) {
            output_ring_reverb_l.clear();
            output_ring_reverb_r.clear();
        }
        return;
    }
    output_ring_reverb_l.write(left, frame_size_);
    output_ring_reverb_r.write(right, frame_size_);
}

void ResonanceStreamPlayback::_apply_reflections_wet(ResonanceServer* srv, bool& out_reverb_to_player,
                                                     float& out_dbg_reverb) {
    out_reverb_to_player = false;
    if (!srv)
        return;
    if (!params_current.enable_reverb) {
        instrumentation_enable_reverb_false_blocks.fetch_add(1, std::memory_order_relaxed);
        resonance::reflections_wet_reset_prev_levels_on_disable(prev_conv_reflections_mix_level_,
                                                                prev_parametric_reflections_mix_level_);
        return;
    }
    // Pathing alone keeps enable_reverb on; without this the last reflection IR keeps convolving.
    if (!params_current.enable_reflections) {
        resonance::reflections_wet_reset_prev_levels_on_disable(prev_conv_reflections_mix_level_,
                                                                prev_parametric_reflections_mix_level_);
        return;
    }

    const int32_t owned_source = get_voice_source_handle();
    if (!resonance::voice_may_convolve_source(owned_source, current_source_handle))
        return;
    ResonancePlayer* owner = owner_player();
    if (!owner)
        return;
    // Dry from every voice sums here. The last voice of this callback Applies once.
    owner->reflection_round_add(this);
    owner->reflection_try_apply(this, out_reverb_to_player, out_dbg_reverb);
}

float ResonanceStreamPlayback::_apply_pathing_wet(ResonanceServer* srv, const IPLCoordinateSpace3& listener_cs) {
    // A mix of 0 still runs the path effect on silence so the ramp stays continuous for unmute.
    if (!srv || !srv->is_pathing_enabled() || !params_current.enable_reverb) {
        prev_pathing_mix_level_ = params_current.pathing_mix_level;
        return 0.0f;
    }

    srv->record_pathing_player_gate_enter();
    FetchedPathingParams fetched{};
    const bool fetch_ok = srv->fetch_pathing_params(current_source_handle, fetched);
    const bool pathing_current = srv->are_pathing_results_current();
    const uint32_t path_epoch = srv->get_pathing_param_cache_epoch();
    IPLPathEffectParams path_params{};
    if (fetch_ok) {
        path_params = fetched.params;
        path_params.order = resonance::pathing_apply_order(srv->get_ambisonic_order());
        // Copied so the end-of-stream Apply keeps these SH coefficients across cache swaps.
        pathing_tail_sh_coeffs_ = fetched.shCoeffs;
        pathing_tail_have_params_ = true;
        pathing_tail_param_epoch_ = path_epoch;
    } else {
        // The last SH coefficients bridge brief cache flips; only count a miss when they cannot.
        if (!resonance::pathing_wet_should_apply_stale(fetch_ok, pathing_tail_have_params_ && pathing_tail_params_.shCoeffs,
                                                       pathing_tail_param_epoch_, path_epoch, pathing_current)) {
            srv->record_pathing_player_fetch_miss();
            return 0.0f;
        }
        path_params = pathing_tail_params_;
        path_params.order = resonance::pathing_apply_order(srv->get_ambisonic_order());
    }

    path_params.listener = listener_cs;
    if (params_current.apply_hrtf_to_pathing) {
        path_params.hrtf = srv->get_hrtf_handle();
        path_params.binaural = IPL_TRUE;
    } else {
        path_params.hrtf = nullptr;
        path_params.binaural = IPL_FALSE;
    }
    path_params.shCoeffs = pathing_tail_sh_coeffs_.data();
    if (fetch_ok)
        pathing_tail_params_ = path_params;

    const int32_t path_order = path_params.order;
    const int n = resonance::pathing_sh_coeff_count(path_order);
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
    prev_conv_reflections_mix_level_ = 0.0f;
}

void ResonanceStreamPlayback::_process_steam_audio_block() {
    auto t0 = std::chrono::steady_clock::now();
    resonance::StageLapClock stage_clock(t0);

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

    if (resonance::spatial_audio_geometry_gate_should_hold_decode(current_source_handle,
                                                                  srv->is_source_spatial_output_ready(current_source_handle)))
        return;

    input_ring_l.read(temp_process_buffer_l.data(), frame_size_);
    input_ring_r.read(temp_process_buffer_r.data(), frame_size_);
    // volume_db scales dry and wet alike, the convolution mixer feed included; dry-only changes belong on direct_mix_level.
    const float node_vol = owner_effective_volume_linear(owner_player());
    if (std::abs(node_vol - 1.0f) > 1.0e-5f) {
        for (int i = 0; i < frame_size_; i++) {
            temp_process_buffer_l[i] *= node_vol;
            temp_process_buffer_r[i] *= node_vol;
        }
    }
    memcpy(sa_in_buffer.data[0], temp_process_buffer_l.data(), frame_size_ * sizeof(float));
    memcpy(sa_in_buffer.data[1], temp_process_buffer_r.data(), frame_size_ * sizeof(float));

    // The first block opens the Steam Audio path, so leading digital silence cannot stall it.
    const bool first_block_ingested = !input_started;
    if (!resonance::playback_steam_input_path_should_run(input_started, first_block_ingested)) {
        _zero_sa_final_mix();
        _write_output_rings_folded();
        return;
    }
    if (!input_started)
        input_started = true;
    stage_clock.lap(instrumentation_block_stages_, resonance::BlockStage::Input);

    if (current_source_handle < 0) {
        _process_passthrough_block();
        stage_clock.lap(instrumentation_block_stages_, resonance::BlockStage::Direct);
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
        const SharedReflectionUse shared_reflection_use(*this);

        direct_processor.process(
            params_current.use_ambisonics_encode, sa_in_buffer, sa_direct_out_buffer, params_current.attenuation,
            params_current.occlusion, params_current.transmission, params_current.air_absorption.data(),
            params_current.apply_air_absorption, params_current.directivity_value, params_current.apply_directivity,
            params_current.enable_direct, params_current.use_binaural, params_current.direct_effect_transmission_type,
            params_current.direct_effect_hrtf_bilinear, params_current.spatial_blend, listener_cs,
            ResonanceUtils::to_ipl_vector3(params_current.source_position));
        stage_clock.lap(instrumentation_block_stages_, resonance::BlockStage::Direct);

        _apply_reflections_wet(srv, reverb_to_player_output, dbg_reverb);
        stage_clock.lap(instrumentation_block_stages_, resonance::BlockStage::Reflections);

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
            ResonancePlayer* owner = owner_player();
            IPLAudioBuffer* reverb_buf = owner ? owner->shared_reflection_direct_output() : nullptr;
            if (reverb_buf && reverb_buf->data) {
                const float refl_mix = 1.0f;
                dbg_reverb = resonance::sanitize_audio_float(params_current.reflections_mix_level * node_vol * wet_occ);
                _add_reverb_to_output(reverb_buf, refl_mix, listener_cs, params_current.apply_hrtf_to_reflections);
            }
        }

        stage_clock.lap(instrumentation_block_stages_, resonance::BlockStage::DirectPost);

        dbg_path = _apply_pathing_wet(srv, listener_cs);
        stage_clock.lap(instrumentation_block_stages_, resonance::BlockStage::Pathing);

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
    stage_clock.lap(instrumentation_block_stages_, resonance::BlockStage::Output, t1);
    uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    instrumentation_last_block_time_us.store(us, std::memory_order_relaxed);
    instrumentation_block_time_total_us_.fetch_add(us, std::memory_order_relaxed);
    resonance::store_max(instrumentation_max_block_time_us, us);
    instrumentation_block_time_histogram_.record(us);
    _remember_audio_thread();
}

void ResonanceStreamPlayback::_remember_audio_thread() {
    if (instrumentation_audio_thread_id_.load(std::memory_order_relaxed) != resonance::kUnknownOsThreadId)
        return;
    instrumentation_audio_thread_id_.store(resonance::current_os_thread_id(), std::memory_order_relaxed);
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

uint64_t ResonanceStreamPlayback::get_block_time_total_us() const {
    return instrumentation_block_time_total_us_.load(std::memory_order_relaxed);
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
        // Ring ran dry: fade from the last sample to zero to avoid a click. volume_db is already applied.
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
    // Taper the part the ring cannot fill; one envelope spans repeated underruns.
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
    instrumentation_block_time_total_us_.store(0, std::memory_order_relaxed);
    instrumentation_block_stages_.reset();
    instrumentation_block_time_histogram_.reset();
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

bool ResonanceStreamPlayback::_drain_shared_reflection_tail(ResonanceServer* srv) {
    ResonancePlayer* owner = owner_player();
    const SharedReflectionUse shared_reflection_use(*this);
    if (!owner || !shared_reflection_use.held())
        return false;
    bool reverb_to_player = false;
    if (!owner->reflection_try_drain_tail(this, reverb_to_player))
        return false;
    if (!reverb_to_player)
        return true;
    IPLAudioBuffer* reverb_buf = owner->shared_reflection_direct_output();
    if (reverb_buf && reverb_buf->data)
        _add_reverb_to_output(reverb_buf, 1.0f, srv->get_current_listener_coords(),
                              params_current.apply_hrtf_to_reflections);
    return true;
}

int32_t ResonanceStreamPlayback::_mix_drain_zero_input_tails(AudioFrame* buffer, int32_t frames,
                                                             ResonanceServer* srv_guard) {
    prev_mix_had_partial_input_pad_ = false;
    prev_mix_had_eos_tapered_input_pad_ = false;
    const bool srv_ok = srv_guard && srv_guard->is_initialized();
    const bool ctx_ok = srv_ok && context == srv_guard->get_context_handle();
    const resonance::TailDrainIplGate gate = resonance::tail_drain_ipl_gate(
        is_initialized, steam_context_stale_.load(std::memory_order_acquire), srv_ok, ctx_ok);
    if (!srv_ok || !ctx_ok)
        steam_context_stale_.store(true, std::memory_order_release);
    if (gate.set_grace_to_zero)
        tail_grace_blocks_remaining_.store(0, std::memory_order_release);

    auto eos_pull_plan = [&](bool gate_open, ResonanceServer* srv) -> resonance::TailDrainEosPullPlan {
        const int direct_tail = direct_processor.get_tail_size_samples();
        ResonancePlayer* owner = owner_player();
        const int reflection_tail = owner ? owner->shared_reflection_tail_samples() : 0;
        const bool reflection_have_params = owner && srv && owner->shared_reflection_tail_applyable(srv);
        const int path_tail = path_processor.get_tail_size_samples();
        const bool path_bufs = sa_path_out_buffer.data && sa_path_out_buffer.data[0] && sa_path_out_buffer.data[1];
        bool reflection_will_pull = false;
        if (gate_open && srv && current_source_handle >= 0 && reflection_have_params && reflection_tail > 0) {
            const int eos_refl_type = srv->get_reflection_type();
            bool mixer_available = true;
            if (eos_refl_type == resonance::kReflectionConvolution || eos_refl_type == resonance::kReflectionTan) {
                auto mixer_probe = srv->scoped_mixer_read();
                mixer_available = mixer_probe.get() != nullptr;
            }
            reflection_will_pull = resonance::tail_drain_reflection_eos_will_pull(
                eos_refl_type, reflection_have_params, reflection_tail, true, mixer_available);
        }
        return resonance::tail_drain_eos_pull_plan(gate_open, direct_tail, reflection_have_params, reflection_tail,
                                                   reflection_will_pull, srv && srv->is_pathing_enabled(), path_tail,
                                                   path_bufs);
    };

    bool produced_any = false;
    if (gate.can_run_ipl_tail) {
        // Cap how long a stuck IPL tail can keep playback alive.
        if (tail_grace_blocks_remaining_.load(std::memory_order_acquire) < 0) {
            const int sr = current_sample_rate > 0 ? current_sample_rate : 48000;
            const int fs = frame_size_ > 0 ? frame_size_ : resonance::kGodotDefaultFrameSize;
            const float ir_duration = srv_guard->get_realtime_simulation_duration();
            const int64_t blocks = (int64_t)((ir_duration * (float)sr) / (float)fs) + 8;
            tail_grace_blocks_remaining_.store(blocks > 0 ? blocks : 8, std::memory_order_release);
        }
        const bool hold_decode =
            resonance::spatial_audio_geometry_gate_should_hold_decode(current_source_handle,
                                                                      srv_guard->is_source_spatial_output_ready(current_source_handle));
        if (resonance::tail_drain_should_flush_input(hold_decode)) {
            while (input_ring_l.get_available_read() >= (size_t)frame_size_ &&
                   output_ring_l.get_available_write() >= (size_t)frame_size_) {
                _process_steam_audio_block();
                instrumentation_blocks_processed.fetch_add(1, std::memory_order_relaxed);
            }
            {
                const size_t rem = input_ring_l.get_available_read();
                if (rem > 0 && rem < (size_t)frame_size_) {
                    // Pad the last partial block with the held sample fading linearly to zero.
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
        } // tail_drain_should_flush_input
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

            if (srv_guard && resonance::voice_may_convolve_source(get_voice_source_handle(), current_source_handle))
                produced |= _drain_shared_reflection_tail(srv_guard);

            if (current_source_handle >= 0 && srv_guard && srv_guard->is_pathing_enabled() &&
                path_processor.get_tail_size_samples() > 0 && sa_path_out_buffer.data && sa_path_out_buffer.data[0] &&
                sa_path_out_buffer.data[1]) {
                if (path_processor.process_tail(sa_path_out_buffer)) {
                    for (int i = 0; i < frame_size_; i++) {
                        if (sa_final_mix_buffer.data[0])
                            sa_final_mix_buffer.data[0][i] += sa_path_out_buffer.data[0][i];
                        if (direct_out_channels_ >= 2 && sa_final_mix_buffer.data[1])
                            sa_final_mix_buffer.data[1][i] += sa_path_out_buffer.data[1][i];
                    }
                    produced = true;
                }
            }
            if (path_processor.get_tail_size_samples() <= 0)
                pathing_tail_have_params_ = false;

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
    } // gate.can_run_ipl_tail

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
    const resonance::TailDrainEosPullPlan eos_pull_plan_end = eos_pull_plan(gate.can_run_ipl_tail, srv_guard);
    const bool eos_actively_pullable = resonance::tail_drain_eos_actively_pullable(eos_pull_plan_end);
    int64_t grace_remaining = tail_grace_blocks_remaining_.load(std::memory_order_acquire);
    if (resonance::tail_grace_end_early(produced_any, to_copy == 0, output_ring_reverb_l.get_available_read() == 0,
                                        last_mix_out_valid_, last_mix_out_l_, last_mix_out_r_, eos_actively_pullable,
                                        grace_remaining)) {
        tail_grace_blocks_remaining_.store(0, std::memory_order_release);
        grace_remaining = 0;
    }
    if (!produced_any && to_copy == 0) {
        if (grace_remaining > 0) {
            tail_grace_blocks_remaining_.store(grace_remaining - 1, std::memory_order_release);
            grace_remaining -= 1;
        }
    }
    const bool drained = resonance::tail_drain_complete(to_copy == 0, output_ring_reverb_l.get_available_read() == 0,
                                                        grace_remaining, eos_actively_pullable);
    if (frames > 0) {
        apply_playback_host_fades(buffer, frames);
        last_mix_out_l_ = buffer[frames - 1].left;
        last_mix_out_r_ = buffer[frames - 1].right;
        last_mix_out_valid_ = true;
    }
    // The player emits finished at dry end of stream and stops the node once this flag is set.
    if (gate.force_drain_complete || drained)
        tail_drain_complete_.store(true, std::memory_order_release);
    return resonance::tail_drain_mix_return_frames(frames);
}

int32_t ResonanceStreamPlayback::_mix_passthrough_pre_steam(AudioFrame* buffer, int32_t frames, const Vector2* src,
                                                            int32_t samples_read, bool base_playing) {
    // Audio thread: never creates or tears down IPL objects; main-thread prewarm must run first.
    const float v = owner_effective_volume_linear(owner_player());
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
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (srv && resonance::geometry_gate_should_pause_input_ingest(current_source_handle,
                                                                  srv->is_source_spatial_output_ready(current_source_handle)))
        return;

    int dec_i = 0;
    const bool eos_input_tail = samples_read > 0 && !base_playing;
    const int eos_taper_w =
        eos_input_tail ? std::min(samples_read, resonance::kEosInputEndTaperMaxSamples) : 0;
    // Crossfade only after a partial mix_audio chunk; on full buffers it would warp a continuous signal.
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
    // Pad short callbacks so the rings keep feeding whole blocks. Live input fades the last sample to zero;
    // end of stream pads zeros because eos_input_end_am_gain already tapered the real samples.
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
    ResonanceServer* srv = ResonanceServer::get_singleton();
    int blocks_processed_this_call = 0;
    // Cap live pump; EOS/tail drain (_mix_drain_zero_input_tails) stays uncapped.
    while (blocks_processed_this_call < kMaxBlocksPerMixCall && input_ring_l.get_available_read() >= frame_size_) {
        if (srv && resonance::spatial_audio_geometry_gate_should_hold_decode(current_source_handle,
                                                                             srv->is_source_spatial_output_ready(current_source_handle)))
            break;
        if (output_ring_l.get_available_write() >= frame_size_) {
            _process_steam_audio_block();
            instrumentation_blocks_processed.fetch_add(1, std::memory_order_relaxed);
            blocks_processed_this_call++;
        } else {
            instrumentation_output_blocked.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
    if (blocks_processed_this_call >= kMaxBlocksPerMixCall &&
        input_ring_l.get_available_read() >= static_cast<size_t>(frame_size_))
        instrumentation_pump_cap_reached.fetch_add(1, std::memory_order_relaxed);
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

    ResonanceServer* srv = ResonanceServer::get_singleton();
    bool holding_decode = false;
    if (srv && current_source_handle >= 0)
        holding_decode = resonance::spatial_audio_geometry_gate_should_hold_decode(current_source_handle,
                                                                                   srv->is_source_spatial_output_ready(current_source_handle));
    if (valid_copy < frames) {
        if (resonance::geometry_gate_output_reopen_hold_active(geometry_gate_was_holding_decode_, holding_decode,
                                                               valid_copy) &&
            last_mix_out_valid_) {
            resonance::fill_geometry_gate_reopen_hold(buffer, frames, valid_copy, last_mix_out_l_, last_mix_out_r_);
        } else {
            resonance::pad_output_with_cosine_underrun_fade(buffer, frames, valid_copy, last_mix_out_l_, last_mix_out_r_,
                                                            last_mix_out_valid_);
        }
    }
    geometry_gate_was_holding_decode_ = holding_decode;

    if (frames > 0) {
        apply_playback_host_fades(buffer, frames);
        last_mix_out_l_ = buffer[frames - 1].left;
        last_mix_out_r_ = buffer[frames - 1].right;
        last_mix_out_valid_ = true;
    }
}

int32_t ResonanceStreamPlayback::_mix_virtualized(AudioFrame* buffer, int32_t frames, float rate_scale,
                                                  const Ref<AudioStreamPlayback>& base) {
    if (!voice_was_virtual_) {
        _reset_voice_dsp_state();
        voice_was_virtual_ = true;
    }
    base->mix_audio(rate_scale, frames);
    for (int32_t i = 0; i < frames; i++) {
        buffer[i].left = 0.0f;
        buffer[i].right = 0.0f;
    }
    last_mix_out_l_ = 0.0f;
    last_mix_out_r_ = 0.0f;
    last_mix_out_valid_ = true;
    return frames;
}

void ResonanceStreamPlayback::_note_mix_timing(int32_t frames) {
    const auto now = std::chrono::steady_clock::now();
    if (mix_timing_started_) {
        const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - last_mix_time_).count();
        const uint64_t gap_us = elapsed_us > 0 ? static_cast<uint64_t>(elapsed_us) : 0;
        instrumentation_last_mix_gap_us_.store(gap_us, std::memory_order_relaxed);
        resonance::store_max(instrumentation_max_mix_gap_us_, gap_us);
        if (last_mix_block_us_ > 0 && mix_lateness_.note_mix(gap_us, last_mix_block_us_))
            instrumentation_late_mix_count.fetch_add(1, std::memory_order_relaxed);
    }
    mix_timing_started_ = true;
    last_mix_time_ = now;
    last_mix_block_us_ = 0;
    if (current_sample_rate > 0 && frames > 0) {
        last_mix_block_us_ = static_cast<uint64_t>((static_cast<double>(frames) * 1000000.0) /
                                                   static_cast<double>(current_sample_rate));
        instrumentation_expected_mix_gap_us_.store(last_mix_block_us_, std::memory_order_relaxed);
    }
}

int32_t ResonanceStreamPlayback::_mix(AudioFrame* buffer, float rate_scale, int32_t frames) {
    const auto owner_lease = owner_link_.lease();
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
    struct EmitterReflectionRound {
        ResonanceStreamPlayback* self = nullptr;
        explicit EmitterReflectionRound(ResonanceStreamPlayback* voice) : self(voice) {
            if (ResonancePlayer* owner = self ? self->owner_player() : nullptr)
                owner->reflection_round_enter(self);
        }
        ~EmitterReflectionRound() {
            if (ResonancePlayer* owner = self ? self->owner_player() : nullptr)
                owner->reflection_round_leave(self);
        }
    } emitter_reflection_round(this);
    if (base_playback.is_null())
        return 0;
    // Silence until the first spatial params arrive, without advancing the decoder.
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
    instrumentation_mix_call_count.fetch_add(1, std::memory_order_relaxed);
    _note_mix_timing(frames);
    _sync_params();

    ResonanceServer* srv_guard = ResonanceServer::get_singleton();
    // With an automatic frame size, a Godot mix length that differs from the Steam Audio frame size triggers a reinit.
    if (srv_guard && srv_guard->is_initialized() && frames > 0 && srv_guard->get_audio_frame_size_was_auto()) {
        const int srv_fs = srv_guard->get_audio_frame_size();
        if (srv_fs > 0 && frames != srv_fs) {
            srv_guard->request_reinit_with_frame_size(frames);
        }
    }
    if (is_initialized && srv_guard && srv_guard->is_initialized() && context != srv_guard->get_context_handle())
        steam_context_stale_.store(true, std::memory_order_release);

    // While the geometry gate is closed, only drain the output rings; do not advance the decoder.
    if (srv_guard && srv_guard->is_initialized() &&
        resonance::geometry_gate_should_hold_decoder_advance(current_source_handle,
                                                             srv_guard->is_source_spatial_output_ready(current_source_handle))) {
        _mix_emit_output_frames(buffer, frames, true);
        return frames;
    }

    // Once the stream ends the voice leaves through the normal tail drain, whose rings are already empty.
    if (params_current.virtualized) {
        const Ref<AudioStreamPlayback> base_guard = base_playback;
        if (base_guard.is_valid() && base_guard->is_playing())
            return _mix_virtualized(buffer, frames, rate_scale, base_guard);
    }
    voice_was_virtual_ = false;

    // No IR and no tail yet: consuming mix_audio here drops the transient on the reverb miss path.
    if (srv_guard && srv_guard->is_initialized() && params_current.enable_reverb) {
        const int32_t owned_source = get_voice_source_handle();
        const bool may_convolve = resonance::voice_may_convolve_source(owned_source, current_source_handle);
        IPLReflectionEffectParams fetched_params{};
        const bool fetch_ok = may_convolve && srv_guard->fetch_reverb_params(current_source_handle, fetched_params, nullptr);
        ResonancePlayer* owner = owner_player();
        const bool tail_applyable = owner && owner->shared_reflection_tail_applyable(srv_guard);
        if (resonance::reflection_decoder_should_wait_for_first_params(params_current.enable_reverb,
                                                                       params_current.enable_reflections, may_convolve, fetch_ok,
                                                                       tail_applyable)) {
            for (int32_t i = 0; i < frames; i++) {
                buffer[i].left = 0.0f;
                buffer[i].right = 0.0f;
            }
            last_mix_out_l_ = 0.0f;
            last_mix_out_r_ = 0.0f;
            last_mix_out_valid_ = true;
            return frames;
        }
    }

    // Strong ref so teardown cannot drop base_playback mid-call.
    const Ref<AudioStreamPlayback> base_guard = base_playback;
    if (base_guard.is_null())
        return 0;
    PackedVector2Array mixed_frames = base_guard->mix_audio(rate_scale, frames);
    int32_t samples_read = static_cast<int32_t>(mixed_frames.size());
    if (samples_read < frames)
        base_short_read_.store(true, std::memory_order_release);
    // At end of stream a full but silent buffer counts as zero input; a non-silent last buffer is kept.
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

    // Drop a stale host fade-out unless a real stop tail is running.
    if (playback_host_fade_out_remaining_ > 0) {
        const bool sr = stop_requested_.load(std::memory_order_acquire);
        const bool fade_in_active = resonance::kPlaybackHostFadeInEnabled &&
                                    playback_host_fade_in_total_samples_ > 0 &&
                                    playback_host_fade_in_elapsed_ < playback_host_fade_in_total_samples_;
        if (resonance::playback_host_fade_out_should_clear_stale(sr, base_guard->is_playing(), samples_read,
                                                                 fade_in_active)) {
            playback_host_fade_out_remaining_ = 0;
        }
    }

    _mix_ingest_decoder_samples(mixed_frames.ptr(), samples_read, frames, base_guard->is_playing());
    _mix_pump_available_steam_blocks();
    _mix_emit_output_frames(buffer, frames, true);
    return frames;
}
