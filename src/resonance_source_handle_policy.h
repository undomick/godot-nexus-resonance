#ifndef RESONANCE_SOURCE_HANDLE_POLICY_H
#define RESONANCE_SOURCE_HANDLE_POLICY_H

#include <cstdint>

namespace resonance {

/// Source and probe-batch handles are recycled after server shutdown/reinit. A client that
/// still holds a pre-reinit integer can collide with a newly allocated handle. Pair each
/// handle with the server lifecycle epoch captured at create/load; mismatch means dead.
inline bool handle_matches_lifecycle_epoch(int32_t handle, uint32_t handle_epoch, uint32_t server_epoch) {
    return handle >= 0 && handle_epoch == server_epoch;
}

inline bool source_handle_matches_lifecycle_epoch(int32_t handle, uint32_t handle_epoch, uint32_t server_epoch) {
    return handle_matches_lifecycle_epoch(handle, handle_epoch, server_epoch);
}

inline bool probe_batch_handle_matches_lifecycle_epoch(int32_t handle, uint32_t handle_epoch, uint32_t server_epoch) {
    return handle_matches_lifecycle_epoch(handle, handle_epoch, server_epoch);
}

/// Bump without wrapping to 0 so a default-constructed client epoch of 0 never matches.
inline uint32_t next_source_lifecycle_epoch(uint32_t current) {
    const uint32_t next = current + 1u;
    return next == 0u ? 1u : next;
}

} // namespace resonance

#endif // RESONANCE_SOURCE_HANDLE_POLICY_H
