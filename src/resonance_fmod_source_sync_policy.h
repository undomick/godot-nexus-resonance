#ifndef RESONANCE_FMOD_SOURCE_SYNC_POLICY_H
#define RESONANCE_FMOD_SOURCE_SYNC_POLICY_H

namespace resonance {

/// try_update_source may fail under simulation_mutex contention; fall back to batched enqueue.
inline bool fmod_source_sync_should_enqueue_on_try_update_failure(bool try_update_succeeded) {
    return !try_update_succeeded;
}

} // namespace resonance

#endif // RESONANCE_FMOD_SOURCE_SYNC_POLICY_H
