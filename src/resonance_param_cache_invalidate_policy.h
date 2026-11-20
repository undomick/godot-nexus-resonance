#ifndef RESONANCE_PARAM_CACHE_INVALIDATE_POLICY_H
#define RESONANCE_PARAM_CACHE_INVALIDATE_POLICY_H

namespace resonance {

constexpr int kParamCacheSlotCount = 2;

/// Per-handle invalidation must clear both slots, or a later front/back swap resurrects stale entries.
/// Slot-epoch bumps are only for full clears: they make every source miss and drop all wet output.
inline bool param_cache_slot_is_valid(int slot) {
    return slot >= 0 && slot < kParamCacheSlotCount;
}

} // namespace resonance

#endif // RESONANCE_PARAM_CACHE_INVALIDATE_POLICY_H
