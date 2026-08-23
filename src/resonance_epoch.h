#ifndef RESONANCE_EPOCH_H
#define RESONANCE_EPOCH_H

#include <cstdint>

namespace resonance {

/// Bump a cache-slot epoch; skip 0 so epoch 0 can mean "empty / never published".
inline uint32_t bump_slot_epoch(uint32_t& slot_epoch) {
    uint32_t e = slot_epoch + 1u;
    slot_epoch = (e == 0u) ? 1u : e;
    return slot_epoch;
}

} // namespace resonance

#endif // RESONANCE_EPOCH_H
