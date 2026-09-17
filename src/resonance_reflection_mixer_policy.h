#ifndef RESONANCE_REFLECTION_MIXER_POLICY_H
#define RESONANCE_REFLECTION_MIXER_POLICY_H

#include <cstdint>

namespace resonance {

/// Safety cap for _release_reflection_mixer_when_unused when readers never drop (dead audio thread).
constexpr uint64_t kReflectionMixerReleaseWaitTimeoutMs = 5000;

/// Target cap for deferred releases when reader wait times out. With stuck readers the queue may grow (soft cap + telemetry).
constexpr size_t kMaxDeferredReflectionMixerReleases = 16;

inline bool reflection_mixer_deferred_at_capacity(size_t pending_count) {
    return pending_count >= kMaxDeferredReflectionMixerReleases;
}

/// When at capacity and readers drained: evict oldest deferred mixer via iplReflectionMixerRelease (never discard).
inline bool reflection_mixer_defer_should_evict_oldest(size_t pending_count, int readers) {
    return reflection_mixer_deferred_at_capacity(pending_count) && readers <= 0;
}

/// When at capacity but readers still hold: enqueue anyway (soft cap exceeded; handle retained until readers drop).
inline bool reflection_mixer_defer_exceeds_soft_cap(size_t pending_count, int readers) {
    return reflection_mixer_deferred_at_capacity(pending_count) && readers > 0;
}

inline bool reflection_mixer_release_wait_finished(int readers, uint64_t elapsed_ms) {
    if (readers <= 0)
        return true;
    return elapsed_ms >= kReflectionMixerReleaseWaitTimeoutMs;
}

/// IPL release is safe only when no MixerReadGuard holds the reader count.
inline bool reflection_mixer_safe_to_release(int readers) {
    return readers <= 0;
}

/// After the wait timeout with stuck readers: defer release instead of forcing iplReflectionMixerRelease (UAF risk).
inline bool reflection_mixer_should_defer_release(int readers, uint64_t elapsed_ms) {
    return readers > 0 && reflection_mixer_release_wait_finished(readers, elapsed_ms);
}

/// Audio thread: last reader dropped; main/worker should drain deferred mixers (never release IPL from audio).
inline bool reflection_mixer_should_request_deferred_drain(int readers_before_sub) {
    return readers_before_sub == 1;
}

} // namespace resonance

#endif // RESONANCE_REFLECTION_MIXER_POLICY_H
