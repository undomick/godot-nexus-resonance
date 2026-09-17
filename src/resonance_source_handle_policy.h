#ifndef RESONANCE_SOURCE_HANDLE_POLICY_H
#define RESONANCE_SOURCE_HANDLE_POLICY_H

#include "resonance_constants.h"
#include <cstdint>
#include <vector>

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

/// Per-handle simulation/audio caches are fixed arrays indexed by handle id.
inline bool source_handle_fits_cache(int32_t handle) {
    return handle >= 0 && handle < kMaxSimulationSourcesUserMax;
}

inline bool source_count_at_simulation_limit(int32_t active_count, int32_t max_sources) {
    return active_count >= max_sources;
}

/// When the free list is empty, sequential ids must not exceed cache capacity.
inline bool can_alloc_sequential_source_handle(int32_t next_handle) {
    return next_handle < kMaxSimulationSourcesUserMax;
}

/// Worker reflection/pathing gate: invalid/out-of-range handles are ignored, not treated as active.
template <typename GetOutputFlagFn>
inline bool any_source_has_output_flag(const std::vector<int32_t>& handles, GetOutputFlagFn get_flag) {
    if (handles.empty())
        return false;
    for (int32_t h : handles) {
        if (!source_handle_fits_cache(h))
            continue;
        if (get_flag(h) != 0)
            return true;
    }
    return false;
}

template <typename GetReflectionFlagFn>
inline bool any_source_has_reflection_outputs(const std::vector<int32_t>& handles, GetReflectionFlagFn get_flag) {
    return any_source_has_output_flag(handles, get_flag);
}

template <typename GetPathingFlagFn>
inline bool any_source_has_pathing_outputs(const std::vector<int32_t>& handles, GetPathingFlagFn get_flag) {
    return any_source_has_output_flag(handles, get_flag);
}

} // namespace resonance

#endif // RESONANCE_SOURCE_HANDLE_POLICY_H
