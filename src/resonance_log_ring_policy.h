#ifndef RESONANCE_LOG_RING_POLICY_H
#define RESONANCE_LOG_RING_POLICY_H

#include <cstdint>

namespace resonance {

/// Lock-free log ring (fixed slots). Posting overwrites the slot for (ticket-1) % slot_count.
/// Returns true when the slot still holds an undrained message that will be lost.
inline bool log_ring_slot_overwrite_drops_message(uint32_t prev_slot_ticket, uint32_t drained_ticket) {
    return prev_slot_ticket > 0u && prev_slot_ticket > drained_ticket;
}

} // namespace resonance

#endif // RESONANCE_LOG_RING_POLICY_H
