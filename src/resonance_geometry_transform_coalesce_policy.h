#ifndef RESONANCE_GEOMETRY_TRANSFORM_COALESCE_POLICY_H
#define RESONANCE_GEOMETRY_TRANSFORM_COALESCE_POLICY_H

#include <cstdint>

namespace resonance {

/// Per ResonanceGeometry instance: every Nth transform notify/enqueue (see kGeometryTransformCoalesceInterval).
inline bool geometry_transform_coalesce_tick_due(uint32_t counter_after_increment, int interval) {
    if (interval <= 1)
        return true;
    return (counter_after_increment % static_cast<uint32_t>(interval)) == 0u;
}

} // namespace resonance

#endif // RESONANCE_GEOMETRY_TRANSFORM_COALESCE_POLICY_H
