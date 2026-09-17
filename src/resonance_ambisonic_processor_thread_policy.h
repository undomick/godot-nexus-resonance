#ifndef RESONANCE_AMBISONIC_PROCESSOR_THREAD_POLICY_H
#define RESONANCE_AMBISONIC_PROCESSOR_THREAD_POLICY_H

namespace resonance {

/// Audio thread: passthrough (W-channel dry) until main-thread prewarm/reinit completes.
inline bool ambisonic_audio_should_passthrough(bool initialized, bool steam_context_stale, bool processor_config_stale,
                                               bool processor_matches) {
    return !initialized || steam_context_stale || processor_config_stale || !processor_matches;
}

/// Main thread: reinit ambisonic IPL effects when config drifted or audio flagged stale.
inline bool ambisonic_main_should_reinit_processor(bool initialized, bool processor_config_stale, bool processor_matches) {
    return initialized && (processor_config_stale || !processor_matches);
}

/// Break ambisonic pump when decode cannot consume input (avoid busy-loop on full input ring).
inline bool ambisonic_pump_should_break(bool processor_matches, bool spatial_output_ready) {
    return !processor_matches || !spatial_output_ready;
}

} // namespace resonance

#endif // RESONANCE_AMBISONIC_PROCESSOR_THREAD_POLICY_H
