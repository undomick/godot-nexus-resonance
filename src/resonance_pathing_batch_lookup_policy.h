#ifndef RESONANCE_PATHING_BATCH_LOOKUP_POLICY_H
#define RESONANCE_PATHING_BATCH_LOOKUP_POLICY_H

#include <cstddef>
#include <cstdint>

namespace resonance {

enum class PathingBatchLookupOutcome : uint8_t {
    PreferredHit,
    SingleVolumeFallback,
    NoPathingBatch,
    PreferredInvalid,
    AmbiguousMultiVolume,
};

struct PathingBatchLookup {
    PathingBatchLookupOutcome outcome = PathingBatchLookupOutcome::NoPathingBatch;
    int32_t handle = -1;
};

/// Resolve which loaded probe batch supplies pathing for a source.
/// Fail-closed when preferred is invalid or multiple pathing volumes exist without an explicit volume.
inline PathingBatchLookup resolve_pathing_batch_lookup(int32_t preferred_handle, bool preferred_exists,
                                                       bool preferred_has_pathing, int32_t sole_pathing_handle,
                                                       size_t pathing_batch_count) {
    if (preferred_handle >= 0) {
        if (preferred_exists && preferred_has_pathing)
            return {PathingBatchLookupOutcome::PreferredHit, preferred_handle};
        return {PathingBatchLookupOutcome::PreferredInvalid, -1};
    }
    if (pathing_batch_count == 0)
        return {PathingBatchLookupOutcome::NoPathingBatch, -1};
    if (pathing_batch_count == 1 && sole_pathing_handle >= 0)
        return {PathingBatchLookupOutcome::SingleVolumeFallback, sole_pathing_handle};
    return {PathingBatchLookupOutcome::AmbiguousMultiVolume, -1};
}

} // namespace resonance

#endif // RESONANCE_PATHING_BATCH_LOOKUP_POLICY_H
