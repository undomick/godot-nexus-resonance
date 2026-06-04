#include "resonance_ambisonics_decode_orientation.h"
#include <cmath>

namespace resonance {
namespace {

IPLVector3 unit_vector(IPLVector3 v) {
    float len_sq = v.x * v.x + v.y * v.y + v.z * v.z;
    float len = std::sqrt(len_sq);
    if (len < 1e-2f)
        len = 1e-2f;
    return IPLVector3{v.x / len, v.y / len, v.z / len};
}

IPLVector3 cross(const IPLVector3& a, const IPLVector3& b) {
    return IPLVector3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

} // namespace

void ambisonics_decode_orientation_row_major(const float source_row_major4[16], const float listener_row_major4[16],
                                             IPLCoordinateSpace3* out_orientation) {
    if (!out_orientation)
        return;

    auto S = source_row_major4;
    auto L = listener_row_major4;

    // Local-to-world basis rows for the HOA bed (ahead = row 8–10, up = row 4–6).
    IPLVector3 source_ahead_raw = IPLVector3{S[8], S[9], S[10]};
    IPLVector3 source_up_raw = IPLVector3{S[4], S[5], S[6]};
    auto source_ahead_u = unit_vector(source_ahead_raw);
    auto source_up_u = unit_vector(source_up_raw);

    // Listener matrix maps world directions into head space for decoding.
    auto ambisonic_ahead_x = L[0] * source_ahead_u.x + L[4] * source_ahead_u.y + L[8] * source_ahead_u.z;
    auto ambisonic_ahead_y = L[1] * source_ahead_u.x + L[5] * source_ahead_u.y + L[9] * source_ahead_u.z;
    auto ambisonic_ahead_z = L[2] * source_ahead_u.x + L[6] * source_ahead_u.y + L[10] * source_ahead_u.z;

    auto ambisonic_up_x = L[0] * source_up_u.x + L[4] * source_up_u.y + L[8] * source_up_u.z;
    auto ambisonic_up_y = L[1] * source_up_u.x + L[5] * source_up_u.y + L[9] * source_up_u.z;
    auto ambisonic_up_z = L[2] * source_up_u.x + L[6] * source_up_u.y + L[10] * source_up_u.z;

    auto ambisonic_ahead = unit_vector(ipl_dir_from_godot_world(ambisonic_ahead_x, ambisonic_ahead_y, ambisonic_ahead_z));
    auto ambisonic_up = unit_vector(ipl_dir_from_godot_world(ambisonic_up_x, ambisonic_up_y, ambisonic_up_z));
    auto ambisonic_right = unit_vector(cross(ambisonic_ahead, ambisonic_up));

    IPLVector3 decode_ahead;
    decode_ahead.x = -ambisonic_right.z;
    decode_ahead.y = -ambisonic_up.z;
    decode_ahead.z = ambisonic_ahead.z;
    decode_ahead = unit_vector(decode_ahead);

    IPLVector3 decode_up;
    decode_up.x = ambisonic_right.y;
    decode_up.y = ambisonic_up.y;
    decode_up.z = -ambisonic_ahead.y;
    decode_up = unit_vector(decode_up);

    IPLVector3 decode_right = unit_vector(cross(decode_ahead, decode_up));

    out_orientation->ahead = decode_ahead;
    out_orientation->up = decode_up;
    out_orientation->right = decode_right;
    out_orientation->origin = IPLVector3{0.0f, 0.0f, 0.0f};
}

} // namespace resonance
