#ifndef RESONANCE_STATIC_GEOMETRY_H
#define RESONANCE_STATIC_GEOMETRY_H

#include "resonance_geometry.h"

namespace godot {

/// Non-moving geometry: exported into the baked static scene and loaded from its asset at runtime.
class ResonanceStaticGeometry : public ResonanceGeometry {
    GDCLASS(ResonanceStaticGeometry, ResonanceGeometry)

  protected:
    static void _bind_methods();
    void _validate_property(PropertyInfo& p_property) const override;

  public:
    ResonanceStaticGeometry();
    ~ResonanceStaticGeometry();
};

} // namespace godot

#endif // RESONANCE_STATIC_GEOMETRY_H
