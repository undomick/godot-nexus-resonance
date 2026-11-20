#ifndef RESONANCE_REFLECTION_CACHE_PUBLISH_POLICY_H
#define RESONANCE_REFLECTION_CACHE_PUBLISH_POLICY_H

#include <cstdint>

namespace resonance {

/// Epoch 0 means the entry was never published.
inline bool reflection_cache_entry_epoch_fresh(uint32_t slot_epoch, uint32_t entry_epoch) {
    return entry_epoch != 0u && entry_epoch == slot_epoch;
}

} // namespace resonance

#endif // RESONANCE_REFLECTION_CACHE_PUBLISH_POLICY_H
