#ifndef RESONANCE_EXPORT_TRANSFORM_H
#define RESONANCE_EXPORT_TRANSFORM_H

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/transform3d.hpp>

namespace godot {
namespace ResonanceUtils {

/// Outside the scene tree (instantiated offline, never added) get_global_transform is invalid, so walk the parents.
inline Transform3D node3d_export_transform(const Node3D* n) {
    if (!n)
        return Transform3D();
    if (n->is_inside_tree())
        return n->get_global_transform();

    Transform3D xform;
    const Node* cur = n;
    xform = n->get_transform();
    cur = n->get_parent();
    while (cur) {
        const Node3D* n3 = Object::cast_to<Node3D>(cur);
        if (n3)
            xform = n3->get_transform() * xform;
        cur = cur->get_parent();
    }
    return xform;
}

/// Outside the scene tree is_visible_in_tree is invalid, so walk the parents.
inline bool node3d_visible_for_export(const Node3D* n) {
    if (!n)
        return false;
    if (n->is_inside_tree())
        return n->is_visible_in_tree();

    const Node* cur = n;
    while (cur) {
        const Node3D* n3 = Object::cast_to<Node3D>(cur);
        if (n3 && !n3->is_visible())
            return false;
        cur = cur->get_parent();
    }
    return true;
}

} // namespace ResonanceUtils
} // namespace godot

#endif // RESONANCE_EXPORT_TRANSFORM_H
