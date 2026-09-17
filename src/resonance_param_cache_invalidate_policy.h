#ifndef RESONANCE_PARAM_CACHE_INVALIDATE_POLICY_H
#define RESONANCE_PARAM_CACHE_INVALIDATE_POLICY_H

namespace resonance {

/// Double-buffered param caches (pathing / reflection / reverb) use two slots.
constexpr int kParamCacheSlotCount = 2;

/// Per-handle probe-batch invalidation must clear every slot (0 and 1), not only the
/// published front. Otherwise a later front/back swap can resurrect stale entries for
/// handles that were cleared on the old front only. Global slot-epoch bumps are
/// reserved for full clears (_clear_all_param_caches) so unrelated sources are not
/// forced to miss fetch (global wet dropout).
inline bool param_cache_slot_is_valid(int slot) {
    return slot >= 0 && slot < kParamCacheSlotCount;
}

} // namespace resonance

#endif // RESONANCE_PARAM_CACHE_INVALIDATE_POLICY_H
