#ifndef RESONANCE_PLAYBACK_FADE_H
#define RESONANCE_PLAYBACK_FADE_H

#include "resonance_constants.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace resonance {

/// EOS pad gain: cosine half-window (1->0).
inline float input_ring_eos_pad_gain(int k, int pad_n, bool eos_pad) {
    if (!eos_pad || pad_n <= 0)
        return 1.0f;
    if (pad_n <= 1)
        return 0.0f;
    const float t = static_cast<float>(k) / static_cast<float>(pad_n - 1);
    constexpr float k_pi = 3.14159265358979323846f;
    return 0.5f * (1.0f + std::cos(k_pi * t));
}

/// Cap for synthetic ring-underrun fades; remainder of the mix frame is silence.
inline int synthetic_eos_output_fade_length(int rem) {
    if (rem <= 0)
        return 0;
    return std::min(rem, kSyntheticEosOutputFadeMaxSamples);
}

/// Linear fade from full gain at k=0 (continuous with sample before pad) to 0 at k=rem-1.
inline float linear_pad_fade_hold_to_zero(int k, int rem) {
    if (rem <= 0)
        return 0.0f;
    if (rem == 1)
        return 1.0f;
    return static_cast<float>(rem - 1 - k) / static_cast<float>(rem - 1);
}

/// Cosine AM on the last `taper_len` samples of a real chunk.
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

/// True when EOS hands off to the zero-input tail drain (full silent buffer while not playing).
inline bool eos_zero_input_from_silent_full_buffer(int samples_read, int frames, bool base_playing, float max_abs_sample) {
    constexpr float k_eos_silent_eps = 1.0e-8f;
    return samples_read == frames && !base_playing && max_abs_sample <= k_eos_silent_eps;
}

/// Pad `buffer[valid_copy..frames)` with a cosine fade from the last valid sample (or last_mix hold).
template <typename FrameLike>
inline void pad_output_with_cosine_underrun_fade(FrameLike* buffer, int32_t frames, int valid_copy, float hold_l,
                                                 float hold_r, bool hold_valid) {
    if (!buffer || valid_copy >= frames)
        return;
    const float tail_l = (valid_copy > 0) ? buffer[valid_copy - 1].left : (hold_valid ? hold_l : 0.0f);
    const float tail_r = (valid_copy > 0) ? buffer[valid_copy - 1].right : (hold_valid ? hold_r : 0.0f);
    const int rem = frames - valid_copy;
    const int fade_len = synthetic_eos_output_fade_length(rem);
    const int pad_n = std::max(2, fade_len);
    for (int i = valid_copy; i < frames; i++) {
        const int idx = i - valid_copy;
        const float g = (idx < fade_len) ? input_ring_eos_pad_gain(idx, pad_n, true) : 0.0f;
        buffer[i].left = tail_l * g;
        buffer[i].right = tail_r * g;
    }
}

} // namespace resonance

#endif // RESONANCE_PLAYBACK_FADE_H
