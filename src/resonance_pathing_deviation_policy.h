#ifndef RESONANCE_PATHING_DEVIATION_POLICY_H
#define RESONANCE_PATHING_DEVIATION_POLICY_H

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace resonance {

constexpr int kPathingDeviationNumBands = 3;
constexpr int kPathingDeviationDefaultLutSamples = 64;

inline int pathing_deviation_lut_samples_clamped(int samples) {
    if (samples < 8)
        return 8;
    if (samples > 256)
        return 256;
    return samples;
}

/// Map deviation angle (radians, expected 0..pi) to LUT index in [0, num_samples - 1].
inline int pathing_deviation_angle_to_lut_index(float angle_rad, int num_samples) {
    if (num_samples <= 1)
        return 0;
    if (!std::isfinite(angle_rad))
        return 0;
    constexpr float kPi = 3.14159265358979323846f;
    const float clamped = std::clamp(angle_rad, 0.0f, kPi);
    const float t = clamped / kPi;
    const int idx = static_cast<int>(std::lround(t * static_cast<float>(num_samples - 1)));
    return std::clamp(idx, 0, num_samples - 1);
}

inline float pathing_deviation_lut_lookup(const float* band_lut, int num_samples, float angle_rad) {
    if (!band_lut || num_samples <= 0)
        return 1.0f;
    const int idx = pathing_deviation_angle_to_lut_index(angle_rad, num_samples);
    const float v = band_lut[idx];
    if (!std::isfinite(v))
        return 1.0f;
    return std::clamp(v, 0.0f, 1.0f);
}

inline int pathing_deviation_band_clamped(int band) {
    if (band < 0)
        return 0;
    if (band >= kPathingDeviationNumBands)
        return kPathingDeviationNumBands - 1;
    return band;
}

} // namespace resonance

#endif
