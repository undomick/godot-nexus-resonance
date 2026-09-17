#ifndef RESONANCE_ATTENUATION_DIRTY_POLICY_H
#define RESONANCE_ATTENUATION_DIRTY_POLICY_H

namespace resonance {

/// Steam Audio: set IPLDistanceAttenuationModel.dirty when the callback curve changes.
inline bool attenuation_callback_model_is_dirty(bool curve_changed_since_last_set_inputs) {
    return curve_changed_since_last_set_inputs;
}

} // namespace resonance

#endif // RESONANCE_ATTENUATION_DIRTY_POLICY_H
