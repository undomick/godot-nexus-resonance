#ifndef RESONANCE_MATH_H
#define RESONANCE_MATH_H

#include <cmath>
#include <cstdint>

namespace resonance {

// Small audio-safe helpers: sanitize floats, IR sizing, wet factors, linear ramps (no Godot/phonon includes).

/// Replace NaN/Inf with 0 for IPL float parameters.
inline float sanitize_audio_float(float v) {
    return std::isfinite(v) ? v : 0.0f;
}

/// Sanitize delay in samples (IPL int field; NaN/Inf → 0).
inline int32_t sanitize_delay_samples(int32_t v) {
    float f = static_cast<float>(v);
    f = sanitize_audio_float(f);
    return static_cast<int32_t>(std::lroundf(f));
}

/// Impulse-response length in samples from sample rate and duration (seconds).
inline int32_t reverb_ir_size_samples(int sample_rate, float duration_sec) {
    float d = sanitize_audio_float(duration_sec);
    return static_cast<int32_t>(std::lroundf(static_cast<float>(sample_rate) * d));
}

/// Minimum band reverb time used by parametric/hybrid paths (IPL expects > 0).
inline float clamp_reverb_time(float v) {
    float s = sanitize_audio_float(v);
    return (s > 0.1f) ? s : 0.1f;
}

/// Extra wet attenuation for baked REVERB (IR has no directional occlusion): blend toward `1 - direct_path_factor`
/// using `reverb_transmission_amount`. Use 0 amount / skip for realtime or static-source bakes where IR encodes geometry.
inline float baked_reverb_wet_occlusion_factor(float occlusion, float transmission_low, float transmission_mid, float transmission_high,
                                               float reverb_transmission_amount) {
    const float occ = std::fmin(std::fmax(sanitize_audio_float(occlusion), 0.0f), 1.0f);
    const float tx_avg = (sanitize_audio_float(transmission_low) + sanitize_audio_float(transmission_mid) + sanitize_audio_float(transmission_high)) / 3.0f;
    const float tx_avg_clamped = std::fmin(std::fmax(tx_avg, 0.0f), 1.0f);
    const float direct_factor = occ + (1.0f - occ) * tx_avg_clamped;
    const float amt = std::fmin(std::fmax(sanitize_audio_float(reverb_transmission_amount), 0.0f), 1.0f);
    const float damping = amt * (1.0f - direct_factor);
    const float factor = 1.0f - damping;
    if (factor <= 0.0f)
        return 0.0f;
    if (factor >= 1.0f)
        return 1.0f;
    return factor;
}

/// Linear gain ramp across `num_samples` (parameter moves without zipper noise).
inline void apply_volume_ramp(float start_vol, float end_vol, int num_samples, float* buffer) {
    if (num_samples == 0 || !buffer)
        return;

    // Fast path: uniform gain
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

/// Like apply_volume_ramp, then sanitize_audio_float per sample (single pass over the buffer).
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
