#include "resonance_probe_exclusion_filter.h"
#include "resonance_probe_exclusion_math.h"

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/variant.hpp>

using namespace godot;

namespace resonance {

bool point_inside_exclusion_box(const Vector3& world_point, const Transform3D& box_xform,
                                const Vector3& region_size) {
    const Vector3 local = box_xform.affine_inverse().xform(world_point);
    const Vector3 half = region_size * 0.5f;
    return point_in_obb_local(local.x, local.y, local.z, half.x, half.y, half.z);
}

PackedVector3Array filter_points_outside_exclusion_boxes(const PackedVector3Array& points, const Array& boxes) {
    if (boxes.is_empty() || points.is_empty()) {
        return points;
    }

    PackedVector3Array out;
    out.resize(0);
    for (int i = 0; i < points.size(); i++) {
        const Vector3& p = points[i];
        bool excluded = false;
        for (int b = 0; b < boxes.size(); b++) {
            const Variant entry = boxes[b];
            if (entry.get_type() != Variant::DICTIONARY) {
                continue;
            }
            Dictionary d = entry;
            Variant vx = d.get("xform", Variant());
            Variant vs = d.get("size", Variant());
            if (vx.get_type() != Variant::TRANSFORM3D || vs.get_type() != Variant::VECTOR3) {
                continue;
            }
            if (point_inside_exclusion_box(p, vx, vs)) {
                excluded = true;
                break;
            }
        }
        if (!excluded) {
            out.push_back(p);
        }
    }
    return out;
}

} // namespace resonance
