#include "resonance_processor_direct.h"
#include "resonance_log.h"
#include "resonance_server.h"
#include "resonance_speaker_layout.h"
#include "resonance_utils.h"
#include <cmath>
#include <cstring>

namespace godot {

ResonanceDirectProcessor::~ResonanceDirectProcessor() { cleanup(); }

void ResonanceDirectProcessor::initialize(IPLContext p_context, int p_sample_rate, int p_frame_size, int p_ambisonic_order, bool p_use_ambisonics_encode,
                                          int p_speaker_channels) {
    if (init_flags != DirectInitFlags::NONE)
        return;

    if (!p_context) {
        ResonanceLog::error("DirectProcessor: Context is null!");
        return;
    }

    context = p_context;
    frame_size = p_frame_size;
    ambisonic_order = p_ambisonic_order;
    use_ambisonics_encode = p_use_ambisonics_encode;
    speaker_channels = resonance::clamp_direct_speaker_channels(p_speaker_channels);

    IPLAudioSettings audioSettings{};
    audioSettings.samplingRate = p_sample_rate;
    audioSettings.frameSize = frame_size;

    // 1. Direct Effect (Mono physics)
    IPLDirectEffectSettings directSettings{};
    directSettings.numChannels = 1;

    if (iplDirectEffectCreate(context, &audioSettings, &directSettings, &direct_effect) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("DirectProcessor: Failed to create IPLDirectEffect");
        return;
    }
    init_flags |= DirectInitFlags::DIRECT_EFFECT;

    // 2. Binaural Effect
    ResonanceServer* srv = ResonanceServer::get_singleton();
    hrtf_handle = (srv) ? srv->get_hrtf_handle() : nullptr;

    if (hrtf_handle) {
        IPLBinauralEffectSettings binSettings{};
        binSettings.hrtf = hrtf_handle;
        if (iplBinauralEffectCreate(context, &audioSettings, &binSettings, &binaural_effect) == IPL_STATUS_SUCCESS) {
            init_flags |= DirectInitFlags::BINAURAL_EFFECT;
        } else {
            ResonanceLog::error("DirectProcessor: Failed to create IPLBinauralEffect");
        }
    } else {
        ResonanceLog::warn("DirectProcessor: No HRTF found. Binaural disabled.");
    }

    // 3. Panning Effect (multi-channel layout when not using HRTF)
    IPLPanningEffectSettings panSettings{};
    panSettings.speakerLayout = resonance::speaker_layout_for_channel_count(speaker_channels);
    if (iplPanningEffectCreate(context, &audioSettings, &panSettings, &panning_effect) == IPL_STATUS_SUCCESS) {
        init_flags |= DirectInitFlags::PANNING_EFFECT;
    } else {
        ResonanceLog::error("DirectProcessor: Failed to create IPLPanningEffect");
    }

    // 4. Optional: Ambisonics Encode + Binaural path (for mixing scenarios)
    if (use_ambisonics_encode && hrtf_handle) {
        IPLAmbisonicsEncodeEffectSettings encSettings{};
        encSettings.maxOrder = ambisonic_order;
        if (iplAmbisonicsEncodeEffectCreate(context, &audioSettings, &encSettings, &ambisonics_encode_effect) != IPL_STATUS_SUCCESS) {
            ResonanceLog::warn("DirectProcessor: Ambisonics Encode effect creation failed.");
        }
        IPLAmbisonicsBinauralEffectSettings ambBinSettings{};
        ambBinSettings.hrtf = hrtf_handle;
        ambBinSettings.maxOrder = ambisonic_order;
        if (iplAmbisonicsBinauralEffectCreate(context, &audioSettings, &ambBinSettings, &ambisonics_binaural_effect) != IPL_STATUS_SUCCESS) {
            ResonanceLog::warn("DirectProcessor: Ambisonics Binaural effect creation failed.");
            if (ambisonics_encode_effect) {
                iplAmbisonicsEncodeEffectRelease(&ambisonics_encode_effect);
                ambisonics_encode_effect = nullptr;
            }
        }
        if (ambisonics_encode_effect && ambisonics_binaural_effect) {
            int num_ambi_channels = (ambisonic_order + 1) * (ambisonic_order + 1);
            if (iplAudioBufferAllocate(context, num_ambi_channels, frame_size, &internal_ambi_buffer) == IPL_STATUS_SUCCESS && internal_ambi_buffer.data) {
                init_flags |= DirectInitFlags::AMBISONICS_ENCODE;
                if (iplAudioBufferAllocate(context, 2, frame_size, &internal_hoa_stereo_scratch) != IPL_STATUS_SUCCESS ||
                    !internal_hoa_stereo_scratch.data || !internal_hoa_stereo_scratch.data[0] || !internal_hoa_stereo_scratch.data[1]) {
                    ResonanceLog::warn("DirectProcessor: HOA stereo scratch allocation failed; spatial_blend blend with Ambisonics path unavailable.");
                } else {
                    init_flags |= DirectInitFlags::HOA_BINAURAL_STEREO_SCRATCH;
                }
                // Non-HRTF surround: decode Ambisonics to speakers (optional; stereo uses cheaper panning path).
                if (speaker_channels > 2) {
                    IPLAmbisonicsPanningEffectSettings ambiPanSettings{};
                    ambiPanSettings.speakerLayout = resonance::speaker_layout_for_channel_count(speaker_channels);
                    ambiPanSettings.maxOrder = ambisonic_order;
                    if (iplAmbisonicsPanningEffectCreate(context, &audioSettings, &ambiPanSettings, &ambisonics_panning_effect) == IPL_STATUS_SUCCESS) {
                        init_flags |= DirectInitFlags::AMBISONICS_PANNING;
                    } else {
                        ResonanceLog::warn("DirectProcessor: Ambisonics Panning effect creation failed; using IPLPanningEffect for non-HRTF.");
                    }
                }
            } else {
                iplAmbisonicsEncodeEffectRelease(&ambisonics_encode_effect);
                iplAmbisonicsBinauralEffectRelease(&ambisonics_binaural_effect);
                ambisonics_encode_effect = nullptr;
                ambisonics_binaural_effect = nullptr;
            }
        }
    }

    // Binaural APIs expect a stereo output buffer; copy to FL/FR when the player uses a surround layout.
    if (speaker_channels > 2 && hrtf_handle && (binaural_effect || ambisonics_binaural_effect)) {
        if (iplAudioBufferAllocate(context, 2, frame_size, &internal_binaural_stereo_out) == IPL_STATUS_SUCCESS && internal_binaural_stereo_out.data &&
            internal_binaural_stereo_out.data[0] && internal_binaural_stereo_out.data[1]) {
            init_flags |= DirectInitFlags::BINAURAL_STEREO_SCRATCH;
        } else {
            ResonanceLog::warn("DirectProcessor: Binaural stereo scratch allocation failed; HRTF with surround direct_speaker_channels may be unstable.");
        }
    }

    // 5. Buffers (separate in/out for iplDirectEffectApply per Steam Audio reference implementations)
    if (iplAudioBufferAllocate(context, 1, frame_size, &internal_mono_buffer) != IPL_STATUS_SUCCESS ||
        !internal_mono_buffer.data) {
        ResonanceLog::error("DirectProcessor: Internal buffer allocation failed.");
        cleanup();
        return;
    }
    if (iplAudioBufferAllocate(context, 1, frame_size, &internal_direct_output) != IPL_STATUS_SUCCESS ||
        !internal_direct_output.data) {
        ResonanceLog::error("DirectProcessor: Direct output buffer allocation failed.");
        iplAudioBufferFree(context, &internal_mono_buffer);
        memset(&internal_mono_buffer, 0, sizeof(internal_mono_buffer));
        cleanup();
        return;
    }
    init_flags |= DirectInitFlags::BUFFERS;

    ResonanceLog::info("DirectProcessor initialized successfully.");
}

void ResonanceDirectProcessor::cleanup() {
    if (direct_effect) {
        iplDirectEffectRelease(&direct_effect);
        direct_effect = nullptr;
    }
    if (binaural_effect) {
        iplBinauralEffectRelease(&binaural_effect);
        binaural_effect = nullptr;
    }
    if (panning_effect) {
        iplPanningEffectRelease(&panning_effect);
        panning_effect = nullptr;
    }
    if (ambisonics_encode_effect) {
        iplAmbisonicsEncodeEffectRelease(&ambisonics_encode_effect);
        ambisonics_encode_effect = nullptr;
    }
    if (ambisonics_binaural_effect) {
        iplAmbisonicsBinauralEffectRelease(&ambisonics_binaural_effect);
        ambisonics_binaural_effect = nullptr;
    }
    if (ambisonics_panning_effect) {
        iplAmbisonicsPanningEffectRelease(&ambisonics_panning_effect);
        ambisonics_panning_effect = nullptr;
    }
    if (context && internal_binaural_stereo_out.data != nullptr) {
        iplAudioBufferFree(context, &internal_binaural_stereo_out);
    }
    memset(&internal_binaural_stereo_out, 0, sizeof(internal_binaural_stereo_out));

    if (context && internal_mono_buffer.data != nullptr) {
        iplAudioBufferFree(context, &internal_mono_buffer);
    }
    if (context && internal_direct_output.data != nullptr) {
        iplAudioBufferFree(context, &internal_direct_output);
    }
    if (context && internal_ambi_buffer.data != nullptr) {
        iplAudioBufferFree(context, &internal_ambi_buffer);
    }
    if (context && internal_hoa_stereo_scratch.data != nullptr) {
        iplAudioBufferFree(context, &internal_hoa_stereo_scratch);
    }
    memset(&internal_mono_buffer, 0, sizeof(internal_mono_buffer));
    memset(&internal_direct_output, 0, sizeof(internal_direct_output));
    memset(&internal_ambi_buffer, 0, sizeof(internal_ambi_buffer));
    memset(&internal_hoa_stereo_scratch, 0, sizeof(internal_hoa_stereo_scratch));
    hrtf_handle = nullptr;
    context = nullptr;
    init_flags = DirectInitFlags::NONE;
}

void ResonanceDirectProcessor::process(
    bool use_ambisonics_encode_path,
    const IPLAudioBuffer& in_buffer,
    IPLAudioBuffer& out_buffer,
    float attenuation, float occlusion, const float* transmission, const float* air_absorption, bool apply_air_absorption,
    float directivity_value, bool apply_directivity, bool apply_effect,
    bool use_binaural,
    int transmission_type,
    bool hrtf_interpolation_bilinear,
    float spatial_blend,
    const IPLCoordinateSpace3& listener_coords,
    const IPLVector3& source_pos) {

    // --- PERFORMANCE CRITICAL SECTION ---

    // 1. Validation (InitFlags guard: only process when fully initialized, avoids partial-init crashes)
    bool has_spatialization = (init_flags & DirectInitFlags::BINAURAL_EFFECT) || (init_flags & DirectInitFlags::PANNING_EFFECT);
    bool init_ok = (init_flags & DirectInitFlags::DIRECT_EFFECT) && (init_flags & DirectInitFlags::BUFFERS) && has_spatialization;
    bool buffers_ok = context && in_buffer.data && out_buffer.data && internal_mono_buffer.data && internal_direct_output.data;

    // Passthrough fallback: when init failed, pass input through instead of silence
    if (!init_ok || !buffers_ok) {
        if (in_buffer.data && out_buffer.data) {
            int ch_in = in_buffer.numChannels;
            int ch_out = out_buffer.numChannels;
            int ch = (ch_in < ch_out) ? ch_in : ch_out;
            for (int i = 0; i < ch && in_buffer.data[i] && out_buffer.data[i]; i++) {
                memcpy(out_buffer.data[i], in_buffer.data[i], frame_size * sizeof(float));
            }
            for (int i = ch; i < ch_out && out_buffer.data[i]; i++) {
                memset(out_buffer.data[i], 0, frame_size * sizeof(float));
            }
        }
        return;
    }

    // When effect disabled by caller, output silence (intentional)
    if (!direct_effect || !apply_effect) {
        for (int i = 0; i < out_buffer.numChannels; i++) {
            if (out_buffer.data[i])
                memset(out_buffer.data[i], 0, frame_size * sizeof(float));
        }
        return;
    }

    // 2. Downmix Input to Mono (IPL API has non-const param; input is read-only)
    iplAudioBufferDownmix(context, const_cast<IPLAudioBuffer*>(&in_buffer), &internal_mono_buffer);

    // 3. Apply Physics (Occlusion, Transmission, Attenuation)
    IPLDirectEffectParams params{};
    params.flags = static_cast<IPLDirectEffectFlags>(
        IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION |
        IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION |
        IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION);

    params.occlusion = occlusion;
    params.transmissionType = (transmission_type == resonance::kTransmissionFreqDependent) ? IPL_TRANSMISSIONTYPE_FREQDEPENDENT : IPL_TRANSMISSIONTYPE_FREQINDEPENDENT;

    if (transmission) {
        params.transmission[0] = transmission[0];
        params.transmission[1] = transmission[1];
        params.transmission[2] = transmission[2];
    } else {
        params.transmission[0] = 1.0f;
        params.transmission[1] = 1.0f;
        params.transmission[2] = 1.0f;
    }

    if (apply_air_absorption && air_absorption) {
        params.flags = static_cast<IPLDirectEffectFlags>(params.flags | IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION);
        params.airAbsorption[0] = air_absorption[0];
        params.airAbsorption[1] = air_absorption[1];
        params.airAbsorption[2] = air_absorption[2];
    } else {
        params.airAbsorption[0] = 1.0f;
        params.airAbsorption[1] = 1.0f;
        params.airAbsorption[2] = 1.0f;
    }

    if (apply_directivity) {
        params.flags = static_cast<IPLDirectEffectFlags>(params.flags | IPL_DIRECTEFFECTFLAGS_APPLYDIRECTIVITY);
        params.directivity = directivity_value;
    } else {
        params.directivity = 1.0f;
    }
    params.distanceAttenuation = resonance::sanitize_audio_float(attenuation);

    iplDirectEffectApply(direct_effect, &params, &internal_mono_buffer, &internal_direct_output);

    // 4. Calculate Vectors
    Vector3 v_l = ResonanceUtils::to_godot_vector3(listener_coords.origin);
    Vector3 v_s = ResonanceUtils::to_godot_vector3(source_pos);
    Vector3 vec = v_s - v_l;

    Vector3 ahead = ResonanceUtils::to_godot_vector3(listener_coords.ahead);
    Vector3 right = ResonanceUtils::to_godot_vector3(listener_coords.right);
    Vector3 up = ResonanceUtils::to_godot_vector3(listener_coords.up);

    float z = -vec.dot(ahead);
    float x = vec.dot(right);
    float y = vec.dot(up);

    IPLVector3 local_dir = {x, y, z};
    float len_sq = x * x + y * y + z * z;
    if (len_sq > resonance::kDegenerateVectorEpsilonSq) {
        float len = static_cast<float>(std::sqrt(static_cast<double>(len_sq)));
        local_dir.x /= len;
        local_dir.y /= len;
        local_dir.z /= len;
    } else {
        local_dir = {0.0f, 0.0f, -1.0f};
    }
    last_direction = local_dir;
    last_hrtf_bilinear = hrtf_interpolation_bilinear;
    last_spatial_blend = spatial_blend;
    last_use_ambisonics_encode_path = use_ambisonics_encode_path;
    last_use_binaural = use_binaural;

    apply_spatialization(local_dir, internal_direct_output, out_buffer, use_ambisonics_encode_path, use_binaural,
                         hrtf_interpolation_bilinear, spatial_blend);
}

void ResonanceDirectProcessor::clear_surround_tail_after_direct_stereo_effect(IPLAudioBuffer& out, const IPLAudioBuffer* stereo_effect_destination) {
    if (!stereo_effect_destination || stereo_effect_destination != &out || out.numChannels <= 2 || !out.data)
        return;
    for (int c = 2; c < out.numChannels; ++c) {
        if (out.data[c])
            memset(out.data[c], 0, frame_size * sizeof(float));
    }
}

void ResonanceDirectProcessor::copy_binaural_stereo_to_output(IPLAudioBuffer& out) {
    const IPLAudioBuffer& st = internal_binaural_stereo_out;
    if (!st.data || !st.data[0] || !st.data[1] || !out.data)
        return;
    int nc = out.numChannels;
    if (nc >= 2 && out.data[0] && out.data[1]) {
        memcpy(out.data[0], st.data[0], frame_size * sizeof(float));
        memcpy(out.data[1], st.data[1], frame_size * sizeof(float));
        for (int c = 2; c < nc; ++c) {
            if (out.data[c])
                memset(out.data[c], 0, frame_size * sizeof(float));
        }
    } else if (nc == 1 && out.data[0]) {
        for (int i = 0; i < frame_size; ++i)
            out.data[0][i] = 0.5f * (st.data[0][i] + st.data[1][i]);
    }
}

void ResonanceDirectProcessor::apply_spatialization(const IPLVector3& dir, const IPLAudioBuffer& direct_out, IPLAudioBuffer& out,
                                                    bool use_ambi_path, bool use_binaural, bool hrtf_bilinear, float spatial_blend) {
    IPLAudioBuffer* binaural_out = &out;
    if (out.numChannels > 2 && (init_flags & DirectInitFlags::BINAURAL_STEREO_SCRATCH) && internal_binaural_stereo_out.data &&
        internal_binaural_stereo_out.data[0] && internal_binaural_stereo_out.data[1]) {
        binaural_out = &internal_binaural_stereo_out;
    }

    if (use_ambi_path && ambisonics_encode_effect && ambisonics_binaural_effect && internal_ambi_buffer.data && use_binaural && hrtf_handle) {
        // AmbisonicsBinauralEffect has no spatialBlend; blend with iplBinauralEffectApply (same as non-HOA path) so
        // spatial_blend crossfades from standard binaural (weight 1-sb) to HOA (weight sb). At sb=0: binaural only; at
        // sb=1: HOA only. Mid values differ from a single BinauralEffect(sb) call - unavoidable without Steam exposing
        // spatialBlend on AmbisonicsBinauralEffect.
        float sb = spatial_blend;
        if (sb < 0.0f)
            sb = 0.0f;
        else if (sb > 1.0f)
            sb = 1.0f;
        constexpr float k_spatial_blend_eps = 1e-5f;
        const bool have_hoa_blend_scratch = (init_flags & DirectInitFlags::HOA_BINAURAL_STEREO_SCRATCH) &&
                                            internal_hoa_stereo_scratch.data && internal_hoa_stereo_scratch.data[0] &&
                                            internal_hoa_stereo_scratch.data[1];

        if (sb <= k_spatial_blend_eps) {
            IPLBinauralEffectParams binParams{};
            binParams.direction = dir;
            binParams.interpolation = hrtf_bilinear ? IPL_HRTFINTERPOLATION_BILINEAR : IPL_HRTFINTERPOLATION_NEAREST;
            binParams.spatialBlend = sb;
            binParams.hrtf = hrtf_handle;
            binParams.peakDelays = nullptr;
            iplBinauralEffectApply(binaural_effect, &binParams, const_cast<IPLAudioBuffer*>(&direct_out), binaural_out);
            if (binaural_out != &out)
                copy_binaural_stereo_to_output(out);
            else
                clear_surround_tail_after_direct_stereo_effect(out, binaural_out);
        } else if (sb >= 1.0f - k_spatial_blend_eps) {
            IPLAmbisonicsEncodeEffectParams encParams{};
            encParams.direction = dir;
            encParams.order = ambisonic_order;
            iplAmbisonicsEncodeEffectApply(ambisonics_encode_effect, &encParams, const_cast<IPLAudioBuffer*>(&direct_out), &internal_ambi_buffer);

            IPLAmbisonicsBinauralEffectParams ambBinParams{};
            ambBinParams.hrtf = hrtf_handle;
            ambBinParams.order = ambisonic_order;
            iplAmbisonicsBinauralEffectApply(ambisonics_binaural_effect, &ambBinParams, &internal_ambi_buffer, binaural_out);
            if (binaural_out != &out)
                copy_binaural_stereo_to_output(out);
            else
                clear_surround_tail_after_direct_stereo_effect(out, binaural_out);
        } else if (have_hoa_blend_scratch) {
            IPLAmbisonicsEncodeEffectParams encParams{};
            encParams.direction = dir;
            encParams.order = ambisonic_order;
            iplAmbisonicsEncodeEffectApply(ambisonics_encode_effect, &encParams, const_cast<IPLAudioBuffer*>(&direct_out), &internal_ambi_buffer);

            IPLAmbisonicsBinauralEffectParams ambBinParams{};
            ambBinParams.hrtf = hrtf_handle;
            ambBinParams.order = ambisonic_order;
            iplAmbisonicsBinauralEffectApply(ambisonics_binaural_effect, &ambBinParams, &internal_ambi_buffer, &internal_hoa_stereo_scratch);

            IPLBinauralEffectParams binParams{};
            binParams.direction = dir;
            binParams.interpolation = hrtf_bilinear ? IPL_HRTFINTERPOLATION_BILINEAR : IPL_HRTFINTERPOLATION_NEAREST;
            binParams.spatialBlend = sb;
            binParams.hrtf = hrtf_handle;
            binParams.peakDelays = nullptr;
            iplBinauralEffectApply(binaural_effect, &binParams, const_cast<IPLAudioBuffer*>(&direct_out), binaural_out);

            const float w_hoa = sb;
            const float w_bin = 1.0f - sb;
            if (binaural_out->data && binaural_out->data[0] && binaural_out->data[1]) {
                for (int i = 0; i < frame_size; ++i) {
                    binaural_out->data[0][i] = w_bin * binaural_out->data[0][i] + w_hoa * internal_hoa_stereo_scratch.data[0][i];
                    binaural_out->data[1][i] = w_bin * binaural_out->data[1][i] + w_hoa * internal_hoa_stereo_scratch.data[1][i];
                }
            }
            if (binaural_out != &out)
                copy_binaural_stereo_to_output(out);
            else
                clear_surround_tail_after_direct_stereo_effect(out, binaural_out);
        } else {
            IPLBinauralEffectParams binParams{};
            binParams.direction = dir;
            binParams.interpolation = hrtf_bilinear ? IPL_HRTFINTERPOLATION_BILINEAR : IPL_HRTFINTERPOLATION_NEAREST;
            binParams.spatialBlend = sb;
            binParams.hrtf = hrtf_handle;
            binParams.peakDelays = nullptr;
            iplBinauralEffectApply(binaural_effect, &binParams, const_cast<IPLAudioBuffer*>(&direct_out), binaural_out);
            if (binaural_out != &out)
                copy_binaural_stereo_to_output(out);
            else
                clear_surround_tail_after_direct_stereo_effect(out, binaural_out);
        }
    } else if (use_binaural && binaural_effect && hrtf_handle) {
        IPLBinauralEffectParams binParams{};
        binParams.direction = dir;
        binParams.interpolation = hrtf_bilinear ? IPL_HRTFINTERPOLATION_BILINEAR : IPL_HRTFINTERPOLATION_NEAREST;
        binParams.spatialBlend = spatial_blend;
        binParams.hrtf = hrtf_handle;
        binParams.peakDelays = nullptr;
        iplBinauralEffectApply(binaural_effect, &binParams, const_cast<IPLAudioBuffer*>(&direct_out), binaural_out);
        if (binaural_out != &out)
            copy_binaural_stereo_to_output(out);
        else
            clear_surround_tail_after_direct_stereo_effect(out, binaural_out);
    } else if (use_ambi_path && ambisonics_encode_effect && ambisonics_panning_effect && internal_ambi_buffer.data) {
        IPLAmbisonicsEncodeEffectParams encParams{};
        encParams.direction = dir;
        encParams.order = ambisonic_order;
        iplAmbisonicsEncodeEffectApply(ambisonics_encode_effect, &encParams, const_cast<IPLAudioBuffer*>(&direct_out), &internal_ambi_buffer);

        IPLAmbisonicsPanningEffectParams ambiPanParams{};
        ambiPanParams.order = ambisonic_order;
        iplAmbisonicsPanningEffectApply(ambisonics_panning_effect, &ambiPanParams, &internal_ambi_buffer, &out);
    } else if (panning_effect) {
        IPLPanningEffectParams panParams{};
        panParams.direction = dir;
        iplPanningEffectApply(panning_effect, &panParams, const_cast<IPLAudioBuffer*>(&direct_out), &out);
    }
}

bool ResonanceDirectProcessor::process_tail(IPLAudioBuffer& out_buffer) {
    if (!(init_flags & DirectInitFlags::DIRECT_EFFECT) || !direct_effect) {
        if (out_buffer.data) {
            for (int i = 0; i < out_buffer.numChannels; i++) {
                if (out_buffer.data[i])
                    memset(out_buffer.data[i], 0, frame_size * sizeof(float));
            }
        }
        return false;
    }
    if (!out_buffer.data)
        return false;
    if (iplDirectEffectGetTailSize(direct_effect) <= 0)
        return false;

    IPLAudioEffectState state = iplDirectEffectGetTail(direct_effect, &internal_direct_output);
    if (state == IPL_AUDIOEFFECTSTATE_TAILCOMPLETE)
        return false;

    apply_spatialization(last_direction, internal_direct_output, out_buffer, last_use_ambisonics_encode_path, last_use_binaural,
                         last_hrtf_bilinear, last_spatial_blend);
    return true;
}

void ResonanceDirectProcessor::reset_for_new_playback() {
    if (direct_effect)
        iplDirectEffectReset(direct_effect);
}
} // namespace godot