#ifndef RESONANCE_EXPORT_TRANSFORM_H
#define RESONANCE_EXPORT_TRANSFORM_H

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/transform3d.hpp>

namespace godot {
namespace ResonanceUtils {

/// World transform for static export / bake. Uses get_global_transform when in the scene tree;
/// otherwise walks local transforms to the root (offline instantiate without add_child).
inline Transform3D node3d_export_transform(const Node3D* n) {
    if (!n)
        return Transform3D();
    if (n->is_inside_tree())
        return n->get_global_transform();

    // Build parent * local ... from root to leaf without get_global_transform.
    Transform3D xform;
    const Node* cur = n;
    // Collect leaf-first then apply root-first multiply.
    // Iterative: start at leaf local, then left-multiply each parent local.
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

/// Visibility for static export. Uses is_visible_in_tree when in the scene tree;
/// otherwise walks ancestors with is_visible() (no get_global_transform).
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
