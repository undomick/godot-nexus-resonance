#pragma once

#include <phonon.h>

namespace resonance {

/// Godot world-space direction to Steam Audio decoder axis convention (negate Z).
inline IPLVector3 ipl_dir_from_godot_world(float x, float y, float z) {
    return IPLVector3{x, y, -z};
}

/// Fills the decode orientation from two row-major 4x4 matrices: the HOA bed's local->world transform
/// and the listener's world->listener transform.
void ambisonics_decode_orientation_row_major(const float source_row_major4[16], const float listener_row_major4[16],
                                             IPLCoordinateSpace3* out_orientation);

} // namespace resonance
