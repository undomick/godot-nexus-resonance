#ifndef RESONANCE_REFLECTION_MIXER_POLICY_H
#define RESONANCE_REFLECTION_MIXER_POLICY_H

#include <cstdint>

namespace resonance {

/// Bounds the wait for readers to drop, e.g. when the audio thread is dead.
constexpr uint64_t kReflectionMixerReleaseWaitTimeoutMs = 5000;

/// Soft cap: with stuck readers the deferred queue grows past it.
constexpr size_t kMaxDeferredReflectionMixerReleases = 16;

inline bool reflection_mixer_deferred_at_capacity(size_t pending_count) {
    return pending_count >= kMaxDeferredReflectionMixerReleases;
}

/// The evicted mixer is released with iplReflectionMixerRelease, never dropped.
inline bool reflection_mixer_defer_should_evict_oldest(size_t pending_count, int readers) {
    return reflection_mixer_deferred_at_capacity(pending_count) && readers <= 0;
}

inline bool reflection_mixer_defer_exceeds_soft_cap(size_t pending_count, int readers) {
    return reflection_mixer_deferred_at_capacity(pending_count) && readers > 0;
}

inline bool reflection_mixer_release_wait_finished(int readers, uint64_t elapsed_ms) {
    if (readers <= 0)
        return true;
    return elapsed_ms >= kReflectionMixerReleaseWaitTimeoutMs;
}

/// Readers are MixerReadGuard holders.
inline bool reflection_mixer_safe_to_release(int readers) {
    return readers <= 0;
}

/// Forcing iplReflectionMixerRelease with stuck readers would be a use-after-free, so defer instead.
inline bool reflection_mixer_should_defer_release(int readers, uint64_t elapsed_ms) {
    return readers > 0 && reflection_mixer_release_wait_finished(readers, elapsed_ms);
}

/// Audio thread, when its reader drops: the audio thread never releases IPL objects, so main/worker drains instead.
inline bool reflection_mixer_should_request_deferred_drain(int readers_before_sub) {
    return readers_before_sub == 1;
}

} // namespace resonance

#endif // RESONANCE_REFLECTION_MIXER_POLICY_H
