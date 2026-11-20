#ifndef RESONANCE_TRANSMISSION_FETCH_POLICY_H
#define RESONANCE_TRANSMISSION_FETCH_POLICY_H

namespace resonance {

/// The direct simulator can report a transmission at full line of sight, which the direct effect ignores
/// (gain = occ + (1 - occ) * T). Reset it so the debug overlay and cached params match what is heard.
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
