#include "resonance_processor_direct.h"
#include "resonance_direct_spatial_tail_policy.h"
#include "resonance_log.h"
#include "resonance_processor_hrtf_policy.h"
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
    sample_rate = p_sample_rate;
    ambisonic_order = p_ambisonic_order;
    use_ambisonics_encode = p_use_ambisonics_encode;
    speaker_channels = resonance::clamp_direct_speaker_channels(p_speaker_channels);

    IPLAudioSettings audioSettings{};
    audioSettings.samplingRate = sample_rate;
    audioSettings.frameSize = frame_size;

    // 1. Direct Effect (Mono physics)
    IPLDirectEffectSettings directSettings{};
    directSettings.numChannels = 1;

    if (iplDirectEffectCreate(context, &audioSettings, &directSettings, &direct_effect) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("DirectProcessor: Failed to create IPLDirectEffect");
        return;
    }
    init_flags |= DirectInitFlags::DIRECT_EFFECT;

    // 2. Binaural + optional HOA path (skipped when HRTF unavailable at init; ensure_hrtf_effects_on_main later).
    ResonanceServer* srv = ResonanceServer::get_singleton();
    hrtf_handle = (srv) ? srv->get_hrtf_handle() : nullptr;
    if (hrtf_handle) {
        if (!create_hrtf_bound_effects(hrtf_handle))
            ResonanceLog::warn("DirectProcessor: Failed to create HRTF-bound effects.");
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

    // 4. Buffers (separate in/out for iplDirectEffectApply per Steam Audio reference implementations)
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
    bound_hrtf_ = nullptr;
    context = nullptr;
    init_flags = DirectInitFlags::NONE;
}

void ResonanceDirectProcessor::release_hrtf_bound_effects() {
    if (binaural_effect) {
        iplBinauralEffectRelease(&binaural_effect);
        binaural_effect = nullptr;
        init_flags = static_cast<DirectInitFlags>(static_cast<int>(init_flags) & ~static_cast<int>(DirectInitFlags::BINAURAL_EFFECT));
    }
    if (ambisonics_binaural_effect) {
        iplAmbisonicsBinauralEffectRelease(&ambisonics_binaural_effect);
        ambisonics_binaural_effect = nullptr;
    }
    if (ambisonics_encode_effect) {
        iplAmbisonicsEncodeEffectRelease(&ambisonics_encode_effect);
        ambisonics_encode_effect = nullptr;
        init_flags = static_cast<DirectInitFlags>(static_cast<int>(init_flags) & ~static_cast<int>(DirectInitFlags::AMBISONICS_ENCODE));
    }
    if (ambisonics_panning_effect) {
        iplAmbisonicsPanningEffectRelease(&ambisonics_panning_effect);
        ambisonics_panning_effect = nullptr;
        init_flags = static_cast<DirectInitFlags>(static_cast<int>(init_flags) & ~static_cast<int>(DirectInitFlags::AMBISONICS_PANNING));
    }
    if (context && internal_ambi_buffer.data != nullptr) {
        iplAudioBufferFree(context, &internal_ambi_buffer);
        memset(&internal_ambi_buffer, 0, sizeof(internal_ambi_buffer));
    }
    if (context && internal_hoa_stereo_scratch.data != nullptr) {
        iplAudioBufferFree(context, &internal_hoa_stereo_scratch);
        memset(&internal_hoa_stereo_scratch, 0, sizeof(internal_hoa_stereo_scratch));
        init_flags = static_cast<DirectInitFlags>(static_cast<int>(init_flags) & ~static_cast<int>(DirectInitFlags::HOA_BINAURAL_STEREO_SCRATCH));
    }
    if (context && internal_binaural_stereo_out.data != nullptr) {
        iplAudioBufferFree(context, &internal_binaural_stereo_out);
        memset(&internal_binaural_stereo_out, 0, sizeof(internal_binaural_stereo_out));
        init_flags = static_cast<DirectInitFlags>(static_cast<int>(init_flags) & ~static_cast<int>(DirectInitFlags::BINAURAL_STEREO_SCRATCH));
    }
    bound_hrtf_ = nullptr;
}

