#ifndef RESONANCE_DYNAMIC_GEOMETRY_H
#define RESONANCE_DYNAMIC_GEOMETRY_H

#include "resonance_geometry.h"

namespace godot {

/// Excluded from the static bake; lives in the global IPLScene as an instanced mesh of its own sub-scene.
/// Transforms are queued to the simulation worker: call flush_dynamic_acoustic_transform when motion ends so
/// path validation sees the final pose without waiting for dynamic_scene_commit_min_interval.
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
