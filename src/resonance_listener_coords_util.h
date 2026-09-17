#ifndef RESONANCE_LISTENER_COORDS_UTIL_H
#define RESONANCE_LISTENER_COORDS_UTIL_H

#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <phonon.h>

namespace resonance {

inline godot::Vector3 listener_origin_godot(const IPLCoordinateSpace3& cs) {
    return godot::Vector3(cs.origin.x, cs.origin.y, cs.origin.z);
}

/// Godot world transform for the active simulation listener (right/up/-ahead basis).
inline godot::Transform3D listener_coords_to_transform(const IPLCoordinateSpace3& cs) {
    godot::Transform3D t;
    t.origin = listener_origin_godot(cs);
    godot::Basis b;
    b.set_column(0, godot::Vector3(cs.right.x, cs.right.y, cs.right.z));
    b.set_column(1, godot::Vector3(cs.up.x, cs.up.y, cs.up.z));
    b.set_column(2, godot::Vector3(-cs.ahead.x, -cs.ahead.y, -cs.ahead.z));
    t.basis = b;
    return t;
}

} // namespace resonance

#endif // RESONANCE_LISTENER_COORDS_UTIL_H
