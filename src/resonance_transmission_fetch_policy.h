#ifndef RESONANCE_TRANSMISSION_FETCH_POLICY_H
#define RESONANCE_TRANSMISSION_FETCH_POLICY_H

namespace resonance {

/// Steam DirectSimulator can report a material transmission even when raycast occlusion is fully
/// visible (closestHit may accept a hit at/behind the source that anyHit rejects). Direct
/// Effect ignores T when occlusion == 1 (gain = occ + (1-occ)*T). Clear T to 1,1,1 on full LOS so
/// F3 overlay and cached Direct params match audible Direct-Effect behavior.
inline void clear_transmission_on_line_of_sight(float occlusion, float* transmission_lmh) {
    if (!transmission_lmh)
        return;
    if (occlusion < 1.0f)
        return;
    transmission_lmh[0] = 1.0f;
    transmission_lmh[1] = 1.0f;
    transmission_lmh[2] = 1.0f;
}

} // namespace resonance

#endif // RESONANCE_TRANSMISSION_FETCH_POLICY_H
