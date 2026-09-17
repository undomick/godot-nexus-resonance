#ifndef RESONANCE_ATTENUATION_CALLBACK_POLICY_H
#define RESONANCE_ATTENUATION_CALLBACK_POLICY_H

#include "resonance_constants.h"

#include <cmath>
#include <cstdint>

namespace resonance {

inline bool attenuation_callback_data_equal(int mode_a, float min_a, float max_a, const float* curve_a, int num_a, int mode_b,
                                            float min_b, float max_b, const float* curve_b, int num_b) {
    if (mode_a != mode_b)
        return false;
    if (std::fabs(min_a - min_b) > 1.0e-5f || std::fabs(max_a - max_b) > 1.0e-5f)
        return false;
    if (num_a != num_b)
        return false;
    for (int i = 0; i < num_a; i++) {
        if (std::fabs(curve_a[i] - curve_b[i]) > 1.0e-5f)
            return false;
    }
    return true;
}

inline int attenuation_callback_num_curve_samples(int mode, int64_t packed_curve_size) {
    if (mode != 1 && mode != 2)
        return 0;
    if (packed_curve_size <= 0)
        return 0;
    if (packed_curve_size > static_cast<int64_t>(kAttenuationCurveSamples))
        return kAttenuationCurveSamples;
    return static_cast<int>(packed_curve_size);
}

} // namespace resonance

#endif // RESONANCE_ATTENUATION_CALLBACK_POLICY_H
