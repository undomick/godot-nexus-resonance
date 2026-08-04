#ifndef RESONANCE_PROBE_EXCLUSION_MATH_H
#define RESONANCE_PROBE_EXCLUSION_MATH_H

#include <cmath>

namespace resonance {

constexpr float kProbeExclusionEpsilon = 1.0e-5f;

/// Axis-aligned test in the exclusion box local space (after affine inverse).
inline bool point_in_obb_local(float lx, float ly, float lz, float half_x, float half_y, float half_z,
                               float eps = kProbeExclusionEpsilon) {
    return std::fabs(lx) <= half_x + eps && std::fabs(ly) <= half_y + eps && std::fabs(lz) <= half_z + eps;
}

} // namespace resonance

#endif
