#ifndef RESONANCE_LISTENER_REVERB_POLICY_H
#define RESONANCE_LISTENER_REVERB_POLICY_H

namespace resonance {

constexpr int kListenerReverbRealtime = 0;
constexpr int kListenerReverbBaked = 1;

// Server baked_data_variation for the listener reverb source: -1 ray-traced, 0 baked reverb probes.
inline int listener_reverb_baked_data_variation(int reverb_type) {
    return reverb_type == kListenerReverbBaked ? 0 : -1;
}

inline bool listener_reverb_source_needed(bool listener_applies_reverb, bool fmod_requested) {
    return listener_applies_reverb || fmod_requested;
}

} // namespace resonance

#endif // RESONANCE_LISTENER_REVERB_POLICY_H
