#include "resonance_processor_reflection.h"
#include "resonance_constants.h"
#include "resonance_log.h"
#include "resonance_math.h"
#include "resonance_utils.h"
#include <algorithm>
#include <cmath>
#include <cstring>

// IPL reflection effect integration for playback (see resonance_processor_reflection.h).

namespace {

// Mono tap before IPL Apply: linear ramp between prev/current reflections mix, or fixed scale if prev < 0 (first block).
void preprocess_reflection_mono(float prev_gain, float gain, int frame_size, float* mono) {
    if (!mono || frame_size <= 0)
        return;
    if (prev_gain >= 0.0f) {
        resonance::apply_volume_ramp_and_sanitize(prev_gain, gain, frame_size, mono);
        return;
    }
    const bool scale = std::abs(gain - 1.0f) > 1e-5f;
    for (int i = 0; i < frame_size; i++) {
        float s = mono[i];
        if (scale)
            s *= gain;
        mono[i] = resonance::sanitize_audio_float(s);
    }
}

} // namespace

namespace godot {

ResonanceReflectionProcessor::~ResonanceReflectionProcessor() {
    cleanup();
}

void ResonanceReflectionProcessor::initialize(IPLContext p_context, int p_sample_rate, int p_frame_size, int p_ambisonic_order, int p_reflection_type,
                                              float p_max_reverb_duration_sec, int p_convolution_ir_max_samples) {
    if (init_flags != ReflectionInitFlags::NONE)
        return;

    if (!p_context) {
        ResonanceLog::error("ResonanceReflectionProcessor: Context is null.");
        return;
    }

    convolution_ir_max_samples_ = std::max(0, p_convolution_ir_max_samples);

    float dur = resonance::sanitize_audio_float(p_max_reverb_duration_sec);
    if (dur < 0.1f)
        dur = 0.1f;
    if (dur > 10.0f)
        dur = 10.0f;
    effect_ir_duration_sec_ = dur;

    context = p_context;
    frame_size = p_frame_size;
    sample_rate = p_sample_rate;
    reflection_type = p_reflection_type;

    if (reflection_type == resonance::kReflectionParametric) {
        num_channels = 1;
    } else {
        num_channels = (p_ambisonic_order + 1) * (p_ambisonic_order + 1);
    }

    IPLAudioSettings audioSettings{};
    audioSettings.samplingRate = sample_rate;
    audioSettings.frameSize = frame_size;

    IPLReflectionEffectType effectType = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
    if (reflection_type == resonance::kReflectionParametric)
        effectType = IPL_REFLECTIONEFFECTTYPE_PARAMETRIC;
    else if (reflection_type == resonance::kReflectionHybrid)
        effectType = IPL_REFLECTIONEFFECTTYPE_HYBRID;
    else if (reflection_type == resonance::kReflectionTan)
        effectType = IPL_REFLECTIONEFFECTTYPE_TAN;

    IPLReflectionEffectSettings reflSettings{};
    reflSettings.type = effectType;
    reflSettings.irSize = (reflection_type == resonance::kReflectionParametric)
                              ? static_cast<IPLint32>(1)
                              : static_cast<IPLint32>(resonance::reverb_ir_size_samples(sample_rate, effect_ir_duration_sec_));
    reflSettings.numChannels = num_channels;

    if (iplReflectionEffectCreate(context, &audioSettings, &reflSettings, &reflection_effect) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceReflectionProcessor: iplReflectionEffectCreate failed.");
        return;
    }
    effect_max_ir_samples_ = static_cast<int>(reflSettings.irSize);
    init_flags = init_flags | ReflectionInitFlags::REFLECTIONEFFECT;

    if (iplAudioBufferAllocate(context, 1, frame_size, &sa_mono_buffer) != IPL_STATUS_SUCCESS ||
        iplAudioBufferAllocate(context, num_channels, frame_size, &sa_temp_out_buffer) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceReflectionProcessor: Buffer allocation failed.");
        iplReflectionEffectRelease(&reflection_effect);
        reflection_effect = nullptr;
        cleanup();
        return;
    }
    init_flags = init_flags | ReflectionInitFlags::BUFFERS;

    // Air-absorption pre-EQ (used only for baked reflections, where the IR does not encode source→listener
    // air absorption). Cheap: 3-band IIR inside Steam Audio's IPLDirectEffect on a single mono channel.
    IPLDirectEffectSettings airSettings{};
    airSettings.numChannels = 1;
    if (iplDirectEffectCreate(context, &audioSettings, &airSettings, &air_absorption_effect) == IPL_STATUS_SUCCESS) {
        if (iplAudioBufferAllocate(context, 1, frame_size, &sa_air_absorption_in_buffer) == IPL_STATUS_SUCCESS &&
            iplAudioBufferAllocate(context, 1, frame_size, &sa_air_absorption_out_buffer) == IPL_STATUS_SUCCESS) {
            init_flags = init_flags | ReflectionInitFlags::AIRABSORPTIONEFFECT;
        } else {
            ResonanceLog::error("ResonanceReflectionProcessor: air-absorption buffer allocation failed; pre-EQ disabled.");
            iplDirectEffectRelease(&air_absorption_effect);
            air_absorption_effect = nullptr;
            if (sa_air_absorption_in_buffer.data)
                iplAudioBufferFree(context, &sa_air_absorption_in_buffer);
            if (sa_air_absorption_out_buffer.data)
                iplAudioBufferFree(context, &sa_air_absorption_out_buffer);
            memset(&sa_air_absorption_in_buffer, 0, sizeof(sa_air_absorption_in_buffer));
            memset(&sa_air_absorption_out_buffer, 0, sizeof(sa_air_absorption_out_buffer));
        }
    } else {
        // Non-fatal: reflection processing still runs without the wet air-absorption stage.
        ResonanceLog::error("ResonanceReflectionProcessor: iplDirectEffectCreate (air-absorption pre-EQ) failed; pre-EQ disabled.");
    }
}

void ResonanceReflectionProcessor::cleanup() {
    if (reflection_effect) {
        iplReflectionEffectRelease(&reflection_effect);
        reflection_effect = nullptr;
    }
    if (air_absorption_effect) {
        iplDirectEffectRelease(&air_absorption_effect);
        air_absorption_effect = nullptr;
    }

    if (context) {
        if (sa_mono_buffer.data)
            iplAudioBufferFree(context, &sa_mono_buffer);
        if (sa_temp_out_buffer.data)
            iplAudioBufferFree(context, &sa_temp_out_buffer);
        if (sa_air_absorption_in_buffer.data)
            iplAudioBufferFree(context, &sa_air_absorption_in_buffer);
        if (sa_air_absorption_out_buffer.data)
            iplAudioBufferFree(context, &sa_air_absorption_out_buffer);
    }
    memset(&sa_mono_buffer, 0, sizeof(sa_mono_buffer));
    memset(&sa_temp_out_buffer, 0, sizeof(sa_temp_out_buffer));
    memset(&sa_air_absorption_in_buffer, 0, sizeof(sa_air_absorption_in_buffer));
    memset(&sa_air_absorption_out_buffer, 0, sizeof(sa_air_absorption_out_buffer));

    context = nullptr;
    init_flags = ReflectionInitFlags::NONE;
    convolution_ir_max_samples_ = 0;
    effect_max_ir_samples_ = 0;
    last_air_absorption_ = {1.0f, 1.0f, 1.0f};
    air_absorption_iir_active_ = false;
}

bool ResonanceReflectionProcessor::process_mix(const IPLAudioBuffer& in_buffer,
                                               const IPLReflectionEffectParams& reverb_params,
                                               IPLReflectionMixer mixer_handle,
                                               float prev_reflections_mix_level,
                                               float reflections_mix_level,
                                               float wet_extra_gain,
                                               bool apply_air_absorption,
                                               const resonance::AudioBands3& air_absorption) {

    if (!(init_flags & ReflectionInitFlags::REFLECTIONEFFECT) || !(init_flags & ReflectionInitFlags::BUFFERS) || !reflection_effect)
        return false;

    IPLReflectionEffectParams params = reverb_params;
    sanitize_reflection_params(&params);

    // Convolution/hybrid need a valid IR before we ramp mono; otherwise skip Apply and keep caller ramp bookkeeping consistent.
    if ((params.type == IPL_REFLECTIONEFFECTTYPE_CONVOLUTION || params.type == IPL_REFLECTIONEFFECTTYPE_HYBRID) && !params.ir)
        return false;

    iplAudioBufferDownmix(context, const_cast<IPLAudioBuffer*>(&in_buffer), &sa_mono_buffer); // API takes non-const buffer ptr

    const bool first_block = (prev_reflections_mix_level < 0.0f);
    float safe_curr_refl = resonance::sanitize_audio_float(reflections_mix_level);
    if (sa_mono_buffer.data && sa_mono_buffer.data[0]) {
        float* mono = sa_mono_buffer.data[0];
        if (first_block) {
            preprocess_reflection_mono(-1.0f, safe_curr_refl, frame_size, mono);
        } else {
            float safe_prev_refl = resonance::sanitize_audio_float(prev_reflections_mix_level);
            preprocess_reflection_mono(safe_prev_refl, safe_curr_refl, frame_size, mono);
        }
        float eg = resonance::sanitize_audio_float(wet_extra_gain);
        if (std::abs(eg - 1.0f) > 1e-5f) {
            for (int i = 0; i < frame_size; i++)
                mono[i] = resonance::sanitize_audio_float(mono[i] * eg);
        }
        if (apply_air_absorption) {
            apply_air_absorption_in_place(air_absorption);
        } else if (air_absorption_iir_active_) {
            // Bypass after air-absorption blocks: reset IIR on next apply instead of stale history.
            air_absorption_iir_active_ = false;
        }
    }

    iplReflectionEffectApply(reflection_effect, &params,
                             &sa_mono_buffer, &sa_temp_out_buffer, mixer_handle);
    return true;
}

bool ResonanceReflectionProcessor::process_mix_direct(const IPLAudioBuffer& in_buffer,
                                                      const IPLReflectionEffectParams& reverb_params,
                                                      float prev_reflections_mix_level,
                                                      float reflections_mix_level,
                                                      bool apply_air_absorption,
                                                      const resonance::AudioBands3& air_absorption) {
    if (!(init_flags & ReflectionInitFlags::REFLECTIONEFFECT) || !(init_flags & ReflectionInitFlags::BUFFERS) || !reflection_effect)
        return false;

    IPLReflectionEffectParams params = reverb_params;
    sanitize_reflection_params(&params);

    if ((params.type == IPL_REFLECTIONEFFECTTYPE_CONVOLUTION || params.type == IPL_REFLECTIONEFFECTTYPE_HYBRID) && !params.ir)
        return false;

    iplAudioBufferDownmix(context, const_cast<IPLAudioBuffer*>(&in_buffer), &sa_mono_buffer);

    if (sa_mono_buffer.data && sa_mono_buffer.data[0]) {
        float p = resonance::sanitize_audio_float(prev_reflections_mix_level);
        float c = resonance::sanitize_audio_float(reflections_mix_level);
        preprocess_reflection_mono(p, c, frame_size, sa_mono_buffer.data[0]);
        if (apply_air_absorption) {
            apply_air_absorption_in_place(air_absorption);
        } else if (air_absorption_iir_active_) {
            air_absorption_iir_active_ = false;
        }
    }

    iplReflectionEffectApply(reflection_effect, &params,
                             &sa_mono_buffer, &sa_temp_out_buffer, nullptr);
    return true;
}

void ResonanceReflectionProcessor::sanitize_reflection_params(IPLReflectionEffectParams* params) const {
    if (!params)
        return;
    if (params->irSize <= 0)
        params->irSize = static_cast<IPLint32>(resonance::reverb_ir_size_samples(sample_rate, effect_ir_duration_sec_));
    if (params->numChannels <= 0)
        params->numChannels = num_channels;
    if (convolution_ir_max_samples_ > 0 && effect_max_ir_samples_ > 0 &&
        (reflection_type == resonance::kReflectionConvolution || reflection_type == resonance::kReflectionHybrid ||
         reflection_type == resonance::kReflectionTan)) {
        const int cap = std::min(convolution_ir_max_samples_, effect_max_ir_samples_);
        if (cap > 0 && params->irSize > cap)
            params->irSize = static_cast<IPLint32>(cap);
    }
    for (int i = 0; i < IPL_NUM_BANDS; i++) {
        params->reverbTimes[i] = resonance::clamp_reverb_time(params->reverbTimes[i]);
        params->eq[i] = resonance::sanitize_audio_float(params->eq[i]);
    }
    params->delay = resonance::sanitize_delay_samples(params->delay);
}

void ResonanceReflectionProcessor::reset_effect() {
    if (reflection_effect)
        iplReflectionEffectReset(reflection_effect);
}

int ResonanceReflectionProcessor::get_tail_size_samples() const {
    if (!(init_flags & ReflectionInitFlags::REFLECTIONEFFECT) || !reflection_effect)
        return 0;
    return iplReflectionEffectGetTailSize(reflection_effect);
}

IPLAudioEffectState ResonanceReflectionProcessor::tail_apply(IPLReflectionEffectParams* params, IPLReflectionMixer mixer) {
    if (!(init_flags & ReflectionInitFlags::REFLECTIONEFFECT) || !(init_flags & ReflectionInitFlags::BUFFERS) || !reflection_effect || !params)
        return IPL_AUDIOEFFECTSTATE_TAILCOMPLETE;
    sanitize_reflection_params(params);
    if ((params->type == IPL_REFLECTIONEFFECTTYPE_CONVOLUTION || params->type == IPL_REFLECTIONEFFECTTYPE_HYBRID) && !params->ir)
        return IPL_AUDIOEFFECTSTATE_TAILCOMPLETE;
    return iplReflectionEffectGetTail(reflection_effect, &sa_temp_out_buffer, mixer);
}

IPLAudioEffectState ResonanceReflectionProcessor::tail_apply_direct(IPLReflectionEffectParams* params) {
    return tail_apply(params, nullptr);
}

IPLAudioEffectState ResonanceReflectionProcessor::tail_apply_to_mixer(IPLReflectionEffectParams* params, IPLReflectionMixer mixer) {
    if (!mixer)
        return IPL_AUDIOEFFECTSTATE_TAILCOMPLETE;
    return tail_apply(params, mixer);
}

void ResonanceReflectionProcessor::apply_air_absorption_in_place(const resonance::AudioBands3& target) {
    if (!(init_flags & ReflectionInitFlags::AIRABSORPTIONEFFECT) || !air_absorption_effect)
        return;
    if (!sa_mono_buffer.data || !sa_mono_buffer.data[0] || !sa_air_absorption_in_buffer.data || !sa_air_absorption_in_buffer.data[0] || !sa_air_absorption_out_buffer.data || !sa_air_absorption_out_buffer.data[0])
        return;
    // Sanitize/clamp band gains. Always run the IIR pass (cheap 3-band) even for identity targets so the internal
    // filter state stays continuous block-to-block; skipping calls on identity blocks would let the next non-identity
    // block ramp from a stale state and produce a click.
    const float a0 = std::fmin(std::fmax(resonance::sanitize_audio_float(target[0]), 0.0f), 1.0f);
    const float a1 = std::fmin(std::fmax(resonance::sanitize_audio_float(target[1]), 0.0f), 1.0f);
    const float a2 = std::fmin(std::fmax(resonance::sanitize_audio_float(target[2]), 0.0f), 1.0f);
    if (!air_absorption_iir_active_)
        iplDirectEffectReset(air_absorption_effect);
    float* in_ch = sa_air_absorption_in_buffer.data[0];
    float* out_ch = sa_air_absorption_out_buffer.data[0];
    float* mono = sa_mono_buffer.data[0];
    memcpy(in_ch, mono, static_cast<size_t>(frame_size) * sizeof(float));
    IPLDirectEffectParams params{};
    params.flags = IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION;
    params.airAbsorption[0] = a0;
    params.airAbsorption[1] = a1;
    params.airAbsorption[2] = a2;
    iplDirectEffectApply(air_absorption_effect, &params, &sa_air_absorption_in_buffer, &sa_air_absorption_out_buffer);
    memcpy(mono, out_ch, static_cast<size_t>(frame_size) * sizeof(float));
    for (int i = 0; i < frame_size; i++)
        mono[i] = resonance::sanitize_audio_float(mono[i]);
    last_air_absorption_ = {a0, a1, a2};
    air_absorption_iir_active_ = true;
}

} // namespace godot
