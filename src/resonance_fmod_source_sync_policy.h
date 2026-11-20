#ifndef RESONANCE_FMOD_SOURCE_SYNC_POLICY_H
#define RESONANCE_FMOD_SOURCE_SYNC_POLICY_H

namespace resonance {

/// try_update_source fails when simulation_mutex is contended; the update then goes through the batched queue.
inline bool fmod_source_sync_should_enqueue_on_try_update_failure(bool try_update_succeeded) {
    return !try_update_succeeded;
}

} // namespace resonance

#endif // RESONANCE_FMOD_SOURCE_SYNC_POLICY_H
