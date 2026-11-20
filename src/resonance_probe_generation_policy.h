#ifndef RESONANCE_PROBE_GENERATION_POLICY_H
#define RESONANCE_PROBE_GENERATION_POLICY_H

namespace resonance {

// Steam Audio's uniform floor casts rays down from the top of the volume and ignores hits
// closer than the probe height, so a volume no taller than that height never finds its floor.
inline bool uniform_floor_fits_volume(float region_height, float height_above_floor) {
    return height_above_floor < region_height;
}

} // namespace resonance

#endif
