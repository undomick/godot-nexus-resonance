#ifndef RESONANCE_LISTENER_ATTENUATION_POLICY_H
#define RESONANCE_LISTENER_ATTENUATION_POLICY_H

namespace resonance {

/// Playback distance/attenuation must use the same ResonanceServer listener pose as spatialization (seqlock),
/// not a stale camera-only fallback updated later in the frame.
inline bool playback_should_sync_viewport_listener_before_attenuation(bool server_initialized) {
    return server_initialized;
}

} // namespace resonance

#endif // RESONANCE_LISTENER_ATTENUATION_POLICY_H