bool ResonanceDirectProcessor::create_hrtf_bound_effects(IPLHRTF hrtf) {
    if (!context || !hrtf)
        return false;

    IPLAudioSettings audioSettings{};
    audioSettings.samplingRate = sample_rate;
    audioSettings.frameSize = frame_size;

    IPLBinauralEffectSettings binSettings{};
    binSettings.hrtf = hrtf;
    if (iplBinauralEffectCreate(context, &audioSettings, &binSettings, &binaural_effect) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("DirectProcessor: Failed to create IPLBinauralEffect");
        return false;
    }
    init_flags |= DirectInitFlags::BINAURAL_EFFECT;

    if (use_ambisonics_encode) {
        IPLAmbisonicsEncodeEffectSettings encSettings{};
        encSettings.maxOrder = ambisonic_order;
        if (iplAmbisonicsEncodeEffectCreate(context, &audioSettings, &encSettings, &ambisonics_encode_effect) != IPL_STATUS_SUCCESS) {
            ResonanceLog::warn("DirectProcessor: Ambisonics Encode effect creation failed.");
        }
        IPLAmbisonicsBinauralEffectSettings ambBinSettings{};
        ambBinSettings.hrtf = hrtf;
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

    if (speaker_channels > 2 && (binaural_effect || ambisonics_binaural_effect)) {
        if (iplAudioBufferAllocate(context, 2, frame_size, &internal_binaural_stereo_out) == IPL_STATUS_SUCCESS && internal_binaural_stereo_out.data &&
            internal_binaural_stereo_out.data[0] && internal_binaural_stereo_out.data[1]) {
            init_flags |= DirectInitFlags::BINAURAL_STEREO_SCRATCH;
        } else {
            ResonanceLog::warn("DirectProcessor: Binaural stereo scratch allocation failed; HRTF with surround direct_speaker_channels may be unstable.");
        }
    }

    bound_hrtf_ = hrtf;
    hrtf_handle = hrtf;
    return true;
}

bool ResonanceDirectProcessor::hrtf_effects_need_main_sync(IPLHRTF runtime_hrtf) const {
    if (!(init_flags & DirectInitFlags::DIRECT_EFFECT))
        return false;
    const bool has_binaural = (init_flags & DirectInitFlags::BINAURAL_EFFECT) && binaural_effect != nullptr;
    return resonance::direct_needs_hrtf_main_sync(has_binaural, bound_hrtf_, runtime_hrtf);
}

void ResonanceDirectProcessor::ensure_hrtf_effects_on_main(IPLHRTF runtime_hrtf) {
    if (!(init_flags & DirectInitFlags::DIRECT_EFFECT) || !context)
        return;
    if (!runtime_hrtf) {
        hrtf_handle = nullptr;
        return;
    }

    const bool has_binaural = (init_flags & DirectInitFlags::BINAURAL_EFFECT) && binaural_effect != nullptr;
    if (resonance::direct_needs_hrtf_effect_create(has_binaural, bound_hrtf_, runtime_hrtf)) {
        create_hrtf_bound_effects(runtime_hrtf);
        return;
    }
    if (resonance::direct_needs_hrtf_effect_recreate(has_binaural, bound_hrtf_, runtime_hrtf)) {
        release_hrtf_bound_effects();
        create_hrtf_bound_effects(runtime_hrtf);
        return;
    }
    hrtf_handle = runtime_hrtf;
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

    ResonanceServer* srv = ResonanceServer::get_singleton();
    hrtf_handle = (srv) ? srv->get_hrtf_handle() : nullptr;

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
            // Mid-range spatial_blend: manual stereo mix (AmbisonicsBinauralEffect has no spatialBlend).
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

bool ResonanceDirectProcessor::apply_spatialization_tail(IPLAudioBuffer& out) {
    IPLAudioBuffer* binaural_out = &out;
    if (out.numChannels > 2 && (init_flags & DirectInitFlags::BINAURAL_STEREO_SCRATCH) && internal_binaural_stereo_out.data &&
        internal_binaural_stereo_out.data[0] && internal_binaural_stereo_out.data[1]) {
        binaural_out = &internal_binaural_stereo_out;
    }

    if (last_use_ambisonics_encode_path && ambisonics_binaural_effect && last_use_binaural && hrtf_handle) {
        float sb = last_spatial_blend;
        if (sb < 0.0f)
            sb = 0.0f;
        else if (sb > 1.0f)
            sb = 1.0f;
        constexpr float k_spatial_blend_eps = 1e-5f;
        const bool have_hoa_blend_scratch = (init_flags & DirectInitFlags::HOA_BINAURAL_STEREO_SCRATCH) &&
                                            internal_hoa_stereo_scratch.data && internal_hoa_stereo_scratch.data[0] &&
                                            internal_hoa_stereo_scratch.data[1];

        if (sb <= k_spatial_blend_eps) {
            if (!binaural_effect || iplBinauralEffectGetTailSize(binaural_effect) <= 0)
                return false;
            const IPLAudioEffectState state = iplBinauralEffectGetTail(binaural_effect, binaural_out);
            if (binaural_out != &out)
                copy_binaural_stereo_to_output(out);
            else
                clear_surround_tail_after_direct_stereo_effect(out, binaural_out);
            return resonance::direct_spatial_tail_produced(state);
        }
        if (sb >= 1.0f - k_spatial_blend_eps) {
            if (iplAmbisonicsBinauralEffectGetTailSize(ambisonics_binaural_effect) <= 0)
                return false;
            const IPLAudioEffectState state = iplAmbisonicsBinauralEffectGetTail(ambisonics_binaural_effect, binaural_out);
            if (binaural_out != &out)
                copy_binaural_stereo_to_output(out);
            else
                clear_surround_tail_after_direct_stereo_effect(out, binaural_out);
            return resonance::direct_spatial_tail_produced(state);
        }
        if (!have_hoa_blend_scratch || !binaural_effect)
            return false;

        const bool bin_active = iplBinauralEffectGetTailSize(binaural_effect) > 0;
        const bool hoa_active = iplAmbisonicsBinauralEffectGetTailSize(ambisonics_binaural_effect) > 0;
        if (!bin_active && !hoa_active)
            return false;

        IPLAudioEffectState bin_state = IPL_AUDIOEFFECTSTATE_TAILCOMPLETE;
        IPLAudioEffectState hoa_state = IPL_AUDIOEFFECTSTATE_TAILCOMPLETE;
        if (bin_active)
            bin_state = iplBinauralEffectGetTail(binaural_effect, binaural_out);
        else if (binaural_out->data && binaural_out->data[0] && binaural_out->data[1]) {
            memset(binaural_out->data[0], 0, frame_size * sizeof(float));
            memset(binaural_out->data[1], 0, frame_size * sizeof(float));
        }

        if (hoa_active)
            hoa_state = iplAmbisonicsBinauralEffectGetTail(ambisonics_binaural_effect, &internal_hoa_stereo_scratch);
        else if (internal_hoa_stereo_scratch.data && internal_hoa_stereo_scratch.data[0] && internal_hoa_stereo_scratch.data[1]) {
            memset(internal_hoa_stereo_scratch.data[0], 0, frame_size * sizeof(float));
            memset(internal_hoa_stereo_scratch.data[1], 0, frame_size * sizeof(float));
        }

        const float w_hoa = sb;
        const float w_bin = 1.0f - sb;
        if (binaural_out->data && binaural_out->data[0] && binaural_out->data[1] && internal_hoa_stereo_scratch.data &&
            internal_hoa_stereo_scratch.data[0] && internal_hoa_stereo_scratch.data[1]) {
            for (int i = 0; i < frame_size; ++i) {
                binaural_out->data[0][i] = w_bin * binaural_out->data[0][i] + w_hoa * internal_hoa_stereo_scratch.data[0][i];
                binaural_out->data[1][i] = w_bin * binaural_out->data[1][i] + w_hoa * internal_hoa_stereo_scratch.data[1][i];
            }
        }
        if (binaural_out != &out)
            copy_binaural_stereo_to_output(out);
        else
            clear_surround_tail_after_direct_stereo_effect(out, binaural_out);
        return resonance::direct_spatial_blend_tail_active(bin_state, hoa_state);
    }

    if (last_use_binaural && binaural_effect && hrtf_handle) {
        if (iplBinauralEffectGetTailSize(binaural_effect) <= 0)
            return false;
        const IPLAudioEffectState state = iplBinauralEffectGetTail(binaural_effect, binaural_out);
        if (binaural_out != &out)
            copy_binaural_stereo_to_output(out);
        else
            clear_surround_tail_after_direct_stereo_effect(out, binaural_out);
        return resonance::direct_spatial_tail_produced(state);
    }

    return false;
}

int ResonanceDirectProcessor::get_tail_size_samples() const {
    if (!(init_flags & DirectInitFlags::DIRECT_EFFECT) || !direct_effect)
        return 0;
    const int direct_tail_samples = iplDirectEffectGetTailSize(direct_effect);
    const bool use_hoa_binaural_path = last_use_ambisonics_encode_path && ambisonics_binaural_effect != nullptr;
    const int binaural_tail_samples = (binaural_effect != nullptr) ? iplBinauralEffectGetTailSize(binaural_effect) : 0;
    const int hoa_binaural_tail_samples =
        (ambisonics_binaural_effect != nullptr) ? iplAmbisonicsBinauralEffectGetTailSize(ambisonics_binaural_effect) : 0;
    return resonance::direct_spatial_tail_sample_budget(last_use_binaural, use_hoa_binaural_path, direct_tail_samples,
                                                        binaural_tail_samples, hoa_binaural_tail_samples);
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

    ResonanceServer* srv = ResonanceServer::get_singleton();
    hrtf_handle = (srv) ? srv->get_hrtf_handle() : nullptr;

    const int direct_tail_samples = iplDirectEffectGetTailSize(direct_effect);
    const bool use_hoa_binaural_path = last_use_ambisonics_encode_path && ambisonics_binaural_effect != nullptr;
    const int binaural_tail_samples = (binaural_effect != nullptr) ? iplBinauralEffectGetTailSize(binaural_effect) : 0;
    const int hoa_binaural_tail_samples =
        (ambisonics_binaural_effect != nullptr) ? iplAmbisonicsBinauralEffectGetTailSize(ambisonics_binaural_effect) : 0;

    if (!resonance::direct_spatial_tail_active(last_use_binaural, use_hoa_binaural_path, direct_tail_samples,
                                               binaural_tail_samples, hoa_binaural_tail_samples)) {
        return false;
    }

    if (resonance::direct_spatial_tail_uses_apply_on_direct_mono(direct_tail_samples)) {
        const IPLAudioEffectState direct_state = iplDirectEffectGetTail(direct_effect, &internal_direct_output);
        if (!resonance::direct_spatial_tail_produced(direct_state))
            return apply_spatialization_tail(out_buffer);
        apply_spatialization(last_direction, internal_direct_output, out_buffer, last_use_ambisonics_encode_path, last_use_binaural,
                             last_hrtf_bilinear, last_spatial_blend);
        return true;
    }

    return apply_spatialization_tail(out_buffer);
}

void ResonanceDirectProcessor::reset_for_new_playback() {
    if (direct_effect)
        iplDirectEffectReset(direct_effect);
    if (binaural_effect)
        iplBinauralEffectReset(binaural_effect);
    if (panning_effect)
        iplPanningEffectReset(panning_effect);
    if (ambisonics_encode_effect)
        iplAmbisonicsEncodeEffectReset(ambisonics_encode_effect);
    if (ambisonics_binaural_effect)
        iplAmbisonicsBinauralEffectReset(ambisonics_binaural_effect);
    if (ambisonics_panning_effect)
        iplAmbisonicsPanningEffectReset(ambisonics_panning_effect);
}
} // namespace godot