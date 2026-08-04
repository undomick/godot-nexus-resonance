#ifndef RESONANCE_PROBE_EXCLUSION_H
#define RESONANCE_PROBE_EXCLUSION_H

#include "resonance_constants.h"
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace godot {

/// Box volume that removes probe candidates inside its OBB when parented under a ResonanceProbeVolume.
class ResonanceProbeExclusion : public Node3D {
    GDCLASS(ResonanceProbeExclusion, Node3D)

  private:
    Vector3 region_size = Vector3(4.0f, 4.0f, 4.0f);
    bool enabled = true;

    void _notify_parent_probe_volume();

  protected:
    static void _bind_methods();
    void _notification(int p_what);

  public:
    ResonanceProbeExclusion();
    ~ResonanceProbeExclusion() override = default;

    void _ready() override;

    void set_region_size(Vector3 p_size);
    Vector3 get_region_size() const;

    void set_enabled(bool p_enabled);
    bool is_enabled() const;
};

} // namespace godot

#endif
