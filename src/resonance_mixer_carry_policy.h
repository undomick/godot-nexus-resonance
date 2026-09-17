#ifndef RESONANCE_MIXER_CARRY_POLICY_H
#define RESONANCE_MIXER_CARRY_POLICY_H

#include <cstddef>
#include <limits>

namespace resonance {

/// Compact unread pending stereo samples to the front of fixed-capacity carry buffers.
inline void mixer_carry_compact_pending(size_t& read_index, size_t& len) {
    if (read_index > 0 && read_index <= len) {
        len -= read_index;
        read_index = 0;
    }
}

/// Samples to drop from the front after compact so one more `frame_size` block fits.
/// Returns max size_t when `frame_size > cap`.
inline size_t mixer_carry_drop_count_for_append(size_t len, size_t frame_size, size_t cap) {
    if (frame_size > cap)
        return std::numeric_limits<size_t>::max();
    if (len + frame_size <= cap)
        return 0;
    return len + frame_size - cap;
}

inline size_t mixer_carry_len_after_drop(size_t len, size_t frame_size, size_t cap) {
    const size_t drop = mixer_carry_drop_count_for_append(len, frame_size, cap);
    if (drop == std::numeric_limits<size_t>::max())
        return 0;
    if (drop >= len)
        return 0;
    return len - drop;
}

} // namespace resonance

#endif // RESONANCE_MIXER_CARRY_POLICY_H
