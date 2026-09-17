#ifndef RESONANCE_PLAYBACK_HOST_FADE_POLICY_H
#define RESONANCE_PLAYBACK_HOST_FADE_POLICY_H

namespace resonance {

/// Clear a stale host fade-out countdown (kPlaybackHostFadeOutEnabled default off).
/// When stop_requested is set (soft-stop tail), preserve an armed fade-out through EOS partial dry.
inline bool playback_host_fade_out_should_clear_stale(bool stop_requested, bool decoder_playing, int samples_read,
                                                      bool fade_in_active) {
    if (stop_requested)
        return false;
    if (decoder_playing && samples_read > 0)
        return true;
    if (fade_in_active && samples_read > 0)
        return true;
    const bool eos_partial_dry = samples_read > 0 && !decoder_playing;
    return eos_partial_dry;
}

} // namespace resonance

#endif // RESONANCE_PLAYBACK_HOST_FADE_POLICY_H
