#include "resonance_constants.h"
#include "resonance_log.h"
#include "resonance_math.h"
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
/// EOS pad gain: cosine half-window (1→0) vs linear; smoother derivative at the silence handoff (less click/step).
inline float input_ring_eos_pad_gain(int k, int pad_n, bool eos_pad) {
    if (!eos_pad || pad_n <= 0) {
        return 1.0f;
    }
    if (pad_n <= 1) {
        return 0.0f;
    }
    const float t = static_cast<float>(k) / static_cast<float>(pad_n - 1);
    constexpr float k_pi = 3.14159265358979323846f;
    return 0.5f * (1.0f + std::cos(k_pi * t));
}

/// Linear fade from full gain at k=0 (continuous with the sample before the padded region) to 0 at k=rem-1.
/// `1-(k+1)/rem` incorrectly scales the first padded sample by (rem-1)/rem and steps vs the previous callback tail.
/// Upper bound for synthetic ring-underrun fades; remainder of the mix frame is silence (steep after ~fade_len).
inline int synthetic_eos_output_fade_length(int rem) {
    if (rem <= 0) {
        return 0;
    }
    return std::min(rem, resonance::kSyntheticEosOutputFadeMaxSamples);
}

inline float linear_pad_fade_hold_to_zero(int k, int rem) {
    if (rem <= 0) {
        return 0.0f;
    }
    if (rem == 1) {
        return 1.0f;
    }
    return static_cast<float>(rem - 1 - k) / static_cast<float>(rem - 1);
}

/// Cosine AM on the last `taper_len` samples of a real chunk (indices `n_real - taper_len` … `n_real - 1`).
inline float eos_input_end_am_gain(int sample_index, int n_real, int taper_len) {
    if (taper_len <= 0 || n_real <= 0)
        return 1.0f;
    const int eff = (taper_len < n_real) ? taper_len : n_real;
    const int start = n_real - eff;
    if (sample_index < start)
        return 1.0f;
    const int k = sample_index - start;
    return input_ring_eos_pad_gain(k, eff, true);
}

/// Godot does not apply AudioStreamPlayer3D volume to GDExtension AudioStreamPlayback::_mix buffers; match Volume + max_db here.
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

    // 1. Initialize Direct (always create Ambisonics Encode path for runtime switching)
    direct_processor.initialize(context, current_sample_rate, frame_size_, order, true, direct_out_channels_);

    // 2. Initialize Reflection (convolution or parametric)
    reflection_processor.initialize(context, current_sample_rate, frame_size_, order, refl_type, srv->get_max_reverb_duration(),
                                    srv->get_convolution_ir_max_samples());

    // 3. Initialize Path Processor (for pathing simulation)
    path_processor.initialize(context, current_sample_rate, frame_size_, order);
    // 4. Initialize Mixer Processor (for convolution ambisonic decode)
    mixer_processor.initialize(context, current_sample_rate, frame_size_, order);

    // 5. Allocate Buffers (direct path matches server speaker layout; path processor stays stereo)
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
    // Main-thread alloc before first _mix (reduces first-block latency vs lazy init on audio thread).
    if (is_initialized)
        return true;
    _lazy_init_steam_audio(0);
    return is_initialized;
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

