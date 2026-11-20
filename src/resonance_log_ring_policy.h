#ifndef RESONANCE_LOG_RING_POLICY_H
#define RESONANCE_LOG_RING_POLICY_H

#include <cstdint>

namespace resonance {

/// True when the slot about to be overwritten still holds an undrained message.
inline bool log_ring_slot_overwrite_drops_message(uint32_t prev_slot_ticket, uint32_t drained_ticket) {
    return prev_slot_ticket > 0u && prev_slot_ticket > drained_ticket;
}

} // namespace resonance

#endif // RESONANCE_LOG_RING_POLICY_H
