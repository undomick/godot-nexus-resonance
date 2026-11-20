#ifndef RESONANCE_AMBISONIC_PROCESSOR_THREAD_POLICY_H
#define RESONANCE_AMBISONIC_PROCESSOR_THREAD_POLICY_H

namespace resonance {

// The audio thread may touch the processor only while no stale flag is set. Only the audio thread sets a flag,
// and it stops touching the processor for the rest of the block once it has; main clears the flag when done.
inline bool ambisonic_audio_owns_processor(bool initialized, bool steam_context_stale, bool processor_config_stale) {
    return initialized && !steam_context_stale && !processor_config_stale;
}

inline bool ambisonic_main_should_reinit_processor(bool initialized, bool processor_config_stale) {
    return initialized && processor_config_stale;
}

} // namespace resonance

#endif // RESONANCE_AMBISONIC_PROCESSOR_THREAD_POLICY_H