void ResonanceStreamPlayback::_process_steam_audio_block() {
    auto t0 = std::chrono::steady_clock::now();

    // Process-entry guards: centralised null checks at audio hot path entry
    ResonanceServer* srv = ResonanceServer::get_singleton();
    if (!context || !srv || !srv->is_initialized())
        return;

    const float node_vol = owner_effective_volume_linear(owner_player_);
    if (!ipl_all_channel_ptrs_ok(sa_in_buffer, 2))
        return;
    if (!ipl_all_channel_ptrs_ok(sa_direct_out_buffer, direct_out_channels_))
        return;
    if (!ipl_all_channel_ptrs_ok(sa_path_out_buffer, 2))
        return;
    if (!ipl_all_channel_ptrs_ok(sa_final_mix_buffer, direct_out_channels_))
        return;

    // 1. Read RingBuffer
    input_ring_l.read(temp_process_buffer_l.data(), frame_size_);
    input_ring_r.read(temp_process_buffer_r.data(), frame_size_);

    // Pack decoded stereo into IPL interleaved buffers (two channels)
    memcpy(sa_in_buffer.data[0], temp_process_buffer_l.data(), frame_size_ * sizeof(float));
    memcpy(sa_in_buffer.data[1], temp_process_buffer_r.data(), frame_size_ * sizeof(float));

    // Input-start detection: delay processing until first non-zero sample to avoid ramp artifacts
    // when Godot sends incorrect params before playback actually starts.
    // Treat any non-zero sample as start-of-audio (fabs, not exact zero - denormals / tiny DC).
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

    // Pipeline keys off `current_source_handle` + server caches (retain held on main only).
    if (current_source_handle >= 0) {
        const bool spatial_ready = srv->is_spatial_audio_output_ready();
        if (!spatial_ready) {
            _zero_sa_final_mix();
        } else {
            float dbg_direct = 0.0f;
            float dbg_reverb = 0.0f;
            float dbg_path = 0.0f;

            instrumentation_last_pathing_sh_rms.store(0.0f, std::memory_order_relaxed);
            instrumentation_last_pathing_sh_energy.store(0.0f, std::memory_order_relaxed);
            instrumentation_last_pathing_out_rms.store(0.0f, std::memory_order_relaxed);
            instrumentation_last_pathing_order.store(-1, std::memory_order_relaxed);

            // Additional wet-input occlusion for baked REVERB (see PlaybackParameters::wet_occlusion_factor).
            // 1.0 = no extra damping (default for realtime / STATICSOURCE / STATICLISTENER or when the feature is off).
            const float wet_occ = resonance::sanitize_audio_float(params_current.wet_occlusion_factor);

            const IPLCoordinateSpace3 listener_cs = srv->get_current_listener_coords(); // seqlock snapshot; shared with reverb bus

            // Direct path
            int trans_type = params_current.direct_effect_transmission_type;
            bool hrtf_bilinear = params_current.direct_effect_hrtf_bilinear;
            direct_processor.process(
                params_current.use_ambisonics_encode,
                sa_in_buffer, sa_direct_out_buffer,
                params_current.attenuation,
                params_current.occlusion,
                params_current.transmission,
                params_current.air_absorption.data(),
                params_current.apply_air_absorption,
                params_current.directivity_value,
                params_current.apply_directivity,
                params_current.enable_direct,
                params_current.use_binaural,
                trans_type,
                hrtf_bilinear,
                params_current.spatial_blend,
                listener_cs,
                ResonanceUtils::to_ipl_vector3(params_current.source_position));
            // Reverb: dry `sa_in_buffer` into reflection effect; wet gain = reflections_mix × node volume × occlusion.
            bool reverb_to_player_output = false; // Only for Parametric/Hybrid
            const float refl_wet_output_gain = 1.0f;
            if (srv && !params_current.enable_reverb) {
                instrumentation_enable_reverb_false_blocks.fetch_add(1, std::memory_order_relaxed);
            }
            if (srv && params_current.enable_reverb) {
                IPLReflectionEffectParams reverb_params{};
                bool has_reverb = srv->fetch_reverb_params(current_source_handle, reverb_params);
                const int refl_type = srv->get_reflection_type();

                if (has_reverb) {
                    if (refl_type == resonance::kReflectionHybrid) {
                        if (params_current.reflections_eq[0] != 1.0f || params_current.reflections_eq[1] != 1.0f || params_current.reflections_eq[2] != 1.0f) {
                            reverb_params.eq[0] *= params_current.reflections_eq[0];
                            reverb_params.eq[1] *= params_current.reflections_eq[1];
                            reverb_params.eq[2] *= params_current.reflections_eq[2];
                        }
                        if (params_current.reflections_delay >= 0) {
                            reverb_params.delay = params_current.reflections_delay;
                        }
                    }

                    // Convolution (0) / TAN (3): Feed mixer only; Reverb Bus reads it.
                    // Parametric (1) / Hybrid (2): process_mix_direct and add to our output.
                    if (refl_type == resonance::kReflectionConvolution || refl_type == resonance::kReflectionTan) {
                        auto mixer_guard = srv->scoped_mixer_read();
                        IPLReflectionMixer mixer = mixer_guard.get();
                        if (mixer) {
                            const float curr_refl_mix = resonance::sanitize_audio_float(params_current.reflections_mix_level);
                            // Unity parity: Convolution/TAN feeds the shared mixer with reflectionsMixLevel only.
                            // Do not apply per-player volume (`node_vol`) on the feed - the reverb bus is global.
                            const float wet_extra = 1.0f;
                            const float conv_reverb_gain = curr_refl_mix;
                            dbg_reverb = conv_reverb_gain;
                            // Mono RMS of input for reverb-bus instrumentation (editor / template_debug only; see DEBUG_ENABLED in godot-cpp).
                            float input_rms = 0.0f;
#ifdef DEBUG_ENABLED
                            float sum_sq = 0.0f;
                            int nch = sa_in_buffer.numChannels;
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
#endif
                            const auto conv_apply_t0 = std::chrono::steady_clock::now();
                            const bool reflection_applied =
                                reflection_processor.process_mix(sa_in_buffer, reverb_params, mixer, prev_conv_reflections_mix_level_, curr_refl_mix, wet_extra,
                                                                 params_current.apply_air_absorption_to_wet, params_current.air_absorption);
                            const auto conv_apply_t1 = std::chrono::steady_clock::now();
                            if (reflection_applied) {
                                srv->record_convolution_reflection_apply_usec(static_cast<uint64_t>(
                                    std::chrono::duration_cast<std::chrono::microseconds>(conv_apply_t1 - conv_apply_t0).count()));
                                srv->record_convolution_feed(reverb_params.ir != nullptr, conv_reverb_gain, input_rms);
                                prev_conv_reflections_mix_level_ = curr_refl_mix;
                                srv->record_mixer_feed();
                                reflection_tail_params_ = reverb_params;
                                reflection_tail_have_params_ = true;
                                reflection_tail_wet_gain_ = resonance::sanitize_audio_float(refl_wet_output_gain);
                                reflection_tail_split_output_ = params_current.reverb_split_output;
                            } else {
                                instrumentation_conv_mix_failed_blocks.fetch_add(1, std::memory_order_relaxed);
                            }
                        } else {
                            instrumentation_conv_mixer_null_blocks.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else {
                        reverb_to_player_output = true;
                        const float parametric_mix_level = resonance::sanitize_audio_float(params_current.reflections_mix_level * wet_occ);
                        if (reflection_processor.process_mix_direct(sa_in_buffer, reverb_params, prev_parametric_reflections_mix_level_, parametric_mix_level,
                                                                    params_current.apply_air_absorption_to_wet, params_current.air_absorption)) {
                            prev_parametric_reflections_mix_level_ = parametric_mix_level;
                            reflection_tail_params_ = reverb_params;
                            reflection_tail_have_params_ = true;
                            reflection_tail_wet_gain_ = resonance::sanitize_audio_float(refl_wet_output_gain);
                            reflection_tail_split_output_ = params_current.reverb_split_output;
                        }
                    }
                } else if (reflection_tail_have_params_ &&
                           (refl_type == resonance::kReflectionParametric || refl_type == resonance::kReflectionHybrid)) {
                    // Stale fetch: keep parametric/hybrid effect stepping with last params to avoid wet “pumping”.
                    IPLReflectionEffectParams rp = reflection_tail_params_;
                    if (refl_type == resonance::kReflectionHybrid && params_current.reflections_delay >= 0)
                        rp.delay = params_current.reflections_delay;
                    reverb_to_player_output = true;
                    const float parametric_mix_level_stale = resonance::sanitize_audio_float(params_current.reflections_mix_level * wet_occ);
                    if (reflection_processor.process_mix_direct(sa_in_buffer, rp, prev_parametric_reflections_mix_level_, parametric_mix_level_stale,
                                                                params_current.apply_air_absorption_to_wet, params_current.air_absorption)) {
                        prev_parametric_reflections_mix_level_ = parametric_mix_level_stale;
                        reflection_tail_wet_gain_ = 1.0f;
                        reflection_tail_split_output_ = params_current.reverb_split_output;
                    }
                } else if (reflection_tail_have_params_ &&
                           (refl_type == resonance::kReflectionConvolution || refl_type == resonance::kReflectionTan)) {
                    // Conv/TAN: reuse last good params for one block when fetch briefly misses (avoids wet dropout clicks).
                    IPLReflectionEffectParams rp = reflection_tail_params_;
                    auto mixer_guard = srv->scoped_mixer_read();
                    IPLReflectionMixer mixer = mixer_guard.get();
                    if (mixer) {
                        const float curr_refl_mix = resonance::sanitize_audio_float(params_current.reflections_mix_level);
                        // Unity parity: shared mixer feed uses reflectionsMixLevel only (no per-player node volume).
                        const float wet_extra = 1.0f;
                        const float conv_reverb_gain = curr_refl_mix;
                        dbg_reverb = conv_reverb_gain;
                        float input_rms = 0.0f;
#ifdef DEBUG_ENABLED
                        float sum_sq = 0.0f;
                        int nch = sa_in_buffer.numChannels;
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
#endif
                        const auto conv_apply_t0 = std::chrono::steady_clock::now();
                        const bool reflection_applied =
                            reflection_processor.process_mix(sa_in_buffer, rp, mixer, prev_conv_reflections_mix_level_, curr_refl_mix, wet_extra,
                                                             params_current.apply_air_absorption_to_wet, params_current.air_absorption);
                        const auto conv_apply_t1 = std::chrono::steady_clock::now();
                        if (reflection_applied) {
                            srv->record_convolution_reflection_apply_usec(static_cast<uint64_t>(
                                std::chrono::duration_cast<std::chrono::microseconds>(conv_apply_t1 - conv_apply_t0).count()));
                            srv->record_convolution_feed(rp.ir != nullptr, conv_reverb_gain, input_rms);
                            prev_conv_reflections_mix_level_ = curr_refl_mix;
                            srv->record_mixer_feed();
                            reflection_tail_wet_gain_ = resonance::sanitize_audio_float(refl_wet_output_gain);
                            reflection_tail_split_output_ = params_current.reverb_split_output;
                        } else {
                            instrumentation_conv_mix_failed_blocks.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else {
                        instrumentation_conv_mixer_null_blocks.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    instrumentation_reverb_miss_blocks.fetch_add(1, std::memory_order_relaxed);
                    // Only the reflections wet path needs fetch_reverb_params; pathing-only (reflections_mix == 0) misses here are normal.
                    const float refl_mix_gate = resonance::sanitize_audio_float(params_current.reflections_mix_level);
                    if (refl_mix_gate > 0.0f) {
                        // Throttle repeated warnings: count misses; log only after kPlayerNoReverbWarnThreshold (reset after log).
                        ++no_reverb_warn_count;
                        if (no_reverb_warn_count > resonance::kPlayerNoReverbWarnThreshold) {
                            String player_label = "(unknown)";
                            if (owner_player_) {
                                player_label = owner_player_->get_name();
                                if (player_label.is_empty())
                                    player_label = "(unnamed ResonancePlayer)";
                            }
                            const int rt = srv->get_reflection_type();
                            String detail;
                            if (rt == resonance::kReflectionConvolution || rt == resonance::kReflectionTan) {
                                detail = "Convolution/TAN: worker reflection cache not ready yet, cache miss, source still attaching, or realtime reflections turned off for this source (mix/gating). Not related to baked probes.";
                            } else if (rt == resonance::kReflectionParametric || rt == resonance::kReflectionHybrid) {
                                detail = "Parametric/Hybrid: wait for RunReflections to populate params; for baked reverb also verify probes are baked and the source lies within probe influence / range.";
                            } else {
                                detail = "Check reflection mode and simulation state.";
                            }
                            String msg = String("Playback (`") + player_label + "`): No reflection effect params from simulation while reflections_mix > 0. " + detail;
                            ResonanceLog::warn(msg);
                            no_reverb_warn_count = 0;
                        }
                    } else {
                        no_reverb_warn_count = 0;
                    }
                }
            }

            // Apply Volume Ramping (Direct Only). Use mix level: enable_direct * direct_mix_level
            float target_direct = (params_current.enable_direct ? 1.0f : 0.0f) * params_current.direct_mix_level;
            if (!spatial_ready)
                target_direct = 0.0f;

            // Debug Sources: report direct-path block RMS after direct_mix ramp (closer to "what you hear" than a gain scalar).
            // Note: Convolution reflections are mixed on the reverb bus, so dbg_reverb here is not comparable to wet loudness at the listener.
            dbg_direct = 0.0f;

            // Apply to Direct (all speaker channels)
            for (int c = 0; c < direct_out_channels_; c++) {
                if (sa_direct_out_buffer.data[c])
                    resonance::apply_volume_ramp(prev_direct_weight, target_direct, frame_size_, sa_direct_out_buffer.data[c]);
            }
            prev_direct_weight = target_direct;

            // Compute direct RMS after ramp.
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

            // Mix Direct into final buffer
            for (int c = 0; c < direct_out_channels_; c++) {
                if (sa_direct_out_buffer.data[c] && sa_final_mix_buffer.data[c])
                    memcpy(sa_final_mix_buffer.data[c], sa_direct_out_buffer.data[c], frame_size_ * sizeof(float));
            }

            // Add Reverb to player output (Parametric/Hybrid only; Convolution feeds mixer, no fallback)
            // When reverb_split_output: write to reverb ring for separate bus; else mix into final.
            if (reverb_to_player_output && spatial_ready) {
                IPLAudioBuffer* reverb_buf = reflection_processor.get_direct_output_buffer();
                if (reverb_buf && reverb_buf->data) {
                    // Wet already scaled in reflection processor from ramped reflections_mix.
                    float refl_mix = resonance::sanitize_audio_float(refl_wet_output_gain);
                    dbg_reverb = resonance::sanitize_audio_float(params_current.reflections_mix_level * node_vol * wet_occ);
                    _add_reverb_to_output(reverb_buf, refl_mix, params_current.reverb_split_output, listener_cs);
                }
            }

            // Add Pathing (multi-path sound propagation around obstacles).
            // Pathing only when enable_reverb is true – pathing is indirect sound (reflections); requires reverb to be active.
            // Pathing wet is ramped on mono before Apply; distance is in baked path SH from the simulation.
            if (spatial_ready && srv && srv->is_pathing_enabled() && params_current.enable_reverb && params_current.pathing_mix_level > 0.0f) {
                srv->record_pathing_player_gate_enter();
                IPLPathEffectParams path_params{};
                bool use_pathing = srv->fetch_pathing_params(current_source_handle, path_params);
                if (use_pathing) {
                    path_params.listener = listener_cs;
                    if (!params_current.apply_hrtf_to_pathing) {
                        path_params.hrtf = nullptr;
                        path_params.binaural = IPL_FALSE;
                    }
                    // Cache params for EOS tail. Deep-copy SH coeffs so the pointer remains valid even if the server swaps caches.
                    pathing_tail_params_ = path_params;
                    pathing_tail_have_params_ = true;
                    if (path_params.shCoeffs && path_params.order >= 0) {
                        const int n = (path_params.order + 1) * (path_params.order + 1);
                        const int to_copy = std::min(n, static_cast<int>(pathing_tail_sh_coeffs_.size()));
                        for (int i = 0; i < to_copy; i++)
                            pathing_tail_sh_coeffs_[static_cast<size_t>(i)] = path_params.shCoeffs[i];
                        for (size_t i = static_cast<size_t>(to_copy); i < pathing_tail_sh_coeffs_.size(); i++)
                            pathing_tail_sh_coeffs_[i] = 0.0f;
                        pathing_tail_params_.shCoeffs = pathing_tail_sh_coeffs_.data();
                    } else {
                        for (size_t i = 0; i < pathing_tail_sh_coeffs_.size(); i++)
                            pathing_tail_sh_coeffs_[i] = 0.0f;
                        pathing_tail_params_.shCoeffs = pathing_tail_sh_coeffs_.data();
                    }
                    const int32_t path_order = path_params.order;
                    instrumentation_last_pathing_order.store(path_order, std::memory_order_relaxed);
                    if (path_params.shCoeffs && path_order >= 0) {
                        const int n = (path_order + 1) * (path_order + 1);
                        double sum_sq = 0.0;
                        for (int i = 0; i < n; i++) {
                            const double c = static_cast<double>(path_params.shCoeffs[i]);
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
                    const float path_out_rms = (frame_size_ > 0) ? std::sqrt(path_sum_sq / (2.0f * static_cast<float>(frame_size_))) : 0.0f;
                    instrumentation_last_pathing_out_rms.store(path_out_rms, std::memory_order_relaxed);
                    dbg_path = resonance::sanitize_audio_float(std::clamp(path_out_rms, 0.0f, 1.0f));
                    for (int i = 0; i < frame_size_; i++) {
                        if (sa_final_mix_buffer.data[0])
                            sa_final_mix_buffer.data[0][i] += sa_path_out_buffer.data[0][i];
                        if (direct_out_channels_ >= 2 && sa_final_mix_buffer.data[1])
                            sa_final_mix_buffer.data[1][i] += sa_path_out_buffer.data[1][i];
                    }
                    prev_pathing_mix_level_ = params_current.pathing_mix_level;
                    srv->record_pathing_player_applied();
                } else {
                    srv->record_pathing_player_fetch_miss();
                }
            } else {
                prev_pathing_mix_level_ = params_current.pathing_mix_level;
            }

            debug_signal_direct.store(dbg_direct, std::memory_order_relaxed);
            debug_signal_reverb.store(dbg_reverb, std::memory_order_relaxed);
            debug_signal_pathing.store(dbg_path, std::memory_order_relaxed);
        }
    } else {
        // Passthrough: no server source handle (decode-only path)
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

        // Reset mix ramps for passthrough / reattach.
        prev_direct_weight = 0.0f;
        prev_parametric_reflections_mix_level_ = 0.0f;
        prev_pathing_mix_level_ = 0.0f;
        // Conv path: `prev_conv_reflections_mix_level_ = -1` re-arms first-block behavior when handle returns.
        prev_conv_reflections_mix_level_ = -1.0f;
    }

    // Safety: clamp output to prevent NaN/overflow from processing bugs
    for (int c = 0; c < direct_out_channels_; c++) {
        if (!sa_final_mix_buffer.data[c])
            continue;
        for (int i = 0; i < frame_size_; i++)
            sa_final_mix_buffer.data[c][i] = std::clamp(sa_final_mix_buffer.data[c][i], -1.0f, 1.0f);
    }

    // Instrumentation: output RMS and silent-block detection (stereo fold-down; matches ring samples)
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
        if (reverb_ring_prev_valid_ && (std::abs(reverb_ring_prev_l_) > 1.0e-6f || std::abs(reverb_ring_prev_r_) > 1.0e-6f)) {
            const float v_rev = owner_effective_volume_linear(owner_player_);
            const float start_l = reverb_ring_prev_l_ * v_rev;
            const float start_r = reverb_ring_prev_r_ * v_rev;
            // pad_n<=1 makes input_ring_eos_pad_gain return 0 - use at least 2 so a single-frame callback fades correctly.
            const int32_t n = std::max(2, static_cast<int32_t>(frames));
            for (int32_t i = 0; i < frames; i++) {
                const float fade = input_ring_eos_pad_gain(static_cast<int>(i), n, true);
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
    const float v_rev = owner_effective_volume_linear(owner_player_);
    for (int32_t i = 0; i < to_read; i++) {
        buffer[i].left = temp_reverb_buffer_l[i] * v_rev;
        buffer[i].right = temp_reverb_buffer_r[i] * v_rev;
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
                    g = input_ring_eos_pad_gain(gi, reverb_ring_gap_fade_total_, true);
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
    // First-params gate: before ResonancePlayer has pushed the first spatial parameters,
    // params_current still holds defaults (listener-position, full gain), which would cause
    // an audible full-volume burst for one block. Silence output until parameters arrive.
    //
    // Do **not** call `base_playback->mix_audio()` while gated. Historically we advanced the
    // decoder and discarded samples to avoid a stuck `finished` signal; that discards the
    // start of the stream and adds a constant delay vs. native `AudioStreamPlayer(3D)` in
    // A/B recordings. `update_parameters` runs on the same frame as `play()` in normal
    // scenes; the first `mix_audio` after the gate opens then reads from sample 0, matching
    // the native player timeline. Natural EOS is reached after params sync and normal mixing.
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
    // If Steam's DSP `frame_size` is larger than Godot's per-callback `frames` (e.g. server 1024 vs mix 512),
    // the input ring must fill an extra block before the first `_process_steam_audio_block` - a constant
    // ~one-buffer delay vs native `AudioStreamPlayer3D`. The reverb bus effect already calls
    // `request_reinit_with_frame_size` when sizes diverge; the player path must do the same so dry output
    // and capture A/B stay aligned. Reinit is async (main thread); after it lands, ipl clients re-init with
    // the snapped size from the observed `frames` value.
    if (srv_guard && srv_guard->is_initialized() && frames > 0 && srv_guard->get_audio_frame_size_was_auto()) {
        const int srv_fs = srv_guard->get_audio_frame_size();
        if (srv_fs > 0 && frames != srv_fs) {
            srv_guard->request_reinit_with_frame_size(frames);
        }
    }
    if (is_initialized && srv_guard && srv_guard->is_initialized() && context != srv_guard->get_context_handle())
        _cleanup_steam_audio();

    // Keep a strong ref for the mix call so teardown on another thread cannot drop base_playback mid-call.
    const Ref<AudioStreamPlayback> base_guard = base_playback;
    if (base_guard.is_null())
        return 0;
    PackedVector2Array mixed_frames = base_guard->mix_audio(rate_scale, frames);
    int32_t samples_read = static_cast<int32_t>(mixed_frames.size());
    // Some stream backends report `is_playing()==false` on EOS but still return one more **full**
    // buffer - sometimes all zeros (harmless "phantom" mix). Old code set `samples_read = 0` for
    // every full buffer when `!is_playing()`, which **discarded the last real samples** and caused
    // a hard jump to silence vs native `AudioStreamPlayer` (audible click / vertical waveform cut).
    // Only collapse to zero-input when that full buffer is actually silent.
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
    // Godot normally returns exactly `frames` samples. If a backend returns more, writing past
    // `buffer` would corrupt memory (defensive guard).
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

    // When no input: drain direct effect tail and any remaining output for clean fade-out
    if (samples_read == 0) {
        prev_mix_had_partial_input_pad_ = false;
        prev_mix_had_eos_tapered_input_pad_ = false;
        if (!is_initialized)
            return 0;
        if (!srv_guard || !srv_guard->is_initialized() || context != srv_guard->get_context_handle()) {
            _cleanup_steam_audio();
            return 0;
        }
        // Arm the tail grace cap on the first transition into this branch so a stuck IPL
        // effect handle (or any other pathological state) cannot keep the playback alive
        // forever. Budget = max_reverb_duration in audio blocks, plus a small safety margin.
        if (tail_grace_blocks_remaining_.load(std::memory_order_acquire) < 0) {
            const int sr = current_sample_rate > 0 ? current_sample_rate : 48000;
            const int fs = frame_size_ > 0 ? frame_size_ : resonance::kGodotDefaultFrameSize;
            const float max_reverb_duration = srv_guard->get_max_reverb_duration();
            const int64_t blocks = (int64_t)((max_reverb_duration * (float)sr) / (float)fs) + 8;
            tail_grace_blocks_remaining_.store(blocks > 0 ? blocks : 8, std::memory_order_release);
        }
        // Dry ended with a partial block still in the input ring: the samples_read > 0 path only
        // calls _process_steam_audio_block when available_read >= frame_size_. Skipping the final
        // partial frame starves one convolution Apply (and can leave the reflection mixer empty for
        // one bus tick) before GetTail - audible as a short dropout.
        while (input_ring_l.get_available_read() >= (size_t)frame_size_ &&
               output_ring_l.get_available_write() >= (size_t)frame_size_) {
            _process_steam_audio_block();
            instrumentation_blocks_processed.fetch_add(1, std::memory_order_relaxed);
        }
        {
            const size_t rem = input_ring_l.get_available_read();
            if (rem > 0 && rem < (size_t)frame_size_) {
                // Pad the final partial block to a full process frame. Zero-fill caused a sharp
                // drop; DC-hold sounded stepped. Linear fade of the pad tail toward zero smooths the
                // last streaming frame before GetTail.
                if (input_ring_r.get_available_read() == rem &&
                    rem <= temp_process_buffer_l.size() && rem <= temp_process_buffer_r.size() &&
                    (size_t)frame_size_ <= temp_process_buffer_l.size() &&
                    (size_t)frame_size_ <= temp_process_buffer_r.size()) {
                    input_ring_l.read(temp_process_buffer_l.data(), rem);
                    input_ring_r.read(temp_process_buffer_r.data(), rem);
                    const float hold_l = temp_process_buffer_l[rem - 1];
                    const float hold_r = temp_process_buffer_r[rem - 1];
                    const size_t pad_count = (size_t)frame_size_ - rem;
                    // Linear fade of pad region from last sample toward zero. DC-hold fed a non-zero
                    // constant into the convolution tail of the same frame as real audio, which can
                    // sound stepped/choppy; tapering to zero smooths the streaming→tail handoff.
                    for (size_t k = 0; k < pad_count; k++) {
                        const float fade = linear_pad_fade_hold_to_zero(static_cast<int>(k), static_cast<int>(pad_count));
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
                            const float fade = linear_pad_fade_hold_to_zero(static_cast<int>(k), static_cast<int>(pad_count));
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
                        // Conv/TAN EOS: feed Apply with silence so the shared mixer advances (no separate GetTail on this path).
                        if (!conv_reverb_eos_silence_apply_done_)
                            conv_reverb_eos_silence_apply_done_ = true;
                        if (sa_in_buffer.data[0])
                            memset(sa_in_buffer.data[0], 0, static_cast<size_t>(frame_size_) * sizeof(float));
                        if (sa_in_buffer.data[1])
                            memset(sa_in_buffer.data[1], 0, static_cast<size_t>(frame_size_) * sizeof(float));

                        const float node_vol_eos = owner_effective_volume_linear(owner_player_);
                        const float wet_occ_eos = resonance::sanitize_audio_float(params_current.wet_occlusion_factor);
                        const float curr_refl_mix_eos = resonance::sanitize_audio_float(params_current.reflections_mix_level);
                        const float wet_extra_eos = resonance::sanitize_audio_float(node_vol_eos * wet_occ_eos);

                        if (reflection_processor.process_mix(sa_in_buffer, rp, eos_mixer, prev_conv_reflections_mix_level_,
                                                             curr_refl_mix_eos, wet_extra_eos,
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

                    // Keep last mix level (no new ramp). Silent input means only tail decay remains.
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
            // Decrement grace budget for every produced tail block.
            int64_t remaining = tail_grace_blocks_remaining_.load(std::memory_order_acquire);
            if (remaining > 0)
                tail_grace_blocks_remaining_.store(remaining - 1, std::memory_order_release);
        }
        int available = (int)output_ring_l.get_available_read();
        int to_copy = (frames < available) ? frames : available;
        const float v_tail = owner_effective_volume_linear(owner_player_);
        for (int i = 0; i < to_copy; i++) {
            float l, r;
            output_ring_l.read(&l, 1);
            output_ring_r.read(&r, 1);
            buffer[i].left = l * v_tail;
            buffer[i].right = r * v_tail;
        }
        if (to_copy < frames) {
            const float tail_l = (to_copy > 0) ? buffer[to_copy - 1].left
                                               : (last_mix_out_valid_ ? last_mix_out_l_ : 0.0f);
            const float tail_r = (to_copy > 0) ? buffer[to_copy - 1].right
                                               : (last_mix_out_valid_ ? last_mix_out_r_ : 0.0f);
            const int rem = frames - to_copy;
            const int fade_len = synthetic_eos_output_fade_length(rem);
            const int pad_n = std::max(2, fade_len);
            for (int i = to_copy; i < frames; i++) {
                const int idx = i - to_copy;
                const float g = (idx < fade_len) ? input_ring_eos_pad_gain(idx, pad_n, true) : 0.0f;
                buffer[i].left = tail_l * g;
                buffer[i].right = tail_r * g;
            }
        }
        // Log pattern: dozens of tail callbacks in <500ms with `to_copy==0`, `produced_any==0`, grace counting
        // down - each outputs a full 512-sample buffer. After one synthetic fade, `last_mix_out_*` is ~0 but
        // we kept burning ~195 grace blocks on near-silence (sounds flat/pumpy vs a clean EOS).
        if (to_copy == 0 && !produced_any && last_mix_out_valid_ &&
            std::abs(last_mix_out_l_) < 1.0e-5f && std::abs(last_mix_out_r_) < 1.0e-5f &&
            !has_active_tail_residue()) {
            tail_grace_blocks_remaining_.store(0, std::memory_order_release);
        }
        // When fully drained, return 0 to signal EOS to AudioServer. Some Godot builds/backends
        // do not reliably poll _is_playing() for custom playbacks, so "always return frames"
        // can prevent `finished` from ever emitting.
        //
        // IPL tail size can be non-zero while we cannot advance wet - still finish so `finished` fires.
        // Do not OR in `!produced_any` alone: when the output ring is empty the tail loop often produces
        // nothing (`produced_any` false) while we still must deliver the synthetic fade from `last_mix_out_*`
        // in the `to_copy < frames` branch below. The old `!produced_any` forced `drained` true, `return 0`,
        // and Godot discarded a full buffer - audible step vs the previous steam callback.
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
        // Do not "naturally" end the playback here (return 0) because Godot would emit `finished`
        // at the wet/tail end. `ResonancePlayer` emits `finished` at dry-EOS and explicitly stops
        // the node once tail drain completes.
        if (drained)
            tail_drain_complete_.store(true, std::memory_order_release);
        return frames;
    }

    if (!is_initialized) {
        _lazy_init_steam_audio(0);
        // If init failed (e.g. out of memory or no context), fallback to passthrough
        if (!is_initialized) {
            const float v = owner_effective_volume_linear(owner_player_);
            const bool eos_pt = samples_read > 0 && !base_guard->is_playing();
            const int eos_tw =
                eos_pt ? std::min(samples_read, resonance::kEosInputEndTaperMaxSamples) : 0;
            for (int i = 0; i < samples_read; i++) {
                const float g_am = eos_input_end_am_gain(i, samples_read, eos_tw);
                buffer[i].left = mixed_frames[i].x * v * g_am;
                buffer[i].right = mixed_frames[i].y * v * g_am;
            }
            if (samples_read < frames) {
                const int pad_n = frames - samples_read;
                if (eos_pt) {
                    for (int i = samples_read; i < frames; i++) {
                        buffer[i].left = 0.0f;
                        buffer[i].right = 0.0f;
                    }
                } else {
                    const float raw_last_l = (samples_read > 0) ? mixed_frames[samples_read - 1].x : 0.0f;
                    const float raw_last_r = (samples_read > 0) ? mixed_frames[samples_read - 1].y : 0.0f;
                    const float last_l = raw_last_l * v;
                    const float last_r = raw_last_r * v;
                    for (int i = samples_read; i < frames; i++) {
                        const int k = i - samples_read;
                        const float fade = linear_pad_fade_hold_to_zero(k, pad_n);
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
    }

    // Host fade-out is armed by `_stop` / `request_soft_stop`. It must only run while we are actually draining
    // a stop tail (`stop_requested_` and no live decoder). Rapid play/stop can leave `stop_requested_` true across
    // one `_mix` before `_start` on the next clip, so the countdown must clear when we are clearly not in that tail.
    //
    // EOS partial: `is_playing()==false` while `samples_read>0` is normal (short one-shots, last decode chunk).
    // Clear stale fade-out when we have decoder samples but the stream already reports finished - not the
    // samples_read==0 silence-only tail path where the fade-out is intended. Trade-off: the last partial block
    // after an explicit soft stop is no longer host-ducked; tail/reverb still drain as before.
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

    const Vector2* src_ptr = mixed_frames.ptr();
    int dec_i = 0;
    const bool eos_input_tail = samples_read > 0 && !base_guard->is_playing();
    const int eos_taper_w =
        eos_input_tail ? std::min(samples_read, resonance::kEosInputEndTaperMaxSamples) : 0;
    // Only crossfade after a partial `mix_audio` chunk - otherwise every full buffer warps a continuous sine.
    const bool do_input_chunk_crossfade = input_ring_tail_valid_ && samples_read > 0 &&
                                          prev_mix_had_partial_input_pad_ && !prev_mix_had_eos_tapered_input_pad_;
    if (do_input_chunk_crossfade) {
        const int K = std::min(samples_read, resonance::kInputRingChunkCrossfadeSamples);
        for (; dec_i < K; dec_i++) {
            const float g = static_cast<float>(dec_i + 1) / static_cast<float>(K);
            const float g_am = eos_input_end_am_gain(dec_i, samples_read, eos_taper_w);
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
        const float g_am = eos_input_end_am_gain(dec_i, samples_read, eos_taper_w);
        float l = src_ptr[dec_i].x * g_am;
        float r = src_ptr[dec_i].y * g_am;
        if (input_ring_l.get_available_write() > 0) {
            input_ring_l.write(&l, 1);
            input_ring_r.write(&r, 1);
        } else {
            instrumentation_input_dropped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // If the decoder produced fewer samples than requested (common for some stream backends and for
    // one-shots near start/stop), pad this callback to a full Godot frame. We must still fill one full
    // Pad short decode callbacks so downstream always consumes full `frame_size_` from rings per block.
    // Live: linear hold-last pad. EOS: zeros - dry tail is tapered via `eos_input_end_am_gain` on real samples
    // so the waveform keeps oscillating instead of a single ramp/hill.
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
                const float fade = linear_pad_fade_hold_to_zero(k, pad_n);
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
        (samples_read < frames) && !base_guard->is_playing();

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

    // Always report `frames` produced, even if mix_audio() returned fewer (end-of-stream tail).
    // Returning samples_read < frames here would cause AudioServer to detach the playback before
    // we get a chance to drain the reverb / pathing tail in the samples_read==0 branch on the
    // next call. Zero-padding the unused tail of `buffer` is safe and the tail-drain branch will
    // produce real wet audio once the dry signal has fully ended.
    int samples_to_output = frames;
    int available = (int)output_ring_l.get_available_read();
    int valid_copy = (samples_to_output < available) ? samples_to_output : available;
    if (valid_copy < samples_to_output) {
        instrumentation_output_underrun.fetch_add((uint64_t)(samples_to_output - valid_copy), std::memory_order_relaxed);
    }

    const float v_out = owner_effective_volume_linear(owner_player_);
    for (int i = 0; i < valid_copy; i++) {
        float l = 0.0f;
        float r = 0.0f;
        output_ring_l.read(&l, 1);
        output_ring_r.read(&r, 1);
        buffer[i].left = l * v_out;
        buffer[i].right = r * v_out;
    }

    if (valid_copy < samples_to_output) {
        const float tail_l = (valid_copy > 0) ? buffer[valid_copy - 1].left
                                              : (last_mix_out_valid_ ? last_mix_out_l_ : 0.0f);
        const float tail_r = (valid_copy > 0) ? buffer[valid_copy - 1].right
                                              : (last_mix_out_valid_ ? last_mix_out_r_ : 0.0f);
        const int rem = samples_to_output - valid_copy;
        const int fade_len = synthetic_eos_output_fade_length(rem);
        const int pad_n = std::max(2, fade_len);
        for (int i = valid_copy; i < samples_to_output; i++) {
            const int idx = i - valid_copy;
            const float g = (idx < fade_len) ? input_ring_eos_pad_gain(idx, pad_n, true) : 0.0f;
            buffer[i].left = tail_l * g;
            buffer[i].right = tail_r * g;
        }
    }

    if (frames > 0) {
        apply_playback_host_fades(buffer, frames);
        last_mix_out_l_ = buffer[frames - 1].left;
        last_mix_out_r_ = buffer[frames - 1].right;
        last_mix_out_valid_ = true;
    }
    return samples_to_output;
}
