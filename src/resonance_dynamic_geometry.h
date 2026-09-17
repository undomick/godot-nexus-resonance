#ifndef RESONANCE_DYNAMIC_GEOMETRY_H
#define RESONANCE_DYNAMIC_GEOMETRY_H

#include "resonance_geometry.h"

namespace godot {

/// Dynamic geometry: excluded from static bake; mesh lives in a local sub-scene and is placed in the global IPLScene
/// as an instanced mesh.
/// Path validation needs the acoustic pose committed (transforms queue to the simulation worker;
/// call ResonanceGeometry::flush_dynamic_acoustic_transform when motion ends to bypass
/// dynamic_scene_commit_min_interval re-queue latency).
/// Use for doors, moving platforms, and other animated objects. Holds mesh_asset; export via Tools menu.
class ResonanceDynamicGeometry : public ResonanceGeometry {
    GDCLASS(ResonanceDynamicGeometry, ResonanceGeometry)

  protected:
    static void _bind_methods();
    void _validate_property(PropertyInfo& p_property) const override;

  public:
    ResonanceDynamicGeometry();
    ~ResonanceDynamicGeometry() = default;
};

} // namespace godot

#endif // RESONANCE_DYNAMIC_GEOMETRY_H
