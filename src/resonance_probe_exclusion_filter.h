#ifndef RESONANCE_PROBE_EXCLUSION_FILTER_H
#define RESONANCE_PROBE_EXCLUSION_FILTER_H

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace resonance {

/// Keeps points that are outside all exclusion OBBs.
/// Each entry in [param boxes] is a Dictionary with keys "xform" (Transform3D) and "size" (Vector3 region_size).
godot::PackedVector3Array filter_points_outside_exclusion_boxes(const godot::PackedVector3Array& points,
                                                                const godot::Array& boxes);

bool point_inside_exclusion_box(const godot::Vector3& world_point, const godot::Transform3D& box_xform,
                                const godot::Vector3& region_size);

} // namespace resonance

#endif
