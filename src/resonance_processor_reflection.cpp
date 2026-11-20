#include "resonance_processor_reflection.h"
#include "resonance_block_stage_timer.h"
#include "resonance_constants.h"
#include "resonance_log.h"
#include "resonance_math.h"
#include "resonance_utils.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace {

// Linear ramp from prev_gain to gain, or a constant gain when prev_gain < 0 (first block).
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

ReflectionEffectRecipe ResonanceReflectionProcessor::effect_recipe(int p_sample_rate, int p_frame_size, int p_ambisonic_order,
                                                                   int p_reflection_type, float p_ir_duration_sec,
                                                                   int p_convolution_ir_max_samples) {
    ReflectionEffectRecipe recipe;
    recipe.ir_duration_sec = std::clamp(resonance::sanitize_audio_float(p_ir_duration_sec), 0.1f, 10.0f);
    recipe.audio.samplingRate = p_sample_rate;
    recipe.audio.frameSize = p_frame_size;

    IPLReflectionEffectSettings& settings = recipe.settings;
    settings.type = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
    if (p_reflection_type == resonance::kReflectionParametric)
        settings.type = IPL_REFLECTIONEFFECTTYPE_PARAMETRIC;
    else if (p_reflection_type == resonance::kReflectionHybrid)
        settings.type = IPL_REFLECTIONEFFECTTYPE_HYBRID;
    else if (p_reflection_type == resonance::kReflectionTan)
        settings.type = IPL_REFLECTIONEFFECTTYPE_TAN;

    if (p_reflection_type == resonance::kReflectionParametric) {
        settings.numChannels = 1;
        settings.irSize = 1;
        return recipe;
    }
    settings.numChannels = (p_ambisonic_order + 1) * (p_ambisonic_order + 1);
    settings.irSize = static_cast<IPLint32>(resonance::reflection_effect_create_ir_size(
        p_sample_rate, recipe.ir_duration_sec, std::max(0, p_convolution_ir_max_samples)));
    return recipe;
}

void ResonanceReflectionProcessor::initialize(IPLContext p_context, int p_sample_rate, int p_frame_size, int p_ambisonic_order, int p_reflection_type,
                                              float p_ir_duration_sec, int p_convolution_ir_max_samples,
                                              IPLReflectionEffect p_pooled_effect) {
    if (init_flags != ReflectionInitFlags::NONE || !p_context) {
        if (!p_context)
            ResonanceLog::error("ResonanceReflectionProcessor: Context is null.");
        if (p_pooled_effect)
            iplReflectionEffectRelease(&p_pooled_effect);
        return;
    }

    ReflectionEffectRecipe recipe = effect_recipe(p_sample_rate, p_frame_size, p_ambisonic_order, p_reflection_type,
                                                  p_ir_duration_sec, p_convolution_ir_max_samples);
    convolution_ir_max_samples_ = std::max(0, p_convolution_ir_max_samples);
    effect_ir_duration_sec_ = recipe.ir_duration_sec;
    context = p_context;
    frame_size = p_frame_size;
    sample_rate = p_sample_rate;
    reflection_type = p_reflection_type;
    num_channels = recipe.settings.numChannels;

    IPLReflectionEffectSettings& reflSettings = recipe.settings;
    if (p_pooled_effect) {
        reflection_effect = p_pooled_effect;
    } else if (iplReflectionEffectCreate(context, &recipe.audio, &reflSettings, &reflection_effect) != IPL_STATUS_SUCCESS) {
        ResonanceLog::error("ResonanceReflectionProcessor: iplReflectionEffectCreate failed.");
        return;
    }
    effect_max_ir_samples_ = static_cast<int>(reflSettings.irSize);
    recipe_ = recipe;
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
}

DetachedReflectionEffect ResonanceReflectionProcessor::detach_effect() {
    DetachedReflectionEffect detached;
    detached.effect = reflection_effect;
    detached.recipe = recipe_;
    reflection_effect = nullptr;
    return detached;
}

void ResonanceReflectionProcessor::cleanup() {
    if (reflection_effect) {
        iplReflectionEffectRelease(&reflection_effect);
        reflection_effect = nullptr;
    }

    if (context) {
        if (sa_mono_buffer.data)
            iplAudioBufferFree(context, &sa_mono_buffer);
        if (sa_temp_out_buffer.data)
            iplAudioBufferFree(context, &sa_temp_out_buffer);
    }
    memset(&sa_mono_buffer, 0, sizeof(sa_mono_buffer));
    memset(&sa_temp_out_buffer, 0, sizeof(sa_temp_out_buffer));

    context = nullptr;
    init_flags = ReflectionInitFlags::NONE;
    convolution_ir_max_samples_ = 0;
    effect_max_ir_samples_ = 0;
}

bool ResonanceReflectionProcessor::process_mix(const IPLAudioBuffer& in_buffer,
                                               const IPLReflectionEffectParams& reverb_params,
                                               IPLReflectionMixer mixer_handle,
                                               float prev_reflections_mix_level,
                                               float reflections_mix_level,
                                               float wet_extra_gain) {

    if (!(init_flags & ReflectionInitFlags::REFLECTIONEFFECT) || !(init_flags & ReflectionInitFlags::BUFFERS) || !reflection_effect)
        return false;

    IPLReflectionEffectParams params = reverb_params;
    sanitize_reflection_params(&params);

    // Without an IR, skip Apply and return false so the caller does not advance its ramp.
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
    }

    const auto apply_start = std::chrono::steady_clock::now();
    iplReflectionEffectApply(reflection_effect, &params,
                             &sa_mono_buffer, &sa_temp_out_buffer, mixer_handle);
    effect_apply_.add(resonance::elapsed_ns(apply_start));
    return true;
}

bool ResonanceReflectionProcessor::process_mix_direct(const IPLAudioBuffer& in_buffer,
                                                      const IPLReflectionEffectParams& reverb_params,
                                                      float prev_reflections_mix_level,
                                                      float reflections_mix_level) {
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
    }

    const auto apply_start = std::chrono::steady_clock::now();
    iplReflectionEffectApply(reflection_effect, &params,
                             &sa_mono_buffer, &sa_temp_out_buffer, nullptr);
    effect_apply_.add(resonance::elapsed_ns(apply_start));
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
    const auto tail_start = std::chrono::steady_clock::now();
    const IPLAudioEffectState state = iplReflectionEffectGetTail(reflection_effect, &sa_temp_out_buffer, mixer);
    effect_apply_.add(resonance::elapsed_ns(tail_start));
    return state;
}

IPLAudioEffectState ResonanceReflectionProcessor::tail_apply_direct(IPLReflectionEffectParams* params) {
    return tail_apply(params, nullptr);
}

IPLAudioEffectState ResonanceReflectionProcessor::tail_apply_to_mixer(IPLReflectionEffectParams* params, IPLReflectionMixer mixer) {
    if (!mixer)
        return IPL_AUDIOEFFECTSTATE_TAILCOMPLETE;
    return tail_apply(params, mixer);
}

} // namespace godot
