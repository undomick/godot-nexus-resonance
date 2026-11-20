#ifndef RESONANCE_MATH_H
#define RESONANCE_MATH_H

#include <cmath>
#include <cstdint>

namespace resonance {

inline float sanitize_audio_float(float v) {
    return std::isfinite(v) ? v : 0.0f;
}

inline int32_t sanitize_delay_samples(int32_t v) {
    float f = static_cast<float>(v);
    f = sanitize_audio_float(f);
    return static_cast<int32_t>(std::lroundf(f));
}

inline int32_t reverb_ir_size_samples(int sample_rate, float duration_sec) {
    float d = sanitize_audio_float(duration_sec);
    return static_cast<int32_t>(std::lroundf(static_cast<float>(sample_rate) * d));
}

/// IR size for iplReflectionEffectCreate. Capped by convolution_ir_max_samples when > 0 so the created
/// effect matches the IR length clamp applied later.
inline int32_t reflection_effect_create_ir_size(int sample_rate, float duration_sec, int convolution_ir_max_samples) {
    int32_t n = reverb_ir_size_samples(sample_rate, duration_sec);
    if (convolution_ir_max_samples > 0 && n > convolution_ir_max_samples)
        n = static_cast<int32_t>(convolution_ir_max_samples);
    return (n < 1) ? 1 : n;
}

/// Floors band reverb times at 0.1 s; IPL expects them > 0.
inline float clamp_reverb_time(float v) {
    float s = sanitize_audio_float(v);
    return (s > 0.1f) ? s : 0.1f;
}

/// AudioStreamPlayer3D volume, min(volume_db, max_db), as linear gain. Applied to the dry signal
/// before direct, HRTF and wet processing, so the wet path follows source loudness.
inline float effective_asp3d_volume_linear(float volume_db, float max_db) {
    const float eff_db = std::fmin(sanitize_audio_float(volume_db), sanitize_audio_float(max_db));
    return sanitize_audio_float(std::pow(10.0f, eff_db / 20.0f));
}

/// Linear gain ramp across num_samples to avoid zipper noise.
inline void apply_volume_ramp(float start_vol, float end_vol, int num_samples, float* buffer) {
    if (num_samples == 0 || !buffer)
        return;

    if (std::abs(start_vol - end_vol) < 1e-5f) {
        if (std::abs(start_vol - 1.0f) > 1e-5f) {
            for (int i = 0; i < num_samples; ++i)
                buffer[i] *= start_vol;
        }
        return;
    }

    float step = (end_vol - start_vol) / (float)num_samples;
    float current = start_vol;
    for (int i = 0; i < num_samples; ++i) {
        buffer[i] *= current;
        current += step;
    }
}

inline void apply_volume_ramp_and_sanitize(float start_vol, float end_vol, int num_samples, float* buffer) {
    if (num_samples == 0 || !buffer)
        return;

    if (std::abs(start_vol - end_vol) < 1e-5f) {
        if (std::abs(start_vol - 1.0f) > 1e-5f) {
            for (int i = 0; i < num_samples; ++i) {
                buffer[i] = sanitize_audio_float(buffer[i] * start_vol);
            }
        } else {
            for (int i = 0; i < num_samples; ++i) {
                buffer[i] = sanitize_audio_float(buffer[i]);
            }
        }
        return;
    }

    float step = (end_vol - start_vol) / (float)num_samples;
    float current = start_vol;
    for (int i = 0; i < num_samples; ++i) {
        buffer[i] = sanitize_audio_float(buffer[i] * current);
        current += step;
    }
}

} // namespace resonance

#endif // RESONANCE_MATH_H
