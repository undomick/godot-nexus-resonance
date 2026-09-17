#ifndef RESONANCE_CONVOLUTION_BUS_POLICY_H
#define RESONANCE_CONVOLUTION_BUS_POLICY_H

#include <cstdint>

namespace resonance {

/// When no source has ever fed the shared reflection mixer and there is no decoded wet to hold, skip
/// iplReflectionMixerApply (dry bus chain is already copied into the output buffer).
inline bool convolution_bus_skip_empty_mixer_apply(uint64_t mixer_feed_count, bool have_decoded_wet_hold_last) {
    if (mixer_feed_count != 0)
        return false;
    return !have_decoded_wet_hold_last;
}

} // namespace resonance

#endif // RESONANCE_CONVOLUTION_BUS_POLICY_H
