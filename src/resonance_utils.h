#ifndef RESONANCE_UTILS_H
#define RESONANCE_UTILS_H

#include "resonance_math.h"
#include <cmath>
#include <cstring>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <phonon.h>

namespace godot {

namespace ResonanceUtils {

/// Minimum vector length before treating the direction as degenerate (avoids divide-by-zero).
inline constexpr real_t kSafeUnitVectorMinLength = (real_t)1e-3;

static inline IPLVector3 to_ipl_vector3(const Vector3& v) {
    return {(float)v.x, (float)v.y, (float)v.z};
}

static inline Vector3 to_godot_vector3(const IPLVector3& v) {
    return Vector3(v.x, v.y, v.z);
}

static inline Vector3 safe_unit_vector(const Vector3& v, const Vector3& fallback = Vector3(0, 1, 0)) {
    real_t len = v.length();
    if (len < kSafeUnitVectorMinLength)
        return fallback;
    return v / len;
}

static inline IPLMatrix4x4 to_ipl_matrix(const Transform3D& tr) {
    IPLMatrix4x4 mat{};
    memset(mat.elements, 0, sizeof(mat.elements));

    Vector3 col_x = tr.basis.get_column(0);
    Vector3 col_y = tr.basis.get_column(1);
    Vector3 col_z = tr.basis.get_column(2);
    Vector3 origin = tr.origin;

    mat.elements[0][0] = (float)col_x.x;
    mat.elements[0][1] = (float)col_y.x;
    mat.elements[0][2] = (float)col_z.x;
    mat.elements[0][3] = (float)origin.x;

    mat.elements[1][0] = (float)col_x.y;
    mat.elements[1][1] = (float)col_y.y;
    mat.elements[1][2] = (float)col_z.y;
    mat.elements[1][3] = (float)origin.y;

    mat.elements[2][0] = (float)col_x.z;
    mat.elements[2][1] = (float)col_y.z;
    mat.elements[2][2] = (float)col_z.z;
    mat.elements[2][3] = (float)origin.z;

    mat.elements[3][0] = 0.0f;
    mat.elements[3][1] = 0.0f;
    mat.elements[3][2] = 0.0f;
    mat.elements[3][3] = 1.0f;

    return mat;
}

/// Steam Audio maps the cube (-0.5, -0.5, -0.5) to (0.5, 0.5, 0.5) onto the probe volume, so the origin is its center.
static inline IPLMatrix4x4 create_volume_transform_rotated(const Transform3D& global_transform, const Vector3& extents) {
    Vector3 safe_extents(extents.x > 0.0f ? extents.x : 0.001f,
                         extents.y > 0.0f ? extents.y : 0.001f,
                         extents.z > 0.0f ? extents.z : 0.001f);
    Vector3 size = safe_extents * 2.0f;
    Basis new_basis = global_transform.basis.orthonormalized();
    new_basis = new_basis.scaled(size);
    Transform3D final_tr;
    final_tr.basis = new_basis;
    final_tr.origin = global_transform.origin;
    return to_ipl_matrix(final_tr);
}

static inline void apply_volume_ramp(float start_vol, float end_vol, int num_samples, float* buffer) {
    resonance::apply_volume_ramp(start_vol, end_vol, num_samples, buffer);
}
} // namespace ResonanceUtils
} // namespace godot

#endif